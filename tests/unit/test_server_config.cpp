#include "serialization/data_section.hpp"
#include "server/server_app_option.hpp"
#include "server/server_config.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace atlas;

// ============================================================================
// Helpers
// ============================================================================

// Build a fake argv from a vector of strings.
struct FakeArgv
{
    std::vector<std::string> storage;
    std::vector<char*> ptrs;

    explicit FakeArgv(std::vector<std::string> args) : storage(std::move(args))
    {
        for (auto& s : storage)
            ptrs.push_back(s.data());
    }

    int argc() { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }
};

// Write a temporary JSON config file and return its path.
static auto write_temp_json(std::string_view content) -> std::filesystem::path
{
    auto path = std::filesystem::temp_directory_path() / "atlas_test_config.json";
    std::ofstream f(path);
    f << content;
    return path;
}

// ============================================================================
// process_type_name / process_type_from_name
// ============================================================================

TEST(ProcessType, RoundTrip)
{
    auto types = {ProcessType::Machined,   ProcessType::LoginApp, ProcessType::BaseApp,
                  ProcessType::BaseAppMgr, ProcessType::CellApp,  ProcessType::CellAppMgr,
                  ProcessType::DBApp,      ProcessType::DBAppMgr, ProcessType::Reviver};

    for (auto t : types)
    {
        auto name = process_type_name(t);
        auto back = process_type_from_name(name);
        ASSERT_TRUE(back.has_value()) << "failed for " << name;
        EXPECT_EQ(*back, t);
    }
}

TEST(ProcessType, CaseInsensitive)
{
    auto r = process_type_from_name("BaseApp");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, ProcessType::BaseApp);

    r = process_type_from_name("CELLAPP");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, ProcessType::CellApp);
}

TEST(ProcessType, UnknownReturnsError)
{
    auto r = process_type_from_name("nonexistent");
    EXPECT_FALSE(r.has_value());
}

// ============================================================================
// ServerConfig::from_args — basic parsing
// ============================================================================

TEST(ServerConfig, FromArgsParsesType)
{
    FakeArgv args({"exe", "--type", "cellapp"});
    auto r = ServerConfig::from_args(args.argc(), args.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->process_type, ProcessType::CellApp);
}

TEST(ServerConfig, FromArgsParsesName)
{
    FakeArgv args({"exe", "--name", "baseapp01"});
    auto r = ServerConfig::from_args(args.argc(), args.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->process_name, "baseapp01");
}

TEST(ServerConfig, FromArgsParsesMachined)
{
    FakeArgv args({"exe", "--machined", "127.0.0.1:20018"});
    auto r = ServerConfig::from_args(args.argc(), args.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->machined_address.port(), 20018);
}

TEST(ServerConfig, FromArgsParsesInternalPort)
{
    FakeArgv args({"exe", "--internal-port", "9000"});
    auto r = ServerConfig::from_args(args.argc(), args.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->internal_port, 9000);
}

TEST(ServerConfig, FromArgsParsesExternalPort)
{
    FakeArgv args({"exe", "--external-port", "20100"});
    auto r = ServerConfig::from_args(args.argc(), args.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->external_port, 20100);
}

TEST(ServerConfig, FromArgsParsesUpdateHertz)
{
    FakeArgv args({"exe", "--update-hertz", "20"});
    auto r = ServerConfig::from_args(args.argc(), args.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->update_hertz, 20);
}

TEST(ServerConfig, FromArgsParsesLogLevel)
{
    FakeArgv args({"exe", "--log-level", "debug"});
    auto r = ServerConfig::from_args(args.argc(), args.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->log_level, LogLevel::Debug);
}

TEST(ServerConfig, FromArgsUnknownFlagsIgnored)
{
    FakeArgv args({"exe", "--unknown-flag", "value", "--name", "test"});
    auto r = ServerConfig::from_args(args.argc(), args.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->process_name, "test");
}

TEST(ServerConfig, FromArgsInvalidTypeReturnsError)
{
    FakeArgv args({"exe", "--type", "bogusapp"});
    auto r = ServerConfig::from_args(args.argc(), args.argv());
    EXPECT_FALSE(r.has_value());
}

TEST(ServerConfig, FromArgsInvalidPortReturnsError)
{
    FakeArgv args({"exe", "--internal-port", "not_a_number"});
    auto r = ServerConfig::from_args(args.argc(), args.argv());
    EXPECT_FALSE(r.has_value());
}

// ============================================================================
// ServerConfig::from_json_file
// ============================================================================

TEST(ServerConfig, FromJsonFile)
{
    auto path = write_temp_json(R"({
        "update_hertz": 20,
        "machined_address": "127.0.0.1:20018",
        "is_production": true,
        "log_level": "warning",
        "script": {
            "assembly": "Atlas.Runtime.dll",
            "runtime_config": "atlas.runtimeconfig.json"
        }
    })");

    auto r = ServerConfig::from_json_file(path);
    ASSERT_TRUE(r.has_value()) << r.error().message();

    EXPECT_EQ(r->update_hertz, 20);
    EXPECT_TRUE(r->is_production);
    EXPECT_EQ(r->machined_address.port(), 20018);
    EXPECT_EQ(r->log_level, LogLevel::Warning);
    EXPECT_EQ(r->script_assembly, std::filesystem::path("Atlas.Runtime.dll"));
    EXPECT_EQ(r->runtime_config, std::filesystem::path("atlas.runtimeconfig.json"));
    EXPECT_NE(r->raw_config, nullptr);
}

TEST(ServerConfig, FromJsonFileMissingFileReturnsError)
{
    auto r = ServerConfig::from_json_file("/nonexistent/path/config.json");
    EXPECT_FALSE(r.has_value());
}

TEST(ServerConfig, FromJsonFileMalformedReturnsError)
{
    auto path = write_temp_json("{ invalid json {{");
    auto r = ServerConfig::from_json_file(path);
    EXPECT_FALSE(r.has_value());
}

TEST(ServerConfig, FromJsonFilePartialKeys)
{
    auto path = write_temp_json(R"({ "update_hertz": 5 })");
    auto r = ServerConfig::from_json_file(path);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->update_hertz, 5);
    // Defaults preserved
    EXPECT_EQ(r->log_level, LogLevel::Info);
    EXPECT_FALSE(r->is_production);
}

// ============================================================================
// ServerConfig::load — CLI overrides JSON
// ============================================================================

TEST(ServerConfig, LoadCliOverridesJson)
{
    auto path = write_temp_json(R"({
        "update_hertz": 10,
        "log_level": "info"
    })");

    FakeArgv args(
        {"exe", "--config", path.string(), "--update-hertz", "30", "--log-level", "debug"});
    auto r = ServerConfig::load(args.argc(), args.argv());
    ASSERT_TRUE(r.has_value()) << r.error().message();

    EXPECT_EQ(r->update_hertz, 30);            // CLI wins
    EXPECT_EQ(r->log_level, LogLevel::Debug);  // CLI wins
}

TEST(ServerConfig, LoadNoConfigFile)
{
    FakeArgv args({"exe", "--type", "machined", "--name", "main"});
    auto r = ServerConfig::load(args.argc(), args.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->process_type, ProcessType::Machined);
    EXPECT_EQ(r->process_name, "main");
}

TEST(ServerConfig, LoadDefaultProcessName)
{
    FakeArgv args({"exe", "--type", "cellapp"});
    auto r = ServerConfig::load(args.argc(), args.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->process_name, "cellapp");  // derived from type
}

TEST(ServerConfig, LoadDefaultMachinedAddress)
{
    FakeArgv args({"exe"});
    auto r = ServerConfig::load(args.argc(), args.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->machined_address.port(), 20018);
}

// ============================================================================
// ServerAppOption — load from DataSection + Watcher registration
// ============================================================================

TEST(ServerAppOption, DefaultValue)
{
    ServerAppOption<int> opt{42, "missing_key", "test/opt"};
    EXPECT_EQ(opt.value(), 42);
    // Cleanup: opt is stack-allocated, destructor removes from global list
}

TEST(ServerAppOption, LoadFromDataSection)
{
    auto path = write_temp_json(R"({ "my_hertz": 25 })");
    auto tree_r = DataSection::from_json(path);
    ASSERT_TRUE(tree_r.has_value());

    ServerAppOption<int> opt{10, "my_hertz", "test/my_hertz"};
    opt.load_from(*(*tree_r)->root());
    EXPECT_EQ(opt.value(), 25);
}

TEST(ServerAppOption, LoadBoolFromDataSection)
{
    auto path = write_temp_json(R"({ "feature_on": true })");
    auto tree_r = DataSection::from_json(path);
    ASSERT_TRUE(tree_r.has_value());

    ServerAppOption<bool> opt{false, "feature_on", "test/feature_on"};
    opt.load_from(*(*tree_r)->root());
    EXPECT_TRUE(opt.value());
}

TEST(ServerAppOption, RegisterWatcherReadOnly)
{
    ServerAppOption<int> opt{7, "key", "test/read_only_opt", WatcherMode::ReadOnly};
    WatcherRegistry reg;
    opt.register_watcher(reg);

    EXPECT_EQ(reg.get("test/read_only_opt").value_or(""), "7");
    EXPECT_FALSE(reg.set("test/read_only_opt", "99"));  // ReadOnly
}

TEST(ServerAppOption, RegisterWatcherReadWrite)
{
    ServerAppOption<int> opt{7, "key", "test/rw_opt", WatcherMode::ReadWrite};
    WatcherRegistry reg;
    opt.register_watcher(reg);

    EXPECT_TRUE(reg.set("test/rw_opt", "50"));
    EXPECT_EQ(reg.get("test/rw_opt").value_or(""), "50");
}
