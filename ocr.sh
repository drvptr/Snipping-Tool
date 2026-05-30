#!/bin/sh

for ARG in "$@" ; do
        if [ "$ARG" = '-h' -o "$ARG" = "--help" ] ;  then
                printf "Usage:\n  $0 \n  $0 en \n  $0 ru \n  $0 whatever_for_en-ru"
                exit 0
        fi
done

if [ $# -eq 0 -o "$1" = "en" ] ; then
        scrot -s -F - | tesseract stdin stdout -l eng | xclip -selection clipboard
else
        if [ "$1" = 'ru' ] ; then
                scrot -s -F - | tesseract stdin stdout -l ru | xclip -selection clipboard
                exit 0
        fi
        scrot -s -F - | tesseract stdin stdout -l eng,ru | xclip -selection clipboard
fi
