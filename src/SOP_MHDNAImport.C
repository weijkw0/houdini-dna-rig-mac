// macOS reimplementation of Epic's MetaHuman for Houdini "mhdna_import" SOP.
// Interface (type name, parameters, output attributes) follows the official
// Linux build (metahuman::SOP_MHDNAImport, DNA_import.C) so that the official
// HDAs (mh_character_import, metahuman_character_rig) can use it unchanged.
// See docs/INTERFACE.md for where each name comes from.

#include <SOP/SOP_Node.h>
#include <OP/OP_AutoLockInputs.h>
#include <OP/OP_Operator.h>
#include <OP/OP_OperatorTable.h>
#include <PRM/PRM_TemplateBuilder.h>
#include <UT/UT_DSOVersion.h>
#include <UT/UT_StringHolder.h>
#include <UT/UT_Matrix4.h>
#include <UT/UT_FileUtil.h>
#include <GU/GU_Detail.h>
#include <GU/GU_PackedGeometry.h>
#include <GU/GU_PrimPacked.h>
#include <GEO/GEO_PrimPoly.h>
#include <GEO/GEO_AttributeCaptureRegion.h>
#include <GEO/GEO_AttributeHandle.h>
#include <GEO/GEO_AttributeIndexPairs.h>
#include <GA/GA_AIFIndexPair.h>
#include <GA/GA_Handle.h>
#include <GA/GA_PolyCounts.h>
#include <GA/GA_Names.h>

#include <dna/BinaryStreamReader.h>
#include <trio/streams/FileStream.h>

#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace UT::Literal;

namespace metahuman {

static const char *theDsFile = R"THEDSFILE(
{
    name        parameters
    parm {
        name    "headdnafile"
        cppname "HeadDNAFile"
        label   "Head DNA File"
        type    file
        default { "" }
        parmtag { "filechooser_pattern" "*.dna" }
        parmtag { "filechooser_mode" "read" }
    }
    parm {
        name    "bodydnafile"
        cppname "BodyDNAFile"
        label   "Body DNA File"
        type    file
        default { "" }
        parmtag { "filechooser_pattern" "*.dna" }
        parmtag { "filechooser_mode" "read" }
    }
    parm {
        name    "maxlod"
        cppname "MaxLOD"
        label   "Max LOD"
        type    integer
        default { "0" }
        range   { 0! 7 }
    }
    parm {
        name    "minlod"
        cppname "MinLOD"
        label   "Min LOD"
        type    integer
        default { "0" }
        range   { 0! 7 }
    }
    parm {
        name    "addmeshes"
        cppname "AddMeshes"
        label   "Add Meshes"
        type    toggle
        default { "on" }
    }
    parm {
        name    "addjoints"
        cppname "AddJoints"
        label   "Add Joints"
        type    toggle
        default { "on" }
    }
    parm {
        name    "addskinning"
        cppname "AddSkinning"
        label   "Add Skinning"
        type    toggle
        default { "on" }
    }
    parm {
        name    "addblendshapes"
        cppname "AddBlendShapes"
        label   "Add Blend Shapes"
        type    toggle
        default { "on" }
    }
    parm {
        name    "addriglogic"
        cppname "AddRigLogic"
        label   "Add Rig Logic"
        type    toggle
        default { "on" }
    }
    parm {
        name    "unitsmultiplier"
        cppname "UnitsMultiplier"
        label   "Units Multiplier"
        type    float
        default { "1.0" }
    }
    parm {
        name    "reload"
        label   "Reload"
        type    button
    }
}
)THEDSFILE";

namespace {

struct ReaderDeleter {
    void operator()(dna::BinaryStreamReader *r) const { dna::BinaryStreamReader::destroy(r); }
};
struct StreamDeleter {
    void operator()(trio::FileStream *s) const { trio::FileStream::destroy(s); }
};

// A loaded DNA file plus the file stream it reads from.
struct DnaFile {
    std::unique_ptr<trio::FileStream, StreamDeleter> stream;
    std::unique_ptr<dna::BinaryStreamReader, ReaderDeleter> reader;
    std::string path;
    int maxLod = -1, minLod = -1;
    time_t mtime = 0;
};

// One joint of the combined (body + head) skeleton.
struct Joint {
    UT_StringHolder name;
    int parent = -1;
    UT_Matrix4D world;
};

std::string toStd(dna::StringView v) { return std::string(v.data(), v.size()); }

const char *dirName(dna::Direction d) {
    switch (d) {
        case dna::Direction::left: return "left";
        case dna::Direction::right: return "right";
        case dna::Direction::up: return "up";
        case dna::Direction::down: return "down";
        case dna::Direction::front: return "front";
        case dna::Direction::back: return "back";
    }
    return "unknown";
}

UT_Vector3D dirVec(dna::Direction d) {
    switch (d) {
        case dna::Direction::left: return {1, 0, 0};  // character's left = Houdini +X (character faces +Z)
        case dna::Direction::right: return {-1, 0, 0};
        case dna::Direction::up: return {0, 1, 0};
        case dna::Direction::down: return {0, -1, 0};
        case dna::Direction::front: return {0, 0, 1};
        case dna::Direction::back: return {0, 0, -1};
    }
    return {0, 0, 0};
}

const char *rotSeqName(dna::RotationSequence s) {
    static const char *names[] = {"xyz", "xzy", "yxz", "yzx", "zxy", "zyx"};
    const int i = static_cast<int>(s);
    return (i >= 0 && i < 6) ? names[i] : "unknown";
}

// Local transform from DNA neutral translation/rotation (row-vector convention:
// the first axis of the rotation sequence is applied first, then translation).
UT_Matrix4D localXform(const dna::Vector3 &t, const dna::Vector3 &r, dna::RotationSequence seq, bool degrees,
                       double scale) {
    const double k = degrees ? M_PI / 180.0 : 1.0;
    const double ang[3] = {r.x * k, r.y * k, r.z * k};
    static const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    const int si = static_cast<int>(seq);
    const int *ord = orders[(si >= 0 && si < 6) ? si : 0];
    UT_Matrix4D m(1.0);
    for (int a = 0; a < 3; ++a) {
        const int axis = ord[a];
        const UT_Axis3::axis ax = axis == 0 ? UT_Axis3::XAXIS : (axis == 1 ? UT_Axis3::YAXIS : UT_Axis3::ZAXIS);
        m.rotate(ax, ang[axis]);
    }
    m.translate(t.x * scale, t.y * scale, t.z * scale);
    return m;
}

}  // namespace

class SOP_MHDNAImport : public SOP_Node {
public:
    static OP_Node *myConstructor(OP_Network *net, const char *name, OP_Operator *op) {
        return new SOP_MHDNAImport(net, name, op);
    }
    static PRM_Template *buildTemplates();
    static const UT_StringHolder theSOPTypeName;

protected:
    SOP_MHDNAImport(OP_Network *net, const char *name, OP_Operator *op) : SOP_Node(net, name, op) {
        mySopFlags.setManagesDataIDs(true);
    }
    ~SOP_MHDNAImport() override = default;
    OP_ERROR cookMySop(OP_Context &context) override;

private:
    static int reloadCallback(void *data, int index, fpreal t, const PRM_Template *tplate);

    bool loadDna(DnaFile &f, const UT_String &path, int maxLod, int minLod);
    void buildJoints();
    void createDetailMetadata(GU_Detail *gdp);
    void createJointAttributes(GU_Detail *gdp);
    void createRLAttributes(GU_Detail *gdp, const char *suffix, const DnaFile &f);
    void createGeometry(GU_Detail *gdp, const DnaFile &f, bool isHead, bool skinning, bool blendshapes);

    DnaFile myHead, myBody;
    std::vector<Joint> myJoints;
    std::unordered_map<std::string, int> myJointIndex;
    std::vector<int> myHeadJointMap, myBodyJointMap;  // DNA joint index -> combined index
    double myScale = 0.01;
    int myMaxLod = 0, myMinLod = 0;
    GA_RWAttributeRef myCapture;
};

const UT_StringHolder SOP_MHDNAImport::theSOPTypeName("mhdna_import"_sh);

PRM_Template *SOP_MHDNAImport::buildTemplates() {
    static PRM_TemplateBuilder templ("DNA_import.C"_sh, theDsFile);
    if (templ.justBuilt())
        templ.setCallback("reload"_sh, &SOP_MHDNAImport::reloadCallback);
    return templ.templates();
}

int SOP_MHDNAImport::reloadCallback(void *data, int, fpreal, const PRM_Template *) {
    auto *sop = static_cast<SOP_MHDNAImport *>(data);
    sop->myHead = DnaFile();
    sop->myBody = DnaFile();
    sop->forceRecook();
    return 1;
}

bool SOP_MHDNAImport::loadDna(DnaFile &f, const UT_String &path, int maxLod, int minLod) {
    const time_t mtime = UT_FileUtil::getFileModTime(path.c_str());
    if (f.reader && f.path == path.toStdString() && f.maxLod == maxLod && f.minLod == minLod && f.mtime == mtime)
        return true;
    f = DnaFile();
    f.stream.reset(trio::FileStream::create(path.c_str(), trio::AccessMode::Read, trio::OpenMode::Binary));
    dna::Configuration cfg;
    cfg.layer = dna::DataLayer::All;
    cfg.maxLOD = static_cast<std::uint16_t>(maxLod);
    cfg.minLOD = static_cast<std::uint16_t>(minLod);
    f.reader.reset(dna::BinaryStreamReader::create(f.stream.get(), cfg));
    f.reader->read();
    if (!dna::Status::isOk()) {
        UT_WorkBuffer msg;
        msg.sprintf("Failed to read DNA file %s: %s", path.c_str(), dna::Status::get().message);
        addError(SOP_MESSAGE, msg.buffer());
        f = DnaFile();
        return false;
    }
    f.path = path.toStdString();
    f.maxLod = maxLod;
    f.minLod = minLod;
    f.mtime = mtime;
    return true;
}

void SOP_MHDNAImport::buildJoints() {
    myJoints.clear();
    myJointIndex.clear();
    myHeadJointMap.clear();
    myBodyJointMap.clear();
    // Body joints first, then head joints that the body does not already have.
    auto add = [this](const DnaFile &f, std::vector<int> &map) {
        if (!f.reader)
            return;
        const auto *r = f.reader.get();
        const bool degrees = r->getRotationUnit() == dna::RotationUnit::degrees;
        const auto seq = r->getRotationSequence();
        const std::uint16_t n = r->getJointCount();
        map.assign(n, -1);
        for (std::uint16_t j = 0; j < n; ++j) {
            const std::string name = toStd(r->getJointName(j));
            const std::uint16_t p = r->getJointParentIndex(j);
            const UT_Matrix4D local =
                localXform(r->getNeutralJointTranslation(j), r->getNeutralJointRotation(j), seq, degrees, myScale);
            UT_Matrix4D world = local;
            if (p != j && map[p] >= 0)
                world = local * myJoints[map[p]].world;
            auto it = myJointIndex.find(name);
            if (it != myJointIndex.end()) {
                map[j] = it->second;  // shared joint: keep the first (body) definition
                continue;
            }
            Joint jt;
            jt.name = UT_StringHolder(name);
            jt.parent = (p != j) ? map[p] : -1;
            jt.world = world;
            map[j] = static_cast<int>(myJoints.size());
            myJointIndex.emplace(name, map[j]);
            myJoints.push_back(jt);
        }
    };
    add(myBody, myBodyJointMap);
    add(myHead, myHeadJointMap);
}

void SOP_MHDNAImport::createDetailMetadata(GU_Detail *gdp) {
    const DnaFile &f = myHead.reader ? myHead : myBody;
    const auto *r = f.reader.get();

    std::string charName = toStd(r->getName());
    if (charName.empty() || charName == "Archetype") {
        // Fall back to the DCC export folder name (.../<Character>/head.dna).
        const std::string &p = f.path;
        const std::size_t slash = p.find_last_of('/');
        if (slash != std::string::npos && slash > 0) {
            const std::size_t prev = p.find_last_of('/', slash - 1);
            charName = p.substr(prev == std::string::npos ? 0 : prev + 1, slash - (prev == std::string::npos ? 0 : prev + 1));
        }
    }
    GA_RWHandleS(gdp->addStringTuple(GA_ATTRIB_DETAIL, "character_name", 1)).set(GA_DETAIL_OFFSET, UT_StringHolder(charName));

    GA_RWHandleT<UT_ValArray<int>> lods(gdp->addIntArray(GA_ATTRIB_DETAIL, "max_min_lod"));
    UT_ValArray<int> lodv;
    lodv.append(myMaxLod);
    lodv.append(myMinLod);
    lods.set(GA_DETAIL_OFFSET, lodv);

    GA_RWHandleF(gdp->addFloatTuple(GA_ATTRIB_DETAIL, "units_multiplier", 1))
        .set(GA_DETAIL_OFFSET, static_cast<float>(myScale / 0.01));

    const auto cs = r->getCoordinateSystem();
    UT_WorkBuffer buf;
    buf.sprintf("%s %s %s", dirName(cs.x), dirName(cs.y), dirName(cs.z));
    GA_RWHandleS(gdp->addStringTuple(GA_ATTRIB_DETAIL, "dna_coord_system", 1)).set(GA_DETAIL_OFFSET, buf.buffer());
    const UT_Vector3D x = dirVec(cs.x), y = dirVec(cs.y), z = dirVec(cs.z);
    const double hand = dot(cross(x, y), z);
    GA_RWHandleS(gdp->addStringTuple(GA_ATTRIB_DETAIL, "dna_handedness", 1))
        .set(GA_DETAIL_OFFSET, hand >= 0 ? "right" : "left");
    GA_RWHandleS(gdp->addStringTuple(GA_ATTRIB_DETAIL, "dna_rotation_sequence", 1))
        .set(GA_DETAIL_OFFSET, rotSeqName(r->getRotationSequence()));
    const auto sign = r->getRotationSign();
    auto sgn = [](dna::RotationDirection d) { return d == dna::RotationDirection::positive ? "+" : "-"; };
    buf.sprintf("%s%s%s", sgn(sign.x), sgn(sign.y), sgn(sign.z));
    GA_RWHandleS(gdp->addStringTuple(GA_ATTRIB_DETAIL, "dna_rotation_sign", 1)).set(GA_DETAIL_OFFSET, buf.buffer());
    GA_RWHandleS(gdp->addStringTuple(GA_ATTRIB_DETAIL, "dna_face_winding", 1))
        .set(GA_DETAIL_OFFSET, r->getFaceWindingOrder() == dna::FaceWindingOrder::ccw ? "ccw" : "cw");
    GA_RWHandleS(gdp->addStringTuple(GA_ATTRIB_DETAIL, "dna_translation_unit", 1))
        .set(GA_DETAIL_OFFSET, r->getTranslationUnit() == dna::TranslationUnit::cm ? "cm" : "m");
    GA_RWHandleS(gdp->addStringTuple(GA_ATTRIB_DETAIL, "dna_rotation_unit", 1))
        .set(GA_DETAIL_OFFSET, r->getRotationUnit() == dna::RotationUnit::degrees ? "degrees" : "radians");
}

void SOP_MHDNAImport::createJointAttributes(GU_Detail *gdp) {
    GA_RWHandleT<UT_StringArray> names(gdp->addStringArray(GA_ATTRIB_DETAIL, "joint_names"));
    GA_RWHandleT<UT_ValArray<int>> parents(gdp->addIntArray(GA_ATTRIB_DETAIL, "joint_parents"));
    GA_Attribute *xa = gdp->addFloatArray(GA_ATTRIB_DETAIL, "joint_xforms", 16);
    xa->setTypeInfo(GA_TYPE_TRANSFORM);
    GA_RWHandleT<UT_ValArray<fpreal32>> xforms(xa);

    UT_StringArray nv;
    UT_ValArray<int> pv;
    UT_ValArray<fpreal32> xv;
    nv.setCapacity(myJoints.size());
    xv.setCapacity(myJoints.size() * 16);
    for (const Joint &j : myJoints) {
        nv.append(j.name);
        pv.append(j.parent);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                xv.append(static_cast<fpreal32>(j.world(r, c)));
    }
    names.set(GA_DETAIL_OFFSET, nv);
    parents.set(GA_DETAIL_OFFSET, pv);
    xforms.set(GA_DETAIL_OFFSET, xv);
}

void SOP_MHDNAImport::createRLAttributes(GU_Detail *gdp, const char *suffix, const DnaFile &f) {
    const auto *r = f.reader.get();
    UT_WorkBuffer name;

    name.sprintf("rl_dna_file%s", suffix);
    GA_RWHandleS(gdp->addStringTuple(GA_ATTRIB_DETAIL, name.buffer(), 1)).set(GA_DETAIL_OFFSET, f.path.c_str());

    UT_StringArray ctrls;
    for (std::uint16_t i = 0; i < r->getRawControlCount(); ++i)
        ctrls.append(UT_StringHolder(toStd(r->getRawControlName(i))));
    name.sprintf("rl_ctrl_names%s", suffix);
    GA_RWHandleT<UT_StringArray>(gdp->addStringArray(GA_ATTRIB_DETAIL, name.buffer())).set(GA_DETAIL_OFFSET, ctrls);

    // Joints written by RigLogic: joint-group outputs (9 attributes per joint) and twist/swing outputs.
    std::vector<char> driven(r->getJointCount(), 0);
    for (std::uint16_t g = 0; g < r->getJointGroupCount(); ++g)
        for (std::uint16_t o : r->getJointGroupOutputIndices(g))
            if (o / 9 < driven.size())
                driven[o / 9] = 1;
    for (std::uint16_t t = 0; t < r->getTwistCount(); ++t)
        for (std::uint16_t j : r->getTwistOutputJointIndices(t))
            if (j < driven.size())
                driven[j] = 1;
    for (std::uint16_t s = 0; s < r->getSwingCount(); ++s)
        for (std::uint16_t j : r->getSwingOutputJointIndices(s))
            if (j < driven.size())
                driven[j] = 1;
    UT_StringArray joints;
    for (std::uint16_t j = 0; j < driven.size(); ++j)
        if (driven[j])
            joints.append(UT_StringHolder(toStd(r->getJointName(j))));
    name.sprintf("rl_joint_names%s", suffix);
    GA_RWHandleT<UT_StringArray>(gdp->addStringArray(GA_ATTRIB_DETAIL, name.buffer())).set(GA_DETAIL_OFFSET, joints);
}

void SOP_MHDNAImport::createGeometry(GU_Detail *gdp, const DnaFile &f, bool isHead, bool skinning,
                                     bool blendshapes) {
    const auto *r = f.reader.get();
    const std::vector<int> &jmap = isHead ? myHeadJointMap : myBodyJointMap;
    const bool flip = r->getFaceWindingOrder() == dna::FaceWindingOrder::ccw;  // Houdini front faces are clockwise
    const double s = myScale;

    GA_RWHandleS nameH(gdp->findPrimitiveAttribute(GA_Names::name));
    GA_RWHandleV3 uvH(gdp->findVertexAttribute(GA_Names::uv));
    GA_RWHandleV3 nH(gdp->findVertexAttribute(GA_Names::N));
    const GA_AIFIndexPair *capt = myCapture.isValid() ? myCapture->getAIFIndexPair() : nullptr;
    GA_PrimitiveGroup *hidden = gdp->findPrimitiveGroup("_3d_hidden_primitives");

    // Every LOD that was loaded (reader LODs are remapped to 0..count-1).
    std::unordered_set<std::uint16_t> seen;
    for (std::uint16_t lod = 0; lod < r->getLODCount(); ++lod) {
        for (std::uint16_t mi : r->getMeshIndicesForLOD(lod)) {
            if (!seen.insert(mi).second)
                continue;
            const UT_StringHolder meshName(toStd(r->getMeshName(mi)));
            const auto px = r->getVertexPositionXs(mi), py = r->getVertexPositionYs(mi),
                       pz = r->getVertexPositionZs(mi);
            const GA_Size npts = static_cast<GA_Size>(px.size());
            if (npts == 0)
                continue;
            const GA_Offset ptStart = gdp->appendPointBlock(npts);
            for (GA_Size i = 0; i < npts; ++i)
                gdp->setPos3(ptStart + i, UT_Vector3(px[i] * s, py[i] * s, pz[i] * s));

            const auto layP = r->getVertexLayoutPositionIndices(mi);
            const auto layT = r->getVertexLayoutTextureCoordinateIndices(mi);
            const auto layN = r->getVertexLayoutNormalIndices(mi);
            const auto us = r->getVertexTextureCoordinateUs(mi), vs = r->getVertexTextureCoordinateVs(mi);
            const auto nx = r->getVertexNormalXs(mi), ny = r->getVertexNormalYs(mi), nz = r->getVertexNormalZs(mi);

            const std::uint32_t nf = r->getFaceCount(mi);
            GA_PolyCounts counts;
            std::vector<int> ptnums;
            std::vector<std::uint32_t> layouts;  // layout index per created vertex, in creation order
            for (std::uint32_t fi = 0; fi < nf; ++fi) {
                const auto fl = r->getFaceVertexLayoutIndices(mi, fi);
                counts.append(static_cast<GA_Size>(fl.size()));
                for (std::size_t k = 0; k < fl.size(); ++k) {
                    const std::uint32_t l = flip ? fl[fl.size() - 1 - k] : fl[k];
                    layouts.push_back(l);
                    ptnums.push_back(static_cast<int>(layP[l]));
                }
            }
            const GA_Offset primStart =
                GEO_PrimPoly::buildBlock(gdp, ptStart, npts, counts, ptnums.data(), /*closed*/ true);
            std::size_t v = 0;
            for (GA_Offset p = primStart; p < primStart + static_cast<GA_Offset>(nf); ++p) {
                nameH.set(p, meshName);
                const GA_OffsetListRef vtx = gdp->getPrimitiveVertexList(p);
                for (GA_Size k = 0; k < vtx.size(); ++k, ++v) {
                    const std::uint32_t l = layouts[v];
                    if (layT.size())
                        uvH.set(vtx(k), UT_Vector3(us[layT[l]], vs[layT[l]], 0.0f));
                    if (layN.size())
                        nH.set(vtx(k), UT_Vector3(nx[layN[l]], ny[layN[l]], nz[layN[l]]));
                }
            }

            if (skinning && capt) {
                const std::uint32_t nw = r->getSkinWeightsCount(mi);
                for (std::uint32_t i = 0; i < nw && i < static_cast<std::uint32_t>(npts); ++i) {
                    const auto ws = r->getSkinWeightsValues(mi, i);
                    const auto js = r->getSkinWeightsJointIndices(mi, i);
                    int e = 0;
                    for (std::size_t k = 0; k < ws.size(); ++k) {
                        const int cj = js[k] < jmap.size() ? jmap[js[k]] : -1;
                        if (cj < 0)
                            continue;
                        capt->setIndex(myCapture.getAttribute(), ptStart + i, e, cj);
                        capt->setData(myCapture.getAttribute(), ptStart + i, e, static_cast<fpreal32>(ws[k]));
                        ++e;
                    }
                    for (; e < capt->getEntries(myCapture.getAttribute()); ++e) {
                        capt->setIndex(myCapture.getAttribute(), ptStart + i, e, -1);
                        capt->setData(myCapture.getAttribute(), ptStart + i, e, 0.0f);
                    }
                }
            }

            if (blendshapes) {
                GA_RWHandleS bsName(gdp->findPrimitiveAttribute("blendshape_name"));
                GA_RWHandleS bsChannel(gdp->findPrimitiveAttribute("blendshape_channel"));
                const std::uint16_t nt = r->getBlendShapeTargetCount(mi);
                for (std::uint16_t t = 0; t < nt; ++t) {
                    const auto vi = r->getBlendShapeTargetVertexIndices(mi, t);
                    const auto dx = r->getBlendShapeTargetDeltaXs(mi, t), dy = r->getBlendShapeTargetDeltaYs(mi, t),
                               dz = r->getBlendShapeTargetDeltaZs(mi, t);
                    GU_Detail *shape = new GU_Detail;
                    const GA_Offset sp = shape->appendPointBlock(static_cast<GA_Size>(vi.size()));
                    GA_RWHandleI idH(shape->addIntTuple(GA_ATTRIB_POINT, GA_Names::id, 1));
                    for (std::size_t k = 0; k < vi.size(); ++k) {
                        const std::uint32_t pi = vi[k];
                        shape->setPos3(sp + k, UT_Vector3((px[pi] + dx[k]) * s, (py[pi] + dy[k]) * s,
                                                          (pz[pi] + dz[k]) * s));
                        idH.set(sp + k, static_cast<int>(pi));
                    }
                    GU_DetailHandle gdh;
                    gdh.allocateAndSet(shape);
                    GU_PrimPacked *pp = GU_PackedGeometry::packGeometry(*gdp, gdh);
                    if (!pp)
                        continue;
                    const UT_StringHolder channel(
                        toStd(r->getBlendShapeChannelName(r->getBlendShapeChannelIndex(mi, t))));
                    UT_WorkBuffer bn;
                    bn.format("{0}.{1}", meshName, channel);
                    const GA_Offset po = pp->getMapOffset();
                    nameH.set(po, meshName);
                    bsChannel.set(po, channel);
                    bsName.set(po, UT_StringHolder(bn));
                    hidden->addOffset(po);
                }
            }
        }
    }
}

OP_ERROR SOP_MHDNAImport::cookMySop(OP_Context &context) {
    const fpreal t = context.getTime();
    gdp->clearAndDestroy();

    UT_String headPath, bodyPath;
    evalString(headPath, "headdnafile", 0, t);
    evalString(bodyPath, "bodydnafile", 0, t);
    headPath.trimBoundingSpace();
    bodyPath.trimBoundingSpace();
    myMaxLod = evalInt("maxlod", 0, t);
    myMinLod = evalInt("minlod", 0, t);
    if (myMinLod < myMaxLod)
        myMinLod = myMaxLod;
    const bool addMeshes = evalInt("addmeshes", 0, t) != 0;
    const bool addJoints = evalInt("addjoints", 0, t) != 0;
    const bool addSkinning = evalInt("addskinning", 0, t) != 0 && addMeshes && addJoints;
    const bool addBlendShapes = evalInt("addblendshapes", 0, t) != 0 && addMeshes;
    const bool addRigLogic = evalInt("addriglogic", 0, t) != 0;
    myScale = 0.01 * evalFloat("unitsmultiplier", 0, t);

    if (!headPath.isstring() && !bodyPath.isstring()) {
        gdp->bumpAllDataIds();
        return error();
    }
    if (headPath.isstring()) {
        if (!loadDna(myHead, headPath, myMaxLod, myMinLod))
            return error();
    } else {
        myHead = DnaFile();
    }
    if (bodyPath.isstring()) {
        // Two head LODs per body LOD.
        const int bmax = myMaxLod / 2, bmin = myMinLod / 2;
        if (!loadDna(myBody, bodyPath, bmax, bmin))
            return error();
    } else {
        myBody = DnaFile();
    }

    buildJoints();
    createDetailMetadata(gdp);
    if (addJoints)
        createJointAttributes(gdp);
    if (addRigLogic) {
        if (myHead.reader)
            createRLAttributes(gdp, "_head", myHead);
        if (myBody.reader)
            createRLAttributes(gdp, "_body", myBody);
    }

    if (addMeshes) {
        gdp->addStringTuple(GA_ATTRIB_PRIMITIVE, GA_Names::name, 1);
        GA_Attribute *uv = gdp->addFloatTuple(GA_ATTRIB_VERTEX, GA_Names::uv, 3);
        uv->setTypeInfo(GA_TYPE_TEXTURE_COORD);
        GA_Attribute *n = gdp->addFloatTuple(GA_ATTRIB_VERTEX, GA_Names::N, 3);
        n->setTypeInfo(GA_TYPE_NORMAL);
        if (addBlendShapes) {
            gdp->addStringTuple(GA_ATTRIB_PRIMITIVE, "blendshape_name", 1);
            gdp->addStringTuple(GA_ATTRIB_PRIMITIVE, "blendshape_channel", 1);
            gdp->newPrimitiveGroup("_3d_hidden_primitives");
        }

        int maxInf = 0;
        for (const DnaFile *f : {&myBody, &myHead}) {
            if (!f->reader)
                continue;
            for (std::uint16_t m = 0; m < f->reader->getMeshCount(); ++m)
                maxInf = std::max<int>(maxInf, f->reader->getMaximumInfluencePerVertex(m));
        }
        myCapture.clear();
        if (addSkinning && maxInf > 0) {
            myCapture = gdp->addPointCaptureAttribute(GEO_Detail::geo_NPairs(maxInf));
            int regionsIdx = -1;
            GA_AIFIndexPairObjects *regions =
                GEO_AttributeCaptureRegion::getBoneCaptureRegionObjects(myCapture, regionsIdx);
            regions->setObjectCount(static_cast<int>(myJoints.size()));
            GEO_RWAttributeCapturePath paths(gdp);
            for (std::size_t i = 0; i < myJoints.size(); ++i) {
                paths.setPath(static_cast<int>(i), myJoints[i].name.c_str());
                GEO_CaptureBoneStorage reg;
                UT_Matrix4D inv = myJoints[i].world;
                inv.invert();
                reg.myXform = UT_Matrix4F(inv);
                regions->setObjectValues(static_cast<int>(i), regionsIdx, reg.floatPtr(),
                                         GEO_CaptureBoneStorage::tuple_size);
            }
        }

        // Body first, then head, so point numbering is stable.
        if (myBody.reader)
            createGeometry(gdp, myBody, false, addSkinning, addBlendShapes);
        if (myHead.reader)
            createGeometry(gdp, myHead, true, addSkinning, addBlendShapes);
    }

    gdp->bumpAllDataIds();
    return error();
}

}  // namespace metahuman

void newSopOperator(OP_OperatorTable *table) {
    auto *op = new OP_Operator(metahuman::SOP_MHDNAImport::theSOPTypeName, "MetaHuman DNA Import",
                               metahuman::SOP_MHDNAImport::myConstructor,
                               metahuman::SOP_MHDNAImport::buildTemplates(), 0, 0, nullptr, OP_FLAG_GENERATOR);
    op->setIconName("mh_default_icon.svg");
    op->setOpTabSubMenuPath("MetaHuman/Character Assembler");
    table->addOperator(op);
}
