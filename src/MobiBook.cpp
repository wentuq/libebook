/* 
 * MobiBook and related classes
 * Based non MobiDoc from SumatraPDF code
 * 
 * Original copyright:
 *   SumatraPDF project authors
 *   License: Simplified BSD (see COPYING.BSD) 
 * 
 * Modified by:
 *   Domenico Rotiroti
 *   License: GPL3 (see COPYING)
 */

#include "MobiBook.h"
#include "BitReader.h"
#include "MobiDumper.h"

#include <time.h>
#include <iostream>
#include <string.h>
#include <stdlib.h>

// From (Base)Utils
inline void *memdup(void *data, size_t len)
{
    void *dup = malloc(len);
    if (dup)
        memcpy(dup, data, len);
    return dup;
}
#define _memdup(ptr) memdup(ptr, sizeof(*(ptr)))

/* Ugly name, but the whole point is to make things shorter.
   SAZA = Struct Allocate and Zero memory for Array
   (note: use operator new for single structs/classes) */
#define SAZA(struct_name, n) (struct_name *)calloc((n), sizeof(struct_name))


// Parse mobi format http://wiki.mobileread.com/wiki/MOBI

#define err(msg) std::cerr << "[ERROR] " << msg << std::endl;

#define PALMDOC_TYPE_CREATOR   "TEXtREAd"
#define MOBI_TYPE_CREATOR      "BOOKMOBI"

#define COMPRESSION_NONE 1
#define COMPRESSION_PALM 2
#define COMPRESSION_HUFF 17480

#define ENCRYPTION_NONE 0
#define ENCRYPTION_OLD  1
#define ENCRYPTION_NEW  2

// http://wiki.mobileread.com/wiki/MOBI#PalmDOC_Header
#define kPalmDocHeaderLen 16
struct PalmDocHeader
{
    uint16      compressionType;
    uint16      reserved1;
    uint32      uncompressedDocSize;
    uint16      recordsCount;
    uint16      maxRecSize;     // usually (always?) 4096
    // if it's palmdoc, we have currPos, if mobi, encrType/reserved2
    union {
        uint32      currPos;
        struct {
          uint16    encrType;
          uint16    reserved2;
        } mobi;
    };
};
STATIC_ASSERT(kPalmDocHeaderLen == sizeof(PalmDocHeader), validMobiFirstRecord);

enum MobiDocType {
    TypeMobiDoc = 2,
    TypePalmDoc = 3,
    TypeAudio = 4,
    TypeNews = 257,
    TypeNewsFeed = 258,
    TypeNewsMagazin = 259,
    TypePics = 513,
    TypeWord = 514,
    TypeXls = 515,
    TypePpt = 516,
    TypeText = 517,
    TyepHtml = 518
};

// http://wiki.mobileread.com/wiki/MOBI#MOBI_Header
// Note: the real length of MobiHeader is in MobiHeader.hdrLen. This is just
// the size of the struct
#define kMobiHeaderLen 232
struct MobiHeader {
    char         id[4];
    uint32       hdrLen;   // including 4 id bytes
    uint32       type;     // MobiDocType
    uint32       textEncoding;
    uint32       uniqueId;
    uint32       mobiFormatVersion;
    uint32       ortographicIdxRec; // -1 if no ortographics index
    uint32       inflectionIdxRec;
    uint32       namesIdxRec;
    uint32       keysIdxRec;
    uint32       extraIdx0Rec;
    uint32       extraIdx1Rec;
    uint32       extraIdx2Rec;
    uint32       extraIdx3Rec;
    uint32       extraIdx4Rec;
    uint32       extraIdx5Rec;
    uint32       firstNonBookRec;
    uint32       fullNameOffset; // offset in record 0
    uint32       fullNameLen;
    // Low byte is main language e.g. 09 = English,
    // next byte is dialect, 08 = British, 04 = US.
    // Thus US English is 1033, UK English is 2057
    uint32       locale;
    uint32       inputDictLanguage;
    uint32       outputDictLanguage;
    uint32       minRequiredMobiFormatVersion;
    uint32       imageFirstRec;
    uint32       huffmanFirstRec;
    uint32       huffmanRecCount;
    uint32       huffmanTableOffset;
    uint32       huffmanTableLen;
    uint32       exhtFlags;  // bitfield. if bit 6 (0x40) is set => there's an EXTH record
    char         reserved1[32];
    uint32       drmOffset; // -1 if no drm info
    uint32       drmEntriesCount; // -1 if no drm
    uint32       drmSize;
    uint32       drmFlags;
    //char         reserved2[62];
    char	reserved[12];
    uint16	firstContentRecord;
    uint16	lastContentRecord;
    char	reserved2[46]; // [reserved...reserved2] should be 62 bytes
    // A set of binary flags, some of which indicate extra data at the end of each text block.
    // This only seems to be valid for Mobipocket format version 5 and 6 (and higher?), when
    // the header length is 228 (0xE4) or 232 (0xE8).
    uint16       extraDataFlags;
    int32        indxRec;
};

STATIC_ASSERT(kMobiHeaderLen == sizeof(MobiHeader), validMobiHeader);

// http://wiki.mobileread.com/wiki/MOBI#EXTH_Header
struct ExthHeader {
    char	id[4];
    uint32	hdrLen;   // including 4 id bytes
    uint32	recCount; // number of records
};

struct ExthRecord {
    uint32	type;   
    uint32	len; 
    void *	data;
};

// change big-endian int16 to little-endian (our native format)
static void SwapU16(uint16& i)
{
    i = BEtoHs(i);
}

static void SwapU32(uint32& i)
{
    i = BEtoHl(i);
}

// Uncompress source data compressed with PalmDoc compression into a buffer.
// Returns size of uncompressed data or -1 on error (if destination buffer too small)
static size_t PalmdocUncompress(uint8 *src, size_t srcLen, uint8 *dst, size_t dstLen)
{
    uint8 *srcEnd = src + srcLen;
    uint8 *dstEnd = dst + dstLen;
    uint8 *dstOrig = dst;
    size_t dstLeft;
    while (src < srcEnd) {
        dstLeft = dstEnd - dst;
        assert(dstLeft > 0);
        if (0 == dstLeft)
            return -1;

        unsigned c = *src++;

        if ((c >= 1) && (c <= 8)) {
            assert(dstLeft >= c);
            if (dstLeft < c)
                return -1;
            while (c > 0) {
                *dst++ = *src++;
                --c;
            }
        } else if (c < 128) {
            assert(c != 0);
            *dst++ = c;
        } else if (c >= 192) {
            assert(dstLeft >= 2);
            if (dstLeft < 2)
                return -1;
            *dst++ = ' ';
            *dst++ = c ^ 0x80;
        } else {
            assert((c >= 128) && (c < 192));
            assert(src < srcEnd);
            if (src < srcEnd) {
                c = (c << 8) | *src++;
                size_t back = (c >> 3) & 0x07ff;
                size_t n = (c & 7) + 3;
                uint8 *dstBack = dst - back;
                assert(dstBack >= dstOrig);
                assert(dstLeft >= n);
                while (n > 0) {
                    *dst++ = *dstBack++;
                    --n;
                }
            }
        }
    }

    // zero-terminate to make inspecting in the debugger easier
    if (dst < dstEnd)
        *dst = 0;

    return dst - dstOrig;
}

#define kHuffHeaderLen 24
struct HuffHeader
{
    char         id[4];             // "HUFF"
    uint32       hdrLen;            // should be 24
    // offset of 256 4-byte elements of cache data, in big endian
    uint32       cacheOffset;       // should be 24 as well
    // offset of 64 4-byte elements of base table data, in big endian
    uint32       baseTableOffset;   // should be 1024 + 24
    // like cacheOffset except data is in little endian
    uint32       cacheOffsetLE;     // should be 64 + 1024 + 24
    // like baseTableOffset except data is in little endian
    uint32       baseTableOffsetLE; // should be 1024 + 64 + 1024 + 24
};
STATIC_ASSERT(kHuffHeaderLen == sizeof(HuffHeader), validHuffHeader);

#define kCdicHeaderLen 16
struct CdicHeader
{
    char        id[4];      // "CIDC"
    uint32      hdrLen;     // should be 16
    uint32      unknown;
    uint32      codeLen;
};

STATIC_ASSERT(kCdicHeaderLen == sizeof(CdicHeader), validCdicHeader);

#define kCacheDataLen      (256*4)
#define kBaseTableDataLen  (64*4)

#define kHuffRecordMinLen (kHuffHeaderLen +     kCacheDataLen +     kBaseTableDataLen)
#define kHuffRecordLen    (kHuffHeaderLen + 2 * kCacheDataLen + 2 * kBaseTableDataLen)

#define kCdicsMax 32


static off_t filesize(const char * localpath)
{
  struct stat sb;
  
  stat(localpath, &sb);
  return sb.st_size;
}


class HuffDicDecompressor
{
    // underlying data for cache and baseTable
    // (an optimization to only do one allocation instead of two)
    uint8 *     huffmanData;

    uint32 *    cacheTable;
    uint32 *    baseTable;

    size_t      dictsCount;
    uint8 *     dicts[kCdicsMax];
    uint32      dictSize[kCdicsMax];

    uint32      code_length;

public:
    HuffDicDecompressor();
    ~HuffDicDecompressor();
    bool SetHuffData(uint8 *huffData, size_t huffDataLen);
    bool AddCdicData(uint8 *cdicData, uint32 cdicDataLen);
    size_t Decompress(uint8 *src, size_t octets, uint8 *dst, size_t avail_in);
    bool DecodeOne(uint32 code, uint8 *& dst, size_t& dstLeft);
};

HuffDicDecompressor::HuffDicDecompressor() :
    huffmanData(NULL), cacheTable(NULL), baseTable(NULL),
    code_length(0), dictsCount(0)
{
}

HuffDicDecompressor::~HuffDicDecompressor()
{
    for (size_t i = 0; i < dictsCount; i++) {
        free(dicts[i]);
    }
    free(huffmanData);
}

uint16 ReadBeU16(uint8 *d)
{
    uint16 v = *((uint16*)d);
    SwapU16(v);
    return v;
}

bool HuffDicDecompressor::DecodeOne(uint32 code, uint8 *& dst, size_t& dstLeft)
{
    uint16 dict = code >> code_length;
    if ((size_t)dict > dictsCount) {
        err("invalid dict value");
        return false;
    }
    code &= ((1 << (code_length)) - 1);
    uint16 offset = ReadBeU16(dicts[dict] + code * 2);

    if ((uint32)offset > dictSize[dict]) {
        err("invalid offset");
        return false;
    }
    uint16 symLen = ReadBeU16(dicts[dict] + offset);
    uint8 *p = dicts[dict] + offset + 2;

    if (!(symLen & 0x8000)) {
        size_t res = Decompress(p, symLen, dst, dstLeft);
        if (-1 == res)
            return false;
        dst += res;
        assert(dstLeft >= res);
        dstLeft -= res;
    } else {
        symLen &= 0x7fff;
        if (symLen > 127) {
            err("symLen too big");
            return false;
        }
        if (symLen > dstLeft) {
            err("not enough space");
            return false;
        }
        memcpy(dst, p, symLen);
        dst += symLen;
        dstLeft -= symLen;
    }
    return true;
}

size_t HuffDicDecompressor::Decompress(uint8 *src, size_t srcSize, uint8 *dst, size_t dstSize)
{
    uint32    bitsConsumed = 0;
    uint32    bits = 0;

    BitReader br(src, srcSize);
    size_t      dstLeft = dstSize;

    for (;;) {
        if (bitsConsumed > br.BitsLeft()) {
            err("not enough data");
            return -1;
        }
        br.Eat(bitsConsumed);
        if (0 == br.BitsLeft())
            break;

        bits = br.Peek(32);
        if (br.BitsLeft() < 8 && 0 == bits)
            break;
        uint32 v = cacheTable[bits >> 24];
        uint32 codeLen = v & 0x1f;
        if (!codeLen) {
            err("corrupted table, zero code len");
            return -1;
        }
        bool isTerminal = (v & 0x80) != 0;

        uint32 code;
        if (isTerminal) {
            code = (v >> 8) - (bits >> (32 - codeLen));
        } else {
            uint32 baseVal;
            codeLen -= 1;
            do {
                baseVal = baseTable[codeLen*2];
                code = (bits >> (32 - (codeLen+1)));
                codeLen++;
                if (codeLen > 32) {
                    err("code len > 32 bits");
                    return -1;
                }
            } while (baseVal > code);
            code = baseTable[1 + ((codeLen - 1) * 2)] - (bits >> (32 - codeLen));
        }

        if (!DecodeOne(code, dst, dstLeft))
            return -1;
        bitsConsumed = codeLen;
    }

    if (br.BitsLeft() > 0 && 0 != bits) {
        err("compressed data left");
    }
    return dstSize - dstLeft;
}

bool HuffDicDecompressor::SetHuffData(uint8 *huffData, size_t huffDataLen)
{
    // for now catch cases where we don't have both big endian and little endian
    // versions of the data
    assert(kHuffRecordLen == huffDataLen);
    // but conservatively assume we only need big endian version
    if (huffDataLen < kHuffRecordMinLen)
        return false;
    HuffHeader *huffHdr = (HuffHeader*)huffData;
    SwapU32(huffHdr->hdrLen);
    SwapU32(huffHdr->cacheOffset);
    SwapU32(huffHdr->baseTableOffset);

    if (!strncmp("HUFF", huffHdr->id, 4))
        return false;
    assert(huffHdr->hdrLen == kHuffHeaderLen);
    if (huffHdr->hdrLen != kHuffHeaderLen)
        return false;
    if (huffHdr->cacheOffset != kHuffHeaderLen)
        return false;
    if (huffHdr->baseTableOffset != (huffHdr->cacheOffset + kCacheDataLen))
        return false;
    assert(NULL == huffmanData);
    huffmanData = (uint8*)memdup(huffData, huffDataLen);
    if (!huffmanData)
        return false;
    // we conservatively use the big-endian version of the data,
    cacheTable = (uint32*)(huffmanData + huffHdr->cacheOffset);
    for (size_t i = 0; i < 256; i++) {
        SwapU32(cacheTable[i]);
    }
    baseTable = (uint32*)(huffmanData + huffHdr->baseTableOffset);
    for (size_t i = 0; i < 64; i++) {
        SwapU32(baseTable[i]);
    }
    return true;
}

bool HuffDicDecompressor::AddCdicData(uint8 *cdicData, uint32 cdicDataLen)
{
    CdicHeader *cdicHdr = (CdicHeader*)cdicData;
    SwapU32(cdicHdr->hdrLen);
    SwapU32(cdicHdr->codeLen);

    assert((0 == code_length) || (cdicHdr->codeLen == code_length));
    code_length = cdicHdr->codeLen;

    if (!strncmp("CDIC", cdicHdr->id, 4))
        return false;
    assert(cdicHdr->hdrLen == kCdicHeaderLen);
    if (cdicHdr->hdrLen != kCdicHeaderLen)
        return false;
    uint32 size = cdicDataLen - cdicHdr->hdrLen;

    uint32 maxSize = 1 << code_length;
    if (maxSize >= size)
        return false;
    dicts[dictsCount] = (uint8*)memdup(cdicData + cdicHdr->hdrLen, size);
    dictSize[dictsCount] = size;
    ++dictsCount;
    return true;
}

static bool IsMobiPdb(PdbHeader *pdbHdr)
{
    return (strncmp(pdbHdr->type, MOBI_TYPE_CREATOR, 8) == 0);
}

static bool IsPalmDocPdb(PdbHeader *pdbHdr)
{
    return (strncmp(pdbHdr->type, PALMDOC_TYPE_CREATOR, 8) == 0);
}

static bool IsValidCompression(int comprType)
{
    return  (COMPRESSION_NONE == comprType) ||
            (COMPRESSION_PALM == comprType) ||
            (COMPRESSION_HUFF == comprType);
}

MobiBook::MobiBook() :
    recHeaders(NULL), firstRecData(NULL),
    isMobi(false), docRecCount(0), compressionType(0), docUncompressedSize(0),
    doc(""), multibyte(false), trailersCount(0), imageFirstRec(0),
    imagesCount(0), images(NULL), bufDynamic(NULL), bufDynamicSize(0),
    coverImage(-1), huffDic(NULL), textEncoding(CP_UTF8)
{
}

MobiBook::~MobiBook()
{
    fclose(fileHandle);
    free(fileName);
    free(firstRecData);
    free(recHeaders);
    free(bufDynamic);
    if(images) {
        for (size_t i = 0; i < imagesCount; i++) {
            if(images[i].data) free(images[i].data);
        }
        free(images);
    }
    delete huffDic;
}

bool MobiBook::parseHeader()
{
    DWORD bytesRead;
    bytesRead = fread((void*)&pdbHeader, 1, kPdbHeaderLen, fileHandle);
    if ((kPdbHeaderLen != bytesRead))
        return false;

    if (IsMobiPdb(&pdbHeader)) {
        isMobi = true;
    } else if (IsPalmDocPdb(&pdbHeader)) {
        isMobi = false;
    } else {
        // TODO: print type/creator
        err(" unknown pdb type/creator");
        return false;
    }

    // the values are in big-endian, so convert to host order
    // but only those that we actually access
    SwapU16(pdbHeader.numRecords);
    if (pdbHeader.numRecords < 1)
        return false;

    // allocate one more record as a sentinel to make calculating
    // size of the records easier
    recHeaders = SAZA(PdbRecordHeader, pdbHeader.numRecords + 1);
    if (!recHeaders)
        return false;
    bytesRead = fread((void*)recHeaders, kPdbRecordHeaderLen, pdbHeader.numRecords, fileHandle);
    if ((pdbHeader.numRecords != bytesRead))
        return false;

    for (int i = 0; i < pdbHeader.numRecords; i++) {
        SwapU32(recHeaders[i].offset);
    }
    size_t fileSize = filesize(fileName);
    recHeaders[pdbHeader.numRecords].offset = fileSize;
    // validate offsets
    for (int i = 0; i < pdbHeader.numRecords; i++) {
        if (recHeaders[i + 1].offset < recHeaders[i].offset) {
            err("invalid offset field");
            return false;
        }
        // technically PDB record size should be less than 64K,
        // but it's not true for mobi files, so we don't validate that
    }

    size_t recLeft;
    char *buf = readRecord(0, recLeft);
    if (NULL == buf) {
        err("failed to read record");
        return false;
    }

    assert(NULL == firstRecData);
    firstRecData = (char*)memdup(buf, recLeft);
    if (!firstRecData)
        return false;
    char *currRecPos = firstRecData;
    PalmDocHeader *palmDocHdr = (PalmDocHeader*)currRecPos;
    currRecPos += sizeof(PalmDocHeader);
    recLeft -= sizeof(PalmDocHeader);

    SwapU16(palmDocHdr->compressionType);
    SwapU32(palmDocHdr->uncompressedDocSize);
    SwapU16(palmDocHdr->recordsCount);
    SwapU16(palmDocHdr->maxRecSize);
    if (!IsValidCompression(palmDocHdr->compressionType)) {
        err("unknown compression type");
        return false;
    }
    if (isMobi) {
        // TODO: this needs to be surfaced to the client so
        // that we can show the right error message
        if (palmDocHdr->mobi.encrType != ENCRYPTION_NONE) {
            err("encryption is not supported");
            return false;
        }
    }

    docRecCount = palmDocHdr->recordsCount;
    docUncompressedSize = palmDocHdr->uncompressedDocSize;
    compressionType = palmDocHdr->compressionType;

    if (0 == recLeft) {
        assert(!isMobi);
        // TODO: calculate imageFirstRec / imagesCount
        return true;
    }
    if (recLeft < 8) // id and hdrLen
        return false;

    MobiHeader *mobiHdr = (MobiHeader*)currRecPos;
    if (strncmp("MOBI", mobiHdr->id, 4)) {
        err("MobiHeader.id is not 'MOBI'");
        return false;
    }
    SwapU32(mobiHdr->hdrLen);
    SwapU32(mobiHdr->type);
    SwapU32(mobiHdr->textEncoding);
    SwapU32(mobiHdr->mobiFormatVersion);
    SwapU32(mobiHdr->firstNonBookRec);
    SwapU32(mobiHdr->fullNameOffset);
    SwapU32(mobiHdr->fullNameLen);
    SwapU32(mobiHdr->locale);
    SwapU32(mobiHdr->minRequiredMobiFormatVersion);
    SwapU32(mobiHdr->imageFirstRec);
    SwapU32(mobiHdr->huffmanFirstRec);
    SwapU32(mobiHdr->huffmanRecCount);
    SwapU32(mobiHdr->huffmanTableOffset);
    SwapU32(mobiHdr->huffmanTableLen);
    SwapU32(mobiHdr->exhtFlags);
    SwapU16(mobiHdr->firstContentRecord);
    SwapU16(mobiHdr->lastContentRecord);
    
    locale = mobiHdr->locale;
    title.append( (char*)(firstRecData+mobiHdr->fullNameOffset), mobiHdr->fullNameLen );

    textEncoding = mobiHdr->textEncoding;

    if (pdbHeader.numRecords > mobiHdr->imageFirstRec) {
        imageFirstRec = mobiHdr->imageFirstRec;
        if (0 == imageFirstRec) {
            // I don't think this should ever happen but I've seen it
            imagesCount = 0;
        } else
            //imagesCount = pdbHeader.numRecords - mobiHdr->imageFirstRec;
	    imagesCount = mobiHdr->lastContentRecord - mobiHdr->imageFirstRec +1;
    }
    size_t hdrLen = mobiHdr->hdrLen;
    if (hdrLen > recLeft) {
        err("MobiHeader too big");
        return false;
    }
    currRecPos += hdrLen;
    recLeft -= hdrLen;
    bool hasExtraFlags = (hdrLen >= 228); // TODO: also only if mobiFormatVersion >= 5?

    if (hasExtraFlags) {
        SwapU16(mobiHdr->extraDataFlags);
        uint16 flags = mobiHdr->extraDataFlags;
        multibyte = ((flags & 1) != 0);
        while (flags > 1) {
            if (0 != (flags & 2))
                trailersCount++;
            flags = flags >> 1;
        }
    }
    
    if(mobiHdr->exhtFlags & 0x40) { //there's EXTH Header 
	ExthHeader * eh = (ExthHeader*)currRecPos;
	if (strncmp("EXTH", eh->id, 4)) {
	    err("ExthHeader.id is not 'EXTH'");
	    return false;
	}
	
	SwapU32(eh->hdrLen);
	SwapU32(eh->recCount);
	
	currRecPos += sizeof(ExthHeader);
	ExthRecord * rec;
	uint32 coverOffset;
	for(int i = 0; i < eh->recCount; ++i) {
	    rec = (ExthRecord*)currRecPos;
	    SwapU32(rec->len);
	    SwapU32(rec->type);

	    switch(rec->type) {
		case 100: 
		    author.append( (char*) &rec->data /*, rec->len*/);
		    break;
		case 101:
		    publisher.append( (char*) &rec->data /*, rec->len*/);
		    break;
		case 201:
		    coverOffset = (uint32) rec->data;
		    SwapU32(coverOffset);
		    coverImage = coverOffset;
		    break;
		case 503: //if present, it's a better choice for title
		    title = (char*) &rec->data;
		    break;
		default:    
		    break;
	    }

	    currRecPos += (rec->len);
	}
    }


    if (palmDocHdr->compressionType == COMPRESSION_HUFF) {
        assert(isMobi);
        size_t recSize;
        char *recData = readRecord(mobiHdr->huffmanFirstRec, recSize);
        if (!recData)
            return false;
        size_t cdicsCount = mobiHdr->huffmanRecCount - 1;
        assert(cdicsCount <= kCdicsMax);
        if (cdicsCount > kCdicsMax)
            return false;
        assert(NULL == huffDic);
        huffDic = new HuffDicDecompressor();
        if (!huffDic->SetHuffData((uint8*)recData, recSize))
            return false;
        for (size_t i = 0; i < cdicsCount; i++) {
            recData = readRecord(mobiHdr->huffmanFirstRec + 1 + i, recSize);
            if (!recData)
                return false;
            if (!huffDic->AddCdicData((uint8*)recData, recSize))
                return false;
        }
    }

    loadImages();
    return true;
}

#define EOF_REC   0xe98e0d0a
#define FLIS_REC  0x464c4953 // 'FLIS'
#define FCIS_REC  0x46434953 // 'FCIS
#define FDST_REC  0x46445354 // 'FDST'
#define DATP_REC  0x44415450 // 'DATP'
#define SRCS_REC  0x53524353 // 'SRCS'
#define VIDE_REC  0x56494445 // 'VIDE'

static uint32 GetUpToFour(uint8*& s, size_t& len)
{
    size_t n = 0;
    uint32 v = *s++; len--;
    while ((n < 3) && (len > 0)) {
        v = v << 8;
        v = v | *s++;
        len--; n++;
    }
    return v;
}

static bool IsEofRecord(uint8 *data, size_t dataLen)
{
    return (4 == dataLen) && (EOF_REC == GetUpToFour(data, dataLen));
}

static bool KnownNonImageRec(uint8 *data, size_t dataLen)
{
    uint32 sig = GetUpToFour(data, dataLen);

    if (FLIS_REC == sig) return true;
    if (FCIS_REC == sig) return true;
    if (FDST_REC == sig) return true;
    if (DATP_REC == sig) return true;
    if (SRCS_REC == sig) return true;
    if (VIDE_REC == sig) return true;
    return false;
}

#define JPG_MAGIC "\xff\xd8\xff\xe0"
#define PNG_MAGIC "\x89PNG"
#define GIF_MAGIC "GIF8"
#define TYPE(data, type) !strncmp((char*)data, type##_MAGIC, strlen(type##_MAGIC))

static char * ImageType(uint8 *data, size_t dataLen)
{
    //return NULL != GfxFileExtFromData((char*)data, dataLen);
    if(TYPE(data, JPG)) return strdup(".jpg");
    if(TYPE(data, PNG)) return strdup(".png");
    if(TYPE(data, GIF)) return strdup(".gif");
    return strdup(".bin");
}

// return false if we should stop loading images (because we
// encountered eof record or ran out of memory)
bool MobiBook::loadImage(size_t imageNo)
{
    size_t imageRec = imageFirstRec + imageNo;
    size_t imgDataLen;

    uint8 *imgData = (uint8*)readRecord(imageRec, imgDataLen);
    if (!imgData || (0 == imgDataLen))
        return true;
    if (IsEofRecord(imgData, imgDataLen))
        return false;
    if (KnownNonImageRec(imgData, imgDataLen))
        return true;

    images[imageNo].data = (char*)memdup(imgData, imgDataLen);
    if (!images[imageNo].data)
        return false;
    images[imageNo].len = imgDataLen;
    images[imageNo].type = ImageType(imgData, imgDataLen);
    return true;
}

void MobiBook::loadImages()
{
    if (0 == imagesCount)
        return;
    images = SAZA(ImageData, imagesCount);
    for (size_t i = 0; i < imagesCount; i++) {
        if (!loadImage(i))
            return;
    }
}

// imgRecIndex corresponds to recindex attribute of <img> tag
// as far as I can tell, this means: it starts at 1 
// returns NULL if there is no image (e.g. it's not a format we
// recognize)
ImageData *MobiBook::getImage(size_t imgRecIndex) const
{
    if ((imgRecIndex > imagesCount) || (imgRecIndex < 1))
        return NULL;
   --imgRecIndex;
   if (!images[imgRecIndex].data || (0 == images[imgRecIndex].len))
       return NULL;
   return &images[imgRecIndex];
}

// first two images seem to be the same picture of the cover
// except at different resolutions
ImageData *MobiBook::getCover()
{
    if(coverImage >= 0) {
	return &images[coverImage];
    }
    
    err("Using unreliable method to get cover");
    size_t coverImg = 0;
    size_t size=0, s;
    size_t maxImageNo = std::min(imagesCount, (size_t)2);
    for (size_t i = 0; i < maxImageNo; i++) {
        if (!images[i].data)
            continue;
        s = images[i].len;
        if (s > size) {
            coverImg = i;
            size = s;
        }
    }
    if (size==0)
        return NULL;
    return &images[coverImg];
}

size_t MobiBook::getRecordSize(size_t recNo)
{
    size_t size = recHeaders[recNo + 1].offset - recHeaders[recNo].offset;
    return size;
}

// returns NULL if error (failed to allocated)
char *MobiBook::getBufForRecordData(size_t size)
{
    if (size <= sizeof(bufStatic))
        return bufStatic;
    if (size <= bufDynamicSize)
        return bufDynamic;
    free(bufDynamic);
    bufDynamic = (char*)malloc(size);
    bufDynamicSize = size;
    return bufDynamic;
}

// read a record and return it's data and size. Return NULL if error
char* MobiBook::readRecord(size_t recNo, size_t& sizeOut)
{
    size_t off = recHeaders[recNo].offset;
    DWORD toRead = getRecordSize(recNo);
    sizeOut = toRead;
    char *buf = getBufForRecordData(toRead);
    if (NULL == buf)
        return NULL;
    DWORD bytesRead;
    int res = fseek(fileHandle, off, SEEK_SET);
    //DWORD res = SetFilePointer(fileHandle, off, NULL, FILE_BEGIN);
    if (res != 0)
        return NULL;
    //BOOL ok = ReadFile(fileHandle, (void*)buf, toRead, &bytesRead, NULL);
    bytesRead = fread((void*)buf, 1, toRead, fileHandle);
    if (/*!ok || */(toRead != bytesRead))
        return NULL;
    return buf;
}

// each record can have extra data at the end, which we must discard
static size_t ExtraDataSize(uint8 *recData, size_t recLen, size_t trailersCount, bool multibyte)
{
    size_t newLen = recLen;
    for (size_t i = 0; i < trailersCount; i++) {
        assert(newLen > 4);
        uint32 n = 0;
        for (size_t j = 0; j < 4; j++) {
            uint8 v = recData[newLen - 4 + j];
            if (0 != (v & 0x80))
                n = 0;
            n = (n << 7) | (v & 0x7f);
        }
        assert(newLen > n);
        newLen -= n;
    }

    if (multibyte) {
        assert(newLen > 0);
        if (newLen > 0) {
            uint8 n = (recData[newLen-1] & 3) + 1;
            assert(newLen >= n);
            newLen -= n;
        }
    }
    return recLen - newLen;
}

// Load a given record of a document into strOut, uncompressing if necessary.
// Returns false if error.
bool MobiBook::loadDocRecordIntoBuffer(size_t recNo, std::string& strOut)
{
    size_t recSize;
    char *recData = readRecord(recNo, recSize);
    if (NULL == recData)
        return false;
    size_t extraSize = ExtraDataSize((uint8*)recData, recSize, trailersCount, multibyte);
    recSize -= extraSize;
    if (COMPRESSION_NONE == compressionType) {
        strOut.append(recData, recSize);
        return true;
    }

    if (COMPRESSION_PALM == compressionType) {
        char buf[6000]; // should be enough to decompress any record
        size_t uncompressedSize = PalmdocUncompress((uint8*)recData, recSize, (uint8*)buf, sizeof(buf));
        if (-1 == uncompressedSize) {
            err("PalmDoc decompression failed");
            return false;
        }
        strOut.append(buf, uncompressedSize);
        return true;
    }

    if (COMPRESSION_HUFF == compressionType) {
        char buf[6000]; // should be enough to decompress any record
        assert(huffDic);
        if (!huffDic)
            return false;
        size_t uncompressedSize = huffDic->Decompress((uint8*)recData, recSize, (uint8*)buf, sizeof(buf));
        if (-1 == uncompressedSize) {
            err("HuffDic decompression failed");
            return false;
        }
        strOut.append(buf, uncompressedSize);
        return true;
    }

    assert(0);
    return false;
}

unsigned int	MobiBook::getLocale() {
    return locale;
}

// assumes that ParseHeader() has been called
bool MobiBook::loadDocument()
{
    assert(docUncompressedSize > 0);

    //doc = new str::Str<char>(docUncompressedSize);
    for (size_t i = 1; i <= docRecCount; i++) {
        if (!loadDocRecordIntoBuffer(i, doc))
            return false;
    }
    assert(docUncompressedSize == doc.length());
    /*
    if (textEncoding != CP_UTF8) {
        char *docUtf8 = str::ToMultiByte(doc->Get(), textEncoding, CP_UTF8);
        if (docUtf8) {
            doc->Reset();
            doc->AppendAndFree(docUtf8);
        }
    }*/
    return true;
}

MobiBook *MobiBook::createFromFile(const char *fileName)
{
    FILE * fh = fopen(fileName, "rb");
    if (fh == NULL)
        return NULL;
    MobiBook *mb = new MobiBook();
    mb->fileName = strdup(fileName);
    mb->fileHandle = fh;

    if (mb->parseHeader()) {
	if (mb->loadDocument()) 
		return mb;
    }

    delete mb;
    return NULL;
}

Dumper * MobiBook::getDumper(const char * outdir) {
	return new MobiDumper(this, outdir);
    }
