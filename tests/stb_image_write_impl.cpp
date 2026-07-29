// stb_image_write implementation for the test binary only. The engine's copy
// lives in asset_manager.cpp, which test_path_tracer does not link (it
// compiles image_io.cpp directly to stay GPU- and engine-free).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
