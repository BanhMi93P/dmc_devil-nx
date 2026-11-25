#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/rex_app.h>
#include <rex/ui/imgui_dialog.h>

struct ImGuiIO;

namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
}  // namespace rex::ui

namespace dmc {

class DmCStartupDialog final : public rex::ui::ImGuiDialog {
 public:
  DmCStartupDialog(rex::ui::ImGuiDrawer* drawer,
                   std::function<rex::ui::ImmediateDrawer*()> immediate_drawer);
  ~DmCStartupDialog() override;

  void Begin(const rex::PathConfig& defaults,
             std::function<void(rex::PathConfig)> on_done,
             std::function<void()> on_cancel = nullptr);
  void Stop();

  bool WantsFrame() const override {
    return stage_ == Stage::kInstalling || stage_ == Stage::kIntro ||
           stage_ == Stage::kMissingUpdate;
  }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class Stage {
    kIdle,
    kInstalling,
    kIntro,
    kMissingUpdate,
    kDone,
  };

  struct QueuedPackage {
    std::filesystem::path src_path;
    std::string filename;
    std::string display_name;
    std::u16string display_name_u16;
    bool is_title_update = false;
    uint32_t title_id = 0;
    uint32_t content_type = 0;
    uint64_t payload_bytes = 0;
  };

  void ScanQueuedPackages();
  void InstallWorker();
  void EnterIntroStage();
  bool ResolveUpdateRoot();
  void FinishAndResume();

  void DrawInstalling(ImGuiIO& io);
  void DrawIntro(ImGuiIO& io);
  void DrawMissingUpdate(ImGuiIO& io);

  std::function<rex::ui::ImmediateDrawer*()> immediate_drawer_;
  rex::PathConfig paths_{};
  std::function<void(rex::PathConfig)> on_done_;
  std::function<void()> on_cancel_;

  Stage stage_ = Stage::kIdle;

  // Stage 1: Asura-style STFS DLC & Title Update installer
  std::vector<QueuedPackage> queue_;
  std::vector<std::filesystem::path> scanned_dirs_;
  std::thread installer_thread_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> install_finished_{false};
  std::atomic<uint64_t> bytes_done_{0};
  std::atomic<uint64_t> bytes_total_{1};
  std::atomic<int> pkg_index_{0};
  std::atomic<int> pkg_count_{0};
  std::mutex status_mtx_;
  std::string status_title_ = "Preparing DLC Installer...";
  std::string status_detail_ = "Scanning dlc and Update folders...";
  std::unique_ptr<rex::ui::ImmediateTexture> installing_tex_;
  bool installing_tex_tried_ = false;

  // Stage 2: Embedded 3-second startup splash (intro.png)
  std::unique_ptr<rex::ui::ImmediateTexture> intro_tex_;
  bool intro_tex_tried_ = false;
  uint32_t intro_w_ = 1280;
  uint32_t intro_h_ = 720;
  std::chrono::steady_clock::time_point intro_start_time_{};
  bool intro_timer_started_ = false;

  // Stage 3: Actionable error screen when Title Update 1 (default.xexp) is missing
  std::string missing_update_detail_;
  std::chrono::steady_clock::time_point missing_update_start_{};
  bool missing_update_timer_started_ = false;
};

}  // namespace dmc
