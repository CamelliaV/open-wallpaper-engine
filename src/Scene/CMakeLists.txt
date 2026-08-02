# ---------------------------------------------------------------------------
# owe-scene-audio-response
# ---------------------------------------------------------------------------
add_library(owe-scene-audio-response STATIC Audio/SceneAudioResponse.cpp)
target_sources(owe-scene-audio-response
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES Audio/SceneAudioResponse.cppm)
target_link_libraries(owe-scene-audio-response PUBLIC owe-audio-response rstd::rstd)
target_compile_features(owe-scene-audio-response PUBLIC cxx_std_20)
set_property(TARGET owe-scene-audio-response PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-scene-audio-response PROPERTY CXX_SCAN_FOR_MODULES ON)

# ---------------------------------------------------------------------------
# owe-load-bench
# ---------------------------------------------------------------------------
add_library(owe-load-bench STATIC)
target_sources(owe-load-bench
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES LoadBench.cppm)
target_link_libraries(owe-load-bench PUBLIC rstd::bench)
target_compile_features(owe-load-bench PUBLIC cxx_std_20)
set_property(TARGET owe-load-bench PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-load-bench PROPERTY CXX_SCAN_FOR_MODULES ON)

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
  Scene/SceneNodeLayer.cpp
  Scene/SceneIndexArray.cpp
  Scene/SceneNode.cpp
  Scene/SceneVertexArray.cpp
  Scene/SceneShader.cpp)
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
         Particle/Particle.cppm
         Particle/ParticleProgram.cppm
         Scene/Id.cppm
         Scene/Runtime.cppm
         Scene/SceneVisibility.cppm
         Scene/Uniform.cppm
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
  PUBLIC  owe-base owe-scene-audio-response
  PRIVATE qjs)
target_include_directories(owe-script
  PUBLIC  .. . Utils/include
  PRIVATE Utils Utils/include/Utils Script)
set_property(TARGET owe-script PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-script PROPERTY CXX_SCAN_FOR_MODULES ON)


# ---------------------------------------------------------------------------
# owe-parse
# ---------------------------------------------------------------------------
add_library(owe-parse STATIC
  Pkg/SceneObj/FieldBinding.cpp
  Pkg/SceneObj/ImageObject.cpp
  Pkg/SceneObj/LightObject.cpp
  Pkg/SceneObj/Material.cpp
  Pkg/SceneObj/Object.cpp
  Pkg/Parse/Puppet/MdlParser.cpp
  Pkg/Parse/Particle/ParticleEmitter.cpp
  Pkg/Parse/Particle/ParticleRawGenerator.cpp
  Pkg/Parse/Particle/ParticleRuntime.cpp
  Pkg/SceneObj/ParticleObject.cpp
  Pkg/Parse/Particle/ParticleParser.cpp
  Pkg/Parse/Puppet/Puppet.cpp
  Pkg/Parse/Uniform/UniformSource.cpp
  Pkg/SceneObj/SceneDocument.cpp
  Pkg/Parse/Scene/SceneAnimationBinding.cpp
  Pkg/Parse/Scene/SceneContext.cpp
  Pkg/Parse/Scene/SceneDynamicAsset.cpp
  Pkg/Parse/Scene/SceneFinalize.cpp
  Pkg/Parse/Scene/SceneGeometryBuilder.cpp
  Pkg/Parse/Image/SceneImageObjectParser.cpp
  Pkg/Parse/Scene/SceneMaterialBuilder.cpp
  Pkg/Parse/Scene/SceneObjectExpansion.cpp
  Pkg/Parse/Scene/SceneObjectParsers.cpp
  Pkg/Parse/Particle/SceneParticleObjectParser.cpp
  Pkg/Parse/Scene/ScenePostProcess.cpp
  Pkg/Parse/Scene/SceneScriptBinding.cpp
  Pkg/Parse/Scene/SceneTextObjectParser.cpp
  Pkg/Parse/Scene/SceneParser.cpp
  Pkg/Parse/Shader/ShaderParser.cpp
  Pkg/Parse/Shader/ShaderAnnot.cpp
  Pkg/Parse/Uniform/Uniform.cpp
  Pkg/Parse/Sound/SoundParser.cpp
  Pkg/Parse/Image/TexImageParser.cpp
  Text/Text.cpp)
target_sources(owe-parse
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         Pkg/Parse/mod.cppm
         Pkg/Parse/Compat.cppm
         Pkg/Parse/Puppet/PuppetCompat.cppm
         Pkg/Parse/Particle/ParticleRuntime.cppm
         Pkg/SceneObj/mod.cppm
         Pkg/SceneObj/AnimationLayer.cppm
         Pkg/SceneObj/FieldBinding.cppm
         Pkg/SceneObj/VisibilityBinding.cppm
         Pkg/SceneObj/ImageObject.cppm
         Pkg/SceneObj/LightObject.cppm
         Pkg/SceneObj/Material.cppm
         Pkg/Parse/Puppet/MdlParser.cppm
         Pkg/SceneObj/MiscObject.cppm
         Pkg/SceneObj/ParticleObject.cppm
         Pkg/Parse/Particle/ParticleParser.cppm
         Pkg/Parse/Puppet/Puppet.cppm
         Pkg/Parse/Uniform/GlobalUniform.cppm
         Pkg/Parse/Uniform/UniformSource.cppm
         Pkg/SceneObj/SceneDocument.cppm
         Pkg/Parse/Scene/SceneStages.cppm
         Pkg/Parse/Scene/SceneParser.cppm
         Pkg/Parse/Shader/ShaderLex.cppm
         Pkg/Parse/Shader/ShaderParser.cppm
         Pkg/SceneObj/SoundObject.cppm
         Pkg/SceneObj/Object.cppm
         Pkg/Parse/Sound/SoundParser.cppm
         Pkg/Parse/Image/TexImageParser.cppm
         Pkg/Parse/Uniform/Uniform.cppm
         Text/Text.cppm)
target_sources(owe-parse
  PRIVATE FILE_SET internal_cxx_modules TYPE CXX_MODULES FILES
          Pkg/Parse/Scene/SceneContext.cppm)
target_link_libraries(owe-parse
  PUBLIC owe-base owe-load-bench owe-shader-compile owe-script owe-scene-audio-response
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
  VulkanRender/UniformBuffer.cpp
  VulkanRender/SceneToRenderGraph.cpp
  VulkanRender/Pass/CopyPass.cpp
  VulkanRender/Pass/CustomShaderPass.cpp
  VulkanRender/Pass/FinPass.cpp
  VulkanRender/Pass/PrePass.cpp)
target_sources(owe-vulkan-renderer
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         VulkanRender/VulkanRender.cppm
         VulkanRender/Program.cppm
         VulkanRender/PipelineLayout.cppm
         VulkanRender/Pass/VulkanPass.cppm
         VulkanRender/ShaderReflectionCache.cppm
         VulkanRender/UniformBuffer.cppm
         VulkanRender/Resource.cppm
         VulkanRender/BufferResolver.cppm
         VulkanRender/Pass/PassCommon.cppm
         VulkanRender/Pass/CopyPass.cppm
         VulkanRender/Pass/CustomShaderPass.cppm
         VulkanRender/Pass/FinPass.cppm
         VulkanRender/Pass/PrePass.cppm)
target_link_libraries(owe-vulkan-renderer
  PUBLIC owe-rdg owe-resource-registry owe-vulkan owe-parse owe-script owe-load-bench wavsen::audio)
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


if(BUILD_TESTS)
  add_executable(owe-scene-audio-response-tests ../../tests/SceneAudioResponseTests.cpp)
  target_link_libraries(owe-scene-audio-response-tests PRIVATE owe-scene-audio-response)
  target_compile_features(owe-scene-audio-response-tests PRIVATE cxx_std_20)
  set_property(TARGET owe-scene-audio-response-tests PROPERTY CXX_SCAN_FOR_MODULES ON)
  add_test(NAME owe-scene-audio-response-tests COMMAND owe-scene-audio-response-tests)

  add_executable(owe-scene-audio-response-dump ../../tests/SceneAudioResponseDump.cpp)
  target_link_libraries(owe-scene-audio-response-dump
    PRIVATE owe-scene-audio-response owe-cli rstd::json wavsen::audio)
  target_compile_features(owe-scene-audio-response-dump PRIVATE cxx_std_20)
  set_property(TARGET owe-scene-audio-response-dump PROPERTY CXX_SCAN_FOR_MODULES ON)
endif()


# ---------------------------------------------------------------------------
# owe-scene-wallpaper
# ---------------------------------------------------------------------------
add_library(owe-scene-wallpaper STATIC SceneWallpaper.cpp SceneUserProperty.cpp)
target_sources(owe-scene-wallpaper
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         SceneWallpaper.cppm
         SceneUserProperty.cppm)
target_link_libraries(owe-scene-wallpaper
  PUBLIC owe-render owe-resource-registry owe-parse owe-script owe-load-bench
         owe-audio-response owe-scene-audio-response wavsen::audio)
target_include_directories(owe-scene-wallpaper
  PUBLIC  .. . Swapchain Utils/include ../Vulkan/include
  PRIVATE VulkanRender Utils Utils/include/Utils Scene ../Vulkan)
set_property(TARGET owe-scene-wallpaper PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET owe-scene-wallpaper PROPERTY CXX_SCAN_FOR_MODULES ON)
