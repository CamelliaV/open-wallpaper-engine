include_directories(.)

if(BUILD_WESCENE)

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)
find_package(Freetype REQUIRED)
pkg_check_modules(LZ4 REQUIRED liblz4)

option(ENABLE_RENDERDOC "Build with renderdoc api" OFF)

if(ENABLE_RENDERDOC)
  add_compile_definitions(ENABLE_RENDERDOC_API=1)
endif()

include(TestBigEndian)
test_big_endian(ENDIAN)
if(ENDIAN)
  add_compile_definitions(WP_BIG_ENDIAN)
endif()

set(warn_opts
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-variable
    CACHE INTERNAL "")

# DXC's dxcapi.h sits under <dxc_SOURCE_DIR>/include. The dxcompiler target
# does not always export this include path correctly when consumed via
# add_subdirectory; force-republish via FetchContent_GetProperties.
include(FetchContent)
FetchContent_GetProperties(dxc SOURCE_DIR _dxc_source_dir)

add_library(
  wescene-base STATIC
  Type.cpp
  Utils/Logging.cpp
  Utils/Sha.cpp
  Utils/DynamicLibrary.cpp
  Utils/FpsCounter.cpp
  Utils/Algorism.cpp
  WPJson.cpp
  WPPkgFs.cpp
  Timer/ThreadTimer.cpp
  Timer/FrameTimer.cpp
  Scene/Scene.cpp
  Scene/SceneCamera.cpp
  Scene/SceneImageEffectLayer.cpp
  Scene/SceneIndexArray.cpp
  Scene/SceneNode.cpp
  Scene/SceneVertexArray.cpp
  Scene/SceneShader.cpp
  Particle/ParticleModify.cpp
  Particle/ParticleSystem.cpp
  Particle/ParticleEmitter.cpp
  Particle/WPParticleRawGener.cpp)
target_sources(
  wescene-base
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         WPCommon.cppm
         WPJson.cppm
         WPPkgFs.cppm
         Utils/Utils.cppm
         MessageLoop/MessageLoop.cppm
         Timer/Timer.cppm
         Timer/ThreadTimer.cppm
         Timer/FrameTimer.cppm
         Fs/Fs.cppm
         Scene/Scene.cppm)
target_link_libraries(
  wescene-base
  PUBLIC rstd::rstd rstd.cppstd Eigen3::Eigen wavsen::audio
  PRIVATE nlohmann_json ${LZ4_LIBRARIES}
          ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})
target_include_directories(
  wescene-base
  PUBLIC . Utils/include
  PRIVATE Utils Utils/include/Utils Scene Particle)
target_compile_options(wescene-base PRIVATE -Wno-missing-field-initializers)
set_property(TARGET wescene-base PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET wescene-base PROPERTY CXX_SCAN_FOR_MODULES ON)

# ---------------------------------------------------------------------------
# wescene-shader-compile (host-only DXC + SPIRV-Reflect)
# ---------------------------------------------------------------------------

add_library(wescene-shader-compile STATIC Vulkan/Shader.cpp)
target_sources(
  wescene-shader-compile
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES Vulkan/ShaderCompile.cppm)
target_link_libraries(
  wescene-shader-compile
  PUBLIC rstd::rstd rstd.cppstd
  PRIVATE dxcompiler spirv-reflect-static)
target_include_directories(
  wescene-shader-compile
  PUBLIC . Utils/include
  PRIVATE Utils Utils/include/Utils Vulkan)
if(_dxc_source_dir)
  target_include_directories(wescene-shader-compile PRIVATE ${_dxc_source_dir}/include)
endif()
target_compile_options(wescene-shader-compile PRIVATE -Wno-missing-field-initializers)
set_property(TARGET wescene-shader-compile PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET wescene-shader-compile PROPERTY CXX_SCAN_FOR_MODULES ON)

# ---------------------------------------------------------------------------
# wescene-script  (QuickJS-NG runtime for the WE scenescript surface)
# ---------------------------------------------------------------------------

add_library(wescene-script STATIC Script/Script.cpp)
target_sources(
  wescene-script
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES Script/Script.cppm)
target_link_libraries(
  wescene-script
  PUBLIC  wescene-base
  PRIVATE qjs nlohmann_json)
target_include_directories(
  wescene-script
  PUBLIC  . Utils/include
  PRIVATE Utils Utils/include/Utils Script)
target_compile_options(wescene-script PRIVATE -Wno-missing-field-initializers)
set_property(TARGET wescene-script PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET wescene-script PROPERTY CXX_SCAN_FOR_MODULES ON)

# ---------------------------------------------------------------------------
# wescene-parse  (Parse/* + Puppet + ShaderValueUpdater)
# ---------------------------------------------------------------------------

add_library(
  wescene-parse STATIC
  WPShaderValueUpdater.cpp
  Parse/WPAnimation.cpp
  Parse/WPImageObject.cpp
  Parse/WPLightObject.cpp
  Parse/WPMaterial.cpp
  Parse/WPMdlParser.cpp
  Parse/WPParticleObject.cpp
  Parse/WPParticleParser.cpp
  Parse/WPPuppet.cpp
  Parse/WPScene.cpp
  Parse/WPSceneParser.cpp
  Parse/WPShaderParser.cpp
  Parse/WPSoundParser.cpp
  Parse/WPTexImageParser.cpp
  Text/Text.cpp)
target_sources(
  wescene-parse
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         WPShaderValueUpdater.cppm
         Parse/Parse.cppm
         Parse/WPAnimation.cppm
         Parse/WPImageObject.cppm
         Parse/WPLightObject.cppm
         Parse/WPMaterial.cppm
         Parse/WPMdlParser.cppm
         Parse/WPMiscObject.cppm
         Parse/WPParticleObject.cppm
         Parse/WPParticleParser.cppm
         Parse/WPPuppet.cppm
         Parse/WPScene.cppm
         Parse/WPSceneParser.cppm
         Parse/WPShaderParser.cppm
         Parse/WPSoundObject.cppm
         Parse/WPSoundParser.cppm
         Parse/WPTexImageParser.cppm
         Parse/WPUniform.cppm
         Text/Text.cppm)
target_link_libraries(
  wescene-parse
  PUBLIC wescene-base wescene-shader-compile wescene-script
  PRIVATE nlohmann_json ${LZ4_LIBRARIES} Freetype::Freetype)
target_include_directories(
  wescene-parse
  PUBLIC . Utils/include
  PRIVATE Utils Utils/include/Utils Scene Particle Audio)
target_compile_options(wescene-parse PRIVATE -Wno-missing-field-initializers)
set_property(TARGET wescene-parse PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET wescene-parse PROPERTY CXX_SCAN_FOR_MODULES ON)

# ---------------------------------------------------------------------------
# wescene-vulkan-runtime  (Vulkan device, instance, swapchain, etc.)
# ---------------------------------------------------------------------------

add_library(
  wescene-vulkan-runtime STATIC
  Vulkan/Parameters.cpp
  Vulkan/Swapchain.cpp
  Vulkan/Instance.cpp
  Vulkan/Device.cpp
  Vulkan/StagingBuffer.cpp
  Vulkan/GraphicsPipeline.cpp
  Vulkan/TextureCache.cpp
  Vulkan/vulkan_wrapper.cpp
  Vulkan/Vma.cpp)
target_sources(
  wescene-vulkan-runtime
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES Vulkan/Vulkan.cppm)
target_link_libraries(
  wescene-vulkan-runtime
  PUBLIC ${Vulkan_LIBRARIES} wescene-base wescene-shader-compile)
target_include_directories(
  wescene-vulkan-runtime
  PUBLIC . Swapchain Utils/include Vulkan/include
  PRIVATE Utils Utils/include/Utils Vulkan)
target_compile_options(wescene-vulkan-runtime PRIVATE -Wno-missing-field-initializers)
set_property(TARGET wescene-vulkan-runtime PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET wescene-vulkan-runtime PROPERTY CXX_SCAN_FOR_MODULES ON)

# ---------------------------------------------------------------------------
# wescene-render  (RenderGraph + VulkanRender + SceneWallpaper)
# ---------------------------------------------------------------------------

add_library(
  wescene-render STATIC
  SceneWallpaper.cpp
  RenderGraph/DependencyGraph.cpp
  RenderGraph/PassNode.cpp
  RenderGraph/TexNode.cpp
  RenderGraph/RenderGraph.cpp
  VulkanRender/VulkanRender.cpp
  VulkanRender/SceneToRenderGraph.cpp
  VulkanRender/CopyPass.cpp
  VulkanRender/CustomShaderPass.cpp
  VulkanRender/FinPass.cpp
  VulkanRender/PrePass.cpp)
target_sources(
  wescene-render
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         SceneWallpaper.cppm
         RenderGraph/RGraph.cppm
         RenderGraph/DependencyGraph.cppm
         RenderGraph/PassNode.cppm
         RenderGraph/TexNode.cppm
         RenderGraph/RenderGraph.cppm
         VulkanRender/VulkanRender.cppm
         VulkanRender/VulkanPass.cppm
         VulkanRender/Resource.cppm
         VulkanRender/PassCommon.cppm
         VulkanRender/CopyPass.cppm
         VulkanRender/CustomShaderPass.cppm
         VulkanRender/FinPass.cppm
         VulkanRender/PrePass.cppm)
target_link_libraries(
  wescene-render
  PUBLIC wescene-vulkan-runtime wescene-parse wescene-script)
target_include_directories(
  wescene-render
  PUBLIC . Swapchain Utils/include RenderGraph/include Vulkan/include
  PRIVATE VulkanRender Utils Utils/include/Utils Scene Vulkan)
target_compile_options(wescene-render PRIVATE -Wno-missing-field-initializers)
set_property(TARGET wescene-render PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET wescene-render PROPERTY CXX_SCAN_FOR_MODULES ON)

# ---------------------------------------------------------------------------
# wescene-renderer (INTERFACE umbrella for back-compat).
# ---------------------------------------------------------------------------

add_library(wescene-renderer INTERFACE)
target_link_libraries(wescene-renderer
  INTERFACE wescene-render wescene-parse wescene-script
            wescene-vulkan-runtime wescene-shader-compile wescene-base)

endif()  # BUILD_WESCENE

# ============================================================================
# weweb-host  (CEF browser embedding for HTML/JS wallpapers)
# ============================================================================
if(BUILD_WEWEB)
    add_library(weweb-host STATIC
        Web/Manifest.cpp
        Web/UserProperties.cpp
        Web/AppHandler.cpp
        Web/ClientHandler.cpp
        Web/OsrRenderHandler.cpp
        Web/BrowserHost.cpp)
    target_include_directories(weweb-host PUBLIC Web)
    target_link_libraries(weweb-host
        PUBLIC libcef_lib libcef_dll_wrapper nlohmann_json)
    target_compile_features(weweb-host PUBLIC cxx_std_20)
    weweb_apply_cef_target_settings(weweb-host)
    set_property(TARGET weweb-host PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()
