/*****************************************************************************

  Copyright (c) 2001, 2002 Zope Corporation and Contributors.
  All Rights Reserved.

  This software is subject to the provisions of the Zope Public License,
  Version 2.0 (ZPL).  A copy of the ZPL should accompany this distribution.
  THIS SOFTWARE IS PROVIDED "AS IS" AND ANY AND ALL EXPRESS OR IMPLIED
  WARRANTIES ARE DISCLAIMED, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF TITLE, MERCHANTABILITY, AGAINST INFRINGEMENT, AND FITNESS
  FOR A PARTICULAR PURPOSE

 ****************************************************************************/
static char cPersistence_doc_string[] =
"Defines Persistent mixin class for persistent objects.\n"
"\n"
"$Id: cPersistence.c,v 1.74 2003/11/28 16:44:55 jim Exp $\n";

#include "cPersistence.h"
#include "structmember.h"

struct ccobject_head_struct {
    CACHE_HEAD
};

#define ASSIGN(V,E) {PyObject *__e; __e=(E); Py_XDECREF(V); (V)=__e;}
#define UNLESS(E) if(!(E))
#define UNLESS_ASSIGN(V,E) ASSIGN(V,E) UNLESS(V)

/* Strings initialized by init_strings() below. */
static PyObject *py_keys, *py_setstate, *py___dict__, *py_timeTime;
static PyObject *py__p_changed, *py__p_deactivate;
static PyObject *py___getattr__, *py___setattr__, *py___delattr__;
static PyObject *py___getstate__;

/* These two objects are initialized when the module is loaded */
static PyObject *TimeStamp, *py_simple_new;

static int
init_strings(void)
{
#define INIT_STRING(S) \
    if (!(py_ ## S = PyString_InternFromString(#S))) \
	return -1;
    INIT_STRING(keys);
    INIT_STRING(setstate);
    INIT_STRING(timeTime);
    INIT_STRING(__dict__);
    INIT_STRING(_p_changed);
    INIT_STRING(_p_deactivate);
    INIT_STRING(__getattr__);
    INIT_STRING(__setattr__);
    INIT_STRING(__delattr__);
    INIT_STRING(__getstate__);
#undef INIT_STRING
    return 0;
}

static void ghostify(cPersistentObject*);

/* Load the state of the object, unghostifying it.  Upon success, return 1.
 * If an error occurred, re-ghostify the object and return 0.
 */
static int
unghostify(cPersistentObject *self)
{
    if (self->state < 0 && self->jar) {
        PyObject *r;

        /* XXX Is it ever possibly to not have a cache? */
        if (self->cache) {
            /* Create a node in the ring for this unghostified object. */
            self->cache->non_ghost_count++;
	    ring_add(&self->cache->ring_home, &self->ring);
	    Py_INCREF(self);
        }
	/* set state to CHANGED while setstate() call is in progress
	   to prevent a recursive call to _PyPersist_Load().
	*/
        self->state = cPersistent_CHANGED_STATE;
        /* Call the object's __setstate__() */
	r = PyObject_CallMethod(self->jar, "setstate", "O", (PyObject *)self);
        if (r == NULL) {
            ghostify(self);
            return 0;
        }
        self->state = cPersistent_UPTODATE_STATE;
        Py_DECREF(r);
    }
    return 1;
}

/****************************************************************************/

static PyTypeObject Pertype;

static void
accessed(cPersistentObject *self)
{
    /* Do nothing unless the object is in a cache and not a ghost. */
    if (self->cache && self->state >= 0 && self->ring.r_next)
	ring_move_to_head(&self->cache->ring_home, &self->ring);
}

static void
unlink_from_ring(cPersistentObject *self)
{
    /* If the cache has been cleared, then a non-ghost object
       isn't in the ring any longer.
    */
    if (self->ring.r_next == NULL)
	return;

    /* if we're ghostifying an object, we better have some non-ghosts */
    assert(self->cache->non_ghost_count > 0);
    self->cache->non_ghost_count--;
    ring_del(&self->ring);
}

static void
ghostify(cPersistentObject *self)
{
    PyObject **dictptr;

    /* are we already a ghost? */
    if (self->state == cPersistent_GHOST_STATE)
        return;

    /* XXX is it ever possible to not have a cache? */
    if (self->cache == NULL) {
        self->state = cPersistent_GHOST_STATE;
        return;
    }

    /* If the cache is still active, we must unlink the object. */
    if (self->ring.r_next) {
	/* if we're ghostifying an object, we better have some non-ghosts */
	assert(self->cache->non_ghost_count > 0);
	self->cache->non_ghost_count--;
	ring_del(&self->ring);
    }
    self->state = cPersistent_GHOST_STATE;
    dictptr = _PyObject_GetDictPtr((PyObject *)self);
    if (dictptr && *dictptr) {
	Py_DECREF(*dictptr);
	*dictptr = NULL;
    }

    /* We remove the reference to the just ghosted object that the ring
     * holds.  Note that the dictionary of oids->objects has an uncounted
     * reference, so if the ring's reference was the only one, this frees
     * the ghost object.  Note further that the object's dealloc knows to
     * inform the dictionary that it is going away.
     */
    Py_DECREF(self);
}

static int
changed(cPersistentObject *self)
{
  if ((self->state == cPersistent_UPTODATE_STATE ||
       self->state == cPersistent_STICKY_STATE)
       && self->jar)
    {
	PyObject *meth, *arg, *result;
	static PyObject *s_register;

	if (s_register == NULL)
	    s_register = PyString_InternFromString("register");
	meth = PyObject_GetAttr((PyObject *)self->jar, s_register);
	if (meth == NULL)
	    return -1;
	arg = PyTuple_New(1);
	if (arg == NULL) {
	    Py_DECREF(meth);
	    return -1;
	}
	PyTuple_SET_ITEM(arg, 0, (PyObject *)self);
	result = PyEval_CallObject(meth, arg);
	PyTuple_SET_ITEM(arg, 0, NULL);
	Py_DECREF(arg);
	Py_DECREF(meth);
	if (result == NULL)
	    return -1;
	Py_DECREF(result);

	self->state = cPersistent_CHANGED_STATE;
    }

  return 0;
}

static PyObject *
Per__p_deactivate(cPersistentObject *self)
{
    if (self->state == cPersistent_UPTODATE_STATE && self->jar) {
	PyObject **dictptr = _PyObject_GetDictPtr((PyObject *)self);
	if (dictptr && *dictptr) {
	    Py_DECREF(*dictptr);
	    *dictptr = NULL;
	}
	/* Note that we need to set to ghost state unless we are
	   called directly. Methods that override this need to
	   do the same! */
	ghostify(self);
    }

    Py_INCREF(Py_None);
    return Py_None;
}


#include "pickle/pickle.c"





/* Return the object's state, a dict or None.

   If the object has no dict, it's state is None.
   Otherwise, return a dict containing all the attributes that
   don't start with "_v_".

   The caller should not modify this dict, as it may be a reference to
   the object's __dict__.
*/

static PyObject *
Per__getstate__(cPersistentObject *self)
{
    /* XXX Should it be an error to call __getstate__() on a ghost? */
    if (!unghostify(self))
        return NULL;

    /* XXX shouldn't we increment stickyness? */
    return pickle___getstate__((PyObject*)self);
}


static struct PyMethodDef Per_methods[] = {
  {"_p_deactivate", (PyCFunction)Per__p_deactivate, METH_NOARGS,
   "_p_deactivate() -- Deactivate the object"},
  {"__getstate__", (PyCFunction)Per__getstate__, METH_NOARGS,
   pickle___getstate__doc },

  PICKLE_SETSTATE_DEF 
  PICKLE_GETNEWARGS_DEF 
  PICKLE_REDUCE_DEF

  {NULL,		NULL}		/* sentinel */
};

/* The Persistent base type provides a traverse function, but not a
   clear function.  An instance of a Persistent subclass will have
   its dict cleared through subtype_clear().

   There is always a cycle between a persistent object and its cache.
   When the cycle becomes unreachable, the clear function for the
   cache will break the cycle.  Thus, the persistent object need not
   have a clear function.  It would be complex to write a clear function
   for the objects, if we needed one, because of the reference count
   tricks done by the cache.
*/

static void
Per_dealloc(cPersistentObject *self)
{
    if (self->state >= 0)
	unlink_from_ring(self);
    if (self->cache)
	cPersistenceCAPI->percachedel(self->cache, self->oid);
    Py_XDECREF(self->cache);
    Py_XDECREF(self->jar);
    Py_XDECREF(self->oid);
    self->ob_type->tp_free(self);
}

static int
Per_traverse(cPersistentObject *self, visitproc visit, void *arg)
{
    int err;

#define VISIT(SLOT) \
    if (SLOT) { \
	err = visit((PyObject *)(SLOT), arg); \
	if (err) \
		     return err; \
    }

    VISIT(self->jar);
    VISIT(self->oid);
    VISIT(self->cache);

#undef VISIT
    return 0;
}

/* convert_name() returns a new reference to a string name
   or sets an exception and returns NULL.
*/

static PyObject *
convert_name(PyObject *name)
{
#ifdef Py_USING_UNICODE
    /* The Unicode to string conversion is done here because the
       existing tp_setattro slots expect a string object as name
       and we wouldn't want to break those. */
    if (PyUnicode_Check(name)) {
	name = PyUnicode_AsEncodedString(name, NULL, NULL);
    }
    else
#endif
    if (!PyString_Check(name)) {
	PyErr_SetString(PyExc_TypeError, "attribute name must be a string");
	return NULL;
    } else
	Py_INCREF(name);
    return name;
}

/* Returns true if the object requires unghostification.

   There are several special attributes that we allow access to without
   requiring that the object be unghostified:
   __class__
   __del__
   __dict__
   __of__
   __setstate__
*/

static int
unghost_getattr(const char *s)
{
    if (*s++ != '_')
	return 1;
    if (*s == 'p') {
	s++;
	if (*s == '_')
	    return 0; /* _p_ */
	else
	    return 1;
    }
    else if (*s == '_') {
	s++;
	switch (*s) {
	case 'c':
	    return strcmp(s, "class__");
	case 'd':
	    s++;
	    if (!strcmp(s, "el__"))
		return 0; /* __del__ */
	    if (!strcmp(s, "ict__"))
		return 0; /* __dict__ */
	    return 1;
	case 'o':
	    return strcmp(s, "of__");
	case 's':
	    return strcmp(s, "setstate__");
	default:
	    return 1;
	}
    }
    return 1;
}

static PyObject*
Per_getattro(cPersistentObject *self, PyObject *name)
{
    PyObject *result = NULL;	/* guilty until proved innocent */
    char *s;

    name = convert_name(name);
    if (!name)
	goto Done;
    s = PyString_AS_STRING(name);

    if (*s != '_' || unghost_getattr(s)) {
	if (!unghostify(self))
	    goto Done;
	accessed(self);
    }
    result = PyObject_GenericGetAttr((PyObject *)self, name);

  Done:
    Py_XDECREF(name);
    return result;
}

/* We need to decide on a reasonable way for a programmer to write
   an __setattr__() or __delattr__() hook.

   The ZODB3 has been that if you write a hook, it will be called if
   the attribute is not an _p_ attribute and after doing any necessary
   unghostifying.  AMK's guide says modification will not be tracked
   automatically, so the hook must explicitly set _p_changed; I'm not
   sure if I believe that.

   This approach won't work with new-style classes, because type will
   install a slot wrapper that calls the derived class's __setattr__().
   That means Persistent's tp_setattro doesn't get a chance to be called.
   Changing this behavior would require a metaclass.

   One option for ZODB 3.3 is to require setattr hooks to know about
   _p_ and to call a prep function before modifying the object's state.
   That's the solution I like best at the moment.
*/

static int
Per_setattro(cPersistentObject *self, PyObject *name, PyObject *v)
{
    int result = -1;	/* guilty until proved innocent */
    char *s;

    name = convert_name(name);
    if (!name)
	goto Done;
    s = PyString_AS_STRING(name);

    if (strncmp(s, "_p_", 3) != 0) {
	if (!unghostify(self))
	    goto Done;
	accessed(self);
	if (strncmp(s, "_v_", 3) != 0
	    && self->state != cPersistent_CHANGED_STATE) {
	    if (changed(self) < 0)
		goto Done;
	}
    }
    result = PyObject_GenericSetAttr((PyObject *)self, name, v);

 Done:
    Py_XDECREF(name);
    return result;
}

static PyObject *
Per_get_changed(cPersistentObject *self)
{
    if (self->state < 0) {
	Py_INCREF(Py_None);
	return Py_None;
    }
    return PyInt_FromLong(self->state == cPersistent_CHANGED_STATE);
}

static int
Per_set_changed(cPersistentObject *self, PyObject *v)
{
    int deactivate = 0, true;
    if (!v) {
	/* delattr is used to invalidate an object even if it has changed. */
	if (self->state != cPersistent_GHOST_STATE)
	    self->state = cPersistent_UPTODATE_STATE;
	deactivate = 1;
    }
    else if (v == Py_None)
	deactivate = 1;

    if (deactivate) {
	PyObject *res, *meth;
	meth = PyObject_GetAttr((PyObject *)self, py__p_deactivate);
	if (meth == NULL)
	    return -1;
	res = PyObject_CallObject(meth, NULL);
	if (res)
	    Py_DECREF(res);
	else {
	    /* an error occured in _p_deactivate().

	    It's not clear what we should do here.  The code is
	    obviously ignoring the exception, but it shouldn't return
	    0 for a getattr and set an exception.  The simplest change
	    is to clear the exception, but that simply masks the
	    error.

	    XXX We'll print an error to stderr just like exceptions in
	    __del__().  It would probably be better to log it but that
	    would be painful from C.
	    */
	    PyErr_WriteUnraisable(meth);
	}
	Py_DECREF(meth);
	return 0;
    }
    true = PyObject_IsTrue(v);
    if (true == -1)
	return -1;
    else if (true)
	return changed(self);

    if (self->state >= 0)
	self->state = cPersistent_UPTODATE_STATE;
    return 0;
}

static PyObject *
Per_get_oid(cPersistentObject *self)
{
    PyObject *oid = self->oid ? self->oid : Py_None;
    Py_INCREF(oid);
    return oid;
}

static int
Per_set_oid(cPersistentObject *self, PyObject *v)
{
    if (self->cache) {
	int result;

	if (v == NULL) {
	    PyErr_SetString(PyExc_ValueError,
			    "can't delete _p_oid of cached object");
	    return -1;
	}
	if (PyObject_Cmp(self->oid, v, &result) < 0)
	    return -1;
	if (result) {
	    PyErr_SetString(PyExc_ValueError,
			    "can not change _p_oid of cached object");
	    return -1;
	}
    }
    Py_XDECREF(self->oid);
    Py_XINCREF(v);
    self->oid = v;
    return 0;
}

static PyObject *
Per_get_jar(cPersistentObject *self)
{
    PyObject *jar = self->jar ? self->jar : Py_None;
    Py_INCREF(jar);
    return jar;
}

static int
Per_set_jar(cPersistentObject *self, PyObject *v)
{
    if (self->cache) {
	int result;

	if (v == NULL) {
	    PyErr_SetString(PyExc_ValueError,
			    "can't delete _p_jar of cached object");
	    return -1;
	}
	if (PyObject_Cmp(self->jar, v, &result) < 0)
	    return -1;
	if (result) {
	    PyErr_SetString(PyExc_ValueError,
			    "can not change _p_jar of cached object");
	    return -1;
	}
    }
    Py_XDECREF(self->jar);
    Py_XINCREF(v);
    self->jar = v;
    return 0;
}

static PyObject *
Per_get_serial(cPersistentObject *self)
{
    return PyString_FromStringAndSize(self->serial, 8);
}

static int
Per_set_serial(cPersistentObject *self, PyObject *v)
{
    if (v) {
	if (PyString_Check(v) && PyString_GET_SIZE(v) == 8)
	    memcpy(self->serial, PyString_AS_STRING(v), 8);
	else {
	    PyErr_SetString(PyExc_ValueError,
			    "_p_serial must be an 8-character string");
	    return -1;
	}
    } else
	memset(self->serial, 0, 8);
    return 0;
}

static PyObject *
Per_get_mtime(cPersistentObject *self)
{
    PyObject *t, *v;

    if (!unghostify(self))
	return NULL;

    accessed(self);

    if (memcmp(self->serial, "\0\0\0\0\0\0\0\0", 8) == 0) {
	Py_INCREF(Py_None);
	return Py_None;
    }

    t = PyObject_CallFunction(TimeStamp, "s#", self->serial, 8);
    if (!t)
	return NULL;
    v = PyObject_CallMethod(t, "timeTime", "");
    Py_DECREF(t);
    return v;
}

static PyGetSetDef Per_getsets[] = {
    {"_p_changed", (getter)Per_get_changed, (setter)Per_set_changed},
    {"_p_jar", (getter)Per_get_jar, (setter)Per_set_jar},
    {"_p_mtime", (getter)Per_get_mtime},
    {"_p_oid", (getter)Per_get_oid, (setter)Per_set_oid},
    {"_p_serial", (getter)Per_get_serial, (setter)Per_set_serial},
    {NULL}
};

/* This module is compiled as a shared library.  Some compilers don't
   allow addresses of Python objects defined in other libraries to be
   used in static initializers here.  The DEFERRED_ADDRESS macro is
   used to tag the slots where such addresses appear; the module init
   function must fill in the tagged slots at runtime.  The argument is
   for documentation -- the macro ignores it.
*/
#define DEFERRED_ADDRESS(ADDR) 0

static PyTypeObject Pertype = {
    PyObject_HEAD_INIT(DEFERRED_ADDRESS(&PyPersist_MetaType))
    0,					/* ob_size */
    "persistent.Persistent",		/* tp_name */
    sizeof(cPersistentObject),		/* tp_basicsize */
    0,					/* tp_itemsize */
    (destructor)Per_dealloc,		/* tp_dealloc */
    0,					/* tp_print */
    0,					/* tp_getattr */
    0,					/* tp_setattr */
    0,					/* tp_compare */
    0,					/* tp_repr */
    0,					/* tp_as_number */
    0,					/* tp_as_sequence */
    0,					/* tp_as_mapping */
    0,					/* tp_hash */
    0,					/* tp_call */
    0,					/* tp_str */
    (getattrofunc)Per_getattro,		/* tp_getattro */
    (setattrofunc)Per_setattro,		/* tp_setattro */
    0,					/* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC,
    					/* tp_flags */
    0,					/* tp_doc */
    (traverseproc)Per_traverse,		/* tp_traverse */
    0,					/* tp_clear */
    0,					/* tp_richcompare */
    0,					/* tp_weaklistoffset */
    0,					/* tp_iter */
    0,					/* tp_iternext */
    Per_methods,			/* tp_methods */
    0,					/* tp_members */
    Per_getsets,			/* tp_getset */
};

/* End of code for Persistent objects */
/* -------------------------------------------------------- */

typedef int (*intfunctionwithpythonarg)(PyObject*);

/* Load the object's state if necessary and become sticky */
static int
Per_setstate(cPersistentObject *self)
{
    if (!unghostify(self))
        return -1;
    self->state = cPersistent_STICKY_STATE;
    return 0;
}

static PyObject *
simple_new(PyObject *self, PyObject *type_object)
{
    return PyType_GenericNew((PyTypeObject *)type_object, NULL, NULL);
}

static PyMethodDef cPersistence_methods[] = {
    {"simple_new", simple_new, METH_O,
     "Create an object by simply calling a class's __new__ method without "
     "arguments."},
    {NULL, NULL}
};


static cPersistenceCAPIstruct
truecPersistenceCAPI = {
    &Pertype,
    (getattrofunc)Per_getattro,	/*tp_getattr with object key*/
    (setattrofunc)Per_setattro,	/*tp_setattr with object key*/
    changed,
    accessed,
    ghostify,
    (intfunctionwithpythonarg)Per_setstate,
    NULL /* The percachedel slot is initialized in cPickleCache.c when
	    the module is loaded.  It uses a function in a different
	    shared library. */
};

void
initcPersistence(void)
{
    PyObject *m, *s;

    if (pickle_setup() < 0)
      return;

    if (init_strings() < 0)
      return;

    m = Py_InitModule3("cPersistence", cPersistence_methods,
		       cPersistence_doc_string);

    Pertype.ob_type = &PyType_Type;
    Pertype.tp_new = PyType_GenericNew;
    if (PyType_Ready(&Pertype) < 0)
	return;
    if (PyModule_AddObject(m, "Persistent", (PyObject *)&Pertype) < 0)
	return;

    cPersistenceCAPI = &truecPersistenceCAPI;
    s = PyCObject_FromVoidPtr(cPersistenceCAPI, NULL);
    if (!s)
	return;
    if (PyModule_AddObject(m, "CAPI", s) < 0)
	return;

    py_simple_new = PyObject_GetAttrString(m, "simple_new");
    if (!py_simple_new)
        return;

    m = PyImport_ImportModule("persistent.TimeStamp");
    if (!m)
	return;
    TimeStamp = PyObject_GetAttrString(m, "TimeStamp");
    if (!TimeStamp)
	return;
    Py_DECREF(m);
}
