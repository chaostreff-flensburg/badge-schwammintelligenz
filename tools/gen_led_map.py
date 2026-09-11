#!/usr/bin/env python3
"""Erzeugt firmware/led_map.h und web/led_map.js aus pcb/pcb.kicad_pcb.

Pro LED: Index der Anoden-Spalte (Col), Index der Kathoden-Zeile (Row) und
die Position auf der Platine, normiert auf 0..255 über die Platinenkontur.
Aufruf aus dem Repo-Root: python3 tools/gen_led_map.py
"""
import re, sys, math, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
PCB = ROOT / "pcb" / "pcb.kicad_pcb"


def parse(s):
    tokens = re.findall(r'"(?:[^"\\]|\\.)*"|[()]|[^\s()"]+', s)
    stack = [[]]
    for t in tokens:
        if t == "(":
            stack.append([])
        elif t == ")":
            node = stack.pop()
            stack[-1].append(node)
        else:
            stack[-1].append(t[1:-1] if t.startswith('"') else t)
    return stack[0][0]


def is_node(n, key):
    return isinstance(n, list) and n and n[0] == key


def child(node, key):
    return next(n for n in node if is_node(n, key))


def bezier(c, t):
    (x0, y0), (x1, y1), (x2, y2), (x3, y3) = c
    u = 1 - t
    return (u**3 * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t**3 * x3,
            u**3 * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t**3 * y3)


def chain(segs):
    """Verkettet Liniensegmente über gemeinsame Endpunkte zu Polylinien."""
    key = lambda pt: (round(pt[0], 2), round(pt[1], 2))
    todo = list(segs)
    polys = []
    while todo:
        a, b = todo.pop()
        poly = [a, b]
        grown = True
        while grown:
            grown = False
            for i, (p, q) in enumerate(todo):
                if key(p) == key(poly[-1]):
                    poly.append(q)
                elif key(q) == key(poly[-1]):
                    poly.append(p)
                elif key(q) == key(poly[0]):
                    poly.insert(0, p)
                elif key(p) == key(poly[0]):
                    poly.insert(0, q)
                else:
                    continue
                todo.pop(i)
                grown = True
                break
        polys.append(poly)
    return polys


def font_from_header():
    src = (ROOT / "firmware" / "font5x7.h").read_text()
    rows = re.findall(r"\{(0x[0-9A-Fa-f]{2}(?:, 0x[0-9A-Fa-f]{2}){4})\}", src)
    assert len(rows) == 59, len(rows)
    return "[" + ",".join("[" + r.replace(" ", "") + "]" for r in rows) + "]"


def main():
    pcb = parse(PCB.read_text())
    leds = []
    edge = []
    segs, curves, holes = [], [], []
    for e in pcb:
        if is_node(e, "footprint"):
            ref = next(p[2] for p in e if is_node(p, "property") and p[1] == "Reference")
            if not re.fullmatch(r"D\d+", ref):
                continue
            at = child(e, "at")
            pads = {p[1]: child(p, "net")[-1] for p in e if is_node(p, "pad") and any(is_node(q, "net") for q in p)}
            row = int(pads["1"].removeprefix("/Row"))  # Pad 1 = Kathode
            col = int(pads["2"].removeprefix("/Col"))  # Pad 2 = Anode
            leds.append((ref, int(ref[1:]), col, row, float(at[1]), float(at[2])))
        elif e[0] in ("gr_line", "gr_arc", "gr_curve", "gr_poly", "gr_circle") and any(
            is_node(p, "layer") and p[1] == "Edge.Cuts" for p in e
        ):
            for p in e:
                if is_node(p, "start") or is_node(p, "end") or is_node(p, "mid"):
                    edge.append((float(p[1]), float(p[2])))
                if is_node(p, "pts"):
                    edge += [(float(q[1]), float(q[2])) for q in p if is_node(q, "xy")]
            if e[0] == "gr_line":
                a, b = child(e, "start"), child(e, "end")
                segs.append(((float(a[1]), float(a[2])), (float(b[1]), float(b[2]))))
            elif e[0] == "gr_curve":
                curves.append([(float(q[1]), float(q[2])) for q in child(e, "pts") if is_node(q, "xy")])
            elif e[0] == "gr_circle":
                c, d = child(e, "center"), child(e, "end")
                holes.append((float(c[1]), float(c[2]), math.hypot(float(d[1]) - float(c[1]), float(d[2]) - float(c[2]))))

    leds.sort(key=lambda l: l[1])
    assert len(leds) == 110, len(leds)
    assert len({(l[2], l[3]) for l in leds}) == 110, "Col/Row-Paare nicht eindeutig"
    assert all(l[2] != l[3] for l in leds), "Col == Row"

    x0, x1 = min(x for x, _ in edge), max(x for x, _ in edge)
    y0, y1 = min(y for _, y in edge), max(y for _, y in edge)
    w, h = x1 - x0, y1 - y0
    norm = lambda v, lo, span: round((v - lo) / span * 255)

    rows_h = "".join(
        f"  {{{col:2d}, {row:2d}, {norm(x, x0, w):3d}, {norm(y, y0, h):3d}}}, // {ref}\n"
        for ref, _, col, row, x, y in leds
    )
    (ROOT / "firmware" / "led_map.h").write_text(
        "// Generiert von tools/gen_led_map.py aus pcb/pcb.kicad_pcb. Nicht von Hand ändern.\n"
        "#pragma once\n#include <stdint.h>\n\n"
        "constexpr int LED_COUNT = 110;\n"
        "constexpr int PIN_COUNT = 11;\n"
        f"constexpr float BOARD_ASPECT = {w / h:.4f}f; // Breite / Höhe\n\n"
        "struct Led {\n"
        "  uint8_t col; // Pin-Index der Anode (HIGH zum Leuchten)\n"
        "  uint8_t row; // Pin-Index der Kathode (LOW zum Leuchten)\n"
        "  uint8_t x;   // Position 0..255 über die Platinenbreite\n"
        "  uint8_t y;   // Position 0..255 über die Platinenhöhe (0 = oben)\n"
        "};\n\n"
        f"constexpr Led LEDS[LED_COUNT] = {{\n{rows_h}}};\n"
    )

    for c in curves:  # kubische Bezier-Kurven als 8 Liniensegmente
        for i in range(8):
            t0, t1 = i / 8, (i + 1) / 8
            segs.append((bezier(c, t0), bezier(c, t1)))
    outline = chain(segs)
    nx = lambda x: round((x - x0) / w * 255, 1)
    ny = lambda y: round((y - y0) / h * 255, 1)
    outline_js = ",\n".join("  [" + ",".join(f"{nx(x)},{ny(y)}" for x, y in poly) + "]" for poly in outline)
    holes_js = ",".join(f"[{nx(x)},{ny(y)},{round(r / w * 255, 1)}]" for x, y, r in holes)
    font_js = font_from_header()

    rows_js = ",\n".join(
        f"  [{norm(x, x0, w)}, {norm(y, y0, h)}]" for _, _, _, _, x, y in leds
    )
    (ROOT / "web" / "led_map.js").write_text(
        "// Generiert von tools/gen_led_map.py aus pcb/pcb.kicad_pcb. Nicht von Hand ändern.\n"
        f"const BOARD_ASPECT = {w / h:.4f};\n"
        f"const LED_XY = [\n{rows_js}\n];\n"
        f"// Platinenkontur als Polylinien [x0,y0,x1,y1,...], gleiche Normierung wie LED_XY\n"
        f"const BOARD_OUTLINE = [\n{outline_js}\n];\n"
        f"const BOARD_HOLES = [{holes_js}];\n"
        f"// 5x7-Zeichensatz aus firmware/font5x7.h, ASCII 32..90, ein Byte pro Spalte\n"
        f"const FONT5X7 = {font_js};\n"
    )
    print(f"{len(leds)} LEDs, Platine {w:.1f} x {h:.1f} mm, Kontur {len(outline)} Polylinien, {len(holes)} Bohrungen")


if __name__ == "__main__":
    main()
