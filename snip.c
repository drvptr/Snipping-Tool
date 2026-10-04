/* See LICENSE file for copyright and license details.
 *
 * snip.c - entry point and the command line version, plus everything the
 * GUI shares with it: screen capture, the selection overlay, the clipboard
 * server, saving and settings.
 *
 * The capture works like the Windows tool: the screen is frozen first,
 * then a full screen window shows the frozen picture under a white veil
 * and the selection is cut out of it. Window geometry handling is based
 * on scrot (see LICENSE).
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include "snip.h"
#include "config.h"

/* window rectangle cached for the window snip */
struct wrect {
	int x, y, w, h;
};

/* state of the selection overlay */
struct sel {
	Window win;
	Pixmap dim, orig;
	GC gc, ink;
	Cursor cursor;
	int mode;
	int drag;
	int x0, y0, x1, y1;	/* drag start and current point */
	int rx, ry, rw, rh;	/* rectangle shown now, rw == 0: none */
	XPoint *pts;		/* free form path */
	int npts, cap;
	struct wrect *wins;	/* viewable top level windows, top first */
	int nwins, cur;
};

/* one INCR transfer of the clipboard server */
struct xfer {
	Window win;
	Atom prop, type;
	size_t pos;
};

/* the clipboard server: owns CLIPBOARD for one piece of data */
struct clip {
	Display *d;
	Window w;
	Time t;
	Atom sel, targets, stamp, incr, txt, types[5];
	int ntypes;
	const unsigned char *data;
	size_t len, chunk;
	struct xfer xf[16];
	int nxf, lost;
};

Display *dpy;
Window root;
Visual *vis;
int scr, depth, sw, sh;
struct opts opt;
Atom atomutf8;

static const char version[] = "0.1";

/* ---------------------------------------------------------------- */
/* small helpers */

void
Die(const char *fmt, ...)
{
	va_list ap;

	fputs("snipping-tool: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

void
Warn(const char *fmt, ...)
{
	va_list ap;

	fputs("snipping-tool: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void *
Ecalloc(size_t n, size_t sz)
{
	void *p;

	p = calloc(n ? n : 1, sz ? sz : 1);
	if (!p)
		Die("out of memory");
	return p;
}

void *
Erealloc(void *p, size_t sz)
{
	p = realloc(p, sz ? sz : 1);
	if (!p)
		Die("out of memory");
	return p;
}

char *
Estrdup(const char *s)
{
	char *p;

	p = Ecalloc(strlen(s) + 1, 1);
	strcpy(p, s);
	return p;
}

void
BufAdd(struct buf *b, const void *p, size_t n)
{
	if (b->len + n > b->cap) {
		b->cap = b->cap ? b->cap * 2 : 4096;
		while (b->cap < b->len + n)
			b->cap *= 2;
		b->data = Erealloc(b->data, b->cap);
	}
	memcpy(b->data + b->len, p, n);
	b->len += n;
}

void
BufByte(struct buf *b, int c)
{
	unsigned char ch = c;

	if (b->len < b->cap)
		b->data[b->len++] = ch;
	else
		BufAdd(b, &ch, 1);
}

void
BufStr(struct buf *b, const char *s)
{
	BufAdd(b, s, strlen(s));
}

void
BufFree(struct buf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

void
ImgNew(struct img *im, int w, int h)
{
	im->w = w;
	im->h = h;
	im->px = Ecalloc((size_t)w * h, sizeof(unsigned int));
}

void
ImgFree(struct img *im)
{
	free(im->px);
	im->px = NULL;
	im->w = 0;
	im->h = 0;
}

void
ImgCrop(struct img *src, int x, int y, int w, int h, struct img *dst)
{
	int i;

	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > src->w)
		w = src->w - x;
	if (y + h > src->h)
		h = src->h - y;
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	ImgNew(dst, w, h);
	for (i = 0; i < h; i++)
		memcpy(dst->px + (size_t)i * w, src->px + (size_t)(y + i) * src->w + x,
		       w * sizeof(unsigned int));
}

long
Now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void
MSleep(int ms)
{
	struct timespec ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

/* ---------------------------------------------------------------- */
/* pixels: rgb <-> pixel values of the TrueColor visual */

static int
NativeOrder(void)
{
	int one = 1;

	return *(char *)&one ? LSBFirst : MSBFirst;
}

int
Native32(XImage *xi)
{
	return xi->bits_per_pixel == 32 && xi->red_mask == 0xff0000 &&
	       xi->green_mask == 0xff00 && xi->blue_mask == 0xff &&
	       xi->byte_order == NativeOrder();
}

static int
MaskShift(unsigned long mask)
{
	int s = 0;

	while (s < 32 && !(mask >> s & 1))
		s++;
	return s;
}

static int
MaskBits(unsigned long mask)
{
	int s = MaskShift(mask), n = 0;

	while (s + n < 32 && (mask >> (s + n) & 1))
		n++;
	return n;
}

/* 8 bit channel value into the bits of mask */
static unsigned long
ToChan(unsigned int v, unsigned long mask)
{
	int bits = MaskBits(mask);

	if (!mask)
		return 0;
	if (bits < 8)
		v >>= 8 - bits;
	else if (bits > 8)
		v = (v << (bits - 8)) | (v >> (16 - bits));
	return ((unsigned long)v << MaskShift(mask)) & mask;
}

/* bits of mask from a pixel into an 8 bit channel value */
static unsigned int
FromChan(unsigned long p, unsigned long mask)
{
	int bits = MaskBits(mask);
	unsigned int v;

	if (!mask)
		return 0;
	v = (p & mask) >> MaskShift(mask);
	if (bits < 8)
		return v * 255 / ((1u << bits) - 1);
	return v >> (bits - 8);
}

unsigned long
Pixel(unsigned int rgb)
{
	return ToChan(rgb >> 16 & 0xff, vis->red_mask) |
	       ToChan(rgb >> 8 & 0xff, vis->green_mask) |
	       ToChan(rgb & 0xff, vis->blue_mask);
}

/* XImage in the screen format holding a copy of im */
static XImage *
ToXImage(struct img *im)
{
	XImage *xi;
	char *data;
	int x, y;

	xi = XCreateImage(dpy, vis, depth, ZPixmap, 0, NULL, im->w, im->h, 32, 0);
	if (!xi)
		return NULL;
	data = malloc((size_t)xi->bytes_per_line * im->h);
	if (!data) {
		XDestroyImage(xi);
		return NULL;
	}
	xi->data = data;
	if (Native32(xi)) {
		for (y = 0; y < im->h; y++)
			memcpy(data + (size_t)y * xi->bytes_per_line,
			       im->px + (size_t)y * im->w, im->w * 4);
	} else {
		for (y = 0; y < im->h; y++)
			for (x = 0; x < im->w; x++)
				XPutPixel(xi, x, y, Pixel(im->px[(size_t)y * im->w + x]));
	}
	return xi;
}

/* the whole screen into im */
int
Grab(struct img *im)
{
	XWindowAttributes wa;
	XImage *xi;
	unsigned long p, rm, gm, bm;
	unsigned int *row;
	int x, y;

	if (XGetWindowAttributes(dpy, root, &wa)) {
		sw = wa.width;
		sh = wa.height;
	}
	xi = XGetImage(dpy, root, 0, 0, sw, sh, AllPlanes, ZPixmap);
	if (!xi)
		return -1;
	ImgNew(im, sw, sh);
	rm = xi->red_mask ? xi->red_mask : vis->red_mask;
	gm = xi->green_mask ? xi->green_mask : vis->green_mask;
	bm = xi->blue_mask ? xi->blue_mask : vis->blue_mask;
	if (Native32(xi)) {
		for (y = 0; y < sh; y++) {
			row = (unsigned int *)(xi->data + (size_t)y * xi->bytes_per_line);
			for (x = 0; x < sw; x++)
				im->px[(size_t)y * sw + x] = row[x] & 0xffffff;
		}
	} else {
		for (y = 0; y < sh; y++) {
			for (x = 0; x < sw; x++) {
				p = XGetPixel(xi, x, y);
				im->px[(size_t)y * sw + x] = FromChan(p, rm) << 16 |
					FromChan(p, gm) << 8 | FromChan(p, bm);
			}
		}
	}
	XDestroyImage(xi);
	return 0;
}

/* ---------------------------------------------------------------- */
/* windows */

static int
XError(Display *d, XErrorEvent *ee)
{
	char msg[256];

	/* windows can vanish while we look at them */
	if (ee->error_code == BadWindow || ee->error_code == BadDrawable ||
	    ee->error_code == BadMatch || ee->error_code == BadAtom)
		return 0;
	XGetErrorText(d, ee->error_code, msg, sizeof(msg));
	Warn("X error: %s (request %d)", msg, ee->request_code);
	return 0;
}

static int
GetLongs(Window w, const char *name, Atom type, long *out, int n)
{
	Atom real, prop;
	int fmt, i;
	unsigned long cnt, after;
	unsigned char *data = NULL;

	prop = XInternAtom(dpy, name, True);
	if (prop == None)
		return 0;
	if (XGetWindowProperty(dpy, w, prop, 0, n, False, type, &real, &fmt,
	                       &cnt, &after, &data) != Success || !data)
		return 0;
	if (real != type || fmt != 32 || cnt < (unsigned long)n) {
		XFree(data);
		return 0;
	}
	for (i = 0; i < n; i++)
		out[i] = ((long *)data)[i];
	XFree(data);
	return 1;
}

static int
HasProp(Window w, Atom prop)
{
	Atom real = None;
	int fmt;
	unsigned long cnt, after;
	unsigned char *data = NULL;

	if (XGetWindowProperty(dpy, w, prop, 0, 0, False, AnyPropertyType,
	                       &real, &fmt, &cnt, &after, &data) == Success && data)
		XFree(data);
	return real != None;
}

/* the client window (the one with WM_STATE) inside a frame, like scrot */
static Window
ClientOf(Window w, Atom wmstate, int lvl)
{
	Window r, p, *kids = NULL, c = None;
	unsigned int n, i;

	if (HasProp(w, wmstate))
		return w;
	if (lvl > 3 || !XQueryTree(dpy, w, &r, &p, &kids, &n))
		return None;
	for (i = 0; i < n && c == None; i++)
		c = ClientOf(kids[i], wmstate, lvl + 1);
	if (kids)
		XFree(kids);
	return c;
}

/* screen rectangle of the top level window w with its frame. GTK client
 * side decorations draw a shadow around the window, that part is cut off */
static int
WinRect(Window w, struct wrect *r)
{
	XWindowAttributes wa, ca;
	Window c, dummy;
	Atom wmstate;
	long ext[4];
	int cx, cy;

	if (!XGetWindowAttributes(dpy, w, &wa))
		return 0;
	if (wa.map_state != IsViewable || wa.class == InputOnly)
		return 0;
	r->x = wa.x;
	r->y = wa.y;
	r->w = wa.width + 2 * wa.border_width;
	r->h = wa.height + 2 * wa.border_width;
	wmstate = XInternAtom(dpy, "WM_STATE", False);
	c = ClientOf(w, wmstate, 0);
	if (c != None && GetLongs(c, "_GTK_FRAME_EXTENTS", XA_CARDINAL, ext, 4) &&
	    XGetWindowAttributes(dpy, c, &ca)) {
		XTranslateCoordinates(dpy, c, root, 0, 0, &cx, &cy, &dummy);
		r->x = cx + ext[0];
		r->y = cy + ext[2];
		r->w = ca.width - ext[0] - ext[1];
		r->h = ca.height - ext[2] - ext[3];
	}
	return r->w > 0 && r->h > 0;
}

/* viewable top level windows, topmost first */
static int
WinList(Window skip, struct wrect **out)
{
	Window r, p, *kids = NULL;
	unsigned int n, i;
	int cnt = 0;

	*out = NULL;
	if (!XQueryTree(dpy, root, &r, &p, &kids, &n))
		return 0;
	*out = Ecalloc(n + 1, sizeof(struct wrect));
	for (i = n; i-- > 0;)
		if (kids[i] != skip && WinRect(kids[i], &(*out)[cnt]))
			cnt++;
	if (kids)
		XFree(kids);
	return cnt;
}

static int
WinIndex(struct wrect *wins, int n, int x, int y)
{
	int i;

	for (i = 0; i < n; i++)
		if (x >= wins[i].x && y >= wins[i].y &&
		    x < wins[i].x + wins[i].w && y < wins[i].y + wins[i].h)
			return i;
	return -1;
}

static void
ClipRect(int *x, int *y, int *w, int *h)
{
	if (*x < 0) {
		*w += *x;
		*x = 0;
	}
	if (*y < 0) {
		*h += *y;
		*y = 0;
	}
	if (*x + *w > sw)
		*w = sw - *x;
	if (*y + *h > sh)
		*h = sh - *y;
}

/* rectangle of the active window with its frame */
int
Activewin(int *x, int *y, int *w, int *h)
{
	Window win = None, r, p, *kids;
	unsigned int n;
	struct wrect wr;
	long v;
	int rev;

	if (GetLongs(root, "_NET_ACTIVE_WINDOW", XA_WINDOW, &v, 1) && v)
		win = v;
	else
		XGetInputFocus(dpy, &win, &rev);
	if (win == None || win == PointerRoot || win == root)
		return -1;
	/* climb up to the frame, the child of the root window */
	for (;;) {
		kids = NULL;
		if (!XQueryTree(dpy, win, &r, &p, &kids, &n))
			return -1;
		if (kids)
			XFree(kids);
		if (p == r || p == None)
			break;
		win = p;
	}
	if (!WinRect(win, &wr))
		return -1;
	ClipRect(&wr.x, &wr.y, &wr.w, &wr.h);
	if (wr.w <= 0 || wr.h <= 0)
		return -1;
	*x = wr.x;
	*y = wr.y;
	*w = wr.w;
	*h = wr.h;
	return 0;
}

/* ---------------------------------------------------------------- */
/* selection overlay */

static int
GrabInput(Window w, Cursor c)
{
	int i;

	for (i = 0; i < 100; i++) {
		if (XGrabPointer(dpy, w, False, ButtonPressMask | ButtonReleaseMask |
		                 PointerMotionMask, GrabModeAsync, GrabModeAsync,
		                 None, c, CurrentTime) == GrabSuccess)
			break;
		MSleep(10);
	}
	if (i == 100)
		return -1;
	/* without the keyboard Esc does not work, the right button still does */
	for (i = 0; i < 100; i++) {
		if (XGrabKeyboard(dpy, w, True, GrabModeAsync, GrabModeAsync,
		                  CurrentTime) == GrabSuccess)
			break;
		MSleep(10);
	}
	return 0;
}

/* parts of rectangle a not covered by b */
static int
Subtract(XRectangle a, XRectangle b, XRectangle *out)
{
	int ax1 = a.x + a.width, ay1 = a.y + a.height;
	int bx1 = b.x + b.width, by1 = b.y + b.height;
	int n = 0, top, bot;

	if (a.width == 0 || a.height == 0)
		return 0;
	if (b.width == 0 || b.height == 0 || bx1 <= a.x || by1 <= a.y ||
	    b.x >= ax1 || b.y >= ay1) {
		out[0] = a;
		return 1;
	}
	top = b.y > a.y ? b.y : a.y;
	bot = by1 < ay1 ? by1 : ay1;
	if (b.y > a.y) {
		out[n].x = a.x;
		out[n].y = a.y;
		out[n].width = a.width;
		out[n].height = b.y - a.y;
		n++;
	}
	if (by1 < ay1) {
		out[n].x = a.x;
		out[n].y = by1;
		out[n].width = a.width;
		out[n].height = ay1 - by1;
		n++;
	}
	if (b.x > a.x) {
		out[n].x = a.x;
		out[n].y = top;
		out[n].width = b.x - a.x;
		out[n].height = bot - top;
		n++;
	}
	if (bx1 < ax1) {
		out[n].x = bx1;
		out[n].y = top;
		out[n].width = ax1 - bx1;
		out[n].height = bot - top;
		n++;
	}
	return n;
}

static XRectangle
Grow(int x, int y, int w, int h, int d)
{
	XRectangle r;

	if (w <= 0 || h <= 0) {
		r.x = r.y = 0;
		r.width = r.height = 0;
		return r;
	}
	r.x = x - d;
	r.y = y - d;
	r.width = w + 2 * d;
	r.height = h + 2 * d;
	return r;
}

/* show the rectangle x,y,w,h unveiled with an ink frame around it */
static void
ShowRect(struct sel *s, int x, int y, int w, int h)
{
	XRectangle old, new, part[4];
	int i, n, b = inkwidth;

	old = Grow(s->rx, s->ry, s->rw, s->rh, b);
	new = Grow(x, y, w, h, b);
	n = Subtract(old, new, part);
	for (i = 0; i < n; i++)
		XCopyArea(dpy, s->dim, s->win, s->gc, part[i].x, part[i].y,
		          part[i].width, part[i].height, part[i].x, part[i].y);
	s->rx = x;
	s->ry = y;
	s->rw = w;
	s->rh = h;
	if (w <= 0 || h <= 0)
		return;
	XCopyArea(dpy, s->orig, s->win, s->gc, x, y, w, h, x, y);
	XFillRectangle(dpy, s->win, s->ink, x - b, y - b, w + 2 * b, b);
	XFillRectangle(dpy, s->win, s->ink, x - b, y + h, w + 2 * b, b);
	XFillRectangle(dpy, s->win, s->ink, x - b, y, b, h);
	XFillRectangle(dpy, s->win, s->ink, x + w, y, b, h);
}

static void
Redraw(struct sel *s)
{
	int x = s->rx, y = s->ry, w = s->rw, h = s->rh;

	XCopyArea(dpy, s->dim, s->win, s->gc, 0, 0, sw, sh, 0, 0);
	s->rw = 0;
	if (w > 0)
		ShowRect(s, x, y, w, h);
	if (s->npts > 1)
		XDrawLines(dpy, s->win, s->ink, s->pts, s->npts, CoordModeOrigin);
}

static void
AddPoint(struct sel *s, int x, int y)
{
	if (s->npts == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 256;
		s->pts = Erealloc(s->pts, s->cap * sizeof(XPoint));
	}
	s->pts[s->npts].x = x;
	s->pts[s->npts].y = y;
	s->npts++;
}

static void
SortRect(int x0, int y0, int x1, int y1, int *r)
{
	r[0] = x0 < x1 ? x0 : x1;
	r[1] = y0 < y1 ? y0 : y1;
	r[2] = abs(x1 - x0) + 1;
	r[3] = abs(y1 - y0) + 1;
}

static Pixmap
Upload(struct img *im)
{
	XImage *xi;
	Pixmap pm;
	GC gc;

	xi = ToXImage(im);
	if (!xi)
		return None;
	pm = XCreatePixmap(dpy, root, im->w, im->h, depth);
	gc = XCreateGC(dpy, pm, 0, NULL);
	XPutImage(dpy, pm, gc, xi, 0, 0, 0, 0, im->w, im->h);
	XFreeGC(dpy, gc);
	XDestroyImage(xi);
	return pm;
}

/* the frozen screen under a white veil of strength a/255 */
static void
Veil(struct img *src, struct img *dst, unsigned int a)
{
	unsigned int c, r, g, b;
	size_t i, n = (size_t)src->w * src->h;

	ImgNew(dst, src->w, src->h);
	for (i = 0; i < n; i++) {
		c = src->px[i];
		r = c >> 16 & 0xff;
		g = c >> 8 & 0xff;
		b = c & 0xff;
		r += (255 - r) * a / 255;
		g += (255 - g) * a / 255;
		b += (255 - b) * a / 255;
		dst->px[i] = r << 16 | g << 8 | b;
	}
}

/* full screen window showing the frozen screen, with the input grabbed */
static int
OverlayOpen(struct sel *s, struct img *full, int mode)
{
	XSetWindowAttributes wa;
	XClassHint ch = { "snipping-tool", "Snipping-tool" };
	XGCValues gv;
	struct img dim;
	Window dw;
	unsigned int mk;
	int x, y, k, shape;

	memset(s, 0, sizeof(*s));
	s->mode = mode;
	s->cur = -1;
	Veil(full, &dim, opt.veil ? veilalpha : 0);
	s->dim = Upload(&dim);
	s->orig = Upload(full);
	ImgFree(&dim);
	if (s->dim == None || s->orig == None)
		return -1;

	if (mode == snipfree)
		shape = XC_pencil;
	else if (mode == snipwin)
		shape = XC_hand2;
	else
		shape = XC_crosshair;
	s->cursor = XCreateFontCursor(dpy, shape);
	wa.override_redirect = True;
	wa.background_pixmap = s->dim;
	wa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
	                PointerMotionMask | KeyPressMask;
	wa.cursor = s->cursor;
	s->win = XCreateWindow(dpy, root, 0, 0, sw, sh, 0, depth, InputOutput, vis,
	                       CWOverrideRedirect | CWBackPixmap | CWEventMask |
	                       CWCursor, &wa);
	XSetClassHint(dpy, s->win, &ch);
	s->gc = XCreateGC(dpy, s->win, 0, NULL);
	gv.foreground = Pixel(opt.inkcol);
	gv.line_width = inkwidth;
	gv.cap_style = CapRound;
	gv.join_style = JoinRound;
	s->ink = XCreateGC(dpy, s->win, GCForeground | GCLineWidth | GCCapStyle |
	                   GCJoinStyle, &gv);
	if (mode == snipwin || mode == sniprect)
		s->nwins = WinList(s->win, &s->wins);
	XMapRaised(dpy, s->win);
	if (GrabInput(s->win, s->cursor) < 0) {
		Warn("cannot grab the pointer");
		return -1;
	}
	/* the window under the pointer is marked right away */
	if (mode == snipwin && XQueryPointer(dpy, root, &dw, &dw, &x, &y, &k, &k, &mk)) {
		s->cur = WinIndex(s->wins, s->nwins, x, y);
		if (s->cur >= 0)
			ShowRect(s, s->wins[s->cur].x, s->wins[s->cur].y,
			         s->wins[s->cur].w, s->wins[s->cur].h);
	}
	return 0;
}

static void
OverlayClose(struct sel *s)
{
	XUngrabKeyboard(dpy, CurrentTime);
	XUngrabPointer(dpy, CurrentTime);
	if (s->win)
		XDestroyWindow(dpy, s->win);
	if (s->gc)
		XFreeGC(dpy, s->gc);
	if (s->ink)
		XFreeGC(dpy, s->ink);
	if (s->dim)
		XFreePixmap(dpy, s->dim);
	if (s->orig)
		XFreePixmap(dpy, s->orig);
	if (s->cursor)
		XFreeCursor(dpy, s->cursor);
	XSync(dpy, False);
	free(s->wins);
}

static void
WinTake(struct sel *s, int k, int *r)
{
	r[0] = s->wins[k].x;
	r[1] = s->wins[k].y;
	r[2] = s->wins[k].w;
	r[3] = s->wins[k].h;
}

/* 0: done, -1: cancelled, -2: go on */
static int
OverlayPress(struct sel *s, XButtonEvent *e, int *r)
{
	if (e->button == Button3)
		return -1;
	if (e->button != Button1)
		return -2;
	if (s->mode == snipwin) {
		s->cur = WinIndex(s->wins, s->nwins, e->x_root, e->y_root);
		if (s->cur < 0)
			return -2;
		WinTake(s, s->cur, r);
		return 0;
	}
	s->drag = 1;
	s->x0 = s->x1 = e->x_root;
	s->y0 = s->y1 = e->y_root;
	if (s->mode == snipfree) {
		s->npts = 0;
		AddPoint(s, e->x_root, e->y_root);
	}
	return -2;
}

static void
OverlayMotion(struct sel *s, XMotionEvent *e, int *r)
{
	int k, x = e->x_root, y = e->y_root;

	if (s->mode == snipwin) {
		k = WinIndex(s->wins, s->nwins, x, y);
		if (k == s->cur)
			return;
		s->cur = k;
		if (k < 0)
			ShowRect(s, 0, 0, 0, 0);
		else
			ShowRect(s, s->wins[k].x, s->wins[k].y, s->wins[k].w, s->wins[k].h);
	} else if (s->drag && s->mode == sniprect) {
		s->x1 = x;
		s->y1 = y;
		SortRect(s->x0, s->y0, s->x1, s->y1, r);
		ShowRect(s, r[0], r[1], r[2], r[3]);
	} else if (s->drag && s->mode == snipfree) {
		XDrawLine(dpy, s->win, s->ink, s->pts[s->npts - 1].x,
		          s->pts[s->npts - 1].y, x, y);
		AddPoint(s, x, y);
	}
}

/* bounding box of the free form path */
static void
PathBox(struct sel *s, int *r)
{
	int k, x1, y1;

	r[0] = x1 = s->pts[0].x;
	r[1] = y1 = s->pts[0].y;
	for (k = 1; k < s->npts; k++) {
		if (s->pts[k].x < r[0])
			r[0] = s->pts[k].x;
		if (s->pts[k].y < r[1])
			r[1] = s->pts[k].y;
		if (s->pts[k].x > x1)
			x1 = s->pts[k].x;
		if (s->pts[k].y > y1)
			y1 = s->pts[k].y;
	}
	r[2] = x1 - r[0] + 1;
	r[3] = y1 - r[1] + 1;
}

/* 0: done, -2: go on */
static int
OverlayRelease(struct sel *s, XButtonEvent *e, int *r)
{
	int k, x = e->x_root, y = e->y_root;

	if (e->button != Button1 || !s->drag)
		return -2;
	s->drag = 0;
	if (s->mode == sniprect) {
		k = WinIndex(s->wins, s->nwins, x, y);
		/* a click without dragging takes the window */
		if (abs(x - s->x0) + abs(y - s->y0) < 4 && k >= 0)
			WinTake(s, k, r);
		else
			SortRect(s->x0, s->y0, x, y, r);
		return 0;
	}
	AddPoint(s, x, y);
	PathBox(s, r);
	if (r[2] < 4 || r[3] < 4) {
		/* too small, let the user try again */
		s->npts = 0;
		Redraw(s);
		return -2;
	}
	return 0;
}

/* run the overlay; r gets x,y,w,h, pts the free form path.
 * -1 when cancelled */
static int
Select(struct img *full, int mode, int *r, XPoint **pts, int *npts)
{
	struct sel s;
	XEvent ev;
	int ret = -2;

	if (OverlayOpen(&s, full, mode) < 0)
		ret = -1;
	while (ret == -2) {
		XNextEvent(dpy, &ev);
		if (ev.type == Expose && ev.xexpose.count == 0) {
			Redraw(&s);
		} else if (ev.type == KeyPress) {
			if (XLookupKeysym(&ev.xkey, 0) == XK_Escape)
				ret = -1;
		} else if (ev.type == ButtonPress) {
			ret = OverlayPress(&s, &ev.xbutton, r);
		} else if (ev.type == MotionNotify) {
			while (XCheckTypedWindowEvent(dpy, s.win, MotionNotify, &ev))
				;
			OverlayMotion(&s, &ev.xmotion, r);
		} else if (ev.type == ButtonRelease) {
			ret = OverlayRelease(&s, &ev.xbutton, r);
		}
	}
	OverlayClose(&s);
	if (ret == 0 && mode == snipfree) {
		*pts = s.pts;
		*npts = s.npts;
	} else {
		free(s.pts);
	}
	if (ret == 0)
		ClipRect(&r[0], &r[1], &r[2], &r[3]);
	if (ret == 0 && (r[2] <= 0 || r[3] <= 0))
		ret = -1;
	return ret;
}

/* ---------------------------------------------------------------- */
/* drawing on the snip */

static void
Dot(struct img *im, int cx, int cy, int r, unsigned int col)
{
	int x, y;

	for (y = cy - r; y <= cy + r; y++) {
		if (y < 0 || y >= im->h)
			continue;
		for (x = cx - r; x <= cx + r; x++) {
			if (x < 0 || x >= im->w)
				continue;
			if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r + r)
				im->px[(size_t)y * im->w + x] = col;
		}
	}
}

static void
ThickLine(struct img *im, int x0, int y0, int x1, int y1, int w,
          unsigned int col)
{
	int n, i;

	n = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
	if (n == 0)
		n = 1;
	for (i = 0; i <= n; i++)
		Dot(im, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, w / 2, col);
}

static void
InkFrame(struct img *im, int w, unsigned int col)
{
	int x, y;

	for (y = 0; y < im->h; y++)
		for (x = 0; x < im->w; x++)
			if (x < w || y < w || x >= im->w - w || y >= im->h - w)
				im->px[(size_t)y * im->w + x] = col;
}

static int
CmpInt(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

/* paint everything outside the polygon white (even-odd rule) */
static void
FreeMask(struct img *im, XPoint *pts, int n, int ox, int oy)
{
	int *xs, nx, x, y, i, j, k, ya, yb;
	double t, cy;

	xs = Ecalloc(n + 1, sizeof(int));
	for (y = 0; y < im->h; y++) {
		cy = oy + y + 0.5;
		nx = 0;
		for (i = 0; i < n; i++) {
			j = (i + 1) % n;
			ya = pts[i].y;
			yb = pts[j].y;
			if ((ya <= cy && yb > cy) || (yb <= cy && ya > cy)) {
				t = (cy - ya) / (double)(yb - ya);
				xs[nx++] = (int)(pts[i].x + t * (pts[j].x - pts[i].x) + 0.5) - ox;
			}
		}
		qsort(xs, nx, sizeof(int), CmpInt);
		k = 0;
		for (x = 0; x < im->w; x++) {
			while (k < nx && xs[k] <= x)
				k++;
			if (k % 2 == 0)
				im->px[(size_t)y * im->w + x] = 0xffffff;
		}
	}
	free(xs);
}

/* freeze the screen, let the user select a part and cut it into out.
 * If inked is not NULL it gets a copy with the selection ink drawn on it */
int
Snip(int mode, struct img *out, struct img *inked)
{
	struct img full;
	XPoint *pts = NULL;
	int r[4], npts = 0, i;

	if (Grab(&full) < 0) {
		Warn("cannot read the screen");
		return -1;
	}
	if (mode == snipfull) {
		*out = full;
		r[0] = r[1] = 0;
		r[2] = full.w;
		r[3] = full.h;
	} else if (Select(&full, mode, r, &pts, &npts) < 0) {
		ImgFree(&full);
		return -1;
	} else {
		ImgCrop(&full, r[0], r[1], r[2], r[3], out);
		ImgFree(&full);
	}
	if (mode == snipfree)
		FreeMask(out, pts, npts, r[0], r[1]);
	if (inked) {
		ImgCrop(out, 0, 0, out->w, out->h, inked);
		if (mode == snipfree) {
			for (i = 1; i < npts; i++)
				ThickLine(inked, pts[i - 1].x - r[0], pts[i - 1].y - r[1],
				          pts[i].x - r[0], pts[i].y - r[1], inkwidth,
				          opt.inkcol);
		} else {
			InkFrame(inked, inkwidth, opt.inkcol);
		}
	}
	free(pts);
	return 0;
}

/* ---------------------------------------------------------------- */
/* clipboard: a forked process owns CLIPBOARD and answers requests until
 * somebody else takes it, like xclip does */

static int
IgnoreError(Display *d, XErrorEvent *ee)
{
	(void)d;
	(void)ee;
	return 0;
}

static Time
ServerTime(Display *d, Window w)
{
	XEvent ev;
	Atom a = XInternAtom(d, "_SNIP_TIME", False);

	XChangeProperty(d, w, a, XA_STRING, 8, PropModeAppend, NULL, 0);
	XWindowEvent(d, w, PropertyChangeMask, &ev);
	return ev.xproperty.time;
}

/* does the requested type match the data */
static int
ClipType(struct clip *c, Atom target)
{
	int i;

	for (i = 0; i < c->ntypes; i++)
		if (target == c->types[i])
			return 1;
	return 0;
}

static void
ClipRequest(struct clip *c, XSelectionRequestEvent *rq)
{
	XSelectionEvent ne;
	Atom list[8], prop, type;
	long v;
	int i;

	memset(&ne, 0, sizeof(ne));
	ne.type = SelectionNotify;
	ne.display = c->d;
	ne.requestor = rq->requestor;
	ne.selection = rq->selection;
	ne.target = rq->target;
	ne.time = rq->time;
	ne.property = None;
	/* obsolete clients ask without a property */
	prop = rq->property != None ? rq->property : rq->target;
	/* TEXT is answered with the real type */
	type = rq->target == c->txt ? c->types[0] : rq->target;
	if (c->lost || rq->selection != c->sel) {
		/* refuse */
	} else if (rq->target == c->targets) {
		list[0] = c->targets;
		list[1] = c->stamp;
		for (i = 0; i < c->ntypes; i++)
			list[2 + i] = c->types[i];
		XChangeProperty(c->d, rq->requestor, prop, XA_ATOM, 32,
		                PropModeReplace, (unsigned char *)list, 2 + c->ntypes);
		ne.property = prop;
	} else if (rq->target == c->stamp) {
		v = c->t;
		XChangeProperty(c->d, rq->requestor, prop, XA_INTEGER, 32,
		                PropModeReplace, (unsigned char *)&v, 1);
		ne.property = prop;
	} else if (ClipType(c, rq->target) && c->len > c->chunk &&
	           c->nxf < (int)(sizeof(c->xf) / sizeof(c->xf[0]))) {
		/* big data goes in pieces, see ICCCM, INCR */
		XSelectInput(c->d, rq->requestor, PropertyChangeMask);
		v = c->len;
		XChangeProperty(c->d, rq->requestor, prop, c->incr, 32,
		                PropModeReplace, (unsigned char *)&v, 1);
		c->xf[c->nxf].win = rq->requestor;
		c->xf[c->nxf].prop = prop;
		c->xf[c->nxf].type = type;
		c->xf[c->nxf].pos = 0;
		c->nxf++;
		ne.property = prop;
	} else if (ClipType(c, rq->target)) {
		XChangeProperty(c->d, rq->requestor, prop, type, 8, PropModeReplace,
		                c->data, c->len);
		ne.property = prop;
	}
	XSendEvent(c->d, rq->requestor, False, 0, (XEvent *)&ne);
	XFlush(c->d);
}

/* the requestor took a piece of an INCR transfer: send the next one */
static void
ClipMore(struct clip *c, XPropertyEvent *pe)
{
	struct xfer *x;
	size_t n;
	int i;

	if (pe->state != PropertyDelete)
		return;
	for (i = 0; i < c->nxf; i++)
		if (c->xf[i].win == pe->window && c->xf[i].prop == pe->atom)
			break;
	if (i == c->nxf)
		return;
	x = &c->xf[i];
	n = c->len - x->pos;
	if (n > c->chunk)
		n = c->chunk;
	XChangeProperty(c->d, x->win, x->prop, x->type, 8, PropModeReplace,
	                c->data + x->pos, n);
	x->pos += n;
	if (n == 0) {
		/* the empty piece ends the transfer */
		XSelectInput(c->d, x->win, NoEventMask);
		c->xf[i] = c->xf[--c->nxf];
	}
	XFlush(c->d);
}

/* own CLIPBOARD with its own connection; 0 on success */
static int
ClipOpen(struct clip *c, int text)
{
	c->d = XOpenDisplay(NULL);
	if (!c->d)
		return -1;
	XSetErrorHandler(IgnoreError);
	c->sel = XInternAtom(c->d, "CLIPBOARD", False);
	c->targets = XInternAtom(c->d, "TARGETS", False);
	c->stamp = XInternAtom(c->d, "TIMESTAMP", False);
	c->incr = XInternAtom(c->d, "INCR", False);
	c->txt = None;
	if (text) {
		c->types[0] = XInternAtom(c->d, "UTF8_STRING", False);
		c->types[1] = XInternAtom(c->d, "text/plain;charset=utf-8", False);
		c->types[2] = XInternAtom(c->d, "text/plain", False);
		c->types[3] = XInternAtom(c->d, "STRING", False);
		c->types[4] = c->txt = XInternAtom(c->d, "TEXT", False);
		c->ntypes = 5;
	} else {
		c->types[0] = XInternAtom(c->d, "image/png", False);
		c->ntypes = 1;
	}
	c->w = XCreateSimpleWindow(c->d, DefaultRootWindow(c->d), 0, 0, 1, 1, 0, 0, 0);
	XSelectInput(c->d, c->w, PropertyChangeMask);
	c->t = ServerTime(c->d, c->w);
	XSetSelectionOwner(c->d, c->sel, c->w, c->t);
	if (XGetSelectionOwner(c->d, c->sel) != c->w)
		return -1;
	/* pieces of INCR transfers, small enough for any request */
	c->chunk = XExtendedMaxRequestSize(c->d);
	if (c->chunk == 0)
		c->chunk = XMaxRequestSize(c->d);
	c->chunk = c->chunk * 4 - 1024;
	if (c->chunk > 256 * 1024)
		c->chunk = 256 * 1024;
	return 0;
}

/* serve requests until another client takes the clipboard */
static void
ClipServe(const unsigned char *data, size_t len, int text, int fd)
{
	struct clip c;
	XEvent ev;

	memset(&c, 0, sizeof(c));
	c.data = data;
	c.len = len;
	if (ClipOpen(&c, text) < 0) {
		close(fd);
		if (c.d)
			XCloseDisplay(c.d);
		return;
	}
	/* tell the parent the clipboard is ready */
	while (write(fd, "1", 1) < 0 && errno == EINTR)
		;
	close(fd);
	while (!c.lost || c.nxf > 0) {
		XNextEvent(c.d, &ev);
		if (ev.type == SelectionClear)
			c.lost = 1;
		else if (ev.type == SelectionRequest)
			ClipRequest(&c, &ev.xselectionrequest);
		else if (ev.type == PropertyNotify)
			ClipMore(&c, &ev.xproperty);
	}
	XCloseDisplay(c.d);
}

/* put data on the clipboard; returns when the server owns the selection */
int
ClipCopy(const unsigned char *data, size_t len, int text)
{
	pid_t pid;
	int fd[2], st, nul;
	char c = 0;

	if (pipe(fd) < 0)
		return -1;
	fflush(stdout);
	fflush(stderr);
	pid = fork();
	if (pid < 0) {
		close(fd[0]);
		close(fd[1]);
		return -1;
	}
	if (pid == 0) {
		close(fd[0]);
		if (dpy)
			close(ConnectionNumber(dpy));
		setsid();
		/* fork again so the server is not our zombie later */
		if (fork() != 0)
			_exit(0);
		nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 1);
			dup2(nul, 2);
			if (nul > 2)
				close(nul);
		}
		signal(SIGHUP, SIG_IGN);
		ClipServe(data, len, text, fd[1]);
		_exit(0);
	}
	close(fd[1]);
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	while (read(fd[0], &c, 1) < 0 && errno == EINTR)
		;
	close(fd[0]);
	return c == '1' ? 0 : -1;
}

/* ---------------------------------------------------------------- */
/* files and settings */

int
SavePng(const char *path, struct img *im)
{
	struct buf b = {0};
	FILE *f;
	int r = 0;

	if (PngWrite(im, &b) < 0)
		return -1;
	f = fopen(path, "wb");
	if (!f) {
		BufFree(&b);
		return -1;
	}
	if (fwrite(b.data, 1, b.len, f) != b.len)
		r = -1;
	if (fclose(f) != 0)
		r = -1;
	BufFree(&b);
	return r;
}

/* dir/ + savename with the time filled in, made unique */
void
SaveName(char *dst, size_t sz, const char *dir)
{
	char name[256], base[256];
	struct stat st;
	time_t t;
	char *dot;
	int i;

	t = time(NULL);
	if (!strftime(name, sizeof(name), savename, localtime(&t)))
		strcpy(name, "snip.png");
	snprintf(dst, sz, "%s/%s", dir, name);
	snprintf(base, sizeof(base), "%s", name);
	dot = strrchr(base, '.');
	if (dot)
		*dot = '\0';
	for (i = 2; stat(dst, &st) == 0 && i < 1000; i++)
		snprintf(dst, sz, "%s/%s-%d%s", dir, base, i,
		         dot ? name + (dot - base) : "");
}

/* $XDG_CONFIG_HOME/snipping-tool/config */
static void
ConfPath(char *dst, size_t sz, int mkdirs)
{
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	size_t n;

	if (xdg && *xdg)
		snprintf(dst, sz, "%s", xdg);
	else
		snprintf(dst, sz, "%s/.config", home ? home : "/tmp");
	if (mkdirs)
		mkdir(dst, 0755);
	n = strlen(dst);
	snprintf(dst + n, sz - n, "/snipping-tool");
	if (mkdirs)
		mkdir(dst, 0755);
	n = strlen(dst);
	snprintf(dst + n, sz - n, "/config");
}

void
OptLoad(void)
{
	char path[PATH_MAX], line[4200], key[32], *val, *nl;
	const char *home = getenv("HOME");
	FILE *f;

	opt = defopts;
	snprintf(opt.dir, sizeof(opt.dir), "%s", home ? home : "/tmp");
	ConfPath(path, sizeof(path), 0);
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
		val = strchr(line, ' ');
		if (!val || val - line >= (int)sizeof(key))
			continue;
		memcpy(key, line, val - line);
		key[val - line] = '\0';
		val++;
		if (!strcmp(key, "hidetext"))
			opt.hidetext = atoi(val);
		else if (!strcmp(key, "autocopy"))
			opt.autocopy = atoi(val);
		else if (!strcmp(key, "asksave"))
			opt.asksave = atoi(val);
		else if (!strcmp(key, "veil"))
			opt.veil = atoi(val);
		else if (!strcmp(key, "showink"))
			opt.showink = atoi(val);
		else if (!strcmp(key, "inkcol"))
			opt.inkcol = strtoul(val, NULL, 16) & 0xffffff;
		else if (!strcmp(key, "autosave"))
			opt.autosave = atoi(val);
		else if (!strcmp(key, "delay"))
			opt.delay = atoi(val);
		else if (!strcmp(key, "mode"))
			opt.mode = atoi(val);
		else if (!strcmp(key, "pencol"))
			opt.pencol = strtoul(val, NULL, 16) & 0xffffff;
		else if (!strcmp(key, "penw"))
			opt.penw = atoi(val);
		else if (!strcmp(key, "pentip"))
			opt.pentip = atoi(val);
		else if (!strcmp(key, "dir"))
			snprintf(opt.dir, sizeof(opt.dir), "%s", val);
	}
	fclose(f);
	if (opt.delay < 0 || opt.delay > 5)
		opt.delay = 0;
	if (opt.mode < snipfree || opt.mode > snipfull)
		opt.mode = sniprect;
	if (opt.penw < 0 || opt.penw > 2)
		opt.penw = 1;
	if (opt.pentip < 0 || opt.pentip > 1)
		opt.pentip = 0;
}

void
OptSave(void)
{
	char path[PATH_MAX];
	FILE *f;

	ConfPath(path, sizeof(path), 1);
	f = fopen(path, "w");
	if (!f) {
		Warn("cannot write %s: %s", path, strerror(errno));
		return;
	}
	fprintf(f, "hidetext %d\nautocopy %d\nasksave %d\nveil %d\n"
	        "showink %d\ninkcol %06x\nautosave %d\ndelay %d\nmode %d\n"
	        "pencol %06x\npenw %d\npentip %d\ndir %s\n",
	        opt.hidetext, opt.autocopy, opt.asksave, opt.veil, opt.showink,
	        opt.inkcol, opt.autosave, opt.delay, opt.mode, opt.pencol,
	        opt.penw, opt.pentip, opt.dir);
	fclose(f);
}

/* ---------------------------------------------------------------- */
/* command line version */

static void
OpenX(void)
{
	dpy = XOpenDisplay(NULL);
	if (!dpy)
		Die("cannot open display");
	scr = DefaultScreen(dpy);
	root = RootWindow(dpy, scr);
	vis = DefaultVisual(dpy, scr);
	depth = DefaultDepth(dpy, scr);
	sw = DisplayWidth(dpy, scr);
	sh = DisplayHeight(dpy, scr);
	if (vis->class != TrueColor && vis->class != DirectColor)
		Die("a TrueColor visual is needed");
	XSetErrorHandler(XError);
	atomutf8 = XInternAtom(dpy, "UTF8_STRING", False);
}

static void
Usage(void)
{
	fputs("usage: snipping-tool [-flwtsnhv] [-d sec] [-o file] [-i file] [-g]\n"
	      "\n"
	      "Without options: select a rectangle (a click takes the window\n"
	      "under the pointer) and copy it to the clipboard as image/png.\n"
	      "\n"
	      "  -g, --graphic     start the graphical version\n"
	      "  -f, --full        whole screen, no selection\n"
	      "  -w, --window      active window, no selection\n"
	      "  -l, --free        free-form selection\n"
	      "  -s, --save        also save the snip into $HOME\n"
	      "  -o, --output FILE also save the snip into FILE ('-' is stdout)\n"
	      "  -d, --delay SEC   wait before taking the snip\n"
	      "  -t, --text        recognize text (OCR): print it and copy it\n"
	      "                    to the clipboard instead of the image\n"
	      "  -n, --no-copy     do not touch the clipboard\n"
	      "  -i, --input FILE  take the image from a PNG file, not the screen\n"
	      "  -h, --help        this help\n"
	      "  -v, --version     print version\n", stderr);
	exit(1);
}

/* a PNG file instead of the screen */
static int
ReadPng(const char *path, struct img *im)
{
	struct buf b = {0};
	char tmp[65536];
	size_t n;
	FILE *f;
	int r;

	f = strcmp(path, "-") ? fopen(path, "rb") : stdin;
	if (!f)
		return -1;
	while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0)
		BufAdd(&b, tmp, n);
	if (f != stdin)
		fclose(f);
	r = PngRead(b.data, b.len, im);
	BufFree(&b);
	return r;
}

static int
Cli(int mode, int delay, int save, const char *out, int text, int copy,
    const char *in)
{
	struct img im, full;
	struct buf png = {0};
	char path[PATH_MAX], *str;
	const char *home = getenv("HOME");
	int x, y, w, h, r = 0;

	if (delay > 0)
		MSleep(delay * 1000);
	if (in) {
		if (ReadPng(in, &im) < 0)
			Die("cannot read the PNG file %s", in);
	} else if (mode == snipwin) {
		if (Activewin(&x, &y, &w, &h) < 0) {
			Warn("no active window");
			return 1;
		}
		if (Grab(&full) < 0)
			Die("cannot read the screen");
		ImgCrop(&full, x, y, w, h, &im);
		ImgFree(&full);
	} else if (Snip(mode, &im, NULL) < 0) {
		return 1;
	}

	if (text) {
		str = Ocr(&im);
		fputs(str, stdout);
		fflush(stdout);
		if (copy && ClipCopy((unsigned char *)str, strlen(str), 1) < 0) {
			Warn("cannot own the clipboard");
			r = 1;
		}
		free(str);
	} else if (copy || (out && !strcmp(out, "-"))) {
		if (PngWrite(&im, &png) < 0)
			Die("cannot encode PNG");
		if (copy && ClipCopy(png.data, png.len, 0) < 0) {
			Warn("cannot own the clipboard");
			r = 1;
		}
		if (out && !strcmp(out, "-")) {
			fwrite(png.data, 1, png.len, stdout);
			fflush(stdout);
		}
	}
	if (save) {
		SaveName(path, sizeof(path), home ? home : ".");
		if (SavePng(path, &im) < 0) {
			Warn("cannot save %s: %s", path, strerror(errno));
			r = 1;
		} else if (!out || strcmp(out, "-")) {
			printf("%s\n", path);
		}
	}
	if (out && strcmp(out, "-") && SavePng(out, &im) < 0) {
		Warn("cannot save %s: %s", out, strerror(errno));
		r = 1;
	}
	BufFree(&png);
	ImgFree(&im);
	return r;
}

/* --long option to its short letter */
static const char *
LongOpt(const char *a)
{
	static const char *map[][2] = {
		{ "--graphic", "-g" }, { "--full", "-f" }, { "--window", "-w" },
		{ "--free", "-l" }, { "--save", "-s" }, { "--output", "-o" },
		{ "--delay", "-d" }, { "--text", "-t" }, { "--no-copy", "-n" },
		{ "--help", "-h" }, { "--version", "-v" }, { "--input", "-i" }
	};
	size_t i;

	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (!strcmp(a, map[i][0]))
			return map[i][1];
	return NULL;
}

int
main(int argc, char *argv[])
{
	const char *out = NULL, *in = NULL, *a, *arg = NULL;
	int mode = sniprect, delay = 0, save = 0, text = 0, copy = 1, gui = 0;
	int other = 0, i, j;

	for (i = 1; i < argc; i++) {
		a = argv[i];
		if (a[0] != '-' || a[1] == '\0')
			Usage();
		if (a[1] == '-' && !(a = LongOpt(a)))
			Usage();
		for (j = 1; a[j]; j++) {
			if (a[j] != 'g')
				other = 1;
			if (a[j] == 'o' || a[j] == 'd' || a[j] == 'i') {
				/* -ofile, -o file */
				if (a[j + 1])
					arg = a + j + 1;
				else if (i + 1 < argc)
					arg = argv[++i];
				else
					Usage();
				if (a[j] == 'o')
					out = arg;
				else if (a[j] == 'i')
					in = arg;
				else
					delay = atoi(arg);
				break;
			}
			switch (a[j]) {
			case 'g': gui = 1; break;
			case 'f': mode = snipfull; break;
			case 'w': mode = snipwin; break;
			case 'l': mode = snipfree; break;
			case 's': save = 1; break;
			case 't': text = 1; break;
			case 'n': copy = 0; break;
			case 'v':
				printf("snipping-tool %s\n", version);
				return 0;
			default:
				Usage();
			}
		}
	}

	/* the alias of make install puts --graphic in front of every
	 * command: with other options the command line version runs */
	if (other)
		gui = 0;

	if (in && !copy && !gui) {
		/* a file in and out needs no X */
		OptLoad();
		return Cli(mode, delay, save, out, text, copy, in);
	}
	OpenX();
	OptLoad();
	if (gui)
		return Gui(argc, argv);
	return Cli(mode, delay, save, out, text, copy, in);
}
