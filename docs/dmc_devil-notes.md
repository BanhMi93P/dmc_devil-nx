# DmC - Devil May Cry (`dmc_devil`) — Port Notes & Censuses

- **Title ID:** `43430824` (`MediaID 021BEF7B`, `Ver 00000007`, `DevilGame-Xbox360-Shipping.pe`)
- **XDK:** `2.0.21119.16384` (`D3D9LTCG`, `D3DX9`, `XAUDIO2`, `X3DAUD`, `XAPOFX`)
- **Engine:** Unreal Engine 3 (Ninja Theory fork; `GlobalShaderCache-Xbox360.bin` + LZX-compressed `.xxx` packages + `.apk` audio + `.bik` movies)

---

## 1. Code Generation & Static Censuses
*(Populated after Phase 2A codegen, `built_ptr_scan.py`, `cc_entry_census.py`, `cc_exit_census.py`.)*

## 2. XDK `2.0.21119.16384` (`D3D9LTCG`) Hook & Device Layout Audit
*(Populated after inspecting generated code and `out/renut_image.bin`.)*

## 3. Shader Extraction Census
*(Populated after extracting `0x102A11xx` containers from `renut_image.bin`, `GlobalShaderCache-Xbox360.bin`, and all Base + Update + DLC `.xxx` packages.)*

## 4. Game-Specific Fixes & Decisions
*(Every game-specific fix, CG-13 census result, and config decision is recorded here.)*

