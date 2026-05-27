#ifndef ATLAS_LIB_SERVER_SERVER_CONFIG_H_
#define ATLAS_LIB_SERVER_SERVER_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "foundation/error.h"
#include "foundation/log.h"
#include "foundation/process_type.h"
#include "network/address.h"

namespace atlas {

class DataSection;
class DataSectionTree;

// The raw DataSectionTree is retained so that ServerAppOption<T> instances
// can extract their own keys without requiring explicit support here.
struct ServerConfig {
  ProcessType process_type = ProcessType::kBaseApp;
  std::string process_name;

  Address machined_address{};  // default resolved lazily from "127.0.0.1:20018"
  uint16_t internal_port = 0;  // 0 = OS-assigned
  uint16_t external_port = 0;

  int update_hertz = 10;
  // Empty frame_name derives from process_name at startup; Tracy stores the
  // final string pointer as the frame identity.
  std::string frame_name;

  std::filesystem::path script_assembly;
  std::filesystem::path runtime_config;

  // Binary descriptor (ATDF, emitted by Atlas.Tools.DefDump) consumed by
  // EntityDefRegistry::RegisterFromBinaryFile on DBApp startup.
  std::filesystem::path entitydef_bin_path;
  std::string db_type{"sqlite"};  // "xml", "sqlite", or "mysql"
  std::filesystem::path db_xml_dir{"data/db"};
  std::filesystem::path db_sqlite_path{"data/atlas_dev.sqlite3"};
  bool db_sqlite_wal{true};
  int db_sqlite_busy_timeout_ms{5000};
  bool db_sqlite_foreign_keys{true};
  std::string db_mysql_host{"127.0.0.1"};
  uint16_t db_mysql_port{3306};
  std::string db_mysql_user;
  std::string db_mysql_password;
  std::string db_mysql_database{"atlas"};
  int db_mysql_pool_size{4};

  bool enable_hot_reload{false};
  std::filesystem::path hot_reload_script_project_path;
  std::filesystem::path hot_reload_output_directory;
  std::string hot_reload_assembly_name{"Atlas.GameScripts.dll"};
  int hot_reload_debounce_ms{500};
  int hot_reload_unload_timeout_ms{5000};
  bool hot_reload_auto_compile{true};

  bool auto_create_accounts{false};
  uint16_t account_type_id{0};
  int login_rate_limit_per_ip{5};
  int login_rate_limit_global{1000};
  int login_rate_limit_window_sec{60};
  std::vector<std::string> login_rate_limit_trusted_cidrs;

  LogLevel log_level = LogLevel::kInfo;

  bool is_production = false;

  std::filesystem::path snapshot_path;
  int snapshot_interval_ms{1000};

  std::filesystem::path revive_cellappmgr_exe;
  std::string revive_cellappmgr_name{"cellappmgr"};
  uint16_t revive_cellappmgr_internal_port{0};
  std::filesystem::path revive_cellappmgr_snapshot_path;
  std::filesystem::path revive_cellappmgr_output_path;
  int revive_cellappmgr_snapshot_interval_ms{-1};
  int revive_cellappmgr_update_hertz{10};
  int revive_cellappmgr_launch_timeout_ms{5000};
  int revive_restart_delay_ms{1000};
  // 0 disables backoff; positive cap doubles the base delay each attempt
  // up to backoff_cap_ms so a wedged exe can't burn the restart budget.
  int revive_restart_backoff_cap_ms{0};
  int revive_max_restarts{3};
  int revive_cellappmgr_health_interval_ms{1000};
  int revive_cellappmgr_heartbeat_timeout_ms{4000};
  int revive_cellappmgr_manager_health_timeout_ms{5000};
  int revive_cellappmgr_health_failure_threshold{2};
  int revive_cellappmgr_audit_interval_ms{1000};
  int revive_cellappmgr_missing_audit_threshold{2};
  bool revive_cellappmgr_on_start{false};
  std::filesystem::path revive_leader_lock_path;

  std::filesystem::path config_path;
  std::shared_ptr<DataSectionTree> raw_config;

  [[nodiscard]] static auto FromArgs(int argc, char* argv[]) -> Result<ServerConfig>;

  [[nodiscard]] static auto FromJsonFile(const std::filesystem::path& path) -> Result<ServerConfig>;

  [[nodiscard]] static auto Load(int argc, char* argv[]) -> Result<ServerConfig>;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_SERVER_CONFIG_H_
