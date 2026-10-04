/* See LICENSE file for copyright and license details.
 *
 * gui.c - the graphical version (snipping-tool --graphic).
 *
 * Everything is drawn by hand into client side pixel buffers (struct surf)
 * that are put on the screen with XPutImage: no toolkit and no X fonts
 * (the font is built in, see font.h), so it looks the same under every
 * window manager.
 *
 * One window, two looks: the small main window (toolbar and instruction
 * text) and, after a snip, the markup window (menu bar, toolbar and the
 * snip with the pen, the highlighter and the eraser).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include "snip.h"
#include "config.h"
#include "font.h"
#include "icons.h"

enum { stmain, stedit };
enum { toolpen, toolmark, toolerase };
enum { penred, penblue, penblack, penmine };
enum { itemsep = 1, itemoff = 2, itemradio = 4, itemswatch = 8, itemon = 16 };
enum { ctllabel, ctlgroup, ctlcheck, ctlcombo, ctlbutton, ctlentry, ctltext, ctlicon };
enum {
	icapp16, icapp32, icapp48, icnew, iccancel, icoptions, icsave, iccopy,
	icsend, icpen, icmarker, iceraser, icocr, ichelp, iclast
};
enum {
	cmdnone, cmdnew, cmdmodes, cmdcancel, cmdoptions, cmdhelp, cmdabout,
	cmdsave, cmdcopy, cmdsend, cmdsends, cmdopen, cmdpen, cmdpens,
	cmdmark, cmderase, cmdocr, cmdundo, cmdexit, cmdcustom,
	cmdfree, cmdrect, cmdwin, cmdfull,	/* same order as snipfree.. */
	cmdred, cmdblue, cmdblack, cmdmine	/* same order as penred.. */
};
enum {
	menuh = 20,		/* menu bar */
	barh = 38,		/* toolbar */
	btnh = 30,		/* toolbar button */
	dropw = 14,		/* arrow part of a button */
	texth = 48,		/* instruction text */
	margin = 10,		/* space around the snip */
	sbw = 17,		/* scroll bar */
	lineh = 16,		/* text line */
	itemh = 22,		/* menu item */
	capy = 9,		/* height of a capital letter */
	tipdelay = 600		/* ms before a tooltip shows */
};

/* colors */
static const unsigned int colface = 0xf0f0f0;
static const unsigned int coltext = 0x1e1e1e;
static const unsigned int coloff = 0x8d8d8d;
static const unsigned int colbar0 = 0xfbfcfd, colbar1 = 0xe3e8ef;
static const unsigned int colmenubar = 0xf5f7fa;
static const unsigned int colline = 0xc3cbd6;
static const unsigned int colhot0 = 0xf6fbff, colhot1 = 0xd7e9fb, colhotb = 0x7eb2e6;
static const unsigned int coldown0 = 0xd3e6f9, coldown1 = 0xaecff1, coldownb = 0x5486c0;
static const unsigned int colinfo0 = 0xfbfcfe, colinfo1 = 0xe7eaf1;
static const unsigned int colcanvas = 0xffffff, colframe = 0x98adc8;
static const unsigned int colmenu = 0xf2f2f2, colgutter = 0xe8e8e8, colsep = 0xd4d4d4;
static const unsigned int colsel0 = 0xf0f6fd, colsel1 = 0xd6e7fb, colselb = 0xa5c6ee;
static const unsigned int colbtn0 = 0xf7f7f7, colbtn1 = 0xdcdcdc, colbtnb = 0x8a8a8a;
static const unsigned int colbox = 0xabadb3, colfocus = 0x3399ff;
static const unsigned int coltip0 = 0xffffff, coltip1 = 0xe4e5f0, coltipb = 0x767676;
static const unsigned int colsb = 0xeaeaea, colsbb = 0xb5b5b5;

struct surf {
	Window win;
	int w, h;
	unsigned int *px;
	XImage *xi;
	int own;		/* xi has its own data (screen is not 32 bit) */
};

struct btn {
	int cmd, drop;		/* commands of the button and of its arrow */
	int icon;
	const char *label;
	const char *tip;
	int sep;		/* separator before the button */
	int x, y, w, h;
};

struct item {
	const char *label;
	const char *key;
	int cmd;
	int flags;
	unsigned int col;
};

struct stroke {
	int *pt;		/* x, y pairs in snip coordinates */
	int n, cap;
	unsigned int col;
	int mark;		/* highlighter: multiplies the colors */
	int round;		/* round tip, else a chisel nib */
	double r;		/* round: radius */
	double ux, uy, a;	/* chisel: along the nib, half length */
	double vx, vy, b;	/* chisel: across the nib, half thickness */
	int x0, y0, x1, y1;	/* bounds */
};

struct undo {
	struct stroke st;	/* erased stroke */
	int idx;
	int add;		/* 1: the stroke idx was added, 0: st was erased */
};

struct ctl {
	int type;
	int x, y, w, h;
	const char *str;
	int val;		/* checked, list index, button is default */
	int id;			/* button result */
	const char **list;	/* combo choices */
	const unsigned int *cols;	/* combo color swatches */
	int nlist;
	char *buf;		/* entry text */
	int bufsz, cur, all;
};

struct dlg {
	struct surf s;
	struct ctl *c;
	int n;
	int hot, down, focus;
};

static void Common(XEvent *ev);
static void Do(int cmd);
static int Ask(const char *text, const char *b1, const char *b2, const char *b3);

static struct surf mainw;
static GC gc;
static int state = stmain;
static struct btn btns[12];
static int nbtns;
static int hotbtn = -1, hotpart, downbtn = -1, downpart;
static int hotmenu = -1, openmenu = -1, menux[5];
static struct img icons[iclast];
static struct img raw, snip, under, comp;	/* snip, with ink, all strokes */
static unsigned char *mask;			/* coverage of the stroke drawn now */
static struct stroke *strokes, cur;
static int nstrokes, capstrokes, drawing, erasing, erasex, erasey;
static struct undo *undos;
static int nundos, capundos;
static int tool = toolpen, pen = penred;
static int dirty;
static int vx, vy;				/* scroll position */
static int sbdrag, sbvert, sbgrab;		/* dragging a scroll bar thumb */
static int countdown, countmode;
static long countnext;
static struct surf tipw;
static int tipbtn = -1;
static long tipat;
static Cursor curarrow, curwait, curtool;
static int curshown = -1;
static struct surf *surfs[8];
static int nsurfs;
static int running = 1;
static Atom atomdel, atomname, atomicon, atomtype, atomnormal,
	atomdialog, atommenu, atomtooltip, atomstate, atommodal, atompid;

static const char *menutitles[4] = { "&File", "&Edit", "&Tools", "&Help" };

static struct item filemenu[] = {
	{ "&New Snip", "Ctrl+N", cmdnew, 0, 0 },
	{ "Save &As...", "Ctrl+S", cmdsave, 0, 0 },
	{ NULL, NULL, 0, itemsep, 0 },
	{ "Send by &E-mail", NULL, cmdsend, 0, 0 },
	{ "&Open in Another Program", NULL, cmdopen, 0, 0 },
	{ NULL, NULL, 0, itemsep, 0 },
	{ "E&xit", "Ctrl+Q", cmdexit, 0, 0 },
};
static struct item editmenu[] = {
	{ "&Undo", "Ctrl+Z", cmdundo, 0, 0 },
	{ "&Copy", "Ctrl+C", cmdcopy, 0, 0 },
};
static struct item toolmenu[] = {
	{ "&Red Pen", NULL, cmdred, itemradio, 0 },
	{ "&Blue Pen", NULL, cmdblue, itemradio, 0 },
	{ "Blac&k Pen", NULL, cmdblack, itemradio, 0 },
	{ "&Custom Pen", NULL, cmdmine, itemradio, 0 },
	{ "C&ustomize Pen...", NULL, cmdcustom, 0, 0 },
	{ NULL, NULL, 0, itemsep, 0 },
	{ "&Highlighter", NULL, cmdmark, itemradio, 0 },
	{ "&Eraser", NULL, cmderase, itemradio, 0 },
	{ NULL, NULL, 0, itemsep, 0 },
	{ "Recognize &Text", "Ctrl+T", cmdocr, 0, 0 },
	{ NULL, NULL, 0, itemsep, 0 },
	{ "&Options...", NULL, cmdoptions, 0, 0 },
};
static struct item helpmenu[] = {
	{ "Snipping Tool &Help", "F1", cmdhelp, 0, 0 },
	{ "&About Snipping Tool", NULL, cmdabout, 0, 0 },
};
static struct item modemenu[] = {
	{ "&Free-form Snip", NULL, cmdfree, itemradio, 0 },
	{ "&Rectangular Snip", NULL, cmdrect, itemradio, 0 },
	{ "&Window Snip", NULL, cmdwin, itemradio, 0 },
	{ "Full-&screen Snip", NULL, cmdfull, itemradio, 0 },
};
static struct item sendmenu[] = {
	{ "&E-mail Recipient (as Attachment)", NULL, cmdsend, 0, 0 },
	{ "&Open in Another Program", NULL, cmdopen, 0, 0 },
};
static struct item penmenu[] = {
	{ "&Red Pen", NULL, cmdred, itemradio, 0 },
	{ "&Blue Pen", NULL, cmdblue, itemradio, 0 },
	{ "Blac&k Pen", NULL, cmdblack, itemradio, 0 },
	{ "&Custom Pen", NULL, cmdmine, itemradio, 0 },
	{ NULL, NULL, 0, itemsep, 0 },
	{ "C&ustomize...", NULL, cmdcustom, 0, 0 },
};

static const char *helptext =
	"Snipping Tool takes a picture of a part of the screen (a snip) that "
	"you can draw on with a pen and a highlighter.\n"
	"\n"
	"Taking a snip\n"
	"Click the arrow next to the New button and choose the type of snip:\n"
	"  Free-form Snip - draw around a part of the screen;\n"
	"  Rectangular Snip - drag a rectangle (a click without dragging "
	"takes the window under the pointer);\n"
	"  Window Snip - click a window;\n"
	"  Full-screen Snip - the snip is taken at once.\n"
	"Esc or the right mouse button cancels the selection. A delay before "
	"the snip can be set in the Options dialog.\n"
	"\n"
	"Marking up\n"
	"The pen draws lines, the highlighter marks text with a see-through "
	"color, the eraser removes only what was drawn: the whole line it "
	"touches. The color, thickness and tip of the pen are set in "
	"Tools - Customize Pen.\n"
	"\n"
	"Recognize Text (OCR)\n"
	"The button with the letter T finds the text in the snip and copies "
	"it to the clipboard. Ordinary screen text is recognized best.\n"
	"\n"
	"Keys\n"
	"  Ctrl+N - new snip\n"
	"  Ctrl+S - save as\n"
	"  Ctrl+C - copy\n"
	"  Ctrl+Z - undo the last mark\n"
	"  Ctrl+T - recognize text\n"
	"  F1 - help\n"
	"\n"
	"Command line\n"
	"Without options snipping-tool selects a rectangle and puts the snip "
	"on the clipboard (image/png). -f - full screen, -w - active window, "
	"-l - free form, -s - also save it in the home directory, -t - "
	"recognize text, -d N - delay. More in man snipping-tool.";

static const char *abouttext =
	"Snipping Tool for X11, version 0.1\n"
	"\n"
	"Screen snips with a pen, a highlighter and text recognition.\n"
	"Plain C and Xlib, no other libraries.\n"
	"\n"
	"License: GNU GPL version 2 or later.\n"
	"Uses parts of scrot (MIT license) and gocr (GPL).\n"
	"Interface font: Noto Sans (SIL Open Font License 1.1).";

/* ---------------------------------------------------------------- */
/* small helpers */

static int
Max(int a, int b)
{
	return a > b ? a : b;
}

static int
Min(int a, int b)
{
	return a < b ? a : b;
}

static int
Utf8(const char *str, int *cp)
{
	const unsigned char *u = (const unsigned char *)str;

	if (u[0] < 0x80) {
		*cp = u[0];
		return 1;
	}
	if ((u[0] & 0xe0) == 0xc0 && (u[1] & 0xc0) == 0x80) {
		*cp = (u[0] & 0x1f) << 6 | (u[1] & 0x3f);
		return 2;
	}
	if ((u[0] & 0xf0) == 0xe0 && (u[1] & 0xc0) == 0x80 && (u[2] & 0xc0) == 0x80) {
		*cp = (u[0] & 0x0f) << 12 | (u[1] & 0x3f) << 6 | (u[2] & 0x3f);
		return 3;
	}
	if ((u[0] & 0xf8) == 0xf0 && (u[1] & 0xc0) == 0x80 &&
	    (u[2] & 0xc0) == 0x80 && (u[3] & 0xc0) == 0x80) {
		*cp = (u[0] & 0x07) << 18 | (u[1] & 0x3f) << 12 |
		      (u[2] & 0x3f) << 6 | (u[3] & 0x3f);
		return 4;
	}
	*cp = 0xfffd;
	return 1;
}

static int
PutUtf8(char *dst, int cp)
{
	if (cp < 0x80) {
		dst[0] = cp;
		return 1;
	}
	if (cp < 0x800) {
		dst[0] = 0xc0 | cp >> 6;
		dst[1] = 0x80 | (cp & 0x3f);
		return 2;
	}
	if (cp < 0x10000) {
		dst[0] = 0xe0 | cp >> 12;
		dst[1] = 0x80 | (cp >> 6 & 0x3f);
		dst[2] = 0x80 | (cp & 0x3f);
		return 3;
	}
	dst[0] = 0xf0 | cp >> 18;
	dst[1] = 0x80 | (cp >> 12 & 0x3f);
	dst[2] = 0x80 | (cp >> 6 & 0x3f);
	dst[3] = 0x80 | (cp & 0x3f);
	return 4;
}

static int
Lower(int cp)
{
	if ((cp >= 'A' && cp <= 'Z') || (cp >= 0x410 && cp <= 0x42f))
		return cp + 0x20;
	if (cp >= 0x400 && cp <= 0x40f)
		return cp + 0x50;
	return cp;
}

/* keysym to unicode: latin-1, cyrillic and the 0x1000000 range */
static int
KeyUcs(KeySym ks)
{
	static const unsigned short cyr1[31] = {	/* 0x6a1..0x6bf */
		0x452, 0x453, 0x451, 0x454, 0x455, 0x456, 0x457, 0x458, 0x459,
		0x45a, 0x45b, 0x45c, 0x491, 0x45e, 0x45f, 0x2116, 0x402, 0x403,
		0x401, 0x404, 0x405, 0x406, 0x407, 0x408, 0x409, 0x40a, 0x40b,
		0x40c, 0x490, 0x40e, 0x40f
	};
	static const unsigned short cyr2[32] = {	/* 0x6c0..0x6df */
		0x44e, 0x430, 0x431, 0x446, 0x434, 0x435, 0x444, 0x433, 0x445,
		0x438, 0x439, 0x43a, 0x43b, 0x43c, 0x43d, 0x43e, 0x43f, 0x44f,
		0x440, 0x441, 0x442, 0x443, 0x436, 0x432, 0x44c, 0x44b, 0x437,
		0x448, 0x44d, 0x449, 0x447, 0x44a
	};

	if ((ks >= 0x20 && ks <= 0x7e) || (ks >= 0xa0 && ks <= 0xff))
		return ks;
	if ((ks & 0xff000000) == 0x01000000)
		return ks & 0xffffff;
	if (ks >= 0x6a1 && ks <= 0x6bf)
		return cyr1[ks - 0x6a1];
	if (ks >= 0x6c0 && ks <= 0x6df)
		return cyr2[ks - 0x6c0];
	if (ks >= 0x6e0 && ks <= 0x6ff)
		return cyr2[ks - 0x6e0] - 0x20;
	return 0;
}

/* ---------------------------------------------------------------- */
/* surfaces */

static void
SurfSize(struct surf *s, int w, int h)
{
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	if (s->xi) {
		if (!s->own)
			s->xi->data = NULL;
		XDestroyImage(s->xi);
	}
	free(s->px);
	s->w = w;
	s->h = h;
	s->px = Ecalloc((size_t)w * h, sizeof(unsigned int));
	s->xi = XCreateImage(dpy, vis, depth, ZPixmap, 0, NULL, w, h, 32, 0);
	if (!s->xi)
		Die("cannot create an image");
	if (Native32(s->xi)) {
		s->xi->data = (char *)s->px;
		s->own = 0;
	} else {
		s->xi->data = Ecalloc((size_t)s->xi->bytes_per_line * h, 1);
		s->own = 1;
	}
}

static void
SurfFree(struct surf *s)
{
	if (s->xi) {
		if (!s->own)
			s->xi->data = NULL;
		XDestroyImage(s->xi);
	}
	free(s->px);
	s->xi = NULL;
	s->px = NULL;
}

static int
Clip(struct surf *s, int *x, int *y, int *w, int *h)
{
	if (*x < 0) {
		*w += *x;
		*x = 0;
	}
	if (*y < 0) {
		*h += *y;
		*y = 0;
	}
	if (*x + *w > s->w)
		*w = s->w - *x;
	if (*y + *h > s->h)
		*h = s->h - *y;
	return *w > 0 && *h > 0;
}

static void
Present(struct surf *s, int x, int y, int w, int h)
{
	int i, j;

	if (!s->win || !Clip(s, &x, &y, &w, &h))
		return;
	if (s->own)
		for (j = y; j < y + h; j++)
			for (i = x; i < x + w; i++)
				XPutPixel(s->xi, i, j, Pixel(s->px[j * s->w + i]));
	XPutImage(dpy, s->win, gc, s->xi, x, y, x, y, w, h);
}

static void
AddSurf(struct surf *s)
{
	if (nsurfs < (int)(sizeof(surfs) / sizeof(surfs[0])))
		surfs[nsurfs++] = s;
}

static void
DelSurf(struct surf *s)
{
	int i;

	for (i = 0; i < nsurfs; i++) {
		if (surfs[i] == s) {
			surfs[i] = surfs[--nsurfs];
			return;
		}
	}
}

static struct surf *
FindSurf(Window w)
{
	int i;

	for (i = 0; i < nsurfs; i++)
		if (surfs[i]->win == w)
			return surfs[i];
	return NULL;
}

/* ---------------------------------------------------------------- */
/* drawing */

static unsigned int
Mix(unsigned int a, unsigned int b, int t)
{
	int ra = a >> 16 & 0xff, ga = a >> 8 & 0xff, ba = a & 0xff;
	int rb = b >> 16 & 0xff, gb = b >> 8 & 0xff, bb = b & 0xff;

	return (unsigned int)(ra + (rb - ra) * t / 255) << 16 |
	       (unsigned int)(ga + (gb - ga) * t / 255) << 8 |
	       (unsigned int)(ba + (bb - ba) * t / 255);
}

static unsigned int
Mul(unsigned int a, unsigned int b)
{
	return ((a >> 16 & 0xff) * (b >> 16 & 0xff) / 255) << 16 |
	       ((a >> 8 & 0xff) * (b >> 8 & 0xff) / 255) << 8 |
	       ((a & 0xff) * (b & 0xff) / 255);
}

static void
Pix(struct surf *s, int x, int y, unsigned int col, int a)
{
	unsigned int *p;

	if (x < 0 || y < 0 || x >= s->w || y >= s->h || a <= 0)
		return;
	p = &s->px[y * s->w + x];
	*p = a >= 255 ? col : Mix(*p, col, a);
}

static void
Fill(struct surf *s, int x, int y, int w, int h, unsigned int col)
{
	int i, j;

	if (!Clip(s, &x, &y, &w, &h))
		return;
	for (j = y; j < y + h; j++)
		for (i = x; i < x + w; i++)
			s->px[j * s->w + i] = col;
}

static void
Grad(struct surf *s, int x, int y, int w, int h, unsigned int top,
     unsigned int bot)
{
	int j;

	for (j = 0; j < h; j++)
		Fill(s, x, y + j, w, 1, Mix(top, bot, h > 1 ? j * 255 / (h - 1) : 0));
}

static void
Box(struct surf *s, int x, int y, int w, int h, unsigned int col)
{
	Fill(s, x, y, w, 1, col);
	Fill(s, x, y + h - 1, w, 1, col);
	Fill(s, x, y, 1, h, col);
	Fill(s, x + w - 1, y, 1, h, col);
}

/* rounded face of a button: gradient and border */
static void
Face(struct surf *s, int x, int y, int w, int h, unsigned int top,
     unsigned int bot, unsigned int border)
{
	Grad(s, x + 1, y + 1, w - 2, h - 2, top, bot);
	Fill(s, x + 2, y + 1, w - 4, 1, Mix(top, 0xffffff, 160));
	Fill(s, x + 2, y, w - 4, 1, border);
	Fill(s, x + 2, y + h - 1, w - 4, 1, border);
	Fill(s, x, y + 2, 1, h - 4, border);
	Fill(s, x + w - 1, y + 2, 1, h - 4, border);
	Pix(s, x + 1, y + 1, border, 160);
	Pix(s, x + w - 2, y + 1, border, 160);
	Pix(s, x + 1, y + h - 2, border, 160);
	Pix(s, x + w - 2, y + h - 2, border, 160);
}

/* small triangle pointing down, centered at cx,cy */
static void
Arrow(struct surf *s, int cx, int cy, unsigned int col)
{
	int i;

	for (i = 0; i < 4; i++)
		Fill(s, cx - 3 + i, cy - 1 + i, 7 - 2 * i, 1, col);
}

/* the same pointing up, left or right */
static void
ArrowDir(struct surf *s, int cx, int cy, int dir, unsigned int col)
{
	int i;

	for (i = 0; i < 4; i++) {
		if (dir == 0)
			Fill(s, cx - i, cy - 2 + i, 2 * i + 1, 1, col);
		else if (dir == 1)
			Fill(s, cx - 3 + i, cy - 1 + i, 7 - 2 * i, 1, col);
		else if (dir == 2)
			Fill(s, cx - 2 + i, cy - i, 1, 2 * i + 1, col);
		else
			Fill(s, cx + 1 - i, cy - 3 + i, 1, 7 - 2 * i, col);
	}
}

static void
CheckMark(struct surf *s, int x, int y, unsigned int col)
{
	static const char *bits[7] = {
		".......#",
		"......##",
		"#....###",
		"##..###.",
		"######..",
		".####...",
		"..##....",
	};
	int i, j;

	for (j = 0; j < 7; j++)
		for (i = 0; i < 8; i++)
			if (bits[j][i] == '#')
				Pix(s, x + i, y + j, col, 255);
}

static void
Disc(struct surf *s, int cx, int cy, double r, unsigned int col)
{
	double d;
	int x, y, a;

	for (y = cy - (int)r - 1; y <= cy + (int)r + 1; y++) {
		for (x = cx - (int)r - 1; x <= cx + (int)r + 1; x++) {
			d = sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
			a = (int)((r + 0.5 - d) * 255);
			if (a > 255)
				a = 255;
			Pix(s, x, y, col, a);
		}
	}
}

static void
DrawIcon(struct surf *s, int ic, int x, int y, int gray)
{
	struct img *im = &icons[ic];
	unsigned int c, l;
	int i, j, a;

	for (j = 0; j < im->h; j++) {
		for (i = 0; i < im->w; i++) {
			c = im->px[j * im->w + i];
			a = c >> 24;
			if (gray) {
				l = ((c >> 16 & 0xff) * 30 + (c >> 8 & 0xff) * 59 +
				     (c & 0xff) * 11) / 100;
				l = l / 2 + 110;
				c = l << 16 | l << 8 | l;
				a = a * 3 / 5;
			}
			Pix(s, x + i, y + j, c & 0xffffff, a);
		}
	}
}

/* ---------------------------------------------------------------- */
/* text */

static const struct glyph *
Glyph(int cp)
{
	int lo = 0, hi = (int)(sizeof(glyphs) / sizeof(glyphs[0])) - 1, mid;

	if (cp == 0xa0)
		cp = ' ';
	while (lo <= hi) {
		mid = (lo + hi) / 2;
		if (glyphs[mid].cp == cp)
			return &glyphs[mid];
		if (glyphs[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return cp == '?' ? &glyphs[0] : Glyph('?');
}

/* next character of a label: '&' marks the access key, "&&" is '&' */
static int
LabelChar(const char **p, int *ul)
{
	int cp;

	*ul = 0;
	if ((*p)[0] == '&' && (*p)[1] == '&') {
		(*p)++;
	} else if ((*p)[0] == '&' && (*p)[1]) {
		(*p)++;
		*ul = 1;
	}
	*p += Utf8(*p, &cp);
	return cp;
}

static int
AccessKey(const char *label)
{
	int ul, cp;

	while (label && *label) {
		cp = LabelChar(&label, &ul);
		if (ul)
			return Lower(cp);
	}
	return 0;
}

/* does the key press the access key of label: the letter of the current
 * keyboard layout or of the first one, so that the Latin access keys
 * work while another layout is on */
static int
IsAccess(XKeyEvent *e, KeySym ks, const char *label)
{
	int k = AccessKey(label);

	if (k == 0)
		return 0;
	if (Lower(KeyUcs(ks)) == k)
		return 1;
	return Lower(KeyUcs(XkbKeycodeToKeysym(dpy, e->keycode, 0, 0))) == k;
}

static void
DrawGlyph(struct surf *s, const struct glyph *g, int x, int y,
          unsigned int col)
{
	int i, j, a;

	for (j = 0; j < g->h; j++) {
		for (i = 0; i < g->w; i++) {
			a = glyphdata[g->off + j * g->w + i];
			if (a)
				Pix(s, x + g->x + i, y + g->y + j, col, a);
		}
	}
}

/* label with an access key at x and baseline y, returns the end x */
static int
Text(struct surf *s, int x, int y, const char *str, unsigned int col)
{
	const struct glyph *g;
	int ul;

	while (*str) {
		g = Glyph(LabelChar(&str, &ul));
		DrawGlyph(s, g, x, y, col);
		if (ul)
			Fill(s, x, y + 2, g->adv, 1, col);
		x += g->adv;
	}
	return x;
}

static int
TextW(const char *str)
{
	int w = 0, ul;

	while (str && *str)
		w += Glyph(LabelChar(&str, &ul))->adv;
	return w;
}

/* plain text, n bytes (-1: all) */
static int
Plain(struct surf *s, int x, int y, const char *str, int n, unsigned int col)
{
	const struct glyph *g;
	int i = 0, cp;

	if (n < 0)
		n = strlen(str);
	while (i < n) {
		i += Utf8(str + i, &cp);
		if (cp == '\t')
			cp = ' ';
		g = Glyph(cp);
		DrawGlyph(s, g, x, y, col);
		x += g->adv;
	}
	return x;
}

static int
PlainW(const char *str, int n)
{
	int i = 0, w = 0, cp;

	if (n < 0)
		n = strlen(str);
	while (i < n) {
		i += Utf8(str + i, &cp);
		w += Glyph(cp == '\t' ? ' ' : cp)->adv;
	}
	return w;
}

/* split str into lines not wider than w. line i is str + st[i], ln[i]
 * bytes long; returns the number of lines (at most max) */
static int
Wrap(const char *str, int w, int *st, int *ln, int max)
{
	int n = 0, i = 0, ls = 0, lw = 0, brk = -1, cw, cp, k;

	while (str[i] && n < max) {
		if (str[i] == '\n') {
			st[n] = ls;
			ln[n++] = i - ls;
			ls = ++i;
			lw = 0;
			brk = -1;
			continue;
		}
		k = Utf8(str + i, &cp);
		cw = Glyph(cp == '\t' ? ' ' : cp)->adv;
		if (cp == ' ')
			brk = i;
		if (lw + cw > w && i > ls) {
			if (brk > ls) {
				st[n] = ls;
				ln[n++] = brk - ls;
				i = brk + 1;
			} else {
				st[n] = ls;
				ln[n++] = i - ls;
			}
			ls = i;
			lw = 0;
			brk = -1;
			continue;
		}
		lw += cw;
		i += k;
	}
	if (n < max && (i > ls || n == 0 || (i > 0 && str[i - 1] == '\n'))) {
		st[n] = ls;
		ln[n++] = i - ls;
	}
	return n;
}

/* ---------------------------------------------------------------- */
/* windows */

static void
SetTitle(Window w, const char *title)
{
	XChangeProperty(dpy, w, atomname, atomutf8, 8, PropModeReplace,
	                (const unsigned char *)title, strlen(title));
	XChangeProperty(dpy, w, XA_WM_NAME, atomutf8, 8, PropModeReplace,
	                (const unsigned char *)title, strlen(title));
}

static void
SetType(Window w, Atom type)
{
	XChangeProperty(dpy, w, atomtype, XA_ATOM, 32, PropModeReplace,
	                (unsigned char *)&type, 1);
}

static void
SetHints(Window w, int x, int y, int width, int height, int fixed)
{
	XSizeHints *hints;

	hints = XAllocSizeHints();
	if (!hints)
		return;
	hints->flags = PPosition | PSize | PMinSize;
	hints->x = x;
	hints->y = y;
	hints->width = width;
	hints->height = height;
	hints->min_width = fixed ? width : 330;
	hints->min_height = fixed ? height : 180;
	if (fixed) {
		hints->flags |= PMaxSize;
		hints->max_width = width;
		hints->max_height = height;
	}
	XSetWMNormalHints(dpy, w, hints);
	XFree(hints);
}

static void
SetIcon(Window w)
{
	static const int ic[3] = { icapp16, icapp32, icapp48 };
	unsigned long *data;
	size_t n = 0, i = 0, k;
	int j;

	for (j = 0; j < 3; j++)
		n += 2 + (size_t)icons[ic[j]].w * icons[ic[j]].h;
	data = Ecalloc(n, sizeof(unsigned long));
	for (j = 0; j < 3; j++) {
		data[i++] = icons[ic[j]].w;
		data[i++] = icons[ic[j]].h;
		for (k = 0; k < (size_t)icons[ic[j]].w * icons[ic[j]].h; k++)
			data[i++] = icons[ic[j]].px[k];
	}
	XChangeProperty(dpy, w, atomicon, XA_CARDINAL, 32, PropModeReplace,
	                (unsigned char *)data, n);
	free(data);
}

/* root position of a point in the main window */
static void
RootPos(int x, int y, int *rx, int *ry)
{
	Window dummy;

	if (!XTranslateCoordinates(dpy, mainw.win, root, x, y, rx, ry, &dummy)) {
		*rx = x;
		*ry = y;
	}
}

static void
WaitEvent(Window w, int type, int ms)
{
	XEvent ev;
	long t = Now();

	XSync(dpy, False);
	while (Now() - t < ms) {
		if (XCheckTypedWindowEvent(dpy, w, type, &ev))
			return;
		MSleep(5);
	}
}

static int
GrabAll(Window w, Cursor c)
{
	int i;

	for (i = 0; i < 50; i++) {
		if (XGrabPointer(dpy, w, False, ButtonPressMask | ButtonReleaseMask |
		                 PointerMotionMask, GrabModeAsync, GrabModeAsync,
		                 None, c, CurrentTime) == GrabSuccess)
			break;
		MSleep(5);
	}
	for (i = 0; i < 50; i++) {
		if (XGrabKeyboard(dpy, w, False, GrabModeAsync, GrabModeAsync,
		                  CurrentTime) == GrabSuccess)
			return 0;
		MSleep(5);
	}
	return -1;
}

/* override redirect window for menus and tooltips */
static void
PopupWindow(struct surf *s, int x, int y, int w, int h, Atom type)
{
	XSetWindowAttributes wa;

	wa.override_redirect = True;
	wa.save_under = True;
	wa.background_pixmap = None;
	wa.event_mask = ExposureMask;
	s->win = XCreateWindow(dpy, root, x, y, w, h, 0, depth, InputOutput, vis,
	                       CWOverrideRedirect | CWSaveUnder | CWBackPixmap |
	                       CWEventMask, &wa);
	SetType(s->win, type);
	SurfSize(s, w, h);
	AddSurf(s);
}

static void
PopupClose(struct surf *s)
{
	DelSurf(s);
	XDestroyWindow(dpy, s->win);
	s->win = None;
	SurfFree(s);
}

/* ---------------------------------------------------------------- */
/* popup menus */

static int
MenuOn(int cmd)
{
	if (cmd >= cmdfree && cmd <= cmdfull)
		return opt.mode == cmd - cmdfree;
	if (cmd >= cmdred && cmd <= cmdmine)
		return pen == cmd - cmdred && (tool == toolpen || state == stmain);
	if (cmd == cmdmark)
		return tool == toolmark;
	if (cmd == cmderase)
		return tool == toolerase;
	return 0;
}

static int
Enabled(int cmd)
{
	if (cmd == cmdcancel)
		return countdown > 0;
	if (cmd == cmdundo)
		return nundos > 0;
	return 1;
}

static void
MarkMenu(struct item *it, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (it[i].flags & itemsep)
			continue;
		it[i].flags &= ~(itemon | itemoff);
		if (MenuOn(it[i].cmd))
			it[i].flags |= itemon;
		if (!Enabled(it[i].cmd))
			it[i].flags |= itemoff;
	}
}

static int
ItemY(struct item *it, int i)
{
	int k, y = 2;

	for (k = 0; k < i; k++)
		y += (it[k].flags & itemsep) ? 7 : itemh;
	return y;
}

static int
ItemAt(struct item *it, int n, int x, int y, int w)
{
	int i, iy;

	if (x < 0 || x >= w)
		return -1;
	for (i = 0; i < n; i++) {
		iy = ItemY(it, i);
		if (y >= iy && y < iy + ((it[i].flags & itemsep) ? 7 : itemh))
			return (it[i].flags & itemsep) ? -1 : i;
	}
	return -1;
}

static void
DrawPopup(struct surf *s, struct item *it, int n, int sel)
{
	unsigned int col;
	int i, y;

	Fill(s, 0, 0, s->w, s->h, colmenu);
	Fill(s, 2, 2, 26, s->h - 4, colgutter);
	Fill(s, 28, 2, 1, s->h - 4, colsep);
	Fill(s, 29, 2, 1, s->h - 4, 0xffffff);
	Box(s, 0, 0, s->w, s->h, 0x979797);
	for (i = 0; i < n; i++) {
		y = ItemY(it, i);
		if (it[i].flags & itemsep) {
			Fill(s, 30, y + 3, s->w - 32, 1, colsep);
			Fill(s, 30, y + 4, s->w - 32, 1, 0xffffff);
			continue;
		}
		col = (it[i].flags & itemoff) ? coloff : coltext;
		if (i == sel && !(it[i].flags & itemoff))
			Face(s, 2, y, s->w - 4, itemh, colsel0, colsel1, colselb);
		if (it[i].flags & itemswatch) {
			Fill(s, 7, y + 4, 15, 14, 0x707070);
			Fill(s, 8, y + 5, 13, 12, it[i].col);
		} else if ((it[i].flags & itemon) && (it[i].flags & itemradio)) {
			Disc(s, 14, y + 11, 3, col);
		}
		if ((it[i].flags & itemon) && (it[i].flags & itemswatch))
			Box(s, 4, y + 1, 21, 20, colselb);
		Text(s, 36, y + 15, it[i].label, col);
		if (it[i].key)
			Text(s, s->w - 14 - TextW(it[i].key), y + 15, it[i].key, col);
	}
}

static int
TitleAt(XRectangle *t, int n, int x, int y)
{
	int i;

	for (i = 0; t && i < n; i++)
		if (x >= t[i].x && y >= t[i].y && x < t[i].x + t[i].width &&
		    y < t[i].y + t[i].height)
			return i;
	return -1;
}

static void
MoveSel(struct item *it, int n, int *sel, int d)
{
	int i, k = *sel;

	for (i = 0; i < n; i++) {
		k = k < 0 ? (d > 0 ? 0 : n - 1) : (k + d + n) % n;
		if (!(it[k].flags & (itemsep | itemoff))) {
			*sel = k;
			return;
		}
	}
}

/* a popup menu on the screen */
struct menu {
	struct surf s;
	struct item *it;
	int n, x, y, sel, moved, self, ntitles;
	XRectangle *titles;
	long t0;
};

static int
MenuInside(struct menu *m, int rx, int ry)
{
	return rx >= m->x && ry >= m->y && rx < m->x + m->s.w && ry < m->y + m->s.h;
}

static void
MenuSelect(struct menu *m, int sel)
{
	if (sel == m->sel)
		return;
	m->sel = sel;
	DrawPopup(&m->s, m->it, m->n, sel);
	Present(&m->s, 0, 0, m->s.w, m->s.h);
}

/* the functions below return the result of the menu or -1000 to go on */
static int
MenuMotion(struct menu *m, XMotionEvent *e)
{
	int k;

	if (MenuInside(m, e->x_root, e->y_root))
		m->moved = 1;
	k = ItemAt(m->it, m->n, e->x_root - m->x, e->y_root - m->y, m->s.w);
	if (k >= 0 && (m->it[k].flags & itemoff))
		k = -1;
	if (MenuInside(m, e->x_root, e->y_root) || k < 0)
		MenuSelect(m, k);
	/* the pointer went to another title of the menu bar */
	k = TitleAt(m->titles, m->ntitles, e->x_root, e->y_root);
	if (k >= 0 && k != m->self)
		return -1 - k;
	return -1000;
}

static int
MenuPress(struct menu *m, XButtonEvent *e)
{
	int k;

	if (MenuInside(m, e->x_root, e->y_root))
		return -1000;
	k = TitleAt(m->titles, m->ntitles, e->x_root, e->y_root);
	return (k >= 0 && k != m->self) ? -1 - k : 0;
}

static int
MenuRelease(struct menu *m, XButtonEvent *e)
{
	int k;

	k = ItemAt(m->it, m->n, e->x_root - m->x, e->y_root - m->y, m->s.w);
	if (MenuInside(m, e->x_root, e->y_root)) {
		if (k >= 0 && !(m->it[k].flags & itemoff))
			return m->it[k].cmd;
		return -1000;
	}
	/* the release of the click that opened the menu keeps it open */
	if (m->moved && Now() - m->t0 > 300 &&
	    TitleAt(m->titles, m->ntitles, e->x_root, e->y_root) != m->self)
		return 0;
	return -1000;
}

static int
MenuKey(struct menu *m, XKeyEvent *e)
{
	KeySym ks;
	char tmp[8];
	int i;

	XLookupString(e, tmp, sizeof(tmp), &ks, NULL);
	if (ks == XK_Escape)
		return 0;
	if (ks == XK_Up || ks == XK_Down) {
		i = m->sel;
		MoveSel(m->it, m->n, &i, ks == XK_Up ? -1 : 1);
		MenuSelect(m, i);
		return -1000;
	}
	if (ks == XK_Return || ks == XK_KP_Enter)
		return m->sel >= 0 ? m->it[m->sel].cmd : -1000;
	if ((ks == XK_Left || ks == XK_Right) && m->titles)
		return -1 - (m->self + (ks == XK_Left ? m->ntitles - 1 : 1)) % m->ntitles;
	for (i = 0; i < m->n; i++)
		if (!(m->it[i].flags & (itemsep | itemoff)) && IsAccess(e, ks, m->it[i].label))
			return m->it[i].cmd;
	return -1000;
}

/* run a popup menu at root x,y. Returns the command of the chosen item or
 * 0. titles are the menu bar titles (root coordinates) when the menu
 * belongs to the menu bar: moving onto another title returns -1 - title */
static int
Popup(struct item *it, int n, int x, int y, int minw, XRectangle *titles,
      int ntitles, int self)
{
	struct menu m;
	XEvent ev;
	int w, h, i, lw = 0, kw = 0, ret = -1000;

	for (i = 0; i < n; i++) {
		if (it[i].flags & itemsep)
			continue;
		lw = Max(lw, TextW(it[i].label));
		if (it[i].key)
			kw = Max(kw, TextW(it[i].key));
	}
	w = Max(minw, 36 + lw + (kw ? 36 + kw : 0) + 22);
	h = ItemY(it, n) + 2;
	x = Max(0, Min(x, sw - w));
	if (y + h > sh)
		y = Max(0, sh - h);

	memset(&m, 0, sizeof(m));
	m.it = it;
	m.n = n;
	m.x = x;
	m.y = y;
	m.sel = -1;
	m.self = self;
	m.titles = titles;
	m.ntitles = ntitles;
	m.t0 = Now();
	PopupWindow(&m.s, x, y, w, h, atommenu);
	DrawPopup(&m.s, it, n, -1);
	XMapRaised(dpy, m.s.win);
	GrabAll(m.s.win, curarrow);
	while (ret == -1000) {
		XNextEvent(dpy, &ev);
		if (ev.type == MotionNotify) {
			while (XCheckTypedEvent(dpy, MotionNotify, &ev))
				;
			ret = MenuMotion(&m, &ev.xmotion);
		} else if (ev.type == ButtonPress) {
			ret = MenuPress(&m, &ev.xbutton);
		} else if (ev.type == ButtonRelease) {
			ret = MenuRelease(&m, &ev.xbutton);
		} else if (ev.type == KeyPress) {
			ret = MenuKey(&m, &ev.xkey);
		} else {
			Common(&ev);
		}
	}
	XUngrabKeyboard(dpy, CurrentTime);
	XUngrabPointer(dpy, CurrentTime);
	PopupClose(&m.s);
	XFlush(dpy);
	return ret;
}

/* ---------------------------------------------------------------- */
/* dialogs */

static int
CtlAt(struct dlg *d, int x, int y)
{
	int i;

	for (i = 0; i < d->n; i++)
		if (x >= d->c[i].x && y >= d->c[i].y && x < d->c[i].x + d->c[i].w &&
		    y < d->c[i].y + d->c[i].h && d->c[i].type != ctllabel &&
		    d->c[i].type != ctlgroup && d->c[i].type != ctlicon)
			return i;
	return -1;
}

static void
DrawCombo(struct surf *s, struct ctl *c, int hot)
{
	int tx = c->x + 5;

	Fill(s, c->x, c->y, c->w, c->h, 0xffffff);
	Box(s, c->x, c->y, c->w, c->h, hot ? colhotb : colbox);
	if (hot)
		Face(s, c->x + c->w - 18, c->y + 1, 17, c->h - 2, colhot0, colhot1, colhotb);
	Arrow(s, c->x + c->w - 10, c->y + c->h / 2 - 1, coltext);
	if (c->cols) {
		Fill(s, c->x + 4, c->y + 4, 16, c->h - 8, 0x707070);
		Fill(s, c->x + 5, c->y + 5, 14, c->h - 10, c->cols[c->val]);
		tx += 20;
	}
	if (c->val >= 0 && c->val < c->nlist)
		Text(s, tx, c->y + (c->h + capy) / 2, c->list[c->val], coltext);
}

static void
DrawEntry(struct surf *s, struct ctl *c, int focus)
{
	int tw, cw, off, x;

	Fill(s, c->x, c->y, c->w, c->h, 0xffffff);
	Box(s, c->x, c->y, c->w, c->h, focus ? colfocus : colbox);
	tw = PlainW(c->buf, -1);
	cw = PlainW(c->buf, c->cur);
	off = 0;
	if (cw > c->w - 10)
		off = cw - (c->w - 10);
	x = c->x + 4 - off;
	if (c->all && focus && c->buf[0]) {
		Fill(s, Max(x, c->x + 2), c->y + 3, Min(tw, c->w - 6), c->h - 6, colfocus);
		Plain(s, x, c->y + (c->h + capy) / 2, c->buf, -1, 0xffffff);
	} else {
		Plain(s, x, c->y + (c->h + capy) / 2, c->buf, -1, coltext);
	}
	/* the text may run out of the box, repaint the border area */
	Fill(s, c->x + 1, c->y + 1, 2, c->h - 2, 0xffffff);
	Fill(s, c->x + c->w - 3, c->y + 1, 2, c->h - 2, 0xffffff);
	Box(s, c->x, c->y, c->w, c->h, focus ? colfocus : colbox);
	if (focus && !c->all)
		Fill(s, x + cw, c->y + 4, 1, c->h - 8, coltext);
}

static int
TextLines(struct ctl *c, int *st, int *ln, int max)
{
	return Wrap(c->str, c->w - 22, st, ln, max);
}

static void
DrawTextView(struct surf *s, struct ctl *c)
{
	static int st[4096], ln[4096];
	int n, shown, i, th, ty;

	Fill(s, c->x, c->y, c->w, c->h, 0xffffff);
	Box(s, c->x, c->y, c->w, c->h, colbox);
	n = TextLines(c, st, ln, 4096);
	shown = (c->h - 8) / lineh;
	if (c->val > n - shown)
		c->val = Max(0, n - shown);
	for (i = 0; i < shown && c->val + i < n; i++)
		Plain(s, c->x + 6, c->y + 4 + lineh * i + 12, c->str + st[c->val + i],
		      ln[c->val + i], coltext);
	if (n > shown) {
		Fill(s, c->x + c->w - 13, c->y + 1, 12, c->h - 2, colsb);
		th = Max(20, (c->h - 4) * shown / n);
		ty = c->y + 2 + (c->h - 4 - th) * c->val / (n - shown);
		Face(s, c->x + c->w - 12, ty, 10, th, 0xf3f3f3, 0xd6d6d6, colsbb);
	}
}

static void
DrawCtl(struct dlg *d, int i)
{
	struct surf *s = &d->s;
	struct ctl *c = &d->c[i];
	int st[16], ln[16], n, k, hot = d->hot == i, down = d->down == i;

	switch (c->type) {
	case ctllabel:
		n = Wrap(c->str, c->w, st, ln, 16);
		for (k = 0; k < n; k++)
			Plain(s, c->x, c->y + 12 + k * lineh, c->str + st[k], ln[k], coltext);
		break;
	case ctlgroup:
		Box(s, c->x, c->y + 7, c->w, c->h - 7, 0xd5dfe5);
		Box(s, c->x + 1, c->y + 8, c->w - 2, c->h - 9, 0xffffff);
		Fill(s, c->x + 6, c->y, TextW(c->str) + 6, 15, colface);
		Text(s, c->x + 9, c->y + 11, c->str, 0x1e3287);
		break;
	case ctlcheck:
		Box(s, c->x, c->y + 3, 13, 13, hot ? 0x3c7fb1 : 0x8e8f8f);
		Grad(s, c->x + 1, c->y + 4, 11, 11, hot ? 0xe8f4fd : 0xe9e9e9,
		     hot ? 0xffffff : 0xfdfdfd);
		if (c->val)
			CheckMark(s, c->x + 3, c->y + 6, 0x1e3f73);
		Text(s, c->x + 19, c->y + 14, c->str, coltext);
		break;
	case ctlcombo:
		DrawCombo(s, c, hot);
		break;
	case ctlbutton:
		if (down && hot)
			Face(s, c->x, c->y, c->w, c->h, coldown0, coldown1, coldownb);
		else if (hot)
			Face(s, c->x, c->y, c->w, c->h, colhot0, colhot1, colhotb);
		else
			Face(s, c->x, c->y, c->w, c->h, colbtn0, colbtn1,
			     c->val ? colfocus : colbtnb);
		Text(s, c->x + (c->w - TextW(c->str)) / 2, c->y + (c->h + capy) / 2,
		     c->str, coltext);
		break;
	case ctlentry:
		DrawEntry(s, c, d->focus == i);
		break;
	case ctltext:
		DrawTextView(s, c);
		break;
	case ctlicon:
		DrawIcon(s, c->val, c->x, c->y, 0);
		break;
	}
}

static void
DrawDlg(struct dlg *d)
{
	int i;

	Fill(&d->s, 0, 0, d->s.w, d->s.h, colface);
	for (i = 0; i < d->n; i++)
		DrawCtl(d, i);
}

static void
DlgOpen(struct dlg *d, const char *title, int w, int h)
{
	XSetWindowAttributes wa;
	XClassHint ch = { "snipping-tool", "Snipping-tool" };
	XWMHints wmh;
	Atom modal = atommodal;
	int x, y, i;

	RootPos(0, 0, &x, &y);
	x += (mainw.w - w) / 2;
	y += (mainw.h - h) / 2;
	x = Max(0, Min(x, sw - w));
	y = Max(0, Min(y, sh - h));
	wa.background_pixmap = None;
	wa.bit_gravity = NorthWestGravity;
	wa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
	                ButtonReleaseMask | PointerMotionMask | LeaveWindowMask;
	d->s.win = XCreateWindow(dpy, root, x, y, w, h, 0, depth, InputOutput, vis,
	                         CWBackPixmap | CWBitGravity | CWEventMask, &wa);
	wmh.flags = InputHint;
	wmh.input = True;
	XSetWMHints(dpy, d->s.win, &wmh);
	XSetClassHint(dpy, d->s.win, &ch);
	XSetTransientForHint(dpy, d->s.win, mainw.win);
	SetTitle(d->s.win, title);
	SetHints(d->s.win, x, y, w, h, 1);
	SetType(d->s.win, atomdialog);
	XChangeProperty(dpy, d->s.win, atomstate, XA_ATOM, 32, PropModeReplace,
	                (unsigned char *)&modal, 1);
	XSetWMProtocols(dpy, d->s.win, &atomdel, 1);
	SurfSize(&d->s, w, h);
	AddSurf(&d->s);
	d->hot = d->down = -1;
	d->focus = -1;
	for (i = 0; i < d->n; i++)
		if (d->c[i].type == ctlentry && d->focus < 0)
			d->focus = i;
	DrawDlg(d);
	XMapRaised(dpy, d->s.win);
	WaitEvent(d->s.win, Expose, 300);
	Present(&d->s, 0, 0, w, h);
	XSetInputFocus(dpy, d->s.win, RevertToParent, CurrentTime);
}

static void
DlgClose(struct dlg *d)
{
	DelSurf(&d->s);
	XDestroyWindow(dpy, d->s.win);
	SurfFree(&d->s);
	XFlush(dpy);
}

static void
Redraw(struct dlg *d, int i)
{
	struct ctl *c;

	if (i < 0 || i >= d->n)
		return;
	c = &d->c[i];
	Fill(&d->s, c->x - 2, c->y - 2, c->w + 4, c->h + 4, colface);
	DrawCtl(d, i);
	Present(&d->s, c->x - 2, c->y - 2, c->w + 4, c->h + 4);
}

static void
ComboPopup(struct dlg *d, int i)
{
	struct ctl *c = &d->c[i];
	struct item *it;
	Window dummy;
	int rx, ry, k, r;

	it = Ecalloc(c->nlist, sizeof(struct item));
	for (k = 0; k < c->nlist; k++) {
		it[k].label = c->list[k];
		it[k].cmd = k + 1;
		it[k].flags = c->cols ? itemswatch : itemradio;
		if (k == c->val)
			it[k].flags |= itemon;
		it[k].col = c->cols ? c->cols[k] : 0;
	}
	XTranslateCoordinates(dpy, d->s.win, root, c->x, c->y + c->h, &rx, &ry, &dummy);
	r = Popup(it, c->nlist, rx, ry, c->w, NULL, 0, -1);
	if (r > 0)
		c->val = r - 1;
	free(it);
	Redraw(d, i);
}

/* tab completion of a path in an entry */
static void
Complete(struct ctl *c)
{
	char dir[PATH_MAX], path[PATH_MAX], best[256];
	const char *home = getenv("HOME"), *pre, *slash;
	struct dirent *de;
	struct stat st;
	DIR *dp;
	size_t pl, k;
	int cnt = 0;

	if (c->buf[0] == '~' && c->buf[1] == '/' && home)
		snprintf(path, sizeof(path), "%s%s", home, c->buf + 1);
	else
		snprintf(path, sizeof(path), "%s", c->buf);
	slash = strrchr(path, '/');
	if (slash) {
		snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path + 1), path);
		pre = slash + 1;
	} else {
		snprintf(dir, sizeof(dir), "./");
		pre = path;
	}
	pl = strlen(pre);
	dp = opendir(dir);
	if (!dp)
		return;
	best[0] = '\0';
	while ((de = readdir(dp))) {
		if (strncmp(de->d_name, pre, pl) != 0)
			continue;
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (de->d_name[0] == '.' && pre[0] != '.')
			continue;
		if (cnt++ == 0) {
			snprintf(best, sizeof(best), "%s", de->d_name);
		} else {
			for (k = 0; best[k] && best[k] == de->d_name[k]; k++)
				;
			best[k] = '\0';
		}
	}
	closedir(dp);
	if (cnt == 0)
		return;
	pl = slash ? strlen(dir) : 0;
	if (pl + strlen(best) + 2 > sizeof(path))
		return;
	memcpy(path, dir, pl);
	strcpy(path + pl, best);
	if (cnt == 1 && stat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
	    strlen(path) + 1 < sizeof(path))
		strcat(path, "/");
	snprintf(c->buf, c->bufsz, "%s", path);
	c->cur = strlen(c->buf);
	c->all = 0;
}

/* editing keys of an entry; returns 1 if the text changed or moved */
static int
EntryKey(struct ctl *c, KeySym ks, int ctrl)
{
	char u[4];
	int cp, n, len = strlen(c->buf);

	if (ctrl && (ks == XK_a || ks == XK_A)) {
		c->all = 1;
		return 1;
	}
	if (ks == XK_Tab) {
		Complete(c);
		return 1;
	}
	if (ks == XK_Left || ks == XK_Home) {
		if (ks == XK_Home)
			c->cur = 0;
		else if (c->cur > 0)
			while (--c->cur > 0 && (c->buf[c->cur] & 0xc0) == 0x80)
				;
		c->all = 0;
		return 1;
	}
	if (ks == XK_Right || ks == XK_End) {
		if (ks == XK_End)
			c->cur = len;
		else if (c->cur < len)
			while (++c->cur < len && (c->buf[c->cur] & 0xc0) == 0x80)
				;
		c->all = 0;
		return 1;
	}
	if (c->all && (ks == XK_BackSpace || ks == XK_Delete || KeyUcs(ks) >= 0x20)) {
		c->buf[0] = '\0';
		c->cur = 0;
		len = 0;
		c->all = 0;
		if (ks == XK_BackSpace || ks == XK_Delete)
			return 1;
	}
	if (ks == XK_BackSpace && c->cur > 0) {
		n = c->cur;
		while (--c->cur > 0 && (c->buf[c->cur] & 0xc0) == 0x80)
			;
		memmove(c->buf + c->cur, c->buf + n, len - n + 1);
		return 1;
	}
	if (ks == XK_Delete && c->cur < len) {
		n = c->cur;
		while (++n < len && (c->buf[n] & 0xc0) == 0x80)
			;
		memmove(c->buf + c->cur, c->buf + n, len - n + 1);
		return 1;
	}
	cp = KeyUcs(ks);
	if (ctrl || cp < 0x20 || cp == 0x7f)
		return 0;
	n = PutUtf8(u, cp);
	if (len + n + 1 > c->bufsz)
		return 0;
	memmove(c->buf + c->cur + n, c->buf + c->cur, len - c->cur + 1);
	memcpy(c->buf + c->cur, u, n);
	c->cur += n;
	return 1;
}

static void
ScrollText(struct dlg *d, int i, int lines)
{
	d->c[i].val = Max(0, d->c[i].val + lines);
	Redraw(d, i);
}

static void
DlgHot(struct dlg *d, int k)
{
	int old = d->hot;

	if (k == old)
		return;
	d->hot = k;
	Redraw(d, old);
	Redraw(d, k);
}

/* the handlers return the id of a pressed button, -1 when the dialog is
 * closed or -1000 to go on */
static int
DlgPress(struct dlg *d, XButtonEvent *e)
{
	struct ctl *c;
	int k, old;

	k = CtlAt(d, e->x, e->y);
	if (k < 0)
		return -1000;
	c = &d->c[k];
	if (c->type == ctltext && (e->button == Button4 || e->button == Button5)) {
		ScrollText(d, k, e->button == Button4 ? -3 : 3);
		return -1000;
	}
	if (e->button != Button1)
		return -1000;
	if (c->type == ctlcombo) {
		ComboPopup(d, k);
	} else if (c->type == ctlentry) {
		old = d->focus;
		d->focus = k;
		c->all = 0;
		c->cur = strlen(c->buf);
		Redraw(d, old);
		Redraw(d, k);
	} else {
		d->down = k;
		Redraw(d, k);
	}
	return -1000;
}

static int
DlgRelease(struct dlg *d, XButtonEvent *e)
{
	int k = d->down;

	d->down = -1;
	if (k < 0)
		return -1000;
	Redraw(d, k);
	if (k != CtlAt(d, e->x, e->y))
		return -1000;
	if (d->c[k].type == ctlbutton)
		return d->c[k].id;
	if (d->c[k].type == ctlcheck) {
		d->c[k].val = !d->c[k].val;
		Redraw(d, k);
	}
	return -1000;
}

static int
DlgKey(struct dlg *d, XKeyEvent *e)
{
	KeySym ks;
	char tmp[8];
	int i;

	XLookupString(e, tmp, sizeof(tmp), &ks, NULL);
	if (ks == XK_Escape)
		return -1;
	/* Enter presses the default button */
	if (ks == XK_Return || ks == XK_KP_Enter) {
		for (i = 0; i < d->n; i++)
			if (d->c[i].type == ctlbutton && d->c[i].val)
				return d->c[i].id;
		return -1000;
	}
	/* the underlined letter presses a button (with Alt next to a text field) */
	for (i = 0; i < d->n; i++)
		if (d->c[i].type == ctlbutton && IsAccess(e, ks, d->c[i].str) &&
		    (d->focus < 0 || (e->state & Mod1Mask)))
			return d->c[i].id;
	for (i = 0; i < d->n; i++) {
		if (d->c[i].type != ctltext)
			continue;
		if (ks == XK_Up || ks == XK_Down)
			ScrollText(d, i, ks == XK_Up ? -1 : 1);
		else if (ks == XK_Prior || ks == XK_Next)
			ScrollText(d, i, ks == XK_Prior ? -10 : 10);
	}
	if (d->focus >= 0 && EntryKey(&d->c[d->focus], ks, e->state & ControlMask))
		Redraw(d, d->focus);
	return -1000;
}

/* run a dialog until a button is pressed: returns its id, -1 if closed */
static int
DlgRun(struct dlg *d)
{
	XEvent ev;
	int ret = -1000;

	while (ret == -1000) {
		XNextEvent(dpy, &ev);
		if (ev.xany.window != d->s.win) {
			/* the main window waits for the dialog */
			if (ev.type == ClientMessage && ev.xany.window == mainw.win)
				XRaiseWindow(dpy, d->s.win);
			Common(&ev);
			continue;
		}
		if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == atomdel)
			ret = -1;
		else if (ev.type == MotionNotify)
			DlgHot(d, CtlAt(d, ev.xmotion.x, ev.xmotion.y));
		else if (ev.type == LeaveNotify)
			DlgHot(d, -1);
		else if (ev.type == ButtonPress)
			ret = DlgPress(d, &ev.xbutton);
		else if (ev.type == ButtonRelease)
			ret = DlgRelease(d, &ev.xbutton);
		else if (ev.type == KeyPress)
			ret = DlgKey(d, &ev.xkey);
		else
			Common(&ev);
	}
	return ret;
}

static void
Btn(struct ctl *c, int x, int y, const char *label, int id, int def)
{
	memset(c, 0, sizeof(*c));
	c->type = ctlbutton;
	c->x = x;
	c->y = y;
	c->w = Max(75, TextW(label) + 20);
	c->h = 23;
	c->str = label;
	c->id = id;
	c->val = def;
}

static void
Ctl(struct ctl *c, int type, int x, int y, int w, int h, const char *str, int val)
{
	memset(c, 0, sizeof(*c));
	c->type = type;
	c->x = x;
	c->y = y;
	c->w = w;
	c->h = h;
	c->str = str;
	c->val = val;
}

/* message box with up to three buttons, returns 1..3 or 0 */
static int
Ask(const char *text, const char *b1, const char *b2, const char *b3)
{
	struct ctl c[5];
	struct dlg d;
	const char *bl[3];
	int st[16], ln[16], n, w, h, nb = 0, i, bx, r;

	bl[0] = b1;
	bl[1] = b2;
	bl[2] = b3;
	while (nb < 3 && bl[nb])
		nb++;
	w = 360;
	n = Wrap(text, w - 70, st, ln, 16);
	h = Max(n * lineh, 32) + 30 + 50;
	Ctl(&c[0], ctlicon, 16, 18, 32, 32, NULL, icapp32);
	Ctl(&c[1], ctllabel, 60, 22, w - 76, n * lineh + 4, text, 0);
	bx = w - 12;
	for (i = nb - 1; i >= 0; i--) {
		Btn(&c[2 + i], 0, h - 35, bl[i], i + 1, i == 0);
		bx -= c[2 + i].w;
		c[2 + i].x = bx;
		bx -= 8;
	}
	memset(&d, 0, sizeof(d));
	d.c = c;
	d.n = 2 + nb;
	DlgOpen(&d, "Snipping Tool", w, h);
	r = DlgRun(&d);
	DlgClose(&d);
	return r < 0 ? 0 : r;
}

static void
TextDlg(const char *title, const char *text, const char *note, int copy)
{
	struct ctl c[4];
	struct dlg d;
	int w = 520, h = 360, n = 0, r;

	Ctl(&c[n++], ctltext, 12, 12, w - 24, h - 64 - (note ? 18 : 0), text, 0);
	if (note)
		Ctl(&c[n++], ctllabel, 12, h - 70, w - 24, 18, note, 0);
	Btn(&c[n], 0, h - 35, "Close", 2, !copy);
	c[n].x = w - 12 - c[n].w;
	n++;
	if (copy) {
		Btn(&c[n], 0, h - 35, "&Copy", 1, 1);
		c[n].x = c[n - 1].x - 8 - c[n].w;
		n++;
	}
	memset(&d, 0, sizeof(d));
	d.c = c;
	d.n = n;
	DlgOpen(&d, title, w, h);
	while ((r = DlgRun(&d)) == 1) {
		if (ClipCopy((const unsigned char *)text, strlen(text), 1) < 0)
			Ask("Could not copy the text to the clipboard.", "OK", NULL, NULL);
	}
	DlgClose(&d);
}

static int
PaletteIndex(unsigned int col)
{
	int i, n = sizeof(palette) / sizeof(palette[0]);

	for (i = 0; i < n; i++)
		if (palette[i].col == col)
			return i;
	return 0;
}

static void
PaletteLists(const char **names, unsigned int *cols)
{
	int i, n = sizeof(palette) / sizeof(palette[0]);

	for (i = 0; i < n; i++) {
		names[i] = palette[i].name;
		cols[i] = palette[i].col;
	}
}

/* ---------------------------------------------------------------- */
/* strokes */

static double
Reach(struct stroke *st)
{
	return st->round ? st->r : st->a + st->b;
}

static void
StrokeInit(struct stroke *st, unsigned int col, int mark, int round, double w)
{
	memset(st, 0, sizeof(*st));
	st->col = col;
	st->mark = mark;
	st->round = round;
	if (mark) {
		st->ux = 1;
		st->uy = 0;
		st->a = markwidth / 2.0;
		st->vx = 0;
		st->vy = 1;
		st->b = markheight / 2.0;
	} else if (round) {
		st->r = w / 2.0;
	} else {
		st->ux = 0.7071;
		st->uy = -0.7071;
		st->a = w * 0.9;
		st->vx = 0.7071;
		st->vy = 0.7071;
		st->b = w * 0.2 < 0.6 ? 0.6 : w * 0.2;
	}
	st->x0 = st->y0 = INT_MAX;
	st->x1 = st->y1 = INT_MIN;
}

static void
StrokeAdd(struct stroke *st, int x, int y)
{
	int e = (int)Reach(st) + 2;

	if (st->n == st->cap) {
		st->cap = st->cap ? st->cap * 2 : 64;
		st->pt = Erealloc(st->pt, st->cap * 2 * sizeof(int));
	}
	st->pt[2 * st->n] = x;
	st->pt[2 * st->n + 1] = y;
	st->n++;
	st->x0 = Min(st->x0, x - e);
	st->y0 = Min(st->y0, y - e);
	st->x1 = Max(st->x1, x + e + 1);
	st->y1 = Max(st->y1, y + e + 1);
}

/* convex hull, counter clockwise (Andrew's monotone chain) */
static int
Hull(double *px, double *py, int n, double *hx, double *hy)
{
	int idx[8], i, j, k = 0, t, lo;
	double cr;

	for (i = 0; i < n; i++)
		idx[i] = i;
	for (i = 1; i < n; i++) {
		t = idx[i];
		for (j = i; j > 0 && (px[idx[j - 1]] > px[t] ||
		     (px[idx[j - 1]] == px[t] && py[idx[j - 1]] > py[t])); j--)
			idx[j] = idx[j - 1];
		idx[j] = t;
	}
	for (lo = 0; lo < 2; lo++) {
		t = k + 1;
		for (j = 0; j < n; j++) {
			i = lo ? idx[n - 1 - j] : idx[j];
			if (lo && j == 0)
				continue;
			while (k >= (lo ? t : 2)) {
				cr = (hx[k - 1] - hx[k - 2]) * (py[i] - hy[k - 2]) -
				     (hy[k - 1] - hy[k - 2]) * (px[i] - hx[k - 2]);
				if (cr > 0)
					break;
				k--;
			}
			hx[k] = px[i];
			hy[k] = py[i];
			k++;
		}
	}
	return k - 1;
}

static void
Cover(unsigned char *p, double c)
{
	int v;

	if (c <= 0)
		return;
	v = c >= 1 ? 255 : (int)(c * 255);
	if (v > *p)
		*p = v;
}

/* coverage of segment i (points i-1..i) of st into the mask m, which
 * covers the rectangle starting at mx,my and is mw wide; only pixels in
 * cx0..cx1, cy0..cy1 (end excluded) are touched */
static void
SegCover(struct stroke *st, int i, unsigned char *m, int mx, int my, int mw,
         int cx0, int cy0, int cx1, int cy1)
{
	double ax, ay, bx, by, dx, dy, l2, t, qx, qy, e;
	double px[8], py[8], hx[10], hy[10], nx[9], ny[9], len, sd, v;
	int x, y, x0, y0, x1, y1, k, nh = 0;

	bx = st->pt[2 * i];
	by = st->pt[2 * i + 1];
	ax = i > 0 ? st->pt[2 * i - 2] : bx;
	ay = i > 0 ? st->pt[2 * i - 1] : by;
	e = Reach(st) + 2;
	x0 = Max(cx0, (int)floor((ax < bx ? ax : bx) - e));
	y0 = Max(cy0, (int)floor((ay < by ? ay : by) - e));
	x1 = Min(cx1, (int)ceil((ax > bx ? ax : bx) + e) + 1);
	y1 = Min(cy1, (int)ceil((ay > by ? ay : by) + e) + 1);
	if (x0 >= x1 || y0 >= y1)
		return;
	dx = bx - ax;
	dy = by - ay;
	l2 = dx * dx + dy * dy;
	if (!st->round) {
		for (k = 0; k < 4; k++) {
			qx = ((k & 1) ? st->a : -st->a) * st->ux +
			     ((k & 2) ? st->b : -st->b) * st->vx;
			qy = ((k & 1) ? st->a : -st->a) * st->uy +
			     ((k & 2) ? st->b : -st->b) * st->vy;
			px[k] = ax + qx;
			py[k] = ay + qy;
			px[k + 4] = bx + qx;
			py[k + 4] = by + qy;
		}
		nh = Hull(px, py, 8, hx, hy);
		for (k = 0; k < nh; k++) {
			nx[k] = hy[k + 1] - hy[k];
			ny[k] = -(hx[k + 1] - hx[k]);
			len = sqrt(nx[k] * nx[k] + ny[k] * ny[k]);
			if (len > 0) {
				nx[k] /= len;
				ny[k] /= len;
			}
		}
	}
	for (y = y0; y < y1; y++) {
		for (x = x0; x < x1; x++) {
			if (st->round) {
				t = l2 > 0 ? ((x - ax) * dx + (y - ay) * dy) / l2 : 0;
				if (t < 0)
					t = 0;
				if (t > 1)
					t = 1;
				qx = x - ax - t * dx;
				qy = y - ay - t * dy;
				v = st->r + 0.5 - sqrt(qx * qx + qy * qy);
			} else {
				sd = -1e9;
				for (k = 0; k < nh; k++) {
					t = (x - hx[k]) * nx[k] + (y - hy[k]) * ny[k];
					if (t > sd)
						sd = t;
				}
				v = 0.5 - sd;
			}
			Cover(&m[(y - my) * mw + (x - mx)], v);
		}
	}
}

static unsigned int
InkPix(unsigned int c, struct stroke *st, int a)
{
	return Mix(c, st->mark ? Mul(c, st->col) : st->col, a);
}

static double
SegDist(double px, double py, double ax, double ay, double bx, double by)
{
	double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy, t = 0;

	if (l2 > 0)
		t = ((px - ax) * dx + (py - ay) * dy) / l2;
	if (t < 0)
		t = 0;
	if (t > 1)
		t = 1;
	dx = px - ax - t * dx;
	dy = py - ay - t * dy;
	return sqrt(dx * dx + dy * dy);
}

static int
Hits(struct stroke *st, int x, int y)
{
	double reach = Reach(st) + eraserad;
	int i;

	if (x < st->x0 - eraserad || y < st->y0 - eraserad ||
	    x > st->x1 + eraserad || y > st->y1 + eraserad)
		return 0;
	for (i = 0; i < st->n; i++)
		if (SegDist(x, y, st->pt[2 * (i > 0 ? i - 1 : 0)],
		            st->pt[2 * (i > 0 ? i - 1 : 0) + 1],
		            st->pt[2 * i], st->pt[2 * i + 1]) <= reach)
			return 1;
	return 0;
}

static void
PushUndo(struct stroke *st, int idx, int add)
{
	if (nundos == capundos) {
		capundos = capundos ? capundos * 2 : 32;
		undos = Erealloc(undos, capundos * sizeof(struct undo));
	}
	memset(&undos[nundos], 0, sizeof(struct undo));
	if (st)
		undos[nundos].st = *st;
	undos[nundos].idx = idx;
	undos[nundos].add = add;
	nundos++;
}

static void
ClearStrokes(void)
{
	int i;

	for (i = 0; i < nstrokes; i++)
		free(strokes[i].pt);
	for (i = 0; i < nundos; i++)
		if (!undos[i].add)
			free(undos[i].st.pt);
	nstrokes = 0;
	nundos = 0;
	free(cur.pt);
	memset(&cur, 0, sizeof(cur));
	drawing = 0;
	erasing = 0;
}

/* ---------------------------------------------------------------- */
/* layout of the main window */

static int
Top(void)
{
	return state == stedit ? menuh : 0;
}

static int
CanvasY(void)
{
	return menuh + barh;
}

static void
AddBtn(int cmd, int drop, int icon, const char *label, const char *tip, int sep)
{
	struct btn *b = &btns[nbtns++];

	b->cmd = cmd;
	b->drop = drop;
	b->icon = icon;
	b->label = label;
	b->tip = tip;
	b->sep = sep;
}

static void
Layout(void)
{
	struct btn *b;
	int i, x = 4;

	nbtns = 0;
	if (state == stmain) {
		AddBtn(cmdnew, cmdmodes, icnew, "&New", "Create a new snip", 0);
		AddBtn(cmdcancel, 0, iccancel, "&Cancel", "Cancel the snip", 0);
		AddBtn(cmdoptions, 0, icoptions, "&Options", "Snipping Tool options", 0);
	} else {
		AddBtn(cmdnew, cmdmodes, icnew, "&New", "Create a new snip (Ctrl+N)", 0);
		AddBtn(cmdsave, 0, icsave, NULL, "Save the snip (Ctrl+S)", 0);
		AddBtn(cmdcopy, 0, iccopy, NULL, "Copy (Ctrl+C)", 0);
		AddBtn(cmdsend, cmdsends, icsend, NULL, "Send the snip", 0);
		AddBtn(cmdpen, cmdpens, icpen, NULL, "Pen", 1);
		AddBtn(cmdmark, 0, icmarker, NULL, "Highlighter", 0);
		AddBtn(cmderase, 0, iceraser, NULL, "Eraser", 0);
		AddBtn(cmdocr, 0, icocr, NULL, "Recognize text (Ctrl+T)", 1);
	}
	for (i = 0; i < nbtns; i++) {
		b = &btns[i];
		if (b->sep)
			x += 9;
		b->x = x;
		b->y = Top() + (barh - btnh) / 2;
		b->h = btnh;
		b->w = 32;
		if (b->label)
			b->w += 2 + TextW(b->label) + 8;
		if (b->drop)
			b->w += dropw;
		x += b->w + 2;
	}
}

static int
BarWidth(void)
{
	return btns[nbtns - 1].x + btns[nbtns - 1].w + 6;
}

/* size of the small main window */
static void
MainSize(int *w, int *h)
{
	*w = Max(BarWidth(), 300);
	*h = barh + (opt.hidetext ? 0 : texth);
}

static int
BtnAt(int x, int y, int *part)
{
	struct btn *b;
	int i;

	for (i = 0; i < nbtns; i++) {
		b = &btns[i];
		if (x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h) {
			*part = b->drop && x >= b->x + b->w - dropw;
			return i;
		}
	}
	return -1;
}

/* ---------------------------------------------------------------- */
/* drawing the main window */

static void
BarBg(int x, int w)
{
	Grad(&mainw, x, Top(), w, barh - 1, colbar0, colbar1);
	Fill(&mainw, x, Top() + barh - 1, w, 1, colline);
}

static void
DrawBtn(int i)
{
	struct btn *b = &btns[i];
	int on = Enabled(b->cmd), mw = b->w - (b->drop ? dropw : 0);
	int checked = (b->cmd == cmdpen && tool == toolpen) ||
	              (b->cmd == cmdmark && tool == toolmark) ||
	              (b->cmd == cmderase && tool == toolerase);
	unsigned int pc;

	BarBg(b->x - 1, b->w + 2);
	if (b->sep) {
		Fill(&mainw, b->x - 6, b->y + 3, 1, b->h - 6, 0xb6c0cc);
		Fill(&mainw, b->x - 5, b->y + 3, 1, b->h - 6, 0xffffff);
	}
	if (on && downbtn == i) {
		Face(&mainw, b->x, b->y, b->w, b->h, colhot0, colhot1, colhotb);
		if (downpart)
			Face(&mainw, b->x + mw - 1, b->y, dropw + 1, b->h, coldown0, coldown1, coldownb);
		else
			Face(&mainw, b->x, b->y, mw, b->h, coldown0, coldown1, coldownb);
	} else if (on && checked) {
		Face(&mainw, b->x, b->y, b->w, b->h, coldown0, coldown1, coldownb);
		if (b->drop)
			Fill(&mainw, b->x + mw - 1, b->y + 2, 1, b->h - 4, coldownb);
	} else if (on && hotbtn == i) {
		Face(&mainw, b->x, b->y, b->w, b->h, colhot0, colhot1, colhotb);
		if (b->drop)
			Fill(&mainw, b->x + mw - 1, b->y + 2, 1, b->h - 4, colhotb);
	}
	DrawIcon(&mainw, b->icon, b->x + 4, b->y + 3, !on);
	if (b->cmd == cmdpen) {
		pc = pen == penmine ? opt.pencol : pencols[pen];
		Fill(&mainw, b->x + 20, b->y + 20, 8, 7, 0xffffff);
		Fill(&mainw, b->x + 21, b->y + 21, 6, 5, pc);
	}
	if (b->label)
		Text(&mainw, b->x + 32, b->y + (b->h + capy) / 2, b->label,
		     on ? coltext : coloff);
	if (b->drop)
		Arrow(&mainw, b->x + b->w - dropw / 2 - 1, b->y + b->h / 2 - 1,
		      on ? coltext : coloff);
}

static void
DrawBar(void)
{
	int i;

	BarBg(0, mainw.w);
	for (i = 0; i < nbtns; i++)
		DrawBtn(i);
}

static void
PresentBar(void)
{
	Present(&mainw, 0, Top(), mainw.w, barh);
}

static void
DrawMenuBar(void)
{
	int i, x = 2, w;

	Fill(&mainw, 0, 0, mainw.w, menuh, colmenubar);
	for (i = 0; i < 4; i++) {
		w = TextW(menutitles[i]) + 14;
		menux[i] = x;
		if (i == openmenu)
			Face(&mainw, x, 1, w, menuh - 2, coldown0, coldown1, coldownb);
		else if (i == hotmenu)
			Face(&mainw, x, 1, w, menuh - 2, colhot0, colhot1, colhotb);
		Text(&mainw, x + 7, 14, menutitles[i], coltext);
		x += w;
	}
	menux[4] = x;
}

static int
TitleIndex(int x)
{
	int i;

	for (i = 0; i < 4; i++)
		if (x >= menux[i] && x < menux[i + 1])
			return i;
	return -1;
}

static void
DrawInfo(void)
{
	char buf[256];
	const char *msg;
	int st[4], ln[4], n, i, y = barh;

	if (state != stmain)
		return;
	Grad(&mainw, 0, y, mainw.w, mainw.h - y, colinfo0, colinfo1);
	if (countdown > 0) {
		snprintf(buf, sizeof(buf), "The snip will be taken in %d s. "
		         "Click Cancel to stop it.", countdown);
		msg = buf;
	} else {
		msg = "Select a snip type from the menu or click the New button.";
	}
	n = Wrap(msg, mainw.w - 50, st, ln, 4);
	for (i = 0; i < n; i++)
		Plain(&mainw, 12, y + 19 + i * lineh, msg + st[i], ln[i], coltext);
	DrawIcon(&mainw, ichelp, mainw.w - 30, y + 9, 0);
}

static void
View(int *vw, int *vh, int *hb, int *vb)
{
	int cw = snip.w + 2 * margin, ch = snip.h + 2 * margin;
	int w = mainw.w, h = mainw.h - CanvasY();

	*vb = ch > h;
	*hb = cw > w - (*vb ? sbw : 0);
	*vb = ch > h - (*hb ? sbw : 0);
	*vw = w - (*vb ? sbw : 0);
	*vh = h - (*hb ? sbw : 0);
}

static void
ClampScroll(void)
{
	int vw, vh, hb, vb;

	View(&vw, &vh, &hb, &vb);
	vx = Max(0, Min(vx, snip.w + 2 * margin - vw));
	vy = Max(0, Min(vy, snip.h + 2 * margin - vh));
}

/* repaint the canvas part of the window rectangle x,y,w,h */
static void
DrawCanvas(int x, int y, int w, int h)
{
	unsigned int *row;
	int vw, vh, hb, vb, i, j, cx, cy, top = CanvasY();

	if (state != stedit)
		return;
	View(&vw, &vh, &hb, &vb);
	if (y < top) {
		h -= top - y;
		y = top;
	}
	if (x + w > vw)
		w = vw - x;
	if (y + h > top + vh)
		h = top + vh - y;
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (w <= 0 || h <= 0)
		return;
	for (j = y; j < y + h; j++) {
		cy = j - top + vy - margin;
		row = &mainw.px[j * mainw.w];
		for (i = x; i < x + w; i++) {
			cx = i + vx - margin;
			if (cx >= 0 && cy >= 0 && cx < comp.w && cy < comp.h)
				row[i] = comp.px[cy * comp.w + cx];
			else if (cx >= -1 && cy >= -1 && cx <= comp.w && cy <= comp.h)
				row[i] = colframe;
			else
				row[i] = colcanvas;
		}
	}
}

/* thumb position and length of a scroll bar */
static void
Thumb(int len, int pos, int total, int view, int *tp, int *tl)
{
	int track = len - 2 * sbw;

	*tl = Max(18, track * view / total);
	if (*tl > track)
		*tl = track;
	*tp = sbw;
	if (total > view)
		*tp += (track - *tl) * pos / (total - view);
}

static void
DrawSb(int x, int y, int len, int vert, int pos, int total, int view)
{
	int tp, tl;

	if (vert)
		Fill(&mainw, x, y, sbw, len, colsb);
	else
		Fill(&mainw, x, y, len, sbw, colsb);
	Thumb(len, pos, total, view, &tp, &tl);
	if (vert) {
		ArrowDir(&mainw, x + sbw / 2, y + sbw / 2, 0, 0x606060);
		ArrowDir(&mainw, x + sbw / 2, y + len - sbw / 2 - 1, 1, 0x606060);
		Face(&mainw, x + 2, y + tp, sbw - 4, tl, 0xf5f5f5, 0xdadada, colsbb);
	} else {
		ArrowDir(&mainw, x + sbw / 2, y + sbw / 2, 2, 0x606060);
		ArrowDir(&mainw, x + len - sbw / 2 - 1, y + sbw / 2, 3, 0x606060);
		Face(&mainw, x + tp, y + 2, tl, sbw - 4, 0xf5f5f5, 0xdadada, colsbb);
	}
}

static void
DrawScroll(void)
{
	int vw, vh, hb, vb, top = CanvasY();

	if (state != stedit)
		return;
	View(&vw, &vh, &hb, &vb);
	if (vb)
		DrawSb(vw, top, vh, 1, vy, snip.h + 2 * margin, vh);
	if (hb)
		DrawSb(0, top + vh, vw, 0, vx, snip.w + 2 * margin, vw);
	if (vb && hb)
		Fill(&mainw, vw, top + vh, sbw, sbw, colface);
}

static void
DrawAll(void)
{
	if (state == stedit) {
		DrawMenuBar();
		DrawBar();
		DrawCanvas(0, CanvasY(), mainw.w, mainw.h - CanvasY());
		DrawScroll();
	} else {
		DrawBar();
		DrawInfo();
	}
}

static void
PresentAll(void)
{
	Present(&mainw, 0, 0, mainw.w, mainw.h);
}

/* show the snip rectangle x0,y0..x1,y1 after comp changed there */
static void
ShowSnip(int x0, int y0, int x1, int y1)
{
	int wx = x0 + margin - vx, wy = y0 + margin - vy + CanvasY();

	DrawCanvas(wx, wy, x1 - x0, y1 - y0);
	Present(&mainw, wx, wy, x1 - x0, y1 - y0);
}

static void
Scroll(int dx, int dy)
{
	int ox = vx, oy = vy;

	vx += dx;
	vy += dy;
	ClampScroll();
	if (vx == ox && vy == oy)
		return;
	DrawCanvas(0, CanvasY(), mainw.w, mainw.h - CanvasY());
	DrawScroll();
	Present(&mainw, 0, CanvasY(), mainw.w, mainw.h - CanvasY());
}

/* the canvas with the active stroke */
static void
DrawSeg(void)
{
	unsigned int *u, *c;
	unsigned char *m;
	int x0, y0, x1, y1, x, y, i = cur.n - 1;
	double e = Reach(&cur) + 2;

	x0 = Max(0, (int)floor(Min(cur.pt[2 * i], cur.pt[2 * (i > 0 ? i - 1 : 0)]) - e));
	y0 = Max(0, (int)floor(Min(cur.pt[2 * i + 1], cur.pt[2 * (i > 0 ? i - 1 : 0) + 1]) - e));
	x1 = Min(snip.w, (int)ceil(Max(cur.pt[2 * i], cur.pt[2 * (i > 0 ? i - 1 : 0)]) + e) + 1);
	y1 = Min(snip.h, (int)ceil(Max(cur.pt[2 * i + 1], cur.pt[2 * (i > 0 ? i - 1 : 0) + 1]) + e) + 1);
	if (x0 >= x1 || y0 >= y1)
		return;
	SegCover(&cur, i, mask, 0, 0, snip.w, x0, y0, x1, y1);
	for (y = y0; y < y1; y++) {
		u = &under.px[y * snip.w];
		c = &comp.px[y * snip.w];
		m = &mask[y * snip.w];
		for (x = x0; x < x1; x++)
			c[x] = m[x] ? InkPix(u[x], &cur, m[x]) : u[x];
	}
	ShowSnip(x0, y0, x1, y1);
}

/* rebuild under and comp in a rectangle from the snip and the strokes */
static void
Rebuild(int x0, int y0, int x1, int y1)
{
	unsigned char *m;
	struct stroke *st;
	int w, h, x, y, k, i;

	x0 = Max(0, x0);
	y0 = Max(0, y0);
	x1 = Min(snip.w, x1);
	y1 = Min(snip.h, y1);
	w = x1 - x0;
	h = y1 - y0;
	if (w <= 0 || h <= 0)
		return;
	m = Ecalloc((size_t)w * h, 1);
	for (y = y0; y < y1; y++)
		memcpy(&under.px[y * snip.w + x0], &snip.px[y * snip.w + x0], w * 4);
	for (k = 0; k < nstrokes; k++) {
		st = &strokes[k];
		if (st->x1 <= x0 || st->y1 <= y0 || st->x0 >= x1 || st->y0 >= y1)
			continue;
		memset(m, 0, (size_t)w * h);
		for (i = 0; i < st->n; i++)
			SegCover(st, i, m, x0, y0, w, x0, y0, x1, y1);
		for (y = y0; y < y1; y++)
			for (x = x0; x < x1; x++)
				if (m[(y - y0) * w + x - x0])
					under.px[y * snip.w + x] = InkPix(under.px[y * snip.w + x],
						st, m[(y - y0) * w + x - x0]);
	}
	for (y = y0; y < y1; y++)
		memcpy(&comp.px[y * snip.w + x0], &under.px[y * snip.w + x0], w * 4);
	free(m);
	ShowSnip(x0, y0, x1, y1);
}

static void
StartStroke(int x, int y)
{
	unsigned int col;
	double w;
	int round;

	if (tool == toolmark) {
		StrokeInit(&cur, markcol, 1, 0, 0);
	} else {
		if (pen == penmine) {
			col = opt.pencol;
			w = penwidths[opt.penw];
			round = !opt.pentip;
		} else {
			col = pencols[pen];
			w = penwidths[1];
			round = 1;
		}
		StrokeInit(&cur, col, 0, round, w);
	}
	StrokeAdd(&cur, x, y);
	drawing = 1;
	DrawSeg();
}

static void
EndStroke(void)
{
	int x0, y0, x1, y1, y;

	drawing = 0;
	x0 = Max(0, cur.x0);
	y0 = Max(0, cur.y0);
	x1 = Min(snip.w, cur.x1);
	y1 = Min(snip.h, cur.y1);
	for (y = y0; y < y1 && x0 < x1; y++) {
		memcpy(&under.px[y * snip.w + x0], &comp.px[y * snip.w + x0], (x1 - x0) * 4);
		memset(&mask[y * snip.w + x0], 0, x1 - x0);
	}
	if (nstrokes == capstrokes) {
		capstrokes = capstrokes ? capstrokes * 2 : 32;
		strokes = Erealloc(strokes, capstrokes * sizeof(struct stroke));
	}
	strokes[nstrokes] = cur;
	PushUndo(NULL, nstrokes, 1);
	nstrokes++;
	memset(&cur, 0, sizeof(cur));
	dirty = 1;
}

static void EraseAt(int x, int y);

/* erase along the way from the last eraser position, motion events can
 * be far apart */
static void
EraseTo(int x, int y)
{
	int n, i, d;

	d = Max(abs(x - erasex), abs(y - erasey));
	n = d / 2 + 1;
	for (i = 1; i <= n; i++)
		EraseAt(erasex + (x - erasex) * i / n, erasey + (y - erasey) * i / n);
	erasex = x;
	erasey = y;
}

static void
EraseAt(int x, int y)
{
	struct stroke st;
	int k;

	for (k = nstrokes - 1; k >= 0; k--) {
		if (!Hits(&strokes[k], x, y))
			continue;
		st = strokes[k];
		memmove(&strokes[k], &strokes[k + 1], (nstrokes - k - 1) * sizeof(struct stroke));
		nstrokes--;
		PushUndo(&st, k, 0);
		Rebuild(st.x0, st.y0, st.x1, st.y1);
		dirty = 1;
		return;
	}
}

static void
Undo(void)
{
	struct undo *u;
	struct stroke st;

	if (nundos == 0 || drawing)
		return;
	u = &undos[--nundos];
	if (u->add) {
		st = strokes[u->idx];
		memmove(&strokes[u->idx], &strokes[u->idx + 1],
		        (nstrokes - u->idx - 1) * sizeof(struct stroke));
		nstrokes--;
		Rebuild(st.x0, st.y0, st.x1, st.y1);
		free(st.pt);
	} else {
		if (nstrokes == capstrokes) {
			capstrokes = capstrokes ? capstrokes * 2 : 32;
			strokes = Erealloc(strokes, capstrokes * sizeof(struct stroke));
		}
		memmove(&strokes[u->idx + 1], &strokes[u->idx],
		        (nstrokes - u->idx) * sizeof(struct stroke));
		strokes[u->idx] = u->st;
		nstrokes++;
		Rebuild(u->st.x0, u->st.y0, u->st.x1, u->st.y1);
	}
	dirty = 1;
}

/* ---------------------------------------------------------------- */
/* cursors */

static Cursor
MakeCursor(int w, int h, int hx, int hy, const unsigned char *src,
           const unsigned char *msk, unsigned int fg, unsigned int bg)
{
	XColor cf, cb;
	Pixmap ps, pm;
	Cursor c;

	ps = XCreateBitmapFromData(dpy, root, (const char *)src, w, h);
	pm = XCreateBitmapFromData(dpy, root, (const char *)msk, w, h);
	cf.red = (fg >> 16 & 0xff) * 257;
	cf.green = (fg >> 8 & 0xff) * 257;
	cf.blue = (fg & 0xff) * 257;
	cb.red = (bg >> 16 & 0xff) * 257;
	cb.green = (bg >> 8 & 0xff) * 257;
	cb.blue = (bg & 0xff) * 257;
	cf.flags = cb.flags = DoRed | DoGreen | DoBlue;
	c = XCreatePixmapCursor(dpy, ps, pm, &cf, &cb, hx, hy);
	XFreePixmap(dpy, ps);
	XFreePixmap(dpy, pm);
	return c;
}

static void
SetBit(unsigned char *b, int w, int x, int y)
{
	b[y * ((w + 7) / 8) + x / 8] |= 1 << (x % 8);
}

/* cursor of the current tool: pen color dot, highlighter nib, eraser */
static void
ToolCursor(void)
{
	unsigned char src[32 * 4], msk[32 * 4];
	unsigned int fg, bg = 0xffffff;
	int x, y, r, c = 15, in, out;

	memset(src, 0, sizeof(src));
	memset(msk, 0, sizeof(msk));
	if (tool == toolmark) {
		fg = markcol;
		bg = 0x000000;
		for (y = 0; y < 32; y++) {
			for (x = 0; x < 32; x++) {
				in = abs(x - c) <= markwidth / 2 && abs(y - c) <= markheight / 2;
				out = abs(x - c) <= markwidth / 2 + 1 && abs(y - c) <= markheight / 2 + 1;
				if (in)
					SetBit(src, 32, x, y);
				if (out)
					SetBit(msk, 32, x, y);
			}
		}
	} else if (tool == toolerase) {
		fg = 0x000000;
		r = eraserad + 1;
		for (y = 0; y < 32; y++) {
			for (x = 0; x < 32; x++) {
				out = abs(x - c) <= r && abs(y - c) <= r;
				in = abs(x - c) < r && abs(y - c) < r;
				if (out && !in)
					SetBit(src, 32, x, y);
				if (out)
					SetBit(msk, 32, x, y);
			}
		}
	} else {
		fg = pen == penmine ? opt.pencol : pencols[pen];
		if (fg == 0xffffff)
			bg = 0x000000;
		r = (pen == penmine ? penwidths[opt.penw] : penwidths[1]) / 2;
		r = Max(r, 1);
		for (y = 0; y < 32; y++) {
			for (x = 0; x < 32; x++) {
				in = (x - c) * (x - c) + (y - c) * (y - c) <= r * r + r;
				out = (x - c) * (x - c) + (y - c) * (y - c) <= (r + 1) * (r + 1) + r + 1;
				if (in)
					SetBit(src, 32, x, y);
				if (out)
					SetBit(msk, 32, x, y);
			}
		}
	}
	if (curtool)
		XFreeCursor(dpy, curtool);
	curtool = MakeCursor(32, 32, c, c, src, msk, fg, bg);
	curshown = -1;
}

static void
ShowCursor(int over)
{
	if (over == curshown)
		return;
	curshown = over;
	XDefineCursor(dpy, mainw.win, over ? curtool : curarrow);
}

static void
Busy(int on)
{
	XDefineCursor(dpy, mainw.win, on ? curwait : (curshown == 1 ? curtool : curarrow));
	XFlush(dpy);
}

/* ---------------------------------------------------------------- */
/* tooltips */

static void
HideTip(void)
{
	if (tipw.win)
		PopupClose(&tipw);
	tipbtn = -1;
}

static void
ShowTip(void)
{
	struct btn *b;
	int x, y, w, h = 20;

	if (tipbtn < 0 || tipbtn >= nbtns || tipw.win)
		return;
	b = &btns[tipbtn];
	if (!b->tip)
		return;
	w = TextW(b->tip) + 12;
	RootPos(b->x, b->y + b->h + 4, &x, &y);
	if (x + w > sw)
		x = sw - w;
	PopupWindow(&tipw, x, y, w, h, atomtooltip);
	Grad(&tipw, 0, 0, w, h, coltip0, coltip1);
	Box(&tipw, 0, 0, w, h, coltipb);
	Text(&tipw, 6, 14, b->tip, 0x575757);
	XMapRaised(dpy, tipw.win);
}

/* ---------------------------------------------------------------- */
/* main window */

static void
Show(void)
{
	int x, y;

	x = (sw - mainw.w) / 2;
	y = state == stmain ? (sh - mainw.h) / 3 : (sh - mainw.h) / 2;
	x = Max(0, x);
	y = Max(0, y);
	/* fixed size while mapping: tiling window managers float the window */
	SetHints(mainw.win, x, y, mainw.w, mainw.h, 1);
	XMoveResizeWindow(dpy, mainw.win, x, y, mainw.w, mainw.h);
	XMapRaised(dpy, mainw.win);
	WaitEvent(mainw.win, MapNotify, 1000);
	if (state == stedit)
		SetHints(mainw.win, x, y, mainw.w, mainw.h, 0);
	DrawAll();
	PresentAll();
	XSetInputFocus(dpy, mainw.win, RevertToParent, CurrentTime);
}

static void
Hide(void)
{
	HideTip();
	XUnmapWindow(dpy, mainw.win);
	WaitEvent(mainw.win, UnmapNotify, 500);
	MSleep(hidedelay);
}

static void
Resized(int w, int h)
{
	if (w == mainw.w && h == mainw.h)
		return;
	SurfSize(&mainw, w, h);
	ClampScroll();
	DrawAll();
	PresentAll();
}

static void
SetMainTitle(void)
{
	char buf[64];

	if (countdown > 0)
		snprintf(buf, sizeof(buf), "Snipping Tool - %d", countdown);
	else
		snprintf(buf, sizeof(buf), "Snipping Tool");
	SetTitle(mainw.win, buf);
}

static void
Refresh(void)
{
	DrawAll();
	PresentAll();
	SetMainTitle();
}

/* switch to the markup look with a window that fits the snip */
static void
Edit(void)
{
	int w, h, maxw = sw * 9 / 10, maxh = sh * 85 / 100;

	state = stedit;
	vx = vy = 0;
	hotbtn = downbtn = hotmenu = -1;
	HideTip();
	Layout();
	w = Max(BarWidth(), snip.w + 2 * margin);
	h = CanvasY() + snip.h + 2 * margin;
	if (w > maxw) {
		w = maxw;
		h += sbw;
	}
	if (h > maxh) {
		h = maxh;
		w = Min(w + sbw, maxw);
	}
	SurfSize(&mainw, Max(w, 330), Max(h, CanvasY() + 120));
}

static void
SetSnip(struct img *r, struct img *ink)
{
	ImgFree(&raw);
	ImgFree(&snip);
	ImgFree(&under);
	ImgFree(&comp);
	free(mask);
	raw = *r;
	if (ink)
		snip = *ink;
	else
		ImgCrop(&raw, 0, 0, raw.w, raw.h, &snip);
	ImgCrop(&snip, 0, 0, snip.w, snip.h, &under);
	ImgCrop(&snip, 0, 0, snip.w, snip.h, &comp);
	mask = Ecalloc((size_t)snip.w * snip.h, 1);
	ClearStrokes();
	tool = toolpen;
	dirty = 1;
	ToolCursor();
}

static void
Error(const char *what, const char *path)
{
	char buf[PATH_MAX + 256];

	snprintf(buf, sizeof(buf), "%s\n%s\n%s", what, path ? path : "", strerror(errno));
	Ask(buf, "OK", NULL, NULL);
}

static void
Copy(void)
{
	struct buf b = {0};

	if (!comp.px)
		return;
	Busy(1);
	if (PngWrite(&comp, &b) < 0 || ClipCopy(b.data, b.len, 0) < 0)
		Ask("Could not copy the snip to the clipboard.", "OK", NULL, NULL);
	Busy(0);
	BufFree(&b);
}

/* "Save As": asks for the path in path, 1 if accepted */
static int
SaveDlg(char *path, size_t sz)
{
	struct ctl c[5];
	struct dlg d;
	struct stat st;
	char buf[PATH_MAX], msg[PATH_MAX + 128];
	const char *home = getenv("HOME");
	size_t n;
	int w = 480, h = 118, r, ok = 0;

	SaveName(buf, sizeof(buf), opt.dir[0] ? opt.dir : (home ? home : "."));
	Ctl(&c[0], ctllabel, 12, 16, 80, 18, "File name:", 0);
	Ctl(&c[1], ctlentry, 92, 12, w - 104, 23, NULL, 0);
	c[1].buf = buf;
	c[1].bufsz = sizeof(buf);
	c[1].cur = strlen(buf);
	c[1].all = 0;
	Ctl(&c[2], ctllabel, 12, 46, w - 24, 18,
	    "Save as type: PNG (*.png). Tab completes the path.", 0);
	Btn(&c[3], 0, h - 35, "&Save", 1, 1);
	Btn(&c[4], 0, h - 35, "Cancel", 2, 0);
	c[4].x = w - 12 - c[4].w;
	c[3].x = c[4].x - 8 - c[3].w;
	memset(&d, 0, sizeof(d));
	d.c = c;
	d.n = 5;
	DlgOpen(&d, "Save As", w, h);
	while (!ok && (r = DlgRun(&d)) == 1) {
		if (buf[0] == '~' && buf[1] == '/' && home)
			snprintf(path, sz, "%s%s", home, buf + 1);
		else
			snprintf(path, sz, "%s", buf);
		n = strlen(path);
		if (n == 0)
			continue;
		if (n < 4 || strcasecmp(path + n - 4, ".png") != 0)
			snprintf(path + n, sz - n, ".png");
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
			continue;
		if (stat(path, &st) == 0) {
			snprintf(msg, sizeof(msg), "%s already exists.\nDo you want to replace it?", path);
			if (Ask(msg, "&Yes", "&No", NULL) != 1)
				continue;
		}
		ok = 1;
	}
	DlgClose(&d);
	return ok;
}

static int
SaveAs(void)
{
	char path[PATH_MAX], *slash;

	if (!comp.px || !SaveDlg(path, sizeof(path)))
		return 0;
	Busy(1);
	if (SavePng(path, &comp) < 0) {
		Busy(0);
		Error("Could not save the snip:", path);
		return 0;
	}
	Busy(0);
	dirty = 0;
	slash = strrchr(path, '/');
	if (slash && slash > path) {
		*slash = '\0';
		snprintf(opt.dir, sizeof(opt.dir), "%s", path);
		OptSave();
	}
	return 1;
}

/* 0 when the user wants to stay with the current snip */
static int
AskSave(void)
{
	int r;

	if (state != stedit || !dirty || !opt.asksave)
		return 1;
	r = Ask("Do you want to save the changes to the snip?", "&Yes", "&No", "Cancel");
	if (r == 1)
		return SaveAs();
	return r == 2;
}

static void
AutoSave(void)
{
	char path[PATH_MAX];
	const char *home = getenv("HOME");

	SaveName(path, sizeof(path), home ? home : ".");
	if (SavePng(path, &snip) < 0)
		Warn("cannot save %s: %s", path, strerror(errno));
}

static void
TakeSnip(int mode)
{
	struct img r, ink;

	Hide();
	if (Snip(mode, &r, opt.showink ? &ink : NULL) == 0) {
		SetSnip(&r, opt.showink ? &ink : NULL);
		Edit();
		Show();
		if (opt.autocopy)
			Copy();
		if (opt.autosave)
			AutoSave();
	} else {
		Show();
	}
}

static void
NewSnip(int mode)
{
	if (countdown > 0)
		return;
	if (opt.mode != mode) {
		opt.mode = mode;
		OptSave();
	}
	if (!AskSave())
		return;
	if (opt.delay > 0) {
		countdown = opt.delay;
		countmode = mode;
		countnext = Now() + 1000;
		Refresh();
		return;
	}
	TakeSnip(mode);
}

static void
Tick(void)
{
	if (--countdown <= 0) {
		countdown = 0;
		Refresh();
		TakeSnip(countmode);
		return;
	}
	countnext += 1000;
	Refresh();
}

static void
Spawn(char *const argv[], const char *errmsg)
{
	pid_t pid;
	int fd[2], err = 0, st;

	if (pipe(fd) < 0)
		return;
	fcntl(fd[1], F_SETFD, FD_CLOEXEC);
	pid = fork();
	if (pid == 0) {
		close(fd[0]);
		close(ConnectionNumber(dpy));
		setsid();
		if (fork() != 0)
			_exit(0);
		execvp(argv[0], argv);
		err = errno;
		if (write(fd[1], &err, sizeof(err)) < 0)
			_exit(127);
		_exit(127);
	}
	close(fd[1]);
	if (pid > 0)
		waitpid(pid, &st, 0);
	if (read(fd[0], &err, sizeof(err)) != sizeof(err))
		err = 0;
	close(fd[0]);
	if (pid < 0 || err)
		Ask(errmsg, "OK", NULL, NULL);
}

/* save the snip into a new temporary directory */
static int
TempSnip(char *path, size_t sz)
{
	char dir[PATH_MAX];
	const char *tmp = getenv("TMPDIR");

	snprintf(dir, sizeof(dir), "%s/snipping-tool-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(dir))
		return -1;
	SaveName(path, sz, dir);
	return SavePng(path, &comp);
}

static void
Send(int mail)
{
	char path[PATH_MAX], *argv[4];

	if (!comp.px)
		return;
	if (TempSnip(path, sizeof(path)) < 0) {
		Error("Could not save a temporary file.", path);
		return;
	}
	if (mail) {
		argv[0] = "xdg-email";
		argv[1] = "--attach";
		argv[2] = path;
		argv[3] = NULL;
		Spawn(argv, "Could not run xdg-email.\n"
		      "Sending by e-mail needs the xdg-utils package.");
	} else {
		argv[0] = "xdg-open";
		argv[1] = path;
		argv[2] = NULL;
		Spawn(argv, "Could not run xdg-open.\n"
		      "Opening in another program needs the xdg-utils package.");
	}
}

static void
Recognize(void)
{
	char *text;

	if (!raw.px)
		return;
	Busy(1);
	text = Ocr(&raw);
	Busy(0);
	if (!text[0]) {
		Ask("No text was found in the snip.", "OK", NULL, NULL);
	} else if (ClipCopy((const unsigned char *)text, strlen(text), 1) < 0) {
		TextDlg("Recognized Text", text, NULL, 1);
	} else {
		TextDlg("Recognized Text", text,
		        "The text has been copied to the clipboard.", 1);
	}
	free(text);
}

static void
Options(void)
{
	static const char *delays[] = { "None", "1 second", "2 seconds", "3 seconds",
	                                "4 seconds", "5 seconds" };
	const char *names[32];
	unsigned int cols[32];
	struct ctl c[14];
	struct dlg d;
	int w = 440, h = 352, oldhide = opt.hidetext, n = 0, y, nw, nh;

	PaletteLists(names, cols);
	y = 10;
	Ctl(&c[n++], ctlgroup, 10, y, w - 20, 172, "Application", 0);
	Ctl(&c[n++], ctlcheck, 22, y + 22, w - 44, 20, "Hide instruction text", opt.hidetext);
	Ctl(&c[n++], ctlcheck, 22, y + 44, w - 44, 20,
	    "Always copy snips to the clipboard", opt.autocopy);
	Ctl(&c[n++], ctlcheck, 22, y + 66, w - 44, 20,
	    "Prompt to save snips before exiting", opt.asksave);
	Ctl(&c[n++], ctlcheck, 22, y + 88, w - 44, 20,
	    "Show screen overlay when Snipping Tool is active", opt.veil);
	Ctl(&c[n++], ctlcheck, 22, y + 110, w - 44, 20,
	    "Also save every snip in the home directory", opt.autosave);
	Ctl(&c[n++], ctllabel, 22, y + 138, 200, 20, "Delay before the snip:", 0);
	Ctl(&c[n], ctlcombo, 220, y + 136, 110, 22, NULL, opt.delay);
	c[n].list = delays;
	c[n++].nlist = 6;
	y += 184;
	Ctl(&c[n++], ctlgroup, 10, y, w - 20, 92, "Selection", 0);
	Ctl(&c[n++], ctllabel, 22, y + 26, 100, 20, "Ink color:", 0);
	Ctl(&c[n], ctlcombo, 120, y + 24, 170, 22, NULL, PaletteIndex(opt.inkcol));
	c[n].list = names;
	c[n].cols = cols;
	c[n++].nlist = sizeof(palette) / sizeof(palette[0]);
	Ctl(&c[n++], ctlcheck, 22, y + 58, w - 44, 20,
	    "Show selection ink after snips are captured", opt.showink);
	Btn(&c[n], 0, h - 35, "Cancel", 2, 0);
	c[n].x = w - 12 - c[n].w;
	n++;
	Btn(&c[n], 0, h - 35, "OK", 1, 1);
	c[n].x = c[n - 1].x - 8 - c[n].w;
	n++;
	memset(&d, 0, sizeof(d));
	d.c = c;
	d.n = n;
	DlgOpen(&d, "Snipping Tool Options", w, h);
	if (DlgRun(&d) == 1) {
		opt.hidetext = c[1].val;
		opt.autocopy = c[2].val;
		opt.asksave = c[3].val;
		opt.veil = c[4].val;
		opt.autosave = c[5].val;
		opt.delay = c[7].val;
		opt.inkcol = palette[c[10].val].col;
		opt.showink = c[11].val;
		OptSave();
	}
	DlgClose(&d);
	if (state == stmain && oldhide != opt.hidetext) {
		MainSize(&nw, &nh);
		SurfSize(&mainw, nw, nh);
		SetHints(mainw.win, 0, 0, nw, nh, 1);
		XResizeWindow(dpy, mainw.win, nw, nh);
		Refresh();
	}
}

static void
PenDialog(void)
{
	static const char *widths[] = { "Fine Point Pen", "Medium Point Pen", "Thick Point Pen" };
	static const char *tips[] = { "Round Tip Pen", "Chisel Tip Pen" };
	const char *names[32];
	unsigned int cols[32];
	struct ctl c[8];
	struct dlg d;
	int w = 340, h = 160;

	PaletteLists(names, cols);
	Ctl(&c[0], ctllabel, 14, 18, 100, 20, "Color:", 0);
	Ctl(&c[1], ctlcombo, 120, 14, 200, 22, NULL, PaletteIndex(opt.pencol));
	c[1].list = names;
	c[1].cols = cols;
	c[1].nlist = sizeof(palette) / sizeof(palette[0]);
	Ctl(&c[2], ctllabel, 14, 50, 100, 20, "Thickness:", 0);
	Ctl(&c[3], ctlcombo, 120, 46, 200, 22, NULL, opt.penw);
	c[3].list = widths;
	c[3].nlist = 3;
	Ctl(&c[4], ctllabel, 14, 82, 100, 20, "Tip:", 0);
	Ctl(&c[5], ctlcombo, 120, 78, 200, 22, NULL, opt.pentip);
	c[5].list = tips;
	c[5].nlist = 2;
	Btn(&c[6], 0, h - 35, "Cancel", 2, 0);
	c[6].x = w - 12 - c[6].w;
	Btn(&c[7], 0, h - 35, "OK", 1, 1);
	c[7].x = c[6].x - 8 - c[7].w;
	memset(&d, 0, sizeof(d));
	d.c = c;
	d.n = 8;
	DlgOpen(&d, "Customize Pen", w, h);
	if (DlgRun(&d) == 1) {
		opt.pencol = palette[c[1].val].col;
		opt.penw = c[3].val;
		opt.pentip = c[5].val;
		OptSave();
		pen = penmine;
		tool = toolpen;
		ToolCursor();
	}
	DlgClose(&d);
	DrawBar();
	PresentBar();
}

static void
Quit(void)
{
	if (countdown > 0) {
		countdown = 0;
		Refresh();
	}
	if (AskSave())
		running = 0;
}

static void
SetTool(int t, int p)
{
	tool = t;
	if (p >= 0)
		pen = p;
	ToolCursor();
	DrawBar();
	PresentBar();
}

static void
Do(int cmd)
{
	HideTip();
	switch (cmd) {
	case cmdnew:
		NewSnip(opt.mode);
		break;
	case cmdcancel:
		countdown = 0;
		Refresh();
		break;
	case cmdoptions:
		Options();
		break;
	case cmdhelp:
		TextDlg("Snipping Tool Help", helptext, NULL, 0);
		break;
	case cmdabout:
		TextDlg("About Snipping Tool", abouttext, NULL, 0);
		break;
	case cmdsave:
		SaveAs();
		break;
	case cmdcopy:
		Copy();
		break;
	case cmdsend:
		Send(1);
		break;
	case cmdopen:
		Send(0);
		break;
	case cmdpen:
		SetTool(toolpen, -1);
		break;
	case cmdmark:
		SetTool(toolmark, -1);
		break;
	case cmderase:
		SetTool(toolerase, -1);
		break;
	case cmdocr:
		Recognize();
		break;
	case cmdundo:
		Undo();
		break;
	case cmdexit:
		Quit();
		break;
	case cmdcustom:
		PenDialog();
		break;
	case cmdfree:
	case cmdrect:
	case cmdwin:
	case cmdfull:
		NewSnip(cmd - cmdfree);
		break;
	case cmdred:
	case cmdblue:
	case cmdblack:
	case cmdmine:
		SetTool(toolpen, cmd - cmdred);
		break;
	}
}

static void
DropMenu(int i)
{
	struct btn *b = &btns[i];
	int rx, ry, cmd = 0;

	HideTip();
	downbtn = i;
	downpart = 1;
	DrawBtn(i);
	PresentBar();
	RootPos(b->x, b->y + b->h, &rx, &ry);
	MarkMenu(modemenu, 4);
	MarkMenu(sendmenu, 2);
	MarkMenu(penmenu, 6);
	if (b->drop == cmdmodes)
		cmd = Popup(modemenu, 4, rx, ry, 0, NULL, 0, -1);
	else if (b->drop == cmdsends)
		cmd = Popup(sendmenu, 2, rx, ry, 0, NULL, 0, -1);
	else if (b->drop == cmdpens)
		cmd = Popup(penmenu, 6, rx, ry, 0, NULL, 0, -1);
	downbtn = -1;
	hotbtn = -1;
	DrawBtn(i);
	PresentBar();
	if (cmd > 0)
		Do(cmd);
}

static void
MenuBar(int i)
{
	struct item *menus[4] = { filemenu, editmenu, toolmenu, helpmenu };
	int counts[4];
	XRectangle titles[4];
	int k, rx, ry, cmd = 0;

	counts[0] = sizeof(filemenu) / sizeof(filemenu[0]);
	counts[1] = sizeof(editmenu) / sizeof(editmenu[0]);
	counts[2] = sizeof(toolmenu) / sizeof(toolmenu[0]);
	counts[3] = sizeof(helpmenu) / sizeof(helpmenu[0]);
	HideTip();
	for (k = 0; k < 4; k++) {
		RootPos(menux[k], 0, &rx, &ry);
		titles[k].x = rx;
		titles[k].y = ry;
		titles[k].width = menux[k + 1] - menux[k];
		titles[k].height = menuh;
	}
	while (i >= 0) {
		openmenu = i;
		hotmenu = -1;
		DrawMenuBar();
		Present(&mainw, 0, 0, mainw.w, menuh);
		RootPos(menux[i], menuh, &rx, &ry);
		MarkMenu(menus[i], counts[i]);
		cmd = Popup(menus[i], counts[i], rx, ry, 0, titles, 4, i);
		i = cmd < 0 ? -1 - cmd : -1;
	}
	openmenu = -1;
	DrawMenuBar();
	Present(&mainw, 0, 0, mainw.w, menuh);
	if (cmd > 0)
		Do(cmd);
}

/* ---------------------------------------------------------------- */
/* events */

/* scroll bar under x,y: 1 vertical, 2 horizontal, 0 none */
static int
SbAt(int x, int y, int *len, int *pos, int *total, int *view, int *off)
{
	int vw, vh, hb, vb, top = CanvasY();

	View(&vw, &vh, &hb, &vb);
	if (vb && x >= vw && x < vw + sbw && y >= top && y < top + vh) {
		*len = vh;
		*pos = vy;
		*total = snip.h + 2 * margin;
		*view = vh;
		*off = y - top;
		return 1;
	}
	if (hb && y >= top + vh && y < top + vh + sbw && x < vw) {
		*len = vw;
		*pos = vx;
		*total = snip.w + 2 * margin;
		*view = vw;
		*off = x;
		return 2;
	}
	return 0;
}

static int
SbPress(int x, int y)
{
	int len, pos, total, view, off, tp, tl, bar, d = 0;

	bar = SbAt(x, y, &len, &pos, &total, &view, &off);
	if (!bar)
		return 0;
	Thumb(len, pos, total, view, &tp, &tl);
	if (off < sbw)
		d = -40;
	else if (off >= len - sbw)
		d = 40;
	else if (off < tp)
		d = -view;
	else if (off >= tp + tl)
		d = view;
	else {
		sbdrag = 1;
		sbvert = bar == 1;
		sbgrab = off - tp;
	}
	if (bar == 1)
		Scroll(0, d);
	else
		Scroll(d, 0);
	return 1;
}

static void
SbMove(int x, int y)
{
	int vw, vh, hb, vb, tp, tl, top = CanvasY();

	View(&vw, &vh, &hb, &vb);
	if (sbvert) {
		Thumb(vh, vy, snip.h + 2 * margin, vh, &tp, &tl);
		tp = y - top - sbgrab - sbw;
		if (vh - 2 * sbw - tl > 0)
			Scroll(0, tp * (snip.h + 2 * margin - vh) / (vh - 2 * sbw - tl) - vy);
	} else {
		Thumb(vw, vx, snip.w + 2 * margin, vw, &tp, &tl);
		tp = x - sbgrab - sbw;
		if (vw - 2 * sbw - tl > 0)
			Scroll(tp * (snip.w + 2 * margin - vw) / (vw - 2 * sbw - tl) - vx, 0);
	}
}

static int
InView(int x, int y)
{
	int vw, vh, hb, vb;

	if (state != stedit)
		return 0;
	View(&vw, &vh, &hb, &vb);
	return x >= 0 && x < vw && y >= CanvasY() && y < CanvasY() + vh;
}

static void
ToSnip(int x, int y, int *sx, int *sy)
{
	*sx = x + vx - margin;
	*sy = y - CanvasY() + vy - margin;
}

static void
Press(XButtonEvent *e)
{
	int b, part, sx, sy, k;

	HideTip();
	if (e->button >= Button4 && e->button <= 7) {
		k = e->button == Button4 || e->button == 6 ? -48 : 48;
		if (state == stedit && (e->button >= 6 || (e->state & ShiftMask)))
			Scroll(k, 0);
		else if (state == stedit)
			Scroll(0, k);
		return;
	}
	if (e->button != Button1)
		return;
	if (state == stedit && e->y < menuh) {
		k = TitleIndex(e->x);
		if (k >= 0)
			MenuBar(k);
		return;
	}
	b = BtnAt(e->x, e->y, &part);
	if (b >= 0) {
		if (!Enabled(btns[b].cmd))
			return;
		if (part) {
			DropMenu(b);
			return;
		}
		downbtn = b;
		downpart = 0;
		DrawBtn(b);
		PresentBar();
		return;
	}
	if (state == stmain) {
		if (!opt.hidetext && e->x >= mainw.w - 34 && e->y >= barh + 5 &&
		    e->y < barh + 29)
			Do(cmdhelp);
		return;
	}
	if (SbPress(e->x, e->y))
		return;
	if (!InView(e->x, e->y))
		return;
	ToSnip(e->x, e->y, &sx, &sy);
	if (tool == toolerase) {
		erasing = 1;
		erasex = sx;
		erasey = sy;
		EraseAt(sx, sy);
	} else {
		StartStroke(sx, sy);
	}
}

static void
Release(XButtonEvent *e)
{
	int b, part, i;

	if (e->button != Button1)
		return;
	if (sbdrag) {
		sbdrag = 0;
		return;
	}
	if (drawing) {
		EndStroke();
		return;
	}
	erasing = 0;
	if (downbtn >= 0) {
		i = downbtn;
		downbtn = -1;
		b = BtnAt(e->x, e->y, &part);
		DrawBtn(i);
		PresentBar();
		if (b == i && !part)
			Do(btns[i].cmd);
	}
}

static void
Hover(int x, int y)
{
	int b, part = 0, old, m;

	if (state == stedit) {
		m = y < menuh ? TitleIndex(x) : -1;
		if (m != hotmenu) {
			hotmenu = m;
			DrawMenuBar();
			Present(&mainw, 0, 0, mainw.w, menuh);
		}
	}
	b = BtnAt(x, y, &part);
	if (b >= 0 && !Enabled(btns[b].cmd))
		b = -1;
	if (b != hotbtn || part != hotpart) {
		old = hotbtn;
		hotbtn = b;
		hotpart = part;
		if (old >= 0)
			DrawBtn(old);
		if (b >= 0)
			DrawBtn(b);
		PresentBar();
	}
	if (b != tipbtn) {
		HideTip();
		tipbtn = b;
		tipat = Now() + tipdelay;
	}
	ShowCursor(InView(x, y));
}

static void
Motion(XMotionEvent *e)
{
	XEvent ev;
	int x = e->x, y = e->y, sx, sy;

	while (XCheckTypedWindowEvent(dpy, mainw.win, MotionNotify, &ev)) {
		x = ev.xmotion.x;
		y = ev.xmotion.y;
	}
	if (drawing) {
		ToSnip(x, y, &sx, &sy);
		if (sx != cur.pt[2 * cur.n - 2] || sy != cur.pt[2 * cur.n - 1]) {
			StrokeAdd(&cur, sx, sy);
			DrawSeg();
		}
		return;
	}
	if (erasing) {
		ToSnip(x, y, &sx, &sy);
		EraseTo(sx, sy);
		return;
	}
	if (sbdrag) {
		SbMove(x, y);
		return;
	}
	Hover(x, y);
}

static void
Leave(void)
{
	int old = hotbtn;

	HideTip();
	hotbtn = -1;
	if (old >= 0) {
		DrawBtn(old);
		PresentBar();
	}
	if (hotmenu >= 0) {
		hotmenu = -1;
		DrawMenuBar();
		Present(&mainw, 0, 0, mainw.w, menuh);
	}
}

static void
Key(XKeyEvent *e)
{
	KeySym ks, base;
	char tmp[8];
	int i, vw, vh, hb, vb;

	XLookupString(e, tmp, sizeof(tmp), &ks, NULL);
	base = XkbKeycodeToKeysym(dpy, e->keycode, 0, 0);
	HideTip();
	if (e->state & ControlMask) {
		if (base == XK_n)
			Do(cmdnew);
		else if (base == XK_q)
			Do(cmdexit);
		else if (state == stedit && base == XK_s)
			Do(cmdsave);
		else if (state == stedit && base == XK_c)
			Do(cmdcopy);
		else if (state == stedit && base == XK_z)
			Do(cmdundo);
		else if (state == stedit && base == XK_t)
			Do(cmdocr);
		return;
	}
	if (ks == XK_F1) {
		Do(cmdhelp);
		return;
	}
	if (ks == XK_Escape) {
		if (countdown > 0)
			Do(cmdcancel);
		return;
	}
	if (state == stedit && ks == XK_F10) {
		MenuBar(0);
		return;
	}
	if (e->state & Mod1Mask) {
		for (i = 0; state == stedit && i < 4; i++) {
			if (IsAccess(e, ks, menutitles[i])) {
				MenuBar(i);
				return;
			}
		}
		for (i = 0; i < nbtns; i++) {
			if (IsAccess(e, ks, btns[i].label) && Enabled(btns[i].cmd)) {
				Do(btns[i].cmd);
				return;
			}
		}
		return;
	}
	if (state != stedit)
		return;
	View(&vw, &vh, &hb, &vb);
	if (ks == XK_Up)
		Scroll(0, -40);
	else if (ks == XK_Down)
		Scroll(0, 40);
	else if (ks == XK_Left)
		Scroll(-40, 0);
	else if (ks == XK_Right)
		Scroll(40, 0);
	else if (ks == XK_Prior)
		Scroll(0, -vh);
	else if (ks == XK_Next)
		Scroll(0, vh);
	else if (ks == XK_Home)
		Scroll(-vx, -vy);
}

static void
Common(XEvent *ev)
{
	struct surf *s;

	if (ev->type == Expose) {
		s = FindSurf(ev->xexpose.window);
		if (s)
			Present(s, ev->xexpose.x, ev->xexpose.y, ev->xexpose.width,
			        ev->xexpose.height);
	} else if (ev->type == ConfigureNotify && ev->xconfigure.window == mainw.win) {
		Resized(ev->xconfigure.width, ev->xconfigure.height);
	} else if (ev->type == MappingNotify) {
		XRefreshKeyboardMapping(&ev->xmapping);
	}
}

static void
Handle(XEvent *ev)
{
	if (ev->xany.window != mainw.win) {
		Common(ev);
		return;
	}
	switch (ev->type) {
	case ButtonPress:
		Press(&ev->xbutton);
		break;
	case ButtonRelease:
		Release(&ev->xbutton);
		break;
	case MotionNotify:
		Motion(&ev->xmotion);
		break;
	case LeaveNotify:
		if (!drawing && !erasing && !sbdrag)
			Leave();
		break;
	case KeyPress:
		Key(&ev->xkey);
		break;
	case ClientMessage:
		if ((Atom)ev->xclient.data.l[0] == atomdel)
			Do(cmdexit);
		break;
	default:
		Common(ev);
		break;
	}
}

static void
Run(void)
{
	XEvent ev;
	fd_set fds;
	struct timeval tv;
	long now, wake;
	int xfd = ConnectionNumber(dpy);

	while (running) {
		while (running && XPending(dpy)) {
			XNextEvent(dpy, &ev);
			Handle(&ev);
		}
		if (!running)
			break;
		wake = 0;
		if (tipbtn >= 0 && !tipw.win && btns[tipbtn].tip)
			wake = tipat;
		if (countdown > 0 && (!wake || countnext < wake))
			wake = countnext;
		FD_ZERO(&fds);
		FD_SET(xfd, &fds);
		now = Now();
		if (!wake) {
			select(xfd + 1, &fds, NULL, NULL, NULL);
			continue;
		}
		if (wake > now) {
			tv.tv_sec = (wake - now) / 1000;
			tv.tv_usec = (wake - now) % 1000 * 1000;
			if (select(xfd + 1, &fds, NULL, NULL, &tv) > 0)
				continue;
		}
		now = Now();
		if (countdown > 0 && countnext <= now)
			Tick();
		else if (tipbtn >= 0 && tipat <= now)
			ShowTip();
	}
}

static void
LoadIcons(void)
{
	static const unsigned char *data[iclast] = {
		app16_png, app32_png, app48_png, new_png, cancel_png, options_png,
		save_png, copy_png, send_png, pen_png, marker_png, eraser_png,
		ocr_png, help_png
	};
	static const unsigned int *len[iclast] = {
		&app16_png_len, &app32_png_len, &app48_png_len, &new_png_len,
		&cancel_png_len, &options_png_len, &save_png_len, &copy_png_len,
		&send_png_len, &pen_png_len, &marker_png_len, &eraser_png_len,
		&ocr_png_len, &help_png_len
	};
	int i;

	for (i = 0; i < iclast; i++) {
		if (PngRead(data[i], *len[i], &icons[i]) < 0) {
			Warn("icon %d is not a valid PNG", i);
			ImgNew(&icons[i], 1, 1);
		}
	}
}

int
Gui(int argc, char *argv[])
{
	XSetWindowAttributes wa;
	XClassHint ch = { "snipping-tool", "Snipping-tool" };
	XWMHints wmh;
	long pid = getpid();
	int w, h;

	atomdel = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	atomname = XInternAtom(dpy, "_NET_WM_NAME", False);
	atomicon = XInternAtom(dpy, "_NET_WM_ICON", False);
	atomtype = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	atomnormal = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
	atomdialog = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	atommenu = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
	atomtooltip = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
	atomstate = XInternAtom(dpy, "_NET_WM_STATE", False);
	atommodal = XInternAtom(dpy, "_NET_WM_STATE_MODAL", False);
	atompid = XInternAtom(dpy, "_NET_WM_PID", False);

	LoadIcons();
	curarrow = XCreateFontCursor(dpy, XC_left_ptr);
	curwait = XCreateFontCursor(dpy, XC_watch);
	gc = XCreateGC(dpy, root, 0, NULL);
	if (opt.mode < snipfree || opt.mode > snipfull)
		opt.mode = sniprect;

	state = stmain;
	Layout();
	MainSize(&w, &h);
	wa.background_pixmap = None;
	wa.bit_gravity = NorthWestGravity;
	wa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
	                ButtonReleaseMask | PointerMotionMask | LeaveWindowMask |
	                StructureNotifyMask;
	mainw.win = XCreateWindow(dpy, root, (sw - w) / 2, (sh - h) / 3, w, h, 0,
	                          depth, InputOutput, vis,
	                          CWBackPixmap | CWBitGravity | CWEventMask, &wa);
	SurfSize(&mainw, w, h);
	AddSurf(&mainw);
	wmh.flags = InputHint;
	wmh.input = True;
	XSetWMProperties(dpy, mainw.win, NULL, NULL, argv, argc, NULL, &wmh, &ch);
	SetTitle(mainw.win, "Snipping Tool");
	SetType(mainw.win, atomnormal);
	SetIcon(mainw.win);
	XSetWMProtocols(dpy, mainw.win, &atomdel, 1);
	XChangeProperty(dpy, mainw.win, atompid, XA_CARDINAL, 32, PropModeReplace,
	                (unsigned char *)&pid, 1);
	ToolCursor();
	XDefineCursor(dpy, mainw.win, curarrow);
	Show();
	Run();

	ClearStrokes();
	XDestroyWindow(dpy, mainw.win);
	XCloseDisplay(dpy);
	return 0;
}
