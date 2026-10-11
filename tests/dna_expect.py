# Independent DNA facts for the oracle, read with Epic's OpenRigLogic Python bindings
# (the macOS build bundled with Blender's Character DNA add-on), NOT with our C++ code.
#   <blender python> -I tests/dna_expect.py <bindings_dir> <character_dir> <out.json>
import json
import sys

sys.path.insert(0, sys.argv[1])
import dna  # noqa: E402

char, out = sys.argv[2], sys.argv[3]


def read(path):
    s = dna.FileStream(path, dna.FileStream.AccessMode_Read, dna.FileStream.OpenMode_Binary)
    r = dna.BinaryStreamReader(s, dna.DataLayer_All)
    r.read()
    if not dna.Status.isOk():
        raise SystemExit(dna.Status.get().message)
    return r, s


res = {}
for part in ("head", "body"):
    r, _s = read(f"{char}/{part}.dna")
    meshes = {}
    for mi in r.getMeshIndicesForLOD(0):
        name = r.getMeshName(mi)
        nf = r.getFaceCount(mi)
        nvtx = sum(len(r.getFaceVertexLayoutIndices(mi, f)) for f in range(nf))
        xs, ys, zs = r.getVertexPositionXs(mi), r.getVertexPositionYs(mi), r.getVertexPositionZs(mi)
        n = len(xs)
        samples = {i: [xs[i], ys[i], zs[i]] for i in sorted({0, n // 3, n // 2, n - 1})}
        targets = []
        nt = r.getBlendShapeTargetCount(mi)
        for t in sorted({0, nt // 2, nt - 1}) if nt else []:
            vi = list(r.getBlendShapeTargetVertexIndices(mi, t))
            dx, dy, dz = (list(f(mi, t)) for f in (r.getBlendShapeTargetDeltaXs, r.getBlendShapeTargetDeltaYs,
                                                   r.getBlendShapeTargetDeltaZs))
            k = max(range(len(vi)), key=lambda i: dx[i] ** 2 + dy[i] ** 2 + dz[i] ** 2) if vi else 0
            targets.append({"target": t, "channel": r.getBlendShapeChannelName(r.getBlendShapeChannelIndex(mi, t)),
                            "count": len(vi), "max_vertex": vi[k] if vi else -1,
                            "max_delta_cm": [dx[k], dy[k], dz[k]] if vi else None})
        meshes[name] = {"points": n, "faces": nf, "vertices": nvtx, "targets": nt, "samples_cm": samples,
                        "target_samples": targets, "max_influence": r.getMaximumInfluencePerVertex(mi)}
    res[part] = {"joints": r.getJointCount(), "joint_names": [r.getJointName(j) for j in range(r.getJointCount())],
                 "raw_controls": [r.getRawControlName(i) for i in range(r.getRawControlCount())],
                 "meshes": meshes}
json.dump(res, open(out, "w"), indent=1)
print("wrote", out, {p: (res[p]["joints"], len(res[p]["meshes"])) for p in res})
