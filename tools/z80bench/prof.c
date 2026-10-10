// tools/z80bench: per-instruction-address execution counts for 0x10000000+4MB
// (the bench image), dumped at exit to $PROF_OUT; symprof.py folds them by symbol.
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include "qemu-plugin.h"
QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
#define BASE 0x10000000u
#define SPAN (1u<<22)
static uint64_t *cnt; static uint64_t total;
static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, i);
        uint64_t va = qemu_plugin_insn_vaddr(in);
        qemu_plugin_register_vcpu_insn_exec_inline(in, QEMU_PLUGIN_INLINE_ADD_U64, &total, 1);
        if (va >= BASE && va < BASE + SPAN) qemu_plugin_register_vcpu_insn_exec_inline(in, QEMU_PLUGIN_INLINE_ADD_U64, &cnt[(va-BASE)>>1], 1);
    }
}
static void at_exit(qemu_plugin_id_t id, void *p) {
    FILE* f = fopen(getenv("PROF_OUT") ? getenv("PROF_OUT") : "prof.txt", "w");
    for (uint32_t i = 0; i < SPAN/2; i++) if (cnt[i]) fprintf(f, "%08x %llu\n", BASE + i*2, (unsigned long long)cnt[i]);
    fclose(f); fprintf(stderr, "INSNS %llu\n", (unsigned long long)total);
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv) {
    cnt = calloc(SPAN/2, 8);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
