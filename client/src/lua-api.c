
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

static jmp_buf panic_env;
static struct menu *menu_global;
static lua_State *ls_global;

static int panic(lua_State *ls)
{
	longjmp(panic_env, 1);
}

static void *getself(lua_State *ls, char *mt, char *f)
{
	struct userdata *data = luaL_checkudata(ls, 1, mt);

	if(!data->valid)
		luaL_error(ls, "attempt to call method '%s' "
			   "on invalid object", f);

	return data->data;
}

static void destroy_userdata(void *data)
{
	struct userdata *le_data = (struct userdata *)data;

	le_data->valid = 0;
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

static int api_menu_show(lua_State *ls)
{
	struct frame *frame = getself(ls, META_MENU, "show");

	frame_show(frame);

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

	char *text = luaL_checkstring(ls, 2);

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

	luaL_checknumber(ls, 2);

	data = lua_newuserdata(ls, sizeof(*data));
	luaL_setmetatable(ls, META_BAR);
	data->valid = 1;
	data->data = item_bar_create(frame, destroy_userdata, data,
				     lua_tonumber(ls, 2));

	return 1;
}

static int api_base_spawn(lua_State *ls)
{
	struct userdata *data;

	data = lua_newuserdata(ls, sizeof(*data));
	luaL_setmetatable(ls, META_MENU);

	data->valid = 1;
	data->data = frame_create(menu_global, NULL, destroy_userdata, data);

	return 1;
}

static int api_base_close(lua_State *ls)
{
	menu_close(menu_global);

	return 0;
}

static int api_base_set_theme(lua_State *ls)
{
	struct theme* theme = menu_get_theme(menu_global);

	return 0;
}

static const luaL_Reg api_base[] = {
	{ "spawn_menu", api_base_spawn },
	{ "close", api_base_close },
	{ "set_theme", api_base_set_theme },
	{ 0, 0 }
};

static const luaL_Reg api_menu[] = {
	{ "show", api_menu_show },
	{ "close", api_menu_close },
	{ "set_theme", api_menu_set_theme },
	{ "add_text", api_menu_add_text },
	{ "add_bar", api_menu_add_bar },
	{ 0, 0 }
};

static const luaL_Reg api_text[] = {
	{ "set_text", api_text_set_text },
	{ 0, 0 }
};

static const luaL_Reg api_bar[] = {
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

int api_init(struct menu *menu)
{
	lua_State *ls = luaL_newstate();
	luaL_openlibs(ls);

	menu_global = menu;

	if(setjmp(panic_env))
		goto err;

	lua_atpanic(ls, panic);

	luaL_requiref(ls, MODULE_NAME, luaopen_mayhem, 1);
	lua_pop(ls, 1);

	if(luaL_loadfile(ls, "/home/murray/Desktop/try/meh.lua"))
		goto err;

	if(lua_pcall(ls, 0, 0, 0))
		goto err;

	lua_getglobal(ls, "main");
	if(lua_pcall(ls, 0, 0, 0))
		goto err;

	ls_global = ls;
	return 0;

err:
	if(lua_isstring(ls, -1))
		printf("Lua error: %s\n", lua_tostring(ls, -1));
	else
		printf("Error without message");

	menu_global = NULL;
	lua_close(ls);
	return -1;
}

void api_finish(void)
{
	menu_global = NULL;
	lua_close(ls_global);
}

