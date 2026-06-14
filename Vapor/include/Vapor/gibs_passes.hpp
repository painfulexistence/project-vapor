#pragma once
#include "renderer_metal.hpp"
#include "gibs_manager.hpp"
#include <Metal/Metal.hpp>

namespace Vapor {

// Forward declaration
class GIBSManager;

// ============================================================================
// GIBS Render Passes
// Implements the GIBS global illumination pipeline
// ============================================================================

// Pass 1: Generate surfels from G-buffer
class SurfelGenerationPass : public RenderPass {
public:
    explicit SurfelGenerationPass(Renderer_Metal* renderer, GIBSManager* gibsManager);

    const char* getName() const override { return "SurfelGenerationPass"; }
    void execute() override;

private:
    GIBSManager* gibsManager;
};

// Pass 2: Build spatial hash for surfel queries
class SurfelHashBuildPass : public RenderPass {
public:
    explicit SurfelHashBuildPass(Renderer_Metal* renderer, GIBSManager* gibsManager);

    const char* getName() const override { return "SurfelHashBuildPass"; }
    void execute() override;

private:
    GIBSManager* gibsManager;
};

// Pass 3: Raytrace indirect lighting between surfels
class SurfelRaytracingPass : public RenderPass {
public:
    explicit SurfelRaytracingPass(Renderer_Metal* renderer, GIBSManager* gibsManager);

    const char* getName() const override { return "SurfelRaytracingPass"; }
    void execute() override;

private:
    GIBSManager* gibsManager;
};

// Pass 4: Temporal stability filtering
class GIBSTemporalPass : public RenderPass {
public:
    explicit GIBSTemporalPass(Renderer_Metal* renderer, GIBSManager* gibsManager);

    const char* getName() const override { return "GIBSTemporalPass"; }
    void execute() override;

private:
    GIBSManager* gibsManager;
};

// Pass 5: Sample GI from surfels to screen
class GIBSSamplePass : public RenderPass {
public:
    explicit GIBSSamplePass(Renderer_Metal* renderer, GIBSManager* gibsManager);

    const char* getName() const override { return "GIBSSamplePass"; }
    void execute() override;

private:
    GIBSManager* gibsManager;
};

} // namespace Vapor
