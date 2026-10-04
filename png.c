/* See LICENSE file for copyright and license details.
 *
 * png.c - PNG writer and reader without zlib.
 *
 * Writer: per row filter choice, LZ77 with hash chains and one step lazy
 * matching, dynamic Huffman codes (falls back to fixed or stored blocks
 * when those are smaller).
 * Reader: plain inflate (canonical Huffman decoding bit by bit), all
 * color types and bit depths, Adam7. Only needed for the embedded icons,
 * so it is written to be short rather than fast.
 */
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>

#include "snip.h"

enum {
	wsize = 32768,		/* LZ77 window */
	hsize = 32768,		/* hash table size, power of two */
	maxchain = 48,		/* how many candidates to try */
	nicelen = 128,		/* stop searching at this length */
	lazylen = 32,		/* don't look one byte ahead after this */
	maxtok = 65536		/* tokens per block */
};

static const unsigned short lenbase[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char lenextra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short distbase[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
	8193, 12289, 16385, 24577
};
static const unsigned char distextra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
/* order of the code length code lengths in a dynamic block header */
static const unsigned char clorder[19] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};
static const unsigned char pngsig[8] = {
	137, 80, 78, 71, 13, 10, 26, 10
};

static unsigned long crctab[256];

/* ---------------------------------------------------------------- */
/* checksums */

static void
CrcInit(void)
{
	unsigned long c;
	int n, k;

	if (crctab[1])
		return;
	for (n = 0; n < 256; n++) {
		c = n;
		for (k = 0; k < 8; k++) {
			if (c & 1)
				c = 0xedb88320UL ^ (c >> 1);
			else
				c = c >> 1;
		}
		crctab[n] = c;
	}
}

static unsigned long
Crc(unsigned long crc, const unsigned char *p, size_t n)
{
	size_t i;

	crc ^= 0xffffffffUL;
	for (i = 0; i < n; i++)
		crc = crctab[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
	return crc ^ 0xffffffffUL;
}

static unsigned long
Adler(const unsigned char *p, size_t n)
{
	unsigned long a = 1, b = 0;
	size_t i, k;

	while (n > 0) {
		/* 5552 is the most bytes that can't overflow b */
		k = n < 5552 ? n : 5552;
		for (i = 0; i < k; i++) {
			a += p[i];
			b += a;
		}
		a %= 65521;
		b %= 65521;
		p += k;
		n -= k;
	}
	return (b << 16) | a;
}

static void
Put32(struct buf *b, unsigned long v)
{
	BufByte(b, (v >> 24) & 0xff);
	BufByte(b, (v >> 16) & 0xff);
	BufByte(b, (v >> 8) & 0xff);
	BufByte(b, v & 0xff);
}

static unsigned long
Get32(const unsigned char *p)
{
	return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
	       (unsigned long)p[2] << 8 | p[3];
}

/* ---------------------------------------------------------------- */
/* deflate */

struct bitw {
	struct buf *out;
	unsigned long acc;
	int n;
};

struct deflate {
	const unsigned char *in;
	int len;
	int *head;		/* newest position for every hash */
	int *prev;		/* previous position with the same hash */
	unsigned short *lit;	/* token: literal byte or match length */
	unsigned short *dist;	/* token: match distance, 0 for literal */
	int ntok;
	int bstart;		/* first input byte of the current block */
	unsigned char lensym[259];
	struct bitw bw;
};

static void
PutBits(struct bitw *w, unsigned int v, int n)
{
	w->acc |= (unsigned long)v << w->n;
	w->n += n;
	while (w->n >= 8) {
		BufByte(w->out, w->acc & 0xff);
		w->acc >>= 8;
		w->n -= 8;
	}
}

static void
FlushBits(struct bitw *w)
{
	if (w->n > 0)
		BufByte(w->out, w->acc & 0xff);
	w->acc = 0;
	w->n = 0;
}

static unsigned int
Reverse(unsigned int v, int n)
{
	unsigned int r = 0;
	int i;

	for (i = 0; i < n; i++) {
		r = (r << 1) | (v & 1);
		v >>= 1;
	}
	return r;
}

static int
DistSym(int d)
{
	int i;

	for (i = 29; i > 0; i--)
		if (d >= distbase[i])
			break;
	return i;
}

/* take the smaller head of the leaf queue and the node queue */
static int
Pick(const unsigned long *w, int *li, int m, int *ni, int k)
{
	if (*li < m && (*ni >= k || w[*li] <= w[*ni]))
		return (*li)++;
	return (*ni)++;
}

/* Huffman code lengths, at most maxbits long, for n symbols */
static void
HuffLens(const unsigned int *freq, int n, int maxbits, unsigned char *lens)
{
	unsigned long w[576], total;
	int sym[288], par[576], dep[576], cnt[16];
	int i, j, k, m, a, b, li, ni, len;

	memset(lens, 0, n);
	m = 0;
	for (i = 0; i < n; i++)
		if (freq[i])
			sym[m++] = i;
	if (m == 0)
		return;
	if (m == 1) {
		/* a lone code would be incomplete, add a dummy one */
		lens[sym[0]] = 1;
		lens[sym[0] == 0 ? 1 : 0] = 1;
		return;
	}
	/* sort symbols by frequency, rarest first */
	for (i = 1; i < m; i++) {
		k = sym[i];
		for (j = i; j > 0 && freq[sym[j - 1]] > freq[k]; j--)
			sym[j] = sym[j - 1];
		sym[j] = k;
	}
	for (i = 0; i < m; i++)
		w[i] = freq[sym[i]];
	/* two queue Huffman: leaves are 0..m-1, inner nodes m..2m-2 */
	li = 0;
	ni = m;
	for (k = m; k < 2 * m - 1; k++) {
		a = Pick(w, &li, m, &ni, k);
		b = Pick(w, &li, m, &ni, k);
		w[k] = w[a] + w[b];
		par[a] = k;
		par[b] = k;
	}
	dep[2 * m - 2] = 0;
	for (k = 2 * m - 3; k >= 0; k--)
		dep[k] = dep[par[k]] + 1;

	/* count codes per length, cut the long ones to maxbits */
	memset(cnt, 0, sizeof(cnt));
	for (i = 0; i < m; i++) {
		len = dep[i];
		if (len > maxbits)
			len = maxbits;
		cnt[len]++;
	}
	/* the cut made the code oversubscribed: move codes down a level
	 * until the Kraft sum is exact again */
	total = 0;
	for (i = 1; i <= maxbits; i++)
		total += (unsigned long)cnt[i] << (maxbits - i);
	while (total > (1UL << maxbits)) {
		cnt[maxbits]--;
		for (i = maxbits - 1; i > 0; i--) {
			if (cnt[i]) {
				cnt[i]--;
				cnt[i + 1] += 2;
				break;
			}
		}
		total--;
	}
	/* longest codes go to the rarest symbols */
	k = 0;
	for (len = maxbits; len >= 1; len--)
		for (j = 0; j < cnt[len]; j++)
			lens[sym[k++]] = len;
}

/* canonical codes, bit reversed because deflate sends them MSB first */
static void
HuffCodes(const unsigned char *lens, int n, unsigned short *codes)
{
	int cnt[16], next[16], i, code;

	memset(cnt, 0, sizeof(cnt));
	for (i = 0; i < n; i++)
		cnt[lens[i]]++;
	cnt[0] = 0;
	code = 0;
	for (i = 1; i < 16; i++) {
		code = (code + cnt[i - 1]) << 1;
		next[i] = code;
	}
	for (i = 0; i < n; i++) {
		codes[i] = 0;
		if (lens[i])
			codes[i] = Reverse(next[lens[i]]++, lens[i]);
	}
}

/* run length code the code lengths: rle[] = symbol | extra << 8 */
static int
Rle(const unsigned char *lens, int n, unsigned short *rle, unsigned int *freq)
{
	int i = 0, run, k, cnt = 0;

	while (i < n) {
		run = 1;
		while (i + run < n && lens[i + run] == lens[i])
			run++;
		if (lens[i] == 0) {
			while (run >= 11) {
				k = run < 138 ? run : 138;
				rle[cnt++] = 18 | (k - 11) << 8;
				freq[18]++;
				run -= k;
				i += k;
			}
			if (run >= 3) {
				rle[cnt++] = 17 | (run - 3) << 8;
				freq[17]++;
				i += run;
				run = 0;
			}
		} else {
			rle[cnt++] = lens[i];
			freq[lens[i]]++;
			i++;
			run--;
			while (run >= 3) {
				k = run < 6 ? run : 6;
				rle[cnt++] = 16 | (k - 3) << 8;
				freq[16]++;
				run -= k;
				i += k;
			}
		}
		while (run > 0) {
			rle[cnt++] = lens[i];
			freq[lens[i]]++;
			i++;
			run--;
		}
	}
	return cnt;
}

static void
FixedLens(unsigned char *ll, unsigned char *dl)
{
	int i;

	for (i = 0; i < 288; i++) {
		if (i < 144)
			ll[i] = 8;
		else if (i < 256)
			ll[i] = 9;
		else if (i < 280)
			ll[i] = 7;
		else
			ll[i] = 8;
	}
	for (i = 0; i < 30; i++)
		dl[i] = 5;
}

/* bits needed for the tokens of the block with the given lengths */
static unsigned long
DataBits(const unsigned int *lf, const unsigned int *df,
         const unsigned char *ll, const unsigned char *dl)
{
	unsigned long bits = 0;
	int i;

	for (i = 0; i < 286; i++) {
		bits += (unsigned long)lf[i] * ll[i];
		if (i > 256)
			bits += (unsigned long)lf[i] * lenextra[i - 257];
	}
	for (i = 0; i < 30; i++)
		bits += (unsigned long)df[i] * (dl[i] + distextra[i]);
	return bits;
}

static void
PutTokens(struct deflate *d, const unsigned char *ll,
          const unsigned char *dl)
{
	unsigned short lc[288], dc[30];
	int i, s, len, dist;

	HuffCodes(ll, 288, lc);
	HuffCodes(dl, 30, dc);
	for (i = 0; i < d->ntok; i++) {
		if (d->dist[i] == 0) {
			PutBits(&d->bw, lc[d->lit[i]], ll[d->lit[i]]);
			continue;
		}
		len = d->lit[i];
		dist = d->dist[i];
		s = d->lensym[len];
		PutBits(&d->bw, lc[257 + s], ll[257 + s]);
		if (lenextra[s])
			PutBits(&d->bw, len - lenbase[s], lenextra[s]);
		s = DistSym(dist);
		PutBits(&d->bw, dc[s], dl[s]);
		if (distextra[s])
			PutBits(&d->bw, dist - distbase[s], distextra[s]);
	}
	PutBits(&d->bw, lc[256], ll[256]);
}

static void
PutStored(struct deflate *d, int end, int last)
{
	int pos = d->bstart, n, i;

	do {
		n = end - pos;
		if (n > 65535)
			n = 65535;
		PutBits(&d->bw, (last && pos + n == end) ? 1 : 0, 1);
		PutBits(&d->bw, 0, 2);
		FlushBits(&d->bw);
		BufByte(d->bw.out, n & 0xff);
		BufByte(d->bw.out, n >> 8);
		BufByte(d->bw.out, ~n & 0xff);
		BufByte(d->bw.out, (~n >> 8) & 0xff);
		for (i = 0; i < n; i++)
			BufByte(d->bw.out, d->in[pos + i]);
		pos += n;
	} while (pos < end);
}

/* write the collected tokens (input bytes bstart..end) as one block */
static void
Block(struct deflate *d, int end, int last)
{
	unsigned int lf[286], df[30], cf[19];
	unsigned char ll[288], dl[30], cl[19], all[316], fl[288], fd[30];
	unsigned short rle[316], cc[19];
	unsigned long dynbits, fixbits, storebits;
	int i, nlit, ndist, nclen, nrle, s;

	memset(lf, 0, sizeof(lf));
	memset(df, 0, sizeof(df));
	memset(cf, 0, sizeof(cf));
	for (i = 0; i < d->ntok; i++) {
		if (d->dist[i] == 0) {
			lf[d->lit[i]]++;
		} else {
			lf[257 + d->lensym[d->lit[i]]]++;
			df[DistSym(d->dist[i])]++;
		}
	}
	lf[256] = 1;

	memset(ll, 0, sizeof(ll));
	HuffLens(lf, 286, 15, ll);
	HuffLens(df, 30, 15, dl);
	nlit = 286;
	while (nlit > 257 && ll[nlit - 1] == 0)
		nlit--;
	ndist = 30;
	while (ndist > 1 && dl[ndist - 1] == 0)
		ndist--;
	memcpy(all, ll, nlit);
	memcpy(all + nlit, dl, ndist);
	nrle = Rle(all, nlit + ndist, rle, cf);
	HuffLens(cf, 19, 7, cl);
	nclen = 19;
	while (nclen > 4 && cl[clorder[nclen - 1]] == 0)
		nclen--;

	dynbits = 3 + 5 + 5 + 4 + 3 * nclen;
	for (i = 0; i < nrle; i++) {
		s = rle[i] & 0xff;
		dynbits += cl[s];
		if (s == 16)
			dynbits += 2;
		else if (s == 17)
			dynbits += 3;
		else if (s == 18)
			dynbits += 7;
	}
	dynbits += DataBits(lf, df, ll, dl);
	FixedLens(fl, fd);
	fixbits = 3 + DataBits(lf, df, fl, fd);
	storebits = 8UL * (end - d->bstart) + 40UL * ((end - d->bstart) / 65535 + 1);

	if (storebits < dynbits && storebits < fixbits) {
		PutStored(d, end, last);
	} else if (fixbits <= dynbits) {
		PutBits(&d->bw, last, 1);
		PutBits(&d->bw, 1, 2);
		PutTokens(d, fl, fd);
	} else {
		PutBits(&d->bw, last, 1);
		PutBits(&d->bw, 2, 2);
		PutBits(&d->bw, nlit - 257, 5);
		PutBits(&d->bw, ndist - 1, 5);
		PutBits(&d->bw, nclen - 4, 4);
		for (i = 0; i < nclen; i++)
			PutBits(&d->bw, cl[clorder[i]], 3);
		HuffCodes(cl, 19, cc);
		for (i = 0; i < nrle; i++) {
			s = rle[i] & 0xff;
			PutBits(&d->bw, cc[s], cl[s]);
			if (s == 16)
				PutBits(&d->bw, rle[i] >> 8, 2);
			else if (s == 17)
				PutBits(&d->bw, rle[i] >> 8, 3);
			else if (s == 18)
				PutBits(&d->bw, rle[i] >> 8, 7);
		}
		PutTokens(d, ll, dl);
	}
	d->ntok = 0;
	d->bstart = end;
}

static int
Hash(const unsigned char *p)
{
	return ((p[0] << 10) ^ (p[1] << 5) ^ p[2]) & (hsize - 1);
}

static void
Insert(struct deflate *d, int pos)
{
	int h;

	if (pos + 2 >= d->len)
		return;
	h = Hash(d->in + pos);
	d->prev[pos & (wsize - 1)] = d->head[h];
	d->head[h] = pos;
}

/* longest match for pos among the earlier positions with the same hash */
static int
Match(struct deflate *d, int pos, int *dist)
{
	const unsigned char *in = d->in;
	int cand, next, chain = maxchain, best = 0, len, max;

	max = d->len - pos;
	if (max > 258)
		max = 258;
	if (max < 3)
		return 0;
	cand = d->head[Hash(in + pos)];
	while (cand >= 0 && pos - cand <= wsize && chain-- > 0) {
		if (in[cand + best] == in[pos + best]) {
			len = 0;
			while (len < max && in[cand + len] == in[pos + len])
				len++;
			if (len > best) {
				best = len;
				*dist = pos - cand;
				if (len >= nicelen || len == max)
					break;
			}
		}
		next = d->prev[cand & (wsize - 1)];
		if (next >= cand)
			break;	/* slot was reused by a newer position */
		cand = next;
	}
	return best >= 3 ? best : 0;
}

static void
Token(struct deflate *d, int lit, int dist, int end)
{
	d->lit[d->ntok] = lit;
	d->dist[d->ntok] = dist;
	d->ntok++;
	if (d->ntok == maxtok)
		Block(d, end, 0);
}

static int
Deflate(const unsigned char *in, int n, struct buf *out)
{
	struct deflate d;
	int pos, i, len, dist, len2, dist2;

	memset(&d, 0, sizeof(d));
	d.in = in;
	d.len = n;
	d.bw.out = out;
	d.head = malloc(hsize * sizeof(int));
	d.prev = malloc(wsize * sizeof(int));
	d.lit = malloc(maxtok * sizeof(unsigned short));
	d.dist = malloc(maxtok * sizeof(unsigned short));
	if (!d.head || !d.prev || !d.lit || !d.dist) {
		free(d.head);
		free(d.prev);
		free(d.lit);
		free(d.dist);
		return -1;
	}
	for (i = 0; i < hsize; i++)
		d.head[i] = -1;
	for (i = 0; i < wsize; i++)
		d.prev[i] = -1;
	for (i = 3; i <= 258; i++) {
		len = 28;
		while (lenbase[len] > i)
			len--;
		d.lensym[i] = len;
	}

	pos = 0;
	while (pos < n) {
		len = Match(&d, pos, &dist);
		Insert(&d, pos);
		if (len >= 3 && len < lazylen && pos + 1 < n) {
			/* a longer match one byte later wins */
			len2 = Match(&d, pos + 1, &dist2);
			if (len2 > len) {
				Token(&d, in[pos], 0, pos + 1);
				pos++;
				Insert(&d, pos);
				len = len2;
				dist = dist2;
			}
		}
		if (len >= 3) {
			Token(&d, len, dist, pos + len);
			for (i = 1; i < len; i++)
				Insert(&d, pos + i);
			pos += len;
		} else {
			Token(&d, in[pos], 0, pos + 1);
			pos++;
		}
	}
	Block(&d, n, 1);
	FlushBits(&d.bw);

	free(d.head);
	free(d.prev);
	free(d.lit);
	free(d.dist);
	return 0;
}

/* ---------------------------------------------------------------- */
/* PNG writer */

static int
Paeth(int a, int b, int c)
{
	int p, pa, pb, pc;

	p = a + b - c;
	pa = abs(p - a);
	pb = abs(p - b);
	pc = abs(p - c);
	if (pa <= pb && pa <= pc)
		return a;
	if (pb <= pc)
		return b;
	return c;
}

/* filter one row with filter f into dst, return the "sum of abs" cost */
static long
FilterRow(int f, const unsigned char *cur, const unsigned char *up,
          int n, unsigned char *dst)
{
	long cost = 0;
	int i, a, b, c, v;

	for (i = 0; i < n; i++) {
		a = i >= 3 ? cur[i - 3] : 0;
		b = up[i];
		c = i >= 3 ? up[i - 3] : 0;
		if (f == 0)
			v = cur[i];
		else if (f == 1)
			v = cur[i] - a;
		else if (f == 2)
			v = cur[i] - b;
		else if (f == 3)
			v = cur[i] - (a + b) / 2;
		else
			v = cur[i] - Paeth(a, b, c);
		v &= 0xff;
		dst[i] = v;
		cost += v < 128 ? v : 256 - v;
	}
	return cost;
}

static void
Chunk(struct buf *out, const char *type, const unsigned char *data,
      size_t n)
{
	unsigned long crc;

	Put32(out, n);
	BufAdd(out, type, 4);
	if (n)
		BufAdd(out, data, n);
	crc = Crc(0, (const unsigned char *)type, 4);
	crc = Crc(crc, data, n);
	Put32(out, crc);
}

int
PngWrite(struct img *im, struct buf *out)
{
	struct buf z = {0};
	unsigned char hdr[13], *raw, *cur, *up, *tmp, *best;
	long cost, bestcost;
	size_t stride, rawlen;
	int x, y, f, bestf;
	unsigned int p;

	CrcInit();
	stride = (size_t)im->w * 3;
	rawlen = (stride + 1) * im->h;
	if (rawlen > 0x7fff0000UL)
		return -1;
	raw = malloc(rawlen);
	cur = malloc(stride);
	up = calloc(1, stride);
	tmp = malloc(stride);
	best = malloc(stride);
	if (!raw || !cur || !up || !tmp || !best) {
		free(raw);
		free(cur);
		free(up);
		free(tmp);
		free(best);
		return -1;
	}
	for (y = 0; y < im->h; y++) {
		for (x = 0; x < im->w; x++) {
			p = im->px[(size_t)y * im->w + x];
			cur[x * 3] = (p >> 16) & 0xff;
			cur[x * 3 + 1] = (p >> 8) & 0xff;
			cur[x * 3 + 2] = p & 0xff;
		}
		bestf = 0;
		bestcost = -1;
		for (f = 0; f < 5; f++) {
			cost = FilterRow(f, cur, up, stride, tmp);
			if (bestcost < 0 || cost < bestcost) {
				bestcost = cost;
				bestf = f;
				memcpy(best, tmp, stride);
			}
		}
		raw[y * (stride + 1)] = bestf;
		memcpy(raw + y * (stride + 1) + 1, best, stride);
		memcpy(up, cur, stride);
	}

	/* zlib stream: header, deflate data, adler32 */
	BufByte(&z, 0x78);
	BufByte(&z, 0x9c);
	if (Deflate(raw, rawlen, &z) < 0) {
		free(raw);
		free(cur);
		free(up);
		free(tmp);
		free(best);
		BufFree(&z);
		return -1;
	}
	Put32(&z, Adler(raw, rawlen));

	hdr[0] = (im->w >> 24) & 0xff;
	hdr[1] = (im->w >> 16) & 0xff;
	hdr[2] = (im->w >> 8) & 0xff;
	hdr[3] = im->w & 0xff;
	hdr[4] = (im->h >> 24) & 0xff;
	hdr[5] = (im->h >> 16) & 0xff;
	hdr[6] = (im->h >> 8) & 0xff;
	hdr[7] = im->h & 0xff;
	hdr[8] = 8;	/* bit depth */
	hdr[9] = 2;	/* RGB */
	hdr[10] = 0;
	hdr[11] = 0;
	hdr[12] = 0;
	BufAdd(out, pngsig, 8);
	Chunk(out, "IHDR", hdr, 13);
	Chunk(out, "IDAT", z.data, z.len);
	Chunk(out, "IEND", NULL, 0);

	free(raw);
	free(cur);
	free(up);
	free(tmp);
	free(best);
	BufFree(&z);
	return 0;
}

/* ---------------------------------------------------------------- */
/* inflate */

struct inf {
	const unsigned char *p;
	size_t n, pos;
	unsigned long acc;
	int nb;
	int err;
	struct buf *out;
};

struct huff {
	short cnt[16];
	short sym[288];
};

static int
GetBits(struct inf *s, int n)
{
	int v;

	while (s->nb < n) {
		if (s->pos >= s->n) {
			s->err = 1;
			return 0;
		}
		s->acc |= (unsigned long)s->p[s->pos++] << s->nb;
		s->nb += 8;
	}
	v = s->acc & ((1UL << n) - 1);
	s->acc >>= n;
	s->nb -= n;
	return v;
}

static int
HuffBuild(struct huff *h, const unsigned char *lens, int n)
{
	short offs[16];
	int i, left;

	memset(h->cnt, 0, sizeof(h->cnt));
	for (i = 0; i < n; i++)
		h->cnt[lens[i]]++;
	left = 1;
	for (i = 1; i < 16; i++) {
		left <<= 1;
		left -= h->cnt[i];
		if (left < 0)
			return -1;
	}
	offs[1] = 0;
	for (i = 1; i < 15; i++)
		offs[i + 1] = offs[i] + h->cnt[i];
	for (i = 0; i < n; i++)
		if (lens[i])
			h->sym[offs[lens[i]]++] = i;
	return 0;
}

static int
Decode(struct inf *s, const struct huff *h)
{
	int code = 0, first = 0, index = 0, len, cnt;

	for (len = 1; len < 16; len++) {
		code |= GetBits(s, 1);
		cnt = h->cnt[len];
		if (code - cnt < first)
			return h->sym[index + (code - first)];
		index += cnt;
		first += cnt;
		first <<= 1;
		code <<= 1;
	}
	s->err = 1;
	return -1;
}

static int
Codes(struct inf *s, const struct huff *lh, const struct huff *dh)
{
	int sym, len, dist, i;

	for (;;) {
		sym = Decode(s, lh);
		if (s->err || sym < 0)
			return -1;
		if (sym < 256) {
			BufByte(s->out, sym);
			continue;
		}
		if (sym == 256)
			return 0;
		sym -= 257;
		if (sym >= 29)
			return -1;
		len = lenbase[sym] + GetBits(s, lenextra[sym]);
		sym = Decode(s, dh);
		if (sym < 0 || sym >= 30)
			return -1;
		dist = distbase[sym] + GetBits(s, distextra[sym]);
		if (s->err || (size_t)dist > s->out->len)
			return -1;
		for (i = 0; i < len; i++)
			BufByte(s->out, s->out->data[s->out->len - dist]);
	}
}

static int
Stored(struct inf *s)
{
	unsigned int n;

	s->acc = 0;	/* skip to the byte boundary */
	s->nb = 0;
	if (s->pos + 4 > s->n)
		return -1;
	n = s->p[s->pos] | s->p[s->pos + 1] << 8;
	if ((n ^ 0xffff) != (unsigned int)(s->p[s->pos + 2] | s->p[s->pos + 3] << 8))
		return -1;
	s->pos += 4;
	if (s->pos + n > s->n)
		return -1;
	BufAdd(s->out, s->p + s->pos, n);
	s->pos += n;
	return 0;
}

static int
Fixed(struct inf *s)
{
	unsigned char ll[288], dl[30];
	struct huff lh, dh;

	FixedLens(ll, dl);
	HuffBuild(&lh, ll, 288);
	HuffBuild(&dh, dl, 30);
	return Codes(s, &lh, &dh);
}

static int
Dynamic(struct inf *s)
{
	unsigned char lens[320], cl[19];
	struct huff lh, dh, ch;
	int nlit, ndist, nclen, i, sym, rep, prev;

	nlit = GetBits(s, 5) + 257;
	ndist = GetBits(s, 5) + 1;
	nclen = GetBits(s, 4) + 4;
	if (nlit > 286 || ndist > 30)
		return -1;
	memset(cl, 0, sizeof(cl));
	for (i = 0; i < nclen; i++)
		cl[clorder[i]] = GetBits(s, 3);
	if (HuffBuild(&ch, cl, 19) < 0)
		return -1;
	i = 0;
	while (i < nlit + ndist) {
		sym = Decode(s, &ch);
		if (s->err || sym < 0)
			return -1;
		if (sym < 16) {
			lens[i++] = sym;
			continue;
		}
		prev = 0;
		if (sym == 16) {
			if (i == 0)
				return -1;
			prev = lens[i - 1];
			rep = 3 + GetBits(s, 2);
		} else if (sym == 17) {
			rep = 3 + GetBits(s, 3);
		} else {
			rep = 11 + GetBits(s, 7);
		}
		if (i + rep > nlit + ndist)
			return -1;
		while (rep--)
			lens[i++] = prev;
	}
	if (HuffBuild(&lh, lens, nlit) < 0 || HuffBuild(&dh, lens + nlit, ndist) < 0)
		return -1;
	return Codes(s, &lh, &dh);
}

static int
Inflate(const unsigned char *p, size_t n, struct buf *out)
{
	struct inf s;
	int last, type, r;

	memset(&s, 0, sizeof(s));
	s.p = p;
	s.n = n;
	s.out = out;
	do {
		last = GetBits(&s, 1);
		type = GetBits(&s, 2);
		if (s.err)
			return -1;
		if (type == 0)
			r = Stored(&s);
		else if (type == 1)
			r = Fixed(&s);
		else if (type == 2)
			r = Dynamic(&s);
		else
			r = -1;
		if (r < 0 || s.err)
			return -1;
	} while (!last);
	return 0;
}

/* ---------------------------------------------------------------- */
/* PNG reader */

struct pnginfo {
	int w, h, depth, type, chans;
	unsigned char pal[256][4];
	int npal;
	int trns;		/* color key present (gray and RGB) */
	int key[3];
};

static int
Sample(const unsigned char *row, int i, int bits)
{
	int bit;

	if (bits == 8)
		return row[i];
	if (bits == 16)
		return row[i * 2];	/* high byte is enough */
	bit = i * bits;
	return (row[bit / 8] >> (8 - bits - bit % 8)) & ((1 << bits) - 1);
}

static int
Key16(const unsigned char *row, int i, int bits)
{
	if (bits == 16)
		return row[i * 2] << 8 | row[i * 2 + 1];
	return Sample(row, i, bits);
}

static unsigned int
PngPixel(struct pnginfo *pi, const unsigned char *row, int x)
{
	int r, g, b, a = 255, v, max;

	max = (1 << (pi->depth < 8 ? pi->depth : 8)) - 1;
	switch (pi->type) {
	case 0:
		v = Sample(row, x, pi->depth);
		if (pi->trns && Key16(row, x, pi->depth) == pi->key[0])
			a = 0;
		r = g = b = v * 255 / max;
		break;
	case 2:
		r = Sample(row, x * 3, pi->depth);
		g = Sample(row, x * 3 + 1, pi->depth);
		b = Sample(row, x * 3 + 2, pi->depth);
		if (pi->trns && Key16(row, x * 3, pi->depth) == pi->key[0] &&
		    Key16(row, x * 3 + 1, pi->depth) == pi->key[1] &&
		    Key16(row, x * 3 + 2, pi->depth) == pi->key[2])
			a = 0;
		break;
	case 3:
		v = Sample(row, x, pi->depth);
		r = pi->pal[v][0];
		g = pi->pal[v][1];
		b = pi->pal[v][2];
		a = pi->pal[v][3];
		break;
	case 4:
		r = g = b = Sample(row, x * 2, pi->depth);
		a = Sample(row, x * 2 + 1, pi->depth);
		break;
	default:
		r = Sample(row, x * 4, pi->depth);
		g = Sample(row, x * 4 + 1, pi->depth);
		b = Sample(row, x * 4 + 2, pi->depth);
		a = Sample(row, x * 4 + 3, pi->depth);
		break;
	}
	return (unsigned int)a << 24 | r << 16 | g << 8 | b;
}

static void
Unfilter(int f, unsigned char *cur, const unsigned char *up, int n, int bpp)
{
	int i, a, b, c;

	for (i = 0; i < n; i++) {
		a = i >= bpp ? cur[i - bpp] : 0;
		b = up ? up[i] : 0;
		c = (up && i >= bpp) ? up[i - bpp] : 0;
		if (f == 1)
			cur[i] += a;
		else if (f == 2)
			cur[i] += b;
		else if (f == 3)
			cur[i] += (a + b) / 2;
		else if (f == 4)
			cur[i] += Paeth(a, b, c);
	}
}

/* decode one (sub)image of w x h pixels placed at x0,y0 step dx,dy */
static int
Pass(struct pnginfo *pi, unsigned char **data, size_t *left, struct img *im,
     int x0, int y0, int dx, int dy)
{
	unsigned char *row, *up = NULL;
	size_t rowlen;
	int w, h, x, y, bpp, f;

	w = (pi->w - x0 + dx - 1) / dx;
	h = (pi->h - y0 + dy - 1) / dy;
	if (w <= 0 || h <= 0)
		return 0;
	rowlen = ((size_t)w * pi->chans * pi->depth + 7) / 8;
	bpp = (pi->chans * pi->depth + 7) / 8;
	for (y = 0; y < h; y++) {
		if (*left < rowlen + 1)
			return -1;
		f = (*data)[0];
		row = *data + 1;
		if (f > 4)
			return -1;
		Unfilter(f, row, up, rowlen, bpp);
		for (x = 0; x < w; x++)
			im->px[(size_t)(y0 + y * dy) * pi->w + x0 + x * dx] =
				PngPixel(pi, row, x);
		up = row;
		*data += rowlen + 1;
		*left -= rowlen + 1;
	}
	return 0;
}

int
PngRead(const unsigned char *d, size_t n, struct img *im)
{
	static const int ax[7] = { 0, 4, 0, 2, 0, 1, 0 };
	static const int ay[7] = { 0, 0, 4, 0, 2, 0, 1 };
	static const int adx[7] = { 8, 8, 4, 4, 2, 2, 1 };
	static const int ady[7] = { 8, 8, 8, 4, 4, 2, 2 };
	struct pnginfo pi;
	struct buf idat = {0}, raw = {0};
	const unsigned char *p;
	unsigned char *q;
	size_t pos, len, left;
	int interlace = 0, i, r = -1;

	memset(&pi, 0, sizeof(pi));
	im->px = NULL;
	if (n < 8 || memcmp(d, pngsig, 8) != 0)
		return -1;
	for (i = 0; i < 256; i++)
		pi.pal[i][3] = 255;
	pos = 8;
	while (pos + 12 <= n) {
		len = Get32(d + pos);
		p = d + pos + 8;
		if (len > n - pos - 12)
			goto end;
		if (!memcmp(d + pos + 4, "IHDR", 4) && len >= 13) {
			pi.w = Get32(p);
			pi.h = Get32(p + 4);
			pi.depth = p[8];
			pi.type = p[9];
			interlace = p[12];
		} else if (!memcmp(d + pos + 4, "PLTE", 4)) {
			pi.npal = len / 3 > 256 ? 256 : len / 3;
			for (i = 0; i < pi.npal; i++) {
				pi.pal[i][0] = p[i * 3];
				pi.pal[i][1] = p[i * 3 + 1];
				pi.pal[i][2] = p[i * 3 + 2];
			}
		} else if (!memcmp(d + pos + 4, "tRNS", 4)) {
			if (pi.type == 3) {
				for (i = 0; i < (int)len && i < 256; i++)
					pi.pal[i][3] = p[i];
			} else if (pi.type == 0 && len >= 2) {
				pi.trns = 1;
				pi.key[0] = p[0] << 8 | p[1];
			} else if (pi.type == 2 && len >= 6) {
				pi.trns = 1;
				for (i = 0; i < 3; i++)
					pi.key[i] = p[i * 2] << 8 | p[i * 2 + 1];
			}
		} else if (!memcmp(d + pos + 4, "IDAT", 4)) {
			BufAdd(&idat, p, len);
		} else if (!memcmp(d + pos + 4, "IEND", 4)) {
			break;
		}
		pos += len + 12;
	}
	switch (pi.type) {
	case 0: pi.chans = 1; break;
	case 2: pi.chans = 3; break;
	case 3: pi.chans = 1; break;
	case 4: pi.chans = 2; break;
	case 6: pi.chans = 4; break;
	default: goto end;
	}
	if (pi.w <= 0 || pi.h <= 0 || pi.w > 16384 || pi.h > 16384)
		goto end;
	if (pi.depth != 1 && pi.depth != 2 && pi.depth != 4 &&
	    pi.depth != 8 && pi.depth != 16)
		goto end;
	/* the stream has a 2 byte zlib header, the adler32 is not checked */
	if (idat.len < 2 || Inflate(idat.data + 2, idat.len - 2, &raw) < 0)
		goto end;

	ImgNew(im, pi.w, pi.h);
	q = raw.data;
	left = raw.len;
	if (interlace) {
		for (i = 0; i < 7; i++)
			if (Pass(&pi, &q, &left, im, ax[i], ay[i], adx[i], ady[i]) < 0)
				goto end;
	} else if (Pass(&pi, &q, &left, im, 0, 0, 1, 1) < 0) {
		goto end;
	}
	r = 0;
end:
	if (r < 0)
		ImgFree(im);
	BufFree(&idat);
	BufFree(&raw);
	return r;
}
