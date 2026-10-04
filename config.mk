# snipping-tool version
VERSION = 0.1

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib
# FreeBSD, OpenBSD ports:
#X11INC = /usr/local/include
#X11LIB = /usr/local/lib

INCS = -I${X11INC}
LIBS = -L${X11LIB} -lX11 -lm

# "make static": libX11 sits on top of xcb
STATICLIBS = -static -L${X11LIB} -lX11 -lxcb -lXau -lXdmcp -lpthread -lm

# flags
CPPFLAGS = -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700L
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Os ${INCS} ${CPPFLAGS}
LDFLAGS = ${LIBS}

# compiler and linker
CC = cc
