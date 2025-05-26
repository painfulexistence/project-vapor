#include <fstream>
#include <stdexcept>
#include <vector>
#include "SDL3/SDL_filesystem.h"

std::vector<char> readFile(const std::string& filename) {
    std::ifstream file((SDL_GetBasePath() + filename), std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file " + filename + "!");
    }
    // TODO: prevent copy construction
    size_t fileSize = (size_t)file.tellg();
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    return buffer;
}