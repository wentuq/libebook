#!/bin/bash
# Find mobi or epub, get metadata from those files and rename files appropiately, also renames pdf if has
# same name as mobi or epub
## Make life easier
set -u
set -e
set -o pipefail

# inherits shell options in subshell
# test it like that
# echo "$-"
# export SHELLOPTS

EBOOKS_PATH='../../ebooks/buy/'


APP_DIR="$(pwd)"
export APP_NAME=""$APP_DIR"/getnicename.sh"


find "$EBOOKS_PATH" -type f -iregex '.*\.\(mobi\|epub\)' -execdir bash -c \
	'
	set -u
	set -e
	set -o pipefail

	OLDNAME_NO_EXT="${0%.*}"
	NEWNAME_NO_EXT="./$("$APP_NAME" "$0")"
	NEWNAME="./$("$APP_NAME" "$0")"."${0##*.}"

	if [[ "$NEWNAME" != "$0" ]]; then
		mv -n  -- "$0" "$NEWNAME"
	fi

	if [[ -f "$OLDNAME_NO_EXT".pdf && "$NEWNAME_NO_EXT".pdf != "$OLDNAME_NO_EXT".pdf ]]; then
		 mv -n  -- "$OLDNAME_NO_EXT".pdf "$NEWNAME_NO_EXT".pdf
	fi



	' "{}" \;
