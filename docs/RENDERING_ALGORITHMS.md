# Rendering Algorithms Reference

This document describes the algorithms and techniques used in Project Vapor's rendering pipeline, along with their academic and industry sources.

---

## God Rays (Volumetric Light Scattering)

Screen-space post-process effect that simulates light scattering through the atmosphere.

| Technique | Source | Description |
|-----------|--------|-------------|
| **Screen-space radial blur** | Mitchell, "Volumetric Light Scattering as a Post-Process", GPU Gems 3 Chapter 13 (2007) | Core algorithm: ray march from each pixel towards light source, accumulating samples with exponential decay |
| **Depth-aware occlusion** | Standard technique | Use depth buffer to determine which samples "see" the sky vs. occluded by geometry |
| **Temporal jitter** | Common TAA technique | Add per-frame random offset to reduce banding artifacts |

### Key Parameters
- `density` - Overall scattering intensity
- `decay` - Exponential falloff per sample (typically 0.96-0.99)
- `weight` - Per-sample contribution weight
- `exposure` - Final brightness multiplier
- `numSamples` - Quality vs. performance tradeoff (32-128)

### Reference Implementation
- `Vapor/assets/shaders/3d_light_scattering.metal`

---

## Volumetric Fog

World-space fog rendering using froxel (frustum-voxel) grid.

| Technique | Source | Description |
|-----------|--------|-------------|
| **Froxel-based volumetrics** | Wronski, "Volumetric Fog and Lighting", SIGGRAPH 2014 (Assassin's Creed 4) | Divide view frustum into 3D grid, compute scattering per cell |
| **Henyey-Greenstein phase function** | Henyey & Greenstein, "Diffuse Radiation in the Galaxy", Astrophysical Journal (1941) | Analytic phase function for anisotropic scattering: `(1-g²) / (4π(1+g²-2g·cosθ)^1.5)` |
| **Beer-Lambert law** | Classical optics | Exponential light extinction: `T = exp(-σ·d)` where σ is extinction coefficient, d is distance |
| **Height-based density falloff** | GPU Gems 2 Chapter 16, "Accurate Atmospheric Scattering" | Exponential density decrease with altitude for realistic ground fog |

### Key Parameters
- `fogDensity` - Base fog density
- `fogHeight` / `fogFalloff` - Height fog parameters
- `scatteringCoeff` - Rayleigh/Mie scattering balance
- `absorptionCoeff` - Light absorption amount
- `anisotropy` - Phase function g parameter (-1 to 1)

### Reference Implementation
- `Vapor/assets/shaders/3d_volumetric_fog.metal`
- `Vapor/assets/shaders/3d_volumetric_common.metal`

---

## Volumetric Clouds

Ray-marched volumetric cloud rendering with temporal reprojection.

| Technique | Source | Description |
|-----------|--------|-------------|
| **Ray marching + temporal reprojection** | Schneider, "The Real-Time Volumetric Cloudscapes of Horizon Zero Dawn", SIGGRAPH 2015 | Industry-standard approach for real-time clouds |
| **Perlin-Worley noise** | Schneider (2015) | Hybrid noise combining Perlin (low frequency) with inverted Worley (erosion detail) |
| **Weather map** | Schneider (2015) | 2D texture encoding coverage, cloud type, and precipitation |
| **Beer-Powder approximation** | Schneider (2015) | Modified Beer's law for cloud silver lining: `2·exp(-d)·(1-exp(-2d))` |
| **Multi-scattering approximation** | Hillaire, "Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite", SIGGRAPH 2016 | Octave-based approximation: each bounce reduces intensity and increases isotropy |
| **Cornette-Shanks phase function** | Cornette & Shanks, "Physically Reasonable Analytic Expression for the Single-Scattering Phase Function", Applied Optics (1992) | Improved Henyey-Greenstein that better matches Mie scattering |
| **Dual-lobe phase function** | Wrenninge et al., "Oz: The Great and Volumetric", SIGGRAPH 2013 (Sony Pictures Imageworks) | Blend forward and back scattering lobes for realistic cloud appearance |
| **Quarter-resolution + TAA** | Industry standard (Guerrilla, DICE, Naughty Dog) | Render at 1/4 resolution with temporal accumulation for performance |

### Key Parameters
- `cloudLayerBottom` / `cloudLayerTop` - Cloud altitude bounds
- `cloudCoverage` - Overall cloud amount (0-1)
- `cloudDensity` - Density multiplier
- `cloudType` - Stratus (0) to Cumulus (1) blend
- `windDirection` / `windSpeed` - Cloud animation
- `multiScatterStrength` - Silver lining intensity

### Rendering Pipeline
1. **Pass 1**: Render at quarter resolution to `cloudRT`
2. **Pass 2**: Temporal resolve - blend with `cloudHistoryRT`
3. **Pass 3**: Upscale and composite to main color buffer

### Reference Implementation
- `Vapor/assets/shaders/3d_volumetric_clouds.metal`
- `Vapor/assets/shaders/3d_volumetric_common.metal`

---

## Sun Flare (Lens Flare)

Fully procedural screen-space lens flare effect.

| Technique | Source | Description |
|-----------|--------|-------------|
| **Procedural lens flare** | Chapman, "Pseudo Lens Flare", john-chapman.net (2013) | Screen-space flare using procedural shapes without textures |
| **Ghost sprites** | Optical physics | Reflections between lens elements create displaced, inverted copies |
| **Chromatic aberration** | Optical physics | Color fringing due to wavelength-dependent refraction |
| **Anamorphic streak** | Film cinematography | Horizontal streak from anamorphic lens compression (J.J. Abrams style) |
| **Starburst/diffraction spikes** | Hullin et al., "Physically-Based Real-Time Lens Flare Rendering", SIGGRAPH 2011 | Diffraction pattern from aperture blades |
| **Screen-space occlusion** | Standard technique | Sample depth buffer to determine light source visibility |

### Procedural Elements
1. **Main glow** - Soft radial gradient at light source
2. **Halo ring** - Circular ring around light source
3. **Ghost sprites** - Multiple displaced circles with chromatic offset
4. **Anamorphic streak** - Horizontal elongated glow
5. **Starburst** - Rotating diffraction pattern
6. **Lens dirt** - Procedural noise overlay (optional)

### Key Parameters
- `glowIntensity` / `glowRadius` - Main glow settings
- `haloIntensity` / `haloRadius` - Halo ring settings
- `ghostCount` / `ghostSpacing` - Ghost sprite configuration
- `streakIntensity` / `streakLength` - Anamorphic streak
- `starburstIntensity` / `starburstRays` - Diffraction spikes

### Reference Implementation
- `Vapor/assets/shaders/3d_sun_flare.metal`

---

## Common Infrastructure

Shared utilities used across volumetric effects.

| Technique | Source | Description |
|-----------|--------|-------------|
| **Value noise** | Perlin, "An Image Synthesizer", SIGGRAPH 1985 | Interpolated random values on integer lattice |
| **Gradient (Perlin) noise** | Perlin, "Improving Noise", SIGGRAPH 2002 | Interpolated random gradients, less grid-aligned artifacts |
| **Worley (cellular) noise** | Worley, "A Cellular Texture Basis Function", SIGGRAPH 1996 | Distance to nearest feature point, creates cellular patterns |
| **FBM (Fractal Brownian Motion)** | Musgrave et al., "Texturing and Modeling: A Procedural Approach", 1994 | Octave summation of noise at increasing frequencies |
| **Ray-sphere intersection** | Geometric derivation | Quadratic formula solution for ray-sphere intersection |
| **Ray-box intersection** | Williams et al., "An Efficient and Robust Ray-Box Intersection Algorithm", 2005 | Slab method for AABB intersection |

### Reference Implementation
- `Vapor/assets/shaders/3d_volumetric_common.metal`

---

## Bibliography

### Books
- Pharr, Jakob, Humphreys. *Physically Based Rendering: From Theory to Implementation*, 3rd Edition (2016)
- Musgrave, Kolb, Mace. *Texturing and Modeling: A Procedural Approach*, 3rd Edition (2003)
- Akenine-Möller, Haines, Hoffman. *Real-Time Rendering*, 4th Edition (2018)

### GPU Gems Series
- GPU Gems 2 Chapter 16: "Accurate Atmospheric Scattering" (O'Neil, 2005)
- GPU Gems 3 Chapter 13: "Volumetric Light Scattering as a Post-Process" (Mitchell, 2007)

### SIGGRAPH/GDC Presentations
- Schneider. "The Real-Time Volumetric Cloudscapes of Horizon Zero Dawn", SIGGRAPH 2015
- Hillaire. "Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite", SIGGRAPH 2016
- Wronski. "Volumetric Fog and Lighting", SIGGRAPH 2014
- Hullin et al. "Physically-Based Real-Time Lens Flare Rendering", SIGGRAPH 2011

### Papers
- Henyey & Greenstein. "Diffuse Radiation in the Galaxy", Astrophysical Journal (1941)
- Cornette & Shanks. "Physically Reasonable Analytic Expression for the Single-Scattering Phase Function", Applied Optics (1992)
- Worley. "A Cellular Texture Basis Function", SIGGRAPH (1996)
- Perlin. "Improving Noise", SIGGRAPH (2002)

---

## Additional Resources

### Online References
- [Shadertoy](https://www.shadertoy.com) - Community shader examples
- [Inigo Quilez's Articles](https://iquilezles.org/articles/) - SDF, noise, and procedural techniques
- [John Chapman's Blog](http://john-chapman.net/) - Lens flare and post-processing

### Source Code References
- [Horizon Zero Dawn clouds breakdown](https://www.guerrilla-games.com/read/the-real-time-volumetric-cloudscapes-of-horizon-zero-dawn)
- [Frostbite atmosphere paper](https://www.ea.com/frostbite/news/physically-based-sky-atmosphere-and-cloud-rendering)
