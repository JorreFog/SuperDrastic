/* lang.h: the in-game menu's language, as BananaLinux's menu has it (banana-ui's ui/src/lang.cpp, ported to C so the
 * three behave the same). The translations are /usr/share/banana/lang/<code>.tsv (part, context, English,
 * translation), the language is settings.conf's "language" (absent or "en": English). The code keeps its English
 * strings; every text the menu lays out is looked up as it is, and a line with a placeholder ({name}, %d, %s...)
 * matches the text the code filled in, the translation getting the same values. Lines with no translation stay
 * English and are listed in /run/banana/lang-missing-<part>.txt for the translator's next round. */
#ifndef BANANA_LANG_H
#define BANANA_LANG_H

/* load the language for one part of the file ("bananads", "superdrastic"); BANANA_LANGUAGE and BANANA_LANG_DIR
 * override the setting and the folder (tests). 1: a language other than English is on */
int lang_init(const char *part);
int lang_active(void);
const char *lang_name(void);        /* "简体中文" */
const char *lang_by(void);          /* "Majirefy": who translated it (the file's translated-by), "" for English */

/* the text to show for s: its translation, or s itself; the pointer stays valid */
const char *lang_tr(const char *s);
/* s is a piece of a line already looked up (wrapped, cut short): lang_tr gives it back as it is, without listing it */
void lang_keep(const char *s);

/* line breaking without spaces: CJK lets a line break between two of its characters, except before closing
 * punctuation and after opening punctuation */
int lang_cjk(int cp);
int lang_break_between(int prev, int cp);

#endif
