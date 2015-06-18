
#ifdef MENU_H
#error "Cannot include both menu.h and api.h"
#endif

#ifndef API_H
#define API_H

struct frame;
struct menu;

//api_init(struct menu*);
//api_finish();

void menu_close(struct menu *);

struct frame *frame_create(struct menu *menu, struct frame *parent)
int frame_show(struct frame *);

#endif

