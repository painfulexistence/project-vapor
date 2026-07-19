#include "Vapor/helper.hpp"
#include "Vapor/file_system.hpp"
#include <fstream>
#include <stdexcept>

using namespace Vapor;

auto Vapor::readFile(const std::string& filename) -> std::string {
    std::ifstream file(FileSystem::instance().resolvePathOrThrow(filename), std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file " + filename + "!");
    }
    // TODO: prevent copy construction
    auto fileSize = static_cast<size_t>(file.tellg());
    std::string buffer(fileSize, '\0');// null terminated string

    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    return buffer;
}