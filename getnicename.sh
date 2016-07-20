#!/bin/bash

## Make life easier
set -u
set -e
set -o pipefail


~/Dropbox/ebooks-scripts/libebook/bin/bookinfo "$1" | tr -d '\n' | tr -s ' ' | tr -d '.' | konwert utf8-ascii/asciichar 

