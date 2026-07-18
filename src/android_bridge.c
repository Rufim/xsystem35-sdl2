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
#include "volume.h"
#include "texthook.h"

#define BRIDGE_PAGE_MAX 256   // совпадает с PAGE_MAX в variable.c

static bool tts_enabled = false;

void bridge_set_tts_enabled(bool on) { tts_enabled = on; }

// Белый список номеров окон сообщений для озвучки (пусто = читать все окна).
// Позволяет читать только окно диалога, исключая боевой лог/статус/меню.
#define READ_WIN_MAX 32
static int read_windows[READ_WIN_MAX];
static int read_windows_n = 0;

void bridge_set_read_windows(const char *csv)
{
	read_windows_n = 0;
	if (!csv)
		return;
	for (const char *p = csv; *p && read_windows_n < READ_WIN_MAX; ) {
		while (*p == ' ' || *p == ',')
			p++;
		if (!*p)
			break;
		read_windows[read_windows_n++] = atoi(p);
		while (*p && *p != ',')
			p++;
	}
}

bool bridge_window_allowed(int winno)
{
	if (read_windows_n == 0)
		return true;   // пусто = читать все
	for (int i = 0; i < read_windows_n; i++)
		if (read_windows[i] == winno)
			return true;
	return false;
}

// Запрос открыть меню движка (громкость/пропуск/…). Ставится из UI-потока,
// исполняется в потоке игры (get_event), т.к. menu_open рисует модалку.
static volatile int menu_request = 0;
void bridge_request_menu(void) { menu_request = 1; }
int bridge_take_menu_request(void) { int r = menu_request; menu_request = 0; return r; }

// Синтетический левый клик мыши в очередь SDL — «дальше» в диалоге.
// Именно кнопка мыши шлёт защёлкнутое AGSEVENT_BUTTON_PRESS (event.c), которое
// движок обрабатывает надёжно; SDL_KEYDOWN лишь ставит мгновенное состояние
// клавиши и при быстрых DOWN+UP теряется на опросе keywait.
void bridge_advance_message(void)
{
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = SDL_MOUSEBUTTONDOWN;
	ev.button.button = SDL_BUTTON_LEFT;
	ev.button.state = SDL_PRESSED;
	ev.button.clicks = 1;
	ev.button.x = 100;
	ev.button.y = 100;
	SDL_PushEvent(&ev);
	ev.type = SDL_MOUSEBUTTONUP;
	ev.button.state = SDL_RELEASED;
	SDL_PushEvent(&ev);
}

static void bridge_emit(const char *utf8);
static void bridge_emit_page(void);
static void bridge_emit_window(int winno, int page);

/* Отбор «что читать» делает подавление по страницам сценария
 * (texthook_set_suppression_list): поведенчески диалог/бой/меню в System 3.9-
 * играх неразличимы (игра сама опрашивает ввод скриптом), но живут на разных
 * страницах — их номера видны в оверлее «стр N · окно M» при включённом TTS.
 *
 * Строки бокса НЕ озвучиваются по одной (иначе TTS делает паузу после каждой):
 * фрагменты копятся в буфере и отдаются одной репликой, когда бокс дорисован —
 * по паузе появления текста (ADV_FLUSH_MS), смене окна или новой странице. */
static int cur_winno = -1;
static int cur_page = -1;

#define ADV_BUF_MAX 4096
#define ADV_FLUSH_MS 200
static char adv_buf[ADV_BUF_MAX];
static size_t adv_len = 0;
static Uint32 adv_last_add = 0;

static void adv_flush(void)
{
	if (!adv_len)
		return;
	bridge_emit(adv_buf);
	adv_len = 0;
	adv_buf[0] = '\0';
}

// Смена окна/страницы: дочитать предыдущий бокс, обновить оверлей.
void bridge_report_window(int winno, int page)
{
	if (winno == cur_winno && page == cur_page)
		return;
	adv_flush();
	cur_winno = winno;
	cur_page = page;
	bridge_emit_window(winno, page);
}

void bridge_adv_message(const char *utf8)
{
	if (!utf8 || !*utf8)
		return;
	size_t l = strlen(utf8);
	if (adv_len + l + 2 >= ADV_BUF_MAX)
		adv_flush();   // переполнение — озвучить накопленное и продолжить
	if (l + 2 >= ADV_BUF_MAX)
		return;
	memcpy(adv_buf + adv_len, utf8, l);
	adv_len += l;
	adv_buf[adv_len] = '\0';
	adv_last_add = SDL_GetTicks();
}

void bridge_adv_newline(void)
{
	// разделить строки бокса пробелом
	if (adv_len && adv_buf[adv_len - 1] != ' ' && adv_len + 2 < ADV_BUF_MAX) {
		adv_buf[adv_len++] = ' ';
		adv_buf[adv_len] = '\0';
	}
}

void bridge_adv_page_break(void)
{
	adv_flush();
	bridge_emit_page();
}

void bridge_adv_keywait(void) { /* спамится каждый кадр ожидания — не используем */ }

// Зовётся каждый кадр из get_event: бокс дорисован (пауза текста) — озвучить.
void bridge_adv_tick(void)
{
	if (adv_len && SDL_GetTicks() - adv_last_add > ADV_FLUSH_MS)
		adv_flush();
}

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

static void bridge_emit_window(int winno, int page) { (void)winno; (void)page; }

#else /* __ANDROID__ */

#include <jni.h>
#include <android/log.h>
#define BLOG(...) __android_log_print(ANDROID_LOG_INFO, "xs35bridge", __VA_ARGS__)

static JavaVM *jvm = NULL;
static jclass bridge_class = NULL;      // GlobalRef на NativeBridge
static jmethodID mid_on_adv_text = NULL;
static jmethodID mid_on_adv_page = NULL;
static jmethodID mid_on_window = NULL;

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
	mid_on_window = (*env)->GetStaticMethodID(env, bridge_class, "onWindow", "(II)V");
	if (!mid_on_window)
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

// Открыть встроенное меню движка (громкость/пропуск/рестарт/выход).
JNIEXPORT void JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeOpenEngineMenu(JNIEnv *env, jobject self)
{
	(void)env; (void)self;
	bridge_request_menu();
}

JNIEXPORT void JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeSetReadWindows(
		JNIEnv *env, jobject self, jstring jcsv)
{
	(void)self;
	const char *p = jcsv ? (*env)->GetStringUTFChars(env, jcsv, NULL) : NULL;
	bridge_set_read_windows(p);
	if (p)
		(*env)->ReleaseStringUTFChars(env, jcsv, p);
}

// Список номеров сценарных страниц (через запятую), текст которых НЕ озвучивать
// (меню/статус-экраны). Пусто — не подавлять ничего.
JNIEXPORT void JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeSetSuppressPages(
		JNIEnv *env, jobject self, jstring jpages)
{
	(void)self;
	const char *p = jpages ? (*env)->GetStringUTFChars(env, jpages, NULL) : NULL;
	texthook_set_suppression_list((p && *p) ? p : NULL);
	if (p)
		(*env)->ReleaseStringUTFChars(env, jpages, p);
}

// Приглушение музыки на время речи через систему громкости движка (volume.c).
JNIEXPORT void JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeDuckMusic(
		JNIEnv *env, jobject self, jboolean on, jint percent)
{
	(void)env; (void)self;
	volume_duck(on, percent);
}

// Счётчик посимвольной отрисовки (модалко-детект авто-листания) — не реализован
// для System 3.x; авто-листание работает без защиты от модалок.
JNIEXPORT jint JNICALL
Java_io_github_rufim_alice_NativeBridge_nativeUiDrawCount(JNIEnv *env, jobject self)
{
	(void)env; (void)self;
	return 0;
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
	// лог до гейта: видно, что ушло бы в озвучку, даже при выключенном TTS
	BLOG("flush win=%d |%s|", cur_winno, utf8);
	if (!mid_on_adv_text)
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
	if (!mid_on_adv_page)
		return;
	JNIEnv *env = bridge_env();
	if (!env)
		return;
	(*env)->CallStaticVoidMethod(env, bridge_class, mid_on_adv_page);
	if ((*env)->ExceptionCheck(env))
		(*env)->ExceptionClear(env);
}

static void bridge_emit_window(int winno, int page)
{
	if (!mid_on_window)
		return;
	JNIEnv *env = bridge_env();
	if (!env)
		return;
	(*env)->CallStaticVoidMethod(env, bridge_class, mid_on_window, (jint)winno, (jint)page);
	if ((*env)->ExceptionCheck(env))
		(*env)->ExceptionClear(env);
}

#endif /* __ANDROID__ */
