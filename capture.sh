#Bourne shell command that allows you to save a piece of screen to the clipboard
#REQUIREMENTS: scrot, xclip
#!/bin/sh
scrot -s -F - | xclip -selection clipboard -t image/png -i $f
