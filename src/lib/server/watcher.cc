#include "server/watcher.h"

#include <cassert>

namespace atlas {

// ============================================================================
// WatcherRegistry — private helpers
// ============================================================================

void WatcherRegistry::insert(std::string_view path, std::unique_ptr<WatcherEntry> entry) {
  Node* node = FindOrCreateNode(path);
  assert(node != nullptr);
  if (!node->entry) ++count_;
  node->entry = std::move(entry);
}

const WatcherRegistry::Node* WatcherRegistry::FindNode(std::string_view path) const {
  const Node* cur = &root_;
  while (!path.empty()) {
    auto sep = path.find('/');
    auto key = (sep == std::string_view::npos) ? path : path.substr(0, sep);
    auto it = cur->children.find(key);
    if (it == cur->children.end()) return nullptr;
    cur = &it->second;
    path = (sep == std::string_view::npos) ? std::string_view{} : path.substr(sep + 1);
  }
  return cur;
}

WatcherRegistry::Node* WatcherRegistry::FindOrCreateNode(std::string_view path) {
  Node* cur = &root_;
  while (!path.empty()) {
    auto sep = path.find('/');
    auto key = (sep == std::string_view::npos) ? path : path.substr(0, sep);
    cur = &cur->children[std::string(key)];
    path = (sep == std::string_view::npos) ? std::string_view{} : path.substr(sep + 1);
  }
  return cur;
}

void WatcherRegistry::CollectPaths(const Node& node, const std::string& prefix,
                                   std::vector<std::string>& out) const {
  if (node.entry) out.push_back(prefix);
  for (const auto& [key, child] : node.children) {
    std::string child_prefix = prefix.empty() ? key : (prefix + '/' + key);
    CollectPaths(child, child_prefix, out);
  }
}

void WatcherRegistry::CollectSnapshot(const Node& node, const std::string& prefix,
                                      std::vector<std::pair<std::string, std::string>>& out) const {
  if (node.entry) out.emplace_back(prefix, node.entry->GetAsString());
  for (const auto& [key, child] : node.children) {
    std::string child_prefix = prefix.empty() ? key : (prefix + '/' + key);
    CollectSnapshot(child, child_prefix, out);
  }
}

// ============================================================================
// WatcherRegistry — public interface
// ============================================================================

auto WatcherRegistry::Get(std::string_view path) const -> std::optional<std::string> {
  const Node* node = FindNode(path);
  if (!node || !node->entry) return std::nullopt;
  return node->entry->GetAsString();
}

auto WatcherRegistry::Set(std::string_view path, std::string_view value) -> bool {
  const Node* node = FindNode(path);
  if (!node || !node->entry) return false;
  return node->entry->SetFromString(value);
}

auto WatcherRegistry::List(std::string_view prefix) const -> std::vector<std::string> {
  std::vector<std::string> result;

  if (prefix.empty()) {
    CollectPaths(root_, "", result);
  } else {
    const Node* node = FindNode(prefix);
    if (node) CollectPaths(*node, std::string(prefix), result);
  }

  return result;
}

auto WatcherRegistry::Snapshot() const -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> result;
  CollectSnapshot(root_, "", result);
  return result;
}

}  // namespace atlas
