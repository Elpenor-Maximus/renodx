/*
 * SPDX-License-Identifier: MIT
 *
 * Runtime HDR coverage for the DKB add-on.
 *
 * Every pixel and compute shader the game creates is classified once (coverage.hpp). Shaders
 * that tone map, grade or encode are listed in ReShade.log with their RenoDX CRC32 and whether
 * the add-on replaces them, at creation and again at their first draw. The overlay shows the
 * same list. Unreplaced scene composites are kept in memory so the overlay can save them beside
 * the add-on, as input for dkb-pc-recomp/tools/build_renodx_gameplay.py. Created and first-draw
 * events are also appended to renodx-dkb-recomp-coverage/coverage.tsv across sessions.
 *
 * Each entry says how it is replaced: "embedded" (a build-time entry), "runtime" (the
 * structural rewrite, runtime_rewrite.hpp) or "none", and for "none" why: the structural
 * recogniser's reason code (E_GRADE, ...), E_PIPELINE when the rewritten pipeline failed to
 * build, or "off" when the runtime rewrite is disabled.
 */

#ifndef SRC_GAMES_DKB_RECOMP_COVERAGE_RUNTIME_HPP_
#define SRC_GAMES_DKB_RECOMP_COVERAGE_RUNTIME_HPP_

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include "../../mods/shader.hpp"
#include "../../utils/hash.hpp"
#include "../../utils/pipeline.hpp"
#include "./coverage.hpp"

namespace dkb::coverage::runtime {

struct Entry {
  Class cls = Class::OTHER;
  bool replaced = false;
  const char* by = "none";  // embedded, runtime or none
  std::string reason;       // why not replaced (runtime rewrite reason code)
  bool compute = false;
  size_t bytes = 0;
  std::atomic_bool drawn = false;
  std::vector<uint8_t> code;  // kept only for unreplaced composites
};

namespace internal {
inline std::shared_mutex mutex;
inline std::map<uint32_t, Entry> entries;            // relevant shaders by CRC32
inline std::unordered_set<uint32_t> seen;            // every CRC32 classified so far
inline std::unordered_map<uint64_t, uint32_t> watched;  // pipeline handle -> relevant CRC32
inline std::unordered_map<uint32_t, const char*> handled;  // CRC32s the add-on replaces -> embedded/runtime
inline std::unordered_map<uint32_t, std::string> reasons;  // CRC32s the runtime rewrite rejected -> reason
inline std::unordered_set<uint32_t> replacement_codes;  // CRC32s of the add-on's own shaders
inline std::atomic_uint32_t runtime_replaced = 0;
inline std::string runtime_status = "runtime rewrite not attached";
inline std::atomic_uint32_t composites_seen = 0, composites_unreplaced = 0, unreplaced_drawn = 0;
inline HMODULE module = nullptr;

inline std::filesystem::path DumpFolder();

// ReShade.log starts over at every launch: the same events are also appended to
// renodx-dkb-recomp-coverage/coverage.tsv, so coverage accumulates across play sessions.
inline std::mutex record_mutex;
inline constexpr const char* HEADER = "time\tevent\tcrc32\tclass\tstage\treplaced\tbytes\tby\treason";
inline void Record(const char* event, uint32_t crc, const Entry& entry) {
  std::lock_guard lock(record_mutex);
  static std::filesystem::path path;
  static bool header = false;
  if (path.empty()) {
    const auto folder = DumpFolder();
    if (folder.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    path = folder / L"coverage.tsv";
    header = !std::filesystem::exists(path, error);
    if (!header) {
      // Files from earlier builds have fewer columns: keep them aside and start a new one.
      std::ifstream existing(path);
      std::string first;
      std::getline(existing, first);
      existing.close();
      if (first != HEADER) {
        std::filesystem::rename(path, folder / L"coverage-v1.tsv", error);
        header = true;
      }
    }
  }
  std::ofstream file(path, std::ios::app);
  if (!file) return;
  if (header) {
    file << HEADER << "\n";
    header = false;
  }
  SYSTEMTIME t;
  GetLocalTime(&t);
  char line[256];
  std::snprintf(line, sizeof line, "%04u-%02u-%02uT%02u:%02u:%02u\t%s\t0x%08X\t%s\t%s\t%s\t%zu\t%s\t%s\n", t.wYear,
                t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, event, crc, Name(entry.cls),
                entry.compute ? "cs" : "fs", entry.replaced ? "yes" : "no", entry.bytes, entry.by,
                entry.reason.c_str());
  file << line;
}

inline std::string Status(const Entry& entry) {
  if (entry.replaced) return std::string("replaced, ") + entry.by;
  return entry.reason.empty() ? std::string("NOT REPLACED") : "NOT REPLACED, " + entry.reason;
}

inline void Log(reshade::log::level level, const char* fmt, uint32_t crc, const Entry& entry,
                const char* event = nullptr) {
  char text[320];
  std::snprintf(text, sizeof text, fmt, crc, Name(entry.cls), entry.compute ? "cs" : "fs", Status(entry).c_str(),
                entry.bytes);
  reshade::log::message(level, text);
  if (event == nullptr) event = std::strstr(fmt, "first draw") != nullptr ? "drawn" : "created";
  Record(event, crc, entry);
}

inline bool IsShaderSubobject(reshade::api::pipeline_subobject_type type, bool* compute) {
  *compute = type == reshade::api::pipeline_subobject_type::compute_shader;
  return *compute || type == reshade::api::pipeline_subobject_type::pixel_shader;
}

inline void OnInit(renodx::utils::pipeline::PipelineInfo* info, uint32_t count,
                   const reshade::api::pipeline_subobject* subobjects) {
  // Classify any shader code this pipeline carries, once per CRC32.
  for (uint32_t i = 0; i < count; ++i) {
    bool compute = false;
    if (!IsShaderSubobject(subobjects[i].type, &compute)) continue;
    const auto* desc = static_cast<const reshade::api::shader_desc*>(subobjects[i].data);
    if (desc == nullptr || desc->code == nullptr || desc->code_size == 0) continue;
    const auto* code = static_cast<const uint8_t*>(desc->code);
    const uint32_t crc = renodx::utils::hash::ComputeCRC32(code, desc->code_size);
    {
      std::shared_lock lock(mutex);
      if (seen.contains(crc) || replacement_codes.contains(crc)) continue;
    }
    const Class cls = Classify(code, desc->code_size);
    std::unique_lock lock(mutex);
    if (!seen.insert(crc).second || cls == Class::OTHER) continue;
    Entry& entry = entries[crc];
    entry.cls = cls;
    entry.compute = compute;
    entry.bytes = desc->code_size;
    if (const auto h = handled.find(crc); h != handled.end()) {
      entry.replaced = true;
      entry.by = h->second;
    } else if (const auto r = reasons.find(crc); r != reasons.end()) {
      entry.reason = r->second;
    }
    if (IsComposite(cls)) {
      ++composites_seen;
      if (!entry.replaced) {
        ++composites_unreplaced;
        entry.code.assign(code, code + desc->code_size);
      }
    }
    Log(IsComposite(cls) && !entry.replaced ? reshade::log::level::warning : reshade::log::level::info,
        "DKB coverage: created 0x%08X %s (%s, %s, %zu bytes)", crc, entry);
  }
  // Watch pipelines that use a relevant shader, including linked pipelines that carry only hashes.
  if (info == nullptr) return;
  for (const auto& detail : info->shader_details) {
    if (detail.stage != reshade::api::pipeline_stage::pixel_shader
        && detail.stage != reshade::api::pipeline_stage::compute_shader) {
      continue;
    }
    std::unique_lock lock(mutex);
    if (entries.contains(detail.shader_hash)) watched[info->pipeline.handle] = detail.shader_hash;
  }
}

inline void OnBind(reshade::api::command_list*, reshade::api::pipeline_stage, reshade::api::pipeline pipeline,
                   renodx::utils::pipeline::PipelineBindPoint) {
  std::shared_lock lock(mutex);
  const auto it = watched.find(pipeline.handle);
  if (it == watched.end()) return;
  const auto entry = entries.find(it->second);
  if (entry == entries.end() || entry->second.drawn.exchange(true)) return;
  if (IsComposite(entry->second.cls) && !entry->second.replaced) ++unreplaced_drawn;
  Log(IsComposite(entry->second.cls) && !entry->second.replaced ? reshade::log::level::warning
                                                                  : reshade::log::level::info,
      "DKB coverage: first draw of 0x%08X %s (%s, %s, %zu bytes)", it->second, entry->second);
}

inline void OnDestroy(const renodx::utils::pipeline::PipelineInfo& info) {
  std::unique_lock lock(mutex);
  watched.erase(info.pipeline.handle);
}

inline std::filesystem::path DumpFolder() {  // declared above Record

  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return {};
  path.resize(length);
  return std::filesystem::path(path).parent_path() / L"renodx-dkb-recomp-coverage";
}
}  // namespace internal

// Runtime rewrite outcomes (runtime_rewrite.hpp). The rewrite runs when the pipeline is
// created, before OnInit classifies it, so the outcome is normally known at "created".

// A shader code the add-on generated (a rewritten composite): not classified.
inline void AddReplacementCode(uint32_t crc) {
  std::unique_lock lock(internal::mutex);
  internal::replacement_codes.insert(crc);
}

inline void SetRuntimeStatus(std::string status) {
  std::unique_lock lock(internal::mutex);
  internal::runtime_status = std::move(status);
}

// The runtime rewrite replaces crc.
inline void MarkRuntimeReplaced(uint32_t crc) {
  using namespace internal;
  std::unique_lock lock(mutex);
  handled[crc] = "runtime";
  reasons.erase(crc);
  ++runtime_replaced;
}

// The runtime rewrite does not replace crc, for reason. If the shader was already listed as
// replaced (its rewritten pipeline failed to build), it is listed again as not replaced.
inline void MarkRuntimeRejected(uint32_t crc, const std::string& reason, std::span<const uint8_t> code = {}) {
  using namespace internal;
  std::unique_lock lock(mutex);
  reasons[crc] = reason;
  const bool was_replaced = handled.erase(crc) != 0;
  if (was_replaced) --runtime_replaced;
  const auto it = entries.find(crc);
  if (it == entries.end()) return;
  Entry& entry = it->second;
  if (!entry.replaced) return;
  entry.replaced = false;
  entry.by = "none";
  entry.reason = reason;
  if (IsComposite(entry.cls)) {
    ++composites_unreplaced;
    if (entry.drawn.load()) ++unreplaced_drawn;
    if (!code.empty()) entry.code.assign(code.begin(), code.end());
  }
  Log(reshade::log::level::warning, "DKB coverage: 0x%08X %s (%s, %s, %zu bytes) after a pipeline failure", crc, entry,
      "failed");
}

inline void Attach(HMODULE module, const renodx::mods::shader::CustomShaders& shaders) {
  internal::module = module;
  for (const auto& [crc, shader] : shaders) {
    internal::handled[crc] = "embedded";
    if (!shader.code.empty()) {
      internal::replacement_codes.insert(renodx::utils::hash::ComputeCRC32(shader.code.data(), shader.code.size()));
    }
  }
  renodx::utils::pipeline::RegisterOnInitCallback(&internal::OnInit);
  renodx::utils::pipeline::RegisterOnBindCallback(&internal::OnBind);
  renodx::utils::pipeline::RegisterOnDestroyCallback(&internal::OnDestroy);
}

inline void Detach() {
  renodx::utils::pipeline::UnregisterOnInitCallback(&internal::OnInit);
  renodx::utils::pipeline::UnregisterOnBindCallback(&internal::OnBind);
  renodx::utils::pipeline::UnregisterOnDestroyCallback(&internal::OnDestroy);
}

// Overlay summary. Returns false: nothing to save.
inline bool Draw() {
  using namespace internal;
  ImGui::Text("Scene composites: %u seen, %u not replaced, %u of those drawn", composites_seen.load(),
              composites_unreplaced.load(), unreplaced_drawn.load());
  std::shared_lock lock(mutex);
  ImGui::Text("Structural rewrite: %s, %u composite(s) replaced at runtime", runtime_status.c_str(),
              runtime_replaced.load());
  for (const auto& [crc, entry] : entries) {
    ImGui::BulletText("0x%08X %s (%s) %s%s", crc, Name(entry.cls), entry.compute ? "cs" : "fs", Status(entry).c_str(),
                      entry.drawn.load() ? ", drawn" : "");
  }
  return false;
}

// Writes each unreplaced composite as <class>_0xCRC32.spv beside the add-on. Returns false: no settings change.
inline bool SaveUnreplaced() {
  using namespace internal;
  const auto folder = DumpFolder();
  if (folder.empty()) return false;
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  std::shared_lock lock(mutex);
  uint32_t written = 0;
  for (const auto& [crc, entry] : entries) {
    if (entry.code.empty()) continue;
    char name[64];
    std::snprintf(name, sizeof name, "%s_0x%08X.spv", Name(entry.cls), crc);
    std::ofstream file(folder / name, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(entry.code.data()), static_cast<std::streamsize>(entry.code.size()));
    written += static_cast<uint32_t>(static_cast<bool>(file));
  }
  char text[128];
  std::snprintf(text, sizeof text, "DKB coverage: saved %u unreplaced composite(s) to renodx-dkb-recomp-coverage",
                written);
  reshade::log::message(reshade::log::level::info, text);
  return false;
}

}  // namespace dkb::coverage::runtime

#endif  // SRC_GAMES_DKB_RECOMP_COVERAGE_RUNTIME_HPP_
