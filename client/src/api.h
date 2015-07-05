
#ifndef API_H
#define API_H

enum item_align {
	ITEM_ALIGN_LEFT = 0x01,
	ITEM_ALIGN_RIGHT = 0x02,
	ITEM_ALIGN_TOP = 0x04,
	ITEM_ALIGN_BOT = 0x08
};

struct theme {
	uint32_t color;
	uint32_t color_from;
	uint32_t color_to;
	char *font_family;
	//cairo_font_slant_t font_slant;
	//cairo_font_weight_t font_weight;
	int padding[4]; /* top, right, bot, left */
	enum item_align align;
	int radius; /* Negative value should curve in edges */
	int min_width, max_width;
	//int item_height;
	//border*
};

//struct frame;
struct menu;

void menu_close(struct menu *);
struct theme *menu_get_theme(struct menu *menu);

struct frame *frame_create(struct menu *menu, struct frame *parent,
			   void (*)(void *), void *);
void frame_destroy(struct frame *frame);
struct theme *frame_get_theme(struct frame *frame);
int frame_show(struct frame *);

struct item_bar *item_bar_create(struct frame *parent, void (*)(void *),
				 void *, int height);
void item_bar_set_fill(struct item_bar *, double);

struct item_text *item_text_create(struct frame *parent, void (*)(void *),
				   void *, const char* text);
void item_text_set_text(struct item_text *item, const char *text);

int api_init(struct menu *);
void api_finish(void);

#endif

