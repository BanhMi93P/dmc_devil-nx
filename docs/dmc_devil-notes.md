# DmC: Devil May Cry (`dmc_devil-nx`) — Porting Notes

## Target & Binary Identity
- **Title:** DmC: Devil May Cry (Xbox 360, Unreal Engine 3)
- **Title ID:** `43430824`
- **Media ID:** `021BEF7B`
- **Version:** `0.0.0.7` (Base XEX; optional Title Update `default.xexp` + `Update/` + `dlc/`)
- **Secondary XEX Module:** `binaries/ntjobcode/xenon/submission/ntjobcode.xex` (`515` functions, base `0x88000000`, code `0x88010000-0x88027ADC`)
  - On PC: loaded dynamically as `renut_NTJobCode.dll`.
  - On Switch: statically linked into `renut.nro` via `dynlib_switch.cpp` static module registration (`ReXModule_NTJobCode_Register` / `ReXModule_NTJobCode_GetImageInfo`).

## Shader Translation
- **Total Shaders:** `35,734 / 35,734` translated (`0` failures with `RENUT_VERTEX_INPUTS_MORPH`).
- **Required Shader Macro:** `RENUT_VERTEX_INPUTS_MORPH` (maps `POSITION5..POSITION7` to SPIR-V locations `23..27`).
- **UE3 Color Grading LUT Pattern:** `FCombineLUTs` renders `16x512` `8_8_8_8` (`32`-row Xenos tile stride per slice) resolved into `BasePage` `0x103E0`, then `ps_3E71AAA08AC3C717` samples the same page as `16x16x16` `stacked` 2D array. Handled in `textures.cpp` via `StackedResolveSource` / `CopyStackedResolve`.

## PC Verification (Gate B — Vulkan)
- **Run `boot14_lut_vk` (100s Mission 1):**
  - `59.93 FPS`, `1875` SDL audio frames / `0` silence frames.
  - `0` `[flash]` warnings, `0` `Resolve through` warnings, `0` `DRAWS NOT DRAWN`, `0` missing shaders.
  - CPU self-checks: `0` differences across `1,578,193,359` `r1` mirror calls, `327,577,917` constant loads, and `1,767,966` stack forwards.
- **Run `cg13_dmc` (90s CG-13 Census + Sampler Fix):**
  - `0` `Sampler table full` warnings, `0` `[flash]`, `0` `Resolve through`.
  - CG-13 Census (`banjo_diag_runtime_census=true`): `1,040,425` flush-mode switches/sec in game code; `87.2%` of denormal hits (`12,182 / 13,963`) are `banjo::FormsSelfCheck` startup tests and the rest are VMX instructions (`vmsum4fp128`/`vmulfp128`, flushed on 360 hardware) + benign `lfs`.
  - **Verdict:** Keep **`BLUR_FPCR_ONE_MODE=ON`** for Nintendo Switch.

## Nintendo Switch (`deko3d`) Defaults
- UE3 safety defaults applied in `app/katamari_switch_defaults.toml`:
  - `banjo_renderer = "renut"`, `renut_gpu_backend = "deko3d"`
  - `banjo_deko3d_native_16bit = false`, `banjo_renut_dynamic_texture_rows = false`, `banjo_renut_keep_unused_targets = false`, `blur_renut_texture_pool = false`, `banjo_renut_stream_redeem = false`
  - `banjo_async_present = false`, `blur_small_alloc_mb = 0`, `banjo_system_core = 0`, `banjo_renut_core_layout = false`
- Release shader packaging (`BANJO_RENUT_RELEASE_SHADERS=ON`): embeds shader metadata table (`shader_cache_switch.cpp`) in the NRO and reads `deko3d_glsl.bin` from the NRO directory on SD.
- **Verified Switch Package (`output/dmc_devil/`):**
  - `dmc_devil.nro`: `109,074,659` bytes (`104.02 MiB`), `NRO0` + `ASET`, NACP Title `DmC - Devil May Cry`, Author `Bánh Mì`, Version `1.0.0`, embedded `30,891` B JPEG icon.
  - `deko3d_glsl.bin`: `111,928,528` bytes (`106.74 MiB`), `BDKG` v1, `35,734 / 35,734` shaders (`0` `.spv` failures), `131,072` B zstd dictionary (round-trip verified).
  - Tooling/Switch fixes: `BANJO_DEKO3D_ONLY=1` (`XenosRecomp/main.cpp`), parallel `deko3d_glsl_opt.py`, in-memory `zstandard` in `deko3d_shader_package.py`, optional `REX_HAS_OPUS` (`libopus`), `memalign` / `_flockfile` newlib fixes, `rectangle_list_gs.hlsl` `EMIT_RECTANGLE` for `uam`, and `patch_nacp_utf8.py` for UTF-8 NACP on Windows.

## Switch Runtime Fixes
- **`DMC-07` — Startup TLS exception crash (`TimerThreadMain`, `std::uncaught_exception` / `__cxa_get_globals`):**
  - Cause: libnx `threadTlsGet()` read unzeroed TLS slots at `tpidrro_el0 + 0x108` (`far 0xffffffdb6908489d`).
  - Fix: Ported `RexSwitchSanitizeThreadTls()` (`sdk/src/core/threading_switch.cpp`) + linker `--wrap` overrides for `__cxa_get_globals`, `__cxa_get_globals_fast`, `std::uncaught_exception`, and `std::uncaught_exceptions` backed by `thread_local s_rex_eh_globals` (`sdk/src/ui/switch_perf.cpp`, `sdk/cmake/rexglue_switch.cmake`).
- **`DMC-08` — Black-screen 100% CPU spin-hang (`Main XThread` + `XThreadAF70CD00` after second `Guest D3D device` at `0x4000db80`):**
  - Cause: Both `default.xex` (`0x831EF598`) and static-linked `NTJobCode.xex` (`0x8801B308`) define `xstart` plus 198 `__savegprlr_*` / `__restgprlr_*` / `__savevmx_*` / `__restvmx_*` helpers (with mismatched `skip_lr` ABI). Linking `librenut_NTJobCode.a` with `-Wl,--allow-multiple-definition` silently discarded `NTJobCode`'s `xstart` and bound `0x8801B308` to `default.xex`'s `xstart`, recursively re-running game startup when `NTJobCode.xex`'s `DllMain` executed.
  - Fix: Added `app/src/ntjobcode_switch_symbols.h` (force-included via `-include` on `renut_NTJobCode` for Switch in `app/CMakeLists.txt`) renaming `xstart` and all save/restore helpers with a `_NTJobCode` suffix via 2-level `DECLARE_REX_FUNC` / `DEFINE_REX_FUNC` macro expansion. Also added Deadpool's proven UE3 ARM64 safety defaults to `app/katamari_switch_defaults.toml` (`blur_guest_cas_relaxed = false`, `blur_guest_fences_split = false`, `kernel_scatter_read_offset_zero = true`).
- **`DMC-09` — Crash after Capcom logo (`ExTerminateThread` -> `Thread::Exit` -> `pthread_exit` -> `threadExit` -> `atexit_thread::run`):**
  - Cause: `pthread_exit` is `noreturn`, so when a guest thread exited via `ExTerminateThread`, it jumped directly to `libnx`'s `threadExit()` and skipped the trailing `RexSwitchSanitizeThreadTls()` in `WrappedThreadEntry`, dereferencing unzeroed TLS pointer `0xffffffd432114020` in `atexit_thread::run(void*)` (`pc = image+0x52833e0`).
  - Fix: Added `RexSwitchThreadExit()` (`sdk/src/core/threading_switch.{h,cpp}`), routed `threading_posix.cpp` `Terminate()` / `Thread::Exit()` through `RexSwitchThreadExit()` plus entry/exit sanitization in `ThreadStartRoutine`, sanitized `ProfilerMain`, and added `LINKER:--wrap=threadExit` in `sdk/cmake/rexglue_switch.cmake` + `__wrap_threadExit()` in `sdk/src/ui/switch_perf.cpp`.
- **`DMC-10` — Black screen upon entering 3D world after `Mission1_a.bik` (audio keeps playing) + Switch face button layout (`A↔B`, `X↔Y`):**
  - Cause: `deko3d_shaders.bin` on SD only contained `7` variants; entering the 3D world triggered `160+` synchronous `uam::compileFast` compiles on the render thread (`banjo_profiler.log` `[stall]` dumps 1–5; `rex_profile.log` thread 1289 at `88–93%` CPU, `0.0 fps`, `0` kickoffs/presents while audio kept playing).
  - Fix: Ported `BDKI` sidecar index (`TryLoadSidecarIndex` / `WriteSidecarIndex`) into `app/src/renut_engine/gpu/banjo_renut_deko3d.cpp`; built PC `uam_batch.exe` (`uam::setBanjoFlags(1471u)`, verified 5/5 bit-identical against `uam.exe` with `UAM_BANJO_FLAGS=1471`) and precompiled all `36,446` variants (`0` failures -> `34,274` deduplicated unique DKSH records) into `deko3d_shaders.bin` (`321.57 MB`) + `deko3d_shaders.bin.idx` (`803.3 KB`) with `164/164` (`100%`) runtime log coverage; set `input_xbox_layout = false` (Switch `A↔B`, `X↔Y` layout) in `app/katamari_switch.toml`, `app/katamari_switch_defaults.toml`, and `output/dmc_devil/dmc_devil.toml`; rebuilt `dmc_devil.nro` (`109,205,731` B) + `symbols/`.

## Update & DLC (Deferred / TODO)
- **Do NOT copy `game/` into `output/dmc_devil/`:** User keeps extracted Base `game/` directly on the Switch SD card (`sdmc:/switch/dmc_devil/game/`).
- **Title Update (`Update/TU_11K6214_000000S000000.0000000000087`, `3.03 MB`, STFS `LIVE`, `ctype=0xB0000`):**
  - Contains 6 files: `default.xexp` (`1,972,224` B), `Binaries\NTJobCode\xenon\submission\NTJobCode.xexp` (`4,096` B), `DevilGame\CookedXbox360\ConceptArt_Kat_004_SF.xxx` (`128 KB`), `ConceptArt_Kat_008_SF.xxx` (`128 KB`), `ConceptArt_Thumbnails_SF.xxx` (`512 KB`), `DevilHUD_Thumbnails_SF.xxx` (`192 KB`).
  - **Deferred for initial Base test (`Update/` left empty):** Current `dmc_devil.nro` is statically recompiled from Base `default.xex` + `NTJobCode.xex` (`0.0.0.7`), and SDK `update:` expects extracted files rather than raw STFS containers.
  - **Future TODO:** Build an auto-installer / STFS extractor (similar to Asura's Wrath) when raw `TU_*` packages exist in `Update/`, and recompile a Base+TU NRO if DLC (*Vergil's Downfall* / *Bloody Palace*) requires Title Update code changes.
- **DLC (`DLC/`, 5 `LIVE` STFS packages, `ctype=0x2`, `Title ID 43430824`):**
  - `8F7A42E2...` (*Vergil Premium DLC*, `1.99 GB`), `C25FEE1E...` (*Bloody Palace DLC*, `211 MB`), `12BEB557...` (*Samurai Weapon Pack*, `3.5 MB`), `38ADB7CE...` (*Gold Weapon Pack*, `3.2 MB`), `C5952365...` (*Bone Weapon Pack*, `4.1 MB`).
  - `renut_nx_ultimate_base` (`app/src/renut_engine/dlc_install.cpp`) already supports reading raw `LIVE` DLC packages in place from `sdmc:/switch/dmc_devil/dlc/` via `StfsContainerDevice` (writing only small `.header` files).
  - **Deferred for initial Base test (`dlc/` left empty):** Verify DLC read-in-place and auto-installer behavior in-game once the Base build passes Switch console testing (and add fallback extraction if any package fails read-in-place).
