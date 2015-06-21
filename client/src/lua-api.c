
#include <setjmp.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "api.h"

#define MODULE_NAME "mayhem"
#define META_MENU MODULE_NAME "_menu_mt"
#define META_TEXT MODULE_NAME "_text_mt"

static jmp_buf panic_env;
static struct menu *menu_global;
static lua_State *ls_global;

static int panic(lua_State *ls)
{
	longjmp(panic_env, 1);
}

static int api_menu_show(lua_State *ls)
{
	struct frame **userdata = luaL_checkudata(ls, 1, META_MENU);
	struct frame *frame = *userdata;

	frame_show(frame);
	
	return 0;
}

static int api_menu_add_text(lua_State *ls)
{
	struct item **item;
	struct frame **userdata = luaL_checkudata(ls, 1, META_MENU);
	struct frame *frame = *userdata;

	char *text = luaL_checkstring(ls, 2);

	item = lua_newuserdata(ls, sizeof(struct item *));
	//luaL_setmetatable(ls, META_TEXT);

	*item = (struct item *)item_text_create(frame, text);

	return 1;
}

static int api_base_spawn(lua_State *ls)
{
	struct frame **frame;

	frame = lua_newuserdata(ls, sizeof(struct frame *));
	luaL_setmetatable(ls, META_MENU);
	
	*frame = frame_create(menu_global, NULL);

	return 1;
}

static const luaL_Reg base_api[] = {
	{ "spawn_menu", api_base_spawn },
	{ 0, 0 }
};

static const luaL_Reg api_menu[] = {
	{ "show", api_menu_show },
	{ "add_text", api_menu_add_text },
	{ 0, 0 }
};

static int register_base(lua_State *ls)
{
	luaL_newlib(ls, base_api);

	luaL_newmetatable(ls, META_MENU);
	lua_pushvalue(ls, -1);
	lua_setfield(ls, -2, "__index");
	luaL_setfuncs(ls, api_menu, 0);
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

	luaL_requiref(ls, MODULE_NAME, register_base, 1);
	lua_pop(ls, 1);

	if(luaL_loadfile(ls, "/home/murray/Desktop/try/meh.lua"))
		goto err;

	if(lua_pcall(ls, 0, 1, 0))
		goto err;

	lua_getglobal(ls, "main");
	if(lua_pcall(ls, 0, 0, 0))
		goto err;

	ls_global = ls;
	return 0;

err:
	if(lua_isstring(ls, -1))
		printf("%s\n", lua_tostring(ls, -1));
	else
		printf("Error without message");

	menu_global = NULL;
	lua_close(ls);
	return -1;
}

void api_finish()
{
	menu_global = NULL;
	lua_close(ls_global);
}

