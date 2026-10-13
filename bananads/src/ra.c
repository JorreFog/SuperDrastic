// RetroAchievements in BananaDS (rcheevos rc_client): SuperDrastic's src/ra.c, without what DraStic made necessary.
//
// - The account comes from the environment: BANANADS_RA_USER and BANANADS_RA_PASSWORD (BananaOS's run-bananads takes
//   them from the setup's banana/accounts.conf). After the first password login only RA's login token is kept, in
//   $BANANADS_DATA/ra.token (mode 600); the badges and game icons are cached in $BANANADS_DATA/ra-badges.
// - The game is identified by rcheevos' own NDS hash of the .nds BananaDS runs (run-bananads unpacks a .zip first).
// - Memory is melonDS's own: main RAM and the ARM9's DTCM through ra_host, read at the frame's end on the emulator's
//   thread (no search through another program's memory as in SuperDrastic).
// - HTTP goes through libcurl (dlopen'd, so BananaDS doesn't depend on it) on a worker thread per request.
// - Softcore only: savestates, the in-game menu's load and fast-forward aren't blocked, which hardcore requires.
#define _GNU_SOURCE
#include "ra.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"

#ifndef BANANADS_VERSION
#define BANANADS_VERSION "dev"
#endif

#define DS_RAM_SIZE 0x400000u
#define DTCM_ADDR 0x1000000u
#define DTCM_SIZE 0x4000u
/* the server's warnings ("Warning: Unknown Emulator") come as achievements with ids from 101000001 up that unlock at
 * once; rcheevos leaves them out of its counts, and so do the pop-ups and the menu's list */
#define WARNING_ACH(a) ((a)->id >= 101000001u)

static ra_host H;
static rc_client_t* rc;
static char TOKEN_FILE[512], BADGE_DIR[512];
static char rom_path[1024], user[128], pass[256], user_agent[256];
static volatile int game_loaded, logged_in, load_started;
static int frames;

static void ralog(const char* fmt, ...) {
    char b[600];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (H.log) H.log(H.ctx, b);
}

/* ---------- pop-ups: queued here, drawn by the frontend (menu.c's bmenu_overlay) ---------- */
typedef struct { char l1[128], l2[160], badge[600]; uint32_t accent; int ms; } popup;
#define NPOP 8
static popup pq[NPOP];
static int pq_head, pq_n;
static pthread_mutex_t pq_mu = PTHREAD_MUTEX_INITIALIZER;
static void popup_push(const char* l1, const char* l2, const char* badge, uint32_t accent, int ms) {
    pthread_mutex_lock(&pq_mu);
    if (pq_n == NPOP) { pq_head = (pq_head + 1) % NPOP; pq_n--; }  /* the oldest makes room */
    popup* p = &pq[(pq_head + pq_n++) % NPOP];
    snprintf(p->l1, sizeof p->l1, "%s", l1 ? l1 : "");
    snprintf(p->l2, sizeof p->l2, "%s", l2 ? l2 : "");
    snprintf(p->badge, sizeof p->badge, "%s", badge ? badge : "");
    p->accent = accent;
    p->ms = ms;
    pthread_mutex_unlock(&pq_mu);
}
int ra_popup_take(char* l1, int n1, char* l2, int n2, char* badge, int nb, uint32_t* accent, int* ms) {
    pthread_mutex_lock(&pq_mu);
    int got = pq_n > 0;
    if (got) {
        popup* p = &pq[pq_head];
        snprintf(l1, n1, "%s", p->l1);
        snprintf(l2, n2, "%s", p->l2);
        snprintf(badge, nb, "%s", p->badge);
        *accent = p->accent;
        *ms = p->ms;
        pq_head = (pq_head + 1) % NPOP;
        pq_n--;
    }
    pthread_mutex_unlock(&pq_mu);
    return got;
}

/* ---------- HTTP via libcurl (dlopen) ---------- */
static void* curl_lib;
static void* (*c_init)(void);
static int (*c_setopt)(void*, int, ...);
static int (*c_perform)(void*);
static int (*c_getinfo)(void*, int, ...);
static void (*c_cleanup)(void*);
static void* (*c_slist_append)(void*, const char*);
static void (*c_slist_free)(void*);
enum { CURLOPT_WRITEDATA = 10001, CURLOPT_URL = 10002, CURLOPT_POSTFIELDS = 10015, CURLOPT_USERAGENT = 10018,
       CURLOPT_HTTPHEADER = 10023, CURLOPT_WRITEFUNCTION = 20011, CURLOPT_TIMEOUT = 13, CURLOPT_NOSIGNAL = 99,
       CURLOPT_FOLLOWLOCATION = 52, CURLINFO_RESPONSE_CODE = 0x200002 };

static int curl_load(void) {
    if (curl_lib) return 1;
    curl_lib = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
    if (!curl_lib) return 0;
    c_init = (void* (*)(void))dlsym(curl_lib, "curl_easy_init");
    c_setopt = (int (*)(void*, int, ...))dlsym(curl_lib, "curl_easy_setopt");
    c_perform = (int (*)(void*))dlsym(curl_lib, "curl_easy_perform");
    c_getinfo = (int (*)(void*, int, ...))dlsym(curl_lib, "curl_easy_getinfo");
    c_cleanup = (void (*)(void*))dlsym(curl_lib, "curl_easy_cleanup");
    c_slist_append = (void* (*)(void*, const char*))dlsym(curl_lib, "curl_slist_append");
    c_slist_free = (void (*)(void*))dlsym(curl_lib, "curl_slist_free_all");
    return c_init && c_setopt && c_perform && c_getinfo && c_cleanup && c_slist_append && c_slist_free;
}

typedef struct { char* data; size_t len; } membuf;
static size_t on_body(char* p, size_t sz, size_t n, void* u) {
    membuf* m = u;
    size_t add = sz * n;
    char* d = realloc(m->data, m->len + add + 1);
    if (!d) return 0;
    memcpy(d + m->len, p, add);
    m->len += add;
    d[m->len] = 0;
    m->data = d;
    return add;
}

typedef struct { char *url, *post, *ctype; rc_client_server_callback_t cb; void* cbdata; } http_req;

static void* http_thread(void* a) {
    http_req* r = a;
    membuf body = {0, 0};
    long status = 0;
    void* h = c_init();
    void* hdrs = 0;
    if (h) {
        c_setopt(h, CURLOPT_URL, r->url);
        c_setopt(h, CURLOPT_USERAGENT, user_agent);
        c_setopt(h, CURLOPT_NOSIGNAL, 1L);
        c_setopt(h, CURLOPT_TIMEOUT, 30L);
        c_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        c_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
        c_setopt(h, CURLOPT_WRITEDATA, &body);
        if (r->post) {
            c_setopt(h, CURLOPT_POSTFIELDS, r->post);
            if (r->ctype) {
                char ct[128];
                snprintf(ct, sizeof ct, "Content-Type: %s", r->ctype);
                hdrs = c_slist_append(0, ct);
                c_setopt(h, CURLOPT_HTTPHEADER, hdrs);
            }
        }
        if (c_perform(h) == 0) c_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        c_cleanup(h);
        if (hdrs) c_slist_free(hdrs);
    }
    rc_api_server_response_t resp;
    resp.body = body.data ? body.data : "";
    resp.body_length = body.len;
    resp.http_status_code = status ? (int)status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    r->cb(&resp, r->cbdata);
    free(body.data);
    free(r->url);
    free(r->post);
    free(r->ctype);
    free(r);
    return 0;
}

static void detach(void* (*fn)(void*), void* arg, const char* name) {
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, fn, arg)) fn(arg);
    else pthread_setname_np(t, name);
    pthread_attr_destroy(&at);
}

static void server_call(const rc_api_request_t* req, rc_client_server_callback_t cb, void* cbdata, rc_client_t* c) {
    (void)c;
    if (!curl_load()) {
        rc_api_server_response_t resp = {"", 0, RC_API_SERVER_RESPONSE_CLIENT_ERROR};
        cb(&resp, cbdata);
        return;
    }
    http_req* r = calloc(1, sizeof *r);
    r->url = strdup(req->url);
    r->post = req->post_data ? strdup(req->post_data) : 0;
    r->ctype = req->content_type ? strdup(req->content_type) : 0;
    r->cb = cb;
    r->cbdata = cbdata;
    detach(http_thread, r, "bds-ra-http");
}

/* ---------- memory: melonDS's own ---------- */
static uint32_t read_memory(uint32_t addr, uint8_t* buf, uint32_t n, rc_client_t* c) {
    (void)c;
    if (addr >= DTCM_ADDR && addr < DTCM_ADDR + DTCM_SIZE) {
        const uint8_t* t = H.dtcm ? H.dtcm(H.ctx) : 0;
        if (!t) return 0;
        if (addr + n > DTCM_ADDR + DTCM_SIZE) n = DTCM_ADDR + DTCM_SIZE - addr;
        memcpy(buf, t + (addr - DTCM_ADDR), n);
        return n;
    }
    const uint8_t* m = H.main_ram ? H.main_ram(H.ctx) : 0;
    if (!m || addr >= DS_RAM_SIZE) return 0;
    if (addr + n > DS_RAM_SIZE) n = DS_RAM_SIZE - addr;
    memcpy(buf, m + addr, n);
    return n;
}

/* ---------- images: badges and game icons, cached as files ---------- */
static int fetch_file(const char* url, const char* path) { /* 1 if path exists afterwards */
    if (access(path, R_OK) == 0) return 1;
    if (!*url || !curl_load()) return 0;
    membuf body = {0, 0};
    long status = 0;
    void* h = c_init();
    if (!h) return 0;
    c_setopt(h, CURLOPT_URL, url);
    c_setopt(h, CURLOPT_USERAGENT, user_agent);
    c_setopt(h, CURLOPT_NOSIGNAL, 1L);
    c_setopt(h, CURLOPT_TIMEOUT, 10L);
    c_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    c_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
    c_setopt(h, CURLOPT_WRITEDATA, &body);
    if (c_perform(h) == 0) c_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    c_cleanup(h);
    int ok = 0;
    if (status == 200 && body.len > 8 && !memcmp(body.data, "\x89PNG", 4)) {
        char tmp[620];
        snprintf(tmp, sizeof tmp, "%s.part", path);
        FILE* f = fopen(tmp, "wb");
        if (f) {
            ok = fwrite(body.data, 1, body.len, f) == body.len;
            if (fclose(f)) ok = 0;
        }
        if (ok) ok = rename(tmp, path) == 0;
        else unlink(tmp);
    }
    free(body.data);
    return ok;
}
static void badge_file(const rc_client_achievement_t* a, char* path, size_t n) {
    snprintf(path, n, "%s/%s.png", BADGE_DIR, a->badge_name);
}
static int badge_fetch(const rc_client_achievement_t* a, char* path, size_t n) {
    char url[512] = "";
    badge_file(a, path, n);
    rc_client_achievement_get_image_url(a, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, url, sizeof url);
    return fetch_file(url, path);
}
/* after the game loads: its icon for the "loaded" pop-up, then every badge, so an unlock shows its badge at once */
typedef struct { char title[128], line[160], icon_url[512]; } load_msg;
static void* prefetch_thread(void* a) {
    load_msg* m = a;
    mkdir(BADGE_DIR, 0755);
    char icon[600];
    const rc_client_game_t* g = rc_client_get_game_info(rc);
    snprintf(icon, sizeof icon, "%s/game-%s.png", BADGE_DIR, g && g->badge_name ? g->badge_name : "0");
    int have_icon = fetch_file(m->icon_url, icon);
    popup_push(m->title, m->line, have_icon ? icon : 0, 0x6ab0ff, 5000);
    free(m);
    rc_client_achievement_list_t* l = rc_client_create_achievement_list(rc, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                                                                        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    int got = 0, n = 0;
    for (uint32_t b = 0; l && b < l->num_buckets; b++)
        for (uint32_t i = 0; i < l->buckets[b].num_achievements; i++) {
            char path[600];
            n++;
            got += badge_fetch(l->buckets[b].achievements[i], path, sizeof path);
        }
    if (l) rc_client_destroy_achievement_list(l);
    ralog("[ra] badges: %d of %d cached in %s\n", got, n, BADGE_DIR);
    return 0;
}
typedef struct { char l1[128], l2[160], badge[600]; const rc_client_achievement_t* a; } unlock_msg;
static void* unlock_thread(void* a) { /* the badge is normally cached already; fetch it if not */
    unlock_msg* m = a;
    int have = badge_fetch(m->a, m->badge, sizeof m->badge);
    popup_push(m->l1, m->l2, have ? m->badge : 0, 0xffd84a, 5000);
    free(m);
    return 0;
}

/* the game's progress for the frontend's library ("<unlocked> <total>" in $BANANA_RA_PROGRESS, BananaLinux's
 * docs/EMULATORS.md): written when the game loads and after each unlock */
static void write_progress(void) {
    const char* p = getenv("BANANA_RA_PROGRESS");
    if (!p || !*p || !rc) return;
    rc_client_user_game_summary_t s;
    rc_client_get_user_game_summary(rc, &s);
    if (!s.num_core_achievements) return;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%u %u\n", s.num_unlocked_achievements, s.num_core_achievements);
    fclose(f);
    rename(tmp, p);
}

/* ---------- events, login, game load ---------- */
static void on_event(const rc_client_event_t* e, rc_client_t* c) {
    (void)c;
    switch (e->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
        if (WARNING_ACH(e->achievement)) {
            ralog("[ra] warning from the server: %s\n", e->achievement->title);
            break;
        }
        ralog("[ra] unlocked: %s (%u)\n", e->achievement->title, e->achievement->points);
        write_progress();
        unlock_msg* m = calloc(1, sizeof *m);
        if (!m) break;
        snprintf(m->l1, sizeof m->l1, "Achievement unlocked \xc2\xb7 %u points", e->achievement->points);
        snprintf(m->l2, sizeof m->l2, "%s", e->achievement->title);
        m->a = e->achievement;
        detach(unlock_thread, m, "bds-ra");
        break;
    }
    case RC_CLIENT_EVENT_GAME_COMPLETED:
        popup_push("All achievements unlocked!", rc_client_get_game_info(rc)->title, 0, 0xffd84a, 6000);
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
        popup_push("Leaderboard submitted", e->leaderboard->title, 0, 0x6ab0ff, 3000);
        break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
        ralog("[ra] server error: %s\n", e->server_error->error_message);
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        popup_push("RetroAchievements", "Offline: unlocks will be sent later", 0, 0xff7a4a, 3000);
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        popup_push("RetroAchievements", "Back online, unlocks sent", 0, 0x6ad07a, 3000);
        break;
    }
}

static void on_load(int result, const char* err, rc_client_t* c, void* u) {
    (void)u;
    if (result != RC_OK) {
        ralog("[ra] game load failed: %s\n", err ? err : "?");
        if (result == RC_NO_GAME_LOADED) popup_push("RetroAchievements", "This game isn't known to RetroAchievements", 0, 0xff7a4a, 4000);
        else popup_push("RetroAchievements", err ? err : "Couldn't load the game", 0, 0xff7a4a, 4000);
        return;
    }
    const rc_client_game_t* g = rc_client_get_game_info(c);
    rc_client_user_game_summary_t s;
    rc_client_get_user_game_summary(c, &s);
    char l2[160];
    if (s.num_core_achievements)
        snprintf(l2, sizeof l2, "%u of %u achievements unlocked", s.num_unlocked_achievements, s.num_core_achievements);
    else snprintf(l2, sizeof l2, "No achievements for this game yet");
    ralog("[ra] game %u '%s': %s\n", g->id, g->title, l2);
    game_loaded = 1;
    write_progress();
    load_msg* m = calloc(1, sizeof *m);
    if (!m) {
        popup_push(g->title, l2, 0, 0x6ab0ff, 5000);
        return;
    }
    snprintf(m->title, sizeof m->title, "%s", g->title);
    snprintf(m->line, sizeof m->line, "%s", l2);
    rc_client_game_get_image_url(g, m->icon_url, sizeof m->icon_url);
    detach(prefetch_thread, m, "bds-ra");
}

static void save_token(void) {
    const rc_client_user_t* u = rc_client_get_user_info(rc);
    if (!u || !u->token) return;
    FILE* f = fopen(TOKEN_FILE, "w");
    if (!f) return;
    fprintf(f, "%s\n%s\n", u->username, u->token);
    fclose(f);
    chmod(TOKEN_FILE, 0600);
}

/* "Logged in": a pop-up as soon as the account is in, with its points (SuperDrastic's logs only; asked for in
 * BananaOS), before the game's own card */
static void logged_in_popup(void) {
    const rc_client_user_t* u = rc_client_get_user_info(rc);
    char l2[160];
    if (u && u->score) snprintf(l2, sizeof l2, "Logged in as %s \xc2\xb7 %u points", u->display_name ? u->display_name : user, u->score);
    else snprintf(l2, sizeof l2, "Logged in as %s", u && u->display_name ? u->display_name : user);
    popup_push("RetroAchievements", l2, 0, 0x6ad07a, 3000);
}

static void on_login_password(int result, const char* err, rc_client_t* c, void* u);
static void on_login_token(int result, const char* err, rc_client_t* c, void* u) {
    (void)c;
    (void)u;
    if (result == RC_OK) {
        ralog("[ra] logged in with the saved token as %s\n", user);
        logged_in_popup();
        logged_in = 1;
        return;
    }
    ralog("[ra] token login failed (%s)%s\n", err ? err : "?", *pass ? ", trying the password" : "");
    unlink(TOKEN_FILE);
    if (*pass) rc_client_begin_login_with_password(rc, user, pass, on_login_password, 0);
    else popup_push("RetroAchievements", "Login expired: enter your password in Settings", 0, 0xff7a4a, 5000);
}
static void on_login_password(int result, const char* err, rc_client_t* c, void* u) {
    (void)c;
    (void)u;
    if (result != RC_OK) {
        ralog("[ra] login failed: %s\n", err ? err : "?");
        popup_push("RetroAchievements login failed", err ? err : "Check the account in Settings", 0, 0xff7a4a, 5000);
        return;
    }
    ralog("[ra] logged in as %s\n", user);
    save_token();
    logged_in_popup();
    logged_in = 1;
}

static void on_rc_log(const char* msg, const rc_client_t* c) {
    (void)c;
    ralog("[rc] %s\n", msg);
}

void ra_init(const ra_host* host, const char* rom) {
    H = *host;
    const char* data = getenv("BANANADS_DATA");
    if (!data || !*data) data = ".";
    snprintf(TOKEN_FILE, sizeof TOKEN_FILE, "%s/ra.token", data);
    snprintf(BADGE_DIR, sizeof BADGE_DIR, "%s/ra-badges", data);
    snprintf(rom_path, sizeof rom_path, "%s", rom ? rom : "");
    const char* eu = getenv("BANANADS_RA_USER");
    const char* ep = getenv("BANANADS_RA_PASSWORD");
    snprintf(user, sizeof user, "%s", eu ? eu : "");
    snprintf(pass, sizeof pass, "%s", ep ? ep : "");
    if (!*user) {
        ralog("[ra] RetroAchievements off: no account (BANANADS_RA_USER)\n");
        return;
    }
    rc = rc_client_create(read_memory, server_call);
    rc_client_enable_logging(rc, RC_CLIENT_LOG_LEVEL_WARN, on_rc_log);
    rc_client_set_event_handler(rc, on_event);
    rc_client_set_hardcore_enabled(rc, 0);
    char clause[128] = "";
    rc_client_get_user_agent_clause(rc, clause, sizeof clause);
    snprintf(user_agent, sizeof user_agent, "BananaDS/%s (BananaOS) %s", BANANADS_VERSION, clause);

    char tu[128] = "", tok[256] = "";
    FILE* tf = fopen(TOKEN_FILE, "r");
    if (tf) {
        if (fgets(tu, sizeof tu, tf) && fgets(tok, sizeof tok, tf)) {
            tu[strcspn(tu, "\r\n")] = 0;
            tok[strcspn(tok, "\r\n")] = 0;
        }
        fclose(tf);
    }
    if (*tok && !strcasecmp(tu, user)) rc_client_begin_login_with_token(rc, user, tok, on_login_token, 0);
    else if (*pass) rc_client_begin_login_with_password(rc, user, pass, on_login_password, 0);
    else popup_push("RetroAchievements", "No password: enter it in Settings", 0, 0xff7a4a, 4000);
}

void ra_reset(void) {
    if (!rc || !game_loaded) return;
    rc_client_reset(rc);  /* the achievements' states start over with the game (the menu's Restart game) */
    ralog("[ra] the game restarted: achievement states reset\n");
}

void ra_frame(void) {
    frames++;
    if (!rc) return;
    /* the game is loaded (identified, its achievements validated against memory) once logged in: melonDS's RAM is
     * there from the start, unlike DraStic's, which had to be found first */
    if (logged_in && !load_started) {
        load_started = 1;
        rc_client_begin_identify_and_load_game(rc, RC_CONSOLE_NINTENDO_DS, rom_path, 0, 0, on_load, 0);
    }
    if (game_loaded) rc_client_do_frame(rc);
    else rc_client_idle(rc);
}

int ra_progress(unsigned* unlocked, unsigned* total) {
    if (!rc || !game_loaded) return 0;
    rc_client_user_game_summary_t s;
    rc_client_get_user_game_summary(rc, &s);
    *unlocked = s.num_unlocked_achievements;
    *total = s.num_core_achievements;
    return 1;
}

int ra_recent(ra_recent_t* out, int max) {
    if (!rc || !game_loaded || max <= 0) return 0;
    rc_client_achievement_list_t* l = rc_client_create_achievement_list(rc, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                                                                        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    int n = 0;
    for (uint32_t b = 0; l && b < l->num_buckets; b++)
        for (uint32_t i = 0; i < l->buckets[b].num_achievements; i++) {
            const rc_client_achievement_t* a = l->buckets[b].achievements[i];
            if (!a->unlocked || WARNING_ACH(a)) continue;
            int k = n < max ? n++ : max; /* insertion by unlock time, newest first */
            while (k > 0 && (long long)a->unlock_time > out[k - 1].when) {
                if (k < max) out[k] = out[k - 1];
                k--;
            }
            if (k >= max) continue;
            ra_recent_t* r = &out[k];
            snprintf(r->title, sizeof r->title, "%s", a->title ? a->title : "");
            snprintf(r->desc, sizeof r->desc, "%s", a->description ? a->description : "");
            badge_file(a, r->badge, sizeof r->badge);
            r->points = a->points;
            r->when = (long long)a->unlock_time;
        }
    if (l) rc_client_destroy_achievement_list(l);
    return n;
}
