
#include <Python.h>
#include <wayland-util.h>
#include <cairo.h>

#include "mayhem-client.h"

#include "pool.h"
#include "menu.h"

#define PYMOD_NAME "mayhem"

static struct menu *menu_global;

struct frame {
	struct wl_list link;
	int const width;
	int height;
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
	ITEM_TEXT,
	ITEM_BAR
};

enum item_align {
	ITEM_ALIGN_LEFT = 0x01,
	ITEM_ALIGN_RIGHT = 0x02,
	ITEM_ALIGN_TOP = 0x04,
	ITEM_ALIGN_BOT = 0x08
};

struct item {
	struct wl_list link;
	enum item_type type;
	void (*draw_func)(struct item *, cairo_t *, int);
	int height;
};

struct item_text {
	struct item base;
	int width;
	char *text, *font;
	uint32_t color;
	int size;
};

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

void item_text_draw(struct item *item, cairo_t *cr, int width)
{
	struct item_text *text = (struct item_text *) item;


	cairo_select_font_face(cr, "serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 24);

	//cairo_show_text(cr, text->text);
	cairo_text_path(cr, text->text);
	cairo_stroke(cr);
}

void item_bar_draw(struct item *item, cairo_t *cr, int width)
{
	struct item_bar *bar = (struct item_bar *)item;

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

struct item_bar *item_bar_create(struct frame *frame)
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

	assert(menu_global);

	if(!frame->dirty)
		goto end;


	if(frame->buffers[i]->flags & BUFFER_BUSY)
		if(frame->buffers[++i]->flags & BUFFER_BUSY)
			assert(0);
	buffer = frame->buffers[i];
	assert(buffer->flags & BUFFER_ACTIVE);

	if(frame->need_resize--) {
		buffer_destroy(buffer);
		buffer = buffer_create(menu_global->pool,
				       frame->width,
				       frame->height,
				       WL_SHM_FORMAT_ARGB8888);
		frame->buffers[i] = buffer;
	}

	stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32,
					       frame->width);
	assert(stride == sizeof(uint32_t) * frame->width);
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

static PyObject *api_menu_add_bar(PyObject *self, PyObject *args, PyObject *kw)
{
	static char const keywords[] = {
		"color", "padding", "padding_top", "padding_right",
		"padding_bottom", "padding_left", "dotted", "round",
		"red", "green", "blue", "alpha", NULL
	};
	char *color_hex;
	uint32_t color;
	int padding_all;
	int padding[4];
	int dotted, round;
	double fill;

	PyArg_ParseTupleAndKeywords(args, kw, "s|IIIIIppd", &keywords, &color_hex
				    &padding_all, &padding[0], &padding[1],
				    &padding[2], &padding[3], &dotted, &round,
				    &fill, );
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
	("add_bar", api_menu_add_bar, METH_VARARGS | METH_KEYWORDS, "Add a bar"},
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

