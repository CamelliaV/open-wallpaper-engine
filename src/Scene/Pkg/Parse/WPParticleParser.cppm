module;

export module wescene.pkg.parse:wp_particle_parser;
import rstd;
import rstd.cppstd;
import wescene.json;
import wescene.scene;
import wescene.fs;
import wescene.particle.program;
import :wp_particle_runtime;

export import wescene.pkg.scene_obj;

using rstd::sync::Arc;

export namespace owe

{
class WPParticleParser {
public:
    static WPParticleSpawnInstruction GenInitializer(const Json&);
    static Box<dyn<particle::ParticleUpdateProgram>>
    GenOperator(const Json&, Arc<wpscene::ParticleInstanceoverride>, WPParticleSubSystem&,
                usize operator_index);
    static Box<dyn<particle::ParticleEmitterProgram>> GenEmitter(const wpscene::Emitter&,
                                                                 WPParticleSubSystem&, usize);
    static WPParticleSpawnInstruction GenOverride(Arc<wpscene::ParticleInstanceoverride>);
};
} // namespace owe
