module;

#include <memory>

module wescene.scene;

import wescene.fs;

namespace wallpaper
{

namespace
{
void delete_vfs(void* p) noexcept { delete static_cast<fs::VFS*>(p); }
} // namespace

Scene::Scene()
    : sceneGraph(std::make_shared<SceneNode>()),
      vfs(nullptr, &delete_vfs),
      paritileSys(std::make_unique<ParticleSystem>(*this)) {}
Scene::~Scene() = default;

} // namespace wallpaper
