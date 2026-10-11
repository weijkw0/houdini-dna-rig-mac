# Which quaternion convention does RigLogic use for a DNA Euler rotation? Evaluate one face pose with
# Euler outputs and with quaternion outputs (RigLogic's own conversion) and dump both for a few joints.
import sys, json
sys.path.insert(0, sys.argv[1]); import dna, riglogic
s = dna.FileStream(sys.argv[2], dna.FileStream.AccessMode_Read, dna.FileStream.OpenMode_Binary)
r = dna.BinaryStreamReader(s, dna.DataLayer_All); r.read()
names = [r.getJointName(i) for i in range(r.getJointCount())]
gui = {r.getGUIControlName(i): i for i in range(r.getGUIControlCount())}
outs = {}
for mode in ("RotationType_EulerAngles", "RotationType_Quaternions"):
    cfg = riglogic.Configuration(); cfg.rotationType = getattr(riglogic, mode)
    rl = riglogic.RigLogic.create(r, cfg); inst = riglogic.RigInstance.create(rl)
    inst.setGUIControl(gui["CTRL_C_jaw.ty"], 1.0); inst.setGUIControl(gui["CTRL_C_jaw.tx"], 0.6)
    rl.mapGUIToRawControls(inst); rl.calculate(inst)
    o = list(inst.getJointOutputs()); st = len(o) // len(names)
    outs[mode] = {n: o[st * j:st * j + st] for j, n in enumerate(names)}
res = []
for n in names:
    e = outs["RotationType_EulerAngles"][n][3:6]
    if max(abs(x) for x in e) > 2.0:
        res.append({"joint": n, "euler_delta": e, "quat": outs["RotationType_Quaternions"][n][3:7]})
json.dump(res[:8], open(sys.argv[3], "w")); print(len(res), res[:2])
