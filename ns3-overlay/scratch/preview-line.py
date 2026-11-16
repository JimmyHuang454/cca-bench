#!/usr/bin/env python3
"""
Check and preview a 3-D railway line file (scratch/lines/*.json, see
scratch/lines/FORMAT.md) without running ns-3.

It validates the file, integrates the speed profile along the track polyline,
prints the time/position table and the cell list, and draws a top view (track,
tunnel, cells with their range, sector directions, interferers, train position
at every segment border) and the speed / interference profile over time.

Usage (from the ns-3 root):
  python3 scratch/preview-line.py scratch/lines/example.json [--out DIR]
Writes <out>/<name>-preview.png/.pdf/.svg (overview) and <out>/<name>-cells.*
(true-scale close-up of every cell with its offset from the track), by default
next to the json file.
"""

import argparse
import json
import math
import os
import sys


DEF_KEYS = {"car_loss_db", "edge_rsrp_dbm", "cell", "tunnel_cell_spacing_m", "tunnel_loss_db",
            "tunnel_ramp_m"}
TOP_KEYS = {"tech", "defaults", "track", "timeline", "cells", "cell_grid", "interferers"}
SEG_KEYS = {"speed_kmh", "interference_dbm", "tunnel", "car_loss_db", "crowd_ues", "crowd_mbps"}
CELL_KEYS = {"id", "pos", "track_m", "side", "offset_m", "height_m", "range_m", "range_mode",
             "tx_dbm", "antenna"}
GRID_KEYS = {"from_m", "to_m", "spacing_m", "side", "offset_m", "height_m", "range_m",
             "range_mode", "tx_dbm", "antenna", "id_prefix"}
ANT_KEYS = {"type", "azimuth_deg", "tilt_deg", "beamwidth_deg", "gain_dbi"}
INTF_KEYS = {"id", "pos", "track_m", "side", "offset_m", "height_m", "range_m", "power_dbm"}


class LineError(Exception):
    """Invalid line file."""


def public(d):
    """Keys that are not comments."""
    return {k: v for k, v in d.items() if not k.startswith("_")}


def check_keys(d, allowed, where):
    """Reject unknown keys (typos)."""
    bad = set(public(d)) - allowed
    if bad:
        raise LineError(f"{where}: unknown field(s) {sorted(bad)}; allowed: {sorted(allowed)}")


def parse_range(key, where):
    """'a-b' -> (a, b)."""
    try:
        a, b = key.split("-")
        a, b = float(a), float(b)
    except ValueError as exc:
        raise LineError(f"{where}: time range '{key}' must look like '10-40'") from exc
    if b <= a:
        raise LineError(f"{where}: time range '{key}' must have start < end")
    return a, b


class Track:
    """Polyline with arc-length parametrisation."""

    def __init__(self, points):
        if len(points) < 2:
            raise LineError("track.points: at least two points are needed")
        self.p = [tuple(float(c) for c in q) for q in points]
        for q in self.p:
            if len(q) != 3:
                raise LineError("track.points: every point must be [x, y, z]")
        self.cum = [0.0]
        for a, b in zip(self.p, self.p[1:]):
            self.cum.append(self.cum[-1] + math.dist(a, b))
        self.length = self.cum[-1]

    def at(self, s):
        """Point at arc length s."""
        s = min(max(s, 0.0), self.length)
        for k in range(len(self.p) - 1):
            if s <= self.cum[k + 1] or k == len(self.p) - 2:
                seg = self.cum[k + 1] - self.cum[k]
                f = 0.0 if seg == 0 else (s - self.cum[k]) / seg
                a, b = self.p[k], self.p[k + 1]
                return tuple(a[i] + f * (b[i] - a[i]) for i in range(3))
        return self.p[-1]

    def frame(self, s):
        """(point, unit tangent, unit left normal) at arc length s (horizontal plane)."""
        s = min(max(s, 0.0), self.length)
        k = next((i for i in range(len(self.p) - 1) if s <= self.cum[i + 1]), len(self.p) - 2)
        a, b = self.p[k], self.p[k + 1]
        dx, dy = b[0] - a[0], b[1] - a[1]
        h = math.hypot(dx, dy) or 1.0
        t = (dx / h, dy / h)
        return self.at(s), t, (-t[1], t[0])

    def beside(self, s, side, offset, height):
        """Point beside the track: side +1 left / -1 right of the direction of travel."""
        p, _t, n = self.frame(s)
        return (p[0] + side * offset * n[0], p[1] + side * offset * n[1], p[2] + height)

    def lateral(self, q):
        """(arc length, signed horizontal offset, + = left) of the track point nearest to q."""
        s, _d = self.project(q)
        p, _t, n = self.frame(s)
        return s, (q[0] - p[0]) * n[0] + (q[1] - p[1]) * n[1]

    def project(self, q):
        """(arc length, distance) of the track point nearest to q."""
        best = (0.0, float("inf"))
        for k in range(len(self.p) - 1):
            a, b = self.p[k], self.p[k + 1]
            ab = [b[i] - a[i] for i in range(3)]
            l2 = sum(c * c for c in ab)
            f = 0.0 if l2 == 0 else max(0.0, min(1.0, sum((q[i] - a[i]) * ab[i]
                                                          for i in range(3)) / l2))
            c = [a[i] + f * ab[i] for i in range(3)]
            d = math.dist(q, c)
            if d < best[1]:
                best = (self.cum[k] + f * math.sqrt(l2), d)
        return best


def load(path):
    """Read and validate a line file; returns a dict of derived data."""
    with open(path, encoding="utf-8") as stream:
        try:
            cfg = json.load(stream)
        except json.JSONDecodeError as exc:
            raise LineError(f"not valid JSON: {exc}") from exc
    check_keys(cfg, TOP_KEYS, "top level")
    if "timeline" not in cfg:
        raise LineError("missing required field 'timeline'")
    if "cells" not in cfg and "cell_grid" not in cfg:
        raise LineError("no cells: give 'cells' and/or 'cell_grid'")
    tech = cfg.get("tech", "4g")
    if tech not in ("4g", "5g"):
        raise LineError("tech must be '4g' or '5g'")
    defaults = public(cfg.get("defaults", {}))
    check_keys(defaults, DEF_KEYS, "defaults")
    cell_def = public(defaults.get("cell", {}))
    check_keys(cell_def, CELL_KEYS - {"id", "pos", "track_m"}, "defaults.cell")

    # Timeline.
    segs = []
    for key, seg in public(cfg["timeline"]).items():
        where = f"timeline['{key}']"
        a, b = parse_range(key, where)
        check_keys(seg, SEG_KEYS, where)
        if "speed_kmh" not in seg:
            raise LineError(f"{where}: speed_kmh is required")
        v = seg["speed_kmh"]
        v0, v1 = (v, v) if isinstance(v, (int, float)) else tuple(v)
        if min(v0, v1) < 0:
            raise LineError(f"{where}: speed must be >= 0")
        crowd = int(seg.get("crowd_ues", 0))
        if crowd < 0 or seg.get("crowd_mbps", 1) <= 0:
            raise LineError(f"{where}: crowd_ues must be >= 0 and crowd_mbps > 0")
        if "crowd_mbps" in seg and not crowd:
            raise LineError(f"{where}: crowd_mbps needs crowd_ues")
        segs.append({"t0": a, "t1": b, "v0": v0 / 3.6, "v1": v1 / 3.6,
                     "intf": seg.get("interference_dbm"), "tunnel": bool(seg.get("tunnel")),
                     "car_loss": seg.get("car_loss_db"), "crowd": crowd,
                     "crowd_mbps": seg.get("crowd_mbps", 4.0),
                     "key": key})
    segs.sort(key=lambda s: s["t0"])
    if not segs or segs[0]["t0"] != 0:
        raise LineError("timeline must start at 0 s")
    for s, n in zip(segs, segs[1:]):
        if abs(s["t1"] - n["t0"]) > 1e-9:
            raise LineError(f"timeline: gap or overlap between '{s['key']}' and '{n['key']}'")
    s_pos = 0.0
    for s in segs:
        s["s0"] = s_pos
        s_pos += 0.5 * (s["v0"] + s["v1"]) * (s["t1"] - s["t0"])
        s["s1"] = s_pos
    distance, duration = s_pos, segs[-1]["t1"]

    track = Track(cfg.get("track", {}).get("points", [[0, 0, 0], [distance, 0, 0]]))
    if distance > track.length + 1e-6:
        raise LineError(f"the train travels {distance:.0f} m but the track is only "
                        f"{track.length:.0f} m long")

    # Cells: explicit list plus the generated grids.
    specs = []
    for k, c in enumerate(cfg.get("cells", [])):
        check_keys(c, CELL_KEYS, f"cells[{k}]")
        specs.append((f"cells[{k}]", public(c), c.get("id", str(k + 1))))
    for g_i, g in enumerate(cfg.get("cell_grid", [])):
        where = f"cell_grid[{g_i}]"
        check_keys(g, GRID_KEYS, where)
        if "spacing_m" not in g or g["spacing_m"] <= 0:
            raise LineError(f"{where}: spacing_m > 0 is required")
        g = public(g)
        x, n = g.get("from_m", 0.0), 0
        to = g.get("to_m", distance)
        while x <= to + 1e-6:
            spec = {k: v for k, v in g.items()
                    if k not in ("from_m", "to_m", "spacing_m", "id_prefix")}
            spec["track_m"] = x
            if g.get("side", cell_def.get("side")) == "alternate":
                spec["side"] = "left" if n % 2 == 0 else "right"
            specs.append((f"{where} at {x:.0f} m", spec, f"{g.get('id_prefix', 'g')}{n + 1}"))
            x += g["spacing_m"]
            n += 1

    cells = []
    alt = 0
    for where, c, cid in specs:
        merged = dict(cell_def)
        merged.update(c)
        if ("pos" in c) == ("track_m" in c):
            raise LineError(f"{where}: give either pos [x, y, z] or track_m (not both)")
        if "pos" in c:
            if len(c["pos"]) != 3:
                raise LineError(f"{where}: pos must be [x, y, z]")
            pos = tuple(float(v) for v in c["pos"])
        else:
            if not 0 <= c["track_m"] <= track.length:
                raise LineError(f"{where}: track_m {c['track_m']} outside the track "
                                f"(0-{track.length:.0f} m)")
            side = merged.get("side", "left")
            if side == "alternate":
                side = "left" if alt % 2 == 0 else "right"
                alt += 1
            if side not in ("left", "right"):
                raise LineError(f"{where}: side must be 'left', 'right' or 'alternate'")
            pos = track.beside(c["track_m"], 1 if side == "left" else -1,
                               merged.get("offset_m", 50.0), merged.get("height_m", 35.0))
        ant = dict(public(cell_def.get("antenna", {})))
        ant.update(public(c.get("antenna", {})))
        check_keys(ant, ANT_KEYS, f"{where}.antenna")
        mode = merged.get("range_mode", "soft")
        if mode not in ("soft", "hard"):
            raise LineError(f"{where}: range_mode must be 'soft' or 'hard'")
        if mode == "hard" and "range_m" not in merged:
            raise LineError(f"{where}: range_mode 'hard' needs range_m")
        along, lat = track.lateral(pos)
        cells.append({"id": cid, "pos": pos, "range": merged.get("range_m"), "mode": mode,
                      "tx": merged.get("tx_dbm", 46), "ant": ant, "along": along,
                      "lat": lat, "dist": abs(lat)})
    cells.sort(key=lambda c: c["along"])
    if not cells:
        raise LineError("cells: at least one cell is needed")

    intfs = []
    for k, i in enumerate(cfg.get("interferers", [])):
        where = f"interferers[{k}]"
        check_keys(i, INTF_KEYS, where)
        for f in ("range_m", "power_dbm"):
            if f not in i:
                raise LineError(f"{where}: {f} is required")
        if ("pos" in i) == ("track_m" in i):
            raise LineError(f"{where}: give either pos [x, y, z] or track_m (not both)")
        if "pos" in i:
            pos = tuple(float(v) for v in i["pos"])
        else:
            side = i.get("side", "left")
            if side not in ("left", "right"):
                raise LineError(f"{where}: side must be 'left' or 'right'")
            pos = track.beside(i["track_m"], 1 if side == "left" else -1,
                               i.get("offset_m", 100.0), i.get("height_m", 20.0))
        along, lat = track.lateral(pos)
        intfs.append({"id": i.get("id", f"J{k + 1}"), "pos": pos, "range": i["range_m"],
                      "power": i["power_dbm"], "along": along, "lat": lat})

    tunnels = [(s["s0"], s["s1"]) for s in segs if s["tunnel"]]
    return {"cfg": cfg, "tech": tech, "segs": segs, "distance": distance,
            "duration": duration, "track": track, "cells": cells, "intfs": intfs,
            "tunnels": tunnels, "car_loss": defaults.get("car_loss_db", 0),
            "edge": defaults.get("edge_rsrp_dbm", -110),
            "tunnel_spacing": defaults.get("tunnel_cell_spacing_m", 500),
            "tunnel_loss": defaults.get("tunnel_loss_db", 40),
            "tunnel_ramp": defaults.get("tunnel_ramp_m", 100)}

def arc_at(segs, t):
    """Distance travelled along the track at time t."""
    for s in segs:
        if s["t0"] <= t <= s["t1"]:
            dt = t - s["t0"]
            acc = (s["v1"] - s["v0"]) / (s["t1"] - s["t0"])
            return s["s0"] + s["v0"] * dt + 0.5 * acc * dt * dt
    return segs[-1]["s1"]


def tunnel_cells(L):
    """In-tunnel cells: on the track (3 m aside, 6 m high) every tunnel_spacing.

    tunnel_cell_spacing_m = 0 means no in-tunnel coverage (e.g. a pedestrian
    underpass without indoor antennas): the phone only hears the masts outside.
    """
    out = []
    if L["tunnel_spacing"] <= 0:
        return out
    for k, (a, b) in enumerate(L["tunnels"]):
        n = max(1, round((b - a) / L["tunnel_spacing"]))
        for j in range(n):
            s = a + (b - a) * (j + 0.5) / n
            out.append((f"T{k + 1}.{j + 1}", L["track"].beside(s, 1, 3.0, 6.0), s))
    return out


def export(L, path, step=0.2):
    """Write the resolved line for the ns-3 program (scratch/tcp-train-line.cc).

    Plain text, one record per line (lengths in m, times in s, powers in dBm):
      tech 4g|5g / duration D / car_loss DB / edge_rsrp DBM / tunnel_loss DB
      tunnel_ramp M                    distance over which the tunnel loss builds up
      track x y z                      polyline, in order
      tunnel s0 s1                     arc-length range along the track
      wp t x y z                       train (UE) waypoints every `step` s
      cell id x y z tx range hard ant az tilt bw gain ground_z
                                       range -1 = none; hard 0/1; ant omni|sector;
                                       ground_z = track height below the mast
      tcell id x y z                   in-tunnel cell
      car t0 t1 DB                     car loss override over a time range
      itime t0 t1 DBM                  interference over a time range
      isrc id x y z range DBM          interference source
      crowd t0 t1 N MBPS x y z         N other users around the phone (at its
                                       position in the middle of the range), each
                                       downloading MBPS Mbit/s over the range
    """
    tr = L["track"]
    lines = [f"tech {L['tech']}", f"duration {L['duration']}", f"car_loss {L['car_loss']}",
             f"edge_rsrp {L['edge']}", f"tunnel_loss {L['tunnel_loss']}",
             f"tunnel_ramp {L['tunnel_ramp']}"]
    lines += [f"track {p[0]} {p[1]} {p[2]}" for p in tr.p]
    lines += [f"tunnel {a} {b}" for a, b in L["tunnels"]]
    n = int(math.ceil(L["duration"] / step))
    for k in range(n + 1):
        t = min(L["duration"], k * step)
        p = tr.at(arc_at(L["segs"], t))
        lines.append(f"wp {t:.3f} {p[0]:.3f} {p[1]:.3f} {p[2] + 1.5:.3f}")
    for c in L["cells"]:
        a = c["ant"]
        sector = a.get("type", "omni") == "sector"
        rng = c["range"] if c["range"] is not None else -1
        lines.append(" ".join(str(v) for v in [
            "cell", c["id"], f"{c['pos'][0]:.3f}", f"{c['pos'][1]:.3f}", f"{c['pos'][2]:.3f}",
            c["tx"], rng, 1 if c["mode"] == "hard" else 0, "sector" if sector else "omni",
            a.get("azimuth_deg", 0), a.get("tilt_deg", 0), a.get("beamwidth_deg", 65),
            a.get("gain_dbi", 15) if sector else 0, f"{tr.at(c['along'])[2]:.3f}"]))
    for cid, p, _s in tunnel_cells(L):
        lines.append(f"tcell {cid} {p[0]:.3f} {p[1]:.3f} {p[2]:.3f}")
    for s in L["segs"]:
        if s["car_loss"] is not None:
            lines.append(f"car {s['t0']} {s['t1']} {s['car_loss']}")
        if s["intf"] is not None:
            lines.append(f"itime {s['t0']} {s['t1']} {s['intf']}")
        if s["crowd"]:
            p = tr.at(arc_at(L["segs"], 0.5 * (s["t0"] + s["t1"])))
            lines.append(f"crowd {s['t0']} {s['t1']} {s['crowd']} {s['crowd_mbps']} "
                         f"{p[0]:.3f} {p[1]:.3f} {p[2] + 1.5:.3f}")
    for i in L["intfs"]:
        lines.append(f"isrc {i['id']} {i['pos'][0]:.3f} {i['pos'][1]:.3f} {i['pos'][2]:.3f} "
                     f"{i['range']} {i['power']}")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")



def speed_at(segs, t):
    """Train speed [m/s] at time t."""
    for s in segs:
        if s["t0"] <= t <= s["t1"]:
            f = (t - s["t0"]) / (s["t1"] - s["t0"])
            return s["v0"] + f * (s["v1"] - s["v0"])
    return 0.0


def report(L):
    """Print the time/position table and the cells."""
    print(f"tech {L['tech']}, duration {L['duration']:.0f} s, distance {L['distance']:.0f} m "
          f"(track {L['track'].length:.0f} m)")
    print(f"{'time (s)':>12} {'speed (km/h)':>14} {'track (m)':>15}  interference  tunnel"
          "  loss    crowd")
    for s in L["segs"]:
        sp = (f"{s['v0'] * 3.6:.0f}" if s["v0"] == s["v1"]
              else f"{s['v0'] * 3.6:.0f}->{s['v1'] * 3.6:.0f}")
        print(f"{s['t0']:5.0f}-{s['t1']:<6.0f} {sp:>14} {s['s0']:7.0f}-{s['s1']:<7.0f}  "
              f"{(str(s['intf']) + ' dBm') if s['intf'] is not None else '-':>12}  "
              f"{'yes' if s['tunnel'] else '':<6}  "
              f"{(str(s['car_loss']) + ' dB') if s['car_loss'] is not None else '-':<6}  "
              f"{(str(s['crowd']) + ' x ' + str(s['crowd_mbps']) + ' Mbps') if s['crowd'] else ''}")
    print("cells:")
    for c in L["cells"]:
        rng = (f"range {c['range']:.0f} m ({c['mode']})" if c["range"] is not None
               else f"tx {c['tx']} dBm")
        print(f"  {c['id']:>3} at ({c['pos'][0]:.0f}, {c['pos'][1]:.0f}, {c['pos'][2]:.0f}) "
              f"{rng}, {c['ant'].get('type', 'omni')}: track {c['along']:.0f} m, "
              f"{abs(c['lat']):.0f} m {'left' if c['lat'] >= 0 else 'right'} of the track")
    for a, b in L["tunnels"]:
        n = max(1, round((b - a) / L["tunnel_spacing"])) if L["tunnel_spacing"] > 0 else 0
        print(f"  tunnel {a:.0f}-{b:.0f} m along the track: {n} in-tunnel cells added")


def draw(L, out):
    """Top view, unrolled side view and time profile."""
    tr = L["track"]
    xs_all = [p[0] for p in tr.p] + [c["pos"][0] for c in L["cells"]]
    ys_all = [p[1] for p in tr.p] + [c["pos"][1] for c in L["cells"]]
    aspect = (max(ys_all) - min(ys_all) + 2000) / (max(xs_all) - min(xs_all) + 2000)
    fig, (ax, au, at) = plt.subplots(
        3, 1, figsize=(9.0, 4.8 + 8.0 * aspect),
        gridspec_kw={"height_ratios": [8.0 * aspect, 2.0, 1.6]}, layout="constrained")
    ink, grey = "#3a3a38", "#8c8b86"

    # Interferers and cell ranges (below the track).
    for i in L["intfs"]:
        ax.add_patch(Circle(i["pos"][:2], i["range"], color="#eb6834", alpha=0.15, lw=0))
        ax.annotate(f"{i['id']}  {i['power']} dBm", (i["pos"][0], i["pos"][1] + i["range"]),
                    xytext=(0, 3), textcoords="offset points", ha="center", va="bottom",
                    fontsize=8, color="#b4441a")
    for c in L["cells"]:
        x, y = c["pos"][:2]
        if c["range"] is not None:
            if c["mode"] == "hard":
                ax.add_patch(Circle((x, y), c["range"], color="#2a78d6", alpha=0.12, lw=0))
            ax.add_patch(Circle((x, y), c["range"], fill=False, ec="#2a78d6", lw=0.7))
        if c["ant"].get("type") == "sector":
            az = c["ant"].get("azimuth_deg", 0)
            bw = c["ant"].get("beamwidth_deg", 65)
            r = c["range"] or 600
            ax.add_patch(Wedge((x, y), r, az - bw / 2, az + bw / 2, color="#2a78d6",
                               alpha=0.15, lw=0))
        ax.plot([x], [y], marker="^", color=ink, markersize=7)
        ax.annotate(c["id"], (x, y), xytext=(5, 4 if c["lat"] >= 0 else -11),
                    textcoords="offset points", ha="left", fontsize=8, color=ink)

    # Track: whole polyline, travelled part, tunnels.
    xs = [p[0] for p in tr.p]
    ys = [p[1] for p in tr.p]
    ax.plot(xs, ys, color=grey, lw=1.0)
    n = 400
    path = [tr.at(L["distance"] * k / n) for k in range(n + 1)]
    ax.plot([p[0] for p in path], [p[1] for p in path], color=ink, lw=2.0)
    for a, b in L["tunnels"]:
        seg = [tr.at(a + (b - a) * k / 50) for k in range(51)]
        ax.plot([p[0] for p in seg], [p[1] for p in seg], color="#8c8b86", lw=6,
                solid_capstyle="butt", alpha=0.6)
        n_tc = (max(1, round((b - a) / L["tunnel_spacing"])) if L["tunnel_spacing"] > 0 else 0)
        for k in range(n_tc):
            q = tr.at(a + (b - a) * (k + 0.5) / n_tc)
            ax.plot([q[0]], [q[1]], marker="s", color=ink, markersize=4, zorder=6)
        mid = tr.at(0.5 * (a + b))
        ax.annotate("tunnel", mid[:2], xytext=(0, -14), textcoords="offset points",
                    ha="center", fontsize=8, color=ink)
    marks = []
    for s in L["segs"] + [{"t0": L["duration"], "s0": L["distance"]}]:
        if marks and s["s0"] - marks[-1][0] < 0.03 * L["distance"]:
            marks[-1][1].append(s["t0"])
        else:
            marks.append((s["s0"], [s["t0"]]))
    for pos, times in marks:
        p = tr.at(pos)
        ax.plot([p[0]], [p[1]], marker="o", color="white", mec=ink, markersize=4, zorder=7)
        ax.annotate("/".join(f"{t:.0f}" for t in times) + " s", p[:2], xytext=(0, -13),
                    textcoords="offset points", ha="center", fontsize=7, color=grey)
    ax.set_aspect("equal")
    ax.set_xlabel("x (m)")
    ax.set_ylabel("y (m)")
    ax.set_title(f"{os.path.basename(out)}: top view ({L['tech'].upper()}, "
                 f"{L['distance'] / 1000:.1f} km in {L['duration']:.0f} s)", fontsize=10)
    ax.grid(color="#e4e4e1", lw=0.5)

    # Unrolled view: track mileage vs lateral offset (+ = left), so the
    # distance of every mast from the track is visible.
    lat_max = max([abs(c["lat"]) for c in L["cells"]] + [abs(i["lat"]) for i in L["intfs"]]
                  + [50.0])
    lim = 1.35 * lat_max
    for a, b in L["tunnels"]:
        au.axvspan(a, b, color=grey, alpha=0.18, lw=0)
        au.text(0.5 * (a + b), -0.92 * lim, "tunnel", ha="center", fontsize=7, color=ink)
        n_tc = (max(1, round((b - a) / L["tunnel_spacing"])) if L["tunnel_spacing"] > 0 else 0)
        for k in range(n_tc):
            au.plot([a + (b - a) * (k + 0.5) / n_tc], [0], marker="s", color=ink,
                    markersize=4, zorder=6)
    for i in L["intfs"]:
        au.axvspan(i["along"] - i["range"], i["along"] + i["range"], color="#eb6834",
                   alpha=0.10, lw=0)
        au.plot([i["along"]], [max(-lim, min(lim, i["lat"]))], marker="*", color="#b4441a",
                markersize=8, linestyle="none")
        au.annotate(f"{i['id']} {i['power']} dBm, r={i['range']:.0f} m",
                    (i["along"], max(-lim, min(lim, i["lat"]))), xytext=(6, 0),
                    textcoords="offset points", va="center", fontsize=7, color="#b4441a")
    au.axhline(0, color=ink, lw=2.0)
    au.axvspan(L["distance"], tr.length * 1.02, color="white", alpha=0.65, lw=0, zorder=3)
    au.text(L["distance"], 0.92 * lim, f" end ({L['duration']:.0f} s)", ha="left", va="top",
            fontsize=7, color=grey, zorder=4)
    for c in L["cells"]:
        au.plot([c["along"], c["along"]], [0, c["lat"]], color=grey, lw=0.6)
        au.plot([c["along"]], [c["lat"]], marker="^", color=ink, markersize=6)
        au.annotate(f"{c['id']} {abs(c['lat']):.0f} m", (c["along"], c["lat"]),
                    xytext=(0, 5 if c["lat"] >= 0 else -11), textcoords="offset points",
                    ha="center", fontsize=7, color=ink)
    au.set_ylim(-lim, lim)
    au.set_xlim(-0.02 * tr.length, tr.length * 1.02)
    au.set_xlabel("distance along the track (m)")
    au.set_ylabel("left (+) / right (-)\nof the track (m)")
    au.grid(color="#e4e4e1", lw=0.5)

    # Time profile: speed and interference.
    ts = [L["duration"] * k / 500 for k in range(501)]
    at.plot(ts, [speed_at(L["segs"], t) * 3.6 for t in ts], color=ink, lw=1.4)
    for s in L["segs"]:
        if s["intf"] is not None:
            at.axvspan(s["t0"], s["t1"], color="#eb6834", alpha=0.15, lw=0)
            at.text(0.5 * (s["t0"] + s["t1"]), 5, f"{s['intf']} dBm", ha="center",
                    fontsize=7, color="#b4441a")
        if s["tunnel"]:
            at.axvspan(s["t0"], s["t1"], color=grey, alpha=0.18, lw=0)
            at.text(0.5 * (s["t0"] + s["t1"]), 5, "tunnel", ha="center", fontsize=7, color=ink)
    vmax = max(max(s["v0"], s["v1"]) for s in L["segs"]) * 3.6
    ytop = max(10, vmax * 1.15) if vmax < 40 else max(50, vmax * 1.15)
    for s in L["segs"]:
        tags = []
        if s["car_loss"] is not None:
            tags.append(f"+{s['car_loss']:g} dB")
        if s["crowd"]:
            tags.append(f"crowd {s['crowd']}")
        if tags:
            at.axvspan(s["t0"], s["t1"], color="#2a78d6", alpha=0.08, lw=0)
            at.text(0.5 * (s["t0"] + s["t1"]), 0.88 * ytop, "\n".join(tags), ha="center",
                    va="top", fontsize=6, color="#1c5aa6")
    at.set_xlim(0, L["duration"])
    at.set_ylim(0, ytop)
    at.set_xlabel("time (s)")
    at.set_ylabel("speed (km/h)")
    at.grid(color="#e4e4e1", lw=0.5)
    for ext in ("png", "pdf", "svg"):
        fig.savefig(f"{out}.{ext}", dpi=200 if ext == "png" else None)
    print(f"preview written to {out}.png")


def draw_cells(L, out, ncols=4, half_window=120.0):
    """True-scale close-up of every outdoor cell: track, mast, its offset.

    Each panel is drawn in real x/y coordinates with equal axis scales,
    centred on the track point nearest to the cell, so the side of the track
    and the perpendicular distance of the mast are seen as they are.
    """
    tr = L["track"]
    cells = L["cells"]
    ink, grey, blue = "#3a3a38", "#8c8b86", "#2a78d6"
    nrows = math.ceil(len(cells) / ncols)
    fig, axes = plt.subplots(nrows, ncols, figsize=(2.6 * ncols, 2.5 * nrows),
                             layout="constrained", squeeze=False)
    for ax in axes.flat[len(cells):]:
        ax.set_visible(False)
    for ax, c in zip(axes.flat, cells):
        foot, tangent, normal = tr.frame(c["along"])
        cx, cy = c["pos"][0], c["pos"][1]
        half = max(half_window, abs(c["lat"]) * 1.4 + 40)
        mx, my = 0.5 * (foot[0] + cx), 0.5 * (foot[1] + cy)
        # Track inside the window, with the direction of travel.
        n = 200
        a = max(0.0, c["along"] - 2 * half)
        b = min(tr.length, c["along"] + 2 * half)
        pts = [tr.at(a + (b - a) * k / n) for k in range(n + 1)]
        ax.plot([p[0] for p in pts], [p[1] for p in pts], color=ink, lw=2.2, zorder=2)
        for t_a, t_b in L["tunnels"]:
            lo, hi = max(a, t_a), min(b, t_b)
            if lo < hi:
                seg = [tr.at(lo + (hi - lo) * k / 40) for k in range(41)]
                ax.plot([p[0] for p in seg], [p[1] for p in seg], color=grey, lw=7,
                        alpha=0.5, solid_capstyle="butt", zorder=1)
        ahead = tr.at(min(tr.length, c["along"] + 0.8 * half))
        ax.annotate("", xy=ahead[:2], xytext=tr.at(min(tr.length, c["along"] + 0.45 * half))[:2],
                    arrowprops={"arrowstyle": "->", "color": ink, "lw": 1.0})
        # Offset dimension line and mast.
        ax.plot([foot[0], cx], [foot[1], cy], color=blue, lw=1.0, zorder=3)
        ax.plot([foot[0]], [foot[1]], marker="o", color=ink, markersize=3, zorder=4)
        ax.plot([cx], [cy], marker="^", color=ink, markersize=9, zorder=5)
        if c["range"] is not None:
            ax.add_patch(Circle((cx, cy), c["range"], fill=c["mode"] == "hard", ec=blue,
                                fc=blue, alpha=0.12 if c["mode"] == "hard" else 1.0,
                                lw=0.8, zorder=1))
        if c["ant"].get("type") == "sector":
            az = c["ant"].get("azimuth_deg", 0)
            bw = c["ant"].get("beamwidth_deg", 65)
            ax.add_patch(Wedge((cx, cy), 0.8 * half, az - bw / 2, az + bw / 2, color=blue,
                               alpha=0.18, lw=0, zorder=1))
        side = "left" if c["lat"] >= 0 else "right"
        ax.annotate(f"{abs(c['lat']):.0f} m {side}",
                    (0.5 * (foot[0] + cx), 0.5 * (foot[1] + cy)),
                    xytext=(-5, 0), textcoords="offset points", fontsize=7, color=blue,
                    va="center", ha="right")
        ax.set_title(f"{c['id']}  km {c['along'] / 1000:.2f},  antenna {c['pos'][2]:.0f} m",
                     fontsize=8, color=ink)
        ax.set_xlim(mx - half, mx + half)
        ax.set_ylim(my - half, my + half)
        ax.set_aspect("equal")
        ax.tick_params(labelsize=6)
        ax.set_xlabel("x (m)", fontsize=6)
        ax.set_ylabel("y (m)", fontsize=6)
        ax.grid(color="#e4e4e1", lw=0.5)
    fig.suptitle("Cells at true scale (arrow = direction of travel; blue line = offset "
                 "from the track)", fontsize=9)
    for ext in ("png", "pdf", "svg"):
        fig.savefig(f"{out}.{ext}", dpi=200 if ext == "png" else None)
    print(f"cell close-ups written to {out}.png")


def load_matplotlib():
    """Import matplotlib only for the figures: --export --no-plot does not need it."""
    global plt, Circle, Wedge  # pylint: disable=global-statement
    import matplotlib  # pylint: disable=import-outside-toplevel

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt  # pylint: disable=import-outside-toplevel
    from matplotlib.patches import Circle, Wedge  # pylint: disable=import-outside-toplevel


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("file")
    parser.add_argument("--out", default="")
    parser.add_argument("--export", default="",
                        help="write the resolved line for tcp-train-line to this file")
    parser.add_argument("--no-plot", action="store_true", help="only validate / export")
    args = parser.parse_args()
    try:
        L = load(args.file)
    except LineError as exc:
        sys.exit(f"{args.file}: {exc}")
    if args.export:
        export(L, args.export)
        print(f"resolved line written to {args.export}")
    if args.no_plot:
        return
    report(L)
    stem = os.path.splitext(os.path.basename(args.file))[0]
    out_dir = args.out or os.path.dirname(os.path.abspath(args.file))
    load_matplotlib()
    draw(L, os.path.join(out_dir, f"{stem}-preview"))
    draw_cells(L, os.path.join(out_dir, f"{stem}-cells"))


if __name__ == "__main__":
    main()
