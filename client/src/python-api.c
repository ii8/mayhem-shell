
#include <Python.h>

#include "api.h"

#define MODULE_NAME "mayhem"

static struct menu *menu_global;

struct py_item {
	PyObject_HEAD
	struct item *item;
};

static PyObject *api_item_text_set_text(PyObject *self, PyObject *args)
{
	struct py_item *this = (struct py_item*)self;
	char *str = NULL;

	printf("called set_text\n");

	if(!PyArg_ParseTuple(args, "s", &str))
		return NULL;

	item_text_set_text((struct item_text*)this->item, str);
	Py_RETURN_TRUE;
}

static PyMethodDef api_item_text[] = {
	{"set_text", api_item_text_set_text, METH_VARARGS, "Set text"},
	{NULL, NULL, 0, NULL}
};

static PyTypeObject py_item_text_type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = MODULE_NAME ".text",
	.tp_basicsize = sizeof(struct py_item),
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = "Text item",
	.tp_methods = api_item_text
};

struct py_menu {
	PyObject_HEAD
	struct frame *frame;
};

static PyObject *api_menu_show(PyObject *self, PyObject *args)
{
	struct py_menu *this = (struct py_menu*)self;
	struct frame *frame = this->frame;
	int ret;

	ret = frame_show(frame);

	if(ret < -1)
		PyErr_NoMemory();
	else if(ret < 0)
		Py_RETURN_FALSE;

	Py_RETURN_TRUE;
}

static PyObject *api_menu_close(PyObject *self, PyObject *args)
{
	struct py_menu *this = (struct py_menu*)self;

	frame_destroy(this->frame);
	this->frame = NULL;
	Py_DECREF(this);
	printf("refcnt: %i\n", Py_REFCNT(self));
	//PyObject_Del(self);

	Py_RETURN_NONE;
}

static void api_menu_del(void *self)
{
	struct py_menu *this = (struct py_menu*)self;

	printf("python deleted py_menu\n");
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

	item = item_text_create(frame, str?str:"");
	if(!item)
		return PyErr_NoMemory();

	py_item = PyObject_New(struct py_item, &py_item_text_type);
	py_item->item = (struct item*)item;

	return (PyObject*)py_item;
}

static PyObject *api_menu_add_bar(PyObject *self, PyObject *args)
{
	struct py_menu *this = (struct py_menu*)self;
	struct py_item *bar;
	PyDictObject *py_theme = NULL;
	int height;

	if(!PyArg_ParseTuple(args, "i|O!", &height, &PyDict_Type, &py_theme))
		return NULL;

	bar = PyObject_New(struct py_item, &py_item_text_type);
	if(bar == NULL)
		return PyErr_NoMemory();

	bar->item = (struct item *)item_bar_create(this->frame, height);
	if(!bar->item) {
		Py_DECREF(bar);
		return PyErr_NoMemory();
	}

	return (PyObject *)bar;
/*
	if(py_theme) {
		struct theme *theme;

		theme = frame_get_theme(this->frame);
		if(theme == NULL)
			return PyErr_NoMemory();

	}*/
}

static PyObject *api_menu_on_enter(PyObject *self, PyObject *args)
{
	struct py_menu *this = (struct py_menu*)self;
	printf("called menu on enter\n");
	Py_RETURN_NONE;
}

static PyMethodDef api_menu[] = {
	{"show", api_menu_show, METH_VARARGS, "Show the menu"},
	{"close", api_menu_close, METH_NOARGS, "Close menu"},
	{"add_text", api_menu_add_text, METH_VARARGS, "Add a text item"},
	{"add_bar", api_menu_add_bar, METH_VARARGS, "Add a bar"},
	{"on_enter", api_menu_on_enter, METH_VARARGS, "Mouse enter callback"},
	{NULL, NULL, 0, NULL}
};

static PyTypeObject py_menu_type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = MODULE_NAME ".menu",
	.tp_basicsize = sizeof(struct py_menu),
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_doc = "Menu Object",
	.tp_methods = api_menu,
	.tp_free = api_menu_del
};

static PyObject *api_base_close(PyObject *self, PyObject *args)
{
	(void)self;
	(void)args;

	menu_close(menu_global);

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

	Py_DECREF(py_frame);
	return (PyObject*)py_frame;
}

static PyMethodDef api_base[] = {
	{"close", api_base_close, METH_VARARGS, "Exit all menus"},
	{"spawn_menu", api_base_spawn, METH_VARARGS, "Create a new menu"},
	{NULL, NULL, 0, NULL}
};

static PyModuleDef python_module = {
	PyModuleDef_HEAD_INIT,
	MODULE_NAME,
	NULL, /* Docstring */
	-1,
	api_base, /* base module api */
	NULL,
	NULL,
	NULL,
	NULL /* destroy function */
};

static PyMODINIT_FUNC module_init(void)
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

int api_init(struct menu *menu)
{
	PyObject *pyFile, *pySysPath, *pyPath, *pyModule, *pyMain;

	if(menu_global)
		assert(0);

	menu_global = menu;

	if(PyImport_AppendInittab(MODULE_NAME, &module_init) < 0)
		return -1;

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
		goto err;
	}

	pyMain = PyObject_GetAttrString(pyModule, "main");
	Py_DECREF(pyModule);
	if(pyMain && PyCallable_Check(pyMain)) {
		PyObject *pyRet = PyObject_CallObject(pyMain, NULL);
		Py_DECREF(pyMain);
		if(pyRet == NULL) {
			PyErr_Print();
			goto err;
		} else {
			printf("pything returned: %li\n", PyLong_AsLong(pyRet));
			Py_DECREF(pyRet);
		}
	} else {
		printf("no main function\n");
		Py_XDECREF(pyMain);
		goto err;
	}

	return 0;

err:
	menu_global = NULL;
	Py_Finalize();
	return -1;
}

void api_finish(void)
{
	menu_global = NULL;
	Py_Finalize();
}
