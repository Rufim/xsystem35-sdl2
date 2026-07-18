/* Мост xsystem35 <-> Android: ADV-текст для TTS, синтетический ввод,
 * доступ к переменным VM для читов (16-битные значения System 3.x).
 *
 * Общая часть платформонезависима; эмиттер текста имеет две реализации:
 * JNI (Android) и stdout-заглушка (прочие платформы, для отладки).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "android_bridge.h"
#include "variable.h"
#include "nact.h"

#define BRIDGE_PAGE_MAX 256   // совпадает с PAGE_MAX в variable.c

static bool tts_enabled = false;

void bridge_set_tts_enabled(bool on) { tts_enabled = on; }

// Синтетический Enter в очередь SDL — игра листает диалог тем же путём, что и
// реальный ввод (thread-safe).
void bridge_advance_message(void)
{
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = SDL_KEYDOWN;
	ev.key.state = SDL_PRESSED;
	ev.key.keysym.sym = SDLK_RETURN;
	ev.key.keysym.scancode = SDL_SCANCODE_RETURN;
	SDL_PushEvent(&ev);
	ev.type = SDL_KEYUP;
	ev.key.state = SDL_RELEASED;
	SDL_PushEvent(&ev);
}

static void bridge_emit(const char *utf8);
static void bridge_emit_page(void);

void bridge_adv_message(const char *utf8) { if (utf8 && *utf8) bridge_emit(utf8); }
void bridge_adv_newline(void) { /* текст уже отдан в bridge_adv_message */ }
void bridge_adv_page_break(void) { bridge_emit_page(); }
void bridge_adv_keywait(void) { /* точка ожидания клавиши; резерв для авто-листания */ }

// --- Читы: переменные VM (16-битные) ---

int bridge_cheat_read16(int page, int varno, int *ok)
{
	*ok = 0;
	if (page == 0) {
		if (varno < 0 || varno >= v_name_count())
			return 0;
		struct VarRef ref;
		vmvar_t *p = v_ref(varno, &ref);
		if (!p)
			return 0;
		*ok = 1;
		return (int)*p;
	}
	if (page < 1 || page >= BRIDGE_PAGE_MAX)
		return 0;
	struct VarPage *vp = &varPage[page];
	if (!vp->value || varno < 0 || varno >= vp->size)
		return 0;
	*ok = 1;
	return (int)vp->value[varno];
}

bool bridge_cheat_write(int page, int varno, int value)
{
	int ok;
	bridge_cheat_read16(page, varno, &ok);
	if (!ok)
		return false;
	if (value < 0) value = 0;
	if (value > 65535) value = 65535;
	if (page == 0) {
		struct VarRef ref;
		vmvar_t *p = v_ref(varno, &ref);
		if (!p) return false;
		*p = (vmvar_t)value;
	} else {
		varPage[page].value[varno] = (vmvar_t)value;
	}
	return true;
}

static char *var_name_utf8(int varno)
{
	const char *nm = v_name(varno);
	char *u = nm ? toUTF8(nm) : NULL;
	return u ? u : strdup("?");
}

int bridge_cheat_list(const char *filter_utf8, struct bridge_var **out, int max)
{
	*out = NULL;
	int total = v_name_count();
	struct bridge_var *arr = calloc(max > 0 ? max : 1, sizeof(*arr));
	int n = 0;
	for (int i = 0; i < total && n < max; i++) {
		int ok;
		int v = bridge_cheat_read16(0, i, &ok);
		if (!ok)
			continue;
		char *name = var_name_utf8(i);
		if (filter_utf8 && *filter_utf8 && !strstr(name, filter_utf8)) {
			free(name);
			continue;
		}
		arr[n].page = 0;
		arr[n].varno = i;
		arr[n].name_utf8 = name;
		arr[n].value = v;
		n++;
	}
	*out = arr;
	return n;
}

// Кандидаты скана (живут между вызовами new/narrow)
#define SCAN_MAX 100000
static struct { int page; int varno; } *scan_cands = NULL;
static int scan_n = 0;

int bridge_cheat_scan(int value, bool narrow, struct bridge_var **out, int max)
{
	*out = NULL;
	if (!scan_cands)
		scan_cands = calloc(SCAN_MAX, sizeof(*scan_cands));
	if (!narrow) {
		scan_n = 0;
		// глобальные скаляры (именованные)
		for (int i = 0; i < v_name_count() && scan_n < SCAN_MAX; i++) {
			int ok;
			if (bridge_cheat_read16(0, i, &ok) == value && ok) {
				scan_cands[scan_n].page = 0;
				scan_cands[scan_n].varno = i;
				scan_n++;
			}
		}
		// страницы массивов
		for (int pg = 1; pg < BRIDGE_PAGE_MAX && scan_n < SCAN_MAX; pg++) {
			struct VarPage *vp = &varPage[pg];
			if (!vp->value || vp->size <= 0)
				continue;
			for (int i = 0; i < vp->size && scan_n < SCAN_MAX; i++) {
				if ((int)vp->value[i] == value) {
					scan_cands[scan_n].page = pg;
					scan_cands[scan_n].varno = i;
					scan_n++;
				}
			}
		}
	} else {
		int kept = 0;
		for (int i = 0; i < scan_n; i++) {
			int ok;
			int v = bridge_cheat_read16(scan_cands[i].page, scan_cands[i].varno, &ok);
			if (ok && v == value)
				scan_cands[kept++] = scan_cands[i];
		}
		scan_n = kept;
	}

	int n = scan_n < max ? scan_n : max;
	struct bridge_var *arr = calloc(n > 0 ? n : 1, sizeof(*arr));
	for (int i = 0; i < n; i++) {
		int ok;
		arr[i].page = scan_cands[i].page;
		arr[i].varno = scan_cands[i].varno;
		arr[i].value = bridge_cheat_read16(arr[i].page, arr[i].varno, &ok);
		if (arr[i].page == 0) {
			arr[i].name_utf8 = var_name_utf8(arr[i].varno);
		} else {
			char buf[48];
			snprintf(buf, sizeof(buf), "[стр %d] #%d", arr[i].page, arr[i].varno);
			arr[i].name_utf8 = strdup(buf);
		}
	}
	*out = arr;
	return scan_n;   // полное число кандидатов (в *out — первые max)
}

void bridge_cheat_free(struct bridge_var *arr, int n)
{
	if (!arr)
		return;
	for (int i = 0; i < n; i++)
		free(arr[i].name_utf8);
	free(arr);
}

#ifndef __ANDROID__

static void bridge_emit(const char *utf8)
{
	if (getenv("XS35_BRIDGE_DEBUG")) {
		printf("[ADV] |%s|\n", utf8);
		fflush(stdout);
	}
}

static void bridge_emit_page(void)
{
	if (getenv("XS35_BRIDGE_DEBUG")) {
		printf("[ADV] --- page ---\n");
		fflush(stdout);
	}
}

#else /* __ANDROID__ */

#include <jni.h>
#include <android/log.h>
#define BLOG(...) __android_log_print(ANDROID_LOG_INFO, "xs35bridge", __VA_ARGS__)

static JavaVM *jvm = NULL;
static jclass bridge_class = NULL;      // GlobalRef на NativeBridge
static jmethodID mid_on_adv_text = NULL;
static jmethodID mid_on_adv_page = NULL;

/* NativeBridge — Kotlin object: external fun-методы НЕ статические,
 * вторым JNI-аргументом приходит экземпляр синглтона (jobject). */
JNIEXPORT void JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeInit(JNIEnv *env, jobject self)
{
	(*env)->GetJavaVM(env, &jvm);
	jclass cls = (*env)->GetObjectClass(env, self);
	bridge_class = (*env)->NewGlobalRef(env, cls);
	mid_on_adv_text = (*env)->GetStaticMethodID(env, bridge_class, "onAdvText",
	                                            "(Ljava/lang/String;Z)V");
	if (!mid_on_adv_text)
		(*env)->ExceptionClear(env);
	mid_on_adv_page = (*env)->GetStaticMethodID(env, bridge_class, "onAdvPage", "()V");
	if (!mid_on_adv_page)
		(*env)->ExceptionClear(env);
	BLOG("nativeInit: text=%p page=%p", (void*)mid_on_adv_text, (void*)mid_on_adv_page);
}

JNIEXPORT void JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeSetTts(JNIEnv *env, jobject self, jboolean on)
{
	(void)env; (void)self;
	bridge_set_tts_enabled(on);
}

JNIEXPORT void JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeAdvance(JNIEnv *env, jobject self)
{
	(void)env; (void)self;
	bridge_advance_message();
}

// Массив строк "page\tvarno\tname\tvalue" из bridge_var[]
static jobjectArray vars_to_jarray(JNIEnv *env, struct bridge_var *vars, int n)
{
	jclass str_cls = (*env)->FindClass(env, "java/lang/String");
	jobjectArray arr = (*env)->NewObjectArray(env, n, str_cls, NULL);
	for (int i = 0; i < n; i++) {
		char buf[512];
		snprintf(buf, sizeof(buf), "%d\t%d\t%s\t%d",
		         vars[i].page, vars[i].varno,
		         vars[i].name_utf8 ? vars[i].name_utf8 : "?", vars[i].value);
		jstring s = (*env)->NewStringUTF(env, buf);
		if (!s) { (*env)->ExceptionClear(env); continue; }
		(*env)->SetObjectArrayElement(env, arr, i, s);
		(*env)->DeleteLocalRef(env, s);
	}
	return arr;
}

#define CHEAT_UI_MAX 500

JNIEXPORT jobjectArray JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeCheatList(JNIEnv *env, jobject self, jstring jfilter)
{
	(void)self;
	const char *filter = jfilter ? (*env)->GetStringUTFChars(env, jfilter, NULL) : NULL;
	struct bridge_var *vars;
	int n = bridge_cheat_list(filter, &vars, CHEAT_UI_MAX);
	if (filter)
		(*env)->ReleaseStringUTFChars(env, jfilter, filter);
	jobjectArray arr = vars_to_jarray(env, vars, n);
	bridge_cheat_free(vars, n);
	return arr;
}

// Элемент [0] — "TOTAL:<полное число кандидатов>", далее строки переменных.
JNIEXPORT jobjectArray JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeCheatScan(JNIEnv *env, jobject self, jint value, jboolean narrow)
{
	(void)self;
	struct bridge_var *vars;
	int total = bridge_cheat_scan(value, narrow, &vars, CHEAT_UI_MAX);
	int shown = total < CHEAT_UI_MAX ? total : CHEAT_UI_MAX;

	jclass str_cls = (*env)->FindClass(env, "java/lang/String");
	jobjectArray arr = (*env)->NewObjectArray(env, shown + 1, str_cls, NULL);
	char hdr[32];
	snprintf(hdr, sizeof(hdr), "TOTAL:%d", total);
	jstring h = (*env)->NewStringUTF(env, hdr);
	(*env)->SetObjectArrayElement(env, arr, 0, h);
	(*env)->DeleteLocalRef(env, h);
	for (int i = 0; i < shown; i++) {
		char buf[512];
		snprintf(buf, sizeof(buf), "%d\t%d\t%s\t%d",
		         vars[i].page, vars[i].varno,
		         vars[i].name_utf8 ? vars[i].name_utf8 : "?", vars[i].value);
		jstring s = (*env)->NewStringUTF(env, buf);
		if (!s) { (*env)->ExceptionClear(env); continue; }
		(*env)->SetObjectArrayElement(env, arr, i + 1, s);
		(*env)->DeleteLocalRef(env, s);
	}
	bridge_cheat_free(vars, shown);
	return arr;
}

JNIEXPORT jboolean JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeCheatWrite(JNIEnv *env, jobject self, jint page, jint varno, jint value)
{
	(void)env; (void)self;
	return bridge_cheat_write(page, varno, value);
}

static JNIEnv *bridge_env(void)
{
	if (!jvm)
		return NULL;
	JNIEnv *env;
	if ((*jvm)->GetEnv(jvm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
		// поток VM живёт до конца процесса — Detach не требуется
		if ((*jvm)->AttachCurrentThread(jvm, &env, NULL) != JNI_OK)
			return NULL;
	}
	return env;
}

static void bridge_emit(const char *utf8)
{
	if (!tts_enabled || !mid_on_adv_text)
		return;
	JNIEnv *env = bridge_env();
	if (!env)
		return;
	jstring s = (*env)->NewStringUTF(env, utf8);
	if (!s) { (*env)->ExceptionClear(env); return; }
	(*env)->CallStaticVoidMethod(env, bridge_class, mid_on_adv_text, s, (jboolean)0);
	if ((*env)->ExceptionCheck(env))
		(*env)->ExceptionClear(env);
	(*env)->DeleteLocalRef(env, s);
}

static void bridge_emit_page(void)
{
	if (!tts_enabled || !mid_on_adv_page)
		return;
	JNIEnv *env = bridge_env();
	if (!env)
		return;
	(*env)->CallStaticVoidMethod(env, bridge_class, mid_on_adv_page);
	if ((*env)->ExceptionCheck(env))
		(*env)->ExceptionClear(env);
}

#endif /* __ANDROID__ */
