/* See LICENSE file for copyright and license details.
 *
 * ocr.c - simple OCR for text on the screen.
 *
 * The pipeline follows gocr (Joerg Schulenburg, GPL v2+, see LICENSE):
 * the threshold comes from gocr's Otsu variant that counts only pixels
 * at contrast edges, connected pixels become boxes, boxes are sorted into
 * text lines with gocr's line marks (top, x-height, baseline), monospaced
 * text is found like gocr's measure_pitch() does and its context rules
 * fix l/I/1, O/0 and S/5. Word gaps are what the side bearings of the
 * letters don't explain.
 *
 * gocr recognizes letters with hand written rules made for scanned paper
 * and Latin script. Screen text is much smaller and the user may want
 * Cyrillic, so a box is compared with the samples of a built in database
 * (ocrdb.h, rendered from common screen fonts) like gocr's database mode
 * does: the nearest sample in shape and position wins.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>

#include "snip.h"
#include "ocrdb.h"

enum { maxalt = 6 };

struct box {
	int x0, y0, x1, y1;	/* inclusive */
	int n;			/* pixels */
	int line;
	int dead;
	int cp[maxalt];		/* candidates, best first */
	int d[maxalt];		/* their distances */
	int si[maxalt];		/* their database samples */
	int nalt;
	int space;		/* spaces before the box */
};

struct line {
	int y0, y1;		/* rows of the band */
	int base;		/* first row under the letters */
	int xh;			/* x-height */
	int cap;		/* height of capitals and ascenders */
	int caps, asc;		/* measured on digits and capitals, on b d f h k */
	int cell;		/* cell width of monospaced text, else 0 */
	int first, n;		/* boxes, sorted by x */
	int row;		/* same row as the line before (columns) */
};

struct ocr {
	int w, h;
	unsigned char *ink;	/* how much a pixel looks like text, 0..255 */
	unsigned char *bin;	/* text pixels */
	int *lab;		/* component of a pixel */
	struct box *box;
	int nbox, cap;
	struct line *ln;
	int nln;
	int full;		/* ink of a fully covered pixel */
	int medh;		/* median height of the boxes */
};

/* unpacked database */
static unsigned char (*zones)[ocrgrid * ocrgrid];
static float *lnaspect;
static signed char *mlsb, *mrsb;	/* median side bearings of the character */
static int nsamples;

/* the same letter shows up again and again: remember the answers */
enum { cachesize = 16384 };
struct memo {
	unsigned char f[ocrgrid * ocrgrid];
	int aspect, top, bot, used, n;
	int cp[maxalt], d[maxalt], si[maxalt];
};
static struct memo *cache;

/* letters that look alike in Latin and Cyrillic: latin, cyrillic */
static const unsigned short twins[][2] = {
	{ 'a', 0x430 }, { 'c', 0x441 }, { 'e', 0x435 }, { 'o', 0x43e },
	{ 'p', 0x440 }, { 'x', 0x445 }, { 'y', 0x443 }, { 'A', 0x410 },
	{ 'B', 0x412 }, { 'C', 0x421 }, { 'E', 0x415 }, { 'H', 0x41d },
	{ 'K', 0x41a }, { 'M', 0x41c }, { 'O', 0x41e }, { 'P', 0x420 },
	{ 'T', 0x422 }, { 'X', 0x425 }, { 'r', 0x433 }, { 'n', 0x43f },
	{ 'k', 0x43a }, { '3', 0x417 }
};

/* letters whose small form is the capital made smaller (Cyrillic ve,
 * ghe, de, zhe, ze, i, ka, el, em, en, o, pe, es, te, ha, tse, che, sha,
 * shcha, hard sign, yeru, soft sign, e, yu, ya): small, capital */
static const unsigned short casepairs[][2] = {
	{ 'c', 'C' }, { 'o', 'O' }, { 's', 'S' }, { 'v', 'V' }, { 'w', 'W' },
	{ 'x', 'X' }, { 'z', 'Z' },
	{ 0x432, 0x412 }, { 0x433, 0x413 }, { 0x434, 0x414 }, { 0x436, 0x416 },
	{ 0x437, 0x417 }, { 0x438, 0x418 }, { 0x43a, 0x41a }, { 0x43b, 0x41b },
	{ 0x43c, 0x41c }, { 0x43d, 0x41d }, { 0x43e, 0x41e }, { 0x43f, 0x41f },
	{ 0x441, 0x421 }, { 0x442, 0x422 }, { 0x445, 0x425 }, { 0x446, 0x426 },
	{ 0x447, 0x427 }, { 0x448, 0x428 }, { 0x449, 0x429 }, { 0x44a, 0x42a },
	{ 0x44b, 0x42b }, { 0x44c, 0x42c }, { 0x44d, 0x42d }, { 0x44e, 0x42e },
	{ 0x44f, 0x42f }
};

/* letters that look like digits: O o I l | S Z B and Cyrillic O, o,
 * Ze, ze, be */
static const unsigned short ambset[] = {
	'O', 'o', 'I', 'l', '|', 'S', 'Z', 'B', 0x41e, 0x43e, 0x417, 0x437,
	0x431, 0
};

/* capitals that may be digits: I O and Cyrillic O, Ze */
static const unsigned short notcaps[] = { 'I', 'O', 0x41e, 0x417, 0 };

/* capitals whose small letter looks different, and Cyrillic Be, Yo, Ef */
static const unsigned short surecaps[] = {
	'A', 'D', 'E', 'F', 'G', 'L', 'N', 'P', 'Q', 'R', 'Y', 0x411, 0x401,
	0x424, 0
};

/* letters as tall as the ascenders */
static const unsigned short ascset[] = { 'b', 'd', 'f', 'h', 'k', 0 };

/* ---------------------------------------------------------------- */
/* helpers */

static int
IsCyr(int c)
{
	return (c >= 0x410 && c <= 0x44f) || c == 0x401 || c == 0x451;
}

static int
IsLat(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int
IsDigit(int c)
{
	return c >= '0' && c <= '9';
}

static int
IsLetter(int c)
{
	return IsCyr(c) || IsLat(c);
}

static int
IsUpper(int c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 0x410 && c <= 0x42f) || c == 0x401;
}

static int
CmpInt(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

static int
Median(int *v, int n)
{
	if (n == 0)
		return 0;
	qsort(v, n, sizeof(int), CmpInt);
	return v[n / 2];
}

/* most frequent value, ties go to the larger one */
static int
Mode(int *v, int n)
{
	int i, best = 0, bestn = 0, run = 0;

	if (n == 0)
		return 0;
	qsort(v, n, sizeof(int), CmpInt);
	for (i = 0; i < n; i++) {
		run = (i > 0 && v[i] == v[i - 1]) ? run + 1 : 1;
		if (run >= bestn) {
			bestn = run;
			best = v[i];
		}
	}
	return best;
}

static int
CmpSample(const void *a, const void *b)
{
	return ocrdb[*(const int *)a].cp - ocrdb[*(const int *)b].cp;
}

static void
Unpack(void)
{
	int i, j, k, ml, mr, *idx, *vl, *vr;

	if (zones)
		return;
	nsamples = sizeof(ocrdb) / sizeof(ocrdb[0]);
	zones = Ecalloc(nsamples, sizeof(*zones));
	lnaspect = Ecalloc(nsamples, sizeof(float));
	for (i = 0; i < nsamples; i++) {
		for (k = 0; k < ocrgrid * ocrgrid; k++)
			zones[i][k] = k % 2 ? ocrdb[i].zone[k / 2] & 15 : ocrdb[i].zone[k / 2] >> 4;
		lnaspect[i] = log(ocrdb[i].aspect + 1.0);
	}

	/* the side bearings of one sample can be those of an unusual font:
	 * keep the median of every character too */
	mlsb = Ecalloc(nsamples, 1);
	mrsb = Ecalloc(nsamples, 1);
	idx = Ecalloc(nsamples, sizeof(int));
	vl = Ecalloc(nsamples, sizeof(int));
	vr = Ecalloc(nsamples, sizeof(int));
	for (i = 0; i < nsamples; i++)
		idx[i] = i;
	qsort(idx, nsamples, sizeof(int), CmpSample);
	for (i = 0; i < nsamples; i = k) {
		for (k = i; k < nsamples && ocrdb[idx[k]].cp == ocrdb[idx[i]].cp; k++) {
			vl[k - i] = ocrdb[idx[k]].lsb;
			vr[k - i] = ocrdb[idx[k]].rsb;
		}
		ml = Median(vl, k - i);
		mr = Median(vr, k - i);
		for (j = i; j < k; j++) {
			mlsb[idx[j]] = ml;
			mrsb[idx[j]] = mr;
		}
	}
	free(idx);
	free(vl);
	free(vr);
}

/* ---------------------------------------------------------------- */
/* ink and threshold */

static int
ColorDist(unsigned int a, unsigned int b)
{
	int r = abs((int)(a >> 16 & 0xff) - (int)(b >> 16 & 0xff));
	int g = abs((int)(a >> 8 & 0xff) - (int)(b >> 8 & 0xff));
	int d = abs((int)(a & 0xff) - (int)(b & 0xff));

	if (g > r)
		r = g;
	return d > r ? d : r;
}

static int
Bin12(unsigned int c)
{
	return (c >> 12 & 0xf00) | (c >> 8 & 0xf0) | (c >> 4 & 0xf);
}

/* how much every pixel differs from its background. The image is split
 * into regions of one color; big and thick regions are background, every
 * other pixel takes the color of the nearest background region. So text
 * on buttons, panels and selections of another color works too */
static void
Ink(struct ocr *o, struct img *im)
{
	struct reg {
		long area, r, g, b;
		int x0, y0, x1, y1, bg;
	} *rg = NULL;
	int *lab = o->lab, *near, *stack, *queue, n = o->w * o->h;
	int nrg = 0, caprg = 0, i, p, q, sp, k, x, y, c, head, tail, thick;
	unsigned int col;
	static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };

	stack = Ecalloc(n + 1, sizeof(int));
	for (i = 0; i < n; i++)
		lab[i] = -1;
	for (i = 0; i < n; i++) {
		if (lab[i] >= 0)
			continue;
		if (nrg == caprg) {
			caprg = caprg ? caprg * 2 : 1024;
			rg = Erealloc(rg, caprg * sizeof(struct reg));
		}
		memset(&rg[nrg], 0, sizeof(struct reg));
		rg[nrg].x0 = rg[nrg].x1 = i % o->w;
		rg[nrg].y0 = rg[nrg].y1 = i / o->w;
		c = Bin12(im->px[i]);
		sp = 0;
		stack[sp++] = i;
		lab[i] = nrg;
		while (sp > 0) {
			p = stack[--sp];
			x = p % o->w;
			y = p / o->w;
			col = im->px[p];
			rg[nrg].area++;
			rg[nrg].r += col >> 16 & 0xff;
			rg[nrg].g += col >> 8 & 0xff;
			rg[nrg].b += col & 0xff;
			if (x < rg[nrg].x0) rg[nrg].x0 = x;
			if (x > rg[nrg].x1) rg[nrg].x1 = x;
			if (y < rg[nrg].y0) rg[nrg].y0 = y;
			if (y > rg[nrg].y1) rg[nrg].y1 = y;
			for (k = 0; k < 4; k++) {
				if (x + dx[k] < 0 || y + dy[k] < 0 ||
				    x + dx[k] >= o->w || y + dy[k] >= o->h)
					continue;
				q = p + dy[k] * o->w + dx[k];
				if (lab[q] < 0 && Bin12(im->px[q]) == c) {
					lab[q] = nrg;
					stack[sp++] = q;
				}
			}
		}
		nrg++;
	}
	free(stack);

	/* background: big, or not too big but thick (letters are thin) */
	for (k = 0; k < nrg; k++) {
		thick = rg[k].area / (1 + (rg[k].x1 - rg[k].x0 > rg[k].y1 - rg[k].y0 ?
		        rg[k].x1 - rg[k].x0 : rg[k].y1 - rg[k].y0));
		rg[k].bg = rg[k].area >= 1500 || (rg[k].area >= 150 && thick >= 5);
		if (rg[k].area) {
			rg[k].r /= rg[k].area;
			rg[k].g /= rg[k].area;
			rg[k].b /= rg[k].area;
		}
	}

	/* every pixel gets the nearest background region (breadth first) */
	near = Ecalloc(n + 1, sizeof(int));
	queue = Ecalloc(n + 1, sizeof(int));
	head = tail = 0;
	for (i = 0; i < n; i++) {
		near[i] = -1;
		if (rg[lab[i]].bg) {
			near[i] = lab[i];
			queue[tail++] = i;
		}
	}
	if (tail == 0) {
		/* no background at all: take the biggest region */
		for (k = 0, c = 0; k < nrg; k++)
			if (rg[k].area > rg[c].area)
				c = k;
		for (i = 0; i < n; i++)
			near[i] = c;
	}
	while (head < tail) {
		p = queue[head++];
		x = p % o->w;
		y = p / o->w;
		for (k = 0; k < 4; k++) {
			if (x + dx[k] < 0 || y + dy[k] < 0 ||
			    x + dx[k] >= o->w || y + dy[k] >= o->h)
				continue;
			q = p + dy[k] * o->w + dx[k];
			if (near[q] < 0) {
				near[q] = near[p];
				queue[tail++] = q;
			}
		}
	}
	for (i = 0; i < n; i++) {
		k = near[i];
		col = (unsigned int)rg[k].r << 16 | (unsigned int)rg[k].g << 8 | (unsigned int)rg[k].b;
		o->ink[i] = ColorDist(im->px[i], col);
	}
	free(near);
	free(queue);
	free(rg);
}

/* gocr's otsu(): Otsu's threshold, but the histogram only counts pixels
 * next to a strong contrast, which works much better on images that are
 * mostly background. Returns the first value that counts as text */
static int
Otsu(unsigned char *img, int w, int h)
{
	int ihist[256], chist[256], i, j, k, maxc = 0, op1 = 0, op2 = 0, t = 1;
	int ns = 0, n1, n2, step;
	double sum = 0, csum = 0, m1, m2, sb, fmax = -1;
	unsigned char *p;

	memset(ihist, 0, sizeof(ihist));
	memset(chist, 0, sizeof(chist));
	step = h / 512 + 1;
	for (i = 0; i < h; i += step) {
		p = img + i * w;
		for (j = 0; j < w; j++) {
			ihist[p[j]]++;
			if (abs(p[j] - op1) > maxc)
				maxc = abs(p[j] - op1);
			if (abs(p[j] - op2) > maxc)
				maxc = abs(p[j] - op2);
			op2 = op1;
			op1 = p[j];
		}
	}
	for (i = 0; i < h; i += step) {
		p = img + i * w;
		for (j = 0; j < w; j++) {
			if (abs(p[j] - op1) > maxc / 4 || abs(p[j] - op2) > maxc / 4)
				chist[p[j]]++;
			op2 = op1;
			op1 = p[j];
		}
	}
	for (k = 0; k < 256; k++) {
		sum += (double)k * chist[k];
		ns += chist[k];
	}
	if (ns == 0)
		return 128;
	n1 = 0;
	for (k = 0; k < 255; k++) {
		n1 += chist[k];
		if (!n1)
			continue;
		n2 = ns - n1;
		if (n2 == 0)
			break;
		csum += (double)k * chist[k];
		m1 = csum / n1;
		m2 = (sum - csum) / n2;
		sb = (double)n1 * n2 * (m2 - m1);	/* gocr: better than squared */
		if (sb > fmax) {
			fmax = sb;
			t = k + 1;
		}
	}
	return t;
}

static void
Threshold(struct ocr *o)
{
	int i, t, n = o->w * o->h, cnt = 0;
	long sum = 0;

	t = Otsu(o->ink, o->w, o->h);
	if (t < 16)
		t = 16;
	/* the ink of solid text, half of it is the edge of a letter */
	for (i = 0; i < n; i++) {
		if (o->ink[i] >= t) {
			sum += o->ink[i];
			cnt++;
		}
	}
	o->full = cnt ? sum / cnt : 255;
	if (o->full < 32)
		o->full = 32;
	t = o->full / 2;
	for (i = 0; i < n; i++)
		o->bin[i] = o->ink[i] >= t;
}

/* ---------------------------------------------------------------- */
/* boxes */

static void
AddBox(struct ocr *o, struct box *b)
{
	if (o->nbox == o->cap) {
		o->cap = o->cap ? o->cap * 2 : 256;
		o->box = Erealloc(o->box, o->cap * sizeof(struct box));
	}
	o->box[o->nbox++] = *b;
}

/* 8-connected components of bin */
static void
Label(struct ocr *o)
{
	int *stack, sp, i, x, y, nx, ny, dx, dy, n = o->w * o->h, p;
	struct box b;

	o->nbox = 0;
	stack = Ecalloc(n + 1, sizeof(int));
	for (i = 0; i < n; i++)
		o->lab[i] = -1;
	for (i = 0; i < n; i++) {
		if (!o->bin[i] || o->lab[i] >= 0)
			continue;
		memset(&b, 0, sizeof(b));
		b.x0 = b.x1 = i % o->w;
		b.y0 = b.y1 = i / o->w;
		sp = 0;
		stack[sp++] = i;
		o->lab[i] = o->nbox;
		while (sp > 0) {
			p = stack[--sp];
			x = p % o->w;
			y = p / o->w;
			b.n++;
			if (x < b.x0) b.x0 = x;
			if (x > b.x1) b.x1 = x;
			if (y < b.y0) b.y0 = y;
			if (y > b.y1) b.y1 = y;
			for (dy = -1; dy <= 1; dy++) {
				for (dx = -1; dx <= 1; dx++) {
					nx = x + dx;
					ny = y + dy;
					if (nx < 0 || ny < 0 || nx >= o->w || ny >= o->h)
						continue;
					p = ny * o->w + nx;
					if (o->bin[p] && o->lab[p] < 0) {
						o->lab[p] = o->nbox;
						stack[sp++] = p;
					}
				}
			}
		}
		AddBox(o, &b);
	}
	free(stack);
}

static int
MedianHeight(struct ocr *o)
{
	int *v, i, n = 0, m;

	v = Ecalloc(o->nbox + 1, sizeof(int));
	for (i = 0; i < o->nbox; i++)
		if (o->box[i].n >= 4 && o->box[i].y1 - o->box[i].y0 >= 3)
			v[n++] = o->box[i].y1 - o->box[i].y0 + 1;
	m = Median(v, n);
	free(v);
	return m > 4 ? m : 4;
}

/* long thin runs are underlines, rulers and table borders: they glue
 * letters together, so remove them (gocr does the same in remove.c) */
static int
RemoveRules(struct ocr *o)
{
	int x, y, run, k, len = 3 * o->medh + 4, cnt = 0;

	for (y = 0; y < o->h; y++) {
		run = 0;
		for (x = 0; x <= o->w; x++) {
			if (x < o->w && o->bin[y * o->w + x]) {
				run++;
				continue;
			}
			if (run >= len) {
				for (k = x - run; k < x; k++)
					o->bin[y * o->w + k] = 0;
				cnt++;
			}
			run = 0;
		}
	}
	for (x = 0; x < o->w; x++) {
		run = 0;
		for (y = 0; y <= o->h; y++) {
			if (y < o->h && o->bin[y * o->w + x]) {
				run++;
				continue;
			}
			if (run >= len) {
				for (k = y - run; k < y; k++)
					o->bin[k * o->w + x] = 0;
				cnt++;
			}
			run = 0;
		}
	}
	return cnt;
}

static int
Height(struct box *b)
{
	return b->y1 - b->y0 + 1;
}

static int
Width(struct box *b)
{
	return b->x1 - b->x0 + 1;
}

/* the outline of a rectangle and nothing else: the text cursor of a
 * terminal without focus */
static int
Hollow(struct ocr *o, struct box *b)
{
	int x, y, w = Width(b), h = Height(b);

	if (w < 4 || h < 6 || b->n != 2 * (w + h) - 4)
		return 0;
	for (x = b->x0; x <= b->x1; x++)
		if (!o->bin[b->y0 * o->w + x] || !o->bin[b->y1 * o->w + x])
			return 0;
	for (y = b->y0; y <= b->y1; y++)
		if (!o->bin[y * o->w + b->x0] || !o->bin[y * o->w + b->x1])
			return 0;
	return 1;
}

/* the line under a letter or a word (menu accelerators, links) glues
 * them into shapes no font has: a flat box a row or two under taller
 * ones, as wide as they are, is an underline. The lower bar of = has a
 * flat box above it, _ has none, the foot of a serif is narrower than
 * its letter and the foot of a 2 touches it */
static void
Underlines(struct ocr *o)
{
	struct box *f, *t;
	int i, j, ov, cover, sumw, gap;

	for (i = 0; i < o->nbox; i++) {
		f = &o->box[i];
		if (f->dead || Height(f) > 2 || Width(f) < 3)
			continue;
		cover = sumw = 0;
		for (j = 0; j < o->nbox; j++) {
			t = &o->box[j];
			if (j == i || t->dead || Height(t) * 2 < o->medh)
				continue;
			gap = f->y0 - t->y1 - 1;
			if (gap < 1 || gap > 2)
				continue;
			ov = (f->x1 < t->x1 ? f->x1 : t->x1) - (f->x0 > t->x0 ? f->x0 : t->x0) + 1;
			if (ov <= 0)
				continue;
			cover += ov;
			sumw += Width(t);
		}
		if (cover * 10 >= Width(f) * 6 && cover * 10 >= sumw * 7)
			f->dead = 1;
	}
}

/* ---------------------------------------------------------------- */
/* lines */

static int
CmpBoxX(const void *a, const void *b)
{
	const struct box *p = a, *q = b;

	if (p->line != q->line)
		return p->line - q->line;
	return p->x0 - q->x0;
}

/* bands of rows covered by letter bodies are the text lines */
static void
Lines(struct ocr *o)
{
	int *prof, y, i, k, best, ov, d, bestd, start = 0, small, *hs, nh, mh;
	struct box *b;

	prof = Ecalloc(o->h + 1, sizeof(int));
	for (i = 0; i < o->nbox; i++) {
		b = &o->box[i];
		if (b->dead || Height(b) * 10 < o->medh * 6)
			continue;
		for (y = b->y0; y <= b->y1; y++)
			prof[y]++;
	}
	o->ln = Ecalloc(o->h / 2 + 2, sizeof(struct line));
	o->nln = 0;
	for (y = 0; y <= o->h; y++) {
		if (y < o->h && prof[y]) {
			if (y == 0 || !prof[y - 1])
				start = y;
			continue;
		}
		if (y > 0 && prof[y - 1]) {
			o->ln[o->nln].y0 = start;
			o->ln[o->nln].y1 = y - 1;
			o->nln++;
		}
	}
	free(prof);

	/* a band much shorter than the others is a row of accents */
	hs = Ecalloc(o->nln + 1, sizeof(int));
	for (nh = 0; nh < o->nln; nh++)
		hs[nh] = o->ln[nh].y1 - o->ln[nh].y0 + 1;
	mh = Median(hs, nh);
	free(hs);
	for (k = 0; k < o->nln; k++) {
		small = (o->ln[k].y1 - o->ln[k].y0 + 1) * 3 < mh;
		if (small && k + 1 < o->nln && o->ln[k + 1].y0 - o->ln[k].y1 <= mh / 3) {
			o->ln[k + 1].y0 = o->ln[k].y0;
			memmove(&o->ln[k], &o->ln[k + 1], (o->nln - k - 1) * sizeof(struct line));
			o->nln--;
			k--;
		}
	}

	/* every box goes to the band it overlaps most or to the nearest */
	for (i = 0; i < o->nbox; i++) {
		b = &o->box[i];
		b->line = -1;
		if (b->dead)
			continue;
		best = -1;
		bestd = 1 << 30;
		for (k = 0; k < o->nln; k++) {
			ov = (b->y1 < o->ln[k].y1 ? b->y1 : o->ln[k].y1) -
			     (b->y0 > o->ln[k].y0 ? b->y0 : o->ln[k].y0) + 1;
			if (ov > 0)
				d = -ov;
			else if (b->y1 < o->ln[k].y0)
				d = o->ln[k].y0 - b->y1;
			else
				d = b->y0 - o->ln[k].y1;
			/* accents and dots belong to the line below them */
			if (ov <= 0 && b->y1 < o->ln[k].y0)
				d -= 1;
			if (d < bestd) {
				bestd = d;
				best = k;
			}
		}
		if (best < 0 || bestd > o->medh)
			b->dead = 1;
		else
			b->line = best;
	}
}

/* text side by side (columns, buttons, panels) can share a band but not
 * the baseline: split the lines at wide gaps */
static void
Columns(struct ocr *o)
{
	struct line *nl;
	int k, i, j, n = 0, cap = o->nln + 16, start, gap = 3 * o->medh + 6;

	nl = Ecalloc(cap, sizeof(struct line));
	for (k = 0; k < o->nln; k++) {
		start = o->ln[k].first;
		for (i = o->ln[k].first; i <= o->ln[k].first + o->ln[k].n; i++) {
			if (i < o->ln[k].first + o->ln[k].n && (i == start ||
			    o->box[i].x0 - o->box[i - 1].x1 <= gap))
				continue;
			if (i == start)
				continue;
			if (n == cap) {
				cap *= 2;
				nl = Erealloc(nl, cap * sizeof(struct line));
			}
			nl[n] = o->ln[k];
			nl[n].first = start;
			nl[n].n = i - start;
			nl[n].row = start != o->ln[k].first;
			/* the band of the column alone */
			nl[n].y0 = o->box[start].y0;
			nl[n].y1 = o->box[start].y1;
			for (j = start; j < i; j++) {
				if (o->box[j].y0 < nl[n].y0)
					nl[n].y0 = o->box[j].y0;
				if (o->box[j].y1 > nl[n].y1)
					nl[n].y1 = o->box[j].y1;
			}
			n++;
			start = i;
		}
	}
	free(o->ln);
	o->ln = nl;
	o->nln = n;
}

/* a line cut by the edge of the snip (the lower half of a line, the top
 * of a text cursor) is not text */
static void
Cut(struct ocr *o)
{
	struct line *l;
	int *hs, k, i, mh, top, bot;

	if (o->nln < 2)
		return;
	hs = Ecalloc(o->nln, sizeof(int));
	for (k = 0; k < o->nln; k++)
		hs[k] = o->ln[k].y1 - o->ln[k].y0 + 1;
	mh = Median(hs, o->nln);
	free(hs);
	for (k = 0; k < o->nln; k++) {
		l = &o->ln[k];
		if ((l->y1 - l->y0 + 1) * 10 >= mh * 6)
			continue;
		top = bot = 1;
		for (i = l->first; i < l->first + l->n; i++) {
			if (o->box[i].y0 > 0)
				top = 0;
			if (o->box[i].y1 < o->h - 1)
				bot = 0;
		}
		if (!top && !bot)
			continue;
		for (i = l->first; i < l->first + l->n; i++)
			o->box[i].dead = 1;
	}
}

/* glue the parts of a letter that sit above each other: i, j, ;, the
 * Cyrillic yo and short i and the like */
static void
Glue(struct ocr *o)
{
	struct box *a, *b;
	int i, j, ov, mw;

	for (i = 0; i < o->nbox; i++) {
		a = &o->box[i];
		if (a->dead)
			continue;
		for (j = i + 1; j < o->nbox; j++) {
			b = &o->box[j];
			if (b->dead || b->line != a->line)
				continue;
			if (b->x0 > a->x1)
				break;
			ov = (a->x1 < b->x1 ? a->x1 : b->x1) - (a->x0 > b->x0 ? a->x0 : b->x0) + 1;
			mw = Width(a) < Width(b) ? Width(a) : Width(b);
			if (ov * 2 < mw && !(ov > 0 && mw <= 2))
				continue;
			/* letters next to each other don't overlap that much,
			 * but stacked parts don't share rows */
			if (a->y1 >= b->y0 && b->y1 >= a->y0 && ov * 4 < mw * 3)
				continue;
			if (b->x0 < a->x0) a->x0 = b->x0;
			if (b->x1 > a->x1) a->x1 = b->x1;
			if (b->y0 < a->y0) a->y0 = b->y0;
			if (b->y1 > a->y1) a->y1 = b->y1;
			a->n += b->n;
			b->dead = 1;
			j = i;	/* the box grew, look again */
		}
	}
}

/* baseline and x-height of every line, gocr's m3 and m2. Returns 1 when
 * the line has letters of one height only (all capitals or all short
 * lowercase letters): then the x-height is a guess to be checked */
static int
Metrics(struct ocr *o, struct line *l)
{
	struct box *b;
	int *v, *hs, n = 0, nh = 0, ns = 0, i, base, bh, hmode, taller = 0, amb = 0;

	v = Ecalloc(l->n + 1, sizeof(int));
	hs = Ecalloc(l->n + 1, sizeof(int));
	bh = l->y1 - l->y0 + 1;
	for (i = l->first; i < l->first + l->n; i++) {
		b = &o->box[i];
		if (Height(b) * 2 >= bh)
			v[n++] = b->y1 + 1;
	}
	base = n ? Mode(v, n) : l->y1 + 1;
	l->base = base;
	/* heights of the letters standing on the baseline */
	for (i = l->first; i < l->first + l->n; i++) {
		b = &o->box[i];
		if (abs(b->y1 + 1 - base) <= 1 && base - b->y0 >= 3)
			hs[nh++] = base - b->y0;
	}
	for (i = 0; i < nh; i++)
		v[i] = hs[i];
	hmode = nh ? Mode(v, nh) : bh * 2 / 3;
	for (i = 0; i < nh; i++) {
		if (hs[i] * 100 >= hmode * 120)
			taller++;
		if (hs[i] * 100 >= hmode * 55 && hs[i] * 100 <= hmode * 85)
			v[ns++] = hs[i];
	}
	if (ns >= 2 && ns * 4 >= nh) {
		l->xh = Mode(v, ns);		/* many digits and capitals */
		l->cap = hmode;
	} else if (taller > 0 && taller * 20 >= nh) {
		l->xh = hmode;			/* mostly lowercase */
		for (i = 0, n = 0; i < nh; i++)
			if (hs[i] * 100 >= hmode * 120)
				v[n++] = hs[i];
		l->cap = Mode(v, n);
	} else if (ns > 0) {
		l->xh = Mode(v, ns);		/* mostly capitals */
		l->cap = hmode;
	} else {
		l->xh = hmode;			/* can't tell */
		l->cap = hmode * 14 / 10;
		amb = 1;
	}
	if (l->xh < 3)
		l->xh = 3;
	if (l->cap <= l->xh)
		l->cap = l->xh * 14 / 10 + 1;
	free(v);
	free(hs);
	return amb;
}

/* ---------------------------------------------------------------- */
/* recognition */

/* ink of the cells of a grid laid over the box (area weighted) */
static void
Zones(struct ocr *o, int x0, int y0, int w, int h, unsigned char *f)
{
	double fx0, fx1, fy0, fy1, wx, wy, s, a, v;
	int i, j, x, y, g = ocrgrid;

	for (j = 0; j < g; j++) {
		fy0 = (double)j * h / g;
		fy1 = (double)(j + 1) * h / g;
		for (i = 0; i < g; i++) {
			fx0 = (double)i * w / g;
			fx1 = (double)(i + 1) * w / g;
			s = a = 0;
			for (y = (int)fy0; y < (int)ceil(fy1); y++) {
				wy = (y + 1 < fy1 ? y + 1 : fy1) - (y > fy0 ? y : fy0);
				if (wy <= 0)
					continue;
				for (x = (int)fx0; x < (int)ceil(fx1); x++) {
					wx = (x + 1 < fx1 ? x + 1 : fx1) - (x > fx0 ? x : fx0);
					if (wx <= 0)
						continue;
					v = o->ink[(y0 + y) * o->w + x0 + x] * 255.0 / o->full;
					s += wx * wy * (v > 255 ? 255 : v);
					a += wx * wy;
				}
			}
			f[j * g + i] = a > 0 ? (int)(15 * s / a / 255 + 0.5) : 0;
		}
	}
}

/* nearest database samples of the box x0..x1, y0..y1 on line l */
static int
Classify(struct ocr *o, struct line *l, int x0, int y0, int x1, int y1,
         int *cp, int *dist, int *si)
{
	unsigned char f[ocrgrid * ocrgrid];
	struct memo *m = NULL;
	unsigned int hash;
	float la;
	int w = x1 - x0 + 1, h = y1 - y0 + 1, top, bot, aspect, i, k, j, d, n = 0;
	int worst;

	Zones(o, x0, y0, w, h, f);
	aspect = 64 * w / h;
	if (aspect > 255)
		aspect = 255;
	la = log(aspect + 1.0);
	top = (l->base - y0) * 32 / l->xh;
	bot = (y1 + 1 - l->base) * 32 / l->xh;

	/* hash of the features */
	hash = 2166136261u;
	for (k = 0; k < ocrgrid * ocrgrid; k++)
		hash = (hash ^ f[k]) * 16777619u;
	hash = (hash ^ (unsigned int)aspect) * 16777619u;
	hash = (hash ^ (unsigned int)(top & 0xffff)) * 16777619u;
	hash = (hash ^ (unsigned int)(bot & 0xffff)) * 16777619u;
	for (j = 0; j < 8; j++) {
		m = &cache[(hash + j) % cachesize];
		if (!m->used)
			break;
		if (m->aspect == aspect && m->top == top && m->bot == bot &&
		    !memcmp(m->f, f, sizeof(f))) {
			memcpy(cp, m->cp, sizeof(m->cp));
			memcpy(dist, m->d, sizeof(m->d));
			memcpy(si, m->si, sizeof(m->si));
			return m->n;
		}
	}

	for (i = 0; i < maxalt; i++) {
		cp[i] = 0;
		dist[i] = 1 << 30;
		si[i] = 0;
	}
	for (i = 0; i < nsamples; i++) {
		worst = dist[maxalt - 1];
		d = 5 * abs(ocrdb[i].top - top) + 5 * abs(ocrdb[i].bot - bot) +
		    (int)(100 * fabs(lnaspect[i] - la));
		if (d >= worst)
			continue;
		for (k = 0; k < ocrgrid * ocrgrid && d < worst; k++)
			d += abs(zones[i][k] - f[k]);
		if (d >= worst)
			continue;
		/* keep one entry per character, sorted */
		for (k = 0; k < maxalt && cp[k] != ocrdb[i].cp; k++)
			;
		if (k < maxalt && d >= dist[k])
			continue;
		if (k == maxalt)
			k = maxalt - 1;
		for (j = k; j > 0 && dist[j - 1] > d; j--) {
			cp[j] = cp[j - 1];
			dist[j] = dist[j - 1];
			si[j] = si[j - 1];
		}
		cp[j] = ocrdb[i].cp;
		dist[j] = d;
		si[j] = i;
	}
	for (n = 0; n < maxalt && cp[n]; n++)
		;
	if (j < 8 && !m->used) {
		memcpy(m->f, f, sizeof(f));
		m->aspect = aspect;
		m->top = top;
		m->bot = bot;
		memcpy(m->cp, cp, sizeof(m->cp));
		memcpy(m->d, dist, sizeof(m->d));
		memcpy(m->si, si, sizeof(m->si));
		m->n = n;
		m->used = 1;
	}
	return n;
}

/* tight box of the text pixels in a column range of a box */
static int
Tight(struct ocr *o, struct box *b, int x0, int x1, struct box *out)
{
	int x, y, found = 0;

	*out = *b;
	out->x0 = x1;
	out->x1 = x0;
	out->y0 = b->y1;
	out->y1 = b->y0;
	out->n = 0;
	for (y = b->y0; y <= b->y1; y++) {
		for (x = x0; x <= x1; x++) {
			if (!o->bin[y * o->w + x])
				continue;
			found = 1;
			out->n++;
			if (x < out->x0) out->x0 = x;
			if (x > out->x1) out->x1 = x;
			if (y < out->y0) out->y0 = y;
			if (y > out->y1) out->y1 = y;
		}
	}
	return found;
}

static void
Recognize(struct ocr *o, struct line *l, struct box *b)
{
	b->nalt = Classify(o, l, b->x0, b->y0, b->x1, b->y1, b->cp, b->d, b->si);
}

/* thin strokes can break into pieces (a '/' into '/' and '.'): glue a
 * small piece to the box it touches when together they match better */
static void
Mend(struct ocr *o, struct line *l)
{
	struct box *a, *b, m;
	int i, small;

	for (i = l->first + 1; i < l->first + l->n; i++) {
		a = &o->box[i - 1];
		b = &o->box[i];
		if (a->dead || b->dead || !a->nalt || !b->nalt)
			continue;
		if (b->x0 - a->x1 - 1 > 0)
			continue;
		small = Height(a) * 2 < l->xh || Height(b) * 2 < l->xh ||
		        Width(a) <= 2 || Width(b) <= 2;
		if (!small)
			continue;
		m = *a;
		if (b->x0 < m.x0) m.x0 = b->x0;
		if (b->x1 > m.x1) m.x1 = b->x1;
		if (b->y0 < m.y0) m.y0 = b->y0;
		if (b->y1 > m.y1) m.y1 = b->y1;
		m.n = a->n + b->n;
		Recognize(o, l, &m);
		if (!m.nalt || m.d[0] >= (a->d[0] > b->d[0] ? a->d[0] : b->d[0]))
			continue;
		*b = m;
		a->dead = 1;
	}
}

/* sum of the distances of the best matches of a line */
static int
LineDist(struct ocr *o, struct line *l)
{
	int i, sum = 0;

	for (i = l->first; i < l->first + l->n; i++) {
		Recognize(o, l, &o->box[i]);
		if (o->box[i].nalt)
			sum += o->box[i].d[0];
	}
	return sum;
}

/* letters that touch make one box that matches badly. Cut it at its
 * weakest columns into all possible pieces and take the sequence of
 * letters that matches best (dynamic programming), gocr's idea of
 * try_to_divide_boxes(). Returns the number of pieces put into out */
static int
Segment(struct ocr *o, struct line *l, struct box *b, struct box *out)
{
	enum { maxcut = 10, penalty = 20 };
	struct box seg[maxcut + 2][maxcut + 2];
	int cost[maxcut + 2][maxcut + 2], best[maxcut + 2], from[maxcut + 2];
	int pos[maxcut + 2], col[512], cand[512], nc = 0, np, i, j, k, x, y, t, n;

	if (Width(b) * 10 < l->xh * 7 || b->d[0] < 60 || Width(b) > 500)
		return 0;
	for (x = b->x0; x <= b->x1; x++) {
		col[x - b->x0] = 0;
		for (y = b->y0; y <= b->y1; y++)
			col[x - b->x0] += o->bin[y * o->w + x];
	}
	/* weak columns: local minima of the ink */
	for (x = b->x0 + 2; x <= b->x1 - 1; x++) {
		i = x - b->x0;
		if (col[i] <= col[i - 1] && col[i] <= col[i + 1])
			cand[nc++] = x;
	}
	/* keep the weakest ones */
	for (i = 1; i < nc; i++) {
		t = cand[i];
		for (j = i; j > 0 && col[cand[j - 1] - b->x0] > col[t - b->x0]; j--)
			cand[j] = cand[j - 1];
		cand[j] = t;
	}
	if (nc > maxcut)
		nc = maxcut;
	qsort(cand, nc, sizeof(int), CmpInt);
	np = 0;
	pos[np++] = b->x0;
	for (i = 0; i < nc; i++)
		pos[np++] = cand[i];
	pos[np++] = b->x1 + 1;

	for (i = 0; i < np; i++) {
		for (j = i + 1; j < np; j++) {
			cost[i][j] = 1 << 28;
			if (pos[j] - pos[i] > 2 * l->xh + 4 && !(i == 0 && j == np - 1))
				continue;
			if (!Tight(o, b, pos[i], pos[j] - 1, &seg[i][j]))
				continue;
			Recognize(o, l, &seg[i][j]);
			/* distance per x-height of width, so that one bad match
			 * over two letters is not cheaper than two good ones */
			if (seg[i][j].nalt)
				cost[i][j] = seg[i][j].d[0] * Width(&seg[i][j]) / l->xh + penalty;
		}
	}
	best[0] = 0;
	for (j = 1; j < np; j++) {
		best[j] = 1 << 29;
		for (i = 0; i < j; i++) {
			if (best[i] + cost[i][j] < best[j]) {
				best[j] = best[i] + cost[i][j];
				from[j] = i;
			}
		}
	}
	if (best[np - 1] >= cost[0][np - 1] || from[np - 1] == 0)
		return 0;
	/* walk back */
	n = 0;
	for (j = np - 1; j > 0; j = from[j])
		n++;
	k = n;
	for (j = np - 1; j > 0; j = from[j])
		out[--k] = seg[from[j]][j];
	return n;
}

/* ---------------------------------------------------------------- */
/* words */

static int
Script(int c)
{
	if (IsCyr(c))
		return 2;
	if (IsLat(c))
		return 1;
	return 0;
}

/* letters that exist in one script only */
static int
Strong(int c)
{
	size_t i;

	for (i = 0; i < sizeof(twins) / sizeof(twins[0]); i++)
		if (twins[i][0] == c || twins[i][1] == c)
			return 0;
	return IsLetter(c) && c != 'l' && c != 'I' && c != 'i';
}

/* such letters differ in height only, the line marks tell them apart
 * better than their shapes */
static void
CaseFix(struct line *l, struct box *b)
{
	size_t i;
	int top = l->base - b->y0, cap, low;

	/* round letters overshoot by a pixel; without digits and capitals
	 * to measure, the cap height may be the height of b d f h k */
	cap = l->caps > 0 ? l->caps : l->cap;
	low = top * 100 <= l->xh * 115 + 50 && top * 2 <= l->xh + cap;
	for (i = 0; i < sizeof(casepairs) / sizeof(casepairs[0]); i++) {
		if (b->cp[0] == casepairs[i][0] || b->cp[0] == casepairs[i][1]) {
			b->cp[0] = casepairs[i][low ? 0 : 1];
			return;
		}
	}
}

/* the look-alike letter of the other script, or 0 */
static int
Twin(int c)
{
	size_t i;

	for (i = 0; i < sizeof(twins) / sizeof(twins[0]); i++) {
		if (twins[i][0] == c)
			return twins[i][1];
		if (twins[i][1] == c)
			return twins[i][0];
	}
	return 0;
}

/* pick a candidate of the wanted kind if it is nearly as good */
static void
Prefer(struct box *b, int kind)
{
	int k, c, ok;

	for (k = 1; k < b->nalt; k++) {
		c = b->cp[k];
		ok = (kind == 1 && IsLat(c)) || (kind == 2 && IsCyr(c)) ||
		     (kind == 3 && IsDigit(c)) || (kind == 4 && IsLetter(c));
		if (!ok)
			continue;
		if (b->d[k] * 100 > b->d[0] * 120 + 20)
			return;
		b->cp[0] = c;
		b->d[0] = b->d[k];
		b->si[0] = b->si[k];
		return;
	}
}

/* is c in set, a list of code points ending with 0 */
static int
InSet(const unsigned short *set, int c)
{
	int i;

	for (i = 0; set[i]; i++)
		if (set[i] == c)
			return 1;
	return 0;
}

/* Russian words have no digits: 0 is the Cyrillic O, 3 is Ze, 6 is be
 * (the case comes later from the height), the others become the
 * nearest Cyrillic letter */
static void
CyrDigit(struct box *b)
{
	if (b->cp[0] == '0')
		b->cp[0] = 0x41e;
	else if (b->cp[0] == '3')
		b->cp[0] = 0x417;
	else if (b->cp[0] == '6')
		b->cp[0] = 0x431;
	else
		Prefer(b, 2);
}

/* make a word (boxes a..e-1) consistent: one script, and digits among
 * digits, letters among letters */
static void
Word(struct ocr *o, int a, int e, int linescript)
{
	struct box *b;
	int i, k, lat = 0, cyr = 0, dig = 0, let = 0, c, p, n, kind;

	for (i = a; i < e; i++) {
		c = o->box[i].cp[0];
		if (IsDigit(c) && c != '0' && c != '1')
			dig++;
		if (IsLetter(c) && !InSet(ambset, c))
			let++;
		if (Strong(c) && IsLat(c))
			lat++;
		if (Strong(c) && IsCyr(c))
			cyr++;
		/* i, l and I have no Cyrillic twin; mail and paths are Latin */
		if (c == 'i' || c == 'l' || c == 'I' || c == '@' || c == '/' ||
		    c == '\\' || c == '_')
			lat++;
	}
	if (cyr > lat)
		kind = 2;
	else if (lat > cyr)
		kind = 1;
	else
		kind = linescript;
	for (i = a; i < e; i++) {
		b = &o->box[i];
		c = b->cp[0];
		p = i > a ? o->box[i - 1].cp[0] : 0;
		n = i + 1 < e ? o->box[i + 1].cp[0] : 0;
		if (IsDigit(c) && IsLetter(p) && IsLetter(n))
			Prefer(b, 4);
		else if (IsDigit(c) && cyr > lat && let > dig && (IsLetter(p) || IsLetter(n)))
			CyrDigit(b);
		else if (IsLetter(c) && (IsDigit(p) || !p) && (IsDigit(n) || !n) &&
		         (IsDigit(p) || IsDigit(n)))
			Prefer(b, 3);
		else if (IsLetter(c) && InSet(ambset, c) && dig > let)
			Prefer(b, 3);
		c = b->cp[0];
		if (kind && IsLetter(c) && Script(c) != kind) {
			k = Twin(c);
			if (k)
				b->cp[0] = k;
			else
				Prefer(b, kind);
			/* no such letter: a Cyrillic be among Latin letters
			 * is a 6 */
			if (Script(b->cp[0]) != kind)
				Prefer(b, 3);
		}
	}
}

/* in monospaced text a narrow letter (i l . ,) still takes a whole cell,
 * in proportional text its neighbours come closer */
static int
NarrowInCells(struct ocr *o, struct line *l, int cell)
{
	struct box *a, *b;
	int i, narrow = 0, ok = 0, md;

	for (i = l->first + 1; i < l->first + l->n; i++) {
		a = &o->box[i - 1];
		b = &o->box[i];
		if (Width(a) * 2 > cell && Width(b) * 2 > cell)
			continue;
		md = ((b->x0 + b->x1) - (a->x0 + a->x1) + 1) / 2;
		if (md > 2 * cell)
			continue;	/* a space between */
		narrow++;
		if (md >= cell - 1)
			ok++;
	}
	return narrow == 0 ? 1 : ok * 10 >= narrow * 8;
}

/* cell width of a monospaced line, 0 for proportional text: most centres
 * of letters are whole cells apart (spaces make two or more) and every
 * letter fits into a cell. After gocr's measure_pitch() */
static int
Cell(struct ocr *o, struct line *l)
{
	struct box *a, *b;
	int *md, n = 0, i, k, near = 0, maxw = 0, cell = 0;

	md = Ecalloc(l->n + 1, sizeof(int));
	for (i = l->first; i < l->first + l->n; i++) {
		b = &o->box[i];
		if (Width(b) > maxw && Height(b) * 2 > l->xh)
			maxw = Width(b);
		if (i == l->first)
			continue;
		a = &o->box[i - 1];
		md[n++] = ((b->x0 + b->x1) - (a->x0 + a->x1) + 1) / 2;
	}
	if (n >= 5) {
		qsort(md, n, sizeof(int), CmpInt);
		k = md[n / 3];
		for (i = 0; k > 0 && i < n; i++)
			if (abs(md[i] - (md[i] + k / 2) / k * k) <= 1)
				near++;
		if (near * 10 >= n * 7 && maxw <= k + 1 && k >= 3 &&
		    NarrowInCells(o, l, k))
			cell = k;
	}
	free(md);
	return cell;
}

/* is c a candidate of b about as good as the best one */
static int
Near(struct box *b, int c)
{
	int k;

	if (b->cp[0] == c)
		return 1;
	for (k = 1; k < b->nalt; k++)
		if (b->cp[k] == c)
			return b->d[k] <= b->d[0] * 6 / 5 + 5;
	return 0;
}

/* Near() for c or its look-alike of the other script */
static int
Like(struct box *b, int c)
{
	int k = Twin(c);

	return Near(b, c) || (k && Near(b, k));
}

/* is c as tall as the capitals. Before CaseFix() only the capitals
 * whose small letter looks different are sure */
static int
CapLike(int c, int fixed)
{
	if (c >= '2' && c <= '9' && c != '3')
		return 1;
	if (fixed)
		return IsUpper(c) && !InSet(notcaps, c);
	return InSet(surecaps, c);
}

/* heights of the digits and capitals and of b d f h k on a line, 0
 * when it has none */
static void
Heights(struct ocr *o, struct line *l, int fixed)
{
	struct box *b;
	int *hc, *ha, nc = 0, na = 0, i, c;

	hc = Ecalloc(l->n + 1, sizeof(int));
	ha = Ecalloc(l->n + 1, sizeof(int));
	for (i = l->first; i < l->first + l->n; i++) {
		b = &o->box[i];
		c = b->cp[0];
		if (b->dead || !b->nalt || abs(b->y1 + 1 - l->base) > 1)
			continue;
		if (CapLike(c, fixed))
			hc[nc++] = l->base - b->y0;
		else if (InSet(ascset, c))
			ha[na++] = l->base - b->y0;
	}
	l->caps = Mode(hc, nc);
	l->asc = Mode(ha, na);
	free(hc);
	free(ha);
}

/* a stroke taller than the capitals is an l, in fonts with ascenders
 * taller than the capitals a shorter one is an I. 0 if it can't tell */
static int
Bar(struct line *l, struct box *b)
{
	int h = l->base - b->y0;

	if (l->caps == 0 || abs(b->y1 + 1 - l->base) > 1)
		return 0;
	if (h > l->caps && (l->asc == 0 || h * 2 > l->caps + l->asc))
		return 'l';
	if (l->asc > l->caps && h * 2 <= l->caps + l->asc)
		return 'I';
	return 0;
}

/* l I 1 | by height and by neighbours */
static int
Stroke(struct line *l, struct box *b, int p, int n)
{
	int t;

	if (IsDigit(p) || IsDigit(n))
		return '1';
	/* a one is as tall as the capitals, so the height can't tell it
	 * from an I */
	t = Bar(l, b);
	if (t == 'l' || (t == 'I' && b->cp[0] != '1'))
		return t;
	if (IsLetter(n) && p == ' ' && IsUpper(n))
		return 'I';
	if ((IsLetter(p) && !IsUpper(p)) || (IsLetter(n) && !IsUpper(n)))
		return 'l';
	if (IsUpper(p) || IsUpper(n))
		return 'I';
	return b->cp[0];
}

/* no letter, digit or apostrophe right before or after box i */
static int
Lone(struct ocr *o, int i, int a, int e)
{
	int c;

	if (i > a && !o->box[i].space) {
		c = o->box[i - 1].cp[0];
		if (IsLetter(c) || IsDigit(c) || c == '\'')
			return 0;
	}
	if (i + 1 < e && !o->box[i + 1].space) {
		c = o->box[i + 1].cp[0];
		if (IsLetter(c) || IsDigit(c) || c == '\'')
			return 0;
	}
	return 1;
}

/* gocr's context_correction(): l I 1 | and O 0 and S 5 by height and by
 * neighbours. Monospaced fonts draw them apart (the one has a flag, the
 * zero a dot), so there the neighbours only choose between shapes that
 * match about equally well */
static void
Context(struct ocr *o, struct line *l)
{
	struct box *b;
	int i, c, p, n, t, h, a = l->first, e = l->first + l->n;

	/* what the height says: a stroke no taller than an x is an i that
	 * lost its dot (often to the arm of an f), a j goes below the
	 * baseline, and strokes taller than the capitals show how tall
	 * b d f h k are */
	for (i = a; i < e; i++) {
		b = &o->box[i];
		c = b->cp[0];
		h = l->base - b->y0;
		if (abs(b->y1 + 1 - l->base) > 1)
			continue;
		if (c == 'j')
			b->cp[0] = 'i';
		if ((c == 'l' || c == 'I' || c == '1' || c == '|') && h <= l->xh &&
		    (l->caps == 0 || h < l->caps) && (l->asc == 0 || h < l->asc))
			b->cp[0] = 'i';
		if ((c == 'l' || c == 'I' || c == '|') && l->caps > 0 && h > l->caps &&
		    h > l->asc)
			l->asc = h;
	}
	/* l or I by height first, so that the neighbours are right below;
	 * a one taller than the other digits is an l */
	for (i = a; i < e; i++) {
		b = &o->box[i];
		c = b->cp[0];
		t = Bar(l, b);
		if ((c == 'l' || c == 'I' || c == '|') && t)
			b->cp[0] = t;
		if (c == '1' && t == 'l')
			b->cp[0] = t;
		/* the aspect of a flat box says little: a dash is longer than
		 * a letter (fills a cell), a hyphen shorter */
		if ((c == '-' || c == 0x2014) && l->cell > 0)
			b->cp[0] = Width(b) * 10 >= l->cell * 9 ? 0x2014 : '-';
		else if (c == '-' || c == 0x2014)
			b->cp[0] = Width(b) * 2 >= l->xh * 3 ? 0x2014 : '-';
	}
	for (i = a; i < e; i++) {
		b = &o->box[i];
		c = b->cp[0];
		p = i > a && !b->space ? o->box[i - 1].cp[0] : ' ';
		n = i + 1 < e && !o->box[i + 1].space ? o->box[i + 1].cp[0] : ' ';
		t = c;
		if (c == 'l' || c == 'I' || c == '1' || c == '|') {
			t = Stroke(l, b, p, n);
		} else if (c == 'O' || c == '0' || c == 0x41e) {
			if (IsDigit(p) || IsDigit(n))
				t = '0';
			else if (c == '0' && (IsCyr(p) || IsCyr(n)))
				t = 0x41e;
			else if (c == '0' && (IsLetter(p) || IsLetter(n)))
				t = 'O';
		} else if (c == 'S' || c == '5') {
			if (IsDigit(p) || IsDigit(n))
				t = '5';
			else if (IsLetter(p) || IsLetter(n))
				t = 'S';
		}
		if (t != c && (l->cell == 0 || Like(b, t)))
			b->cp[0] = t;
	}

	/* a lone round capital is a zero, but not the Russian word O;
	 * a lone stroke is a one in code. A small o is never a zero */
	for (i = a; i < e; i++) {
		b = &o->box[i];
		c = b->cp[0];
		if (!Lone(o, i, a, e))
			continue;
		if ((c == 'O' || c == 0x41e) && !(i + 1 < e && IsCyr(o->box[i + 1].cp[0])))
			t = '0';
		else if (l->cell > 0 && (c == 'I' || c == 'l' || c == '|'))
			t = '1';
		else
			continue;
		if (l->cell == 0 || Like(b, t))
			b->cp[0] = t;
	}

	/* the Cyrillic yeru is a soft sign and a stroke next to it */
	for (i = a; i + 1 < e; i++) {
		b = &o->box[i];
		c = o->box[i + 1].cp[0];
		if ((b->cp[0] == 0x44c || b->cp[0] == 0x42c) &&
		    !o->box[i + 1].space &&
		    (c == 'l' || c == 'I' || c == '1' || c == '|' || c == 'i') &&
		    o->box[i + 1].x0 - b->x1 <= 2 + l->xh / 4) {
			b->cp[0] = b->cp[0] == 0x42c ? 0x42b : 0x44b;
			b->x1 = o->box[i + 1].x1;
			o->box[i + 1].dead = 1;
		}
	}
}

/* ---------------------------------------------------------------- */
/* output */

static void
PutCp(struct buf *b, int cp)
{
	char u[4];
	int n;

	/* the ligatures fi and fl are two letters */
	if (cp == 0xfb01 || cp == 0xfb02) {
		BufByte(b, 'f');
		BufByte(b, cp == 0xfb01 ? 'i' : 'l');
		return;
	}
	if (cp < 0x80) {
		u[0] = cp;
		n = 1;
	} else if (cp < 0x800) {
		u[0] = 0xc0 | cp >> 6;
		u[1] = 0x80 | (cp & 0x3f);
		n = 2;
	} else {
		u[0] = 0xe0 | cp >> 12;
		u[1] = 0x80 | (cp >> 6 & 0x3f);
		u[2] = 0x80 | (cp & 0x3f);
		n = 3;
	}
	BufAdd(b, u, n);
}

/* side bearings, in pixels, that explain the gap between a and b. The
 * nearest sample may come from an unusual font: letters take the mean of
 * its bearings and the median of all fonts, punctuation, whose bearings
 * differ most from font to font, the smaller of the two */
static int
Bearings(struct line *l, struct box *a, struct box *b)
{
	int ra = ocrdb[a->si[0]].rsb, lb = ocrdb[b->si[0]].lsb;
	int mra = mrsb[a->si[0]], mlb = mlsb[b->si[0]];
	int wa = IsLetter(a->cp[0]) || IsDigit(a->cp[0]);
	int wb = IsLetter(b->cp[0]) || IsDigit(b->cp[0]);

	if (wa && wb) {
		ra = (ra + mra) / 2;
		lb = (lb + mlb) / 2;
	}
	if (!wa && mra < ra)
		ra = mra;
	if (!wb && mlb < lb)
		lb = mlb;
	return ((ra + lb) * l->xh + 16) / 32;
}

static void
Spaces(struct ocr *o, struct line *l, int mono)
{
	struct box *a, *b;
	int i, gap;

	/* columns only when most of the text is monospaced */
	l->cell = mono;
	for (i = l->first; i < l->first + l->n; i++) {
		b = &o->box[i];
		b->space = 0;
		if (i == l->first)
			continue;
		a = &o->box[i - 1];
		if (l->cell > 0) {
			/* monospaced: keep the columns */
			b->space = ((b->x0 + b->x1) - (a->x0 + a->x1) + l->cell) /
			           (2 * l->cell) - 1;
			if (b->space < 0)
				b->space = 0;
			continue;
		}
		/* the gap the side bearings explain is not a space */
		gap = b->x0 - a->x1 - 1 - Bearings(l, a, b);
		if (gap >= (l->xh + 2) / 4 && gap >= 2)
			b->space = 1;
	}
}

/* ---------------------------------------------------------------- */

char *
Ocr(struct img *im)
{
	struct ocr o;
	struct buf out = {0};
	struct box *b, parts[16];
	struct line *l;
	int i, k, n, e, lat, cyr, script, left, mono, *cnt;

	Unpack();
	cache = Ecalloc(cachesize, sizeof(struct memo));
	memset(&o, 0, sizeof(o));
	o.w = im->w;
	o.h = im->h;
	o.ink = Ecalloc((size_t)o.w * o.h, 1);
	o.bin = Ecalloc((size_t)o.w * o.h, 1);
	o.lab = Ecalloc((size_t)o.w * o.h, sizeof(int));

	Ink(&o, im);
	Threshold(&o);
	Label(&o);
	o.medh = MedianHeight(&o);
	if (RemoveRules(&o) > 0) {
		Label(&o);
		o.medh = MedianHeight(&o);
	}
	/* pictures, specks and solid blocks (text cursors) are not text */
	for (i = 0; i < o.nbox; i++) {
		b = &o.box[i];
		if (Height(b) > 3 * o.medh + 4 || Width(b) > 8 * o.medh + 8)
			b->dead = 1;
		if (Width(b) * 2 >= o.medh && Height(b) * 10 >= o.medh * 8 &&
		    b->n * 10 >= Width(b) * Height(b) * 9)
			b->dead = 1;
		if (Height(b) * 10 >= o.medh * 8 && Hollow(&o, b))
			b->dead = 1;
	}
	Underlines(&o);
	Lines(&o);
	qsort(o.box, o.nbox, sizeof(struct box), CmpBoxX);
	Glue(&o);

	/* drop dead boxes, find the boxes of every line */
	for (i = k = 0; i < o.nbox; i++)
		if (!o.box[i].dead && o.box[i].line >= 0)
			o.box[k++] = o.box[i];
	o.nbox = k;
	cnt = Ecalloc(o.nln + 1, sizeof(int));
	for (i = 0; i < o.nbox; i++)
		cnt[o.box[i].line]++;
	for (k = 0, n = 0; k < o.nln; k++) {
		o.ln[k].first = n;
		o.ln[k].n = cnt[k];
		n += cnt[k];
	}
	free(cnt);
	Columns(&o);
	Cut(&o);
	for (k = 0; k < o.nln; k++) {
		l = &o.ln[k];
		if (Metrics(&o, l)) {
			/* capitals or short letters? the better match decides */
			n = l->xh;
			e = LineDist(&o, l);
			l->xh = n * 7 / 10;
			if (LineDist(&o, l) >= e) {
				l->xh = n;
			} else {
				l->cap = n;
			}
		}
	}

	/* recognize, mend broken letters, cut boxes that match badly */
	for (k = 0; k < o.nln; k++) {
		l = &o.ln[k];
		for (i = l->first; i < l->first + l->n; i++)
			Recognize(&o, l, &o.box[i]);
		Mend(&o, l);
		for (i = l->first; i < l->first + l->n; i++) {
			b = &o.box[i];
			if (b->dead || !b->nalt || (n = Segment(&o, l, b, parts)) < 2)
				continue;
			while (o.nbox + n > o.cap) {
				o.cap *= 2;
				o.box = Erealloc(o.box, o.cap * sizeof(struct box));
			}
			memmove(&o.box[i + n], &o.box[i + 1], (o.nbox - i - 1) * sizeof(struct box));
			memcpy(&o.box[i], parts, n * sizeof(struct box));
			o.nbox += n - 1;
			l->n += n - 1;
			for (e = k + 1; e < o.nln; e++)
				o.ln[e].first += n - 1;
			i += n - 1;
		}
	}

	/* drop the boxes glued away */
	for (k = 0, n = 0; k < o.nln; k++) {
		l = &o.ln[k];
		e = n;
		for (i = l->first; i < l->first + l->n; i++)
			if (!o.box[i].dead)
				o.box[n++] = o.box[i];
		l->first = e;
		l->n = n - e;
	}
	o.nbox = n;

	left = o.w;
	for (k = 0; k < o.nln; k++)
		if (!o.ln[k].row && o.ln[k].n > 0 && o.box[o.ln[k].first].x0 < left)
			left = o.box[o.ln[k].first].x0;
	/* monospaced text: all lines share the cell width of the majority */
	cnt = Ecalloc(o.nln + 1, sizeof(int));
	for (k = 0, n = 0, e = 0; k < o.nln; k++) {
		i = Cell(&o, &o.ln[k]);
		if (i > 0)
			cnt[n++] = i;
		else if (o.ln[k].n >= 5)
			e++;
	}
	mono = n > e ? Median(cnt, n) : 0;
	free(cnt);
	for (k = 0; k < o.nln; k++) {
		l = &o.ln[k];
		if (l->n == 0)
			continue;
		Spaces(&o, l, mono);
		/* main script of the line */
		lat = cyr = 0;
		for (i = l->first; i < l->first + l->n; i++) {
			if (Strong(o.box[i].cp[0]))
				IsCyr(o.box[i].cp[0]) ? cyr++ : lat++;
		}
		script = cyr > lat ? 2 : (lat > cyr ? 1 : 0);
		for (i = l->first; i < l->first + l->n; i = e) {
			for (e = i + 1; e < l->first + l->n && !o.box[e].space; e++)
				;
			Word(&o, i, e, script);
		}
		Heights(&o, l, 0);
		for (i = l->first; i < l->first + l->n; i++)
			if (o.box[i].nalt)
				CaseFix(l, &o.box[i]);
		Heights(&o, l, 1);
		Context(&o, l);
		if (l->row && out.len > 0 && out.data[out.len - 1] == '\n') {
			out.len--;
			BufStr(&out, "    ");
		} else if (l->cell > 0 && l->n > 0) {
			/* monospaced text keeps its indentation */
			for (n = (o.box[l->first].x0 - left + l->cell / 2) / l->cell; n > 0; n--)
				BufByte(&out, ' ');
		}
		for (i = l->first; i < l->first + l->n; i++) {
			b = &o.box[i];
			if (b->dead || !b->nalt)
				continue;
			for (n = 0; n < b->space; n++)
				BufByte(&out, ' ');
			/* '' is " */
			if (b->cp[0] == '\'' && i + 1 < l->first + l->n &&
			    o.box[i + 1].cp[0] == '\'' && !o.box[i + 1].space) {
				BufByte(&out, '"');
				o.box[i + 1].dead = 1;
				continue;
			}
			PutCp(&out, b->cp[0]);
		}
		BufByte(&out, '\n');
	}
	BufByte(&out, '\0');

	free(cache);
	cache = NULL;
	free(o.ink);
	free(o.bin);
	free(o.lab);
	free(o.box);
	free(o.ln);
	return (char *)out.data;
}
