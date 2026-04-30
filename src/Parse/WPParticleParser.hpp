#pragma once
#include "Particle/ParticleEmitter.h"
#include "Parse/WPParticleObject.hpp"

namespace wallpaper
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
