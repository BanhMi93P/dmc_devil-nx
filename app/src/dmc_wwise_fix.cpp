// DMC-14 / DMC-16 (TU1): Protect Wwise GlobalCallback list (0x836973F4) with Wwise critical section (0x8369734C).
// On Base 1.0 these were RegisterGlobalCallback (0x830AF158), UnregisterGlobalCallback (0x830AF1B0), CS (0x836972DC).
// On Title Update 1 (0.0.1.7), RegisterGlobalCallback = 0x830B94F0, UnregisterGlobalCallback = 0x830B9548, CS = 0x8369734C.

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/types.h>
#include "renut_pch.h"

namespace {

constexpr uint32_t kWwiseGlobalCsVa = 0x8369734Cu;

REX_IMPORT(__imp__RtlEnterCriticalSection, GuestRtlEnterCriticalSection, void(u32));
REX_IMPORT(__imp__RtlLeaveCriticalSection, GuestRtlLeaveCriticalSection, void(u32));

template <typename OrigFn>
void CallWithWwiseGlobalCs(PPCContext& __restrict ctx, uint8_t* base, OrigFn orig_fn) {
  const bool initialized = base && (REX_LOAD_U8(kWwiseGlobalCsVa) == 1u);
  if (initialized) {
    rex::CallFrame frame(ctx);
    GuestRtlEnterCriticalSection(frame, base, kWwiseGlobalCsVa);
  }
  orig_fn(ctx, base);
  if (initialized) {
    rex::CallFrame frame(ctx);
    GuestRtlLeaveCriticalSection(frame, base, kWwiseGlobalCsVa);
  }
}

}  // namespace

REX_EXTERN(__imp__sub_830B94F0);
REX_HOOK_RAW(sub_830B94F0) {
  CallWithWwiseGlobalCs(ctx, base, __imp__sub_830B94F0);
}

REX_EXTERN(__imp__sub_830B9548);
REX_HOOK_RAW(sub_830B9548) {
  CallWithWwiseGlobalCs(ctx, base, __imp__sub_830B9548);
}
