#include <cstdint>

#include <Python.h>

#include "jlist/jlist.h"
#include "jlist/scope_guard.h"

namespace jl::ops {
struct module_state {
    PyTypeObject* jlist_type;
    PyObject* builtin_all;
    PyObject* builtin_any;
    PyObject* builtin_sum;
};

namespace detail {
jlist* new_jlist(PyObject* module, entry_tag tag) {
    module_state* state = reinterpret_cast<module_state*>(PyModule_GetState(module));
    jlist* out = PyObject_GC_New(jlist, state->jlist_type);
    if (!out) {
        return nullptr;
    }
    out->tag(tag);
    new (&out->entries) std::vector<entry>;

    return out;
}

template<bool any, typename T>
struct any_all;

template<bool any>
struct any_all<any, PyObject*> {
private:
    static constexpr bool all = !any;

    template<typename F>
    static int loop(F&& is_true, jlist& self) {
        for (entry e : self.entries) {
            int r = is_true(e.as_ob);
            if (r < 0) {
                return r;
            }
            if (any && r) {
                return 1;
            }
            if (all && !r) {
                return 0;
            }
        }
        return all;
    }

public:
    static int heterogeneous(jlist& self) {
        return loop(PyObject_IsTrue, self);
    }

    static int homogeneous(jlist& self) {
        if (!self.size()) {
            return 1;
        }

        PyTypeObject* tp = self.homogeneous_type_ptr();

        if (tp == Py_TYPE(Py_True)) {
            return loop([](PyObject* ob) { return ob == Py_True; }, self);
        }
        else if (tp == Py_TYPE(Py_None)) {
            // is_true is just False for None
            return 0;
        }
        else if (tp->tp_as_number &&
                 tp->tp_as_number->nb_bool) {
            return loop([nb_bool = tp->tp_as_number->nb_bool](
                            PyObject* ob) { return nb_bool(ob); },
                        self);
        }
        else if (tp->tp_as_mapping &&
                 tp->tp_as_mapping->mp_length) {
            return loop([mp_length = tp->tp_as_mapping->mp_length](
                            PyObject* ob) { return mp_length(ob); },
                        self);
        }
        else if (tp->tp_as_sequence &&
                 tp->tp_as_sequence->sq_length) {
            return loop([sq_length = tp->tp_as_sequence->sq_length](
                            PyObject* ob) { return sq_length(ob); },
                        self);
        }

        // is_true is just True for this type, so any or all is true
        return 1;
    }
};

template<bool any>
struct any_all<any, std::int64_t> {
    static constexpr bool all = !any;

    static int f(jlist& self) {
        for (entry e : self.entries) {
            if (any && e.as_int) {
                return 1;
            }
            if (all && !e.as_int) {
                return 0;
            }
        }
        return all;
    }
};

template<bool any>
struct any_all<any, double> {
    static constexpr bool all = !any;

    static int f(jlist& self) {
        for (entry e : self.entries) {
            if (any && e.as_double) {
                return 1;
            }
            if (all && !e.as_double) {
                return 0;
            }
        }
        return all;
    }
};
}  // namespace detail

template<bool any>
PyObject* any_all(PyObject* module, PyObject* iterable) {
    module_state* state = reinterpret_cast<module_state*>(PyModule_GetState(module));

    if (Py_TYPE(iterable) != state->jlist_type) {
        return PyObject_CallFunctionObjArgs((any) ? state->builtin_any
                                                  : state->builtin_all,
                                            iterable,
                                            nullptr);
    }

    jlist& self = *reinterpret_cast<jlist*>(iterable);

    if (!self.size()) {
        return PyBool_FromLong(!any);
    }

    int out;
    switch (self.tag()) {
    case entry_tag::as_homogeneous_ob:
        out = detail::any_all<any, PyObject*>::homogeneous(self);
        break;
    case entry_tag::as_heterogeneous_ob:
        out = detail::any_all<any, PyObject*>::heterogeneous(self);
        break;
    case entry_tag::as_int:
        out = detail::any_all<any, std::int64_t>::f(self);
        break;
    case entry_tag::as_double:
        out = detail::any_all<any, double>::f(self);
        break;
    default:
        // `tag` cannot be `unset` because we check the size above.
        __builtin_unreachable();
    }

    if (out < 0) {
        return nullptr;
    }

    return PyBool_FromLong(out);
}

PyDoc_STRVAR(all_doc,
             "Return True if bool(x) is True for all values x in the iterable.\n"
             "\n"
             "If the iterable is empty, return True.");

PyMethodDef all_method = {"all", any_all<false>, METH_O, all_doc};

PyDoc_STRVAR(any_doc,
             "Return True if bool(x) is True for any x in the iterable.\n"
             "\n"
             "If the iterable is empty, return True.");

PyMethodDef any_method = {"any", any_all<true>, METH_O, any_doc};

PyDoc_STRVAR(
    sum_doc,
    "Return the sum of a 'start' value (default: 0) plus an iterable of numbers\n"
    "\n"
    "When the iterable is empty, return the start value.\n"
    "This function is intended specifically for use with numeric values and may\n"
    "reject non-numeric types.");

namespace detail {
template<typename T>
PyObject* boxing_sum(jlist& self, PyObject* result, Py_ssize_t start = 0) {
    if (!result) {
        result = PyLong_FromLong(0);
        if (!result) {
            return nullptr;
        }
    }
    else {
        Py_INCREF(result);
    }

    for (Py_ssize_t ix = start; ix < self.size(); ++ix) {
        PyObject* boxed = box_value(entry_value<T>(self.entries[ix]));
        if (!boxed) {
            Py_DECREF(result);
        }
        PyObject* intermediate = PyNumber_Add(result, boxed);
        Py_DECREF(result);
        Py_DECREF(boxed);
        if (!intermediate) {
            return nullptr;
        }
        result = intermediate;
    }
    return result;
}

template<typename T>
struct sum;

template<>
struct sum<PyObject*> {
private:
    template<typename F>
    static PyObject* homogeneous_loop(F&& add, jlist& self, PyObject* result) {
        PyTypeObject* tp = self.homogeneous_type_ptr();
        for (Py_ssize_t ix = 0; ix < self.size(); ++ix) {
            PyObject* summand = add(self.entries[ix].as_ob, result);
            Py_DECREF(result);
            if (!summand) {
                return nullptr;
            }
            if (__builtin_expect(Py_TYPE(summand) != tp, 0)) {
                return boxing_sum<PyObject*>(self, summand, ix + 1);
            }
            result = summand;
        }
        return result;
    }

public:
    static PyObject* homogeneous(jlist& self, PyObject* result) {
        PyTypeObject* tp = self.homogeneous_type_ptr();
        if (!result || tp != Py_TYPE(result)) {
            return boxing_sum<PyObject*>(self, result);
        }

        if (tp->tp_as_number && tp->tp_as_number->nb_add) {
            return homogeneous_loop(tp->tp_as_number->nb_add, self, result);
        }
        else if (tp->tp_as_sequence && tp->tp_as_sequence->sq_concat) {
            return homogeneous_loop(tp->tp_as_sequence->sq_concat, self, result);
        }

        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for +: '%.200s' and '%.200s'",
                     tp->tp_name,
                     tp->tp_name);
        return nullptr;
    }
    static PyObject* heterogeneous(jlist& self, PyObject* result) {
        return boxing_sum<PyObject*>(self, result);
    }
};

template<>
struct sum<std::int64_t> {
    static PyObject* f(jlist& self, PyObject* start_ob) {
        std::int64_t result = 0;
        if (start_ob) {
            auto maybe_result = maybe_unbox<std::int64_t>(start_ob);
            if (!maybe_result) {
                return boxing_sum<std::int64_t>(self, start_ob);
            }
            result = *maybe_result;
        }

        Py_ssize_t ix = 0;
        for (entry e : self.entries) {
            std::int64_t intermediate_result;
            if (__builtin_add_overflow(result, e.as_int, &intermediate_result)) {
                PyObject* boxed_start = box_value(result);
                if (!boxed_start) {
                    return nullptr;
                }
                PyObject* out = boxing_sum<std::int64_t>(self, boxed_start, ix);
                Py_DECREF(boxed_start);
                return out;
            }
            else {
                result = intermediate_result;
            }
            ++ix;
        }

        return PyLong_FromLongLong(result);
    }
};

template<>
struct sum<double> {
    static PyObject* f(jlist& self, PyObject* start_ob) {
        double result = 0;
        if (start_ob) {
            if (PyFloat_CheckExact(start_ob)) {
                result = PyFloat_AsDouble(start_ob);
            }
            else if (PyLong_CheckExact(start_ob)) {
                auto maybe_result = maybe_unbox<std::int64_t>(start_ob);
                if (!maybe_result) {
                    return boxing_sum<double>(self, start_ob);
                }
                result = *maybe_result;
            }
            else {
                return boxing_sum<double>(self, start_ob);
            }
        }

        for (entry e : self.entries) {
            result += e.as_double;
        }

        return PyFloat_FromDouble(result);
    }
};
}  // namespace detail

PyObject* sum(PyObject* module, PyObject* args) {
    module_state* state = reinterpret_cast<module_state*>(PyModule_GetState(module));
    PyObject* iterable;
    PyObject* start = nullptr;

    if (!PyArg_UnpackTuple(args, "sum", 1, 2, &iterable, &start)) {
        return nullptr;
    }

    if (Py_TYPE(iterable) != state->jlist_type) {
        return PyObject_Call(state->builtin_sum, args, nullptr);
    }

    jlist& self = *reinterpret_cast<jlist*>(iterable);

    if (!self.size()) {
        if (!start) {
            return PyLong_FromLong(0);
        }
        Py_INCREF(start);
        return start;
    }

    switch (self.tag()) {
    case entry_tag::as_homogeneous_ob:
        return detail::sum<PyObject*>::homogeneous(self, start);
    case entry_tag::as_heterogeneous_ob:
        return detail::sum<PyObject*>::heterogeneous(self, start);
    case entry_tag::as_int:
        return detail::sum<std::int64_t>::f(self, start);
    case entry_tag::as_double:
        return detail::sum<double>::f(self, start);
    default:
        // `tag` cannot be `unset` because we check the size above.
        __builtin_unreachable();
    }
}

PyMethodDef sum_method = {"sum", sum, METH_VARARGS, sum_doc};

PyDoc_STRVAR(
    range_doc,
    "range(stop) -> jlist\n"
    "range(start, stop[, step]) -> jlist\n"
    "\n"
    "Return an object that produces a sequence of integers from start (inclusive)\n"
    "to stop (exclusive) by step.  range(i, j) produces i, i+1, i+2, ..., j-1.\n"
    "start defaults to 0, and stop is omitted!  range(4) produces 0, 1, 2, 3.\n"
    "These are exactly the valid indices for a list of 4 elements.\n"
    "When step is given, it specifies the increment (or decrement).");

PyObject* range(PyObject* module, PyObject* args) {
    Py_ssize_t start = 0;
    Py_ssize_t stop;
    Py_ssize_t step = 1;

    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    if (!nargs) {
        PyErr_SetString(PyExc_TypeError, "range expected 1 argument, got 0");
        return nullptr;
    }
    if (nargs == 1) {
        PyObject* stop_ob = PyTuple_GET_ITEM(args, 0);
        stop = PyNumber_AsSsize_t(stop_ob, PyExc_OverflowError);
        if (stop == -1 && PyErr_Occurred()) {
            return nullptr;
        }
    }
    else if (nargs == 2) {
        PyObject* start_ob = PyTuple_GET_ITEM(args, 0);
        start = PyNumber_AsSsize_t(start_ob, PyExc_OverflowError);
        if (start == -1 && PyErr_Occurred()) {
            return nullptr;
        }

        PyObject* stop_ob = PyTuple_GET_ITEM(args, 1);
        stop = PyNumber_AsSsize_t(stop_ob, PyExc_OverflowError);
        if (stop == -1 && PyErr_Occurred()) {
            return nullptr;
        }
    }
    else if (nargs == 3) {
        PyObject* start_ob = PyTuple_GET_ITEM(args, 0);
        start = PyNumber_AsSsize_t(start_ob, PyExc_OverflowError);
        if (start == -1 && PyErr_Occurred()) {
            return nullptr;
        }

        PyObject* stop_ob = PyTuple_GET_ITEM(args, 1);
        stop = PyNumber_AsSsize_t(stop_ob, PyExc_OverflowError);
        if (stop == -1 && PyErr_Occurred()) {
            return nullptr;
        }

        PyObject* step_ob = PyTuple_GET_ITEM(args, 2);
        step = PyNumber_AsSsize_t(step_ob, PyExc_OverflowError);
        if (step == -1 && PyErr_Occurred()) {
            return nullptr;
        }
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "range expected at most 3 arguments, got: %zd",
                     nargs);
        return nullptr;
    }

    jlist* out = detail::new_jlist(module, entry_tag::as_int);
    if (!out) {
        return nullptr;
    }

    auto compute_size =
        [&](Py_ssize_t low, Py_ssize_t high, Py_ssize_t step) -> Py_ssize_t {
        if (low >= high) {
            return 0;
        }

        return (high - low - 1) / step + 1;
    };

    if (step > 0) {
        out->entries.resize(compute_size(start, stop, step));
        Py_ssize_t out_ix = 0;
        for (std::int64_t ix = start; ix < stop; ix += step) {
            out->entries[out_ix++].as_int = ix;
        }
    }
    else {
        out->entries.resize(compute_size(stop, start, -step));
        Py_ssize_t out_ix = 0;
        for (std::int64_t ix = start; ix > stop; ix += step) {
            out->entries[out_ix++].as_int = ix;
        }
    }

    return reinterpret_cast<PyObject*>(out);
}

PyMethodDef range_method = {"range", range, METH_VARARGS, range_doc};

PyDoc_STRVAR(zeros_doc, "Returns a new jlist with the given size filled with zeros.");

PyObject* zeros(PyObject* module, PyObject* size_ob) {
    Py_ssize_t size = PyNumber_AsSsize_t(size_ob, PyExc_OverflowError);
    if (size == -1 && PyErr_Occurred()) {
        return nullptr;
    }

    jlist* out = detail::new_jlist(module, entry_tag::as_int);
    if (!out) {
        return nullptr;
    }

    out->entries.resize(size);
    return reinterpret_cast<PyObject*>(out);
}

PyMethodDef zeros_method = {"zeros", zeros, METH_O, zeros_doc};

PyMethodDef methods[] = {
    all_method,
    any_method,
    sum_method,
    range_method,
    zeros_method,
    {nullptr, nullptr, 0, nullptr},
};

int module_traverse(PyObject* self, visitproc visit, void* arg) {
    module_state* state = reinterpret_cast<module_state*>(PyModule_GetState(self));
    if (!state) {
        return 0;
    }

    Py_VISIT(state->jlist_type);
    Py_VISIT(state->builtin_sum);
    return 0;
}

void module_free(PyObject* self) {
    module_state* state = reinterpret_cast<module_state*>(PyModule_GetState(self));
    if (state) {
        Py_CLEAR(state->jlist_type);
        Py_CLEAR(state->builtin_sum);
    }
}

PyModuleDef module = {
    PyModuleDef_HEAD_INIT,
    "jlist.ops",
    nullptr,
    sizeof(module_state),
    methods,
    nullptr,
    module_traverse,
    nullptr,
    reinterpret_cast<freefunc>(module_free),
};

PyMODINIT_FUNC PyInit_ops() {
    PyObject* builtins = PyImport_ImportModule("builtins");
    if (!builtins) {
        return nullptr;
    }
    scope_guard decref_builtins([&] { Py_DECREF(builtins); });

    PyObject* m = PyModule_Create(&module);
    if (!m) {
        return nullptr;
    }
    scope_guard decref_m([&] { Py_DECREF(m); });

    module_state* state = reinterpret_cast<module_state*>(PyModule_GetState(m));

    PyObject* jlist_mod = PyImport_ImportModule("jlist.jlist");
    if (!jlist_mod) {
        return nullptr;
    }

    state->jlist_type = reinterpret_cast<PyTypeObject*>(
        PyObject_GetAttrString(jlist_mod, "jlist"));
    Py_DECREF(jlist_mod);
    if (!state->jlist_type) {
        return nullptr;
    }

    if (!(state->builtin_all = PyObject_GetAttrString(builtins, "all"))) {
        return nullptr;
    }
    scope_guard decref_builtin_all([&] { Py_DECREF(state->builtin_all); });

    if (!(state->builtin_any = PyObject_GetAttrString(builtins, "any"))) {
        return nullptr;
    }
    scope_guard decref_builtin_any([&] { Py_DECREF(state->builtin_any); });

    if (!(state->builtin_sum = PyObject_GetAttrString(builtins, "sum"))) {
        return nullptr;
    }

    decref_builtin_any.dismiss();
    decref_builtin_all.dismiss();
    decref_m.dismiss();
    return m;
}
}  // namespace jl::ops
