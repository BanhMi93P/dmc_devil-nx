// Fast UE3 Xbox 360 shader container extractor for DmC - Devil May Cry.
// Scans raw binaries (GlobalShaderCache-Xbox360.bin, renut_image.bin) and UE3 .xxx packages
// (both whole-file LZX compressed 0x9E2A83C1+0x20000 and PKG_StoreCompressed chunked LZX),
// writing unique Xenos containers as {vs,ps}_<xxh3_64>.bin.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#include "lzx.h"
#include "mspack.h"
#include "system.h"
#include "xxhash.h"

namespace fs = std::filesystem;

namespace {

inline uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline int32_t Be32s(const uint8_t* p) { return static_cast<int32_t>(Be32(p)); }

struct MemReader {
  const uint8_t* data;
  size_t size;
  size_t pos;
};

struct MemWriter {
  uint8_t* data;
  size_t capacity;
  size_t pos;
};

struct MemFile {
  bool is_write;
  MemReader r;
  MemWriter w;
};

struct mspack_file* MemOpen(struct mspack_system*, const char*, int) { return nullptr; }
void MemClose(struct mspack_file*) {}
int MemRead(struct mspack_file* file, void* buffer, int bytes) {
  auto* mf = reinterpret_cast<MemFile*>(file);
  if (!mf || mf->is_write || bytes < 0) return -1;
  size_t avail = (mf->r.pos < mf->r.size) ? (mf->r.size - mf->r.pos) : 0;
  size_t n = std::min<size_t>(static_cast<size_t>(bytes), avail);
  if (n > 0) {
    std::memcpy(buffer, mf->r.data + mf->r.pos, n);
    mf->r.pos += n;
  }
  return static_cast<int>(n);
}
int MemWrite(struct mspack_file* file, void* buffer, int bytes) {
  auto* mf = reinterpret_cast<MemFile*>(file);
  if (!mf || !mf->is_write || bytes < 0) return -1;
  size_t n = static_cast<size_t>(bytes);
  if (mf->w.pos + n > mf->w.capacity) return -1;
  if (n > 0) {
    std::memcpy(mf->w.data + mf->w.pos, buffer, n);
    mf->w.pos += n;
  }
  return static_cast<int>(n);
}
int MemSeek(struct mspack_file* file, off_t offset, int mode) {
  auto* mf = reinterpret_cast<MemFile*>(file);
  if (!mf) return -1;
  size_t sz = mf->is_write ? mf->w.capacity : mf->r.size;
  size_t cur = mf->is_write ? mf->w.pos : mf->r.pos;
  int64_t next = 0;
  if (mode == MSPACK_SYS_SEEK_START) next = offset;
  else if (mode == MSPACK_SYS_SEEK_CUR) next = static_cast<int64_t>(cur) + offset;
  else if (mode == MSPACK_SYS_SEEK_END) next = static_cast<int64_t>(sz) + offset;
  else return -1;
  if (next < 0 || static_cast<size_t>(next) > sz) return -1;
  if (mf->is_write) mf->w.pos = static_cast<size_t>(next);
  else mf->r.pos = static_cast<size_t>(next);
  return 0;
}
off_t MemTell(struct mspack_file* file) {
  auto* mf = reinterpret_cast<MemFile*>(file);
  if (!mf) return 0;
  return static_cast<off_t>(mf->is_write ? mf->w.pos : mf->r.pos);
}
void MemMessage(struct mspack_file*, const char*, ...) {}
void* MemAlloc(struct mspack_system*, size_t bytes) { return std::malloc(bytes); }
void MemFree(void* ptr) { std::free(ptr); }
void MemCopy(void* src, void* dest, size_t bytes) { std::memcpy(dest, src, bytes); }

struct mspack_system g_mem_sys = {
    MemOpen, MemClose, MemRead, MemWrite, MemSeek, MemTell, MemMessage, MemAlloc, MemFree, MemCopy, nullptr,
};

bool DecompressLzxChunk(const uint8_t* data, size_t size, size_t offset, std::vector<uint8_t>& out, size_t* next_off = nullptr) {
  if (offset + 16 > size) return false;
  uint32_t magic = Be32(data + offset);
  if (magic != 0x9E2A83C1u) return false;
  uint32_t block_size = Be32(data + offset + 4);
  uint32_t total_comp = Be32(data + offset + 8);
  uint32_t total_uncomp = Be32(data + offset + 12);
  if (block_size == 0 || block_size > 0x100000 || total_uncomp == 0 || total_uncomp > 256u * 1024u * 1024u) {
    return false;
  }
  uint32_t num_blocks = (total_uncomp + block_size - 1) / block_size;
  size_t table_off = offset + 16;
  size_t payload_off = table_off + size_t(num_blocks) * 8;
  if (payload_off > size) return false;

  struct Blk { uint32_t comp; uint32_t uncomp; };
  std::vector<Blk> blocks(num_blocks);
  size_t sum_comp = 0, sum_uncomp = 0;
  for (uint32_t i = 0; i < num_blocks; ++i) {
    blocks[i].comp = Be32(data + table_off + i * 8);
    blocks[i].uncomp = Be32(data + table_off + i * 8 + 4);
    sum_comp += blocks[i].comp;
    sum_uncomp += blocks[i].uncomp;
  }
  if (payload_off + sum_comp > size) return false;

  out.resize(sum_uncomp);
  std::vector<uint8_t> stripped;
  size_t in_cursor = payload_off;
  size_t out_cursor = 0;
  for (uint32_t i = 0; i < num_blocks; ++i) {
    uint32_t csz = blocks[i].comp;
    uint32_t usz = blocks[i].uncomp;
    if (usz == 0 || in_cursor + csz > size || out_cursor + usz > out.size()) return false;

    // Xbox 360 XMemCompress frames each <=32KB LZX sub-block with either:
    //   0xFF <u16_be uncomp> <u16_be comp> <comp bytes>
    // or <u16_be comp> <comp bytes> (uncomp = 0x8000), terminated by comp == 0 or end of block.
    stripped.clear();
    const uint8_t* bp = data + in_cursor;
    size_t pos = 0;
    size_t u_acc = 0;
    while (pos + 2 <= csz && u_acc < usz) {
      uint32_t sub_u = 0x8000;
      uint32_t sub_c = 0;
      if (bp[pos] == 0xFF) {
        if (pos + 5 > csz) break;
        sub_u = (uint32_t(bp[pos + 1]) << 8) | bp[pos + 2];
        sub_c = (uint32_t(bp[pos + 3]) << 8) | bp[pos + 4];
        pos += 5;
      } else {
        sub_c = (uint32_t(bp[pos]) << 8) | bp[pos + 1];
        pos += 2;
      }
      if (sub_c == 0 || pos + sub_c > csz) break;
      stripped.insert(stripped.end(), bp + pos, bp + pos + sub_c);
      pos += sub_c;
      u_acc += sub_u;
    }

    const uint8_t* feed_ptr = stripped.empty() ? bp : stripped.data();
    size_t feed_sz = stripped.empty() ? csz : stripped.size();

    MemFile fin{};
    fin.is_write = false;
    fin.r = {feed_ptr, feed_sz, 0};
    MemFile fout{};
    fout.is_write = true;
    fout.w = {out.data() + out_cursor, usz, 0};

    int wbits = 17;
    if (block_size <= 0x8000) wbits = 15;
    else if (block_size <= 0x10000) wbits = 16;
    struct lzxd_stream* lzx = lzxd_init(&g_mem_sys, reinterpret_cast<struct mspack_file*>(&fin),
                                        reinterpret_cast<struct mspack_file*>(&fout), wbits, 0, 0x8000,
                                        static_cast<off_t>(usz), 0);
    if (!lzx) return false;
    int err = lzxd_decompress(lzx, static_cast<off_t>(usz));
    lzxd_free(lzx);
    if (err != MSPACK_ERR_OK) return false;

    in_cursor += csz;
    out_cursor += usz;
  }
  if (next_off) *next_off = in_cursor;
  return true;
}

struct Stats {
  uint64_t vs_new = 0;
  uint64_t ps_new = 0;
  uint64_t hits_total = 0;
  uint64_t files_scanned = 0;
  uint64_t chunks_ok = 0;
  uint64_t chunks_fail = 0;
};

void ScanBuffer(const uint8_t* buf, size_t len, const fs::path& out_dir,
                std::unordered_set<uint64_t>& seen_hashes, Stats& st) {
  if (len < 0x24) return;
  for (size_t i = 0; i + 0x24 <= len; ++i) {
    if (buf[i] != 0x10 || buf[i + 1] != 0x2A || buf[i + 2] != 0x11) continue;
    uint32_t flags = Be32(buf + i);
    uint32_t virt = Be32(buf + i + 4);
    uint32_t phys = Be32(buf + i + 8);
    uint32_t ct = Be32(buf + i + 0x10);
    uint32_t dt = Be32(buf + i + 0x14);
    uint32_t so = Be32(buf + i + 0x10 + 8);
    if (virt < 0x24 || virt >= 0x10000 || phys == 0 || phys >= 0x100000) continue;
    if (i + virt + phys > len) continue;
    if (ct >= virt || dt >= virt || so < 0x24 || so + 8 > virt) continue;
    uint32_t poff = Be32(buf + i + so);
    uint32_t psize = Be32(buf + i + so + 4);
    if (poff + psize != phys) continue;

    size_t blob_sz = size_t(virt) + size_t(phys);
    uint64_t h = XXH3_64bits(buf + i, blob_sz);
    st.hits_total++;
    bool is_vs = (flags & 1u) != 0;
    uint64_t key = h ^ (is_vs ? 0x9E3779B97F4A7C15ull : 0ull);
    if (seen_hashes.insert(key).second) {
      char name[64];
      std::snprintf(name, sizeof(name), "%s_%016llx.bin", is_vs ? "vs" : "ps",
                    static_cast<unsigned long long>(h));
      fs::path dst = out_dir / name;
      if (!fs::exists(dst)) {
        FILE* fp = std::fopen(dst.string().c_str(), "wb");
        if (fp) {
          std::fwrite(buf + i, 1, blob_sz, fp);
          std::fclose(fp);
        }
      }
      if (is_vs) st.vs_new++;
      else st.ps_new++;
    }
    i += blob_sz - 1;
  }
}

void ProcessXxxFile(const fs::path& path, const fs::path& out_dir,
                    std::unordered_set<uint64_t>& seen_hashes, Stats& st) {
  FILE* fp = std::fopen(path.string().c_str(), "rb");
  if (!fp) return;
  std::fseek(fp, 0, SEEK_END);
  long fsz = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  if (fsz <= 0) {
    std::fclose(fp);
    return;
  }
  std::vector<uint8_t> data(static_cast<size_t>(fsz));
  if (std::fread(data.data(), 1, data.size(), fp) != data.size()) {
    std::fclose(fp);
    return;
  }
  std::fclose(fp);
  st.files_scanned++;

  // Always scan raw bytes first (uncompressed packages / GlobalShaderCache / binaries).
  ScanBuffer(data.data(), data.size(), out_dir, seen_hashes, st);

  if (data.size() < 32 || Be32(data.data()) != 0x9E2A83C1u) return;

  uint32_t word1 = Be32(data.data() + 4);
  // Case 1: Whole-file LZX compressed chunk(s) (block_size = 0x20000 or 0x10000).
  if (word1 == 0x00020000u || word1 == 0x00010000u || word1 == 0x00008000u) {
    std::vector<uint8_t> decomp;
    size_t off = 0;
    while (off + 16 <= data.size() && Be32(data.data() + off) == 0x9E2A83C1u) {
      size_t next_off = 0;
      if (DecompressLzxChunk(data.data(), data.size(), off, decomp, &next_off)) {
        st.chunks_ok++;
        ScanBuffer(decomp.data(), decomp.size(), out_dir, seen_hashes, st);
        if (next_off <= off) break;
        off = next_off;
      } else {
        st.chunks_fail++;
        break;
      }
    }
    return;
  }

  // Case 2: Standard UE3 FPackageFileSummary (DmC ver=845, lic=4).
  size_t pos = 12;
  if (pos + 4 > data.size()) return;
  int32_t flen = Be32s(data.data() + pos);
  pos += 4;
  if (flen > 0) pos += static_cast<size_t>(flen);
  else if (flen < 0) pos += static_cast<size_t>(-flen) * 2;
  if (pos + 48 > data.size()) return;

  uint32_t pkg_flags = Be32(data.data() + pos);
  pos += 4;
  if (!(pkg_flags & 0x02000000u)) return;  // PKG_StoreCompressed not set

  // Skip NameCount..DependsOffset (7*4), ImportExportGuidsOffset (4), Import/ExportGuidsCount (8), ThumbnailTableOffset (4), Guid (16) = 60 bytes
  pos += 60;
  if (pos + 4 > data.size()) return;
  uint32_t gen_count = Be32(data.data() + pos);
  pos += 4;
  if (gen_count > 64 || pos + size_t(gen_count) * 12 + 16 > data.size()) return;
  pos += size_t(gen_count) * 12;

  // EngineVersion (4), CookerVersion (4), CompressionFlags (4), CompressedChunks.Count (4)
  pos += 8;
  uint32_t comp_flags = Be32(data.data() + pos);
  uint32_t chunk_count = Be32(data.data() + pos + 4);
  pos += 8;
  if (comp_flags != 4 || chunk_count == 0 || chunk_count > 4096) return;
  if (pos + size_t(chunk_count) * 16 > data.size()) return;

  std::vector<uint8_t> decomp;
  for (uint32_t c = 0; c < chunk_count; ++c) {
    uint32_t comp_off = Be32(data.data() + pos + c * 16 + 8);
    uint32_t comp_sz = Be32(data.data() + pos + c * 16 + 12);
    if (comp_sz == 0 || size_t(comp_off) + comp_sz > data.size()) {
      st.chunks_fail++;
      continue;
    }
    if (DecompressLzxChunk(data.data(), data.size(), comp_off, decomp)) {
      st.chunks_ok++;
      ScanBuffer(decomp.data(), decomp.size(), out_dir, seen_hashes, st);
    } else {
      st.chunks_fail++;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  int selftest = 0;
  MSPACK_SYS_SELFTEST(selftest);
  if (selftest != MSPACK_ERR_OK) {
    std::fprintf(stderr, "libmspack selftest failed: %d\n", selftest);
    return 1;
  }
  if (argc < 3) {
    std::fprintf(stderr, "usage: dmc_ue3_shader_scan <out_dir> <file_or_dir> [...]\n");
    return 2;
  }
  fs::path out_dir = argv[1];
  fs::create_directories(out_dir);

  std::unordered_set<uint64_t> seen_hashes;
  // Preload already-existing containers in out_dir so totals are accurate.
  for (const auto& entry : fs::directory_iterator(out_dir)) {
    if (!entry.is_regular_file()) continue;
    std::string fn = entry.path().filename().string();
    if (fn.size() == 23 && fn.ends_with(".bin") && (fn.starts_with("vs_") || fn.starts_with("ps_"))) {
      uint64_t h = std::strtoull(fn.substr(3, 16).c_str(), nullptr, 16);
      bool is_vs = fn.starts_with("vs_");
      seen_hashes.insert(h ^ (is_vs ? 0x9E3779B97F4A7C15ull : 0ull));
    }
  }

  Stats st{};
  for (int i = 2; i < argc; ++i) {
    fs::path p = argv[i];
    if (!fs::exists(p)) continue;
    if (fs::is_regular_file(p)) {
      ProcessXxxFile(p, out_dir, seen_hashes, st);
    } else if (fs::is_directory(p)) {
      for (const auto& entry : fs::recursive_directory_iterator(p)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext == ".xxx" || ext == ".bin" || ext == ".upk") {
          ProcessXxxFile(entry.path(), out_dir, seen_hashes, st);
        }
      }
    }
  }

  std::printf("dmc_ue3_shader_scan: files=%llu chunks_ok=%llu chunks_fail=%llu hits=%llu new_vs=%llu new_ps=%llu total_unique=%zu\n",
              static_cast<unsigned long long>(st.files_scanned),
              static_cast<unsigned long long>(st.chunks_ok),
              static_cast<unsigned long long>(st.chunks_fail),
              static_cast<unsigned long long>(st.hits_total),
              static_cast<unsigned long long>(st.vs_new),
              static_cast<unsigned long long>(st.ps_new),
              seen_hashes.size());
  return 0;
}
