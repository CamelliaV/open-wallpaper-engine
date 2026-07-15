add_library(owe-resource STATIC)
target_sources(owe-resource
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         Resource.cppm
         TextureRegistry.cppm
         PipelineRegistry.cppm
         ShaderRegistry.cppm)
target_link_libraries(owe-resource
  PUBLIC rstd::rstd rstd.cppstd owe-render)
target_include_directories(owe-resource PUBLIC ../..)
target_compile_features(owe-resource PUBLIC cxx_std_20)
set_property(TARGET owe-resource PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-resource PROPERTY CXX_SCAN_FOR_MODULES ON)
