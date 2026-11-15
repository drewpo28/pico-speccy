// Stub for tools/nextint_test.cpp: NextInt.cpp only reads guest memory in its
// NEXT_LINE_DIAG probe.
#pragma once
#include <stdint.h>
struct MemESP { static uint8_t dbgPeek(uint16_t) { return 0; } };
