#pragma once

#include <filesystem>

namespace viewer {

std::filesystem::path ExecutableDir(const char* argv0);

void InitGlfwPlatformHint(bool force_x11);

}  // namespace viewer
