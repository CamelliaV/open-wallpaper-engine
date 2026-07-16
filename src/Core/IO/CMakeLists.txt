add_library(owe-io STATIC)
target_sources(owe-io
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES Binary.cppm)
target_link_libraries(owe-io PUBLIC rstd::rstd rstd.cppstd)
target_compile_features(owe-io PUBLIC cxx_std_20)
set_property(TARGET owe-io PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-io PROPERTY CXX_SCAN_FOR_MODULES ON)
