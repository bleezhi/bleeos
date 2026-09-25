/* Tiny Debian-like dialog kit for VGA text mode: menus, line input,
 * info screens, progress bars. Blocking, keyboard only (Up/Down/j/k,
 * Enter, Esc, number shortcuts). */
#ifndef TUI_H
#define TUI_H

#include "drivers.h"

/* centered modal dialog; returns button index, -1 on Esc */
int tui_dialog(const char *title, const char *body, const char **buttons,
               int n);
/* menu; returns selected index, -1 on Esc */
int tui_menu(const char *title, const char *body, const char **items, int n);
/* line input with default; returns len, -1 on Esc */
int tui_input(const char *title, const char *prompt, const char *def,
              char *buf, u32 cap);
/* info screen, waits for any key */
void tui_msg(const char *title, const char *msg);
/* progress screen + in-place updates (0..100) */
void tui_progress(const char *title, const char *label);
void tui_progress_update(int pct);

#endif
