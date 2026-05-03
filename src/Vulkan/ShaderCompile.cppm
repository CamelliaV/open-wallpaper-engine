module;

// Vulkan headers only — no dispatch / loader. ShaderReflected exposes
// VkDescriptorSetLayoutBinding and VkFormat, which SPIRV-Reflect produces
// natively, so consumers can hand the reflection straight to a pipeline
// builder without translating types. We do NOT link Vulkan_LIBRARIES from
// this module's target; downstream binaries that only want shader
// compilation get the headers but no libvulkan dependency.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "Core/MapSet.hpp"
#include "Type.hpp"

export module wescene.shader_compile;
import cppstd;

export namespace wallpaper::vulkan
{

// ---------- Spv.hpp ----------

struct ShaderSpv {
    std::string entry_point { "main" };
    ShaderType  stage;

    std::vector<unsigned int> spirv;
};

using Uni_ShaderSpv = std::unique_ptr<ShaderSpv>;

// ---------- ShaderReflect.hpp ----------

struct ShaderReflected {
    struct BlockedUniform {
        int    block_index;
        unsigned offset;
        std::size_t size { 0 };
        std::size_t num { 1 };
    };
    struct Block {
        int         index;
        unsigned    size;
        std::string name;

        Map<std::string, BlockedUniform> member_map;
    };
    std::vector<Block> blocks;

    Map<std::string, VkDescriptorSetLayoutBinding> binding_map;

    struct Input {
        unsigned location;
        VkFormat format;
    };
    Map<std::string, Input> input_location_map;
};

bool GenReflect(std::span<const std::vector<unsigned int>> codes,
                std::vector<Uni_ShaderSpv>&                spvs,
                ShaderReflected&                           ref);

// ---------- ShaderComp.hpp ----------

enum class VulkanTarget : unsigned
{
    Vulkan_1_0,
    Vulkan_1_1,
    Vulkan_1_2,
    Vulkan_1_3,
};

struct ShaderCompUnit {
    ShaderType  stage;
    std::string src;
    std::string entry_point; // if empty, "main_<stage>" is used.
};

struct ShaderCompOpt {
    VulkanTarget target { VulkanTarget::Vulkan_1_1 };
    bool         optimize { false };
};

bool CompileAndLinkShaderUnits(std::span<const ShaderCompUnit> compUnits,
                               const ShaderCompOpt&            opt,
                               std::vector<Uni_ShaderSpv>&     spvs);

// Run DXC in -P (preprocess-only) mode. Expands every `#if`, `#include`
// and `#define` so downstream regex passes see only live declarations
// with macros already resolved (e.g. `g_Bones[BONECOUNT]` becomes
// `g_Bones[4]`, `#if SKINNING=0` blocks vanish entirely). On failure
// returns false and leaves `out` untouched.
bool Preprocess(std::string_view src, std::string& out);

} // namespace wallpaper::vulkan
