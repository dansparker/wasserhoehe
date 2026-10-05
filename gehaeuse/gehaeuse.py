"""Parametrisches Gehaeuse fuer Bluepill + JSN-SR04T-Platine + CC2530.

    python gehaeuse.py          -> gehaeuse_unterteil.stl, gehaeuse_deckel.stl
    (pip install manifold3d numpy)

Masse in mm. Die Platinenmasse vor dem Druck nachmessen!
"""
import struct
import numpy as np
from manifold3d import Manifold

# ---------- Parameter -------------------------------------------------------
INNER_L, INNER_W, INNER_H = 115.0, 86.0, 32.0   # Innenraum
WALL, FLOOR, LID_T = 2.4, 2.0, 2.4
R_OUT = 4.0                                     # Eckenradius aussen
CLR = 0.6                                       # Spiel Platine/Tasche
RIB_T, RIB_H = 1.2, 4.0                         # Taschenrippen

BOARDS = {                                      # Name: (Laenge, Breite, x, y)
    "bluepill": (53.3, 22.9, 14.0, 10.0),
    "cc2530":   (38.0, 27.0, 73.0, 10.0),       # TENSTAR: nachmessen!
    "jsn":      (41.0, 28.5, 14.0, 46.0),
}

GLAND_D = 12.5                                  # PG7 (Mutter SW19: Abstand >= 22, frei von Domes/Platinen)
GLAND_Y = (21.0, 43.0, 65.0)                    # Sensor, DS18B20, Netzteil
GLAND_Z = 15.0                                  # Mitte ueber Boden innen
ANT_D = 6.5                                     # SMA-Buchse (0 = kein Loch)
ANT_Y, ANT_Z = 23.5, 20.0

BOSS_D, BOSS_HOLE = 7.0, 2.5                    # M3 selbstschneidend
BOSS_INSET = 4.5                                # Bossmitte ab Innenwand
SCREW_D, HEAD_D = 3.4, 6.2
LIP_H, LIP_T, LIP_CLR = 4.0, 1.6, 0.3
TAB_L, TAB_T, TAB_HOLE = 12.0, 4.0, 4.5         # Befestigungslaschen
SEG = 64
# ---------------------------------------------------------------------------

OUT_L, OUT_W = INNER_L + 2 * WALL, INNER_W + 2 * WALL
BASE_H = FLOOR + INNER_H


def box(x, y, z, at=(0, 0, 0)):
    return Manifold.cube((x, y, z)).translate(at)


def cyl(h, d, at=(0, 0, 0), d2=None):
    r2 = -1.0 if d2 is None else d2 / 2
    return Manifold.cylinder(h, d / 2, r2, SEG).translate(at)


def rbox(x, y, z, r, at=(0, 0, 0)):
    """Quader mit gerundeten senkrechten Kanten."""
    r = max(min(r, x / 2 - 0.01, y / 2 - 0.01), 0.01)
    posts = [cyl(z, 2 * r, (px, py, 0)) for px in (r, x - r) for py in (r, y - r)]
    return Manifold.batch_hull(posts).translate(at)


def boss_xy():
    xs = (WALL + BOSS_INSET, WALL + INNER_L - BOSS_INSET)
    ys = (WALL + BOSS_INSET, WALL + INNER_W - BOSS_INSET)
    return [(x, y) for x in xs for y in ys]


def pocket(l, w, x, y):
    """Rahmen aus Rippen mit Luecken fuer Kabel; Platine wird eingelegt/geklebt."""
    il, iw = l + 2 * CLR, w + 2 * CLR
    ox, oy, z = WALL + x - CLR - RIB_T, WALL + y - CLR - RIB_T, FLOOR
    frame = box(il + 2 * RIB_T, iw + 2 * RIB_T, RIB_H, (ox, oy, z)) \
        - box(il, iw, RIB_H + 1, (ox + RIB_T, oy + RIB_T, z - 0.5))
    gl, gw = 0.4 * il, 0.4 * iw                 # Luecken mittig je Seite
    frame -= box(gl, iw + 4 * RIB_T, RIB_H + 1, (ox + RIB_T + (il - gl) / 2, oy - RIB_T, z - 0.5))
    frame -= box(il + 4 * RIB_T, gw, RIB_H + 1, (ox - RIB_T, oy + RIB_T + (iw - gw) / 2, z - 0.5))
    return frame


def base():
    body = rbox(OUT_L, OUT_W, BASE_H, R_OUT)
    body -= rbox(INNER_L, INNER_W, BASE_H, R_OUT - WALL, (WALL, WALL, FLOOR))

    for bx, by in boss_xy():
        body += cyl(INNER_H, BOSS_D, (bx, by, FLOOR))
        body -= cyl(INNER_H - 2, BOSS_HOLE, (bx, by, FLOOR + 2.01))
    for l, w, x, y in BOARDS.values():
        body += pocket(l, w, x, y)

    # Kabelverschraubungen (Seite x=0, im Einbau nach UNTEN zeigen lassen)
    for gy in GLAND_Y:
        body -= cyl(WALL + 2, GLAND_D, (0, 0, 0)).rotate((0, 90, 0)) \
            .translate((-1, WALL + gy, FLOOR + GLAND_Z))
    if ANT_D > 0:                               # Antenne (Seite x=L, nach oben)
        body -= cyl(WALL + 2, ANT_D).rotate((0, 90, 0)) \
            .translate((OUT_L - WALL - 1, WALL + ANT_Y, FLOOR + ANT_Z))

    # Befestigungslaschen an den Laengsseiten
    for ty, sgn in ((0, -1), (OUT_W, 1)):
        tab = rbox(TAB_L * 1.6, TAB_L + R_OUT, TAB_T, 3,
                   (OUT_L / 2 - TAB_L * 0.8, ty - (TAB_L if sgn < 0 else R_OUT), 0))
        hole = cyl(TAB_T + 2, TAB_HOLE, (OUT_L / 2, ty + sgn * TAB_L / 2, -1))
        body = body + tab - hole
    return body


def lid():
    plate = rbox(OUT_L, OUT_W, LID_T, R_OUT)
    o = WALL + LIP_CLR
    lip = rbox(INNER_L - 2 * LIP_CLR, INNER_W - 2 * LIP_CLR, LIP_H, R_OUT - o, (o, o, LID_T)) \
        - rbox(INNER_L - 2 * LIP_CLR - 2 * LIP_T, INNER_W - 2 * LIP_CLR - 2 * LIP_T,
               LIP_H + 1, R_OUT - o - LIP_T, (o + LIP_T, o + LIP_T, LID_T - 0.5))
    for bx, by in boss_xy():                    # Platz fuer die Domes
        lip -= cyl(LIP_H + 1, BOSS_D + 2 * LIP_CLR + 0.4, (bx, by, LID_T - 0.5))
    body = plate + lip
    for bx, by in boss_xy():                    # Senkung liegt aussen (z=0)
        body -= cyl(LID_T + 2, SCREW_D, (bx, by, -1))
        body -= cyl(1.4, HEAD_D, (bx, by, -0.01), d2=SCREW_D)
    return body


def write_stl(m, path):
    mesh = m.to_mesh()
    v = np.asarray(mesh.vert_properties)[:, :3]
    t = np.asarray(mesh.tri_verts)
    tri = v[t]
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
    with open(path, "wb") as f:
        f.write(b"wasserhoehe gehaeuse".ljust(80, b" "))
        f.write(struct.pack("<I", len(t)))
        rec = np.zeros(len(t), dtype=[("n", "<f4", 3), ("v", "<f4", (3, 3)), ("a", "<u2")])
        rec["n"], rec["v"] = n, tri
        f.write(rec.tobytes())


def check(m, name):
    assert m.status().name == "NoError", (name, m.status())
    assert m.genus() >= 0 and len(m.decompose()) == 1, f"{name}: nicht zusammenhaengend"
    bb = m.bounding_box()
    print(f"{name}: {bb[3]-bb[0]:.1f} x {bb[4]-bb[1]:.1f} x {bb[5]-bb[2]:.1f} mm, "
          f"{m.volume()/1000:.1f} cm3, {m.num_tri()} Dreiecke, OK")


if __name__ == "__main__":
    b, d = base(), lid()
    check(b, "Unterteil")
    check(d, "Deckel")
    write_stl(b, "gehaeuse_unterteil.stl")
    write_stl(d, "gehaeuse_deckel.stl")
