#!/bin/sh
# Turn a text file into a C string constant.
#   usage: embed.sh <symbol> <file>
set -e
sym="$1"
file="$2"

printf '/* Generated from %s - do not edit. */\n' "$file"
printf 'const char %s[] =\n' "$sym"
sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/\t/\\t/g' \
    -e 's/^/"/' -e 's/$/\\n"/' "$file"
printf ';\n'
