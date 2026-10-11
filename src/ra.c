// RetroAchievements for DraStic, inside libdsflip (rcheevos rc_client).
//
// - Credentials come from ROCKNIX's own settings (ES > RetroAchievements writes them to system.cfg:
//   global.retroachievements=1, .username, .password). After the first password login only RA's
//   login token is kept, in /storage/.config/drastic/dsflip/ra.token (mode 600).
// - The ROM is identified by rcheevos' own NDS hash of the file DraStic was started with (for a .zip, of the .nds in it).
// - DS main RAM (RA addresses 0x000000-0x3FFFFF) is found inside DraStic's process: the DS keeps a
//   copy of the cartridge header at 0x027FFE00, i.e. at offset 0x3FFE00 of main RAM, so we look for the
//   ROM's header in DraStic's writable memory at that offset from a 4 MB span.
// - HTTP goes through libcurl (dlopen'd, so libdsflip doesn't depend on it) on a worker thread per request.
// - Softcore only: DraStic's savestates/fast-forward/cheats can't be blocked from outside, which hardcore requires.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"

#ifndef DSFLIP_VERSION
#define DSFLIP_VERSION "dev"
#endif

void dsflip_toast(const char *line1, const char *line2, uint32_t accent, int ms);
void ui_popup(const char *l1, const char *l2, const char *badge_png, uint32_t accent, int ms);
void ui_progress(const char *text, const char *badge_png);
void dsflip_log(const char *fmt, ...);

#define SYSCFG "/storage/.config/system/configs/system.cfg"
/* the login token and the badge cache live in DSFLIP_DATA (default: ROCKNIX's libdsflip folder) */
static char TOKEN_FILE[512], BADGE_DIR[512];      /* BADGE_DIR: achievement badges and game icons, fetched once */
#define DS_RAM_SIZE 0x400000u
#define HDR_OFF 0x3FFE00u

static rc_client_t *rc;
/* the server's warnings ("Warning: Unknown Emulator": no hardcore with this emulator) come as achievements with ids
 * from 101000001 up that unlock at once; rcheevos leaves them out of its counts, and so do our pop-ups and lists */
#define WARNING_ACH(a) ((a)->id >= 101000001u)
static volatile uint8_t *ram;           /* DS main RAM inside DraStic, once found */
/* DTCM (the ARM9's 16 KB data TCM; RA addresses 0x1000000-0x1003FFF). DraStic backs the whole DS address space with
 * one shared-memory file (/dev/shm/drastic_mapped_memory.dat, kept open, unlinked) mapped view by view at a fixed
 * base: main RAM at file offset 0, ITCM at 0x400000, DTCM at 0x410000, mapped wherever the game's CP15 puts it
 * (HeartGold: 0x027E0000, verified byte-identical). We map our own read-only view of that 16 KB from DraStic's fd,
 * so it stays valid if the game moves DTCM (DraStic remaps its view then). */
#define DTCM_ADDR 0x1000000u
#define DTCM_SIZE 0x4000u
#define DTCM_FILE_OFF 0x410000
static volatile uint8_t *dtcm;
static volatile int game_loaded, scanning, logged_in, load_started;
static char rom_path[1024], user[128], pass[256];
static uint8_t rom_hdr[0x160];
static int frames;

int audio_sfx_load(const char *path);   /* audio.c */
void audio_sfx_play(void);

/* ---------- settings ---------- */
static int cfg_get(const char *key, char *out, size_t n) {
    FILE *f = fopen(SYSCFG, "r"); if (!f) return 0;
    char line[512]; size_t kl = strlen(key); int found = 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            char *v = line + kl + 1; v[strcspn(v, "\r\n")] = 0;
            snprintf(out, n, "%s", v); found = 1;
        }
    fclose(f);
    return found;
}

/* ---------- HTTP via libcurl (dlopen) ---------- */
static void *curl_lib;
static void *(*c_init)(void); static int (*c_setopt)(void *, int, ...); static int (*c_perform)(void *);
static int (*c_getinfo)(void *, int, ...); static void (*c_cleanup)(void *);
static void *(*c_slist_append)(void *, const char *); static void (*c_slist_free)(void *);
static const char *(*c_strerror)(int);
enum { CURLOPT_WRITEDATA = 10001, CURLOPT_URL = 10002, CURLOPT_POSTFIELDS = 10015, CURLOPT_USERAGENT = 10018,
       CURLOPT_HTTPHEADER = 10023, CURLOPT_WRITEFUNCTION = 20011, CURLOPT_TIMEOUT = 13, CURLOPT_NOSIGNAL = 99,
       CURLOPT_FOLLOWLOCATION = 52, CURLOPT_ERRORBUFFER = 10010, CURLOPT_CONNECTTIMEOUT = 78,
       CURLINFO_RESPONSE_CODE = 0x200002 };

static int curl_load(void) {
    if (curl_lib) return 1;
    curl_lib = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
    if (!curl_lib) return 0;
    c_init = dlsym(curl_lib, "curl_easy_init"); c_setopt = dlsym(curl_lib, "curl_easy_setopt");
    c_perform = dlsym(curl_lib, "curl_easy_perform"); c_getinfo = dlsym(curl_lib, "curl_easy_getinfo");
    c_cleanup = dlsym(curl_lib, "curl_easy_cleanup"); c_slist_append = dlsym(curl_lib, "curl_slist_append");
    c_slist_free = dlsym(curl_lib, "curl_slist_free_all");
    c_strerror = dlsym(curl_lib, "curl_easy_strerror");
    return c_init && c_setopt && c_perform && c_getinfo && c_cleanup && c_slist_append && c_slist_free;
}

typedef struct { char *data; size_t len; } membuf;
static size_t on_body(char *p, size_t sz, size_t n, void *u) {
    membuf *m = u; size_t add = sz * n;
    char *d = realloc(m->data, m->len + add + 1); if (!d) return 0;
    memcpy(d + m->len, p, add); m->len += add; d[m->len] = 0; m->data = d;
    return add;
}

typedef struct { char *url, *post, *ctype; rc_client_server_callback_t cb; void *cbdata; } http_req;
static char user_agent[256];
/* the HTTP status of the request whose callback runs on this thread (0: no answer at all): the login callbacks tell a
 * network failure, which must not cost the saved token, from a real refusal */
static __thread long http_status;

/* the API a request is for ("r=login2"), for the log: never the post data itself, it holds the password or token */
static void api_name(const http_req *r, char *out, size_t n) {
    const char *q = r->post ? r->post : strchr(r->url, '?');
    const char *p = q ? strstr(q, "r=") : 0;
    while (p && p != q && p[-1] != '&' && p[-1] != '?') p = strstr(p + 1, "r=");
    if (!p) { snprintf(out, n, "?"); return; }
    p += 2; size_t k = strcspn(p, "&"); if (k >= n) k = n - 1;
    memcpy(out, p, k); out[k] = 0;
}

static void *http_thread(void *a) {
    http_req *r = a;
    membuf body = { 0, 0 }; long status = 0;
    char err[256] = "";
    void *h = c_init();
    void *hdrs = 0;
    int code = -1;
    if (h) {
        c_setopt(h, CURLOPT_URL, r->url);
        c_setopt(h, CURLOPT_USERAGENT, user_agent);
        c_setopt(h, CURLOPT_NOSIGNAL, 1L);
        c_setopt(h, CURLOPT_TIMEOUT, 30L);
        c_setopt(h, CURLOPT_CONNECTTIMEOUT, 10L);
        c_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        c_setopt(h, CURLOPT_ERRORBUFFER, err);
        c_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
        c_setopt(h, CURLOPT_WRITEDATA, &body);
        if (r->post) {
            c_setopt(h, CURLOPT_POSTFIELDS, r->post);
            if (r->ctype) {
                char ct[128]; snprintf(ct, sizeof ct, "Content-Type: %s", r->ctype);
                hdrs = c_slist_append(0, ct); c_setopt(h, CURLOPT_HTTPHEADER, hdrs);
            }
        }
        if ((code = c_perform(h)) == 0) c_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        c_cleanup(h);
        if (hdrs) c_slist_free(hdrs);
    }
    rc_api_server_response_t resp;
    char api[32]; api_name(r, api, sizeof api);
    char msg[320];
    if (!status) {
        /* no answer: rcheevos takes the body of a client error as the error text. It used to be empty, so the log and
         * the pop-up said "Login failed: " with nothing after it (2026-10-05) */
        snprintf(msg, sizeof msg, "no connection to RetroAchievements (%s)",
                 err[0] ? err : !h ? "libcurl failed to start" : c_strerror ? c_strerror(code) : "network error");
        dsflip_log("[ra] %s: %s (curl %d)\n", api, msg, code);
        free(body.data); body.data = 0;
        resp.body = msg; resp.body_length = strlen(msg);
        resp.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    } else {
        if (status != 200) dsflip_log("[ra] %s: HTTP %ld (%zu bytes)\n", api, status, body.len);
        resp.body = body.data ? body.data : ""; resp.body_length = body.len;
        resp.http_status_code = (int)status;
    }
    http_status = status;
    r->cb(&resp, r->cbdata);
    free(body.data); free(r->url); free(r->post); free(r->ctype); free(r);
    return 0;
}

static void server_call(const rc_api_request_t *req, rc_client_server_callback_t cb, void *cbdata, rc_client_t *c) {
    (void)c;
    if (!curl_load()) {
        static const char nolib[] = "libcurl.so.4 not found";
        rc_api_server_response_t resp = { nolib, sizeof nolib - 1, RC_API_SERVER_RESPONSE_CLIENT_ERROR };
        http_status = -1;
        cb(&resp, cbdata); return;
    }
    http_req *r = calloc(1, sizeof *r);
    r->url = strdup(req->url); r->post = req->post_data ? strdup(req->post_data) : 0;
    r->ctype = req->content_type ? strdup(req->content_type) : 0; r->cb = cb; r->cbdata = cbdata;
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, http_thread, r)) { http_thread(r); } else pthread_setname_np(t, "dsf-http");
    pthread_attr_destroy(&at);
}

/* ---------- memory ---------- */
static void dtcm_map(void) {
    if (dtcm) return;
    DIR *d = opendir("/proc/self/fd"); if (!d) return;
    struct dirent *e; int fd = -1;
    while ((e = readdir(d)) && fd < 0) {
        char l[64], t[256]; snprintf(l, sizeof l, "/proc/self/fd/%s", e->d_name);
        ssize_t n = readlink(l, t, sizeof t - 1); if (n <= 0) continue; t[n] = 0;
        if (!strncmp(t, "/dev/shm/drastic_mapped_memory.dat", 34)) fd = atoi(e->d_name);
    }
    closedir(d);
    if (fd < 0) { dsflip_log("[ra] DTCM: DraStic's shared memory file not open, DTCM achievements unavailable\n"); return; }
    void *p = mmap(0, DTCM_SIZE, PROT_READ, MAP_SHARED, fd, DTCM_FILE_OFF);
    if (p == MAP_FAILED) { dsflip_log("[ra] DTCM: mmap failed: %s\n", strerror(errno)); return; }
    dtcm = p;
    dsflip_log("[ra] DTCM mapped (fd %d, offset %#x)\n", fd, DTCM_FILE_OFF);
}

static uint32_t read_memory(uint32_t addr, uint8_t *buf, uint32_t n, rc_client_t *c) {
    (void)c;
    if (addr >= DTCM_ADDR && addr < DTCM_ADDR + DTCM_SIZE) {
        const volatile uint8_t *t = dtcm;
        if (!t) return 0;
        if (addr + n > DTCM_ADDR + DTCM_SIZE) n = DTCM_ADDR + DTCM_SIZE - addr;
        memcpy(buf, (const void *)(t + (addr - DTCM_ADDR)), n);
        return n;
    }
    const volatile uint8_t *m = ram;
    if (!m || addr >= DS_RAM_SIZE) return 0;
    if (addr + n > DS_RAM_SIZE) n = DS_RAM_SIZE - addr;
    memcpy(buf, (const void *)(m + addr), n);
    return n;
}

/* find DS main RAM: the ROM header at offset 0x3FFE00 of a 4 MB span inside one writable mapping.
 * Read through /proc/self/mem, never by dereferencing: some mappings fault (SIGBUS) when touched.
 * DraStic maps the DS address space at a fixed base and mirrors main RAM several times; prefer a
 * candidate that starts an exactly-4 MB mapping (the canonical 0x02000000 view). */
static void *scan_thread(void *a) {
    (void)a;
    FILE *f = fopen("/proc/self/maps", "r");
    int mem = open("/proc/self/mem", O_RDONLY);
    static uint8_t chunk[(1 << 20) + 0x160];
    char line[512]; int cands = 0; const uint8_t *best = 0; int best_exact = 0;
    while (f && mem >= 0 && fgets(line, sizeof line, f)) {
        unsigned long lo, hi; char perms[8]; int pathoff = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &lo, &hi, perms, &pathoff) < 3 || perms[0] != 'r' || perms[1] != 'w') continue;
        if (hi - lo < DS_RAM_SIZE) continue;
        /* device mappings (GPU/DRM buffers) fault when read; DraStic's own RAM is a shared /dev/zero mapping */
        if (pathoff && !strncmp(line + pathoff, "/dev/", 5) && strncmp(line + pathoff, "/dev/zero", 9) &&
            strncmp(line + pathoff, "/dev/shm", 8)) continue;
        int exact = hi - lo == DS_RAM_SIZE;

        for (unsigned long off = lo + HDR_OFF; off < hi; off += 1 << 20) {
            size_t want = hi - off < sizeof chunk ? hi - off : sizeof chunk;
            ssize_t got = pread(mem, chunk, want, (off_t)off);
            if (got < (ssize_t)sizeof rom_hdr) break;                        /* unreadable: skip the mapping */
            for (const uint8_t *p = chunk; (p = memmem(p, (size_t)got - (p - chunk), rom_hdr, 0x40)) != 0; p++) {
                unsigned long at = off + (unsigned long)(p - chunk);
                if ((size_t)(p - chunk) + sizeof rom_hdr > (size_t)got || memcmp(p, rom_hdr, sizeof rom_hdr)) continue;
                if (at < lo + HDR_OFF || at - HDR_OFF + DS_RAM_SIZE > hi) continue;
                cands++;
                dsflip_log("[ra] DS RAM candidate at %#lx (mapping %lx-%lx%s)\n", at - HDR_OFF, lo, hi, exact ? ", exactly 4 MB" : "");
                if (!best || (exact && !best_exact && at - HDR_OFF == lo)) { best = (const uint8_t *)(at - HDR_OFF); best_exact = exact && at - HDR_OFF == lo; }
            }
        }
    }
    if (f) fclose(f);
    if (mem >= 0) close(mem);
    dtcm_map();                                           /* before ram is published: the set loads after that */
    if (best) ram = (volatile uint8_t *)best;
    dsflip_log("[ra] RAM scan: %d candidate(s)%s%p\n", cands, best ? ", using " : ", will retry", (void *)best);
    scanning = 0;
    return 0;
}

/* ---------- images: badges and game icons, cached as files ---------- */
static int fetch_file(const char *url, const char *path) {         /* 1 if path exists afterwards */
    if (access(path, R_OK) == 0) return 1;
    if (!*url || !curl_load()) return 0;
    membuf body = { 0, 0 }; long status = 0;
    void *h = c_init(); if (!h) return 0;
    c_setopt(h, CURLOPT_URL, url); c_setopt(h, CURLOPT_USERAGENT, user_agent); c_setopt(h, CURLOPT_NOSIGNAL, 1L);
    c_setopt(h, CURLOPT_TIMEOUT, 10L); c_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    c_setopt(h, CURLOPT_WRITEFUNCTION, on_body); c_setopt(h, CURLOPT_WRITEDATA, &body);
    if (c_perform(h) == 0) c_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    c_cleanup(h);
    int ok = 0;
    if (status == 200 && body.len > 8 && !memcmp(body.data, "\x89PNG", 4)) {
        char tmp[600]; snprintf(tmp, sizeof tmp, "%s.part", path);
        FILE *f = fopen(tmp, "wb");
        if (f) { ok = fwrite(body.data, 1, body.len, f) == body.len; if (fclose(f)) ok = 0; }
        if (ok) ok = rename(tmp, path) == 0; else unlink(tmp);
    }
    free(body.data);
    return ok;
}
static void badge_file(const rc_client_achievement_t *a, char *path, size_t n) { snprintf(path, n, "%s/%s.png", BADGE_DIR, a->badge_name); }
static int badge_fetch(const rc_client_achievement_t *a, char *path, size_t n) {
    char url[512] = "";
    badge_file(a, path, n);
    rc_client_achievement_get_image_url(a, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, url, sizeof url);
    return fetch_file(url, path);
}
static void detach(void *(*fn)(void *), void *arg) {
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, fn, arg)) fn(arg); else pthread_setname_np(t, "dsf-ra");
    pthread_attr_destroy(&at);
}
/* after the game loads: its icon for the "loaded" pop-up, then every badge, so an unlock shows its badge at once */
typedef struct { char title[128], line[160], icon_url[512]; } load_msg;
static void *prefetch_thread(void *a) {
    load_msg *m = a;
    mkdir(BADGE_DIR, 0755);
    char icon[600]; const rc_client_game_t *g = rc_client_get_game_info(rc);
    snprintf(icon, sizeof icon, "%s/game-%s.png", BADGE_DIR, g && g->badge_name ? g->badge_name : "0");
    int have_icon = fetch_file(m->icon_url, icon);
    ui_popup(m->title, m->line, have_icon ? icon : 0, 0x6ab0ff, 5000);
    free(m);
    rc_client_achievement_list_t *l = rc_client_create_achievement_list(rc, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    int got = 0, n = 0;
    for (uint32_t b = 0; l && b < l->num_buckets; b++)
        for (uint32_t i = 0; i < l->buckets[b].num_achievements; i++) {
            char path[600]; n++; got += badge_fetch(l->buckets[b].achievements[i], path, sizeof path);
        }
    if (l) rc_client_destroy_achievement_list(l);
    dsflip_log("[ra] badges: %d of %d cached in %s\n", got, n, BADGE_DIR);
    if (getenv("DSFLIP_UI_DEMO")) {                  /* test hook: what an unlock and a progress update look like */
        rc_client_achievement_list_t *d = rc_client_create_achievement_list(rc, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
        if (d && d->num_buckets && d->buckets[0].num_achievements) {
            const rc_client_achievement_t *a = d->buckets[0].achievements[0];
            char path[600], l1[64]; badge_file(a, path, sizeof path);
            snprintf(l1, sizeof l1, "Achievement unlocked \xc2\xb7 %u points", a->points);
            sleep(6); audio_sfx_play(); ui_popup(l1, a->title, path, 0xffd84a, 5000);
            sleep(7); ui_progress("3/5", path);
            sleep(5); ui_progress(0, 0);
        }
        if (d) rc_client_destroy_achievement_list(d);
    }
    return 0;
}
typedef struct { char l1[128], l2[160], badge[600]; const rc_client_achievement_t *a; } unlock_msg;
static void *unlock_thread(void *a) {                /* the badge is normally cached already; fetch it if not */
    unlock_msg *m = a;
    int have = badge_fetch(m->a, m->badge, sizeof m->badge);
    ui_popup(m->l1, m->l2, have ? m->badge : 0, 0xffd84a, 5000);
    free(m);
    return 0;
}

/* ---------- events, login, game load ---------- */
static void on_event(const rc_client_event_t *e, rc_client_t *c) {
    (void)c;
    char l2[160];
    switch (e->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
        if (WARNING_ACH(e->achievement)) { dsflip_log("[ra] warning from the server: %s\n", e->achievement->title); break; }
        unlock_msg *m = calloc(1, sizeof *m);
        dsflip_log("[ra] unlocked: %s (%u)\n", e->achievement->title, e->achievement->points);
        if (!m) break;
        snprintf(m->l1, sizeof m->l1, "Achievement unlocked \xc2\xb7 %u points", e->achievement->points);
        snprintf(m->l2, sizeof m->l2, "%s", e->achievement->title);
        m->a = e->achievement;
        audio_sfx_play();
        detach(unlock_thread, m);
        break;
    }
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE: {
        char path[600]; badge_file(e->achievement, path, sizeof path);
        ui_progress(e->achievement->measured_progress, access(path, R_OK) == 0 ? path : 0);
        break;
    }
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
        ui_progress(0, 0);
        break;
    case RC_CLIENT_EVENT_GAME_COMPLETED:
        dsflip_toast("All achievements unlocked!", rc_client_get_game_info(rc)->title, 0xffd84a, 6000);
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
        snprintf(l2, sizeof l2, "%s", e->leaderboard->title);
        dsflip_toast("Leaderboard submitted", l2, 0x6ab0ff, 3000);
        break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
        dsflip_log("[ra] server error: %s\n", e->server_error->error_message);
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        dsflip_toast("RetroAchievements", "offline: unlocks will be sent later", 0xff7a4a, 3000);
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        dsflip_toast("RetroAchievements", "back online, unlocks sent", 0x6ad07a, 3000);
        break;
    }
}

static void on_load(int result, const char *err, rc_client_t *c, void *u) {
    (void)u;
    if (result != RC_OK) {
        dsflip_log("[ra] game load failed: %s\n", err ? err : "?");
        if (result == RC_NO_GAME_LOADED) dsflip_toast("RetroAchievements", "this ROM isn't recognised", 0xff7a4a, 4000);
        else dsflip_toast("RetroAchievements", err ? err : "couldn't load the game", 0xff7a4a, 4000);
        return;
    }
    const rc_client_game_t *g = rc_client_get_game_info(c);
    rc_client_user_game_summary_t s; rc_client_get_user_game_summary(c, &s);
    char l2[160];
    if (s.num_core_achievements)
        snprintf(l2, sizeof l2, "%u of %u unlocked (softcore)", s.num_unlocked_achievements, s.num_core_achievements);
    else snprintf(l2, sizeof l2, "no achievements for this game yet");
    dsflip_log("[ra] game %u '%s': %s\n", g->id, g->title, l2);
    game_loaded = 1;
    load_msg *m = calloc(1, sizeof *m);
    if (!m) { dsflip_toast(g->title, l2, 0x6ab0ff, 5000); return; }
    snprintf(m->title, sizeof m->title, "%s", g->title); snprintf(m->line, sizeof m->line, "%s", l2);
    rc_client_game_get_image_url(g, m->icon_url, sizeof m->icon_url);
    detach(prefetch_thread, m);
}

static void save_token(void) {
    const rc_client_user_t *u = rc_client_get_user_info(rc);
    if (!u || !u->token) return;
    FILE *f = fopen(TOKEN_FILE, "w"); if (!f) return;
    fprintf(f, "%s\n%s\n", u->username, u->token); fclose(f);
    chmod(TOKEN_FILE, 0600);
}

static void on_login_password(int result, const char *err, rc_client_t *c, void *u);
static void start_load(void) {
    rc_client_begin_identify_and_load_game(rc, RC_CONSOLE_NINTENDO_DS, rom_path, 0, 0, on_load, 0);
}
/* A login that got no answer (no network yet, DNS, a timeout) or a busy server's (429, 5xx) is tried again later with
 * the same credentials: it says nothing about them. Before, a failed token login deleted the token and tried the
 * password at once, which failed the same way: the player lost the saved login to a moment without network.
 * Retries at 15 s, 30 s, 60 s, then every 2 minutes; one pop-up for the first. ra_frame starts them. */
static char tok_user[128], tok_token[256];       /* the token login's credentials, for its retries */
static volatile long long retry_at; static volatile int retry_n, retry_with_token;
static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static int transient(int result) {
    long st = http_status;
    return result == RC_NO_RESPONSE || st <= 0 || st == 429 || st >= 500;
}
static void retry_later(int with_token, const char *err) {
    static const int wait_s[] = { 15, 30, 60, 120 };
    int k = retry_n < 4 ? retry_n : 3;
    dsflip_log("[ra] %s login: %s (HTTP %ld): trying again in %d s\n", with_token ? "token" : "password", err && *err ? err : "no answer",
               http_status, wait_s[k]);
    if (!retry_n) dsflip_toast("RetroAchievements: no connection", "Trying again in the background", 0xff7a4a, 4000);
    retry_with_token = with_token; retry_n++;
    retry_at = mono_ms() + wait_s[k] * 1000LL;
}
static void on_login_token(int result, const char *err, rc_client_t *c, void *u) {
    (void)c; (void)u;
    if (result == RC_OK) { dsflip_log("[ra] logged in with token as %s\n", user); retry_n = 0; logged_in = 1; return; }
    if (transient(result)) { retry_later(1, err); return; }      /* the token is kept */
    dsflip_log("[ra] token login refused (%s, HTTP %ld)%s\n", err && *err ? err : "no reason given", http_status, *pass ? ", trying the password" : "");
    unlink(TOKEN_FILE);
    if (*pass) rc_client_begin_login_with_password(rc, user, pass, on_login_password, 0);
    else dsflip_toast("RetroAchievements", "Login expired: re-enter your password in ES", 0xff7a4a, 5000);
}
static void on_login_password(int result, const char *err, rc_client_t *c, void *u) {
    (void)c; (void)u;
    if (result != RC_OK) {
        if (transient(result)) { retry_later(0, err); return; }
        dsflip_log("[ra] login failed: %s (HTTP %ld)\n", err && *err ? err : "no reason given", http_status);
        dsflip_toast("RetroAchievements login failed", err && *err ? err : "Check the user and password in ES", 0xff7a4a, 5000);
        return;
    }
    dsflip_log("[ra] logged in as %s\n", user);
    save_token();
    retry_n = 0;
    logged_in = 1;
}

/* ---------- unlock sound ----------
 * The one chosen in ES > RetroAchievements > Unlock sound (system.cfg retroachievements.sound; "none" or unset: no
 * sound), looked up like ROCKNIX's setsettings.sh does for RetroArch: this game, then the nds system, then global.
 * The files are ES's list: /storage/roms/music/retroachievements/<name>.ogg, then /usr/share/libretro/sounds. */
static char sound_path[600];
static void *sound_load_thread(void *a) { (void)a; audio_sfx_load(sound_path); return 0; }
static void sound_setup(void) {
    const char *file = getenv("DSFLIP_RA_SOUND");     /* standalone: an .ogg path, or "none" */
    if (file && *file) {
        if (!strcmp(file, "none")) { dsflip_log("[ra] unlock sound: none\n"); return; }
        snprintf(sound_path, sizeof sound_path, "%s", file); detach(sound_load_thread, 0); return;
    }
    char key[700], name[128] = "";
    const char *base = strrchr(rom_path, '/'); base = base ? base + 1 : rom_path;
    snprintf(key, sizeof key, "nds[\"%s\"].retroachievements.sound", base);
    if (!cfg_get(key, name, sizeof name) && !cfg_get("nds.retroachievements.sound", name, sizeof name))
        cfg_get("global.retroachievements.sound", name, sizeof name);
    if (!*name || !strcmp(name, "none") || strchr(name, '/')) { dsflip_log("[ra] unlock sound: none\n"); return; }
    const char *dirs[] = { "/storage/roms/music/retroachievements", "/usr/share/libretro/sounds" };
    for (int i = 0; i < 2; i++) {
        snprintf(sound_path, sizeof sound_path, "%s/%s.ogg", dirs[i], name);
        if (access(sound_path, R_OK) == 0) { detach(sound_load_thread, 0); return; }
    }
    dsflip_log("[ra] unlock sound '%s' not found\n", name);
}

static void on_rc_log(const char *msg, const rc_client_t *c) { (void)c; dsflip_log("[rc] %s\n", msg); }

/* ---------- the ROM's bytes, also from inside a .zip ----------
 * DraStic starts zipped ROMs too (ES lists .nds .zip .7z for the DS) and unpacks them in memory. rcheevos' NDS hash
 * and our RAM scan both need the .nds's own bytes: given the zip they read its zip headers, the hash failed ("arm9
 * code size ... exceeds 16MB") and the RAM scan never matched the cartridge header (ROCKNIXDS issue 31). This reader
 * hands rcheevos (rc_hash_init_custom_filereader) and ra_start the first .nds in a zip, inflated as it is read; the
 * hash only reads the header, the ARM9/ARM7 code and the icon, so nothing is unpacked whole. zlib is dlopen()ed like
 * libcurl; its z_stream is declared here (the sysroot has no zlib.h; inflateInit2_ checks the size). Stored (0) and
 * deflated (8) entries, no zip64 (DS ROMs are at most 512 MB). */
typedef struct {
    const uint8_t *next_in; unsigned avail_in; unsigned long total_in;
    uint8_t *next_out; unsigned avail_out; unsigned long total_out;
    const char *msg; void *state, *zalloc, *zfree, *opaque;
    int data_type; unsigned long adler, reserved;
} zstream_t;
static int (*z_init2)(zstream_t *, int, const char *, int), (*z_inflate)(zstream_t *, int), (*z_end)(zstream_t *);
static void zlib_load(void) {
    void *h = dlopen("libz.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) return;
    *(void **)&z_init2 = dlsym(h, "inflateInit2_"); *(void **)&z_inflate = dlsym(h, "inflate"); *(void **)&z_end = dlsym(h, "inflateEnd");
}
typedef struct {
    FILE *f;
    int zip, method, zinit;
    long data;                      /* zip: where the entry's bytes start */
    uint32_t csize, usize, cread;   /* zip: compressed and unpacked sizes, compressed bytes fed to zlib */
    int64_t pos, want;              /* zip: bytes unpacked so far, where the next read starts */
    zstream_t z;
    uint8_t in[65536];
} romfile_t;
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static int has_ext(const char *path, const char *ext) {
    size_t n = strlen(path), e = strlen(ext);
    return n > e && !strcasecmp(path + n - e, ext);
}
/* finds the first .nds in the zip's central directory (else its first file) */
static int zip_find(romfile_t *r) {
    uint8_t buf[65536 + 22];
    if (fseek(r->f, 0, SEEK_END)) return 0;
    long size = ftell(r->f), n = size < (long)sizeof buf ? size : (long)sizeof buf;
    if (n < 22 || fseek(r->f, size - n, SEEK_SET) || fread(buf, 1, n, r->f) != (size_t)n) return 0;
    long e = n - 22; while (e >= 0 && le32(buf + e) != 0x06054b50) e--;
    if (e < 0) return 0;
    uint32_t cd_size = le32(buf + e + 12), cd_off = le32(buf + e + 16);
    uint8_t *cd = malloc(cd_size);
    if (!cd || fseek(r->f, cd_off, SEEK_SET) || fread(cd, 1, cd_size, r->f) != cd_size) { free(cd); return 0; }
    long pick = -1;
    for (uint32_t i = 0; i + 46 <= cd_size && le32(cd + i) == 0x02014b50; ) {
        uint16_t nl = le16(cd + i + 28), xl = le16(cd + i + 30), cl = le16(cd + i + 32);
        if (i + 46 + nl > cd_size) break;
        char name[512]; snprintf(name, sizeof name, "%.*s", nl, (const char *)cd + i + 46);
        if (name[0] && name[strlen(name) - 1] != '/') {
            if (pick < 0) pick = i;
            if (has_ext(name, ".nds")) { pick = i; break; }
        }
        i += 46 + nl + xl + cl;
    }
    int ok = 0;
    if (pick >= 0) {
        uint8_t lh[30];
        r->method = le16(cd + pick + 10); r->csize = le32(cd + pick + 20); r->usize = le32(cd + pick + 24);
        uint32_t lo = le32(cd + pick + 42);
        if (!fseek(r->f, lo, SEEK_SET) && fread(lh, 1, 30, r->f) == 30 && le32(lh) == 0x04034b50 &&
            r->csize != 0xffffffffu && (r->method == 0 || r->method == 8)) {
            r->data = lo + 30 + le16(lh + 26) + le16(lh + 28);
            ok = 1;
        }
    }
    free(cd);
    return ok;
}
static void *rom_open(const char *path) {
    romfile_t *r = calloc(1, sizeof *r);
    if (!r || !(r->f = fopen(path, "rb"))) { free(r); return 0; }
    if (!has_ext(path, ".zip")) return r;
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, zlib_load);
    r->zip = 1;
    if (!zip_find(r) || (r->method == 8 && !(z_init2 && z_inflate && z_end))) {
        dsflip_log("[ra] no usable .nds in %s%s\n", path, z_init2 ? "" : " (no zlib)");
        fclose(r->f); free(r); return 0;
    }
    return r;
}
static void rom_seek(void *h, int64_t off, int origin) {
    romfile_t *r = h;
    if (!r->zip) { fseeko(r->f, off, origin); return; }
    r->want = origin == SEEK_SET ? off : origin == SEEK_CUR ? r->want + off : (int64_t)r->usize + off;
    if (r->want < 0) r->want = 0;
}
static int64_t rom_tell(void *h) { romfile_t *r = h; return r->zip ? r->want : ftello(r->f); }
/* inflates up to n bytes at r->pos into out (out NULL: skip them); returns the bytes produced */
static size_t zip_inflate(romfile_t *r, uint8_t *out, size_t n) {
    uint8_t skip[16384];
    size_t done = 0;
    while (done < n && r->pos < r->usize) {
        if (!r->z.avail_in && r->cread < r->csize) {
            uint32_t k = r->csize - r->cread < sizeof r->in ? r->csize - r->cread : sizeof r->in;
            if (fseek(r->f, r->data + r->cread, SEEK_SET) || fread(r->in, 1, k, r->f) != k) break;
            r->z.next_in = r->in; r->z.avail_in = k; r->cread += k;
        }
        size_t chunk = n - done; if (!out && chunk > sizeof skip) chunk = sizeof skip;
        r->z.next_out = out ? out + done : skip; r->z.avail_out = (unsigned)chunk;
        int e = z_inflate(&r->z, 0 /* Z_NO_FLUSH */);
        size_t got = chunk - r->z.avail_out;
        done += got; r->pos += got;
        if (e == 1 /* Z_STREAM_END */) break;
        if (e < 0 || (!got && !r->z.avail_in && r->cread >= r->csize)) break;
    }
    return done;
}
static size_t rom_read(void *h, void *buf, size_t n) {
    romfile_t *r = h;
    if (!r->zip) return fread(buf, 1, n, r->f);
    if (r->want >= r->usize) return 0;
    if (n > r->usize - r->want) n = r->usize - r->want;
    if (r->method == 0) {           /* stored */
        if (fseek(r->f, r->data + r->want, SEEK_SET)) return 0;
        size_t got = fread(buf, 1, n, r->f); r->want += got; return got;
    }
    if (!r->zinit || r->want < r->pos) {   /* first read, or backwards: start the stream again */
        if (r->zinit) z_end(&r->z);
        memset(&r->z, 0, sizeof r->z); r->cread = 0; r->pos = 0;
        if (z_init2(&r->z, -15 /* raw deflate */, "1.2.11", (int)sizeof r->z) != 0) { r->zinit = 0; return 0; }
        r->zinit = 1;
    }
    if (r->pos < r->want) zip_inflate(r, 0, r->want - r->pos);
    size_t got = r->pos == r->want ? zip_inflate(r, buf, n) : 0;
    r->want += got;
    return got;
}
static void rom_close(void *h) {
    romfile_t *r = h;
    if (r->zinit) z_end(&r->z);
    fclose(r->f); free(r);
}

/* called once, from the first SDL_RenderPresent (DraStic's main thread) */
static void ra_start(void) {
    char v[16] = "";
    const char *data = getenv("DSFLIP_DATA"); if (!data || !*data) data = "/storage/.config/drastic/dsflip";
    snprintf(TOKEN_FILE, sizeof TOKEN_FILE, "%s/ra.token", data);
    snprintf(BADGE_DIR, sizeof BADGE_DIR, "%s/badges", data);
    const char *eu = getenv("DSFLIP_RA_USER");        /* standalone: the account from the environment */
    if (eu && *eu) {
        const char *ep = getenv("DSFLIP_RA_PASSWORD");
        strcpy(v, "1"); snprintf(user, sizeof user, "%s", eu); snprintf(pass, sizeof pass, "%s", ep ? ep : "");
    } else {                                          /* ROCKNIX: ES > RetroAchievements (system.cfg) */
        cfg_get("global.retroachievements", v, sizeof v);
        cfg_get("global.retroachievements.username", user, sizeof user);
        cfg_get("global.retroachievements.password", pass, sizeof pass);
    }
    const char *test = getenv("DSFLIP_RA_TEST");
    /* ROM = DraStic's last argument */
    FILE *f = fopen("/proc/self/cmdline", "rb");
    if (f) {
        char buf[4096]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
        char *last = buf; for (char *p = buf; p < buf + n; p += strlen(p) + 1) if (*p) last = p;
        snprintf(rom_path, sizeof rom_path, "%s", last);
    }
    if (has_ext(rom_path, ".7z")) { dsflip_log("[ra] %s: RetroAchievements needs the .nds or a .zip, not a .7z\n", rom_path); return; }
    static rc_hash_filereader_t reader = { rom_open, rom_seek, rom_tell, rom_read, rom_close };
    rc_hash_init_custom_filereader(&reader);
    void *r = rom_open(rom_path);
    if (!r || rom_read(r, rom_hdr, sizeof rom_hdr) != sizeof rom_hdr) { if (r) rom_close(r); dsflip_log("[ra] can't read ROM %s\n", rom_path); return; }
    rom_close(r);
    if (test) {       /* offline self-test: hash + RAM discovery only */
        char hash[33] = "";
        if (!rc_hash_generate_from_file(hash, RC_CONSOLE_NINTENDO_DS, rom_path)) hash[0] = 0;
        dsflip_log("[ra] test mode: %s hash %s\n", rom_path, hash);
        dsflip_toast("RetroAchievements test", hash, 0x6ab0ff, 8000);
        rc = rc_client_create(read_memory, server_call);
        return;
    }
    if (strcmp(v, "1") || !*user) { dsflip_log("[ra] RetroAchievements off (ES: RetroAchievements settings)\n"); return; }

    sound_setup();
    rc = rc_client_create(read_memory, server_call);
    rc_client_enable_logging(rc, RC_CLIENT_LOG_LEVEL_WARN, on_rc_log);
    rc_client_set_event_handler(rc, on_event);
    rc_client_set_hardcore_enabled(rc, 0);
    char clause[128] = ""; rc_client_get_user_agent_clause(rc, clause, sizeof clause);
    snprintf(user_agent, sizeof user_agent, "dsflip/%s (ROCKNIX; RG DS) %s", DSFLIP_VERSION, clause);

    char tu[128] = "", tok[256] = "";
    FILE *tf = fopen(TOKEN_FILE, "r");
    if (tf) {
        if (fgets(tu, sizeof tu, tf) && fgets(tok, sizeof tok, tf)) { tu[strcspn(tu, "\r\n")] = 0; tok[strcspn(tok, "\r\n")] = 0; }
        fclose(tf);
    }
    if (*tok && !strcasecmp(tu, user)) {
        snprintf(tok_user, sizeof tok_user, "%s", user); snprintf(tok_token, sizeof tok_token, "%s", tok);
        rc_client_begin_login_with_token(rc, user, tok, on_login_token, 0);
    }
    else if (*pass) rc_client_begin_login_with_password(rc, user, pass, on_login_password, 0);
    else dsflip_toast("RetroAchievements", "no password set in ES", 0xff7a4a, 4000);
}

/* ---------- the frame's evaluation on its own thread ----------
 * rc_client_do_frame on DraStic's main thread was ~20% of that thread in HeartGold (136 achievements at 1104 MHz:
 * ~2.4 ms a frame), and the main thread is what sets the clock. Only the memory reads must happen at the frame
 * boundary: rc_client_do_frame_update runs here, rc_client_do_frame_evaluate on "dsf-ra". The next update waits for
 * the previous evaluation (rcheevos' delta/prior values are per frame); it has a whole frame for ~1.5 ms of work.
 * DSFLIP_RA_THREAD=0: the whole frame on the main thread, as before. */
static pthread_mutex_t ev_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ev_cv = PTHREAD_COND_INITIALIZER;
static int ev_pending, ev_thread = -1;   /* -1: not decided yet */
static unsigned ev_waits;               /* frames whose update had to wait for the previous evaluation */
static void *ev_loop(void *a) {
    (void)a;
    pthread_mutex_lock(&ev_mu);
    for (;;) {
        while (!ev_pending) pthread_cond_wait(&ev_cv, &ev_mu);
        pthread_mutex_unlock(&ev_mu);
        rc_client_do_frame_evaluate(rc);
        pthread_mutex_lock(&ev_mu);
        ev_pending = 0;
        pthread_cond_broadcast(&ev_cv);
    }
    return 0;
}
static void ev_wait_idle(void) {          /* the previous frame's evaluation has returned */
    if (ev_thread != 1) return;
    pthread_mutex_lock(&ev_mu);
    if (ev_pending) ev_waits++;
    while (ev_pending) pthread_cond_wait(&ev_cv, &ev_mu);
    pthread_mutex_unlock(&ev_mu);
}
static void ra_do_frame(void) {
    if (ev_thread < 0) {
        const char *e = getenv("DSFLIP_RA_THREAD");
        pthread_t t;
        ev_thread = !(e && *e == '0') && pthread_create(&t, 0, ev_loop, 0) == 0;
        if (ev_thread) { pthread_detach(t); pthread_setname_np(t, "dsf-ra"); }
        dsflip_log("[ra] achievements evaluated %s\n", ev_thread ? "on their own thread (dsf-ra)" : "on DraStic's main thread");
    }
    if (!ev_thread) { rc_client_do_frame(rc); return; }
    ev_wait_idle();
    rc_client_do_frame_update(rc);
    pthread_mutex_lock(&ev_mu);
    ev_pending = 1;
    pthread_cond_signal(&ev_cv);
    pthread_mutex_unlock(&ev_mu);
}

/* called from every SDL_RenderPresent */
/* the menu's progress line (menu.c): 1 with the counts once a game with achievements is loaded */
int ra_progress(unsigned *unlocked, unsigned *total) {
    if (!rc || !game_loaded) return 0;
    rc_client_user_game_summary_t s; rc_client_get_user_game_summary(rc, &s);
    *unlocked = s.num_unlocked_achievements; *total = s.num_core_achievements;
    return 1;
}

/* the menu's latest achievements (menu.c): the n most recently unlocked, newest first, with their cached badges */
typedef struct { char title[96], desc[200], badge[600]; unsigned points; long long when; } ra_recent_t;
int ra_recent(ra_recent_t *out, int max) {
    if (!rc || !game_loaded || max <= 0) return 0;
    rc_client_achievement_list_t *l = rc_client_create_achievement_list(rc, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    int n = 0;
    for (uint32_t b = 0; l && b < l->num_buckets; b++)
        for (uint32_t i = 0; i < l->buckets[b].num_achievements; i++) {
            const rc_client_achievement_t *a = l->buckets[b].achievements[i];
            if (!a->unlocked || WARNING_ACH(a)) continue;
            int k = n < max ? n++ : max;               /* insertion by unlock time, newest first */
            while (k > 0 && (long long)a->unlock_time > out[k - 1].when) { if (k < max) out[k] = out[k - 1]; k--; }
            if (k >= max) continue;
            ra_recent_t *r = &out[k];
            snprintf(r->title, sizeof r->title, "%s", a->title ? a->title : "");
            snprintf(r->desc, sizeof r->desc, "%s", a->description ? a->description : "");
            badge_file(a, r->badge, sizeof r->badge);
            r->points = a->points; r->when = (long long)a->unlock_time;
        }
    if (l) rc_client_destroy_achievement_list(l);
    return n;
}

void ra_frame(void) {
    frames++;
    if (frames == 1) ra_start();
    if (!rc) return;
    if (retry_at && !logged_in && mono_ms() >= retry_at) {     /* a login that got no answer: again (see retry_later) */
        retry_at = 0;
        dsflip_log("[ra] login: trying again (%d)\n", retry_n);
        if (retry_with_token && *tok_token) rc_client_begin_login_with_token(rc, tok_user, tok_token, on_login_token, 0);
        else if (*pass) rc_client_begin_login_with_password(rc, user, pass, on_login_password, 0);
    }
    /* the cart header is copied into RAM at boot: look once the game is running, retry every ~2 s */
    if (!ram && !scanning && frames >= 90 && frames % 120 == 90) {
        scanning = 1;
        pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, scan_thread, 0)) scanning = 0; else pthread_setname_np(t, "dsf-ramscan");
        pthread_attr_destroy(&at);
    }
    /* load the game only once DS RAM is found, so rc_client validates the achievement set against real memory
     * (loading before the async scan finished disabled every achievement with "Invalid address"). Fall back to
     * loading anyway after ~15 s so a game whose RAM we never find at least identifies. */
    if (logged_in && !load_started && (ram || frames > 900)) {
        load_started = 1;
        if (!ram) dsflip_log("[ra] RAM not found after 15 s; loading anyway (achievements may not work)\n");
        start_load();
    }
    /* RAM moved (reset / new game)? */
    if (ram && frames % 600 == 0 && memcmp((const void *)(ram + HDR_OFF), rom_hdr, 0x40)) {
        dsflip_log("[ra] header no longer at the RAM location, rescanning\n");
        ram = 0;
    }
    if (getenv("DSFLIP_RA_TEST") && ram && frames % 120 == 0) {
        uint32_t sum = 0; for (uint32_t i = 0; i < DS_RAM_SIZE; i += 64) sum = sum * 31 + ram[i];
        dsflip_log("[ra] test: RAM checksum %08x at frame %d\n", sum, frames);
        uint8_t t[DTCM_SIZE]; uint32_t got = read_memory(DTCM_ADDR, t, DTCM_SIZE, rc), nz = 0;
        for (uint32_t i = 0; i < got; i++) nz += t[i] != 0;
        dsflip_log("[ra] test: DTCM read %u bytes, %u non-zero, top word %02x%02x%02x%02x\n", got, nz,
                   t[DTCM_SIZE - 5], t[DTCM_SIZE - 6], t[DTCM_SIZE - 7], t[DTCM_SIZE - 8]);
    }
    if (game_loaded && ram) ra_do_frame();
    else { ev_wait_idle(); rc_client_idle(rc); }
    if (ev_waits && frames % 600 == 0) { dsflip_log("[ra] %u frames waited for the previous evaluation\n", ev_waits); ev_waits = 0; }
}
