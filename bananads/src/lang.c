/* lang.c: see lang.h. A port of banana-ui's ui/src/lang.cpp: exact English first, then the lines with placeholders
 * as patterns (a placeholder takes one character at least, the shortest that lets the rest match; the patterns with
 * the most fixed text first), named placeholders filled by name, printf's in order. */
#include "lang.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *text; int len, slot, named; } piece;   /* text: into the pattern's own string */
typedef struct { char *en, *tr; piece *pe, *pt; int npe, npt, fixed; } entry;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static entry *E; static int nE;                     /* every line of the part, in the file's order */
static int *pat; static int npat;                   /* the ones with placeholders, the most fixed text first */
static char code[16] = "en", name[64] = "English", by[64] = "", missing_path[128];
static int active;

/* the results handed out (translations filled in, or the text itself), kept so the pointers stay valid */
#define MEMO 4096
static struct { char *in, *out; int keep; } memo[MEMO];
static int nmemo;
static int nmissing;

static int pieces(const char *s, piece **out) {
    int n = 0, cap = 8; piece *p = malloc(cap * sizeof *p);
    const char *lit = 0;
    for (const char *c = s; *c;) {
        int slot = 0, named = 0; const char *e = 0;
        if (*c == '{') {
            const char *k = c + 1;
            while (*k && ((*k >= 'a' && *k <= 'z') || *k == '_')) k++;
            if (*k == '}' && k > c + 1) { slot = named = 1; e = k + 1; }
        } else if (*c == '%' && c[1]) {
            if (c[1] == '%') {   /* a literal % (kept as "%%" in the piece: compared as one %) */
                if (!lit) lit = c;
                c += 2; continue;
            }
            const char *k = c + 1;
            while (*k && strchr("-+ #0", *k)) k++;
            while (isdigit((unsigned char)*k)) k++;
            if (*k == '.') { k++; while (isdigit((unsigned char)*k)) k++; }
            while (*k && strchr("lhzj", *k)) k++;
            if (*k && strchr("dsuxXfgceip", *k)) { slot = 1; e = k + 1; }
        }
        if (slot) {
            if (n + 2 > cap) p = realloc(p, (cap *= 2) * sizeof *p);
            if (lit) { p[n++] = (piece){ lit, (int)(c - lit), 0, 0 }; lit = 0; }
            p[n++] = (piece){ c, (int)(e - c), 1, named };
            c = e; continue;
        }
        if (!lit) lit = c;
        c++;
    }
    if (lit) { if (n + 1 > cap) p = realloc(p, (cap *= 2) * sizeof *p); p[n++] = (piece){ lit, (int)strlen(lit), 0, 0 }; }
    *out = p; return n;
}

/* a literal piece against the text: "%%" in the piece stands for one '%'; the text's characters used, or -1 */
static int lit_match(const char *t, const piece *p) {
    int i = 0, j = 0;
    while (j < p->len) {
        char want = p->text[j];
        if (want == '%' && j + 1 < p->len && p->text[j + 1] == '%') j++;
        if (t[i] != want) return -1;
        i++; j++;
    }
    return i;
}
static void lit_append(char **o, size_t *n, size_t *cap, const char *s, int len, int is_lit) {
    for (int j = 0; j < len; j++) {
        if (is_lit && s[j] == '%' && j + 1 < len && s[j + 1] == '%') j++;
        if (*n + 2 > *cap) *o = realloc(*o, *cap *= 2);
        (*o)[(*n)++] = s[j];
    }
    (*o)[*n] = 0;
}

/* does t match the English's pieces from k on? caps: where each placeholder's text starts and ends */
static int match(const char *t, const piece *p, int np, int k, const char **cs, const char **ce, int nc) {
    if (k == np) return !*t;
    if (!p[k].slot) { int m = lit_match(t, &p[k]); return m >= 0 && match(t + m, p, np, k + 1, cs, ce, nc); }
    int len = (int)strlen(t);
    for (int end = 1; end <= len; end++) {   /* one character at least, the shortest that lets the rest match */
        cs[nc] = t; ce[nc] = t + end;
        if (match(t + end, p, np, k + 1, cs, ce, nc + 1)) return 1;
    }
    return 0;
}

static char *fill(const entry *x, const char **cs, const char **ce) {
    size_t n = 0, cap = 64; char *o = malloc(cap); o[0] = 0;
    int next = 0;   /* printf's placeholders in the order they come (the translator may move them, not swap two) */
    int pf[32], npf = 0;
    for (int i = 0, c = 0; i < x->npe; i++) if (x->pe[i].slot) { if (!x->pe[i].named && npf < 32) pf[npf++] = c; c++; }
    for (int i = 0; i < x->npt; i++) {
        const piece *p = &x->pt[i];
        if (!p->slot) { lit_append(&o, &n, &cap, p->text, p->len, 1); continue; }
        int c = -1;
        if (p->named) {
            for (int j = 0, cc = 0; j < x->npe; j++) if (x->pe[j].slot) {
                if (x->pe[j].named && x->pe[j].len == p->len && !strncmp(x->pe[j].text, p->text, p->len)) { c = cc; break; }
                cc++;
            }
        } else if (next < npf) c = pf[next++];
        if (c >= 0) lit_append(&o, &n, &cap, cs[c], (int)(ce[c] - cs[c]), 0);
        else lit_append(&o, &n, &cap, p->text, p->len, 0);
    }
    return o;
}

static int fixed_len(const entry *x) { int f = 0; for (int i = 0; i < x->npe; i++) if (!x->pe[i].slot) f += x->pe[i].len; return f; }
static int by_fixed(const void *a, const void *b) {
    int fa = E[*(const int *)a].fixed, fb = E[*(const int *)b].fixed;
    return fa != fb ? fb - fa : *(const int *)a - *(const int *)b;   /* (stable: the file's order among equals) */
}

static void setting(char *out, size_t n) {
    const char *e = getenv("BANANA_LANGUAGE");
    if (e && *e) { snprintf(out, n, "%s", e); return; }
    snprintf(out, n, "en");
    FILE *f = fopen("/data/banana/settings.conf", "r"); if (!f) return;
    char l[256];
    while (fgets(l, sizeof l, f)) {
        char *p = l; while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "language", 8)) continue;
        p += 8; while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') continue;
        p++; while (*p == ' ' || *p == '\t') p++;
        char *e2 = p + strcspn(p, " \t\r\n"); *e2 = 0;
        if (*p) snprintf(out, n, "%s", p);
    }
    fclose(f);
}

int lang_init(const char *part) {
    char c[16]; setting(c, sizeof c);
    pthread_mutex_lock(&mu);
    active = 0;
    if (!strcmp(c, "en")) { pthread_mutex_unlock(&mu); return 0; }
    const char *dir = getenv("BANANA_LANG_DIR"); if (!dir || !*dir) dir = "/usr/share/banana/lang";
    char path[256]; snprintf(path, sizeof path, "%s/%s.tsv", dir, c);
    FILE *f = fopen(path, "r");
    if (!f) { pthread_mutex_unlock(&mu); return 0; }
    snprintf(code, sizeof code, "%s", c); snprintf(name, sizeof name, "%s", c);
    snprintf(missing_path, sizeof missing_path, "/run/banana/lang-missing-%s.txt", part);
    char *l = 0; size_t cap = 0; ssize_t len;
    while ((len = getline(&l, &cap, f)) >= 0) {
        while (len && (l[len - 1] == '\n' || l[len - 1] == '\r')) l[--len] = 0;
        if (!len) continue;
        if (l[0] == '#') {   /* "#: key = value" */
            char *eq = strstr(l, " = ");
            if (!strncmp(l, "#: ", 3) && eq) {
                *eq = 0;
                if (!strcmp(l + 3, "language")) snprintf(name, sizeof name, "%s", eq + 3);
                else if (!strcmp(l + 3, "translated-by")) snprintf(by, sizeof by, "%s", eq + 3);
            }
            continue;
        }
        char *a = strchr(l, '\t'), *b = a ? strchr(a + 1, '\t') : 0, *d = b ? strchr(b + 1, '\t') : 0;
        if (!d) continue;
        *a = *b = *d = 0;
        if (strcmp(l, part) || !d[1] || a[1]) continue;   /* this part's lines, with a translation, no context */
        int dup = 0; for (int i = 0; i < nE && !dup; i++) dup = !strcmp(E[i].en, b + 1);
        if (dup) continue;   /* (the first, as the file has them) */
        E = realloc(E, (nE + 1) * sizeof *E);
        entry *x = &E[nE];
        x->en = strdup(b + 1); x->tr = strdup(d + 1);
        x->npe = pieces(x->en, &x->pe); x->npt = pieces(x->tr, &x->pt);
        x->fixed = fixed_len(x);
        int slots = 0; for (int i = 0; i < x->npe; i++) slots |= x->pe[i].slot;
        if (slots) { pat = realloc(pat, (npat + 1) * sizeof *pat); pat[npat++] = nE; }
        nE++;
    }
    free(l); fclose(f);
    qsort(pat, npat, sizeof *pat, by_fixed);
    active = nE > 0;
    pthread_mutex_unlock(&mu);
    return active;
}

int lang_active(void) { return active; }
const char *lang_name(void) { return active ? name : "English"; }
const char *lang_by(void) { return active ? by : ""; }

static int memo_find(const char *s) {
    for (int i = 0; i < nmemo; i++) if (!strcmp(memo[i].in, s)) return i;
    return -1;
}
static const char *memo_add(const char *in, char *out, int keep) {
    if (nmemo == MEMO) {   /* full: the oldest half goes (their pointers were for frames long drawn) */
        for (int i = 0; i < MEMO / 2; i++) { if (memo[i].out != memo[i].in) free(memo[i].out); free(memo[i].in); }
        memmove(memo, memo + MEMO / 2, (MEMO / 2) * sizeof memo[0]); nmemo = MEMO / 2;
    }
    memo[nmemo].in = strdup(in); memo[nmemo].out = out ? out : memo[nmemo].in; memo[nmemo].keep = keep;
    return memo[nmemo++].out;
}

static void note_missing(const char *s) {
    /* only text with letters in it (not a time, a number), once each, and not a piece of a translated line or one
     * cut short already (CJK, an ellipsis: lead bytes from 0xe2 on) */
    int letters = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p >= 0xe2 && *p < 0xf8) return;
        if ((*p | 0x20) >= 'a' && (*p | 0x20) <= 'z') letters = 1;
    }
    if (!letters || !missing_path[0] || nmissing >= 3000) return;
    FILE *f = fopen(missing_path, "a"); if (!f) return;
    fprintf(f, "%s\n", s); fclose(f); nmissing++;
}

const char *lang_tr(const char *s) {
    if (!active || !s || !*s) return s;
    pthread_mutex_lock(&mu);
    int m = memo_find(s);
    if (m >= 0) { const char *o = memo[m].out; pthread_mutex_unlock(&mu); return o; }
    char *out = 0;
    for (int i = 0; i < nE && !out; i++) if (!strcmp(E[i].en, s)) out = strdup(E[i].tr);
    for (int k = 0; k < npat && !out; k++) {
        const entry *x = &E[pat[k]];
        const char *cs[32], *ce[32];
        int slots = 0; for (int i = 0; i < x->npe; i++) slots += x->pe[i].slot;
        if (slots <= 32 && match(s, x->pe, x->npe, 0, cs, ce, 0)) out = fill(x, cs, ce);
    }
    if (!out) note_missing(s);
    const char *o = memo_add(s, out, 0);
    pthread_mutex_unlock(&mu);
    return o;
}

void lang_keep(const char *s) {
    if (!active || !s || !*s) return;
    pthread_mutex_lock(&mu);
    if (memo_find(s) < 0) memo_add(s, 0, 1);
    pthread_mutex_unlock(&mu);
}

int lang_cjk(int cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}
static int closing(int cp) {
    static const int c[] = { 0xFF0C, 0x3002, 0x3001, 0xFF1B, 0xFF1A, 0xFF01, 0xFF1F, 0xFF09, 0x300D, 0x300F, 0x300B,
        0x3009, 0x3011, 0x3015, 0x2026, ',', '.', ';', ':', '!', '?', ')', ']', '%' };
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) if (c[i] == cp) return 1;
    return 0;
}
static int opening(int cp) {
    static const int c[] = { 0xFF08, 0x300C, 0x300E, 0x300A, 0x3008, 0x3010, 0x3014, '(', '[' };
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) if (c[i] == cp) return 1;
    return 0;
}
int lang_break_between(int prev, int cp) {
    return lang_cjk(prev) && lang_cjk(cp) && !closing(cp) && !opening(prev);
}
