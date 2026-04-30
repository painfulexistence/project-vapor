#pragma once
// Umbrella header — includes all graphics sub-headers.
// Existing code that includes "graphics.hpp" continues to work unchanged.
// Prefer including the specific sub-header when only part of the API is needed:
//   graphics_handles.hpp   — GPUHandle<Tag>, PipelineHandle, BufferHandle, TextureHandle, RenderTargetHandle
//   graphics_batch2d.hpp   — BlendMode, Batch2DVertex, Batch2DUniforms, Batch2DStats
//   graphics_gpu_structs.hpp — PrimitiveMode, MaterialData, lights, CameraData, InstanceData, Cluster, IBLCaptureData
//   graphics_mesh.hpp      — AlphaMode, Image, Material, VertexData, WaterVertexData, Mesh
//   graphics_effects.hpp   — WaterData, AtmosphereData, VolumetricFogData, VolumetricCloudData,
//                            LightScatteringData, SunFlareData, GPUParticle, Particle

#include "graphics_handles.hpp"
#include "graphics_batch2d.hpp"
#include "graphics_gpu_structs.hpp"
#include "graphics_mesh.hpp"
#include "graphics_effects.hpp"
