#ifndef MENU_H
#define MENU_H

struct menu;

struct menu *menu_create(struct wl_compositor *ec, struct ms_menu *ms,
			 struct pool *pool);
void menu_destroy(struct menu *menu);

#endif

