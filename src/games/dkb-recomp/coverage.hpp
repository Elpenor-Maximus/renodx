/*
 * SPDX-License-Identifier: MIT
 *
 * Structural classification of the game's final SPIR-V modules, for HDR coverage logging.
 *
 * The tests match dkb-pc-recomp/tools/dkb_hdr_survey.py, and its exact composite test is the
 * one dkb-pc-recomp/runtime/spirv_hdr_shoulder.c uses to find the native HDR composite:
 *   three colour stores FMin(FMax(0, FMul(x, Load Y)), Load U[m][4].x) for one Function
 *   variable Y and one uniform block U, and a comparison Load Ys >= Load U[m][4].y.
 * DKB's NaN guard runs after that test, so here a store's value is also looked through a
 * Select whose other operand is a constant.
 */

#ifndef SRC_GAMES_DKB_RECOMP_COVERAGE_HPP_
#define SRC_GAMES_DKB_RECOMP_COVERAGE_HPP_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <unordered_map>
#include <vector>

namespace dkb::coverage {

enum class Class : uint8_t {
  OTHER,
  HDR_COMPOSITE,             // the native HDR scene composite
  HDR_COMPOSITE_SHOULDERED,  // the same, already rewritten by DKB's hdr_shoulder
  HDR_COMPOSITE_LIKE,        // two of the three composite signals: review it
  PQ_ENCODER,                // ST 2084 encode, e.g. the final HDR10 pass
  LUT_GRADE,                 // a 3D LUT without cube maps, e.g. the SDR composite
};

inline const char* Name(Class value) {
  switch (value) {
    case Class::HDR_COMPOSITE:            return "hdr_composite";
    case Class::HDR_COMPOSITE_SHOULDERED: return "hdr_composite_shouldered";
    case Class::HDR_COMPOSITE_LIKE:       return "hdr_composite_like";
    case Class::PQ_ENCODER:               return "pq_encoder";
    case Class::LUT_GRADE:                return "lut_grade";
    default:                              return "other";
  }
}

// Whether this class is the scene composite the add-on is meant to replace.
inline bool IsComposite(Class value) {
  return value == Class::HDR_COMPOSITE || value == Class::HDR_COMPOSITE_SHOULDERED
         || value == Class::HDR_COMPOSITE_LIKE;
}

namespace internal {

constexpr uint32_t OP_EXT_INST_IMPORT = 11, OP_EXT_INST = 12, OP_ENTRY_POINT = 15;
constexpr uint32_t OP_TYPE_FLOAT = 22, OP_TYPE_IMAGE = 25, OP_CONSTANT = 43, OP_FUNCTION = 54;
constexpr uint32_t OP_VARIABLE = 59, OP_LOAD = 61, OP_STORE = 62, OP_ACCESS_CHAIN = 65;
constexpr uint32_t OP_FMUL = 133, OP_SELECT = 169;
constexpr uint32_t GLSL_EXP = 27, GLSL_EXP2 = 29, GLSL_FMIN = 37, GLSL_FMAX = 40;
constexpr uint32_t SC_UNIFORM = 2, SC_OUTPUT = 3, SC_FUNCTION = 7, MODEL_FRAGMENT = 4;
constexpr uint32_t DIM_3D = 2, DIM_CUBE = 3;
constexpr uint32_t MAX_BOUND = 1u << 22;

inline bool IsCompare(uint32_t op) { return op >= 180 && op <= 191; }

struct Module {
  const uint32_t* w = nullptr;
  size_t n = 0;
  uint32_t bound = 0;
  std::vector<uint32_t> def;  // result id -> word offset (0: none)
  std::vector<uint8_t> var_sc;  // variable id -> storage class + 1
  std::unordered_map<uint32_t, uint32_t> constants;
  std::set<uint32_t> constant_values;
  std::set<uint32_t> f32;
  uint32_t glsl = 0;
  size_t fn_at = 0;
  uint32_t out_var = 0;
  bool fragment = false;
  bool image_3d = false;
  bool image_cube = false;

  [[nodiscard]] uint32_t Op(size_t at) const { return w[at] & 0xFFFFu; }
  [[nodiscard]] uint32_t Count(size_t at) const { return w[at] >> 16; }

  // Operand words of the instruction defining id when it has opcode op, else nullptr.
  [[nodiscard]] const uint32_t* DefOf(uint32_t id, uint32_t op, uint32_t* count = nullptr) const {
    if (id == 0 || id >= bound || def[id] == 0) return nullptr;
    const size_t at = def[id];
    if (Op(at) != op) return nullptr;
    if (count != nullptr) *count = Count(at) - 1u;
    return w + at + 1;
  }

  [[nodiscard]] bool IsConst(uint32_t id) const { return constants.contains(id); }
  [[nodiscard]] uint32_t ConstValue(uint32_t id) const {
    const auto it = constants.find(id);
    return it == constants.end() ? 0xFFFFFFFFu : it->second;
  }

  // GLSL.std.450 call arguments when id is that call, else nullptr.
  [[nodiscard]] const uint32_t* Glsl(uint32_t id, uint32_t inst, uint32_t nargs) const {
    uint32_t count = 0;
    const uint32_t* a = DefOf(id, OP_EXT_INST, &count);
    if (a == nullptr || count != 4u + nargs || a[2] != glsl || a[3] != inst) return nullptr;
    return a + 4;
  }

  [[nodiscard]] uint32_t FunctionVarLoaded(uint32_t id) const {
    uint32_t count = 0;
    const uint32_t* a = DefOf(id, OP_LOAD, &count);
    if (a == nullptr || count < 3 || a[2] >= bound || var_sc[a[2]] != SC_FUNCTION + 1u) return 0;
    return a[2];
  }

  // Whether id is a Load of U[member][row].comp of a Uniform variable; fills u and member.
  bool UniformElement(uint32_t id, uint32_t row, uint32_t comp, uint32_t* u, uint32_t* member) const {
    uint32_t count = 0;
    const uint32_t* ld = DefOf(id, OP_LOAD, &count);
    if (ld == nullptr || count < 3) return false;
    const uint32_t* ac = DefOf(ld[2], OP_ACCESS_CHAIN, &count);
    if (ac == nullptr || count != 6) return false;
    if (ac[2] >= bound || var_sc[ac[2]] != SC_UNIFORM + 1u) return false;
    if (!IsConst(ac[3]) || !IsConst(ac[4]) || !IsConst(ac[5])) return false;
    if (ConstValue(ac[4]) != row || ConstValue(ac[5]) != comp) return false;
    if (u != nullptr) *u = ac[2];
    if (member != nullptr) *member = ConstValue(ac[3]);
    return true;
  }
};

inline bool Parse(const uint32_t* w, size_t n, Module* m) {
  if (n < 5 || w[0] != 0x07230203u || w[3] > MAX_BOUND) return false;
  m->w = w, m->n = n, m->bound = w[3];
  m->def.assign(m->bound, 0u);
  m->var_sc.assign(m->bound, 0u);
  std::vector<std::pair<uint32_t, uint32_t>> image_types;  // dim, sampled
  for (size_t i = 5; i < n;) {
    const uint32_t op = w[i] & 0xFFFFu, wc = w[i] >> 16;
    if (wc == 0 || i + wc > n) return false;
    const uint32_t* a = w + i + 1;
    const uint32_t na = wc - 1u;
    if (op == OP_FUNCTION && m->fn_at == 0) m->fn_at = i;
    if (op == OP_ENTRY_POINT && na >= 1) {
      m->fragment = m->fragment || a[0] == MODEL_FRAGMENT;
    } else if (op == OP_EXT_INST_IMPORT && na >= 4 && a[0] < m->bound
               && std::memcmp(a + 1, "GLSL.std.450", 13) == 0) {
      m->glsl = a[0];
    } else if (op == OP_TYPE_FLOAT && na >= 2 && a[1] == 32u) {
      m->f32.insert(a[0]);
    } else if (op == OP_TYPE_IMAGE && na >= 7) {
      if (a[6] == 1u && a[2] == DIM_3D) m->image_3d = true;
      if (a[6] == 1u && a[2] == DIM_CUBE) m->image_cube = true;
    } else if (op == OP_CONSTANT && na == 3 && a[1] < m->bound) {
      m->constants[a[1]] = a[2];
      m->constant_values.insert(a[2]);
    } else if (op == OP_VARIABLE && na >= 3 && a[1] < m->bound) {
      m->var_sc[a[1]] = static_cast<uint8_t>(a[2] + 1u);
      if (a[2] == SC_OUTPUT && m->out_var == 0) m->out_var = a[1];
    }
    // Typed results this classifier follows.
    if ((op == OP_LOAD || op == OP_ACCESS_CHAIN || op == OP_EXT_INST || op == OP_FMUL || op == OP_SELECT
         || op == OP_CONSTANT)
        && na >= 2 && a[1] < m->bound) {
      m->def[a[1]] = static_cast<uint32_t>(i);
    }
    i += wc;
  }
  return true;
}

}  // namespace internal

inline Class Classify(const void* code, size_t size) {
  using namespace internal;
  if (code == nullptr || size < 20 || size % 4 != 0) return Class::OTHER;
  Module m;
  if (!Parse(static_cast<const uint32_t*>(code), size / 4, &m)) return Class::OTHER;

  // Colour stores into the first output's x, y and z.
  std::set<uint32_t> clamp_comps, exact_comps, shouldered_comps, y_vars, uniforms;
  if (m.fragment && m.fn_at != 0 && m.out_var != 0 && m.glsl != 0) {
    for (size_t i = m.fn_at; i < m.n; i += m.Count(i)) {
      if (m.Op(i) != OP_STORE || m.Count(i) < 3) continue;
      const uint32_t* a = m.w + i + 1;
      uint32_t count = 0;
      const uint32_t* ac = m.DefOf(a[0], OP_ACCESS_CHAIN, &count);
      if (ac == nullptr || count != 4 || ac[2] != m.out_var || !m.IsConst(ac[3]) || m.ConstValue(ac[3]) >= 3) continue;
      const uint32_t comp = m.ConstValue(ac[3]);
      uint32_t values[2] = {a[1], 0};
      if (const uint32_t* sel = m.DefOf(a[1], OP_SELECT, &count); sel != nullptr && count == 5) {
        values[0] = m.IsConst(sel[3]) ? 0 : sel[3];
        values[1] = m.IsConst(sel[4]) ? 0 : sel[4];
      }
      for (const uint32_t value : values) {
        const uint32_t* fmin = value != 0 ? m.Glsl(value, GLSL_FMIN, 2) : nullptr;
        uint32_t u = 0, member = 0;
        if (fmin == nullptr || !m.UniformElement(fmin[1], 4, 0, &u, &member)) continue;
        clamp_comps.insert(comp);
        uniforms.insert(u * 64u + member);
        const uint32_t* fmax = m.Glsl(fmin[0], GLSL_FMAX, 2);
        if (fmax == nullptr || !m.IsConst(fmax[0]) || m.ConstValue(fmax[0]) != 0u) continue;
        const uint32_t* mul = m.DefOf(fmax[1], OP_FMUL, &count);
        if (mul == nullptr || count != 4 || !m.f32.contains(mul[0])) continue;
        if (const uint32_t y = m.FunctionVarLoaded(mul[3]); y != 0) {
          exact_comps.insert(comp);
          y_vars.insert(y);
        } else if (m.DefOf(mul[3], OP_SELECT) != nullptr) {
          shouldered_comps.insert(comp);
        }
      }
    }
  }

  bool knee = false, loads_c1z = false, loads_c1w = false;
  uint32_t exps = 0;
  std::set<uint32_t> ys_vars;
  for (size_t i = (m.fn_at != 0 ? m.fn_at : 5); i < m.n; i += m.Count(i)) {
    const uint32_t op = m.Op(i), na = m.Count(i) - 1u;
    const uint32_t* a = m.w + i + 1;
    if (IsCompare(op) && na == 4) {
      if (m.UniformElement(a[3], 4, 1, nullptr, nullptr)) {
        knee = true;
        if (op == 190 || op == 191) {
          if (const uint32_t ys = m.FunctionVarLoaded(a[2]); ys != 0) ys_vars.insert(ys);
        }
      }
    } else if (op == OP_LOAD && na >= 2) {
      if (m.UniformElement(a[1], 1, 2, nullptr, nullptr)) loads_c1z = true;
      if (m.UniformElement(a[1], 1, 3, nullptr, nullptr)) loads_c1w = true;
    } else if (op == OP_EXT_INST && na >= 4 && a[2] == m.glsl && (a[3] == GLSL_EXP || a[3] == GLSL_EXP2)) {
      ++exps;
    }
  }
  const bool logistic = loads_c1z && loads_c1w && exps > 0;

  bool ys_distinct = false;
  for (const uint32_t ys : ys_vars) ys_distinct = ys_distinct || !y_vars.contains(ys);
  if (m.fragment && exact_comps.size() == 3 && y_vars.size() == 1 && uniforms.size() == 1 && ys_distinct) {
    return Class::HDR_COMPOSITE;
  }
  if (m.fragment && shouldered_comps.size() == 3 && knee) return Class::HDR_COMPOSITE_SHOULDERED;
  if (static_cast<int>(clamp_comps.size() == 3) + static_cast<int>(knee) + static_cast<int>(logistic) >= 2) {
    return Class::HDR_COMPOSITE_LIKE;
  }
  static constexpr uint32_t PQ[] = {0x3E232000u, 0x429DB000u, 0x3F560000u, 0x4196D000u, 0x41958000u};
  int pq = 0;
  for (const uint32_t c : PQ) pq += static_cast<int>(m.constant_values.contains(c));
  if (pq >= 3) return Class::PQ_ENCODER;
  if (m.image_3d && !m.image_cube) return Class::LUT_GRADE;
  return Class::OTHER;
}

}  // namespace dkb::coverage

#endif  // SRC_GAMES_DKB_RECOMP_COVERAGE_HPP_
