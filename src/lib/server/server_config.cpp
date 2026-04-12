#include "server/server_config.hpp"

#include "serialization/data_section.hpp"

#include <algorithm>
#include <format>
#include <string>

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

        if (key == "type")
        {
            auto pt = process_type_from_name(val);
            if (!pt)
                return pt.error();
            cfg.process_type = *pt;
            ++i;
        }
        else if (key == "name")
        {
            cfg.process_name = std::string(val);
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
        else if (key == "internal-port")
        {
            try
            {
                cfg.internal_port = static_cast<uint16_t>(std::stoi(std::string(val)));
            }
            catch (...)
            {
                return Error{ErrorCode::InvalidArgument, "invalid --internal-port"};
            }
            ++i;
        }
        else if (key == "external-port")
        {
            try
            {
                cfg.external_port = static_cast<uint16_t>(std::stoi(std::string(val)));
            }
            catch (...)
            {
                return Error{ErrorCode::InvalidArgument, "invalid --external-port"};
            }
            ++i;
        }
        else if (key == "update-hertz")
        {
            try
            {
                cfg.update_hertz = std::stoi(std::string(val));
            }
            catch (...)
            {
                return Error{ErrorCode::InvalidArgument, "invalid --update-hertz"};
            }
            ++i;
        }
        else if (key == "log-level")
        {
            cfg.log_level = parse_log_level(val);
            ++i;
        }
        else if (key == "assembly")
        {
            cfg.script_assembly = std::string(val);
            ++i;
        }
        else if (key == "runtime-config")
        {
            cfg.runtime_config = std::string(val);
            ++i;
        }
        else if (key == "entitydef-path")
        {
            cfg.entitydef_path = std::string(val);
            ++i;
        }
        else if (key == "db-type")
        {
            cfg.db_type = std::string(val);
            ++i;
        }
        else if (key == "db-xml-dir")
        {
            cfg.db_xml_dir = std::string(val);
            ++i;
        }
        else if (key == "db-sqlite-path")
        {
            cfg.db_sqlite_path = std::string(val);
            ++i;
        }
        else if (key == "db-sqlite-wal")
        {
            cfg.db_sqlite_wal = parse_bool_string(val);
            ++i;
        }
        else if (key == "db-sqlite-busy-timeout-ms")
        {
            try
            {
                cfg.db_sqlite_busy_timeout_ms = std::stoi(std::string(val));
            }
            catch (...)
            {
                return Error{ErrorCode::InvalidArgument, "invalid --db-sqlite-busy-timeout-ms"};
            }
            ++i;
        }
        else if (key == "db-sqlite-foreign-keys")
        {
            cfg.db_sqlite_foreign_keys = parse_bool_string(val);
            ++i;
        }
        else if (key == "db-mysql-host")
        {
            cfg.db_mysql_host = std::string(val);
            ++i;
        }
        else if (key == "db-mysql-port")
        {
            try
            {
                cfg.db_mysql_port = static_cast<uint16_t>(std::stoi(std::string(val)));
            }
            catch (...)
            {
                return Error{ErrorCode::InvalidArgument, "invalid --db-mysql-port"};
            }
            ++i;
        }
        else if (key == "db-mysql-user")
        {
            cfg.db_mysql_user = std::string(val);
            ++i;
        }
        else if (key == "db-mysql-password")
        {
            cfg.db_mysql_password = std::string(val);
            ++i;
        }
        else if (key == "db-mysql-database")
        {
            cfg.db_mysql_database = std::string(val);
            ++i;
        }
        else if (key == "db-mysql-pool-size")
        {
            try
            {
                cfg.db_mysql_pool_size = std::stoi(std::string(val));
            }
            catch (...)
            {
                return Error{ErrorCode::InvalidArgument, "invalid --db-mysql-pool-size"};
            }
            ++i;
        }
        else if (key == "auto-create-accounts")
        {
            cfg.auto_create_accounts = parse_bool_string(val);
            ++i;
        }
        else if (key == "account-type-id")
        {
            try
            {
                cfg.account_type_id = static_cast<uint16_t>(std::stoi(std::string(val)));
            }
            catch (...)
            {
                return Error{ErrorCode::InvalidArgument, "invalid --account-type-id"};
            }
            ++i;
        }
        else if (key == "login-rate-limit-per-ip")
        {
            try
            {
                cfg.login_rate_limit_per_ip = std::stoi(std::string(val));
            }
            catch (...)
            {
                return Error{ErrorCode::InvalidArgument, "invalid --login-rate-limit-per-ip"};
            }
            ++i;
        }
        else if (key == "login-rate-limit-global")
        {
            try
            {
                cfg.login_rate_limit_global = std::stoi(std::string(val));
            }
            catch (...)
            {
                return Error{ErrorCode::InvalidArgument, "invalid --login-rate-limit-global"};
            }
            ++i;
        }
        else if (key == "login-rate-limit-window-sec")
        {
            try
            {
                cfg.login_rate_limit_window_sec = std::stoi(std::string(val));
            }
            catch (...)
            {
                return Error{ErrorCode::InvalidArgument, "invalid --login-rate-limit-window-sec"};
            }
            ++i;
        }
        else if (key == "login-rate-limit-trusted-cidr")
        {
            cfg.login_rate_limit_trusted_cidrs.emplace_back(val);
            ++i;
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

    if (has_cli_key("type"))
        cfg.process_type = cli.process_type;
    if (has_cli_key("name"))
        cfg.process_name = cli.process_name;
    if (has_cli_key("machined"))
        cfg.machined_address = cli.machined_address;
    if (has_cli_key("internal-port"))
        cfg.internal_port = cli.internal_port;
    if (has_cli_key("external-port"))
        cfg.external_port = cli.external_port;
    if (has_cli_key("update-hertz"))
        cfg.update_hertz = cli.update_hertz;
    if (has_cli_key("log-level"))
        cfg.log_level = cli.log_level;
    if (has_cli_key("assembly"))
        cfg.script_assembly = cli.script_assembly;
    if (has_cli_key("runtime-config"))
        cfg.runtime_config = cli.runtime_config;
    if (has_cli_key("entitydef-path"))
        cfg.entitydef_path = cli.entitydef_path;
    if (has_cli_key("db-type"))
        cfg.db_type = cli.db_type;
    if (has_cli_key("db-xml-dir"))
        cfg.db_xml_dir = cli.db_xml_dir;
    if (has_cli_key("db-sqlite-path"))
        cfg.db_sqlite_path = cli.db_sqlite_path;
    if (has_cli_key("db-sqlite-wal"))
        cfg.db_sqlite_wal = cli.db_sqlite_wal;
    if (has_cli_key("db-sqlite-busy-timeout-ms"))
        cfg.db_sqlite_busy_timeout_ms = cli.db_sqlite_busy_timeout_ms;
    if (has_cli_key("db-sqlite-foreign-keys"))
        cfg.db_sqlite_foreign_keys = cli.db_sqlite_foreign_keys;
    if (has_cli_key("db-mysql-host"))
        cfg.db_mysql_host = cli.db_mysql_host;
    if (has_cli_key("db-mysql-port"))
        cfg.db_mysql_port = cli.db_mysql_port;
    if (has_cli_key("db-mysql-user"))
        cfg.db_mysql_user = cli.db_mysql_user;
    if (has_cli_key("db-mysql-password"))
        cfg.db_mysql_password = cli.db_mysql_password;
    if (has_cli_key("db-mysql-database"))
        cfg.db_mysql_database = cli.db_mysql_database;
    if (has_cli_key("db-mysql-pool-size"))
        cfg.db_mysql_pool_size = cli.db_mysql_pool_size;
    if (has_cli_key("auto-create-accounts"))
        cfg.auto_create_accounts = cli.auto_create_accounts;
    if (has_cli_key("account-type-id"))
        cfg.account_type_id = cli.account_type_id;
    if (has_cli_key("login-rate-limit-per-ip"))
        cfg.login_rate_limit_per_ip = cli.login_rate_limit_per_ip;
    if (has_cli_key("login-rate-limit-global"))
        cfg.login_rate_limit_global = cli.login_rate_limit_global;
    if (has_cli_key("login-rate-limit-window-sec"))
        cfg.login_rate_limit_window_sec = cli.login_rate_limit_window_sec;
    if (has_cli_key("login-rate-limit-trusted-cidr"))
        cfg.login_rate_limit_trusted_cidrs = cli.login_rate_limit_trusted_cidrs;

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
