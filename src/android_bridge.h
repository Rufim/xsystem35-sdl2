/* Мост xsystem35 <-> Android: ADV-текст для TTS, синтетический ввод,
 * доступ к переменным VM для читов.
 *
 * Общая часть платформонезависима; эмиттер имеет две реализации:
 * JNI (Android) и stdout-заглушка (прочие платформы, для отладки).
 */
#ifndef XSYSTEM35_ANDROID_BRIDGE_H
#define XSYSTEM35_ANDROID_BRIDGE_H

#include <stdbool.h>

// --- ADV-текст (зовётся из texthook.c) ---
void bridge_adv_message(const char *utf8);
void bridge_adv_newline(void);
void bridge_adv_page_break(void);
void bridge_adv_keywait(void);

// --- Управление (зовётся из JNI) ---
void bridge_set_tts_enabled(bool on);
void bridge_advance_message(void);

// --- Читы: доступ к переменным VM (16-битные значения) ---
struct bridge_var {
	int page;          // 0 — глобальные скаляры; >0 — страница массива varPage
	int varno;
	char *name_utf8;   // владеет вызывающий (bridge_cheat_free)
	int value;
};
int  bridge_cheat_read16(int page, int varno, int *ok);
bool bridge_cheat_write(int page, int varno, int value);
int  bridge_cheat_list(const char *filter_utf8, struct bridge_var **out, int max);
int  bridge_cheat_scan(int value, bool narrow, struct bridge_var **out, int max);
void bridge_cheat_free(struct bridge_var *arr, int n);

#endif /* XSYSTEM35_ANDROID_BRIDGE_H */
