module;

#include <rstd/macro.hpp>

module wescene.pkg.parse;
import :scene_context;
import rstd;
import rstd.log;
import wescene.load_bench;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

auto owe::SceneParser::Parse(ref<str> scene_id, ref<wpscene::SceneDocument> document,
                             mut_ref<fs::VFS> vfs, mut_ref<wavsen::audio::SoundManager> sound,
                             SceneParseOptions options) -> Result<ParsedScene, SceneParseError> {
    auto&       vfs_owner   = *vfs.as_raw_ptr();
    auto&       sound_owner = *sound.as_raw_ptr();
    auto        total_span  = SceneLoadSpan(options.load_bench, &SceneLoadProbeIds::parse_total);
    const auto& metadata    = document->metadata;
    rstd_info("scene: pkg_version={} scene_json_version={}",
              static_cast<unsigned>(metadata.pkg_version),
              static_cast<unsigned>(metadata.scene_json_version));

    if (! document->objects_are_array) {
        return Err(SceneParseError {
            .kind    = SceneParseErrorKind::ObjectExpansion,
            .message = String::make("scene objects must be an array"_str),
        });
    }

    auto expanded = [&] {
        auto span = SceneLoadSpan(options.load_bench, &SceneLoadProbeIds::parse_expand_objects);
        return ExpandSceneObjects(document, vfs, options.user_properties);
    }();
    auto objects = rstd::move(expanded.objects);

    auto context_span  = SceneLoadSpan(options.load_bench, &SceneLoadProbeIds::parse_context);
    auto context       = BuildContext(vfs_owner,
                                      scene_id,
                                      metadata,
                                      ResolveOrthoProjectionExtent(metadata, objects.as_slice()),
                                      options.user_properties,
                                      rstd::move(options.shader_cache_dir));
    auto runtime_input = Arc<UniformRuntimeInput>::make(context.uniform_state.clone());
    context.hidden_link_source_ids = rstd::move(expanded.hidden_link_source_ids);
    context.linked_source_ids      = rstd::move(expanded.linked_source_ids);
    IndexSceneDocument(context, document, objects.as_slice());
    ProcessContainers(context, objects.as_mut_slice().as_mut_ref());
    (void)context_span.finish();

    {
        auto span = SceneLoadSpan(options.load_bench, &SceneLoadProbeIds::parse_objects);
        ProcessObjects(
            context, objects.as_mut_slice().as_mut_ref(), &sound_owner, {}, options.load_bench);
    }
    {
        auto span = SceneLoadSpan(options.load_bench, &SceneLoadProbeIds::parse_post_process);
        if (metadata.general.bloom) BuildBloomPostProcess(context, vfs_owner, metadata.general);
    }

    auto finalize_span = SceneLoadSpan(options.load_bench, &SceneLoadProbeIds::parse_finalize);
    return Ok(ParsedScene {
        .scene         = FinalizeScene(context),
        .runtime_input = rstd::move(runtime_input),
    });
}
