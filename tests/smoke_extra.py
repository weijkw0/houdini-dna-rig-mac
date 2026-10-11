# hython tests/smoke_extra.py   Extra coverage: $MH_CHARACTER at LOD1 and with units multiplier 100,
# plus a second DCC export ($MH_CHARACTER2, optional) at LOD0.
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hou
from rigeval import RigEval, joint_positions
char1, char2 = os.environ["MH_CHARACTER"], os.environ.get("MH_CHARACTER2")
cases = ([("char2 lod0", char2, 0, 1.0)] if char2 else []) + [("char1 lod1", char1, 1, 1.0), ("char1 units100", char1, 0, 100.0)]
ok_all = True
for label, char, lod, units in cases:
    t = time.time()
    R = RigEval(char, maxlod=lod)
    if units != 1.0:
        R.rig.parm("unitsmultiplier").set(units)
        R.folder = {p.attribValue("name"): p.getEmbeddedGeometry() for p in R.rig.geometry().prims()}
        import apex
        R.graph = apex.Graph(); R.graph.loadFromGeometry(R.folder["Base.rig"])
        R.inputs = {R.graph.portName(p): R.graph.portTypeName(p) for p in R.graph.inputPorts()}
    errs = [n.path() for n in R.rig.allSubChildren() if n.errors()]
    face = [p for p in R.inputs if p.startswith("CTRL_") and p.endswith(("_X", "_Y"))]
    r0, e0 = R.evaluate({p: 0.0 for p in face})
    c = {p: 0.0 for p in face}; c["CTRL_C_jaw_Y"] = 1.0
    r1, e1 = R.evaluate(c)
    p0, p1 = joint_positions(r0["Base.skel"]), joint_positions(r1["Base.skel"])
    lip = (p1["FACIAL_C_LipLower"] - p0["FACIAL_C_LipLower"]).length()
    head = p0["head"][1]
    names = sorted({p.attribValue("name") for p in r0["Base.shp"].prims() if p.type() == hou.primType.Polygon})
    ok = not errs and not e0 and not e1 and lip > 0
    ok_all &= ok
    print(f"[{'OK' if ok else 'BAD'}] {label}: build+2 evals {time.time()-t:.1f}s, node errors {errs[:2]}, eval errors {e0[:1]+e1[:1]}, "
          f"jaw-open lip move {lip:.4f} (scene units), head height {head:.3f}, meshes {names[:3]}... ({len(names)})")
sys.exit(0 if ok_all else 1)
