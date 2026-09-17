#ifndef _MENU_H_
#define _MENU_H_
static inline int run_menu(void) { return 0; }
void gn_init_pbar(const char *title, int max);
void gn_update_pbar(int v);
void gn_terminate_pbar(void);
#endif
