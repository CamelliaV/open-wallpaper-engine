# ---------------------------------------------------------------------------
# owe-core — small foundational utilities (Literals, NoCopy/NoMove,
# Map/Set, BlockingQueue, ArrayHelper, Visitors, Random, StringHelper)
# packaged as the `wescene.core` module. Configured against rstd's
# `cppstd` for std:: re-exports plus the third_party effolkronium random
# header in the GMF.
# ---------------------------------------------------------------------------
add_library(owe-core STATIC)
target_sources(owe-core
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES Core.cppm)
target_link_libraries(owe-core
  PUBLIC rstd::rstd rstd.cppstd)
target_compile_features(owe-core PUBLIC cxx_std_20)
target_compile_options(owe-core PRIVATE -Wno-missing-field-initializers)
set_property(TARGET owe-core PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-core PROPERTY CXX_SCAN_FOR_MODULES ON)
