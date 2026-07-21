module;

export module wescene.pkg.parse:wp_particle_parser;
import rstd.cppstd;
import wescene.json;
import wescene.scene;
import wescene.fs;
import wescene.particle.program;
import :wp_particle_runtime;

export import wescene.pkg.scene_obj;

export namespace owe

{
class WPParticleParser {
public:
    static Box<dyn<particle::ParticleSpawnProgram>> GenInitializer(const Json&,
                                                                   WPParticleAttributes);
    static Box<dyn<particle::ParticleUpdateProgram>>
    GenOperator(const Json&, std::shared_ptr<const wpscene::ParticleInstanceoverride>,
                WPParticleSubSystem&, usize operator_index);
    static Box<dyn<particle::ParticleEmitterProgram>> GenEmitter(const wpscene::Emitter&,
                                                                 WPParticleAttributes);
    static Box<dyn<particle::ParticleSpawnProgram>>
        GenOverride(std::shared_ptr<const wpscene::ParticleInstanceoverride>, WPParticleAttributes);
};
} // namespace owe
