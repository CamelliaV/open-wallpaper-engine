# ---------------------------------------------------------------------------
# owe-shader-compile (host-only glslang + SPIRV-Reflect)
# ---------------------------------------------------------------------------

add_library(owe-shader-compile STATIC Shader.cpp)
target_sources(owe-shader-compile
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES ShaderCompile.cppm)
target_link_libraries(owe-shader-compile
  PUBLIC rstd::rstd rstd.cppstd owe-types vvk::ffi::vulkan
  PRIVATE glslang SPIRV glslang-default-resource-limits spirv-reflect-static)
target_include_directories(owe-shader-compile
  PUBLIC  .. ../Scene/Utils/include
  PRIVATE ../Scene/Utils ../Scene/Utils/include/Utils .)
set_property(TARGET owe-shader-compile PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-shader-compile PROPERTY CXX_SCAN_FOR_MODULES ON)


# ---------------------------------------------------------------------------
# owe-vulkan (Vulkan device, instance, swapchain, etc.)
# ---------------------------------------------------------------------------

add_library(owe-vulkan STATIC
  Parameters.cpp
  Swapchain.cpp
  Instance.cpp
  Device.cpp
  StagingBuffer.cpp
  GraphicsPipeline.cpp
  TextureCache.cpp
  MeshCache.cpp)
target_sources(owe-vulkan
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         Vulkan.cppm)
target_link_libraries(owe-vulkan
  PUBLIC vvk::vvk owe-types owe-shader-compile
  # owe-core supplies the video source range; wavsen::video supplies the
  # decoder and shared device API.
  PRIVATE owe-core wavsen::video)
target_include_directories(owe-vulkan
  PUBLIC  .. ../Scene ../Scene/Swapchain ../Scene/Utils/include
  PRIVATE ../Scene/Utils ../Scene/Utils/include/Utils .)
set_property(TARGET owe-vulkan PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-vulkan PROPERTY CXX_SCAN_FOR_MODULES ON)
