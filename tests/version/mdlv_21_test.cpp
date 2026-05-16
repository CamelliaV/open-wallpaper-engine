// Deep test for mdlv_21.
//
// To regenerate after a deliberate parser change:
//     ninja -C build/user-clang-debug wpdump
//     ./build/user-clang-debug/tests/wpdump workshop/3400879974 \
//         tests/fixtures/mdlv_21/3400879974.json

#include "fixture_helpers.hpp"

DEFINE_FIXTURE_TEST_SUITE(
    Mdlv21Test,
    { "3400879974", WAYWALLEN_FIXTURE_DIR "/mdlv_21/3400879974.json" });
