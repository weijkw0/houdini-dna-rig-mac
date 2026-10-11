# Evaluate the official MetaHuman character rig (with our callbacks) in hython via the APEX Python API.
import hou
import apex


class RigEval:
    def __init__(self, char_dir, maxlod=0):
        self.geo = hou.node("/obj").createNode("geo", "rigeval")
        self.rig = self.geo.createNode("metahuman::metahuman_character_rig::1.2")
        self.rig.parm("mhfolder").set(char_dir)
        if maxlod:
            self.rig.parm("maxlod").set(maxlod)
        self.folder = {p.attribValue("name"): p.getEmbeddedGeometry() for p in self.rig.geometry().prims()}
        self.graph = apex.Graph()
        self.graph.loadFromGeometry(self.folder["Base.rig"])
        self.inputs = {self.graph.portName(p): self.graph.portTypeName(p) for p in self.graph.inputPorts()}

    def evaluate(self, ctrls, outputs=("Base.skel", "Base.shp")):
        G = self.graph
        for k, v in self.folder.items():
            if self.inputs.get(k) == "Geometry":
                G.setParm(k, v)
        for k, v in ctrls.items():
            G.setParm(k, v)
        ports = [G.findGraphOutputPort(o) for o in outputs]
        res = G.evaluateOutputs(ports)
        return dict(zip(outputs, res)), list(G.executionErrors())


def joint_positions(skel):
    return {pt.attribValue("name"): pt.position() for pt in skel.points()}
