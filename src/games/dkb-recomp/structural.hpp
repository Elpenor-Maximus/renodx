/*
 * SPDX-License-Identifier: MIT
 *
 * Structural RenoDX correction for DKB's native HDR composites, at runtime.
 *
 * C++ port of dkb-pc-recomp/tools/dkb_composite_struct.py (the prototype and the
 * test oracle; docs/renodx-structural.md). With no hooks, Rewrite() produces the
 * same bytes as the prototype's `rewrite`; tools/structural_cli.cpp checks that.
 *
 *   Recognize(module)            fail-safe recognition: a Recognition, or a Reject
 *                                with a reason code (E_CLAMPS, E_GRADE, ...)
 *   Rewrite(module, recognition) the instrumented module with two empty functions,
 *                                dkb_tonemap_hook() and dkb_output_hook()
 *   Splice(rewritten, hooks)     the hook bodies (dkb_tonemap_hook.frag.slang and
 *                                dkb_output_hook.frag.slang, compiled when the add-on
 *                                is built) merged into those two functions
 *   SelfCheck(words)             ids unique and below the bound, every id operand
 *                                of main defined at a dominating point
 *
 * No ReShade or Vulkan dependency: the add-on (runtime_rewrite.hpp) and the
 * command-line tool both include this file. Every failure throws Reject; callers
 * keep the original shader.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dkb::structural {

struct Reject {
  std::string code;
  std::string detail;
};

[[noreturn]] inline void Fail(const char* code, std::string detail = {}) {
  throw Reject{code, std::move(detail)};
}

namespace op {
constexpr uint32_t NAME = 5, EXT_IMPORT = 11, EXT = 12, MEMORY_MODEL = 14, ENTRY = 15, EXEC_MODE = 16, CAPABILITY = 17;
constexpr uint32_t TYPE_VOID = 19, TYPE_BOOL = 20, TYPE_INT = 21, TYPE_FLOAT = 22, TYPE_VECTOR = 23, TYPE_MATRIX = 24;
constexpr uint32_t TYPE_IMAGE = 25, TYPE_SAMPLER = 26, TYPE_SAMPLED_IMAGE = 27, TYPE_ARRAY = 28, TYPE_RUNTIME_ARRAY = 29;
constexpr uint32_t TYPE_STRUCT = 30, TYPE_POINTER = 32, TYPE_FUNCTION = 33;
constexpr uint32_t CONSTANT_TRUE = 41, CONSTANT_FALSE = 42, CONSTANT = 43, CONSTANT_COMPOSITE = 44, CONSTANT_NULL = 46;
constexpr uint32_t FUNCTION = 54, FUNCTION_PARAM = 55, FUNCTION_END = 56, CALL = 57, VARIABLE = 59;
constexpr uint32_t LOAD = 61, STORE = 62, AC = 65, IBAC = 66, DECORATE = 71, MEMBER_DECORATE = 72;
constexpr uint32_t SHUFFLE = 79, CONSTRUCT = 80, EXTRACT = 81, INSERT = 82, COPY = 83;
constexpr uint32_t SAMPLED_IMAGE = 86, SAMPLE = 87, SAMPLE_LOD = 88;
constexpr uint32_t BITCAST = 124, FNEG = 127, FADD = 129, FSUB = 131, FMUL = 133, FDIV = 136;
constexpr uint32_t ISNAN = 156, SELECT = 169, UGT = 172, FGE = 190, FUGE = 191;
constexpr uint32_t XOR = 198, AND = 199;
constexpr uint32_t PHI = 245, LOOP_MERGE = 246, SELECTION_MERGE = 247, LABEL = 248;
constexpr uint32_t BRANCH = 249, BRANCH_COND = 250, SWITCH = 251, KILL = 252, RETURN = 253, RETURN_VALUE = 254;
constexpr uint32_t UNREACHABLE = 255, TERMINATE = 4416;
}  // namespace op

namespace glsl {
constexpr uint32_t FABS = 4, EXP2 = 29, LOG2 = 30, FMIN = 37, FMAX = 40, FCLAMP = 43, FMA = 50, NMIN = 79, NMAX = 80;
}

constexpr uint32_t SC_INPUT = 1, SC_UNIFORM = 2, SC_OUTPUT = 3, SC_PRIVATE = 6, SC_FUNCTION = 7, SC_PUSH_CONSTANT = 9;
constexpr uint32_t DEC_LOCATION = 30;
constexpr uint32_t MAGIC = 0x07230203u;
constexpr std::array<double, 3> GAME_LUMA = {0.298911988735198974609375, 0.586610972881317138671875,
                                             0.114477999508380889892578125};

inline bool IsTerminator(uint32_t o) {
  return o == op::BRANCH || o == op::BRANCH_COND || o == op::SWITCH || o == op::KILL || o == op::RETURN
         || o == op::RETURN_VALUE || o == op::UNREACHABLE || o == op::TERMINATE;
}

inline float F32(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

inline uint32_t Bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

inline std::string StringOperand(std::span<const uint32_t> words) {
  std::string s;
  for (uint32_t w : words) {
    for (int i = 0; i < 4; ++i) {
      char c = static_cast<char>((w >> (8 * i)) & 0xFF);
      if (c == 0) return s;
      s.push_back(c);
    }
  }
  return s;
}

inline std::vector<uint32_t> EncodeString(const std::string& text) {
  std::vector<uint32_t> out((text.size() + 4) / 4, 0u);
  for (size_t i = 0; i < text.size(); ++i) out[i / 4] |= static_cast<uint32_t>(static_cast<uint8_t>(text[i])) << (8 * (i % 4));
  return out;
}

struct Inst {
  uint32_t op = 0;
  std::vector<uint32_t> a;
};

// Result id of an instruction, or 0 (same rules as the prototype).
inline uint32_t ResultOf(uint32_t o, const std::vector<uint32_t>& a) {
  static const std::unordered_set<uint32_t> no_result = {
      0, 2, 3, 4, op::NAME, 6, 8, 10, 14, op::ENTRY, op::EXEC_MODE, op::CAPABILITY, op::DECORATE,
      op::MEMBER_DECORATE, 74, 75, op::STORE, 63, 64, op::LOOP_MERGE, op::SELECTION_MERGE, op::FUNCTION_END,
      224, 225, 317, 330, 331, 332, 5632, 5633, 5380, op::BRANCH, op::BRANCH_COND, op::SWITCH, op::KILL,
      op::RETURN, op::RETURN_VALUE, op::UNREACHABLE, op::TERMINATE};
  if (no_result.contains(o) || a.empty()) return 0;
  if (o == 7 || o == op::EXT_IMPORT || o == 73 || o == op::LABEL || (o >= 19 && o <= 39)) return a[0];
  if (o == op::EXT && a.size() < 2) return 0;
  return a.size() >= 2 ? a[1] : 0;
}

struct TypeInfo {
  uint32_t op;
  std::vector<uint32_t> rest;  // operands after the result id
};

struct Module {
  std::array<uint32_t, 5> header{};
  std::vector<Inst> insts;

  // Index (rebuilt by Index()).
  std::unordered_map<uint32_t, size_t> where;           // id -> instruction index
  std::vector<uint32_t> def_order;                       // ids in definition order
  std::unordered_map<uint32_t, std::string> names;
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> constants;  // OpConstant id -> (type, first word)
  std::vector<uint32_t> constant_order;
  std::unordered_map<uint32_t, TypeInfo> types;
  std::vector<uint32_t> type_order;
  std::unordered_map<uint32_t, std::map<uint32_t, std::vector<uint32_t>>> decorations;
  uint32_t glsl = 0;
  std::vector<std::pair<size_t, size_t>> functions;     // (OpFunction, OpFunctionEnd)

  static Module Parse(std::span<const uint8_t> bytes) {
    if (bytes.size() % 4 != 0 || bytes.size() < 20) Fail("E_SPIRV", "not a SPIR-V module");
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return Parse(words);
  }

  static Module Parse(std::span<const uint32_t> words) {
    if (words.size() < 5 || words[0] != MAGIC) Fail("E_SPIRV", "bad magic");
    Module m;
    std::copy(words.begin(), words.begin() + 5, m.header.begin());
    size_t i = 5;
    while (i < words.size()) {
      const uint32_t wc = words[i] >> 16;
      const uint32_t o = words[i] & 0xFFFFu;
      if (wc == 0 || i + wc > words.size()) Fail("E_SPIRV", "truncated instruction");
      m.insts.push_back({o, std::vector<uint32_t>(words.begin() + static_cast<ptrdiff_t>(i + 1),
                                                  words.begin() + static_cast<ptrdiff_t>(i + wc))});
      i += wc;
    }
    m.Index();
    return m;
  }

  [[nodiscard]] std::vector<uint32_t> Serialize() const {
    std::vector<uint32_t> out(header.begin(), header.end());
    for (const auto& inst : insts) {
      out.push_back(static_cast<uint32_t>((inst.a.size() + 1) << 16) | inst.op);
      out.insert(out.end(), inst.a.begin(), inst.a.end());
    }
    return out;
  }

  void Index() {
    where.clear();
    def_order.clear();
    names.clear();
    constants.clear();
    constant_order.clear();
    types.clear();
    type_order.clear();
    decorations.clear();
    glsl = 0;
    functions.clear();
    size_t fn_start = 0;
    for (size_t k = 0; k < insts.size(); ++k) {
      const auto& [o, a] = insts[k];
      const uint32_t r = ResultOf(o, a);
      if (r != 0) {
        if (!where.contains(r)) def_order.push_back(r);
        where[r] = k;
      }
      if (o == op::NAME && !a.empty()) {
        names[a[0]] = StringOperand(std::span(a).subspan(1));
      } else if (o == op::EXT_IMPORT && StringOperand(std::span(a).subspan(1)) == "GLSL.std.450") {
        glsl = a[0];
      } else if (o == op::CONSTANT && a.size() >= 2) {
        if (!constants.contains(a[1])) constant_order.push_back(a[1]);
        constants[a[1]] = {a[0], a.size() > 2 ? a[2] : 0u};
      } else if (o >= 19 && o <= 39 && !a.empty()) {
        if (!types.contains(a[0])) type_order.push_back(a[0]);
        types[a[0]] = {o, std::vector<uint32_t>(a.begin() + 1, a.end())};
      } else if (o == op::DECORATE && a.size() >= 2) {
        decorations[a[0]][a[1]] = std::vector<uint32_t>(a.begin() + 2, a.end());
      } else if (o == op::FUNCTION) {
        fn_start = k;
      } else if (o == op::FUNCTION_END) {
        functions.emplace_back(fn_start, k);
      }
    }
  }

  [[nodiscard]] uint32_t version() const { return header[1]; }
  [[nodiscard]] uint32_t bound() const { return header[3]; }

  [[nodiscard]] const Inst* Def(uint32_t id) const {
    auto it = where.find(id);
    return it == where.end() ? nullptr : &insts[it->second];
  }
  [[nodiscard]] uint32_t OpOf(uint32_t id) const {
    const auto* d = Def(id);
    return d == nullptr ? 0u : d->op;
  }
  [[nodiscard]] const TypeInfo* Type(uint32_t id) const {
    auto it = types.find(id);
    return it == types.end() ? nullptr : &it->second;
  }
  [[nodiscard]] bool IsFloatConst(uint32_t id, double value, double tol = 0.0) const {
    auto it = constants.find(id);
    if (it == constants.end()) return false;
    const auto* t = Type(it->second.first);
    if (t == nullptr || t->op != op::TYPE_FLOAT) return false;
    return std::abs(static_cast<double>(F32(it->second.second)) - value) <= tol;
  }
  [[nodiscard]] std::optional<uint32_t> IntConst(uint32_t id) const {
    auto it = constants.find(id);
    if (it == constants.end()) return std::nullopt;
    const auto* t = Type(it->second.first);
    if (t == nullptr || t->op != op::TYPE_INT) return std::nullopt;
    return it->second.second;
  }
  struct Ext {
    uint32_t inst;
    std::vector<uint32_t> operands;
  };
  [[nodiscard]] std::optional<Ext> GetExt(uint32_t id, std::optional<uint32_t> inst = std::nullopt) const {
    const auto* d = Def(id);
    if (d == nullptr || d->op != op::EXT || d->a.size() < 4 || d->a[2] != glsl) return std::nullopt;
    if (inst.has_value() && d->a[3] != *inst) return std::nullopt;
    return Ext{d->a[3], std::vector<uint32_t>(d->a.begin() + 4, d->a.end())};
  }
  [[nodiscard]] std::optional<uint32_t> VarClass(uint32_t id) const {
    const auto* d = Def(id);
    if (d == nullptr || d->op != op::VARIABLE || d->a.size() < 3) return std::nullopt;
    return d->a[2];
  }
  [[nodiscard]] uint32_t Pointee(uint32_t ptr_type) const {
    const auto* t = Type(ptr_type);
    return t != nullptr && t->op == op::TYPE_POINTER && t->rest.size() >= 2 ? t->rest[1] : 0u;
  }
  struct UniformElem {
    uint32_t var, member, row, comp;
  };
  [[nodiscard]] std::optional<UniformElem> GetUniformElem(uint32_t ptr) const {
    const auto* d = Def(ptr);
    if (d == nullptr || (d->op != op::AC && d->op != op::IBAC) || d->a.size() != 6) return std::nullopt;
    if (VarClass(d->a[2]) != SC_UNIFORM) return std::nullopt;
    auto m0 = IntConst(d->a[3]), r = IntConst(d->a[4]), c = IntConst(d->a[5]);
    if (!m0 || !r || !c) return std::nullopt;
    return UniformElem{d->a[2], *m0, *r, *c};
  }
  using UboKey = std::pair<uint32_t, uint32_t>;
  [[nodiscard]] std::optional<UboKey> LoadsUniform(uint32_t id, uint32_t row, uint32_t comp,
                                                   std::optional<UboKey> var = std::nullopt) const {
    const auto* d = Def(id);
    if (d == nullptr || d->op != op::LOAD || d->a.size() < 3) return std::nullopt;
    auto u = GetUniformElem(d->a[2]);
    if (!u || u->row != row || u->comp != comp) return std::nullopt;
    UboKey key{u->var, u->member};
    if (var.has_value() && key != *var) return std::nullopt;
    return key;
  }
};

// CFG, dominators and reaching stores of one function.
struct Function {
  const Module* m;
  size_t start, end;
  std::vector<uint32_t> block_order;
  std::unordered_map<uint32_t, std::vector<size_t>> blocks;
  std::unordered_map<size_t, uint32_t> block_of;
  std::unordered_map<uint32_t, std::vector<uint32_t>> succ;
  std::unordered_map<uint32_t, std::set<uint32_t>> dom;
  std::unordered_set<uint32_t> reachable;                    // blocks reachable from the entry
  std::unordered_map<uint32_t, std::vector<size_t>> stores;  // base variable -> store indices
  uint32_t entry = 0;

  Function(const Module& mod, size_t s, size_t e) : m(&mod), start(s), end(e) {
    uint32_t label = 0;
    bool have = false;
    for (size_t k = start + 1; k < end; ++k) {
      const auto& inst = m->insts[k];
      if (inst.op == op::LABEL) {
        label = inst.a[0];
        have = true;
        block_order.push_back(label);
        blocks[label] = {};
      }
      if (have) {
        blocks[label].push_back(k);
        block_of[k] = label;
      }
    }
    if (block_order.empty()) Fail("E_SPIRV", "function without blocks");
    for (uint32_t b : block_order) {
      const auto& last = m->insts[blocks[b].back()];
      std::vector<uint32_t> s2;
      if (last.op == op::BRANCH) {
        s2 = {last.a[0]};
      } else if (last.op == op::BRANCH_COND) {
        s2 = {last.a[1], last.a[2]};
      } else if (last.op == op::SWITCH) {
        s2.push_back(last.a[1]);
        for (size_t i = 3; i < last.a.size(); i += 2) s2.push_back(last.a[i]);
      }
      succ[b] = s2;
    }
    entry = block_order.front();
    std::vector<uint32_t> work = {entry};
    reachable.insert(entry);
    while (!work.empty()) {
      const uint32_t b = work.back();
      work.pop_back();
      for (uint32_t t : succ[b]) {
        if (blocks.contains(t) && reachable.insert(t).second) work.push_back(t);
      }
    }
    Dominators();
    for (size_t k = start; k < end; ++k) {
      const auto& inst = m->insts[k];
      if (inst.op == op::STORE) stores[Base(inst.a[0])].push_back(k);
    }
  }

  void Dominators() {
    std::unordered_map<uint32_t, std::vector<uint32_t>> pred;
    for (uint32_t b : block_order) pred[b];
    for (uint32_t b : block_order) {
      if (!reachable.contains(b)) continue;  // an unreachable predecessor dominates nothing
      for (uint32_t s : succ[b]) pred[s].push_back(b);
    }
    std::set<uint32_t> everything(block_order.begin(), block_order.end());
    for (uint32_t b : block_order) dom[b] = everything;
    dom[entry] = {entry};
    bool changed = true;
    while (changed) {
      changed = false;
      for (uint32_t b : block_order) {
        if (b == entry) continue;
        std::optional<std::set<uint32_t>> acc;
        for (uint32_t p : pred[b]) {
          if (!dom.contains(p)) continue;
          if (!acc) {
            acc = dom[p];
          } else {
            std::set<uint32_t> inter;
            std::ranges::set_intersection(*acc, dom[p], std::inserter(inter, inter.begin()));
            acc = std::move(inter);
          }
        }
        std::set<uint32_t> next = acc ? *acc : std::set<uint32_t>{};
        next.insert(b);
        if (next != dom[b]) {
          dom[b] = std::move(next);
          changed = true;
        }
      }
    }
  }

  [[nodiscard]] bool Dominates(size_t k_def, size_t k_use) const {
    auto bd = block_of.find(k_def);
    if (bd == block_of.end()) return k_def < start || (k_def > start && k_def < end);  // globals, parameters
    auto bu = block_of.find(k_use);
    if (bu == block_of.end()) return false;
    if (bd->second == bu->second) return k_def < k_use;
    return dom.at(bu->second).contains(bd->second);
  }

  [[nodiscard]] uint32_t Base(uint32_t ptr) const {
    for (int i = 0; i < 64; ++i) {
      const auto* d = m->Def(ptr);
      if (d == nullptr || (d->op != op::AC && d->op != op::IBAC)) return ptr;
      ptr = d->a[2];
    }
    return ptr;
  }

  // Value stored to a Function variable that the Load at k_load reads, or 0.
  [[nodiscard]] uint32_t Reaching(size_t k_load) const {
    const auto& a = m->insts[k_load].a;
    const uint32_t var = a[2];
    if (m->VarClass(var) != SC_FUNCTION) return 0;
    const auto& blk = blocks.at(block_of.at(k_load));
    for (auto it = blk.rbegin(); it != blk.rend(); ++it) {
      if (*it >= k_load) continue;
      const auto& s = m->insts[*it];
      if (s.op == op::STORE && Base(s.a[0]) == var) return s.a[0] == var ? s.a[1] : 0u;
    }
    auto st = stores.find(var);
    if (st != stores.end() && st->second.size() == 1 && m->insts[st->second[0]].a[0] == var
        && Dominates(st->second[0], k_load)) {
      return m->insts[st->second[0]].a[1];
    }
    return 0;
  }
};

struct PostClamp {
  std::string kind;  // "standard" or "extra_stage"
  std::map<std::string, int> ops;
  size_t registers = 0;
};

struct Recognition {
  uint32_t block = 0;
  std::array<uint32_t, 3> clamp_ids{};
  std::array<uint32_t, 3> graded_loads{};
  uint32_t chroma_load = 0, y_load = 0;
  size_t k3 = 0;
  uint32_t ys = 0;
  std::optional<uint32_t> lut;
  std::optional<std::array<uint32_t, 3>> identity;  // the curve before a saturation grade
  std::array<uint32_t, 3> exposed{};
  Module::UboKey ubo{};
  uint32_t ubo_rows = 0, vec4 = 0, float_type = 0, ubo_array_type = 0;
  PostClamp post;
  std::vector<size_t> moves;
  uint32_t output = 0;
  std::string family;
};

// Linear combination: leaf -> weight plus a constant term.
struct Linear {
  std::map<uint32_t, double> terms;
  double constant = 0.0;
  bool has_constant = false;
};

class Recognizer {
 public:
  explicit Recognizer(const Module& m) : m_(m) {
    std::vector<const Inst*> entries;
    for (const auto& inst : m.insts) {
      if (inst.op == op::ENTRY) entries.push_back(&inst);
    }
    if (entries.size() != 1 || entries[0]->a.empty() || entries[0]->a[0] != 4) Fail("E_ENTRY", "expected one Fragment entry point");
    main_ = entries[0]->a[1];
    if (m.glsl == 0) Fail("E_ENTRY", "no GLSL.std.450 import");
    if (m.functions.size() != 1) Fail("E_FUNCTIONS", std::to_string(m.functions.size()) + " functions; only single-function modules are handled");
    auto [start, end] = m.functions[0];
    if (m.insts[start].a[1] != main_) Fail("E_ENTRY", "entry function mismatch");
    for (size_t k = start; k < end; ++k) {
      if (m.insts[k].op == op::CALL) Fail("E_FUNCTIONS", "function calls");
    }
    fn_.emplace(m, start, end);
  }

  Recognition Run();

 private:
  const Module& m_;
  uint32_t main_ = 0;
  std::optional<Function> fn_;
  Module::UboKey ubo_{};

  [[nodiscard]] uint32_t Val(uint32_t id) const {
    for (int i = 0; i < 64; ++i) {
      const auto* d = m_.Def(id);
      if (d == nullptr) return id;
      if (d->op == op::LOAD && m_.VarClass(d->a[2]) == SC_FUNCTION) {
        const uint32_t v = fn_->Reaching(m_.where.at(id));
        if (v == 0) return id;
        id = v;
      } else if (d->op == op::COPY) {
        id = d->a[2];
      } else {
        return id;
      }
    }
    return id;
  }

  [[nodiscard]] std::optional<uint32_t> Negated(uint32_t id) const {
    id = Val(id);
    const auto* d = m_.Def(id);
    if (d == nullptr) return std::nullopt;
    if (d->op == op::FNEG) return d->a[2];
    if (d->op == op::BITCAST) {
      const auto* i = m_.Def(Val(d->a[2]));
      if (i != nullptr && i->op == op::XOR && i->a.size() == 4) {
        for (auto [x, y] : {std::pair{i->a[2], i->a[3]}, std::pair{i->a[3], i->a[2]}}) {
          if (m_.IntConst(y) == 0x80000000u) {
            const auto* b = m_.Def(Val(x));
            if (b != nullptr && b->op == op::BITCAST) return b->a[2];
          }
        }
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<uint32_t> Absolute(uint32_t id) const {
    id = Val(id);
    if (auto e = m_.GetExt(id, glsl::FABS)) return e->operands[0];
    const auto* d = m_.Def(id);
    if (d != nullptr && d->op == op::BITCAST) {
      const auto* i = m_.Def(Val(d->a[2]));
      if (i != nullptr && i->op == op::AND && i->a.size() == 4) {
        for (auto [x, y] : {std::pair{i->a[2], i->a[3]}, std::pair{i->a[3], i->a[2]}}) {
          if (m_.IntConst(y) == 0x7FFFFFFFu) {
            const auto* b = m_.Def(Val(x));
            if (b != nullptr && b->op == op::BITCAST) return b->a[2];
          }
        }
      }
    }
    return std::nullopt;
  }

  static Linear Leaf(uint32_t id) {
    Linear l;
    l.terms[id] = 1.0;
    return l;
  }
  static Linear Scale(Linear l, double s) {
    for (auto& [k, v] : l.terms) v *= s;
    l.constant *= s;
    return l;
  }
  static void Add(Linear* l, const Linear& r, double sign) {
    for (const auto& [k, v] : r.terms) l->terms[k] += sign * v;
    if (r.has_constant) {
      l->constant += sign * r.constant;
      l->has_constant = true;
    }
  }

  [[nodiscard]] std::optional<Linear> Lin(uint32_t id, int depth, const std::set<uint32_t>& stop) const {
    if (depth > 16) return std::nullopt;
    id = Val(id);
    if (stop.contains(id)) return Leaf(id);
    auto c = m_.constants.find(id);
    if (c != m_.constants.end()) {
      const auto* t = m_.Type(c->second.first);
      if (t != nullptr && t->op == op::TYPE_FLOAT) {
        Linear l;
        l.constant = F32(c->second.second);
        l.has_constant = true;
        return l;
      }
    }
    if (auto n = Negated(id)) {
      auto inner = Lin(*n, depth + 1, stop);
      if (!inner) return std::nullopt;
      return Scale(*inner, -1.0);
    }
    const auto* d = m_.Def(id);
    auto scaled = [&](uint32_t x, uint32_t y) -> std::optional<Linear> {
      auto yc = m_.constants.find(y);
      if (yc == m_.constants.end()) return std::nullopt;
      auto inner = Lin(x, depth + 1, stop);
      if (!inner) return std::nullopt;
      return Scale(*inner, F32(yc->second.second));
    };
    if (d != nullptr && (d->op == op::FADD || d->op == op::FSUB)) {
      auto l = Lin(d->a[2], depth + 1, stop);
      auto r = Lin(d->a[3], depth + 1, stop);
      if (!l || !r) return std::nullopt;
      Add(&*l, *r, d->op == op::FADD ? 1.0 : -1.0);
      return l;
    }
    if (d != nullptr && d->op == op::FMUL) {
      if (auto s = scaled(d->a[2], Val(d->a[3]))) return s;
      if (auto s = scaled(d->a[3], Val(d->a[2]))) return s;
      return Leaf(id);
    }
    if (auto e = m_.GetExt(id, glsl::FMA)) {
      const uint32_t x = e->operands[0], y = e->operands[1], z = e->operands[2];
      auto prod = scaled(x, Val(y));
      if (!prod) prod = scaled(y, Val(x));
      auto rest = Lin(z, depth + 1, stop);
      if (!prod || !rest) return std::nullopt;
      Add(&*prod, *rest, 1.0);
      return prod;
    }
    return Leaf(id);
  }

  // (x_c, leaves) if gid is min(max(0, fma(x_c - Y, U[2].w, Y)), C) with C >= 1 a constant,
  // Y = the GAME_LUMA-weighted sum of three values (leaves, in GAME_LUMA order) and x_c one of
  // them (dkb_composite_struct.py saturation_grade).
  [[nodiscard]] std::optional<std::pair<uint32_t, std::array<uint32_t, 3>>> SaturationGrade(uint32_t gid) const {
    auto mn = m_.GetExt(gid, glsl::FMIN);
    if (!mn || mn->operands.size() != 2) return std::nullopt;
    auto big = [&](uint32_t id) {
      auto c = m_.constants.find(Val(id));
      return c != m_.constants.end() && F32(c->second.second) >= 1.0;
    };
    uint32_t x = mn->operands[0], cap = mn->operands[1];
    if (!big(cap)) {
      std::swap(x, cap);
      if (!big(cap)) return std::nullopt;
    }
    auto mx = m_.GetExt(Val(x), glsl::FMAX);
    if (!mx || mx->operands.size() != 2) return std::nullopt;
    uint32_t lo = mx->operands[0], inner = mx->operands[1];
    if (!m_.IsFloatConst(lo, 0.0)) {
      std::swap(lo, inner);
      if (!m_.IsFloatConst(lo, 0.0)) return std::nullopt;
    }
    auto fma = m_.GetExt(Val(inner), glsl::FMA);
    if (!fma || fma->operands.size() != 3) return std::nullopt;
    uint32_t a = fma->operands[0], sat = fma->operands[1], b = fma->operands[2];
    if (!m_.LoadsUniform(sat, 2, 3, ubo_)) {
      std::swap(a, sat);
      if (!m_.LoadsUniform(sat, 2, 3, ubo_)) return std::nullopt;
    }
    const auto y = Lin(b, 0, {});
    const auto d = Lin(a, 0, {});
    if (!y || !d || std::abs(y->constant) > 1e-12 || std::abs(d->constant) > 1e-12) return std::nullopt;
    if (y->terms.size() != 3) return std::nullopt;
    std::array<uint32_t, 3> leaves{};
    std::array<bool, 3> have{};
    for (const auto& [leaf, w] : y->terms) {
      int hit = -1, hits = 0;
      for (int i = 0; i < 3; ++i) {
        if (std::abs(w - GAME_LUMA[i]) < 1e-7) {
          hit = i;
          ++hits;
        }
      }
      if (hits != 1 || have[hit]) return std::nullopt;
      leaves[hit] = leaf;
      have[hit] = true;
    }
    if (d->terms.size() != y->terms.size()) return std::nullopt;
    for (const auto& [leaf, w] : d->terms) {
      if (!y->terms.contains(leaf)) return std::nullopt;
    }
    std::vector<uint32_t> kept;
    for (const auto& [leaf, w] : d->terms) {
      if (std::abs(w - (1.0 - y->terms.at(leaf))) < 1e-6) kept.push_back(leaf);
    }
    if (kept.size() != 1) return std::nullopt;
    for (const auto& [leaf, w] : d->terms) {
      if (leaf != kept[0] && std::abs(w + y->terms.at(leaf)) > 1e-6) return std::nullopt;
    }
    return std::pair{kept[0], leaves};
  }

  [[nodiscard]] bool Is3D(uint32_t sampled) const {
    const auto* d = m_.Def(Val(sampled));
    if (d == nullptr || d->a.empty()) return false;
    const auto* st = m_.Type(d->a[0]);
    if (st != nullptr && st->op == op::TYPE_SAMPLED_IMAGE) st = m_.Type(st->rest[0]);
    return st != nullptr && st->op == op::TYPE_IMAGE && st->rest.size() >= 4 && st->rest[1] == 2 && st->rest[3] == 0;
  }

  [[nodiscard]] bool Flows(const std::array<uint32_t, 3>& sources, const std::set<uint32_t>& targets) const {
    std::unordered_set<uint32_t> taint(sources.begin(), sources.end());
    std::unordered_set<uint32_t> tvars;
    for (int pass = 0; pass < 4; ++pass) {
      const auto before = std::pair{taint.size(), tvars.size()};
      for (size_t k = fn_->start; k < fn_->end; ++k) {
        const auto& [o, a] = m_.insts[k];
        if (o == op::STORE) {
          if (taint.contains(a[1])) tvars.insert(fn_->Base(a[0]));
          continue;
        }
        if (o == op::LOAD) {
          if (tvars.contains(fn_->Base(a[2]))) taint.insert(a[1]);
          continue;
        }
        const uint32_t r = ResultOf(o, a);
        if (r != 0 && o != op::LABEL && a.size() > 2
            && std::any_of(a.begin() + 2, a.end(), [&](uint32_t x) { return taint.contains(x); })) {
          taint.insert(r);
        }
      }
      if (std::pair{taint.size(), tvars.size()} == before) break;
    }
    return std::ranges::all_of(targets, [&](uint32_t t) { return taint.contains(t); });
  }

  void CheckUbo(Recognition* r) const {
    auto [var, member] = ubo_;
    const auto* v = m_.Def(var);
    const auto* st = m_.Type(m_.Pointee(v->a[0]));
    if (st == nullptr || st->op != op::TYPE_STRUCT || member >= st->rest.size()) Fail("E_UBO", "uniform block is not a struct");
    const auto* arr = m_.Type(st->rest[member]);
    if (arr == nullptr || arr->op != op::TYPE_ARRAY) Fail("E_UBO", "uniform member is not an array");
    const auto* vec = m_.Type(arr->rest[0]);
    auto n = m_.IntConst(arr->rest[1]);
    const auto* comp = vec != nullptr ? m_.Type(vec->rest[0]) : nullptr;
    if (vec == nullptr || vec->op != op::TYPE_VECTOR || vec->rest[1] != 4 || comp == nullptr || comp->op != op::TYPE_FLOAT) {
      Fail("E_UBO", "uniform member is not an array of vec4");
    }
    if (!n || *n < 8) Fail("E_UBO", "uniform array is shorter than 8 rows");
    r->ubo_rows = 8;
    r->vec4 = arr->rest[0];
    r->float_type = vec->rest[0];
    r->ubo_array_type = st->rest[member];
  }

  [[nodiscard]] uint32_t OutputVar() const {
    std::vector<uint32_t> loc0;
    for (uint32_t id : m_.def_order) {
      const auto* d = m_.Def(id);
      if (d->op != op::VARIABLE || d->a[2] != SC_OUTPUT) continue;
      auto dec = m_.decorations.find(id);
      if (dec == m_.decorations.end()) continue;
      auto loc = dec->second.find(DEC_LOCATION);
      if (loc != dec->second.end() && loc->second == std::vector<uint32_t>{0}) loc0.push_back(id);
    }
    if (loc0.size() != 1) Fail("E_OUTPUT", "no single colour output at location 0");
    const auto* t = m_.Type(m_.Pointee(m_.Def(loc0[0])->a[0]));
    if (t == nullptr || t->op != op::TYPE_VECTOR || t->rest[1] != 4) Fail("E_OUTPUT", "colour output is not a vec4");
    return loc0[0];
  }

  [[nodiscard]] std::string Classify(uint32_t o, const std::vector<uint32_t>& a, const std::vector<uint32_t>& t,
                                     const std::unordered_set<uint32_t>& taint,
                                     std::unordered_set<uint32_t>* guard_tests) const {
    if (o == op::BITCAST) return "Bitcast";
    if (o == op::COPY) return "Copy";
    if (o == op::CONSTRUCT) return "Construct";
    if (o == op::EXTRACT) return "Extract";
    if (o == op::INSERT) return "Insert";
    if (o == op::SHUFFLE) return "Shuffle";
    if (o == op::PHI) return "Phi";
    if (o == op::FNEG) return "FNeg";
    if (o == op::FADD) return "FAdd";
    if (o == op::FSUB) return "FSub";
    if (o == op::FMUL) return t.size() == 1 ? "FMul" : "!FMul(C*C)";
    if (o == op::XOR && std::any_of(a.begin() + 2, a.end(), [&](uint32_t x) { return m_.IntConst(x) == 0x80000000u; })) return "negate";
    if (o == op::AND && std::any_of(a.begin() + 2, a.end(), [&](uint32_t x) { return m_.IntConst(x) == 0x7FFFFFFFu; })) {
      std::vector<const Inst*> users;
      for (const auto& inst : m_.insts) {
        if (inst.op == op::UGT && inst.a.size() >= 4 && (inst.a[2] == a[1] || inst.a[3] == a[1])) users.push_back(&inst);
      }
      const bool all_guard = !users.empty() && std::ranges::all_of(users, [&](const Inst* u) {
        return m_.IntConst(u->a[2]) == 0x7F800000u || m_.IntConst(u->a[3]) == 0x7F800000u;
      });
      if (all_guard) {
        for (const auto* u : users) guard_tests->insert(u->a[1]);
        return "nan_guard";
      }
      return "!And";
    }
    if (o == op::UGT && guard_tests->contains(a[1])) return "nan_guard";
    if (o == op::ISNAN) return "nan_guard";
    if (o == op::SELECT) {
      if (guard_tests->contains(a[2]) || m_.OpOf(a[2]) == op::ISNAN) return "nan_guard";
      return taint.contains(a[2]) ? "!Select(cond on C)" : "Select";
    }
    if (o == op::EXT) {
      const uint32_t inst = a[3];
      if (inst == glsl::FMA) return (taint.contains(a[4]) && taint.contains(a[5])) ? "!fma(C*C)" : "fma";
      if (inst == 46) return taint.contains(a[6]) ? "!mix(weight on C)" : "mix";
      return "!glsl" + std::to_string(inst);
    }
    return "!op" + std::to_string(o);
  }

  [[nodiscard]] PostClamp PostClampAudit(const std::array<uint32_t, 3>& cids) const {
    std::unordered_set<uint32_t> taint(cids.begin(), cids.end()), tvars, guard_tests;
    std::unordered_set<uint32_t> outputs;
    for (uint32_t id : m_.def_order) {
      const auto* d = m_.Def(id);
      if (d->op == op::VARIABLE && d->a[2] == SC_OUTPUT) outputs.insert(id);
    }
    std::map<std::string, int> ops, unsafe;
    std::set<std::pair<uint32_t, std::vector<int64_t>>> to_output;
    size_t start = SIZE_MAX;
    for (uint32_t c : cids) start = std::min(start, m_.where.at(c));
    for (int pass = 0; pass < 3; ++pass) {
      for (size_t k = start; k < fn_->end; ++k) {
        const auto& [o, a] = m_.insts[k];
        if (o == op::STORE) {
          const uint32_t base = fn_->Base(a[0]);
          if (taint.contains(a[1])) {
            if (outputs.contains(base)) {
              std::vector<int64_t> idx;
              const auto* p = m_.Def(a[0]);
              if (p != nullptr && (p->op == op::AC || p->op == op::IBAC)) {
                for (size_t i = 3; i < p->a.size(); ++i) {
                  auto v = m_.IntConst(p->a[i]);
                  idx.push_back(v ? static_cast<int64_t>(*v) : -1);
                }
              }
              to_output.insert({base, idx});
            } else {
              tvars.insert(base);
            }
          }
          continue;
        }
        if (o == op::LOAD) {
          if (tvars.contains(fn_->Base(a[2]))) taint.insert(a[1]);
          continue;
        }
        const uint32_t r = ResultOf(o, a);
        if (r == 0 || std::ranges::find(cids, r) != cids.end() || taint.contains(r)) continue;
        const size_t first = o == op::EXT ? 4 : 2;
        std::vector<uint32_t> t;
        for (size_t i = first; i < a.size(); ++i) {
          if (taint.contains(a[i])) t.push_back(a[i]);
        }
        if (t.empty()) continue;
        taint.insert(r);
        const std::string name = Classify(o, a, t, taint, &guard_tests);
        if (name[0] == '!') {
          unsafe[name.substr(1)]++;
        } else {
          ops[name]++;
        }
      }
    }
    if (!unsafe.empty()) {
      std::string list;
      for (const auto& [k, v] : unsafe) list += (list.empty() ? "" : ", ") + k;
      Fail("E_POSTCLAMP", "non-linear use of the clamped colour: " + list);
    }
    for (const auto& [base, idx] : to_output) {
      if (idx.size() != 1 || idx[0] < 0 || idx[0] > 2) Fail("E_POSTCLAMP", "the clamped colour reaches an output other than rgb");
    }
    PostClamp pc;
    bool standard = tvars.empty();
    for (const auto& [k, v] : ops) {
      if (k != "nan_guard" && k != "Bitcast") standard = false;
    }
    pc.kind = standard ? "standard" : "extra_stage";
    pc.ops = ops;
    pc.registers = tvars.size();
    return pc;
  }

  [[nodiscard]] std::vector<size_t> PlanMoves(const Recognition& r) const {
    const auto& blk = fn_->blocks.at(r.block);
    std::vector<size_t> span;
    for (size_t k : blk) {
      if (k <= r.k3 && m_.insts[k].op != op::LABEL && m_.insts[k].op != op::PHI) span.push_back(k);
    }
    std::unordered_set<uint32_t> cset(r.clamp_ids.begin(), r.clamp_ids.end());
    std::unordered_set<uint32_t> dep = cset;
    std::vector<size_t> moved;
    for (size_t k : span) {
      const auto& [o, a] = m_.insts[k];
      if (o == op::STORE) {
        if (dep.contains(a[1])) moved.push_back(k);
        continue;
      }
      const uint32_t res = ResultOf(o, a);
      if (cset.contains(res)) continue;
      const size_t first = o == op::EXT ? 4 : 2;
      bool uses = false;
      for (size_t i = first; i < a.size(); ++i) uses = uses || dep.contains(a[i]);
      if (res != 0 && uses) {
        if (o == op::LOAD || o == op::CALL || IsTerminator(o)) Fail("E_REORDER", "op " + std::to_string(o) + " reads a clamp result");
        dep.insert(res);
        moved.push_back(k);
      }
    }
    std::unordered_set<size_t> mset(moved.begin(), moved.end());
    for (size_t k : moved) {
      const auto& [o, a] = m_.insts[k];
      if (o != op::STORE) continue;
      const uint32_t base = fn_->Base(a[0]);
      for (size_t k2 : span) {
        if (k2 <= k || mset.contains(k2)) continue;
        const auto& i2 = m_.insts[k2];
        if (i2.op == op::LOAD && fn_->Base(i2.a[2]) == base) Fail("E_REORDER", "a moved store is read before the third clamp");
        if (i2.op == op::STORE && fn_->Base(i2.a[0]) == base) Fail("E_REORDER", "a moved store would pass another store to the same variable");
      }
    }
    return moved;
  }
};

inline Recognition Recognizer::Run() {
  const auto& m = m_;
  const auto& fn = *fn_;
  Recognition r;

  // 1. The three peak clamps.
  struct ClampHit {
    size_t k;
    uint32_t id;
    Module::UboKey ubo;
    uint32_t prod;
  };
  std::vector<ClampHit> clamps;
  for (size_t k = fn.start; k < fn.end; ++k) {
    const auto& [o, a] = m.insts[k];
    if (o != op::EXT || a.size() < 6 || a[2] != m.glsl || (a[3] != glsl::FMIN && a[3] != glsl::NMIN)) continue;
    for (auto [x, lim] : {std::pair{a[4], a[5]}, std::pair{a[5], a[4]}}) {
      auto u = m.LoadsUniform(lim, 4, 0);
      auto e = m.GetExt(x);
      if (!u || !e || (e->inst != glsl::FMAX && e->inst != glsl::NMAX) || e->operands.size() < 2) continue;
      uint32_t zero = e->operands[0], prod = e->operands[1];
      if (!m.IsFloatConst(zero, 0.0)) std::swap(zero, prod);
      if (!m.IsFloatConst(zero, 0.0) || m.OpOf(prod) != op::FMUL) continue;
      clamps.push_back({k, a[1], *u, prod});
      break;
    }
  }
  if (clamps.size() != 3) Fail("E_CLAMPS", std::to_string(clamps.size()) + " peak clamps");
  if (!(clamps[0].ubo == clamps[1].ubo && clamps[1].ubo == clamps[2].ubo)) Fail("E_CLAMPS", "clamps read different uniform blocks");
  ubo_ = clamps[0].ubo;
  const uint32_t b0 = fn.block_of.at(clamps[0].k);
  if (fn.block_of.at(clamps[1].k) != b0 || fn.block_of.at(clamps[2].k) != b0) Fail("E_CLAMP_BLOCK", "clamps are in different blocks");
  r.block = b0;

  // 2. C_i = clamp(fma(chroma, fma(G_i, invL, -1), 1) * Y).
  struct Chain {
    uint32_t cid;
    size_t k;
    uint32_t chroma, g, inv, y;
  };
  std::vector<Chain> chain;
  for (const auto& c : clamps) {
    const auto& pa = m.Def(c.prod)->a;
    std::optional<Chain> got;
    for (auto [a_id, y_id] : {std::pair{pa[2], pa[3]}, std::pair{pa[3], pa[2]}}) {
      auto e = m.GetExt(Val(a_id), glsl::FMA);
      if (!e || !m.IsFloatConst(e->operands[2], 1.0)) continue;
      const uint32_t chroma = e->operands[0], inner = e->operands[1];
      auto ei = m.GetExt(Val(inner), glsl::FMA);
      if (!ei || !m.IsFloatConst(ei->operands[2], -1.0)) continue;
      got = Chain{c.id, c.k, chroma, ei->operands[0], ei->operands[1], y_id};
      break;
    }
    if (!got) Fail("E_CLAMP_FORM", "clamp %" + std::to_string(c.id) + " is not Y * lerp(1, G / L, chroma)");
    chain.push_back(*got);
  }
  auto distinct = [&](auto field) {
    std::set<uint32_t> s;
    for (const auto& c : chain) s.insert(Val(field(c)));
    return s.size();
  };
  if (distinct([](const Chain& c) { return c.chroma; }) != 1 || distinct([](const Chain& c) { return c.inv; }) != 1
      || distinct([](const Chain& c) { return c.y; }) != 1) {
    Fail("E_CLAMP_FORM", "channels disagree on chroma, 1/L or Y");
  }
  if (distinct([](const Chain& c) { return c.g; }) != 3) Fail("E_CLAMP_FORM", "graded channels are not distinct");

  // 3. 1/L with the game's luma weights gives each channel its index.
  {
    const uint32_t inv = Val(chain[0].inv);
    const auto* d = m.Def(inv);
    if (d == nullptr || d->op != op::FDIV || !m.IsFloatConst(d->a[2], 1.0)) Fail("E_LUMA", "1/L is not a reciprocal");
    auto lin = Lin(d->a[3], 0, {});
    if (!lin || std::abs(lin->constant - 1e-10) > 1e-12 || lin->terms.size() != 3) Fail("E_LUMA", "L is not a weighted sum of three channels + 1e-10");
    std::map<uint32_t, int> channel;
    for (const auto& [leaf, w] : lin->terms) {
      int hit = -1, hits = 0;
      for (int i = 0; i < 3; ++i) {
        if (std::abs(w - GAME_LUMA[i]) < 1e-7) {
          hit = i;
          hits++;
        }
      }
      if (hits != 1) Fail("E_LUMA", "unexpected luma weight " + std::to_string(w));
      channel[leaf] = hit;
    }
    std::array<const Chain*, 3> ordered{};
    for (const auto& c : chain) {
      auto it = channel.find(Val(c.g));
      if (it == channel.end() || ordered[it->second] != nullptr) Fail("E_LUMA", "graded channel not in L");
      ordered[it->second] = &c;
    }
    for (int i = 0; i < 3; ++i) {
      r.clamp_ids[i] = ordered[i]->cid;
      r.graded_loads[i] = ordered[i]->g;
      r.k3 = std::max(r.k3, ordered[i]->k);
    }
    r.chroma_load = ordered[0]->chroma;
    r.y_load = ordered[0]->y;
  }

  // 4./5. chroma = exp2(log2(|1 - Ys + Y|) * U[4].w), Ys being the knee-tested brightness.
  std::set<uint32_t> knees;
  for (size_t k = fn.start; k < fn.end; ++k) {
    const auto& [o, a] = m.insts[k];
    if ((o == op::FGE || o == op::FUGE) && m.LoadsUniform(a[3], 4, 1, ubo_)) knees.insert(Val(a[2]));
  }
  if (knees.empty()) Fail("E_KNEE", "no Ys >= U[4].y comparison");
  {
    auto ch = m.GetExt(Val(r.chroma_load), glsl::EXP2);
    const auto* mul = ch ? m.Def(ch->operands[0]) : nullptr;
    if (!ch || mul == nullptr || mul->op != op::FMUL) Fail("E_CHROMA", "chroma is not exp2(... * U[4].w)");
    std::optional<Module::Ext> lg;
    for (auto [x, y] : {std::pair{mul->a[2], mul->a[3]}, std::pair{mul->a[3], mul->a[2]}}) {
      if (m.LoadsUniform(y, 4, 3, ubo_)) lg = m.GetExt(Val(x), glsl::LOG2);
    }
    std::optional<uint32_t> base = lg ? Absolute(lg->operands[0]) : std::nullopt;
    const uint32_t y_val = Val(r.y_load);
    std::set<uint32_t> stop = knees;
    stop.insert(y_val);
    std::optional<Linear> lin = base ? Lin(*base, 0, stop) : std::nullopt;
    if (!lin || std::abs(lin->constant - 1.0) > 1e-6) Fail("E_CHROMA", "chroma base is not 1 - Ys + Y");
    std::vector<uint32_t> ys;
    for (const auto& [leaf, w] : lin->terms) {
      if (std::abs(w + 1) < 1e-6) ys.push_back(leaf);
    }
    auto yw = lin->terms.find(y_val);
    if (lin->terms.size() != 2 || std::abs((yw == lin->terms.end() ? 0.0 : yw->second) - 1) > 1e-6 || ys.size() != 1) {
      Fail("E_CHROMA", "chroma base does not combine this Y with one Ys");
    }
    r.ys = ys[0];
    if (!knees.contains(r.ys)) Fail("E_KNEE", "the chroma's Ys is not the knee-tested brightness");
    if (!m.where.contains(r.ys) || !fn.Dominates(m.where.at(r.ys), r.k3)) Fail("E_DOMINANCE", "Ys is not available at the clamps");
  }

  // 6. Graded colour: a 3D LUT sample, the clamped SDR curve, or a saturation grade of the curve.
  std::array<uint32_t, 3> g{};
  {
    std::set<std::string> kinds;
    std::set<uint32_t> vecs;
    std::array<std::pair<uint32_t, std::array<uint32_t, 3>>, 3> sat{};
    for (int i = 0; i < 3; ++i) {
      g[i] = Val(r.graded_loads[i]);
      const auto* d = m.Def(g[i]);
      auto e = m.GetExt(g[i], glsl::FCLAMP);
      if (d != nullptr && d->op == op::EXTRACT && d->a.size() == 4 && d->a[3] == static_cast<uint32_t>(i)) {
        kinds.insert("lut");
        vecs.insert(Val(d->a[2]));
      } else if (e && e->operands.size() == 3 && m.IsFloatConst(e->operands[1], 0.0) && m.IsFloatConst(e->operands[2], 1.0)) {
        kinds.insert("curve");
      } else if (auto sg = SaturationGrade(g[i])) {
        kinds.insert("sat");
        sat[i] = *sg;
      } else {
        kinds.insert("?");
      }
    }
    if (kinds == std::set<std::string>{"lut"}) {
      if (vecs.size() != 1) Fail("E_GRADE", "graded channels come from different samples");
      uint32_t v = *vecs.begin();
      const auto* d = m.Def(v);
      if (d != nullptr && d->op == op::SHUFFLE && d->a[2] == d->a[3] && d->a.size() == 7 && d->a[4] == 0 && d->a[5] == 1 && d->a[6] == 2) {
        v = Val(d->a[2]);
        d = m.Def(v);
      }
      if (d == nullptr || (d->op != op::SAMPLE && d->op != op::SAMPLE_LOD)) Fail("E_GRADE", "graded colour is not a texture sample");
      if (!Is3D(d->a[2])) Fail("E_GRADE", "graded colour is not a 3D LUT sample");
      uint32_t coord = Val(d->a[3]);
      const auto* s = m.Def(coord);
      if (s != nullptr && s->op == op::SELECT) {
        const auto* cond = m.Def(s->a[2]);
        if (cond != nullptr && cond->op == op::ISNAN && cond->a[2] == s->a[4]) {
          coord = Val(s->a[4]);
          s = m.Def(coord);
        }
      }
      if (s == nullptr || s->op != op::CONSTRUCT || s->a.size() != 5) Fail("E_LUT", "LUT coordinate is not a vec3 construct");
      for (size_t i = 2; i < 5; ++i) {
        auto e = m.GetExt(Val(s->a[i]), glsl::FMA);
        if (!e || !m.LoadsUniform(e->operands[1], 0, 2, ubo_) || !m.LoadsUniform(e->operands[2], 0, 3, ubo_)) {
          Fail("E_LUT", "LUT coordinate is not v * U[0].z + U[0].w");
        }
      }
      if (!m.where.contains(coord) || !fn.Dominates(m.where.at(coord), r.k3)) Fail("E_DOMINANCE", "LUT coordinate is not available at the clamps");
      r.lut = coord;
    } else if (kinds == std::set<std::string>{"curve"}) {
      r.lut.reset();
    } else if (kinds == std::set<std::string>{"sat"}) {
      // The three channels grade the same three curve values, channel c keeping x_c.
      if (sat[0].second != sat[1].second || sat[0].second != sat[2].second) {
        Fail("E_GRADE", "saturation grade channels read different curve values");
      }
      std::array<uint32_t, 3> curve = {sat[0].first, sat[1].first, sat[2].first};
      std::array<uint32_t, 3> a = curve, b = sat[0].second;
      std::ranges::sort(a);
      std::ranges::sort(b);
      if (a != b || std::set<uint32_t>(curve.begin(), curve.end()).size() != 3) {
        Fail("E_GRADE", "saturation grade does not keep one curve value per channel");
      }
      for (uint32_t x : curve) {
        if (!m.where.contains(x) || !fn.Dominates(m.where.at(x), r.k3)) {
          Fail("E_DOMINANCE", "the curve before the saturation grade is not available at the clamps");
        }
      }
      r.lut.reset();
      r.identity = curve;
    } else {
      Fail("E_GRADE", "graded colour is neither a 3D LUT sample nor the clamped SDR curve");
    }
  }

  // 7. Exposed scene colour.
  {
    std::array<std::vector<uint32_t>, 3> exposed;
    for (size_t k = fn.start; k < fn.end; ++k) {
      const auto& [o, a] = m.insts[k];
      if (o != op::FMUL) continue;
      for (auto [x, y] : {std::pair{a[2], a[3]}, std::pair{a[3], a[2]}}) {
        auto lg = m.GetExt(Val(x), glsl::LOG2);
        const auto* dv = m.Def(Val(y));
        if (!lg || dv == nullptr || dv->op != op::FDIV || !m.IsFloatConst(dv->a[2], 1.0)) continue;
        const auto* pv = m.Def(Val(dv->a[3]));
        if (pv == nullptr || pv->op != op::FMUL) continue;
        for (auto [p, q] : {std::pair{pv->a[2], pv->a[3]}, std::pair{pv->a[3], pv->a[2]}}) {
          if (!m.LoadsUniform(p, 3, 3, ubo_)) continue;
          for (uint32_t c = 0; c < 3; ++c) {
            if (m.LoadsUniform(q, 3, c, ubo_)) {
              if (auto e = Absolute(lg->operands[0])) exposed[c].push_back(Val(*e));
            }
          }
        }
      }
    }
    std::string counts;
    bool bad = false;
    for (int c = 0; c < 3; ++c) {
      std::set<uint32_t> s(exposed[c].begin(), exposed[c].end());
      counts += (c ? "," : "") + std::to_string(s.size());
      bad = bad || s.size() != 1;
    }
    if (bad) Fail("E_EXPOSED", "exposed colour not found exactly once per channel: " + counts);
    for (int c = 0; c < 3; ++c) {
      r.exposed[c] = exposed[c][0];
      if (!m.where.contains(r.exposed[c]) || !fn.Dominates(m.where.at(r.exposed[c]), r.k3)) {
        Fail("E_DOMINANCE", "exposed colour is not available at the clamps");
      }
    }
    std::set<uint32_t> targets(g.begin(), g.end());
    targets.insert(r.ys);
    if (r.lut) targets.insert(*r.lut);
    if (!Flows(r.exposed, targets)) Fail("E_FLOW", "exposed colour does not feed the graded colour and Ys");
  }

  // 8. Rows the correction reads.
  {
    std::vector<std::pair<uint32_t, uint32_t>> rows = {{1, 2}, {1, 3}, {3, 3}};
    if (r.lut) {
      rows.emplace_back(0, 2);
      rows.emplace_back(0, 3);
    }
    if (r.identity) rows.emplace_back(2, 3);
    for (auto [row, comp] : rows) {
      bool found = false;
      for (uint32_t id : m.def_order) {
        if (m.LoadsUniform(id, row, comp, ubo_)) {
          found = true;
          break;
        }
      }
      if (!found) Fail("E_UNIFORMS", "U[" + std::to_string(row) + "]." + "xyzw"[comp] + " is not read");
    }
  }
  CheckUbo(&r);
  r.ubo = ubo_;
  r.post = PostClampAudit(r.clamp_ids);
  r.moves = PlanMoves(r);
  r.output = OutputVar();
  for (uint32_t b : fn.block_order) {
    const auto& last = m.insts[fn.blocks.at(b).back()];
    if ((last.op == op::RETURN || last.op == op::RETURN_VALUE) && !fn.dom.at(b).contains(r.block)) {
      Fail("E_DOMINANCE", "a return is reachable without passing the clamps");
    }
  }
  r.family = r.post.kind == "extra_stage" ? "extra_stage" : "standard";
  return r;
}

inline Recognition Recognize(const Module& m) { return Recognizer(m).Run(); }

// The instrumented module; byte-identical to the prototype's Rewriter.
inline Module Rewrite(const Module& m, const Recognition& r) {
  uint32_t next = m.bound();
  auto fresh = [&]() { return next++; };
  std::vector<Inst> types_new, consts_new, vars_new, names_new;

  auto find_type = [&](uint32_t o, const std::vector<uint32_t>& rest) -> uint32_t {
    for (uint32_t id : m.type_order) {
      const auto& t = m.types.at(id);
      if (t.op == o && t.rest == rest) return id;
    }
    return 0;
  };
  auto type_ = [&](uint32_t o, std::vector<uint32_t> rest) -> uint32_t {
    uint32_t t = o == op::TYPE_ARRAY ? 0u : find_type(o, rest);
    if (t == 0) {
      for (const auto& nt : types_new) {
        if (nt.op == o && std::equal(nt.a.begin() + 1, nt.a.end(), rest.begin(), rest.end())) return nt.a[0];
      }
      t = fresh();
      std::vector<uint32_t> a = {t};
      a.insert(a.end(), rest.begin(), rest.end());
      types_new.push_back({o, a});
    }
    return t;
  };
  const uint32_t f = r.float_type, v4 = r.vec4;
  const uint32_t v3 = type_(op::TYPE_VECTOR, {f, 3});
  const uint32_t void_t = type_(op::TYPE_VOID, {});
  const uint32_t fnty = type_(op::TYPE_FUNCTION, {void_t});
  const uint32_t p_f = type_(op::TYPE_POINTER, {SC_PRIVATE, f});
  const uint32_t p_v3 = type_(op::TYPE_POINTER, {SC_PRIVATE, v3});
  const uint32_t p_v4 = type_(op::TYPE_POINTER, {SC_PRIVATE, v4});
  uint32_t int_t = 0;
  for (uint32_t id : m.type_order) {
    const auto& t = m.types.at(id);
    if (t.op == op::TYPE_INT && t.rest == std::vector<uint32_t>{32, 1}) {
      int_t = id;
      break;
    }
  }
  if (int_t == 0) int_t = type_(op::TYPE_INT, {32, 1});
  std::map<uint32_t, uint32_t> iconsts, fconsts;
  auto iconst = [&](uint32_t v) -> uint32_t {
    for (uint32_t id : m.constant_order) {
      const auto& [t, b] = m.constants.at(id);
      if (t == int_t && b == v) return id;
    }
    auto it = iconsts.find(v);
    if (it != iconsts.end()) return it->second;
    const uint32_t id = fresh();
    iconsts[v] = id;
    consts_new.push_back({op::CONSTANT, {int_t, id, v}});
    return id;
  };
  auto fconst = [&](float value) -> uint32_t {
    const uint32_t bits = Bits(value);
    for (uint32_t id : m.constant_order) {
      const auto& [t, b] = m.constants.at(id);
      if (t == f && b == bits) return id;
    }
    auto it = fconsts.find(bits);
    if (it != fconsts.end()) return it->second;
    const uint32_t id = fresh();
    fconsts[bits] = id;
    consts_new.push_back({op::CONSTANT, {f, id, bits}});
    return id;
  };
  const uint32_t rows = r.ubo_rows;
  const uint32_t rows_c = iconst(rows);
  const uint32_t arr_t = type_(op::TYPE_ARRAY, {v4, rows_c});
  const uint32_t p_arr = type_(op::TYPE_POINTER, {SC_PRIVATE, arr_t});
  const uint32_t p_uv4 = type_(op::TYPE_POINTER, {SC_UNIFORM, v4});

  std::map<std::string, uint32_t> g;
  std::vector<std::string> g_order;
  auto var = [&](uint32_t ptr, const std::string& name) {
    const uint32_t id = fresh();
    vars_new.push_back({op::VARIABLE, {ptr, id, SC_PRIVATE}});
    std::vector<uint32_t> a = {id};
    auto s = EncodeString(name);
    a.insert(a.end(), s.begin(), s.end());
    names_new.push_back({op::NAME, a});
    g[name] = id;
    g_order.push_back(name);
  };
  var(p_v3, "dkb_c");
  var(p_v3, "dkb_exposed");
  var(p_v3, "dkb_lut_uvw");
  var(p_v3, "dkb_graded");
  var(p_f, "dkb_ys");
  var(p_f, "dkb_chroma");
  var(p_arr, "dkb_c3");
  var(p_f, "dkb_has_lut");
  std::map<std::string, uint32_t> hooks;
  for (const char* name : {"dkb_tonemap_hook", "dkb_output_hook"}) {
    hooks[name] = fresh();
    std::vector<uint32_t> a = {hooks[name]};
    auto s = EncodeString(name);
    a.insert(a.end(), s.begin(), s.end());
    names_new.push_back({op::NAME, a});
  }

  const auto [ubo_var, ubo_member] = r.ubo;
  std::vector<Inst> at_k3;
  const uint32_t cvec = fresh();
  at_k3.push_back({op::CONSTRUCT, {v3, cvec, r.clamp_ids[0], r.clamp_ids[1], r.clamp_ids[2]}});
  at_k3.push_back({op::STORE, {g["dkb_c"], cvec}});
  const uint32_t evec = fresh();
  at_k3.push_back({op::CONSTRUCT, {v3, evec, r.exposed[0], r.exposed[1], r.exposed[2]}});
  at_k3.push_back({op::STORE, {g["dkb_exposed"], evec}});
  if (r.lut) at_k3.push_back({op::STORE, {g["dkb_lut_uvw"], *r.lut}});
  if (r.identity) {
    const uint32_t ivec = fresh();  // the curve before the saturation grade
    at_k3.push_back({op::CONSTRUCT, {v3, ivec, (*r.identity)[0], (*r.identity)[1], (*r.identity)[2]}});
    at_k3.push_back({op::STORE, {g["dkb_lut_uvw"], ivec}});
  }
  const uint32_t gvec = fresh();
  at_k3.push_back({op::CONSTRUCT, {v3, gvec, r.graded_loads[0], r.graded_loads[1], r.graded_loads[2]}});
  at_k3.push_back({op::STORE, {g["dkb_graded"], gvec}});
  at_k3.push_back({op::STORE, {g["dkb_ys"], r.ys}});
  at_k3.push_back({op::STORE, {g["dkb_chroma"], r.chroma_load}});
  // 0 the clamped curve, 1 a 3D LUT, 2 a saturation grade (dkb_lut_uvw: the curve before it).
  at_k3.push_back({op::STORE, {g["dkb_has_lut"], fconst(r.lut ? 1.f : r.identity ? 2.f : 0.f)}});
  for (uint32_t row = 0; row < rows; ++row) {
    const uint32_t src = fresh(), val = fresh(), dst = fresh();
    const uint32_t cm = iconst(ubo_member);
    const uint32_t cr = iconst(row);
    at_k3.push_back({op::AC, {p_uv4, src, ubo_var, cm, cr}});
    at_k3.push_back({op::LOAD, {v4, val, src}});
    at_k3.push_back({op::AC, {p_v4, dst, g["dkb_c3"], iconst(row)}});
    at_k3.push_back({op::STORE, {dst, val}});
  }
  at_k3.push_back({op::CALL, {void_t, fresh(), hooks["dkb_tonemap_hook"]}});
  const uint32_t back = fresh();
  at_k3.push_back({op::LOAD, {v3, back, g["dkb_c"]}});
  std::unordered_map<uint32_t, uint32_t> replace;
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t nid = fresh();
    at_k3.push_back({op::EXTRACT, {f, nid, back, i}});
    replace[r.clamp_ids[i]] = nid;
  }

  auto subst = [&](uint32_t o, std::vector<uint32_t> a) {
    auto rp = [&](uint32_t x) {
      auto it = replace.find(x);
      return it == replace.end() ? x : it->second;
    };
    if (o == op::EXT) {
      for (size_t i = 4; i < a.size(); ++i) a[i] = rp(a[i]);
    } else if (o == op::STORE) {
      a[1] = rp(a[1]);
    } else if (o == op::EXTRACT) {
      a[2] = rp(a[2]);
    } else if (o == op::SHUFFLE) {
      a[2] = rp(a[2]);
      a[3] = rp(a[3]);
    } else if (o == op::PHI) {
      for (size_t i = 2; i < a.size(); i += 2) a[i] = rp(a[i]);
    } else {
      for (size_t i = 2; i < a.size(); ++i) a[i] = rp(a[i]);
    }
    return a;
  };

  std::unordered_set<size_t> moved(r.moves.begin(), r.moves.end());
  const auto [start, end] = m.functions[0];
  std::optional<size_t> last_name;
  for (size_t k = 0; k < m.insts.size(); ++k) {
    if (m.insts[k].op == op::NAME) last_name = k;
  }
  if (!last_name) Fail("E_SPIRV", "module has no debug names");
  auto out_name = [&]() {
    std::vector<uint32_t> a = {r.output};
    auto s = EncodeString("dkb_frag_out");
    a.insert(a.end(), s.begin(), s.end());
    return Inst{op::NAME, a};
  };

  std::vector<Inst> out;
  for (size_t k = 0; k < m.insts.size(); ++k) {
    const auto& [o, a0] = m.insts[k];
    std::vector<uint32_t> a = a0;
    if (k == start) {
      // Plain types, then constants, then arrays and pointers (as order_globals).
      for (const auto& t : types_new) {
        if (t.op != op::TYPE_ARRAY && t.op != op::TYPE_POINTER) out.push_back(t);
      }
      out.insert(out.end(), consts_new.begin(), consts_new.end());
      for (const auto& t : types_new) {
        if (t.op == op::TYPE_ARRAY || t.op == op::TYPE_POINTER) out.push_back(t);
      }
      out.insert(out.end(), vars_new.begin(), vars_new.end());
    }
    if (o == op::NAME && !a.empty() && a[0] == r.output) {
      if (k == *last_name) {
        out.insert(out.end(), names_new.begin(), names_new.end());
        out.push_back(out_name());
      }
      continue;
    }
    if (o == op::ENTRY && m.version() >= 0x00010400u) {
      for (const auto& n : g_order) a.push_back(g[n]);
    }
    if (k > start && k < end) {
      if (moved.contains(k)) continue;
      if (o == op::RETURN) out.push_back({op::CALL, {void_t, fresh(), hooks["dkb_output_hook"]}});
      if (k > r.k3) a = subst(o, a);
      out.push_back({o, a});
      if (k == r.k3) {
        out.insert(out.end(), at_k3.begin(), at_k3.end());
        for (size_t km : r.moves) out.push_back({m.insts[km].op, subst(m.insts[km].op, m.insts[km].a)});
      }
      continue;
    }
    out.push_back({o, a});
    if (o == op::NAME && k == *last_name) {
      out.insert(out.end(), names_new.begin(), names_new.end());
      out.push_back(out_name());
    }
  }
  for (const char* name : {"dkb_tonemap_hook", "dkb_output_hook"}) {
    const uint32_t label = fresh();
    out.push_back({op::FUNCTION, {void_t, hooks[name], 0u, fnty}});
    out.push_back({op::LABEL, {label}});
    out.push_back({op::RETURN, {}});
    out.push_back({op::FUNCTION_END, {}});
  }
  Module m2;
  m2.header = m.header;
  m2.header[3] = next;
  m2.insts = std::move(out);
  m2.Index();
  return m2;
}

// Id operands of an instruction inside a function (labels of branch targets excluded:
// they are defined later in the function by design).
inline void ForEachIdUse(const Inst& inst, const auto& fn) {
  const auto& a = inst.a;
  const uint32_t o = inst.op;
  auto from = [&](size_t i) {
    for (; i < a.size(); ++i) fn(a[i]);
  };
  if (o == op::STORE) {
    if (a.size() >= 2) {
      fn(a[0]);
      fn(a[1]);
    }
  } else if (o == op::LOAD) {
    if (a.size() >= 3) fn(a[2]);
  } else if (o == op::EXT) {
    from(4);
  } else if (o == op::EXTRACT) {
    if (a.size() > 2) fn(a[2]);
  } else if (o == op::INSERT || o == op::SHUFFLE) {
    if (a.size() > 3) {
      fn(a[2]);
      fn(a[3]);
    }
  } else if (o == op::CALL) {
    from(3);
  } else if (o == op::SAMPLE || o == op::SAMPLE_LOD) {
    if (a.size() > 3) {
      fn(a[2]);
      fn(a[3]);
    }
    from(5);
  } else if (o == op::PHI) {
    for (size_t i = 2; i < a.size(); i += 2) fn(a[i]);
  } else if (o == op::BRANCH_COND || o == op::RETURN_VALUE) {
    if (!a.empty()) fn(a[0]);
  } else if (o == op::SWITCH) {
    if (!a.empty()) fn(a[0]);
  } else if (o == op::VARIABLE) {
    if (a.size() > 3) fn(a[3]);
  } else if (o == op::AC || o == op::IBAC || o == op::CONSTRUCT || o == op::COPY || o == op::SAMPLED_IMAGE || o == 77
             || o == 78 || o == 84 || (o >= 109 && o <= 200)) {
    from(2);
  }
}

inline void SelfCheck(const Module& m) {
  std::unordered_set<uint32_t> seen;
  for (const auto& inst : m.insts) {
    const uint32_t r = ResultOf(inst.op, inst.a);
    if (r == 0) continue;
    if (!seen.insert(r).second) Fail("E_SELFCHECK", "id " + std::to_string(r) + " defined twice");
    if (r >= m.bound()) Fail("E_SELFCHECK", "id " + std::to_string(r) + " not below the bound");
  }
  const size_t first_function = m.functions.empty() ? m.insts.size() : m.functions.front().first;
  auto global = [&](size_t kd) {
    const uint32_t o = m.insts[kd].op;
    if (o == op::FUNCTION) return true;  // a call target
    return kd < first_function;          // types, constants, global variables, imports
  };
  for (const auto& [s, e] : m.functions) {
    Function fn(m, s, e);
    for (size_t k = s + 1; k < e; ++k) {
      const auto& inst = m.insts[k];
      const auto bk = fn.block_of.find(k);
      if (bk != fn.block_of.end() && !fn.reachable.contains(bk->second)) continue;  // never runs
      if (inst.op == op::PHI) {
        // A phi's value must be available at the end of its parent block, not at the phi.
        for (size_t i = 2; i + 1 < inst.a.size(); i += 2) {
          const uint32_t u = inst.a[i], parent = inst.a[i + 1];
          auto it = m.where.find(u);
          if (it == m.where.end()) Fail("E_SELFCHECK", "id " + std::to_string(u) + " used but never defined");
          auto pb = fn.blocks.find(parent);
          if (pb == fn.blocks.end()) Fail("E_SELFCHECK", "phi parent " + std::to_string(parent) + " is not a block");
          if (!fn.reachable.contains(parent)) continue;
          const size_t kd = it->second;
          if (kd > s && kd < e) {
            if (!fn.Dominates(kd, pb->second.back())) {
              Fail("E_SELFCHECK", "id " + std::to_string(u) + " is not available at the end of phi parent " + std::to_string(parent));
            }
          } else if (!global(kd)) {
            Fail("E_SELFCHECK", "id " + std::to_string(u) + " is local to another function");
          }
        }
        continue;
      }
      ForEachIdUse(inst, [&](uint32_t u) {
        auto it = m.where.find(u);
        if (it == m.where.end()) Fail("E_SELFCHECK", "id " + std::to_string(u) + " used but never defined");
        const size_t kd = it->second;
        if (kd > s && kd < e) {
          if (!fn.Dominates(kd, k)) Fail("E_SELFCHECK", "id " + std::to_string(u) + " does not dominate its use at instruction " + std::to_string(k));
        } else if (!global(kd)) {
          Fail("E_SELFCHECK", "id " + std::to_string(u) + " is local to another function");
        }
      });
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Splicing the compiled hooks.
//
// A hook module is a fragment shader compiled from dkb_*_hook.frag.slang. Its main() reads
// Input variables and writes one Output variable; Splice() copies main() (and any other
// function it has) into the rewritten composite, maps each Input/Output variable by its
// Location to a variable of the composite, and adds the push-constant block, types and
// constants it needs. Only instructions in HOOK_OPCODES are accepted, a set that is valid in
// SPIR-V 1.3 (the composites' version) whatever version the hook module declares.

struct HookPort {
  uint32_t location;
  const char* target;  // name of the composite's variable (Rewrite() names them)
};

constexpr std::array<HookPort, 8> TONEMAP_INPUTS = {{{0, "dkb_c"},
                                                     {1, "dkb_exposed"},
                                                     {2, "dkb_lut_uvw"},
                                                     {3, "dkb_graded"},
                                                     {4, "dkb_ys"},
                                                     {5, "dkb_chroma"},
                                                     {6, "dkb_has_lut"},
                                                     {7, "dkb_c3"}}};
constexpr std::array<HookPort, 2> OUTPUT_INPUTS = {{{0, "dkb_frag_out"}, {1, "dkb_c3"}}};

struct Hook {
  Module module;
  std::string function;                 // dkb_tonemap_hook or dkb_output_hook
  std::span<const HookPort> inputs;
  const char* output_target;            // composite variable the hook's output is stored to
};

inline bool HookOpcodeAllowed(uint32_t o) {
  // Arithmetic, conversion, composite, comparison, logic and control flow core to SPIR-V 1.0.
  static const std::unordered_set<uint32_t> allowed = {
      1, op::EXT, op::FUNCTION, op::FUNCTION_PARAM, op::FUNCTION_END, op::CALL, op::VARIABLE, op::LOAD, op::STORE,
      op::AC, op::IBAC, 77, 78, op::SHUFFLE, op::CONSTRUCT, op::EXTRACT, op::INSERT, op::COPY, 84,
      109, 110, 111, 112, 113, 114, 115, 116, op::BITCAST, 126, op::FNEG, 128, op::FADD, 130, op::FSUB, 132,
      op::FMUL, 134, 135, op::FDIV, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151,
      152, 154, 155, op::ISNAN, 157, 164, 165, 166, 167, 168, op::SELECT, 170, 171, op::UGT, 173, 174, 175, 176, 177, 178, 179,
      180, 181, 182, 183, 184, 185, 186, 187, 188, 189, op::FGE, op::FUGE, 194, 195, 196, 197, op::XOR, op::AND, 200,
      op::PHI, op::LOOP_MERGE, op::SELECTION_MERGE, op::LABEL, op::BRANCH, op::BRANCH_COND, op::SWITCH,
      op::KILL, op::RETURN, op::RETURN_VALUE, op::UNREACHABLE, 8, 317,  // 8/317: OpLine/OpNoLine, dropped
      400};  // OpCopyLogical (SPIR-V 1.4), lowered by the splice to extracts and a construct
  return allowed.contains(o);
}

inline Hook LoadHook(std::span<const uint8_t> bytes, std::string function, std::span<const HookPort> inputs,
                     const char* output_target) {
  Hook h{Module::Parse(bytes), std::move(function), inputs, output_target};
  const auto& m = h.module;
  for (const auto& inst : m.insts) {
    if (inst.op == op::CAPABILITY && inst.a[0] != 1) Fail("E_HOOK", "hook needs capability " + std::to_string(inst.a[0]));
    if (inst.op == 10) Fail("E_HOOK", "hook uses an extension");
    if (inst.op == op::EXT_IMPORT && m.glsl != inst.a[0]) Fail("E_HOOK", "hook imports another instruction set");
  }
  if (m.functions.empty()) Fail("E_HOOK", "hook has no function");
  for (const auto& [s, e] : m.functions) {
    for (size_t k = s; k <= e; ++k) {
      if (!HookOpcodeAllowed(m.insts[k].op)) Fail("E_HOOK", "hook instruction " + std::to_string(m.insts[k].op) + " is not allowed");
    }
  }
  return h;
}

inline Module Splice(const Module& target, const std::array<const Hook*, 2>& hooks) {
  const Module& out = target;
  uint32_t next = out.bound();
  std::unordered_map<std::string, uint32_t> by_name;
  for (const auto& [id, name] : out.names) by_name[name] = id;
  for (const char* n : {"dkb_tonemap_hook", "dkb_output_hook", "dkb_c", "dkb_c3", "dkb_frag_out"}) {
    if (!by_name.contains(n)) Fail("E_SPLICE", std::string("composite has no ") + n);
  }
  for (const auto& inst : out.insts) {
    if (inst.op == op::VARIABLE && inst.a[2] == SC_PUSH_CONSTANT) Fail("E_SPLICE", "composite already has push constants");
  }

  // Literal operand positions of type declarations (everything else is an id).
  auto literal = [](uint32_t o, size_t i) {
    return o == op::TYPE_INT || o == op::TYPE_FLOAT || (o == op::TYPE_VECTOR && i == 1) || (o == op::TYPE_MATRIX && i == 1)
           || (o == op::TYPE_POINTER && i == 0) || (o == op::TYPE_IMAGE && i >= 1);
  };
  // Structural key of a type; array lengths by value. ids already in composite numbering.
  auto type_key = [&](uint32_t o, const std::vector<uint32_t>& rest, const auto& length_value) {
    std::vector<uint32_t> k = {o};
    for (size_t i = 0; i < rest.size(); ++i) {
      if (o == op::TYPE_ARRAY && i == 1) {
        k.push_back(0xFFFFFFFFu);
        k.push_back(length_value(rest[i]));
      } else {
        k.push_back(rest[i]);
      }
    }
    return k;
  };
  auto composite_length = [&](uint32_t id) {
    auto c = out.constants.find(id);
    return c == out.constants.end() ? 0u : c->second.second;
  };
  std::map<std::vector<uint32_t>, uint32_t> reusable;  // undecorated composite types
  for (uint32_t id : out.type_order) {
    const auto& t = out.types.at(id);
    if (out.decorations.contains(id) || t.op == op::TYPE_STRUCT) continue;
    reusable.emplace(type_key(t.op, t.rest, composite_length), id);
  }

  std::vector<Inst> annotations, globals;
  std::map<uint32_t, uint32_t> new_lengths;  // new constant id -> value, for keys of new arrays
  std::unordered_map<std::string, std::vector<Inst>> bodies;
  // Decorated or struct types already added by a hook, by structure and decorations, so the
  // second hook shares the first one's types (and, through them, its push-constant block).
  std::map<std::vector<uint32_t>, uint32_t> hook_types;
  uint32_t shared_push_var = 0, shared_push_ptr = 0;

  auto pointer_to = [&](uint32_t storage, uint32_t pointee) {
    for (uint32_t id : out.type_order) {
      const auto& t = out.types.at(id);
      if (t.op == op::TYPE_POINTER && t.rest == std::vector<uint32_t>{storage, pointee}) return id;
    }
    for (const auto& gi : globals) {
      if (gi.op == op::TYPE_POINTER && gi.a[1] == storage && gi.a[2] == pointee) return gi.a[0];
    }
    const uint32_t id = next++;
    globals.push_back({op::TYPE_POINTER, {id, storage, pointee}});
    return id;
  };

  for (const Hook* hook : hooks) {
    const Module& h = hook->module;
    std::unordered_map<uint32_t, uint32_t> map;       // hook id -> composite id
    std::unordered_map<uint32_t, uint32_t> port_var;  // hook Input/Output variable -> composite variable
    std::unordered_map<uint32_t, uint32_t> port_type; // hook interface pointee type -> composite variable's type
    std::unordered_set<uint32_t> no_decorations;      // hook ids mapped onto existing ids
    const size_t hook_first_function = h.functions.front().first;

    // Interface variables, by location.
    for (uint32_t id : h.def_order) {
      const auto* d = h.Def(id);
      if (d->op != op::VARIABLE || (d->a[2] != SC_INPUT && d->a[2] != SC_OUTPUT)) continue;
      auto dec = h.decorations.find(id);
      if (dec == h.decorations.end() || !dec->second.contains(DEC_LOCATION)) Fail("E_HOOK", "hook interface variable without a location");
      const uint32_t loc = dec->second.at(DEC_LOCATION)[0];
      const char* name = nullptr;
      if (d->a[2] == SC_OUTPUT) {
        if (loc != 0) Fail("E_HOOK", "hook output is not at location 0");
        name = hook->output_target;
      } else {
        for (const auto& p : hook->inputs) {
          if (p.location == loc) name = p.target;
        }
      }
      if (name == nullptr || !by_name.contains(name)) Fail("E_HOOK", "hook input at an unmapped location");
      if (port_var.contains(id)) Fail("E_HOOK", "hook interface variable declared twice");
      port_var[id] = by_name.at(name);
      const uint32_t ht = h.Pointee(d->a[0]);
      const uint32_t ct = out.Pointee(out.Def(by_name.at(name))->a[0]);
      if (auto pt = port_type.find(ht); pt != port_type.end() && pt->second != ct) Fail("E_HOOK", "hook ports of one type map to different types");
      port_type[ht] = ct;
    }

    // Decorations of each hook id, as a flat key: decoration words, then member decorations.
    std::unordered_map<uint32_t, std::vector<uint32_t>> decoration_key;
    for (const auto& [o, a] : h.insts) {
      if (o != op::DECORATE && o != op::MEMBER_DECORATE) continue;
      auto& k = decoration_key[a[0]];
      k.push_back(0xFFFFFFF0u + (o == op::MEMBER_DECORATE ? 1u : 0u));
      k.push_back(static_cast<uint32_t>(a.size() - 1));
      k.insert(k.end(), a.begin() + 1, a.end());
    }

    // Globals in declaration order.
    for (size_t k = 0; k < hook_first_function; ++k) {
      const auto& [o, a] = h.insts[k];
      if (o >= 19 && o <= 39) {
        std::vector<uint32_t> rest;
        for (size_t i = 1; i < a.size(); ++i) {
          if (literal(o, i - 1)) {
            rest.push_back(a[i]);
          } else {
            auto it = map.find(a[i]);
            if (it == map.end()) Fail("E_HOOK", "hook type refers to an undeclared id");
            rest.push_back(it->second);
          }
        }
        auto length_value = [&](uint32_t id) {
          auto it = new_lengths.find(id);
          return it != new_lengths.end() ? it->second : composite_length(id);
        };
        const auto key = type_key(o, rest, length_value);
        // The type of an interface variable becomes the composite variable's type; the two
        // must have the same structure (a layout decoration on an Input array is irrelevant).
        if (auto pt = port_type.find(a[0]); pt != port_type.end()) {
          const auto& ct = out.types.at(pt->second);
          if (ct.op == op::TYPE_STRUCT || type_key(ct.op, ct.rest, composite_length) != key) {
            Fail("E_HOOK", "hook interface type differs from the composite's");
          }
          map[a[0]] = pt->second;
          no_decorations.insert(a[0]);
          continue;
        }
        const auto dk = decoration_key.find(a[0]);
        const bool decorated = dk != decoration_key.end() || o == op::TYPE_STRUCT;
        if (!decorated) {
          auto found = reusable.find(key);
          if (found != reusable.end()) {
            map[a[0]] = found->second;
            continue;
          }
        }
        std::vector<uint32_t> full = key;
        if (decorated) {
          full.push_back(0xFFFFFFFEu);
          if (dk != decoration_key.end()) full.insert(full.end(), dk->second.begin(), dk->second.end());
          if (auto found = hook_types.find(full); found != hook_types.end()) {
            map[a[0]] = found->second;
            no_decorations.insert(a[0]);
            continue;
          }
        }
        const uint32_t id = next++;
        map[a[0]] = id;
        std::vector<uint32_t> na = {id};
        na.insert(na.end(), rest.begin(), rest.end());
        globals.push_back({o, na});
        if (decorated) {
          hook_types.emplace(std::move(full), id);
        } else {
          reusable.emplace(key, id);
        }
      } else if (o == op::CONSTANT || o == op::CONSTANT_TRUE || o == op::CONSTANT_FALSE || o == op::CONSTANT_COMPOSITE
                 || o == op::CONSTANT_NULL || o == 1) {
        const uint32_t id = next++;
        map[a[1]] = id;
        std::vector<uint32_t> na = {map.at(a[0]), id};
        for (size_t i = 2; i < a.size(); ++i) na.push_back(o == op::CONSTANT_COMPOSITE ? map.at(a[i]) : a[i]);
        if (o == op::CONSTANT && a.size() >= 3) new_lengths[id] = a[2];
        globals.push_back({o, na});
      } else if (o == op::VARIABLE) {
        if (port_var.contains(a[1])) continue;
        if (a[2] != SC_PUSH_CONSTANT && a[2] != SC_PRIVATE) Fail("E_HOOK", "hook declares a resource");
        if (a.size() > 3) Fail("E_HOOK", "hook variable has an initializer");
        if (a[2] == SC_PUSH_CONSTANT && shared_push_var != 0) {
          // One push-constant block per entry point: both hooks must declare the same one.
          if (map.at(a[0]) != shared_push_ptr) Fail("E_HOOK", "hooks declare different push-constant layouts");
          map[a[1]] = shared_push_var;
          no_decorations.insert(a[1]);
          continue;
        }
        const uint32_t id = next++;
        map[a[1]] = id;
        globals.push_back({op::VARIABLE, {map.at(a[0]), id, a[2]}});
        if (a[2] == SC_PUSH_CONSTANT) {
          shared_push_var = id;
          shared_push_ptr = map.at(a[0]);
        }
      } else if (o == op::EXT_IMPORT) {
        map[a[0]] = out.glsl;
      }
    }

    // Functions: the entry point becomes the composite's hook function.
    uint32_t entry = 0;
    for (const auto& inst : h.insts) {
      if (inst.op == op::ENTRY) entry = inst.a[1];
    }
    std::unordered_set<uint32_t> local_ids;
    for (const auto& [s, e] : h.functions) {
      const uint32_t fid = h.insts[s].a[1];
      map[fid] = fid == entry ? by_name.at(hook->function) : next++;
      for (size_t k = s + 1; k < e; ++k) {
        const uint32_t r = ResultOf(h.insts[k].op, h.insts[k].a);
        if (r != 0) local_ids.insert(r);
      }
    }
    auto m_id = [&](uint32_t x) -> uint32_t {
      if (auto pv = port_var.find(x); pv != port_var.end()) return pv->second;
      if (auto it = map.find(x); it != map.end()) return it->second;
      if (!local_ids.contains(x)) Fail("E_HOOK", "hook uses unmapped id " + std::to_string(x));
      const uint32_t id = next++;
      map[x] = id;
      return id;
    };
    std::unordered_map<uint32_t, uint32_t> chain_class;  // pointers into interface variables -> storage class
    for (const auto& [v, cv] : port_var) chain_class[v] = *out.VarClass(cv);
    std::vector<Inst> body;
    // OpCopyLogical is SPIR-V 1.4; the game's modules are older. A copy between two types of
    // the same shape becomes member extracts and a construct, recursively.
    std::function<uint32_t(uint32_t, uint32_t, uint32_t)> copy_logical = [&](uint32_t dst_t, uint32_t src_t,
                                                                             uint32_t value) -> uint32_t {
      if (m_id(dst_t) == m_id(src_t)) return value;
      const auto* dt = h.Type(dst_t);
      const auto* st = h.Type(src_t);
      if (dt == nullptr || st == nullptr || dt->op != st->op) Fail("E_HOOK", "OpCopyLogical between different shapes");
      std::vector<std::pair<uint32_t, uint32_t>> members;  // (dst type, src type)
      if (dt->op == op::TYPE_STRUCT) {
        if (dt->rest.size() != st->rest.size()) Fail("E_HOOK", "OpCopyLogical between different structs");
        for (size_t i = 0; i < dt->rest.size(); ++i) members.emplace_back(dt->rest[i], st->rest[i]);
      } else if (dt->op == op::TYPE_ARRAY) {
        const auto* dl = h.Def(dt->rest[1]);
        const auto* sl = h.Def(st->rest[1]);
        if (dl == nullptr || sl == nullptr || dl->op != op::CONSTANT || sl->op != op::CONSTANT || dl->a[2] != sl->a[2]
            || dl->a[2] > 4096) {
          Fail("E_HOOK", "OpCopyLogical between different arrays");
        }
        members.assign(dl->a[2], {dt->rest[0], st->rest[0]});
      } else {
        Fail("E_HOOK", "OpCopyLogical between different types");
      }
      std::vector<uint32_t> parts = {m_id(dst_t), 0};
      for (size_t i = 0; i < members.size(); ++i) {
        const uint32_t part = next++;
        body.push_back({op::EXTRACT, {m_id(members[i].second), part, value, static_cast<uint32_t>(i)}});
        parts.push_back(copy_logical(members[i].first, members[i].second, part));
      }
      parts[1] = next++;
      body.push_back({op::CONSTRUCT, parts});
      return parts[1];
    };
    for (const auto& [s, e] : h.functions) {
      const uint32_t fid = h.insts[s].a[1];
      for (size_t k = s; k <= e; ++k) {
        const auto& [o, a] = h.insts[k];
        if (o == 8 || o == 317) continue;  // OpLine / OpNoLine
        if (o == 400) {
          const auto* src = h.Def(a[2]);
          if (src == nullptr || ResultOf(src->op, src->a) != a[2] || src->a.size() < 2) Fail("E_HOOK", "OpCopyLogical of an untyped value");
          const uint32_t value = copy_logical(a[0], src->a[0], m_id(a[2]));
          body.push_back({op::COPY, {m_id(a[0]), m_id(a[1]), value}});
          continue;
        }
        std::vector<uint32_t> na = a;
        switch (o) {
          case op::FUNCTION: {
            if (fid == entry) {
              const auto* ft = h.Type(a[3]);
              if (ft == nullptr || ft->rest.size() != 1) Fail("E_HOOK", "hook entry takes parameters");
            }
            na = {m_id(a[0]), map.at(fid), a[2], m_id(a[3])};
            break;
          }
          case op::AC:
          case op::IBAC: {
            if (chain_class.contains(a[2])) {
              na[0] = pointer_to(chain_class.at(a[2]), m_id(h.Pointee(a[0])));
              chain_class[a[1]] = chain_class.at(a[2]);
            } else {
              na[0] = m_id(a[0]);
            }
            for (size_t i = 1; i < na.size(); ++i) na[i] = m_id(a[i]);
            break;
          }
          case op::EXT:
            na[0] = m_id(a[0]);
            na[1] = m_id(a[1]);
            na[2] = out.glsl;
            for (size_t i = 4; i < na.size(); ++i) na[i] = m_id(a[i]);
            break;
          case op::EXTRACT:
            for (size_t i = 0; i < 3; ++i) na[i] = m_id(a[i]);
            break;
          case op::INSERT:
          case op::SHUFFLE:
            for (size_t i = 0; i < 4; ++i) na[i] = m_id(a[i]);
            break;
          case op::VARIABLE:
            na[0] = m_id(a[0]);
            na[1] = m_id(a[1]);
            if (a.size() > 3) na[3] = m_id(a[3]);
            break;
          case op::STORE:
            na[0] = m_id(a[0]);
            na[1] = m_id(a[1]);
            if (port_var.contains(a[0])) {
              const uint32_t cv = port_var.at(a[0]);
              if (out.Pointee(out.Def(cv)->a[0]) != m_id(h.Pointee(h.Def(a[0])->a[0]))) Fail("E_HOOK", "hook output type differs from its target");
            }
            break;
          case op::LOAD:
            for (size_t i = 0; i < 3; ++i) na[i] = m_id(a[i]);
            if (port_var.contains(a[2]) && out.Pointee(out.Def(port_var.at(a[2]))->a[0]) != na[0]) {
              Fail("E_HOOK", "hook input type differs from its source");
            }
            break;
          case op::LABEL:
          case op::BRANCH:
          case op::SELECTION_MERGE:
          case op::RETURN_VALUE:
            na[0] = m_id(a[0]);
            break;
          case op::BRANCH_COND:
            for (size_t i = 0; i < 3; ++i) na[i] = m_id(a[i]);
            break;
          case op::LOOP_MERGE:
            na[0] = m_id(a[0]);
            na[1] = m_id(a[1]);
            break;
          case op::SWITCH:
            na[0] = m_id(a[0]);
            na[1] = m_id(a[1]);
            for (size_t i = 3; i < na.size(); i += 2) na[i] = m_id(a[i]);
            break;
          case op::FUNCTION_END:
          case op::RETURN:
          case op::KILL:
          case op::UNREACHABLE:
            break;
          default:
            // Result type, result and id operands (calls, phis, arithmetic, conversions, logic).
            for (size_t i = 0; i < na.size(); ++i) na[i] = m_id(a[i]);
        }
        body.push_back({o, na});
      }
    }
    // Decorations of copied ids, after the bodies so that local results (NoContraction,
    // RelaxedPrecision) keep theirs. Interface variables and shared ids keep the composite's.
    for (const auto& [o, a] : h.insts) {
      if (o != op::DECORATE && o != op::MEMBER_DECORATE) continue;
      if (port_var.contains(a[0]) || no_decorations.contains(a[0])) continue;
      auto it = map.find(a[0]);
      if (it == map.end()) continue;
      std::vector<uint32_t> na = a;
      na[0] = it->second;
      annotations.push_back({o, na});
    }
    bodies[hook->function] = std::move(body);
  }

  // Assemble: annotations after the composite's last decoration, globals before its first
  // function, hook bodies in place of the empty hook functions.
  std::vector<Inst> result;
  std::optional<size_t> last_annotation;
  size_t first_function = out.insts.size();
  for (size_t k = 0; k < out.insts.size(); ++k) {
    const auto o = out.insts[k].op;
    if (o == op::DECORATE || o == op::MEMBER_DECORATE || o == 73 || o == 74 || o == 75) last_annotation = k;
    if (o == op::FUNCTION && first_function == out.insts.size()) first_function = k;
  }
  if (!last_annotation) Fail("E_SPLICE", "composite has no decorations");
  const std::unordered_set<uint32_t> hook_fns = {by_name.at("dkb_tonemap_hook"), by_name.at("dkb_output_hook")};
  for (size_t k = 0; k < out.insts.size(); ++k) {
    Inst inst = out.insts[k];
    if (k == first_function) result.insert(result.end(), globals.begin(), globals.end());
    if (inst.op == op::FUNCTION && hook_fns.contains(inst.a[1])) {
      const std::string& name = out.names.at(inst.a[1]);
      auto b = bodies.find(name);
      if (b == bodies.end()) Fail("E_SPLICE", "no hook for " + name);
      result.insert(result.end(), b->second.begin(), b->second.end());
      while (out.insts[k].op != op::FUNCTION_END) ++k;
      continue;
    }
    if (inst.op == op::ENTRY && out.version() >= 0x00010400u && shared_push_var != 0) inst.a.push_back(shared_push_var);
    result.push_back(std::move(inst));
    if (k == *last_annotation) result.insert(result.end(), annotations.begin(), annotations.end());
  }
  Module m2;
  m2.header = out.header;
  m2.header[3] = next;
  m2.insts = std::move(result);
  m2.Index();
  return m2;
}

struct Result {
  std::vector<uint32_t> words;
  Recognition recognition;
};

// Recognise, rewrite, optionally splice, self-check. Throws Reject.
inline Result Transform(std::span<const uint8_t> code, const Hook* tonemap = nullptr, const Hook* output = nullptr,
                        bool self_check = true) {
  Module m = Module::Parse(code);
  Recognition r = Recognize(m);
  Module rewritten = Rewrite(m, r);
  if (tonemap != nullptr && output != nullptr) rewritten = Splice(rewritten, {tonemap, output});
  if (self_check) SelfCheck(rewritten);
  return {rewritten.Serialize(), std::move(r)};
}

}  // namespace dkb::structural
