// Host-test stand-in for the firmware's DivMMC.h (NextInt::dmaDelay reads two flags).
#pragma once
struct DivMMC { static inline bool next_nmi_hold = false, next_nmi_btn = false, automap = false; };
