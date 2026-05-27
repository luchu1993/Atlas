#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "serialization/data_section.h"
#include "server/server_app_option.h"
#include "server/server_config.h"

using namespace atlas;

struct FakeArgv {
  std::vector<std::string> storage;
  std::vector<char*> ptrs;

  explicit FakeArgv(std::vector<std::string> args) : storage(std::move(args)) {
    for (auto& s : storage) ptrs.push_back(s.data());
  }

  int argc() { return static_cast<int>(ptrs.size()); }
  char** argv() { return ptrs.data(); }
};

static auto write_temp_json(std::string_view content) -> std::filesystem::path {
  auto path = std::filesystem::temp_directory_path() / "atlas_test_config.json";
  std::ofstream f(path);
  f << content;
  return path;
}

TEST(ProcessType, RoundTrip) {
  auto types = {ProcessType::kMachined,   ProcessType::kLoginApp, ProcessType::kBaseApp,
                ProcessType::kBaseAppMgr, ProcessType::kCellApp,  ProcessType::kCellAppMgr,
                ProcessType::kDbApp,      ProcessType::kDbAppMgr, ProcessType::kReviver};

  for (auto t : types) {
    auto name = ProcessTypeName(t);
    auto back = ProcessTypeFromName(name);
    ASSERT_TRUE(back.HasValue()) << "failed for " << name;
    EXPECT_EQ(*back, t);
  }
}

TEST(ProcessType, CaseInsensitive) {
  auto r = ProcessTypeFromName("BaseApp");
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(*r, ProcessType::kBaseApp);

  r = ProcessTypeFromName("CELLAPP");
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(*r, ProcessType::kCellApp);
}

TEST(ProcessType, UnknownReturnsError) {
  auto r = ProcessTypeFromName("nonexistent");
  EXPECT_FALSE(r.HasValue());
}

TEST(ServerConfig, FromArgsParsesType) {
  FakeArgv args({"exe", "--type", "cellapp"});
  auto r = ServerConfig::FromArgs(args.argc(), args.argv());
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(r->process_type, ProcessType::kCellApp);
}

TEST(ServerConfig, FromArgsParsesName) {
  FakeArgv args({"exe", "--name", "baseapp01"});
  auto r = ServerConfig::FromArgs(args.argc(), args.argv());
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(r->process_name, "baseapp01");
}

TEST(ServerConfig, FromArgsParsesMachined) {
  FakeArgv args({"exe", "--machined", "127.0.0.1:20018"});
  auto r = ServerConfig::FromArgs(args.argc(), args.argv());
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(r->machined_address.Port(), 20018);
}

TEST(ServerConfig, FromArgsParsesInternalPort) {
  FakeArgv args({"exe", "--internal-port", "9000"});
  auto r = ServerConfig::FromArgs(args.argc(), args.argv());
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(r->internal_port, 9000);
}

TEST(ServerConfig, FromArgsParsesExternalPort) {
  FakeArgv args({"exe", "--external-port", "20100"});
  auto r = ServerConfig::FromArgs(args.argc(), args.argv());
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(r->external_port, 20100);
}

TEST(ServerConfig, FromArgsParsesUpdateHertz) {
  FakeArgv args({"exe", "--update-hertz", "20"});
  auto r = ServerConfig::FromArgs(args.argc(), args.argv());
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(r->update_hertz, 20);
}

TEST(ServerConfig, FromArgsParsesLogLevel) {
  FakeArgv args({"exe", "--log-level", "debug"});
  auto r = ServerConfig::FromArgs(args.argc(), args.argv());
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(r->log_level, LogLevel::kDebug);
}

TEST(ServerConfig, FromArgsUnknownFlagsIgnored) {
  FakeArgv args({"exe", "--unknown-flag", "value", "--name", "test"});
  auto r = ServerConfig::FromArgs(args.argc(), args.argv());
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(r->process_name, "test");
}

TEST(ServerConfig, FromArgsInvalidTypeReturnsError) {
  FakeArgv args({"exe", "--type", "bogusapp"});
  auto r = ServerConfig::FromArgs(args.argc(), args.argv());
  EXPECT_FALSE(r.HasValue());
}

TEST(ServerConfig, FromArgsParsesEntitydefBinPath) {
  FakeArgv args({"exe", "--entitydef-bin-path", "data/entity_defs.bin"});
  auto r = ServerConfig::FromArgs(args.argc(), args.argv());
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(r->entitydef_bin_path, std::filesystem::path("data/entity_defs.bin"));
}

TEST(ServerConfig, FromArgsParsesHaOptions) {
  FakeArgv args({"exe",
                 "--snapshot-path",
                 "snapshots/cellappmgr.bin",
                 "--snapshot-interval-ms",
                 "250",
                 "--revive-cellappmgr-exe",
                 "bin/atlas_cellappmgr.exe",
                 "--revive-cellappmgr-name",
                 "cellappmgr_a",
                 "--revive-cellappmgr-port",
                 "31000",
                 "--revive-cellappmgr-snapshot-path",
                 "snapshots/revived_cellappmgr.bin",
                 "--revive-cellappmgr-output-path",
                 "logs/revived_cellappmgr.log",
                 "--revive-cellappmgr-snapshot-interval-ms",
                 "1250",
                 "--revive-cellappmgr-update-hertz",
                 "25",
                 "--revive-cellappmgr-launch-timeout-ms",
                 "650",
                 "--revive-restart-delay-ms",
                 "75",
                 "--revive-restart-backoff-cap-ms",
                 "8000",
                 "--revive-max-restarts",
                 "7",
                 "--revive-cellappmgr-health-interval-ms",
                 "100",
                 "--revive-cellappmgr-heartbeat-timeout-ms",
                 "900",
                 "--revive-cellappmgr-manager-health-timeout-ms",
                 "450",
                 "--revive-cellappmgr-health-failure-threshold",
                 "3",
                 "--revive-cellappmgr-audit-interval-ms",
                 "125",
                 "--revive-cellappmgr-missing-audit-threshold",
                 "4",
                 "--revive-leader-lock-path",
                 "run/reviver_cellappmgr.lock",
                 "--revive-cellappmgr-on-start",
                 "true",
                 "--revive-baseappmgr-exe",
                 "bin/atlas_baseappmgr.exe",
                 "--revive-baseappmgr-name",
                 "baseappmgr_a",
                 "--revive-baseappmgr-port",
                 "31100",
                 "--revive-baseappmgr-snapshot-path",
                 "snapshots/revived_baseappmgr.bin",
                 "--revive-baseappmgr-output-path",
                 "logs/revived_baseappmgr.log",
                 "--revive-baseappmgr-snapshot-interval-ms",
                 "1500",
                 "--revive-baseappmgr-update-hertz",
                 "30",
                 "--revive-baseappmgr-launch-timeout-ms",
                 "700",
                 "--revive-baseappmgr-leader-lock-path",
                 "run/reviver_baseappmgr.lock",
                 "--revive-baseappmgr-on-start",
                 "true"});
  auto r = ServerConfig::FromArgs(args.argc(), args.argv());
  ASSERT_TRUE(r.HasValue()) << r.Error().Message();
  EXPECT_EQ(r->snapshot_path, std::filesystem::path("snapshots/cellappmgr.bin"));
  EXPECT_EQ(r->snapshot_interval_ms, 250);
  EXPECT_EQ(r->revive_cellappmgr_exe, std::filesystem::path("bin/atlas_cellappmgr.exe"));
  EXPECT_EQ(r->revive_cellappmgr_name, "cellappmgr_a");
  EXPECT_EQ(r->revive_cellappmgr_internal_port, 31000);
  EXPECT_EQ(r->revive_cellappmgr_snapshot_path,
            std::filesystem::path("snapshots/revived_cellappmgr.bin"));
  EXPECT_EQ(r->revive_cellappmgr_output_path,
            std::filesystem::path("logs/revived_cellappmgr.log"));
  EXPECT_EQ(r->revive_cellappmgr_snapshot_interval_ms, 1250);
  EXPECT_EQ(r->revive_cellappmgr_update_hertz, 25);
  EXPECT_EQ(r->revive_cellappmgr_launch_timeout_ms, 650);
  EXPECT_EQ(r->revive_restart_delay_ms, 75);
  EXPECT_EQ(r->revive_restart_backoff_cap_ms, 8000);
  EXPECT_EQ(r->revive_max_restarts, 7);
  EXPECT_EQ(r->revive_cellappmgr_health_interval_ms, 100);
  EXPECT_EQ(r->revive_cellappmgr_heartbeat_timeout_ms, 900);
  EXPECT_EQ(r->revive_cellappmgr_manager_health_timeout_ms, 450);
  EXPECT_EQ(r->revive_cellappmgr_health_failure_threshold, 3);
  EXPECT_EQ(r->revive_cellappmgr_audit_interval_ms, 125);
  EXPECT_EQ(r->revive_cellappmgr_missing_audit_threshold, 4);
  EXPECT_EQ(r->revive_leader_lock_path, std::filesystem::path("run/reviver_cellappmgr.lock"));
  EXPECT_TRUE(r->revive_cellappmgr_on_start);
  EXPECT_EQ(r->revive_baseappmgr_exe, std::filesystem::path("bin/atlas_baseappmgr.exe"));
  EXPECT_EQ(r->revive_baseappmgr_name, "baseappmgr_a");
  EXPECT_EQ(r->revive_baseappmgr_internal_port, 31100);
  EXPECT_EQ(r->revive_baseappmgr_snapshot_path,
            std::filesystem::path("snapshots/revived_baseappmgr.bin"));
  EXPECT_EQ(r->revive_baseappmgr_output_path,
            std::filesystem::path("logs/revived_baseappmgr.log"));
  EXPECT_EQ(r->revive_baseappmgr_snapshot_interval_ms, 1500);
  EXPECT_EQ(r->revive_baseappmgr_update_hertz, 30);
  EXPECT_EQ(r->revive_baseappmgr_launch_timeout_ms, 700);
  EXPECT_EQ(r->revive_baseappmgr_leader_lock_path,
            std::filesystem::path("run/reviver_baseappmgr.lock"));
  EXPECT_TRUE(r->revive_baseappmgr_on_start);
}

TEST(ServerConfig, FromJsonFileParsesEntitydefBinPath) {
  auto path = write_temp_json(R"({
        "database": {
            "entitydef_bin_path": "data/entity_defs.bin"
        }
    })");
  auto r = ServerConfig::FromJsonFile(path);
  ASSERT_TRUE(r.HasValue()) << r.Error().Message();
  EXPECT_EQ(r->entitydef_bin_path, std::filesystem::path("data/entity_defs.bin"));
}

TEST(ServerConfig, FromJsonFileParsesHaOptions) {
  auto path = write_temp_json(R"({
        "snapshot": {
            "path": "snapshots/cellappmgr.bin",
            "interval_ms": 250
        },
        "reviver": {
            "restart_delay_ms": 75,
            "restart_backoff_cap_ms": 8000,
            "max_restarts": 7,
            "leader_lock_path": "run/reviver_cellappmgr.lock",
            "cellappmgr": {
                "exe": "bin/atlas_cellappmgr.exe",
                "name": "cellappmgr_a",
                "internal_port": 31000,
                "snapshot_path": "snapshots/revived_cellappmgr.bin",
                "output_path": "logs/revived_cellappmgr.log",
                "snapshot_interval_ms": 1250,
                "update_hertz": 25,
                "launch_timeout_ms": 650,
                "health_interval_ms": 100,
                "heartbeat_timeout_ms": 900,
                "manager_health_timeout_ms": 450,
                "health_failure_threshold": 3,
                "audit_interval_ms": 125,
                "missing_audit_threshold": 4,
                "on_start": true
            },
            "baseappmgr": {
                "exe": "bin/atlas_baseappmgr.exe",
                "name": "baseappmgr_a",
                "internal_port": 31100,
                "snapshot_path": "snapshots/revived_baseappmgr.bin",
                "output_path": "logs/revived_baseappmgr.log",
                "snapshot_interval_ms": 1500,
                "update_hertz": 30,
                "launch_timeout_ms": 700,
                "leader_lock_path": "run/reviver_baseappmgr.lock",
                "on_start": true
            }
        }
    })");
  auto r = ServerConfig::FromJsonFile(path);
  ASSERT_TRUE(r.HasValue()) << r.Error().Message();
  EXPECT_EQ(r->snapshot_path, std::filesystem::path("snapshots/cellappmgr.bin"));
  EXPECT_EQ(r->snapshot_interval_ms, 250);
  EXPECT_EQ(r->revive_restart_delay_ms, 75);
  EXPECT_EQ(r->revive_restart_backoff_cap_ms, 8000);
  EXPECT_EQ(r->revive_max_restarts, 7);
  EXPECT_EQ(r->revive_leader_lock_path, std::filesystem::path("run/reviver_cellappmgr.lock"));
  EXPECT_EQ(r->revive_cellappmgr_exe, std::filesystem::path("bin/atlas_cellappmgr.exe"));
  EXPECT_EQ(r->revive_cellappmgr_name, "cellappmgr_a");
  EXPECT_EQ(r->revive_cellappmgr_internal_port, 31000);
  EXPECT_EQ(r->revive_cellappmgr_snapshot_path,
            std::filesystem::path("snapshots/revived_cellappmgr.bin"));
  EXPECT_EQ(r->revive_cellappmgr_output_path,
            std::filesystem::path("logs/revived_cellappmgr.log"));
  EXPECT_EQ(r->revive_cellappmgr_snapshot_interval_ms, 1250);
  EXPECT_EQ(r->revive_cellappmgr_update_hertz, 25);
  EXPECT_EQ(r->revive_cellappmgr_launch_timeout_ms, 650);
  EXPECT_EQ(r->revive_cellappmgr_health_interval_ms, 100);
  EXPECT_EQ(r->revive_cellappmgr_heartbeat_timeout_ms, 900);
  EXPECT_EQ(r->revive_cellappmgr_manager_health_timeout_ms, 450);
  EXPECT_EQ(r->revive_cellappmgr_health_failure_threshold, 3);
  EXPECT_EQ(r->revive_cellappmgr_audit_interval_ms, 125);
  EXPECT_EQ(r->revive_cellappmgr_missing_audit_threshold, 4);
  EXPECT_TRUE(r->revive_cellappmgr_on_start);
  EXPECT_EQ(r->revive_baseappmgr_exe, std::filesystem::path("bin/atlas_baseappmgr.exe"));
  EXPECT_EQ(r->revive_baseappmgr_name, "baseappmgr_a");
  EXPECT_EQ(r->revive_baseappmgr_internal_port, 31100);
  EXPECT_EQ(r->revive_baseappmgr_snapshot_path,
            std::filesystem::path("snapshots/revived_baseappmgr.bin"));
  EXPECT_EQ(r->revive_baseappmgr_output_path,
            std::filesystem::path("logs/revived_baseappmgr.log"));
  EXPECT_EQ(r->revive_baseappmgr_snapshot_interval_ms, 1500);
  EXPECT_EQ(r->revive_baseappmgr_update_hertz, 30);
  EXPECT_EQ(r->revive_baseappmgr_launch_timeout_ms, 700);
  EXPECT_EQ(r->revive_baseappmgr_leader_lock_path,
            std::filesystem::path("run/reviver_baseappmgr.lock"));
  EXPECT_TRUE(r->revive_baseappmgr_on_start);
}

TEST(ServerConfig, FromArgsInvalidPortReturnsError) {
  FakeArgv args({"exe", "--internal-port", "not_a_number"});
  auto r = ServerConfig::FromArgs(args.argc(), args.argv());
  EXPECT_FALSE(r.HasValue());
}

TEST(ServerConfig, FromJsonFile) {
  auto path = write_temp_json(R"({
        "update_hertz": 20,
        "machined_address": "127.0.0.1:20018",
        "is_production": true,
        "log_level": "warning",
        "login_rate_limit_per_ip": 12,
        "login_rate_limit_global": 345,
        "login_rate_limit_window_sec": 9,
        "login_rate_limit_trusted_cidrs": ["127.0.0.1/32", "10.0.0.0/8"],
        "script": {
            "assembly": "Atlas.Runtime.dll",
            "runtime_config": "atlas.runtimeconfig.json"
        },
        "database": {
            "type": "sqlite",
            "sqlite_path": "data/dev.sqlite3",
            "sqlite_wal": false,
            "sqlite_busy_timeout_ms": 1234,
            "sqlite_foreign_keys": false
        }
    })");

  auto r = ServerConfig::FromJsonFile(path);
  ASSERT_TRUE(r.HasValue()) << r.Error().Message();

  EXPECT_EQ(r->update_hertz, 20);
  EXPECT_TRUE(r->is_production);
  EXPECT_EQ(r->machined_address.Port(), 20018);
  EXPECT_EQ(r->log_level, LogLevel::kWarning);
  EXPECT_EQ(r->login_rate_limit_per_ip, 12);
  EXPECT_EQ(r->login_rate_limit_global, 345);
  EXPECT_EQ(r->login_rate_limit_window_sec, 9);
  ASSERT_EQ(r->login_rate_limit_trusted_cidrs.size(), 2u);
  EXPECT_EQ(r->login_rate_limit_trusted_cidrs[0], "127.0.0.1/32");
  EXPECT_EQ(r->login_rate_limit_trusted_cidrs[1], "10.0.0.0/8");
  EXPECT_EQ(r->script_assembly, std::filesystem::path("Atlas.Runtime.dll"));
  EXPECT_EQ(r->runtime_config, std::filesystem::path("atlas.runtimeconfig.json"));
  EXPECT_EQ(r->db_type, "sqlite");
  EXPECT_EQ(r->db_sqlite_path, std::filesystem::path("data/dev.sqlite3"));
  EXPECT_FALSE(r->db_sqlite_wal);
  EXPECT_EQ(r->db_sqlite_busy_timeout_ms, 1234);
  EXPECT_FALSE(r->db_sqlite_foreign_keys);
  EXPECT_NE(r->raw_config, nullptr);
}

TEST(ServerConfig, FromJsonFileMissingFileReturnsError) {
  auto r = ServerConfig::FromJsonFile("/nonexistent/path/config.json");
  EXPECT_FALSE(r.HasValue());
}

TEST(ServerConfig, FromJsonFileMalformedReturnsError) {
  auto path = write_temp_json("{ invalid json {{");
  auto r = ServerConfig::FromJsonFile(path);
  EXPECT_FALSE(r.HasValue());
}

TEST(ServerConfig, FromJsonFilePartialKeys) {
  auto path = write_temp_json(R"({ "update_hertz": 5 })");
  auto r = ServerConfig::FromJsonFile(path);
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(r->update_hertz, 5);
  EXPECT_EQ(r->db_type, "sqlite");
  EXPECT_EQ(r->log_level, LogLevel::kInfo);
  EXPECT_FALSE(r->is_production);
}

TEST(ServerConfig, LoadCliOverridesJson) {
  auto path = write_temp_json(R"({
        "update_hertz": 10,
        "log_level": "info",
        "database": {
            "type": "xml",
            "xml_dir": "data/xml",
            "sqlite_path": "data/from_json.sqlite3",
            "sqlite_wal": true,
            "sqlite_busy_timeout_ms": 5000,
            "sqlite_foreign_keys": true
        },
        "authentication": {
            "rate_limit": {
                "per_ip": 5,
                "global": 1000,
                "window_sec": 60,
                "trusted_cidrs": ["127.0.0.1/32"]
            }
        }
    })");

  FakeArgv args({"exe",
                 "--config",
                 path.string(),
                 "--update-hertz",
                 "30",
                 "--log-level",
                 "debug",
                 "--db-type",
                 "sqlite",
                 "--db-sqlite-path",
                 "data/from_cli.sqlite3",
                 "--db-sqlite-wal",
                 "false",
                 "--db-sqlite-busy-timeout-ms",
                 "2222",
                 "--db-sqlite-foreign-keys",
                 "false",
                 "--login-rate-limit-per-ip",
                 "77",
                 "--login-rate-limit-global",
                 "88",
                 "--login-rate-limit-window-sec",
                 "99",
                 "--login-rate-limit-trusted-cidr",
                 "192.168.1.0/24"});
  auto r = ServerConfig::Load(args.argc(), args.argv());
  ASSERT_TRUE(r.HasValue()) << r.Error().Message();

  EXPECT_EQ(r->update_hertz, 30);
  EXPECT_EQ(r->log_level, LogLevel::kDebug);
  EXPECT_EQ(r->db_type, "sqlite");
  EXPECT_EQ(r->db_sqlite_path, std::filesystem::path("data/from_cli.sqlite3"));
  EXPECT_FALSE(r->db_sqlite_wal);
  EXPECT_EQ(r->db_sqlite_busy_timeout_ms, 2222);
  EXPECT_FALSE(r->db_sqlite_foreign_keys);
  EXPECT_EQ(r->login_rate_limit_per_ip, 77);
  EXPECT_EQ(r->login_rate_limit_global, 88);
  EXPECT_EQ(r->login_rate_limit_window_sec, 99);
  ASSERT_EQ(r->login_rate_limit_trusted_cidrs.size(), 1u);
  EXPECT_EQ(r->login_rate_limit_trusted_cidrs[0], "192.168.1.0/24");
  EXPECT_EQ(r->config_path, std::filesystem::absolute(path));
}

TEST(ServerConfig, LoadNoConfigFile) {
  FakeArgv args({"exe", "--type", "machined", "--name", "main"});
  auto r = ServerConfig::Load(args.argc(), args.argv());
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(r->process_type, ProcessType::kMachined);
  EXPECT_EQ(r->process_name, "main");
  EXPECT_EQ(r->db_type, "sqlite");
}

TEST(ServerConfig, LoadDefaultProcessName) {
  FakeArgv args({"exe", "--type", "cellapp"});
  auto r = ServerConfig::Load(args.argc(), args.argv());
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(r->process_name, "cellapp");
}

TEST(ServerConfig, LoadDefaultMachinedAddress) {
  FakeArgv args({"exe"});
  auto r = ServerConfig::Load(args.argc(), args.argv());
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(r->machined_address.Port(), 20018);
}

TEST(ServerAppOption, DefaultValue) {
  ServerAppOption<int> opt{42, "missing_key", "test/opt"};
  EXPECT_EQ(opt.Value(), 42);
}

TEST(ServerAppOption, LoadFromDataSection) {
  auto path = write_temp_json(R"({ "my_hertz": 25 })");
  auto tree_r = DataSection::FromJson(path);
  ASSERT_TRUE(tree_r.HasValue());

  ServerAppOption<int> opt{10, "my_hertz", "test/my_hertz"};
  opt.LoadFrom(*(*tree_r)->Root());
  EXPECT_EQ(opt.Value(), 25);
}

TEST(ServerAppOption, LoadBoolFromDataSection) {
  auto path = write_temp_json(R"({ "feature_on": true })");
  auto tree_r = DataSection::FromJson(path);
  ASSERT_TRUE(tree_r.HasValue());

  ServerAppOption<bool> opt{false, "feature_on", "test/feature_on"};
  opt.LoadFrom(*(*tree_r)->Root());
  EXPECT_TRUE(opt.Value());
}

TEST(ServerAppOption, RegisterWatcherReadOnly) {
  ServerAppOption<int> opt{7, "key", "test/read_only_opt", WatcherMode::kReadOnly};
  WatcherRegistry reg;
  opt.RegisterWatcher(reg);

  EXPECT_EQ(reg.Get("test/read_only_opt").value_or(""), "7");
  EXPECT_FALSE(reg.Set("test/read_only_opt", "99"));
}

TEST(ServerAppOption, RegisterWatcherReadWrite) {
  ServerAppOption<int> opt{7, "key", "test/rw_opt", WatcherMode::kReadWrite};
  WatcherRegistry reg;
  opt.RegisterWatcher(reg);

  EXPECT_TRUE(reg.Set("test/rw_opt", "50"));
  EXPECT_EQ(reg.Get("test/rw_opt").value_or(""), "50");
}
