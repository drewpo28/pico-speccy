// The SRAM the eLeMeNt's pages57 strip has left over (docs/elm-sram-plan.md item 1):
// .elmovl takes the head of ZX pages 5/7's 32 KB, the framebuffer's last donor rows
// follow it, and the rest belonged to nobody - ~14.8 KB at 360 wide, ~20 KB at 320x240.
// Session-lifetime SRAM objects of the machine (never freed) take it before the heap.
#pragma once
#include <stddef.h>

namespace Elm {
// Bump allocation from the tail, 8-byte aligned; tryMalloc when the tail is absent
// (no window this boot, no butter) or too short. Never free() what it returns.
void* tailAlloc(size_t n);
}
