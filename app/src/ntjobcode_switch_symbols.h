#pragma once

// Isolate static-linked NTJobCode.xex symbols from default.xex on Switch.
// Without this, -Wl,--allow-multiple-definition silently discards NTJobCode's
// xstart (DllMain at 0x8801B308) and binds it to default.xex's xstart, causing
// recursive game re-boot and a 100% CPU deadlock. Save/restore helpers also
// differ (default.xex uses REX_LR_SKIP; NTJobCode has skip_lr = false).

#define REX_NTJC_STR2(x) #x
#define REX_NTJC_STR(x) REX_NTJC_STR2(x)

#define REX_NTJC_DECLARE_EXPAND(name) \
  REX_EXTERN(name);                   \
  REX_EXTERN(__imp__##name)
#define DECLARE_REX_FUNC(name) REX_NTJC_DECLARE_EXPAND(name)

#if defined(__GNUC__) && !defined(__clang__)
#define REX_NTJC_DEFINE_EXPAND(name)                                        \
  extern "C" REX_FUNC(name)                                                 \
      __attribute__((alias("__imp__" REX_NTJC_STR(name)), weak, noinline)); \
  REX_EXTERN(__imp__##name)
#else
#define REX_NTJC_DEFINE_EXPAND(name)                                        \
  __attribute__((alias("__imp__" REX_NTJC_STR(name))))                      \
  __attribute__((weak, noinline)) extern "C" REX_FUNC(name);                \
  REX_EXTERN(__imp__##name)
#endif
#define DEFINE_REX_FUNC(name) REX_NTJC_DEFINE_EXPAND(name)

#define xstart xstart_NTJobCode

// GPR/LR save and restore helpers (r14..r31)
#define __savegprlr_14 __savegprlr_14_NTJobCode
#define __savegprlr_15 __savegprlr_15_NTJobCode
#define __savegprlr_16 __savegprlr_16_NTJobCode
#define __savegprlr_17 __savegprlr_17_NTJobCode
#define __savegprlr_18 __savegprlr_18_NTJobCode
#define __savegprlr_19 __savegprlr_19_NTJobCode
#define __savegprlr_20 __savegprlr_20_NTJobCode
#define __savegprlr_21 __savegprlr_21_NTJobCode
#define __savegprlr_22 __savegprlr_22_NTJobCode
#define __savegprlr_23 __savegprlr_23_NTJobCode
#define __savegprlr_24 __savegprlr_24_NTJobCode
#define __savegprlr_25 __savegprlr_25_NTJobCode
#define __savegprlr_26 __savegprlr_26_NTJobCode
#define __savegprlr_27 __savegprlr_27_NTJobCode
#define __savegprlr_28 __savegprlr_28_NTJobCode
#define __savegprlr_29 __savegprlr_29_NTJobCode
#define __savegprlr_30 __savegprlr_30_NTJobCode
#define __savegprlr_31 __savegprlr_31_NTJobCode

#define __restgprlr_14 __restgprlr_14_NTJobCode
#define __restgprlr_15 __restgprlr_15_NTJobCode
#define __restgprlr_16 __restgprlr_16_NTJobCode
#define __restgprlr_17 __restgprlr_17_NTJobCode
#define __restgprlr_18 __restgprlr_18_NTJobCode
#define __restgprlr_19 __restgprlr_19_NTJobCode
#define __restgprlr_20 __restgprlr_20_NTJobCode
#define __restgprlr_21 __restgprlr_21_NTJobCode
#define __restgprlr_22 __restgprlr_22_NTJobCode
#define __restgprlr_23 __restgprlr_23_NTJobCode
#define __restgprlr_24 __restgprlr_24_NTJobCode
#define __restgprlr_25 __restgprlr_25_NTJobCode
#define __restgprlr_26 __restgprlr_26_NTJobCode
#define __restgprlr_27 __restgprlr_27_NTJobCode
#define __restgprlr_28 __restgprlr_28_NTJobCode
#define __restgprlr_29 __restgprlr_29_NTJobCode
#define __restgprlr_30 __restgprlr_30_NTJobCode
#define __restgprlr_31 __restgprlr_31_NTJobCode

// FPR save and restore helpers (f14..f31)
#define __savefpr_14 __savefpr_14_NTJobCode
#define __savefpr_15 __savefpr_15_NTJobCode
#define __savefpr_16 __savefpr_16_NTJobCode
#define __savefpr_17 __savefpr_17_NTJobCode
#define __savefpr_18 __savefpr_18_NTJobCode
#define __savefpr_19 __savefpr_19_NTJobCode
#define __savefpr_20 __savefpr_20_NTJobCode
#define __savefpr_21 __savefpr_21_NTJobCode
#define __savefpr_22 __savefpr_22_NTJobCode
#define __savefpr_23 __savefpr_23_NTJobCode
#define __savefpr_24 __savefpr_24_NTJobCode
#define __savefpr_25 __savefpr_25_NTJobCode
#define __savefpr_26 __savefpr_26_NTJobCode
#define __savefpr_27 __savefpr_27_NTJobCode
#define __savefpr_28 __savefpr_28_NTJobCode
#define __savefpr_29 __savefpr_29_NTJobCode
#define __savefpr_30 __savefpr_30_NTJobCode
#define __savefpr_31 __savefpr_31_NTJobCode

#define __restfpr_14 __restfpr_14_NTJobCode
#define __restfpr_15 __restfpr_15_NTJobCode
#define __restfpr_16 __restfpr_16_NTJobCode
#define __restfpr_17 __restfpr_17_NTJobCode
#define __restfpr_18 __restfpr_18_NTJobCode
#define __restfpr_19 __restfpr_19_NTJobCode
#define __restfpr_20 __restfpr_20_NTJobCode
#define __restfpr_21 __restfpr_21_NTJobCode
#define __restfpr_22 __restfpr_22_NTJobCode
#define __restfpr_23 __restfpr_23_NTJobCode
#define __restfpr_24 __restfpr_24_NTJobCode
#define __restfpr_25 __restfpr_25_NTJobCode
#define __restfpr_26 __restfpr_26_NTJobCode
#define __restfpr_27 __restfpr_27_NTJobCode
#define __restfpr_28 __restfpr_28_NTJobCode
#define __restfpr_29 __restfpr_29_NTJobCode
#define __restfpr_30 __restfpr_30_NTJobCode
#define __restfpr_31 __restfpr_31_NTJobCode

// VMX save and restore helpers (v14..v31, v64..v127)
#define __savevmx_14 __savevmx_14_NTJobCode
#define __savevmx_15 __savevmx_15_NTJobCode
#define __savevmx_16 __savevmx_16_NTJobCode
#define __savevmx_17 __savevmx_17_NTJobCode
#define __savevmx_18 __savevmx_18_NTJobCode
#define __savevmx_19 __savevmx_19_NTJobCode
#define __savevmx_20 __savevmx_20_NTJobCode
#define __savevmx_21 __savevmx_21_NTJobCode
#define __savevmx_22 __savevmx_22_NTJobCode
#define __savevmx_23 __savevmx_23_NTJobCode
#define __savevmx_24 __savevmx_24_NTJobCode
#define __savevmx_25 __savevmx_25_NTJobCode
#define __savevmx_26 __savevmx_26_NTJobCode
#define __savevmx_27 __savevmx_27_NTJobCode
#define __savevmx_28 __savevmx_28_NTJobCode
#define __savevmx_29 __savevmx_29_NTJobCode
#define __savevmx_30 __savevmx_30_NTJobCode
#define __savevmx_31 __savevmx_31_NTJobCode

#define __savevmx_64 __savevmx_64_NTJobCode
#define __savevmx_65 __savevmx_65_NTJobCode
#define __savevmx_66 __savevmx_66_NTJobCode
#define __savevmx_67 __savevmx_67_NTJobCode
#define __savevmx_68 __savevmx_68_NTJobCode
#define __savevmx_69 __savevmx_69_NTJobCode
#define __savevmx_70 __savevmx_70_NTJobCode
#define __savevmx_71 __savevmx_71_NTJobCode
#define __savevmx_72 __savevmx_72_NTJobCode
#define __savevmx_73 __savevmx_73_NTJobCode
#define __savevmx_74 __savevmx_74_NTJobCode
#define __savevmx_75 __savevmx_75_NTJobCode
#define __savevmx_76 __savevmx_76_NTJobCode
#define __savevmx_77 __savevmx_77_NTJobCode
#define __savevmx_78 __savevmx_78_NTJobCode
#define __savevmx_79 __savevmx_79_NTJobCode
#define __savevmx_80 __savevmx_80_NTJobCode
#define __savevmx_81 __savevmx_81_NTJobCode
#define __savevmx_82 __savevmx_82_NTJobCode
#define __savevmx_83 __savevmx_83_NTJobCode
#define __savevmx_84 __savevmx_84_NTJobCode
#define __savevmx_85 __savevmx_85_NTJobCode
#define __savevmx_86 __savevmx_86_NTJobCode
#define __savevmx_87 __savevmx_87_NTJobCode
#define __savevmx_88 __savevmx_88_NTJobCode
#define __savevmx_89 __savevmx_89_NTJobCode
#define __savevmx_90 __savevmx_90_NTJobCode
#define __savevmx_91 __savevmx_91_NTJobCode
#define __savevmx_92 __savevmx_92_NTJobCode
#define __savevmx_93 __savevmx_93_NTJobCode
#define __savevmx_94 __savevmx_94_NTJobCode
#define __savevmx_95 __savevmx_95_NTJobCode
#define __savevmx_96 __savevmx_96_NTJobCode
#define __savevmx_97 __savevmx_97_NTJobCode
#define __savevmx_98 __savevmx_98_NTJobCode
#define __savevmx_99 __savevmx_99_NTJobCode
#define __savevmx_100 __savevmx_100_NTJobCode
#define __savevmx_101 __savevmx_101_NTJobCode
#define __savevmx_102 __savevmx_102_NTJobCode
#define __savevmx_103 __savevmx_103_NTJobCode
#define __savevmx_104 __savevmx_104_NTJobCode
#define __savevmx_105 __savevmx_105_NTJobCode
#define __savevmx_106 __savevmx_106_NTJobCode
#define __savevmx_107 __savevmx_107_NTJobCode
#define __savevmx_108 __savevmx_108_NTJobCode
#define __savevmx_109 __savevmx_109_NTJobCode
#define __savevmx_110 __savevmx_110_NTJobCode
#define __savevmx_111 __savevmx_111_NTJobCode
#define __savevmx_112 __savevmx_112_NTJobCode
#define __savevmx_113 __savevmx_113_NTJobCode
#define __savevmx_114 __savevmx_114_NTJobCode
#define __savevmx_115 __savevmx_115_NTJobCode
#define __savevmx_116 __savevmx_116_NTJobCode
#define __savevmx_117 __savevmx_117_NTJobCode
#define __savevmx_118 __savevmx_118_NTJobCode
#define __savevmx_119 __savevmx_119_NTJobCode
#define __savevmx_120 __savevmx_120_NTJobCode
#define __savevmx_121 __savevmx_121_NTJobCode
#define __savevmx_122 __savevmx_122_NTJobCode
#define __savevmx_123 __savevmx_123_NTJobCode
#define __savevmx_124 __savevmx_124_NTJobCode
#define __savevmx_125 __savevmx_125_NTJobCode
#define __savevmx_126 __savevmx_126_NTJobCode
#define __savevmx_127 __savevmx_127_NTJobCode

#define __restvmx_14 __restvmx_14_NTJobCode
#define __restvmx_15 __restvmx_15_NTJobCode
#define __restvmx_16 __restvmx_16_NTJobCode
#define __restvmx_17 __restvmx_17_NTJobCode
#define __restvmx_18 __restvmx_18_NTJobCode
#define __restvmx_19 __restvmx_19_NTJobCode
#define __restvmx_20 __restvmx_20_NTJobCode
#define __restvmx_21 __restvmx_21_NTJobCode
#define __restvmx_22 __restvmx_22_NTJobCode
#define __restvmx_23 __restvmx_23_NTJobCode
#define __restvmx_24 __restvmx_24_NTJobCode
#define __restvmx_25 __restvmx_25_NTJobCode
#define __restvmx_26 __restvmx_26_NTJobCode
#define __restvmx_27 __restvmx_27_NTJobCode
#define __restvmx_28 __restvmx_28_NTJobCode
#define __restvmx_29 __restvmx_29_NTJobCode
#define __restvmx_30 __restvmx_30_NTJobCode
#define __restvmx_31 __restvmx_31_NTJobCode

#define __restvmx_64 __restvmx_64_NTJobCode
#define __restvmx_65 __restvmx_65_NTJobCode
#define __restvmx_66 __restvmx_66_NTJobCode
#define __restvmx_67 __restvmx_67_NTJobCode
#define __restvmx_68 __restvmx_68_NTJobCode
#define __restvmx_69 __restvmx_69_NTJobCode
#define __restvmx_70 __restvmx_70_NTJobCode
#define __restvmx_71 __restvmx_71_NTJobCode
#define __restvmx_72 __restvmx_72_NTJobCode
#define __restvmx_73 __restvmx_73_NTJobCode
#define __restvmx_74 __restvmx_74_NTJobCode
#define __restvmx_75 __restvmx_75_NTJobCode
#define __restvmx_76 __restvmx_76_NTJobCode
#define __restvmx_77 __restvmx_77_NTJobCode
#define __restvmx_78 __restvmx_78_NTJobCode
#define __restvmx_79 __restvmx_79_NTJobCode
#define __restvmx_80 __restvmx_80_NTJobCode
#define __restvmx_81 __restvmx_81_NTJobCode
#define __restvmx_82 __restvmx_82_NTJobCode
#define __restvmx_83 __restvmx_83_NTJobCode
#define __restvmx_84 __restvmx_84_NTJobCode
#define __restvmx_85 __restvmx_85_NTJobCode
#define __restvmx_86 __restvmx_86_NTJobCode
#define __restvmx_87 __restvmx_87_NTJobCode
#define __restvmx_88 __restvmx_88_NTJobCode
#define __restvmx_89 __restvmx_89_NTJobCode
#define __restvmx_90 __restvmx_90_NTJobCode
#define __restvmx_91 __restvmx_91_NTJobCode
#define __restvmx_92 __restvmx_92_NTJobCode
#define __restvmx_93 __restvmx_93_NTJobCode
#define __restvmx_94 __restvmx_94_NTJobCode
#define __restvmx_95 __restvmx_95_NTJobCode
#define __restvmx_96 __restvmx_96_NTJobCode
#define __restvmx_97 __restvmx_97_NTJobCode
#define __restvmx_98 __restvmx_98_NTJobCode
#define __restvmx_99 __restvmx_99_NTJobCode
#define __restvmx_100 __restvmx_100_NTJobCode
#define __restvmx_101 __restvmx_101_NTJobCode
#define __restvmx_102 __restvmx_102_NTJobCode
#define __restvmx_103 __restvmx_103_NTJobCode
#define __restvmx_104 __restvmx_104_NTJobCode
#define __restvmx_105 __restvmx_105_NTJobCode
#define __restvmx_106 __restvmx_106_NTJobCode
#define __restvmx_107 __restvmx_107_NTJobCode
#define __restvmx_108 __restvmx_108_NTJobCode
#define __restvmx_109 __restvmx_109_NTJobCode
#define __restvmx_110 __restvmx_110_NTJobCode
#define __restvmx_111 __restvmx_111_NTJobCode
#define __restvmx_112 __restvmx_112_NTJobCode
#define __restvmx_113 __restvmx_113_NTJobCode
#define __restvmx_114 __restvmx_114_NTJobCode
#define __restvmx_115 __restvmx_115_NTJobCode
#define __restvmx_116 __restvmx_116_NTJobCode
#define __restvmx_117 __restvmx_117_NTJobCode
#define __restvmx_118 __restvmx_118_NTJobCode
#define __restvmx_119 __restvmx_119_NTJobCode
#define __restvmx_120 __restvmx_120_NTJobCode
#define __restvmx_121 __restvmx_121_NTJobCode
#define __restvmx_122 __restvmx_122_NTJobCode
#define __restvmx_123 __restvmx_123_NTJobCode
#define __restvmx_124 __restvmx_124_NTJobCode
#define __restvmx_125 __restvmx_125_NTJobCode
#define __restvmx_126 __restvmx_126_NTJobCode
#define __restvmx_127 __restvmx_127_NTJobCode
