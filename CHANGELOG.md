# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.3]

### Added

- Scanning of bundled `defaultprojects` and user-authored `myprojects` wallpapers.
- Perspective scene cameras now follow the scene's camera object, with a lookat fallback.
- `WEVector` scripting module, unblocking audio-visualizer wallpapers.
- Content rating metadata on scanned wallpapers.

### Changed

- Reworked the waywallen plugin layout.
- Broader spec-texture name coverage for layer composites and self-links.

### Fixed

- Off-aspect scene wallpapers are no longer stretched.
- Point-sampled (`noInterpolation`) image layers stay point-sampled through the whole effect chain.
- Cross-stage shader varyings are widened to the wider declared type.
- Dynamic text content render-target width.
- Audio-bar fanout ordering.
- `ApplyBlending` override and stray `shadowAtlas` handling.

### Community

- Merged #13 (@burlone0): fix off-aspect scene wallpapers being stretched.
- Fixed #12: point-art image layers losing their nearest filtering.

[0.1.3]: https://github.com/waywallen/open-wallpaper-engine/compare/v0.1.2...v0.1.3

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
