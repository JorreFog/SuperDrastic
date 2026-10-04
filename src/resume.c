// resume.c: quit to the menu with a savestate, and pick up there next time.
//
// DraStic can't be asked to save from outside, and exit hotkeys kill it outright (ROCKNIX: killall -9). So the
// frontend sends SIGUSR1 instead (on ROCKNIX, through the kill command the exit hotkey runs), and this presses
// DraStic's own "save state" control (the joystick button drastic.cfg maps to it), waits for DraStic to finish the
// file, and exits the way the hotkey would have (SIGKILL). The savestate never lands in one of the player's slots:
// DraStic writes <savestates>/_savestate_temp.dss and renames it to <game>_<slot>.dss, and that rename goes to
// DSFLIP_RESUME_FILE instead. At the next start, with DSFLIP_RESUME_LOAD=1, the "load state" control is pressed once
// the game runs, and DraStic's lookup of <game>_<slot>.dss is pointed at the resume file, which is then deleted:
// one resume per quit, so an older state is never loaded twice (with backup_in_savestates it would also put back an
// older in-game save).
//
// DSFLIP_RESUME_FILE: where the resume state goes (unset: SIGUSR1 just quits). DSFLIP_RESUME_LOAD=1: load it at start.
// A second SIGUSR1 while saving, or no file within 5 s, quits at once. The previous resume file stays until DraStic's
// rename replaces it, so a save that doesn't finish (the 5 s timeout, a full disk) leaves the last good state in
// place. DSFLIP_RESUME_TRACE=1 logs DraStic's savestate file calls.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void dsflip_log(const char *fmt, ...);
void cpugov_boost(int ms);
void dsflip_toast(const char *l1, const char *l2, uint32_t accent, int ms);

static char resume_path[512];
static int trace;
static volatile int want_save, saving, saved, want_load, loading, load_redirects;
static volatile int btn_press, btn_release;          /* joystick button to press now / to release (-1: none) */
static long long release_at;                          /* the frame to release it at: DraStic ignores a tap, it wants it held */
static int btn_save = -1, btn_load = -1;
static int joy_id = -1;                               /* the SDL instance id of DraStic's joystick */
static long long load_at_frame, loaded_frame;
static long long frames;

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

/* a DraStic savestate slot file: ".../<game>_<digit>.dss" (not the temp file) */
static int is_slot(const char *p) {
    if (!p) return 0;
    size_t n = strlen(p);
    return n > 6 && !strcmp(p + n - 4, ".dss") && p[n - 5] >= '0' && p[n - 5] <= '9' && p[n - 6] == '_' && !strstr(p, "_savestate_temp");
}

/* ---- DraStic's file calls ---- */
static int move_file(const char *a, const char *b) {
    char tmp[600]; snprintf(tmp, sizeof tmp, "%s.new", b);
    FILE *in = fopen(a, "rb"), *out = in ? fopen(tmp, "wb") : 0; char buf[65536]; size_t n; int bad = !out;
    while (!bad && (n = fread(buf, 1, sizeof buf, in)) > 0) bad = fwrite(buf, 1, n, out) != n;
    if (in) fclose(in);
    if (out && fclose(out)) bad = 1;
    if (bad) { unlink(tmp); errno = EIO; return -1; }
    int (*ren)(const char *, const char *) = (int (*)(const char *, const char *))dlsym(RTLD_NEXT, "rename");
    int (*unl)(const char *) = (int (*)(const char *))dlsym(RTLD_NEXT, "unlink");
    if (ren(tmp, b)) return -1;
    unl(a);
    return 0;
}
#define NEXT(ret, name, ...) static ret (*real)(__VA_ARGS__); if (!real) real = (ret (*)(__VA_ARGS__))dlsym(RTLD_NEXT, name)
static const char *redirect_load(const char *p, const char *call) {
    if (trace && p && strstr(p, ".dss")) dsflip_log("[resume] %s %s\n", call, p);
    if (p && strstr(p, ".dss")) cpugov_boost(2000);   /* a savestate being written or read: full clock (cpugov.c) */
    if (!loading || !is_slot(p)) return p;
    if (!load_redirects++) dsflip_log("[resume] loading the resume state instead of %s\n", strrchr(p, '/') ? strrchr(p, '/') + 1 : p);
    return resume_path;
}
int rename(const char *a, const char *b) {
    NEXT(int, "rename", const char *, const char *);
    if (trace && b && strstr(b, ".dss")) dsflip_log("[resume] rename %s -> %s\n", a, b);
    if (saving && is_slot(b)) {
        int r = real(a, resume_path);
        if (r && errno == EXDEV) r = move_file(a, resume_path);   /* another filesystem: copy it over */
        dsflip_log("[resume] saved to %s%s\n", resume_path, r ? " (failed)" : "");
        saved = r ? -1 : 1;
        return r;
    }
    return real(a, b);
}
/* DraStic deletes the slot's old file before renaming the new one in: not while the save goes to the resume file */
int unlink(const char *p) {
    NEXT(int, "unlink", const char *);
    if (saving && is_slot(p)) { if (trace) dsflip_log("[resume] kept %s (DraStic deletes it before saving)\n", p); return 0; }
    return real(p);
}
int remove(const char *p) {
    NEXT(int, "remove", const char *);
    if (saving && is_slot(p)) { if (trace) dsflip_log("[resume] kept %s (DraStic deletes it before saving)\n", p); return 0; }
    return real(p);
}
FILE *fopen(const char *p, const char *m) { NEXT(FILE *, "fopen", const char *, const char *); return real(redirect_load(p, "fopen"), m); }
FILE *fopen64(const char *p, const char *m) { NEXT(FILE *, "fopen64", const char *, const char *); return real(redirect_load(p, "fopen64"), m); }
int open(const char *p, int f, ...) {
    NEXT(int, "open", const char *, int, ...);
    mode_t md = 0; if (f & O_CREAT) { va_list ap; va_start(ap, f); md = va_arg(ap, int); va_end(ap); }
    return real(redirect_load(p, "open"), f, md);
}
int open64(const char *p, int f, ...) {
    NEXT(int, "open64", const char *, int, ...);
    mode_t md = 0; if (f & O_CREAT) { va_list ap; va_start(ap, f); md = va_arg(ap, int); va_end(ap); }
    return real(redirect_load(p, "open64"), f, md);
}
int access(const char *p, int m) { NEXT(int, "access", const char *, int); return real(redirect_load(p, "access"), m); }
int __xstat(int v, const char *p, struct stat *s) { NEXT(int, "__xstat", int, const char *, struct stat *); return real(v, redirect_load(p, "stat"), s); }
int __xstat64(int v, const char *p, struct stat64 *s) { NEXT(int, "__xstat64", int, const char *, struct stat64 *); return real(v, redirect_load(p, "stat64"), s); }

/* ---- DraStic's controls ---- */
/* controls_<set>[CONTROL_INDEX_<name>] from config/drastic.cfg: 1024 + n is joystick button n */
static int control_button_in(const char *set, const char *name) {
    FILE *f = fopen("config/drastic.cfg", "r"); if (!f) return -1;
    char key[96], line[256]; int b = -1;
    snprintf(key, sizeof key, "controls_%s[CONTROL_INDEX_%s] = ", set, name);
    while (fgets(line, sizeof line, f)) if (!strncmp(line, key, strlen(key))) {
        int v = atoi(line + strlen(key)); if (v >= 1024 && v < 1024 + 64) b = v - 1024;
        break;
    }
    fclose(f);
    return b;
}
static int control_button(const char *name) { return control_button_in("a", name); }
/* either set (ROCKNIX maps START and SELECT in controls_b) */
static int control_button_any(const char *name) { int b = control_button_in("a", name); return b >= 0 ? b : control_button_in("b", name); }

void *SDL_JoystickOpen(int i) {
    static void *(*real)(int); if (!real) real = (void *(*)(int))dlsym(RTLD_NEXT, "SDL_JoystickOpen");
    void *j = real(i);
    static int (*inst)(void *); if (!inst) inst = (int (*)(void *))dlsym(RTLD_NEXT, "SDL_JoystickInstanceID");
    if (j && inst && joy_id < 0) joy_id = inst(j);
    return j;
}

/* SDL_PollEvent (dsflip.c) asks this first: a pressed or released control button, as SDL delivers them */
int resume_poll(void *ev) {
    int b = -1, down = 0;
    if (btn_release >= 0 && frames >= release_at) { b = btn_release; btn_release = -1; }
    else if (btn_press >= 0) { b = btn_press; btn_press = -1; btn_release = b; release_at = frames + 40; down = 1; }
    else return 0;
    uint8_t *e = ev; memset(e, 0, 56);
    uint32_t type = down ? 0x603 : 0x604;             /* SDL_JOYBUTTONDOWN / SDL_JOYBUTTONUP */
    int32_t which = joy_id >= 0 ? joy_id : 0;
    memcpy(e, &type, 4); memcpy(e + 8, &which, 4); e[12] = (uint8_t)b; e[13] = (uint8_t)down;
    return 1;
}

/* Exit combos: Start + Select, or Menu + Start, held for EXIT_HOLD_MS quit the game the way the exit hotkey does
 * (with a resume save when that is on). ROCKNIX's DraStic had them through gptokeyb, which this session doesn't
 * run. DSFLIP_EXIT_COMBO=0: off. */
#define EXIT_HOLD_MS 500
static int btn_start = -1, btn_select = -1, btn_menu = -1, exit_combo = 1;
static uint64_t held_btns;                             /* joystick buttons down, by number */
static long long combo_since;                          /* when the combo was complete (0: it isn't) */
static void on_usr1(int sig);

/* SDL_PollEvent (dsflip.c) shows this every event DraStic gets: a press of its save or load state button boosts the
 * CPU before DraStic starts on the state (it compresses it all first, then writes the file); the exit combos */
void resume_saw_event(const void *ev) {
    const uint8_t *e = ev; uint32_t type; memcpy(&type, e, 4);
    if (type != 0x603 && type != 0x604) return;          /* SDL_JOYBUTTONDOWN / UP */
    int b = e[12], down = type == 0x603;
    if (down && (b == btn_save || b == btn_load)) cpugov_boost(3000);
    if (b < 64) { if (down) held_btns |= 1ULL << b; else held_btns &= ~(1ULL << b); }
    #define HELD(x) ((x) >= 0 && (held_btns >> (x) & 1))
    int combo = exit_combo && HELD(btn_start) && (HELD(btn_select) || HELD(btn_menu));
    #undef HELD
    if (!combo) combo_since = 0;
    else if (!combo_since) combo_since = now_ms();
}

/* ---- quitting with a save ---- */
static void *save_thread(void *a) {
    (void)a;
    long long t0 = now_ms();
    while (!saved && now_ms() - t0 < 5000) usleep(20000);
    if (saved == 1) dsflip_log("[resume] saved in %lld ms: quitting\n", now_ms() - t0);
    else dsflip_log("[resume] %s: quitting without one%s\n",
                    saved ? "the resume state couldn't be written" : "no savestate within 5 s",
                    resume_path[0] && access(resume_path, R_OK) == 0 ? " (previous resume state kept)" : "");
    kill(getpid(), SIGKILL);
    return 0;
}
static void on_usr1(int sig) {
    (void)sig;
    if (saving || !resume_path[0] || btn_save < 0) kill(getpid(), SIGKILL);   /* again, or nothing to save with */
    want_save = 1;
}

/* every present (dsflip.c): starts a pending save or load on DraStic's own thread */
void resume_frame(void) {
    frames++;
    if (combo_since && now_ms() - combo_since >= EXIT_HOLD_MS) {
        combo_since = 0; held_btns = 0;
        dsflip_log("[resume] exit combo held: quitting\n");
        on_usr1(SIGUSR1);
    }
    if (want_save && !saving) {
        saving = 1; want_save = 0;
        dsflip_log("[resume] quit requested: saving a resume state\n");
        /* don't unlink the previous state here: the rename hook replaces it only once the new file is complete.
         * Deleting first threw the last good resume away whenever the save timed out or failed. */
        btn_press = btn_save;
        pthread_t t; if (pthread_create(&t, 0, save_thread, 0)) kill(getpid(), SIGKILL);
        pthread_setname_np(t, "dsf-resume");
    }
    if (want_load && frames >= load_at_frame && !loading) {
        loading = 1; want_load = 0; loaded_frame = frames;
        btn_press = btn_load;
    }
    if (loading && frames - loaded_frame > 150) {     /* 2.5 s later (0.7 s held): DraStic has read it (or never will) */
        loading = 0;
        if (load_redirects) {
            unlink(resume_path);
            dsflip_log("[resume] resumed\n");
            dsflip_toast("Resumed where you left off", "Quit with the exit hotkey to save your place again", 0x3aa0ff, 3500);
        } else dsflip_log("[resume] DraStic didn't load the state (control not handled?): kept %s\n", resume_path);
    }
}

void resume_start(void) {
    trace = getenv("DSFLIP_RESUME_TRACE") != 0;
    btn_press = btn_release = -1;
    btn_save = control_button("SAVE_STATE"); btn_load = control_button("LOAD_STATE");   /* (also for the CPU boost) */
    btn_start = control_button_any("START"); btn_select = control_button_any("SELECT"); btn_menu = control_button_any("MENU");
    { const char *x = getenv("DSFLIP_EXIT_COMBO"); exit_combo = !(x && *x == '0'); }
    if (exit_combo) dsflip_log("[resume] exit combos: start %d + select %d, menu %d + start\n", btn_start, btn_select, btn_menu);
    const char *p = getenv("DSFLIP_RESUME_FILE");
    if (!p || !*p) return;
    snprintf(resume_path, sizeof resume_path, "%s", p);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_usr1; sigaction(SIGUSR1, &sa, 0);
    const char *l = getenv("DSFLIP_RESUME_LOAD");
    int have = access(resume_path, R_OK) == 0;
    if (l && *l == '1' && have && btn_load >= 0) { want_load = 1; load_at_frame = 120; }   /* 2 s in: the game runs */
    dsflip_log("[resume] quit-with-save on SIGUSR1 (save = button %d, load = button %d)%s\n", btn_save, btn_load,
               want_load ? ", resuming this session" : have ? ", a resume state exists but isn't loaded" : "");
    if (btn_save < 0) dsflip_log("[resume] DraStic's save-state control isn't a joystick button: SIGUSR1 quits without saving\n");
}
