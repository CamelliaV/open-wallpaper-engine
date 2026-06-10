#include <gtest/gtest.h>

import nlohmann.json;
import wescene.parse;

TEST(WPMaterialParser, ParsesLegacyUserShaderValues) {
    const auto j = nlohmann::json::parse(R"({
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
    })");

    owe::wpscene::WPMaterial material;
    ASSERT_TRUE(material.FromJson(j));

    ASSERT_EQ(material.user_shader_values.size(), 3u);
    EXPECT_EQ(material.user_shader_values.at("flagcolor1"), "color2");
    EXPECT_EQ(material.user_shader_values.at("flagcolor2"), "color3");
    EXPECT_EQ(material.user_shader_values.at("schemecolor"), "color1");
}
