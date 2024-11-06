#pragma once

enum class BufferUsage {
    VERTEX,
    INDEX,
    UNIFORM,
    COPY_SRC,
    COPY_DST
};

class Renderer {
public:
    ~Renderer(){};

    virtual void init() = 0;

    virtual void draw() = 0;
};