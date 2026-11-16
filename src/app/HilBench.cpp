// Hardware-in-the-loop benchmark mailbox: see HilBench.h and tools/hil/README.md.
#include "HilBench.h"

#if HIL_BENCH
#include <string.h>
#include <string>
#include "ui/OSDMain.h"
#include "speccy/core/Snapshot.h"
#include "app/Debug.h"

// Plain .data (not .bss): the magic is there from reset, so the runner can tell a
// HIL build from one without it before the main loop has run once.
HilBench g_hil = { HIL_MAGIC, HIL_VERSION };

static void resetWindow() {
    g_hil.frames = 0;
    g_hil.idle_min = 0; g_hil.idle_max = 0;
    g_hil.neg_frames = 0;
    g_hil.idle_sum = 0;
    g_hil.elapsed_sum = 0; g_hil.elapsed_max = 0;
    g_hil.t_first = 0; g_hil.t_last = 0;
    g_hil.cpu_sum = 0; g_hil.mix_sum = 0; g_hil.kbd_sum = 0;
}

void __attribute__((noinline)) HilBenchSvc::service() {
    const uint32_t cmd = g_hil.cmd;
    g_hil.status = HIL_ST_BUSY;
    bool ok = true;
    switch (cmd) {
    case HIL_CMD_RESET:
        break;
    case HIL_CMD_LOAD_SLOT:
        Debug::log("HIL: load slot %u", (unsigned)g_hil.arg);
        ok = persistLoad((uint8_t)g_hil.arg);
        break;
    case HIL_CMD_LOAD_FILE: {
        g_hil.path[sizeof(g_hil.path) - 1] = 0;
        Debug::log("HIL: load %s", g_hil.path);
        ok = LoadSnapshot(std::string(g_hil.path), A_NONE, R_NONE);
        break;
    }
    default:
        ok = false;
        break;
    }
    resetWindow();
    g_hil.status = ok ? HIL_ST_OK : HIL_ST_FAIL;
    g_hil.seq = g_hil.seq + 1;
    g_hil.cmd = HIL_CMD_NONE;
}
#endif
