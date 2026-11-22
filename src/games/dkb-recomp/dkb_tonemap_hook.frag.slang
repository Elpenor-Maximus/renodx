// SPDX-License-Identifier: MIT
//
// Runtime hook for the structural composite rewrite (structural.hpp). This is not a
// replacement shader: CMake embeds it as __dkb_tonemap_hook, and the add-on splices
// main() into each recognised composite as the body of dkb_tonemap_hook(). The
// inputs are mapped by location to the composite's Private copies (structural.hpp
// TONEMAP_INPUTS) and the output to dkb_c. The correction is the shared
// structural_tonemap.slangi.

#include "./shared.h"

void DkbTonemap(inout float3 dkb_c, float3 dkb_exposed, float3 dkb_lut_uvw, float3 dkb_graded,
                float dkb_ys, float dkb_chroma, float dkb_has_lut, float4 dkb_c3[8]) {
#define DKB_LUT_ENABLED (dkb_has_lut == 1.f)
#define DKB_SAT_GRADE (dkb_has_lut == 2.f)
#include "./structural_tonemap.slangi"
}

struct HookInput {
  [[vk::location(0)]] float3 c : DKB_C;
  [[vk::location(1)]] float3 exposed : DKB_EXPOSED;
  [[vk::location(2)]] float3 lut_uvw : DKB_LUT_UVW;
  [[vk::location(3)]] float3 graded : DKB_GRADED;
  [[vk::location(4)]] float ys : DKB_YS;
  [[vk::location(5)]] float chroma : DKB_CHROMA;
  [[vk::location(6)]] float has_lut : DKB_HAS_LUT;
  [[vk::location(7)]] float4 c3[8] : DKB_C3;
};

float3 main(HookInput i) : SV_Target0 {
  float3 c = i.c;
  DkbTonemap(c, i.exposed, i.lut_uvw, i.graded, i.ys, i.chroma, i.has_lut, i.c3);
  return c;
}
