#pragma once
#include "graphics.hpp"

enum class GPUBufferUsage {
    VERTEX,
    INDEX,
    UNIFORM,
    COPY_SRC,
    COPY_DST
};

enum class GPUImageUsage {
    COLOR_MSAA,
    COLOR,
    DEPTH,
    DEPTH_STENCIL
};

class Renderer {
public:
    ~Renderer(){};

    virtual void init() = 0;

    virtual void draw() = 0;
};