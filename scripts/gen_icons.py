#!/usr/bin/env python3
"""Generate grayscale 48x48 PNG weather icons for e-ink display."""

import math, os, sys

def save_png(pixels, w, h, path):
    """Write a minimal 8-bit grayscale PNG."""
    # Simple uncompressed PNG writer
    import struct, zlib

    def chunk(ctype, data):
        c = ctype + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xFFFFFFFF)

    raw = b''
    for y in range(h):
        raw += b'\x00'  # filter byte
        raw += bytes(pixels[y * w:(y + 1) * w])

    ihdr = struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0)  # 8-bit grayscale
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', ihdr)
    png += chunk(b'IDAT', zlib.compress(raw))
    png += chunk(b'IEND', b'')

    with open(path, 'wb') as f:
        f.write(png)

def draw_sun(pixels, w, cx, cy, r):
    for y in range(h := len(pixels) // w):
        for x in range(w):
            d = math.sqrt((x - cx)**2 + (y - cy)**2)
            if d <= r:
                pixels[y * w + x] = 0  # black

def draw_ray(pixels, w, cx, cy, angle, length, thickness):
    import math
    for t in range(-thickness, thickness + 1):
        ex = int(cx + math.cos(angle) * length + t * math.cos(angle + math.pi/2))
        ey = int(cy + math.sin(angle) * length + t * math.sin(angle + math.pi/2))
        px = max(0, min(w - 1, ex))
        py = max(0, min(len(pixels) // w - 1, ey))
        mid_x = (cx + px) // 2
        mid_y = (cy + py) // 2
        # Simple line from circle edge to end
        for f in range(11):
            lx = int(cx + (ex - cx) * f / 10)
            ly = int(cy + (ey - cy) * f / 10)
            if 0 <= lx < w and 0 <= ly < len(pixels) // w:
                pixels[ly * w + lx] = 0

def draw_cloud(pixels, w, cx, cy, r):
    for y in range(len(pixels) // w):
        for x in range(w):
            d1 = math.sqrt((x - cx + r//2)**2 + (y - cy + r//4)**2)
            d2 = math.sqrt((x - cx - r//3)**2 + (y - cy + r//6)**2)
            d3 = math.sqrt((x - cx)**2 + (y - cy - r//6)**2)
            if (d1 <= r * 0.7 or d2 <= r * 0.55 or d3 <= r * 0.6) or \
               (y >= cy and abs(x - cx) <= r):
                pixels[y * w + x] = 0

def draw_lines(pixels, w, y_start, n_lines, gap, thickness):
    h = len(pixels) // w
    for i in range(n_lines):
        ly = y_start + i * gap
        for t in range(thickness):
            yy = ly + t + i
            if yy >= h:
                break
            for x in range(w // 4, w - w // 4):
                pixels[yy * w + x] = 0

def draw_dots(pixels, w, y_start, n_rows, dots_per_row, size):
    h = len(pixels) // w
    import random
    rng = random.Random(42)
    for row in range(n_rows):
        yy = y_start + row * (size + 2)
        for _ in range(dots_per_row):
            x = rng.randint(w // 4, w - w // 4 - size)
            y = yy + rng.randint(0, size)
            for dy in range(size):
                for dx in range(size):
                    px = x + dx
                    py = y + dy
                    if 0 <= px < w and 0 <= py < h:
                        pixels[py * w + px] = 0

def draw_bolt(pixels, w, cx, top, bot, thickness):
    h = len(pixels) // w
    # Zigzag lightning bolt
    mx = cx - thickness
    mid_y1 = (top + bot) // 3
    mid_y2 = (top + bot) * 2 // 3
    # upper segment
    for y in range(top, mid_y1):
        for t in range(thickness * 2):
            px = cx - thickness + t
            py = y
            if 0 <= px < w and 0 <= py < h:
                pixels[py * w + px] = 0
    # middle segment down-right
    for y in range(mid_y1, mid_y2):
        x = mx + (y - mid_y1) * (thickness * 4) // (mid_y2 - mid_y1)
        for t in range(thickness * 2):
            if 0 <= x + t < w and 0 <= y < h:
                pixels[y * w + x + t] = 0
    # lower segment
    for y in range(mid_y2, bot):
        for t in range(thickness * 2):
            px = cx + thickness * 2 - t
            py = y
            if 0 <= px < w and 0 <= py < h:
                pixels[py * w + px] = 0

def make_icons(outdir):
    os.makedirs(outdir, exist_ok=True)
    w, h = 48, 48
    images = {}

    # Sun
    pixels = [255] * (w * h)
    draw_sun(pixels, w, 24, 24, 12)
    draw_ray(pixels, w, 24, 24, 0, 20, 2)
    draw_ray(pixels, w, 24, 24, math.pi/4, 18, 2)
    draw_ray(pixels, w, 24, 24, math.pi/2, 20, 2)
    draw_ray(pixels, w, 24, 24, 3*math.pi/4, 18, 2)
    save_png(pixels, w, h, os.path.join(outdir, 'sun.png'))

    # Cloud
    pixels = [255] * (w * h)
    draw_cloud(pixels, w, 24, 28, 14)
    save_png(pixels, w, h, os.path.join(outdir, 'cloud.png'))

    # Partly cloudy (sun behind cloud)
    pixels = [255] * (w * h)
    draw_sun(pixels, w, 16, 20, 8)
    draw_cloud(pixels, w, 28, 30, 14)
    save_png(pixels, w, h, os.path.join(outdir, 'partcloud.png'))

    # Fog (horizontal lines)
    pixels = [255] * (w * h)
    draw_lines(pixels, w, 12, 4, 8, 3)
    save_png(pixels, w, h, os.path.join(outdir, 'fog.png'))

    # Drizzle (cloud + small dots)
    pixels = [255] * (w * h)
    draw_cloud(pixels, w, 24, 18, 12)
    draw_dots(pixels, w, 30, 3, 6, 1)
    save_png(pixels, w, h, os.path.join(outdir, 'drizzle.png'))

    # Rain (cloud + lines)
    pixels = [255] * (w * h)
    draw_cloud(pixels, w, 24, 18, 12)
    draw_lines(pixels, w, 30, 3, 6, 2)
    save_png(pixels, w, h, os.path.join(outdir, 'rain.png'))

    # Snow (cloud + stars/dots)
    pixels = [255] * (w * h)
    draw_cloud(pixels, w, 24, 18, 12)
    draw_dots(pixels, w, 30, 4, 5, 2)
    save_png(pixels, w, h, os.path.join(outdir, 'snow.png'))

    # Showers (cloud + heavy lines)
    pixels = [255] * (w * h)
    draw_cloud(pixels, w, 24, 16, 12)
    draw_lines(pixels, w, 28, 4, 5, 2)
    save_png(pixels, w, h, os.path.join(outdir, 'showers.png'))

    # Thunderstorm (cloud + lightning)
    pixels = [255] * (w * h)
    draw_cloud(pixels, w, 24, 18, 12)
    draw_bolt(pixels, w, 24, 28, 44, 3)
    save_png(pixels, w, h, os.path.join(outdir, 'thunder.png'))

    print(f"Generated 9 weather icons in {outdir}")

if __name__ == '__main__':
    make_icons(sys.argv[1] if len(sys.argv) > 1 else 'icons')
