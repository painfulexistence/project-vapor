#pragma once

class Renderer {
public:
    ~Renderer(){};

    virtual void init() = 0;

    virtual void draw() = 0;
};