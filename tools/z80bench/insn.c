// tools/z80bench: total executed guest instructions, printed as INSNS at exit.
#include <stdio.h>
#include <glib.h>
#include "qemu-plugin.h"
QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
static uint64_t count;
/* Count only inside [lo,hi) when set: one window = the code under test. */
static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, i);
        qemu_plugin_register_vcpu_insn_exec_inline(in, QEMU_PLUGIN_INLINE_ADD_U64, &count, 1);
    }
}
static void at_exit(qemu_plugin_id_t id, void *p) { fprintf(stderr, "INSNS %llu\n", (unsigned long long)count); }
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv) {
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
