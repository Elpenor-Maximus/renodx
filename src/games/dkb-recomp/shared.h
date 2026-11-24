// SPDX-License-Identifier: MIT
//
// Settings shared by the add-on (C++) and the replacement shaders (Slang).
// RenoDX appends a push-constant range of sizeof(ShaderInjectData) to every
// pipeline layout and pushes this struct before each replaced draw. DKB's own
// layouts carry no push constants, so the range starts at offset 0.

#ifndef SRC_GAMES_DKB_RECOMP_SHARED_H_
#define SRC_GAMES_DKB_RECOMP_SHARED_H_

#define DKB_TONE_MAP_TYPE_VANILLA 0.f
#define DKB_TONE_MAP_TYPE_RENODRT 1.f
// The game's curve with the recompilation's own highlight shoulder
// (dkb-pc-recomp runtime/spirv_hdr_shoulder.c), which hdr_shoulder=0 turns off.
#define DKB_TONE_MAP_TYPE_SHOULDER 2.f
// RenoDX PsychoV test35 (shaders/tonemap/psychov/test35.hlsl) in place of RenoDRT, experimental.
#define DKB_TONE_MAP_TYPE_PSYCHOV 3.f

// Game: follow the peak and white the game passes in its own uniforms
// (DKB hdr_peak_nits, capped by the game at 1400, and hdr_game_nits).
// Custom: use the add-on's Peak Brightness and Game Brightness sliders.
#define DKB_BRIGHTNESS_SOURCE_GAME   0.f
#define DKB_BRIGHTNESS_SOURCE_CUSTOM 1.f

// Keep every member a 32-bit float: the struct is pushed as raw dwords.
struct ShaderInjectData {
  float tone_map_type;
  float brightness_source;
  float peak_white_nits;
  float diffuse_white_nits;
  float tone_map_hue_correction;
  float tone_map_highlight_saturation;
  float tone_map_blowout;
  float tone_map_flare;
};

#ifndef __cplusplus
// Same declaration pattern as the Kirby and Zelda Vulkan mods.
struct DkbPushData {
  [[vk::offset(0)]] ShaderInjectData shader_injection;
};
[[vk::push_constant]] DkbPushData dkb_push;
#define shader_injection dkb_push.shader_injection

#define RENODX_RENO_DRT_TONE_MAP_METHOD      renodx::tonemap::renodrt::config::tone_map_method::NEUTWO
#define RENODX_GAMMA_CORRECTION              0.f
#define RENODX_TONE_MAP_CLAMP_PEAK           renodx::color::convert::COLOR_SPACE_NONE
#define RENODX_TONE_MAP_HUE_CORRECTION       shader_injection.tone_map_hue_correction
#define RENODX_TONE_MAP_HIGHLIGHT_SATURATION shader_injection.tone_map_highlight_saturation
#define RENODX_TONE_MAP_BLOWOUT              shader_injection.tone_map_blowout
#define RENODX_TONE_MAP_FLARE                shader_injection.tone_map_flare

#include "../../shaders/draw.hlsl"
#include "../../shaders/tonemap/psychov/test35.hlsl"

// The two tone mappers that extend highlights: RenoDRT and PsychoV.
bool DkbIsExtended() {
  return shader_injection.tone_map_type == DKB_TONE_MAP_TYPE_RENODRT
         || shader_injection.tone_map_type == DKB_TONE_MAP_TYPE_PSYCHOV;
}

// Scene value of middle grey. RenoDRT (Neutwo) is inverted so that grey comes out at 0.18;
// PsychoV keeps grey at its 0.18 adaptation anchor, so its input is 0.18.
float DkbGrayInput(float peakRatio, float whiteClip) {
  if (shader_injection.tone_map_type == DKB_TONE_MAP_TYPE_PSYCHOV) return 0.18f;
  return renodx::tonemap::inverse::Neutwo(0.18f, peakRatio, max(whiteClip, peakRatio));
}

// Contrast of the game's own curve at middle grey: d ln(output) / d ln(scene) there. The
// curve is Y = 1 - exp(-L(x)), L = (sigma(x) - sigmaBlack) / (sigmaWhite - sigmaBlack),
// sigma(x) = 1 / (1 + exp((center - x) slope)), with x the curve input and Y(nativeGray) = 0.18;
// the scene is proportional to x^p (DkbSceneFromExposed, p the mean power). So
//   x Y'/Y = nativeGray (1 - 0.18) slope sigmaGray (1 - sigmaGray) / ((sigmaWhite - sigmaBlack) 0.18),
// divided by p. PsychoV takes it as its cone response exponent, as RenoDX's Zelda OoT mod does with
// its game's curve, so its tone matches the game's around grey and only the highlight range
// differs. Kept within 0.5 to 3; a value that is not finite leaves PsychoV's own 1.
float DkbGrayContrast(float nativeGray, float sigmaGray, float sigmaBlack, float sigmaWhite, float slope,
                      float3 power) {
  float range = sigmaWhite - sigmaBlack;
  float contrast = nativeGray * (1.f - 0.18f) * slope * sigmaGray * (1.f - sigmaGray)
                   / (range * 0.18f * DkbMeanPower(power));
  return isfinite(contrast) ? clamp(contrast, 0.5f, 3.f) : 1.f;
}

// PsychoV parameters (psychotm_test35 arguments). The defaults are the add-on's; the tuning
// script (dkb-pc-recomp/tools/dkb_psychov_tune.py) compiles variants with -D.
#ifndef DKB_PSYCHOV_PURITY
#define DKB_PSYCHOV_PURITY 1.f       // purity_scale: colour kept, 1 PsychoV's own
#endif
#ifndef DKB_PSYCHOV_TARGET
#define DKB_PSYCHOV_TARGET 1         // gamut_compression_mode: 0 fits to BT.709, otherwise BT.2020
#endif
#ifndef DKB_PSYCHOV_COMPRESSION
// Response compression. 0: PsychoV's automatic setting, from the display's range (peak over
// grey), so it follows the peak. Tuned on the replay ramps (2026-11-22): white tops out at
// 1347 nits of 1400 against 1315 with a fixed 1 (RenoDRT 1385), with the hue correction on.
#define DKB_PSYCHOV_COMPRESSION 0.f
#endif
#ifndef DKB_PSYCHOV_HUE
#define DKB_PSYCHOV_HUE shader_injection.tone_map_hue_correction  // OKLab hue correction to the graded scene
#endif
#ifndef DKB_PSYCHOV_FIT_MODE
#define DKB_PSYCHOV_FIT_MODE 2       // 0 none, 1 RGB clip, 2 exact, 3 smooth, 4 contrast trade
#endif

// The tone mapping step, linear BT.709 relative to diffuse white. RenoDRT: RenoDX ToneMapPass.
// PsychoV: the same grade transfer (ComputeUntonemappedGraded, as ToneMapPass does), then
// psychotm_test35 at the peak ratio, adapted at 0.18 (grey stays at the game's 0.18), with the
// game's contrast at grey (DkbGrayContrast) and fitted to BT.2020 (gamut_compression_mode 1, as
// the Zelda OoT mod does for HDR). Its other controls stay at their defaults.
float3 DkbToneMap(float3 scene709, float3 graded709, float3 neutral709, renodx::draw::Config config,
                  float peakRatio, float grayContrast) {
  if (shader_injection.tone_map_type == DKB_TONE_MAP_TYPE_PSYCHOV) {
    float3 graded = renodx::draw::ComputeUntonemappedGraded(scene709, graded709, neutral709, config);
    float3 mapped = renodx::tonemap::psychov::psychotm_test35(
        graded, peakRatio, 1.f, 1.f, 1.f, 1.f, DKB_PSYCHOV_PURITY, 1.f, 100.f, 1.f, 1.f, 0, grayContrast,
        float3(0.18f, 0.18f, 0.18f), float3(0.18f, 0.18f, 0.18f), 1.f, DKB_PSYCHOV_TARGET, 1.f,
        DKB_PSYCHOV_COMPRESSION, renodx::tonemap::psychov::PSYCHO35_SOURCE_BOUNDARY_BT709, false, false,
        DKB_PSYCHOV_FIT_MODE);
    // PsychoV's cone response turns very bright saturated colours in hue (bright blue up to about
    // 29 degrees towards purple on the replay ramps, whatever its fit). The Hue Correction setting
    // takes the hue back to the graded scene's, as RenoDRT's hue correction does, keeping
    // PsychoV's brightness and chroma.
    return renodx::color::correct::Hue(mapped, graded, DKB_PSYCHOV_HUE);
  }
  return renodx::draw::ToneMapPass(scene709, graded709, neutral709, config);
}

// The recompilation's highlight shoulder at hdr_shoulder_strength 100, applied to the
// composite's clamped colour c. The game runs its shoulder
//   Y = k + (1 - k) r t / (1 + (r - 1)(1 - t)^p),  t = (Ys - k) / (1 - k),  r = (M - k) / (1 - k)
// from the knee k to Ys = 1, which its logistic curve never reaches (top = 1 - exp(-(1 - e0) n)),
// so Y stops near 1.36. spirv_hdr_shoulder.c runs the same shoulder from k to top instead and
// multiplies the graded colour by that luminance; c = min(max(0, A Y), M), so the same result is
// min(c * Yext / Y, M). Ys is the game's knee-tested brightness, c1 and c4 its uniform rows 1 and 4.
// Below the knee, without an HDR ceiling or with top not above the knee, c is returned unchanged,
// as in spirv_hdr_shoulder.c.
float3 DkbRecompShoulder(float3 c, float ys, float4 c1, float4 c4) {
  float M = c4.x, k = c4.y, p = c4.z;
  float e0 = 1.f / (1.f + exp(c1.w * c1.z));
  float e1 = 1.f / (1.f + exp((c1.w - 1.f) * c1.z));
  float lmax = (1.f - e0) * (1.f / max(e1 - e0, 1e-7f));
  float top = 1.f - exp(-lmax);
  float d = top - k;
  if (!(ys >= k && M > 1.001f && d > 0.001f)) return c;
  float t = saturate((ys - k) / d);
  float r = (M - k) / d;
  float ext = k + d * r * t / (1.f + (r - 1.f) * pow(1.f - t, p));
  float tg = saturate((ys - k) / (1.f - k));
  float rg = (M - k) / (1.f - k);
  float yg = k + (1.f - k) * rg * tg / (1.f + (rg - 1.f) * pow(1.f - tg, p));
  if (!(yg > 0.f && ext >= yg)) return c;
  return min(c * (ext / yg), M);
}

// Channel powers of the game's curve input, x_c = |E_c|^(1 / p_c) with p = c[3].rgb * c[3].w.
// Most grades use one power; a tinted grade makes them differ (outside the cave towards Ingot
// Isle, F9 dump 2026-11-21: 0.835 0.86875 0.86875), and the correction used to return the
// game's image there. Accepted: every power positive and within 25% of their luma-weighted mean.
float DkbMeanPower(float3 power) {
  return dot(power, float3(0.29891199f, 0.58661097f, 0.114478f));
}
bool DkbPowerDomain(float3 power) {
  float mean = DkbMeanPower(power);
  return all(power > 0.f) && all(abs(power - mean) <= 0.25f * mean);
}

// The scene colour for ToneMapPass from the exposed colour E, with middle grey (curve input
// nativeGray) mapped to grayInput. Equal powers: E * grayInput / nativeGray^p, the form
// validated first. Unequal powers: each channel goes to the curve's input with its own power
// and comes back with the mean power, so a colour the game's curve treats as neutral stays
// neutral and middle grey still maps to grayInput; with equal powers both forms agree.
float3 DkbSceneFromExposed(float3 exposed, float3 power, float nativeGray, float grayInput) {
  if (all(abs(power - power.x) < 0.00001f)) return exposed * (grayInput / pow(nativeGray, power.x));
  float3 x = pow(abs(exposed), 1.f / power);
  return sign(exposed) * grayInput * pow(x / nativeGray, DkbMeanPower(power));
}
#endif  // __cplusplus

#endif  // SRC_GAMES_DKB_RECOMP_SHARED_H_
