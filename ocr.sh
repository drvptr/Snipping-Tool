#!/bin/sh
scrot -s -F - | tesseract stdin stdout -l rus+eng | xclip -selection clipboard
