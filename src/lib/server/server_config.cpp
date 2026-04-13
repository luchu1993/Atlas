#include "server/server_config.hpp"

#include "serialization/data_section.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>

namespace atlas
{

// ============================================================================
// ProcessType helpers
// ============================================================================

auto process_type_name(ProcessType type) -> std::string_view
{
    switch (type)
    {
        case ProcessType::Machined:
            return "machined";
        case ProcessType::LoginApp:
            return "loginapp";
        case ProcessType::BaseApp:
            return "baseapp";
        case ProcessType::BaseAppMgr:
            return "baseappmgr";
        case ProcessType::CellApp:
            return "cellapp";
        case ProcessType::CellAppMgr:
            return "cellappmgr";
        case ProcessType::DBApp:
            return "dbapp";
        case ProcessType::DBAppMgr:
            return "dbappmgr";
        case ProcessType::Reviver:
            return "reviver";
    }
    return "unknown";
}

auto process_type_from_name(std::string_view name) -> Result<ProcessType>
{
    auto lower = std::string(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower == "machined")
        return ProcessType::Machined;
    if (lower == "loginapp")
        return ProcessType::LoginApp;
    if (lower == "baseapp")
        return ProcessType::BaseApp;
    if (lower == "baseappmgr")
        return ProcessType::BaseAppMgr;
    if (lower == "cellapp")
        return ProcessType::CellApp;
    if (lower == "cellappmgr")
        return ProcessType::CellAppMgr;
    if (lower == "dbapp")
        return ProcessType::DBApp;
    if (lower == "dbappmgr")
        return ProcessType::DBAppMgr;
    if (lower == "reviver")
        return ProcessType::Reviver;

    return Error{ErrorCode::InvalidArgument, std::format("unknown process type: '{}'", name)};
}

// ============================================================================
// Internal helpers
// ============================================================================

namespace
{

/// Parse "host:port" string into an Address.
auto parse_address(std::string_view s) -> Result<Address>
{
    auto colon = s.rfind(':');
    if (colon == std::string_view::npos)
        return Error{ErrorCode::InvalidArgument, std::format("address '{}' missing port", s)};

    auto host = s.substr(0, colon);
    auto port_str = s.substr(colon + 1);

    uint16_t port = 0;
    try
    {
        int p = std::stoi(std::string(port_str));
        if (p < 0 || p > 65535)
            throw std::out_of_range("port out of range");
        port = static_cast<uint16_t>(p);
    }
    catch (...)
    {
        return Error{ErrorCode::InvalidArgument, std::format("invalid port in address '{}'", s)};
    }

    return Address::resolve(host, port).and_then([](Address a) -> Result<Address> { return a; });
}

auto parse_log_level(std::string_view s) -> LogLevel
{
    auto lower = std::string(s);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower == "trace")
        return LogLevel::Trace;
    if (lower == "debug")
        return LogLevel::Debug;
    if (lower == "info")
        return LogLevel::Info;
    if (lower == "warning" || lower == "warn")
        return LogLevel::Warning;
    if (lower == "error")
        return LogLevel::Error;
    if (lower == "critical")
        return LogLevel::Critical;
    if (lower == "off")
        return LogLevel::Off;
    return LogLevel::Info;
}

auto parse_bool_string(std::string_view s) -> bool
{
    auto lower = std::string(s);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

void load_string_array(const DataSection* section, std::vector<std::string>& out)
{
    if (!section)
    {
        return;
    }

    out.clear();
    for (auto* child : section->children())
    {
        out.emplace_back(child->value());
    }
}

// ============================================================================
// CLI field descriptor table
// ============================================================================

/// Type-erased pointer to a ServerConfig member field.
using FieldPtr =
    std::variant<std::string ServerConfig::*, int ServerConfig::*, uint16_t ServerConfig::*,
                 bool ServerConfig::*, std::filesystem::path ServerConfig::*>;

/// Maps a CLI key name to its corresponding ServerConfig member.
struct CliField
{
    std::string_view key;
    FieldPtr ptr;
};

// clang-format off
static const CliField cli_fields[] = {
    {"name",                       &ServerConfig::process_name},
    {"internal-port",              &ServerConfig::internal_port},
    {"external-port",              &ServerConfig::external_port},
    {"update-hertz",               &ServerConfig::update_hertz},
    {"assembly",                   &ServerConfig::script_assembly},
    {"runtime-config",             &ServerConfig::runtime_config},
    {"entitydef-path",             &ServerConfig::entitydef_path},
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

/// Parse a string value and assign it to the config field identified by @p ptr.
auto parse_and_assign(ServerConfig& cfg, const FieldPtr& ptr, std::string_view key,
                      std::string_view val) -> std::optional<Error>
{
    return std::visit(
        [&](auto member) -> std::optional<Error>
        {
            using T = std::remove_reference_t<decltype(cfg.*member)>;

            if constexpr (std::is_same_v<T, std::string> ||
                          std::is_same_v<T, std::filesystem::path>)
            {
                cfg.*member = std::string(val);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                cfg.*member = parse_bool_string(val);
            }
            else if constexpr (std::is_same_v<T, int>)
            {
                try
                {
                    cfg.*member = std::stoi(std::string(val));
                }
                catch (...)
                {
                    return Error{ErrorCode::InvalidArgument, std::format("invalid --{}", key)};
                }
            }
            else if constexpr (std::is_same_v<T, uint16_t>)
            {
                try
                {
                    int p = std::stoi(std::string(val));
                    if (p < 0 || p > 65535)
                        throw std::out_of_range("out of range");
                    cfg.*member = static_cast<uint16_t>(p);
                }
                catch (...)
                {
                    return Error{ErrorCode::InvalidArgument, std::format("invalid --{}", key)};
                }
            }
            return std::nullopt;
        },
        ptr);
}

}  // namespace

// ============================================================================
// ServerConfig::from_json_file
// ============================================================================

auto ServerConfig::from_json_file(const std::filesystem::path& path) -> Result<ServerConfig>
{
    auto tree_result = DataSectionTree::from_json(path);
    if (!tree_result)
        return tree_result.error();

    auto tree = std::move(*tree_result);
    auto* root = tree->root();

    ServerConfig cfg;
    cfg.raw_config = tree;

    cfg.update_hertz = root->read_int("update_hertz", cfg.update_hertz);
    cfg.is_production = root->read_bool("is_production", cfg.is_production);

    auto machined_str = root->read_string("machined_address", "");
    if (!machined_str.empty())
    {
        auto addr_result = parse_address(machined_str);
        if (addr_result)
            cfg.machined_address = *addr_result;
    }

    auto log_level_str = root->read_string("log_level", "");
    if (!log_level_str.empty())
        cfg.log_level = parse_log_level(log_level_str);

    if (auto* script = root->child("script"))
    {
        auto assembly = script->read_string("assembly", "");
        if (!assembly.empty())
            cfg.script_assembly = assembly;

        auto runtimecfg = script->read_string("runtime_config", "");
        if (!runtimecfg.empty())
            cfg.runtime_config = runtimecfg;
    }

    if (auto* db = root->child("database"))
    {
        auto edef = db->read_string("entitydef_path", "");
        if (!edef.empty())
            cfg.entitydef_path = edef;

        auto dbtype = db->read_string("type", "");
        if (!dbtype.empty())
            cfg.db_type = dbtype;

        cfg.db_xml_dir = db->read_string("xml_dir", cfg.db_xml_dir.string());
        cfg.db_sqlite_path = db->read_string("sqlite_path", cfg.db_sqlite_path.string());
        cfg.db_sqlite_wal = db->read_bool("sqlite_wal", cfg.db_sqlite_wal);
        cfg.db_sqlite_busy_timeout_ms =
            db->read_int("sqlite_busy_timeout_ms", cfg.db_sqlite_busy_timeout_ms);
        cfg.db_sqlite_foreign_keys =
            db->read_bool("sqlite_foreign_keys", cfg.db_sqlite_foreign_keys);
        cfg.db_mysql_host = db->read_string("mysql_host", cfg.db_mysql_host);
        cfg.db_mysql_port = static_cast<uint16_t>(db->read_uint("mysql_port", cfg.db_mysql_port));
        cfg.db_mysql_user = db->read_string("mysql_user", cfg.db_mysql_user);
        cfg.db_mysql_password = db->read_string("mysql_password", cfg.db_mysql_password);
        cfg.db_mysql_database = db->read_string("mysql_database", cfg.db_mysql_database);
        cfg.db_mysql_pool_size = db->read_int("mysql_pool_size", cfg.db_mysql_pool_size);
    }

    cfg.auto_create_accounts = root->read_bool("auto_create_accounts", cfg.auto_create_accounts);
    cfg.account_type_id =
        static_cast<uint16_t>(root->read_uint("account_type_id", cfg.account_type_id));
    cfg.login_rate_limit_per_ip =
        root->read_int("login_rate_limit_per_ip", cfg.login_rate_limit_per_ip);
    cfg.login_rate_limit_global =
        root->read_int("login_rate_limit_global", cfg.login_rate_limit_global);
    cfg.login_rate_limit_window_sec =
        root->read_int("login_rate_limit_window_sec", cfg.login_rate_limit_window_sec);
    load_string_array(root->child("login_rate_limit_trusted_cidrs"),
                      cfg.login_rate_limit_trusted_cidrs);

    if (auto* auth = root->child("authentication"))
    {
        cfg.auto_create_accounts =
            auth->read_bool("auto_create_accounts", cfg.auto_create_accounts);
        cfg.account_type_id =
            static_cast<uint16_t>(auth->read_uint("account_type_id", cfg.account_type_id));

        if (auto* rate_limit = auth->child("rate_limit"))
        {
            cfg.login_rate_limit_per_ip =
                rate_limit->read_int("per_ip", cfg.login_rate_limit_per_ip);
            cfg.login_rate_limit_global =
                rate_limit->read_int("global", cfg.login_rate_limit_global);
            cfg.login_rate_limit_window_sec =
                rate_limit->read_int("window_sec", cfg.login_rate_limit_window_sec);
            load_string_array(rate_limit->child("trusted_cidrs"),
                              cfg.login_rate_limit_trusted_cidrs);
        }
    }

    return cfg;
}

// ============================================================================
// ServerConfig::from_args
// ============================================================================

auto ServerConfig::from_args(int argc, char* argv[]) -> Result<ServerConfig>
{
    ServerConfig cfg;

    // Simple linear scan of argv pairs: --key value
    for (int i = 1; i < argc - 1; ++i)
    {
        std::string_view key = argv[i];
        std::string_view val = argv[i + 1];

        if (key.starts_with("--"))
            key.remove_prefix(2);
        else
            continue;

        // Special-case fields that need custom parsing
        if (key == "type")
        {
            auto pt = process_type_from_name(val);
            if (!pt)
                return pt.error();
            cfg.process_type = *pt;
            ++i;
        }
        else if (key == "machined")
        {
            auto addr = parse_address(val);
            if (!addr)
                return addr.error();
            cfg.machined_address = *addr;
            ++i;
        }
        else if (key == "log-level")
        {
            cfg.log_level = parse_log_level(val);
            ++i;
        }
        else if (key == "login-rate-limit-trusted-cidr")
        {
            cfg.login_rate_limit_trusted_cidrs.emplace_back(val);
            ++i;
        }
        else
        {
            // Table-driven: match key against cli_fields[]
            for (const auto& field : cli_fields)
            {
                if (key == field.key)
                {
                    auto err = parse_and_assign(cfg, field.ptr, key, val);
                    if (err)
                        return *err;
                    ++i;
                    break;
                }
            }
        }
        // "--config" is consumed by load(), not here
    }

    return cfg;
}

// ============================================================================
// ServerConfig::load  (JSON file + CLI override)
// ============================================================================

auto ServerConfig::load(int argc, char* argv[]) -> Result<ServerConfig>
{
    // Find --config <path> in argv
    std::filesystem::path config_path;
    for (int i = 1; i < argc - 1; ++i)
    {
        std::string_view key = argv[i];
        if (key == "--config")
        {
            config_path = argv[i + 1];
            break;
        }
    }

    ServerConfig cfg;

    if (!config_path.empty())
    {
        auto file_result = from_json_file(config_path);
        if (!file_result)
            return file_result.error();
        cfg = std::move(*file_result);
    }

    // Apply CLI overrides
    auto cli_result = from_args(argc, argv);
    if (!cli_result)
        return cli_result.error();

    auto& cli = *cli_result;

    const auto has_cli_key = [argc, argv](std::string_view wanted) -> bool
    {
        for (int i = 1; i < argc; ++i)
        {
            std::string_view key = argv[i];
            if (!key.starts_with("--"))
                continue;
            key.remove_prefix(2);
            if (key == wanted)
                return true;
        }
        return false;
    };

    ServerConfig defaults;

    // Special-case overrides
    if (has_cli_key("type"))
        cfg.process_type = cli.process_type;
    if (has_cli_key("machined"))
        cfg.machined_address = cli.machined_address;
    if (has_cli_key("log-level"))
        cfg.log_level = cli.log_level;
    if (has_cli_key("login-rate-limit-trusted-cidr"))
        cfg.login_rate_limit_trusted_cidrs = cli.login_rate_limit_trusted_cidrs;

    // Table-driven overrides
    for (const auto& field : cli_fields)
    {
        if (has_cli_key(field.key))
            std::visit([&](auto m) { cfg.*m = cli.*m; }, field.ptr);
    }

    // Derive process_name from type if not set
    if (cfg.process_name.empty())
        cfg.process_name = std::string(process_type_name(cfg.process_type));

    // Apply default machined address if still unset
    if (cfg.machined_address == Address::NONE || cfg.machined_address == defaults.machined_address)
    {
        auto default_addr = Address::resolve("127.0.0.1", 20018);
        if (default_addr)
            cfg.machined_address = *default_addr;
    }

    return cfg;
}

}  // namespace atlas
