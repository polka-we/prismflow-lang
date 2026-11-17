/*
 * Prism — встраиваемое ядро языка (версия 0.1, тестовая).
 * Один заголовок + один файл prism.c, без зависимостей кроме стандартной библиотеки C99.
 */
#ifndef PRISM_H
#define PRISM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRISM_VERSION "0.9.0"

typedef struct PrismVM PrismVM;

/* Вывод программы (print). Вызывается с кусками UTF-8 текста. */
typedef void (*PrismWriteFn)(const char *text, size_t len, void *user);
/* Ввод (request.line / request.lines). Записывает строку без перевода строки в buf,
 * возвращает её длину или -1 при конце ввода. */
typedef int (*PrismReadFn)(char *buf, size_t cap, void *user);

/* HTTP: хост выполняет запрос (TLS и сеть — на его стороне). Возвращает 0 при успехе (заполнить status, body, headers),
 * иначе ненулевое значение и error. Строки body/headers/error выделяются malloc, освобождает ядро.
 * headers — запрос: "name: value\n...", ответ — так же. */
typedef struct PrismHttpResponse { int status; char *body; size_t blen; char *headers; char *error; } PrismHttpResponse;
typedef int (*PrismHttpFn)(const char *method, const char *url, const char *headers, const char *body, size_t body_len,
                           int timeout_ms, PrismHttpResponse *resp, void *user);

typedef struct PrismResult {
    int ok;               /* 1 - программа выполнена, 0 - ошибка */
    int line;             /* строка ошибки (0 - неизвестна) */
    char message[512];    /* текст ошибки по-русски, пустой при успехе */
} PrismResult;

PrismVM *prism_new(void);
void prism_free(PrismVM *vm);

/* Файлы: file.* работает только внутри папки dir (пути вне неё и абсолютные отклоняются). NULL — выключить (по умолчанию). */
void prism_set_file_root(PrismVM *vm, const char *dir);
/* Сеть: http.* вызывает fn. NULL — выключить (по умолчанию). */
void prism_set_http_handler(PrismVM *vm, PrismHttpFn fn, void *user);
void prism_set_output(PrismVM *vm, PrismWriteFn fn, void *user);
void prism_set_input(PrismVM *vm, PrismReadFn fn, void *user);
/* Лимит памяти арены в байтах (по умолчанию 256 МБ). */
void prism_set_memory_limit(PrismVM *vm, size_t bytes);

/* Выполнить исходный текст. Состояние (переменные верхнего уровня) сохраняется
 * между вызовами одной ВМ; память освобождается при prism_free. */
PrismResult prism_run(PrismVM *vm, const char *source);

/* Удобная обёртка для Android/JNI: выполняет программу в новой ВМ и собирает вывод в буфер.
 * Возвращает 0 при успехе, иначе 1; при ошибке текст ошибки записан в err. */
int prism_run_to_buffer(const char *source, char *out, size_t outcap, char *err, size_t errcap);

/* depth limit of nested calls (default 1200); the calling thread needs a large enough stack */
void prism_set_max_depth(PrismVM *vm, int depth);
/* Modules for from("name"): registered sources are searched first, then <dir>/name.prism|.pr, then the working directory. */
void prism_add_module(PrismVM *vm, const char *name, const char *source);
void prism_set_module_dir(PrismVM *vm, const char *dir);
/* Number of threads for ::par (default: number of CPUs, at most 8; 1 = sequential). */
void prism_set_threads(PrismVM *vm, int n);
const char *prism_version(void);

#ifdef __cplusplus
}
#endif
#endif
