#include <fstream>
#include <stdexcept>
#include <SDL3/SDL_filesystem.h>

std::string readFile(const std::string& filename) {
    std::ifstream file((SDL_GetBasePath() + filename), std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file " + filename + "!");
    }
    // TODO: prevent copy construction
    auto fileSize = static_cast<size_t>(file.tellg());
    std::string buffer(fileSize, '\0'); // null terminated string

    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    return buffer;
}