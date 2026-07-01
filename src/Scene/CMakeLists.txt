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
target_compile_options(owe-types PRIVATE -Wno-missing-field-initializers)
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
         SpecTexs.cppm
         Pkg/mod.cppm
         Pkg/AssetVersion.cppm
         Pkg/PkgFs.cppm
         Utils/Utils.cppm
         MessageLoop/MessageLoop.cppm
         Timer/Timer.cppm
         Timer/ThreadTimer.cppm
         Timer/FrameTimer.cppm
         Scene/SceneVisibility.cppm
         Lighting/Lighting.cppm
         Scene/Scene.cppm)
target_link_libraries(owe-base
  PUBLIC rstd::rstd rstd.cppstd Eigen3::Eigen wavsen::audio owe-types nlohmann_json
  PRIVATE ${LZ4_LIBRARIES}
          ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})
target_include_directories(owe-base
  PUBLIC  .. . Utils/include
  PRIVATE Utils Utils/include/Utils Scene Particle)
target_compile_options(owe-base PRIVATE -Wno-missing-field-initializers)
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
  PRIVATE qjs nlohmann_json)
target_include_directories(owe-script
  PUBLIC  .. . Utils/include
  PRIVATE Utils Utils/include/Utils Script)
target_compile_options(owe-script PRIVATE -Wno-missing-field-initializers)
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
  PRIVATE nlohmann_json ${LZ4_LIBRARIES} Freetype::Freetype PkgConfig::FONTCONFIG)
target_include_directories(owe-parse
  PUBLIC  .. . Utils/include
  PRIVATE Utils Utils/include/Utils Scene Particle)
target_compile_options(owe-parse PRIVATE -Wno-missing-field-initializers)
set_property(TARGET owe-parse PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-parse PROPERTY CXX_SCAN_FOR_MODULES ON)


# ---------------------------------------------------------------------------
# owe-render (RenderGraph + VulkanRender + SceneWallpaper)
# ---------------------------------------------------------------------------
add_library(owe-render STATIC
  SceneWallpaper.cpp
  RenderGraph/DependencyGraph.cpp
  RenderGraph/PassNode.cpp
  RenderGraph/TexNode.cpp
  RenderGraph/RenderGraph.cpp
  VulkanRender/VulkanRender.cpp
  VulkanRender/BufferResolver.cpp
  VulkanRender/ShaderReflectionCache.cpp
  VulkanRender/SceneToRenderGraph.cpp
  VulkanRender/CopyPass.cpp
  VulkanRender/CustomShaderPass.cpp
  VulkanRender/FinPass.cpp
  VulkanRender/PrePass.cpp)
target_sources(owe-render
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         SceneWallpaper.cppm
         RenderGraph/RGraph.cppm
         RenderGraph/DependencyGraph.cppm
         RenderGraph/PassNode.cppm
         RenderGraph/TexNode.cppm
         RenderGraph/RenderGraph.cppm
         VulkanRender/VulkanRender.cppm
         VulkanRender/VulkanPass.cppm
         VulkanRender/ShaderReflectionCache.cppm
         VulkanRender/ResourceKey.cppm
         VulkanRender/Resource.cppm
         VulkanRender/BufferResolver.cppm
         VulkanRender/PassCommon.cppm
         VulkanRender/CopyPass.cppm
         VulkanRender/CustomShaderPass.cppm
         VulkanRender/FinPass.cppm
         VulkanRender/PrePass.cppm)
target_link_libraries(owe-render
  PUBLIC owe-vulkan owe-parse owe-script wavsen::audio)
target_include_directories(owe-render
  PUBLIC  .. . Swapchain Utils/include RenderGraph/include ../Vulkan/include
  PRIVATE VulkanRender Utils Utils/include/Utils Scene ../Vulkan)
target_compile_options(owe-render PRIVATE -Wno-missing-field-initializers)
set_property(TARGET owe-render PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-render PROPERTY CXX_SCAN_FOR_MODULES ON)
