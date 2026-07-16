add_library(owe-fs STATIC)
target_sources(owe-fs
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES Fs.cppm)
target_link_libraries(owe-fs PUBLIC rstd::rstd)
target_compile_features(owe-fs PUBLIC cxx_std_20)
set_property(TARGET owe-fs PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-fs PROPERTY CXX_SCAN_FOR_MODULES ON)
