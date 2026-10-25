#!/usr/bin/env python3
"""Generates the app icon: packaging/icons/audioknowledgeexplorer.svg, the PNG sizes for launchers
(packaging/icons/hicolor/<n>x<n>/apps/) and the copies embedded in the program (resources/icons/<n>.png).
One geometry (512 x 512 units) feeds both the SVG and the PNG drawing, so they cannot drift apart.
    python3 tools/make_icon.py
A knowledge card (white) with a sound wave on it, and a magnifier: recordings you can search."""
from PIL import Image, ImageDraw
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = [16, 24, 32, 48, 64, 128, 256, 512]
EMBED = [16, 32, 48, 64, 128, 256]
BLUE_TOP, BLUE_BOT = (0x4F, 0x8F, 0xFF), (0x2A, 0x5F, 0xDB)
CARD, WAVE, LINE, GLASS = "#FFFFFF", "#2F6FEB", "#C5D3F2", "#FFB400"

TILE = (16, 16, 496, 496, 112)                      # x0 y0 x1 y1 radius
CARD_R = (88, 84, 424, 372, 40)                     # the card
BARS = [(132, 188), (172, 156), (212, 124), (252, 168), (292, 144), (332, 196)]   # (x, top): bars centred on y = 186+, see below
BAR_W, WAVE_CY = 26, 186
BAR_HALF = {132: 28, 172: 48, 212: 68, 252: 42, 292: 56, 332: 24}
LINES = [(132, 262, 376, 278), (132, 300, 316, 316)]  # summary lines under the wave
RING = (360, 352, 84, 28)                           # magnifier: cx cy r stroke
HANDLE = (418, 410, 462, 454, 30)                   # x0 y0 x1 y1 stroke


def bar_boxes():
    for x, _ in BARS:
        h = BAR_HALF[x]
        yield x - BAR_W / 2, WAVE_CY - h, x + BAR_W / 2, WAVE_CY + h


def svg():
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 512 512">',
         '  <defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1">',
         '    <stop offset="0" stop-color="#%02X%02X%02X"/><stop offset="1" stop-color="#%02X%02X%02X"/></linearGradient></defs>' % (BLUE_TOP + BLUE_BOT),
         '  <rect x="%d" y="%d" width="%d" height="%d" rx="%d" fill="url(#g)"/>' % (TILE[0], TILE[1], TILE[2] - TILE[0], TILE[3] - TILE[1], TILE[4]),
         '  <rect x="%d" y="%d" width="%d" height="%d" rx="%d" fill="%s"/>' % (CARD_R[0], CARD_R[1], CARD_R[2] - CARD_R[0], CARD_R[3] - CARD_R[1], CARD_R[4], CARD)]
    for x0, y0, x1, y1 in bar_boxes():
        o.append('  <rect x="%g" y="%g" width="%d" height="%g" rx="%d" fill="%s"/>' % (x0, y0, BAR_W, y1 - y0, BAR_W // 2, WAVE))
    for x0, y0, x1, y1 in LINES:
        o.append('  <rect x="%d" y="%d" width="%d" height="%d" rx="8" fill="%s"/>' % (x0, y0, x1 - x0, y1 - y0, LINE))
    o.append('  <circle cx="%d" cy="%d" r="%d" fill="rgba(255,255,255,0.55)" stroke="%s" stroke-width="%d"/>' % (RING[0], RING[1], RING[2] - RING[3] // 2, GLASS, RING[3]))
    o.append('  <line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="%d" stroke-linecap="round"/>' % (HANDLE[0], HANDLE[1], HANDLE[2], HANDLE[3], GLASS, HANDLE[4]))
    o.append('</svg>')
    return "\n".join(o) + "\n"


def render(px):
    k = 4 * px / 512                                  # supersample 4x, then shrink
    S = lambda v: v * k
    big = int(px * 4)
    # vertical gradient clipped to the rounded tile
    grad = Image.new("RGB", (big, big))
    gd = ImageDraw.Draw(grad)
    for y in range(big):
        t = max(0, min(1, (y / k - TILE[1]) / (TILE[3] - TILE[1])))
        gd.line([(0, y), (big, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(BLUE_TOP, BLUE_BOT)))
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).rounded_rectangle([S(TILE[0]), S(TILE[1]), S(TILE[2]), S(TILE[3])], S(TILE[4]), fill=255)
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img, "RGBA")
    d.rounded_rectangle([S(CARD_R[0]), S(CARD_R[1]), S(CARD_R[2]), S(CARD_R[3])], S(CARD_R[4]), fill=CARD)
    for x0, y0, x1, y1 in bar_boxes():
        d.rounded_rectangle([S(x0), S(y0), S(x1), S(y1)], S(BAR_W / 2), fill=WAVE)
    if px >= 48:                                      # the summary lines are noise at launcher-small sizes
        for x0, y0, x1, y1 in LINES:
            d.rounded_rectangle([S(x0), S(y0), S(x1), S(y1)], S(8), fill=LINE)
    cx, cy, r, w = RING
    ro = r
    d.ellipse([S(cx - ro), S(cy - ro), S(cx + ro), S(cy + ro)], fill=(255, 255, 255, 140))
    d.ellipse([S(cx - ro), S(cy - ro), S(cx + ro), S(cy + ro)], outline=GLASS, width=max(1, round(S(w))))
    # outline() draws inward, so the ring's outer edge is r; the handle starts on its rim
    x0, y0, x1, y1, hw = HANDLE
    d.line([S(x0), S(y0), S(x1), S(y1)], fill=GLASS, width=round(S(hw)))
    for ex, ey in ((x0, y0), (x1, y1)):               # round caps
        d.ellipse([S(ex - hw / 2), S(ey - hw / 2), S(ex + hw / 2), S(ey + hw / 2)], fill=GLASS)
    return img.resize((px, px), Image.LANCZOS)


def main():
    open(os.path.join(ROOT, "packaging/icons/audioknowledgeexplorer.svg"), "w").write(svg())
    for n in SIZES:
        p = os.path.join(ROOT, "packaging/icons/hicolor/%dx%d/apps" % (n, n))
        os.makedirs(p, exist_ok=True)
        render(n).save(os.path.join(p, "audioknowledgeexplorer.png"))
    big = render(256)                                 # Windows icon: several sizes in one .ico
    big.save(os.path.join(ROOT, "packaging/icons/audioknowledgeexplorer.ico"), sizes=[(n, n) for n in (16, 24, 32, 48, 64, 128, 256)])
    os.makedirs(os.path.join(ROOT, "resources/icons"), exist_ok=True)
    for n in EMBED:
        render(n).save(os.path.join(ROOT, "resources/icons/%d.png" % n))
    print("icons written:", SIZES)


if __name__ == "__main__":
    main()
