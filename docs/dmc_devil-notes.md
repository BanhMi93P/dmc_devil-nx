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
