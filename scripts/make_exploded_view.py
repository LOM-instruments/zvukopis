#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11,<3.15"
# dependencies = ["cadquery-ocp-novtk==8.0.1.1.0", "numpy>=2", "pillow>=10"]
# ///
"""Draw the exploded view of the case: the board in front, the case behind it,
the four bolts behind the case, a screenshot of the device on the screen.

    pipx run scripts/make_exploded_view.py

reads the newest docs/case/zvukopis-case-vN.step and writes
docs/case/zvukopis-case-vN.png beside it, the line drawing the project page
shows in its Build section. pipx fetches what the script needs (OpenCascade,
the CAD library that reads STEP files and removes the hidden lines) the first
time, about 65 MB; the drawing itself takes about twenty seconds.

It needs two more files:

- the board's 3D model, a STEP file. It is Waveshare's
  (ESP32-S3-EPAPER-3_97-20260821.stp) and is not in this repository. The
  script looks for it in ../reference/ (see BOARD below); give another place
  with --board.
- a screenshot of the device, 480 x 800 (serial command "s"), for the screen.
  scripts/exploded-view-screen.png unless --screen names another.

The board's model lies along X where the case stands along Y, so it is turned
a quarter turn about Z; apart from that the two share their coordinates (the
case was drawn around the board's model). The script checks that before it
draws: the case must have a hole under each of the board's four standoffs.
"""
import argparse
import math
import re
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops, ImageDraw

from OCP.BRep import BRep_Builder
from OCP.BRepAdaptor import BRepAdaptor_Curve
from OCP.BRepBndLib import BRepBndLib
from OCP.BRepBuilderAPI import (BRepBuilderAPI_MakeEdge, BRepBuilderAPI_MakeFace, BRepBuilderAPI_MakeWire,
                                BRepBuilderAPI_Transform)
from OCP.BRepClass3d import BRepClass3d_SolidClassifier
from OCP.BRepLib import BRepLib
from OCP.BRepMesh import BRepMesh_IncrementalMesh
from OCP.BRepPrimAPI import BRepPrimAPI_MakeRevol
from OCP.Bnd import Bnd_Box
from OCP.GCPnts import GCPnts_TangentialDeflection
from OCP.HLRAlgo import HLRAlgo_Projector
from OCP.HLRBRep import HLRBRep_PolyAlgo, HLRBRep_PolyHLRToShape
from OCP.IFSelect import IFSelect_RetDone
from OCP.STEPControl import STEPControl_Reader
from OCP.TopAbs import TopAbs_EDGE, TopAbs_IN
from OCP.TopExp import TopExp_Explorer
from OCP.TopoDS import TopoDS, TopoDS_Compound
from OCP.gp import gp_Ax1, gp_Ax2, gp_Dir, gp_Pnt, gp_Trsf, gp_Vec

ROOT = Path(__file__).resolve().parent.parent
CASES = ROOT / "docs" / "case"
BOARD = ROOT.parent / "reference" / "ESP32-S3-EPAPER-3" / "ESP32-S3-EPAPER-3_97-20260821.stp"
SCREEN = Path(__file__).resolve().parent / "exploded-view-screen.png"

# The board, in the case's coordinates (millimetres; X across, Y up the device,
# Z towards the eye that reads the screen). Read off Waveshare's model once.
BOARD_BOX = ((-62.41, -0.01, -5.6), (0.69, 99.51, 1.35))      # with the wheel and the buttons that stick out
STANDOFFS = ((-2.7, 2.7), (-57.3, 2.7), (-2.7, 96.8), (-57.3, 96.8))
STANDOFF_END = -5.6                                            # where the bolts' threads begin
PANEL = (-56.029, 9.718, -4.189, 96.118)                       # the screen's 480 x 800 dots: X left, Y bottom, X right, Y top
PANEL_Z = 1.35

# The bolts: countersunk M2.5 x 6. Shank and head diameters, head height, length.
BOLT_SHANK, BOLT_HEAD, BOLT_HEAD_HEIGHT, BOLT_LENGTH = 2.5, 4.7, 1.3, 6.0

# How far the parts are pulled apart along Z, and how the drawing sits on its square.
BOARD_LIFT = 20.0        # the board, towards the eye
BOLT_DROP = 20.0         # the bolts, away behind the case
SIZE = 1828              # the picture's side, in pixels
SCALE = 11.0             # pixels for a millimetre on the page (lines along X, Y and Z come out 0.82 of that)
MARGIN = 0.05            # white kept at each side, as a part of the picture; a larger drawing is scaled down to keep it
LINE = 1.0               # line width, in pixels of the picture
OVERSAMPLE = 4

# The view: from the front, the right and above, all three alike (isometric).
EYE = (1.0, 1.0, 1.0)
RIGHT = (1.0, 0.0, -1.0)


def read_step(path):
    reader = STEPControl_Reader()
    if reader.ReadFile(str(path)) != IFSelect_RetDone:
        sys.exit(f"Cannot read {path} as a STEP file.")
    reader.TransferRoots()
    return reader.OneShape()


def box(shape):
    b = Bnd_Box()
    BRepBndLib.AddOptimal_s(shape, b, False, False)
    lo, hi = b.CornerMin(), b.CornerMax()
    return np.array([lo.X(), lo.Y(), lo.Z()]), np.array([hi.X(), hi.Y(), hi.Z()])


def moved(shape, dz=0.0, quarter_turn=False):
    t = gp_Trsf()
    if quarter_turn:
        t.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), math.pi / 2)
    shift = gp_Trsf()
    shift.SetTranslation(gp_Vec(0, 0, dz))
    shift.Multiply(t)
    return BRepBuilderAPI_Transform(shape, shift, True).Shape()


def compound(shapes):
    builder, c = BRep_Builder(), TopoDS_Compound()
    builder.MakeCompound(c)
    for s in shapes:
        builder.Add(c, s)
    return c


def newest_case():
    numbered = []
    for p in CASES.glob("zvukopis-case-v*.step"):
        m = re.fullmatch(r"zvukopis-case-v(\d+)\.step", p.name)
        if m:
            numbered.append((int(m.group(1)), p))
    if not numbered:
        sys.exit(f"No zvukopis-case-vN.step in {CASES}.")
    return max(numbered)[1]


def check_fit(case, board):
    """The board as it sits on the case: stop if this case would not go on this board."""
    lo, hi = box(board)
    if np.abs(lo - BOARD_BOX[0]).max() > 0.05 or np.abs(hi - BOARD_BOX[1]).max() > 0.05:
        sys.exit(f"The board's model is not the one this script knows: it spans {lo.round(2)} to {hi.round(2)}, "
                 f"expected {BOARD_BOX[0]} to {BOARD_BOX[1]}. Check BOARD_BOX, STANDOFFS and PANEL against it.")
    case_lo, case_hi = box(case)
    if case_hi[2] > BOARD_BOX[1][2] or case_lo[2] > STANDOFF_END:
        sys.exit(f"The case spans Z {case_lo[2]:.2f} to {case_hi[2]:.2f}: it is not drawn in the board's coordinates.")
    inside = BRepClass3d_SolidClassifier(case)
    for x, y in STANDOFFS:
        for z in np.linspace(case_lo[2] - 0.5, STANDOFF_END, 12):
            inside.Perform(gp_Pnt(x, y, float(z)), 1e-6)
            if inside.State() == TopAbs_IN:
                sys.exit(f"The case has no bolt hole under the standoff at X {x}, Y {y}: it is solid at Z {z:.2f}.")
    return case_lo, case_hi


def bolt():
    """One countersunk bolt, standing on its head at the origin, its thread up along Z."""
    r, R, k, L = BOLT_SHANK / 2, BOLT_HEAD / 2, BOLT_HEAD_HEIGHT, BOLT_LENGTH
    rim = k - (R - r)                    # a 90-degree cone under a thin flat rim
    chamfer = 0.3
    outline = [(0, 0), (R, 0), (R, rim), (r, k), (r, L - chamfer), (r - chamfer, L), (0, L)]
    wire = BRepBuilderAPI_MakeWire()
    for (x0, z0), (x1, z1) in zip(outline, outline[1:] + outline[:1]):
        wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(x0, 0, z0), gp_Pnt(x1, 0, z1)).Edge())
    face = BRepBuilderAPI_MakeFace(wire.Wire()).Face()
    return BRepPrimAPI_MakeRevol(face, gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1))).Shape()


def at(shape, x, y, z):
    t = gp_Trsf()
    t.SetTranslation(gp_Vec(x, y, z))
    return BRepBuilderAPI_Transform(shape, t, True).Shape()


def page(points):
    """Where points of the scene fall on the drawing, in millimetres, y up."""
    p = np.asarray(points, dtype=float)
    eye = np.array(EYE) / np.linalg.norm(EYE)
    right = np.array(RIGHT) / np.linalg.norm(RIGHT)
    up = np.cross(eye, right)
    return np.stack([p @ right, p @ up], axis=-1)


def visible_lines(scene, deflection=0.02, kinds=("sharp", "smooth", "outline")):
    """The scene's edges and outlines as the eye sees them, hidden parts removed: polylines on the page."""
    BRepMesh_IncrementalMesh(scene, deflection, False, 0.2, True)
    algo = HLRBRep_PolyAlgo()
    algo.Load(scene)
    algo.Projector(HLRAlgo_Projector(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(*EYE), gp_Dir(*RIGHT))))
    algo.Update()
    shapes = HLRBRep_PolyHLRToShape()
    shapes.Update(algo)
    # Edges where faces meet at an angle, edges where they meet smoothly (the ends of
    # roundings), and the outlines of round faces. Left out: "seam", the line where a
    # round face closes on itself, which a drawing does not show.
    found = {"sharp": shapes.VCompound, "smooth": shapes.Rg1LineVCompound, "seam": shapes.RgNLineVCompound,
             "outline": shapes.OutLineVCompound}
    lines = []
    for kind in kinds:
        part = found[kind]()
        if part is None or part.IsNull():
            continue
        BRepLib.BuildCurves3d_s(part)
        e = TopExp_Explorer(part, TopAbs_EDGE)
        while e.More():
            curve = BRepAdaptor_Curve(TopoDS.Edge(e.Current()))
            pts = GCPnts_TangentialDeflection(curve, 0.08, 0.005)
            line = [(pts.Value(i).X(), pts.Value(i).Y()) for i in range(1, pts.NbPoints() + 1)]
            if len(line) >= 2:
                lines.append(np.array(line))
            e.Next()
    return lines


def build_scene(case, board, case_lo):
    head = bolt()
    bolts = [at(head, x, y, case_lo[2] - BOLT_DROP) for x, y in STANDOFFS]
    return compound([moved(board, BOARD_LIFT), case] + bolts)


def draw(lines, screenshot, size=SIZE):
    """Lines and screenshot on a white square."""
    corners = np.array([(PANEL[0], PANEL[3], PANEL_Z + BOARD_LIFT), (PANEL[2], PANEL[3], PANEL_Z + BOARD_LIFT),
                        (PANEL[0], PANEL[1], PANEL_Z + BOARD_LIFT)])                 # top left, top right, bottom left
    quad = page(corners)
    # The screen's border is an edge of the board's model too: its corners must lie on drawn lines.
    drawn = np.concatenate(lines)
    for c in quad:
        if np.hypot(*(drawn - c).T).min() > 0.05:
            sys.exit("The screen is not where PANEL says: no drawn line passes its corner. Check PANEL against the board's model.")
    lo, hi = drawn.min(0), drawn.max(0)
    scale = min(SCALE * size / SIZE, (1 - 2 * MARGIN) * size / (hi - lo).max())
    origin = (lo + hi) / 2                                   # the drawing's middle goes to the picture's middle
    big = size * OVERSAMPLE

    def to_pixels(p):
        p = (np.asarray(p) - origin) * scale * OVERSAMPLE
        return np.stack([big / 2 + p[..., 0], big / 2 - p[..., 1]], axis=-1)

    canvas = Image.new("L", (big, big), 255)
    # The screenshot: dot (u, v) goes to top_left + u * across + v * down. PIL wants the way back.
    tl, tr, bl = to_pixels(quad)
    shot = screenshot.convert("L")
    w, h = shot.size
    across, down = (tr - tl) / w, (bl - tl) / h
    m = np.linalg.inv(np.array([[across[0], down[0]], [across[1], down[1]]]))
    offset = -m @ tl
    layer = shot.transform((big, big), Image.AFFINE, (m[0, 0], m[0, 1], offset[0], m[1, 0], m[1, 1], offset[1]),
                           resample=Image.NEAREST, fillcolor=255)
    canvas = ImageChops.darker(canvas, layer)
    pen = ImageDraw.Draw(canvas)
    width = max(1, round(LINE * OVERSAMPLE))
    for line in lines:
        pts = [tuple(p) for p in to_pixels(line)]
        pen.line(pts, fill=0, width=width, joint="curve")
        for x, y in (pts[0], pts[-1]):
            pen.ellipse((x - width / 2, y - width / 2, x + width / 2, y + width / 2), fill=0)
    return canvas.resize((size, size), Image.LANCZOS)


def main():
    parser = argparse.ArgumentParser(description="Draw the exploded view of the case.")
    parser.add_argument("--case", type=Path, help="the case's STEP file (the newest in docs/case/ if not given)")
    parser.add_argument("--board", type=Path, default=BOARD, help="Waveshare's STEP model of the board")
    parser.add_argument("--screen", type=Path, default=SCREEN, help="a 480 x 800 screenshot for the screen")
    parser.add_argument("--out", type=Path, help="the picture to write (the case's name with .png if not given)")
    args = parser.parse_args()
    case_path = args.case or newest_case()
    out = args.out or case_path.with_suffix(".png")
    for needed, what in ((case_path, "the case"), (args.board, "the board's 3D model (see the top of this script)"),
                         (args.screen, "the screenshot")):
        if not needed.is_file():
            sys.exit(f"{needed} is missing: {what}.")
    screenshot = Image.open(args.screen)
    if screenshot.size != (480, 800):
        sys.exit(f"{args.screen} is {screenshot.size[0]} x {screenshot.size[1]}, not a 480 x 800 screenshot.")

    start = time.time()
    print(f"Case: {case_path.relative_to(ROOT) if case_path.is_relative_to(ROOT) else case_path}")
    case = read_step(case_path)
    board = moved(read_step(args.board), quarter_turn=True)
    case_lo, _ = check_fit(case, board)
    print(f"  a bolt hole under each of the board's standoffs ({time.time() - start:.0f} s)")
    lines = visible_lines(build_scene(case, board, case_lo))
    print(f"  {len(lines)} visible lines ({time.time() - start:.0f} s)")
    picture = draw(lines, screenshot)
    picture.save(out, optimize=True)
    print(f"Wrote {out.relative_to(ROOT) if out.is_relative_to(ROOT) else out}, {SIZE} x {SIZE}, {out.stat().st_size // 1024} KB")


if __name__ == "__main__":
    main()
