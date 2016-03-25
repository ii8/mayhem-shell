
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>

#include <cairo.h>
#include <wayland-client-core.h>
#include <wayland-cursor.h>
#include "mayhem-client.h"
#include "pool.h"
#include "menu.h"
#include "cursors.h"
#include "util.h"

//#include <sys/types.h>

#define SHM_NAME "/mayhem-shm"

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_list outputs;
	struct ms_menu *ms;
	struct menu *menu;

	int has_argb;

	struct wl_cursor_theme *cursor_theme;
	struct wl_surface *grab_surface;
	struct wl_surface *pointer_surface;
	struct wl_cursor **cursors;
	enum cursor_type grab_cursor;

	struct pool *pool;
};

struct output {
	struct wl_output *output;
	uint32_t id;
	struct wl_list link;

	struct buffer *buffer;
	struct wl_surface *surf;
	char *file;
};

static volatile sig_atomic_t running = 1;

static void ms_grab_cursor(void *data, struct ms_menu *ms, uint32_t cursor)
{
	struct display *display = data;

	switch (cursor) {
	case MS_MENU_CURSOR_NONE:
		display->grab_cursor = CURSOR_BLANK;
		break;
	case MS_MENU_CURSOR_BUSY:
		display->grab_cursor = CURSOR_WATCH;
		break;
	case MS_MENU_CURSOR_MOVE:
		display->grab_cursor = CURSOR_DRAGGING;
		break;
	case MS_MENU_CURSOR_RESIZE_TOP:
		display->grab_cursor = CURSOR_TOP;
		break;
	case MS_MENU_CURSOR_RESIZE_BOTTOM:
		display->grab_cursor = CURSOR_BOTTOM;
		break;
	case MS_MENU_CURSOR_RESIZE_LEFT:
		display->grab_cursor = CURSOR_LEFT;
		break;
	case MS_MENU_CURSOR_RESIZE_RIGHT:
		display->grab_cursor = CURSOR_RIGHT;
		break;
	case MS_MENU_CURSOR_RESIZE_TOP_LEFT:
		display->grab_cursor = CURSOR_TOP_LEFT;
		break;
	case MS_MENU_CURSOR_RESIZE_TOP_RIGHT:
		display->grab_cursor = CURSOR_TOP_RIGHT;
		break;
	case MS_MENU_CURSOR_RESIZE_BOTTOM_LEFT:
		display->grab_cursor = CURSOR_BOTTOM_LEFT;
		break;
	case MS_MENU_CURSOR_RESIZE_BOTTOM_RIGHT:
		display->grab_cursor = CURSOR_BOTTOM_RIGHT;
		break;
	case MS_MENU_CURSOR_ARROW:
	default:
		display->grab_cursor = CURSOR_LEFT_PTR;
	}
}

static void ms_spawn(void *data, struct ms_menu *shell, uint32_t x, uint32_t y)
{
	struct display *d = data;
	struct menu *menu;

	if(d->menu != NULL)
		return;

	menu = menu_create(d->compositor, d->ms, d->pool);
	if(!menu) {
		fprintf(stderr, "Could not create menu\n");
		return;
	}
	d->menu = menu;
}

static void ms_despawn(void* data, struct ms_menu *ms_menu)
{
	struct display *d = data;

	if(d->menu == NULL)
		return;

	menu_destroy(d->menu);
	d->menu = NULL;
}

static const struct ms_menu_listener ms_listener = {
	ms_grab_cursor,
	ms_spawn,
	ms_despawn
};

static void output_handle_geometry(void *data,
				   struct wl_output *output,
				   int x, int y,
				   int physical_width,
				   int physical_height,
				   int subpixel,
				   const char *make,
				   const char *model,
				   int transform)
{
}

static void output_handle_mode(void *data,
			       struct wl_output *output,
			       uint32_t flags,
			       int width,
			       int height,
			       int refresh)
{
}

static void output_handle_done(void *data, struct wl_output *wl_output)
{
}

static void output_handle_scale(void *data, struct wl_output *wl_output,
				int32_t scale)
{
}

static const struct wl_output_listener output_listener = {
	output_handle_geometry,
	output_handle_mode,
	output_handle_done,
	output_handle_scale
};

static void output_draw_bg(struct output* o)
{
	cairo_t *cr;
	cairo_pattern_t *pattern;
	cairo_surface_t *surface;
	void *data;

	surface = cairo_image_surface_create_from_png(o->file);
	cr = cairo_create(surface);
	pattern = cairo_pattern_create_for_surface(surface);
	cairo_set_source(cr, pattern);
	cairo_pattern_destroy(pattern);
	cairo_paint(cr);
	cairo_surface_flush(surface);
	data = cairo_image_surface_get_data(surface);
	memcpy(o->buffer->addr, data, o->buffer->size);
	cairo_surface_destroy(surface);
	cairo_destroy(cr);

	wl_surface_attach(o->surf, o->buffer->buffer, 0, 0);
	wl_surface_damage_buffer(o->surf, 0, 0, 1920, 1080);
	wl_surface_commit(o->surf);
}

static int output_init(struct output *o, struct display *d)
{
	char *file = "/home/murray/.bg/Anime/default.png";

	o->buffer = buffer_create(d->pool, 1920, 1080,
				  WL_SHM_FORMAT_XRGB8888);
	if(o->buffer == NULL)
		goto err;

	o->surf = wl_compositor_create_surface(d->compositor);
	o->file = strdup(file);
	if(o->file == NULL)
		goto err_file;

	ms_menu_set_background(d->ms, o->output, o->surf);

	output_draw_bg(o);
	return 0;

err_file:
	buffer_destroy(o->buffer);
err:
	wl_list_remove(&o->link);
	wl_output_destroy(o->output);
	free(o);
	return -1;
}

static void output_destroy(struct output *o)
{
	buffer_destroy(o->buffer);
	wl_list_remove(&o->link);
	wl_surface_destroy(o->surf);
	wl_output_destroy(o->output);
	free(o->file);
	free(o);
}

static void shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
	struct display *d = data;

	if(format == WL_SHM_FORMAT_ARGB8888)
	   d->has_argb = 1;
}

static struct wl_shm_listener const shm_listener = {
	shm_format
};

static void set_cursor(struct display *d, struct wl_cursor *cursor,
		       uint32_t serial)
{
	struct wl_buffer *buffer;
	struct wl_cursor_image *img;

	if(!d->pointer)
		return;

	if(!cursor)
		return;

	img = cursor->images[0]; //TODO animate
	buffer = wl_cursor_image_get_buffer(img);
	if(!buffer)
		return;

	wl_surface_attach(d->pointer_surface, buffer, 0, 0);
	wl_surface_damage(d->pointer_surface, 0, 0, img->width, img->height);
	wl_surface_commit(d->pointer_surface);
	wl_pointer_set_cursor(d->pointer,
			      serial,
			      d->pointer_surface,
			      img->hotspot_x,
			      img->hotspot_y);
}

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
			  struct wl_surface *surf, wl_fixed_t x, wl_fixed_t y)
{
	struct display *d = data;

	if(!surf)
		return;

	if(surf == d->grab_surface)
		return set_cursor(d, d->cursors[d->grab_cursor], serial);

	set_cursor(d, d->cursors[CURSOR_LEFT_PTR], serial);

	menu_event_pointer_enter(surf);
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
			  struct wl_surface *surf)
{
	if(!surf)
		return;

	menu_event_pointer_leave(surf);
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
			   wl_fixed_t x, wl_fixed_t y)
{
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
			   uint32_t time, uint32_t button, uint32_t state)
{
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time,
			 uint32_t axis, wl_fixed_t value)
{
}

static void pointer_frame(void *data, struct wl_pointer *p)
{
}

static void pointer_axis_source(void *data, struct wl_pointer *p,
				uint32_t axis_source)
{
}

static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
			      uint32_t axis)
{
}

static void pointer_axis_discrete(void *data, struct wl_pointer *p,
				  uint32_t axis, int32_t discrete)
{
}

static struct wl_pointer_listener const pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete
};

void seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
			      uint32_t capabilities)
{
	struct display *d = data;

	if(capabilities & WL_SEAT_CAPABILITY_POINTER) {
		if(d->pointer)
			return;
		d->pointer = wl_seat_get_pointer(d->seat);
		wl_pointer_add_listener(d->pointer, &pointer_listener, d);
	} else if(d->pointer) {
		wl_pointer_release(d->pointer);
		d->pointer = NULL;
	}
}

void seat_handle_name(void *data, struct wl_seat *wl_seat, const char *name)
{
}

static struct wl_seat_listener const seat_listener = {
	seat_handle_capabilities,
	seat_handle_name
};

static void registry_handle_global(void *data, struct wl_registry *r,
				   uint32_t id, const char *interface,
				   uint32_t version)
{
	struct display *d = data;

	/* printf("got %s version %i\n", interface, version); */
	if(strcmp(interface, "wl_compositor") == 0) {
		d->compositor = wl_registry_bind(r, id,
						 &wl_compositor_interface, 4);
	} else if(strcmp(interface, "wl_shm") == 0) {
		d->shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
		wl_shm_add_listener(d->shm, &shm_listener, d);
	} else if(strcmp(interface, "wl_output") == 0) {
		struct output *o = zalloc(sizeof *o);
		if(!o)
			return;
		o->output = wl_registry_bind(r, id, &wl_output_interface, 2);
		o->id = id;
		wl_output_add_listener(o->output, &output_listener, o);
		wl_list_insert(&d->outputs, &o->link);
	} else if(strcmp(interface, "ms_menu") == 0) {
		d->ms = wl_registry_bind(r, id, &ms_menu_interface, 1);
		ms_menu_add_listener(d->ms, &ms_listener, d);
	} else if(strcmp(interface, "wl_seat") == 0) {
		d->seat = wl_registry_bind(r, id, &wl_seat_interface, 5);
		wl_seat_add_listener(d->seat, &seat_listener, d);
	}
}

static void registry_handle_global_remove(void *data,
					  struct wl_registry *registry,
					  uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

static struct display *display_create(void)
{
	struct display *d;
	struct wl_cursor *cursor;
	struct output *o, *tmp;
	unsigned i, j;

	d = zalloc(sizeof *d);
	if(!d)
		return NULL;

	wl_list_init(&d->outputs);

	d->display = wl_display_connect(NULL);
	if(!d->display) {
		fprintf(stderr, "Could not connect to display");
		goto err_display;
	}

	d->registry = wl_display_get_registry(d->display);
	if(!d->registry)
		goto err_registry;
	wl_registry_add_listener(d->registry, &registry_listener, d);

	if(wl_display_roundtrip(d->display) < 0)
		goto err;

	if(!d->shm || !d->compositor || !d->ms) {
		fprintf(stderr, "Missing globals\n");
		goto err;
	}

	if(wl_display_roundtrip(d->display) < 0)
		goto err;

	if(!d->has_argb) {
		fprintf(stderr, "No ARGB32, Compositor sucks!\n");
		goto err;
	}

	d->pool = pool_create(d->shm, SHM_NAME, 8000);
	if(!d->pool)
		goto err;

	d->cursor_theme = wl_cursor_theme_load(NULL, 32, d->shm);
	if(!d->cursor_theme)
		goto err_theme;

	d->cursors = calloc(cursor_count, sizeof d->cursors[0]);
	if(!d->cursors)
		goto err_cursors;

	for(i = 0; i < cursor_count; i++) {
		cursor = NULL;
		for(j = 0; !cursor && j < cursors[i].count; ++j)
			cursor = wl_cursor_theme_get_cursor(
				d->cursor_theme, cursors[i].names[j]);

		if(!cursor)
			fprintf(stderr, "could not load cursor '%s'\n",
				cursors[i].names[0]);

		d->cursors[i] = cursor;
	}

	d->grab_surface = wl_compositor_create_surface(d->compositor);
	d->pointer_surface = wl_compositor_create_surface(d->compositor);
	ms_menu_set_grab_surface(d->ms, d->grab_surface);

	wl_list_for_each(o, &d->outputs, link)
		if(output_init(o, d) < 0)
			goto err_output;

	return d;

err_output:
	wl_surface_destroy(d->pointer_surface);
	wl_surface_destroy(d->grab_surface);
	free(d->cursors);
err_cursors:
	wl_cursor_theme_destroy(d->cursor_theme);
err_theme:
	pool_destroy(d->pool);
err:
	if(d->pointer)
		wl_pointer_release(d->pointer);
	if(d->menu)
		menu_destroy(d->menu);
	if(d->shm)
		wl_shm_destroy(d->shm);
	if(d->seat)
		wl_seat_destroy(d->seat);
	if(d->compositor)
		wl_compositor_destroy(d->compositor);
	if(d->ms)
		ms_menu_destroy(d->ms);
	wl_list_for_each_safe(o, tmp, &d->outputs, link)
		output_destroy(o);

	wl_registry_destroy(d->registry);
err_registry:
	wl_display_flush(d->display);
	wl_display_disconnect(d->display);

err_display:
	free(d);
	return NULL;
}

static void display_destroy(struct display *d)
{
	struct output *o, *tmp;

	wl_surface_destroy(d->pointer_surface);
	wl_surface_destroy(d->grab_surface);
	free(d->cursors);
	wl_cursor_theme_destroy(d->cursor_theme);
	pool_destroy(d->pool);
	if(d->pointer)
		wl_pointer_release(d->pointer);
	if(d->menu)
		menu_destroy(d->menu);
	if(d->shm)
		wl_shm_destroy(d->shm);
	if(d->seat)
		wl_seat_destroy(d->seat);
	if(d->compositor)
		wl_compositor_destroy(d->compositor);
	if(d->ms)
		ms_menu_destroy(d->ms);
	wl_list_for_each_safe(o, tmp, &d->outputs, link)
		output_destroy(o);

	wl_registry_destroy(d->registry);
	wl_display_flush(d->display);
	wl_display_disconnect(d->display);
	free(d);
}

static void handle_sigint(int signum)
{
	running = 0;
}

int main(int argc, char *argv[])
{
	struct sigaction sigint;
	struct display *display;
	int ret = 0;

	display = display_create();
	if(!display)
		return EXIT_FAILURE;

	sigint.sa_handler = handle_sigint;
	sigemptyset(&sigint.sa_mask);
	sigint.sa_flags = SA_RESETHAND;
	sigaction(SIGINT, &sigint, NULL);

	while(running && ret != -1)
		ret = wl_display_dispatch(display->display);

	printf("Quit\n");

	display_destroy(display);

	return EXIT_SUCCESS;
}

