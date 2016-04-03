
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "api.h"

#define MODULE_NAME "mayhem"
#define META_MENU MODULE_NAME "_menu_mt"
#define META_TEXT MODULE_NAME "_text_mt"
#define META_BAR MODULE_NAME "_bar_mt"

struct userdata {
	int valid;
	void *data;
};

struct cb_data {
	lua_State *ls;
	int f;
};

static char const * const event_array[] = {
	"none",
	"open",
	"close",
	"enter",
	"leave",
	"click",
	NULL
};

#ifdef DEBUG
static void __attribute__((unused)) dump_stack(lua_State *ls)
{
	int i;
	int top = lua_gettop(ls);

	printf("| ");
	for(i = 1; i <= top; i++) {
		int t = lua_type(ls, i);

		switch(t) {
		case LUA_TSTRING:
			printf("\"%s\"", lua_tostring(ls, i));
			break;

		case LUA_TBOOLEAN:
			printf(lua_toboolean(ls, i)?"true":"false");
			break;

		case LUA_TNUMBER:
			printf("%g", lua_tonumber(ls, i));
			break;

		default:
			printf("%s", lua_typename(ls, t));
		}

		printf(" | ");
	}
	printf("\n");
}
#endif

static void throw_error(lua_State *ls)
{
	char const *e = lua_tostring(ls, -1);

	if(e)
		throw(e);
	else
		throw("Error without message");
}

static struct menu *getmenu(lua_State *ls)
{
	struct menu *menu;

	lua_pushlightuserdata(ls, ls);
	lua_rawget(ls, LUA_REGISTRYINDEX);
	menu = lua_touserdata(ls, -1);
	lua_pop(ls, 1);
	return menu;
}

static void *getself(lua_State *ls, char *mt, char *f)
{
	struct userdata *data = luaL_checkudata(ls, 1, mt);

	if(!data->valid)
		luaL_error(ls, "attempt to call method '%s' "
			   "on invalid object", f);

	return data->data;
}

static struct item *itemself(lua_State *ls, char *f)
{
	struct userdata *data = lua_touserdata(ls, 1);
	static char const *mt[] = {
		META_TEXT,
		META_BAR,
		NULL
	};
	int i = 0;

	if(!data || !data->valid)
		goto err;

	if(!lua_getmetatable(ls, 1))
		goto err;

	while(mt[i]) {
		luaL_getmetatable(ls, mt[i++]);
		if(lua_rawequal(ls, -1, -2)) {
			lua_pop(ls, 2);
			return data->data;
		}
		lua_pop(ls, 1);
	}
	lua_pop(ls, 1);

err:
	luaL_error(ls, "attempt to call method '%s' "
		   "on invalid object", f);
	return NULL; /* silence warning */
}

static void destroy_userdata(void *data)
{
	struct userdata *le_data = (struct userdata *)data;

	le_data->valid = 0;
}

static struct userdata *create_userdata(lua_State *ls, void *content,
					char const *mt)
{
	struct userdata *data;

	data = lua_newuserdata(ls, sizeof(*data));
	luaL_setmetatable(ls, mt);

	data->valid = 1;
	data->data = content;

	return data;
}

static void fire_open(void *cb_data, void *event)
{
	struct cb_data *cb = cb_data;
	struct frame *frame = ((struct ev_open *)event)->frame;
	struct userdata *data = create_userdata(cb->ls, frame, META_MENU);

	frame_init(frame, destroy_userdata, data);

	lua_rawgeti(cb->ls, LUA_REGISTRYINDEX, cb->f);
	lua_pushvalue(cb->ls, -2);

	if(lua_pcall(cb->ls, 1, 0, 0))
		throw_error(cb->ls);

	if(data->valid)
		frame_show(data->data);
	lua_pop(cb->ls, 1);
}

static void fire_close(void *data, void *event)
{
}

static void fire_enter(void *data, void *event)
{
	struct cb_data *cb = data;

	lua_rawgeti(cb->ls, LUA_REGISTRYINDEX, cb->f);
	if(lua_pcall(cb->ls, 0, 0, 0))
		throw_error(cb->ls);
}

static void fire_leave(void *data, void *event)
{
}

static void fire_click(void *data, void *event)
{
}

static void (*select_cb[])(void *, void *) = {
	NULL,
	fire_open,
	fire_close,
	fire_enter,
	fire_leave,
	fire_click,
};

static void destroy_event(void *data)
{
	struct cb_data *cb = data;

	luaL_unref(cb->ls, LUA_REGISTRYINDEX, cb->f);
	free(cb);
}

struct color {
	char *name;
	uint32_t val;
};

static uint32_t lookup_color(lua_State *ls, const char *s)
{
	static const struct color colors[] = {
		{ "white", 0xFFFFFFFF },
		{ "yellow", 0xFFFF00FF },
		{ 0, 0 }
	};
	uint32_t color;

	if(*s == '#') {
		char *p;

		color = strtol(++s, &p, 16);
		if(*p != '\0')
			luaL_error(ls, "Invalid color value");

		return color;
	} else {
		char *str = strdup(s);
		char *p = str;
		int i = 0;

		if(str == NULL)
			luaL_error(ls, "malloc fail");

		for(; *p; ++p)
			*p = tolower(*p);

		while(colors[i].name) {
			if(strcmp(str, colors[i].name) == 0) {
				color = colors[i].val;
				free(str);
				return color;
			}
			i++;
		}
		free(str);
		return luaL_error(ls, "Don't have color: %s", s);
	}
}

static void parse_color(lua_State *ls, char *name, uint32_t *dest)
{
	switch(lua_getfield(ls, 1, name)) {
	case LUA_TSTRING:
		if(lua_isnumber(ls, -1))
	case LUA_TNUMBER:
			*dest = lua_tonumber(ls, -1);
		else
			*dest = lookup_color(ls, lua_tostring(ls, -1));
	case LUA_TNIL:
		break;
	default:
		luaL_error(ls, "not a valid color");
	}
}

static int api_text_set_text(lua_State *ls)
{
	struct item_text *item = getself(ls, META_TEXT, "set_text");
	const char *text = luaL_checkstring(ls, 2);

	item_text_set_text(item, text);

	return 0;
}

static int api_bar_set_fill(lua_State *ls)
{
	struct item_bar *item = getself(ls, META_BAR, "set_fill");
	double fill = luaL_checknumber(ls, 2);
	fill /= 100;

	if(fill > 1.0)
		fill = 1.0;
	else if(fill < 0.0)
		fill = 0.0;

	item_bar_set_fill(item, fill);

	return 0;
}

static int api_item_on(lua_State *ls)
{
	struct cb_data *cb_data;
	int func_ref;
	struct item *item = itemself(ls, "on");
	enum event_type ev = luaL_checkoption(ls, 2, NULL, event_array);

	luaL_checktype(ls, 3, LUA_TFUNCTION);
	lua_pushvalue(ls, 3);
	func_ref = luaL_ref(ls, LUA_REGISTRYINDEX);

	cb_data = malloc(sizeof *cb_data);
	cb_data->ls = ls;
	cb_data->f = func_ref;

	item_register_event(item,
			    ev,
			    select_cb[ev],
			    destroy_event,
			    cb_data);
	return 0;
}

static int api_menu_close(lua_State *ls)
{
	struct frame *frame = getself(ls, META_MENU, "close");

	frame_destroy(frame);
	return 0;
}

static int api_menu_set_theme(lua_State *ls)
{

	return 0;
}

static int api_menu_add_text(lua_State *ls)
{
	struct userdata *data;
	struct frame *frame = getself(ls, META_MENU, "add_text");

	char const *text = luaL_checkstring(ls, 2);

	data = lua_newuserdata(ls, sizeof(*data));
	luaL_setmetatable(ls, META_TEXT);

	data->valid = 1;
	data->data = item_text_create(frame, destroy_userdata, data, text);

	return 1;
}

static int api_menu_add_bar(lua_State *ls)
{
	struct userdata *data;
	struct frame *frame = getself(ls, META_MENU, "add_bar");
	lua_Number n = luaL_checknumber(ls, 2);

	data = lua_newuserdata(ls, sizeof(*data));
	luaL_setmetatable(ls, META_BAR);
	data->valid = 1;
	data->data = item_bar_create(frame, destroy_userdata, data, n);

	return 1;
}

static int api_menu_on(lua_State *ls)
{
	struct cb_data *cb_data;
	int func_ref;
	struct frame *frame = getself(ls, META_MENU, "on");
	enum event_type ev = luaL_checkoption(ls, 2, NULL, event_array);

	luaL_checktype(ls, 3, LUA_TFUNCTION);
	lua_pushvalue(ls, 3);
	func_ref = luaL_ref(ls, LUA_REGISTRYINDEX);

	cb_data = malloc(sizeof *cb_data);
	cb_data->ls = ls;
	cb_data->f = func_ref;

	frame_register_event(frame,
			     ev,
			     select_cb[ev],
			     destroy_event,
			     cb_data);
	return 0;
}

static int api_menu_off(lua_State *ls)
{
	struct frame *frame = getself(ls, META_MENU, "off");
	enum event_type ev = luaL_checkoption(ls, 2, NULL, event_array);

	frame_remove_events(frame, ev);
	return 0;
}

static int api_base_set_theme(lua_State *ls)
{
	struct menu *menu = getmenu(ls);
	struct theme* theme = menu_get_theme(menu);

	luaL_checktype(ls, 1, LUA_TTABLE);

	parse_color(ls, "color", &theme->color);
	parse_color(ls, "bg_color_from", &theme->color_from);
	parse_color(ls, "bg_color_to", &theme->color_to);

	return 0;
}

static const luaL_Reg api_base[] = {
	{ "set_theme", api_base_set_theme },
	{ 0, 0 }
};

static const luaL_Reg api_menu[] = {
	{ "close", api_menu_close },
	{ "set_theme", api_menu_set_theme },
	{ "add_text", api_menu_add_text },
	{ "add_bar", api_menu_add_bar },
	{ "on", api_menu_on },
	{ "off", api_menu_off },
	{ 0, 0 }
};

#define api_item \
	{ "on", api_item_on }

static const luaL_Reg api_text[] = {
	api_item,
	{ "set_text", api_text_set_text },
	{ 0, 0 }
};

static const luaL_Reg api_bar[] = {
	api_item,
	{ "set_fill", api_bar_set_fill },
	{ 0, 0 }
};

static int luaopen_mayhem(lua_State *ls)
{
	luaL_newlib(ls, api_base);

	luaL_newmetatable(ls, META_MENU);
	lua_pushvalue(ls, -1);
	lua_setfield(ls, -2, "__index");
	luaL_setfuncs(ls, api_menu, 0);
	lua_pop(ls, 1);

	luaL_newmetatable(ls, META_TEXT);
	lua_pushvalue(ls, -1);
	lua_setfield(ls, -2, "__index");
	luaL_setfuncs(ls, api_text, 0);
	lua_pop(ls, 1);

	luaL_newmetatable(ls, META_BAR);
	lua_pushvalue(ls, -1);
	lua_setfield(ls, -2, "__index");
	luaL_setfuncs(ls, api_bar, 0);
	lua_pop(ls, 1);

	return 1;
}

static void api_finish(void *context)
{
	lua_close(context);
}

void *api_init(struct menu *menu, struct frame *frame, char const *file,
	       void **context)
{
	struct userdata *data;
	lua_State *ls = luaL_newstate();

	luaL_openlibs(ls);

	lua_pushlightuserdata(ls, menu);
	lua_rawsetp(ls, LUA_REGISTRYINDEX, ls);

	luaL_requiref(ls, MODULE_NAME, luaopen_mayhem, 1);
	lua_pop(ls, 1);

	if(luaL_loadfile(ls, file))
		goto err;

	if(lua_pcall(ls, 0, 0, 0))
		goto err;

	data = create_userdata(ls, frame, META_MENU);
	frame_init(data->data, destroy_userdata, data);
	lua_getglobal(ls, "main");
	lua_pushvalue(ls, -2); /* Stop gc */

	if(lua_pcall(ls, 1, 0, 0))
		goto err;

	if(data->valid)
		frame_show(data->data);
	lua_pop(ls, 1);

	*context = ls;
	return &api_finish;

err:
	throw_error(ls);

	lua_close(ls);
	return NULL;
}
