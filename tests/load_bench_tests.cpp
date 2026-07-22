#include <gtest/gtest.h>

import rstd.bench;
import wescene.load_bench;

namespace
{

auto ProbeLabel(const owe::SceneLoadBenchContext& context, rstd::bench::probe::ProbeId id)
    -> rstd::ref<rstd::str> {
    auto schema = context.schema_owner();
    return schema->label(id).unwrap();
}

TEST(SceneLoadBench, EmptyOutputDisablesCollection) {
    EXPECT_TRUE(owe::CreateSceneLoadBench("").is_none());
}

TEST(SceneLoadBench, RegistersStableCatalog) {
    auto context = owe::CreateSceneLoadBench("/tmp/owe-load-bench-test.txt");
    ASSERT_TRUE(context.is_some());
    auto& bench = **context;

    auto schema = bench.schema_owner();
    EXPECT_EQ(schema->len().to_primitive(), 43u);
    EXPECT_EQ(ProbeLabel(bench, bench.ids().preload_scene_document),
              rstd::ref<rstd::str>("preload.scene_document"));
    EXPECT_EQ(ProbeLabel(bench, bench.ids().parse_object_particle),
              rstd::ref<rstd::str>("parse.object.particle"));
    EXPECT_EQ(ProbeLabel(bench, bench.ids().render_user_property_graph_rebuild),
              rstd::ref<rstd::str>("render.user_property.graph_rebuild"));
    EXPECT_EQ(ProbeLabel(bench, bench.ids().render_first_frame_prepare),
              rstd::ref<rstd::str>("render.first_frame.prepare"));
    EXPECT_EQ(ProbeLabel(bench, bench.ids().render_first_draw),
              rstd::ref<rstd::str>("render.first_draw"));
}

TEST(SceneLoadBench, AssignsIncreasingRunIds) {
    auto first  = owe::CreateSceneLoadBench("/tmp/owe-load-bench-first.txt");
    auto second = owe::CreateSceneLoadBench("/tmp/owe-load-bench-second.txt");

    ASSERT_TRUE(first.is_some());
    ASSERT_TRUE(second.is_some());
    EXPECT_LT((**first).run_id().to_primitive(), (**second).run_id().to_primitive());
}

TEST(SceneLoadBench, MovesPreloadBatchOnce) {
    auto context = owe::CreateSceneLoadBench("/tmp/owe-load-bench-test.txt");
    ASSERT_TRUE(context.is_some());
    auto& bench = **context;

    auto recorder = bench.session().recorder();
    {
        auto span = recorder.span(bench.ids().preload_scene_document);
    }
    auto drained = recorder.drain();
    ASSERT_TRUE(drained.is_ok());
    bench.add_preload_batch(rstd::move(drained).unwrap_unchecked());

    auto batches = bench.take_preload_batches();
    ASSERT_EQ(batches.len().to_primitive(), 1u);
    EXPECT_TRUE(bench.take_preload_batches().is_empty());

    auto collector = rstd::bench::probe::ProbeCollector::new_(bench.schema_owner());
    ASSERT_TRUE(collector.ingest(batches[rstd::usize()]).is_ok());
    auto report = rstd::move(collector).finish();
    ASSERT_EQ(report.overall().len().to_primitive(), 1u);
    EXPECT_EQ(report.overall()[rstd::usize()].count.to_primitive(), 1u);
    EXPECT_EQ(report.dropped_samples().to_primitive(), 0u);
    EXPECT_TRUE(report.diagnostics().is_empty());
}

} // namespace
