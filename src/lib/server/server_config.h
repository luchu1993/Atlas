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

// The raw DataSectionTree is retained so that ServerAppOption<T> instances
// can extract their own keys without requiring explicit support here.
struct ServerConfig {
  ProcessType process_type = ProcessType::kBaseApp;
  std::string process_name;

  Address machined_address{};  // default resolved lazily from "127.0.0.1:20018"
  uint16_t internal_port = 0;  // 0 = OS-assigned
  uint16_t external_port = 0;

  int update_hertz = 10;
  // Label the profiler attaches to each frame produced by AdvanceTime().
  // Empty means "derive from process_name at startup", which is what we
  // want by default. Tracy stores the final string pointer as frame identity.
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

  bool auto_create_accounts{false};
  uint16_t account_type_id{0};
  int login_rate_limit_per_ip{5};
  int login_rate_limit_global{1000};
  int login_rate_limit_window_sec{60};
  std::vector<std::string> login_rate_limit_trusted_cidrs;

  LogLevel log_level = LogLevel::kInfo;

  bool is_production = false;

  std::shared_ptr<DataSectionTree> raw_config;

  [[nodiscard]] static auto FromArgs(int argc, char* argv[]) -> Result<ServerConfig>;

  [[nodiscard]] static auto FromJsonFile(const std::filesystem::path& path) -> Result<ServerConfig>;

  [[nodiscard]] static auto Load(int argc, char* argv[]) -> Result<ServerConfig>;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_SERVER_CONFIG_H_
