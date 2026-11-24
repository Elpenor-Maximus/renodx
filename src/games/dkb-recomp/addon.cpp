/*
 * SPDX-License-Identifier: MIT
 *
 * RenoDX for the Donkey Kong Bananza PC recompilation (Vulkan, native HDR).
 *
 * Replaces the native HDR scene composites (final Vulkan fragment modules).
 * Build-time entries (the gameplay reference 0xB537A7CA and any structural_
 * entries) come first; every other composite is rewritten when the game
 * creates it (runtime_rewrite.hpp). The game keeps its float scene, HUD
 * composite and PQ encoder. See README.md and dkb-pc-recomp/docs/renodx-vulkan.md.
 */

#define ImTextureID ImU64

#include <embed/shaders.h>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include <cstring>
#include <string>

#include "../../mods/shader.hpp"
#include "../../utils/platform.hpp"
#include "../../utils/settings.hpp"
#include "../../utils/command_action.hpp"
#include "./coverage_runtime.hpp"
#include "./runtime_rewrite.hpp"
#include "./shared.h"

// Build-time entries are generated locally from the user's own capture by
// dkb-pc-recomp/tools/build_renodx_gameplay.py and dkb_composite_struct.py.
// Without them the add-on relies on the runtime rewrite alone.
#ifndef __ALL_CUSTOM_SHADERS
#define __ALL_CUSTOM_SHADERS
#endif

namespace {

constexpr uint32_t GAMEPLAY_CRC32 = 0xB537A7CA;

renodx::mods::shader::CustomShaders custom_shaders = {__ALL_CUSTOM_SHADERS};

ShaderInjectData shader_injection;

// Read once at startup: the runtime rewrite applies to pipelines as the game creates them.
float runtime_rewrite_setting = 1.f;

// mods::shader turns on its draw hooks only if a shader is registered when it attaches. With
// no build-time entries, this placeholder keeps them on for the runtime rewrite's shaders.
// 0xFFFFFFFF is not the CRC32 of any composite in the corpus; a match would only push the
// settings to that shader.
renodx::mods::shader::CustomShader draw_hook_placeholder = {.crc32 = 0xFFFFFFFFu};

bool IsCustomBrightness() {
  return shader_injection.brightness_source == DKB_BRIGHTNESS_SOURCE_CUSTOM;
}

bool IsRenoDRT() {
  return shader_injection.tone_map_type == DKB_TONE_MAP_TYPE_RENODRT;
}

// RenoDRT or PsychoV: the tone mappers that use the brightness settings.
bool IsExtended() {
  return IsRenoDRT() || shader_injection.tone_map_type == DKB_TONE_MAP_TYPE_PSYCHOV;
}

renodx::utils::settings::Settings settings = {
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::CUSTOM,
        .label = "hdr_shoulder warning",
        .section = "Tone Mapping",
        .on_draw = [] {
          ImGui::TextColored(ImVec4(1.f, 0.35f, 0.25f, 1.f),
                             "hdr_shoulder is on in dkb.ini, so these settings do nothing.\n"
                             "Set hdr_shoulder = 0 in dkb.ini and restart the game.");
          return false;
        },
        .is_visible = &dkb::coverage::runtime::ShoulderOn,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapType",
        .binding = &shader_injection.tone_map_type,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .can_reset = true,
        .label = "Tone Mapper",
        .section = "Tone Mapping",
        .tooltip = "Vanilla runs the game's own HDR composite (its console curve, with hdr_shoulder=0).\n"
                   "RenoDRT extends its highlights to the peak brightness.\n"
                   "Vanilla + shoulder adds the recompilation's own highlight shoulder (hdr_shoulder),\n"
                   "as the main DKB build shows it.\n"
                   "PsychoV (experimental) extends the highlights with RenoDX's PsychoV test35 tone mapper instead of RenoDRT.\n"
                   "Highlight Saturation, Blowout and Flare apply to RenoDRT only.",
        .labels = {"Vanilla", "RenoDRT", "Vanilla + shoulder", "PsychoV (experimental)"},
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapBrightnessSource",
        .binding = &shader_injection.brightness_source,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .can_reset = true,
        .label = "Brightness Source",
        .section = "Tone Mapping",
        .tooltip = "Game follows the game's HDR settings (hdr_peak_nits, capped at 1400 nits, and hdr_game_nits).\n"
                   "Custom uses the two sliders below.",
        .labels = {"Game", "Custom"},
        .is_enabled = &IsExtended,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapPeakNits",
        .binding = &shader_injection.peak_white_nits,
        .default_value = 1000.f,
        .can_reset = false,
        .label = "Peak Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of peak white in nits.\n"
                   "The game's HDR10 metadata still reports its own peak.",
        .min = 48.f,
        .max = 4000.f,
        .is_enabled = []() { return IsExtended() && IsCustomBrightness(); },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapGameNits",
        .binding = &shader_injection.diffuse_white_nits,
        .default_value = 203.f,
        .label = "Game Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of 100% white in nits.\n"
                   "The HUD stays at the game's hdr_game_nits.",
        .min = 48.f,
        .max = 500.f,
        .is_enabled = []() { return IsExtended() && IsCustomBrightness(); },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapHueCorrection",
        .binding = &shader_injection.tone_map_hue_correction,
        .default_value = 100.f,
        .label = "Hue Correction",
        .section = "Tone Mapping",
        .tooltip = "Hue retention strength in extended highlights (RenoDRT and PsychoV).",
        .min = 0.f,
        .max = 100.f,
        .is_enabled = &IsExtended,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeHighlightSaturation",
        .binding = &shader_injection.tone_map_highlight_saturation,
        .default_value = 50.f,
        .label = "Highlight Saturation",
        .section = "Color Grading",
        .tooltip = "Adds or removes highlight color.",
        .max = 100.f,
        .is_enabled = &IsRenoDRT,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeBlowout",
        .binding = &shader_injection.tone_map_blowout,
        .default_value = 0.f,
        .label = "Blowout",
        .section = "Color Grading",
        .tooltip = "Controls highlight desaturation due to overexposure.",
        .max = 100.f,
        .is_enabled = &IsRenoDRT,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeFlare",
        .binding = &shader_injection.tone_map_flare,
        .default_value = 0.f,
        .label = "Flare",
        .section = "Color Grading",
        .tooltip = "Flare/Glare Compensation",
        .max = 100.f,
        .is_enabled = &IsRenoDRT,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::CUSTOM,
        .label = "Shader coverage",
        .section = "Coverage",
        .on_draw = &dkb::coverage::runtime::Draw,
    },
    new renodx::utils::settings::Setting{
        .key = "StructuralRuntime",
        .binding = &runtime_rewrite_setting,
        .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
        .default_value = 1.f,
        .label = "Structural rewrite",
        .section = "Coverage",
        .tooltip = "Applies the correction to scene composites without a build-time entry, as the game creates them.\n"
                   "Composites it does not recognise keep the game's shader; the list above gives the reason.\n"
                   "Takes effect the next time the game starts.",
        .is_global = true,
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::BUTTON,
        .label = "Save unreplaced composites",
        .section = "Coverage",
        .tooltip = "Writes each scene composite the add-on does not replace to renodx-dkb-recomp-coverage\\ "
                   "beside the add-on, for dkb-pc-recomp/tools/build_renodx_gameplay.py.",
        .on_click = &dkb::coverage::runtime::SaveUnreplaced,
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::TEXT,
        .label = "Highlights only: below the game's HDR knee the image is the game's own. "
                 "Requires native HDR with hdr_shoulder=0 in dkb.ini, so Vanilla is the game's console "
                 "HDR curve; Vanilla + shoulder applies the recompilation's highlight shoulder in the add-on.",
        .section = "About",
    },
};

void OnPresetOff() {
  renodx::utils::settings::UpdateSetting("ToneMapType", 0.f);
  renodx::utils::settings::UpdateSetting("ToneMapBrightnessSource", 0.f);
}

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for Donkey Kong Bananza PC Recompilation";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH: {
      // Only attach to the recompilation, not to other Vulkan processes that
      // share the ReShade install.
      const auto filename = renodx::utils::platform::GetCurrentProcessPath().filename().string();
      if (_stricmp(filename.c_str(), "dkb.exe") != 0) return FALSE;
      if (!reshade::register_addon(h_module)) return FALSE;

      if (!custom_shaders.contains(GAMEPLAY_CRC32)) {
        reshade::log::message(reshade::log::level::info,
                              "DKB: no build-time gameplay entry (release build); the structural rewrite "
                              "replaces the gameplay composite.");
      }
      if (custom_shaders.empty()) {
        renodx::utils::command_action::Register(
            renodx::mods::shader::OnCommandAction,
            {
                .shader_hash = draw_hook_placeholder.crc32,
                .command_types = renodx::utils::command_action::COMMAND_TYPE_DIRECT_DRAW
                                 | renodx::utils::command_action::COMMAND_TYPE_INDIRECT,
            },
            &draw_hook_placeholder);
      }

      // The bind-time replacement exercised in the DevKit runs. It leaves
      // DKB's early graphics-pipeline-library creation on its native path.
      renodx::mods::shader::force_pipeline_cloning = true;
      renodx::utils::shader::use_replace_async = true;

      // Settings travel as a push-constant range RenoDX appends to every
      // pipeline layout. DKB's layouts declare none, so it starts at 0.
      renodx::mods::shader::allow_multiple_push_constants = true;
      renodx::mods::shader::minimum_constant_buffer_stages = reshade::api::shader_stage::pixel;
      break;
    }
    case DLL_PROCESS_DETACH:
      dkb::runtime_rewrite::Detach();
      dkb::coverage::runtime::Detach();
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  // Coverage logging and the runtime rewrite hook RenoDX's pipeline tracking, which
  // mods::shader attaches. The settings are loaded by now.
  if (fdw_reason == DLL_PROCESS_ATTACH) {
    dkb::coverage::runtime::Attach(h_module, custom_shaders);
    dkb::runtime_rewrite::Attach(runtime_rewrite_setting != 0.f, __dkb_tonemap_hook, __dkb_output_hook,
                                 custom_shaders);
  }

  return TRUE;
}
