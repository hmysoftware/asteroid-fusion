#!/usr/bin/env python3
"""HMY AFX (Asteroid Effects) - ikon adaptive minimalis, digambar prosedural dengan pycairo.

Konsep: satu asteroid hangat (amber) melaju diagonal, meninggalkan tiga jejak tipis yang memudar;
satu jejak cyan merujuk efek "RGB split". Latar ruang angkasa gelap dengan orbit samar dan sedikit bintang.
Keluaran: res/mipmap-*/ (lapisan adaptive + ikon lama + monochrome) dan icon-512.png (pratinjau).
"""
import math, os, random, sys
import cairo

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "res")

AMBER = (1.00, 0.604, 0.180)      # C_ACC aplikasi  (255,154,46)
AMBER_HI = (1.00, 0.816, 0.541)
AMBER_LO = (0.847, 0.322, 0.133)
CYAN = (0.298, 0.788, 0.941)      # C_ACC2 aplikasi (76,201,240)
D = math.radians(45)              # arah laju: kanan-bawah
DX, DY = math.cos(D), math.sin(D)
NX, NY = -DY, DX                  # tegak lurus arah laju


def lump(ctx, cx, cy, r, seed=5, n=11, amp=0.075):
    """Lingkaran tak beraturan halus (spline Catmull-Rom tertutup)."""
    rnd = random.Random(seed)
    pts = []
    for i in range(n):
        a = 2 * math.pi * i / n + 0.35
        rr = r * (1 + rnd.uniform(-amp, amp))
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    ctx.new_path()
    ctx.move_to(*pts[0])
    for i in range(n):
        p0, p1, p2, p3 = pts[(i - 1) % n], pts[i], pts[(i + 1) % n], pts[(i + 2) % n]
        ctx.curve_to(p1[0] + (p2[0] - p0[0]) / 6, p1[1] + (p2[1] - p0[1]) / 6,
                     p2[0] - (p3[0] - p1[0]) / 6, p2[1] - (p3[1] - p1[1]) / 6, p2[0], p2[1])
    ctx.close_path()


def streak(ctx, ox, oy, length, w, rgb, a0, mono=False):
    """Jejak meruncing: kepala lebar (w) di (ox,oy) menuju ekor di belakang, alpha memudar."""
    hx, hy = ox, oy
    tx, ty = ox - DX * length, oy - DY * length
    h = w / 2
    ctx.new_path()
    ctx.move_to(hx + NX * h, hy + NY * h)
    ctx.line_to(tx + NX * h * 0.18, ty + NY * h * 0.18)
    ctx.arc(tx, ty, h * 0.18, D + math.pi / 2, D + 3 * math.pi / 2)
    ctx.line_to(hx - NX * h, hy - NY * h)
    ctx.arc(hx, hy, h, D - math.pi / 2, D + math.pi / 2)
    ctx.close_path()
    g = cairo.LinearGradient(hx, hy, tx, ty)
    c = (1, 1, 1) if mono else rgb
    g.add_color_stop_rgba(0.0, *c, a0)
    g.add_color_stop_rgba(0.55, *c, a0 * 0.45)
    g.add_color_stop_rgba(1.0, *c, 0.0)
    ctx.set_source(g)
    ctx.fill()


def draw_foreground(ctx, mono=False):
    """Lapisan depan, kanvas 108x108 (zona aman: lingkaran Ø66 di tengah)."""
    ax, ay, r = 60.5, 60.5, 12.5
    if not mono:  # pendar halus di belakang asteroid (efek "glow")
        g = cairo.RadialGradient(ax, ay, r * 0.5, ax, ay, r * 2.3)
        g.add_color_stop_rgba(0, *AMBER, 0.20)
        g.add_color_stop_rgba(1, *AMBER, 0.0)
        ctx.set_source(g)
        ctx.arc(ax, ay, r * 2.3, 0, 2 * math.pi)
        ctx.fill()
    # tiga jejak: cyan (atas), amber utama (tengah), amber pendek (bawah)
    streak(ctx, ax + NX * -8.5 - DX * 5, ay + NY * -8.5 - DY * 5, 24, 2.6, CYAN, 0.95, mono)
    streak(ctx, ax - DX * 5, ay - DY * 5, 42, 3.6, AMBER, 1.0, mono)
    streak(ctx, ax + NX * 8.5 - DX * 5, ay + NY * 8.5 - DY * 5, 29, 2.6, AMBER, 0.75, mono)
    # asteroid
    lump(ctx, ax, ay, r)
    if mono:
        ctx.set_source_rgba(1, 1, 1, 1)
    else:
        g = cairo.RadialGradient(ax - r * 0.35, ay - r * 0.4, r * 0.1, ax, ay, r * 1.15)
        g.add_color_stop_rgb(0.0, *AMBER_HI)
        g.add_color_stop_rgb(0.45, *AMBER)
        g.add_color_stop_rgb(1.0, *AMBER_LO)
        ctx.set_source(g)
    ctx.fill()
    # kawah (dibuat bolong pada monochrome)
    for dx, dy, cr in ((-0.28, 0.22, 0.24), (0.34, -0.2, 0.15), (0.12, 0.5, 0.11)):
        ctx.new_path()
        ctx.arc(ax + dx * r, ay + dy * r, cr * r, 0, 2 * math.pi)
        if mono:
            ctx.save(); ctx.set_operator(cairo.OPERATOR_DEST_OUT); ctx.set_source_rgba(0, 0, 0, 0.55); ctx.fill(); ctx.restore()
        else:
            ctx.set_source_rgba(*AMBER_LO, 0.42); ctx.fill()


def draw_background(ctx):
    """Lapisan belakang: ruang angkasa gelap, orbit samar, bintang kecil."""
    g = cairo.RadialGradient(40, 36, 4, 54, 54, 82)
    g.add_color_stop_rgb(0.0, 0.150, 0.150, 0.290)
    g.add_color_stop_rgb(0.55, 0.075, 0.075, 0.140)
    g.add_color_stop_rgb(1.0, 0.035, 0.035, 0.075)
    ctx.set_source(g)
    ctx.paint()
    ctx.save()
    ctx.translate(54, 54)
    ctx.rotate(math.radians(-32))
    ctx.scale(1, 0.34)
    ctx.arc(0, 0, 40, 0, 2 * math.pi)
    ctx.restore()
    ctx.set_line_width(0.55)
    ctx.set_source_rgba(0.78, 0.82, 1.0, 0.13)
    ctx.stroke()
    for x, y, rr, a in ((24, 30, 0.7, 0.55), (82, 26, 0.55, 0.40), (88, 78, 0.8, 0.50), (30, 84, 0.55, 0.35),
                        (70, 14, 0.45, 0.30), (14, 60, 0.45, 0.30)):
        ctx.set_source_rgba(1, 1, 1, a)
        ctx.arc(x, y, rr, 0, 2 * math.pi)
        ctx.fill()


def surf(px):
    s = cairo.ImageSurface(cairo.FORMAT_ARGB32, px, px)
    c = cairo.Context(s)
    c.set_antialias(cairo.ANTIALIAS_BEST)
    return s, c


def save(s, *path):
    p = os.path.join(RES, *path)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    s.write_to_png(p)


def rounded(ctx, x, y, w, h, r):
    ctx.new_path()
    ctx.arc(x + w - r, y + r, r, -math.pi / 2, 0)
    ctx.arc(x + w - r, y + h - r, r, 0, math.pi / 2)
    ctx.arc(x + r, y + h - r, r, math.pi / 2, math.pi)
    ctx.arc(x + r, y + r, r, math.pi, 3 * math.pi / 2)
    ctx.close_path()


def compose(px, shape):
    """Ikon gabungan dari bagian tengah 72/108 lapisan (area yang terlihat di launcher)."""
    s, c = surf(px)
    if shape == "circle":
        c.arc(px / 2, px / 2, px / 2, 0, 2 * math.pi)
    else:
        rounded(c, 0, 0, px, px, px * 0.225)
    c.clip()
    c.scale(px / 72, px / 72)
    c.translate(-18, -18)
    draw_background(c)
    draw_foreground(c)
    return s


def main():
    dens = {"mdpi": 1, "hdpi": 1.5, "xhdpi": 2, "xxhdpi": 3, "xxxhdpi": 4}
    for name, k in dens.items():
        px = int(round(108 * k))
        s, c = surf(px); c.scale(px / 108, px / 108); draw_background(c); save(s, f"mipmap-{name}", "ic_launcher_background.png")
        s, c = surf(px); c.scale(px / 108, px / 108); draw_foreground(c); save(s, f"mipmap-{name}", "ic_launcher_foreground.png")
        s, c = surf(px); c.scale(px / 108, px / 108); draw_foreground(c, mono=True); save(s, f"mipmap-{name}", "ic_launcher_monochrome.png")
        lp = int(round(48 * k))
        save(compose(lp, "squircle"), f"mipmap-{name}", "ic_launcher.png")
        save(compose(lp, "circle"), f"mipmap-{name}", "ic_launcher_round.png")
    xml = os.path.join(RES, "mipmap-anydpi-v26")
    os.makedirs(xml, exist_ok=True)
    body = ('<?xml version="1.0" encoding="utf-8"?>\n<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
            '    <background android:drawable="@mipmap/ic_launcher_background" />\n'
            '    <foreground android:drawable="@mipmap/ic_launcher_foreground" />\n'
            '    <monochrome android:drawable="@mipmap/ic_launcher_monochrome" />\n</adaptive-icon>\n')
    for n in ("ic_launcher.xml", "ic_launcher_round.xml"):
        open(os.path.join(xml, n), "w").write(body)
    # pratinjau 512 + lembar kontak bentuk topeng (untuk pemeriksaan visual)
    save_prev = os.path.join(HERE, "icon-512.png")
    compose(512, "squircle").write_to_png(save_prev)
    sheet = cairo.ImageSurface(cairo.FORMAT_ARGB32, 1100, 400)
    c = cairo.Context(sheet)
    c.set_source_rgb(0.93, 0.93, 0.95); c.paint()
    for i, shape in enumerate(("squircle", "circle")):
        c.set_source_surface(compose(360, shape), 20 + i * 380, 20); c.paint()
    for i, px in enumerate((96, 64, 48)):
        c.set_source_surface(compose(px, "circle"), 800 + i * 0, 30 + sum((96, 64, 48)[:i]) + i * 20); c.paint()
    sheet.write_to_png(os.path.join(HERE, "icon-preview.png"))
    print("ikon selesai")


if __name__ == "__main__":
    main()
