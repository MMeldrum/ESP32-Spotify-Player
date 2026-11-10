"""Generates original abstract 'album covers' for the simulator demo (sim/art/1.jpg ... 5.jpg).

Run with: python3 sim/make_demo_art.py   (needs Pillow). Replace the JPEGs with your own to change the art;
they must be baseline (not progressive) JPEGs, ideally 640x640 like Spotify's large images.
"""
import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter

SIZE = 640
OUT = os.path.join(os.path.dirname(__file__), "art")

# (background top, background bottom, accent colours, style)
COVERS = [
    ((20, 24, 70), (230, 70, 120), [(255, 200, 90), (90, 220, 255)], "sun"),
    ((10, 60, 70), (10, 20, 30), [(120, 255, 210), (255, 255, 255)], "orbits"),
    ((255, 170, 90), (250, 90, 110), [(255, 240, 200), (120, 40, 90)], "waves"),
    ((40, 90, 40), (200, 220, 120), [(250, 250, 220), (30, 60, 30)], "fields"),
    ((60, 20, 10), (230, 120, 40), [(255, 220, 160), (90, 30, 10)], "shards"),
]


def gradient(top, bottom):
    img = Image.new("RGB", (SIZE, SIZE))
    px = img.load()
    for y in range(SIZE):
        t = y / (SIZE - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(SIZE):
            px[x, y] = c
    return img


def draw(index, top, bottom, accents, style):
    rnd = random.Random(index)
    img = gradient(top, bottom)
    d = ImageDraw.Draw(img, "RGBA")
    a, b = accents
    if style == "sun":
        d.ellipse((170, 140, 470, 440), fill=a + (255,))
        for i in range(8):
            y = 300 + i * 24
            d.rectangle((0, y, SIZE, y + 10 - i), fill=top + (255,))
        d.line((0, 470, SIZE, 470), fill=b + (200,), width=4)
    elif style == "orbits":
        for r in range(60, 320, 36):
            d.ellipse((320 - r, 320 - r, 320 + r, 320 + r), outline=a + (120,), width=3)
        for _ in range(5):
            ang = rnd.random() * math.tau
            r = rnd.choice(range(60, 320, 36))
            x, y = 320 + r * math.cos(ang), 320 + r * math.sin(ang)
            d.ellipse((x - 12, y - 12, x + 12, y + 12), fill=b + (255,))
    elif style == "waves":
        for k in range(9):
            pts = [(x, 260 + k * 38 + 30 * math.sin(x / 70 + k)) for x in range(0, SIZE + 10, 10)]
            d.line(pts, fill=(a if k % 2 else b) + (200,), width=8)
    elif style == "fields":
        for k in range(14):
            y = 300 + k * k * 2.2
            d.polygon([(0, y), (SIZE, y - 40), (SIZE, y - 30), (0, y + 10)], fill=(b if k % 2 else a) + (110,))
        d.ellipse((440, 80, 540, 180), fill=a + (230,))
    else:
        for _ in range(16):
            x, y = rnd.randrange(SIZE), rnd.randrange(SIZE)
            pts = [(x + rnd.randrange(-160, 160), y + rnd.randrange(-160, 160)) for _ in range(3)]
            d.polygon(pts, fill=rnd.choice(accents) + (rnd.randrange(80, 200),))
    img = img.filter(ImageFilter.GaussianBlur(0.6))
    img.save(os.path.join(OUT, f"{index}.jpg"), "JPEG", quality=88, progressive=False)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for i, cover in enumerate(COVERS, start=1):
        draw(i, *cover)
    print("Wrote", len(COVERS), "covers to", OUT)
