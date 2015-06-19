
#ifndef API_H
#define API_H

struct theme {
	uint32_t color_fg;
	uint32_t color_bg_from;
	uint32_t color_bg_to;
	char *font_family;
	cairo_font_slant_t font_slant;
	cairo_font_weight_t font_weight;
	int padding[4]; /* top, right, bot, left */
	enum item_align align;
	int radius; /* Negative value should curve in edges */
};

struct frame;
struct menu;

//api_init(struct menu*);
//api_finish();

void menu_close(struct menu *);

struct frame *frame_create(struct menu *menu, struct frame *parent)
int frame_show(struct frame *);

#endif

