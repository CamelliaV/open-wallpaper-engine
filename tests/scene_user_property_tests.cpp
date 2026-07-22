#include <gtest/gtest.h>

import rstd;
import rstd.json;
import wescene.scene;
import wescene.scene_user_property;

TEST(SceneUserProperty, CanonicalizesHostSchemeColor) {
    EXPECT_EQ(owe::CanonicalSceneUserPropertyKey("waywallen.scheme_color"), "schemecolor");
    EXPECT_EQ(owe::CanonicalSceneUserPropertyKey("custom"), "custom");
}

TEST(SceneUserProperty, BatchUsesSinglePropertySemantics) {
    owe::Scene single;
    owe::Scene batch;
    single.clearColorUserKey = "schemecolor";
    batch.clearColorUserKey  = "schemecolor";

    auto property = rstd::json::from_str(R"({"type":"color","value":"0.2 0.4 0.6"})").unwrap();
    auto properties =
        rstd::json::from_str(R"({"waywallen.scheme_color":{"type":"color","value":"0.2 0.4 0.6"}})")
            .unwrap();
    auto values = properties.as_object();
    ASSERT_TRUE(values.is_some());

    auto single_mutation =
        owe::SceneUserPropertyApplier::Apply(single, "waywallen.scheme_color", property);
    auto batch_mutation = owe::SceneUserPropertyApplier::ApplyAll(batch, **values);

    EXPECT_FALSE(single_mutation.graph_changed);
    EXPECT_FALSE(batch_mutation.graph_changed);
    EXPECT_TRUE(single_mutation.texture_materials.empty());
    EXPECT_TRUE(batch_mutation.texture_materials.empty());
    for (rstd::usize index; index < rstd::usize(3); ++index) {
        EXPECT_FLOAT_EQ(single.clearColor[index], batch.clearColor[index]);
    }
    EXPECT_FLOAT_EQ(batch.clearColor[rstd::usize()], 0.2f);
    EXPECT_FLOAT_EQ(batch.clearColor[rstd::usize(1)], 0.4f);
    EXPECT_FLOAT_EQ(batch.clearColor[rstd::usize(2)], 0.6f);
}
