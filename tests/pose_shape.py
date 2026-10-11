# hython tests/pose_shape.py <char> <out.bgeo.sc> : evaluate Base.shp with a face expression (APEX rig + our callbacks)
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hou
from rigeval import RigEval
R = RigEval(sys.argv[1])
R.rig.parm("importtextures").set(1)
R.folder = {p.attribValue("name"): p.getEmbeddedGeometry() for p in R.rig.geometry().prims()}
res, errs = R.evaluate({"CTRL_C_jaw_Y": 0.35, "CTRL_L_mouth_cornerPull_Y": 1.0, "CTRL_R_mouth_cornerPull_Y": 1.0,
                        "CTRL_L_brow_raiseOut_Y": 0.6, "CTRL_R_brow_raiseOut_Y": 0.6}, outputs=("Base.shp",))
g = res["Base.shp"]
print("errors", errs, "prims", len(g.prims()), "mat attr", g.findPrimAttrib("shop_materialpath") is not None,
      sorted({p.attribValue("shop_materialpath") for p in g.prims()})[:6] if g.findPrimAttrib("shop_materialpath") else "")
g.saveToFile(sys.argv[2]); print("saved")
