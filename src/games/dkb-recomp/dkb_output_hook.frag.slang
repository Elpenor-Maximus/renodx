// SPDX-License-Identifier: MIT
//
// Runtime hook for the structural composite rewrite (structural.hpp), spliced into
// each recognised composite as the body of dkb_output_hook(), which runs before main
// returns. Input 0 and the output are mapped to the composite's colour output
// (dkb_frag_out), input 1 to its Private copy of the uniform rows. The correction is
// the shared structural_output.slangi.

#include "./shared.h"

void DkbOutput(inout float4 dkb_frag_out, float4 dkb_c3[8]) {
#include "./structural_output.slangi"
}

struct HookInput {
  [[vk::location(0)]] float4 frag : DKB_FRAG;
  [[vk::location(1)]] float4 c3[8] : DKB_C3;
};

float4 main(HookInput i) : SV_Target0 {
  float4 o = i.frag;
  DkbOutput(o, i.c3);
  return o;
}
