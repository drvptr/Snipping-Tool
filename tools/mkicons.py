#!/usr/bin/env python3
# Draws the icons in icons/ (they are plain PNG files, any editor will do
# too). Needs python3-pil. Run from the source directory:
#   python3 tools/mkicons.py && make icons
import math
import os
from PIL import Image, ImageDraw, ImageFont

SS = 8  # supersampling


def canvas(size):
    im = Image.new("RGBA", (size * SS, size * SS), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def s(v):
    return v * SS


def pts(seq):
    return [(s(x), s(y)) for x, y in seq]


def finish(im, size, name):
    out = im.resize((size, size), Image.LANCZOS)
    os.makedirs("icons", exist_ok=True)
    out.save(os.path.join("icons", name))


def line(d, a, b, w, col):
    d.line(pts([a, b]), fill=col, width=int(s(w)))
    r = s(w) / 2
    for x, y in (a, b):
        d.ellipse((s(x) - r, s(y) - r, s(x) + r, s(y) + r), fill=col)


def ring(d, cx, cy, r, w, col, inner=None):
    d.ellipse((s(cx - r), s(cy - r), s(cx + r), s(cy + r)), fill=col)
    ri = r - w
    d.ellipse((s(cx - ri), s(cy - ri), s(cx + ri), s(cy + ri)),
              fill=inner if inner else (0, 0, 0, 0))


def rot(p, c, a):
    x, y = p[0] - c[0], p[1] - c[1]
    ca, sa = math.cos(a), math.sin(a)
    return (c[0] + x * ca - y * sa, c[1] + x * sa + y * ca)


def scissors(d, ox=0.0, oy=0.0, k=1.0):
    def P(x, y):
        return (ox + x * k, oy + y * k)
    steel, dark = (214, 220, 228, 255), (60, 70, 84, 255)
    handle, hdark = (232, 120, 34, 255), (150, 66, 10, 255)
    # blades: an open V above the pivot
    for side in (-1, 1):
        tip = P(12 + side * 6.8, 1.2)
        poly = [P(12 - side * 1.2, 13.6), P(12 + side * 6.0, 1.0), tip,
                P(12 + side * 7.2, 2.2), P(12 + side * 0.9, 13.4)]
        d.polygon(pts(poly), fill=steel, outline=dark, width=int(s(0.7 * k)))
    # arms down to the rings
    line(d, P(11.4, 13), P(8.2, 16.2), 1.8 * k, hdark)
    line(d, P(12.6, 13), P(15.8, 16.2), 1.8 * k, hdark)
    for cx in (7.3, 16.7):
        c = P(cx, 18.6)
        ring(d, c[0], c[1], 3.9 * k, 2.0 * k, hdark)
        ring(d, c[0], c[1], 3.3 * k, 1.0 * k, handle)
    c = P(12, 12.9)
    d.ellipse((s(c[0] - 1.1 * k), s(c[1] - 1.1 * k), s(c[0] + 1.1 * k),
               s(c[1] + 1.1 * k)), fill=dark)


def icon_new():
    im, d = canvas(24)
    scissors(d)
    finish(im, 24, "new.png")


def icon_cancel():
    im, d = canvas(24)
    d.ellipse((s(2), s(2), s(22), s(22)), fill=(142, 26, 22, 255))
    d.ellipse((s(2.8), s(2.8), s(21.2), s(21.2)), fill=(210, 52, 44, 255))
    d.ellipse((s(4), s(3.2), s(20), s(13)), fill=(232, 98, 88, 255))
    d.ellipse((s(4), s(5.5), s(20), s(20.6)), fill=(208, 50, 42, 255))
    line(d, (8.2, 8.2), (15.8, 15.8), 2.6, (255, 255, 255, 255))
    line(d, (15.8, 8.2), (8.2, 15.8), 2.6, (255, 255, 255, 255))
    finish(im, 24, "cancel.png")


def gear(d, cx, cy, r, teeth, col, dark, hole):
    for i in range(teeth):
        a = 2 * math.pi * i / teeth
        w, h = 2.6, r + 2.4
        box = [(-w / 2, 0), (w / 2, 0), (w / 2 * 0.8, h), (-w / 2 * 0.8, h)]
        poly = [rot((cx + x, cy + y), (cx, cy), a) for x, y in box]
        d.polygon(pts(poly), fill=dark)
    ring(d, cx, cy, r + 0.6, r + 0.6, dark)
    for i in range(teeth):
        a = 2 * math.pi * i / teeth
        w, h = 1.6, r + 1.7
        box = [(-w / 2, 0), (w / 2, 0), (w / 2 * 0.8, h), (-w / 2 * 0.8, h)]
        poly = [rot((cx + x, cy + y), (cx, cy), a) for x, y in box]
        d.polygon(pts(poly), fill=col)
    ring(d, cx, cy, r, r, col)
    ring(d, cx, cy, hole + 0.8, 0.8, dark, inner=(250, 252, 255, 255))


def icon_options():
    im, d = canvas(24)
    gear(d, 12, 12, 6.2, 9, (150, 166, 186, 255), (64, 78, 98, 255), 2.6)
    finish(im, 24, "options.png")


def icon_save():
    im, d = canvas(24)
    d.rounded_rectangle((s(2.5), s(2.5), s(21.5), s(21.5)), radius=s(2),
                        fill=(52, 98, 172, 255), outline=(26, 56, 108, 255),
                        width=s(1))
    d.rectangle((s(6.5), s(3), s(17), s(9.5)), fill=(222, 226, 232, 255),
                outline=(120, 130, 142, 255), width=int(s(0.6)))
    d.rectangle((s(13.5), s(4.2), s(15.6), s(8.4)), fill=(52, 98, 172, 255))
    d.rectangle((s(5.5), s(12.5), s(18.5), s(21.5)), fill=(250, 250, 250, 255),
                outline=(26, 56, 108, 255), width=int(s(0.6)))
    for y in (15, 17.6):
        line(d, (7.5, y), (16.5, y), 0.9, (150, 160, 175, 255))
    finish(im, 24, "save.png")


def sheet(d, x0, y0, x1, y1, fold):
    o = (96, 110, 126, 255)
    poly = [(x0, y0), (x1 - fold, y0), (x1, y0 + fold), (x1, y1), (x0, y1)]
    d.polygon(pts(poly), fill=(255, 255, 255, 255), outline=o, width=s(1))
    d.polygon(pts([(x1 - fold, y0), (x1 - fold, y0 + fold), (x1, y0 + fold)]),
              fill=(214, 222, 232, 255), outline=o, width=int(s(0.8)))


def icon_copy():
    im, d = canvas(24)
    sheet(d, 2.5, 1.5, 14.5, 16.5, 3.5)
    sheet(d, 8.5, 6.5, 21.5, 22.5, 4)
    for y in (12, 14.5, 17, 19.5):
        line(d, (11, y), (19, y), 0.9, (110, 150, 205, 255))
    finish(im, 24, "copy.png")


def icon_send():
    im, d = canvas(24)
    d.rectangle((s(1.5), s(7), s(19.5), s(20.5)), fill=(250, 246, 232, 255),
                outline=(118, 102, 70, 255), width=s(1))
    d.line(pts([(2, 7.5), (10.5, 14.5), (19, 7.5)]), fill=(118, 102, 70, 255),
           width=s(1), joint="curve")
    d.line(pts([(2, 20), (8, 14.5)]), fill=(176, 160, 128, 255), width=int(s(0.8)))
    d.line(pts([(19, 20), (13, 14.5)]), fill=(176, 160, 128, 255), width=int(s(0.8)))
    g, gd = (60, 170, 70, 255), (24, 104, 34, 255)
    arrow = [(14.5, 6.2), (19, 1.7), (17.2, 0.6), (22.6, 0.6), (22.6, 6), (21.5, 4.2),
             (17, 8.7)]
    d.polygon(pts(arrow), fill=g, outline=gd, width=int(s(0.7)))
    finish(im, 24, "send.png")


def stick(d, a, b, w, col, outline):
    ang = math.atan2(b[1] - a[1], b[0] - a[0])
    nx, ny = -math.sin(ang) * w / 2, math.cos(ang) * w / 2
    poly = [(a[0] + nx, a[1] + ny), (b[0] + nx, b[1] + ny),
            (b[0] - nx, b[1] - ny), (a[0] - nx, a[1] - ny)]
    d.polygon(pts(poly), fill=col, outline=outline, width=int(s(0.7)))


def icon_pen():
    im, d = canvas(24)
    dark = (24, 30, 46, 255)
    # tip cone
    d.polygon(pts([(3.6, 20.4), (5.2, 14.9), (9.1, 18.8)]),
              fill=(196, 202, 212, 255), outline=dark, width=int(s(0.7)))
    d.polygon(pts([(3.4, 20.6), (4.0, 18.6), (5.4, 20.0)]), fill=dark)
    stick(d, (7.1, 16.9), (17.6, 6.4), 5.4, (40, 66, 128, 255), dark)
    stick(d, (17.2, 6.8), (20.4, 3.6), 5.4, (120, 140, 186, 255), dark)
    line(d, (9.2, 13.0), (15.6, 6.6), 0.9, (110, 140, 200, 255))
    finish(im, 24, "pen.png")


def icon_marker():
    im, d = canvas(24)
    dark = (110, 92, 0, 255)
    line(d, (2.6, 21.2), (9.5, 21.2), 2.6, (255, 236, 64, 200))
    d.polygon(pts([(3.2, 19.8), (5.0, 14.6), (9.4, 19.0), (4.6, 20.8)]),
              fill=(250, 220, 30, 255), outline=dark, width=int(s(0.7)))
    stick(d, (7.4, 16.6), (11.0, 13.0), 6.4, (60, 60, 64, 255), (20, 20, 22, 255))
    stick(d, (10.6, 13.4), (19.0, 5.0), 6.4, (252, 226, 40, 255), dark)
    stick(d, (18.6, 5.4), (21.2, 2.8), 5.2, (230, 200, 20, 255), dark)
    finish(im, 24, "marker.png")


def icon_eraser():
    im, d = canvas(24)
    c = (12, 12.5)
    a = math.radians(-40)

    def box(x0, y0, x1, y1):
        return [rot(p, c, a) for p in ((x0, y0), (x1, y0), (x1, y1), (x0, y1))]
    outl = (96, 64, 78, 255)
    d.polygon(pts(box(1.5, 8.5, 12.5, 16.5)), fill=(246, 168, 186, 255),
              outline=outl, width=s(1))
    d.polygon(pts(box(12.5, 8.5, 22.5, 16.5)), fill=(72, 128, 212, 255),
              outline=(30, 60, 120, 255), width=s(1))
    d.polygon(pts(box(2.5, 9.5, 12, 11)), fill=(252, 206, 216, 255))
    d.polygon(pts(box(13, 9.5, 21.5, 11)), fill=(130, 172, 232, 255))
    finish(im, 24, "eraser.png")


def icon_ocr():
    im, d = canvas(24)
    blue = (36, 118, 206, 255)
    for (x, y, dx, dy) in ((2.5, 2.5, 1, 1), (21.5, 2.5, -1, 1),
                           (2.5, 21.5, 1, -1), (21.5, 21.5, -1, -1)):
        line(d, (x, y), (x + dx * 5, y), 1.6, blue)
        line(d, (x, y), (x, y + dy * 5), 1.6, blue)
    t = (40, 44, 52, 255)
    d.rectangle((s(7), s(6.5), s(17), s(9)), fill=t)
    d.rectangle((s(10.8), s(6.5), s(13.2), s(17.5)), fill=t)
    d.rectangle((s(9.4), s(16.2), s(14.6), s(17.6)), fill=t)
    finish(im, 24, "ocr.png")


def icon_help():
    im, d = canvas(16)
    d.ellipse((s(0.5), s(0.5), s(15.5), s(15.5)), fill=(22, 76, 150, 255))
    d.ellipse((s(1.3), s(1.3), s(14.7), s(14.7)), fill=(52, 120, 206, 255))
    d.ellipse((s(2.5), s(1.6), s(13.5), s(8.5)), fill=(110, 164, 230, 255))
    d.ellipse((s(2), s(4.2), s(14), s(14.4)), fill=(46, 112, 198, 255))
    f = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", s(11))
    d.text((s(8), s(8.3)), "?", font=f, fill=(255, 255, 255, 255), anchor="mm")
    finish(im, 16, "help.png")


def icon_app(size):
    im, d = canvas(24)
    if size >= 32:
        d.rounded_rectangle((s(1), s(1), s(23), s(23)), radius=s(3),
                            fill=(244, 248, 253, 255), outline=(150, 168, 190, 255),
                            width=int(s(0.6)))
        blue = (36, 118, 206, 255)
        for x in range(3, 21, 3):
            line(d, (x, 3.2), (x + 1.5, 3.2), 0.9, blue)
            line(d, (x, 20.8), (x + 1.5, 20.8), 0.9, blue)
        for y in range(3, 21, 3):
            line(d, (3.2, y), (3.2, y + 1.5), 0.9, blue)
            line(d, (20.8, y), (20.8, y + 1.5), 0.9, blue)
        scissors(d, 2.4, 2.6, 0.8)
    else:
        scissors(d)
    out = im.resize((size, size), Image.LANCZOS)
    out.save(os.path.join("icons", "app%d.png" % size))


if __name__ == "__main__":
    icon_new()
    icon_cancel()
    icon_options()
    icon_save()
    icon_copy()
    icon_send()
    icon_pen()
    icon_marker()
    icon_eraser()
    icon_ocr()
    icon_help()
    for n in (16, 32, 48):
        icon_app(n)
