#pragma once
#include "rhi.hpp"

// ImGui glue for the D3D12 backend, implemented inside rhi_d3d12.cpp so that
// renderer.cpp never has to include Windows/D3D12 headers (windows.h's macro
// surface — near/far/min/max — is hostile to engine code). The functions
// mirror what the Metal/Vulkan branches in renderer.cpp do inline with
// ImGui_ImplMetal_*/ImGui_ImplVulkan_*.
//
// All of them are safe to call only when the active RHI is the D3D12 backend
// (createRHID3D12()); `rhi` is downcast internally.

struct SDL_Window;

namespace Vapor {

// ImGui_ImplSDL3_InitForD3D + ImGui_ImplDX12_Init against the backend's
// device/queue/SRV-heap. Returns false when ImGui init fails.
bool imguiD3D12Init(RHI* rhi, SDL_Window* window);
void imguiD3D12Shutdown();
void imguiD3D12NewFrame();
// Records ImGui draw data into the backend's current frame command list.
// Must be called inside a render pass targeting the swapchain.
void imguiD3D12RenderDrawData(RHI* rhi);
// Persistent shader-visible SRV slot for an engine texture, packaged as an
// ImTextureID (D3D12_GPU_DESCRIPTOR_HANDLE::ptr). Returns 0 for an invalid
// handle. Used by the ImGui debug-preview windows.
Uint64 imguiD3D12TextureID(RHI* rhi, TextureHandle texture);

} // namespace Vapor
