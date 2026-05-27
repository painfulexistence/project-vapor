#ifndef __APPLE__
#include "ui_renderer.hpp"

namespace Vapor {

std::unique_ptr<UIRenderer> UIRenderer::create(int /*width*/, int /*height*/)
{
    return nullptr;
}

}// namespace Vapor
#endif
