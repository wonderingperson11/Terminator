/*
 * keys.h - configurable key bindings (~/.terminatorrc).
 *
 * File format, one binding per line:
 *     bind <action> <key>          e.g.   bind line-start Ctrl+B
 * Keys are written as modifiers joined by '+': Ctrl, Shift, Alt, followed
 * by a key name (a letter/digit/symbol, or Left, Right, Up, Down, Home, End,
 * Tab, PageUp, PageDown, Insert, Delete, Escape, Space, F1..F12).
 *
 * The rc file is read by the GUI at start-up and whenever the `bind`
 * builtin changes it, so custom bindings apply without recompiling and
 * survive restarts.  Invalid or conflicting bindings are reported and the
 * action falls back to its default key.
 */
#ifndef TERMINATOR_KEYS_H
#define TERMINATOR_KEYS_H

#include "util.h"

#include <stddef.h>

#define KMOD_SHIFT 1u
#define KMOD_CTRL 2u
#define KMOD_ALT 4u

typedef struct {
    unsigned mods;
    unsigned long sym; /* X keysym (letters normalised to lower case) */
} KeyCombo;

typedef enum {
    KA_CURSOR_LEFT,
    KA_CURSOR_RIGHT,
    KA_LINE_START,
    KA_LINE_END,
    KA_HISTORY_PREV,
    KA_HISTORY_NEXT,
    KA_KILL_START,
    KA_KILL_END,
    KA_DELETE_WORD,
    KA_CLEAR_SCREEN,
    KA_EOF,
    KA_COPY,
    KA_PASTE,
    KA_INTERRUPT,
    KA_SUSPEND,
    KA_HISTORY_SEARCH,
    KA_COMPLETE,
    KA_NEW_TAB,
    KA_CLOSE_TAB,
    KA_NEXT_TAB,
    KA_PREV_TAB,
    KA_SCROLL_UP,
    KA_SCROLL_DOWN,
    KA_COUNT
} KeyAction;

typedef struct {
    KeyCombo combo[KA_COUNT];
    int custom[KA_COUNT];
} KeyBindings;

const char *keyaction_name(int a);
const char *keyaction_help(int a);
int keyaction_from_name(const char *name);
int keyaction_shell_action(int a); /* proto.h Action, or 0 for GUI-only */

int key_parse(const char *spec, KeyCombo *out, char *err, size_t errlen);
void key_format(const KeyCombo *k, char *out, size_t outlen);
int key_equal(const KeyCombo *a, const KeyCombo *b);
unsigned long key_normalise_sym(unsigned long sym);

void bindings_defaults(KeyBindings *kb);
/* Load ~/.terminatorrc over the defaults; problems are appended to errors
 * (if non-NULL) as human-readable messages. */
void bindings_load(KeyBindings *kb, StrVec *errors);
int bindings_lookup(const KeyBindings *kb, const KeyCombo *k); /* action or -1 */

const char *rc_path(void);
/* Set (key != NULL) or reset (key == NULL) one binding in the rc file. */
int rc_write_binding(const char *action, const char *key, char *err, size_t errlen);
int rc_reset_all(char *err, size_t errlen);

#endif
