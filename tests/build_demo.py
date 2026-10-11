# hython tests/build_demo.py <character_dir> <out.hipnc>
import sys, time
import hou
char, out = sys.argv[1], sys.argv[2]
obj = hou.node("/obj")
geo = obj.createNode("geo", "MetaHuman")
rig = geo.createNode("metahuman::metahuman_character_rig::1.2", "metahuman_character_rig")
rig.parm("mhfolder").set(char)
rig.parm("importtextures").set(1)
scene = geo.createNode("apex::sceneaddcharacter", "scene_add_character")
scene.setInput(0, None)
scene.setInput(1, rig) if len(scene.inputConnectors()) > 1 else scene.setInput(0, rig)
anim = geo.createNode("apex::sceneanimate", "scene_animate")
anim.setInput(0, scene)
anim.setDisplayFlag(True); anim.setRenderFlag(True)
geo.layoutChildren()
t = time.time()
for n in (rig, scene, anim):
    try:
        n.cook(force=True)
    except hou.OperationFailed:
        pass
    print(n.name(), "errors", n.errors()[:2], "warnings", n.warnings()[:2])
print(f"cook {time.time()-t:.1f}s; scene_add inputs: {[c.inputName() if hasattr(c,'inputName') else '' for c in scene.inputConnectors()]}")
g = anim.geometry()
print("animate output prims", len(g.prims()) if g else None)
mats = [n for n in rig.allSubChildren() if n.type().name().startswith("mtlximage")]
print("texture files used:", sorted({n.parm("file").evalAsString().split("/")[-1] for n in mats if n.parm("file") and n.parm("file").evalAsString()})[:20])
hou.hipFile.save(out)
print("saved", out)
