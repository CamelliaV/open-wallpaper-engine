find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
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

include_directories(.)

add_library(
  ${PROJECT_NAME} STATIC
  Type.cpp
  WPShaderValueUpdater.cpp
  Utils/Logging.cpp
  Utils/Sha.cpp
  RenderGraph/DependencyGraph.cpp
  RenderGraph/PassNode.cpp
  RenderGraph/TexNode.cpp
  RenderGraph/RenderGraph.cpp
  Vulkan/Vma.cpp)

# C++20 module: wescene.scene_wallpaper. Interface unit + impl unit live
# in the same target. Consumers (viewer, waywallen host) `import` it.
target_sources(
  ${PROJECT_NAME}
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         SceneWallpaper.cppm
         WPCommon.cppm
         WPJson.cppm
         WPPkgFs.cppm
         WPShaderValueUpdater.cppm
         Utils/Utils.cppm
         Audio/SoundManager.cppm
         Looper/Looper.cppm
         Timer/Timer.cppm
         Timer/ThreadTimer.cppm
         Timer/FrameTimer.cppm
         Fs/Fs.cppm
         Scene/Scene.cppm
         Vulkan/Vulkan.cppm
         VulkanRender/VulkanRender.cppm
         VulkanRender/VulkanPass.cppm
         VulkanRender/Resource.cppm
         VulkanRender/PassCommon.cppm
         VulkanRender/CopyPass.cppm
         VulkanRender/CustomShaderPass.cppm
         VulkanRender/FinPass.cppm
         VulkanRender/PrePass.cppm
         RenderGraph/RGraph.cppm
         RenderGraph/DependencyGraph.cppm
         RenderGraph/PassNode.cppm
         RenderGraph/TexNode.cppm
         RenderGraph/RenderGraph.cppm
         Parse/Parse.cppm
         Parse/WPImageObject.cppm
         Parse/WPLightObject.cppm
         Parse/WPMaterial.cppm
         Parse/WPMdlParser.cppm
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
  PRIVATE
         SceneWallpaper.cpp
         WPJson.cpp
         WPPkgFs.cpp
         Utils/DynamicLibrary.cpp
         Utils/FpsCounter.cpp
         Utils/Algorism.cpp
         Vulkan/Parameters.cpp
         Vulkan/Swapchain.cpp
         Vulkan/Instance.cpp
         Vulkan/Device.cpp
         Vulkan/StagingBuffer.cpp
         Vulkan/GraphicsPipeline.cpp
         Vulkan/Shader.cpp
         Vulkan/TextureCache.cpp
         Vulkan/vulkan_wrapper.cpp
         Audio/SoundManager.cpp
         Looper/Looper.cpp
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
         Particle/WPParticleRawGener.cpp
         VulkanRender/VulkanRender.cpp
         VulkanRender/SceneToRenderGraph.cpp
         VulkanRender/CopyPass.cpp
         VulkanRender/CustomShaderPass.cpp
         VulkanRender/FinPass.cpp
         VulkanRender/PrePass.cpp
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
         Parse/WPTexImageParser.cpp)

find_package(Vulkan REQUIRED)

target_link_libraries(
  ${PROJECT_NAME}
  PUBLIC ${Vulkan_LIBRARIES} glslang
  PRIVATE nlohmann_json ${LZ4_LIBRARIES}
          SPIRV spirv-reflect-static miniaudio
          ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS} rstd::rstd rstd.cppstd)

target_include_directories(${PROJECT_NAME}
  PUBLIC . Swapchain Utils/include RenderGraph/include Vulkan/include
  PRIVATE VulkanRender
          Audio
          Utils Utils/include/Utils
          Scene
          Particle
          Vulkan)
target_compile_options(${PROJECT_NAME} PRIVATE -Wno-missing-field-initializers)
set_property(TARGET ${PROJECT_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_SCAN_FOR_MODULES ON)
