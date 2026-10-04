# snipping-tool - screen snips for X11 in the spirit of the Windows tool
# See LICENSE file for copyright and license details.

include config.mk

SRC = snip.c gui.c png.c ocr.c
OBJ = ${SRC:.c=.o}
ICONS = app16.png app32.png app48.png new.png cancel.png options.png \
	save.png copy.png send.png pen.png marker.png eraser.png ocr.png help.png
ALIAS = alias snipping-tool='snipping-tool --graphic'
CSHALIAS = alias snipping-tool 'snipping-tool --graphic'

all: snipping-tool

.c.o:
	${CC} -c ${CFLAGS} $<

${OBJ}: config.h snip.h config.mk
gui.o: font.h icons.h
ocr.o: ocrdb.h

config.h:
	cp config.def.h $@

snipping-tool: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

# fully static binary, needs libX11.a, libxcb.a, libXau.a and libXdmcp.a
static: clean
	${MAKE} LDFLAGS="${STATICLIBS}"

# icons/*.png -> icons.h (xxd from vim); run it after replacing an icon
icons:
	cd icons && for f in ${ICONS}; do xxd -i $$f || exit 1; done > ../icons.h

# font.h and ocrdb.h are generated too, see tools/ (needs python3-pil)
font:
	python3 tools/mkfont.py > font.h

ocrdb:
	python3 tools/mkocrdb.py > ocrdb.h

clean:
	rm -f snipping-tool ${OBJ} snipping-tool-${VERSION}.tar.gz

dist: clean
	mkdir -p snipping-tool-${VERSION}/icons snipping-tool-${VERSION}/tools
	cp -R LICENSE COPYING Makefile README config.def.h config.mk snip.h ${SRC} \
		icons.h font.h ocrdb.h snipping-tool.1 snipping-tool.desktop \
		snipping-tool-${VERSION}
	cp icons/*.png snipping-tool-${VERSION}/icons
	cp tools/*.py snipping-tool-${VERSION}/tools
	tar -cf - snipping-tool-${VERSION} | gzip > snipping-tool-${VERSION}.tar.gz
	rm -rf snipping-tool-${VERSION}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f snipping-tool ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/snipping-tool
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	sed "s/VERSION/${VERSION}/g" < snipping-tool.1 > ${DESTDIR}${MANPREFIX}/man1/snipping-tool.1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/snipping-tool.1
	mkdir -p ${DESTDIR}${PREFIX}/share/applications ${DESTDIR}${PREFIX}/share/pixmaps
	cp -f snipping-tool.desktop ${DESTDIR}${PREFIX}/share/applications
	cp -f icons/app48.png ${DESTDIR}${PREFIX}/share/pixmaps/snipping-tool.png
	chmod 644 ${DESTDIR}${PREFIX}/share/applications/snipping-tool.desktop \
		${DESTDIR}${PREFIX}/share/pixmaps/snipping-tool.png
	@if [ -z "${DESTDIR}" ] && [ -z "${NOALIAS}" ]; then ${MAKE} -s alias; fi

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/snipping-tool \
		${DESTDIR}${MANPREFIX}/man1/snipping-tool.1 \
		${DESTDIR}${PREFIX}/share/applications/snipping-tool.desktop \
		${DESTDIR}${PREFIX}/share/pixmaps/snipping-tool.png
	@if [ -z "${DESTDIR}" ]; then ${MAKE} -s unalias; fi

# alias in the shell rc files of the user who builds (also under sudo),
# so that typing snipping-tool starts the graphical version; the alias of
# an older version is replaced
alias:
	@h=$$HOME; \
	if [ -n "$$SUDO_USER" ]; then h=$$(eval echo "~$$SUDO_USER"); fi; \
	n=0; \
	for rc in .bashrc .zshrc .kshrc .mkshrc .shrc .cshrc .tcshrc .config/fish/config.fish; do \
		f="$$h/$$rc"; \
		[ -f "$$f" ] || continue; \
		n=1; \
		case $$rc in \
		.cshrc|.tcshrc|*fish*) a="${CSHALIAS}";; \
		*) a="${ALIAS}";; \
		esac; \
		grep -qxF "$$a" "$$f" && continue; \
		if grep -q "snipping-tool --graphic" "$$f"; then \
			t="$$f.snipping-tool.$$$$"; \
			grep -v -e "snipping-tool --graphic" -e "added by snipping-tool make install" "$$f" > "$$t"; \
			cat "$$t" > "$$f"; \
			rm -f "$$t"; \
		fi; \
		printf '%s\n%s\n' "# added by snipping-tool make install" "$$a" >> "$$f"; \
		echo "added to $$f: $$a"; \
	done; \
	if [ $$n = 0 ]; then \
		printf '%s\n%s\n' "# added by snipping-tool make install" "${ALIAS}" >> "$$h/.bashrc"; \
		echo "added to $$h/.bashrc: ${ALIAS}"; \
	fi; \
	echo "open a new terminal (or source the rc file) to use the alias;"; \
	echo "the command line version is still there as: command snipping-tool"

unalias:
	@h=$$HOME; \
	if [ -n "$$SUDO_USER" ]; then h=$$(eval echo "~$$SUDO_USER"); fi; \
	for rc in .bashrc .zshrc .kshrc .mkshrc .shrc .cshrc .tcshrc .config/fish/config.fish; do \
		f="$$h/$$rc"; \
		[ -f "$$f" ] && grep -q "snipping-tool --graphic" "$$f" || continue; \
		t="$$f.snipping-tool.$$$$"; \
		grep -v -e "snipping-tool --graphic" -e "added by snipping-tool make install" "$$f" > "$$t"; \
		cat "$$t" > "$$f"; \
		rm -f "$$t"; \
		echo "removed the alias from $$f"; \
	done

.PHONY: all static icons font ocrdb clean dist install uninstall alias unalias
