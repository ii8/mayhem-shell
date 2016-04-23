#ifndef FONT_H
#define FONT_H

#include <cairo-ft.h>

int font_setup(void);
int font_finish(void);

struct font *font_create(char const *family, int size);
void font_ref(struct font *font);
void font_unref(struct font *font);

struct text *text_create(struct font *font, char const *text);
void text_destroy(struct text *text);
void text_extents(struct text *text, int *width, int *height);
void text_draw(struct text *text, cairo_t *cr);

#endif
