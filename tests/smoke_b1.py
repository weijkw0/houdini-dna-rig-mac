# hython tests/smoke_b1.py <character_dir>
# Quick look: cook our mhdna_import SOP and the official mh_character_import HDA, print stats.
import sys
import time

import hou

char = sys.argv[1]
head, body = f"{char}/head.dna", f"{char}/body.dna"

print("mhdna_import type:", hou.sopNodeTypeCategory().nodeTypes().get("mhdna_import"))
geo = hou.node("/obj").createNode("geo", "mh")

sop = geo.createNode("mhdna_import")
sop.parm("headdnafile").set(head)
sop.parm("bodydnafile").set(body)
t = time.time()
sop.cook(force=True)
print(f"mhdna_import cook {time.time() - t:.1f}s errors={sop.errors()} warnings={sop.warnings()}")
g = sop.geometry()
print("points", len(g.points()), "prims", len(g.prims()))
names = {}
for p in g.prims():
    if p.type() == hou.primType.Polygon:
        names[p.attribValue("name")] = names.get(p.attribValue("name"), 0) + 1
print("meshes:", names)
print("packed blendshapes:", sum(1 for p in g.prims() if p.type() != hou.primType.Polygon))
for a in g.globalAttribs():
    v = g.attribValue(a)
    print(f"  detail {a.name()}: {v if not isinstance(v, (tuple, list)) else (len(v), v[:4])}")

imp = geo.createNode("metahuman::mh_character_import::1.0")
imp.parm("headdnafile").set(head)
imp.parm("bodydnafile").set(body)
t = time.time()
imp.cook(force=True)
print(f"mh_character_import cook {time.time() - t:.1f}s errors={imp.errors()}")
for i in range(2):
    og = imp.geometry(i)
    print(f"  output {i}: points={len(og.points())} prims={len(og.prims())}")
