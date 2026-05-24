# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.2]

### Added

- Support for newest puppet (MDLV21) wallpapers, including skinning, submeshes and clip masks.
- Scripting support for `SceneNode`, `setTimeout`, `cursor`, `localStorage`, `WEMath` and `createLayer`.
- Audio-reactive shaders driven by the wavsen spectrum.
- User properties on scene layers, including scripted updates.
- System and workshop font resolution.
- A resolution setting.

### Changed

- Switched the shader frontend from DXC to glslang for broader HLSL compatibility.
- Audio spectrum bars are now smoother and frame-rate independent.
- Text alignment now follows the original `horizontalalign` / `verticalalign` fields.

### Fixed

- Particle movement and `controlpointattract` behavior.
- Rope rendering and `link_mouse` follow.
- Puppet animation speed on multi-pass layers.
- Various Vulkan synchronization issues.

### Community

- Merged #6 (@Shnimlz): audio-responsive particles and scripted scene user properties.

[0.1.2]: https://github.com/waywallen/open-wallpaper-engine/compare/v0.1.1...v0.1.2
