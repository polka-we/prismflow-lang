/* JNI-обвязка Prism для Android. Класс: prism.Prism (android/src/main/java/prism/Prism.java). */
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "prism.h"

#define OUT_CAP (1 << 20) /* 1 МБ вывода */

typedef struct InCtx { const char *text; size_t pos; } InCtx;
static int in_cb(char *buf, size_t cap, void *u) {
    InCtx *c = (InCtx *)u;
    if (!c->text || !c->text[c->pos]) return -1;
    size_t n = 0;
    while (c->text[c->pos] && c->text[c->pos] != '\n' && n + 1 < cap) buf[n++] = c->text[c->pos++];
    if (c->text[c->pos] == '\n') c->pos++;
    buf[n] = 0;
    return (int)n;
}
typedef struct OutCtx { char *b; size_t n, cap; } OutCtx;
static void out_cb(const char *t, size_t len, void *u) {
    OutCtx *o = (OutCtx *)u;
    if (o->n + len >= o->cap) len = o->cap - 1 - o->n;
    memcpy(o->b + o->n, t, len); o->n += len; o->b[o->n] = 0;
}

/* Возвращает массив из двух строк: [вывод, ошибка]. ошибка == "" при успехе. */
JNIEXPORT jobjectArray JNICALL Java_prism_Prism_nativeRun(JNIEnv *env, jclass cls, jstring jsrc, jstring jinput) {
    (void)cls;
    const char *src = (*env)->GetStringUTFChars(env, jsrc, NULL);
    const char *inp = jinput ? (*env)->GetStringUTFChars(env, jinput, NULL) : NULL;
    OutCtx oc = { (char *)calloc(1, OUT_CAP), 0, OUT_CAP };
    InCtx ic = { inp, 0 };
    char err[512]; err[0] = 0;
    PrismVM *vm = prism_new();
    if (!vm) snprintf(err, sizeof err, "не удалось создать ВМ");
    else {
        prism_set_output(vm, out_cb, &oc);
        prism_set_input(vm, in_cb, &ic);
        PrismResult r = prism_run(vm, src);
        if (!r.ok) snprintf(err, sizeof err, "%s", r.message);
        prism_free(vm);
    }
    jclass sc = (*env)->FindClass(env, "java/lang/String");
    jobjectArray arr = (*env)->NewObjectArray(env, 2, sc, NULL);
    (*env)->SetObjectArrayElement(env, arr, 0, (*env)->NewStringUTF(env, oc.b));
    (*env)->SetObjectArrayElement(env, arr, 1, (*env)->NewStringUTF(env, err));
    free(oc.b);
    (*env)->ReleaseStringUTFChars(env, jsrc, src);
    if (jinput) (*env)->ReleaseStringUTFChars(env, jinput, inp);
    return arr;
}

JNIEXPORT jstring JNICALL Java_prism_Prism_nativeVersion(JNIEnv *env, jclass cls) {
    (void)cls; return (*env)->NewStringUTF(env, prism_version());
}
