
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
#include "font.h"
#include "util.h"

#define PI 3.141592654

struct callback {
	struct wl_list link;
	void (*callback)(void *, void *);
	void (*destroy)(void *);
	void *data;
};

enum event_state {
	EVENT_RED = 1,
	EVENT_ACTIVE = 2,
	EVENT_OFF = 4,
	EVENT_DEAD = 8
};

struct event {
	enum event_state state;
	struct event *link[2];
	enum event_type type;
	struct wl_list callbacks;
};

static struct event leaf = { 0, { NULL, NULL }, EVENT_NONE, { NULL, NULL } };

static void flip(struct event *event)
{
	event->state |= EVENT_RED;
	event->link[0]->state &= ~EVENT_RED;
	event->link[1]->state &= ~EVENT_RED;
}

static struct event *rotate(struct event *event, int dir)
{
	struct event *tmp = event->link[!dir];

	event->link[!dir] = tmp->link[dir];
	tmp->link[dir] = event;

	tmp->state &= ~EVENT_RED;
	event->state |= EVENT_RED;

	return tmp;
}

static struct event *rotate2(struct event *event, int dir)
{
	event->link[!dir] = rotate(event->link[!dir], !dir);

	return rotate(event, dir);
}

static struct event *event_create(enum event_type event, int red)
{
	struct event *e;

	e = malloc(sizeof *e);
	e->link[0] = e->link[1] = &leaf;
	e->state = red;
	e->type = event;
	wl_list_init(&e->callbacks);
	return e;
}

static void event_register(struct event **root,
			   enum event_type event,
			   struct callback *cb)
{
	struct event fake = { 0, { NULL, *root }, EVENT_NONE, { NULL, NULL } };
	struct event *i = *root;
	struct event *p, *g, *gg;
	int dir = 0, last;

	/* Initializing `last` is not actually necessary, but it stops the
	 * compiler crying */
	last = 0;

	if(*root == &leaf) {
		*root = event_create(event, 0);
		wl_list_insert(&(*root)->callbacks, &cb->link);
		return;
	}

	gg = &fake;
	p = g = &leaf;

	for(; ; ) {
		if(i == &leaf)
			p->link[dir] = i = event_create(event, EVENT_RED);
		else if(i->link[0]->state & i->link[1]->state & EVENT_RED)
			flip(i);

		if(i->state & p->state & EVENT_RED) {
			int d = gg->link[1] == g;

			if(i == p->link[last])
				gg->link[d] = rotate(g, !last);
			else
				gg->link[d] = rotate2(g, !last);
		}

		if(event == i->type) {
			wl_list_insert(&i->callbacks, &cb->link);
			break;
		}

		last = dir;
		dir = i->type < event;

		if(g != &leaf)
			gg = g;
		g = p;
		p = i;
		i = i->link[dir];
	}
	*root = fake.link[1];
	(*root)->state &= ~EVENT_RED;
}

static void event_destroy_late(struct event *event)
{
	struct callback *cb, *tmp;

	wl_list_for_each_safe(cb, tmp, &event->callbacks, link) {
		cb->destroy(cb->data);
		wl_list_remove(&cb->link);
		free(cb);
	}
	if(event->state & EVENT_DEAD)
		free(event);
}

static void run_event(enum event_type ev, struct callback *cb, void *event);

static void event_fire(struct event *n, enum event_type ev, void *event)
{
	while(n) {
		if(n->type == ev) {
			struct callback *cb;

			n->state |= EVENT_ACTIVE;
			wl_list_for_each(cb, &n->callbacks, link)
				run_event(ev, cb, event);

			if(n->state & EVENT_OFF)
				event_destroy_late(n);
			else
				n->state &= ~EVENT_ACTIVE;
			return;
		}
		n = n->link[n->type < ev];
	}
}

static void event_remove(struct event *n, enum event_type ev)
{
	while(n) {
		if(n->type == ev) {
			if(n->state & EVENT_ACTIVE)
				n->state |= EVENT_OFF;
			else
				event_destroy_late(n);
			return;
		}
		n = n->link[n->type < ev];
	}
}

static void event_destroy(struct event *root)
{
	if(root == &leaf)
		return;

	event_destroy(root->link[0]);
	event_destroy(root->link[1]);

	root->state |= EVENT_DEAD | EVENT_OFF;
	if(~root->state & EVENT_ACTIVE)
		event_destroy_late(root);
}

struct frame {
	int width, height;
	struct wl_surface *surface;
	union {
		struct ms_surface *main;
		struct wl_subsurface *sub;
	} role;
	struct buffer *buffers[2];
	struct wl_callback *callback;
	/* need_resize is usually set to 2, one resize for each buffer */
	int dirty, need_resize;
	struct menu *menu;
	struct frame *parent; /* NULL if first */
	struct item *open;
	struct frame *child;
	struct theme *theme;
	struct wl_list items;
	uint32_t enter_time;
	struct item *pfocus;
	void (*api_destroy)(void *data);
	void *api_data;
	int api_focus;
	struct event *event;
};

struct menu {
	struct pool *pool;
	struct wl_compositor *ec;
	struct wl_subcompositor *sc;
	struct ms_menu *ms;
	struct theme *theme;
	struct frame *mainframe;
	struct frame *pfocus;
	void (*api_destroy)(void *data);
	void *api_context;
};

enum item_type {
	ITEM_NONE,
	ITEM_TEXT,
	ITEM_BAR
};

struct item {
	struct wl_list link;
	struct frame *parent;
	struct theme *theme;
	enum item_type type; //might not need this
	void (*draw)(struct item *, cairo_t *, int);
	void (*destroy)(struct item *);
	void (*api_destroy)(void *data);
	void *api_data;
	int api_focus;
	int height;
	struct event *event;
};

static const struct theme default_theme = {
	.open_delay = 300,
	.color = 0x4791FFFF,
	.color_from = 0x14395EFF,
	.color_to = 0x14537DFF,
	.color_item = 0x00000000,
	.font = NULL,
	.frame_padding = { 0, 0, 0, 0 },
	.item_padding = { 0, 0, 0, 0 },
	.align = ITEM_ALIGN_LEFT,
	.radius = 10,
	.min_width = 100,
	.max_width = 400,
};

static struct theme *theme_replicate(struct theme const *theme)
{
	struct theme *dup = xmalloc(sizeof *theme);
	memcpy(dup, theme, sizeof *theme);

	if(!dup->font)
		dup->font = font_create("monospace", 16);
	else
		font_ref(dup->font);

	return dup;
}

static void theme_destroy(struct theme *theme)
{
	font_unref(theme->font);
	free(theme);
}

static void decimal_color(uint32_t color, double *r, double *g, double *b,
			  double *a)
{
	*a = (double)(color & 0xFF) / 255;
	*b = (double)(color >> 0x08 & 0xFF) / 255;
	*g = (double)(color >> 0x10 & 0xFF) / 255;
	*r = (double)(color >> 0x18 & 0xFF) / 255;
}

static struct buffer *swap_buffers(struct frame *frame)
{
	struct buffer *buffer;
	int i = 0;

	if(frame->buffers[i]->flags & BUFFER_BUSY)
		if(frame->buffers[++i]->flags & BUFFER_BUSY)
			assert(0);
	buffer = frame->buffers[i];
	assert(buffer->flags & BUFFER_ACTIVE);

	if(frame->need_resize) {
		buffer_destroy(buffer);
		buffer = buffer_create(frame->menu->pool,
				       frame->width,
				       frame->height,
				       WL_SHM_FORMAT_ARGB8888);
		frame->buffers[i] = buffer;
		frame->need_resize--;
	}
	buffer->flags |= BUFFER_BUSY;

	return buffer;
}

static void draw_bg(cairo_t *cr, struct frame *frame)
{
	struct theme *theme = frame->theme;
	int rad = frame->theme->radius;
	cairo_pattern_t *bg;
	double rf, gf, bf, af;
	double rt, gt, bt, at;

	decimal_color(theme->color_from, &rf, &gf, &bf, &af);
	decimal_color(theme->color_to, &rt, &gt, &bt, &at);

	bg = cairo_pattern_create_linear(0, 0, 0, frame->height);
	cairo_pattern_add_color_stop_rgba(bg, 0, rf, gf, bf, af);
	cairo_pattern_add_color_stop_rgba(bg, 1, rt, gt, bt, at);

	cairo_new_sub_path(cr);
	cairo_arc(cr, rad, rad, rad, PI, 1.5 * PI);
	cairo_arc(cr, frame->width - rad, rad, rad, 1.5 * PI, 0);
	cairo_arc(cr, frame->width - rad, frame->height - rad, rad, 0, 0.5 * PI);
	cairo_arc(cr, rad, frame->height - rad, rad, 0.5 * PI, PI);
	cairo_close_path(cr);

//	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
//	cairo_paint(cr);
//	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	cairo_set_source(cr, bg);
	cairo_fill(cr);
	cairo_pattern_destroy(bg);
}

static void try_open(struct frame *frame, uint32_t time)
{
	struct item *item = frame->pfocus;
	struct ev_open *ev;

	if(!item)
		return;

	if(frame->enter_time == 0)
		frame->enter_time = time;
	if(time - frame->enter_time < frame->theme->open_delay)
		return;

	if(frame->child) {
		if(item == frame->open)
			return;

		frame_destroy(frame->child);
		frame->child = NULL;
	}

	ev = xmalloc(sizeof *ev);
	ev->frame = frame;
	ev->item = item;

	event_fire(item->event, EVENT_OPEN, ev);
	free(ev);
}

static const struct wl_callback_listener frame_listener;

static void redraw(void *data, struct wl_callback *callback, uint32_t time)
{
	struct frame *frame = data;
	struct item *item;
	struct buffer *buffer;
	cairo_surface_t *surface;
	cairo_t *cr;
	unsigned int stride;

	try_open(frame, time);

	assert(callback == frame->callback);

	if(!frame->dirty)
		goto end;

	buffer = swap_buffers(frame);

	stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32,
					       frame->width);
	surface = cairo_image_surface_create_for_data(buffer->addr,
						      CAIRO_FORMAT_ARGB32,
						      frame->width,
						      frame->height,
						      stride);

	cr = cairo_create(surface);
	cairo_surface_destroy(surface);

	draw_bg(cr, frame);
	cairo_set_source_rgba(cr, 1, 1, 1, 1.0);

	wl_list_for_each(item, &frame->items, link) {
		cairo_translate(cr, 0, item->height);
		//cairo_save(cr);
		item->draw(item, cr, frame->width);
		//cairo_restore(cr);
	}

	if(cairo_status(cr) != CAIRO_STATUS_SUCCESS)
		printf("cairo: %s\n", cairo_status_to_string(cairo_status(cr)));

	cairo_destroy(cr);

	wl_surface_attach(frame->surface, buffer->buffer, 0, 0);
	wl_surface_damage(frame->surface, 0, 0, frame->width, frame->height);
	frame->dirty = 0;

end:
	if(callback)
		wl_callback_destroy(callback);

	frame->callback = wl_surface_frame(frame->surface);
	wl_callback_add_listener(frame->callback, &frame_listener, frame);
	wl_surface_commit(frame->surface);
}

static const struct wl_callback_listener frame_listener = {
	redraw
};

unsigned item_offset(struct item *item)
{
	struct item *i;
	unsigned offset = 0;

	wl_list_for_each(i, &item->parent->items, link) {
		if(item == i)
			return offset;
		offset += i->height;
	}
	assert(0);
}

void item_register_event(struct item *item,
			 enum event_type ev,
			 void (*cb)(void *, void *),
			 void (*destroy)(void *),
			 void *data)
{
	struct callback *callback;

	callback = malloc(sizeof *callback);
	callback->callback = cb;
	callback->destroy = destroy;
	callback->data = data;
	event_register(&item->event, ev, callback);
}

struct theme *item_get_theme(struct item *item)
{
	if(item->theme != item->parent->theme)
		return item->theme;

	item->theme = theme_replicate(item->parent->theme);
	return item->theme;
}

static void item_init(struct item *item, struct frame *parent,
		      void (*api_destroy)(void *), void *api_data)
{
	item->parent = parent;
	item->theme = parent->theme;

	item->api_destroy = api_destroy;
	item->api_data = api_data;
	item->event = &leaf;

	wl_list_insert(parent->items.prev, &item->link);

	parent->need_resize = 2;
	parent->dirty = 1;

	parent->height += item->height;
}

static void item_destroy(struct item *item)
{
	if(item->api_destroy)
		item->api_destroy(item->api_data);

	if(item->theme != item->parent->theme)
		theme_destroy(item->theme);

	wl_list_remove(&item->link);
	free(item);
}

/* Text item */
struct item_text {
	struct item base;
	int width;
	struct text *text;
};

void item_text_set_text(struct item_text *item, const char *text)
{
	text_destroy(item->text);
	item->text = text_create(item->base.theme->font, text);
	item->base.parent->dirty = 1;
}

static void item_text_draw(struct item *item, cairo_t *cr, int width)
{
	struct item_text *text = (struct item_text *) item;
	double r, g, b, a;

	decimal_color(item->theme->color, &r, &g, &b, &a);
	cairo_set_source_rgba(cr, r, g, b, a);

	text_draw(text->text, cr);
}

static void item_text_destroy(struct item *item)
{
	struct item_text *item_text = (struct item_text *)item;

	text_destroy(item_text->text);
	item_destroy(item);
}

struct item_text *item_text_create(struct frame *parent,
				   void (*callback)(void *),
				   void *data,
				   const char *text)
{
	struct item_text *item;

	item = zalloc(sizeof(struct item_text));
	if(!item)
		return NULL;

	item->base.type = ITEM_TEXT;
	item->base.draw = item_text_draw;
	item->base.destroy = item_text_destroy;

	item->text = text_create(parent->theme->font, text);
	text_extents(item->text, &item->width, &item->base.height);

	if(!parent->callback) {
		if(item->width > parent->width)
			parent->width = item->width;
		if(parent->width > parent->theme->max_width)
			parent->width = parent->theme->max_width;
	}

	item_init((struct item *)item, parent, callback, data);

	return item;
}

/* Bar item */
struct item_bar {
	struct item base;
	double fill;
};

void item_bar_set_fill(struct item_bar *item, double fill)
{
	item->fill = fill;
	item->base.parent->dirty = 1;
}

static void item_bar_draw(struct item *item, cairo_t *cr, int width)
{
	struct item_bar *bar = (struct item_bar *)item;
	double r, g, b, a;

	decimal_color(item->theme->color, &r, &g, &b, &a);

	cairo_set_line_width(cr, item->height);
	cairo_set_source_rgba(cr, r, g, b, a);

	//cairo_rel_move_to(cr, 10, 10);//TODO padding??

	cairo_move_to(cr, 0, item->height / 2);
	cairo_rel_line_to(cr, width * bar->fill, 0);
	cairo_stroke(cr);
}

struct item_bar *item_bar_create(struct frame *parent,
				 void (*callback)(void *),
				 void *data,
				 int height)
{
	struct item_bar *item;

	item = zalloc(sizeof *item);
	if(!item)
		return NULL;

	item->base.type = ITEM_BAR;
	item->base.draw = item_bar_draw;
	item->base.destroy = item_destroy;

	item->base.height = height;

	item->fill = 1;

	item_init((struct item *)item, parent, callback, data);

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

int frame_width(struct frame *frame)
{
	return frame->width;
}

struct theme *frame_get_theme(struct frame *frame)
{
	if(frame->theme != frame->menu->theme)
		return frame->theme;

	frame->theme = theme_replicate(frame->menu->theme);
	return frame->theme;
}

void frame_register_event(struct frame *frame,
			  enum event_type ev,
			  void (*cb)(void *, void *),
			  void (*destroy)(void *),
			  void *data)
{
	struct callback *callback;

	callback = malloc(sizeof *callback);
	callback->callback = cb;
	callback->destroy = destroy;
	callback->data = data;
	event_register(&frame->event, ev, callback);
}

void frame_remove_events(struct frame *frame, enum event_type ev)
{
	event_remove(frame->event, ev);
}

static struct frame *frame_create(struct menu *menu, struct frame *parent)
{
	struct frame *frame;

	frame = zalloc(sizeof *frame);
	if(!frame)
		return NULL;

	frame->menu = menu;
	frame->parent = parent;
	frame->surface = wl_compositor_create_surface(menu->ec);
	wl_surface_set_user_data(frame->surface, frame);
	if(parent) {
		frame->role.sub =
			wl_subcompositor_get_subsurface(menu->sc,
							frame->surface,
							parent->surface);
		wl_subsurface_set_desync(frame->role.sub);
		parent->child = frame;
	} else {
		frame->role.main = ms_menu_get_menu_surface(menu->ms,
							    frame->surface);
		menu->mainframe = frame;
	}

	frame->theme = menu->theme;
	frame->width = frame->theme->min_width;
	frame->event = &leaf;
	frame->child = NULL;

	wl_list_init(&frame->items);

	return frame;
}

void frame_init(struct frame *frame, void (*callback)(void *), void *data)
{
	frame->api_destroy = callback;
	frame->api_data = data;
}

void frame_destroy(struct frame *frame)
{
	struct item *item, *tmp;

	if(frame->child)
		frame_destroy(frame->child);

	wl_list_for_each_safe(item, tmp, &frame->items, link)
		item->destroy(item);

	event_destroy(frame->event);

	if(frame->api_destroy)
		frame->api_destroy(frame->api_data);

	if(frame->callback) {
		wl_callback_destroy(frame->callback);

		buffer_destroy(frame->buffers[1]);
		buffer_destroy(frame->buffers[0]);
	}

	if(frame->theme != frame->menu->theme)
		theme_destroy(frame->theme);

	if(frame->parent) {
		frame->parent->child = NULL;
	} else {
		frame->menu->mainframe = NULL;
		ms_surface_destroy(frame->role.main);
	}
	wl_surface_destroy(frame->surface);

	free(frame);
}

struct theme *menu_get_theme(struct menu *menu)
{
	return menu->theme;
}

struct menu *menu_create(struct wl_compositor *ec, struct wl_subcompositor *sc,
			 struct ms_menu *ms, struct pool *pool, uint32_t lang,
			 char const *file)
{
	struct menu *menu;

	menu = malloc(sizeof *menu);
	if(!menu)
		goto err_menu;

	menu->ec = ec;
	menu->sc = sc;
	menu->ms = ms;
	menu->pool = pool;
	menu->pfocus = NULL;

	menu->theme = theme_replicate(&default_theme);

	menu->mainframe = frame_create(menu, NULL);
	if(menu->mainframe == NULL)
		goto err;

	switch(lang) {
	case MS_MENU_FRONTEND_LUA:
#ifdef API_LUA
		menu->api_destroy = api_init(menu, menu->mainframe, file,
					     &menu->api_context);
		if(!menu->api_destroy)
			goto err;
		break;
#else
		fprintf(stderr, "Not compiled with lua frontend\n");
		goto err;
#endif
	case MS_MENU_FRONTEND_PYTHON:
#ifdef API_PYTHON
		fprintf(stderr, "Python frontend not implemented\n");
		goto err;
#else
		fprintf(stderr, "Not compiled with python frontend\n");
		goto err;
#endif
	}

	return menu;

err:
	if(menu->mainframe)
		frame_destroy(menu->mainframe);
	theme_destroy(menu->theme);
	free(menu);
err_menu:
	return NULL;
}

void throw(char const *e)
{
	printf("%s\n", e);
}

void menu_destroy(struct menu *menu)
{
	if(menu->mainframe)
		frame_destroy(menu->mainframe);
	menu->api_destroy(menu->api_context);
	theme_destroy(menu->theme);
	free(menu);
}

int menu_focused(struct menu *menu)
{
	return menu->pfocus ? 1 : 0;
}

void menu_event_pointer_enter(struct wl_surface *surf)
{
	struct frame *frame = wl_surface_get_user_data(surf);

	if(frame == NULL)
		return;

	frame->menu->pfocus = frame;
	event_fire(frame->event, EVENT_ENTER, NULL);
}

void menu_event_pointer_leave(struct wl_surface *surf)
{
	struct frame *frame = wl_surface_get_user_data(surf);

	if(frame == NULL)
		return;

	frame->menu->pfocus->pfocus = NULL;
	frame->menu->pfocus = NULL;
}

void menu_event_pointer_motion(struct menu *menu, wl_fixed_t fx, wl_fixed_t fy)
{
	if(menu->pfocus) {
		struct frame *frame = menu->pfocus;
		int y = wl_fixed_to_int(fy);
		int offset = 0;
		struct item *i;

		wl_list_for_each(i, &frame->items, link) {
			offset += i->height;
			if(offset > y) {
				if(frame->pfocus != i) {
					frame->pfocus = i;
					frame->enter_time = 0;
				}
				return;
			}
		}
	}
}

static void run_event(enum event_type ev, struct callback *cb, void *event)
{
	switch(ev) {
	case EVENT_OPEN:
	{
		struct ev_open *ev_open = event;
		struct frame *parent = ev_open->frame;
		struct item *item = ev_open->item;
		struct frame *frame = frame_create(parent->menu, parent);
		int x = frame_width(parent);
		int y = item_offset(item);

		parent->child = frame;
		parent->open = item;
		wl_subsurface_set_position(frame->role.sub, x, y);
		ev_open->frame = frame;
	}
	default:
		cb->callback(cb->data, event);
	}
}
