#include <gtest/gtest.h>

import rstd.cppstd;
import wescene.json;
import wescene.pkg.parse;

using namespace rstd::literals;

TEST(MaterialParser, ParsesLegacyUserShaderValues) {
    auto j = rstd::json::from_str(R"({
        "passes": [
            {
                "shader": "flag",
                "textures": ["eagle", "flag_normal", "cloth"],
                "usershadervalues": {
                    "flagcolor1": "color2",
                    "flagcolor2": "color3",
                    "schemecolor": "color1"
                }
            }
        ]
    })"_str)
                 .unwrap();

    owe::wpscene::Material material;
    ASSERT_TRUE(material.FromJson(j));

    ASSERT_EQ(material.user_shader_values.size(), 3u);
    EXPECT_EQ(material.user_shader_values.at("flagcolor1"), "color2");
    EXPECT_EQ(material.user_shader_values.at("flagcolor2"), "color3");
    EXPECT_EQ(material.user_shader_values.at("schemecolor"), "color1");
}

TEST(MaterialParser, PreservesConstantShaderValueScriptBindingsAcrossPassMerge) {
    auto material_json = rstd::json::from_str(R"({
        "passes": [{
            "shader": "effect",
            "constantshadervalues": {
                "color": [1.0, 1.0, 1.0]
            }
        }]
    })"_str)
                             .unwrap();
    auto pass_json     = rstd::json::from_str(R"({
        "constantshadervalues": {
            "color": {
                "script": "export function update(value) { return value; }",
                "scriptproperties": {"speed": 2.0},
                "animation": {"c0": [{"frame": 0, "value": 0.1}]},
                "value": "0.1 0.2 0.3"
            }
        }
    })"_str)
                             .unwrap();

    owe::wpscene::Material material;
    ASSERT_TRUE(material.FromJson(material_json));
    owe::wpscene::MaterialPass pass;
    ASSERT_TRUE(pass.FromJson(pass_json));
    material.MergePass(pass);

    auto binding = material.constantshadervalues_bindings.scripts.find("color");
    ASSERT_NE(binding, material.constantshadervalues_bindings.scripts.end());
    EXPECT_EQ(binding->second.source, "export function update(value) { return value; }");
    EXPECT_TRUE(binding->second.properties.is_object());
    EXPECT_TRUE(binding->second.initial_value.is_string());
    ASSERT_EQ(material.constantshadervalues_animations.at("color").c0.size(), 1u);
    EXPECT_FLOAT_EQ(material.constantshadervalues_animations.at("color").c0[0].value, 0.1f);

    auto clone          = material.clone();
    auto cloned_binding = clone.constantshadervalues_bindings.scripts.find("color");
    ASSERT_NE(cloned_binding, clone.constantshadervalues_bindings.scripts.end());
    EXPECT_EQ(cloned_binding->second.source, binding->second.source);
    EXPECT_TRUE(cloned_binding->second.properties.is_object());
    EXPECT_TRUE(cloned_binding->second.initial_value.is_string());
}
