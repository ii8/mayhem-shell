
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <wayland-util.h>
#include <cairo.h>

#include "mayhem-client.h"

#include "pool.h"
#include "menu.h"
#include "api.h"

struct frame {
	struct wl_list link;
	int width, height;
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
	void (*draw)(struct item *, cairo_t *, int);
	void (*destroy)(struct item *);
	int height;
	int padding[4];
	// struct border*
};

static const struct theme default_theme = {
	.color = 0xFF4791FF,
	.color_from = 0xFF363636,
	.color_to = 0xFF363636,
	.font_family = "serif",
	//.font_slant = CAIRO_FONT_SLANT_NORMAL,
	//.font_weight = CAIRO_FONT_WEIGHT_NORMAL,
	.padding = { 0, 0, 0, 0 },
	.align = ITEM_ALIGN_LEFT,
	.radius = 0,
	.min_width = 100,
	.max_width = 400
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
		item->draw(item, cr, frame->width);
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

void item_text_set_text(struct item_text *item, const char *text)
{
	free(item->text);
	item->text = strdup(text);
}

static void item_text_destroy(struct item *item)
{
	struct item_text *item_text = (struct item_text *)item;

	free(item_text->text);
	free(item_text->font);

	wl_list_remove(&item->link);
	free(item);
}

struct item_text *item_text_create(struct frame *parent, const char* text)
{
	struct item_text *item;
	int width = 150;

	item = calloc(1, sizeof(struct item_text));
	if(!item)
		return NULL;

	item->base.type = ITEM_TEXT;
	item->base.draw = item_text_draw;
	item->base.destroy = item_text_destroy;

	//TODO
	item->base.height = 100;
	item->width = width;
	item->text = strdup(text);

	wl_list_insert(parent->items.prev, &item->base.link);
	parent->need_resize = 1;
	parent->dirty = 1;
	parent->width = width > parent->width ? width : parent->width;
	if(parent->width > parent->theme->max_width)
		parent->width = parent->theme->max_width;
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
	/*
	enum item_bar_style {
		ITEM_BAR_STYLE_DOTTED = 0x01,
		ITEM_BAR_STYLE_ROUND = 0x02
	} style;
	*/
};

static void item_bar_draw(struct item *item, cairo_t *cr, int width)
{
	struct item_bar *bar = (struct item_bar *)item;

}

static void item_bar_destroy(struct item *item)
{
	wl_list_remove(&item->link);
	free(item);
}

struct item_bar *item_bar_create(struct frame *parent, int height)
{
	struct item_bar * item;

	item = calloc(1, sizeof(struct item_bar));
	if(!item)
		return NULL;

	wl_list_insert(parent->items.prev, &item->base.link);
	item->base.type = ITEM_BAR;
	item->base.draw = item_bar_draw;
	item->base.destroy = item_bar_destroy;
	item->base.height = height;
	memcpy(item->base.padding, parent->theme->padding, sizeof(int[4]));

	item->color = parent->theme->color;
	item->align = ITEM_ALIGN_LEFT;
	item->fill = 1;

	parent->need_resize = 1;
	parent->height += item->base.height;

	return item;
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
		return buffer_destroy(frame->buffers[0]), -2;

	frame->dirty = 1;
	frame->need_resize = 0;
	redraw(frame, NULL, 0);

	return 0;
}

struct theme *frame_get_theme(struct frame *frame)
{
	if(frame->theme != frame->menu->theme)
		return frame->theme;

	frame->theme = malloc(sizeof(struct theme));
	if(frame->theme == NULL)
		return NULL;

	memcpy(frame->theme, frame->menu->theme, sizeof *frame->theme);

	frame->theme->font_family = strdup(frame->menu->theme->font_family);
	if(frame->theme->font_family == NULL) {
		free(frame->theme);
		frame->theme = frame->menu->theme;
		return NULL;
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

	frame->width = frame->theme->min_width;

	if(parent)
		wl_list_insert(&parent->children, &frame->link);
	else
		wl_list_insert(&menu->top_frames, &frame->link);

	wl_list_init(&frame->children);
	wl_list_init(&frame->items);

	return frame;
}

void frame_destroy(struct frame *frame)
{
	struct frame *child, *tmp;
	struct item *item, *itemp;

	wl_list_for_each_safe(child, tmp, &frame->children, link) {
		printf("descending more\n");
		frame_destroy(child);
	}

	wl_list_for_each_safe(item, itemp, &frame->items, link) {
		printf("clearing item\n");
		item->destroy(item);
	}

	if(frame->callback) {
		wl_callback_destroy(frame->callback);

		buffer_destroy(frame->buffers[1]);
		buffer_destroy(frame->buffers[0]);
	}

	if(frame->theme != frame->menu->theme) {
		free(frame->theme->font_family);
		free(frame->theme);
	}

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
	return menu->theme;
}

int api_init(struct menu *);
void api_finish(void);

struct menu *menu_create(struct wl_compositor *ec, struct ms_menu *ms,
			 struct pool *pool)
{
	struct menu *menu;

	menu = malloc(sizeof *menu);
	if(!menu)
		goto err_menu;

	menu->closed = 0;
	menu->ec = ec;
	menu->ms = ms;
	menu->pool = pool;
	wl_list_init(&menu->top_frames);

	menu->theme = malloc(sizeof(struct theme));
	if(menu->theme == NULL)
		goto err_theme;
	memcpy(menu->theme, &default_theme, sizeof *menu->theme);

	menu->theme->font_family = strdup(default_theme.font_family);
	if(menu->theme->font_family == NULL)
		goto err_font;

	if(api_init(menu) < 0)
		goto err;

	return menu;

err:
	free(menu->theme->font_family);
err_font:
	free(menu->theme);
err_theme:
	free(menu);
err_menu:
	return NULL;
}

void menu_destroy(struct menu *menu)
{
	menu_close(menu);
	free(menu->theme->font_family);
	free(menu->theme);
	free(menu);
	api_finish();
}

