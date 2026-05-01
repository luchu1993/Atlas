#include "server/server_config.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>

#include "serialization/data_section.h"

namespace atlas {

namespace {

auto ParseAddress(std::string_view s) -> Result<Address> {
  auto colon = s.rfind(':');
  if (colon == std::string_view::npos)
    return Error{ErrorCode::kInvalidArgument, std::format("address '{}' missing port", s)};

  auto host = s.substr(0, colon);
  auto port_str = s.substr(colon + 1);

  uint16_t port = 0;
  try {
    int p = std::stoi(std::string(port_str));
    if (p < 0 || p > 65535) throw std::out_of_range("port out of range");
    port = static_cast<uint16_t>(p);
  } catch (...) {
    return Error{ErrorCode::kInvalidArgument, std::format("invalid port in address '{}'", s)};
  }

  return Address::Resolve(host, port).AndThen([](Address a) -> Result<Address> { return a; });
}

auto ParseLogLevel(std::string_view s) -> LogLevel {
  auto lower = std::string(s);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (lower == "trace") return LogLevel::kTrace;
  if (lower == "debug") return LogLevel::kDebug;
  if (lower == "info") return LogLevel::kInfo;
  if (lower == "warning" || lower == "warn") return LogLevel::kWarning;
  if (lower == "error") return LogLevel::kError;
  if (lower == "critical") return LogLevel::kCritical;
  if (lower == "off") return LogLevel::kOff;
  return LogLevel::kInfo;
}

auto ParseBoolString(std::string_view s) -> bool {
  auto lower = std::string(s);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

void LoadStringArray(const DataSection* section, std::vector<std::string>& out) {
  if (!section) {
    return;
  }

  out.clear();
  for (auto* child : section->Children()) {
    out.emplace_back(child->Value());
  }
}

using FieldPtr =
    std::variant<std::string ServerConfig::*, int ServerConfig::*, uint16_t ServerConfig::*,
                 bool ServerConfig::*, std::filesystem::path ServerConfig::*>;

struct CliField {
  std::string_view key;
  FieldPtr ptr;
};

// clang-format off
static const CliField kCliFields[] = {
    {"name",                       &ServerConfig::process_name},
    {"internal-port",              &ServerConfig::internal_port},
    {"external-port",              &ServerConfig::external_port},
    {"update-hertz",               &ServerConfig::update_hertz},
    {"assembly",                   &ServerConfig::script_assembly},
    {"runtime-config",             &ServerConfig::runtime_config},
    {"entitydef-bin-path",         &ServerConfig::entitydef_bin_path},
    {"db-type",                    &ServerConfig::db_type},
    {"db-xml-dir",                 &ServerConfig::db_xml_dir},
    {"db-sqlite-path",             &ServerConfig::db_sqlite_path},
    {"db-sqlite-wal",              &ServerConfig::db_sqlite_wal},
    {"db-sqlite-busy-timeout-ms",  &ServerConfig::db_sqlite_busy_timeout_ms},
    {"db-sqlite-foreign-keys",     &ServerConfig::db_sqlite_foreign_keys},
    {"db-mysql-host",              &ServerConfig::db_mysql_host},
    {"db-mysql-port",              &ServerConfig::db_mysql_port},
    {"db-mysql-user",              &ServerConfig::db_mysql_user},
    {"db-mysql-password",          &ServerConfig::db_mysql_password},
    {"db-mysql-database",          &ServerConfig::db_mysql_database},
    {"db-mysql-pool-size",         &ServerConfig::db_mysql_pool_size},
    {"auto-create-accounts",       &ServerConfig::auto_create_accounts},
    {"account-type-id",            &ServerConfig::account_type_id},
    {"login-rate-limit-per-ip",    &ServerConfig::login_rate_limit_per_ip},
    {"login-rate-limit-global",    &ServerConfig::login_rate_limit_global},
    {"login-rate-limit-window-sec",&ServerConfig::login_rate_limit_window_sec},
};
// clang-format on

auto ParseAndAssign(ServerConfig& cfg, const FieldPtr& ptr, std::string_view key,
                    std::string_view val) -> std::optional<Error> {
  return std::visit(
      [&](auto member) -> std::optional<Error> {
        using T = std::remove_reference_t<decltype(cfg.*member)>;

        if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::filesystem::path>) {
          cfg.*member = std::string(val);
        } else if constexpr (std::is_same_v<T, bool>) {
          cfg.*member = ParseBoolString(val);
        } else if constexpr (std::is_same_v<T, int>) {
          try {
            cfg.*member = std::stoi(std::string(val));
          } catch (...) {
            return Error{ErrorCode::kInvalidArgument, std::format("invalid --{}", key)};
          }
        } else if constexpr (std::is_same_v<T, uint16_t>) {
          try {
            int p = std::stoi(std::string(val));
            if (p < 0 || p > 65535) throw std::out_of_range("out of range");
            cfg.*member = static_cast<uint16_t>(p);
          } catch (...) {
            return Error{ErrorCode::kInvalidArgument, std::format("invalid --{}", key)};
          }
        }
        return std::nullopt;
      },
      ptr);
}

}  // namespace

auto ServerConfig::FromJsonFile(const std::filesystem::path& path) -> Result<ServerConfig> {
  auto tree_result = DataSectionTree::FromJson(path);
  if (!tree_result) return tree_result.Error();

  auto tree = std::move(*tree_result);
  auto* root = tree->Root();

  ServerConfig cfg;
  cfg.raw_config = tree;

  cfg.update_hertz =
      static_cast<int>(root->ReadUint("update_hertz", static_cast<uint32_t>(cfg.update_hertz)));
  cfg.is_production = root->ReadBool("is_production", cfg.is_production);

  auto machined_str = root->ReadString("machined_address", "");
  if (!machined_str.empty()) {
    auto addr_result = ParseAddress(machined_str);
    if (addr_result) cfg.machined_address = *addr_result;
  }

  auto log_level_str = root->ReadString("log_level", "");
  if (!log_level_str.empty()) cfg.log_level = ParseLogLevel(log_level_str);

  if (auto* script = root->Child("script")) {
    auto assembly = script->ReadString("assembly", "");
    if (!assembly.empty()) cfg.script_assembly = assembly;

    auto runtimecfg = script->ReadString("runtime_config", "");
    if (!runtimecfg.empty()) cfg.runtime_config = runtimecfg;
  }

  if (auto* db = root->Child("database")) {
    auto edef_bin = db->ReadString("entitydef_bin_path", "");
    if (!edef_bin.empty()) cfg.entitydef_bin_path = edef_bin;

    auto dbtype = db->ReadString("type", "");
    if (!dbtype.empty()) cfg.db_type = dbtype;

    cfg.db_xml_dir = db->ReadString("xml_dir", cfg.db_xml_dir.string());
    cfg.db_sqlite_path = db->ReadString("sqlite_path", cfg.db_sqlite_path.string());
    cfg.db_sqlite_wal = db->ReadBool("sqlite_wal", cfg.db_sqlite_wal);
    cfg.db_sqlite_busy_timeout_ms =
        db->ReadInt("sqlite_busy_timeout_ms", cfg.db_sqlite_busy_timeout_ms);
    cfg.db_sqlite_foreign_keys = db->ReadBool("sqlite_foreign_keys", cfg.db_sqlite_foreign_keys);
    cfg.db_mysql_host = db->ReadString("mysql_host", cfg.db_mysql_host);
    cfg.db_mysql_port = static_cast<uint16_t>(db->ReadUint("mysql_port", cfg.db_mysql_port));
    cfg.db_mysql_user = db->ReadString("mysql_user", cfg.db_mysql_user);
    cfg.db_mysql_password = db->ReadString("mysql_password", cfg.db_mysql_password);
    cfg.db_mysql_database = db->ReadString("mysql_database", cfg.db_mysql_database);
    cfg.db_mysql_pool_size = db->ReadInt("mysql_pool_size", cfg.db_mysql_pool_size);
  }

  cfg.auto_create_accounts = root->ReadBool("auto_create_accounts", cfg.auto_create_accounts);
  cfg.account_type_id =
      static_cast<uint16_t>(root->ReadUint("account_type_id", cfg.account_type_id));
  cfg.login_rate_limit_per_ip =
      root->ReadInt("login_rate_limit_per_ip", cfg.login_rate_limit_per_ip);
  cfg.login_rate_limit_global =
      root->ReadInt("login_rate_limit_global", cfg.login_rate_limit_global);
  cfg.login_rate_limit_window_sec =
      root->ReadInt("login_rate_limit_window_sec", cfg.login_rate_limit_window_sec);
  LoadStringArray(root->Child("login_rate_limit_trusted_cidrs"),
                  cfg.login_rate_limit_trusted_cidrs);

  if (auto* auth = root->Child("authentication")) {
    cfg.auto_create_accounts = auth->ReadBool("auto_create_accounts", cfg.auto_create_accounts);
    cfg.account_type_id =
        static_cast<uint16_t>(auth->ReadUint("account_type_id", cfg.account_type_id));

    if (auto* rate_limit = auth->Child("rate_limit")) {
      cfg.login_rate_limit_per_ip = rate_limit->ReadInt("per_ip", cfg.login_rate_limit_per_ip);
      cfg.login_rate_limit_global = rate_limit->ReadInt("global", cfg.login_rate_limit_global);
      cfg.login_rate_limit_window_sec =
          rate_limit->ReadInt("window_sec", cfg.login_rate_limit_window_sec);
      LoadStringArray(rate_limit->Child("trusted_cidrs"), cfg.login_rate_limit_trusted_cidrs);
    }
  }

  return cfg;
}

auto ServerConfig::FromArgs(int argc, char* argv[]) -> Result<ServerConfig> {
  ServerConfig cfg;

  for (int i = 1; i < argc - 1; ++i) {
    std::string_view key = argv[i];
    std::string_view val = argv[i + 1];

    if (key.starts_with("--"))
      key.remove_prefix(2);
    else
      continue;

    if (key == "type") {
      auto pt = ProcessTypeFromName(val);
      if (!pt) return pt.Error();
      cfg.process_type = *pt;
      ++i;
    } else if (key == "machined") {
      auto addr = ParseAddress(val);
      if (!addr) return addr.Error();
      cfg.machined_address = *addr;
      ++i;
    } else if (key == "log-level") {
      cfg.log_level = ParseLogLevel(val);
      ++i;
    } else if (key == "login-rate-limit-trusted-cidr") {
      cfg.login_rate_limit_trusted_cidrs.emplace_back(val);
      ++i;
    } else {
      for (const auto& field : kCliFields) {
        if (key == field.key) {
          auto err = ParseAndAssign(cfg, field.ptr, key, val);
          if (err) return *err;
          ++i;
          break;
        }
      }
    }
  }

  return cfg;
}

auto ServerConfig::Load(int argc, char* argv[]) -> Result<ServerConfig> {
  std::filesystem::path config_path;
  for (int i = 1; i < argc - 1; ++i) {
    std::string_view key = argv[i];
    if (key == "--config") {
      config_path = argv[i + 1];
      break;
    }
  }

  ServerConfig cfg;

  if (!config_path.empty()) {
    auto file_result = FromJsonFile(config_path);
    if (!file_result) return file_result.Error();
    cfg = std::move(*file_result);
  }

  auto cli_result = FromArgs(argc, argv);
  if (!cli_result) return cli_result.Error();

  auto& cli = *cli_result;

  const auto kHasCliKey = [argc, argv](std::string_view wanted) -> bool {
    for (int i = 1; i < argc; ++i) {
      std::string_view key = argv[i];
      if (!key.starts_with("--")) continue;
      key.remove_prefix(2);
      if (key == wanted) return true;
    }
    return false;
  };

  ServerConfig defaults;

  if (kHasCliKey("type")) cfg.process_type = cli.process_type;
  if (kHasCliKey("machined")) cfg.machined_address = cli.machined_address;
  if (kHasCliKey("log-level")) cfg.log_level = cli.log_level;
  if (kHasCliKey("login-rate-limit-trusted-cidr"))
    cfg.login_rate_limit_trusted_cidrs = cli.login_rate_limit_trusted_cidrs;

  for (const auto& field : kCliFields) {
    if (kHasCliKey(field.key)) std::visit([&](auto m) { cfg.*m = cli.*m; }, field.ptr);
  }

  if (cfg.process_name.empty()) cfg.process_name = std::string(ProcessTypeName(cfg.process_type));

  if (cfg.machined_address == Address::kNone || cfg.machined_address == defaults.machined_address) {
    auto default_addr = Address::Resolve("127.0.0.1", 20018);
    if (default_addr) cfg.machined_address = *default_addr;
  }

  return cfg;
}

}  // namespace atlas
