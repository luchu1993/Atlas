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
#include "network/address.h"

namespace atlas {

class DataSection;
class DataSectionTree;

// ============================================================================
// ProcessType
// ============================================================================

enum class ProcessType : uint8_t {
  kMachined = 0,
  kLoginApp = 1,
  kBaseApp = 2,
  kBaseAppMgr = 3,
  kCellApp = 4,
  kCellAppMgr = 5,
  kDbApp = 6,
  kDbAppMgr = 7,
  kReviver = 8,
};

[[nodiscard]] auto ProcessTypeName(ProcessType type) -> std::string_view;
[[nodiscard]] auto ProcessTypeFromName(std::string_view name) -> Result<ProcessType>;

// ============================================================================
// ServerConfig
//
// Loaded via ServerConfig::load(argc, argv):
//   1. Parse command-line flags.
//   2. If --config <file> was given, parse the JSON config file.
//   3. Command-line values override the JSON file.
//
// The raw DataSectionTree is retained so that ServerAppOption<T> instances
// can extract their own keys without requiring explicit support here.
// ============================================================================

struct ServerConfig {
  // ---- Identity -----------------------------------------------------------
  ProcessType process_type = ProcessType::kBaseApp;
  std::string process_name;

  // ---- Network ------------------------------------------------------------
  Address machined_address{};  // default resolved lazily from "127.0.0.1:20018"
  uint16_t internal_port = 0;  // 0 = OS-assigned
  uint16_t external_port = 0;

  // ---- Tick ---------------------------------------------------------------
  int update_hertz = 10;

  // ---- Script -------------------------------------------------------------
  std::filesystem::path script_assembly;
  std::filesystem::path runtime_config;

  // ---- Database -----------------------------------------------------------
  std::filesystem::path entitydef_path;  // entity_defs.json (DBApp)
  std::string db_type{"sqlite"};         // "xml", "sqlite", or "mysql"
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

  // ---- Authentication (DBApp) ---------------------------------------------
  bool auto_create_accounts{false};
  uint16_t account_type_id{0};
  int login_rate_limit_per_ip{5};
  int login_rate_limit_global{1000};
  int login_rate_limit_window_sec{60};
  std::vector<std::string> login_rate_limit_trusted_cidrs;

  // ---- Logging ------------------------------------------------------------
  LogLevel log_level = LogLevel::kInfo;

  // ---- Misc ---------------------------------------------------------------
  bool is_production = false;

  // ---- Raw config tree (for ServerAppOption) ------------------------------
  // Non-null when a JSON config file was loaded.
  std::shared_ptr<DataSectionTree> raw_config;

  // ========================================================================
  // Factory functions
  // ========================================================================

  /// Parse only from command-line arguments.
  [[nodiscard]] static auto FromArgs(int argc, char* argv[]) -> Result<ServerConfig>;

  /// Parse only from a JSON config file.
  [[nodiscard]] static auto FromJsonFile(const std::filesystem::path& path) -> Result<ServerConfig>;

  /// Full load: parse JSON file first (if --config given), then apply CLI overrides.
  [[nodiscard]] static auto Load(int argc, char* argv[]) -> Result<ServerConfig>;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_SERVER_CONFIG_H_
