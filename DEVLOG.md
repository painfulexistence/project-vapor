# Devlog

- 2024/04/01 - Created the repo 🎉!
- 2026/07/16 - ReSTIR denoising for the stochastic RT shadows: per-pixel light-sample reservoirs with temporal + spatial reuse (RHI renderer, Metal RT)
- 2026/07/19 - Fog volume overhaul: froxel volumetric fog (inject → integrate → composite compute) on the native Metal renderer, bounded AABB fog volumes, and multi-volume blend. Every VolumetricFogComponent is now one volume (global haze or a local bank); VolumetricFogSystem gathers them all and the renderer blends overlapping volumes. The RHI Vulkan raymarch consumes the same bounded/blended volume list.
