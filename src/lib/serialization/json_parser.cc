#include "serialization/json_parser.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996 5054)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <cassert>
#include <charconv>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "platform/filesystem.h"

namespace atlas::json {

namespace {

void PopulateFromValue(DataSection* section, const rapidjson::Value& val);

auto ValueToString(const rapidjson::Value& val) -> std::string {
  if (val.IsString()) {
    return std::string(val.GetString(), val.GetStringLength());
  }
  if (val.IsInt()) {
    return std::to_string(val.GetInt());
  }
  if (val.IsUint()) {
    return std::to_string(val.GetUint());
  }
  if (val.IsInt64()) {
    return std::to_string(val.GetInt64());
  }
  if (val.IsUint64()) {
    return std::to_string(val.GetUint64());
  }
  if (val.IsDouble()) {
    return std::to_string(val.GetDouble());
  }
  if (val.IsBool()) {
    return val.GetBool() ? "true" : "false";
  }
  if (val.IsNull()) {
    return "";
  }
  return "";
}

void ConvertValue(DataSection* parent, std::string_view key, const rapidjson::Value& val) {
  if (val.IsObject()) {
    auto* child = parent->AddChild(std::string(key));
    PopulateFromValue(child, val);
  } else if (val.IsArray()) {
    auto* child = parent->AddChild(std::string(key));
    for (rapidjson::SizeType i = 0; i < val.Size(); ++i) {
      char idx_buf[32];
      auto [ptr, ec] =
          std::to_chars(idx_buf, idx_buf + sizeof(idx_buf), static_cast<unsigned long long>(i));
      assert(ec == std::errc{});
      ConvertValue(child, std::string_view(idx_buf, static_cast<std::size_t>(ptr - idx_buf)),
                   val[i]);
    }
  } else {
    parent->AddChild(std::string(key), ValueToString(val));
  }
}

void PopulateFromValue(DataSection* section, const rapidjson::Value& val) {
  if (val.IsObject()) {
    for (auto it = val.MemberBegin(); it != val.MemberEnd(); ++it) {
      ConvertValue(section, std::string_view(it->name.GetString(), it->name.GetStringLength()),
                   it->value);
    }
  } else if (val.IsArray()) {
    for (rapidjson::SizeType i = 0; i < val.Size(); ++i) {
      char idx_buf[32];
      auto [ptr, ec] =
          std::to_chars(idx_buf, idx_buf + sizeof(idx_buf), static_cast<unsigned long long>(i));
      assert(ec == std::errc{});
      ConvertValue(section, std::string_view(idx_buf, static_cast<std::size_t>(ptr - idx_buf)),
                   val[i]);
    }
  } else {
    section->SetValue(ValueToString(val));
  }
}

}  // anonymous namespace

auto ParseFile(const std::filesystem::path& path) -> Result<std::shared_ptr<DataSectionTree>> {
  auto text = fs::ReadTextFile(path);
  if (!text) {
    return text.Error();
  }
  return ParseString(*text);
}

auto ParseString(std::string_view json) -> Result<std::shared_ptr<DataSectionTree>> {
  rapidjson::Document doc;
  doc.Parse(json.data(), json.size());

  if (doc.HasParseError()) {
    std::string msg = "JSON parse error: ";
    msg += rapidjson::GetParseError_En(doc.GetParseError());
    msg += " at offset ";
    msg += std::to_string(doc.GetErrorOffset());
    return Error(ErrorCode::kInvalidArgument, std::move(msg));
  }

  auto tree = std::make_shared<DataSectionTree>("root");
  PopulateFromValue(tree->Root(), doc);
  return tree;
}

}  // namespace atlas::json
