#include "dmc_startup_overlay.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <queue>
#include <system_error>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/string.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include "renut_engine/gpu/overlay/overlay_drawer.h"

#if defined(__GNUC__)
__asm__(
    ".section .rodata\n"
    ".balign 16\n"
    ".global g_dmc_installer_jpg_start\n"
    "g_dmc_installer_jpg_start:\n"
    ".incbin \"g:/!Project/!port/DmC - Devil May Cry/dmc_devil-nx/app/assets/instller.jpg\"\n"
    ".global g_dmc_installer_jpg_end\n"
    "g_dmc_installer_jpg_end:\n"
    ".balign 16\n"
    ".global g_dmc_intro_png_start\n"
    "g_dmc_intro_png_start:\n"
    ".incbin \"g:/!Project/!port/DmC - Devil May Cry/dmc_devil-nx/app/assets/intro.png\"\n"
    ".global g_dmc_intro_png_end\n"
    "g_dmc_intro_png_end:\n"
    ".previous\n"
);
extern "C" {
extern const uint8_t g_dmc_installer_jpg_start[];
extern const uint8_t g_dmc_installer_jpg_end[];
extern const uint8_t g_dmc_intro_png_start[];
extern const uint8_t g_dmc_intro_png_end[];
}
#else
static const uint8_t g_dmc_installer_jpg_start[1] = {0};
static const uint8_t* const g_dmc_installer_jpg_end = g_dmc_installer_jpg_start;
static const uint8_t g_dmc_intro_png_start[1] = {0};
static const uint8_t* const g_dmc_intro_png_end = g_dmc_intro_png_start;
#endif

namespace dmc {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kDmCTitleId = 0x43430824u;
constexpr uint32_t kContentTypeMarketplace = 0x00000002u;
constexpr uint32_t kContentTypeTitleUpdate = 0x000B0000u;
constexpr const char kProfileFolder[] = "0000000000000000";
constexpr const char kTitleFolder[] = "43430824";
constexpr const char kContentTypeFolder[] = "00000002";
constexpr float kIntroDurationSec = 3.0f;

fs::path SanitizePath(fs::path p) {
#if defined(__SWITCH__)
  std::string s = rex::path_to_utf8(p);
  if (s.rfind("sdmc:/", 0) == 0) {
    p = rex::to_path(s.substr(5));
  }
#endif
  return p;
}

uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

bool ProbeStfsPackage(const fs::path& path, uint32_t& out_title_id, uint32_t& out_content_type,
                      std::string& out_display_utf8, std::u16string& out_display_u16) {
  FILE* f = rex::filesystem::OpenFile(path, "rb");
  if (!f) {
    return false;
  }
  std::array<uint8_t, 0x520> hdr{};
  const size_t n = std::fread(hdr.data(), 1, hdr.size(), f);
  std::fclose(f);
  if (n < 0x500) {
    return false;
  }
  const std::string_view magic(reinterpret_cast<const char*>(hdr.data()), 4);
  if (magic != "LIVE" && magic != "PIRS" && magic != "CON ") {
    return false;
  }
  out_content_type = LoadBe32(hdr.data() + 0x344);
  out_title_id = LoadBe32(hdr.data() + 0x360);
  out_display_u16.clear();
  out_display_utf8.clear();
  for (size_t i = 0; i < 64; ++i) {
    const uint8_t* c = hdr.data() + 0x411 + i * 2;
    const uint16_t ch = uint16_t((c[0] << 8) | c[1]);
    if (ch == 0) {
      break;
    }
    out_display_u16.push_back(char16_t(ch));
  }
  if (!out_display_u16.empty()) {
    out_display_utf8 = rex::string::to_utf8(out_display_u16);
  }
  return true;
}

bool WriteContentHeaderFileDirect(const fs::path& user_data_root,
                                  const rex::system::xam::XCONTENT_AGGREGATE_DATA& data,
                                  uint32_t license_mask, fs::path& out_header_path) {
  const fs::path header_dir =
      user_data_root / kProfileFolder / kTitleFolder / "Headers" / kContentTypeFolder;
  std::error_code ec;
  fs::create_directories(header_dir, ec);
  if (ec) {
    return false;
  }
  out_header_path = header_dir / (data.file_name() + ".header");
  FILE* file = rex::filesystem::OpenFile(out_header_path, "wb");
  if (!file) {
    return false;
  }
  bool ok = std::fwrite(&data, 1, sizeof(data), file) == sizeof(data);
  if (ok && license_mask != 0) {
    ok = std::fwrite(&license_mask, 1, sizeof(license_mask), file) == sizeof(license_mask);
  }
  if (std::fclose(file) != 0) {
    ok = false;
  }
  return ok;
}

}  // namespace

DmCStartupDialog::DmCStartupDialog(rex::ui::ImGuiDrawer* drawer,
                                   std::function<rex::ui::ImmediateDrawer*()> immediate_drawer)
    : rex::ui::ImGuiDialog(drawer), immediate_drawer_(std::move(immediate_drawer)) {}

DmCStartupDialog::~DmCStartupDialog() {
  Stop();
}

void DmCStartupDialog::Stop() {
  stop_requested_.store(true, std::memory_order_release);
  if (installer_thread_.joinable()) {
    installer_thread_.join();
  }
  installing_tex_.reset();
  intro_tex_.reset();
  if (stage_ == Stage::kInstalling || stage_ == Stage::kIntro ||
      stage_ == Stage::kMissingUpdate) {
    rex::ui::SetGameInputBlocked(false);
    stage_ = Stage::kDone;
  }
}

void DmCStartupDialog::Begin(const rex::PathConfig& defaults,
                             std::function<void(rex::PathConfig)> on_done,
                             std::function<void()> on_cancel) {
  Stop();
  stop_requested_.store(false, std::memory_order_release);
  install_finished_.store(false, std::memory_order_release);
  installing_tex_tried_ = false;
  intro_tex_tried_ = false;
  missing_update_detail_.clear();
  missing_update_timer_started_ = false;

  paths_ = defaults;
  paths_.game_data_root = SanitizePath(paths_.game_data_root);
  paths_.user_data_root = SanitizePath(paths_.user_data_root);
  paths_.update_data_root = SanitizePath(paths_.update_data_root);
  on_done_ = std::move(on_done);
  on_cancel_ = std::move(on_cancel);

  rex::ui::SetGameInputBlocked(true);

  ScanQueuedPackages();

  if (!queue_.empty()) {
    stage_ = Stage::kInstalling;
    bytes_done_.store(0, std::memory_order_release);
    bytes_total_.store(1, std::memory_order_release);
    pkg_index_.store(0, std::memory_order_release);
    pkg_count_.store(int(queue_.size()), std::memory_order_release);
    installer_thread_ = std::thread([this] { InstallWorker(); });
  } else {
    EnterIntroStage();
  }
}

void DmCStartupDialog::ScanQueuedPackages() {
  queue_.clear();
  scanned_dirs_.clear();

  std::error_code ec;
  const fs::path exe_dir = SanitizePath(rex::filesystem::GetExecutableFolder());
  const fs::path user_dir = paths_.user_data_root.empty() ? exe_dir : paths_.user_data_root;

  // Ensure default dlc directory exists next to executable.
  fs::create_directories(exe_dir / "dlc", ec);
  ec.clear();

  struct Candidate {
    fs::path dir;
    bool is_update;
  };
  std::vector<Candidate> candidates = {
      {exe_dir / "Update", true},
      {exe_dir / "update", true},
      {exe_dir / "dlc", false},
      {exe_dir / "DLC", false},
  };
  if (user_dir != exe_dir) {
    candidates.push_back({user_dir / "dlc", false});
  }
#if defined(__SWITCH__)
  candidates.push_back({fs::path("/switch/dmc_devil/Update"), true});
  candidates.push_back({fs::path("/switch/dmc_devil/dlc"), false});
#endif

  auto normalize_key = [](const fs::path& p) {
    std::error_code kec;
    fs::path c = fs::weakly_canonical(p, kec);
    std::string s = rex::path_to_utf8(kec ? p.lexically_normal() : c);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) {
      s.pop_back();
    }
    return s;
  };

  std::vector<std::string> seen_dirs;
  std::vector<QueuedPackage> update_items;
  std::vector<QueuedPackage> dlc_items;

  for (const auto& cand : candidates) {
    if (!fs::is_directory(cand.dir, ec)) {
      ec.clear();
      continue;
    }
    const std::string key = normalize_key(cand.dir);
    if (std::find(seen_dirs.begin(), seen_dirs.end(), key) != seen_dirs.end()) {
      continue;
    }
    seen_dirs.push_back(key);
    scanned_dirs_.push_back(cand.dir);

    for (fs::recursive_directory_iterator it(cand.dir, fs::directory_options::skip_permission_denied, ec),
         end;
         it != end && !ec; it.increment(ec)) {
      if (!it->is_regular_file(ec)) {
        ec.clear();
        continue;
      }
      const fs::path p = it->path();
      const std::string fname = rex::path_to_utf8(p.filename());
      if (fname.empty() || fname[0] == '.' || fname == "installed.txt") {
        continue;
      }
      const uintmax_t fsize = fs::file_size(p, ec);
      if (ec || fsize == 0) {
        ec.clear();
        continue;
      }

      uint32_t title_id = 0;
      uint32_t content_type = 0;
      std::string display_utf8;
      std::u16string display_u16;
      if (!ProbeStfsPackage(p, title_id, content_type, display_utf8, display_u16)) {
        continue;
      }
      if (title_id != 0 && title_id != kDmCTitleId) {
        REXLOG_WARN("[dmc_installer] Skipping {} for foreign title {:08X}", fname, title_id);
        continue;
      }
      if (content_type != kContentTypeTitleUpdate && content_type != kContentTypeMarketplace) {
        REXLOG_WARN("[dmc_installer] Skipping unsupported STFS package {} (type {:08X})", fname,
                    content_type);
        continue;
      }

      QueuedPackage item;
      item.src_path = p;
      item.filename = fname;
      item.display_name = display_utf8.empty() ? fname : display_utf8;
      item.display_name_u16 = display_u16.empty() ? rex::path_to_utf16(p.filename()) : display_u16;
      item.is_title_update = (content_type == kContentTypeTitleUpdate);
      item.title_id = title_id;
      item.content_type = content_type;
      item.payload_bytes = uint64_t(fsize);

      if (item.is_title_update) {
        update_items.push_back(std::move(item));
      } else {
        dlc_items.push_back(std::move(item));
      }
    }
    ec.clear();
  }

  auto by_name = [](const QueuedPackage& a, const QueuedPackage& b) {
    return a.filename < b.filename;
  };
  std::sort(update_items.begin(), update_items.end(), by_name);
  std::sort(dlc_items.begin(), dlc_items.end(), by_name);

  queue_.reserve(update_items.size() + dlc_items.size());
  for (auto& u : update_items) {
    queue_.push_back(std::move(u));
  }
  for (auto& d : dlc_items) {
    queue_.push_back(std::move(d));
  }
}

void DmCStartupDialog::InstallWorker() {
  const fs::path exe_dir = SanitizePath(rex::filesystem::GetExecutableFolder());
  const fs::path user_root = paths_.user_data_root.empty() ? exe_dir : paths_.user_data_root;

  {
    std::lock_guard lock(status_mtx_);
    status_title_ = "Scanning DLC Packages...";
    status_detail_ = "Measuring package contents before installation...";
  }

  // Pass 1: measure total payload bytes across all queued packages for exact % progress.
  uint64_t total_bytes = 0;
  for (size_t i = 0; i < queue_.size(); ++i) {
    if (stop_requested_.load(std::memory_order_acquire)) {
      return;
    }
    QueuedPackage& pkg = queue_[i];
    auto device = std::make_unique<rex::filesystem::StfsContainerDevice>("", pkg.src_path);
    if (device->Initialize()) {
      uint64_t pkg_sum = 0;
      if (auto* root = device->ResolvePath("")) {
        std::queue<rex::filesystem::Entry*> q;
        q.push(root);
        while (!q.empty()) {
          auto* e = q.front();
          q.pop();
          for (auto& c : e->children()) {
            q.push(c.get());
          }
          if ((e->attributes() & rex::filesystem::kFileAttributeDirectory) == 0) {
            pkg_sum += uint64_t(e->size());
          }
        }
      }
      pkg.payload_bytes = std::max<uint64_t>(pkg_sum, 1);
    } else {
      REXLOG_ERROR("[dmc_installer] Failed to mount STFS package {}", pkg.filename);
      pkg.payload_bytes = 0;
    }
    total_bytes += pkg.payload_bytes;
  }

  bytes_total_.store(std::max<uint64_t>(total_bytes, 1), std::memory_order_release);

  constexpr size_t kChunkSize = 2 * 1024 * 1024;
  std::vector<uint8_t> copy_buf(kChunkSize);
  std::error_code ec;

  // Pass 2: extract with live byte-accurate progress, verify, write header, then remove raw source.
  for (size_t i = 0; i < queue_.size(); ++i) {
    if (stop_requested_.load(std::memory_order_acquire)) {
      return;
    }
    const QueuedPackage& pkg = queue_[i];
    pkg_index_.store(int(i + 1), std::memory_order_release);

    if (pkg.payload_bytes == 0) {
      continue;
    }

    {
      std::lock_guard lock(status_mtx_);
      status_title_ =
          pkg.is_title_update ? "Installing Title Update..." : "Installing DLC Content...";
      status_detail_ =
          fmt::format("[{}/{}] Preparing {}...", i + 1, queue_.size(), pkg.display_name);
    }

    auto device = std::make_unique<rex::filesystem::StfsContainerDevice>("", pkg.src_path);
    if (!device->Initialize()) {
      REXLOG_ERROR("[dmc_installer] Failed to initialize STFS {}", pkg.filename);
      continue;
    }

    const fs::path install_path =
        pkg.is_title_update
            ? (exe_dir / "Update")
            : (user_root / kProfileFolder / kTitleFolder / kContentTypeFolder / pkg.filename);
    fs::create_directories(install_path, ec);
    if (ec) {
      REXLOG_ERROR("[dmc_installer] Failed to create install dir {}: {}",
                   rex::path_to_utf8(install_path), ec.message());
      ec.clear();
      continue;
    }

    auto* root = device->ResolvePath("");
    if (!root) {
      REXLOG_ERROR("[dmc_installer] Missing root entry in {}", pkg.filename);
      continue;
    }

    bool pkg_ok = true;
    size_t files_extracted = 0;
    uint64_t pkg_bytes_done = 0;
    const double pkg_tot_mb = double(pkg.payload_bytes) / (1024.0 * 1024.0);

    std::queue<rex::filesystem::Entry*> q;
    q.push(root);
    while (!q.empty() && pkg_ok) {
      if (stop_requested_.load(std::memory_order_acquire)) {
        return;
      }
      auto* entry = q.front();
      q.pop();
      for (auto& child : entry->children()) {
        q.push(child.get());
      }

      const fs::path dest_path =
          install_path / rex::to_path(rex::string::utf8_fix_path_separators(entry->path()));
      if (entry->attributes() & rex::filesystem::kFileAttributeDirectory) {
        fs::create_directories(dest_path, ec);
        if (ec) {
          pkg_ok = false;
          ec.clear();
        }
        continue;
      }

      fs::create_directories(dest_path.parent_path(), ec);
      ec.clear();

      rex::filesystem::File* in_file = nullptr;
      if (entry->Open(rex::filesystem::FileAccess::kFileReadData, &in_file) != 0 || !in_file) {
        pkg_ok = false;
        break;
      }
      FILE* out_file = rex::filesystem::OpenFile(dest_path, "wb");
      if (!out_file) {
        in_file->Destroy();
        pkg_ok = false;
        break;
      }

      size_t remaining = entry->size();
      size_t offset = 0;
      const std::string entry_name = entry->name();
      while (remaining > 0) {
        if (stop_requested_.load(std::memory_order_acquire)) {
          std::fclose(out_file);
          in_file->Destroy();
          return;
        }
        const size_t to_read = std::min(remaining, kChunkSize);
        size_t bytes_read = 0;
        in_file->ReadSync(std::span<uint8_t>(copy_buf.data(), to_read), offset, &bytes_read);
        if (bytes_read == 0) {
          pkg_ok = false;
          break;
        }
        if (std::fwrite(copy_buf.data(), 1, bytes_read, out_file) != bytes_read) {
          pkg_ok = false;
          break;
        }
        offset += bytes_read;
        remaining -= bytes_read;
        pkg_bytes_done += uint64_t(bytes_read);

        const uint64_t done =
            bytes_done_.fetch_add(uint64_t(bytes_read), std::memory_order_relaxed) +
            uint64_t(bytes_read);
        const double pkg_done_mb = double(pkg_bytes_done) / (1024.0 * 1024.0);
        const double tot_done_mb = double(done) / (1024.0 * 1024.0);
        const double tot_mb =
            double(bytes_total_.load(std::memory_order_relaxed)) / (1024.0 * 1024.0);
        std::lock_guard lock(status_mtx_);
        status_detail_ =
            fmt::format("[{}/{}] {} ({:.1f} / {:.1f} MB) - {} [Total: {:.1f} / {:.1f} MB]",
                        i + 1, queue_.size(), pkg.display_name, pkg_done_mb, pkg_tot_mb, entry_name,
                        tot_done_mb, tot_mb);
      }
      if (std::fclose(out_file) != 0) {
        pkg_ok = false;
      }
      in_file->Destroy();
      if (pkg_ok) {
        ++files_extracted;
      }
    }

    uint32_t license_mask = 0;
    for (size_t li = 0; li < 0x10; ++li) {
      if (device->header().header.licenses[li].license_flags) {
        license_mask |= device->header().header.licenses[li].license_bits;
      }
    }
    if (license_mask == 0) {
      license_mask = 0xFFFFFFFFu;
    }
    // Release the STFS file handle BEFORE removing pkg.src_path.
    device.reset();

    pkg_ok = pkg_ok && files_extracted > 0;
    if (pkg_ok) {
      if (pkg.is_title_update) {
        pkg_ok = fs::exists(install_path / "default.xexp", ec);
        ec.clear();
        if (pkg_ok) {
          paths_.update_data_root = install_path;
        }
      } else {
        rex::system::xam::XCONTENT_AGGREGATE_DATA content_data{};
        content_data.device_id = static_cast<uint32_t>(rex::system::xam::DummyDeviceId::HDD);
        content_data.content_type = rex::system::XContentType::kMarketplaceContent;
        content_data.title_id = kDmCTitleId;
        content_data.xuid = 0;
        content_data.set_file_name(pkg.filename);
        content_data.set_display_name(pkg.display_name_u16);
        fs::path header_path;
        pkg_ok = WriteContentHeaderFileDirect(user_root, content_data, license_mask, header_path) &&
                 fs::exists(header_path, ec) &&
                 fs::file_size(header_path, ec) >= sizeof(content_data);
        ec.clear();
      }
    }

    if (pkg_ok) {
      {
        std::lock_guard lock(status_mtx_);
        status_detail_ =
            fmt::format("[{}/{}] Removing raw package: {}", i + 1, queue_.size(), pkg.filename);
      }
      fs::remove(pkg.src_path, ec);
      if (ec) {
        REXLOG_WARN("[dmc_installer] Installed {} but failed to remove raw file {}: {}",
                    pkg.filename, rex::path_to_utf8(pkg.src_path), ec.message());
        ec.clear();
      } else {
        REXLOG_INFO("[dmc_installer] Installed & removed raw package: {} ({})", pkg.filename,
                    pkg.display_name);
      }
    } else {
      REXLOG_ERROR("[dmc_installer] Failed to install {}; raw file preserved", pkg.filename);
    }
  }

  for (const auto& dir : scanned_dirs_) {
    fs::remove(dir / "installed.txt", ec);
    ec.clear();
  }

  bytes_done_.store(bytes_total_.load(std::memory_order_relaxed), std::memory_order_release);
  {
    std::lock_guard lock(status_mtx_);
    status_title_ = "Installation Complete!";
    status_detail_ = "All packages installed and raw files removed to save space.";
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(450));
  install_finished_.store(true, std::memory_order_release);
}

void DmCStartupDialog::EnterIntroStage() {
  intro_timer_started_ = false;
  stage_ = Stage::kIntro;
}

bool DmCStartupDialog::ResolveUpdateRoot() {
  std::error_code ec;
  auto has_default_xexp = [&ec](const fs::path& dir) {
    if (dir.empty()) {
      return false;
    }
    ec.clear();
    if (fs::is_regular_file(dir / "default.xexp", ec)) {
      return true;
    }
    ec.clear();
    return fs::is_regular_file(dir / "DEFAULT.XEXP", ec);
  };

  if (has_default_xexp(paths_.game_data_root)) {
    return true;
  }

  const fs::path exe_dir = SanitizePath(rex::filesystem::GetExecutableFolder());
  const fs::path candidates[] = {
      SanitizePath(paths_.update_data_root),
      exe_dir.empty() ? fs::path() : (exe_dir / "Update"),
      exe_dir.empty() ? fs::path() : (exe_dir / "update"),
      paths_.game_data_root.empty() ? fs::path() : (paths_.game_data_root / "Update"),
      paths_.game_data_root.empty() ? fs::path() : (paths_.game_data_root / "update"),
#if defined(__SWITCH__)
      fs::path("/switch/dmc_devil/Update"),
      fs::path("/switch/dmc_devil/update"),
#endif
  };
  for (const auto& cand : candidates) {
    if (has_default_xexp(cand)) {
      paths_.update_data_root = SanitizePath(cand);
      REXLOG_INFO("[dmc_startup] Using Title Update 1 directory: {}",
                  rex::path_to_utf8(paths_.update_data_root));
      return true;
    }
  }

  const fs::path primary_update =
      exe_dir.empty() ? fs::path("/switch/dmc_devil/Update") : (exe_dir / "Update");
  missing_update_detail_ = fmt::format("Checked: {}/default.xexp and {}/default.xexp",
                                       rex::path_to_utf8(primary_update),
                                       rex::path_to_utf8(paths_.game_data_root));
  REXLOG_ERROR("[dmc_startup] Title Update 1 (default.xexp) not found! {}", missing_update_detail_);
  return false;
}

void DmCStartupDialog::FinishAndResume() {
  if (!ResolveUpdateRoot()) {
    installing_tex_.reset();
    intro_tex_.reset();
    missing_update_timer_started_ = false;
    stage_ = Stage::kMissingUpdate;
    return;
  }

  installing_tex_.reset();
  intro_tex_.reset();
  stage_ = Stage::kDone;
  renut::gpu::OverlayDrawer::ClearLatest();
  rex::ui::SetGameInputBlocked(false);

  if (on_done_) {
    auto cb = std::move(on_done_);
    on_done_ = nullptr;
    cb(paths_);
  }
}

void DmCStartupDialog::OnDraw(ImGuiIO& io) {
  if (stage_ == Stage::kInstalling) {
    if (install_finished_.load(std::memory_order_acquire)) {
      if (installer_thread_.joinable()) {
        installer_thread_.join();
      }
      installing_tex_.reset();
      EnterIntroStage();
    } else {
      DrawInstalling(io);
      return;
    }
  }

  if (stage_ == Stage::kIntro) {
    DrawIntro(io);
    return;
  }

  if (stage_ == Stage::kMissingUpdate) {
    DrawMissingUpdate(io);
  }
}

void DmCStartupDialog::DrawInstalling(ImGuiIO& io) {
  rex::ui::ImmediateDrawer* immediate = immediate_drawer_ ? immediate_drawer_() : nullptr;
  if (!installing_tex_ && !installing_tex_tried_ && immediate) {
    installing_tex_tried_ = true;
    const size_t jpg_len = size_t(g_dmc_installer_jpg_end - g_dmc_installer_jpg_start);
    if (jpg_len > 0) {
      int w = 0, h = 0;
      std::vector<uint8_t> rgba =
          rex::ui::DecodeImageRGBA(g_dmc_installer_jpg_start, jpg_len, w, h);
      if (!rgba.empty() && w > 0 && h > 0) {
        installing_tex_ = immediate->CreateTexture(
            uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, false, rgba.data());
      }
    }
  }

  const float W = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : 1280.0f;
  const float H = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : 720.0f;
  const float scale = H / 720.0f;

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
  if (ImGui::Begin("##dmc_installing", nullptr, flags)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(W, H), IM_COL32(8, 8, 12, 255));
    if (installing_tex_) {
      dl->AddImage(reinterpret_cast<ImTextureID>(installing_tex_.get()), ImVec2(0.0f, 0.0f),
                   ImVec2(W, H));
    }

    const float panel_x0 = W * 0.05f;
    const float panel_x1 = W * 0.95f;
    const float panel_y0 = H * 0.72f;
    const float panel_y1 = H * 0.955f;
    const float pad_x = 24.0f * scale;
    const float pad_y = 14.0f * scale;

    dl->AddRectFilled(ImVec2(panel_x0, panel_y0), ImVec2(panel_x1, panel_y1),
                      IM_COL32(10, 10, 14, 218), 12.0f * scale);
    dl->AddRect(ImVec2(panel_x0, panel_y0), ImVec2(panel_x1, panel_y1),
                IM_COL32(215, 45, 45, 225), 12.0f * scale, 0, 2.0f * scale);

    std::string title, detail;
    {
      std::lock_guard lock(status_mtx_);
      title = status_title_;
      detail = status_detail_;
    }
    const uint64_t done = bytes_done_.load(std::memory_order_relaxed);
    const uint64_t total = std::max<uint64_t>(bytes_total_.load(std::memory_order_relaxed), 1);
    const float frac = std::clamp(float(double(done) / double(total)), 0.0f, 1.0f);
    const float pct = frac * 100.0f;

    ImFont* font = ImGui::GetFont();
    const float title_sz = 24.0f * scale;
    const float pct_sz = 26.0f * scale;
    const float detail_sz = 18.0f * scale;
    const float hint_sz = 16.0f * scale;

    char pct_buf[24];
    std::snprintf(pct_buf, sizeof(pct_buf), "%.1f%%", pct);

    const float text_x = panel_x0 + pad_x;
    const float right_x = panel_x1 - pad_x;
    const float top_y = panel_y0 + pad_y;

    dl->AddText(font, title_sz, ImVec2(text_x, top_y), IM_COL32(255, 238, 232, 255),
                title.c_str());
    const ImVec2 pct_size = font->CalcTextSizeA(pct_sz, FLT_MAX, 0.0f, pct_buf);
    dl->AddText(font, pct_sz, ImVec2(right_x - pct_size.x, top_y - 1.0f * scale),
                IM_COL32(255, 190, 72, 255), pct_buf);

    const float bar_y0 = top_y + title_sz + 10.0f * scale;
    const float bar_h = 22.0f * scale;
    const float bar_y1 = bar_y0 + bar_h;
    dl->AddRectFilled(ImVec2(text_x, bar_y0), ImVec2(right_x, bar_y1),
                      IM_COL32(36, 30, 38, 245), 6.0f * scale);
    if (frac > 0.001f) {
      const float fill_x1 = text_x + (right_x - text_x) * frac;
      dl->AddRectFilledMultiColor(ImVec2(text_x, bar_y0), ImVec2(fill_x1, bar_y1),
                                  IM_COL32(205, 32, 32, 255), IM_COL32(255, 168, 48, 255),
                                  IM_COL32(255, 168, 48, 255), IM_COL32(205, 32, 32, 255));
    }
    dl->AddRect(ImVec2(text_x, bar_y0), ImVec2(right_x, bar_y1),
                IM_COL32(255, 120, 64, 175), 6.0f * scale, 0, 1.5f * scale);

    const float detail_y = bar_y1 + 10.0f * scale;
    dl->AddText(font, detail_sz, ImVec2(text_x, detail_y), IM_COL32(224, 228, 236, 255),
                detail.c_str(), nullptr, right_x - text_x);

    const char* hint = "Tip: Press + and - together anytime to open Settings / Debug Menu";
    const float hint_y = panel_y1 - pad_y - hint_sz + 2.0f * scale;
    dl->AddText(font, hint_sz, ImVec2(text_x, hint_y), IM_COL32(240, 185, 150, 220), hint);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void DmCStartupDialog::DrawIntro(ImGuiIO& io) {
  rex::ui::ImmediateDrawer* immediate = immediate_drawer_ ? immediate_drawer_() : nullptr;
  if (!intro_tex_ && !intro_tex_tried_ && immediate) {
    intro_tex_tried_ = true;
    const size_t png_len = size_t(g_dmc_intro_png_end - g_dmc_intro_png_start);
    if (png_len > 0) {
      int w = 0, h = 0;
      std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(g_dmc_intro_png_start, png_len, w, h);
      if (!rgba.empty() && w > 0 && h > 0) {
        intro_w_ = uint32_t(w);
        intro_h_ = uint32_t(h);
        intro_tex_ = immediate->CreateTexture(
            intro_w_, intro_h_, rex::ui::ImmediateTextureFilter::kLinear, false, rgba.data());
      }
    }
  }

  const auto now = std::chrono::steady_clock::now();
  if (!intro_timer_started_) {
    intro_start_time_ = now;
    intro_timer_started_ = true;
  }
  const float elapsed =
      std::chrono::duration_cast<std::chrono::duration<float>>(now - intro_start_time_).count();
  if (elapsed >= kIntroDurationSec) {
    FinishAndResume();
    return;
  }

  float alpha = 1.0f;
  if (elapsed < 0.35f) {
    alpha = std::clamp(elapsed / 0.35f, 0.0f, 1.0f);
  } else if (elapsed > (kIntroDurationSec - 0.35f)) {
    alpha = std::clamp((kIntroDurationSec - elapsed) / 0.35f, 0.0f, 1.0f);
  }
  const ImU32 tint = IM_COL32(255, 255, 255, int(alpha * 255.0f));

  const float W = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : 1280.0f;
  const float H = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : 720.0f;

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
  if (ImGui::Begin("##dmc_intro", nullptr, flags)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(W, H), IM_COL32(0, 0, 0, 255));
    if (intro_tex_) {
      const float src_aspect = float(intro_w_) / float(std::max<uint32_t>(1, intro_h_));
      const float dst_aspect = W / H;
      ImVec2 p0(0.0f, 0.0f), p1(W, H);
      if (dst_aspect > src_aspect) {
        const float draw_w = H * src_aspect;
        p0.x = (W - draw_w) * 0.5f;
        p1.x = p0.x + draw_w;
      } else if (dst_aspect < src_aspect) {
        const float draw_h = W / src_aspect;
        p0.y = (H - draw_h) * 0.5f;
        p1.y = p0.y + draw_h;
      }
      dl->AddImage(reinterpret_cast<ImTextureID>(intro_tex_.get()), p0, p1, ImVec2(0.0f, 0.0f),
                   ImVec2(1.0f, 1.0f), tint);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void DmCStartupDialog::DrawMissingUpdate(ImGuiIO& io) {
  const auto now = std::chrono::steady_clock::now();
  if (!missing_update_timer_started_) {
    missing_update_start_ = now;
    missing_update_timer_started_ = true;
  }
  const float elapsed =
      std::chrono::duration_cast<std::chrono::duration<float>>(now - missing_update_start_)
          .count();

  if (elapsed >= 0.5f &&
      (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
       ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
       ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) ||
       ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
       ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
    stage_ = Stage::kDone;
    renut::gpu::OverlayDrawer::ClearLatest();
    rex::ui::SetGameInputBlocked(false);
    if (on_cancel_) {
      auto cb = std::move(on_cancel_);
      on_cancel_ = nullptr;
      cb();
    }
    return;
  }

  const float W = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : 1280.0f;
  const float H = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : 720.0f;
  const float scale = H / 720.0f;

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
  if (ImGui::Begin("##dmc_missing_update", nullptr, flags)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(W, H), IM_COL32(10, 8, 12, 255));

    const float box_x0 = W * 0.10f;
    const float box_x1 = W * 0.90f;
    const float box_y0 = H * 0.18f;
    const float box_y1 = H * 0.82f;
    const float pad_x = 36.0f * scale;
    const float wrap_w = (box_x1 - box_x0) - pad_x * 2.0f;

    dl->AddRectFilled(ImVec2(box_x0, box_y0), ImVec2(box_x1, box_y1), IM_COL32(18, 16, 22, 245),
                      14.0f * scale);
    dl->AddRect(ImVec2(box_x0, box_y0), ImVec2(box_x1, box_y1), IM_COL32(220, 50, 50, 235),
                14.0f * scale, 0, 2.5f * scale);

    ImFont* font = ImGui::GetFont();
    const float title_sz = 30.0f * scale;
    const float body_sz = 21.0f * scale;
    const float small_sz = 17.0f * scale;

    float y = box_y0 + 32.0f * scale;
    const float x = box_x0 + pad_x;

    dl->AddText(font, title_sz, ImVec2(x, y), IM_COL32(255, 96, 96, 255),
                "Title Update 1 (Update/default.xexp) Missing");
    y += title_sz + 22.0f * scale;

    dl->AddText(font, body_sz, ImVec2(x, y), IM_COL32(242, 242, 246, 255),
                "This build requires DmC Title Update 1 (v0.0.1.7) to boot and load DLC.", nullptr,
                wrap_w);
    y += body_sz * 2.1f;

    dl->AddText(font, body_sz, ImVec2(x, y), IM_COL32(255, 210, 120, 255),
                "How to fix on your Nintendo Switch SD card:", nullptr, wrap_w);
    y += body_sz * 1.6f;

    dl->AddText(font, body_sz, ImVec2(x, y), IM_COL32(228, 232, 240, 255),
                "1. Copy the 'Update' folder from the release package to:\n"
                "   sdmc:/switch/dmc_devil/Update/  (must contain default.xexp)\n"
                "2. Or drop the raw Xbox 360 Title Update file into 'Update/' or 'dlc/' for auto-install.",
                nullptr, wrap_w);
    y += body_sz * 4.6f;

    if (!missing_update_detail_.empty()) {
      dl->AddText(font, small_sz, ImVec2(x, y), IM_COL32(180, 185, 198, 220),
                  missing_update_detail_.c_str(), nullptr, wrap_w);
    }

    dl->AddText(font, body_sz, ImVec2(x, box_y1 - 48.0f * scale), IM_COL32(255, 190, 90, 255),
                "Press A, B, or + to exit");
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

}  // namespace dmc
