module;

export module wescene.pkg.parse:wp_particle_parser;
import rstd.cppstd;
import wescene.json;
import wescene.scene;
import wescene.fs;

export import wescene.pkg.scene_obj;

export namespace owe

{
class WPParticleParser {
public:
    static ParticleInitOp genParticleInitOp(const Json&);
    static ParticleOperatorOp
    genParticleOperatorOp(const Json&,
                          std::shared_ptr<const wpscene::ParticleInstanceoverride>);
    static ParticleEmittOp genParticleEmittOp(const wpscene::Emitter&, bool sort = false);
    static ParticleInitOp
        genOverrideInitOp(std::shared_ptr<const wpscene::ParticleInstanceoverride>);
};
} // namespace owe
