#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Vapor/resource_manager.hpp>
#include <Vapor/task_scheduler.hpp>
#include <fstream>
#include <filesystem>

using namespace Vapor;

TEST_CASE("ResourceManager Basic Loading", "[resource]") {
    TaskScheduler scheduler;
    scheduler.init(1);
    
    ResourceManager rm(scheduler);

    SECTION("Text file loading") {
        // Create a temporary file
        const std::string testFile = "test_resource.txt";
        {
            std::ofstream f(testFile);
            f << "Hello Vapor!";
        }

        auto resource = rm.loadText(testFile, LoadMode::Sync);
        
        REQUIRE(resource->isReady());
        REQUIRE(*resource->get() == "Hello Vapor!");

        // Test cache
        auto resource2 = rm.loadText(testFile, LoadMode::Sync);
        REQUIRE(resource == resource2); // Should be the same object from cache

        std::filesystem::remove(testFile);
    }

    SECTION("Async text loading") {
        const std::string testFile = "test_async.txt";
        {
            std::ofstream f(testFile);
            f << "Async Data";
        }

        auto resource = rm.loadText(testFile, LoadMode::Async);
        
        // It might not be ready immediately, so we wait
        rm.waitForAll();
        
        REQUIRE(resource->isReady());
        REQUIRE(*resource->get() == "Async Data");

        std::filesystem::remove(testFile);
    }

    scheduler.shutdown();
}

TEST_CASE("ResourceManager Cache Management", "[resource]") {
    TaskScheduler scheduler;
    scheduler.init(1);
    ResourceManager rm(scheduler);

    const std::string testFile = "test_cache.txt";
    {
        std::ofstream f(testFile);
        f << "Cache test";
    }

    rm.loadText(testFile, LoadMode::Sync);
    REQUIRE(rm.getTextCacheSize() == 1);

    rm.clearTextCache();
    REQUIRE(rm.getTextCacheSize() == 0);

    std::filesystem::remove(testFile);
    scheduler.shutdown();
}
