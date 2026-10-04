#!/usr/bin/env python3
# Builds ocrdb.h, the character samples of the OCR (see ocr.c).
# Needs python3-pil and the fonts listed below.
#   python3 tools/mkocrdb.py > ocrdb.h
#
# Every sample is a character rendered with anti-aliasing at a pixel size,
# described like the OCR sees glyphs on the screen: its box (pixels with
# at least half coverage), the mean coverage of the cells of a GRID x GRID
# grid laid over the box, and where the box is relative to the baseline,
# measured in x-heights of the font.
import math
import sys
from PIL import ImageFont

GRID = 7
FONTS = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSerif-Regular.ttf",
    "/usr/share/fonts/truetype/open-sans/OpenSans-Regular.ttf",
    "/usr/share/fonts/truetype/open-sans/OpenSans-Bold.ttf",
    "/usr/share/fonts/truetype/paratype/PTS55F.ttf",
    "/usr/share/fonts/truetype/paratype/PTF55F.ttf",
    "/usr/share/fonts/truetype/roboto/unhinted/RobotoTTF/Roboto-Regular.ttf",
    "/usr/share/fonts/opentype/cantarell/Cantarell-Regular.otf",
    "/usr/share/fonts/truetype/crosextra/Carlito-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSerif.ttf",
]
SIZES = [10, 11, 12, 13, 14, 16, 18]


def crange(a, b):
    """the characters from code point a to b"""
    return "".join(chr(c) for c in range(a, b + 1))


# Cyrillic in the order of the alphabet: yo comes after ie
CYRILLIC = (crange(0x410, 0x415) + chr(0x401) + crange(0x416, 0x42f) +
            crange(0x430, 0x435) + chr(0x451) + crange(0x436, 0x44f))
# and the em dash, the guillemets and the numero sign
CHARS = ("0123456789"
         "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz" +
         CYRILLIC +
         ".,:;!?'()[]{}<>/\\|-_+=*&%$#@~^`" +
         chr(0x2014) + chr(0xab) + chr(0xbb) + chr(0x2116))
# fi and fl: ligatures of many fonts, or an f whose arm touches the i or
# the l; ocr.c writes them as two letters
LIGATURES = {chr(0xfb01): "fi", chr(0xfb02): "fl"}


def glyph(font, ch):
    """coverage rows and box of ch relative to the baseline origin"""
    mask, (ox, oy) = font.getmask2(ch, mode="L", anchor="ls")
    w, h = mask.size
    cov = [[mask.getpixel((x, y)) for x in range(w)] for y in range(h)]
    pts = [(x, y) for y in range(h) for x in range(w) if cov[y][x] >= 128]
    if not pts:
        return None
    x0 = min(p[0] for p in pts)
    x1 = max(p[0] for p in pts) + 1
    y0 = min(p[1] for p in pts)
    y1 = max(p[1] for p in pts) + 1
    return cov, x0, y0, x1, y1, ox, oy


def zones(cov, x0, y0, w, h):
    """mean coverage (0..15) of the cells of a GRID x GRID grid"""
    f = []
    for j in range(GRID):
        fy0, fy1 = j * h / GRID, (j + 1) * h / GRID
        for i in range(GRID):
            fx0, fx1 = i * w / GRID, (i + 1) * w / GRID
            s = a = 0.0
            for y in range(int(fy0), int(math.ceil(fy1))):
                wy = min(y + 1, fy1) - max(y, fy0)
                if wy <= 0:
                    continue
                for x in range(int(fx0), int(math.ceil(fx1))):
                    wx = min(x + 1, fx1) - max(x, fx0)
                    if wx <= 0:
                        continue
                    s += wx * wy * cov[y0 + y][x0 + x]
                    a += wx * wy
            f.append(int(15 * s / a / 255 + 0.5) if a else 0)
    return f


def rel(v, xh):
    """pixels to x-heights * 32, rounded, as a signed byte"""
    r = 32.0 * v / xh
    return max(-127, min(127, int(r + (0.5 if r >= 0 else -0.5))))


def sample(font, ch, xh):
    g = glyph(font, ch)
    if g is None:
        return None
    cov, x0, y0, x1, y1, ox, oy = g
    w, h = x1 - x0, y1 - y0
    top = -(oy + y0)          # box top above the baseline, pixels
    bot = oy + y1             # box bottom below the baseline
    lsb = ox + x0             # space left of the box
    rsb = font.getlength(ch) - (ox + x1)
    return {
        "f": zones(cov, x0, y0, w, h),
        "aspect": min(255, int(64.0 * w / h + 0.5)),
        "top": rel(top, xh),
        "bot": rel(bot, xh),
        "lsb": rel(lsb, xh),
        "rsb": rel(rsb, xh),
    }


def xheight(font):
    g = glyph(font, "x")
    cov, x0, y0, x1, y1, ox, oy = g
    return y1 - y0


def build(fonts=FONTS, sizes=SIZES, chars=CHARS):
    out = []
    for fi, path in enumerate(fonts):
        for size in sizes:
            font = ImageFont.truetype(path, size)
            xh = xheight(font)
            for ch in list(chars) + list(LIGATURES):
                s = sample(font, LIGATURES.get(ch, ch), xh)
                if s is None:
                    continue
                s["ch"] = ch
                s["font"] = fi
                s["size"] = size
                out.append(s)
    return out


# small sizes lose the detail that tells these from a simpler character
LOSSY = {chr(0x451): chr(0x435), chr(0x401): chr(0x415),    # yo, ie
         chr(0x449): chr(0x448), chr(0x429): chr(0x428),    # shcha, sha
         ";": ":", ",": "."}


def dist(a, b):
    """the distance ocr.c uses"""
    d = 5 * abs(a["top"] - b["top"]) + 5 * abs(a["bot"] - b["bot"])
    d += 100 * abs(math.log(a["aspect"] + 1) - math.log(b["aspect"] + 1))
    return d + sum(abs(x - y) for x, y in zip(a["f"], b["f"]))


def degenerate(samples):
    """drop the samples that lost their detail: a comma without its tail
    is a period, a plus without its stem a hyphen, yo without dots ie"""
    same = {(s["font"], s["size"], s["ch"]): s for s in samples}
    kept = []
    for s in samples:
        t = same.get((s["font"], s["size"], LOSSY.get(s["ch"], "")))
        if t is not None and dist(s, t) < 30:
            continue
        if s["ch"] in ",;" and s["bot"] <= 3:
            continue
        if s["ch"] == "+" and s["aspect"] > 128:
            continue
        kept.append(s)
    return kept


def dedupe(samples):
    """drop samples that are (almost) the same as a kept one"""
    kept = []
    seen = {}
    for s in samples:
        key = (s["ch"], s["aspect"] // 4, s["top"] // 3, s["bot"] // 3, tuple(v // 2 for v in s["f"]))
        if key in seen:
            continue
        seen[key] = 1
        kept.append(s)
    return kept


def main():
    samples = dedupe(degenerate(build()))
    o = sys.stdout
    o.write("/* generated by tools/mkocrdb.py: %d samples, %dx%d zones */\n" % (len(samples), GRID, GRID))
    o.write("/* Rendered from DejaVu (Bitstream Vera license), Liberation, Noto,\n"
            " * Open Sans, PT Sans/Serif, Roboto, Cantarell, Carlito (SIL Open Font\n"
            " * License 1.1 / Apache 2.0) and FreeFont (GPL with font exception),\n"
            " * see LICENSE. */\n\n")
    o.write("enum { ocrgrid = %d };\n\n" % GRID)
    o.write("/* code point, width/height * 64, top and bottom relative to the\n"
            " * baseline and the side bearings in x-heights * 32 (up is positive\n"
            " * for top, down for bottom), zones: two 4 bit cells per byte */\n")
    o.write("static const struct ocrsample {\n\tunsigned short cp;\n\tunsigned char aspect;\n"
            "\tsigned char top, bot, lsb, rsb;\n\tunsigned char zone[%d];\n} ocrdb[] = {\n" % ((GRID * GRID + 1) // 2))
    for s in samples:
        z = s["f"]
        z = z + [0] * (len(z) % 2)
        packed = [z[i] << 4 | z[i + 1] for i in range(0, len(z), 2)]
        o.write("\t{ 0x%04x, %d, %d, %d, %d, %d, { %s } },\n" % (
            ord(s["ch"]), s["aspect"], s["top"], s["bot"], s["lsb"], s["rsb"],
            ",".join("%d" % b for b in packed)))
    o.write("};\n")


if __name__ == "__main__":
    main()
