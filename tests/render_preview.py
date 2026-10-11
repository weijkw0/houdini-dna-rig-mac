# hython tests/render_preview.py <hip> <pose.bgeo.sc> <out.png>  (one render per process)
import sys, hou
hou.hipFile.load(sys.argv[1], suppress_save_prompt=True, ignore_load_warnings=True)
mh = hou.node("/obj/MetaHuman"); mh.setDisplayFlag(False)
geo = hou.node("/obj").createNode("geo", "preview_pose")
f = geo.createNode("file"); f.parm("file").set(sys.argv[2])
w = geo.createNode("attribwrangle"); w.setInput(0, f); w.parm("class").set(1)
w.parm("snippet").set('s@shop_materialpath = re_replace("^/obj/rigeval/metahuman_character_rig1", "/obj/MetaHuman/metahuman_character_rig", s@shop_materialpath);')
w.setDisplayFlag(True); w.setRenderFlag(True)
cam = hou.node("/obj").createNode("cam", "cam_face")
cam.parmTuple("t").set((0.0, 1.62, 0.62)); cam.parmTuple("r").set((-4, 0, 0))
cam.parm("resx").set(900); cam.parm("resy").set(1100); cam.parm("focal").set(55)
light = hou.node("/obj").createNode("hlight::2.0", "key"); light.parmTuple("t").set((0.6, 2.0, 1.4)); light.parmTuple("r").set((-35, 25, 0))
light.parm("light_intensity").set(2.5) if light.parm("light_intensity") else None
env = hou.node("/obj").createNode("envlight", "env"); env.parm("light_intensity").set(1.2) if env.parm("light_intensity") else None
fill = hou.node("/obj").createNode("hlight::2.0", "fill"); fill.parmTuple("t").set((-1.0, 1.7, 1.2)); fill.parmTuple("r").set((-10, -40, 0))
fill.parm("light_intensity").set(1.2) if fill.parm("light_intensity") else None
rop = hou.node("/out").createNode("opengl", "preview")
rop.parm("camera").set(cam.path()); rop.parm("picture").set(sys.argv[3])
rop.parm("tres").set(True) if rop.parm("tres") else None
rop.parm("res1").set(900) if rop.parm("res1") else None; rop.parm("res2").set(1100) if rop.parm("res2") else None
rop.parm("shadingmode").set("smooth") if rop.parm("shadingmode") else None
rop.parm("bgcolor").set(0.18) if rop.parm("bgcolor") else None
rop.render(); print("rendered", sys.argv[3])
