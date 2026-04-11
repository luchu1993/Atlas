#include "server/watcher.hpp"

#include <cassert>

namespace atlas
{

// ============================================================================
// WatcherRegistry — private helpers
// ============================================================================

void WatcherRegistry::insert(std::string_view path, std::unique_ptr<WatcherEntry> entry)
{
    Node* node = find_or_create_node(path);
    assert(node != nullptr);
    if (!node->entry)
        ++count_;
    node->entry = std::move(entry);
}

const WatcherRegistry::Node* WatcherRegistry::find_node(std::string_view path) const
{
    const Node* cur = &root_;
    while (!path.empty())
    {
        auto sep = path.find('/');
        auto key = (sep == std::string_view::npos) ? path : path.substr(0, sep);
        auto it = cur->children.find(key);
        if (it == cur->children.end())
            return nullptr;
        cur = &it->second;
        path = (sep == std::string_view::npos) ? std::string_view{} : path.substr(sep + 1);
    }
    return cur;
}

WatcherRegistry::Node* WatcherRegistry::find_or_create_node(std::string_view path)
{
    Node* cur = &root_;
    while (!path.empty())
    {
        auto sep = path.find('/');
        auto key = (sep == std::string_view::npos) ? path : path.substr(0, sep);
        cur = &cur->children[std::string(key)];
        path = (sep == std::string_view::npos) ? std::string_view{} : path.substr(sep + 1);
    }
    return cur;
}

void WatcherRegistry::collect_paths(const Node& node, const std::string& prefix,
                                    std::vector<std::string>& out) const
{
    if (node.entry)
        out.push_back(prefix);
    for (const auto& [key, child] : node.children)
    {
        std::string child_prefix = prefix.empty() ? key : (prefix + '/' + key);
        collect_paths(child, child_prefix, out);
    }
}

void WatcherRegistry::collect_snapshot(const Node& node, const std::string& prefix,
                                       std::vector<std::pair<std::string, std::string>>& out) const
{
    if (node.entry)
        out.emplace_back(prefix, node.entry->get_as_string());
    for (const auto& [key, child] : node.children)
    {
        std::string child_prefix = prefix.empty() ? key : (prefix + '/' + key);
        collect_snapshot(child, child_prefix, out);
    }
}

// ============================================================================
// WatcherRegistry — public interface
// ============================================================================

auto WatcherRegistry::get(std::string_view path) const -> std::optional<std::string>
{
    const Node* node = find_node(path);
    if (!node || !node->entry)
        return std::nullopt;
    return node->entry->get_as_string();
}

auto WatcherRegistry::set(std::string_view path, std::string_view value) -> bool
{
    const Node* node = find_node(path);
    if (!node || !node->entry)
        return false;
    return node->entry->set_from_string(value);
}

auto WatcherRegistry::list(std::string_view prefix) const -> std::vector<std::string>
{
    std::vector<std::string> result;

    if (prefix.empty())
    {
        collect_paths(root_, "", result);
    }
    else
    {
        const Node* node = find_node(prefix);
        if (node)
            collect_paths(*node, std::string(prefix), result);
    }

    return result;
}

auto WatcherRegistry::snapshot() const -> std::vector<std::pair<std::string, std::string>>
{
    std::vector<std::pair<std::string, std::string>> result;
    collect_snapshot(root_, "", result);
    return result;
}

}  // namespace atlas
