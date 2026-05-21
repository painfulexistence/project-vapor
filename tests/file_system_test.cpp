#include <catch2/catch_test_macros.hpp>
#include "Vapor/file_system.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>

static std::filesystem::path makeTempDir(const std::string& suffix) {
    auto p = std::filesystem::temp_directory_path() / ("vapor_fs_test_" + suffix);
    std::filesystem::create_directories(p);
    return p;
}

static void touchFile(const std::filesystem::path& dir, const std::string& name) {
    std::ofstream(dir / name);
}

TEST_CASE("FileSystem - resolvePath finds file in registered search path", "[filesystem]") {
    auto dir = makeTempDir("basic");
    touchFile(dir, "hello.txt");

    FileSystem::instance().addSearchPath(dir.string(), 99);

    auto result = FileSystem::instance().resolvePath("hello.txt");
    REQUIRE(result.has_value());
    CHECK(std::filesystem::exists(*result));

    FileSystem::instance().removeSearchPath(dir.string());
    std::filesystem::remove_all(dir);
}

TEST_CASE("FileSystem - resolvePath returns nullopt for missing file", "[filesystem]") {
    auto result = FileSystem::instance().resolvePath("__vapor_fs_test_missing__.xyz");
    CHECK_FALSE(result.has_value());
}

TEST_CASE("FileSystem - priority ordering: lower number wins", "[filesystem]") {
    auto hiPri = makeTempDir("pri_hi");
    auto loPri = makeTempDir("pri_lo");
    touchFile(hiPri, "shared_vapor_fs.txt");
    touchFile(loPri, "shared_vapor_fs.txt");

    FileSystem::instance().addSearchPath(hiPri.string(), 10);
    FileSystem::instance().addSearchPath(loPri.string(), 20);

    auto result = FileSystem::instance().resolvePath("shared_vapor_fs.txt");
    REQUIRE(result.has_value());
    // Resolved path must be under the higher-priority (lower number) directory
    auto resolved = std::filesystem::path(*result).parent_path();
    CHECK(std::filesystem::equivalent(resolved, hiPri));

    FileSystem::instance().removeSearchPath(hiPri.string());
    FileSystem::instance().removeSearchPath(loPri.string());
    std::filesystem::remove_all(hiPri);
    std::filesystem::remove_all(loPri);
}

TEST_CASE("FileSystem - removeSearchPath stops file from being found", "[filesystem]") {
    auto dir = makeTempDir("remove");
    touchFile(dir, "removable_vapor_fs.txt");

    FileSystem::instance().addSearchPath(dir.string(), 99);
    REQUIRE(FileSystem::instance().resolvePath("removable_vapor_fs.txt").has_value());

    FileSystem::instance().removeSearchPath(dir.string());
    CHECK_FALSE(FileSystem::instance().resolvePath("removable_vapor_fs.txt").has_value());

    std::filesystem::remove_all(dir);
}

TEST_CASE("FileSystem - resolvePathOrThrow throws when not found", "[filesystem]") {
    CHECK_THROWS_AS(
        FileSystem::instance().resolvePathOrThrow("__vapor_fs_test_missing__.xyz"),
        std::runtime_error
    );
}

TEST_CASE("FileSystem - resolvePathOrThrow returns path when found", "[filesystem]") {
    auto dir = makeTempDir("throw_hit");
    touchFile(dir, "present_vapor_fs.txt");

    FileSystem::instance().addSearchPath(dir.string(), 99);

    std::string resolved;
    CHECK_NOTHROW(resolved = FileSystem::instance().resolvePathOrThrow("present_vapor_fs.txt"));
    CHECK_FALSE(resolved.empty());
    CHECK(std::filesystem::exists(resolved));

    FileSystem::instance().removeSearchPath(dir.string());
    std::filesystem::remove_all(dir);
}

TEST_CASE("FileSystem - initialize is idempotent", "[filesystem]") {
    FileSystem::instance().initialize();
    FileSystem::instance().initialize(); // second call must not duplicate paths

    auto dir = makeTempDir("idem");
    touchFile(dir, "idem_vapor_fs.txt");
    FileSystem::instance().addSearchPath(dir.string(), 99);

    // Paths added after a double-init must still be searched
    auto result = FileSystem::instance().resolvePath("idem_vapor_fs.txt");
    REQUIRE(result.has_value());

    FileSystem::instance().removeSearchPath(dir.string());
    std::filesystem::remove_all(dir);
}
