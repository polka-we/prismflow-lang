#define _POSIX_C_SOURCE 200809L
/* Консольная программа Prismflow: prismflow файл.pr | prismflow -e "код" | prismflow < файл.pr */
#include "prism.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __unix__
#include <unistd.h>
#include <pthread.h>
#endif

static void out_cb(const char *t, size_t n, void *u) { (void)u; fwrite(t, 1, n, stdout); }
static int in_cb(char *buf, size_t cap, void *u) {
    (void)u;
    if (!fgets(buf, (int)cap, stdin)) return -1;
    return (int)strlen(buf);
}

static char *read_all(FILE *f) {
    size_t cap = 4096, n = 0; char *b = (char *)malloc(cap);
    if (!b) return NULL;
    for (;;) {
        size_t k = fread(b + n, 1, cap - n - 1, f);
        n += k;
        if (n + 1 < cap) break;
        cap *= 2; char *nb = (char *)realloc(b, cap);
        if (!nb) { free(b); return NULL; }
        b = nb;
    }
    b[n] = 0; return b;
}

static const char *g_files = NULL; static int g_http = 0;
static void shq(char *dst, size_t cap, const char *s) {   /* строка в одинарных кавычках для sh */
    size_t n = 0; dst[n++] = '\'';
    for (; *s && n + 5 < cap; s++) { if (*s == '\'') { memcpy(dst + n, "'\\''", 4); n += 4; } else dst[n++] = *s; }
    dst[n++] = '\''; dst[n] = 0;
}
/* HTTP через curl (Termux/Linux/macOS): prismflow --http */
static int curl_http(const char *method, const char *url, const char *headers, const char *body, size_t blen,
                     int timeout_ms, PrismHttpResponse *rs, void *u) {
    (void)u;
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) { rs->error = strdup("url must start with http:// or https://"); return 1; }
    char tmp[64] = ""; size_t cap = 4096 + strlen(url) * 4 + strlen(headers) * 4; char *cmd = (char *)malloc(cap);
    if (!cmd) return 1;
    char q[8192]; int n = snprintf(cmd, cap, "curl -sS -i -L --max-redirs 5 --proto '=http,https' --max-time %d -X ", (timeout_ms + 999) / 1000);
    shq(q, sizeof q, method); n += snprintf(cmd + n, cap - (size_t)n, "%s", q);
    for (const char *p = headers; *p;) {
        const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
        if (e > p) { char h[2048]; size_t l = (size_t)(e - p); if (l > 2000) l = 2000; memcpy(h, p, l); h[l] = 0; shq(q, sizeof q, h); n += snprintf(cmd + n, cap - (size_t)n, " -H %s", q); }
        p = *e ? e + 1 : e;
    }
    if (blen) {
        snprintf(tmp, sizeof tmp, "/tmp/prismflow_%d.body", (int)getpid());
        FILE *f = fopen(tmp, "wb"); if (!f) { free(cmd); rs->error = strdup("cannot create temp file"); return 1; }
        fwrite(body, 1, blen, f); fclose(f);
        shq(q, sizeof q, tmp); n += snprintf(cmd + n, cap - (size_t)n, " --data-binary @%s", q);
    }
    shq(q, sizeof q, url); n += snprintf(cmd + n, cap - (size_t)n, " %s 2>&1", q);
    FILE *p = popen(cmd, "r"); free(cmd);
    if (!p) { if (tmp[0]) remove(tmp); rs->error = strdup("cannot run curl"); return 1; }
    char *out = read_all(p); int rc = pclose(p); if (tmp[0]) remove(tmp);
    if (!out) return 1;
    size_t L = strlen(out);
    if (strncmp(out, "HTTP/", 5)) { while (L && (out[L - 1] == '\n' || out[L - 1] == '\r')) out[--L] = 0; rs->error = strdup(out[0] ? out : (rc ? "curl failed (is curl installed?)" : "empty response")); free(out); return 1; }
    /* последний блок заголовков (после редиректов) */
    char *hb = out;
    for (;;) { char *sep = strstr(hb, "\r\n\r\n"); if (!sep) break; char *nx = sep + 4; if (!strncmp(nx, "HTTP/", 5)) hb = nx; else { *sep = 0; rs->body = strdup(nx); rs->blen = L - (size_t)(nx - out); break; } }
    if (!rs->body) { rs->body = strdup(""); rs->blen = 0; }
    rs->status = atoi(hb + 9);
    char *nl = strchr(hb, '\n'); rs->headers = strdup(nl ? nl + 1 : "");
    /* curl с -sS при ошибке вывода нет, тело содержит только ответ */
    free(out); return 0;
}

typedef struct Job { const char *src; const char *dir; int rc, depth; } Job;
static void *run_job(void *arg) {
    Job *j = (Job *)arg;
    PrismVM *vm = prism_new();
    if (!vm) { fprintf(stderr, "cannot create VM\n"); j->rc = 2; return NULL; }
    if (j->depth > 0) prism_set_max_depth(vm, j->depth);
    if (getenv("PRISM_DEPTH")) prism_set_max_depth(vm, atoi(getenv("PRISM_DEPTH")));
    prism_set_output(vm, out_cb, NULL);
    prism_set_input(vm, in_cb, NULL);
    if (j->dir) prism_set_module_dir(vm, j->dir);
    if (g_files) prism_set_file_root(vm, g_files);
    if (g_http) prism_set_http_handler(vm, curl_http, NULL);
    PrismResult r = prism_run(vm, j->src);
    fflush(stdout);
    if (!r.ok) { fprintf(stderr, "%s\n", r.message); j->rc = 1; }
    prism_free(vm);
    return NULL;
}

int main(int argc, char **argv) {
    const char *src = NULL; char *owned = NULL;
    for (int i = 1; i < argc;) {   /* флаги доступа: --files DIR, --http */
        if (!strcmp(argv[i], "--files") && i + 1 < argc) { g_files = argv[i + 1]; memmove(argv + i, argv + i + 2, sizeof(char *) * (size_t)(argc - i - 1)); argc -= 2; }
        else if (!strcmp(argv[i], "--http")) { g_http = 1; memmove(argv + i, argv + i + 1, sizeof(char *) * (size_t)(argc - i)); argc -= 1; }
        else if (!strcmp(argv[i], "-e")) break;
        else i++;
    }
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) { printf("Prismflow %s\n", prism_version()); return 0; }
    if (argc >= 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        printf("Prismflow %s\nUsage:\n  prismflow file.pr      run a file\n  prismflow -e \"code\"    run a string\n  prismflow              read the program from stdin (use a file for request.line())\n  flags (before the file): --files DIR  allow file.* inside DIR;  --http  allow http.* (needs curl)\n", prism_version());
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "-e")) src = argv[2];
    else if (argc >= 2) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
        owned = read_all(f); fclose(f); src = owned;
    } else { owned = read_all(stdin); src = owned; }
    if (!src) { fprintf(stderr, "out of memory\n"); return 2; }
    char *dir = NULL;
    if (argc >= 2 && strcmp(argv[1], "-e")) { const char *sl = strrchr(argv[1], '/'); if (sl) { size_t l = (size_t)(sl - argv[1]); dir = (char *)malloc(l + 2); memcpy(dir, argv[1], l ? l : 1); dir[l ? l : 1] = 0; if (!l) { dir[0] = '/'; dir[1] = 0; } } }
    Job job = { src, dir, 0, 0 };
#ifdef __unix__
    /* Интерпретатор рекурсивен: запускаем его в потоке с большим стеком (64 МБ). */
    pthread_attr_t at; pthread_t th;
    size_t mb[] = { 1024, 256, 64, 0 }; int started = 0;
    for (int i = 0; mb[i] && !started; i++) {
        pthread_attr_init(&at); pthread_attr_setstacksize(&at, mb[i] << 20);
        job.depth = (int)(mb[i] * 100);   /* ~10 КБ стека на уровень вызова с запасом */
        if (pthread_create(&th, &at, run_job, &job) == 0) { pthread_join(th, NULL); started = 1; }
        pthread_attr_destroy(&at);
    }
    if (!started) { job.depth = 0; run_job(&job); }
#else
    run_job(&job);
#endif
    free(owned);
    return job.rc;
}
