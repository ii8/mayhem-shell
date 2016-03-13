/*
 * Copyright © 2010-2012 Intel Corporation
 * Copyright © 2011-2012 Collabora, Ltd.
 * Copyright © 2013 Raspberry Pi Foundation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>
#include <assert.h>
#include <signal.h>
#include <math.h>
#include <sys/types.h>

#include "shell.h"
#include <weston/config-parser.h>
#include "xdg-shell-server.h"

#define DEFAULT_NUM_WORKSPACES 7
#define DEFAULT_WORKSPACE_CHANGE_ANIMATION_LENGTH 200

#ifndef static_assert
#define static_assert(cond, msg)
#endif

#define container_of(ptr, type, member) ({\
	const __typeof__( ((type *)0)->member ) *__mptr = (ptr);\
	(type *)( (char *)__mptr - offsetof(type,member) );})

struct focus_state {
	struct weston_seat *seat;
	struct workspace *ws;
	struct weston_surface *keyboard_focus;
	struct wl_list link;
	struct wl_listener seat_destroy_listener;
	struct wl_listener surface_destroy_listener;
};

enum shell_surface_type {
	SHELL_SURFACE_NONE,
	SHELL_SURFACE_TOPLEVEL,
	SHELL_SURFACE_POPUP,
	SHELL_SURFACE_XWAYLAND,
	SHELL_SURFACE_MENU
};

struct shell_client;

struct shell_surface {
	struct wl_resource *resource;
	struct wl_signal destroy_signal;
	struct shell_client *owner;

	struct weston_surface *surface;
	struct weston_view *view;
	int32_t last_width, last_height;
	struct wl_listener surface_destroy_listener;
	struct wl_listener resource_destroy_listener;

	struct weston_surface *parent;
	struct wl_list children_list;  /* child surfaces of this one */
	struct wl_list children_link;  /* sibling surfaces of this one */
	struct mayhem_shell *shell;

	enum shell_surface_type type;
	char *title, *class;
	int32_t saved_x, saved_y;
	int32_t saved_width, saved_height;
	bool saved_position_valid;
	bool saved_size_valid;
	bool saved_rotation_valid;
	int unresponsive, grabbed;
	uint32_t resize_edges;

	struct {
		struct weston_transform transform;
		struct weston_matrix rotation;
	} rotation;

	struct {
		struct wl_list grab_link;
		int32_t x, y;
		struct shell_seat *shseat;
		uint32_t serial;
	} popup;

	struct {
		int32_t x, y;
		uint32_t flags;
	} transient;

	struct {
		enum wl_shell_surface_fullscreen_method type;
		struct weston_transform transform; /* matrix from x, y */
		uint32_t framerate;
	} fullscreen;

	struct weston_transform workspace_transform;

	struct weston_output *fullscreen_output;
	struct weston_output *output;
	struct wl_list link;

	const struct weston_shell_client *client;

	struct surface_state {
		bool maximized;
		bool fullscreen;
		bool relative;
	} state, next_state, requested_state; /* surface states */
	bool state_changed;
	bool state_requested;

	struct {
		int32_t x, y, width, height;
	} geometry, next_geometry;
	bool has_set_geometry, has_next_geometry;

	int focus_count;
};

struct shell_grab {
	struct weston_pointer_grab grab;
	struct shell_surface *shsurf;
	struct wl_listener shsurf_destroy_listener;
};

struct weston_move_grab {
	struct shell_grab base;
	wl_fixed_t dx, dy;
	int client_initiated;
};

struct rotate_grab {
	struct shell_grab base;
	struct weston_matrix rotation;
	struct {
		float x;
		float y;
	} center;
};

struct shell_seat {
	struct weston_seat *seat;
	struct wl_listener seat_destroy_listener;
	struct weston_surface *focused_surface;

	struct wl_listener caps_changed_listener;
	struct wl_listener pointer_focus_listener;
	struct wl_listener keyboard_focus_listener;

	struct {
		struct weston_pointer_grab grab;
		struct wl_list surfaces_list;
		struct wl_client *client;
		int32_t initial_up;
		enum { POINTER, TOUCH } type;
	} popup_grab;
};

struct shell_client {
	struct wl_resource *resource;
	struct wl_client *client;
	struct mayhem_shell *shell;
	struct wl_listener destroy_listener;
	struct wl_event_source *ping_timer;
	uint32_t ping_serial;
	int unresponsive;
};

static void
surface_rotate(struct shell_surface *surface, struct weston_pointer *pointer);

static void
shell_fade_startup(struct mayhem_shell *shell);

static struct shell_seat *
get_shell_seat(struct weston_seat *seat);

static void
shell_surface_update_child_surface_layers(struct shell_surface *shsurf);

static bool
shell_surface_is_wl_shell_surface(struct shell_surface *shsurf);

static bool
shell_surface_is_xdg_surface(struct shell_surface *shsurf);

static bool
shell_surface_is_xdg_popup(struct shell_surface *shsurf);

static void
shell_surface_set_parent(struct shell_surface *shsurf,
                         struct weston_surface *parent);

static int
shell_surface_get_label(struct weston_surface *surface, char *buf, size_t len)
{
	struct shell_surface *shsurf;
	const char *typestr[] = {
		[SHELL_SURFACE_NONE] = "unidentified",
		[SHELL_SURFACE_TOPLEVEL] = "top-level",
		[SHELL_SURFACE_POPUP] = "popup",
		[SHELL_SURFACE_XWAYLAND] = "Xwayland",
		[SHELL_SURFACE_MENU] = "menu"
	};
	const char *t, *c;

	shsurf = get_shell_surface(surface);
	if (!shsurf)
		return snprintf(buf, len, "unidentified window");

	t = shsurf->title;
	c = shsurf->class;

	return snprintf(buf, len, "%s window%s%s%s%s%s",
		typestr[shsurf->type],
		t ? " '" : "", t ?: "", t ? "'" : "",
		c ? " of " : "", c ?: "");
}

static bool shell_surface_is_top_fullscreen(struct shell_surface *shsurf)
{
	return true;
/*
	struct mayhem_shell *shell;
	struct weston_view *top_fs_ev;

	shell = shsurf->shell;

	if (wl_list_empty(&shell->fullscreen_layer.view_list.link))
		return false;

	top_fs_ev = container_of(shell->fullscreen_layer.view_list.link.next,
			         struct weston_view,
				 layer_link.link);
	return (shsurf == get_shell_surface(top_fs_ev->surface));
*/
}

static void destroy_shell_grab_shsurf(struct wl_listener *listener, void *data)
{
	struct shell_grab *grab;

	grab = container_of(listener, struct shell_grab,
			    shsurf_destroy_listener);

	grab->shsurf = NULL;
}

struct weston_view *get_default_view(struct weston_surface *surface)
{
	struct shell_surface *shsurf;
	struct weston_view *view;

	if (!surface || wl_list_empty(&surface->views))
		return NULL;

	shsurf = get_shell_surface(surface);
	if (shsurf)
		return shsurf->view;

	wl_list_for_each(view, &surface->views, surface_link)
		if (weston_view_is_mapped(view))
			return view;

	return container_of(surface->views.next, struct weston_view, surface_link);
}

static void popup_grab_end(struct weston_pointer *pointer);

static void shell_grab_start(struct shell_grab *grab,
			     const struct weston_pointer_grab_interface *interface,
			     struct shell_surface *shsurf,
			     struct weston_pointer *pointer,
			     enum ms_menu_cursor cursor)
{
	struct mayhem_shell *shell = shsurf->shell;

	popup_grab_end(pointer);

	grab->grab.interface = interface;
	grab->shsurf = shsurf;
	grab->shsurf_destroy_listener.notify = destroy_shell_grab_shsurf;
	wl_signal_add(&shsurf->destroy_signal,
		      &grab->shsurf_destroy_listener);

	shsurf->grabbed = 1;
	weston_pointer_start_grab(pointer, &grab->grab);
	if(shell->child.mayhem_shell) {
		ms_menu_send_grab_cursor(shell->child.mayhem_shell, cursor);
		weston_pointer_set_focus(pointer,
					 get_default_view(shell->grab_surface),
					 wl_fixed_from_int(0),
					 wl_fixed_from_int(0));
	}
}

static void send_configure_for_surface(struct shell_surface *shsurf)
{
	int32_t width, height;
	struct surface_state *state;

	if (shsurf->state_requested)
		state = &shsurf->requested_state;
	else if (shsurf->state_changed)
		state = &shsurf->next_state;
	else
		state = &shsurf->state;

	if (state->fullscreen || state->maximized) {
		width = shsurf->output->width;
		height = shsurf->output->height;
	} else {
		width = 0;
		height = 0;
	}

	shsurf->client->send_configure(shsurf->surface, width, height);
}

static void shell_surface_state_changed(struct shell_surface *shsurf)
{
	if (shell_surface_is_xdg_surface(shsurf))
		send_configure_for_surface(shsurf);
}

static void shell_grab_end(struct shell_grab *grab)
{
	if (grab->shsurf) {
		wl_list_remove(&grab->shsurf_destroy_listener.link);
		grab->shsurf->grabbed = 0;

		if (grab->shsurf->resize_edges) {
			grab->shsurf->resize_edges = 0;
			shell_surface_state_changed(grab->shsurf);
		}
	}

	weston_pointer_end_grab(grab->grab.pointer);
}

static void center_on_output(struct weston_view *view,
			     struct weston_output *output);

static enum weston_keyboard_modifier get_modifier(char *modifier)
{
	if (!modifier)
		return MODIFIER_SUPER;

	if (!strcmp("ctrl", modifier))
		return MODIFIER_CTRL;
	else if (!strcmp("alt", modifier))
		return MODIFIER_ALT;
	else if (!strcmp("super", modifier))
		return MODIFIER_SUPER;
	else
		return MODIFIER_SUPER;
}

static enum animation_type get_animation_type(char *animation)
{
	if (!animation)
		return ANIMATION_NONE;

	if (!strcmp("zoom", animation))
		return ANIMATION_ZOOM;
	else if (!strcmp("fade", animation))
		return ANIMATION_FADE;
	else if (!strcmp("dim-layer", animation))
		return ANIMATION_DIM_LAYER;
	else
		return ANIMATION_NONE;
}

static void shell_configuration(struct mayhem_shell *shell)
{
	struct weston_config_section *section;
	char *s, *client;
	const char *libexec_dir;

	section = weston_config_get_section(shell->compositor->config,
					    "shell", NULL, NULL);

	//ret = asprintf(&client, "%s/%s", weston_config_get_libexec_dir(),
	//a	       "mayhem-menu");
	//if (ret < 0)
	//	client = NULL;
	libexec_dir = weston_config_get_libexec_dir();
	client = malloc(strlen(libexec_dir) + strlen("/mayhem-menu"));
	strcpy(client, libexec_dir);
	strcat(client, "/mayhem-menu");

	weston_config_section_get_string(section, "client", &s, client);
	free(client);
	shell->client = s;
	weston_config_section_get_string(section,
					 "binding-modifier", &s, "super");
	shell->binding_modifier = get_modifier(s);
	free(s);

	weston_config_section_get_string(section, "animation", &s, "none");
	shell->win_animation_type = get_animation_type(s);
	free(s);
	weston_config_section_get_string(section, "close-animation", &s, "fade");
	shell->win_close_animation_type = get_animation_type(s);
	free(s);
	weston_config_section_get_string(section,
					 "startup-animation", &s, "fade");
	shell->startup_animation_type = get_animation_type(s);
	free(s);
	if (shell->startup_animation_type == ANIMATION_ZOOM)
		shell->startup_animation_type = ANIMATION_NONE;
	weston_config_section_get_string(section, "focus-animation", &s, "none");
	shell->focus_animation_type = get_animation_type(s);
	free(s);
	weston_config_section_get_uint(section, "num-workspaces",
				       &shell->workspaces.num,
				       DEFAULT_NUM_WORKSPACES);
}

struct weston_output *get_default_output(struct weston_compositor *compositor)
{
	return container_of(compositor->output_list.next,
			    struct weston_output, link);
}

static int focus_surface_get_label(struct weston_surface *surface,
				   char *buf, size_t len)
{
	return snprintf(buf, len, "focus highlight effect for output %s",
			surface->output->name);
}

/* no-op func for checking focus surface */
static void focus_surface_configure(struct weston_surface *es,
				    int32_t sx, int32_t sy)
{
}

static struct focus_surface *get_focus_surface(struct weston_surface *surface)
{
	if (surface->configure == focus_surface_configure)
		return surface->configure_private;
	else
		return NULL;
}

static bool is_focus_surface(struct weston_surface *es)
{
	return (es->configure == focus_surface_configure);
}

static bool is_focus_view(struct weston_view *view)
{
	return is_focus_surface (view->surface);
}

static struct focus_surface *create_focus_surface(struct weston_compositor *ec,
						  struct weston_output *output)
{
	struct focus_surface *fsurf = NULL;
	struct weston_surface *surface = NULL;

	fsurf = malloc(sizeof *fsurf);
	if (!fsurf)
		return NULL;

	fsurf->surface = weston_surface_create(ec);
	surface = fsurf->surface;
	if (surface == NULL) {
		free(fsurf);
		return NULL;
	}

	surface->configure = focus_surface_configure;
	surface->output = output;
	surface->configure_private = fsurf;
	weston_surface_set_label_func(surface, focus_surface_get_label);

	fsurf->view = weston_view_create(surface);
	if (fsurf->view == NULL) {
		weston_surface_destroy(surface);
		free(fsurf);
		return NULL;
	}
	fsurf->view->output = output;

	weston_surface_set_size(surface, output->width, output->height);
	weston_view_set_position(fsurf->view, output->x, output->y);
	weston_surface_set_color(surface, 0.0, 0.0, 0.0, 1.0);
	pixman_region32_fini(&surface->opaque);
	pixman_region32_init_rect(&surface->opaque, output->x, output->y,
				  output->width, output->height);
	pixman_region32_fini(&surface->input);
	pixman_region32_init(&surface->input);

	wl_list_init(&fsurf->workspace_transform.link);

	return fsurf;
}

static void focus_surface_destroy(struct focus_surface *fsurf)
{
	weston_surface_destroy(fsurf->surface);
	free(fsurf);
}

static void focus_animation_done(struct weston_view_animation *animation,
				 void *data)
{
	struct workspace *ws = data;

	ws->focus_animation = NULL;
}

static void focus_state_destroy(struct focus_state *state)
{
	wl_list_remove(&state->seat_destroy_listener.link);
	wl_list_remove(&state->surface_destroy_listener.link);
	free(state);
}

static void focus_state_seat_destroy(struct wl_listener *listener, void *data)
{
	struct focus_state *state = container_of(listener,
						 struct focus_state,
						 seat_destroy_listener);

	wl_list_remove(&state->link);
	focus_state_destroy(state);
}

static void focus_state_surface_destroy(struct wl_listener *listener, void *data)
{
	struct focus_state *state = container_of(listener,
						 struct focus_state,
						 surface_destroy_listener);
	struct mayhem_shell *shell;
	struct weston_surface *main_surface, *next;
	struct weston_view *view;

	main_surface = weston_surface_get_main_surface(state->keyboard_focus);

	next = NULL;
	wl_list_for_each(view,
			 &state->ws->layer.view_list.link, layer_link.link) {
		if (view->surface == main_surface)
			continue;
		if (is_focus_view(view))
			continue;

		next = view->surface;
		break;
	}

	/* if the focus was a sub-surface, activate its main surface */
	if (main_surface != state->keyboard_focus)
		next = main_surface;

	shell = state->seat->compositor->shell_interface.shell;
	if (next) {
		state->keyboard_focus = NULL;
		activate(shell, next, state->seat, true);
	} else {
		if (shell->focus_animation_type == ANIMATION_DIM_LAYER) {
			if (state->ws->focus_animation)
				weston_view_animation_destroy(state->ws->focus_animation);

			state->ws->focus_animation = weston_fade_run(
				state->ws->fsurf_front->view,
				state->ws->fsurf_front->view->alpha, 0.0, 300,
				focus_animation_done, state->ws);
		}

		wl_list_remove(&state->link);
		focus_state_destroy(state);
	}
}

static struct focus_state *focus_state_create(struct weston_seat *seat,
					      struct workspace *ws)
{
	struct focus_state *state;

	state = malloc(sizeof *state);
	if (state == NULL)
		return NULL;

	state->keyboard_focus = NULL;
	state->ws = ws;
	state->seat = seat;
	wl_list_insert(&ws->focus_list, &state->link);

	state->seat_destroy_listener.notify = focus_state_seat_destroy;
	state->surface_destroy_listener.notify = focus_state_surface_destroy;
	wl_signal_add(&seat->destroy_signal,
		      &state->seat_destroy_listener);
	wl_list_init(&state->surface_destroy_listener.link);

	return state;
}

static struct focus_state *ensure_focus_state(struct mayhem_shell *shell,
					      struct weston_seat *seat)
{
	struct workspace *ws = get_current_workspace(shell);
	struct focus_state *state;

	wl_list_for_each(state, &ws->focus_list, link)
		if (state->seat == seat)
			break;

	if (&state->link == &ws->focus_list)
		state = focus_state_create(seat, ws);

	return state;
}

static void focus_state_set_focus(struct focus_state *state,
				  struct weston_surface *surface)
{
	if (state->keyboard_focus) {
		wl_list_remove(&state->surface_destroy_listener.link);
		wl_list_init(&state->surface_destroy_listener.link);
	}

	state->keyboard_focus = surface;
	if (surface)
		wl_signal_add(&surface->destroy_signal,
			      &state->surface_destroy_listener);
}

static void restore_focus_state(struct mayhem_shell *shell,
				struct workspace *ws)
{
	struct focus_state *state, *next;
	struct weston_surface *surface;
	struct wl_list pending_seat_list;
	struct weston_seat *seat, *next_seat;

	/* Temporarily steal the list of seats so that we can keep
	 * track of the seats we've already processed */
	wl_list_init(&pending_seat_list);
	wl_list_insert_list(&pending_seat_list, &shell->compositor->seat_list);
	wl_list_init(&shell->compositor->seat_list);

	wl_list_for_each_safe(state, next, &ws->focus_list, link) {
		struct weston_keyboard *keyboard =
			weston_seat_get_keyboard(state->seat);
		wl_list_remove(&state->seat->link);
		wl_list_insert(&shell->compositor->seat_list,
			       &state->seat->link);

		if (!keyboard)
			continue;

		surface = state->keyboard_focus;

		weston_keyboard_set_focus(keyboard, surface);
	}

	/* For any remaining seats that we don't have a focus state
	 * for we'll reset the keyboard focus to NULL */
	wl_list_for_each_safe(seat, next_seat, &pending_seat_list, link) {
		struct weston_keyboard *keyboard =
			weston_seat_get_keyboard(seat);

		wl_list_insert(&shell->compositor->seat_list, &seat->link);

		if (!keyboard)
			continue;

		weston_keyboard_set_focus(keyboard, NULL);
	}
}

static void replace_focus_state(struct mayhem_shell *shell,
				struct workspace *ws, struct weston_seat *seat)
{
	struct focus_state *state;
	struct weston_keyboard *keyboard = weston_seat_get_keyboard(seat);

	wl_list_for_each(state, &ws->focus_list, link) {
		if (state->seat == seat) {
			focus_state_set_focus(state, keyboard->focus);
			return;
		}
	}
}

static void drop_focus_state(struct mayhem_shell *shell, struct workspace *ws,
			     struct weston_surface *surface)
{
	struct focus_state *state;

	wl_list_for_each(state, &ws->focus_list, link)
		if (state->keyboard_focus == surface)
			focus_state_set_focus(state, NULL);
}

static void animate_focus_change(struct mayhem_shell *shell,
				 struct workspace *ws, struct weston_view *from,
				 struct weston_view *to)
{
	struct weston_output *output;
	bool focus_surface_created = false;

	/* FIXME: Only support dim animation using two layers */
	if (from == to || shell->focus_animation_type != ANIMATION_DIM_LAYER)
		return;

	output = get_default_output(shell->compositor);
	if (ws->fsurf_front == NULL && (from || to)) {
		ws->fsurf_front = create_focus_surface(shell->compositor, output);
		if (ws->fsurf_front == NULL)
			return;
		ws->fsurf_front->view->alpha = 0.0;

		ws->fsurf_back = create_focus_surface(shell->compositor, output);
		if (ws->fsurf_back == NULL) {
			focus_surface_destroy(ws->fsurf_front);
			return;
		}
		ws->fsurf_back->view->alpha = 0.0;

		focus_surface_created = true;
	} else {
		weston_layer_entry_remove(&ws->fsurf_front->view->layer_link);
		weston_layer_entry_remove(&ws->fsurf_back->view->layer_link);
	}

	if (ws->focus_animation) {
		weston_view_animation_destroy(ws->focus_animation);
		ws->focus_animation = NULL;
	}

	if (to)
		weston_layer_entry_insert(&to->layer_link,
					  &ws->fsurf_front->view->layer_link);
	else if (from)
		weston_layer_entry_insert(&ws->layer.view_list,
					  &ws->fsurf_front->view->layer_link);

	if (focus_surface_created) {
		ws->focus_animation = weston_fade_run(
			ws->fsurf_front->view,
			ws->fsurf_front->view->alpha, 0.4, 300,
			focus_animation_done, ws);
	} else if (from) {
		weston_layer_entry_insert(&from->layer_link,
					  &ws->fsurf_back->view->layer_link);
		ws->focus_animation = weston_stable_fade_run(
			ws->fsurf_front->view, 0.0,
			ws->fsurf_back->view, 0.4,
			focus_animation_done, ws);
	} else if (to) {
		weston_layer_entry_insert(&ws->layer.view_list,
					  &ws->fsurf_back->view->layer_link);
		ws->focus_animation = weston_stable_fade_run(
			ws->fsurf_front->view, 0.0,
			ws->fsurf_back->view, 0.4,
			focus_animation_done, ws);
	}
}

static void workspace_destroy(struct workspace *ws)
{
	struct focus_state *state, *next;

	wl_list_for_each_safe(state, next, &ws->focus_list, link)
		focus_state_destroy(state);

	if (ws->fsurf_front)
		focus_surface_destroy(ws->fsurf_front);
	if (ws->fsurf_back)
		focus_surface_destroy(ws->fsurf_back);

	free(ws);
}

static void seat_destroyed(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat = data;
	struct focus_state *state, *next;
	struct workspace *ws = container_of(listener,
					    struct workspace,
					    seat_destroyed_listener);

	wl_list_for_each_safe(state, next, &ws->focus_list, link)
		if (state->seat == seat)
			wl_list_remove(&state->link);
}

static struct workspace *workspace_create(void)
{
	struct workspace *ws = malloc(sizeof *ws);
	if (ws == NULL)
		return NULL;

	weston_layer_init(&ws->layer, NULL);

	wl_list_init(&ws->focus_list);
	wl_list_init(&ws->seat_destroyed_listener.link);
	ws->seat_destroyed_listener.notify = seat_destroyed;
	ws->fsurf_front = NULL;
	ws->fsurf_back = NULL;
	ws->focus_animation = NULL;

	return ws;
}

static int workspace_is_empty(struct workspace *ws)
{
	return wl_list_empty(&ws->layer.view_list.link);
}

static struct workspace *get_workspace(struct mayhem_shell *shell,
				       unsigned int index)
{
	struct workspace **pws = shell->workspaces.array.data;
	assert(index < shell->workspaces.num);
	pws += index;
	return *pws;
}

struct workspace *get_current_workspace(struct mayhem_shell *shell)
{
	return get_workspace(shell, shell->workspaces.current);
}

static void activate_workspace(struct mayhem_shell *shell, unsigned int index)
{
	struct workspace *ws;

	ws = get_workspace(shell, index);
	wl_list_insert(&shell->compositor->cursor_layer.link, &ws->layer.link);

	shell->workspaces.current = index;
}

static unsigned int get_output_height(struct weston_output *output)
{
	return abs(output->region.extents.y1 - output->region.extents.y2);
}

static void view_translate(struct workspace *ws, struct weston_view *view,
			   double d)
{
	struct weston_transform *transform;

	if (is_focus_view(view)) {
		struct focus_surface *fsurf = get_focus_surface(view->surface);
		transform = &fsurf->workspace_transform;
	} else {
		struct shell_surface *shsurf = get_shell_surface(view->surface);
		transform = &shsurf->workspace_transform;
	}

	if (wl_list_empty(&transform->link))
		wl_list_insert(view->geometry.transformation_list.prev,
			       &transform->link);

	weston_matrix_init(&transform->matrix);
	weston_matrix_translate(&transform->matrix,
				0.0, d, 0.0);
	weston_view_geometry_dirty(view);
}

static void workspace_translate_out(struct workspace *ws, double fraction)
{
	struct weston_view *view;
	unsigned int height;
	double d;

	wl_list_for_each(view, &ws->layer.view_list.link, layer_link.link) {
		height = get_output_height(view->surface->output);
		d = height * fraction;

		view_translate(ws, view, d);
	}
}

static void workspace_translate_in(struct workspace *ws, double fraction)
{
	struct weston_view *view;
	unsigned int height;
	double d;

	wl_list_for_each(view, &ws->layer.view_list.link, layer_link.link) {
		height = get_output_height(view->surface->output);

		if (fraction > 0)
			d = -(height - height * fraction);
		else
			d = height + height * fraction;

		view_translate(ws, view, d);
	}
}

static void reverse_workspace_change_animation(struct mayhem_shell *shell,
					       unsigned int index,
					       struct workspace *from,
					       struct workspace *to)
{
	shell->workspaces.current = index;

	shell->workspaces.anim_to = to;
	shell->workspaces.anim_from = from;
	shell->workspaces.anim_dir = -1 * shell->workspaces.anim_dir;
	shell->workspaces.anim_timestamp = 0;

	weston_compositor_schedule_repaint(shell->compositor);
}

static void workspace_deactivate_transforms(struct workspace *ws)
{
	struct weston_view *view;
	struct weston_transform *transform;

	wl_list_for_each(view, &ws->layer.view_list.link, layer_link.link) {
		if (is_focus_view(view)) {
			struct focus_surface *fsurf = get_focus_surface(view->surface);
			transform = &fsurf->workspace_transform;
		} else {
			struct shell_surface *shsurf = get_shell_surface(view->surface);
			transform = &shsurf->workspace_transform;
		}

		if (!wl_list_empty(&transform->link)) {
			wl_list_remove(&transform->link);
			wl_list_init(&transform->link);
		}
		weston_view_geometry_dirty(view);
	}
}

static void finish_workspace_change_animation(struct mayhem_shell *shell,
					      struct workspace *from,
					      struct workspace *to)
{
	struct weston_view *view;

	weston_compositor_schedule_repaint(shell->compositor);

	/* Views that extend past the bottom of the output are still
	 * visible after the workspace animation ends but before its layer
	 * is hidden. In that case, we need to damage below those views so
	 * that the screen is properly repainted. */
	wl_list_for_each(view, &from->layer.view_list.link, layer_link.link)
		weston_view_damage_below(view);

	wl_list_remove(&shell->workspaces.animation.link);
	workspace_deactivate_transforms(from);
	workspace_deactivate_transforms(to);
	shell->workspaces.anim_to = NULL;

	wl_list_remove(&shell->workspaces.anim_from->layer.link);
}

static void animate_workspace_change_frame(struct weston_animation *animation,
					   struct weston_output *output,
					   uint32_t msecs)
{
	struct mayhem_shell *shell =
		container_of(animation, struct mayhem_shell,
			     workspaces.animation);
	struct workspace *from = shell->workspaces.anim_from;
	struct workspace *to = shell->workspaces.anim_to;
	uint32_t t;
	double x, y;

	if (workspace_is_empty(from) && workspace_is_empty(to)) {
		finish_workspace_change_animation(shell, from, to);
		return;
	}

	if (shell->workspaces.anim_timestamp == 0) {
		if (shell->workspaces.anim_current == 0.0)
			shell->workspaces.anim_timestamp = msecs;
		else
			shell->workspaces.anim_timestamp =
				msecs -
				/* Invers of movement function 'y' below. */
				(asin(1.0 - shell->workspaces.anim_current) *
				 DEFAULT_WORKSPACE_CHANGE_ANIMATION_LENGTH *
				 M_2_PI);
	}

	t = msecs - shell->workspaces.anim_timestamp;

	/*
	 * x = [0, π/2]
	 * y(x) = sin(x)
	 */
	x = t * (1.0/DEFAULT_WORKSPACE_CHANGE_ANIMATION_LENGTH) * M_PI_2;
	y = sin(x);

	if (t < DEFAULT_WORKSPACE_CHANGE_ANIMATION_LENGTH) {
		weston_compositor_schedule_repaint(shell->compositor);

		workspace_translate_out(from, shell->workspaces.anim_dir * y);
		workspace_translate_in(to, shell->workspaces.anim_dir * y);
		shell->workspaces.anim_current = y;

		weston_compositor_schedule_repaint(shell->compositor);
	}
	else
		finish_workspace_change_animation(shell, from, to);
}

static void animate_workspace_change(struct mayhem_shell *shell,
				     unsigned int index,
				     struct workspace *from,
				     struct workspace *to)
{
	struct weston_output *output;
	int dir;

	if(index == 0 && shell->workspaces.current == shell->workspaces.num-1)
		dir = -1;
	else if(index == shell->workspaces.num-1
		&& shell->workspaces.current == 0)
		dir = 1;
	else if(index > shell->workspaces.current)
		dir = -1;
	else
		dir = 1;

	shell->workspaces.current = index;

	shell->workspaces.anim_dir = dir;
	shell->workspaces.anim_from = from;
	shell->workspaces.anim_to = to;
	shell->workspaces.anim_current = 0.0;
	shell->workspaces.anim_timestamp = 0;

	output = container_of(shell->compositor->output_list.next,
			      struct weston_output, link);
	wl_list_insert(&output->animation_list,
		       &shell->workspaces.animation.link);

	wl_list_insert(from->layer.link.prev, &to->layer.link);

	workspace_translate_in(to, 0);

	restore_focus_state(shell, to);

	weston_compositor_schedule_repaint(shell->compositor);
}

static void update_workspace(struct mayhem_shell *shell, unsigned int index,
			     struct workspace *from, struct workspace *to)
{
	shell->workspaces.current = index;
	wl_list_insert(&from->layer.link, &to->layer.link);
	wl_list_remove(&from->layer.link);
}

static void change_workspace(struct mayhem_shell *shell, unsigned int index)
{
	struct workspace *from;
	struct workspace *to;
	struct focus_state *state;

	if (index == shell->workspaces.current)
		return;

	/* Don't change workspace when there is any fullscreen surfaces. */
	//if (!wl_list_empty(&shell->fullscreen_layer.view_list.link))
	//	return;

	ms_menu_send_despawn(shell->child.mayhem_shell);

	from = get_current_workspace(shell);
	to = get_workspace(shell, index);

	if (shell->workspaces.anim_from == to &&
	    shell->workspaces.anim_to == from) {
		restore_focus_state(shell, to);
		reverse_workspace_change_animation(shell, index, from, to);
		return;
	}

	if (shell->workspaces.anim_to != NULL)
		finish_workspace_change_animation(shell,
						  shell->workspaces.anim_from,
						  shell->workspaces.anim_to);

	restore_focus_state(shell, to);

	if (shell->focus_animation_type != ANIMATION_NONE) {
		wl_list_for_each(state, &from->focus_list, link)
			if (state->keyboard_focus)
				animate_focus_change(shell, from,
						     get_default_view(state->keyboard_focus), NULL);

		wl_list_for_each(state, &to->focus_list, link)
			if (state->keyboard_focus)
				animate_focus_change(shell, to,
						     NULL, get_default_view(state->keyboard_focus));
	}

	if (workspace_is_empty(to) && workspace_is_empty(from))
		update_workspace(shell, index, from, to);
	else
		animate_workspace_change(shell, index, from, to);

}

static bool workspace_has_only(struct workspace *ws,
			       struct weston_surface *surface)
{
	struct wl_list *list = &ws->layer.view_list.link;
	struct wl_list *e;

	if (wl_list_empty(list))
		return false;

	e = list->next;

	if (e->next != list)
		return false;

	return container_of(e, struct weston_view, layer_link.link)->surface == surface;
}

static void surface_keyboard_focus_lost(struct weston_surface *surface)
{
	struct weston_compositor *compositor = surface->compositor;
	struct weston_seat *seat;
	struct weston_surface *focus;

	wl_list_for_each(seat, &compositor->seat_list, link) {
		struct weston_keyboard *keyboard =
			weston_seat_get_keyboard(seat);

		if (!keyboard)
			continue;

		focus = weston_surface_get_main_surface(keyboard->focus);
		if (focus == surface)
			weston_keyboard_set_focus(keyboard, NULL);
	}
}

static void take_surface_to_workspace_by_seat(struct mayhem_shell *shell,
					      struct weston_seat *seat,
					      unsigned int index)
{
	struct weston_keyboard *keyboard = weston_seat_get_keyboard(seat);
	struct weston_surface *surface;
	struct weston_view *view;
	struct shell_surface *shsurf;
	struct workspace *from;
	struct workspace *to;
	struct focus_state *state;

	surface = weston_surface_get_main_surface(keyboard->focus);
	view = get_default_view(surface);
	if (view == NULL ||
	    index == shell->workspaces.current ||
	    is_focus_view(view))
		return;

	from = get_current_workspace(shell);
	to = get_workspace(shell, index);

	weston_layer_entry_remove(&view->layer_link);
	weston_layer_entry_insert(&to->layer.view_list, &view->layer_link);

	shsurf = get_shell_surface(surface);
	if (shsurf != NULL)
		shell_surface_update_child_surface_layers(shsurf);

	replace_focus_state(shell, to, seat);
	drop_focus_state(shell, from, surface);

	if (shell->workspaces.anim_from == to &&
	    shell->workspaces.anim_to == from) {
		wl_list_remove(&to->layer.link);
		wl_list_insert(from->layer.link.prev, &to->layer.link);

		reverse_workspace_change_animation(shell, index, from, to);
		return;
	}

	if (shell->workspaces.anim_to != NULL)
		finish_workspace_change_animation(shell,
						  shell->workspaces.anim_from,
						  shell->workspaces.anim_to);

	if (workspace_is_empty(from) &&
	    workspace_has_only(to, surface))
		update_workspace(shell, index, from, to);
	else {
		if (shsurf != NULL &&
		    wl_list_empty(&shsurf->workspace_transform.link))
			wl_list_insert(&shell->workspaces.anim_sticky_list,
				       &shsurf->workspace_transform.link);

		animate_workspace_change(shell, index, from, to);
	}

	state = ensure_focus_state(shell, seat);
	if (state != NULL)
		focus_state_set_focus(state, surface);
}

static void noop_grab_focus(struct weston_pointer_grab *grab)
{
}

static void noop_grab_axis(struct weston_pointer_grab *grab,
			   uint32_t time,
			   struct weston_pointer_axis_event *event)
{
}

static void noop_grab_axis_source(struct weston_pointer_grab *grab,
				  uint32_t source)
{
}

static void noop_grab_frame(struct weston_pointer_grab *grab)
{
}


static void constrain_position(struct weston_move_grab *move, int *cx, int *cy)
{
	struct weston_pointer *pointer = move->base.grab.pointer;

	*cx = wl_fixed_to_int(pointer->x + move->dx);
	*cy = wl_fixed_to_int(pointer->y + move->dy);
}

static void move_grab_motion(struct weston_pointer_grab *grab, uint32_t time,
			     struct weston_pointer_motion_event *event)
{
	struct weston_move_grab *move = (struct weston_move_grab *) grab;
	struct weston_pointer *pointer = grab->pointer;
	struct shell_surface *shsurf = move->base.shsurf;
	int cx, cy;

	weston_pointer_move(pointer, event);
	if (!shsurf)
		return;

	constrain_position(move, &cx, &cy);

	weston_view_set_position(shsurf->view, cx, cy);

	weston_compositor_schedule_repaint(shsurf->surface->compositor);
}

static void
move_grab_button(struct weston_pointer_grab *grab,
		 uint32_t time, uint32_t button, uint32_t state_w)
{
	struct shell_grab *shell_grab = container_of(grab, struct shell_grab,
						    grab);
	struct weston_pointer *pointer = grab->pointer;
	enum wl_pointer_button_state state = state_w;

	if (pointer->button_count == 0 &&
	    state == WL_POINTER_BUTTON_STATE_RELEASED) {
		shell_grab_end(shell_grab);
		free(grab);
	}
}

static void
move_grab_cancel(struct weston_pointer_grab *grab)
{
	struct shell_grab *shell_grab =
		container_of(grab, struct shell_grab, grab);

	shell_grab_end(shell_grab);
	free(grab);
}

static const struct weston_pointer_grab_interface move_grab_interface = {
	noop_grab_focus,
	move_grab_motion,
	move_grab_button,
	noop_grab_axis,
	noop_grab_axis_source,
	noop_grab_frame,
	move_grab_cancel,
};

static int surface_move(struct shell_surface *shsurf, struct weston_pointer *pointer,
			int client_initiated)
{
	struct weston_move_grab *move;

	if (!shsurf)
		return -1;

	if (shsurf->grabbed ||
	    shsurf->state.fullscreen || shsurf->state.maximized)
		return 0;

	move = malloc(sizeof *move);
	if (!move)
		return -1;

	move->dx = wl_fixed_from_double(shsurf->view->geometry.x) -
			pointer->grab_x;
	move->dy = wl_fixed_from_double(shsurf->view->geometry.y) -
			pointer->grab_y;
	move->client_initiated = client_initiated;

	shell_grab_start(&move->base, &move_grab_interface, shsurf,
			 pointer, MS_MENU_CURSOR_MOVE);

	return 0;
}

static void
common_surface_move(struct wl_resource *resource,
		    struct wl_resource *seat_resource, uint32_t serial)
{
	struct weston_seat *seat = wl_resource_get_user_data(seat_resource);
	struct weston_pointer *pointer = weston_seat_get_pointer(seat);
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);
	struct weston_surface *surface;

	if (pointer &&
	    pointer->focus &&
	    pointer->button_count > 0 &&
	    pointer->grab_serial == serial) {
		surface = weston_surface_get_main_surface(pointer->focus->surface);
		if ((surface == shsurf->surface) &&
		    (surface_move(shsurf, pointer, 1) < 0))
			wl_resource_post_no_memory(resource);
	}
}

static void
shell_surface_move(struct wl_client *client, struct wl_resource *resource,
		   struct wl_resource *seat_resource, uint32_t serial)
{
	common_surface_move(resource, seat_resource, serial);
}

struct weston_resize_grab {
	struct shell_grab base;
	uint32_t edges;
	int32_t width, height;
};

static void
resize_grab_motion(struct weston_pointer_grab *grab, uint32_t time,
		   struct weston_pointer_motion_event *event)
{
	struct weston_resize_grab *resize = (struct weston_resize_grab *) grab;
	struct weston_pointer *pointer = grab->pointer;
	struct shell_surface *shsurf = resize->base.shsurf;
	int32_t width, height;
	wl_fixed_t from_x, from_y;
	wl_fixed_t to_x, to_y;

	weston_pointer_move(pointer, event);

	if (!shsurf)
		return;

	weston_view_from_global_fixed(shsurf->view,
				      pointer->grab_x, pointer->grab_y,
				      &from_x, &from_y);
	weston_view_from_global_fixed(shsurf->view,
				      pointer->x, pointer->y, &to_x, &to_y);

	width = resize->width;
	if (resize->edges & WL_SHELL_SURFACE_RESIZE_LEFT) {
		width += wl_fixed_to_int(from_x - to_x);
	} else if (resize->edges & WL_SHELL_SURFACE_RESIZE_RIGHT) {
		width += wl_fixed_to_int(to_x - from_x);
	}

	height = resize->height;
	if (resize->edges & WL_SHELL_SURFACE_RESIZE_TOP) {
		height += wl_fixed_to_int(from_y - to_y);
	} else if (resize->edges & WL_SHELL_SURFACE_RESIZE_BOTTOM) {
		height += wl_fixed_to_int(to_y - from_y);
	}

	if (width < 1)
		width = 1;
	if (height < 1)
		height = 1;
	shsurf->client->send_configure(shsurf->surface, width, height);
}

static void
send_configure(struct weston_surface *surface, int32_t width, int32_t height)
{
	struct shell_surface *shsurf = get_shell_surface(surface);

	assert(shsurf);

	if (shsurf->resource)
		wl_shell_surface_send_configure(shsurf->resource,
						shsurf->resize_edges,
						width, height);
}

static const struct weston_shell_client shell_client = {
	send_configure,
	NULL
};

static void
resize_grab_button(struct weston_pointer_grab *grab,
		   uint32_t time, uint32_t button, uint32_t state_w)
{
	struct weston_resize_grab *resize = (struct weston_resize_grab *) grab;
	struct weston_pointer *pointer = grab->pointer;
	enum wl_pointer_button_state state = state_w;

	if (pointer->button_count == 0 &&
	    state == WL_POINTER_BUTTON_STATE_RELEASED) {
		shell_grab_end(&resize->base);
		free(grab);
	}
}

static void
resize_grab_cancel(struct weston_pointer_grab *grab)
{
	struct weston_resize_grab *resize = (struct weston_resize_grab *) grab;

	shell_grab_end(&resize->base);
	free(grab);
}

static const struct weston_pointer_grab_interface resize_grab_interface = {
	noop_grab_focus,
	resize_grab_motion,
	resize_grab_button,
	noop_grab_axis,
	noop_grab_axis_source,
	noop_grab_frame,
	resize_grab_cancel,
};

/*
 * Returns the bounding box of a surface and all its sub-surfaces,
 * in the surface coordinates system. */
static void
surface_subsurfaces_boundingbox(struct weston_surface *surface, int32_t *x,
				int32_t *y, int32_t *w, int32_t *h) {
	pixman_region32_t region;
	pixman_box32_t *box;
	struct weston_subsurface *subsurface;

	pixman_region32_init_rect(&region, 0, 0,
	                          surface->width,
	                          surface->height);

	wl_list_for_each(subsurface, &surface->subsurface_list, parent_link) {
		pixman_region32_union_rect(&region, &region,
		                           subsurface->position.x,
		                           subsurface->position.y,
		                           subsurface->surface->width,
		                           subsurface->surface->height);
	}

	box = pixman_region32_extents(&region);
	if (x)
		*x = box->x1;
	if (y)
		*y = box->y1;
	if (w)
		*w = box->x2 - box->x1;
	if (h)
		*h = box->y2 - box->y1;

	pixman_region32_fini(&region);
}

static int
surface_resize(struct shell_surface *shsurf,
	       struct weston_pointer *pointer, uint32_t edges)
{
	struct weston_resize_grab *resize;
	const unsigned resize_topbottom =
		WL_SHELL_SURFACE_RESIZE_TOP | WL_SHELL_SURFACE_RESIZE_BOTTOM;
	const unsigned resize_leftright =
		WL_SHELL_SURFACE_RESIZE_LEFT | WL_SHELL_SURFACE_RESIZE_RIGHT;
	const unsigned resize_any = resize_topbottom | resize_leftright;

	if (shsurf->grabbed ||
	    shsurf->state.fullscreen || shsurf->state.maximized)
		return 0;

	/* Check for invalid edge combinations. */
	if (edges == WL_SHELL_SURFACE_RESIZE_NONE || edges > resize_any ||
	    (edges & resize_topbottom) == resize_topbottom ||
	    (edges & resize_leftright) == resize_leftright)
		return 0;

	resize = malloc(sizeof *resize);
	if (!resize)
		return -1;

	resize->edges = edges;

	resize->width = shsurf->geometry.width;
	resize->height = shsurf->geometry.height;

	shsurf->resize_edges = edges;
	shell_surface_state_changed(shsurf);
	shell_grab_start(&resize->base, &resize_grab_interface, shsurf,
			 pointer, edges);

	return 0;
}

static void
common_surface_resize(struct wl_resource *resource,
		      struct wl_resource *seat_resource, uint32_t serial,
		      uint32_t edges)
{
	struct weston_seat *seat = wl_resource_get_user_data(seat_resource);
	struct weston_pointer *pointer = weston_seat_get_pointer(seat);
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);
	struct weston_surface *surface;

	if (!pointer ||
	    pointer->button_count == 0 ||
	    pointer->grab_serial != serial ||
	    pointer->focus == NULL)
		return;

	surface = weston_surface_get_main_surface(pointer->focus->surface);
	if (surface != shsurf->surface)
		return;

	if (surface_resize(shsurf, pointer, edges) < 0)
		wl_resource_post_no_memory(resource);
}

static void
shell_surface_resize(struct wl_client *client, struct wl_resource *resource,
		     struct wl_resource *seat_resource, uint32_t serial,
		     uint32_t edges)
{
	common_surface_resize(resource, seat_resource, serial, edges);
}

static void busy_cursor_grab_focus(struct weston_pointer_grab *base)
{
	struct shell_grab *grab = (struct shell_grab *) base;
	struct weston_pointer *pointer = base->pointer;
	struct weston_view *view;
	wl_fixed_t sx, sy;

	view = weston_compositor_pick_view(pointer->seat->compositor,
					   pointer->x, pointer->y,
					   &sx, &sy);
	//FIXME: temporary fix
	if(!view) {
		shell_grab_end(grab);
		free(grab);
		return;
	}

	if(!grab->shsurf || grab->shsurf->surface != view->surface) {
		shell_grab_end(grab);
		free(grab);
	}
}

static void
busy_cursor_grab_motion(struct weston_pointer_grab *grab, uint32_t time,
			struct weston_pointer_motion_event *event)
{
	weston_pointer_move(grab->pointer, event);
}

static void
busy_cursor_grab_button(struct weston_pointer_grab *base,
			uint32_t time, uint32_t button, uint32_t state)
{
	struct shell_grab *grab = (struct shell_grab *) base;
	struct shell_surface *shsurf = grab->shsurf;
	struct weston_pointer *pointer = grab->grab.pointer;
	struct weston_seat *seat = pointer->seat;

	if (shsurf && button == BTN_LEFT && state) {
		activate(shsurf->shell, shsurf->surface, seat, true);
		surface_move(shsurf, pointer, 0);
	} else if (shsurf && button == BTN_RIGHT && state) {
		activate(shsurf->shell, shsurf->surface, seat, true);
		surface_rotate(shsurf, pointer);
	}
}

static void
busy_cursor_grab_cancel(struct weston_pointer_grab *base)
{
	struct shell_grab *grab = (struct shell_grab *) base;

	shell_grab_end(grab);
	free(grab);
}

static const struct weston_pointer_grab_interface busy_cursor_grab_interface = {
	busy_cursor_grab_focus,
	busy_cursor_grab_motion,
	busy_cursor_grab_button,
	noop_grab_axis,
	noop_grab_axis_source,
	noop_grab_frame,
	busy_cursor_grab_cancel,
};

static void
set_busy_cursor(struct shell_surface *shsurf, struct weston_pointer *pointer)
{
	struct shell_grab *grab;

	if (pointer->grab->interface == &busy_cursor_grab_interface)
		return;

	grab = malloc(sizeof *grab);
	if (!grab)
		return;

	shell_grab_start(grab, &busy_cursor_grab_interface, shsurf, pointer,
			 MS_MENU_CURSOR_BUSY);
	/* Mark the shsurf as ungrabbed so that button binding is able
	 * to move it. */
	shsurf->grabbed = 0;
}

static void end_busy_cursor(struct weston_compositor *compositor,
			    struct wl_client *client)
{
	struct shell_grab *grab;
	struct weston_seat *seat;

	wl_list_for_each(seat, &compositor->seat_list, link) {
		struct weston_pointer *pointer = weston_seat_get_pointer(seat);

		if (!pointer)
			continue;

		grab = (struct shell_grab *) pointer->grab;
		if (grab->grab.interface == &busy_cursor_grab_interface
		    && grab->shsurf->resource
		    && wl_resource_get_client(grab->shsurf->resource) == client)
		{
			shell_grab_end(grab);
			free(grab);
		}
	}
}

static void handle_shell_client_destroy(struct wl_listener *listener, void *data);

static int xdg_ping_timeout_handler(void *data)
{
	struct shell_client *sc = data;
	struct weston_seat *seat;
	struct shell_surface *shsurf;

	/* Client is not responding */
	sc->unresponsive = 1;
	wl_list_for_each(seat, &sc->shell->compositor->seat_list, link) {
		struct weston_pointer *pointer = weston_seat_get_pointer(seat);

		if (!pointer ||
		    !pointer->focus ||
		    !pointer->focus->surface->resource)
			continue;

		shsurf = get_shell_surface(pointer->focus->surface);
		if (shsurf && shsurf->resource &&
		    wl_resource_get_client(shsurf->resource) == sc->client)
			set_busy_cursor(shsurf, pointer);
	}

	return 1;
}

static void
handle_xdg_ping(struct shell_surface *shsurf, uint32_t serial)
{
	struct weston_compositor *compositor = shsurf->shell->compositor;
	struct shell_client *sc = shsurf->owner;
	struct wl_event_loop *loop;
	static const int ping_timeout = 200;

	if (sc->unresponsive) {
		xdg_ping_timeout_handler(sc);
		return;
	}

	sc->ping_serial = serial;
	loop = wl_display_get_event_loop(compositor->wl_display);
	if (sc->ping_timer == NULL)
		sc->ping_timer =
			wl_event_loop_add_timer(loop,
						xdg_ping_timeout_handler, sc);
	if (sc->ping_timer == NULL)
		return;

	wl_event_source_timer_update(sc->ping_timer, ping_timeout);

	if (shell_surface_is_xdg_surface(shsurf) ||
	    shell_surface_is_xdg_popup(shsurf))
		xdg_shell_send_ping(sc->resource, serial);
	else if (shell_surface_is_wl_shell_surface(shsurf) && shsurf->resource)
		wl_shell_surface_send_ping(shsurf->resource, serial);
}

static void
ping_handler(struct weston_surface *surface, uint32_t serial)
{
	struct shell_surface *shsurf = get_shell_surface(surface);

	if (!shsurf)
		return;
	if (!shsurf->resource)
		return;
	if (shsurf->surface == shsurf->shell->grab_surface)
		return;

	handle_xdg_ping(shsurf, serial);
}

static void
handle_pointer_focus(struct wl_listener *listener, void *data)
{
	struct weston_pointer *pointer = data;
	struct weston_view *view = pointer->focus;
	struct weston_compositor *compositor;
	uint32_t serial;

	if (!view)
		return;

	compositor = view->surface->compositor;
	serial = wl_display_next_serial(compositor->wl_display);
	ping_handler(view->surface, serial);
}

static void
shell_surface_lose_keyboard_focus(struct shell_surface *shsurf)
{
	if (--shsurf->focus_count == 0)
		shell_surface_state_changed(shsurf);
}

static void
shell_surface_gain_keyboard_focus(struct shell_surface *shsurf)
{
	if (shsurf->focus_count++ == 0)
		shell_surface_state_changed(shsurf);
}

static void
handle_keyboard_focus(struct wl_listener *listener, void *data)
{
	struct weston_keyboard *keyboard = data;
	struct shell_seat *seat = get_shell_seat(keyboard->seat);

	if (seat->focused_surface) {
		struct shell_surface *shsurf = get_shell_surface(seat->focused_surface);
		if (shsurf)
			shell_surface_lose_keyboard_focus(shsurf);
	}

	seat->focused_surface = keyboard->focus;

	if (seat->focused_surface) {
		struct shell_surface *shsurf = get_shell_surface(seat->focused_surface);
		if (shsurf)
			shell_surface_gain_keyboard_focus(shsurf);
	}
}

static void
shell_client_pong(struct shell_client *sc, uint32_t serial)
{
	if (sc->ping_serial != serial)
		return;

	sc->unresponsive = 0;
	end_busy_cursor(sc->shell->compositor, sc->client);

	if (sc->ping_timer) {
		wl_event_source_remove(sc->ping_timer);
		sc->ping_timer = NULL;
	}

}

static void
shell_surface_pong(struct wl_client *client,
		   struct wl_resource *resource, uint32_t serial)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);
	struct shell_client *sc = shsurf->owner;

	shell_client_pong(sc, serial);
}

static void
set_title(struct shell_surface *shsurf, const char *title)
{
	free(shsurf->title);
	shsurf->title = strdup(title);
	shsurf->surface->timeline.force_refresh = 1;
}

static void
set_pid(struct shell_surface *shsurf, pid_t pid)
{
	/* We have no use for it */
}

static void
set_type(struct shell_surface *shsurf, enum shell_surface_type t)
{
	shsurf->type = t;
	shsurf->surface->timeline.force_refresh = 1;
}

static void
set_window_geometry(struct shell_surface *shsurf,
		    int32_t x, int32_t y, int32_t width, int32_t height)
{
	shsurf->next_geometry.x = x;
	shsurf->next_geometry.y = y;
	shsurf->next_geometry.width = width;
	shsurf->next_geometry.height = height;
	shsurf->has_next_geometry = true;
}

static void
shell_surface_set_title(struct wl_client *client,
			struct wl_resource *resource, const char *title)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);

	set_title(shsurf, title);
}

static void
shell_surface_set_class(struct wl_client *client,
			struct wl_resource *resource, const char *class)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);

	free(shsurf->class);
	shsurf->class = strdup(class);
	shsurf->surface->timeline.force_refresh = 1;
}

static void
restore_output_mode(struct weston_output *output)
{
	if (output->original_mode)
		weston_output_mode_switch_to_native(output);
}

static void
restore_all_output_modes(struct weston_compositor *compositor)
{
	struct weston_output *output;

	wl_list_for_each(output, &compositor->output_list, link)
		restore_output_mode(output);
}

/* The surface will be inserted into the list immediately after the link
 * returned by this function (i.e. will be stacked immediately above the
 * returned link). */
static struct weston_layer_entry *shell_surface_calculate_layer_link(struct shell_surface *shsurf)
{
	struct workspace *ws;
	struct weston_view *parent;

	switch (shsurf->type) {
	//case SHELL_SURFACE_XWAYLAND:/*used to be fullscreen, not sure if this will break */
		//return &shsurf->shell->compositor->cursor_layer.view_list;

	case SHELL_SURFACE_NONE:
		return NULL;

	case SHELL_SURFACE_MENU:
	case SHELL_SURFACE_XWAYLAND:
	case SHELL_SURFACE_POPUP:
	case SHELL_SURFACE_TOPLEVEL:
		if (shsurf->parent) {
			/* Move the surface to its parent layer so
			 * that surfaces which are transient for
			 * fullscreen surfaces don't get hidden by the
			 * fullscreen surfaces. */

			/* TODO: Handle a parent with multiple views */
			parent = get_default_view(shsurf->parent);
			if (parent)
				return container_of(parent->layer_link.link.prev,
						    struct weston_layer_entry, link);
		}

		/* Move the surface to a normal workspace layer so that surfaces
		 * which were previously fullscreen or transient are no longer
		 * rendered on top. */
		ws = get_current_workspace(shsurf->shell);
		return &ws->layer.view_list;
	}

	assert(0 && "Unknown shell surface type");
}

static void
shell_surface_update_child_surface_layers (struct shell_surface *shsurf)
{
	struct shell_surface *child;
	struct weston_layer_entry *prev;

	/* Move the child layers to the same workspace as shsurf. They will be
	 * stacked above shsurf. */
	wl_list_for_each_reverse(child, &shsurf->children_list, children_link) {
		if (shsurf->view->layer_link.link.prev != &child->view->layer_link.link) {
			weston_view_damage_below(child->view);
			weston_view_geometry_dirty(child->view);
			prev = container_of(shsurf->view->layer_link.link.prev,
					    struct weston_layer_entry, link);
			weston_layer_entry_remove(&child->view->layer_link);
			weston_layer_entry_insert(prev,
						  &child->view->layer_link);
			weston_view_geometry_dirty(child->view);
			weston_surface_damage(child->surface);

			/* Recurse. We don’t expect this to recurse very far (if
			 * at all) because that would imply we have transient
			 * (or popup) children of transient surfaces, which
			 * would be unusual. */
			shell_surface_update_child_surface_layers(child);
		}
	}
}

/* Update the surface’s layer. Mark both the old and new views as having dirty
 * geometry to ensure the changes are redrawn.
 *
 * If any child surfaces exist and are mapped, ensure they’re in the same layer
 * as this surface. */
static void shell_surface_update_layer(struct shell_surface *shsurf)
{
	struct weston_layer_entry *new_layer_link;

	new_layer_link = shell_surface_calculate_layer_link(shsurf);

	if (new_layer_link == NULL)
		return;
	if (new_layer_link == &shsurf->view->layer_link)
		return;

	weston_view_geometry_dirty(shsurf->view);
	weston_layer_entry_remove(&shsurf->view->layer_link);
	weston_layer_entry_insert(new_layer_link, &shsurf->view->layer_link);
	weston_view_geometry_dirty(shsurf->view);
	weston_surface_damage(shsurf->surface);

	shell_surface_update_child_surface_layers(shsurf);
}

static void
shell_surface_set_parent(struct shell_surface *shsurf,
                         struct weston_surface *parent)
{
	shsurf->parent = parent;

	wl_list_remove(&shsurf->children_link);
	wl_list_init(&shsurf->children_link);

	/* Insert into the parent surface’s child list. */
	if (parent != NULL) {
		struct shell_surface *parent_shsurf = get_shell_surface(parent);
		if (parent_shsurf != NULL)
			wl_list_insert(&parent_shsurf->children_list,
			               &shsurf->children_link);
	}
}

static void
shell_surface_set_output(struct shell_surface *shsurf,
                         struct weston_output *output)
{
	struct weston_surface *es = shsurf->surface;

	/* get the default output, if the client set it as NULL
	   check whether the ouput is available */
	if (output)
		shsurf->output = output;
	else if (es->output)
		shsurf->output = es->output;
	else
		shsurf->output = get_default_output(es->compositor);
}

static void surface_clear_next_states(struct shell_surface *shsurf)
{
	shsurf->next_state.maximized = false;
	shsurf->next_state.fullscreen = false;

	if ((shsurf->next_state.maximized != shsurf->state.maximized) ||
	    (shsurf->next_state.fullscreen != shsurf->state.fullscreen))
		shsurf->state_changed = true;
}

static void set_toplevel(struct shell_surface *shsurf)
{
	shell_surface_set_parent(shsurf, NULL);
	surface_clear_next_states(shsurf);
	set_type(shsurf, SHELL_SURFACE_TOPLEVEL);

	/* The layer_link is updated in set_surface_type(),
	 * called from configure. */
}

static void
shell_surface_set_toplevel(struct wl_client *client,
			   struct wl_resource *resource)
{
	struct shell_surface *surface = wl_resource_get_user_data(resource);

	set_toplevel(surface);
}

static void
set_transient(struct shell_surface *shsurf,
	      struct weston_surface *parent, int x, int y, uint32_t flags)
{
	assert(parent != NULL);

	shell_surface_set_parent(shsurf, parent);

	surface_clear_next_states(shsurf);

	shsurf->transient.x = x;
	shsurf->transient.y = y;
	shsurf->transient.flags = flags;

	shsurf->next_state.relative = true;
	shsurf->state_changed = true;
	set_type(shsurf, SHELL_SURFACE_TOPLEVEL);

	/* The layer_link is updated in set_surface_type(),
	 * called from configure. */
}

static void
shell_surface_set_transient(struct wl_client *client,
			    struct wl_resource *resource,
			    struct wl_resource *parent_resource,
			    int x, int y, uint32_t flags)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);
	struct weston_surface *parent =
		wl_resource_get_user_data(parent_resource);

	set_transient(shsurf, parent, x, y, flags);
}

static void
set_fullscreen(struct shell_surface *shsurf,
	       uint32_t method,
	       uint32_t framerate,
	       struct weston_output *output)
{
	shell_surface_set_output(shsurf, output);
	set_type(shsurf, SHELL_SURFACE_TOPLEVEL);

	shsurf->fullscreen_output = shsurf->output;
	shsurf->fullscreen.type = method;
	shsurf->fullscreen.framerate = framerate;

	send_configure_for_surface(shsurf);
}

static void
weston_view_set_initial_position(struct weston_view *view,
				 struct mayhem_shell *shell);

static void
unset_fullscreen(struct shell_surface *shsurf)
{
	/* Unset the fullscreen output, driver configuration and transforms. */
	if(shsurf->fullscreen.type == WL_SHELL_SURFACE_FULLSCREEN_METHOD_DRIVER &&
	    shell_surface_is_top_fullscreen(shsurf)) {
		restore_output_mode(shsurf->fullscreen_output);
	}

	shsurf->fullscreen.type = WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT;
	shsurf->fullscreen.framerate = 0;

	wl_list_remove(&shsurf->fullscreen.transform.link);
	wl_list_init(&shsurf->fullscreen.transform.link);

	if(shsurf->saved_position_valid)
		weston_view_set_position(shsurf->view,
					 shsurf->saved_x, shsurf->saved_y);
	else
		weston_view_set_initial_position(shsurf->view, shsurf->shell);

	if(shsurf->saved_rotation_valid) {
		wl_list_insert(&shsurf->view->geometry.transformation_list,
		               &shsurf->rotation.transform.link);
		shsurf->saved_rotation_valid = false;
	}

	/* Layer is updated in set_surface_type(). */
}

static void
shell_surface_set_fullscreen(struct wl_client *client,
			     struct wl_resource *resource,
			     uint32_t method,
			     uint32_t framerate,
			     struct wl_resource *output_resource)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);
	struct weston_output *output;

	if (output_resource)
		output = wl_resource_get_user_data(output_resource);
	else
		output = NULL;

	shell_surface_set_parent(shsurf, NULL);

	surface_clear_next_states(shsurf);
	shsurf->next_state.fullscreen = true;
	shsurf->state_changed = true;
	set_fullscreen(shsurf, method, framerate, output);
}

static void
set_popup(struct shell_surface *shsurf,
          struct weston_surface *parent,
          struct weston_seat *seat,
          uint32_t serial,
          int32_t x,
          int32_t y)
{
	assert(parent != NULL);

	shsurf->popup.shseat = get_shell_seat(seat);
	shsurf->popup.serial = serial;
	shsurf->popup.x = x;
	shsurf->popup.y = y;

	set_type(shsurf, SHELL_SURFACE_POPUP);
}

static void
shell_surface_set_popup(struct wl_client *client,
			struct wl_resource *resource,
			struct wl_resource *seat_resource,
			uint32_t serial,
			struct wl_resource *parent_resource,
			int32_t x, int32_t y, uint32_t flags)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);
	struct weston_surface *parent =
		wl_resource_get_user_data(parent_resource);

	shell_surface_set_parent(shsurf, parent);

	surface_clear_next_states(shsurf);
	set_popup(shsurf,
	          parent,
	          wl_resource_get_user_data(seat_resource),
	          serial, x, y);
}

static void unset_maximized(struct shell_surface *shsurf)
{
	/* undo all maximized things here */
	shsurf->output = get_default_output(shsurf->surface->compositor);

	if (shsurf->saved_position_valid)
		weston_view_set_position(shsurf->view,
					 shsurf->saved_x, shsurf->saved_y);
	else
		weston_view_set_initial_position(shsurf->view, shsurf->shell);

	if (shsurf->saved_rotation_valid) {
		wl_list_insert(&shsurf->view->geometry.transformation_list,
			       &shsurf->rotation.transform.link);
		shsurf->saved_rotation_valid = false;
	}

	/* Layer is updated in set_surface_type(). */
}

static void set_minimized(struct weston_surface *surface)
{
	struct shell_surface *shsurf;
	struct workspace *current_ws;
	struct weston_view *view;

	view = get_default_view(surface);
	if (!view)
		return;

	assert(weston_surface_get_main_surface(view->surface) == view->surface);

	shsurf = get_shell_surface(surface);
	current_ws = get_current_workspace(shsurf->shell);

	weston_layer_entry_remove(&view->layer_link);
	weston_layer_entry_insert(&shsurf->shell->minimized_layer.view_list, &view->layer_link);

	drop_focus_state(shsurf->shell, current_ws, view->surface);
	surface_keyboard_focus_lost(surface);

	shell_surface_update_child_surface_layers(shsurf);
	weston_view_damage_below(view);
}

static void
shell_surface_set_maximized(struct wl_client *client,
                            struct wl_resource *resource,
                            struct wl_resource *output_resource)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);
	struct weston_output *output;

	surface_clear_next_states(shsurf);
	shsurf->next_state.maximized = true;
	shsurf->state_changed = true;

	set_type(shsurf, SHELL_SURFACE_TOPLEVEL);
	shell_surface_set_parent(shsurf, NULL);

	if (output_resource)
		output = wl_resource_get_user_data(output_resource);
	else
		output = NULL;

	shell_surface_set_output(shsurf, output);

	send_configure_for_surface(shsurf);
}

/* This is only ever called from set_surface_type(), so there’s no need to
 * update layer_links here, since they’ll be updated when we return. */
static int reset_surface_type(struct shell_surface *surface)
{
	if (surface->state.fullscreen)
		unset_fullscreen(surface);
	if (surface->state.maximized)
		unset_maximized(surface);

	return 0;
}

static void set_full_output(struct shell_surface *shsurf)
{
	shsurf->saved_x = shsurf->view->geometry.x;
	shsurf->saved_y = shsurf->view->geometry.y;
	shsurf->saved_width = shsurf->surface->width;
	shsurf->saved_height = shsurf->surface->height;
	shsurf->saved_size_valid = true;
	shsurf->saved_position_valid = true;

	if (!wl_list_empty(&shsurf->rotation.transform.link)) {
		wl_list_remove(&shsurf->rotation.transform.link);
		wl_list_init(&shsurf->rotation.transform.link);
		weston_view_geometry_dirty(shsurf->view);
		shsurf->saved_rotation_valid = true;
	}
}

static void set_surface_type(struct shell_surface *shsurf)
{
	struct weston_surface *pes = shsurf->parent;
	struct weston_view *pev = get_default_view(pes);

	reset_surface_type(shsurf);

	shsurf->state = shsurf->next_state;
	shsurf->state_changed = false;

	switch (shsurf->type) {
	case SHELL_SURFACE_TOPLEVEL:
		if (shsurf->state.maximized || shsurf->state.fullscreen) {
			set_full_output(shsurf);
		} else if (shsurf->state.relative && pev) {
			weston_view_set_position(shsurf->view,
						 pev->geometry.x + shsurf->transient.x,
						 pev->geometry.y + shsurf->transient.y);
		}
		break;

	case SHELL_SURFACE_MENU:
	case SHELL_SURFACE_XWAYLAND:
		weston_view_set_position(shsurf->view, shsurf->transient.x,
					 shsurf->transient.y);
		break;

	case SHELL_SURFACE_POPUP:
	case SHELL_SURFACE_NONE:
	default:
		break;
	}

	/* Update the surface’s layer. */
	shell_surface_update_layer(shsurf);
}

/* Create black surface and append it to the associated fullscreen surface.
 * Handle size dismatch and positioning according to the method. */
static void shell_configure_fullscreen(struct shell_surface *shsurf)
{
	struct weston_output *output = shsurf->fullscreen_output;
	struct weston_surface *surface = shsurf->surface;
	struct weston_matrix *matrix;
	float scale, output_aspect, surface_aspect, x, y;
	int32_t surf_x, surf_y, surf_width, surf_height;

	if(shsurf->fullscreen.type != WL_SHELL_SURFACE_FULLSCREEN_METHOD_DRIVER)
		restore_output_mode(output);

	/* Reverse the effect of lower_fullscreen_layer() */
	//weston_layer_entry_remove(&shsurf->view->layer_link);
	//weston_layer_entry_insert(&shsurf->shell->fullscreen_layer.view_list,
	//			  &shsurf->view->layer_link);


	surface_subsurfaces_boundingbox(shsurf->surface, &surf_x, &surf_y,
	                                &surf_width, &surf_height);

	switch (shsurf->fullscreen.type) {
	case WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT:
		if (surface->buffer_ref.buffer)
			center_on_output(shsurf->view, shsurf->fullscreen_output);
		break;
	case WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE:
		/* 1:1 mapping between surface and output dimensions */
		if (output->width == surf_width &&
			output->height == surf_height) {
			weston_view_set_position(shsurf->view,
						 output->x - surf_x,
						 output->y - surf_y);
			break;
		}

		matrix = &shsurf->fullscreen.transform.matrix;
		weston_matrix_init(matrix);

		output_aspect = (float) output->width /
			(float) output->height;
		/* XXX: Use surf_width and surf_height here? */
		surface_aspect = (float) surface->width /
			(float) surface->height;
		if (output_aspect < surface_aspect)
			scale = (float) output->width /
				(float) surf_width;
		else
			scale = (float) output->height /
				(float) surf_height;

		weston_matrix_scale(matrix, scale, scale, 1);
		wl_list_remove(&shsurf->fullscreen.transform.link);
		wl_list_insert(&shsurf->view->geometry.transformation_list,
			       &shsurf->fullscreen.transform.link);
		x = output->x + (output->width - surf_width * scale) / 2 - surf_x;
		y = output->y + (output->height - surf_height * scale) / 2 - surf_y;
		weston_view_set_position(shsurf->view, x, y);

		break;
	case WL_SHELL_SURFACE_FULLSCREEN_METHOD_DRIVER:
		if (shell_surface_is_top_fullscreen(shsurf)) {
			struct weston_mode mode = {0,
				surf_width * surface->buffer_viewport.buffer.scale,
				surf_height * surface->buffer_viewport.buffer.scale,
				shsurf->fullscreen.framerate};

			if (weston_output_mode_switch_to_temporary(output, &mode,
					surface->buffer_viewport.buffer.scale) == 0) {
				weston_view_set_position(shsurf->view,
							 output->x - surf_x,
							 output->y - surf_y);
				break;
			} else {
				restore_output_mode(output);
				center_on_output(shsurf->view, output);
			}
		}
		break;
	case WL_SHELL_SURFACE_FULLSCREEN_METHOD_FILL:
		center_on_output(shsurf->view, output);
		break;
	default:
		break;
	}
}

static void
shell_map_fullscreen(struct shell_surface *shsurf)
{
	shell_configure_fullscreen(shsurf);
}

static void
set_xwayland(struct shell_surface *shsurf, int x, int y, uint32_t flags)
{
	/* XXX: using the same fields for transient type */
	surface_clear_next_states(shsurf);
	shsurf->transient.x = x;
	shsurf->transient.y = y;
	shsurf->transient.flags = flags;

	shell_surface_set_parent(shsurf, NULL);

	set_type(shsurf, SHELL_SURFACE_XWAYLAND);
	shsurf->state_changed = true;
}

static void
shell_interface_set_fullscreen(struct shell_surface *shsurf,
			       uint32_t method,
			       uint32_t framerate,
			       struct weston_output *output)
{
	surface_clear_next_states(shsurf);
	shsurf->next_state.fullscreen = true;
	shsurf->state_changed = true;

	set_fullscreen(shsurf, method, framerate, output);
}

static struct weston_output *
get_focused_output(struct weston_compositor *compositor)
{
	struct weston_seat *seat;
	struct weston_output *output = NULL;

	wl_list_for_each(seat, &compositor->seat_list, link) {
		struct weston_pointer *pointer = weston_seat_get_pointer(seat);
		struct weston_keyboard *keyboard = weston_seat_get_keyboard(seat);

		/* Priority has touch focus, then pointer and
		 * then keyboard focus. We should probably have
		 * three for loops and check frist for touch,
		 * then for pointer, etc. but unless somebody has some
		 * objections, I think this is sufficient. */
		if (pointer && pointer->focus)
			output = pointer->focus->output;
		else if (keyboard && keyboard->focus)
			output = keyboard->focus->output;

		if (output)
			break;
	}

	return output;
}

static void shell_interface_set_maximized(struct shell_surface *shsurf)
{
	struct weston_output *output;

	surface_clear_next_states(shsurf);
	shsurf->next_state.maximized = true;
	shsurf->state_changed = true;
	shsurf->type = SHELL_SURFACE_TOPLEVEL;

	if (!weston_surface_is_mapped(shsurf->surface))
		output = get_focused_output(shsurf->surface->compositor);
	else
		output = shsurf->surface->output;

	shell_surface_set_output(shsurf, output);
	send_configure_for_surface(shsurf);
}

static int shell_interface_move(struct shell_surface *shsurf,
				struct weston_pointer *pointer)
{
	return surface_move(shsurf, pointer, 1);
}

static int shell_interface_resize(struct shell_surface *shsurf,
				  struct weston_pointer *pointer, uint32_t edges)
{
	return surface_resize(shsurf, pointer, edges);
}


static const struct weston_pointer_grab_interface popup_grab_interface;

static void
destroy_shell_seat(struct wl_listener *listener, void *data)
{
	struct shell_seat *shseat =
		container_of(listener,
			     struct shell_seat, seat_destroy_listener);
	struct shell_surface *shsurf, *next;

	if (shseat->popup_grab.grab.interface == &popup_grab_interface) {
		weston_pointer_end_grab(shseat->popup_grab.grab.pointer);
		shseat->popup_grab.client = NULL;

		wl_list_for_each_safe(shsurf, next,
				      &shseat->popup_grab.surfaces_list,
				      popup.grab_link) {
			shsurf->popup.shseat = NULL;
			wl_list_init(&shsurf->popup.grab_link);
		}
	}

	wl_list_remove(&shseat->seat_destroy_listener.link);
	free(shseat);
}

static void
shell_seat_caps_changed(struct wl_listener *listener, void *data)
{
	struct weston_keyboard *keyboard;
	struct weston_pointer *pointer;
	struct shell_seat *seat;

	seat = container_of(listener, struct shell_seat, caps_changed_listener);
	keyboard = weston_seat_get_keyboard(seat->seat);
	pointer = weston_seat_get_pointer(seat->seat);

	if (keyboard && wl_list_empty(&seat->keyboard_focus_listener.link)) {
		wl_signal_add(&keyboard->focus_signal,
			      &seat->keyboard_focus_listener);
	} else if (!keyboard) {
		wl_list_remove(&seat->keyboard_focus_listener.link);
		wl_list_init(&seat->keyboard_focus_listener.link);
	}

	if (pointer && wl_list_empty(&seat->pointer_focus_listener.link)) {
		wl_signal_add(&pointer->focus_signal,
			      &seat->pointer_focus_listener);
	} else if (!pointer) {
		wl_list_remove(&seat->pointer_focus_listener.link);
		wl_list_init(&seat->pointer_focus_listener.link);
	}
}

static struct shell_seat *
create_shell_seat(struct weston_seat *seat)
{
	struct shell_seat *shseat;

	shseat = calloc(1, sizeof *shseat);
	if (!shseat) {
		weston_log("no memory to allocate shell seat\n");
		return NULL;
	}

	shseat->seat = seat;
	wl_list_init(&shseat->popup_grab.surfaces_list);

	shseat->seat_destroy_listener.notify = destroy_shell_seat;
	wl_signal_add(&seat->destroy_signal,
	              &shseat->seat_destroy_listener);

	shseat->keyboard_focus_listener.notify = handle_keyboard_focus;
	wl_list_init(&shseat->keyboard_focus_listener.link);

	shseat->pointer_focus_listener.notify = handle_pointer_focus;
	wl_list_init(&shseat->pointer_focus_listener.link);

	shseat->caps_changed_listener.notify = shell_seat_caps_changed;
	wl_signal_add(&seat->updated_caps_signal,
		      &shseat->caps_changed_listener);
	shell_seat_caps_changed(&shseat->caps_changed_listener, NULL);

	return shseat;
}

static struct shell_seat *
get_shell_seat(struct weston_seat *seat)
{
	struct wl_listener *listener;

	listener = wl_signal_get(&seat->destroy_signal, destroy_shell_seat);
	assert(listener != NULL);

	return container_of(listener,
			    struct shell_seat, seat_destroy_listener);
}

static void
popup_grab_focus(struct weston_pointer_grab *grab)
{
	struct weston_pointer *pointer = grab->pointer;
	struct weston_view *view;
	struct shell_seat *shseat =
	    container_of(grab, struct shell_seat, popup_grab.grab);
	struct wl_client *client = shseat->popup_grab.client;
	wl_fixed_t sx, sy;

	view = weston_compositor_pick_view(pointer->seat->compositor,
					   pointer->x, pointer->y,
					   &sx, &sy);

	if (view && view->surface->resource &&
	    wl_resource_get_client(view->surface->resource) == client) {
		weston_pointer_set_focus(pointer, view, sx, sy);
	} else {
		weston_pointer_clear_focus(pointer);
	}
}

static void
popup_grab_motion(struct weston_pointer_grab *grab, uint32_t time,
		  struct weston_pointer_motion_event *event)
{
	struct weston_pointer *pointer = grab->pointer;
	struct wl_resource *resource;
	struct wl_list *resource_list;
	wl_fixed_t x, y;
	wl_fixed_t sx, sy;

	if (pointer->focus) {
		weston_pointer_motion_to_abs(pointer, event, &x, &y);
		weston_view_from_global_fixed(pointer->focus, x, y,
					      &pointer->sx, &pointer->sy);
	}

	weston_pointer_move(pointer, event);

	if (!pointer->focus_client)
		return;

	resource_list = &pointer->focus_client->pointer_resources;
	wl_resource_for_each(resource, resource_list) {
		weston_view_from_global_fixed(pointer->focus,
					      pointer->x, pointer->y,
					      &sx, &sy);
		wl_pointer_send_motion(resource, time, sx, sy);
	}
}

static void
popup_grab_button(struct weston_pointer_grab *grab,
		  uint32_t time, uint32_t button, uint32_t state_w)
{
	struct wl_resource *resource;
	struct shell_seat *shseat =
	    container_of(grab, struct shell_seat, popup_grab.grab);
	struct wl_display *display = shseat->seat->compositor->wl_display;
	enum wl_pointer_button_state state = state_w;
	uint32_t serial;
	struct wl_list *resource_list = NULL;

	if (grab->pointer->focus_client)
		resource_list = &grab->pointer->focus_client->pointer_resources;
	if (resource_list && !wl_list_empty(resource_list)) {
		serial = wl_display_get_serial(display);
		wl_resource_for_each(resource, resource_list) {
			wl_pointer_send_button(resource, serial,
					       time, button, state);
		}
	} else if (state == WL_POINTER_BUTTON_STATE_RELEASED &&
		   (shseat->popup_grab.initial_up ||
		    time - grab->pointer->grab_time > 500)) {
		popup_grab_end(grab->pointer);
	}

	if (state == WL_POINTER_BUTTON_STATE_RELEASED)
		shseat->popup_grab.initial_up = 1;
}

static void
popup_grab_axis(struct weston_pointer_grab *grab,
		uint32_t time, struct weston_pointer_axis_event *event)
{
	weston_pointer_send_axis(grab->pointer, time, event);
}

static void popup_grab_axis_source(struct weston_pointer_grab *grab,
				   uint32_t source)
{
	weston_pointer_send_axis_source(grab->pointer, source);
}

static void popup_grab_frame(struct weston_pointer_grab *grab)
{
	weston_pointer_send_frame(grab->pointer);
}

static void
popup_grab_cancel(struct weston_pointer_grab *grab)
{
	popup_grab_end(grab->pointer);
}

static const struct weston_pointer_grab_interface popup_grab_interface = {
	popup_grab_focus,
	popup_grab_motion,
	popup_grab_button,
	popup_grab_axis,
	popup_grab_axis_source,
	popup_grab_frame,
	popup_grab_cancel,
};

static void
shell_surface_send_popup_done(struct shell_surface *shsurf)
{
	if (shsurf->resource == NULL)
		return;

	if (shell_surface_is_wl_shell_surface(shsurf))
		wl_shell_surface_send_popup_done(shsurf->resource);
	else if (shell_surface_is_xdg_popup(shsurf))
		xdg_popup_send_popup_done(shsurf->resource);
}

static void
popup_grab_end(struct weston_pointer *pointer)
{
	struct weston_pointer_grab *grab = pointer->grab;
	struct shell_seat *shseat =
	    container_of(grab, struct shell_seat, popup_grab.grab);
	struct shell_surface *shsurf;
	struct shell_surface *next;

	if (pointer->grab->interface == &popup_grab_interface) {
		weston_pointer_end_grab(grab->pointer);
		shseat->popup_grab.client = NULL;
		shseat->popup_grab.grab.interface = NULL;
		assert(!wl_list_empty(&shseat->popup_grab.surfaces_list));
		/* Send the popup_done event to all the popups open */
		wl_list_for_each_safe(shsurf, next,
				      &shseat->popup_grab.surfaces_list,
				      popup.grab_link) {
			shell_surface_send_popup_done(shsurf);
			shsurf->popup.shseat = NULL;
			wl_list_init(&shsurf->popup.grab_link);
		}
		wl_list_init(&shseat->popup_grab.surfaces_list);
	}
}

static struct shell_surface *
get_top_popup(struct shell_seat *shseat)
{
	struct shell_surface *shsurf;

	if (wl_list_empty(&shseat->popup_grab.surfaces_list)) {
		return NULL;
	} else {
		shsurf = container_of(shseat->popup_grab.surfaces_list.next,
				      struct shell_surface,
				      popup.grab_link);
		return shsurf;
	}
}

static int
add_popup_grab(struct shell_surface *shsurf,
	       struct shell_seat *shseat,
	       int32_t type)
{
	struct weston_seat *seat = shseat->seat;
	struct shell_surface *parent, *top_surface;
	struct weston_pointer *pointer = weston_seat_get_pointer(seat);

	parent = get_shell_surface(shsurf->parent);
	top_surface = get_top_popup(shseat);
	if (shell_surface_is_xdg_popup(shsurf) &&
	    ((top_surface == NULL && !shell_surface_is_xdg_surface(parent)) ||
	     (top_surface != NULL && parent != top_surface))) {
		wl_resource_post_error(shsurf->owner->resource,
				       XDG_SHELL_ERROR_NOT_THE_TOPMOST_POPUP,
				       "xdg_popup was not created on the "
				       "topmost popup");
		return -1;
	}

	if (wl_list_empty(&shseat->popup_grab.surfaces_list)) {
		shseat->popup_grab.type = type;
		shseat->popup_grab.client =
			wl_resource_get_client(shsurf->resource);

		if (type == POINTER) {
			shseat->popup_grab.grab.interface =
				&popup_grab_interface;

			/* We must make sure here that this popup was opened
			 * after a mouse press, and not just by moving around
			 * with other popups already open. */
			if (pointer->button_count > 0)
				shseat->popup_grab.initial_up = 0;
		}

		wl_list_insert(&shseat->popup_grab.surfaces_list,
			       &shsurf->popup.grab_link);

		if (type == POINTER) {
			weston_pointer_start_grab(pointer,
						  &shseat->popup_grab.grab);
		}
	} else {
		wl_list_insert(&shseat->popup_grab.surfaces_list,
			       &shsurf->popup.grab_link);
	}

	return 0;
}

static void
remove_popup_grab(struct shell_surface *shsurf)
{
	struct shell_seat *shseat = shsurf->popup.shseat;

	if (shell_surface_is_xdg_popup(shsurf) &&
	    get_top_popup(shseat) != shsurf) {
		wl_resource_post_error(shsurf->resource,
				       XDG_SHELL_ERROR_NOT_THE_TOPMOST_POPUP,
				       "xdg_popup was destroyed while it was "
				       "not the topmost popup.");
		return;
	}

	wl_list_remove(&shsurf->popup.grab_link);
	wl_list_init(&shsurf->popup.grab_link);
	if (wl_list_empty(&shseat->popup_grab.surfaces_list)) {
		if (shseat->popup_grab.type == POINTER) {
			weston_pointer_end_grab(shseat->popup_grab.grab.pointer);
			shseat->popup_grab.grab.interface = NULL;
		}
	}
}

static int
shell_map_popup(struct shell_surface *shsurf)
{
	struct shell_seat *shseat = shsurf->popup.shseat;
	struct weston_view *parent_view = get_default_view(shsurf->parent);
	struct weston_pointer *pointer = weston_seat_get_pointer(shseat->seat);

	shsurf->surface->output = parent_view->output;
	shsurf->view->output = parent_view->output;

	weston_view_set_transform_parent(shsurf->view, parent_view);
	weston_view_set_position(shsurf->view, shsurf->popup.x, shsurf->popup.y);
	weston_view_update_transform(shsurf->view);

	if (pointer && pointer->grab_serial == shsurf->popup.serial) {
		if (add_popup_grab(shsurf, shseat, POINTER) != 0)
			return -1;
	} else {
		shell_surface_send_popup_done(shsurf);
		shseat->popup_grab.client = NULL;
	}

	return 0;
}

static const struct wl_shell_surface_interface shell_surface_implementation = {
	shell_surface_pong,
	shell_surface_move,
	shell_surface_resize,
	shell_surface_set_toplevel,
	shell_surface_set_transient,
	shell_surface_set_fullscreen,
	shell_surface_set_popup,
	shell_surface_set_maximized,
	shell_surface_set_title,
	shell_surface_set_class
};

static void
destroy_shell_surface(struct shell_surface *shsurf)
{
	struct shell_surface *child, *next;

	wl_signal_emit(&shsurf->destroy_signal, shsurf);

	if (!wl_list_empty(&shsurf->popup.grab_link)) {
		remove_popup_grab(shsurf);
	}

	if (shsurf->fullscreen.type == WL_SHELL_SURFACE_FULLSCREEN_METHOD_DRIVER &&
	    shell_surface_is_top_fullscreen(shsurf))
		restore_output_mode (shsurf->fullscreen_output);

	/* As destroy_resource() use wl_list_for_each_safe(),
	 * we can always remove the listener.
	 */
	wl_list_remove(&shsurf->surface_destroy_listener.link);
	shsurf->surface->configure = NULL;
	weston_surface_set_label_func(shsurf->surface, NULL);
	free(shsurf->title);

	weston_view_destroy(shsurf->view);

	wl_list_remove(&shsurf->children_link);
	wl_list_for_each_safe(child, next, &shsurf->children_list, children_link)
		shell_surface_set_parent(child, NULL);

	wl_list_remove(&shsurf->link);
	free(shsurf);
}

static void
shell_destroy_shell_surface(struct wl_resource *resource)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);

	if (!wl_list_empty(&shsurf->popup.grab_link))
		remove_popup_grab(shsurf);
	//if (shsurf->resource)
	//	wl_list_remove(wl_resource_get_link(shsurf->resource));
	shsurf->resource = NULL;
}

static void
shell_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct shell_surface *shsurf = container_of(listener,
						    struct shell_surface,
						    surface_destroy_listener);

	if (shsurf->resource)
		wl_resource_destroy(shsurf->resource);

	destroy_shell_surface(shsurf);
}

static void
fade_out_done(struct weston_view_animation *animation, void *data)
{
	struct shell_surface *shsurf = data;

	weston_surface_destroy(shsurf->surface);
}

static void
handle_resource_destroy(struct wl_listener *listener, void *data)
{
	struct shell_surface *shsurf =
		container_of(listener, struct shell_surface,
			     resource_destroy_listener);

	if (!weston_surface_is_mapped(shsurf->surface))
		return;

	shsurf->surface->ref_count++;

	pixman_region32_fini(&shsurf->surface->pending.input);
	pixman_region32_init(&shsurf->surface->pending.input);
	pixman_region32_fini(&shsurf->surface->input);
	pixman_region32_init(&shsurf->surface->input);
	if (shsurf->shell->win_close_animation_type == ANIMATION_FADE) {
		weston_fade_run(shsurf->view, 1.0, 0.0, 300.0,
				fade_out_done, shsurf);
	} else {
		weston_surface_destroy(shsurf->surface);
	}
}

static void
shell_surface_configure(struct weston_surface *, int32_t, int32_t);

struct shell_surface *
get_shell_surface(struct weston_surface *surface)
{
	if (surface->configure == shell_surface_configure)
		return surface->configure_private;
	else
		return NULL;
}

static struct shell_surface *
create_common_surface(struct shell_client *owner, void *shell,
		      struct weston_surface *surface,
		      const struct weston_shell_client *client)
{
	struct shell_surface *shsurf;

	assert(surface->configure == NULL);

	shsurf = calloc(1, sizeof *shsurf);
	if (!shsurf) {
		weston_log("no memory to allocate shell surface\n");
		return NULL;
	}

	shsurf->view = weston_view_create(surface);
	if (!shsurf->view) {
		weston_log("no memory to allocate shell surface\n");
		free(shsurf);
		return NULL;
	}

	surface->configure = shell_surface_configure;
	surface->configure_private = shsurf;
	weston_surface_set_label_func(surface, shell_surface_get_label);

	shsurf->resource_destroy_listener.notify = handle_resource_destroy;
	wl_resource_add_destroy_listener(surface->resource,
					 &shsurf->resource_destroy_listener);
	shsurf->owner = owner;

	shsurf->shell = (struct mayhem_shell *) shell;
	shsurf->unresponsive = 0;
	shsurf->saved_position_valid = false;
	shsurf->saved_size_valid = false;
	shsurf->saved_rotation_valid = false;
	shsurf->surface = surface;
	shsurf->fullscreen.type = WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT;
	shsurf->fullscreen.framerate = 0;
	wl_list_init(&shsurf->fullscreen.transform.link);

	shsurf->output = get_default_output(shsurf->shell->compositor);

	wl_signal_init(&shsurf->destroy_signal);
	shsurf->surface_destroy_listener.notify = shell_handle_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &shsurf->surface_destroy_listener);

	/* init link so its safe to always remove it in destroy_shell_surface */
	wl_list_init(&shsurf->link);
	wl_list_init(&shsurf->popup.grab_link);

	/* empty when not in use */
	wl_list_init(&shsurf->rotation.transform.link);
	weston_matrix_init(&shsurf->rotation.rotation);

	wl_list_init(&shsurf->workspace_transform.link);

	wl_list_init(&shsurf->children_link);
	wl_list_init(&shsurf->children_list);
	shsurf->parent = NULL;

	set_type(shsurf, SHELL_SURFACE_NONE);

	shsurf->client = client;

	return shsurf;
}

static struct shell_surface *
create_shell_surface(void *shell, struct weston_surface *surface,
		     const struct weston_shell_client *client)
{
	return create_common_surface(NULL, shell, surface, client);
}

static void
shell_get_shell_surface(struct wl_client *client,
			struct wl_resource *resource,
			uint32_t id,
			struct wl_resource *surface_resource)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);
	struct shell_client *sc = wl_resource_get_user_data(resource);
	struct mayhem_shell *shell = sc->shell;
	struct shell_surface *shsurf;

	if (weston_surface_set_role(surface, "wl_shell_surface",
				    resource, WL_SHELL_ERROR_ROLE) < 0)
		return;

	shsurf = create_common_surface(sc, shell, surface, &shell_client);
	if (!shsurf) {
		wl_resource_post_no_memory(surface_resource);
		return;
	}

	shsurf->resource =
		wl_resource_create(client,
				   &wl_shell_surface_interface, 1, id);
	if (!shsurf->resource) {
		wl_resource_post_no_memory(surface_resource);
	        return;
	}
	wl_resource_set_implementation(shsurf->resource,
				       &shell_surface_implementation,
				       shsurf, shell_destroy_shell_surface);
}

static bool
shell_surface_is_wl_shell_surface(struct shell_surface *shsurf)
{
	/* A shell surface without a resource is created from xwayland
	 * and is considered a wl_shell surface for now. */

	return shsurf->resource == NULL ||
		wl_resource_instance_of(shsurf->resource,
					&wl_shell_surface_interface,
					&shell_surface_implementation);
}

static const struct wl_shell_interface shell_implementation = {
	shell_get_shell_surface
};

/****************************
 * xdg-shell implementation */

static void
xdg_surface_destroy(struct wl_client *client,
		    struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
xdg_surface_set_parent(struct wl_client *client,
		       struct wl_resource *resource,
		       struct wl_resource *parent_resource)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);
	struct shell_surface *parent;

	if (parent_resource) {
		parent = wl_resource_get_user_data(parent_resource);
		shell_surface_set_parent(shsurf, parent->surface);
	} else {
		shell_surface_set_parent(shsurf, NULL);
	}
}

static void
xdg_surface_set_app_id(struct wl_client *client,
		       struct wl_resource *resource,
		       const char *app_id)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);

	free(shsurf->class);
	shsurf->class = strdup(app_id);
	shsurf->surface->timeline.force_refresh = 1;
}

static void
xdg_surface_show_window_menu(struct wl_client *client,
			     struct wl_resource *surface_resource,
			     struct wl_resource *seat_resource,
			     uint32_t serial,
			     int32_t x,
			     int32_t y)
{
	/* TODO */
}

static void
xdg_surface_set_title(struct wl_client *client,
			struct wl_resource *resource, const char *title)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);

	set_title(shsurf, title);
}

static void
xdg_surface_move(struct wl_client *client, struct wl_resource *resource,
		 struct wl_resource *seat_resource, uint32_t serial)
{
	common_surface_move(resource, seat_resource, serial);
}

static void
xdg_surface_resize(struct wl_client *client, struct wl_resource *resource,
		   struct wl_resource *seat_resource, uint32_t serial,
		   uint32_t edges)
{
	common_surface_resize(resource, seat_resource, serial, edges);
}

static void
xdg_surface_ack_configure(struct wl_client *client,
			  struct wl_resource *resource,
			  uint32_t serial)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);

	if (shsurf->state_requested) {
		shsurf->next_state = shsurf->requested_state;
		shsurf->state_changed = true;
		shsurf->state_requested = false;
	}
}

static void
xdg_surface_set_window_geometry(struct wl_client *client,
				struct wl_resource *resource,
				int32_t x,
				int32_t y,
				int32_t width,
				int32_t height)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);

	set_window_geometry(shsurf, x, y, width, height);
}

static void
xdg_surface_set_maximized(struct wl_client *client,
			  struct wl_resource *resource)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);
	struct weston_output *output;

	shsurf->state_requested = true;
	shsurf->requested_state.maximized = true;

	if (!weston_surface_is_mapped(shsurf->surface))
		output = get_focused_output(shsurf->surface->compositor);
	else
		output = shsurf->surface->output;

	shell_surface_set_output(shsurf, output);
 	send_configure_for_surface(shsurf);
}

static void
xdg_surface_unset_maximized(struct wl_client *client,
			    struct wl_resource *resource)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);

	shsurf->state_requested = true;
	shsurf->requested_state.maximized = false;
	send_configure_for_surface(shsurf);
}

static void xdg_surface_set_fullscreen(struct wl_client *client,
				       struct wl_resource *resource,
				       struct wl_resource *output_resource)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);
	struct weston_output *output;

	shsurf->state_requested = true;
	shsurf->requested_state.fullscreen = true;

	if (output_resource)
		output = wl_resource_get_user_data(output_resource);
	else
		output = NULL;

	/* handle clients launching in fullscreen */
	if (output == NULL && !weston_surface_is_mapped(shsurf->surface)) {
		/* Set the output to the one that has focus currently. */
		assert(shsurf->surface);
		output = get_focused_output(shsurf->surface->compositor);
	}

	shell_surface_set_output(shsurf, output);
	shsurf->fullscreen_output = shsurf->output;

	send_configure_for_surface(shsurf);
}

static void
xdg_surface_unset_fullscreen(struct wl_client *client,
			     struct wl_resource *resource)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);

	shsurf->state_requested = true;
	shsurf->requested_state.fullscreen = false;
	send_configure_for_surface(shsurf);
}

static void xdg_surface_set_minimized(struct wl_client *client,
				      struct wl_resource *resource)
{
	struct shell_surface *shsurf = wl_resource_get_user_data(resource);

	if (shsurf->type != SHELL_SURFACE_TOPLEVEL)
		return;

	 /* apply compositor's own minimization logic (hide) */
	set_minimized(shsurf->surface);
}

static const struct xdg_surface_interface xdg_surface_implementation = {
	xdg_surface_destroy,
	xdg_surface_set_parent,
	xdg_surface_set_title,
	xdg_surface_set_app_id,
	xdg_surface_show_window_menu,
	xdg_surface_move,
	xdg_surface_resize,
	xdg_surface_ack_configure,
	xdg_surface_set_window_geometry,
	xdg_surface_set_maximized,
	xdg_surface_unset_maximized,
	xdg_surface_set_fullscreen,
	xdg_surface_unset_fullscreen,
	xdg_surface_set_minimized,
};

static void xdg_send_configure(struct weston_surface *surface, int32_t width,
			       int32_t height)
{
	struct shell_surface *shsurf = get_shell_surface(surface);
	uint32_t *s;
	struct wl_array states;
	uint32_t serial;

	assert(shsurf);

	if (!shsurf->resource)
		return;

	wl_array_init(&states);
	if (shsurf->requested_state.fullscreen) {
		s = wl_array_add(&states, sizeof *s);
		*s = XDG_SURFACE_STATE_FULLSCREEN;
	} else if (shsurf->requested_state.maximized) {
		s = wl_array_add(&states, sizeof *s);
		*s = XDG_SURFACE_STATE_MAXIMIZED;
	}
	if (shsurf->resize_edges != 0) {
		s = wl_array_add(&states, sizeof *s);
		*s = XDG_SURFACE_STATE_RESIZING;
	}
	if (shsurf->focus_count > 0) {
		s = wl_array_add(&states, sizeof *s);
		*s = XDG_SURFACE_STATE_ACTIVATED;
	}

	serial = wl_display_next_serial(shsurf->surface->compositor->wl_display);
	xdg_surface_send_configure(shsurf->resource, width, height, &states, serial);

	wl_array_release(&states);
}

static const struct weston_shell_client xdg_client = {
	xdg_send_configure,
	NULL
};

static void
xdg_shell_destroy(struct wl_client *client,
		  struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
xdg_use_unstable_version(struct wl_client *client,
			 struct wl_resource *resource,
			 int32_t version)
{
	if (version > 1) {
		wl_resource_post_error(resource,
				       1,
				       "xdg-shell:: version not implemented yet.");
		return;
	}
}

static struct shell_surface *
create_xdg_surface(struct shell_client *owner, void *shell,
		   struct weston_surface *surface,
		   const struct weston_shell_client *client)
{
	struct shell_surface *shsurf;

	shsurf = create_common_surface(owner, shell, surface, client);
	if (!shsurf)
		return NULL;

	set_type(shsurf, SHELL_SURFACE_TOPLEVEL);

	return shsurf;
}

static void
xdg_get_xdg_surface(struct wl_client *client,
		    struct wl_resource *resource,
		    uint32_t id,
		    struct wl_resource *surface_resource)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);
	struct shell_client *sc = wl_resource_get_user_data(resource);
	struct mayhem_shell *shell = sc->shell;
	struct shell_surface *shsurf;

	shsurf = get_shell_surface(surface);
	if (shsurf && shell_surface_is_xdg_surface(shsurf)) {
		wl_resource_post_error(resource, XDG_SHELL_ERROR_ROLE,
				       "This wl_surface is already an "
				       "xdg_surface");
		return;
	}

	if (weston_surface_set_role(surface, "xdg_surface",
				    resource, XDG_SHELL_ERROR_ROLE) < 0)
		return;

	shsurf = create_xdg_surface(sc, shell, surface, &xdg_client);
	if (!shsurf) {
		wl_resource_post_no_memory(surface_resource);
		return;
	}

	shsurf->resource =
		wl_resource_create(client,
				   &xdg_surface_interface, 1, id);
	if (!shsurf->resource) {
		wl_resource_post_no_memory(surface_resource);
		return;
	}
	wl_resource_set_implementation(shsurf->resource,
				       &xdg_surface_implementation,
				       shsurf, shell_destroy_shell_surface);
}

static bool shell_surface_is_xdg_surface(struct shell_surface *shsurf)
{
	return shsurf->resource &&
		wl_resource_instance_of(shsurf->resource,
					&xdg_surface_interface,
					&xdg_surface_implementation);
}

/* xdg-popup implementation */

static void xdg_popup_destroy(struct wl_client *client,
			      struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct xdg_popup_interface xdg_popup_implementation = {
	xdg_popup_destroy,
};

static void
xdg_popup_send_configure(struct weston_surface *surface,
			 int32_t width, int32_t height)
{
}

static const struct weston_shell_client xdg_popup_client = {
	xdg_popup_send_configure,
	NULL
};

static struct shell_surface *
create_xdg_popup(struct shell_client *owner, void *shell,
		 struct weston_surface *surface,
		 const struct weston_shell_client *client,
		 struct weston_surface *parent,
		 struct shell_seat *seat,
		 uint32_t serial,
		 int32_t x, int32_t y)
{
	struct shell_surface *shsurf;


	shsurf = create_common_surface(owner, shell, surface, client);
	if (!shsurf)
		return NULL;

	set_type(shsurf, SHELL_SURFACE_POPUP);
	shsurf->popup.shseat = seat;
	shsurf->popup.serial = serial;
	shsurf->popup.x = x;
	shsurf->popup.y = y;
	shell_surface_set_parent(shsurf, parent);

	return shsurf;
}

static void
xdg_get_xdg_popup(struct wl_client *client,
		  struct wl_resource *resource,
		  uint32_t id,
		  struct wl_resource *surface_resource,
		  struct wl_resource *parent_resource,
		  struct wl_resource *seat_resource,
		  uint32_t serial,
		  int32_t x, int32_t y)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);
	struct shell_client *sc = wl_resource_get_user_data(resource);
	struct mayhem_shell *shell = sc->shell;
	struct shell_surface *shsurf;
	struct weston_surface *parent;
	struct shell_seat *seat;

	shsurf = get_shell_surface(surface);
	if (shsurf && shell_surface_is_xdg_popup(shsurf)) {
		wl_resource_post_error(resource, XDG_SHELL_ERROR_ROLE,
				       "This wl_surface is already an "
				       "xdg_popup");
		return;
	}

	if (weston_surface_set_role(surface, "xdg_popup",
				    resource, XDG_SHELL_ERROR_ROLE) < 0)
		return;

	if (!parent_resource) {
		wl_resource_post_error(surface_resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "xdg_shell::get_xdg_popup requires a parent shell surface");
		return;
	}

	parent = wl_resource_get_user_data(parent_resource);
	seat = get_shell_seat(wl_resource_get_user_data(seat_resource));

	shsurf = create_xdg_popup(sc, shell, surface, &xdg_popup_client,
				  parent, seat, serial, x, y);
	if (!shsurf) {
		wl_resource_post_no_memory(surface_resource);
		return;
	}

	shsurf->resource = wl_resource_create(client, &xdg_popup_interface,
					      1, id);
	if (!shsurf->resource) {
		wl_resource_post_no_memory(surface_resource);
		return;
	}
	wl_resource_set_implementation(shsurf->resource,
				       &xdg_popup_implementation,
				       shsurf, shell_destroy_shell_surface);
}

static void xdg_pong(struct wl_client *client, struct wl_resource *resource,
		     uint32_t serial)
{
	struct shell_client *sc = wl_resource_get_user_data(resource);

	shell_client_pong(sc, serial);
}

static bool shell_surface_is_xdg_popup(struct shell_surface *shsurf)
{
	return (shsurf->resource &&
		wl_resource_instance_of(shsurf->resource,
					&xdg_popup_interface,
					&xdg_popup_implementation));
}

static const struct xdg_shell_interface xdg_implementation = {
	xdg_shell_destroy,
	xdg_use_unstable_version,
	xdg_get_xdg_surface,
	xdg_get_xdg_popup,
	xdg_pong
};

static int
xdg_shell_unversioned_dispatch(const void *implementation,
			       void *_target, uint32_t opcode,
			       const struct wl_message *message,
			       union wl_argument *args)
{
	struct wl_resource *resource = _target;
	struct shell_client *sc = wl_resource_get_user_data(resource);

	if (opcode != 1 /* XDG_SHELL_USE_UNSTABLE_VERSION */) {
		wl_resource_post_error(resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "must call use_unstable_version first");
		return 0;
	}

#define XDG_SERVER_VERSION 5

	static_assert(XDG_SERVER_VERSION == XDG_SHELL_VERSION_CURRENT,
		      "shell implementation doesn't match protocol version");

	if (args[0].i != XDG_SERVER_VERSION) {
		wl_resource_post_error(resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "incompatible version, server is %d "
				       "client wants %d",
				       XDG_SERVER_VERSION, args[0].i);
		return 0;
	}

	wl_resource_set_implementation(resource, &xdg_implementation,
				       sc, NULL);

	return 1;
}

/* end of xdg-shell implementation */
/***********************************/

static void
shell_fade(struct mayhem_shell *shell, enum fade_type type);


static void configure_static_view(struct weston_view *ev, struct weston_layer *layer)
{
	struct weston_view *v, *next;

	wl_list_for_each_safe(v, next, &layer->view_list.link, layer_link.link) {
		if (v->output == ev->output && v != ev) {
			weston_view_unmap(v);
			v->surface->configure = NULL;
			weston_surface_set_label_func(v->surface, NULL);
		}
	}

	weston_view_set_position(ev, ev->output->x, ev->output->y);

	if (wl_list_empty(&ev->layer_link.link)) {
		weston_layer_entry_insert(&layer->view_list, &ev->layer_link);
		weston_compositor_schedule_repaint(ev->surface->compositor);
	}
}

static int background_get_label(struct weston_surface *surface, char *buf, size_t len)
{
	return snprintf(buf, len, "background for output %s",
			surface->output->name);
}

static void background_configure(struct weston_surface *es,
				 int32_t sx, int32_t sy)
{
	printf("server s configuring bg\n");
	struct mayhem_shell *shell = es->configure_private;
	struct weston_view *view;

	view = container_of(es->views.next, struct weston_view, surface_link);

	configure_static_view(view, &shell->background_layer);
}

static const struct weston_shell_client ms_menu_client = {
	NULL
};

static void msurf_destroy(struct wl_client *client,
			  struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct ms_surface_interface ms_surface_implementation = {
	.destroy = msurf_destroy
};

static bool shell_surface_is_ms_surface(struct shell_surface *shsurf)
{
	return shsurf->resource &&
		wl_resource_instance_of(shsurf->resource,
					&ms_surface_interface,
					&ms_surface_implementation);
}

static void ms_setbg(struct wl_client *client,
		     struct wl_resource *resource,
		     struct wl_resource *output_resource,
		     struct wl_resource *surface_resource)
{
	printf("server got setbg request\n");
	struct shell_client *sc = wl_resource_get_user_data(resource);
	struct mayhem_shell *shell = sc->shell;
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);
	struct weston_view *view, *next;

	if (surface->configure) {
		wl_resource_post_error(surface_resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "surface role already assigned");
		return;
	}

	wl_list_for_each_safe(view, next, &surface->views, surface_link)
		weston_view_destroy(view);
	view = weston_view_create(surface);

	surface->configure = background_configure;
	surface->configure_private = shell;
	weston_surface_set_label_func(surface, background_get_label);
	surface->output = wl_resource_get_user_data(output_resource);
	view->output = surface->output;

	//TODO REMOVE
	//ms_menu_send_spawn(shell->child.mayhem_shell, 0, 0);
}

static void ms_setgrab(struct wl_client *client, struct wl_resource *resource,
		       struct wl_resource *surface_resource)
{
	struct shell_client *sc = wl_resource_get_user_data(resource);
	struct mayhem_shell *shell = sc->shell;

	shell->grab_surface = wl_resource_get_user_data(surface_resource);
	weston_view_create(shell->grab_surface);
}

static void ms_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void ms_getsurf(struct wl_client* client, struct wl_resource *resource,
		       uint32_t id, struct wl_resource* surface_resource)
{
	struct weston_surface *surface = wl_resource_get_user_data(surface_resource);
	struct shell_client *sc = wl_resource_get_user_data(resource);
	struct mayhem_shell *shell = sc->shell;
	struct shell_surface *shsurf;

	shsurf = get_shell_surface(surface);
	if(shsurf && shell_surface_is_ms_surface(shsurf)) {
		wl_resource_post_error(resource, MS_MENU_ERROR_ROLE,
				       "This wl_surface is already an "
				       "ms_surface");
		return;
	}

	if(weston_surface_set_role(surface, "ms_surface",
				   resource, MS_MENU_ERROR_ROLE) < 0)
		return;

	shsurf = create_common_surface(sc, shell, surface, &ms_menu_client);
	if(!shsurf) {
		wl_resource_post_no_memory(surface_resource);
		return;
	}
	set_type(shsurf, SHELL_SURFACE_MENU);

	shsurf->resource = wl_resource_create(client, &ms_surface_interface,
					      1, id);
	wl_resource_set_implementation(shsurf->resource,
				       &ms_surface_implementation,
				       shsurf, shell_destroy_shell_surface);
}

static const struct ms_menu_interface ms_menu_implementation = {
	.destroy = ms_destroy,
	.set_background = ms_setbg,
	.set_grab_surface = ms_setgrab,
	.get_menu_surface = ms_getsurf
};



static enum shell_surface_type get_shell_surface_type(struct weston_surface *surface)
{
	struct shell_surface *shsurf;

	shsurf = get_shell_surface(surface);
	if (!shsurf)
		return SHELL_SURFACE_NONE;
	return shsurf->type;
}

static void move_binding(struct weston_pointer *pointer, uint32_t time,
			 uint32_t button, void *data)
{
	struct weston_surface *focus;
	struct weston_surface *surface;
	struct shell_surface *shsurf;

	if (pointer->focus == NULL)
		return;

	focus = pointer->focus->surface;

	surface = weston_surface_get_main_surface(focus);
	if (surface == NULL)
		return;

	shsurf = get_shell_surface(surface);
	if (shsurf == NULL || shsurf->state.fullscreen ||
	    shsurf->state.maximized)
		return;

	surface_move(shsurf, pointer, 0);
}

static void maximize_binding(struct weston_keyboard *keyboard, uint32_t time,
			     uint32_t button, void *data)
{
	struct weston_surface *focus = keyboard->focus;
	struct weston_surface *surface;
	struct shell_surface *shsurf;

	surface = weston_surface_get_main_surface(focus);
	if (surface == NULL)
		return;

	shsurf = get_shell_surface(surface);
	if (shsurf == NULL)
		return;

	if (!shell_surface_is_xdg_surface(shsurf))
		return;

	shsurf->state_requested = true;
	shsurf->requested_state.maximized = !shsurf->state.maximized;
	send_configure_for_surface(shsurf);
}

static void fullscreen_binding(struct weston_keyboard *keyboard, uint32_t time,
			       uint32_t button, void *data)
{
	struct weston_surface *focus = keyboard->focus;
	struct weston_surface *surface;
	struct shell_surface *shsurf;

	surface = weston_surface_get_main_surface(focus);
	if(surface == NULL)
		return;

	shsurf = get_shell_surface(surface);
	if(shsurf == NULL)
		return;

	if(!shell_surface_is_xdg_surface(shsurf))
		return;

	shsurf->state_requested = true;
	shsurf->requested_state.fullscreen = !shsurf->state.fullscreen;
	shsurf->fullscreen_output = shsurf->output;
	send_configure_for_surface(shsurf);
}

static void resize_binding(struct weston_pointer *pointer, uint32_t time,
			   uint32_t button, void *data)
{
	struct weston_surface *focus;
	struct weston_surface *surface;
	uint32_t edges = 0;
	int32_t x, y;
	struct shell_surface *shsurf;

	if (pointer->focus == NULL)
		return;

	focus = pointer->focus->surface;

	surface = weston_surface_get_main_surface(focus);
	if (surface == NULL)
		return;

	shsurf = get_shell_surface(surface);
	if (shsurf == NULL || shsurf->state.fullscreen ||
	    shsurf->state.maximized)
		return;

	weston_view_from_global(shsurf->view,
				wl_fixed_to_int(pointer->grab_x),
				wl_fixed_to_int(pointer->grab_y),
				&x, &y);

	if (x < shsurf->surface->width / 3)
		edges |= WL_SHELL_SURFACE_RESIZE_LEFT;
	else if (x < 2 * shsurf->surface->width / 3)
		edges |= 0;
	else
		edges |= WL_SHELL_SURFACE_RESIZE_RIGHT;

	if (y < shsurf->surface->height / 3)
		edges |= WL_SHELL_SURFACE_RESIZE_TOP;
	else if (y < 2 * shsurf->surface->height / 3)
		edges |= 0;
	else
		edges |= WL_SHELL_SURFACE_RESIZE_BOTTOM;

	surface_resize(shsurf, pointer, edges);
}

static void surface_opacity_binding(struct weston_pointer *pointer,
				    uint32_t time,
				    struct weston_pointer_axis_event *event,
				    void *data)
{
	float step = 0.005;
	struct shell_surface *shsurf;
	struct weston_surface *focus = pointer->focus->surface;
	struct weston_surface *surface;

	/* XXX: broken for windows containing sub-surfaces */
	surface = weston_surface_get_main_surface(focus);
	if (surface == NULL)
		return;

	shsurf = get_shell_surface(surface);
	if (!shsurf)
		return;

	shsurf->view->alpha -= wl_fixed_to_double(event->value) * step;

	if (shsurf->view->alpha > 1.0)
		shsurf->view->alpha = 1.0;
	if (shsurf->view->alpha < step)
		shsurf->view->alpha = step;

	weston_view_geometry_dirty(shsurf->view);
	weston_surface_damage(surface);
}

static void do_zoom(struct weston_seat *seat, uint32_t time, uint32_t key,
		    uint32_t axis, wl_fixed_t value)
{
	struct weston_compositor *compositor = seat->compositor;
	struct weston_pointer *pointer = weston_seat_get_pointer(seat);
	struct weston_output *output;
	float increment;

	if (!pointer) {
		weston_log("Zoom hotkey pressed but seat '%s' contains no "
			   "pointer.\n", seat->seat_name);
		return;
	}

	wl_list_for_each(output, &compositor->output_list, link) {
		if (pixman_region32_contains_point(&output->region,
						   wl_fixed_to_double(pointer->x),
						   wl_fixed_to_double(pointer->y),
						   NULL)) {
			if (key == KEY_PAGEUP)
				increment = output->zoom.increment;
			else if (key == KEY_PAGEDOWN)
				increment = -output->zoom.increment;
			else if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
				/* For every pixel zoom 20th of a step */
				increment = output->zoom.increment *
					    -wl_fixed_to_double(value) / 20.0;
			else
				increment = 0;

			output->zoom.level += increment;

			if (output->zoom.level < 0.0)
				output->zoom.level = 0.0;
			else if (output->zoom.level > output->zoom.max_level)
				output->zoom.level = output->zoom.max_level;

			if (!output->zoom.active) {
				if (output->zoom.level <= 0.0)
					continue;
				weston_output_activate_zoom(output, seat);
			}

			output->zoom.spring_z.target = output->zoom.level;

			weston_output_update_zoom(output);
		}
	}
}

static void
zoom_axis_binding(struct weston_pointer *pointer, uint32_t time,
		  struct weston_pointer_axis_event *event, void *data)
{
	do_zoom(pointer->seat, time, 0, event->axis, event->value);
}

static void
zoom_key_binding(struct weston_keyboard *keyboard, uint32_t time, uint32_t key,
		 void *data)
{
	do_zoom(keyboard->seat, time, key, 0, 0);
}

static void terminate_binding(struct weston_keyboard *keyboard, uint32_t time,
			      uint32_t key, void *data)
{
	struct weston_compositor *compositor = data;

	wl_display_terminate(compositor->wl_display);
}

static void exec_binding(struct weston_keyboard *keyboard, uint32_t time,
			 uint32_t key, void *data)
{
	/*
	char *c;
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		weston_log("fork failed: %m\n");
		return;
	}

	if (pid)
		return;

	c = "";
	if (execvp(data, &c) < 0) {
		weston_log("execvp '%s' failed: %m\n", (char*)data);
		exit(1);
	}*/
	char *cmd = malloc(strlen(data)+2);
	strcpy(cmd, data);
	strcat(cmd, " &");
	system(cmd);
}

static void
rotate_grab_motion(struct weston_pointer_grab *grab, uint32_t time,
		   struct weston_pointer_motion_event *event)
{
	struct rotate_grab *rotate =
		container_of(grab, struct rotate_grab, base.grab);
	struct weston_pointer *pointer = grab->pointer;
	struct shell_surface *shsurf = rotate->base.shsurf;
	float cx, cy, dx, dy, cposx, cposy, dposx, dposy, r;

	weston_pointer_move(pointer, event);

	if (!shsurf)
		return;

	cx = 0.5f * shsurf->surface->width;
	cy = 0.5f * shsurf->surface->height;

	dx = wl_fixed_to_double(pointer->x) - rotate->center.x;
	dy = wl_fixed_to_double(pointer->y) - rotate->center.y;
	r = sqrtf(dx * dx + dy * dy);

	wl_list_remove(&shsurf->rotation.transform.link);
	weston_view_geometry_dirty(shsurf->view);

	if (r > 20.0f) {
		struct weston_matrix *matrix =
			&shsurf->rotation.transform.matrix;

		weston_matrix_init(&rotate->rotation);
		weston_matrix_rotate_xy(&rotate->rotation, dx / r, dy / r);

		weston_matrix_init(matrix);
		weston_matrix_translate(matrix, -cx, -cy, 0.0f);
		weston_matrix_multiply(matrix, &shsurf->rotation.rotation);
		weston_matrix_multiply(matrix, &rotate->rotation);
		weston_matrix_translate(matrix, cx, cy, 0.0f);

		wl_list_insert(
			&shsurf->view->geometry.transformation_list,
			&shsurf->rotation.transform.link);
	} else {
		wl_list_init(&shsurf->rotation.transform.link);
		weston_matrix_init(&shsurf->rotation.rotation);
		weston_matrix_init(&rotate->rotation);
	}

	/* We need to adjust the position of the surface
	 * in case it was resized in a rotated state before */
	cposx = shsurf->view->geometry.x + cx;
	cposy = shsurf->view->geometry.y + cy;
	dposx = rotate->center.x - cposx;
	dposy = rotate->center.y - cposy;
	if (dposx != 0.0f || dposy != 0.0f) {
		weston_view_set_position(shsurf->view,
					 shsurf->view->geometry.x + dposx,
					 shsurf->view->geometry.y + dposy);
	}

	/* Repaint implies weston_view_update_transform(), which
	 * lazily applies the damage due to rotation update.
	 */
	weston_compositor_schedule_repaint(shsurf->surface->compositor);
}

static void
rotate_grab_button(struct weston_pointer_grab *grab,
		   uint32_t time, uint32_t button, uint32_t state_w)
{
	struct rotate_grab *rotate =
		container_of(grab, struct rotate_grab, base.grab);
	struct weston_pointer *pointer = grab->pointer;
	struct shell_surface *shsurf = rotate->base.shsurf;
	enum wl_pointer_button_state state = state_w;

	if (pointer->button_count == 0 &&
	    state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (shsurf)
			weston_matrix_multiply(&shsurf->rotation.rotation,
					       &rotate->rotation);
		shell_grab_end(&rotate->base);
		free(rotate);
	}
}

static void
rotate_grab_cancel(struct weston_pointer_grab *grab)
{
	struct rotate_grab *rotate =
		container_of(grab, struct rotate_grab, base.grab);

	shell_grab_end(&rotate->base);
	free(rotate);
}

static const struct weston_pointer_grab_interface rotate_grab_interface = {
	noop_grab_focus,
	rotate_grab_motion,
	rotate_grab_button,
	noop_grab_axis,
	noop_grab_axis_source,
	noop_grab_frame,
	rotate_grab_cancel,
};

static void
surface_rotate(struct shell_surface *surface, struct weston_pointer *pointer)
{
	struct rotate_grab *rotate;
	float dx, dy;
	float r;

	rotate = malloc(sizeof *rotate);
	if (!rotate)
		return;

	weston_view_to_global_float(surface->view,
				    surface->surface->width * 0.5f,
				    surface->surface->height * 0.5f,
				    &rotate->center.x, &rotate->center.y);

	dx = wl_fixed_to_double(pointer->x) - rotate->center.x;
	dy = wl_fixed_to_double(pointer->y) - rotate->center.y;
	r = sqrtf(dx * dx + dy * dy);
	if (r > 20.0f) {
		struct weston_matrix inverse;

		weston_matrix_init(&inverse);
		weston_matrix_rotate_xy(&inverse, dx / r, -dy / r);
		weston_matrix_multiply(&surface->rotation.rotation, &inverse);

		weston_matrix_init(&rotate->rotation);
		weston_matrix_rotate_xy(&rotate->rotation, dx / r, dy / r);
	} else {
		weston_matrix_init(&surface->rotation.rotation);
		weston_matrix_init(&rotate->rotation);
	}

	shell_grab_start(&rotate->base, &rotate_grab_interface, surface,
			 pointer, MS_MENU_CURSOR_ARROW);
}

static void rotate_binding(struct weston_pointer *pointer, uint32_t time,
			   uint32_t button, void *data)
{
	struct weston_surface *focus;
	struct weston_surface *base_surface;
	struct shell_surface *surface;

	if (pointer->focus == NULL)
		return;

	focus = pointer->focus->surface;

	base_surface = weston_surface_get_main_surface(focus);
	if (base_surface == NULL)
		return;

	surface = get_shell_surface(base_surface);
	if (surface == NULL || surface->state.fullscreen ||
	    surface->state.maximized)
		return;

	surface_rotate(surface, pointer);
}

/* Move all fullscreen layers down to the current workspace and hide their
 * black views. The surfaces' state is set to both fullscreen and lowered,
 * and this is reversed when such a surface is re-configured, see
 * shell_configure_fullscreen() and shell_ensure_fullscreen_black_view().
 *
 * This should be used when implementing shell-wide overlays, such as
 * the alt-tab switcher, which need to de-promote fullscreen layers. */
/*
void lower_fullscreen_layer(struct mayhem_shell *shell)
{
	struct workspace *ws;
	struct weston_view *view, *prev;

	ws = get_current_workspace(shell);
	wl_list_for_each_reverse_safe(view, prev,
				      &shell->fullscreen_layer.view_list.link,
				      layer_link.link) {
		struct shell_surface *shsurf = get_shell_surface(view->surface);

		if (!shsurf)
			continue;

		* We can have a non-fullscreen popup for a fullscreen surface
		 * in the fullscreen layer. *

		* Lower the view to the workspace layer *
		//weston_layer_entry_remove(&view->layer_link);
		//weston_layer_entry_insert(&ws->layer.view_list, &view->layer_link);
		weston_view_damage_below(view);
		weston_surface_damage(view->surface);

	}
}
*/

void activate(struct mayhem_shell *shell, struct weston_surface *es,
	 struct weston_seat *seat, bool configure)
{
	struct weston_surface *main_surface;
	struct focus_state *state;
	struct workspace *ws;
	struct weston_surface *old_es;
	struct shell_surface *shsurf;

	//lower_fullscreen_layer(shell);

	main_surface = weston_surface_get_main_surface(es);

	weston_surface_activate(es, seat);

	state = ensure_focus_state(shell, seat);
	if (state == NULL)
		return;

	old_es = state->keyboard_focus;
	focus_state_set_focus(state, es);

	shsurf = get_shell_surface(main_surface);
	assert(shsurf);

	if (shsurf->state.fullscreen && configure)
		shell_configure_fullscreen(shsurf);
	else
		restore_output_mode(shsurf->output);

	/* Update the surface’s layer. This brings it to the top of the stacking
	 * order as appropriate. */
	shell_surface_update_layer(shsurf);

	if (shell->focus_animation_type != ANIMATION_NONE) {
		ws = get_current_workspace(shell);
		animate_focus_change(shell, ws, get_default_view(old_es), get_default_view(es));
	}
}

static void activate_binding(struct weston_seat *seat,
			     struct mayhem_shell *shell,
			     struct weston_view *focus_view)
{
	struct focus_state *state;
	struct weston_surface *focus;
	struct weston_surface *main_surface;

	focus = focus_view->surface;

	main_surface = weston_surface_get_main_surface(focus);
	if (get_shell_surface_type(main_surface) == SHELL_SURFACE_NONE)
		return;

	state = ensure_focus_state(shell, seat);
	if (state == NULL)
		return;

	if (state->keyboard_focus == focus)
		return;

	activate(shell, focus, seat, true);
}

static void click_to_activate_binding(struct weston_pointer *pointer,
				      uint32_t time, uint32_t button,
				      void *data)
{
	if (pointer->grab != &pointer->default_grab)
		return;
	if (pointer->focus == NULL)
		return;

	activate_binding(pointer->seat, data, pointer->focus);
}

static void shell_fade_done(struct weston_view_animation *animation, void *data)
{
	struct mayhem_shell *shell = data;

	shell->fade.animation = NULL;

	switch (shell->fade.type) {
	case FADE_IN:
		weston_surface_destroy(shell->fade.view->surface);
		shell->fade.view = NULL;
		break;
	case FADE_OUT:
		//lock(shell);
		break;
	default:
		break;
	}
}

static struct weston_view *shell_fade_create_surface(struct mayhem_shell *shell)
{
	struct weston_compositor *compositor = shell->compositor;
	struct weston_surface *surface;
	struct weston_view *view;

	surface = weston_surface_create(compositor);
	if (!surface)
		return NULL;

	view = weston_view_create(surface);
	if (!view) {
		weston_surface_destroy(surface);
		return NULL;
	}

	weston_surface_set_size(surface, 8192, 8192);
	weston_view_set_position(view, 0, 0);
	weston_surface_set_color(surface, 0.0, 0.0, 0.0, 1.0);
	weston_layer_entry_insert(&compositor->fade_layer.view_list,
				  &view->layer_link);
	pixman_region32_init(&surface->input);

	return view;
}

static void shell_fade(struct mayhem_shell *shell, enum fade_type type)
{
	float tint;

	switch (type) {
	case FADE_IN:
		tint = 0.0;
		break;
	case FADE_OUT:
		tint = 1.0;
		break;
	default:
		weston_log("shell: invalid fade type\n");
		return;
	}

	shell->fade.type = type;

	if (shell->fade.view == NULL) {
		shell->fade.view = shell_fade_create_surface(shell);
		if (!shell->fade.view)
			return;

		shell->fade.view->alpha = 1.0 - tint;
		weston_view_update_transform(shell->fade.view);
	}

	if (shell->fade.view->output == NULL) {
		/* If the black view gets a NULL output, we lost the
		 * last output and we'll just cancel the fade.  This
		 * happens when you close the last window under the
		 * X11 or Wayland backends. */
		weston_surface_destroy(shell->fade.view->surface);
		shell->fade.view = NULL;
	} else if (shell->fade.animation) {
		weston_fade_update(shell->fade.animation, tint);
	} else {
		shell->fade.animation =
			weston_fade_run(shell->fade.view,
					1.0 - tint, tint, 300.0,
					shell_fade_done, shell);
	}
}

static void
do_shell_fade_startup(void *data)
{
	struct mayhem_shell *shell = data;

	if (shell->startup_animation_type == ANIMATION_FADE)
		shell_fade(shell, FADE_IN);
	else if (shell->startup_animation_type == ANIMATION_NONE) {
		weston_surface_destroy(shell->fade.view->surface);
		shell->fade.view = NULL;
	}
}

static void
shell_fade_startup(struct mayhem_shell *shell)
{
	struct wl_event_loop *loop;

	if (!shell->fade.startup_timer)
		return;

	wl_event_source_remove(shell->fade.startup_timer);
	shell->fade.startup_timer = NULL;

	loop = wl_display_get_event_loop(shell->compositor->wl_display);
	wl_event_loop_add_idle(loop, do_shell_fade_startup, shell);
}

static int
fade_startup_timeout(void *data)
{
	struct mayhem_shell *shell = data;

	shell_fade_startup(shell);
	return 0;
}

static void
shell_fade_init(struct mayhem_shell *shell)
{
	/* Make compositor output all black, and wait for the desktop-shell
	 * client to signal it is ready, then fade in. The timer triggers a
	 * fade-in, in case the desktop-shell client takes too long.
	 */

	struct wl_event_loop *loop;

	if (shell->fade.view != NULL) {
		weston_log("%s: warning: fade surface already exists\n",
			   __func__);
		return;
	}

	shell->fade.view = shell_fade_create_surface(shell);
	if (!shell->fade.view)
		return;

	weston_view_update_transform(shell->fade.view);
	weston_surface_damage(shell->fade.view->surface);

	loop = wl_display_get_event_loop(shell->compositor->wl_display);
	shell->fade.startup_timer =
		wl_event_loop_add_timer(loop, fade_startup_timeout, shell);
	wl_event_source_timer_update(shell->fade.startup_timer, 15000);
}

static void
idle_handler(struct wl_listener *listener, void *data)
{
	struct mayhem_shell *shell =
		container_of(listener, struct mayhem_shell, idle_listener);
	struct weston_seat *seat;

	wl_list_for_each(seat, &shell->compositor->seat_list, link) {
		struct weston_pointer *pointer = weston_seat_get_pointer(seat);

		if (pointer)
			popup_grab_end(pointer);
	}

	shell_fade(shell, FADE_OUT);
	/* lock() is called from shell_fade_done() */
}

static void
wake_handler(struct wl_listener *listener, void *data)
{
	struct mayhem_shell *shell =
		container_of(listener, struct mayhem_shell, wake_listener);

	shell_fade(shell, FADE_IN);
}

static void
transform_handler(struct wl_listener *listener, void *data)
{
	struct weston_surface *surface = data;
	struct shell_surface *shsurf = get_shell_surface(surface);
	struct weston_view *view;;
	int x, y;

	if (!shsurf || !shsurf->client->send_position)
		return;

	view = shsurf->view;
	if (!view || !weston_view_is_mapped(view))
		return;

	x = view->geometry.x;
	y = view->geometry.y;

	shsurf->client->send_position(surface, x, y);
}


static void
center_on_output(struct weston_view *view, struct weston_output *output)
{
	int32_t surf_x, surf_y, width, height;
	float x, y;

	surface_subsurfaces_boundingbox(view->surface, &surf_x, &surf_y, &width, &height);

	x = output->x + (output->width - width) / 2 - surf_x / 2;
	y = output->y + (output->height - height) / 2 - surf_y / 2;

	weston_view_set_position(view, x, y);
}

static void weston_view_set_initial_position(struct weston_view *view,
					     struct mayhem_shell *shell)
{
	struct weston_compositor *compositor = shell->compositor;
	struct weston_seat *seat;
	int x = 0, y = 0;

	wl_list_for_each(seat, &compositor->seat_list, link) {
		struct weston_pointer *pointer = weston_seat_get_pointer(seat);

		if (pointer) {
			x = wl_fixed_to_int(pointer->x);
			y = wl_fixed_to_int(pointer->y);
			break;
		}
	}
	weston_view_set_position(view, x-20, y-20);
}

static void set_maximized_position(struct mayhem_shell *shell,
				   struct shell_surface *shsurf)
{
	int32_t surf_x, surf_y;

	if (shsurf->has_set_geometry) {
		surf_x = shsurf->geometry.x;
		surf_y = shsurf->geometry.y;
	} else {
		surface_subsurfaces_boundingbox(shsurf->surface, &surf_x,
						&surf_y, NULL, NULL);
	}

	weston_view_set_position(shsurf->view, surf_x, surf_y);
}

static void map(struct mayhem_shell *shell, struct shell_surface *shsurf,
		int32_t sx, int32_t sy)
{
	struct weston_compositor *compositor = shell->compositor;
	struct weston_seat *seat;

	/* initial positioning, see also configure() */
	switch (shsurf->type) {
	case SHELL_SURFACE_TOPLEVEL:
		if (shsurf->state.fullscreen) {
			center_on_output(shsurf->view, shsurf->fullscreen_output);
			shell_map_fullscreen(shsurf);
		} else if (shsurf->state.maximized) {
			set_maximized_position(shell, shsurf);
		} else if (!shsurf->state.relative) {
			weston_view_set_initial_position(shsurf->view, shell);
		}
		break;
	case SHELL_SURFACE_POPUP:
		if (shell_map_popup(shsurf) != 0)
			return;
		break;
	case SHELL_SURFACE_MENU:
	case SHELL_SURFACE_NONE:
		weston_view_set_position(shsurf->view,
					 shsurf->view->geometry.x + sx,
					 shsurf->view->geometry.y + sy);
		break;
	case SHELL_SURFACE_XWAYLAND:
	default:
		;
	}

	/* Surface stacking order, see also activate(). */
	shell_surface_update_layer(shsurf);

	if (shsurf->type != SHELL_SURFACE_NONE) {
		weston_view_update_transform(shsurf->view);
		if (shsurf->state.maximized) {
			shsurf->surface->output = shsurf->output;
			shsurf->view->output = shsurf->output;
		}
	}

	switch (shsurf->type) {
	/* XXX: xwayland's using the same fields for transient type */
	case SHELL_SURFACE_XWAYLAND:
		if (shsurf->transient.flags ==
				WL_SHELL_SURFACE_TRANSIENT_INACTIVE)
			break;
	case SHELL_SURFACE_TOPLEVEL:
	case SHELL_SURFACE_MENU:
		if (shsurf->state.relative &&
		    shsurf->transient.flags == WL_SHELL_SURFACE_TRANSIENT_INACTIVE)
			break;
		wl_list_for_each(seat, &compositor->seat_list, link)
			activate(shell, shsurf->surface, seat, true);
		break;
	case SHELL_SURFACE_POPUP:
	case SHELL_SURFACE_NONE:
	default:
		break;
	}

	if (shsurf->type == SHELL_SURFACE_TOPLEVEL &&
	    !shsurf->state.maximized && !shsurf->state.fullscreen)
	{
		switch (shell->win_animation_type) {
		case ANIMATION_FADE:
			weston_fade_run(shsurf->view, 0.0, 1.0, 300.0, NULL, NULL);
			break;
		case ANIMATION_ZOOM:
			weston_zoom_run(shsurf->view, 0.5, 1.0, NULL, NULL);
			break;
		case ANIMATION_NONE:
		default:
			break;
		}
	}
}

static void
configure(struct mayhem_shell *shell, struct weston_surface *surface,
	  float x, float y)
{
	struct shell_surface *shsurf;
	struct weston_view *view;

	shsurf = get_shell_surface(surface);

	assert(shsurf);

	if (shsurf->state.fullscreen)
		shell_configure_fullscreen(shsurf);
	else if (shsurf->state.maximized) {
		set_maximized_position(shell, shsurf);
	} else {
		weston_view_set_position(shsurf->view, x, y);
	}

	/* XXX: would a fullscreen surface need the same handling? */
	if (surface->output) {
		wl_list_for_each(view, &surface->views, surface_link)
			weston_view_update_transform(view);

		if (shsurf->state.maximized)
			surface->output = shsurf->output;
	}
}

static void
shell_surface_configure(struct weston_surface *es, int32_t sx, int32_t sy)
{
	struct shell_surface *shsurf = get_shell_surface(es);
	struct mayhem_shell *shell;
	int type_changed = 0;

	assert(shsurf);

	shell = shsurf->shell;

	if (!weston_surface_is_mapped(es) &&
	    !wl_list_empty(&shsurf->popup.grab_link)) {
		remove_popup_grab(shsurf);
	}

	if (es->width == 0)
		return;

	if (shsurf->has_next_geometry) {
		shsurf->geometry = shsurf->next_geometry;
		shsurf->has_next_geometry = false;
		shsurf->has_set_geometry = true;
	} else if (!shsurf->has_set_geometry) {
		surface_subsurfaces_boundingbox(shsurf->surface,
						&shsurf->geometry.x,
						&shsurf->geometry.y,
						&shsurf->geometry.width,
						&shsurf->geometry.height);
	}

	if (shsurf->state_changed) {
		set_surface_type(shsurf);
		type_changed = 1;
	}

	if (!weston_surface_is_mapped(es)) {
		map(shell, shsurf, sx, sy);
	} else if (type_changed || sx != 0 || sy != 0 ||
		   shsurf->last_width != es->width ||
		   shsurf->last_height != es->height) {
		float from_x, from_y;
		float to_x, to_y;

		if (shsurf->resize_edges) {
			sx = 0;
			sy = 0;
		}

		if (shsurf->resize_edges & WL_SHELL_SURFACE_RESIZE_LEFT)
			sx = shsurf->last_width - es->width;
		if (shsurf->resize_edges & WL_SHELL_SURFACE_RESIZE_TOP)
			sy = shsurf->last_height - es->height;

		shsurf->last_width = es->width;
		shsurf->last_height = es->height;

		weston_view_to_global_float(shsurf->view, 0, 0, &from_x, &from_y);
		weston_view_to_global_float(shsurf->view, sx, sy, &to_x, &to_y);
		configure(shell, es,
			  shsurf->view->geometry.x + to_x - from_x,
			  shsurf->view->geometry.y + to_y - from_y);
	}
}

static bool
check_mayhem_shell_crash_too_early(struct mayhem_shell *shell)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return false;

	/*
	 * If the shell helper client dies before the session has been
	 * up for roughly 30 seconds, better just make Weston shut down,
	 * because the user likely has no way to interact with the desktop
	 * anyway.
	 */
	if (now.tv_sec - shell->startup_time.tv_sec < 30) {
		weston_log("Error: %s apparently cannot run at all.\n",
			   shell->client);
		//weston_log_continue(STAMP_SPACE "Quitting...");
		//wl_display_terminate(shell->compositor->wl_display);

		return true;
	}

	return false;
}

static void launch_mayhem_shell_process(void *data);

static void
respawn_mayhem_shell_process(struct mayhem_shell *shell)
{
	uint32_t time;

	/* if desktop-shell dies more than 5 times in 30 seconds, give up */
	time = weston_compositor_get_time();
	if (time - shell->child.deathstamp > 30000) {
		shell->child.deathstamp = time;
		shell->child.deathcount = 0;
	}

	shell->child.deathcount++;
	if (shell->child.deathcount > 5) {
		weston_log("%s disconnected, giving up.\n", shell->client);
		return;
	}

	weston_log("%s disconnected, respawning...\n", shell->client);
	launch_mayhem_shell_process(shell);
}

static void
mayhem_shell_client_destroy(struct wl_listener *listener, void *data)
{
	struct mayhem_shell *shell;

	shell = container_of(listener, struct mayhem_shell,
			     child.client_destroy_listener);

	wl_list_remove(&shell->child.client_destroy_listener.link);
	shell->child.client = NULL;
	/*
	 * unbind_ms_menu() will reset shell->child.mayhem_shell
	 * before the respawned process has a chance to create a new
	 * mayhem_shell object, because we are being called from the
	 * wl_client destructor which destroys all wl_resources before
	 * returning.
	 */

	if (!check_mayhem_shell_crash_too_early(shell))
		respawn_mayhem_shell_process(shell);

	shell_fade_startup(shell);
}

static void
launch_mayhem_shell_process(void *data)
{
	struct mayhem_shell *shell = data;

	shell->child.client = weston_client_start(shell->compositor,
						  shell->client);

	if (!shell->child.client) {
		weston_log("not able to start %s\n", shell->client);
		return;
	}

	shell->child.client_destroy_listener.notify =
		mayhem_shell_client_destroy;
	wl_client_add_destroy_listener(shell->child.client,
				       &shell->child.client_destroy_listener);
}

static void
handle_shell_client_destroy(struct wl_listener *listener, void *data)
{
	struct shell_client *sc =
		container_of(listener, struct shell_client, destroy_listener);

	if (sc->ping_timer)
		wl_event_source_remove(sc->ping_timer);

	//wl_list_remove(&sc->surface_list);
	free(sc);
}

static struct shell_client *
shell_client_create(struct wl_client *client, struct mayhem_shell *shell,
		    const struct wl_interface *interface, uint32_t id)
{
	struct shell_client *sc;

	sc = zalloc(sizeof *sc);
	if (sc == NULL) {
		wl_client_post_no_memory(client);
		return NULL;
	}

	sc->resource = wl_resource_create(client, interface, 1, id);
	if (sc->resource == NULL) {
		free(sc);
		wl_client_post_no_memory(client);
		return NULL;
	}

	sc->client = client;
	sc->shell = shell;
	sc->destroy_listener.notify = handle_shell_client_destroy;
	wl_client_add_destroy_listener(client, &sc->destroy_listener);

	return sc;
}

static void
bind_wl_shell(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct mayhem_shell *shell = data;
	struct shell_client *sc;

	sc = shell_client_create(client, shell, &wl_shell_interface, id);
	if (sc)
		wl_resource_set_implementation(sc->resource,
					       &shell_implementation,
					       sc, NULL);
}

static void bind_xdg_shell(struct wl_client *client, void *data,
			   uint32_t version, uint32_t id)
{
	struct mayhem_shell *shell = data;
	struct shell_client *sc;

	sc = shell_client_create(client, shell, &xdg_shell_interface, id);
	if (sc)
		wl_resource_set_dispatcher(sc->resource,
					   xdg_shell_unversioned_dispatch,
					   NULL, sc, NULL);
}

static void unbind_ms_menu(struct wl_resource *resource)
{
	struct mayhem_shell *shell = wl_resource_get_user_data(resource);

	printf("unbinding ms menu");

	shell->child.mayhem_shell = NULL;
	shell->prepare_event_sent = false;
}

static void bind_ms_menu(struct wl_client *client, void *data,
			 uint32_t version, uint32_t id)
{
	struct mayhem_shell *shell = data;
	struct shell_client *sc;

	sc = shell_client_create(client, shell, &ms_menu_interface, id);

	printf("binding ms menu");
	// allow only our special client to use ms_menu interface
	if (client == shell->child.client || 1) {
		wl_resource_set_implementation(sc->resource,
					       &ms_menu_implementation,
					       sc, &unbind_ms_menu);
		shell->child.mayhem_shell = sc->resource;
		shell_fade_startup(shell);

		return;
	}

	wl_resource_post_error(sc->resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			       "permission to bind mayhem_shell denied");
}

struct switcher {
	struct mayhem_shell *shell;
	struct weston_surface *current;
	struct wl_listener listener;
	struct weston_keyboard_grab grab;
	struct wl_array minimized_array;
};

static void switcher_next(struct switcher *switcher)
{
	struct weston_view *view;
	struct weston_surface *first = NULL, *prev = NULL, *next = NULL;
	struct shell_surface *shsurf;
	struct workspace *ws = get_current_workspace(switcher->shell);

	 /* temporary re-display minimized surfaces */
	struct weston_view *tmp;
	struct weston_view **minimized;
	wl_list_for_each_safe(view, tmp, &switcher->shell->minimized_layer.view_list.link, layer_link.link) {
		weston_layer_entry_remove(&view->layer_link);
		weston_layer_entry_insert(&ws->layer.view_list, &view->layer_link);
		minimized = wl_array_add(&switcher->minimized_array, sizeof *minimized);
		*minimized = view;
	}

	wl_list_for_each(view, &ws->layer.view_list.link, layer_link.link) {
		shsurf = get_shell_surface(view->surface);
		if (shsurf &&
		    shsurf->type == SHELL_SURFACE_TOPLEVEL &&
		    shsurf->parent == NULL) {
			if (first == NULL)
				first = view->surface;
			if (prev == switcher->current)
				next = view->surface;
			prev = view->surface;
			view->alpha = 0.25;
			weston_view_geometry_dirty(view);
			weston_surface_damage(view->surface);
		}
	}

	if (next == NULL)
		next = first;

	if (next == NULL)
		return;

	wl_list_remove(&switcher->listener.link);
	wl_signal_add(&next->destroy_signal, &switcher->listener);

	switcher->current = next;
	wl_list_for_each(view, &next->views, surface_link)
		view->alpha = 1.0;

	shsurf = get_shell_surface(switcher->current);
}

static void
switcher_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct switcher *switcher =
		container_of(listener, struct switcher, listener);

	switcher_next(switcher);
}

static void switcher_destroy(struct switcher *switcher)
{
	struct weston_view *view;
	struct weston_keyboard *keyboard = switcher->grab.keyboard;
	struct workspace *ws = get_current_workspace(switcher->shell);

	wl_list_for_each(view, &ws->layer.view_list.link, layer_link.link) {
		if (is_focus_view(view))
			continue;

		view->alpha = 1.0;
		weston_surface_damage(view->surface);
	}

	if (switcher->current)
		activate(switcher->shell, switcher->current,
			 keyboard->seat, true);
	wl_list_remove(&switcher->listener.link);
	weston_keyboard_end_grab(keyboard);
	if (keyboard->input_method_resource)
		keyboard->grab = &keyboard->input_method_grab;

	 /* re-hide surfaces that were temporary shown during the switch */
	struct weston_view **minimized;
	wl_array_for_each(minimized, &switcher->minimized_array) {
		/* with the exception of the current selected */
		if ((*minimized)->surface != switcher->current) {
			weston_layer_entry_remove(&(*minimized)->layer_link);
			weston_layer_entry_insert(&switcher->shell->minimized_layer.view_list, &(*minimized)->layer_link);
			weston_view_damage_below(*minimized);
		}
	}
	wl_array_release(&switcher->minimized_array);

	free(switcher);
}

static void
switcher_key(struct weston_keyboard_grab *grab,
	     uint32_t time, uint32_t key, uint32_t state_w)
{
	struct switcher *switcher = container_of(grab, struct switcher, grab);
	enum wl_keyboard_key_state state = state_w;

	if (key == KEY_TAB && state == WL_KEYBOARD_KEY_STATE_PRESSED)
		switcher_next(switcher);
}

static void
switcher_modifier(struct weston_keyboard_grab *grab, uint32_t serial,
		  uint32_t mods_depressed, uint32_t mods_latched,
		  uint32_t mods_locked, uint32_t group)
{
	struct switcher *switcher = container_of(grab, struct switcher, grab);
	struct weston_seat *seat = grab->keyboard->seat;

	if ((seat->modifier_state & switcher->shell->binding_modifier) == 0)
		switcher_destroy(switcher);
}

static void
switcher_cancel(struct weston_keyboard_grab *grab)
{
	struct switcher *switcher = container_of(grab, struct switcher, grab);

	switcher_destroy(switcher);
}

static const struct weston_keyboard_grab_interface switcher_grab = {
	switcher_key,
	switcher_modifier,
	switcher_cancel,
};

static void switcher_binding(struct weston_keyboard *keyboard, uint32_t time,
			     uint32_t key, void *data)
{
	struct mayhem_shell *shell = data;
	struct switcher *switcher;

	switcher = malloc(sizeof *switcher);
	switcher->shell = shell;
	switcher->current = NULL;
	switcher->listener.notify = switcher_handle_surface_destroy;
	wl_list_init(&switcher->listener.link);
	wl_array_init(&switcher->minimized_array);

	restore_all_output_modes(shell->compositor);
	//lower_fullscreen_layer(switcher->shell);
	switcher->grab.interface = &switcher_grab;
	weston_keyboard_start_grab(keyboard, &switcher->grab);
	weston_keyboard_set_focus(keyboard, NULL);
	switcher_next(switcher);
}

static void force_kill_binding(struct weston_keyboard *keyboard, uint32_t time,
			       uint32_t key, void *data)
{
	struct weston_surface *focus_surface;
	struct wl_client *client;
	struct mayhem_shell *shell = data;
	struct weston_compositor *compositor = shell->compositor;
	pid_t pid;

	focus_surface = keyboard->focus;
	if (!focus_surface)
		return;

	wl_signal_emit(&compositor->kill_signal, focus_surface);

	client = wl_resource_get_client(focus_surface->resource);
	wl_client_get_credentials(client, &pid, NULL, NULL);

	/* Skip clients that we launched ourselves (the credentials of
	 * the socketpair is ours) */
	if (pid == getpid())
		return;

	kill(pid, SIGKILL);
}

static void workspace_up_binding(struct weston_keyboard *keyboard, uint32_t time,
				 uint32_t key, void *data)
{
	struct mayhem_shell *shell = data;
	unsigned int new_index = shell->workspaces.current;

	if (new_index != 0)
		new_index--;

	change_workspace(shell, new_index);
}

static void workspace_down_binding(struct weston_keyboard *keyboard, uint32_t time,
				   uint32_t key, void *data)
{
	struct mayhem_shell *shell = data;
	unsigned int new_index = shell->workspaces.current;

	if (new_index < shell->workspaces.num - 1)
		new_index++;

	change_workspace(shell, new_index);
}

static void workspace_axis_binding(struct weston_pointer *pointer,
				   uint32_t time,
				   struct weston_pointer_axis_event *event,
				   void *data)
{
	struct mayhem_shell *shell = data;
	int new_index = shell->workspaces.current;

	new_index += event->value>0?1:-1;
	if(new_index < 0)
		new_index = shell->workspaces.num-1;
	else if((unsigned)new_index >= shell->workspaces.num)
		new_index = 0;

	change_workspace(shell, new_index);
}

static void workspace_f_binding(struct weston_keyboard *keyboard, uint32_t time,
				uint32_t key, void *data)
{
	struct mayhem_shell *shell = data;
	unsigned int new_index;

	new_index = key - KEY_F1;
	if (new_index >= shell->workspaces.num)
		new_index = shell->workspaces.num - 1;

	change_workspace(shell, new_index);
}

static void
workspace_move_surface_up_binding(struct weston_keyboard *keyboard, uint32_t time,
				  uint32_t key, void *data)
{
	struct mayhem_shell *shell = data;
	unsigned int new_index = shell->workspaces.current;

	if (new_index != 0)
		new_index--;

	take_surface_to_workspace_by_seat(shell, keyboard->seat, new_index);
}

static void workspace_move_surface_down_binding(struct weston_keyboard *keyboard,
						uint32_t time, uint32_t key,
						void *data)
{
	struct mayhem_shell *shell = data;
	unsigned int new_index = shell->workspaces.current;

	if (new_index < shell->workspaces.num - 1)
		new_index++;

	take_surface_to_workspace_by_seat(shell, keyboard->seat, new_index);
}

static void show_menu_binding(struct weston_keyboard *keyboard, uint32_t time,
			      uint32_t key, void *data)
{
	struct mayhem_shell *shell = data;

	ms_menu_send_spawn(shell->child.mayhem_shell, 0, 0);
}

static void shell_reposition_view_on_output_destroy(struct weston_view *view)
{
	struct weston_output *output, *first_output;
	struct weston_compositor *ec = view->surface->compositor;
	struct shell_surface *shsurf;
	float x, y;
	int visible;

	x = view->geometry.x;
	y = view->geometry.y;

	/* At this point the destroyed output is not in the list anymore.
	 * If the view is still visible somewhere, we leave where it is,
	 * otherwise, move it to the first output. */
	visible = 0;
	wl_list_for_each(output, &ec->output_list, link) {
		if (pixman_region32_contains_point(&output->region,
						   x, y, NULL)) {
			visible = 1;
			break;
		}
	}

	if (!visible) {
		first_output = container_of(ec->output_list.next,
					    struct weston_output, link);

		x = first_output->x + first_output->width / 4;
		y = first_output->y + first_output->height / 4;

		weston_view_set_position(view, x, y);
	} else {
		weston_view_geometry_dirty(view);
	}


	shsurf = get_shell_surface(view->surface);

	if (shsurf) {
		shsurf->saved_position_valid = false;
		shsurf->next_state.maximized = false;
		shsurf->next_state.fullscreen = false;
		shsurf->state_changed = true;
	}
}

void shell_for_each_layer(struct mayhem_shell *shell,
			  shell_for_each_layer_func_t func, void *data)
{
	struct workspace **ws;

	func(shell, &shell->background_layer, data);

	wl_array_for_each(ws, &shell->workspaces.array)
		func(shell, &(*ws)->layer, data);
}

static void shell_output_destroy_move_layer(struct mayhem_shell *shell,
					    struct weston_layer *layer,
					    void *data)
{
	struct weston_output *output = data;
	struct weston_view *view;

	wl_list_for_each(view, &layer->view_list.link, layer_link.link) {
		if (view->output != output)
			continue;

		shell_reposition_view_on_output_destroy(view);
	}
}

static void handle_output_destroy(struct wl_listener *listener, void *data)
{
	struct shell_output *output_listener =
		container_of(listener, struct shell_output, destroy_listener);
	struct weston_output *output = output_listener->output;
	struct mayhem_shell *shell = output_listener->shell;

	shell_for_each_layer(shell, shell_output_destroy_move_layer, output);

	wl_list_remove(&output_listener->destroy_listener.link);
	wl_list_remove(&output_listener->link);
	free(output_listener);
}

static void create_shell_output(struct mayhem_shell *shell,
				struct weston_output *output)
{
	struct shell_output *shell_output;

	shell_output = zalloc(sizeof *shell_output);
	if (shell_output == NULL)
		return;

	shell_output->output = output;
	shell_output->shell = shell;
	shell_output->destroy_listener.notify = handle_output_destroy;
	wl_signal_add(&output->destroy_signal,
		      &shell_output->destroy_listener);
	wl_list_insert(shell->output_list.prev, &shell_output->link);
}

static void handle_output_create(struct wl_listener *listener, void *data)
{
	struct mayhem_shell *shell =
		container_of(listener, struct mayhem_shell, output_create_listener);
	struct weston_output *output = (struct weston_output *)data;

	create_shell_output(shell, output);
}

static void handle_output_move_layer(struct mayhem_shell *shell,
				     struct weston_layer *layer, void *data)
{
	struct weston_output *output = data;
	struct weston_view *view;
	float x, y;

	wl_list_for_each(view, &layer->view_list.link, layer_link.link) {
		if (view->output != output)
			continue;

		x = view->geometry.x + output->move_x;
		y = view->geometry.y + output->move_y;
		weston_view_set_position(view, x, y);
	}
}

static void handle_output_move(struct wl_listener *listener, void *data)
{
	struct mayhem_shell *shell;

	shell = container_of(listener, struct mayhem_shell,
			     output_move_listener);

	shell_for_each_layer(shell, handle_output_move_layer, data);
}

static void setup_output_destroy_handler(struct weston_compositor *ec,
					 struct mayhem_shell *shell)
{
	struct weston_output *output;

	wl_list_init(&shell->output_list);
	wl_list_for_each(output, &ec->output_list, link)
		create_shell_output(shell, output);

	shell->output_create_listener.notify = handle_output_create;
	wl_signal_add(&ec->output_created_signal,
				&shell->output_create_listener);

	shell->output_move_listener.notify = handle_output_move;
	wl_signal_add(&ec->output_moved_signal, &shell->output_move_listener);
}

static void shell_destroy(struct wl_listener *listener, void *data)
{
	struct mayhem_shell *shell =
		container_of(listener, struct mayhem_shell, destroy_listener);
	struct workspace **ws;
	struct shell_output *shell_output, *tmp;

	/* Force state to unlocked so we don't try to fade */

	if (shell->child.client) {
		/* disable respawn */
		wl_list_remove(&shell->child.client_destroy_listener.link);
		wl_client_destroy(shell->child.client);
	}

	wl_list_remove(&shell->idle_listener.link);
	wl_list_remove(&shell->wake_listener.link);
	wl_list_remove(&shell->transform_listener.link);

	wl_list_for_each_safe(shell_output, tmp, &shell->output_list, link) {
		wl_list_remove(&shell_output->destroy_listener.link);
		wl_list_remove(&shell_output->link);
		free(shell_output);
	}

	wl_list_remove(&shell->output_create_listener.link);
	wl_list_remove(&shell->output_move_listener.link);

	wl_array_for_each(ws, &shell->workspaces.array)
		workspace_destroy(*ws);
	wl_array_release(&shell->workspaces.array);

	free(shell->client);
	free(shell);
}

static void shell_add_bindings(struct weston_compositor *ec,
			       struct mayhem_shell *shell)
{
	uint32_t mod;
	int i, num_workspace_bindings;

	/* fixed bindings */
	weston_compositor_add_key_binding(ec, KEY_BACKSPACE,
				          MODIFIER_CTRL | MODIFIER_ALT,
				          terminate_binding, ec);
	weston_compositor_add_button_binding(ec, BTN_LEFT, 0,
					     click_to_activate_binding,
					     shell);
	weston_compositor_add_button_binding(ec, BTN_RIGHT, 0,
					     click_to_activate_binding,
					     shell);
	weston_compositor_add_axis_binding(ec, WL_POINTER_AXIS_VERTICAL_SCROLL,
				           MODIFIER_SUPER | MODIFIER_CTRL,
				           surface_opacity_binding, NULL);
	weston_compositor_add_axis_binding(ec, WL_POINTER_AXIS_VERTICAL_SCROLL,
					   MODIFIER_SUPER | MODIFIER_ALT,
					   zoom_axis_binding, NULL);
	weston_compositor_add_axis_binding(ec, WL_POINTER_AXIS_VERTICAL_SCROLL,
					   MODIFIER_SUPER, workspace_axis_binding,
					   shell);

	/* configurable bindings */
	mod = shell->binding_modifier;
	weston_compositor_add_key_binding(ec, KEY_ENTER, mod, exec_binding,
					  "weston-terminal");
	weston_compositor_add_key_binding(ec, KEY_PAGEUP, mod,
					  zoom_key_binding, NULL);
	weston_compositor_add_key_binding(ec, KEY_PAGEDOWN, mod,
					  zoom_key_binding, NULL);
	weston_compositor_add_key_binding(ec, KEY_M, mod,
					  maximize_binding, NULL);
	weston_compositor_add_key_binding(ec, KEY_F, mod,
					  fullscreen_binding, NULL);

	weston_compositor_add_button_binding(ec, BTN_LEFT, mod,
					     move_binding, shell);
	weston_compositor_add_button_binding(ec, BTN_MIDDLE, mod,
					     rotate_binding, shell);
	weston_compositor_add_button_binding(ec, BTN_RIGHT, mod,
					     resize_binding, NULL);

	weston_compositor_add_key_binding(ec, KEY_TAB, mod, switcher_binding,
					  shell);
	weston_compositor_add_key_binding(ec, KEY_K, mod,
				          force_kill_binding, shell);
	weston_compositor_add_key_binding(ec, KEY_UP, mod,
					  workspace_up_binding, shell);
	weston_compositor_add_key_binding(ec, KEY_DOWN, mod,
					  workspace_down_binding, shell);
	weston_compositor_add_key_binding(ec, KEY_UP, mod | MODIFIER_SHIFT,
					  workspace_move_surface_up_binding,
					  shell);
	weston_compositor_add_key_binding(ec, KEY_DOWN, mod | MODIFIER_SHIFT,
					  workspace_move_surface_down_binding,
					  shell);
	weston_compositor_add_key_binding(ec, KEY_SPACE, mod | MODIFIER_SHIFT,
					  show_menu_binding,
					  shell);


	/* Add bindings for mod+F[1-6] for workspace 1 to 6. */
	if (shell->workspaces.num > 1) {
		num_workspace_bindings = shell->workspaces.num;
		if (num_workspace_bindings > 7)
			num_workspace_bindings = 7;
		for (i = 0; i < num_workspace_bindings; i++)
			weston_compositor_add_key_binding(ec, KEY_F1 + i, mod,
							  workspace_f_binding,
							  shell);
	}

	weston_install_debug_key_binding(ec, mod);
}

static void handle_seat_created(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat = data;
	create_shell_seat(seat);
}

WL_EXPORT int module_init(struct weston_compositor *ec, int *argc, char *argv[])
{
	struct weston_seat *seat;
	struct mayhem_shell *shell;
	struct workspace **pws;
	unsigned int i;
	struct wl_event_loop *loop;

	shell = zalloc(sizeof *shell);
	if (shell == NULL)
		return -1;

	shell->compositor = ec;
	//printf("repeat: %i, delay: %i", ec->kb_repeat_rate, ec->kb_repeat_delay);

	shell->destroy_listener.notify = shell_destroy;
	wl_signal_add(&ec->destroy_signal, &shell->destroy_listener);
	shell->idle_listener.notify = idle_handler;
	wl_signal_add(&ec->idle_signal, &shell->idle_listener);
	shell->wake_listener.notify = wake_handler;
	wl_signal_add(&ec->wake_signal, &shell->wake_listener);
	shell->transform_listener.notify = transform_handler;
	wl_signal_add(&ec->transform_signal, &shell->transform_listener);

	ec->shell_interface.shell = shell;
	ec->shell_interface.create_shell_surface = create_shell_surface;
	ec->shell_interface.set_toplevel = set_toplevel;
	ec->shell_interface.set_transient = set_transient;
	ec->shell_interface.set_fullscreen = shell_interface_set_fullscreen;
	ec->shell_interface.set_xwayland = set_xwayland;
	ec->shell_interface.move = shell_interface_move;
	ec->shell_interface.resize = shell_interface_resize;
	ec->shell_interface.set_title = set_title;
	ec->shell_interface.set_window_geometry = set_window_geometry;
	ec->shell_interface.set_maximized = shell_interface_set_maximized;
	ec->shell_interface.set_pid = set_pid;

	weston_layer_init(&shell->background_layer, &ec->cursor_layer.link);

	wl_array_init(&shell->workspaces.array);
	wl_list_init(&shell->workspaces.client_list);

	shell_configuration(shell);

	for (i = 0; i < shell->workspaces.num; i++) {
		pws = wl_array_add(&shell->workspaces.array, sizeof *pws);
		if (pws == NULL)
			return -1;

		*pws = workspace_create();
		if (*pws == NULL)
			return -1;
	}
	activate_workspace(shell, 0);

	weston_layer_init(&shell->minimized_layer, NULL);

	wl_list_init(&shell->workspaces.anim_sticky_list);
	wl_list_init(&shell->workspaces.animation.link);
	shell->workspaces.animation.frame = animate_workspace_change_frame;

	if (wl_global_create(ec->wl_display, &wl_shell_interface, 1,
				  shell, bind_wl_shell) == NULL)
		return -1;

	if (wl_global_create(ec->wl_display, &xdg_shell_interface, 1,
				  shell, bind_xdg_shell) == NULL)
		return -1;

	if(wl_global_create(ec->wl_display, &ms_menu_interface, 1,
			    shell, bind_ms_menu) == NULL)
		return -1;

	shell->child.deathstamp = weston_compositor_get_time();

	setup_output_destroy_handler(ec, shell);

	loop = wl_display_get_event_loop(ec->wl_display);
	wl_event_loop_add_idle(loop, launch_mayhem_shell_process, shell);

	wl_list_for_each(seat, &ec->seat_list, link)
		handle_seat_created(NULL, seat);
	shell->seat_create_listener.notify = handle_seat_created;
	wl_signal_add(&ec->seat_created_signal, &shell->seat_create_listener);

	screenshooter_create(ec);

	shell_add_bindings(ec, shell);

	shell_fade_init(shell);

	clock_gettime(CLOCK_MONOTONIC, &shell->startup_time);

	return 0;
}

