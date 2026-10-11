// macOS reimplementation of the APEX callbacks in Epic's MetaHuman for Houdini
// (apexdso/metahuman_rig_logic): mh::RigLogic and mh::DrivenKeys.
// Signatures and port names follow the official Linux build (see docs/INTERFACE.md);
// the official HDA scripts (create_head_RL / create_body_RL / connect_gui_ctrls)
// wire these ports by name.

#include <UT/UT_DSOVersion.h>

#include <APEX/APEX_Include.h>
#include <APEX/APEX_Callback.h>
#include <APEX/APEX_CallbackPlugin.h>
#include <APEX/APEX_Generic.h>
#include <GU/GU_Detail.h>
#include <GA/GA_Handle.h>
#include <UT/UT_Matrix3.h>
#include <UT/UT_Matrix4.h>
#include <UT/UT_Options.h>
#include <UT/UT_Quaternion.h>
#include <UT/UT_StringHolder.h>
#include <UT/UT_StringMap.h>

#include <dna/BinaryStreamReader.h>
#include <riglogic/RigLogic.h>
#include <trio/streams/FileStream.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace apex;

namespace {

std::string toStd(dna::StringView v) { return std::string(v.data(), v.size()); }

}  // namespace

class MHRigLogic : public APEX_GenericFunctionRunData<MHRigLogic, Geometry, VariadicArg<Matrix4>, const String,
                                                      const Int, const Int, const Int, const Float,
                                                      const VariadicArg<Float>, const VariadicArg<Matrix4>> {
public:
    static constexpr UT_StringLit funcname = "mh::RigLogic";
    static constexpr const char *argnames[] = {"*skel",   "joints", "dna_file",         "max_lod",    "min_lod",
                                               "lod",     "units_multiplier", "basic_ctrls", "joint_ctrls"};

    struct RunData {
        RunData() = default;
        ~RunData() { release(); }
        RunData(const RunData &) = delete;
        const RunData &operator=(const RunData &) = delete;

        // Loaded state
        trio::FileStream *stream = nullptr;
        dna::BinaryStreamReader *reader = nullptr;
        rl4::RigLogic *rigLogic = nullptr;
        rl4::RigInstance *instance = nullptr;
        UT_StringHolder file;
        exint maxLod = -1, minLod = -1;
        double scale = 0.01;
        std::vector<float> neutral;            // 10 values per joint (t, q, s)
        std::vector<UT_Matrix3D> neutralRot;    // neutral rotation per joint

        // Name lookups (DNA side)
        UT_StringMap<int> jointIndex;     // joint name -> DNA joint index
        UT_StringMap<int> basicCtrl;      // "browDownL" -> raw control index
        UT_StringMap<int> quatCtrl;       // "<joint>.qx" -> raw control index
        UT_StringArray channelNames;      // blend shape channel names (attribute names)
        UT_StringArray animMapParms;      // animated map names with '.' -> '__'

        // Port-name caches (graph side), rebuilt when the variadic names change
        UT_StringArray basicNames, jointCtrlNames, jointOutNames;
        std::vector<int> basicIdx, jointOutIdx;
        struct Driver {
            int q[4] = {-1, -1, -1, -1};
            UT_Matrix3D restInv;
        };
        std::vector<Driver> drivers;

        void release() {
            if (instance)
                rl4::RigInstance::destroy(instance);
            if (rigLogic)
                rl4::RigLogic::destroy(rigLogic);
            if (reader)
                dna::BinaryStreamReader::destroy(reader);
            if (stream)
                trio::FileStream::destroy(stream);
            instance = nullptr;
            rigLogic = nullptr;
            reader = nullptr;
            stream = nullptr;
            file.clear();
            basicNames.clear();
            jointCtrlNames.clear();
            jointOutNames.clear();
        }

        bool initRigLogic(const UT_StringHolder &path, exint maxl, exint minl, double units) {
            const double newScale = 0.01 * units;
            if (instance && path == file && maxl == maxLod && minl == minLod && newScale == scale)
                return true;
            release();
            if (!path.isstring())
                return false;
            stream = trio::FileStream::create(path.c_str(), trio::AccessMode::Read, trio::OpenMode::Binary);
            dna::Configuration cfg;
            cfg.layer = dna::DataLayer::All;
            cfg.maxLOD = static_cast<std::uint16_t>(std::max<exint>(0, maxl));
            cfg.minLOD = static_cast<std::uint16_t>(std::max<exint>(maxl, minl));
            reader = dna::BinaryStreamReader::create(stream, cfg);
            reader->read();
            if (!dna::Status::isOk()) {
                release();
                return false;
            }
            rl4::Configuration rlcfg;
            rlcfg.floatingPointType = rl4::FloatingPointType::Float;
            // Quaternion outputs, as in UE's AnimNode_RigLogic: joints whose DNA rotation representation is a
            // quaternion (body correctives) only produce rotations in this mode.
            rlcfg.rotationType = rl4::RotationType::Quaternions;
            rigLogic = rl4::RigLogic::create(reader, rlcfg);
            if (!rigLogic) {
                release();
                return false;
            }
            instance = rl4::RigInstance::create(rigLogic);
            file = path;
            maxLod = maxl;
            minLod = minl;
            scale = newScale;

            jointIndex.clear();
            for (std::uint16_t j = 0; j < reader->getJointCount(); ++j)
                jointIndex[UT_StringHolder(toStd(reader->getJointName(j)))] = j;
            basicCtrl.clear();
            quatCtrl.clear();
            for (std::uint16_t i = 0; i < reader->getRawControlCount(); ++i) {
                const std::string n = toStd(reader->getRawControlName(i));
                const std::size_t dot = n.find('.');
                const std::string attr = dot == std::string::npos ? n : n.substr(dot + 1);
                if (attr == "qx" || attr == "qy" || attr == "qz" || attr == "qw")
                    quatCtrl[UT_StringHolder(n)] = i;
                else
                    basicCtrl[UT_StringHolder(attr)] = i;
            }
            channelNames.clear();
            for (std::uint16_t c = 0; c < reader->getBlendShapeChannelCount(); ++c)
                channelNames.append(UT_StringHolder(toStd(reader->getBlendShapeChannelName(c))));
            animMapParms.clear();
            for (std::uint16_t a = 0; a < reader->getAnimatedMapCount(); ++a) {
                std::string n = toStd(reader->getAnimatedMapName(a));
                for (std::size_t p; (p = n.find('.')) != std::string::npos;)
                    n.replace(p, 1, "__");
                animMapParms.append(UT_StringHolder(n));
            }

            // Neutral values: 10 per joint (t xyz, q xyzw, s xyz). RigLogic outputs are deltas on top.
            const auto nv = rigLogic->getNeutralJointValues();
            neutral.assign(nv.begin(), nv.end());
            const std::size_t nj = neutral.size() / 10;
            neutralRot.resize(nj);
            for (std::size_t j = 0; j < nj; ++j) {
                const float *q = &neutral[10 * j + 3];
                UT_QuaternionD(q[0], q[1], q[2], q[3]).getRotationMatrix(neutralRot[j]);
            }
            return true;
        }

        // Same composition as UE's AnimNode_RigLogic: T = Nt + Dt, R = Nq * Dq, S = Ns + Ds.
        // Row-vector matrices: the delta rotation applies first, in the joint's own frame.
        UT_Matrix4D jointLocal(int j, const float *d) const {
            const float *n = &neutral[10 * j];
            UT_Matrix3D rd;
            UT_QuaternionD(d[3], d[4], d[5], d[6]).getRotationMatrix(rd);
            UT_Matrix3D r = rd * neutralRot[j];
            UT_Matrix3D sc(1.0);
            sc.scale(n[7] + d[7], n[8] + d[8], n[9] + d[9]);
            UT_Matrix4D m(sc * r);
            m.setTranslates(UT_Vector3D((n[0] + d[0]) * scale, (n[1] + d[1]) * scale, (n[2] + d[2]) * scale));
            return m;
        }
    };

    APEX_DEF_TYPE_RUNDATA(MHRigLogic)

    void operator()(RunData &rd, Geometry &skel, VariadicArg<Matrix4> &joints, const String &dna_file,
                    const Int &max_lod, const Int &min_lod, const Int &lod, const Float &units_multiplier,
                    const VariadicArg<Float> &basic_ctrls, const VariadicArg<Matrix4> &joint_ctrls) const {
        if (!rd.initRigLogic(dna_file, max_lod, min_lod, units_multiplier))
            return;
        const std::uint16_t lodCount = rd.rigLogic->getLODCount();
        const std::uint16_t l = static_cast<std::uint16_t>(std::clamp<exint>(lod, 0, lodCount ? lodCount - 1 : 0));
        if (rd.instance->getLOD() != l)
            rd.instance->setLOD(l);

        sourceBasicControls(rd, basic_ctrls);
        sourceJointControls(rd, joint_ctrls);
        rd.rigLogic->calculate(rd.instance);
        updateJoints(rd, joints);
        updateBlendShapes(rd, skel);
        updateAnimatedMaps(rd, skel);
    }

private:
    static bool sameNames(const UT_StringArray &a, const UT_StringArray &b) {
        if (a.size() != b.size())
            return false;
        for (exint i = 0; i < a.size(); ++i)
            if (a[i] != b[i])
                return false;
        return true;
    }

    void sourceBasicControls(RunData &rd, const VariadicArg<Float> &ctrls) const {
        const UT_StringArray &names = ctrls.names();
        if (!sameNames(names, rd.basicNames)) {
            rd.basicNames = names;
            rd.basicIdx.assign(names.size(), -1);
            for (exint i = 0; i < names.size(); ++i) {
                auto it = rd.basicCtrl.find(names[i]);
                if (it != rd.basicCtrl.end())
                    rd.basicIdx[i] = it->second;
            }
        }
        for (exint i = 0; i < names.size(); ++i)
            if (rd.basicIdx[i] >= 0)
                rd.instance->setRawControl(static_cast<std::uint16_t>(rd.basicIdx[i]), static_cast<float>(*ctrls[i]));
    }

    void sourceJointControls(RunData &rd, const VariadicArg<Matrix4> &ctrls) const {
        const UT_StringArray &names = ctrls.names();
        if (!sameNames(names, rd.jointCtrlNames)) {
            rd.jointCtrlNames = names;
            rd.drivers.assign(names.size(), RunData::Driver());
            static const char *comp[4] = {".qx", ".qy", ".qz", ".qw"};
            for (exint i = 0; i < names.size(); ++i) {
                RunData::Driver &d = rd.drivers[i];
                for (int c = 0; c < 4; ++c) {
                    UT_WorkBuffer n;
                    n.sprintf("%s%s", names[i].c_str(), comp[c]);
                    auto it = rd.quatCtrl.find(UT_StringHolder(n));
                    d.q[c] = it != rd.quatCtrl.end() ? it->second : -1;
                }
                auto jt = rd.jointIndex.find(names[i]);
                if (jt != rd.jointIndex.end() && static_cast<std::size_t>(jt->second) < rd.neutralRot.size()) {
                    d.restInv = rd.neutralRot[jt->second];
                    d.restInv.invert();
                } else {
                    d.restInv.identity();
                }
            }
        }
        for (exint i = 0; i < names.size(); ++i) {
            const RunData::Driver &d = rd.drivers[i];
            if (d.q[0] < 0)
                continue;
            // RBF inputs are the joint's rotation relative to its neutral local rotation, applied in the joint's
            // own frame (UE: Delta = InvNeutral * Abs): R_current = D * R_rest  =>  D = R_current * R_rest^-1.
            UT_Matrix3D cur(*ctrls[i]);
            cur.makeRotationMatrix();
            UT_Matrix3D delta = cur * d.restInv;
            UT_QuaternionD q;
            q.updateFromRotationMatrix(delta);
            if (q.w() < 0)
                q = -q;
            const double v[4] = {q.x(), q.y(), q.z(), q.w()};
            for (int c = 0; c < 4; ++c)
                if (d.q[c] >= 0)
                    rd.instance->setRawControl(static_cast<std::uint16_t>(d.q[c]), static_cast<float>(v[c]));
        }
    }

    void updateJoints(RunData &rd, VariadicArg<Matrix4> &joints) const {
        const UT_StringArray &names = joints.names();
        if (!sameNames(names, rd.jointOutNames)) {
            rd.jointOutNames = names;
            rd.jointOutIdx.assign(names.size(), -1);
            for (exint i = 0; i < names.size(); ++i) {
                auto it = rd.jointIndex.find(names[i]);
                if (it != rd.jointIndex.end())
                    rd.jointOutIdx[i] = it->second;
            }
        }
        const auto out = rd.instance->getJointOutputs();
        for (exint i = 0; i < names.size(); ++i) {
            const int j = rd.jointOutIdx[i];
            if (j < 0 || static_cast<std::size_t>(10 * j + 10) > out.size())
                continue;
            *joints[i] = rd.jointLocal(j, out.data() + 10 * j);
        }
    }

    void updateBlendShapes(RunData &rd, Geometry &skel) const {
        GU_Detail *gdp = skel.gdp();
        if (!gdp)
            return;
        const auto w = rd.instance->getBlendShapeOutputs();
        for (exint c = 0; c < rd.channelNames.size() && c < static_cast<exint>(w.size()); ++c) {
            GA_RWHandleF h(gdp->findFloatTuple(GA_ATTRIB_DETAIL, rd.channelNames[c], 1));
            if (h.isInvalid())
                h.bind(gdp->addFloatTuple(GA_ATTRIB_DETAIL, rd.channelNames[c], 1));
            h.set(GA_DETAIL_OFFSET, w[c]);
            h.bumpDataId();
        }
    }

    void updateAnimatedMaps(RunData &rd, Geometry &skel) const {
        GU_Detail *gdp = skel.gdp();
        if (!gdp)
            return;
        const auto a = rd.instance->getAnimatedMapOutputs();
        UT_OptionsHolder opts;
        UT_Options &o = *opts.makeUnique();
        for (exint i = 0; i < rd.animMapParms.size() && i < static_cast<exint>(a.size()); ++i)
            o.setOptionF(rd.animMapParms[i], a[i]);
        GA_RWHandleDict h(gdp->findDictTuple(GA_ATTRIB_DETAIL, "material_override", 1));
        if (h.isInvalid())
            h.bind(gdp->addDictTuple(GA_ATTRIB_DETAIL, "material_override", 1));
        h.set(GA_DETAIL_OFFSET, opts);
        h.bumpDataId();
    }
};

// Piecewise-linear set-driven-key evaluation (GUI control -> raw control), clamped at both ends.
class MHDrivenKeys : public APEX_GenericFunction<MHDrivenKeys, Float, const Float, const VariadicArg<Float>,
                                                 const VariadicArg<Float>> {
public:
    static constexpr UT_StringLit funcname = "mh::DrivenKeys";
    static constexpr const char *argnames[] = {"driven", "driver", "driver_keys", "driven_keys"};

    void operator()(Float &driven, const Float &driver, const VariadicArg<Float> &driver_keys,
                    const VariadicArg<Float> &driven_keys) const {
        const exint n = SYSmin(driver_keys.size(), driven_keys.size());
        if (n == 0) {
            driven = 0;
            return;
        }
        if (n == 1 || driver <= *driver_keys[0]) {
            driven = *driven_keys[0];
            return;
        }
        if (driver >= *driver_keys[n - 1]) {
            driven = *driven_keys[n - 1];
            return;
        }
        for (exint i = 1; i < n; ++i) {
            const Float x0 = *driver_keys[i - 1], x1 = *driver_keys[i];
            if (driver <= x1) {
                const Float t = x1 > x0 ? (driver - x0) / (x1 - x0) : 0.0;
                driven = *driven_keys[i - 1] + t * (*driven_keys[i] - *driven_keys[i - 1]);
                return;
            }
        }
        driven = *driven_keys[n - 1];
    }
};

extern "C" APEX_VISIBILITY_EXPORT void addApexFunction(APEX_Registry &reg) {
    static MHRigLogic rigLogic;
    static MHDrivenKeys drivenKeys;
    reg.addCallback(&rigLogic);
    reg.addCallback(&drivenKeys);
}
