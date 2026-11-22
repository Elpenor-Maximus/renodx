/*
 * SPDX-License-Identifier: MIT
 *
 * Runtime structural rewrite of DKB's native HDR composites.
 *
 * When the game creates a pipeline whose pixel shader is a scene composite (coverage.hpp)
 * without a build-time entry, structural.hpp recognises it, rewrites it and splices in the
 * two hooks compiled with the add-on (dkb_tonemap_hook.frag.slang and
 * dkb_output_hook.frag.slang). The result is registered as a RenoDX runtime replacement for
 * that CRC32, with the same command action as the embedded shaders, so it receives the
 * add-on's settings as push constants and is built at its first bind like them.
 *
 * Fail-safe at each step; the game's own shader stays in place when:
 *   - the hooks cannot be loaded (logged once; the rewrite is off),
 *   - recognition, the splice or the self-check rejects the shader (its reason code is
 *     logged and shown in the coverage list),
 *   - the replacement pipeline does not build: after a few binds without a replacement the
 *     runtime replacement is removed again (E_PIPELINE), so RenoDX stops retrying it.
 * CRC32s with a build-time entry are never rewritten; the build-time entry wins.
 */

#ifndef SRC_GAMES_DKB_RECOMP_RUNTIME_REWRITE_HPP_
#define SRC_GAMES_DKB_RECOMP_RUNTIME_REWRITE_HPP_

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <include/reshade.hpp>

#include "../../mods/shader.hpp"
#include "../../utils/command_action.hpp"
#include "../../utils/hash.hpp"
#include "../../utils/pipeline.hpp"
#include "../../utils/shader.hpp"
#include "./coverage.hpp"
#include "./coverage_runtime.hpp"
#include "./structural.hpp"

namespace dkb::runtime_rewrite {

namespace internal {

// Binds of a pipeline without a built replacement before the replacement is given up. RenoDX
// builds it after the bind callbacks of the first bind, so the second bind normally sees it.
constexpr uint32_t MAX_UNBUILT_BINDS = 4;

struct Shader {
  bool ok = false;
  std::string reason;               // reason code when not ok
  std::vector<uint8_t> code;        // rewritten module when ok
  std::vector<uint8_t> original;    // when ok: kept for coverage if the pipeline later fails
  uint32_t code_crc = 0;
  std::string family;
  std::unique_ptr<renodx::mods::shader::CustomShader> custom;  // stable address for command_action
};

struct Watch {
  uint32_t crc = 0;
  reshade::api::device* device = nullptr;
  uint32_t unbuilt_binds = 0;
};

inline std::mutex mutex;
inline bool enabled = false;
inline std::optional<dkb::structural::Hook> tonemap, output;
inline std::unordered_set<uint32_t> embedded;           // CRC32s with a build-time entry
inline std::unordered_map<uint32_t, Shader> shaders;    // composites tried, by original CRC32
inline std::unordered_set<uint32_t> not_composites;     // pixel shaders that are not composites
inline std::unordered_set<uint32_t> generated;          // CRC32s of rewritten modules
inline std::set<std::pair<reshade::api::device*, uint32_t>> registered;  // runtime replacements added
inline std::unordered_map<uint64_t, Watch> watched;     // pipeline handle -> runtime-replaced shader
inline std::vector<std::pair<reshade::api::device*, uint32_t>> pending_removal;

inline void Message(reshade::log::level level, const char* fmt, uint32_t crc, const std::string& text) {
  char line[384];
  std::snprintf(line, sizeof line, fmt, crc, text.c_str());
  reshade::log::message(level, line);
}

// Recognise, rewrite and splice one composite. Never throws.
inline Shader Build(std::span<const uint8_t> code) {
  Shader s;
  try {
    auto result = dkb::structural::Transform(code, &*tonemap, &*output);
    s.code.resize(result.words.size() * sizeof(uint32_t));
    std::memcpy(s.code.data(), result.words.data(), s.code.size());
    s.code_crc = renodx::utils::hash::ComputeCRC32(s.code.data(), s.code.size());
    s.family = result.recognition.family + "/" + result.recognition.post.kind;
    s.ok = true;
  } catch (const dkb::structural::Reject& reject) {
    s.reason = reject.code;
    if (!reject.detail.empty()) s.family = reject.detail;
  } catch (const std::exception& error) {
    s.reason = "E_INTERNAL";
    s.family = error.what();
  } catch (...) {
    s.reason = "E_INTERNAL";
  }
  if (s.ok) s.original.assign(code.begin(), code.end());
  return s;
}

// Locks: `mutex` guards this file's tables and is also taken by OnInitPipeline, which RenoDX
// may call while it holds its pipeline table, so nothing that takes RenoDX's pipeline table
// (AddRuntimeReplacement, RemoveRuntimeReplacements, GetPipelineInfo) runs under `mutex`.
// `add_mutex` serialises AddRuntimeReplacement so that a second pipeline with the same shader,
// created on another thread, is not bound before the replacement exists.
inline std::mutex add_mutex;

inline bool OnCreatePipeline(reshade::api::device* device, reshade::api::pipeline_layout,
                             uint32_t subobject_count, const reshade::api::pipeline_subobject* subobjects) {
  if (!enabled) return false;
  for (uint32_t i = 0; i < subobject_count; ++i) {
    if (subobjects[i].type != reshade::api::pipeline_subobject_type::pixel_shader) continue;
    const auto* desc = static_cast<const reshade::api::shader_desc*>(subobjects[i].data);
    if (desc == nullptr || desc->code == nullptr || desc->code_size == 0) continue;
    const std::span<const uint8_t> code(static_cast<const uint8_t*>(desc->code), desc->code_size);
    const uint32_t crc = renodx::utils::hash::ComputeCRC32(code.data(), code.size());

    bool known = false;
    {
      std::lock_guard lock(mutex);
      if (embedded.contains(crc) || generated.contains(crc) || not_composites.contains(crc)) continue;
      known = shaders.contains(crc);
    }
    if (!known) {
      if (!dkb::coverage::IsComposite(dkb::coverage::Classify(code.data(), code.size()))) {
        std::lock_guard lock(mutex);
        not_composites.insert(crc);
        continue;
      }
      Shader built = Build(code);  // outside the lock: other pipelines keep compiling
      std::lock_guard lock(mutex);
      auto [it, fresh] = shaders.try_emplace(crc, std::move(built));
      if (fresh) {
        Shader& s = it->second;
        if (s.ok) {
          generated.insert(s.code_crc);
          s.custom = std::make_unique<renodx::mods::shader::CustomShader>();
          s.custom->crc32 = crc;
          s.custom->code = s.code;
          // Registered before this pipeline's init event, which prepares its command actions:
          // the settings are pushed before each draw of this shader, as for embedded shaders.
          renodx::utils::command_action::Register(
              renodx::mods::shader::OnCommandAction,
              {
                  .shader_hash = crc,
                  .command_types = renodx::utils::command_action::COMMAND_TYPE_DIRECT_DRAW
                                   | renodx::utils::command_action::COMMAND_TYPE_DISPATCH
                                   | renodx::utils::command_action::COMMAND_TYPE_DISPATCH_MESH
                                   | renodx::utils::command_action::COMMAND_TYPE_INDIRECT,
              },
              s.custom.get());
          dkb::coverage::runtime::AddReplacementCode(s.code_crc);
          dkb::coverage::runtime::MarkRuntimeReplaced(crc);
          Message(reshade::log::level::info, "DKB runtime: rewrote composite 0x%08X (%s)", crc,
                  s.family + ", " + std::to_string(s.code.size()) + " bytes");
        } else {
          dkb::coverage::runtime::MarkRuntimeRejected(crc, s.reason);
          Message(reshade::log::level::warning, "DKB runtime: kept the game's composite 0x%08X: %s", crc,
                  s.reason + (s.family.empty() ? "" : " (" + s.family + ")"));
        }
      }
    }

    std::lock_guard add_lock(add_mutex);
    std::span<const uint8_t> replacement;
    {
      std::lock_guard lock(mutex);
      const Shader& s = shaders.at(crc);
      if (!s.ok || registered.contains({device, crc})) continue;
      registered.insert({device, crc});
      replacement = s.code;  // never resized once stored
    }
    renodx::utils::shader::AddRuntimeReplacement(device, crc, replacement);
  }
  return false;  // the game's create call is unchanged; RenoDX swaps the shader at bind
}

inline void OnInitPipeline(renodx::utils::pipeline::PipelineInfo* info, uint32_t, const reshade::api::pipeline_subobject*) {
  if (!enabled || info == nullptr) return;
  std::lock_guard lock(mutex);
  for (const auto& detail : info->shader_details) {
    if (detail.stage != reshade::api::pipeline_stage::pixel_shader) continue;
    if (!registered.contains({info->device, detail.shader_hash})) continue;
    watched[info->pipeline.handle] = {detail.shader_hash, info->device, 0};
  }
}

inline void OnDestroyPipeline(const renodx::utils::pipeline::PipelineInfo& info) {
  std::lock_guard lock(mutex);
  watched.erase(info.pipeline.handle);
}

// Checks, at each bind of a watched pipeline, whether RenoDX has built its replacement.
inline void OnBindPipeline(reshade::api::command_list*, reshade::api::pipeline_stage, reshade::api::pipeline pipeline,
                           renodx::utils::pipeline::PipelineBindPoint) {
  {
    std::lock_guard lock(mutex);
    if (watched.empty() || !watched.contains(pipeline.handle)) return;
  }
  bool initialized = false, built = false;
  renodx::utils::pipeline::GetPipelineInfo(pipeline, [&](const auto& info) {
    initialized = info.details.initialized_replacement;
    built = info.details.replacement_pipeline.handle != 0u;
  });
  std::lock_guard lock(mutex);
  auto it = watched.find(pipeline.handle);
  if (it == watched.end()) return;
  if (initialized) {
    if (!built) {
      Message(reshade::log::level::warning, "DKB runtime: 0x%08X has no replacement pipeline%s", it->second.crc,
              " (no cached pipeline state); the game's shader is used");
    }
    watched.erase(it);
    return;
  }
  if (++it->second.unbuilt_binds < MAX_UNBUILT_BINDS) return;
  pending_removal.emplace_back(it->second.device, it->second.crc);
  watched.erase(it);
}

// Gives up replacements whose pipeline did not build, outside any bind.
inline void OnPresent(reshade::api::command_queue*, reshade::api::swapchain*, const reshade::api::rect*,
                      const reshade::api::rect*, uint32_t, const reshade::api::rect*) {
  std::vector<std::pair<reshade::api::device*, uint32_t>> removals;
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> failed;
  {
    std::lock_guard lock(mutex);
    if (pending_removal.empty()) return;
    removals.swap(pending_removal);
    for (const auto& [device, crc] : removals) {
      auto it = shaders.find(crc);
      if (it == shaders.end() || !it->second.ok) continue;
      it->second.ok = false;
      it->second.reason = "E_PIPELINE";
      failed.emplace_back(crc, it->second.original);
      for (auto w = watched.begin(); w != watched.end();) {
        w = w->second.crc == crc ? watched.erase(w) : std::next(w);
      }
    }
    for (auto r = registered.begin(); r != registered.end();) {
      const bool drop = std::ranges::any_of(failed, [&](const auto& f) { return f.first == r->second; });
      r = drop ? registered.erase(r) : std::next(r);
    }
  }
  for (const auto& [device, crc] : removals) {
    renodx::utils::shader::RemoveRuntimeReplacements(device, {crc});
  }
  for (const auto& [crc, code] : failed) {
    dkb::coverage::runtime::MarkRuntimeRejected(crc, "E_PIPELINE", code);
    Message(reshade::log::level::error,
            "DKB runtime: the rewritten pipeline for 0x%08X did not build; %s", crc,
            std::string("the game's composite is used again (E_PIPELINE)"));
  }
}

inline void OnDestroyDevice(reshade::api::device* device) {
  std::lock_guard lock(mutex);
  for (auto r = registered.begin(); r != registered.end();) {
    r = r->first == device ? registered.erase(r) : std::next(r);
  }
  for (auto w = watched.begin(); w != watched.end();) {
    w = w->second.device == device ? watched.erase(w) : std::next(w);
  }
  std::erase_if(pending_removal, [&](const auto& p) { return p.first == device; });
}

}  // namespace internal

// Call at DLL_PROCESS_ATTACH, after the settings are loaded and mods::shader is attached.
// hook spans are the embedded dkb_*_hook modules; shaders are the build-time entries.
inline void Attach(bool enable, std::span<const uint8_t> tonemap_hook, std::span<const uint8_t> output_hook,
                   const renodx::mods::shader::CustomShaders& shaders) {
  using namespace internal;
  for (const auto& [crc, shader] : shaders) embedded.insert(crc);
  if (!enable) {
    dkb::coverage::runtime::SetRuntimeStatus("off (enable it under Coverage, then restart the game)");
    reshade::log::message(reshade::log::level::info, "DKB runtime: structural rewrite is off.");
    return;
  }
  try {
    tonemap = dkb::structural::LoadHook(tonemap_hook, "dkb_tonemap_hook", dkb::structural::TONEMAP_INPUTS, "dkb_c");
    output = dkb::structural::LoadHook(output_hook, "dkb_output_hook", dkb::structural::OUTPUT_INPUTS, "dkb_frag_out");
  } catch (const dkb::structural::Reject& reject) {
    const std::string text = "hooks not usable: " + reject.code + " " + reject.detail;
    dkb::coverage::runtime::SetRuntimeStatus(text);
    reshade::log::message(reshade::log::level::error, ("DKB runtime: structural rewrite is off, " + text).c_str());
    return;
  }
  enabled = true;
  dkb::coverage::runtime::SetRuntimeStatus("on");
  renodx::utils::pipeline::RegisterOnCreateCallback(&OnCreatePipeline);
  renodx::utils::pipeline::RegisterOnInitCallback(&OnInitPipeline);
  renodx::utils::pipeline::RegisterOnBindCallback(&OnBindPipeline);
  renodx::utils::pipeline::RegisterOnDestroyCallback(&OnDestroyPipeline);
  reshade::register_event<reshade::addon_event::present>(&OnPresent);
  reshade::register_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
  reshade::log::message(reshade::log::level::info, "DKB runtime: structural rewrite is on.");
}

inline void Detach() {
  using namespace internal;
  if (!enabled) return;
  enabled = false;
  renodx::utils::pipeline::UnregisterOnCreateCallback(&OnCreatePipeline);
  renodx::utils::pipeline::UnregisterOnInitCallback(&OnInitPipeline);
  renodx::utils::pipeline::UnregisterOnBindCallback(&OnBindPipeline);
  renodx::utils::pipeline::UnregisterOnDestroyCallback(&OnDestroyPipeline);
  reshade::unregister_event<reshade::addon_event::present>(&OnPresent);
  reshade::unregister_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
}

}  // namespace dkb::runtime_rewrite

#endif  // SRC_GAMES_DKB_RECOMP_RUNTIME_REWRITE_HPP_
