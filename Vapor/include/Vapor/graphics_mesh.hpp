#pragma once
// This file redirects to graphics.hpp for RHI architecture compatibility.
// All mesh and material types are now defined in graphics.hpp.
#include "graphics.hpp"
#include "graphics_gpu_structs.hpp"
#include "graphics_handles.hpp"

// WaterVertexData is the only type unique to this file
namespace Vapor {
    // Water vertex — two UV channels: tiled (uv0) and whole-grid (uv1)
    struct WaterVertexData {
        glm::vec3 position;
        glm::vec2 uv0;
        glm::vec2 uv1;
    };
}
        Uint32 vertexCount = 0;
        Uint32 indexCount = 0;

        std::vector<BufferHandle> vbos;
        BufferHandle ebo;
        Uint32 instanceID = UINT32_MAX;
        Uint32 materialID = UINT32_MAX;
    };

};// namespace Vapor