module;


export module wescene.parse:wp_particle_parser;
import nlohmann.json;
import wescene.scene;
import wescene.fs;

export import :wp_particle_object;

export namespace owe

{
class WPParticleParser {
public:
    static ParticleInitOp     genParticleInitOp(const nlohmann::json&);
    static ParticleOperatorOp genParticleOperatorOp(
        const nlohmann::json&,
        std::shared_ptr<const wpscene::ParticleInstanceoverride>);
    static ParticleEmittOp genParticleEmittOp(const wpscene::Emitter&, bool sort = false);
    static ParticleInitOp  genOverrideInitOp(
        std::shared_ptr<const wpscene::ParticleInstanceoverride>);
};
} // namespace owe
