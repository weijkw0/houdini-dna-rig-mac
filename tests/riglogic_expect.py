# Independent RigLogic reference for oracle_b2: Epic's OpenRigLogic Python bindings (macOS build bundled
# with Blender's Character DNA add-on) evaluate the head DNA for a few face-board poses.
#   <blender python> -I tests/riglogic_expect.py <bindings_dir> <head.dna> <out.json>
import json
import math
import sys

sys.path.insert(0, sys.argv[1])
import dna  # noqa: E402
import riglogic  # noqa: E402

CASES = {
    "jaw_open": {"CTRL_C_jaw.ty": 1.0},
    "smile_L": {"CTRL_L_mouth_cornerPull.ty": 1.0},
    "blink_R": {"CTRL_R_eye_blink.ty": 1.0},
    "brow_mix": {"CTRL_L_brow_down.ty": 0.6, "CTRL_R_brow_raiseOut.ty": 0.8},
    "jaw_side": {"CTRL_C_jaw.tx": -0.5, "CTRL_C_jaw.ty": 0.3},
}

s = dna.FileStream(sys.argv[2], dna.FileStream.AccessMode_Read, dna.FileStream.OpenMode_Binary)
r = dna.BinaryStreamReader(s, dna.DataLayer_All)
r.read()
cfg = riglogic.Configuration()
cfg.floatingPointType = riglogic.FloatingPointType_Float
cfg.rotationType = riglogic.RotationType_Quaternions  # as UE's AnimNode_RigLogic
rl = riglogic.RigLogic.create(r, cfg)
inst = riglogic.RigInstance.create(rl)
gui = {r.getGUIControlName(i): i for i in range(r.getGUIControlCount())}
names = [r.getJointName(j) for j in range(r.getJointCount())]
parents = [r.getJointParentIndex(j) for j in range(r.getJointCount())]


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def qmul(a, b):  # Hamilton product a*b (UE FQuat: applies b first, then a)
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return (aw*bx + ax*bw + ay*bz - az*by, aw*by - ax*bz + ay*bw + az*bx, aw*bz + ax*by - ay*bx + az*bw, aw*bw - ax*bx - ay*by - az*bz)


def qrow(q):  # row-vector rotation matrix of quaternion q (x, y, z, w)
    x, y, z, w = q
    return [[1 - 2*(y*y + z*z), 2*(x*y + z*w), 2*(x*z - y*w)],
            [2*(x*y - z*w), 1 - 2*(x*x + z*z), 2*(y*z + x*w)],
            [2*(x*z + y*w), 2*(y*z - x*w), 1 - 2*(x*x + y*y)]]


def local(n, d):  # UE AnimNode_RigLogic: T = Nt + Dt, R = Nq * Dq, S = Ns + Ds; cm -> m
    t = [(n[k] + d[k]) * 0.01 for k in range(3)]
    r = qrow(qmul(n[3:7], d[3:7]))
    sc = [n[7 + k] + d[7 + k] for k in range(3)]
    m = [[sc[i] * r[i][j] for j in range(3)] + [0] for i in range(3)]
    return m + [t + [1]]


neutral = list(rl.getNeutralJointValues())
out = {"gui_controls": list(gui), "cases": {}}
for case, ctrls in CASES.items():
    for i in range(r.getGUIControlCount()):
        inst.setGUIControl(i, 0.0)
    for k, v in ctrls.items():
        inst.setGUIControl(gui[k], v)
    rl.mapGUIToRawControls(inst)
    rl.calculate(inst)
    jo = list(inst.getJointOutputs())  # deltas, 10 per joint
    world = [None] * len(names)
    for j in range(len(names)):
        m = local(neutral[10 * j:10 * j + 10], jo[10 * j:10 * j + 10])
        world[j] = m if parents[j] == j else mul(m, world[parents[j]])
    bs = list(inst.getBlendShapeOutputs())
    am = list(inst.getAnimatedMapOutputs())
    out["cases"][case] = {
        "controls": ctrls,
        "joint_world_pos": {names[j]: world[j][3][:3] for j in range(len(names))},
        "blendshapes": {r.getBlendShapeChannelName(c): bs[c] for c in range(len(bs)) if abs(bs[c]) > 1e-6},
        "animmaps": {r.getAnimatedMapName(a).replace(".", "__"): am[a] for a in range(len(am)) if abs(am[a]) > 1e-6},
        "raw_nonzero": {r.getRawControlName(i): inst.getRawControl(i) for i in range(r.getRawControlCount())
                        if abs(inst.getRawControl(i)) > 1e-6},
    }
    print(case, "raw", out["cases"][case]["raw_nonzero"], "bs", len(out["cases"][case]["blendshapes"]))
json.dump(out, open(sys.argv[3], "w"))
