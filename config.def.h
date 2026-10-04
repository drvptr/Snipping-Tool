/* See LICENSE file for copyright and license details. */
/* Copy this file to config.h and edit it to taste, then rebuild. */

/* screen overlay: strength of the white veil, 0..255 */
static const unsigned int veilalpha = 96;

/* width of the selection ink in pixels */
static const int inkwidth = 2;

/* ms to wait after hiding the window before the screen is read;
 * a compositor needs that much to fade the window out */
static const int hidedelay = 300;

/* name of saved snips, strftime(3) format */
static const char savename[] = "snip-%Y-%m-%d-%H%M%S.png";

/* settings used until they are changed in the "Options" dialog,
 * which stores them in ~/.config/snipping-tool/config */
static const struct opts defopts = {
	.hidetext = 0,
	.autocopy = 1,
	.asksave = 1,
	.veil = 1,
	.showink = 1,
	.inkcol = 0xff0000,
	.autosave = 0,
	.delay = 0,
	.mode = sniprect,
	.pencol = 0x008000,	/* "Custom Pen" */
	.penw = 1,
	.pentip = 0,
	.dir = "",
};

/* pens of the "Pen" menu: red, blue, black */
static const unsigned int pencols[] = { 0xff0000, 0x0000ff, 0x000000 };

/* pen width in pixels: fine, medium, thick */
static const int penwidths[] = { 2, 3, 6 };

/* highlighter: color and the size of its chisel nib */
static const unsigned int markcol = 0xffff00;
static const int markwidth = 5;
static const int markheight = 14;

/* the eraser removes strokes closer than this to the pointer */
static const int eraserad = 4;

/* colors offered by the color lists */
static const struct swatch {
	unsigned int col;
	const char *name;
} palette[] = {
	{ 0x000000, "Black" },
	{ 0x808080, "Gray" },
	{ 0xc0c0c0, "Light Gray" },
	{ 0xffffff, "White" },
	{ 0x800000, "Dark Red" },
	{ 0xff0000, "Red" },
	{ 0xff8000, "Orange" },
	{ 0xffff00, "Yellow" },
	{ 0x008000, "Green" },
	{ 0x00ff00, "Lime" },
	{ 0x008080, "Teal" },
	{ 0x00ffff, "Cyan" },
	{ 0x000080, "Navy" },
	{ 0x0000ff, "Blue" },
	{ 0x800080, "Purple" },
	{ 0xff00ff, "Magenta" },
};
