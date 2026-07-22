module;

export module wescene.scene_user_property;

import rstd;
import rstd.cppstd;
import wescene.json;
import wescene.scene;

export namespace owe
{

struct SceneUserPropertyMutation {
    bool                         graph_changed {};
    bool                         diagnostics_changed {};
    std::vector<SceneMaterialId> texture_materials;
};

std::string CanonicalSceneUserPropertyKey(std::string_view key);

class SceneUserPropertyApplier {
public:
    static SceneUserPropertyMutation    Apply(Scene&, std::string_view key, const Json&);
    static SceneUserPropertyMutation    ApplyAll(Scene&, const rstd::json::Map&);
    static std::vector<SceneMaterialId> ApplyTexture(Scene&, std::string_view key, const Json&);
};

std::vector<SceneUserPropertyDiagnostic> CollectSceneUserPropertyDiagnostics(const Scene&,
                                                                             std::string_view key);

} // namespace owe
