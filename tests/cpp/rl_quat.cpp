// Standalone: OpenRigLogic in quaternion mode, head jawOpen=1 / body calf_l bent; print a few outputs.
//   clang++ -std=c++17 -O2 -I third_party/openriglogic/include -DRL_BUILD_WITH_XYZ_ROTATION_ORDER tests/cpp/rl_quat.cpp build/librigdna.a -o build/rl_quat
#include <dna/BinaryStreamReader.h>
#include <riglogic/RigLogic.h>
#include <trio/streams/FileStream.h>
#include <cstdio>
#include <string>
int main(int argc, char **argv) {
    auto *s = trio::FileStream::create(argv[1], trio::AccessMode::Read, trio::OpenMode::Binary);
    auto *r = dna::BinaryStreamReader::create(s, dna::DataLayer::All);
    r->read();
    rl4::Configuration cfg;
    cfg.floatingPointType = rl4::FloatingPointType::Float;
    cfg.rotationType = rl4::RotationType::Quaternions;
    if (argc > 2) cfg.calculationType = argv[2][0] == 's' ? rl4::CalculationType::Scalar : rl4::CalculationType::SSE;
    auto *rl = rl4::RigLogic::create(r, cfg);
    auto *in = rl4::RigInstance::create(rl);
    for (std::uint16_t i = 0; i < r->getRawControlCount(); ++i) {
        std::string n(r->getRawControlName(i).data(), r->getRawControlName(i).size());
        float v = n.size() > 3 && n.substr(n.size() - 3) == ".qw" ? 1.0f : 0.0f;
        if (n == "CTRL_expressions.jawOpen") v = 1.0f;
        if (n == "calf_l.qz") v = -0.70710678f;
        if (n == "calf_l.qw") v = 0.70710678f;
        in->setRawControl(i, v);
    }
    rl->calculate(in);
    auto out = in->getJointOutputs();
    std::printf("outputs %zu (joints %u)\n", out.size(), r->getJointCount());
    for (std::uint16_t j = 0; j < r->getJointCount(); ++j) {
        std::string n(r->getJointName(j).data(), r->getJointName(j).size());
        if (n != "FACIAL_C_LipLower" && n != "FACIAL_C_Jaw" && n != "calf_kneeBack_l" && n != "calf_correctiveRoot_l") continue;
        std::printf("%s:", n.c_str());
        for (int k = 0; k < 10; ++k) std::printf(" %.4f", out[10 * j + k]);
        std::printf("\n");
    }
}
