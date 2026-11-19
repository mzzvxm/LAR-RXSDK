#include "output_stamp.h"

#include <algorithm>
#include <fstream>

#include <format>
#include <nlohmann/json.hpp>

#include <rex/hash.h>

#include <rex/logging/log_category.h>
#include "codegen_categories.h"
#include "file_io.h"

namespace rex::codegen {

constexpr int kStampVersion = 1;

std::optional<OutputStamp> OutputStamp::Load(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::nullopt;

  nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    REXCG_WARN("Ignoring malformed codegen stamp: {}", path.string());
    return std::nullopt;
  }

  auto version = doc.find("version");
  if (version == doc.end() || !version->is_number_integer() || version->get<int>() != kStampVersion)
    return std::nullopt;

  OutputStamp stamp;
  if (auto it = doc.find("fingerprint"); it != doc.end() && it->is_string())
    stamp.fingerprint = it->get<std::string>();
  if (stamp.fingerprint.empty())
    return std::nullopt;

  if (auto it = doc.find("outputs"); it != doc.end() && it->is_array()) {
    for (const auto& entry : *it) {
      if (entry.is_string())
        stamp.outputs.push_back(entry.get<std::string>());
    }
  }
  return stamp;
}

std::string OutputStamp::Serialize() const {
  nlohmann::json doc;
  doc["version"] = kStampVersion;
  doc["fingerprint"] = fingerprint;
  doc["outputs"] = outputs;
  return doc.dump(2) + "\n";
}

std::string ComputeInputFingerprint(std::span<const std::filesystem::path> input_files,
                                    std::string_view sdk_version,
                                    std::span<const std::string> flag_values) {
  std::string accumulator;
  accumulator += "sdk=";
  accumulator += sdk_version;
  accumulator += '\n';

  for (const auto& flag : flag_values) {
    accumulator += "flag=";
    accumulator += flag;
    accumulator += '\n';
  }

  // The callers sort the inputs by path, so their order depends on how a path is spelled: the build
  // step passes the manifest as an absolute path, `rexglue codegen larecomp_manifest.toml` as a
  // relative one, and the same files sort differently. A fingerprint that followed that order
  // made the build step regenerate everything the first time it ran after a manual run. The
  // entries name a file by its base name and content only, so they are sorted here and the order
  // of `input_files` does not matter.
  std::vector<std::string> entries;
  entries.reserve(input_files.size());
  for (const auto& path : input_files) {
    // Hash content, not mtime, so the stamp survives a checkout, a copy or a touch.
    std::error_code ec;
    std::string digest = "<missing>";
    if (std::filesystem::exists(path, ec)) {
      digest = rex::HashFile(path);
      if (digest.empty()) {
        // A constant would report a locked input as unchanged and skip the module forever.
        REXCG_WARN("Could not read {} for fingerprinting", path.string());
        digest = std::format("<unreadable:{}:{}>", std::filesystem::file_size(path, ec),
                             std::filesystem::last_write_time(path, ec).time_since_epoch().count());
      }
    }
    entries.push_back(std::format("file={} {}\n", path.filename().string(), digest));
  }
  std::sort(entries.begin(), entries.end());
  for (const auto& entry : entries) {
    accumulator += entry;
  }

  return rex::HashBytes(accumulator);
}

namespace {

std::string EscapeDepfilePath(const std::filesystem::path& path) {
  std::string source = std::filesystem::absolute(path).generic_string();
  std::string escaped;
  escaped.reserve(source.size() + 8);
  for (char c : source) {
    if (c == ' ' || c == '#')
      escaped += '\\';
    if (c == '$')
      escaped += '$';
    escaped += c;
  }
  return escaped;
}

}  // namespace

bool WriteDepfile(const std::filesystem::path& path, const std::filesystem::path& target,
                  std::span<const std::filesystem::path> inputs) {
  std::string out = EscapeDepfilePath(target);
  out += ':';
  for (const auto& input : inputs) {
    out += " \\\n  ";
    out += EscapeDepfilePath(input);
  }
  out += '\n';

  return rex::platform::WriteHostFile(path, out).has_value();
}

bool AreOutputsUpToDate(const OutputStamp& stamp, std::string_view fingerprint,
                        const std::filesystem::path& output_dir) {
  if (stamp.fingerprint != fingerprint)
    return false;
  if (stamp.outputs.empty())
    return false;

  for (const auto& name : stamp.outputs) {
    std::error_code ec;
    if (!std::filesystem::exists(output_dir / name, ec))
      return false;
  }
  return true;
}

}  // namespace rex::codegen
