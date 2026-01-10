#include "lod_generator.hpp"
#include "gltf_loader.hpp"
#include <args.hxx>
#include <fmt/core.h>
#include <fstream>
#include <cereal/archives/binary.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/memory.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <filesystem>

// Cereal serialization for GLM types
namespace cereal {
    template<class Archive>
    void serialize(Archive& archive, glm::vec2& vec) {
        archive(vec.x, vec.y);
    }

    template<class Archive>
    void serialize(Archive& archive, glm::vec3& vec) {
        archive(vec.x, vec.y, vec.z);
    }

    template<class Archive>
    void serialize(Archive& archive, glm::vec4& vec) {
        archive(vec.x, vec.y, vec.z, vec.w);
    }

    template<class Archive>
    void serialize(Archive& archive, glm::mat4& mat) {
        archive(mat[0], mat[1], mat[2], mat[3]);
    }
}

// Cereal serialization for vapor_asset types
namespace cereal {
    template<class Archive>
    void serialize(Archive& archive, vapor_asset::VertexData& v) {
        archive(v.position, v.uv, v.normal, v.tangent);
    }

    template<class Archive>
    void serialize(Archive& archive, vapor_asset::LODLevel& lod) {
        archive(lod.vertices, lod.indices, lod.error, lod.screenSizeThreshold);
    }

    template<class Archive>
    void serialize(Archive& archive, vapor_asset::LODMesh& mesh) {
        archive(
            mesh.name,
            mesh.lodLevels,
            mesh.localAABBMin,
            mesh.localAABBMax,
            mesh.boundingSphereCenter,
            mesh.boundingSphereRadius,
            mesh.materialIndex
        );
    }

    template<class Archive>
    void serialize(Archive& archive, vapor_asset::MaterialData& mat) {
        archive(
            mat.name,
            mat.baseColorFactor,
            mat.metallicFactor,
            mat.roughnessFactor,
            mat.albedoTexturePath,
            mat.normalTexturePath,
            mat.metallicRoughnessTexturePath
        );
    }

    template<class Archive>
    void serialize(Archive& archive, vapor_asset::SceneNode& node) {
        archive(node.name, node.localTransform, node.meshIndices, node.children);
    }

    template<class Archive>
    void serialize(Archive& archive, vapor_asset::SceneData& scene) {
        archive(
            scene.name,
            scene.meshes,
            scene.materials,
            scene.rootNodes,
            scene.totalOriginalTriangles,
            scene.totalTrianglesWithLODs
        );
    }
}

namespace {

constexpr const char* VERSION = "0.1.0";
constexpr uint32_t VSCENE_LOD_MAGIC = 0x564C4F44; // "VLOD"
constexpr uint32_t VSCENE_LOD_VERSION = 1;

void writeVSceneLOD(const vapor_asset::SceneData& scene, const std::string& outputPath) {
    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error(fmt::format("Failed to open output file: {}", outputPath));
    }

    // Write header
    file.write(reinterpret_cast<const char*>(&VSCENE_LOD_MAGIC), sizeof(VSCENE_LOD_MAGIC));
    file.write(reinterpret_cast<const char*>(&VSCENE_LOD_VERSION), sizeof(VSCENE_LOD_VERSION));

    // Write scene data using cereal
    cereal::BinaryOutputArchive archive(file);
    archive(scene);

    fmt::print("Written to: {}\n", outputPath);
}

vapor_asset::SceneData readVSceneLOD(const std::string& inputPath) {
    std::ifstream file(inputPath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error(fmt::format("Failed to open input file: {}", inputPath));
    }

    // Read and verify header
    uint32_t magic, version;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));

    if (magic != VSCENE_LOD_MAGIC) {
        throw std::runtime_error("Invalid file format: not a .vscene_lod file");
    }
    if (version != VSCENE_LOD_VERSION) {
        throw std::runtime_error(fmt::format("Unsupported version: {}", version));
    }

    // Read scene data
    vapor_asset::SceneData scene;
    cereal::BinaryInputArchive archive(file);
    archive(scene);

    return scene;
}

void printSceneInfo(const vapor_asset::SceneData& scene) {
    fmt::print("\n=== Scene Info ===\n");
    fmt::print("Name: {}\n", scene.name);
    fmt::print("Meshes: {}\n", scene.meshes.size());
    fmt::print("Materials: {}\n", scene.materials.size());
    fmt::print("Root nodes: {}\n", scene.rootNodes.size());
    fmt::print("Original triangles: {}\n", scene.totalOriginalTriangles);
    fmt::print("Total triangles (all LODs): {}\n", scene.totalTrianglesWithLODs);

    fmt::print("\n--- Mesh Details ---\n");
    for (const auto& mesh : scene.meshes) {
        fmt::print("  {}: {} LODs, material={}\n",
            mesh.name, mesh.lodLevels.size(), mesh.materialIndex);
        for (size_t i = 0; i < mesh.lodLevels.size(); ++i) {
            const auto& lod = mesh.lodLevels[i];
            fmt::print("    LOD{}: {} tris, threshold={:.3f}, error={:.6f}\n",
                i, lod.indices.size() / 3, lod.screenSizeThreshold, lod.error);
        }
    }
}

} // anonymous namespace

int main(int argc, char** argv) {
    args::ArgumentParser parser(
        "vapor-asset - Asset processing tool for Vapor Engine",
        "Generates LOD levels for mesh assets"
    );

    args::HelpFlag help(parser, "help", "Display this help menu", {'h', "help"});
    args::Flag version(parser, "version", "Display version", {'v', "version"});

    args::Group commands(parser, "commands");
    args::Command process(commands, "process", "Process a GLTF file and generate LODs");
    args::Command info(commands, "info", "Display info about a .vscene_lod file");

    // Process command arguments
    args::Positional<std::string> inputFile(process, "input", "Input GLTF file (.gltf or .glb)");
    args::ValueFlag<std::string> outputFile(process, "output", "Output file path", {'o', "output"});
    args::ValueFlag<uint32_t> maxLODs(process, "lods", "Maximum LOD levels (default: 5)", {'l', "lods"}, 5);
    args::ValueFlag<float> reduction(process, "reduction", "Reduction ratio per level (default: 0.5)", {'r', "reduction"}, 0.5f);
    args::ValueFlag<float> errorThreshold(process, "error", "Error threshold (default: 0.01)", {'e', "error"}, 0.01f);
    args::Flag noLockBorders(process, "no-lock-borders", "Don't lock mesh border vertices", {"no-lock-borders"});

    // Info command arguments
    args::Positional<std::string> infoFile(info, "file", ".vscene_lod file to inspect");

    try {
        parser.ParseCLI(argc, argv);

        if (version) {
            fmt::print("vapor-asset version {}\n", VERSION);
            return 0;
        }

        if (process) {
            if (!inputFile) {
                fmt::print("Error: Input file required\n");
                return 1;
            }

            std::string input = args::get(inputFile);
            std::string output = outputFile
                ? args::get(outputFile)
                : std::filesystem::path(input).replace_extension(".vscene_lod").string();

            vapor_asset::LODConfig config;
            config.maxLODLevels = args::get(maxLODs);
            config.targetReductionPerLevel = args::get(reduction);
            config.errorThreshold = args::get(errorThreshold);
            config.lockBorders = !noLockBorders;

            // Adjust screen size thresholds based on LOD count
            config.screenSizeThresholds.clear();
            float threshold = 0.15f;
            for (uint32_t i = 0; i < config.maxLODLevels; ++i) {
                config.screenSizeThresholds.push_back(threshold);
                threshold *= 0.5f;
            }

            fmt::print("vapor-asset v{}\n", VERSION);
            fmt::print("Processing: {}\n", input);
            fmt::print("Output: {}\n", output);
            fmt::print("Config: {} LODs, {:.0f}% reduction/level, error={:.4f}\n",
                config.maxLODLevels, config.targetReductionPerLevel * 100, config.errorThreshold);
            fmt::print("\n");

            vapor_asset::GLTFLoader loader;
            auto scene = loader.loadAndGenerateLODs(input, config);

            writeVSceneLOD(scene, output);
            printSceneInfo(scene);

            return 0;
        }

        if (info) {
            if (!infoFile) {
                fmt::print("Error: File path required\n");
                return 1;
            }

            auto scene = readVSceneLOD(args::get(infoFile));
            printSceneInfo(scene);

            return 0;
        }

        // No command specified, show help
        fmt::print("{}\n", parser.Help());
        return 0;

    } catch (const args::Help&) {
        fmt::print("{}\n", parser.Help());
        return 0;
    } catch (const args::ParseError& e) {
        fmt::print("Error: {}\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        fmt::print("Error: {}\n", e.what());
        return 1;
    }
}
