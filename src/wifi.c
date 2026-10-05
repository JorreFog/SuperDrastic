/* Online play for DraStic, the way melonDS does it: the DS joins an open access point and
 * DHCP tells it which DNS server to use (Kaeru, AltWFC, WiiLink, or an address of your own).
 *
 * DraStic r2.5.2.2 already has wifi RAM and a baseband serial engine, but the 8/32-bit handlers
 * are stubs, a few registers are hardcoded, and nothing ever queues a frame or raises the ARM7
 * wifi IRQ. The handlers are not in the dynamic symbol table, so LD_PRELOAD cannot interpose
 * them. They are reached through two function tables in .data.rel.ro (ext_function_wifi_load /
 * ext_function_wifi_store). initialize_memory_map_arm7 copies those tables into the ARM7 map,
 * and a type-2 region reloads the pointer on every access, so replacing the tables in a
 * constructor (before main) is enough. The original handlers stay in .text and are called for
 * ordinary register and RAM traffic.
 *
 * Offsets below are for BuildID 7a5e0e5fc6e52e6e8f5499c3d4d667ef51db0748. `add Xd, Xn, #imm,
 * lsl #12` adds imm<<12. The hook checks that id and the register/RAM layout, and does nothing
 * if this is a different DraStic. The feature stays off until ES's "wfc dns" (nds.wfc_dns) or
 * DSFLIP_WFC says otherwise, so an unmodified config behaves exactly as stock DraStic.
 *
 * Setting: DSFLIP_WFC = off | kaeru | altwfc | wiilink | <dotted ip> wins (superdrastic-run users);
 * otherwise nds.wfc_dns from the per-ROM config, then /storage/.config/system/configs/system.cfg.
 * DSFLIP_WFC_DEBUG=1 logs every frame. Beacons, the TSF compare and the NAT sockets are driven by
 * the "dsf-wfc" service thread (10 ms), not by the display path, so dsflip.c needs no per-frame hook.
 */
#define _GNU_SOURCE
#include "wififrame.h"
#include "wfcnet.h"
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void dsflip_log(const char *fmt, ...);
void dsflip_toast(const char *l1, const char *l2, uint32_t accent, int ms);

/* load_wifi_16 / store_wifi_16, initialize_memory, store_io_register_arm7_*. */
enum {
    WIFI_LOAD_TBL = 0x15d3a0, WIFI_STORE_TBL = 0x15d3b8,
    OFF_LD8 = 0x10450, OFF_LD16 = 0x12590, OFF_LD32 = 0x10460,
    OFF_ST8 = 0x10430, OFF_ST16 = 0x12670, OFF_ST32 = 0x10440,
    REG_BASE = 0x0fb5b8,    /* halfword at mem + REG_BASE + (addr & 0x3fff) */
    RAM_BASE = 0x017070,    /* byte at mem + RAM_BASE + (addr & 0x3fff); 0x4000..0x7fff is RAM */
    STATE_AT = 0x0fba68,    /* *(void **) = the cpu-state block */
    ARM7_OFF = 0x25ce340,   /* ARM7 cpu inside that block (ARM9 is 0x15c7d50, io at 0x1b070) */
    ARM7_IO = 8320,         /* *(void **)(arm7 + ARM7_IO) == mem + IO_BASE */
    IO_BASE = 0x023070,
    IO_IME = 0x208, IO_IE = 0x210, IO_IF = 0x214,
    ARM7_PEND = 8456, ARM7_HALT = 8464, ARM7_ALERT = 8872
};
static const uint8_t BUILD_ID[20] = {
    0x7a, 0x5e, 0x0e, 0x5f, 0xc6, 0xe5, 0x2e, 0x6e, 0x8f, 0x54,
    0x99, 0xc3, 0xd4, 0xd6, 0x67, 0xef, 0x51, 0xdb, 0x07, 0x48
};
static const uint8_t BSSID[6] = { 0x02, 0x09, 0xbf, 0x11, 0x22, 0x33 };
#define SYSCFG "/storage/.config/system/configs/system.cfg"

typedef uint32_t (*wld_fn)(void *, uint32_t);
typedef void (*wst_fn)(void *, uint32_t, uint32_t);
static wld_fn orig_ld8, orig_ld16, orig_ld32;
static wst_fn orig_st8, orig_st16, orig_st32;

static pthread_mutex_t wmu;
static int wmu_ok, hooked, layout_ok, layout_tried, active, debug;
static const char *fail_reason;
static void *g_mem;
static wfcnet *net;
static char srv_name[32], srv_ip[20];
static uint32_t dns_ip;

static uint16_t rnd = 1;
static uint64_t us_base_mono, us_base_val;
static int us_inited;
static int rx_on, rx_latched, bc_armed, bc_left;
static int mode_on;                     /* W_MODE_RST bit 0: the MAC is switched on */
/* test switches (/tmp/wfc-knobs with DSFLIP_WFC_DEBUG=2): answer probes, send beacons, beacon after a wake-up (us, 0 off);
 * old: the access point's beacon at the DS's own beacon time; gate: no reception while the MAC is off; txclr: LOC1..3
 * taken back at the beacon time */
static int k_old, k_gate = 1, k_txclr = 1, k_ack = 1;
static int in_tx;                       /* inside do_tx: what is queued for the DS now is an answer to the frame being sent */
static int k_probe = 1, k_bcn = 1, k_wake = 4000;
static uint64_t ap_next;                /* when the access point's next beacon is due (its own clock, 100 TU apart) */
static uint64_t bc_stamp, cmp_fired;
static uint16_t rx_begin = 0x4000, rx_end = 0x4800;
static uint16_t seqno;
static int assoc, want_toast, toasted, irq_logged, layout_logged;
static char joined_ssid[33];

static uint32_t hook_load16(void *mem, uint32_t addr);
static void hook_store16(void *mem, uint32_t addr, uint32_t val);

/* DSFLIP_WFC_DEBUG=2: a register trace on top of the frame log, for finding out on a handheld where the game's
 * driver stops: every register write ("w off=val", repeats folded), the first read of a register and every read
 * that returns something new ("r off->val"), the ARM7 interrupts raised and a state line every 2 s. */
static uint64_t mono_us(void);
static int trace, trace_cfg, tr_lines;
static uint64_t tr_t0;
#define TR_MAX 20000
#define TR(fmt, ...) do { if (trace && tr_lines < TR_MAX) { tr_lines++; dsflip_log("[wfc] %7.1f " fmt, (double)(mono_us() - tr_t0) / 1000.0, ##__VA_ARGS__); } } while (0)
static uint8_t rd_seen[0x800];
static uint16_t rd_last[0x800];
static int n_beacon, n_rxq, n_rxdrop, n_rxoff, n_irq, n_tx;
static int n_st8, n_st16, n_st32, n_ld8, n_ld16, n_ld32, raw_logged;
static uintptr_t g_base;
#define RAWTR(what, addr, val) do { if (trace && raw_logged < 40) { raw_logged++; dsflip_log("[wfc] raw %s %08x %08x\n", what, (unsigned)(addr), (unsigned)(val)); } } while (0)
static uint16_t txreq;                  /* W_TXREQ_READ (0x0b0): the slots allowed to send, set and cleared through 0x0ae / 0x0ac */
static int tx_more;

static void wlock(void) { if (wmu_ok) pthread_mutex_lock(&wmu); }
static void wunlock(void) { if (wmu_ok) pthread_mutex_unlock(&wmu); }

static uint64_t mono_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000ull + t.tv_nsec / 1000ull;
}
static uint64_t us_now(void) {
    uint64_t m = mono_us();
    if (!us_inited) { us_base_mono = m; us_base_val = 0; us_inited = 1; }
    return us_base_val + (m - us_base_mono);
}
static void us_set(uint64_t v) { us_base_mono = mono_us(); us_base_val = v; us_inited = 1; }

static int is_ram(uint32_t addr) {
    uint32_t d = (addr & 0xffffu) - 0x4000u;
    return d <= 0x3fffu;
}
/* DraStic keeps 0x6000..0x7fff as a second 8 KiB, not a mirror of the MAC. Fold it back. */
static uint32_t fold_ram(uint32_t addr) {
    uint32_t a = addr & 0xffffu;
    if (a >= 0x6000u) a = 0x4000u + (a & 0x1fffu);
    return (addr & ~0xffffu) | a;
}
static uint16_t regoff(uint32_t addr) { return (uint16_t)(addr & 0x3fffu); }

static uint16_t raw16(void *mem, uint16_t off) {
    return *(volatile uint16_t *)((uint8_t *)mem + REG_BASE + off);
}
static uint8_t *mac_base(void *mem) { return (uint8_t *)mem + RAM_BASE; }

static void arm7_irq(void *mem) {
    uintptr_t state = *(uintptr_t *)((uint8_t *)mem + STATE_AT);
    if (state < 0x10000) {
        if (!irq_logged) { dsflip_log("[wfc] no ARM7 state at mem+%#x\n", STATE_AT); irq_logged = 1; }
        return;
    }
    uint8_t *arm7 = (uint8_t *)state + ARM7_OFF;
    uint8_t *io = *(uint8_t **)(arm7 + ARM7_IO);
    if (io != (uint8_t *)mem + IO_BASE) {
        if (!irq_logged) {
            dsflip_log("[wfc] ARM7 io %p, expected mem+%#x (state %p)\n", io, IO_BASE, (void *)state);
            irq_logged = 1;
        }
        return;
    }
    uint32_t iff = *(uint32_t *)(io + IO_IF) | (1u << 24);
    *(uint32_t *)(io + IO_IF) = iff;
    uint32_t halt = *(uint32_t *)(arm7 + ARM7_HALT);
    uint32_t pend = *(uint32_t *)(arm7 + ARM7_PEND);
    if ((halt & 6) == 0) {
        uint32_t ime = *(uint32_t *)(io + IO_IME);
        uint32_t ie = *(uint32_t *)(io + IO_IE);
        pend = (uint32_t)(-(int32_t)ime) & iff & ie;
        *(uint32_t *)(arm7 + ARM7_PEND) = pend;
    }
    if (pend) *(uint32_t *)(arm7 + ARM7_ALERT) |= 2;
    TR("irq7: IF %08x IE %08x IME %x halt %x pend %x (W_IF %04x W_IE %04x)\n", iff, *(uint32_t *)(io + IO_IE),
       *(uint32_t *)(io + IO_IME), halt, pend, raw16(mem, 0x10), raw16(mem, 0x12));
}

/* W_IF (0x010) is written through DraStic's own 16-bit handler when the layout check found it stores the value as
 * is; a handler that already clears the written bits (write-1-to-clear, what the hardware does) would turn our set
 * into a clear, so the flags then go straight into the register memory */
static int if_plain = 1;
static void if_put(void *mem, uint16_t v) {
    if (if_plain) orig_st16(mem, 0x0010, v);
    else *(volatile uint16_t *)((uint8_t *)mem + REG_BASE + 0x10) = v;
}
static void wifi_if(void *mem, uint16_t bits) {
    uint16_t old = raw16(mem, 0x10), now = old | bits, ie = raw16(mem, 0x12);
    if_put(mem, now);
    if (now & ie) arm7_irq(mem);
    (void)old;
}

static int layout_check(void *mem) {
    uint16_t old = orig_ld16(mem, 0x0012);
    orig_st16(mem, 0x0012, 0x5a5a);
    int reg_ok = raw16(mem, 0x0012) == 0x5a5a;
    orig_st16(mem, 0x0012, old);
    /* and whether the stock handler stores W_IF as is (then our set/clear go through it) or clears the written
     * bits (then they go into the register memory directly); the probe is undone either way */
    uint16_t iold = raw16(mem, 0x0010);
    orig_st16(mem, 0x0010, 0x5a5a);
    if_plain = raw16(mem, 0x0010) == 0x5a5a;
    *(volatile uint16_t *)((uint8_t *)mem + REG_BASE + 0x10) = iold;
    uint16_t rold = orig_ld16(mem, 0x4000);
    orig_st16(mem, 0x4000, 0xbeef);
    /* load_wifi_16 indexes RAM with addr & 0x3fff, so 0x4000 lands at the base */
    uint16_t got = *(volatile uint16_t *)(mac_base(mem) + (0x4000u & 0x3fffu));
    orig_st16(mem, 0x4000, rold);
    if (!layout_logged) {
        dsflip_log("[wfc] layout %s (reg %s, mac %s, IF store %s)\n", reg_ok && got == 0xbeef ? "ok" : "mismatch",
                   reg_ok ? "ok" : "no", got == 0xbeef ? "ok" : "no", if_plain ? "plain" : "write-1-to-clear");
        layout_logged = 1;
    }
    return reg_ok && got == 0xbeef;
}

static int bssid_match(void *mem, const uint8_t *frm, int len) {
    if (len < 16) return 0;
    for (int i = 0; i < 3; i++) {
        uint16_t h = raw16(mem, (uint16_t)(0x20 + i * 2));
        if (frm[10 + i * 2] != (uint8_t)h || frm[11 + i * 2] != (uint8_t)(h >> 8)) return 0;
    }
    return 1;
}

static void rx_queue(void *mem, const uint8_t *frm, int len) {
    if ((!mode_on && k_gate) || !rx_on || !rx_latched || len <= 4) { n_rxoff++; return; }
    /* the record holds the frame with its FCS, as the hardware stores it; the header's length takes off what
     * W_RXLEN_CROP says (the firmware sets 0x0602: the 4-byte FCS on plain frames, 12 bytes on WEP ones) */
    uint16_t crop = raw16(mem, 0x0da), fc = (uint16_t)(frm[0] | (frm[1] << 8));
    int cb = (fc & 0x4000) ? ((crop >> 7) & 0x1fe) : ((crop << 1) & 0x1fe);
    uint16_t wr = raw16(mem, 0x54), rd = raw16(mem, 0x5a);
    if (!wififrame_rx_push(mac_base(mem), &wr, rd, rx_begin, rx_end, frm, len, cb, bssid_match(mem, frm, len))) {
        if (debug) dsflip_log("[wfc] rx drop, %d bytes\n", len);
        n_rxdrop++;
        return;
    }
    n_rxq++;
    TR("rx fc %02x%02x len %d -> wrcsr %04x (rd %04x) hdr0 %s\n", frm[1], frm[0], len, wr, rd, bssid_match(mem, frm, len) ? "bssid" : "-");
    orig_st16(mem, 0x0054, wr);
    wifi_if(mem, 1);                                  /* RX complete */
}

/* What the access point sends in answer to the DS (probe, authentication and association responses, and the
 * network's replies that are made while the DS's own frame is being taken: ARP, DHCP, ICMP) waits here for a moment.
 * Delivered at once, an answer reached the driver in the same interrupt as the end of its own transmission, and
 * before it: on real hardware nothing can arrive until the frame is out. DSFLIP_WFC_RXDELAY: the wait in
 * microseconds (1500). The service thread hands the frames over, 1 ms apart at most. */
#define RXQ_N 48
static struct { uint64_t due; int len, answer; uint8_t frm[1700]; } rxq[RXQ_N];
static int rxq_head, rxq_tail, rx_delay_us = 1500;
static void rx_later(void *mem, const uint8_t *frm, int len) {
    if (rx_delay_us <= 0) { rx_queue(mem, frm, len); return; }
    int nx = (rxq_tail + 1) % RXQ_N;
    if (len <= 0 || len > (int)sizeof rxq[0].frm || nx == rxq_head) { n_rxdrop++; return; }
    rxq[rxq_tail].due = mono_us() + (uint64_t)rx_delay_us; rxq[rxq_tail].len = len; rxq[rxq_tail].answer = in_tx;
    memcpy(rxq[rxq_tail].frm, frm, len);
    rxq_tail = nx;
}
/* the driver has acknowledged the end of its transmission: its answers may come in now */
static void rx_answers_due(void) {
    for (int i = rxq_head; i != rxq_tail; i = (i + 1) % RXQ_N) if (rxq[i].answer) { rxq[i].due = 0; rxq[i].answer = 0; }
}
static void rx_due(void *mem) {
    uint64_t now = mono_us();
    while (rxq_head != rxq_tail && rxq[rxq_head].due <= now) {
        rx_queue(mem, rxq[rxq_head].frm, rxq[rxq_head].len);
        rxq_head = (rxq_head + 1) % RXQ_N;
    }
}
/* The channel the access point names in its beacons and probe responses. DraStic's firmware image has no radio
 * tables (every channel change writes zeros to the RF chip), so the channel the DS is on cannot be read from what
 * the driver does. DSFLIP_WFC_CHANNEL: a channel (1..13), 0 to leave the DS parameter set out, "cycle" to name
 * the next channel in every frame. */
static int adv_ch = 1, adv_cycle;
static int adv_channel(void) {
    static int c;
    if (!adv_cycle) return adv_ch;
    c = c % 13 + 1;
    return c;
}

static void inject_eth(void *ud, const uint8_t dst[6], const uint8_t src[6], uint16_t ethertype,
                       const uint8_t *payload, int len) {
    (void)ud;
    void *mem = g_mem;
    if (!mem || !active) return;
    uint8_t frm[2048];
    const uint8_t *da = dst;
    int n = wififrame_fromds(frm, sizeof frm, da, BSSID, src, seqno++, ethertype, payload, len);
    if (n > 0) rx_later(mem, frm, n);
}
static void net_log(void *ud, const char *line) { (void)ud; dsflip_log("[wfc] %s\n", line); }

static void latch_rx(void *mem) {
    uint16_t b = raw16(mem, 0x50), e = raw16(mem, 0x52), w = raw16(mem, 0x56);
    if (b < 0x4000 || e > 0x6000 || e <= b) { b = 0x4000; e = 0x4800; }
    rx_begin = b & ~1; rx_end = e & ~1;
    orig_st16(mem, 0x0054, w);
    rx_latched = 1;
}

static void note_mac(const uint8_t *mac) {
    if (!mac || (mac[0] == 0xff && mac[1] == 0xff)) return;
    if (net) wfcnet_note_mac(net, mac);
}

static uint64_t tsf_now(void) { return us_now(); }

static void reply_mgmt(void *mem, const uint8_t *da, int kind, const char *ssid) {
    uint8_t frm[320];
    int n = -1;
    if (kind == 0) n = wififrame_probe_resp(frm, sizeof frm, BSSID, da, ssid, adv_channel(), seqno++, tsf_now());
    else if (kind == 1) n = wififrame_auth(frm, sizeof frm, BSSID, da, seqno++);
    else n = wififrame_assoc_resp(frm, sizeof frm, BSSID, da, ssid, seqno++);
    if (n > 0) rx_later(mem, frm, n);
}

static void on_assoc(const char *ssid) {
    if (ssid && ssid[0]) snprintf(joined_ssid, sizeof joined_ssid, "%s", ssid);
    if (assoc) return;
    assoc = 1; want_toast = 1;
    dsflip_log("[wfc] associated%s%s, dns %s\n", joined_ssid[0] ? " to " : "", joined_ssid, srv_ip);
}

static void handle_frame(void *mem, const uint8_t *frm, int len) {
    if (len < 24) return;
    unsigned fc = frm[0] | (frm[1] << 8);
    int type = (fc >> 2) & 3, subtype = (fc >> 4) & 0xf;
    const uint8_t *sa = frm + 10;
    note_mac(sa);
    if (trace) TR("tx fc %04x len %d\n", fc, len); else if (debug) dsflip_log("[wfc] tx fc %04x len %d\n", fc, len);
    if (type == 0) {
        char ssid[33]; ssid[0] = 0;
        if (subtype == 4) {                           /* probe request */
            int sl = len >= 24 ? wififrame_ie_ssid(frm + 24, len - 24, ssid, sizeof ssid) : -1;
            if (k_probe) reply_mgmt(mem, sa, 0, sl > 0 ? ssid : "rocknixds");
        } else if (subtype == 11) reply_mgmt(mem, sa, 1, 0);          /* auth */
        else if (subtype == 0 || subtype == 2) {                      /* assoc / reassoc: cap, listen, then IEs */
            if (len >= 28) wififrame_ie_ssid(frm + 28, len - 28, ssid, sizeof ssid);
            reply_mgmt(mem, sa, 2, ssid[0] ? ssid : "rocknixds");
            on_assoc(ssid[0] ? ssid : "rocknixds");
        }
        return;
    }
    if (type != 2 || fc & 0x0040) return;             /* not data, or WEP: local multiplayer stays local */
    int qos = subtype >= 8;
    int hdr = 24 + (qos ? 2 : 0);
    if (len < hdr + 8) return;
    const uint8_t *llc = frm + hdr;
    if (llc[0] != 0xaa || llc[1] != 0xaa || llc[2] != 0x03) return;
    unsigned et = (llc[6] << 8) | llc[7];
    const uint8_t *da = frm + 16;
    if (net) wfcnet_input(net, da, sa, et, llc + 8, len - hdr - 8);
}

/* Sends what the slots hold: a slot goes out when its W_TXBUF_LOCn has bit 15 and W_TXREQ_READ allows it (the
 * driver sets W_TXREQ_SET once and then only writes the slot registers). One slot per call, LOC3 before LOC2 before
 * LOC1 as the hardware orders them, each with its own W_TXSTAT and TX-end interrupt; the service thread sends the
 * rest. The frame's TX header gets its status word (1: sent). */
static void do_tx(void *mem, uint16_t mask) {
    static const uint16_t locreg[4] = { 0x00a0, 0x0090, 0x00a4, 0x00a8 }; /* LOC1, CMD, LOC2, LOC3 */
    static const int order[4] = { 3, 2, 0, 1 };
    static const uint16_t stat[4] = { 0x0001, 0x0801, 0x1001, 0x2001 };
    int sent = -1; tx_more = 0;
    for (int k = 0; k < 4; k++) {
        int i = order[k];
        if (!(mask & (1u << i))) continue;
        uint16_t loc = raw16(mem, locreg[i]);
        if (!(loc & 0x8000)) continue;
        if (sent >= 0) { tx_more = 1; break; }
        unsigned byte = 0x4000u + (unsigned)(loc & 0x0fff) * 2u;
        uint8_t *p = mac_base(mem) + (byte & 0x3fff);
        unsigned flen = p[10] | (p[11] << 8);         /* includes the 4-byte FCS the hardware would add */
        TR("tx slot %d loc %04x len %u rate %02x fc %02x%02x\n", i, loc, flen, p[8], p[13], p[12]);
        in_tx = 1;
        if (flen >= 4 && flen < 2400) handle_frame(mem, p + 12, (int)flen - 4);
        in_tx = 0;
        p[0] = 1; p[1] = 0; p[5] = 0;                 /* TX header: sent, no retries */
        orig_st16(mem, locreg[i], loc & (uint16_t)~0x8000);
        sent = i; n_tx++;
    }
    if (sent < 0) return;
    orig_st16(mem, 0x00b8, stat[sent]);               /* which slot finished, not failed */
    orig_st16(mem, 0x00b6, 0);
    wifi_if(mem, (1u << 1) | (1u << 7));              /* TX end and TX start */
}

/* The access point's beacon: every 100 TU by the access point's own clock, and a few milliseconds after the driver
 * wakes the radio, so a passive scan that stays ~30 ms on a channel hears one on every channel it visits. The DS's
 * own beacon timer (W_BEACONCOUNT1, reloaded from W_BEACONINT: 500 while it scans) is a different thing and only
 * raises its interrupt (svc_tick). */
static void ap_beacon(void *mem) {
    uint64_t now = mono_us();
    if (!ap_next) ap_next = now + 102400;
    if (now < ap_next) return;
    ap_next = now + 102400;
    if (!mode_on || !rx_on || !rx_latched || !k_bcn || k_old) return;
    uint8_t frm[256];
    int n = wififrame_beacon(frm, sizeof frm, BSSID, "rocknixds", adv_channel(), seqno++, tsf_now());
    if (n > 0) rx_queue(mem, frm, n);
    n_beacon++;
}

static uint16_t us_half(unsigned off) {
    unsigned sh = (off - 0xf8) * 8;
    return (us_now() >> sh) & 0xffff;
}
static void us_store_half(unsigned off, uint16_t v) {
    unsigned sh = (off - 0xf8) * 8;
    uint64_t cur = us_now();
    cur = (cur & ~(0xffffull << sh)) | ((uint64_t)v << sh);
    us_set(cur);
}
static uint64_t cmp_now(void *mem) {
    uint64_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint64_t)raw16(mem, 0xf0 + i * 2) << (16 * i);
    return v;
}

static int ensure(void *mem) {
    g_mem = mem;
    if (layout_ok) return 1;
    if (layout_tried) return 0;
    layout_tried = 1;
    if (layout_check(mem)) { layout_ok = 1; return 1; }
    fail_reason = "register layout";
    active = 0;
    return 0;
}

static uint32_t hook_load8(void *mem, uint32_t addr) {
    n_ld8++;
    uint32_t h = hook_load16(mem, addr & ~1u);
    return (addr & 1) ? (h >> 8) & 0xff : h & 0xff;
}
static uint32_t hook_load32(void *mem, uint32_t addr) {
    n_ld32++;
    uint16_t o = regoff(addr);
    if (!is_ram(addr) && (o == 0xf8 || o == 0xfc)) {
        wlock();
        unsigned sh = (o - 0xf8) * 8;
        uint32_t v = (uint32_t)(us_now() >> sh);
        wunlock();
        return v;
    }
    return hook_load16(mem, addr) | (hook_load16(mem, addr + 2) << 16);
}
static void hook_store8(void *mem, uint32_t addr, uint32_t val) {
    n_st8++; RAWTR("st8", addr, val);
    uint16_t o = regoff(addr);
    if (!is_ram(addr) && (o == 0x10 || o == 0x11)) {
        wlock();
        if (ensure(mem)) {
            uint16_t old = raw16(mem, 0x10);
            uint16_t mask = (addr & 1) ? ((val & 0xff) << 8) : (val & 0xff);
            orig_st16(mem, 0x0010, old & ~mask);
        }
        wunlock();
        return;
    }
    uint32_t h = hook_load16(mem, addr & ~1u);
    if (addr & 1) h = (h & 0x00ff) | ((val & 0xff) << 8);
    else h = (h & 0xff00) | (val & 0xff);
    hook_store16(mem, addr & ~1u, h);
}
static void hook_store32(void *mem, uint32_t addr, uint32_t val) {
    n_st32++; RAWTR("st32", addr, val);
    uint16_t o = regoff(addr);
    if (!is_ram(addr) && (o == 0xf8 || o == 0xfc)) {
        wlock();
        if (ensure(mem)) {
            unsigned sh = (o - 0xf8) * 8;
            uint64_t cur = us_now();
            cur = (cur & ~(0xffffffffull << sh)) | ((uint64_t)(val) << sh);
            us_set(cur);
        }
        wunlock();
        return;
    }
    hook_store16(mem, addr, val & 0xffff);
    hook_store16(mem, addr + 2, val >> 16);
}

static uint32_t hook_load16(void *mem, uint32_t addr) {
    n_ld16++;
    if (!hooked) return orig_ld16(mem, addr);
    if (is_ram(addr)) { wlock(); ensure(mem); uint32_t r = orig_ld16(mem, fold_ram(addr)); wunlock(); return r; }
    uint16_t o = regoff(addr);
    wlock();
    uint32_t r;
    if (!ensure(mem)) { r = orig_ld16(mem, addr); wunlock(); return r; }
    switch (o) {
    case 0x000: r = 0x1440; break;                    /* W_ID */
    case 0x004: r = raw16(mem, 0x004) & (uint16_t)~0x8000; break;
    case 0x03c: r = (raw16(mem, 0x03c) & (uint16_t)~0x0200) | 0x0200; break;
    case 0x044: {                                     /* 11-bit LFSR, never 0 */
        uint16_t x = rnd & 0x7ff; if (!x) x = 1;
        uint16_t rol = ((x << 1) | (x >> 10)) & 0x7ff;
        rnd = ((x & 1) ^ rol) & 0x7ff; if (!rnd) rnd = 1;
        r = x; break;
    }
    case 0x060: {                                     /* circular-buffer read port */
        uint16_t adr = raw16(mem, 0x58) & 0x1ffe;
        uint8_t *p = mac_base(mem) + ((0x4000u + adr) & 0x3fff);
        r = p[0] | (p[1] << 8);
        unsigned nxt = (adr + 2) & 0x1ffe;
        unsigned end = (rx_end - 0x4000) & 0x1ffe, begin = (rx_begin - 0x4000) & 0x1ffe;
        if (nxt == end) nxt = begin;
        orig_st16(mem, 0x0058, (uint16_t)nxt);
        break;
    }
    case 0x0b0: r = txreq; break;                     /* W_TXREQ_READ */
    case 0x0b6: r = 0; break;                         /* TX busy */
    case 0x0f8: case 0x0fa: case 0x0fc: case 0x0fe: r = us_half(o); break;
    case 0x11c: {
        if (!bc_armed) { r = raw16(mem, 0x11c); break; }
        int left = bc_left - (int)((us_now() - bc_stamp) / 1000);
        r = left > 0 ? left : 0; break;
    }
    case 0x15e: case 0x180: r = 0; break;             /* BB / RF busy */
    default: r = orig_ld16(mem, addr); break;
    }
    if (trace) {
        unsigned i = (o & 0xfff) >> 1;
        int vol = o == 0x044 || (o >= 0x0f8 && o <= 0x0fe) || o == 0x11c || o == 0x060;   /* change on every read */
        if (!rd_seen[i] || (!vol && rd_last[i] != (uint16_t)r)) TR("r %03x->%04x\n", o, r & 0xffff);
        rd_seen[i] = 1; rd_last[i] = (uint16_t)r;
    }
    wunlock();
    return r;
}

static void hook_store16(void *mem, uint32_t addr, uint32_t val) {
    n_st16++;
    if (trace && !is_ram(addr) && raw_logged < 40) {
        raw_logged++;
        dsflip_log("[wfc] raw st16 %08x %08x from %p (binary at %lx)\n", addr, val, __builtin_return_address(0), (unsigned long)g_base);
    }
    if (!hooked) { orig_st16(mem, addr, val); return; }
    if (is_ram(addr)) { wlock(); ensure(mem); orig_st16(mem, fold_ram(addr), val); wunlock(); return; }
    uint16_t o = regoff(addr), v = (uint16_t)val;
    wlock();
    if (!ensure(mem)) { orig_st16(mem, addr, val); wunlock(); return; }
    if (trace) {
        static unsigned lo = ~0u, lv = ~0u; static int reps;
        if (o == lo && v == lv) reps++;
        else {
            if (reps) TR("  (%d more)\n", reps);
            reps = 0; lo = o; lv = v;
            TR("w %03x=%04x\n", o, v);
        }
    }
    switch (o) {
    case 0x004:
        orig_st16(mem, addr, v & (uint16_t)~0x8000);
        if (v & 0x8000 && net) wfcnet_reset(net);
        mode_on = v & 1;
        break;
    case 0x03c:                                       /* W_POWERSTATE: bit 1 asks the radio to wake up (a scan does on every channel) */
        orig_st16(mem, addr, v);
        if ((v & 2) && mode_on && k_wake > 0) { uint64_t soon = mono_us() + (uint64_t)k_wake; if (!ap_next || soon < ap_next) ap_next = soon; }
        break;
    case 0x010:                                       /* write-1-to-clear */
        if_put(mem, raw16(mem, 0x10) & ~v);
        if ((v & 2) && k_ack) { rx_answers_due(); rx_due(mem); }   /* TX end acknowledged: the answers to that frame */
        break;
    case 0x030:
        orig_st16(mem, addr, v);
        if (v & 1) latch_rx(mem);
        rx_on = (v & 0x8000) != 0;
        if (rx_on && !bc_armed) { bc_armed = 1; bc_left = 100; bc_stamp = us_now(); }
        break;
    case 0x0ac:                                       /* W_TXREQ_RESET */
        txreq &= (uint16_t)~v;
        orig_st16(mem, addr, v);
        break;
    case 0x0ae:                                       /* W_TXREQ_SET */
        txreq |= v;
        orig_st16(mem, addr, v);
        do_tx(mem, txreq);
        break;
    case 0x090: case 0x0a0: case 0x0a4: case 0x0a8:   /* W_TXBUF_CMD, LOC1..3: bit 15 queues the slot */
        orig_st16(mem, addr, v);
        if (v & 0x8000) do_tx(mem, txreq);
        break;
    case 0x0b4: {                                     /* W_TXBUF_RESET: takes queued slots back */
        static const uint16_t rr[4] = { 0x00a0, 0x0090, 0x00a4, 0x00a8 };
        for (int i = 0; i < 4; i++) if (v & (1u << i)) orig_st16(mem, rr[i], raw16(mem, rr[i]) & 0x7fff);
        orig_st16(mem, addr, v);
        break;
    }
    case 0x0f8: case 0x0fa: case 0x0fc: case 0x0fe:
        us_store_half(o, v);
        break;
    case 0x11c:
        bc_left = v; bc_stamp = us_now(); bc_armed = 1;
        orig_st16(mem, addr, v);
        break;
    case 0x21c:                                       /* IF set */
        orig_st16(mem, addr, v);
        wifi_if(mem, v);
        break;
    default:
        if (o == 0x012) {
            uint16_t iff = raw16(mem, 0x10), oldie = raw16(mem, 0x12);
            orig_st16(mem, addr, v);
            if ((iff & oldie) == 0 && (iff & v)) arm7_irq(mem);
        } else orig_st16(mem, addr, v);
        break;
    }
    wunlock();
}

static void svc_tick(void) {
    void *mem = g_mem;
    if (!mem || !layout_ok) return;
    if (rx_on && rx_latched && bc_armed) {
        int left = bc_left - (int)((us_now() - bc_stamp) / 1000);
        int irq14 = 0;
        if (left <= 0) {
            int period = raw16(mem, 0x8c);
            if (period <= 0) period = 100;
            bc_left = period; bc_stamp = us_now();
            orig_st16(mem, 0x011c, (uint16_t)period);
            irq14 = 1;
            if (k_old) {                              /* as before: the access point's beacon at the DS's own beacon time */
                uint8_t frm[256];
                int n = wififrame_beacon(frm, sizeof frm, BSSID, "rocknixds", adv_channel(), seqno++, tsf_now());
                if (n > 0) rx_queue(mem, frm, n);
                n_beacon++;
            }
        }
        if (raw16(mem, 0xea) & 1) {
            uint64_t cmp = cmp_now(mem), now = us_now();
            if (cmp && now >= cmp && cmp_fired != cmp) { cmp_fired = cmp; irq14 = 1; }
        }
        if (irq14 && (mode_on || !k_gate)) {
            if (k_txclr) txreq &= 0xfff2;                          /* the hardware takes LOC1..3 back at the beacon time; the driver sets them again */
            wifi_if(mem, 1u << 14);
        }
    }
    if (tx_more) do_tx(mem, txreq);
    if (trace_cfg) {
        static int chk;
        if (++chk >= 10) {
            chk = 0;
            FILE *kf = fopen("/tmp/wfc-knobs", "r");
            if (kf) {
                char k[32]; int v; static char was[160]; char now_s[160];
                while (fscanf(kf, " %31[a-z]=%d", k, &v) == 2) {
                    if (!strcmp(k, "ch")) { adv_cycle = v < 0; if (v >= 0 && v <= 14) adv_ch = v; }
                    else if (!strcmp(k, "delay")) rx_delay_us = v;
                    else if (!strcmp(k, "probe")) k_probe = v;
                    else if (!strcmp(k, "bcn")) k_bcn = v;
                    else if (!strcmp(k, "wake")) k_wake = v;
                    else if (!strcmp(k, "old")) k_old = v;
                    else if (!strcmp(k, "gate")) k_gate = v;
                    else if (!strcmp(k, "txclr")) k_txclr = v;
                    else if (!strcmp(k, "ack")) k_ack = v;
                }
                fclose(kf);
                snprintf(now_s, sizeof now_s, "ch %d%s delay %d probe %d bcn %d wake %d old %d gate %d txclr %d ack %d", adv_ch, adv_cycle ? " (cycling)" : "", rx_delay_us, k_probe, k_bcn, k_wake, k_old, k_gate, k_txclr, k_ack);
                if (strcmp(now_s, was)) { dsflip_log("[wfc] knobs: %s\n", now_s); strcpy(was, now_s); }
            }
            int on = access("/tmp/wfc-trace", F_OK) == 0;
            if (on && !trace) { tr_lines = 0; tr_t0 = mono_us(); memset(rd_seen, 0, sizeof rd_seen); dsflip_log("[wfc] trace on\n"); }
            if (!on && trace) dsflip_log("[wfc] trace off (%d lines)\n", tr_lines);
            trace = on;
        }
    }
    if (trace_cfg) {
        static int tick; static char last[320];
        if (++tick >= 200) {
            tick = 0;
            char b[320];
            snprintf(b, sizeof b, "mode %04x rxcnt %04x (on %d latched %d) IF %04x IE %04x pwr %04x/%04x/%04x txreq %04x buf %04x-%04x wr %04x rd %04x crop %04x filt %04x/%04x bssid %04x%04x%04x bcn %d (armed %d) rxq %d drop %d off %d tx %d calls ld %d/%d/%d st %d/%d/%d",
                     raw16(mem, 0x04), raw16(mem, 0x30), rx_on, rx_latched, raw16(mem, 0x10), raw16(mem, 0x12), raw16(mem, 0x36),
                     raw16(mem, 0x3c), raw16(mem, 0x40), txreq, rx_begin, rx_end, raw16(mem, 0x54), raw16(mem, 0x5a), raw16(mem, 0xda),
                     raw16(mem, 0xd0), raw16(mem, 0xe0), raw16(mem, 0x20), raw16(mem, 0x22), raw16(mem, 0x24), n_beacon, bc_armed,
                     n_rxq, n_rxdrop, n_rxoff, n_tx, n_ld8, n_ld16, n_ld32, n_st8, n_st16, n_st32);
            if (strcmp(b, last)) { dsflip_log("[wfc] state: %s\n", b); strcpy(last, b); }
        }
    }
    if (net) wfcnet_poll(net);
    if (want_toast && !toasted) {
        dsflip_toast("Wi-Fi", srv_name, 0x6ab0ff, 4000);
        toasted = 1;
    }
}

static uintptr_t load_bias(void);
static void *svc(void *p) {
    (void)p;
    usleep(300000);                                   /* dsflip_log's file is opened by a later constructor */
    if (!active) { dsflip_log("[wfc] online left off: %s\n", fail_reason ? fail_reason : "not hooked"); return 0; }
    dsflip_log("[wfc] online via %s (%s)\n", srv_name, srv_ip);
    if (debug) dsflip_log("[wfc] access point: channel %s%d, answers after %d us\n", adv_cycle ? "cycling, now " : "", adv_ch, rx_delay_us);
    if (trace_cfg) {
        uintptr_t base = load_bias();
        uint64_t *ld = (uint64_t *)(base + WIFI_LOAD_TBL), *st = (uint64_t *)(base + WIFI_STORE_TBL);
        dsflip_log("[wfc] tables at %p: ld %lx %lx %lx st %lx %lx %lx; hooks ld16 %p st16 %p; base %lx\n", (void *)ld, (unsigned long)ld[0],
                   (unsigned long)ld[1], (unsigned long)ld[2], (unsigned long)st[0], (unsigned long)st[1], (unsigned long)st[2],
                   (void *)hook_load16, (void *)hook_store16, (unsigned long)base);
    }
    for (int turn = 0; active; turn++) {
        usleep(1000);
        wlock();
        if (g_mem && layout_ok) { rx_due(g_mem); ap_beacon(g_mem); }
        if (turn % 10 == 9) svc_tick();
        wunlock();
    }
    return 0;
}

/* ---------- settings ---------- */
static int cfg_get(const char *path, const char *key, char *out, size_t n) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char line[512]; size_t kl = strlen(key); int found = 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            char *v = line + kl + 1;
            while (*v == ' ' || *v == '\t' || *v == '"') v++;
            v[strcspn(v, "\r\n")] = 0;
            size_t L = strlen(v);
            while (L && (v[L - 1] == ' ' || v[L - 1] == '"')) v[--L] = 0;
            snprintf(out, n, "%s", v); found = 1;
        }
    fclose(f);
    return found && out[0];
}
static int rom_base(char *out, size_t n) {
    int fd = open("/proc/self/cmdline", O_RDONLY); if (fd < 0) return 0;
    char buf[4096]; int len = read(fd, buf, sizeof buf - 1); close(fd);
    if (len <= 0) return 0;
    buf[len] = 0;
    for (int i = 0; i < len; ) {
        char *a = buf + i; int al = strlen(a); i += al + 1;
        if (al > 4 && !strcasecmp(a + al - 4, ".nds")) {
            const char *b = strrchr(a, '/'); b = b ? b + 1 : a;
            snprintf(out, n, "%s", b); return 1;
        }
    }
    return 0;
}
static int parse_ip(const char *s, uint32_t *ip) {
    unsigned a, b, c, d; char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    *ip = htonl((a << 24) | (b << 16) | (c << 8) | d);
    return 1;
}
static int known_server(const char *s, uint32_t *ip, char *name, size_t nn) {
    if (!s || !s[0] || !strcasecmp(s, "off") || !strcmp(s, "0") || !strcasecmp(s, "none") || !strcasecmp(s, "disable"))
        return 0;
    if (!strcasecmp(s, "kaeru")) { *ip = htonl(0xb23e2bd4u); snprintf(name, nn, "Kaeru WFC"); return 1; }          /* 178.62.43.212 */
    if (!strcasecmp(s, "altwfc")) { *ip = htonl(0xac6858edu); snprintf(name, nn, "AltWFC"); return 1; }           /* 172.104.88.237 */
    if (!strcasecmp(s, "wiilink") || !strcasecmp(s, "riiconnect24")) {
        *ip = htonl(0xa7ebe524u); snprintf(name, nn, "WiiLink"); return 1;                                       /* 167.235.229.36 */
    }
    if (parse_ip(s, ip)) { snprintf(name, nn, "%s", s); return 1; }
    return 0;
}
static int setting_on(void) {
    char v[64]; v[0] = 0;
    const char *e = getenv("DSFLIP_WFC");
    if (e && e[0]) snprintf(v, sizeof v, "%s", e);
    else {
        char rom[256], path[512];
        int have_rom = rom_base(rom, sizeof rom);
        if (have_rom) {
            /* ROCKNIX keeps per-game settings in system.cfg as nds["<rom>.nds"].<key>= (see session.sh) */
            snprintf(path, sizeof path, "nds[\"%s\"].wfc_dns", rom);
            cfg_get(SYSCFG, path, v, sizeof v);
        }
        if (!v[0] && have_rom) {
            snprintf(path, sizeof path, "/storage/.config/system/configs/nds/%s.cfg", rom);
            if (!cfg_get(path, "nds.wfc_dns", v, sizeof v) && !cfg_get(path, "nds.wfc", v, sizeof v) &&
                !cfg_get(path, "wfc_dns", v, sizeof v)) {
                char *dot = strrchr(rom, '.'); if (dot) *dot = 0;
                snprintf(path, sizeof path, "/storage/.config/system/configs/nds/%s.cfg", rom);
                if (!cfg_get(path, "nds.wfc_dns", v, sizeof v)) cfg_get(path, "wfc_dns", v, sizeof v);
            }
        }
        if (!v[0] && !cfg_get(SYSCFG, "nds.wfc_dns", v, sizeof v)) cfg_get(SYSCFG, "nds.wfc", v, sizeof v);
    }
    if (!known_server(v, &dns_ip, srv_name, sizeof srv_name)) return 0;
    uint8_t *d = (uint8_t *)&dns_ip;
    snprintf(srv_ip, sizeof srv_ip, "%u.%u.%u.%u", d[0], d[1], d[2], d[3]);
    return 1;
}

static int build_ok(void) {
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    unsigned char eh[64];
    if (read(fd, eh, 64) != 64 || memcmp(eh, "\177ELF", 4) || eh[4] != 2 || eh[5] != 1) { close(fd); return 0; }
    uint64_t phoff; uint16_t phsz, phn;
    memcpy(&phoff, eh + 32, 8); memcpy(&phsz, eh + 54, 2); memcpy(&phn, eh + 56, 2);
    if (phsz < 56 || phn > 64) { close(fd); return 0; }
    int match = 0, found = 0;
    for (uint16_t i = 0; i < phn && !found; i++) {
        unsigned char ph[56];
        if (pread(fd, ph, 56, (off_t)(phoff + (uint64_t)i * phsz)) != 56) break;
        uint32_t ptype; uint64_t off, sz;
        memcpy(&ptype, ph, 4); memcpy(&off, ph + 8, 8); memcpy(&sz, ph + 32, 8);
        if (ptype != 4 || sz < 12 || sz > (1u << 20)) continue;
        unsigned char *buf = malloc(sz);
        if (!buf || pread(fd, buf, sz, (off_t)off) != (ssize_t)sz) { free(buf); continue; }
        uint64_t p = 0;
        while (p + 12 <= sz) {
            uint32_t ns, ds, ty;
            memcpy(&ns, buf + p, 4); memcpy(&ds, buf + p + 4, 4); memcpy(&ty, buf + p + 8, 4);
            p += 12;
            if (ns > sz || p + ns > sz) break;
            const unsigned char *name = buf + p;
            p += (ns + 3u) & ~3u;
            if (ds > sz || p + ds > sz) break;
            if (ty == 3 && ns >= 4 && !memcmp(name, "GNU", 4) && ds == 20) {
                found = 1; match = !memcmp(buf + p, BUILD_ID, 20);
            }
            p += (ds + 3u) & ~3u;
        }
        free(buf);
    }
    close(fd);
    return match;
}

static uintptr_t load_bias(void) {
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return 0;
    exe[n] = 0;
    struct stat se; if (stat(exe, &se)) return 0;
    FILE *f = fopen("/proc/self/maps", "r"); if (!f) return 0;
    char line[768]; uintptr_t bias = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, off; char perms[8]; int poff = 0;
        if (sscanf(line, "%lx-%*x %7s %lx %*s %*s %n", &lo, perms, &off, &poff) < 3) continue;
        if (perms[0] != 'r' || perms[2] != 'x') continue;
        char *path = line + poff; path[strcspn(path, "\n")] = 0;
        struct stat sm;
        if (path[0] == '/' && !stat(path, &sm) && sm.st_ino == se.st_ino && sm.st_dev == se.st_dev) {
            bias = lo - off; break;                   /* first LOAD: p_vaddr == p_offset == 0 */
        }
    }
    fclose(f);
    return bias;
}

static int protect(void *page, size_t n, int prot) {
    if (mprotect(page, n, prot)) return 0;
    return 1;
}
static int patch_tables(uintptr_t base) {
    uint64_t *ld = (uint64_t *)(base + WIFI_LOAD_TBL);
    uint64_t *st = (uint64_t *)(base + WIFI_STORE_TBL);
    if (ld[0] != base + OFF_LD8 || ld[1] != base + OFF_LD16 || ld[2] != base + OFF_LD32) return 0;
    if (st[0] != base + OFF_ST8 || st[1] != base + OFF_ST16 || st[2] != base + OFF_ST32) return 0;
    orig_ld8 = (wld_fn)(uintptr_t)ld[0]; orig_ld16 = (wld_fn)(uintptr_t)ld[1]; orig_ld32 = (wld_fn)(uintptr_t)ld[2];
    orig_st8 = (wst_fn)(uintptr_t)st[0]; orig_st16 = (wst_fn)(uintptr_t)st[1]; orig_st32 = (wst_fn)(uintptr_t)st[2];
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = (base + WIFI_LOAD_TBL) & ~(uintptr_t)(ps - 1);
    if (!protect((void *)page, ps, PROT_READ | PROT_WRITE)) return 0;
    ld[0] = (uint64_t)(uintptr_t)hook_load8; ld[1] = (uint64_t)(uintptr_t)hook_load16; ld[2] = (uint64_t)(uintptr_t)hook_load32;
    st[0] = (uint64_t)(uintptr_t)hook_store8; st[1] = (uint64_t)(uintptr_t)hook_store16; st[2] = (uint64_t)(uintptr_t)hook_store32;
    protect((void *)page, ps, PROT_READ);
    return 1;
}

/* Writes do not come through those tables. DraStic's recompiled ARM7 code hands every store to 0x04xxxxxx to
 * store_io_register_arm7_8/16/32 directly (the stub at 0x8d9b8: "bl store_io_register_arm7_16" with the address's low
 * 24 bits), and those handlers carry their own inlined copy of the wifi store for addresses above 0x7fffff; the map's
 * region-9 store pointers are only used by DraStic's C accessors. Seen on an RG DS Plus (2026-10-05): 15722 register
 * reads reached the hooks during the driver's start-up test and not one write, so the game talked to DraStic's stub
 * and never sent a frame. So the three I/O store handlers are patched in place: their first 16 bytes (four
 * position-independent instructions, which move to a trampoline that jumps back behind the patch) become
 * "ldr x16, #8; br x16; <wrapper>". x16 is the linker's scratch register, and the recompiler's stub saves its own
 * w16/w17 before the call. The wrappers send wifi addresses to the hooks above and everything else to the handler. */
enum { OFF_IOST8 = 0x10fa0, OFF_IOST16 = 0x117f0, OFF_IOST32 = 0x12120 };
static wst_fn io_st8, io_st16, io_st32;
static void io_store8(void *mem, uint32_t addr, uint32_t val) {
    if (addr > 0x7fffffu) hook_store8(mem, addr, val); else io_st8(mem, addr, val);
}
static void io_store16(void *mem, uint32_t addr, uint32_t val) {
    if (addr > 0x7fffffu) hook_store16(mem, addr, val); else io_st16(mem, addr, val);
}
static void io_store32(void *mem, uint32_t addr, uint32_t val) {
    if (addr > 0x7fffffu) hook_store32(mem, addr, val); else io_st32(mem, addr, val);
}
static void *patch_code(uintptr_t fn, const uint32_t expect[4], void *hook) {
    long ps = sysconf(_SC_PAGESIZE);
    uint32_t *code = (uint32_t *)fn, jump[4] = { 0x58000050u /* ldr x16, #8 */, 0xd61f0200u /* br x16 */ };
    if (memcmp(code, expect, 16)) return 0;
    uint32_t *t = mmap(0, ps, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) return 0;
    uint64_t back = fn + 16;
    memcpy(t, code, 16); memcpy(t + 4, jump, 8); memcpy(t + 6, &back, 8);
    __builtin___clear_cache((char *)t, (char *)(t + 8));
    uint64_t h = (uint64_t)(uintptr_t)hook; memcpy(jump + 2, &h, 8);
    uintptr_t page = fn & ~(uintptr_t)(ps - 1);
    size_t len = ((fn + 16 + ps - 1) & ~(uintptr_t)(ps - 1)) - page;
    if (mprotect((void *)page, len, PROT_READ | PROT_WRITE | PROT_EXEC)) { munmap(t, ps); return 0; }
    memcpy(code, jump, 16);
    mprotect((void *)page, len, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)code, (char *)(code + 4));
    return t;
}
static int patch_io_stores(uintptr_t base) {
    /* stp x29, x30, [sp, #-N]!; cmp / sub on the address; mov x29, sp; stp x19, x20, [sp, #0x10] */
    static const uint32_t e8[4] = { 0xa9bd7bfdu, 0x71085c3fu, 0x910003fdu, 0xa90153f3u }, e16[4] = { 0xa9bb7bfdu, 0x5102e824u, 0x910003fdu, 0xa90153f3u }, e32[4] = { 0xa9bb7bfdu, 0x7106203fu, 0x910003fdu, 0xa90153f3u };
    /* all three or none: check first, patch after */
    if (memcmp((void *)(base + OFF_IOST8), e8, 16) || memcmp((void *)(base + OFF_IOST16), e16, 16) ||
        memcmp((void *)(base + OFF_IOST32), e32, 16)) return 0;
    void *t8 = patch_code(base + OFF_IOST8, e8, io_store8); if (!t8) return 0;
    io_st8 = (wst_fn)t8;
    void *t16 = patch_code(base + OFF_IOST16, e16, io_store16); if (!t16) return 0;
    io_st16 = (wst_fn)t16;
    void *t32 = patch_code(base + OFF_IOST32, e32, io_store32); if (!t32) return 0;
    io_st32 = (wst_fn)t32;
    return 1;
}

/* Runs after dsflip.c's init_locks (101) and before its unprioritised init(), i.e. before DraStic's main.
 * DraStic's children (`sh -c pactl subscribe`) inherit the preload; init() marks the game process with
 * DSFLIP_IN_GAME, so in a child that variable is already set and there is nothing to hook. */
__attribute__((constructor(102))) static void wifi_init(void) {
    if (getenv("DSFLIP_IN_GAME")) return;
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&wmu, &a);
    pthread_mutexattr_destroy(&a);
    wmu_ok = 1;
    debug = getenv("DSFLIP_WFC_DEBUG") && strcmp(getenv("DSFLIP_WFC_DEBUG"), "0");
    { const char *e = getenv("DSFLIP_WFC_RXDELAY"); if (e && *e) rx_delay_us = atoi(e); }
    { const char *e = getenv("DSFLIP_WFC_CHANNEL");
      if (e && !strcmp(e, "cycle")) adv_cycle = 1; else if (e && *e) { adv_ch = atoi(e); if (adv_ch < 0 || adv_ch > 14) adv_ch = 1; } }
    trace_cfg = debug && atoi(getenv("DSFLIP_WFC_DEBUG")) >= 2;   /* /tmp/wfc-trace then switches the trace on and off */
    if (!setting_on()) return;
    if (!build_ok()) { fail_reason = "DraStic build id is not r2.5.2.2"; goto thread; }
    uintptr_t base = load_bias();
    if (!base || !patch_tables(base)) { fail_reason = "wifi handler table was not where r2.5.2.2 keeps it"; goto thread; }
    g_base = base;
    if (!patch_io_stores(base)) { fail_reason = "the ARM7 I/O store handlers do not start as r2.5.2.2's do"; goto thread; }
    wfcnet_cfg cfg; memset(&cfg, 0, sizeof cfg);
    cfg.ds_ip = htonl(0x0a0d2514u);               /* 10.13.37.20 */
    cfg.gw_ip = htonl(0x0a0d2501u);               /* 10.13.37.1 */
    cfg.dns_ip = dns_ip;
    memcpy(cfg.gw_mac, BSSID, 6);
    cfg.inject = inject_eth; cfg.log = net_log; cfg.debug = debug;
    net = wfcnet_create(&cfg);
    if (!net) { fail_reason = "out of memory"; goto thread; }
    hooked = 1; active = 1;
thread: {
        pthread_t t; pthread_attr_t at;
        pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (!pthread_create(&t, &at, svc, 0)) pthread_setname_np(t, "dsf-wfc");
        pthread_attr_destroy(&at);
    }
}
