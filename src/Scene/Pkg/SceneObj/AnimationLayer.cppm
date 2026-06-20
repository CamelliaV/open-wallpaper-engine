module;

export module wescene.pkg.scene_obj:animation_layer;
import rstd.cppstd;
import wescene.json;
export import wescene.pkg.puppet;

export namespace owe::wpscene
{

inline void ReadPuppetAnimationLayers(const nlohmann::json&                       json,
                                      std::vector<WPPuppetLayer::AnimationLayer>& out) {
    if (! json.contains("animationlayers")) return;
    for (const auto& jLayer : json.at("animationlayers")) {
        WPPuppetLayer::AnimationLayer layer;
        owe::GetJsonValue(jLayer, "animation", layer.id);
        owe::GetJsonValue(jLayer, "blend", layer.blend);
        owe::GetJsonValue(jLayer, "rate", layer.rate);
        owe::GetJsonValue(jLayer, "visible", layer.visible, false);
        owe::GetJsonValue(jLayer, "id", layer.layer_id, false);
        owe::GetJsonValue(jLayer, "name", layer.name, false);
        owe::GetJsonValue(jLayer, "additive", layer.additive, false);
        owe::GetJsonValue(jLayer, "blendin", layer.blendin, false);
        owe::GetJsonValue(jLayer, "blendout", layer.blendout, false);
        owe::GetJsonValue(jLayer, "blendtime", layer.blendtime, false);
        out.push_back(std::move(layer));
    }
}

} // namespace owe::wpscene
