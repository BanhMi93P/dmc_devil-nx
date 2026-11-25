/**
 * @file    gpu/render/shader_library.cpp
 */
#include "renut_engine/gpu/render/shader_library.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <smolv.h>
#include <xxhash.h>
#include <zstd.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"
#if defined(PLUME_DEKO3D)
#include "renut_engine/gpu/banjo_renut_deko3d.h"  // banjo-nx (2026-10-17): the deko3d back end's GLSL package
#endif

REXCVAR_DEFINE_STRING(renut_shader_capture_dir, "shader_capture", "Nuts&Bolts/Graphics",
                      "Where shaders missing from the shader cache are written; the next build "
                      "recompiles them into the cache");

namespace renut::gpu {

namespace {

struct DecompressedCache {
  std::once_flag once;
  std::vector<uint8_t> bytes;
};

DecompressedCache g_spirv;
#if defined(_WIN32)
DecompressedCache g_dxil;
#endif
std::atomic<uint64_t> g_decompressed_bytes{0};  // ShaderCacheHostBytes

const uint8_t *Decompress(DecompressedCache &cache, const uint8_t *compressed,
                          size_t compressed_size, size_t decompressed_size, const char *name) {
  std::call_once(cache.once, [&] {
    cache.bytes.resize(decompressed_size);
    const size_t n = ZSTD_decompress(cache.bytes.data(), decompressed_size, compressed,
                                     compressed_size);
    if (ZSTD_isError(n) || n != decompressed_size) {
      RGPU_ERROR("{} shader cache decompression failed", name);
      cache.bytes.clear();
      cache.bytes.shrink_to_fit();
    }
    g_decompressed_bytes.fetch_add(cache.bytes.capacity(), std::memory_order_relaxed);
  });
  return cache.bytes.empty() ? nullptr : cache.bytes.data();
}

std::mutex g_capture_mutex;
std::unordered_set<uint64_t> g_captured;  // CaptureMissingShader

// banjo-nx (2026-10-22): the table FindShaderEntry searches.
struct ShaderTable {
  std::once_flag once;
  const ShaderCacheEntry *entries = nullptr;
  size_t count = 0;
  std::vector<ShaderCacheEntry> merged_entries;
  std::vector<ShaderVertexInput> merged_inputs;
  std::vector<ShaderLiteral> merged_literals;
};
ShaderTable g_table;

ShaderTable &Table() {
  std::call_once(g_table.once, [] {
    ShaderTable &t = g_table;
    t.entries = g_shaderCacheEntries;
    t.count = g_shaderCacheEntryCount;
#if defined(PLUME_DEKO3D)
    // Not IsDeko3D(): the pipeline list can look shaders up before the device is made (and the Switch build has only
    // the deko3d back end).
    if (!banjo::renut_gpu::deko3d::InstalledShaderTable(t.merged_entries, t.merged_inputs, t.merged_literals))
      return;
    // The installer's shaders, then the build's own for those its package lacks (the ones the game makes at runtime,
    // which no installer finds in the game's files), their inputs and literals after the package's.
    const size_t installed = t.merged_entries.size();
    for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) {
      ShaderCacheEntry e = g_shaderCacheEntries[i];
      const auto begin = t.merged_entries.begin(), end = begin + installed;
      const auto it = std::lower_bound(begin, end, e.hash,
                                       [](const ShaderCacheEntry &a, uint64_t h) { return a.hash < h; });
      if (it != end && it->hash == e.hash)
        continue;
      const uint32_t inputs = uint32_t(t.merged_inputs.size()), literals = uint32_t(t.merged_literals.size());
      t.merged_inputs.insert(t.merged_inputs.end(), g_shaderVertexInputs + e.vertexInputOffset,
                             g_shaderVertexInputs + e.vertexInputOffset + e.vertexInputCount);
      t.merged_literals.insert(t.merged_literals.end(), g_shaderLiterals + e.literalOffset,
                               g_shaderLiterals + e.literalOffset + e.literalCount);
      e.vertexInputOffset = inputs;
      e.literalOffset = literals;
      t.merged_entries.push_back(e);
    }
    const size_t own = t.merged_entries.size() - installed;
    std::sort(t.merged_entries.begin(), t.merged_entries.end(),
              [](const ShaderCacheEntry &a, const ShaderCacheEntry &b) { return a.hash < b.hash; });
    t.entries = t.merged_entries.data();
    t.count = t.merged_entries.size();
    g_activeShaderVertexInputs = t.merged_inputs.data();
    g_activeShaderLiterals = t.merged_literals.data();
    RGPU_INFO("Shader table: {} shaders from the installed package, {} of this build's own", installed, own);
#endif
  });
  return g_table;
}

} // namespace

const ShaderVertexInput *g_activeShaderVertexInputs = g_shaderVertexInputs;
const ShaderLiteral *g_activeShaderLiterals = g_shaderLiterals;

uint64_t HashShaderContainer(const uint8_t *container) {
  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  const size_t size = size_t(u32(header->virtualSize)) + u32(header->physicalSize);
  return XXH3_64bits(container, size);
}

const ShaderCacheEntry *FindShaderEntry(uint64_t hash) {
  if (g_shaderCacheLayout != kShaderCacheLayout) {
    static bool logged = false;
    if (!logged) {
      RGPU_ERROR("Shader cache layout {} does not match the renderer's {}; rebuild it",
                 g_shaderCacheLayout, kShaderCacheLayout);
      logged = true;
    }
    return nullptr;
  }
  const ShaderTable &table = Table();
  const ShaderCacheEntry *begin = table.entries;
  const ShaderCacheEntry *end = begin + table.count;
  const ShaderCacheEntry *it = std::lower_bound(
      begin, end, hash, [](const ShaderCacheEntry &e, uint64_t h) { return e.hash < h; });
  return (it != end && it->hash == hash) ? it : nullptr;
}

void ShaderBoolSpecMask(const ShaderCacheEntry &entry, uint32_t mask[8]) {
  std::fill(mask, mask + 8, 0u);
#if defined(PLUME_DEKO3D)
  // banjo-nx: the GLSL package lists them (the SPIR-V cache need not be unpacked for deko3d).
  if (IsDeko3D()) {
    banjo::renut_gpu::deko3d::BoolSpecMask(entry.hash, mask);
    return;
  }
#endif
  if (!IsVulkan() || entry.spirvSize == 0)
    return;
  const uint8_t *cache = Decompress(g_spirv, g_compressedSpirvCache, g_spirvCacheCompressedSize,
                                    g_spirvCacheDecompressedSize, "SPIR-V");
  if (!cache)
    return;
  const uint8_t *smol = cache + entry.spirvOffset;
  std::vector<uint8_t> spirv(smolv::GetDecodedBufferSize(smol, entry.spirvSize));
  if (spirv.size() < 20 || !smolv::Decode(smol, entry.spirvSize, spirv.data(), spirv.size()))
    return;
  // After the five header words, each instruction's first word is (word count << 16) | opcode. OpDecorate (71):
  // target, decoration, literals; SpecId is decoration 1.
  const auto *words = reinterpret_cast<const uint32_t *>(spirv.data());
  const size_t count = spirv.size() / 4;
  for (size_t i = 5; i < count;) {
    const uint32_t length = words[i] >> 16, opcode = words[i] & 0xFFFF;
    if (length == 0)
      break;
    if (opcode == 71 && length >= 4 && i + 3 < count && words[i + 2] == 1) {
      const uint32_t id = words[i + 3];
      if (id >= 1000 && id < 1256)
        mask[(id - 1000) / 32] |= 1u << ((id - 1000) % 32);
    }
    i += length;
  }
}

namespace {

struct SpvBlock {
  uint32_t label = 0;
  uint32_t func_id = 0;
  std::vector<std::vector<uint32_t>> insts;  // All instructions in the block (OpLabel..terminator).

  bool empty() const { return insts.empty(); }
  const std::vector<uint32_t> *Term() const {
    return insts.empty() ? nullptr : &insts.back();
  }
  std::vector<uint32_t> *Term() {
    return insts.empty() ? nullptr : &insts.back();
  }
  const std::vector<uint32_t> *Penultimate() const {
    return insts.size() >= 2 ? &insts[insts.size() - 2] : nullptr;
  }
  std::vector<uint32_t> *Penultimate() {
    return insts.size() >= 2 ? &insts[insts.size() - 2] : nullptr;
  }
};

void CollectTermTargets(const std::vector<uint32_t> &term, std::vector<uint32_t> &out) {
  out.clear();
  if (term.empty())
    return;
  const uint32_t op = term[0] & 0xFFFFu;
  if (op == 249u && term.size() >= 2) {  // OpBranch
    out.push_back(term[1]);
  } else if (op == 250u && term.size() >= 4) {  // OpBranchConditional
    out.push_back(term[2]);
    if (term[3] != term[2])
      out.push_back(term[3]);
  } else if (op == 251u && term.size() >= 3) {  // OpSwitch
    out.push_back(term[2]);
    for (size_t k = 4; k < term.size(); k += 2) {
      if (std::find(out.begin(), out.end(), term[k]) == out.end())
        out.push_back(term[k]);
    }
  }
}

void ReplaceTermTarget(std::vector<uint32_t> &term, uint32_t old_lbl, uint32_t new_lbl) {
  if (term.empty())
    return;
  const uint32_t op = term[0] & 0xFFFFu;
  if (op == 249u && term.size() >= 2) {
    if (term[1] == old_lbl)
      term[1] = new_lbl;
  } else if (op == 250u && term.size() >= 4) {
    if (term[2] == old_lbl)
      term[2] = new_lbl;
    if (term[3] == old_lbl)
      term[3] = new_lbl;
  } else if (op == 251u && term.size() >= 3) {
    if (term[2] == old_lbl)
      term[2] = new_lbl;
    for (size_t k = 4; k < term.size(); k += 2) {
      if (term[k] == old_lbl)
        term[k] = new_lbl;
    }
  }
}

bool TermHasTarget(const std::vector<uint32_t> &term, uint32_t target_lbl) {
  if (term.empty())
    return false;
  const uint32_t op = term[0] & 0xFFFFu;
  if (op == 249u && term.size() >= 2)
    return term[1] == target_lbl;
  if (op == 250u && term.size() >= 4)
    return term[2] == target_lbl || term[3] == target_lbl;
  if (op == 251u && term.size() >= 3) {
    if (term[2] == target_lbl)
      return true;
    for (size_t k = 4; k < term.size(); k += 2) {
      if (term[k] == target_lbl)
        return true;
    }
  }
  return false;
}

bool TryFlattenOneZeroSwitch(std::vector<uint32_t> &words, uint32_t &next_id, uint32_t bool_type_id,
                             std::unordered_set<uint32_t> &unflattenable_hdrs) {
  const size_t wc = words.size();
  std::vector<SpvBlock> blocks;
  // Non-block instructions are stored as chunks emitted at fixed points relative to blocks:
  // prefix[k] holds any non-block instructions immediately preceding block k,
  // and trailing holds instructions after the last block (last OpFunctionEnd).
  std::vector<std::vector<std::vector<uint32_t>>> prefix;
  std::vector<std::vector<uint32_t>> trailing;
  std::vector<std::vector<uint32_t>> pending_nonblock;

  bool in_func_body = false;
  uint32_t cur_func_id = 0;
  int cur_block = -1;

  for (size_t i = 5; i < wc;) {
    const uint32_t header = words[i];
    const uint32_t op = header & 0xFFFFu;
    const uint32_t len = header >> 16;
    if (len == 0 || i + len > wc)
      return false;

    std::vector<uint32_t> inst(words.begin() + i, words.begin() + i + len);
    if (op == 54u && len >= 3) {  // OpFunction
      cur_func_id = inst[2];
      in_func_body = false;
      cur_block = -1;
      pending_nonblock.push_back(std::move(inst));
    } else if (op == 56u) {  // OpFunctionEnd
      in_func_body = false;
      cur_block = -1;
      pending_nonblock.push_back(std::move(inst));
    } else if (cur_func_id != 0 && op == 248u && len == 2) {  // OpLabel
      in_func_body = true;
      prefix.push_back(std::move(pending_nonblock));
      pending_nonblock.clear();
      SpvBlock blk;
      blk.label = inst[1];
      blk.func_id = cur_func_id;
      blk.insts.push_back(std::move(inst));
      cur_block = int(blocks.size());
      blocks.push_back(std::move(blk));
    } else if (in_func_body && cur_block >= 0) {
      blocks[cur_block].insts.push_back(std::move(inst));
    } else {
      pending_nonblock.push_back(std::move(inst));
    }
    i += len;
  }
  trailing = std::move(pending_nonblock);

  std::unordered_map<uint32_t, size_t> lbl_to_blk;
  lbl_to_blk.reserve(blocks.size());
  for (size_t b = 0; b < blocks.size(); ++b)
    lbl_to_blk[blocks[b].label] = b;

  // Pick the LAST 0-case OpSwitch header not yet marked unflattenable (innermost first).
  size_t H = SIZE_MAX;
  uint32_t M = 0;
  uint32_t B = 0;
  for (size_t b = blocks.size(); b-- > 0;) {
    const auto *pen = blocks[b].Penultimate();
    const auto *term = blocks[b].Term();
    if (!pen || !term)
      continue;
    if ((*pen)[0] == ((3u << 16) | 247u) && (*term)[0] == ((3u << 16) | 251u)) {
      if (unflattenable_hdrs.count(blocks[b].label))
        continue;
      H = b;
      M = (*pen)[1];
      B = (*term)[2];
      break;
    }
  }
  if (H == SIZE_MAX)
    return false;

  const uint32_t hdr_lbl = blocks[H].label;
  const uint32_t func_id = blocks[H].func_id;
  auto m_it = lbl_to_blk.find(M);
  auto b_it = lbl_to_blk.find(B);
  if (m_it == lbl_to_blk.end() || b_it == lbl_to_blk.end() || blocks[m_it->second].func_id != func_id ||
      blocks[b_it->second].func_id != func_id) {
    unflattenable_hdrs.insert(hdr_lbl);
    return true;
  }
  const size_t M_blk = m_it->second;

  auto rebuild_words = [&](const std::vector<SpvBlock> *trampolines_before_M) {
    std::vector<uint32_t> out;
    out.reserve(words.size() + (trampolines_before_M ? trampolines_before_M->size() * 16 : 0));
    out.insert(out.end(), words.begin(), words.begin() + 5);
    for (size_t b = 0; b < blocks.size(); ++b) {
      for (const auto &inst : prefix[b])
        out.insert(out.end(), inst.begin(), inst.end());
      if (b == M_blk && trampolines_before_M) {
        for (const auto &tb : *trampolines_before_M) {
          for (const auto &inst : tb.insts)
            out.insert(out.end(), inst.begin(), inst.end());
        }
      }
      for (const auto &inst : blocks[b].insts)
        out.insert(out.end(), inst.begin(), inst.end());
    }
    for (const auto &inst : trailing)
      out.insert(out.end(), inst.begin(), inst.end());
    out[3] = next_id;
    words = std::move(out);
  };

  if (B == M) {
    blocks[H].insts.pop_back();
    blocks[H].insts.pop_back();
    blocks[H].insts.push_back({(2u << 16) | 249u, M});
    rebuild_words(nullptr);
    return true;
  }

  // Walk the top-level spine from B to M.
  std::vector<size_t> spine;
  std::unordered_set<uint32_t> spine_seen;
  uint32_t cur_lbl = B;
  while (cur_lbl != M) {
    auto it = lbl_to_blk.find(cur_lbl);
    if (it == lbl_to_blk.end() || blocks[it->second].func_id != func_id || !spine_seen.insert(cur_lbl).second) {
      unflattenable_hdrs.insert(hdr_lbl);
      return true;
    }
    const size_t sb = it->second;
    spine.push_back(sb);
    const auto *pen = blocks[sb].Penultimate();
    const auto *term = blocks[sb].Term();
    if (!term) {
      unflattenable_hdrs.insert(hdr_lbl);
      return true;
    }
    if (pen && ((*pen)[0] & 0xFFFFu) == 247u && pen->size() >= 2) {
      cur_lbl = (*pen)[1];
    } else if (pen && ((*pen)[0] & 0xFFFFu) == 246u && pen->size() >= 3) {
      cur_lbl = (*pen)[1];
    } else if (((*term)[0] & 0xFFFFu) == 249u && term->size() >= 2) {
      cur_lbl = (*term)[1];
    } else {
      unflattenable_hdrs.insert(hdr_lbl);
      return true;
    }
  }
  if (spine.empty()) {
    unflattenable_hdrs.insert(hdr_lbl);
    return true;
  }

  // Collect all predecessors of M within this function.
  std::unordered_set<size_t> preds_of_M;
  for (size_t b = 0; b < blocks.size(); ++b) {
    if (blocks[b].func_id != func_id)
      continue;
    const auto *term = blocks[b].Term();
    if (term && TermHasTarget(*term, M))
      preds_of_M.insert(b);
  }
  if (preds_of_M.empty()) {
    unflattenable_hdrs.insert(hdr_lbl);
    return true;
  }

  // BFS helper: explores from start_lbl inside function, stopping at stop1 or stop2 (never expands
  // past them, but records whether edges hit stop1/stop2 and collects blocks whose term targets M).
  struct ReachInfo {
    bool ok = true;
    bool hit_stop1 = false;
    bool hit_stop2 = false;
    std::unordered_set<size_t> visited;
    std::unordered_set<size_t> m_preds;
  };
  auto bfs_stop = [&](uint32_t start_lbl, uint32_t stop1, uint32_t stop2) -> ReachInfo {
    ReachInfo info;
    if (start_lbl == stop1) {
      info.hit_stop1 = true;
      return info;
    }
    if (start_lbl == stop2) {
      info.hit_stop2 = true;
      return info;
    }
    std::vector<size_t> q;
    auto push_lbl = [&](uint32_t l) {
      if (l == stop1) {
        info.hit_stop1 = true;
        return;
      }
      if (l == stop2) {
        info.hit_stop2 = true;
        return;
      }
      auto it = lbl_to_blk.find(l);
      if (it == lbl_to_blk.end() || blocks[it->second].func_id != func_id) {
        info.ok = false;
        return;
      }
      if (info.visited.insert(it->second).second)
        q.push_back(it->second);
    };
    push_lbl(start_lbl);
    std::vector<uint32_t> targets;
    for (size_t qi = 0; qi < q.size() && info.ok; ++qi) {
      const size_t blk = q[qi];
      const auto *term = blocks[blk].Term();
      if (!term) {
        info.ok = false;
        break;
      }
      const uint32_t top = (*term)[0] & 0xFFFFu;
      if (top == 253u || top == 254u || top == 252u) {  // Return, ReturnValue, Kill
        info.ok = false;
        break;
      }
      if (TermHasTarget(*term, M))
        info.m_preds.insert(blk);
      CollectTermTargets(*term, targets);
      for (uint32_t t : targets)
        push_lbl(t);
    }
    return info;
  };

  struct ExitStep {
    size_t hdr_blk = 0;
    uint32_t next_lbl = 0;
    uint32_t exit_arm = 0;
    bool direct_to_M = false;
    std::unordered_set<size_t> exit_preds;
  };
  std::vector<ExitStep> steps;

  for (size_t sb : spine) {
    const auto *pen = blocks[sb].Penultimate();
    const auto *term = blocks[sb].Term();
    if (!pen || !term)
      continue;
    if (((*pen)[0] & 0xFFFFu) != 247u || pen->size() < 2)
      continue;
    if (((*term)[0] & 0xFFFFu) != 250u || term->size() < 4)
      continue;
    const uint32_t N_i = (*pen)[1];
    const uint32_t T = (*term)[2];
    const uint32_t F = (*term)[3];
    if (T == F)
      continue;

    auto try_arm = [&](uint32_t exit_arm, uint32_t cont_arm, ExitStep &out_step) -> bool {
      if (exit_arm == N_i)
        return false;
      ReachInfo cont_info = bfs_stop(cont_arm, N_i, M);
      if (!cont_info.ok || cont_info.hit_stop2)
        return false;
      if (exit_arm == M) {
        out_step.hdr_blk = sb;
        out_step.next_lbl = N_i;
        out_step.exit_arm = exit_arm;
        out_step.direct_to_M = true;
        out_step.exit_preds = {sb};
        return true;
      }
      ReachInfo exit_info = bfs_stop(exit_arm, M, N_i);
      if (!exit_info.ok || !exit_info.hit_stop1 || exit_info.hit_stop2 || exit_info.m_preds.empty())
        return false;
      out_step.hdr_blk = sb;
      out_step.next_lbl = N_i;
      out_step.exit_arm = exit_arm;
      out_step.direct_to_M = false;
      out_step.exit_preds = std::move(exit_info.m_preds);
      return true;
    };

    ExitStep step;
    if (try_arm(T, F, step) || try_arm(F, T, step))
      steps.push_back(std::move(step));
  }

  // Verify pairwise disjointness of exit_preds and partition of preds_of_M.
  std::unordered_set<size_t> all_exit_preds;
  for (const auto &st : steps) {
    for (size_t p : st.exit_preds) {
      if (!all_exit_preds.insert(p).second) {
        unflattenable_hdrs.insert(hdr_lbl);
        return true;
      }
    }
  }
  std::unordered_set<size_t> fall_preds;
  for (size_t p : preds_of_M) {
    if (!all_exit_preds.count(p))
      fall_preds.insert(p);
  }

  if (steps.empty()) {
    // Only safe if there is a single predecessor at the end of the spine (no inner breaks to M).
    if (preds_of_M.size() != 1 || *preds_of_M.begin() != spine.back()) {
      unflattenable_hdrs.insert(hdr_lbl);
      return true;
    }
    blocks[H].insts.pop_back();
    blocks[H].insts.pop_back();
    blocks[H].insts.push_back({(2u << 16) | 249u, B});
    rebuild_words(nullptr);
    return true;
  }

  if (fall_preds.empty()) {
    unflattenable_hdrs.insert(hdr_lbl);
    return true;
  }
  ReachInfo fall_check = bfs_stop(steps.back().next_lbl, M, 0u);
  if (!fall_check.ok || fall_check.m_preds != fall_preds) {
    unflattenable_hdrs.insert(hdr_lbl);
    return true;
  }

  // Verify all OpPhi incoming labels in M map to a known predecessor in all_exit_preds or fall_preds.
  std::unordered_map<uint32_t, int> pred_lbl_group;
  for (size_t j = 0; j < steps.size(); ++j) {
    for (size_t p : steps[j].exit_preds)
      pred_lbl_group[blocks[p].label] = int(j);
  }
  for (size_t p : fall_preds)
    pred_lbl_group[blocks[p].label] = int(steps.size());  // fall group

  std::vector<size_t> m_phi_indices;
  for (size_t ii = 1; ii < blocks[M_blk].insts.size(); ++ii) {
    const auto &inst = blocks[M_blk].insts[ii];
    const uint32_t op = inst[0] & 0xFFFFu;
    if (op == 245u) {
      if (inst.size() < 5 || (inst.size() % 2) == 0) {
        unflattenable_hdrs.insert(hdr_lbl);
        return true;
      }
      for (size_t m = 3; m + 1 < inst.size(); m += 2) {
        if (!pred_lbl_group.count(inst[m + 1])) {
          unflattenable_hdrs.insert(hdr_lbl);
          return true;
        }
      }
      m_phi_indices.push_back(ii);
    } else if (op != 8u) {  // Stop past OpPhi (allow OpLine=8 if ever present)
      break;
    }
  }

  // Perform the acyclic rewrite.
  const size_t k = steps.size();
  std::vector<uint32_t> fresh_M(k, 0u);  // fresh_M[j] used for j = 1..k-1
  for (size_t j = 1; j < k; ++j)
    fresh_M[j] = next_id++;
  auto merge_for = [&](size_t j) -> uint32_t { return j == 0 ? M : fresh_M[j]; };
  const uint32_t fall_target = (k <= 1) ? M : fresh_M[k - 1];

  // Replace H's OpSelectionMerge M 0; OpSwitch %sel B -> OpBranch B.
  blocks[H].insts.pop_back();
  blocks[H].insts.pop_back();
  blocks[H].insts.push_back({(2u << 16) | 249u, B});

  // Update each exit step j = 0..k-1.
  for (size_t j = 0; j < k; ++j) {
    const uint32_t target_M = merge_for(j);
    auto *pen = blocks[steps[j].hdr_blk].Penultimate();
    *pen = {(3u << 16) | 247u, target_M, 0u};
    if (steps[j].direct_to_M) {
      ReplaceTermTarget(*blocks[steps[j].hdr_blk].Term(), M, target_M);
    } else if (m_phi_indices.empty() && steps[j].exit_preds.size() == 1) {
      // Eliminate empty single-branch exit stubs so the branch targets target_M directly.
      const size_t stub_blk = *steps[j].exit_preds.begin();
      if (blocks[stub_blk].label == steps[j].exit_arm && blocks[stub_blk].insts.size() == 2 &&
          (blocks[stub_blk].insts[1][0] & 0xFFFFu) == 249u) {
        bool unique_pred = true;
        for (size_t b = 0; b < blocks.size(); ++b) {
          if (b == steps[j].hdr_blk || blocks[b].func_id != func_id)
            continue;
          if (const auto *t = blocks[b].Term()) {
            if (TermHasTarget(*t, blocks[stub_blk].label)) {
              unique_pred = false;
              break;
            }
          }
        }
        if (unique_pred) {
          ReplaceTermTarget(*blocks[steps[j].hdr_blk].Term(), blocks[stub_blk].label, target_M);
          blocks[stub_blk].insts.clear();
          steps[j].direct_to_M = true;
        }
      }
    }
    for (size_t p : steps[j].exit_preds) {
      if (p != steps[j].hdr_blk) {
        if (auto *pt = blocks[p].Term())
          ReplaceTermTarget(*pt, M, target_M);
      }
    }
    if (steps[j].direct_to_M && bool_type_id != 0) {
      // Canonicalize true-arm-to-merge into continue-arm true, merge false (matches structured if).
      auto &hdr_insts = blocks[steps[j].hdr_blk].insts;
      auto &term = hdr_insts.back();
      if ((term[0] & 0xFFFFu) == 250u && term.size() >= 4 && term[2] == target_M &&
          term[3] != target_M) {
        const uint32_t not_id = next_id++;
        const uint32_t cond_id = term[1];
        term[1] = not_id;
        std::swap(term[2], term[3]);
        if (term.size() >= 6)
          std::swap(term[4], term[5]);
        hdr_insts.insert(hdr_insts.end() - 2, {(4u << 16) | 168u, bool_type_id, not_id, cond_id});
      }
    }
  }
  for (size_t p : fall_preds)
    ReplaceTermTarget(*blocks[p].Term(), M, fall_target);

  std::vector<SpvBlock> trampolines;
  if (k >= 2) {
    // Partition each OpPhi in M into pairs_by_step[0..k-1] and pairs_fall, and allocate fresh phi IDs.
    struct PhiSplit {
      size_t inst_idx = 0;
      uint32_t res_type = 0;
      uint32_t orig_res_id = 0;
      std::vector<uint32_t> fresh_ids;  // fresh_ids[j] for j = 1..k-1
      std::vector<std::vector<std::pair<uint32_t, uint32_t>>> step_pairs;
      std::vector<std::pair<uint32_t, uint32_t>> fall_pairs;
    };
    std::vector<PhiSplit> phi_splits;
    phi_splits.reserve(m_phi_indices.size());
    for (size_t ii : m_phi_indices) {
      const auto &inst = blocks[M_blk].insts[ii];
      PhiSplit ps;
      ps.inst_idx = ii;
      ps.res_type = inst[1];
      ps.orig_res_id = inst[2];
      ps.fresh_ids.assign(k, 0u);
      for (size_t j = 1; j < k; ++j)
        ps.fresh_ids[j] = next_id++;
      ps.step_pairs.resize(k);
      for (size_t m = 3; m + 1 < inst.size(); m += 2) {
        const uint32_t val = inst[m];
        const uint32_t blk_lbl = inst[m + 1];
        const int grp = pred_lbl_group[blk_lbl];
        if (grp < int(k))
          ps.step_pairs[grp].emplace_back(val, blk_lbl);
        else
          ps.fall_pairs.emplace_back(val, blk_lbl);
      }
      phi_splits.push_back(std::move(ps));
    }

    // Build trampoline blocks fresh_M[k-1] .. fresh_M[1] in reverse order right before M.
    for (size_t j = k; --j >= 1;) {
      SpvBlock tb;
      tb.label = fresh_M[j];
      tb.func_id = func_id;
      tb.insts.push_back({(2u << 16) | 248u, fresh_M[j]});
      for (const PhiSplit &ps : phi_splits) {
        std::vector<std::pair<uint32_t, uint32_t>> pairs = ps.step_pairs[j];
        if (j == k - 1)
          pairs.insert(pairs.end(), ps.fall_pairs.begin(), ps.fall_pairs.end());
        else
          pairs.emplace_back(ps.fresh_ids[j + 1], fresh_M[j + 1]);
        std::vector<uint32_t> phi_inst;
        phi_inst.reserve(3 + pairs.size() * 2);
        phi_inst.push_back((uint32_t(3 + pairs.size() * 2) << 16) | 245u);
        phi_inst.push_back(ps.res_type);
        phi_inst.push_back(ps.fresh_ids[j]);
        for (const auto &pr : pairs) {
          phi_inst.push_back(pr.first);
          phi_inst.push_back(pr.second);
        }
        tb.insts.push_back(std::move(phi_inst));
      }
      tb.insts.push_back({(2u << 16) | 249u, merge_for(j - 1)});
      trampolines.push_back(std::move(tb));
    }

    // Update OpPhis in block M.
    for (const PhiSplit &ps : phi_splits) {
      std::vector<std::pair<uint32_t, uint32_t>> pairs = ps.step_pairs[0];
      pairs.emplace_back(ps.fresh_ids[1], fresh_M[1]);
      std::vector<uint32_t> phi_inst;
      phi_inst.reserve(3 + pairs.size() * 2);
      phi_inst.push_back((uint32_t(3 + pairs.size() * 2) << 16) | 245u);
      phi_inst.push_back(ps.res_type);
      phi_inst.push_back(ps.orig_res_id);
      for (const auto &pr : pairs) {
        phi_inst.push_back(pr.first);
        phi_inst.push_back(pr.second);
      }
      blocks[M_blk].insts[ps.inst_idx] = std::move(phi_inst);
    }
  }

  rebuild_words(k >= 2 ? &trampolines : nullptr);
  return true;
}

}  // namespace

// Work around DXC and AMD Windows Vulkan driver SPIR-V issues at shader load time:
// - Flatten DXC 0-case OpSwitch early-return ladders into canonical structured selections.
// - Convert module-scope OpUndef on nullable types to OpConstantNull.
// - Rewrite OpBitcast -> OpConvertUToPtr for PhysicalStorageBuffer (BDA) pointers.
static void PatchSpirvForVulkan(std::vector<uint8_t> &spirv) {
  if (spirv.size() < 20 || (spirv.size() % 4) != 0)
    return;

  const uint32_t *init_words = reinterpret_cast<const uint32_t *>(spirv.data());
  if (init_words[0] != 0x07230203u)
    return;

  const size_t init_wc = spirv.size() / 4;
  bool has_bitcast = false;
  bool has_zero_switch = false;
  bool has_undef = false;
  bool in_preamble = true;
  uint32_t init_bool_type_id = 0;
  std::vector<uint32_t> bda_ptr_types;
  std::vector<uint32_t> nullable_types;
  bool prev_was_sel_merge = false;

  auto is_nullable_op = [](uint32_t op) {
    return op == 20u || op == 21u || op == 22u || op == 23u || op == 24u || op == 28u || op == 30u;
  };

  for (size_t i = 5; i < init_wc;) {
    const uint32_t header = init_words[i];
    const uint32_t op = header & 0xFFFFu;
    const uint32_t len = header >> 16;
    if (len == 0 || i + len > init_wc)
      return;
    if (op == 20u && len == 2u && init_bool_type_id == 0)
      init_bool_type_id = init_words[i + 1];
    if (op == 54u)
      in_preamble = false;
    if (in_preamble && is_nullable_op(op) && len >= 2u) {
      nullable_types.push_back(init_words[i + 1]);
    } else if (in_preamble && op == 1u && len == 3u &&
               std::find(nullable_types.begin(), nullable_types.end(), init_words[i + 1]) !=
                   nullable_types.end()) {
      has_undef = true;
    } else if (op == 32u && len >= 4u && init_words[i + 2] == 5349u) {
      bda_ptr_types.push_back(init_words[i + 1]);
    } else if (op == 124u && len == 4u &&
               std::find(bda_ptr_types.begin(), bda_ptr_types.end(), init_words[i + 1]) !=
                   bda_ptr_types.end()) {
      has_bitcast = true;
    } else if (op == 251u && len == 3u && prev_was_sel_merge) {
      has_zero_switch = true;
    }
    prev_was_sel_merge = (op == 247u && len == 3u);
    i += len;
  }

  if (!has_bitcast && !has_zero_switch && !has_undef)
    return;

  std::vector<uint32_t> words(init_words, init_words + init_wc);
  uint32_t next_id = words[3];

  if (has_zero_switch) {
    std::unordered_set<uint32_t> unflattenable_hdrs;
    while (TryFlattenOneZeroSwitch(words, next_id, init_bool_type_id, unflattenable_hdrs)) {
    }
  }

  // Rewrite preamble OpUndef -> OpConstantNull, PhysicalStorageBuffer OpBitcast -> OpConvertUToPtr,
  // and count any remaining 0-case switches.
  uint32_t bool_type_id = 0;
  uint32_t true_const_id = 0;
  size_t first_func_word = 0;
  bda_ptr_types.clear();
  nullable_types.clear();
  uint32_t remaining_zero_switch = 0;
  prev_was_sel_merge = false;

  for (size_t i = 5; i < words.size();) {
    const uint32_t header = words[i];
    const uint32_t op = header & 0xFFFFu;
    const uint32_t len = header >> 16;
    if (len == 0 || i + len > words.size())
      break;

    if (op == 20u && len == 2u && bool_type_id == 0)
      bool_type_id = words[i + 1];
    else if (op == 41u && len == 3u && true_const_id == 0)
      true_const_id = words[i + 2];
    else if (op == 54u && first_func_word == 0)
      first_func_word = i;

    if (first_func_word == 0 && is_nullable_op(op) && len >= 2u) {
      nullable_types.push_back(words[i + 1]);
    } else if (first_func_word == 0 && op == 1u && len == 3u &&
               std::find(nullable_types.begin(), nullable_types.end(), words[i + 1]) !=
                   nullable_types.end()) {
      words[i] = (3u << 16) | 46u;
    } else if (op == 32u && len >= 4u && words[i + 2] == 5349u) {
      bda_ptr_types.push_back(words[i + 1]);
    } else if (op == 124u && len == 4u &&
               std::find(bda_ptr_types.begin(), bda_ptr_types.end(), words[i + 1]) !=
                   bda_ptr_types.end()) {
      words[i] = (4u << 16) | 120u;
    } else if (op == 251u && len == 3u && prev_was_sel_merge) {
      ++remaining_zero_switch;
    }

    prev_was_sel_merge = (op == 247u && len == 3u);
    i += len;
  }

  if (remaining_zero_switch > 0 && first_func_word != 0) {
    const bool need_bool_type = (bool_type_id == 0);
    if (need_bool_type)
      bool_type_id = next_id++;
    const bool need_true_const = (true_const_id == 0);
    if (need_true_const)
      true_const_id = next_id++;

    std::vector<uint32_t> out;
    out.reserve(words.size() + 5 + size_t(remaining_zero_switch) * 12);
    out.insert(out.end(), words.begin(), words.begin() + 5);

    struct PhiFix {
      uint32_t body_label;
      uint32_t old_hdr;
      uint32_t new_hdr;
    };
    std::vector<PhiFix> phi_fixes;
    uint32_t cur_label = 0;

    for (size_t i = 5; i < words.size();) {
      if (i == first_func_word) {
        if (need_bool_type) {
          out.push_back((2u << 16) | 20u);
          out.push_back(bool_type_id);
        }
        if (need_true_const) {
          out.push_back((3u << 16) | 41u);
          out.push_back(bool_type_id);
          out.push_back(true_const_id);
        }
      }
      const uint32_t header = words[i];
      const uint32_t op = header & 0xFFFFu;
      const uint32_t len = header >> 16;
      if (op == 248u && len == 2u)
        cur_label = words[i + 1];

      if (op == 247u && len == 3u) {
        const size_t next_i = i + 3;
        if (next_i + 3 <= words.size() && (words[next_i] & 0xFFFFu) == 251u &&
            (words[next_i] >> 16) == 3u) {
          const uint32_t merge_id = words[i + 1];
          const uint32_t body_id = words[next_i + 2];
          if (body_id == merge_id) {
            out.push_back((2u << 16) | 249u);
            out.push_back(merge_id);
          } else {
            const uint32_t loop_hdr = next_id++;
            const uint32_t cont_blk = next_id++;
            out.push_back((2u << 16) | 249u);
            out.push_back(loop_hdr);
            out.push_back((2u << 16) | 248u);
            out.push_back(loop_hdr);
            out.push_back((4u << 16) | 246u);
            out.push_back(merge_id);
            out.push_back(cont_blk);
            out.push_back(0u);
            out.push_back((4u << 16) | 250u);
            out.push_back(true_const_id);
            out.push_back(body_id);
            out.push_back(cont_blk);
            out.push_back((2u << 16) | 248u);
            out.push_back(cont_blk);
            out.push_back((2u << 16) | 249u);
            out.push_back(loop_hdr);
            phi_fixes.push_back({body_id, cur_label, loop_hdr});
          }
          i = next_i + 3;
          continue;
        }
      }
      out.insert(out.end(), words.begin() + i, words.begin() + i + len);
      i += len;
    }

    if (!phi_fixes.empty()) {
      uint32_t out_label = 0;
      for (size_t i = 5; i < out.size();) {
        const uint32_t header = out[i];
        const uint32_t op = header & 0xFFFFu;
        const uint32_t len = header >> 16;
        if (op == 248u && len == 2u) {
          out_label = out[i + 1];
        } else if (op == 245u && len >= 5u) {
          for (const PhiFix &pf : phi_fixes) {
            if (out_label == pf.body_label) {
              for (size_t k = 3; k + 1 < len; k += 2) {
                if (out[i + k + 1] == pf.old_hdr)
                  out[i + k + 1] = pf.new_hdr;
              }
            }
          }
        }
        i += len;
      }
    }
    out[3] = next_id;
    words = std::move(out);
  }

  words[3] = next_id;
  spirv.resize(words.size() * 4);
  std::memcpy(spirv.data(), words.data(), spirv.size());
}

std::unique_ptr<plume::RenderShader> CreateCachedShader(plume::RenderDevice *device,
                                                        const ShaderCacheEntry &entry) {
  if (!device)
    return nullptr;

#if defined(PLUME_DEKO3D)
  // banjo-nx (2026-10-17): the deko3d back end's GLSL for uam, from the package in the NRO.
  if (IsDeko3D())
    return banjo::renut_gpu::deko3d::CreateGameShader(device, entry.hash);
#endif

  if (IsVulkan()) {
    const uint8_t *cache = Decompress(g_spirv, g_compressedSpirvCache, g_spirvCacheCompressedSize,
                                      g_spirvCacheDecompressedSize, "SPIR-V");
    if (!cache || entry.spirvSize == 0)
      return nullptr;
    const uint8_t *smol = cache + entry.spirvOffset;
    std::vector<uint8_t> spirv(smolv::GetDecodedBufferSize(smol, entry.spirvSize));
    if (spirv.empty() || !smolv::Decode(smol, entry.spirvSize, spirv.data(), spirv.size())) {
      RGPU_ERROR("SPIR-V decode failed for shader {:016X}", entry.hash);
      return nullptr;
    }
    PatchSpirvForVulkan(spirv);
    return device->createShader(spirv.data(), spirv.size(), "main",
                                plume::RenderShaderFormat::SPIRV);
  }

#if defined(_WIN32)
  const uint8_t *cache = Decompress(g_dxil, g_compressedDxilCache, g_dxilCacheCompressedSize,
                                    g_dxilCacheDecompressedSize, "DXIL");
  if (!cache || entry.dxilSize == 0)
    return nullptr;
  return device->createShader(cache + entry.dxilOffset, entry.dxilSize, "main",
                              plume::RenderShaderFormat::DXIL);
#else
  return nullptr;
#endif
}

uint64_t ShaderCacheHostBytes() {
  return g_decompressed_bytes.load(std::memory_order_relaxed);
}

void CaptureMissingShader(const uint8_t *container, bool pixel_shader, uint64_t hash) {
  std::lock_guard lock(g_capture_mutex);
  if (!g_captured.insert(hash).second)
    return;

  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  const size_t size = size_t(u32(header->virtualSize)) + u32(header->physicalSize);
  // banjo-nx: next to the executable (on the Switch, the NRO's folder), not the working directory.
  const std::filesystem::path dir = banjo::renut_gpu::NextToExecutable(REXCVAR_GET(renut_shader_capture_dir));
  const std::filesystem::path path =
      dir / fmt::format("{}_{:016x}.bin", pixel_shader ? "ps" : "vs", hash);

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream out(path, std::ios::binary);
  if (out)
    out.write(reinterpret_cast<const char *>(container), std::streamsize(size));
  if (!out) {
    RGPU_WARN("Shader {:016X} is not in the shader cache and could not be captured to {}", hash,
              path.string());
    return;
  }
  RGPU_WARN("Shader {:016X} is not in the shader cache: captured to {}, the next build adds it",
            hash, path.string());
}

} // namespace renut::gpu
