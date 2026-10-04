/* See LICENSE file for copyright and license details. */

/* image: w*h pixels 0xAARRGGBB, row by row (alpha is used by icons only) */
struct img {
	int w, h;
	unsigned int *px;
};

/* growing byte buffer */
struct buf {
	unsigned char *data;
	size_t len, cap;
};

/* snip modes, same order as in the "New" menu */
enum { snipfree, sniprect, snipwin, snipfull };

/* settings changed in the "Options" dialog, kept in ~/.config */
struct opts {
	int hidetext;		/* hide the instruction text */
	int autocopy;		/* always copy snips to the clipboard */
	int asksave;		/* prompt to save snips before exiting */
	int veil;		/* show the white screen overlay */
	int showink;		/* draw the selection ink onto the snip */
	unsigned int inkcol;	/* selection ink color */
	int autosave;		/* also save every snip to $HOME */
	int delay;		/* seconds to wait before a snip */
	int mode;		/* last used snip mode */
	unsigned int pencol;	/* custom pen */
	int penw;		/* 0 fine, 1 medium, 2 thick */
	int pentip;		/* 0 round, 1 chisel */
	char dir[4096];		/* last save directory */
};

/* snip.c */
extern Display *dpy;
extern Window root;
extern Visual *vis;
extern int scr, depth, sw, sh;
extern struct opts opt;
extern Atom atomutf8;

void Die(const char *fmt, ...);
void Warn(const char *fmt, ...);
void *Ecalloc(size_t n, size_t sz);
void *Erealloc(void *p, size_t sz);
char *Estrdup(const char *s);
void BufAdd(struct buf *b, const void *p, size_t n);
void BufByte(struct buf *b, int c);
void BufStr(struct buf *b, const char *s);
void BufFree(struct buf *b);
void ImgNew(struct img *im, int w, int h);
void ImgFree(struct img *im);
void ImgCrop(struct img *src, int x, int y, int w, int h, struct img *dst);
long Now(void);
void MSleep(int ms);
int Native32(XImage *xi);
unsigned long Pixel(unsigned int rgb);
int Grab(struct img *im);
int Snip(int mode, struct img *out, struct img *inked);
int Activewin(int *x, int *y, int *w, int *h);
int ClipCopy(const unsigned char *data, size_t len, int text);
int SavePng(const char *path, struct img *im);
void SaveName(char *dst, size_t sz, const char *dir);
void OptLoad(void);
void OptSave(void);

/* png.c */
int PngWrite(struct img *im, struct buf *out);
int PngRead(const unsigned char *data, size_t len, struct img *im);

/* gui.c */
int Gui(int argc, char *argv[]);

/* ocr.c */
char *Ocr(struct img *im);
