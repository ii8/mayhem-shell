
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>

#include <cairo.h>
#include <wayland-client-core.h>
//#include <wayland-cursor.h>
#include "mayhem-client.h"
#include "pool.h"
#include "menu.h"

//#include <sys/types.h>

#define SHM_NAME "/mayhem-shm"

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_output *output;//temporary
	int output_width, output_height;
	struct ms_menu *ms;
	struct menu *menu;
	struct list *outputs;

	uint32_t formats;
	//enum cursor_type grab_cursor;

	struct pool *pool;
};

struct bg {
	struct buffer *buffer;
	struct wl_surface *wsurf;
	//cairo_surface_t *csurf;
	//cairo_t *cr;
	int width, height;
	char *file;
};

static volatile sig_atomic_t running = 1;

static void ms_configure(void *data,
				   struct ms_menu *ms,
				   uint32_t edges,
				   struct wl_surface *surface,
				   int32_t width, int32_t height)
{
	//struct display *d = data;
}

static void ms_grab_cursor(void *data, struct ms_menu *ms,
				     uint32_t cursor)
{
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
	ms_configure,
	ms_grab_cursor,
	ms_spawn,
	ms_despawn
};

static void output_handle_geometry(void *data,
                       struct wl_output *wl_output,
                       int x, int y,
                       int physical_width,
                       int physical_height,
                       int subpixel,
                       const char *make,
                       const char *model,
                       int transform)
{
	//struct display *d = data;
	//d->output_width = physical_width;
	//d->output_height = physical_height;
}

static void output_handle_mode(void *data,
		   struct wl_output *wl_output,
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

static void shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
	printf("format: %x\n", format);
	struct display *d = data;

	d->formats |= (1 << format);
}

static struct wl_shm_listener const shm_listener = {
	shm_format
};

void seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
			      uint32_t capabilities)
{
	struct display *d = data;

	if(capabilities & WL_SEAT_CAPABILITY_POINTER) {
		printf("have pointer\n");
		if(d->pointer)
			return;
		d->pointer = wl_seat_get_pointer(d->seat);
		wl_pointer_add_listener(d->pointer, menu_get_pointer_listener(),
					NULL);
	} else if(d->pointer) {
		printf("no pointer\n");
		wl_pointer_release(d->pointer);
	}
}

void seat_handle_name(void *data, struct wl_seat *wl_seat, const char *name)
{
}

static struct wl_seat_listener const seat_listener = {
	seat_handle_capabilities,
	seat_handle_name
};

static void registry_handle_global(void *data, struct wl_registry *registry,
				   uint32_t id, const char *interface,
				   uint32_t version)
{
	struct display *d = data;

	printf("got %s version %i\n", interface, version);
	if(strcmp(interface, "wl_compositor") == 0) {
		d->compositor = wl_registry_bind(registry, id,
						 &wl_compositor_interface, 1);
	} else if(strcmp(interface, "wl_shm") == 0) {
		printf("binding shm\n");
		d->shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
		wl_shm_add_listener(d->shm, &shm_listener, d);
	} else if(strcmp(interface, "wl_output") == 0) {
		printf("binding output\n");
		d->output = wl_registry_bind(registry, id,
					     &wl_output_interface, 2);

		wl_output_add_listener(d->output, &output_listener, d);
	} else if(strcmp(interface, "ms_menu") == 0) {
		printf("binding ms_menu\n");
		d->ms = wl_registry_bind(registry, id, &ms_menu_interface, 1);
		ms_menu_add_listener(d->ms, &ms_listener, d);
	} else if(strcmp(interface, "wl_seat") == 0) {
		printf("binding wl_seat\n");
		d->seat = wl_registry_bind(registry, id, &wl_seat_interface, 1);
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
	struct display *display;

	display = malloc(sizeof *display);
	if(display == NULL)
		return NULL;

	display->display = wl_display_connect(NULL);
	if(!display->display) {
		fprintf(stderr, "Could not connect to display");
		goto err_display;
	}

	display->formats = 0;
	display->registry = wl_display_get_registry(display->display);
	wl_registry_add_listener(display->registry, &registry_listener,
				 display);

	wl_display_roundtrip(display->display);
	if(display->shm == NULL || display->compositor == NULL) {
		fprintf(stderr, "Missing globals\n");
		goto err;
	}

	wl_display_roundtrip(display->display);

	if(!(display->formats & (1 << WL_SHM_FORMAT_ARGB8888))) {
		fprintf(stderr, "No ARGB32, Compositor sucks!\n");
		goto err;
	}

	display->pool = pool_create(display->shm, SHM_NAME, 8000);
	if(display->pool == NULL)
		goto err;

	return display;

err:
	if(display->shm)
		wl_shm_destroy(display->shm);
	if(display->seat)
		wl_seat_destroy(display->seat);
	if(display->compositor)
		wl_compositor_destroy(display->compositor);
	wl_registry_destroy(display->registry);
	wl_display_flush(display->display);
	wl_display_disconnect(display->display);

err_display:
	free(display);
	return NULL;
}

static void display_destroy(struct display *display)
{
	pool_destroy(display->pool);

	wl_shm_destroy(display->shm);
	wl_compositor_destroy(display->compositor);

	wl_registry_destroy(display->registry);
	wl_display_flush(display->display);
	wl_display_disconnect(display->display);

	free(display);
}

static struct bg *bg_create(struct display *d, int width, int height)
{
	struct bg *bg;

	bg = calloc(1, sizeof *bg);
	if(!bg)
		return NULL;
	bg->width = width;
	bg->height = height;
	bg->buffer = buffer_create(d->pool, width, height,
				   WL_SHM_FORMAT_XRGB8888);
	bg->wsurf = wl_compositor_create_surface(d->compositor);
	bg->file = "/home/murray/.bg/Anime/default.png";

	ms_menu_set_background(d->ms, d->output, bg->wsurf);

	return bg;
}

static void bg_destroy(struct bg* bg)
{
}

static void bg_draw(struct bg* bg)
{
	cairo_t *cr;
	cairo_pattern_t *pattern;
	cairo_surface_t *surface;
	void *data;

	surface = cairo_image_surface_create_from_png(bg->file);
	cr = cairo_create(surface);
	pattern = cairo_pattern_create_for_surface(surface);
	cairo_set_source(cr, pattern);
	cairo_pattern_destroy(pattern);
	cairo_paint(cr);
	cairo_surface_flush(surface);
	data = cairo_image_surface_get_data(surface);
	memcpy(bg->buffer->addr, data, bg->buffer->size);
	cairo_surface_destroy(surface);
	cairo_destroy(cr);

	wl_surface_attach(bg->wsurf, bg->buffer->buffer, 0, 0);
	wl_surface_damage(bg->wsurf, 0, 0, bg->width, bg->height);
	wl_surface_commit(bg->wsurf);
}

static void handle_sigint(int signum)
{
	running = 0;
}

int main(int argc, char *argv[])
{
	struct sigaction sigint;
	struct display *display;
	struct bg *bg;
	int ret = 0;

	display = display_create();
	if(!display)
		return EXIT_FAILURE;

	sigint.sa_handler = handle_sigint;
	sigemptyset(&sigint.sa_mask);
	sigint.sa_flags = SA_RESETHAND;
	sigaction(SIGINT, &sigint, NULL);

	bg = bg_create(display, 1920, 1080);
	if(bg)
		bg_draw(bg);

	while(running && ret != -1)
		ret = wl_display_dispatch(display->display);

	printf("Quit\n");

	if(bg)
		bg_destroy(bg);

	if(display->menu)
		menu_destroy(display->menu);

	display_destroy(display);

	return EXIT_SUCCESS;
}

