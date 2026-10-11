# Independent RigLogic reference for body RBF correctives (OpenRigLogic Python bindings).
# A driver joint rotated by an Euler delta (degrees, xyz, in its own frame) feeds RigLogic the quaternion of
# that delta (Houdini/RigLogic share the convention, see tests/quat_convention.py). Records the local
# transforms (absolute, Euler mode) of every joint RigLogic moves away from neutral.
#   <blender python> -I tests/riglogic_body_expect.py <bindings_dir> <body.dna> <out.json>
import json, math, sys
sys.path.insert(0, sys.argv[1]); import dna, riglogic  # noqa: E401,E402

CASES = {"calf_l_back90": ("calf_l", [0, 0, -90]), "upperarm_l_x40": ("upperarm_l", [40, 0, 0]),
         "upperarm_l_mix": ("upperarm_l", [0, -35, 20]), "thigh_r_z50": ("thigh_r", [0, 0, 50])}


def quat_xyz(e):  # rotation applied x, then y, then z (row-vector) -> quaternion, verified == RigLogic/Houdini
    hx, hy, hz = (math.radians(a) / 2 for a in e)
    qx = (math.sin(hx), 0, 0, math.cos(hx)); qy = (0, math.sin(hy), 0, math.cos(hy)); qz = (0, 0, math.sin(hz), math.cos(hz))
    def m(a, b):  # Hamilton product a*b (apply b then a)
        ax, ay, az, aw = a; bx, by, bz, bw = b
        return (aw*bx + ax*bw + ay*bz - az*by, aw*by - ax*bz + ay*bw + az*bx, aw*bz + ax*by - ay*bx + az*bw, aw*bw - ax*bx - ay*by - az*bz)
    return m(qz, m(qy, qx))


s = dna.FileStream(sys.argv[2], dna.FileStream.AccessMode_Read, dna.FileStream.OpenMode_Binary)
r = dna.BinaryStreamReader(s, dna.DataLayer_All); r.read()
cfg = riglogic.Configuration(); cfg.floatingPointType = riglogic.FloatingPointType_Float
cfg.rotationType = riglogic.RotationType_Quaternions  # as UE
rl = riglogic.RigLogic.create(r, cfg); inst = riglogic.RigInstance.create(rl)
names = [r.getJointName(j) for j in range(r.getJointCount())]
raw = {r.getRawControlName(i): i for i in range(r.getRawControlCount())}
neutral = list(rl.getNeutralJointValues())
out = {}
for case, (joint, euler) in CASES.items():
    for i in range(r.getRawControlCount()):
        inst.setRawControl(i, 0.0)
    for drv in {n.split(".")[0] for n in raw}:  # every driver at rest -> identity quaternion
        inst.setRawControl(raw[f"{drv}.qw"], 1.0)
    q = quat_xyz(euler)
    for c, v in zip(("qx", "qy", "qz", "qw"), q):
        inst.setRawControl(raw[f"{joint}.{c}"], v)
    rl.calculate(inst)
    jo = list(inst.getJointOutputs())  # deltas, 10 per joint (t, q, s)
    moved = {}
    def qmul(a, b):
        ax, ay, az, aw = a; bx, by, bz, bw = b
        return [aw*bx + ax*bw + ay*bz - az*by, aw*by - ax*bz + ay*bw + az*bx, aw*bz + ax*by - ay*bx + az*bw, aw*bw - ax*bx - ay*by - az*bz]
    for j, n in enumerate(names):
        d, n0 = jo[10*j:10*j+10], neutral[10*j:10*j+10]
        if max(abs(x) for x in d[0:3] + d[7:10]) > 1e-4 or max(abs(x) for x in d[3:6]) > 1e-5:
            moved[n] = {"t_m": [(n0[k] + d[k]) * 0.01 for k in range(3)], "q": qmul(n0[3:7], d[3:7]),
                        "s": [n0[7 + k] + d[7 + k] for k in range(3)]}
    out[case] = {"driver": joint, "euler": euler, "quat": q, "moved": moved}
    print(case, "quat", [round(x, 4) for x in q], "moved joints", len(moved), list(moved)[:4])
json.dump(out, open(sys.argv[3], "w"))
