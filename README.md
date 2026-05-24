# Open Wallpaper Engine
Made this for fun.

## Scene Renderer
Open source scene renderer, mostly for linux.  

- vulkan 1.1
- render graph for automatic pass dependencies

## Web Renderer
CEF-based host for Wallpaper Engine web wallpapers.

## How to use

Build first — see [BUILD.md](BUILD.md). Examples below assume the `clang-release` build tree.

### Standalone viewer

`SceneViewer` is a GLFW + Vulkan window that loads a Wallpaper Engine `pkg` directly. Point it at the engine `assets/` directory (this checkout has one at the project root) and a `scene.pkg`:

```bash
./build/clang-release/viewer/SceneViewer \
    assets/ \
    workshop/<id>/scene.pkg
```

Flags (see `viewer/arg.hpp`):

| Flag | Default | Description |
|------|---------|-------------|
| `-R, --resolution WxH` | `1280x720` | window size, e.g. `1920x1080` |
| `-f, --fps N`          | `15`       | render fps |
| `-M, --msaa N`         | `1`        | MSAA samples for the screen RT (1/2/4/8/16; `1` = off) |
| `-C, --cache-path DIR` | XDG cache  | shader/blob cache directory |
| `-V, --valid-layer`    | off        | enable Vulkan validation layers |
| `-G, --graphviz`       | off        | dump render graph to `graph.dot` |


`WebViewer` is the CEF-backed HTML/JS counterpart (only built with `BUILD_WEWEB=ON`):

```bash
./build/clang-release/viewer/WebViewer \
    workshop/<id> \
    --width 1920 --height 1080 \
    --presenter egl     # egl (default) or vulkan
```

Pass `--remote-debugging-port 9222` to expose Chrome DevTools on `http://127.0.0.1:9222`.


## Feature list

- [x] Layer
	- [x] Image
	- [x] Composition / Fullscreen
	- [x] Text
- [x] Effect
    - [x] Basic
	- [x] Mouse position with delay
	- [x] Parallax
    - [x] Depth Parallax
	- [x] ColorBlendMode
	- [x] PBR light
	- [x] Global bloom
- [x] Camera
	- [x] Zoom
	- [ ] Shake
	- [ ] Fade / Path
- [x] Audio
	- [x] Loop
	- [ ] Random
	- [x] Visualization
- [x] Particle System
	- [x] Renderers
	- [x] Emitters
		- [ ] Duration 
	- [x] Initializers
	- [x] Operators
	- [x] Control Points
        - [x] Mouse Follow
	- [x] Children
    - [x] Audio Response
- [x] Puppet
    - [x] Mesh
    - [x] Skeleton
    - [x] Animation
	- [x] Attachments (MDAT)
	- [x] Mesh masks
    - [ ] Bone simulation
    - [ ] Morph (MDMP)
- [ ] 3D model
	- [x] Parse
	- [ ] Render
- [x] Scenescript  
- [x] User Properties
