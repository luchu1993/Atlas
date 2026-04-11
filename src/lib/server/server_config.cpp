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
        cfg.db_mysql_host = db->read_string("mysql_host", cfg.db_mysql_host);
        cfg.db_mysql_port = static_cast<uint16_t>(db->read_uint("mysql_port", cfg.db_mysql_port));
        cfg.db_mysql_user = db->read_string("mysql_user", cfg.db_mysql_user);
        cfg.db_mysql_password = db->read_string("mysql_password", cfg.db_mysql_password);
        cfg.db_mysql_database = db->read_string("mysql_database", cfg.db_mysql_database);
        cfg.db_mysql_pool_size = db->read_int("mysql_pool_size", cfg.db_mysql_pool_size);
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

    // Only override fields that CLI explicitly set (non-default)
    // We detect "was set" by comparing against a default-constructed ServerConfig.
    ServerConfig defaults;

    if (cli.process_type != defaults.process_type)
        cfg.process_type = cli.process_type;
    if (!cli.process_name.empty())
        cfg.process_name = cli.process_name;
    if (cli.machined_address != defaults.machined_address)
        cfg.machined_address = cli.machined_address;
    if (cli.internal_port != defaults.internal_port)
        cfg.internal_port = cli.internal_port;
    if (cli.external_port != defaults.external_port)
        cfg.external_port = cli.external_port;
    if (cli.update_hertz != defaults.update_hertz)
        cfg.update_hertz = cli.update_hertz;
    if (cli.log_level != defaults.log_level)
        cfg.log_level = cli.log_level;
    if (!cli.script_assembly.empty())
        cfg.script_assembly = cli.script_assembly;
    if (!cli.runtime_config.empty())
        cfg.runtime_config = cli.runtime_config;

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
