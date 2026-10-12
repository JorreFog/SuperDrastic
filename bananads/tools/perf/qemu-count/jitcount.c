/* qemu TCG plugin: executed guest instructions per function symbol (from the guest ELF) and, for code without a
   symbol (BananaDS's JIT blocks, shared libraries), per TB start address. Counts per vCPU, inline (cheap).
   At exit writes <out>.tsv: vcpu, kind (sym|addr), key, insns, memops, branches. args: out=<path> */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAXSLOTS (1 << 19)
typedef struct { uint64_t v[3]; } Slot;          /* insns, memops, branches */
static struct qemu_plugin_scoreboard *sb;         /* MAXSLOTS Slots per vcpu */
static GMutex lock;
static GHashTable *symslot;                       /* symbol name -> slot+1 */
static GHashTable *addrslot;                      /* tb vaddr -> slot+1 */
static char **slotkey; static int *slotkind; static int nslots;
static const char *outpath = "jitcount";

static int slot_for(const char *sym, uint64_t vaddr)
{
    gpointer v;
    if (sym) {
        v = g_hash_table_lookup(symslot, sym);
        if (v) return GPOINTER_TO_INT(v) - 1;
    } else {
        v = g_hash_table_lookup(addrslot, GSIZE_TO_POINTER(vaddr));
        if (v) return GPOINTER_TO_INT(v) - 1;
    }
    if (nslots >= MAXSLOTS) return MAXSLOTS - 1;  /* overflow bucket */
    int s = nslots++;
    if (sym) { slotkey[s] = g_strdup(sym); slotkind[s] = 0; g_hash_table_insert(symslot, slotkey[s], GINT_TO_POINTER(s + 1)); }
    else { slotkey[s] = g_strdup_printf("%" PRIx64, vaddr); slotkind[s] = 1; g_hash_table_insert(addrslot, GSIZE_TO_POINTER(vaddr), GINT_TO_POINTER(s + 1)); }
    return s;
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    if (!n) return;
    struct qemu_plugin_insn *first = qemu_plugin_tb_get_insn(tb, 0);
    const char *sym = qemu_plugin_insn_symbol(first);
    uint64_t va = qemu_plugin_tb_vaddr(tb);
    uint64_t mem = 0, br = 0;
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, i);
        uint32_t w = 0;
        qemu_plugin_insn_data(in, &w, 4);
        uint32_t op0 = (w >> 25) & 0xF;
        if ((op0 & 0x5) == 0x4) mem++;              /* loads and stores: x1x0 */
        else if ((op0 & 0xE) == 0xA) br++;          /* branches, exceptions, system: 101x */
    }
    g_mutex_lock(&lock);
    int s = slot_for(sym, va);
    g_mutex_unlock(&lock);
    uint64_t vals[3] = { n, mem, br };
    for (int k = 0; k < 3; k++) {
        if (!vals[k]) continue;
        qemu_plugin_u64 e = { sb, (size_t)s * sizeof(Slot) + k * sizeof(uint64_t) };
        qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(tb, QEMU_PLUGIN_INLINE_ADD_U64, e, vals[k]);
    }
}

static void at_exit(qemu_plugin_id_t id, void *p)
{
    char *path = g_strdup_printf("%s.tsv", outpath);
    FILE *f = fopen(path, "w");
    int nv = qemu_plugin_num_vcpus();
    for (int c = 0; c < nv; c++) {
        Slot *a = qemu_plugin_scoreboard_find(sb, c);
        for (int s = 0; s < nslots; s++)
            if (a[s].v[0]) fprintf(f, "%d\t%s\t%s\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\n", c, slotkind[s] ? "addr" : "sym",
                                   slotkey[s], a[s].v[0], a[s].v[1], a[s].v[2]);
    }
    fclose(f);
    g_free(path);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv)
{
    for (int i = 0; i < argc; i++)
        if (!strncmp(argv[i], "out=", 4)) outpath = g_strdup(argv[i] + 4);
    sb = qemu_plugin_scoreboard_new(sizeof(Slot) * MAXSLOTS);
    symslot = g_hash_table_new(g_str_hash, g_str_equal);
    addrslot = g_hash_table_new(g_direct_hash, g_direct_equal);
    slotkey = g_new0(char *, MAXSLOTS); slotkind = g_new0(int, MAXSLOTS);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
