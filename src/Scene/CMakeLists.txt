# ---------------------------------------------------------------------------
# owe-types — minimal types module (enums, BitFlags, Image, SpriteAnim)
# Independent of owe-base so consumers (e.g. owe-vulkan)
# can pick it up without dragging the rest of base in.
# ---------------------------------------------------------------------------
add_library(owe-types STATIC)
target_sources(owe-types
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES types/types.cppm)
target_sources(owe-types PRIVATE types/types.cpp)
target_link_libraries(owe-types
  PUBLIC rstd::rstd rstd.cppstd owe-core)
target_include_directories(owe-types PUBLIC ..)
target_compile_features(owe-types PUBLIC cxx_std_20)
set_property(TARGET owe-types PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-types PROPERTY CXX_SCAN_FOR_MODULES ON)


# ---------------------------------------------------------------------------
# owe-base
# ---------------------------------------------------------------------------
add_library(owe-base STATIC
  Utils/Sha.cpp
  Utils/FpsCounter.cpp
  Utils/Algorism.cpp
  Pkg/PkgFs.cpp
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
target_sources(owe-base
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         SpecNames.cppm
         Pkg/mod.cppm
         Pkg/AssetVersion.cppm
         Pkg/PkgFs.cppm
         Utils/Utils.cppm
         Timer/Timer.cppm
         Timer/ThreadTimer.cppm
         Timer/FrameTimer.cppm
         Scene/SceneVisibility.cppm
         Lighting/Lighting.cppm
         Scene/Scene.cppm)
target_link_libraries(owe-base
  PUBLIC rstd::rstd rstd.cppstd Eigen3::Eigen wavsen::audio owe-types
  PRIVATE ${LZ4_LIBRARIES}
          ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})
target_include_directories(owe-base
  PUBLIC  .. . Utils/include
  PRIVATE Utils Utils/include/Utils Scene Particle)
set_property(TARGET owe-base PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-base PROPERTY CXX_SCAN_FOR_MODULES ON)


# ---------------------------------------------------------------------------
# owe-script (QuickJS-NG runtime for the WE scenescript surface)
# ---------------------------------------------------------------------------
add_library(owe-script STATIC Script/Script.cpp)
target_sources(owe-script
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES Script/Script.cppm)
target_link_libraries(owe-script
  PUBLIC  owe-base
  PRIVATE qjs)
target_include_directories(owe-script
  PUBLIC  .. . Utils/include
  PRIVATE Utils Utils/include/Utils Script)
set_property(TARGET owe-script PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-script PROPERTY CXX_SCAN_FOR_MODULES ON)


# ---------------------------------------------------------------------------
# owe-parse (Pkg/Parse/* + Puppet + SceneUniformUpdater)
# ---------------------------------------------------------------------------
add_library(owe-parse STATIC
  SceneUniformUpdater.cpp
  Pkg/SceneObj/FieldBinding.cpp
  Pkg/SceneObj/ImageObject.cpp
  Pkg/SceneObj/LightObject.cpp
  Pkg/SceneObj/Material.cpp
  Pkg/Parse/WPMdlParser.cpp
  Pkg/SceneObj/ParticleObject.cpp
  Pkg/Parse/WPParticleParser.cpp
  Pkg/Parse/WPPuppet.cpp
  Pkg/SceneObj/SceneDocument.cpp
  Pkg/Parse/WPSceneParser.cpp
  Pkg/Parse/WPShaderParser.cpp
  Pkg/Parse/WPShaderAnnot.cpp
  Pkg/Parse/WPSoundParser.cpp
  Pkg/Parse/WPTexImageParser.cpp
  Text/Text.cpp)
target_sources(owe-parse
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         SceneUniformUpdater.cppm
         Pkg/Parse/mod.cppm
         Pkg/Parse/Compat.cppm
         Pkg/Parse/PuppetCompat.cppm
         Pkg/SceneObj/mod.cppm
         Pkg/SceneObj/AnimationLayer.cppm
         Pkg/SceneObj/FieldBinding.cppm
         Pkg/SceneObj/VisibilityBinding.cppm
         Pkg/SceneObj/ImageObject.cppm
         Pkg/SceneObj/LightObject.cppm
         Pkg/SceneObj/Material.cppm
         Pkg/Parse/WPMdlParser.cppm
         Pkg/SceneObj/MiscObject.cppm
         Pkg/SceneObj/ParticleObject.cppm
         Pkg/Parse/WPParticleParser.cppm
         Pkg/Parse/WPPuppet.cppm
         Pkg/SceneObj/SceneDocument.cppm
         Pkg/Parse/SceneStages.cppm
         Pkg/Parse/WPSceneParser.cppm
         Pkg/Parse/ShaderLex.cppm
         Pkg/Parse/WPShaderParser.cppm
         Pkg/SceneObj/SoundObject.cppm
         Pkg/Parse/WPSoundParser.cppm
         Pkg/Parse/WPTexImageParser.cppm
         Pkg/Parse/WPUniform.cppm
         Text/Text.cppm)
target_link_libraries(owe-parse
  PUBLIC owe-base owe-shader-compile owe-script
  PRIVATE ${LZ4_LIBRARIES} Freetype::Freetype PkgConfig::FONTCONFIG)
target_include_directories(owe-parse
  PUBLIC  .. . Utils/include
  PRIVATE Utils Utils/include/Utils Scene Particle)
set_property(TARGET owe-parse PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-parse PROPERTY CXX_SCAN_FOR_MODULES ON)


# ---------------------------------------------------------------------------
# owe-resource / owe-resource-registry
# ---------------------------------------------------------------------------
add_subdirectory(Resource)


# ---------------------------------------------------------------------------
# owe-rdg
# ---------------------------------------------------------------------------
add_library(owe-rdg STATIC
  RenderGraph/DependencyGraph.cpp
  RenderGraph/PassNode.cpp
  RenderGraph/TexNode.cpp
  RenderGraph/RenderGraph.cpp)
target_sources(owe-rdg
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         RenderGraph/RGraph.cppm
         RenderGraph/DependencyGraph.cppm
         RenderGraph/Pass.cppm
         RenderGraph/PassNode.cppm
         RenderGraph/TexNode.cppm
         RenderGraph/RenderGraph.cppm)
target_link_libraries(owe-rdg
  PUBLIC rstd::rstd rstd.cppstd owe-resource)
target_include_directories(owe-rdg PUBLIC .. .)
set_property(TARGET owe-rdg PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-rdg PROPERTY CXX_SCAN_FOR_MODULES ON)


# ---------------------------------------------------------------------------
# owe-vulkan-renderer
# ---------------------------------------------------------------------------
add_library(owe-vulkan-renderer STATIC
  VulkanRender/VulkanRender.cpp
  VulkanRender/BufferResolver.cpp
  VulkanRender/ShaderReflectionCache.cpp
  VulkanRender/SceneToRenderGraph.cpp
  VulkanRender/Pass/CopyPass.cpp
  VulkanRender/Pass/CustomShaderPass.cpp
  VulkanRender/Pass/FinPass.cpp
  VulkanRender/Pass/PrePass.cpp)
target_sources(owe-vulkan-renderer
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         VulkanRender/VulkanRender.cppm
         VulkanRender/Program.cppm
         VulkanRender/Pass/VulkanPass.cppm
         VulkanRender/ShaderReflectionCache.cppm
         VulkanRender/Resource.cppm
         VulkanRender/BufferResolver.cppm
         VulkanRender/Pass/PassCommon.cppm
         VulkanRender/Pass/CopyPass.cppm
         VulkanRender/Pass/CustomShaderPass.cppm
         VulkanRender/Pass/FinPass.cppm
         VulkanRender/Pass/PrePass.cppm)
target_link_libraries(owe-vulkan-renderer
  PUBLIC owe-rdg owe-resource-registry owe-vulkan owe-parse owe-script wavsen::audio)
target_include_directories(owe-vulkan-renderer
  PUBLIC  .. . Swapchain Utils/include ../Vulkan/include
  PRIVATE VulkanRender Utils Utils/include/Utils Scene ../Vulkan)
set_property(TARGET owe-vulkan-renderer PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-vulkan-renderer PROPERTY CXX_SCAN_FOR_MODULES ON)


# ---------------------------------------------------------------------------
# owe-render compatibility umbrella
# ---------------------------------------------------------------------------
add_library(owe-render INTERFACE)
target_link_libraries(owe-render INTERFACE owe-rdg owe-vulkan-renderer)


# ---------------------------------------------------------------------------
# owe-scene-wallpaper
# ---------------------------------------------------------------------------
add_library(owe-scene-wallpaper STATIC SceneWallpaper.cpp)
target_sources(owe-scene-wallpaper
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES SceneWallpaper.cppm)
target_link_libraries(owe-scene-wallpaper
  PUBLIC owe-render owe-resource-registry owe-parse owe-script wavsen::audio)
target_include_directories(owe-scene-wallpaper
  PUBLIC  .. . Swapchain Utils/include ../Vulkan/include
  PRIVATE VulkanRender Utils Utils/include/Utils Scene ../Vulkan)
set_property(TARGET owe-scene-wallpaper PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-scene-wallpaper PROPERTY CXX_SCAN_FOR_MODULES ON)
