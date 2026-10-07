import svgwrite

SIZE = 25
OUT_DIR = "design_generated"  # change as you like

def path_d(*subpaths):
    """Build a path 'd' string from subpaths: each is (points, closed)."""
    parts = []
    for points, closed in subpaths:
        (x0, y0), rest = points[0], points[1:]
        parts.append(f"M{x0} {y0}")
        parts += [f"L{x} {y}" for x, y in rest]
        if closed:
            parts.append("Z")
    return " ".join(parts)

def new_icon(name):
    dwg = svgwrite.Drawing(f"{OUT_DIR}/25px_{name}.svg", size=(SIZE, SIZE))
    dwg.viewbox(0, 0, SIZE, SIZE)
    return dwg

def stroke_style(width, round_ends=False):
    style = dict(fill="none", stroke="black", stroke_width=width)
    if round_ends:
        style.update(stroke_linecap="round", stroke_linejoin="round")
    return style

# --- Caret down / up: 3px stroke on half-pixel coordinates ---------------
def caret(name, tip_y, end_y):
    dwg = new_icon(name)
    d = path_d(([(21.5, end_y), (13, tip_y), (4.5, end_y)], False))
    dwg.add(dwg.path(d=d, **stroke_style(3)))
    return dwg

# --- Checkmark -------------------------------------------------------------
def checkmark():
    dwg = new_icon("Checkmark")
    d = path_d(([(19.5, 4.5), (11.5, 20.5), (4.5, 13.5)], False))
    dwg.add(dwg.path(d=d, **stroke_style(3, round_ends=True)))
    return dwg

# --- Music note: white fill silhouette + 2px outline on integer coords -----
def music_note():
    dwg = new_icon("Music_note")
    silhouette = [(8, 1), (23, 6), (23, 23), (17, 23), (17, 16), (23, 16),
                  (23, 11), (8, 6), (8, 10), (2, 10), (2, 17), (8, 17), (8, 6)]
    dwg.add(dwg.path(d=path_d((silhouette, True)), fill="white"))

    outline = path_d(
        ([(8, 6), (8, 1), (23, 6), (23, 11), (8, 6)], False),  # beam top + diagonal
        ([(23, 11), (23, 16)], False),                          # right stem
        ([(8, 6), (8, 10)], False),                             # left stem
        ([(23, 16), (23, 23), (17, 23), (17, 16)], True),       # right note head
        ([(8, 10), (8, 17), (2, 17), (2, 10)], True),           # left note head
    )
    dwg.add(dwg.path(d=outline, fill="none", stroke="black",
                     stroke_width=2, stroke_linejoin="round"))
    return dwg

# --- Phone: handset + three signal ticks -----------------------------------
def phone():
    dwg = new_icon("Phone")
    handset = [(10, 11), (4, 5), (1, 14), (12, 24), (21, 21),
               (15, 15), (13, 19), (6, 13)]
    dwg.add(dwg.path(d=path_d((handset, True)), fill="white", stroke="black",
                     stroke_width=2, stroke_linecap="round", stroke_linejoin="round"))
    ticks = path_d(
        ([(12, 7), (15.5, 5.5)], False),
        ([(8, 4), (8.5, 1.5)], False),
        ([(11, 4), (13, 1.5)], False),
    )
    dwg.add(dwg.path(d=ticks, **stroke_style(2, round_ends=True)))
    return dwg

if __name__ == "__main__":
    import os
    os.makedirs(OUT_DIR, exist_ok=True)
    for icon in (caret("Caret_down", tip_y=16.5, end_y=8.5),
                 caret("Caret_up",   tip_y=8.5,  end_y=16.5),
                 checkmark(), music_note(), phone()):
        icon.save()
        print("wrote", icon.filename)