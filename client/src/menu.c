

#include <wayland-util.h>
#include <Python.h>

#include "mayhem-client.h"

#include "pool.h"
#include "menu.h"

#define PYMOD_NAME "mayhem"

static struct menu *menu_global;

struct frame {
	struct wl_list link;
	int width, height;
	struct wl_surface *surface;
	struct ms_surface *msurf;
	struct buffer *buffers[2];
	struct wl_callback *callback;
	int dirty, need_resize;
	struct frame *parent; /* NULL if first */
	struct wl_list children;
	struct wl_list items;
};

struct menu {
	int closed;
	struct pool *pool;
	struct wl_compositor *ec;
	struct ms_menu *ms;
	struct wl_list top_frames;
};

enum item_type {
	ITEM_NONE,
	ITEM_TEXT
};

struct item {
	struct wl_list link;
	enum item_type type;
	void (*draw_func)(struct item*, void*, int);
	int offset;
	int height;
};

struct item_text {
	struct item base;
	const char *text;
	uint32_t fg_color;
	uint32_t bg_color;
};

void item_text_draw(struct item *item, void *addr, int width)
{
	int x, y;
	uint32_t *pixel = addr;

	for(y = 0; y < item->height; y++) {
		for(x =0; x < width; x++) {
			*pixel++ = 0x80FF3300;
		}
	}
}

struct py_item {
	PyObject_HEAD
	struct item *item;
};

static PyObject *api_item_text_set_text(PyObject *self, PyObject *args)
{
	struct py_item *this = (struct py_item*)self;
	char *str = NULL;

	printf("called set_text\n");

	if(this->item->type != ITEM_TEXT)
		return NULL;

	if(!PyArg_ParseTuple(args, "s", str))
		return NULL;

	((struct item_text*)this->item)->text = str;
	Py_RETURN_TRUE;
}

static PyMethodDef api_item_text[] = {
	{"set_text", api_item_text_set_text, METH_VARARGS, "Set text"},
	{NULL, NULL, 0, NULL}
};

static PyTypeObject py_item_text_type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = PYMOD_NAME ".text",
	.tp_basicsize = sizeof(struct py_item),
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = "Text item",
	.tp_methods = api_item_text
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

struct item_text *item_text_create(struct frame *parent, const char* text)
{
	struct item_text *item;

	item = calloc(1, sizeof(struct item_text));
	if(!item)
		return NULL;

	parent->need_resize = 1;
	item->base.type = ITEM_TEXT;
	item->base.draw_func = item_text_draw;
	if(wl_list_empty(&parent->items)) {
		wl_list_insert(parent->items.prev, &item->base.link);
		item->base.offset = 0;
	} else {
		struct item *prev;

		wl_list_insert(parent->items.prev, &item->base.link);
		prev = wl_container_of(item->base.link.prev, prev, link);
		item->base.offset = prev->offset + prev->height * parent->width * 4;
	}
	//TODO
	item->base.height = 100;
	item->text = text;

	parent->width = 200;
	parent->height = 100;

	return item;
}

static const struct wl_callback_listener frame_listener;

static void redraw(void *data, struct wl_callback *callback, uint32_t time)
{
	struct frame *frame = data;

	if(frame->dirty)
	{
		struct item *item;
		int buffer = 0;

		if(frame->buffers[buffer]->flags & BUFFER_BUSY)
			if(frame->buffers[++buffer]->flags & BUFFER_BUSY)
				assert(0);
		assert(frame->buffers[buffer]->flags & BUFFER_ACTIVE);

		if(frame->need_resize--) {
			assert(menu_global);
			buffer_destroy(frame->buffers[buffer]);
			frame->buffers[buffer] =
				buffer_create(menu_global->pool, frame->width,
					      frame->height,
					      WL_SHM_FORMAT_ARGB8888);
		}

		wl_list_for_each(item, &frame->items, link) {
			void *addr = frame->buffers[buffer]->addr + item->offset;
			item->draw_func(item, addr, frame->width);
		}

		wl_surface_attach(frame->surface, frame->buffers[buffer]->buffer, 0, 0);
		wl_surface_damage(frame->surface, 0, 0, frame->width, frame->height);
		wl_surface_commit(frame->surface);
		frame->buffers[buffer]->flags |= BUFFER_BUSY;
		frame->dirty = 0;
	}

	if(callback)
		wl_callback_destroy(callback);

	frame->callback = wl_surface_frame(frame->surface);
	wl_callback_add_listener(frame->callback, &frame_listener, frame);
}

static const struct wl_callback_listener frame_listener = {
	redraw
};

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

	ms_surface_destroy(frame->msurf);
	wl_surface_destroy(frame->surface);

	wl_list_remove(&frame->link);
	free(frame);
}

static struct frame *frame_create(struct menu *menu, struct frame *parent)
{
	struct frame *frame;

	frame = calloc(1, sizeof *frame);
	if(!frame)
		return NULL;

	frame->callback = NULL;
	frame->parent = parent;
	frame->surface = wl_compositor_create_surface(menu->ec);
	frame->msurf = ms_menu_get_menu_surface(menu->ms, frame->surface);

	if(parent)
		wl_list_insert(&parent->children, &frame->link);
	else
		wl_list_insert(&menu->top_frames, &frame->link);
	wl_list_init(&frame->children);
	wl_list_init(&frame->items);

	return frame;
}

struct py_menu {
	PyObject_HEAD
	struct frame *frame;
};

static PyObject *api_menu_show(PyObject *self, PyObject *args)
{
	struct py_menu *this = (struct py_menu*)self;
	struct frame *frame = this->frame;

	if(frame->callback || !frame->width || !frame->height)
		Py_RETURN_FALSE;

	frame->buffers[0] = buffer_create(menu_global->pool,
					  frame->width, frame->height,
					  WL_SHM_FORMAT_ARGB8888);
	if(!frame->buffers[0])
		return PyErr_NoMemory();

	frame->buffers[1] = buffer_create(menu_global->pool,
					  frame->width, frame->height,
					  WL_SHM_FORMAT_ARGB8888);
	if(!frame->buffers[1])
		return buffer_destroy(frame->buffers[0]), PyErr_NoMemory();

	frame->dirty = 1;
	frame->need_resize = 0;
	redraw(frame, NULL, 0);

	Py_RETURN_TRUE;
}

static PyObject *api_menu_add_text(PyObject *self, PyObject *args)
{
	struct py_menu *this = (struct py_menu*)self;
	struct frame *frame = this->frame;
	struct py_item *py_item;
	struct item_text *item;
	char *str = NULL;

	if(!PyArg_ParseTuple(args, "s", &str))
		return NULL;

	item = item_text_create(frame, str?:"");
	if(!item)
		return PyErr_NoMemory();

	py_item = PyObject_New(struct py_item, &py_item_text_type);
	py_item->item = (struct item*)item;

	return (PyObject*)py_item;
}

static PyObject *api_menu_on_enter(PyObject *self, PyObject *args)
{
	struct py_menu *this = (struct py_menu*)self;
	printf("called menu on enter\n");
	Py_RETURN_NONE;
}

static PyMethodDef api_menu[] = {
	{"show", api_menu_show, METH_VARARGS, "Show the menu"},
	{"add_text", api_menu_add_text, METH_VARARGS, "Add a text item"},
	{"on_enter", api_menu_on_enter, METH_VARARGS, "Mouse enter callback"},
	{NULL, NULL, 0, NULL}
};

static PyTypeObject py_menu_type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = PYMOD_NAME ".menu",
	.tp_basicsize = sizeof(struct py_menu),
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = "Menu Object",
	.tp_methods = api_menu
};

static PyObject *api_base_close(PyObject *self, PyObject *args)
{
	struct frame *child, *tmp;
	(void)self;
	(void)args;

	wl_list_for_each_safe(child, tmp, &menu_global->top_frames, link) {
		printf("descending into child\n");
		frame_destroy(child);
	}

	Py_RETURN_NONE;
}

static PyObject *api_base_spawn(PyObject *self, PyObject *args)
{
	struct py_menu *py_frame;
	struct frame *frame;
	int w, h;

	(void)self;

	if(!PyArg_ParseTuple(args, "ii", &w, &h))
		return NULL;

	assert(menu_global);
	frame = frame_create(menu_global, NULL);
	if(!frame)
		return PyErr_NoMemory();

	py_frame = PyObject_New(struct py_menu, &py_menu_type);
	py_frame->frame = frame;

	return (PyObject*)py_frame;
}

static PyMethodDef api_base[] = {
	{"close", api_base_close, METH_VARARGS, "Exit all menus"},
	{"spawn_menu", api_base_spawn, METH_VARARGS, "Create a new menu"},
	{NULL, NULL, 0, NULL}
};

static PyModuleDef python_module = {
	PyModuleDef_HEAD_INIT,
	PYMOD_NAME,
	NULL, /* Docstring */
	-1,
	api_base, /* base module api */
	NULL,
	NULL,
	NULL,
	NULL /* destroy function */
};

PyMODINIT_FUNC module_init(void)
{
	PyObject* module;

	py_menu_type.tp_new = PyType_GenericNew;
	if(PyType_Ready(&py_menu_type) < 0)
		return NULL;

	py_item_text_type.tp_new = PyType_GenericNew;
	if(PyType_Ready(&py_item_text_type) < 0)
		return NULL;

	module = PyModule_Create(&python_module);
	if(module == NULL)
		return NULL;

    	Py_INCREF(&py_menu_type);
    	Py_INCREF(&py_item_text_type);

	return module;
}

struct menu *menu_create(struct wl_compositor *ec, struct ms_menu *ms,
			 struct pool *pool)
{
	PyObject *pyFile, *pySysPath, *pyPath, *pyModule, *pyMain;
	struct menu *menu;

	if(menu_global)
		return NULL;

	if(PyImport_AppendInittab(PYMOD_NAME, &module_init) < 0)
		return NULL;

	Py_Initialize();

	pySysPath = PySys_GetObject("path");
	pyPath = PyUnicode_DecodeFSDefault("/home/murray/Desktop/try");
	PyList_Append(pySysPath, pyPath);
	Py_DECREF(pyPath);

	pyFile = PyUnicode_DecodeFSDefault("lol");
	pyModule = PyImport_Import(pyFile);
	Py_DECREF(pyFile);

	if(pyModule == NULL) {
		PyErr_Print();
		goto err1;
	}

	menu = malloc(sizeof *menu);
	if(!menu)
		goto err1;

	menu->closed = 0;
	menu->ec = ec;
	menu->ms = ms;
	menu->pool = pool;
	wl_list_init(&menu->top_frames);
	menu_global = menu;

	pyMain = PyObject_GetAttrString(pyModule, "main");
	Py_DECREF(pyModule);
	if(pyMain && PyCallable_Check(pyMain)) {
		PyObject *pyRet = PyObject_CallObject(pyMain, NULL);
		Py_DECREF(pyMain);
		if(pyRet == NULL) {
			PyErr_Print();
			goto err2;
		} else {
			printf("pything returned: %li\n", PyLong_AsLong(pyRet));
			Py_DECREF(pyRet);
		}
	} else {
		printf("no main function\n");
		Py_XDECREF(pyMain);
		goto err2;
	}

	return menu;

err2:
	free(menu);
err1:
	Py_Finalize();
	return NULL;
}

void menu_destroy(struct menu *menu)
{
	struct frame *child, *tmp;

	assert(menu = menu_global);
	wl_list_for_each_safe(child, tmp, &menu->top_frames, link) {
		printf("descending into child\n");
		frame_destroy(child);
	}

	menu_global = NULL;
	free(menu);
	Py_Finalize();
}

