// libultra functions this game calls that N64Recomp ignores but the runtime doesn't provide.

#include "hwtr/hwtr.hpp"
#include "recomp.h"
#include "librecomp/helpers.hpp"

namespace {
    // The game only ever selects OS_VI_NTSC_LAN1 (it passes mode index 2 to its scheduler
    // setup), so that's the current mode type for the whole session.
    constexpr uint32_t OS_VI_NTSC_LAN1 = 2;
}

// u32 osViGetCurrentMode(void)
extern "C" void osViGetCurrentMode_recomp(uint8_t* rdram, recomp_context* ctx) {
    _return<uint32_t>(ctx, OS_VI_NTSC_LAN1);
}

// u32 osViGetStatus(void): reads VI_STATUS_REG. Used by the crash screen to mirror the
// current gamma/dither/AA flags.
extern "C" void osViGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    _return<uint32_t>(ctx, ultramodern::renderer::get_vi_regs()->VI_STATUS_REG);
}
