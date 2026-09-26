/* Demo apps for the window manager. */
#ifndef APPS_H
#define APPS_H

void apps_open_demo(void);   /* opens Counter + SysInfo */
void apps_open_calc(void);
void apps_open_display(void);
void apps_open_aap(void);
int  apps_aap_key(int k);
void apps_aap_closed(int id);
void apps_aap_reset(void);
void apps_aap_tick(void);
int  apps_custom_key(int k);  /* custom-res editor: 1 = key consumed */
void apps_open_doom(void);
int  apps_game_key(int k);    /* doom window: 1 = key consumed */
int  apps_game_active(void);
void apps_game_close(void);
void apps_open_term(void);
int  apps_term_key(int k);    /* terminal window: 1 = key consumed */
int  apps_term_active(void);
int  apps_term_busy(void);
void apps_term_close(void);
void apps_doom_closed(int id);   /* doom window closed (X button) */
void apps_term_closed(int id);   /* terminal window closed (X button) */
void apps_doom_reset(void);      /* fresh login session */
void apps_term_reset(void);
void apps_window_closed(int id);  /* wm closed a window (X button) */
void apps_session_reset(void);    /* fresh login session */

#endif
