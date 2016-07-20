#!/bin/bash

## Make life easier
### Programs scans for ebooks

EBOOKS_PATH='../../ebooks/'
APP_DIR="$(pwd)"

function mybookinfo {
	"$APP_DIR"/bin/bookinfo2 "$1"
	realpath --relative-to="$EBOOKS_PATH" "$1"
}

function remove_list {
	rm list_mobi_epub.txt
	rm list_mobi_epub_sorted.txt
	rm list_pdf_sorted.txt
	rm list_others_sorted.txt
	rm list_all.txt
	rm list_err.txt
	rm "$EBOOKS_PATH"list_all.txt
}


function concat_all {
	echo "----------------MOBI OR EPUB----------------" >> list_all.txt
	echo "title - author;(publisher);path to file" >> list_all.txt
	cat list_mobi_epub_sorted.txt >>  list_all.txt
	echo "----------------PDF-------------------------" >> list_all.txt
	cat list_pdf_sorted.txt >>  list_all.txt
	echo "----------------OTHERS----------------------" >> list_all.txt
	cat list_others_sorted.txt >>  list_all.txt
	echo "----------------ERRORS----------------------" >> list_all.txt
	cat list_err.txt >>  list_all.txt
	cp list_all.txt "$EBOOKS_PATH"
}

# Real program starts

# Redirects errors
remove_list

# Find mobi or epub and get metadata
find "$EBOOKS_PATH" -type f -iregex '.*\.\(mobi\|epub\)' -print0 | while IFS= read -r -d $'\0' line; do
    mybookinfo "$line" >> list_mobi_epub.txt 2>>list_err.txt
done

# Sort it
sort list_mobi_epub.txt | sed '/^\s*$/d' > list_mobi_epub_sorted.txt


# Find pdf
find "$EBOOKS_PATH" -type f -iname "*.pdf" | sort > list_pdf_sorted.txt


# Find everything else
find "$EBOOKS_PATH" -type f -print | grep -vi ".*\.\(mobi\|epub\|pdf\)" | sort > list_others_sorted.txt


# Concatenate all
concat_all
