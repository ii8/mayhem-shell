
#ifndef API_H
#define API_H

enum event_type {
	EVENT_NONE,
	EVENT_OPEN,
	EVENT_CLOSE,
	EVENT_ENTER,
	EVENT_LEAVE,
	EVENT_CLICK
};

struct ev_open {
	struct item *item;
	struct frame *frame;
};

enum item_align {
	ITEM_ALIGN_LEFT = 0x01,
	ITEM_ALIGN_RIGHT = 0x02,
	ITEM_ALIGN_TOP = 0x04,
	ITEM_ALIGN_BOT = 0x08
};

struct theme {
	unsigned open_delay;
	uint32_t color;
	uint32_t color_from;
	uint32_t color_to;
	uint32_t color_item;
	struct font *font;
	int frame_padding[4]; /* top, right, bot, left */
	int item_padding[4];
	enum item_align align;
	int radius; /* Negative value should curve in edges */
	int min_width, max_width;
	//int item_height;
	//border*
};

//struct frame;
struct menu;
struct item;

struct theme *menu_get_theme(struct menu *menu);

void frame_init(struct frame *frame, void (*callback)(void *), void *data);
int frame_show(struct frame *);
void frame_destroy(struct frame *frame);
void frame_register_event(struct frame *,
			  enum event_type,
			  void (*)(void *, void *),
			  void (*)(void *),
			  void *);
void frame_remove_events(struct frame *frame, enum event_type ev);
int frame_width(struct frame *frame);
struct theme *frame_get_theme(struct frame *frame);

unsigned item_offset(struct item *item);
void item_register_event(struct item *,
			 enum event_type,
			 void (*)(void *, void *),
			 void (*)(void *),
			 void *);

struct item_bar *item_bar_create(struct frame *parent, void (*)(void *),
				 void *, int height);
void item_bar_set_fill(struct item_bar *, double);

struct item_text *item_text_create(struct frame *parent, void (*)(void *),
				   void *, const char* text);
void item_text_set_text(struct item_text *item, const char *text);

void throw(char const *e);

#ifdef API_LUA
void *api_init(struct menu *, struct frame *, char const *, void **);
#endif

#endif

