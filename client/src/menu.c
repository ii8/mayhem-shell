
#include <wayland-util.h>
#include <cairo.h>

#include "mayhem-client.h"

#include "pool.h"
#include "menu.h"

enum item_align {
	ITEM_ALIGN_LEFT = 0x01,
	ITEM_ALIGN_RIGHT = 0x02,
	ITEM_ALIGN_TOP = 0x04,
	ITEM_ALIGN_BOT = 0x08
};

struct frame {
	struct wl_list link;
	int const width;
	int height;
	struct wl_surface *surface;
	struct ms_surface *msurf;
	struct buffer *buffers[2];
	struct wl_callback *callback;
	int dirty, need_resize;
	struct menu *menu;
	struct frame *parent; /* NULL if first */
	struct theme *theme;
	struct wl_list children;
	struct wl_list items;
};

struct menu {
	int closed;
	struct pool *pool;
	struct wl_compositor *ec;
	struct ms_menu *ms;
	struct theme *theme;
	struct wl_list top_frames;
};

enum item_type {
	ITEM_NONE,
	ITEM_TEXT,
	ITEM_BAR
};

struct item {
	struct wl_list link;
	enum item_type type;
	void (*draw_func)(struct item *, cairo_t *, int);
	int height;
};

static const struct theme default_theme = {
	.color_fg = 0xFF4791FF,
	.color_from = 0xFF363636,
	.color_to = 0xFF363636,
	.font_family = "serif",
	.font_slant = CAIRO_FONT_SLANT_NORMAL,
	.font_weight = CAIRO_FONT_WEIGHT_NORMAL,
	.padding = { 0, 0, 0, 0 },
	.align = ITEM_ALIGN_LEFT,
	.radius = 0
};

/*
struct item *item_create(struct frame *parent, enum item_type type)
{
	struct item *item;

	switch(type) {
		case ITEM_NONE:
			item = calloc(1, sizeof(struct item));
			break;
		case ITEM_TEXT:
			item = calloc(1, sizeof(struct item_text));
			break;
	}
	if(!item)
		return NULL;
	item->type = type;
	wl_list_insert(&parent->items, item->link);

	return item;
}
*/

static const struct wl_callback_listener frame_listener;

static void redraw(void *data, struct wl_callback *callback, uint32_t time)
{
	struct frame *frame = data;
	struct item *item;
	struct buffer *buffer;
	cairo_surface_t *surface;
	cairo_t *cr;
	int i = 0;
	unsigned int stride;

	if(!frame->dirty)
		goto end;


	if(frame->buffers[i]->flags & BUFFER_BUSY)
		if(frame->buffers[++i]->flags & BUFFER_BUSY)
			assert(0);
	buffer = frame->buffers[i];
	assert(buffer->flags & BUFFER_ACTIVE);

	if(frame->need_resize--) {
		buffer_destroy(buffer);
		buffer = buffer_create(frame->menu->pool,
				       frame->width,
				       frame->height,
				       WL_SHM_FORMAT_ARGB8888);
		frame->buffers[i] = buffer;
	}

	stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32,
					       frame->width);
	surface = cairo_image_surface_create_for_data(buffer->addr,
						      CAIRO_FORMAT_ARGB32,
						      frame->width,
						      frame->height,
						      stride);
	cr = cairo_create(surface);
	cairo_surface_destroy(surface);

	cairo_set_source_rgba(cr, 1, 1, 0, 0.80);
	//cairo_paint(cr);
	cairo_move_to(cr, 0, 50);

	wl_list_for_each(item, &frame->items, link) {
		cairo_save(cr);
		item->draw_func(item, cr, frame->width);
		cairo_restore(cr);
		cairo_rel_move_to(cr, 0, item->height);
	}

	if(cairo_status(cr) != CAIRO_STATUS_SUCCESS)
		printf("%s\n", cairo_status_to_string(cairo_status(cr)));

	cairo_destroy(cr);

	wl_surface_attach(frame->surface, buffer->buffer, 0, 0);
	wl_surface_damage(frame->surface, 0, 0, frame->width, frame->height);
	wl_surface_commit(frame->surface);
	buffer->flags |= BUFFER_BUSY;
	frame->dirty = 0;

end:
	if(callback)
		wl_callback_destroy(callback);

	frame->callback = wl_surface_frame(frame->surface);
	wl_callback_add_listener(frame->callback, &frame_listener, frame);
}

static const struct wl_callback_listener frame_listener = {
	redraw
};

/* Text item */
struct item_text {
	struct item base;
	int width;
	char *text, *font;
	uint32_t color;
	int size;
};

static void item_text_draw(struct item *item, cairo_t *cr, int width)
{
	struct item_text *text = (struct item_text *) item;

	cairo_select_font_face(cr, "serif", CAIRO_FONT_SLANT_NORMAL,
			       CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 24);

	//cairo_show_text(cr, text->text);
	cairo_text_path(cr, text->text);
	cairo_stroke(cr);
}

static void item_text_set_text(struct item_text *item, const char *text)
{
	item->text = text;
}

struct item_text *item_text_create(struct frame *parent, const char* text)
{
	struct item_text *item;

	item = calloc(1, sizeof(struct item_text));
	if(!item)
		return NULL;

	item->base.type = ITEM_TEXT;
	item->base.draw_func = item_text_draw;
	if(wl_list_empty(&parent->items)) {
		wl_list_insert(parent->items.prev, &item->base.link);
		//item->base.offset = 0;
	} else {
		struct item *prev;

		wl_list_insert(parent->items.prev, &item->base.link);
		prev = wl_container_of(item->base.link.prev, prev, link);
		//item->base.offset = prev->offset + prev->height * parent->width * 4;
	}
	//TODO
	item->base.width = 100;
	item->base.height = 100;
	item->text = text;

	parent->need_resize = 1;
	parent->height += item->base.height;

	return item;
}

/* Bar item */
struct item_bar {
	struct item base;
	uint32_t color;
	int padding[4]; /* top, right, bot, left */
	enum item_align align;
	double fill;
	enum item_bar_style {
		ITEM_BAR_STYLE_DOTTED = 0x01,
		ITEM_BAR_STYLE_ROUND = 0x02
	} style;
};

static void item_bar_draw(struct item *item, cairo_t *cr, int width)
{
	struct item_bar *bar = (struct item_bar *)item;

}

struct item_bar *item_bar_create(struct frame *frame, struct theme *theme)
{
	struct item_bar * item;

	item = calloc(1, sizeof(struct item_bar));
	if(!item)
		return NULL;

	wl_list_insert(parent->items.prev, &item->base.link);
	item->base.type = ITEM_BAR;
	item->base.draw_func = item_bar_draw;
	item->base.height = height;

	item->color = color;
	item->padding = padding;
	item->align
}

int frame_show(struct frame *frame)
{
	if(frame->callback || !frame->width || !frame->height)
		return -1;

	frame->buffers[0] = buffer_create(frame->menu->pool,
					  frame->width, frame->height,
					  WL_SHM_FORMAT_ARGB8888);
	if(!frame->buffers[0])
		return -2;

	frame->buffers[1] = buffer_create(frame->menu->pool,
					  frame->width, frame->height,
					  WL_SHM_FORMAT_ARGB8888);
	if(!frame->buffers[1])
		return buffer_destroy(frame->buffers[0]), return -2;

	frame->dirty = 1;
	frame->need_resize = 0;
	redraw(frame, NULL, 0);

	return 0;
}

struct theme *frame_get_theme(struct frame *frame)
{
	if(frame->theme == frame->menu->theme) {
		frame->theme = malloc(sizeof(struct theme));
		if(frame->theme == NULL)
			return NULL;
		memcpy(frame->theme, frame->menu->theme,
		       sizeof *frame->theme);
	}
	return frame->theme;
}

struct frame *frame_create(struct menu *menu, struct frame *parent)
{
	struct frame *frame;

	frame = calloc(1, sizeof *frame);
	if(!frame)
		return NULL;

	frame->menu = menu;
	frame->parent = parent;
	frame->surface = wl_compositor_create_surface(menu->ec);
	frame->msurf = ms_menu_get_menu_surface(menu->ms, frame->surface);

	frame->theme = menu->theme;

	if(parent)
		wl_list_insert(&parent->children, &frame->link);
	else
		wl_list_insert(&menu->top_frames, &frame->link);

	wl_list_init(&frame->children);
	wl_list_init(&frame->items);

	return frame;
}

static void frame_destroy(struct frame *frame)
{
	struct frame *child, *tmp;

	wl_list_for_each_safe(child, tmp, &frame->children, link) {
		printf("descending more\n");
		frame_destroy(child);
	}

	if(frame->callback) {
		wl_callback_destroy(frame->callback);

		buffer_destroy(frame->buffers[1]);
		buffer_destroy(frame->buffers[0]);
	}

	if(frame->theme != frame->menu->theme)
		free(frame->theme);

	ms_surface_destroy(frame->msurf);
	wl_surface_destroy(frame->surface);

	wl_list_remove(&frame->link);
	free(frame);
}

void menu_close(struct menu *menu)
{
	struct frame *child, *tmp;

	wl_list_for_each_safe(child, tmp, &menu->top_frames, link) {
		printf("descending into child\n");
		frame_destroy(child);
	}
}

struct theme *menu_get_theme(struct menu *menu)
{
	if(menu->theme == &default_theme) {
		menu->theme = malloc(sizeof(struct theme));
		if(menu->theme == NULL)
			return NULL;
		memcpy(menu->theme, &default_theme, sizeof *menu->theme);
	}
	return menu->theme;
}

struct menu *menu_create(struct wl_compositor *ec, struct ms_menu *ms,
			 struct pool *pool)
{
	struct menu *menu;

	menu = malloc(sizeof *menu);
	if(!menu)
		return NULL;

	menu->closed = 0;
	menu->ec = ec;
	menu->ms = ms;
	menu->pool = pool;
	wl_list_init(&menu->top_frames);
	menu->theme = &default_theme;

	if(api_init(menu) < 0) {
		free(menu);
		return NULL;
	}

	return menu;
}

void menu_destroy(struct menu *menu)
{
	menu_close(menu);

	if(menu->theme != &default_theme)
		free(menu->theme);

	free(menu);
	api_finish();
}

