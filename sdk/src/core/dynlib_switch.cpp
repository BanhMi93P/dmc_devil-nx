/**
 * @file        rex/core/dynlib_switch.cpp
 * @brief       DynamicLibrary backend for Horizon: there is no dynamic loading
 *
 * An NRO cannot load libraries at run time: dlopen does not exist and everything is
 * linked statically. This backend does not fail loudly; it returns "not loaded" for
 * any path, because the SDK's two callers already handle that case:
 *
 *   - gpu_plugin_loader.cpp, which on Switch must be resolved through static
 *     registration (see rex::system::LoadGpuPlugin)
 *   - the optional RenderDoc and SPIRV-Tools loaders, which make no sense on the
 *     console
 *
 * If a third caller that depends on dlopen is ever added, it will show up here as a
 * Load() that returns false, not as a link error.
 */

#include <rex/platform.h>
#include <rex/platform/dynlib.h>

#include <cstring>
#include <string>
#include <vector>

static_assert(REX_PLATFORM_SWITCH, "This file is Nintendo Switch-only");

namespace {
struct StaticModuleEntry {
  std::string name;
  void* reg_fn;
  void* info_fn;
};

std::vector<StaticModuleEntry>& StaticModules() {
  static std::vector<StaticModuleEntry> modules;
  return modules;
}
}  // namespace

extern "C" void RexSwitchRegisterStaticModule(const char* name, void* reg_fn, void* info_fn) {
  if (!name) return;
  for (auto& m : StaticModules()) {
    if (m.name == name) {
      m.reg_fn = reg_fn;
      m.info_fn = info_fn;
      return;
    }
  }
  StaticModules().push_back({name, reg_fn, info_fn});
}

namespace rex::platform {

DynamicLibrary::~DynamicLibrary() {
  Close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool DynamicLibrary::Load(const std::filesystem::path& path, SymbolResolution mode) {
  (void)mode;
  Close();
  const std::string fname = path.filename().string();
  for (auto& m : StaticModules()) {
    if (fname == m.name) {
      handle_ = &m;
      return true;
    }
  }
  return false;
}

void DynamicLibrary::Close() {
  handle_ = nullptr;
}

void* DynamicLibrary::GetRawSymbol(const char* name) const {
  if (!handle_ || !name) return nullptr;
  const auto* m = static_cast<const StaticModuleEntry*>(handle_);
  if (std::strcmp(name, "ReXModule_Register") == 0) return m->reg_fn;
  if (std::strcmp(name, "ReXModule_GetImageInfo") == 0) return m->info_fn;
  return nullptr;
}

}  // namespace rex::platform
