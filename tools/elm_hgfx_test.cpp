// Host test of the HGFX bit rules. g++ -O2 -Wall -Wextra -Isrc -o /tmp/ehg tools/elm_hgfx_test.cpp && /tmp/ehg
#include <stdio.h>
#include "speccy/machines/Element/ElmHgfx.h"
int main() {
    int f = 0;
    // HGFX-2EN: old 145, new 233, mask 143 -> 153.
    if (Hgfx::planar(145, 233, 143) != 153) { f++; puts("FAIL planar example"); }
    // Reference, PlanarMask: 00110011 / 11111110 through 00001111 -> 00111110.
    if (Hgfx::planar(0x33, 0xFE, 0x0F) != 0x3E) { f++; puts("FAIL planar reference"); }
    for (int y = 0; y < 192; y++) {     // the ZX row inverse of the screen address
        const uint16_t off = ((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2);
        if (Hgfx::zxRow(off) != y) { f++; printf("FAIL zxRow %d\n", y); }
    }
    printf("%d failed\n", f);
    return f != 0;
}
