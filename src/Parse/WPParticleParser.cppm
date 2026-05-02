module;

#include <nlohmann/json.hpp>

export module wescene.parse:wp_particle_parser;
import wescene.scene;
import wescene.fs;

export import :wp_particle_object;

export namespace wallpaper

{
class WPParticleParser {
public:
    static ParticleInitOp     genParticleInitOp(const nlohmann::json&);
    static ParticleOperatorOp genParticleOperatorOp(const nlohmann::json&,
                                                    const wpscene::ParticleInstanceoverride&);
    static ParticleEmittOp genParticleEmittOp(const wpscene::Emitter&, bool sort = false);
    static ParticleInitOp  genOverrideInitOp(const wpscene::ParticleInstanceoverride&);
};
} // namespace wallpaper
