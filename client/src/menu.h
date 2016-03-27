#ifndef MENU_H
#define MENU_H

struct menu;

struct menu *menu_create(struct wl_compositor *ec, struct wl_subcompositor *sc,
			 struct ms_menu *ms, struct pool *pool, uint32_t lang,
			 char const *file);
void menu_destroy(struct menu *menu);

void menu_event_pointer_enter(struct wl_surface *surf);
void menu_event_pointer_leave(struct wl_surface *surf);

#endif

