# B1 oracle. Run:  HOUDINI_PACKAGE_DIR=build/package hython tests/oracle_b1.py [character_dir]   (default: $MH_CHARACTER)
# Expectations come from independent sources:
#   tests/expect_dna.json            - OpenRigLogic Python bindings (tests/dna_expect.py)
#   tests/expect_blender_joints.json - joint positions from Blender's Character DNA import
# Exit code 0 only if every gate passes.
import json
import os
import sys
import time

import hou

HERE = os.path.dirname(os.path.abspath(__file__))
CHAR = sys.argv[1] if len(sys.argv) > 1 else os.environ["MH_CHARACTER"]
EXP = json.load(open(os.path.join(HERE, "expect_dna.json")))
# Optional third-party cross-check; without it G3c checks joint names/count only.
BL_PATH = os.path.join(HERE, "expect_blender_joints.json")
BL_JOINTS = json.load(open(BL_PATH)) if os.path.exists(BL_PATH) else None

results = []


def gate(name, ok, detail):
    results.append((name, bool(ok), detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}")


# ---------------- G1: node type and parameters ----------------
nt = hou.sopNodeTypeCategory().nodeTypes().get("mhdna_import")
expected_parms = {"headdnafile": "", "bodydnafile": "", "maxlod": 0, "minlod": 0, "addmeshes": 1, "addjoints": 1,
                  "addskinning": 1, "addblendshapes": 1, "addriglogic": 1, "unitsmultiplier": 1.0, "reload": None}
geo = hou.node("/obj").createNode("geo", "oracle")
sop = geo.createNode("mhdna_import") if nt else None
bad = []
if sop:
    for p, d in expected_parms.items():
        parm = sop.parm(p)
        if parm is None:
            bad.append(f"missing {p}")
        elif d is not None and parm.eval() != d:
            bad.append(f"{p}={parm.eval()}!={d}")
gate("G1 node+parms", nt is not None and not bad, "ok" if not bad else bad)

# ---------------- G2: official HDA cooks ----------------
imp = geo.createNode("metahuman::mh_character_import::1.0")
imp.parm("headdnafile").set(f"{CHAR}/head.dna")
imp.parm("bodydnafile").set(f"{CHAR}/body.dna")
t0 = time.time()
imp.cook(force=True)
dt = time.time() - t0
rest, capt = imp.geometry(0), imp.geometry(1)
gate("G2 official mh_character_import cooks", not imp.errors() and len(rest.prims()) > 0 and len(capt.points()) > 0,
     f"{dt:.1f}s errors={imp.errors()} rest_prims={len(rest.prims())} skel_pts={len(capt.points())}")

# ---------------- G3: geometry, capture, skeleton ----------------
polys = [p for p in rest.prims() if p.type() == hou.primType.Polygon]
by_name = {}
for p in polys:
    by_name.setdefault(p.attribValue("name"), []).append(p)
mesh_bad = []
P = rest.pointFloatAttribValues("P")
for part in ("head", "body"):
    for mname, e in EXP[part]["meshes"].items():
        prims = by_name.get(mname, [])
        pts = {v.point().number() for p in prims for v in p.vertices()}
        nv = sum(p.numVertices() for p in prims)
        if (len(prims), len(pts), nv) != (e["faces"], e["points"], e["vertices"]):
            mesh_bad.append(f"{mname}: faces/points/verts {len(prims)}/{len(pts)}/{nv} != "
                            f"{e['faces']}/{e['points']}/{e['vertices']}")
            continue
        base = min(pts)
        for i, cm in e["samples_cm"].items():
            q = P[3 * (base + int(i)):3 * (base + int(i)) + 3]
            err = max(abs(q[k] - cm[k] * 0.01) for k in range(3))
            if err > 1e-6:
                mesh_bad.append(f"{mname} pt{i} err {err:.2e} m")
gate("G3a meshes match DNA (faces/points/vertices/positions)", not mesh_bad,
     f"{sum(len(m['meshes']) for m in EXP.values())} meshes" if not mesh_bad else mesh_bad[:5])

gate("G3b uv + N vertex attributes", rest.findVertexAttrib("uv") is not None and rest.findVertexAttrib("N") is not None,
     f"uv={rest.findVertexAttrib('uv') is not None} N={rest.findVertexAttrib('N') is not None}")

# Skeleton names/parents vs DNA (union) and positions vs Blender import.
skel_names = [pt.attribValue("name") for pt in capt.points()]
union = list(dict.fromkeys(EXP["body"]["joint_names"] + EXP["head"]["joint_names"]))
pos_err = []
for pt in capt.points():
    b = BL_JOINTS.get(pt.attribValue("name")) if BL_JOINTS else None
    if b is not None:
        pos_err.append(max(abs(pt.position()[k] - b[k]) for k in range(3)))
gate("G3c skeleton = head∪body, positions vs Blender import",
     sorted(skel_names) == sorted(union) and (BL_JOINTS is None or (pos_err and max(pos_err) < 1e-4)),
     f"joints {len(skel_names)} (union {len(union)}), " + (f"max |Δ| vs Blender {max(pos_err) * 1000:.4f} mm over {len(pos_err)}"
                                                         if BL_JOINTS else "Blender positions skipped (no expect_blender_joints.json)"))

# Capture weights sum to 1; rest-pose bone deform reproduces the rest mesh.
wsum_bad = 0
ca = rest.findPointAttrib("boneCapture")
wmin = wmax = 0.0
if ca is not None:
    # Unpack the index-pair capture attribute into plain arrays and sum the weights.
    up = geo.createNode("captureattribunpack")
    up.setInput(0, imp, 0)
    up.cook(force=True)
    ug = up.geometry()
    da = ug.findPointAttrib("boneCapture_data")
    mesh_pts = {v.point().number() for p in polys for v in p.vertices()}  # skip packed-prim anchor points
    ws = [sum(pt.attribValue(da)) for pt in ug.points() if pt.number() in mesh_pts]
    wsum_bad = sum(1 for x in ws if abs(x - 1.0) > 1e-3)
    wmin, wmax = min(ws), max(ws)
bd = geo.createNode("bonedeform")
bd.setInput(0, imp, 0)
bd.setInput(1, imp, 1)
bd.setInput(2, imp, 1)
bd.cook(force=True)
D = bd.geometry().pointFloatAttribValues("P")
derr = max(abs(a - b) for a, b in zip(D, P)) if len(D) == len(P) else float("inf")
gate("G3d boneCapture weights sum to 1, rest Bone Deform error < 0.01 mm",
     ca is not None and wsum_bad == 0 and derr < 1e-5,
     f"capture={'yes' if ca is not None else 'no'} bad_wsum={wsum_bad} range=[{wmin:.4f},{wmax:.4f}] "
     f"bonedeform max err {derr * 1000:.5f} mm errors={bd.errors()}" if ca is not None else "no boneCapture")

# ---------------- G4: blend shapes ----------------
packed = [p for p in rest.prims() if p.type() != hou.primType.Polygon]
hidden = rest.findPrimGroup("_3d_hidden_primitives")
bs_bad = []
counts = {}
for p in packed:
    counts[p.attribValue("name")] = counts.get(p.attribValue("name"), 0) + 1
for part in ("head", "body"):
    for mname, e in EXP[part]["meshes"].items():
        if counts.get(mname, 0) != e["targets"]:
            bs_bad.append(f"{mname}: {counts.get(mname, 0)} packed != {e['targets']} targets")
        base = min({v.point().number() for p in by_name.get(mname, []) for v in p.vertices()}, default=0)
        for ts in e["target_samples"]:
            cand = [p for p in packed if p.attribValue("name") == mname
                    and p.attribValue("blendshape_channel") == ts["channel"]]
            if not cand:
                bs_bad.append(f"{mname}.{ts['channel']} missing")
                continue
            sg = cand[0].getEmbeddedGeometry()
            ids = sg.pointIntAttribValues("id")
            if ts["max_vertex"] not in ids:
                bs_bad.append(f"{mname}.{ts['channel']} lacks vertex {ts['max_vertex']}")
                continue
            q = sg.points()[ids.index(ts["max_vertex"])].position()
            b = P[3 * (base + ts["max_vertex"]):3 * (base + ts["max_vertex"]) + 3]
            err = max(abs((q[k] - b[k]) - ts["max_delta_cm"][k] * 0.01) for k in range(3))
            if err > 1e-6 or len(ids) != ts["count"]:
                bs_bad.append(f"{mname}.{ts['channel']} delta err {err:.2e} count {len(ids)}/{ts['count']}")
gate("G4 blend shapes (count per mesh, sparse ids, deltas)",
     not bs_bad and hidden is not None and len(hidden.prims()) == len(packed),
     f"{len(packed)} packed, hidden group {'ok' if hidden is not None else 'missing'}" if not bs_bad else bs_bad[:5])

npass = sum(ok for _, ok, _ in results)
print(f"\nB1 oracle: {npass}/{len(results)} gates pass")
with open(os.path.join(HERE, "..", "SCOREBOARD.md"), "a") as f:
    f.write(f"| {time.strftime('%Y-%m-%d %H:%M')} | B1 | {npass}/{len(results)} | "
            + "; ".join(f"{n.split()[0]}:{'P' if ok else 'F'}" for n, ok, _ in results) + " |\n")
sys.exit(0 if npass == len(results) else 1)
