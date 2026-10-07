#!/usr/bin/env python3
import svgwrite

# We want a bell that's something like this.  The parameters are firstly for the viewbox
# size, and then for the y positions in order from top to bottom, startin and ending with
# the extent of the arcs.  We assume circular arcs and a 45 degree angle on the lower bell
# rim.
#
#
# <svg viewBox="0 0 16 16">
#   <path
#     d="
#       M 4,8
#       L 4,5
#       A 4,4 0 0 1 12,5
#       L 12,8
#       Z
#       M 4,9
#       L 2,11
#       L 2,12
#       L 14,12
#       L 14,11
#       L 12,9
#       Z
#       M 6,13
#       A 2,2 0,0,0 10,13
#       Z
#     "
#   />
# </svg>


OUT_DIR = "design_generated"  # change as you like

def new_icon(name,size):
    dwg = svgwrite.Drawing(f"{OUT_DIR}/{size}px_{name}.svg", size=(size, size))
    dwg.viewbox(0, 0, size,size)
    return dwg

def stroke_style():
    style = dict(fill="black", stroke_width=0)
    return style


def bell(size, y1,y2,y3,y4,y5,y6,y7,y8):
    dwg = new_icon("bell",size)

    x4 = size/2          # centre line
    r1 = y2-y1           # radius of the top arc
    r2 = y8 - y7         # radius of the bottom arc

    x2 = x4 - r1         # outer left extent of top arc
    x6 = x4 + r1         # outer right extent of top arc

    x3 = x4 - r2         # outer left extent of bottom arc
    x5 = x4 + r2         # outer right extent of bottom arc

    x1 = x2 - (y5 - y4)  # outer left rim of bell
    x7 = x6 + (y5 - y4)  # outer right rim of bell


    d=f"""
      M {x2},{y3}
      L {x2},{y2}
      A {r1},{r1} 0 0 1 {x6},{y2}
      L {x6},{y3}
      Z
      M {x2},{y4}
      L {x1},{y5}
      L {x1},{y6}
      L {x7},{y6}
      L {x7},{y5}
      L {x6},{y4}
      Z
      M {x3},{y7}
      A {r2},{r2} 0 0 0 {x5},{y7}
      Z
    """

    d = " ".join(d.split())

    print(repr(d))

    dwg.add(dwg.path(d=d, **stroke_style()))
    return dwg

if __name__ == "__main__":
    import os
    os.makedirs(OUT_DIR, exist_ok=True)
    icon100 = bell(100,10,30,45,50,65,70,75,85)
    icon100.save()
    print("wrote", icon100.filename)

    icon30 = bell(30,3,10,15,17,22,24,25,28)
    icon30.save()
    print("wrote", icon30.filename)

    icon40 = bell(40,4,12,19,21,28,31,32,36)
    icon40.save()
    print("wrote", icon40.filename)
