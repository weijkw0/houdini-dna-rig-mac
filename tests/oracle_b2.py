# B2 oracle. Run:  HOUDINI_PACKAGE_DIR=build/package hython tests/oracle_b2.py [character_dir]   (default: $MH_CHARACTER)
# Evaluates the official metahuman_character_rig graph (our mh::RigLogic / mh::DrivenKeys callbacks)
# and compares against tests/expect_riglogic.json, produced by Epic's OpenRigLogic Python bindings
# (tests/riglogic_expect.py) - an independent RigLogic build fed through the DNA's own GUI->raw mapping.
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hou  # noqa: E402
from rigeval import RigEval, joint_positions  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
CHAR = sys.argv[1] if len(sys.argv) > 1 else os.environ["MH_CHARACTER"]
EXP = json.load(open(os.path.join(HERE, "expect_riglogic.json")))
results = []


def gate(name, ok, detail):
    results.append((name, bool(ok), detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}")


def parm_name(gui):  # DNA GUI control "CTRL_C_jaw.ty" -> rig graph parameter "CTRL_C_jaw_Y"
    base, attr = gui.rsplit(".", 1)
    return f"{base}_{attr[-1].upper()}"


t0 = time.time()
R = RigEval(CHAR)
build = time.time() - t0
face_parms = [p for p in R.inputs if p.endswith(("_X", "_Y")) and p.startswith("CTRL_")]
gate("G5a rig builds, face controls exposed", len(face_parms) > 100, f"{build:.1f}s, {len(face_parms)} face params")

rest, errs = R.evaluate({p: 0.0 for p in face_parms})
rest_pos = joint_positions(rest["Base.skel"])
exp0 = None
worst = {}
for case, e in EXP["cases"].items():
    ctrls = {p: 0.0 for p in face_parms}
    for g, v in e["controls"].items():
        ctrls[parm_name(g)] = v
    res, errs = R.evaluate(ctrls)
    pos = joint_positions(res["Base.skel"])
    # joints: world positions vs independent RigLogic + forward kinematics
    jerr, moved = 0.0, 0
    for n, ep in e["joint_world_pos"].items():
        if n not in pos:
            continue
        d = max(abs(pos[n][k] - ep[k]) for k in range(3))
        jerr = max(jerr, d)
        if (pos[n] - rest_pos[n]).length() > 1e-4:
            moved += 1
    # blend shapes + animated maps: read the RigLogic node's skel output
    skel_rl = R.graph.getNodeOutputData("riglogic_head", "skel")
    bs_err, bs_n = 0.0, 0
    for ch, w in e["blendshapes"].items():
        a = skel_rl.findGlobalAttrib(ch)
        got = skel_rl.attribValue(a) if a else None
        bs_err = max(bs_err, abs((got or 0.0) - w))
        bs_n += 1
    mo = skel_rl.findGlobalAttrib("material_override")
    md = skel_rl.dictAttribValue("material_override") if mo else {}
    am_err = max([abs(md.get(k, 0.0) - v) for k, v in e["animmaps"].items()] or [0.0])
    worst[case] = (jerr, moved, bs_err, bs_n, am_err, len(e["animmaps"]), errs[:1])
    print(f"   {case}: joints max err {jerr * 1000:.4f} mm ({moved} moved), blendshape max err {bs_err:.2e} "
          f"over {bs_n}, animmap max err {am_err:.2e} over {len(e['animmaps'])}, errors={errs[:1]}")

gate("G5b face joints match independent RigLogic (< 0.05 mm)",
     all(v[0] < 5e-5 and v[1] > 0 for v in worst.values()),
     {k: f"{v[0] * 1000:.4f}mm/{v[1]}moved" for k, v in worst.items()})
gate("G5c blend shape weights + animated maps match (< 1e-4)",
     all(v[2] < 1e-4 and v[4] < 1e-4 for v in worst.values()),
     {k: f"bs {v[2]:.1e}/{v[3]} am {v[4]:.1e}/{v[5]}" for k, v in worst.items()})


# ---------------- body RBF correctives ----------------
BODY = json.load(open(os.path.join(HERE, "expect_riglogic_body.json")))


def world_mats(skel):
    out, parent = {}, {}
    nm = skel.findPointAttrib("name")
    for pt in skel.points():
        m = hou.Matrix4(hou.Matrix3(pt.attribValue("transform")))
        p = pt.position()
        m.setAt(3, 0, p[0]); m.setAt(3, 1, p[1]); m.setAt(3, 2, p[2])
        out[pt.attribValue(nm)] = m
    for pr in skel.prims():
        vs = pr.vertices()
        if len(vs) == 2:
            parent[vs[1].point().attribValue(nm)] = vs[0].point().attribValue(nm)
    return out, parent


def local(W, parent, n):
    return W[n] * W[parent[n]].inverted() if n in parent else W[n]


body_r = [p for p, t in R.inputs.items() if p.endswith("_r") and t == "Vector3" and not p.startswith("CTRL_")]
zero = {p: 0.0 for p in face_parms}
zero.update({p: hou.Vector3(0, 0, 0) for p in body_r})
rest_b, _ = R.evaluate(zero)
W0, PAR = world_mats(rest_b["Base.skel"])
# rest rig == capture pose from the importer (correctives sit at neutral when nothing is posed)
imp = R.geo.createNode("metahuman::mh_character_import::1.0")
imp.parm("headdnafile").set(f"{CHAR}/head.dna"); imp.parm("bodydnafile").set(f"{CHAR}/body.dna")
cap = joint_positions(imp.geometry(1))
rest_err = max((W0[n].extractTranslates() - cap[n]).length() for n in W0 if n in cap)
gate("G5d rig at rest == capture pose", rest_err < 1e-5, f"max {rest_err * 1000:.5f} mm over {len(W0)} joints")

bad, summary = [], {}
for case, e in BODY.items():
    ctrls = dict(zero)
    ctrls[f"{e['driver']}_r"] = hou.Vector3(e["euler"])
    res, errs = R.evaluate(ctrls)
    W, _ = world_mats(res["Base.skel"])
    # the FK control must rotate the driver by exactly this delta in its own frame
    D = local(W, PAR, e["driver"]) * local(W0, PAR, e["driver"]).inverted()
    Dexp = hou.hmath.buildRotate(hou.Vector3(e["euler"]), "xyz")
    derr = max(abs(D.at(i, j) - Dexp.at(i, j)) for i in range(3) for j in range(3))
    terr = rerr = 0.0
    for n, x in e["moved"].items():
        L = local(W, PAR, n)
        terr = max(terr, max(abs(L.extractTranslates()[k] - x["t_m"][k]) for k in range(3)))
        # full local 3x3 (scale then rotation, row-vector) vs RigLogic's s/r outputs
        Mexp = hou.hmath.buildScale(hou.Vector3(x["s"])) * hou.Matrix4(hou.Quaternion(x["q"]).extractRotationMatrix3())
        rerr = max(rerr, max(abs(L.at(i, j) - Mexp.at(i, j)) for i in range(3) for j in range(3)))
    summary[case] = f"drv {derr:.1e} t {terr * 1000:.4f}mm r {rerr:.1e} ({len(e['moved'])} joints)"
    if derr > 1e-4 or terr > 5e-5 or rerr > 1e-4:
        bad.append(case)
    print(f"   {case}: {summary[case]} errors={errs[:1]}")
gate("G5e body RBF correctives match independent RigLogic", not bad, summary)

npass = sum(ok for _, ok, _ in results)
print(f"\nB2 oracle: {npass}/{len(results)} gates pass")
with open(os.path.join(HERE, "..", "SCOREBOARD.md"), "a") as f:
    f.write(f"| {time.strftime('%Y-%m-%d %H:%M')} | B2 | {npass}/{len(results)} | "
            + "; ".join(f"{n.split()[0]}:{'P' if ok else 'F'}" for n, ok, _ in results) + " |\n")
sys.exit(0 if npass == len(results) else 1)
