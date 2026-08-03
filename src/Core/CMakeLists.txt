# ---------------------------------------------------------------------------
# owe-core — small foundational utilities (Literals, NoCopy/NoMove,
# Map/Set, BlockingQueue, ArrayHelper, Visitors, Random, StringHelper)
# packaged as the `wescene.core` module. Configured against rstd's
# `cppstd` for std:: re-exports plus the third_party effolkronium random
# header in the GMF.
# ---------------------------------------------------------------------------
add_subdirectory(IO)
add_subdirectory(Fs)

add_library(owe-fft STATIC Fft.cpp)
target_sources(owe-fft
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES Fft.cppm)
target_link_libraries(owe-fft PUBLIC rstd::rstd)
target_compile_features(owe-fft PUBLIC cxx_std_20)
set_property(TARGET owe-fft PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-fft PROPERTY CXX_SCAN_FOR_MODULES ON)

if(BUILD_TESTS)
  add_executable(owe-fft-tests ../../tests/FftTests.cpp)
  target_link_libraries(owe-fft-tests PRIVATE owe-fft)
  target_compile_features(owe-fft-tests PRIVATE cxx_std_20)
  set_property(TARGET owe-fft-tests PROPERTY CXX_SCAN_FOR_MODULES ON)
  add_test(NAME owe-fft-tests COMMAND owe-fft-tests)
endif()

add_library(owe-core STATIC)
target_sources(owe-core PRIVATE Json.cpp UserProperty.cpp)
target_sources(owe-core
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         Core.cppm
         eigen.cppm
         Fs.cppm
         Json.cppm
         UserProperty.cppm)
target_link_libraries(owe-core
  PUBLIC owe-io owe-fs rstd::rstd rstd.cppstd rstd.json rstd.log Eigen3::Eigen)
target_compile_features(owe-core PUBLIC cxx_std_20)
set_property(TARGET owe-core PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-core PROPERTY CXX_SCAN_FOR_MODULES ON)

add_library(owe-cli STATIC)
target_sources(owe-cli
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES Cli.cppm)
target_link_libraries(owe-cli PUBLIC rstd::argparse rstd.cppstd)
target_compile_features(owe-cli PUBLIC cxx_std_20)
set_property(TARGET owe-cli PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-cli PROPERTY CXX_SCAN_FOR_MODULES ON)
