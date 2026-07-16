add_library(owe-resource STATIC)
target_sources(owe-resource
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         Resource.cppm Handle.cppm Error.cppm Texture.cppm Buffer.cppm Shader.cppm Plan.cppm
         Catalog.cppm)
target_link_libraries(owe-resource
  PUBLIC rstd::rstd rstd.cppstd owe-types)
target_include_directories(owe-resource PUBLIC ../..)
target_compile_features(owe-resource PUBLIC cxx_std_20)
set_property(TARGET owe-resource PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-resource PROPERTY CXX_SCAN_FOR_MODULES ON)

add_library(owe-resource-registry STATIC)
target_sources(owe-resource-registry
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         Registry.cppm
         Owner.cppm
         System.cppm
         Descriptor.cppm
         Prepared.cppm
         Lifetime.cppm
         Barrier.cppm
         State.cppm
         Policy.cppm
         External.cppm
         ResourceKey.cppm
         Graphics.cppm
         TextureRegistry.cppm
         BufferRegistry.cppm
         ShaderRegistry.cppm)
target_link_libraries(owe-resource-registry
  PUBLIC owe-resource owe-vulkan)
target_include_directories(owe-resource-registry PUBLIC ../..)
target_compile_features(owe-resource-registry PUBLIC cxx_std_20)
set_property(TARGET owe-resource-registry PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-resource-registry PROPERTY CXX_SCAN_FOR_MODULES ON)
