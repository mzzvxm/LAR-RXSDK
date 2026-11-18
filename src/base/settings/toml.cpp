#include <fstream>
#include <format>
#include <iterator>
#include <string>

#include <toml++/toml.hpp>

#include "base/settings/settings_internal.h"

namespace rex {

namespace {

toml::table& TableFor(toml::table& root, std::string_view dotted_scope) {
  toml::table* table = &root;
  size_t start = 0;
  while (start <= dotted_scope.size()) {
    size_t dot = dotted_scope.find('.', start);
    std::string part(dotted_scope.substr(start, dot == std::string_view::npos ? dot : dot - start));
    auto* child = table->get_as<toml::table>(part);
    if (!child) {
      table->insert_or_assign(part, toml::table());
      child = table->get_as<toml::table>(part);
    }
    table = child;
    if (dot == std::string_view::npos)
      break;
    start = dot + 1;
  }
  return *table;
}

/**
 * Collects the name and text of every scalar key under table. An array is an error that stops the
 * walk, or, when skipped is given, a line in skipped and the walk goes on.
 */
Result<void> CollectLeaves(const toml::table& table, const std::string& prefix,
                           const std::filesystem::path& file,
                           std::vector<std::pair<std::string, std::string>>& out,
                           std::vector<std::string>* skipped) {
  for (auto&& [key, node] : table) {
    std::string name =
        prefix.empty() ? std::string(key.str()) : prefix + "." + std::string(key.str());
    if (auto* child = node.as_table()) {
      REX_TRY_VOID(CollectLeaves(*child, name, file, out, skipped));
    } else if (node.is_array()) {
      if (skipped) {
        skipped->push_back(
            std::format("{}: key '{}' is an array and is skipped", file.string(), name));
        continue;
      }
      return Err(ErrorCategory::kConfig,
                 std::format("{}: key '{}' is an array", file.string(), name));
    } else if (auto* b = node.as_boolean()) {
      out.emplace_back(name, b->get() ? "true" : "false");
    } else if (auto* i = node.as_integer()) {
      out.emplace_back(name, std::to_string(i->get()));
    } else if (auto* f = node.as_floating_point()) {
      out.emplace_back(name, detail::RenderSetting(f->get()));
    } else if (auto* s = node.as_string()) {
      out.emplace_back(name, s->get());
    }
  }
  return Ok();
}

}  // namespace

Result<void> SettingsRegistry::Save(const std::filesystem::path& file) const {
  toml::table root;
  {
    std::lock_guard lock(state_->mutex);
    for (auto& [name, setting] : state_->settings) {
      if (setting->GetConstraints().no_save || setting->GetText() == setting->GetDefaultText()) {
        continue;
      }
      const size_t dot = name.rfind('.');
      toml::table& table = TableFor(root, std::string_view(name).substr(0, dot));
      const std::string key = name.substr(dot + 1);
      const std::string text = setting->GetText();
      switch (setting->GetType()) {
        case SettingType::kBool:
          table.insert_or_assign(key, text == "true");
          break;
        case SettingType::kI32:
        case SettingType::kI64:
          table.insert_or_assign(key, static_cast<i64>(std::stoll(text)));
          break;
        case SettingType::kU32:
        case SettingType::kU64:
          table.insert_or_assign(key, static_cast<i64>(std::stoull(text)));
          break;
        case SettingType::kF64:
          table.insert_or_assign(key, std::stod(text));
          break;
        default:
          table.insert_or_assign(key, text);
          break;
      }
    }
  }
  std::error_code ec;
  if (file.has_parent_path())
    std::filesystem::create_directories(file.parent_path(), ec);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Err(ErrorCategory::kIO, std::format("cannot write {}", file.string()));
  }
  out << root << "\n";
  out.flush();
  if (!out) {
    return Err(ErrorCategory::kIO, std::format("cannot write {}", file.string()));
  }
  return Ok();
}

namespace {

/**
 * Reads file and applies its keys at layer. A refused key stops the load with an error, or, when
 * skipped is given, is noted in skipped and passed over.
 */
Result<void> LoadFile(SettingsRegistry& registry, const std::filesystem::path& file,
                      SettingLayer layer, std::vector<std::string>* skipped) {
  // Read through the path object rather than handing the file name to toml::parse_file: that takes
  // the name as UTF-8, while path::string() yields the ANSI code page on Windows, so a non-ASCII
  // character anywhere in the path (a user folder like "Area de Trabalho") makes the open fail.
  // std::ifstream(path) opens the native wide path, which is also what Save writes through.
  std::string content;
  {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      return Err(ErrorCategory::kIO, std::format("cannot read {}", file.string()));
    }
    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
      return Err(ErrorCategory::kIO, std::format("cannot read {}", file.string()));
    }
  }
  toml::table parsed;
  try {
    parsed = toml::parse(content, file.string());
  } catch (const toml::parse_error& error) {
    return Err(ErrorCategory::kConfig,
               std::format("{}: {}", file.string(), std::string(error.description())));
  }
  std::vector<std::pair<std::string, std::string>> leaves;
  REX_TRY_VOID(CollectLeaves(parsed, {}, file, leaves, skipped));
  for (auto& [name, text] : leaves) {
    if (auto r = registry.SetText(layer, name, text); !r) {
      if (skipped) {
        skipped->push_back(std::format("{}: key '{}' is skipped, {}", file.string(), name,
                                       r.error().GetDescription()));
        continue;
      }
      return Err(ErrorCategory::kConfig,
                 std::format("{}: {}", file.string(), r.error().GetDescription()));
    }
  }
  return Ok();
}

}  // namespace

Result<void> SettingsRegistry::Load(const std::filesystem::path& file, SettingLayer layer) {
  return LoadFile(*this, file, layer, nullptr);
}

Result<void> SettingsRegistry::LoadSkippingRefused(const std::filesystem::path& file,
                                                   SettingLayer layer,
                                                   std::vector<std::string>& skipped) {
  return LoadFile(*this, file, layer, &skipped);
}

}  // namespace rex
