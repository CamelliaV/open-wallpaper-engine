module;

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <rstd/macro.hpp>
#include <dxc/dxcapi.h>
#include <spirv_reflect.h>

#include "Utils/Sha.hpp"
module wescene.shader_compile;
import wescene.core;
import wescene.types;
import cppstd;
import rstd.log;
import rstd.cppstd;

using namespace owe;
using namespace owe::vulkan;

namespace
{
// Spill a payload to /tmp/<sha1> for post-mortem inspection. Returns the
// written path so callers can mention it in the error message.
std::string logToTmpfileWithSha1(std::span<const char> in, const char* fmt, ...) {
    std::va_list          args;
    std::string           name   = utils::genSha1(in);
    std::filesystem::path fspath = std::filesystem::temp_directory_path() / name;
    std::string           path   = fspath.native();
    auto*                 file   = std::fopen(path.c_str(), "w+");
    if (! file) return path;
    {
        va_start(args, fmt);
        std::vfprintf(file, fmt, args);
        va_end(args);
    }
    std::fprintf(file, "\n");
    std::fclose(file);
    return path;
}
} // namespace

namespace
{

inline VkShaderStageFlagBits ToVkType(owe::ShaderType s) {
    switch (s) {
    case ShaderType::VERTEX:   return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderType::FRAGMENT: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderType::GEOMETRY: return VK_SHADER_STAGE_GEOMETRY_BIT;
    }
    assert(false);
    return VK_SHADER_STAGE_VERTEX_BIT;
}

inline VkFormat ToVkType(SpvReflectFormat type) { return static_cast<VkFormat>(type); }

inline VkShaderStageFlagBits ToVkType(SpvReflectShaderStageFlagBits s) {
    switch (s) {
    case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT:   return VK_SHADER_STAGE_VERTEX_BIT;
    case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT: return VK_SHADER_STAGE_GEOMETRY_BIT;
    default: assert(false); return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

inline owe::ShaderType FromSpvStage(SpvReflectShaderStageFlagBits s) {
    switch (s) {
    case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT:   return ShaderType::VERTEX;
    case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT: return ShaderType::FRAGMENT;
    case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT: return ShaderType::GEOMETRY;
    default: assert(false); return ShaderType::VERTEX;
    }
}

template<typename VEC, typename FUNC>
bool EnumAllRef(VEC& vec, FUNC&& func) {
    unsigned count { 0 };
    auto result = func(&count, nullptr);
    assert(result == SPV_REFLECT_RESULT_SUCCESS);
    vec.resize(count);
    result = func(&count, vec.data());
    assert(result == SPV_REFLECT_RESULT_SUCCESS);
    return result == SPV_REFLECT_RESULT_SUCCESS;
}

inline LPCWSTR DxcStageProfile(owe::ShaderType s) {
    switch (s) {
    case ShaderType::VERTEX:   return L"vs_6_0";
    case ShaderType::FRAGMENT: return L"ps_6_0";
    case ShaderType::GEOMETRY: return L"gs_6_0";
    }
    assert(false);
    return L"vs_6_0";
}

inline const char* DefaultEntryName(owe::ShaderType s) {
    switch (s) {
    case ShaderType::VERTEX:   return "main_vs";
    case ShaderType::FRAGMENT: return "main_ps";
    case ShaderType::GEOMETRY: return "main_gs";
    }
    assert(false);
    return "main";
}

inline LPCWSTR TargetEnvFlag(VulkanTarget target) {
    switch (target) {
    case VulkanTarget::Vulkan_1_0: return L"-fspv-target-env=vulkan1.0";
    case VulkanTarget::Vulkan_1_1: return L"-fspv-target-env=vulkan1.1";
    case VulkanTarget::Vulkan_1_2: return L"-fspv-target-env=vulkan1.2";
    case VulkanTarget::Vulkan_1_3: return L"-fspv-target-env=vulkan1.3";
    }
    return L"-fspv-target-env=vulkan1.1";
}

// DXC takes wide-string args. WE shaders are pure-ASCII identifiers, so a
// straight char→wchar_t widening is sufficient.
inline std::wstring ToWide(std::string_view s) {
    std::wstring w;
    w.reserve(s.size());
    for (auto c : s) w.push_back(static_cast<wchar_t>(c));
    return w;
}

// COM helper: scope-bound Release on any IDxc* pointer.
template<typename T>
struct ComRelease {
    void operator()(T* p) const noexcept {
        if (p) p->Release();
    }
};
template<typename T>
using ComPtr = std::unique_ptr<T, ComRelease<T>>;

// IDxcUtils / IDxcCompiler3 / DefaultIncludeHandler are COM thread-affine
// but otherwise reusable across compile calls — DXC has no per-call state
// that would invalidate them. Cached per thread; raw pointers returned
// are non-owning (lifetime tied to the thread_local ComPtr).
struct DxcCtx {
    IDxcUtils*          utils;
    IDxcCompiler3*      compiler;
    IDxcIncludeHandler* default_include;
};

inline DxcCtx GetDxcCtx() {
    static thread_local ComPtr<IDxcUtils>          tl_utils;
    static thread_local ComPtr<IDxcCompiler3>      tl_compiler;
    static thread_local ComPtr<IDxcIncludeHandler> tl_include;

    if (! tl_utils) {
        IDxcUtils* raw = nullptr;
        HRESULT    hr  = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&raw));
        if (FAILED(hr) || ! raw) {
            rstd_error("dxc: DxcCreateInstance(DxcUtils) failed: 0x{:x}", (unsigned long)hr);
            return {};
        }
        tl_utils.reset(raw);
    }
    if (! tl_compiler) {
        IDxcCompiler3* raw = nullptr;
        HRESULT        hr  = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&raw));
        if (FAILED(hr) || ! raw) {
            rstd_error("dxc: DxcCreateInstance(DxcCompiler) failed: 0x{:x}", (unsigned long)hr);
            return {};
        }
        tl_compiler.reset(raw);
    }
    if (! tl_include) {
        IDxcIncludeHandler* raw = nullptr;
        tl_utils->CreateDefaultIncludeHandler(&raw);
        if (! raw) {
            rstd_error("dxc: CreateDefaultIncludeHandler failed");
            return {};
        }
        tl_include.reset(raw);
    }
    return { tl_utils.get(), tl_compiler.get(), tl_include.get() };
}

} // namespace

bool owe::vulkan::GenReflect(std::span<const std::vector<unsigned>> codes,
                                   std::vector<Uni_ShaderSpv>& spvs, ShaderReflected& ref) {
    spvs.clear();
    for (const auto& code : codes) {
        spv_reflect::ShaderModule spv_ref(code, SPV_REFLECT_MODULE_FLAG_NO_COPY);
        VkShaderStageFlagBits     stage = ::ToVkType(spv_ref.GetShaderStage());
        {
            Uni_ShaderSpv spv  = std::make_unique<ShaderSpv>();
            spv->stage         = ::FromSpvStage(spv_ref.GetShaderStage());
            spv->spirv         = code;
            // SPIRV-Reflect gives us the entry-point name baked into the
            // module — use it so the pipeline's pName matches what DXC
            // produced (e.g. "main_vs" / "main_ps") instead of defaulting
            // to "main" and tripping VUID-VkPipelineShaderStageCreateInfo.
            if (const char* ep = spv_ref.GetEntryPointName(); ep && ep[0] != '\0') {
                spv->entry_point = ep;
            }
            spvs.emplace_back(std::move(spv));
        }
        std::vector<SpvReflectInterfaceVariable*> inputs;
        std::vector<SpvReflectDescriptorBinding*> bindings;

        bool ok = EnumAllRef(bindings, [&](auto&&... args) {
            return spv_ref.EnumerateDescriptorBindings(args...);
        });
        if (! ok) return false;

        VkDescriptorSetLayoutBinding vkbinding {};
        vkbinding.stageFlags = stage;

        for (auto pb : bindings) {
            auto& b = *pb;
            if (! b.accessed) continue;

            auto bind_name = std::string(b.name).empty() && b.type_description->type_name != nullptr
                                 ? b.type_description->type_name
                                 : b.name;

            if (exists(ref.binding_map, bind_name)) {
                auto& bind = ref.binding_map[bind_name];
                bind.stageFlags |= stage;
                continue;
            }
            if (b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                auto& block      = b.block;
                auto  block_name = std::string(block.name).empty() ? bind_name : block.name;
                ref.blocks.push_back(ShaderReflected::Block { //.index = i,
                                                              .size       = block.size,
                                                              .name       = block.name,
                                                              .member_map = {} });
                auto& ref_block = ref.blocks.front();

                vkbinding.binding         = b.binding;
                vkbinding.descriptorCount = 1;
                vkbinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

                for (u32 i = 0; i < block.member_count; i++) {
                    auto&                           unif = block.members[i];
                    ShaderReflected::BlockedUniform bunif {};
                    {
                        bunif.size   = unif.size;
                        bunif.offset = unif.offset;
                    }
                    ref_block.member_map[unif.name] = bunif;
                }
            } else if (b.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                vkbinding.binding         = b.binding;
                vkbinding.descriptorCount = 1;
                vkbinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            } else {
                rstd_error("unknown DescriptorBinding {}", (int)b.descriptor_type);
                return false;
            }

            ref.binding_map[bind_name] = vkbinding;
        }

        if (stage == VK_SHADER_STAGE_VERTEX_BIT) {
            EnumAllRef(inputs, [&](auto&&... args) {
                return spv_ref.EnumerateInputVariables(args...);
            });

            for (auto pinput : inputs) {
                auto& input = *pinput;
                if (owe::sstart_with(input.name, "gl_")) continue;

                if (input.location == std::numeric_limits<decltype(input.location)>::max()) {
                    rstd_error("shader input {} no location", input.name);
                    return false;
                }
                ShaderReflected::Input rinput;
                rinput.location = input.location;
                rinput.format   = ::ToVkType(input.format);

                // DXC names input vars `in.var.<SEMANTIC>`. The synthesizer
                // sets the semantic to the original attribute name (a_X),
                // so stripping the `in.var.` prefix yields a key the C++
                // vertex-buffer setup can match against `attrs_map`.
                std::string_view name = input.name;
                if (name.starts_with("in.var.")) name.remove_prefix(7);
                ref.input_location_map[std::string(name)] = rinput;
            }
        }
    }
    return true;
}

bool owe::vulkan::Preprocess(std::string_view src, std::string& out) {
    DxcCtx ctx = GetDxcCtx();
    if (! ctx.compiler) return false;

    // -P alone (no filename) writes the preprocessed text to DXC_OUT_HLSL.
    std::vector<LPCWSTR> args { L"-P" };

    DxcBuffer source_buf {};
    source_buf.Ptr      = src.data();
    source_buf.Size     = src.size();
    source_buf.Encoding = DXC_CP_UTF8;

    IDxcResult* result_raw = nullptr;
    HRESULT     hr         = ctx.compiler->Compile(&source_buf,
                                       args.data(),
                                       static_cast<UINT32>(args.size()),
                                       ctx.default_include,
                                       IID_PPV_ARGS(&result_raw));
    if (FAILED(hr) || ! result_raw) {
        rstd_error("dxc(preprocess): IDxcCompiler3::Compile failed: 0x{:x}",
                  (unsigned long)hr);
        return false;
    }
    ComPtr<IDxcResult> result(result_raw);

    HRESULT compile_status = E_FAIL;
    result->GetStatus(&compile_status);
    const bool failed = FAILED(compile_status);

    IDxcBlobUtf8* errors_raw = nullptr;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors_raw), nullptr);
    ComPtr<IDxcBlobUtf8> errors(errors_raw);
    if (errors && errors->GetStringLength() > 0) {
        if (failed) {
            std::string tmp_name = logToTmpfileWithSha1(std::string(src), "%.*s",
                                                        static_cast<int>(src.size()), src);
            rstd_error("dxc(preprocess): {}",
                      std::string_view(errors->GetStringPointer(),
                                       errors->GetStringLength()));
            rstd_error("shader source is at {}", tmp_name);
        } else {
            rstd_warn("dxc(preprocess): {}",
                     std::string_view(errors->GetStringPointer(),
                                      errors->GetStringLength()));
        }
    }
    if (failed) return false;

    IDxcBlobUtf8* hlsl_raw = nullptr;
    result->GetOutput(DXC_OUT_HLSL, IID_PPV_ARGS(&hlsl_raw), nullptr);
    ComPtr<IDxcBlobUtf8> hlsl(hlsl_raw);
    if (! hlsl || hlsl->GetStringLength() == 0) {
        rstd_error("dxc(preprocess): no preprocessed output");
        return false;
    }

    out.assign(hlsl->GetStringPointer(), hlsl->GetStringLength());
    return true;
}

bool owe::vulkan::CompileAndLinkShaderUnits(std::span<const ShaderCompUnit>  compUnits,
                                                  const ShaderCompOpt&        opt,
                                                  std::vector<Uni_ShaderSpv>& spvs) {
    DxcCtx ctx = GetDxcCtx();
    if (! ctx.compiler) return false;

    spvs.clear();
    spvs.reserve(compUnits.size());

    for (const auto& unit : compUnits) {
        const std::wstring entry =
            ToWide(unit.entry_point.empty() ? DefaultEntryName(unit.stage) : unit.entry_point);

        std::vector<LPCWSTR> args;
        args.push_back(L"-T");        args.push_back(DxcStageProfile(unit.stage));
        args.push_back(L"-E");        args.push_back(entry.c_str());
        args.push_back(L"-spirv");
        args.push_back(TargetEnvFlag(opt.target));
        // Pack matrices column-major to match the C++ side's glm uploads.
        args.push_back(L"-Zpc");
        // Force std140 cbuffer layout. Default DX packing lets scalars
        // share a 16-byte slot which Vulkan's default cbuffer layout
        // doesn't allow — RADV reads such cbuffers inconsistently and
        // SPIRV-Cross outright rejects them ("Buffer block cannot be
        // expressed as any of std430, std140, scalar"). std140 also
        // matches what the C++ uploader was originally written for under
        // glslang, so reflection-reported offsets line up with what the
        // host data structure expects.
        args.push_back(L"-fvk-use-gl-layout");
        // No -fvk-bind-globals: WPShaderParser strips `uniform TYPE NAME;`
        // declarations and re-emits them as members of an explicit shared
        // `cbuffer ww_Uniforms` at [[vk::binding(0, 0)]] with the cross-
        // stage union of names in alphabetic order. That keeps VS and FS
        // looking at the same cbuffer layout — under -fvk-bind-globals
        // each stage's $Globals had a different field set and FS-only
        // uniforms (g_Brightness, g_UserAlpha) read as zero because the
        // C++ uploader laid out the buffer per-VS-reflection.
        if (opt.optimize) {
            args.push_back(L"-O3");
        } else {
            args.push_back(L"-Od");
        }

        DxcBuffer source_buf {};
        source_buf.Ptr      = unit.src.data();
        source_buf.Size     = unit.src.size();
        source_buf.Encoding = DXC_CP_UTF8;

        IDxcResult* result_raw = nullptr;
        HRESULT     hr         = ctx.compiler->Compile(&source_buf,
                                       args.data(),
                                       static_cast<UINT32>(args.size()),
                                       ctx.default_include,
                                       IID_PPV_ARGS(&result_raw));
        if (FAILED(hr) || ! result_raw) {
            rstd_error("dxc(compile): IDxcCompiler3::Compile failed: 0x{:x}", (unsigned long)hr);
            return false;
        }
        ComPtr<IDxcResult> result(result_raw);

        IDxcBlobUtf8* errors_raw = nullptr;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors_raw), nullptr);
        ComPtr<IDxcBlobUtf8> errors(errors_raw);

        HRESULT compile_status = E_FAIL;
        result->GetStatus(&compile_status);
        const bool compile_failed = FAILED(compile_status);

        if (errors && errors->GetStringLength() > 0) {
            // The errors blob from DXC carries both warnings and errors.
            // Only escalate to ERROR severity on actual compile failure;
            // warnings ride at WARN. The temp-file path is only useful
            // when the compile actually failed.
            if (compile_failed) {
                std::string tmp_name = logToTmpfileWithSha1(unit.src, "%s", unit.src.c_str());
                rstd_error("dxc(compile): {}",
                          std::string_view(errors->GetStringPointer(),
                                           errors->GetStringLength()));
                rstd_error("shader source is at {}", tmp_name);
            } else {
                rstd_warn("dxc(compile): {}",
                         std::string_view(errors->GetStringPointer(),
                                          errors->GetStringLength()));
            }
        }

        if (compile_failed) {
            return false;
        }

        IDxcBlob* spv_blob_raw = nullptr;
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&spv_blob_raw), nullptr);
        ComPtr<IDxcBlob> spv_blob(spv_blob_raw);
        if (! spv_blob || spv_blob->GetBufferSize() == 0) {
            rstd_error("dxc(compile): no SPIR-V output produced");
            return false;
        }

        Uni_ShaderSpv spv = std::make_unique<ShaderSpv>();
        spv->stage        = unit.stage;
        spv->entry_point  = unit.entry_point.empty() ? DefaultEntryName(unit.stage)
                                                     : unit.entry_point;
        const u32* word_ptr = static_cast<const u32*>(spv_blob->GetBufferPointer());
        const usize words   = spv_blob->GetBufferSize() / sizeof(u32);
        spv->spirv.assign(word_ptr, word_ptr + words);

        // Debug: dump compiled SPIR-V to /tmp for inspection. Toggled via
        // env var WP_DUMP_SPIRV=1.
        if (std::getenv("WP_DUMP_SPIRV")) {
            static int  dump_idx  = 0;
            std::string base      = "/tmp/ww_dump_" + std::to_string(dump_idx++) + "_" +
                                    std::string(DefaultEntryName(unit.stage));
            std::string spv_path  = base + ".spv";
            std::string src_path  = base + ".hlsl";
            if (auto* f = std::fopen(spv_path.c_str(), "wb")) {
                std::fwrite(spv->spirv.data(), sizeof(u32), spv->spirv.size(), f);
                std::fclose(f);
            }
            if (auto* f = std::fopen(src_path.c_str(), "wb")) {
                std::fwrite(unit.src.data(), 1, unit.src.size(), f);
                std::fclose(f);
            }
            rstd_info("dumped SPIR-V + HLSL: {}.{{spv,hlsl}}", base);
        }

        spvs.emplace_back(std::move(spv));
    }

    return true;
}
