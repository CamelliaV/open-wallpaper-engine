# Building open-wallpaper-engine

## System dependencies

| Dependency | Version | Notes |
|------------|---------|-------|
| Clang      | recent  | required — the top-level `CMakeLists.txt` aborts on non-Clang unless `-DWAYWALLEN_ALLOW_NONCLANG=ON` |
| lld        | matching clang | the `_clang-base` preset wires `CMAKE_LINKER=lld` |
| CMake      | 3.28+   | C++20 module scanning |
| Ninja      | recent  | generator pinned in the presets |
| pkg-config | -       | |
| Vulkan SDK | ≥ 1.1   | loader + headers; `glslangValidator` only needed if you tweak shaders |
| GLFW3      | -       | required by `SceneViewer` / `WebViewer` (`BUILD_VIEWER`) |
| Freetype   | -       | |
| OpenGL     | -       | |
| liblz4     | -       | `.pkg` decompression |
| EGL / GLESv2 / X11 / wayland-egl | - | `BUILD_WEWEB` viewer only (`WebViewer.cpp` presenters) |

All other deps (Eigen, nlohmann_json, cubeb, SPIRV-Reflect, DXC, argparse, rstd, wavsen, QuickJS-NG, CEF) come in through `deps.json` via the `fetchdeps()` driver in `cmake/FetchDeps.cmake` — see [FetchDeps](#fetchdeps) below.

## Build, install, run

The build uses CMake presets. Configure with one preset, then build + install:

```bash
cmake --preset clang-release
cmake --build   build/clang-release
cmake --install build/clang-release --prefix install
```

## CMake options

| Option | Default | What it gates |
|--------|---------|---------------|
| `BUILD_WESCENE` | `ON`  | the scene renderer (`owe-renderer` lib + `SceneViewer` + `waywallen-wescene-renderer`) |
| `BUILD_WEWEB`   | `ON`  | the CEF-backed web renderer (`owe-web-host` + `WebViewer` + `waywallen-weweb-renderer`); pulls in the CEF archive |
| `BUILD_WAYWALLEN` | `ON` | the waywallen host subprocesses under `waywallen/` |
| `BUILD_VIEWER`  | `ON` if `glfw3` is found | the standalone GLFW viewers under `viewer/` |
| `BUILD_TESTS`   | `OFF` | the gtest suite under `tests/` (needs `BUILD_WESCENE`); enable CTest with `--preset clang-debug` and `ctest --preset clang-debug` |
| `ENABLE_RENDERDOC` | `OFF` | RenderDoc in-application API hooks |
| `WAYWALLEN_ALLOW_NONCLANG` | `OFF` | bypass the top-level Clang check |

## Output layout

A full `BUILD_WESCENE=ON BUILD_WEWEB=ON BUILD_WAYWALLEN=ON BUILD_VIEWER=ON` install produces:

```
<prefix>/bin/
    waywallen-wescene-renderer       # scene host subprocess for waywallen
<prefix>/lib/
    libdxcompiler.so                 # DXC, used at runtime by owe-shader-compile
<prefix>/share/waywallen/
    renderers/wescene.toml
    renderers/weweb.toml             # only when BUILD_WEWEB=ON
    sources/wallpaper_engine.lua
    weweb/                           # only when BUILD_WEWEB=ON
        waywallen-weweb-renderer
        libcef.so, *.pak, icudtl.dat, locales/...   # CEF runtime
```

The standalone viewers (`SceneViewer`, `WebViewer`) are *not* `install()`-ed — run them from the build tree at `build/<preset>/viewer/`.