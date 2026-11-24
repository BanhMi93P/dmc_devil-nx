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
