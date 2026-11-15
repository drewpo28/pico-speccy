// tools/z80bench: per-instruction-address execution counts for 0x10000000+4MB
// (the bench image), dumped at exit to $PROF_OUT; symprof.py folds them by symbol.
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include "qemu-plugin.h"
QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
#define BASE 0x10000000u
#define SPAN (1u<<22)
/* QEMU >= 9 (plugin API 3+): inline ops are per vcpu, on a scoreboard — one
 * element = the total + every counter (16 MB; the bench has one vcpu). */
static struct qemu_plugin_scoreboard *sb;
static qemu_plugin_u64 at(size_t i) { qemu_plugin_u64 e = { sb, i * 8 }; return e; }
static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, i);
        uint64_t va = qemu_plugin_insn_vaddr(in);
        qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(in, QEMU_PLUGIN_INLINE_ADD_U64, at(0), 1);
        if (va >= BASE && va < BASE + SPAN) qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(in, QEMU_PLUGIN_INLINE_ADD_U64, at(1 + ((va-BASE)>>1)), 1);
    }
}
static void at_exit(qemu_plugin_id_t id, void *p) {
    FILE* f = fopen(getenv("PROF_OUT") ? getenv("PROF_OUT") : "prof.txt", "w");
    for (uint32_t i = 0; i < SPAN/2; i++) { uint64_t c = qemu_plugin_u64_sum(at(1 + i)); if (c) fprintf(f, "%08x %llu\n", BASE + i*2, (unsigned long long)c); }
    fclose(f); fprintf(stderr, "INSNS %llu\n", (unsigned long long)qemu_plugin_u64_sum(at(0)));
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv) {
    sb = qemu_plugin_scoreboard_new(8 * (1 + SPAN/2));
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
