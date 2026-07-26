# Building open-wallpaper-engine

## System dependencies

| Dependency | Version | Notes |
|------------|---------|-------|
| Clang      | 21+  | required — the top-level `CMakeLists.txt` aborts on non-Clang unless `-DWAYWALLEN_ALLOW_NONCLANG=ON` |
| CMake      | 3.28+   | C++20 module scanning |
| Ninja      | recent  | generator pinned in the presets |
| pkg-config | -       | |
| Vulkan SDK | ≥ 1.1   | loader + headers; `glslangValidator` only needed if you tweak shaders |
| GLFW3      | -       | required by `SceneViewer` / `WebViewer` (`BUILD_VIEWER`) |
| liblz4     | -       | `.pkg` decompression |
| EGL / GLESv2 / X11 / wayland-egl | - | `BUILD_WEWEB` viewer only (`WebViewer.cpp` presenters) |

## Build, install, run

The build uses CMake presets. Configure with one preset, then build + install:

```bash
cmake --preset clang-release
cmake --build   build/clang-release
cmake --install build/clang-release --prefix install
```

### CMake 4.4 synthetic BMI builds

OWE uses C++ modules. Starting with CMake 4.4, CMake may create a
consumer-specific [synthetic target](https://cmake.org/cmake/help/latest/manual/cmake-cxxmodules.7.html#term-synthetic-target)
when a module consumer and its provider have different compile options. The
synthetic target rebuilds the provider's BMI with the consumer's compile
profile; it does not rebuild the provider's object files.

In the tested Linux/Clang build, target-local warning and pthread options, as
well as CEF target settings, produced multiple `@synth_` variants across the
rstd and wavsen module graph. Deep synthetic provider closures can contain
missing build edges or inconsistent original and synthetic BMI mappings.
Typical symptoms are a missing synthetic BMI or a Clang `ASTReader` crash while
importing a module.

Current OWE avoids this path by defining one C++ compile profile at the top
level, before rstd, wavsen, CEF, or any other module provider and consumer is
created. Targets that import workspace modules use this profile. CEF compile
definitions and SYSTEM include paths still come from the CEF integration, but
its complete private compile policy is not applied to module importers.

## CMake options

| Option | Default | What it gates |
|--------|---------|---------------|
| `BUILD_WESCENE` | `ON`  | the scene renderer (`owe-renderer` lib + `SceneViewer` + `waywallen-wescene-renderer`) |
| `BUILD_WEWEB`   | `ON`  | the CEF-backed web renderer (`owe-web-host` + `WebViewer` + `waywallen-weweb-renderer`); pulls in the CEF archive |
| `BUILD_WAYWALLEN` | `ON` | the waywallen host subprocesses under `waywallen/` |
| `BUILD_VIEWER`  | `ON` if `glfw3` is found | the standalone GLFW viewers under `viewer/` |

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
