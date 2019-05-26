#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <vector>

#include <Python.h>
#include <structmember.h>

#include "jlist/jlist.h"
#include "jlist/scope_guard.h"

namespace jl {
extern PyTypeObject jlist_type;

template<typename UnboxedType>
bool box_values(jlist& list) {
    bool unwind = false;
    Py_ssize_t ix = 0;
    for (; ix < list.size(); ++ix) {
        UnboxedType unboxed = entry_value<UnboxedType>(list.entries[ix]);
        PyObject* as_ob = box_value(unboxed);
        if (!as_ob) {
            unwind = true;
            break;
        }
        // move the new reference into the list
        list.entries[ix].as_ob = as_ob;
    }

    if (unwind) {
        for (Py_ssize_t unwind_ix = 0; unwind_ix < ix; ++unwind_ix) {
            PyObject* boxed = list.entries[ix].as_ob;
            UnboxedType unboxed = unbox_value<UnboxedType>(boxed);
            Py_DECREF(boxed);
            entry_value<UnboxedType>(list.entries[ix]) = unboxed;
        }
    }

    list.set_tag(entry_tag::as_ob);
    return unwind;
}

bool maybe_box_values(jlist& list) {
    switch (list.tag) {
    case entry_tag::unset:
    case entry_tag::as_ob:
        return false;
    case entry_tag::as_int:
        return box_values<std::int64_t>(list);
    case entry_tag::as_double:
        return box_values<double>(list);
    default:
        __builtin_unreachable();
    }
}

namespace detail {
Py_ssize_t adjust_ix(Py_ssize_t ix, Py_ssize_t size, bool clamp) {
    if (ix < 0) {
        ix += size;
    }
    if (clamp) {
        ix = std::clamp<Py_ssize_t>(ix, 0, size);
    }
    return ix;
}
}  // namespace detail

namespace methods {
namespace detail {
bool setitem_helper(jlist& self, entry& entry, PyObject* ob, bool clear) {
    auto add_object = [&] {
        if (clear) {
            Py_DECREF(entry.as_ob);
        }
        Py_INCREF(ob);
        entry.as_ob = ob;
    };

    auto add_unboxed = [&](auto type) {
        using T = decltype(type);
        auto maybe_unboxed = maybe_unbox<T>(ob);
        if (!maybe_unboxed) {
            if (box_values<T>(self)) {
                return true;
            }
            add_object();
            return false;
        }
        entry_value<T>(entry) = *maybe_unboxed;
        return false;
    };

    PyTypeObject* tp = Py_TYPE(ob);
    int overflow = 0;

    switch (self.tag) {
    case entry_tag::unset:
        if (tp == &PyFloat_Type) {
            self.set_tag(entry_tag::as_double);
            entry.as_double = PyFloat_AsDouble(ob);
            return false;
        }

        if (tp == &PyLong_Type) {
            entry.as_int = PyLong_AsLongLongAndOverflow(ob, &overflow);
            if (!overflow) {
                self.set_tag(entry_tag::as_int);
                return false;
            }
        }
        // fallthrough
        self.set_tag(entry_tag::as_ob);
        add_object();
        return false;
    case entry_tag::as_ob:
        add_object();
        return false;
    case entry_tag::as_int:
        return add_unboxed(std::int64_t{});
    case entry_tag::as_double:
        return add_unboxed(double{});
    default:
        __builtin_unreachable();
    }
}

entry* get_entry(jlist& self, Py_ssize_t ix) {
    if (ix < 0 || ix >= self.size()) {
        return nullptr;
    }

    return &self.entries[ix];
}

template<typename T>
bool box_and_extend(jlist& self, jlist& other) {
    std::size_t original_size = self.entries.size();
    for (entry& e : other.entries) {
        PyObject* boxed = box_value(entry_value<T>(e));

        if (!boxed) {
            for (std::size_t ix = original_size; ix < self.entries.size(); ++ix) {
                Py_DECREF(self.entries[ix].as_ob);
            }
            self.entries.erase(self.entries.begin() + original_size, self.entries.end());
            return true;
        }
    }

    return false;
}

bool extend_helper(jlist& self, jlist& other) {
    if (!other.size()) {
        // don't start boxing if there are no entries in `other`
        return false;
    }

    if (self.tag == other.tag || self.tag == entry_tag::unset) {
        // the types are the same, just use vector insert to add all the items
        self.entries.insert(self.entries.end(),
                            other.entries.begin(),
                            other.entries.end());
        if (self.tag == entry_tag::as_ob) {
            // the type is object, so we need to add a new reference to all the
            // items
            for (entry& e : other.entries) {
                Py_INCREF(e.as_ob);
            }
        }
        self.set_tag(other.tag);  // update in case `tag` was `unset`
        return false;
    }

    // the types are difference, we may need to box the lhs into objects so
    // that we can add all the items into a single list
    if (maybe_box_values(self)) {
        return true;
    }

    switch (other.tag) {
    case entry_tag::as_ob:
        return box_and_extend<PyObject*>(self, other);
    case entry_tag::as_int:
        return box_and_extend<std::int64_t>(self, other);
    case entry_tag::as_double:
        return box_and_extend<double>(self, other);
    default:
        __builtin_unreachable();
    }
}

bool extend_fast_sequence(jlist& self, PyObject* other) {
    Py_ssize_t size = PySequence_Fast_GET_SIZE(other);
    if (!size) {
        // don't do anything if the sequence is empty
        return false;
    }
    Py_ssize_t base_size = self.size();
    self.entries.resize(base_size + size);

    PyObject** items = PySequence_Fast_ITEMS(other);
    for (Py_ssize_t ix = 0; ix < size; ++ix) {
        if (detail::setitem_helper(self,
                                   self.entries[base_size + ix],
                                   items[ix],
                                   false)) {
            if (self.tag == entry_tag::as_ob) {
                for (Py_ssize_t unwind_ix = 0; unwind_ix < ix; ++unwind_ix) {
                    Py_DECREF(self.entries[unwind_ix].as_ob);
                }
            }
            return true;
        }
    }

    return false;
}

bool extend_iterable(jlist& self, PyObject* other) {
    PyObject* it = PyObject_GetIter(other);
    if (!it) {
        return true;
    }

    Py_ssize_t maybe_size = PySequence_Size(other);
    if (maybe_size > 0) {
        self.entries.reserve(self.size() + maybe_size);
    }
    else {
        PyErr_Clear();
    }

    Py_ssize_t ix = 0;
    PyObject* ob;
    while ((ob = PyIter_Next(it))) {
        entry& e = self.entries.emplace_back();
        if (detail::setitem_helper(self, e, ob, false)) {
            if (self.tag == entry_tag::as_ob) {
                for (Py_ssize_t unwind_ix = 0; unwind_ix < ix; ++unwind_ix) {
                    Py_DECREF(self.entries[unwind_ix].as_ob);
                }
            }
            return true;
        }
        ++ix;
    }
    Py_DECREF(it);
    return PyErr_Occurred();
}

bool extend_helper(jlist& self, PyObject* other) {
    if (Py_TYPE(other) == &jlist_type) {
        // fast path code when we know the rhs is also a jlist
        return extend_helper(self, *reinterpret_cast<jlist*>(other));
    }

    if (PyList_CheckExact(other) || PyTuple_CheckExact(other)) {
        return extend_fast_sequence(self, other);
    }

    return extend_iterable(self, other);
}

template<typename I>
jlist* new_jlist(entry_tag tag, I begin, I end) {
    jlist* out = PyObject_GC_New(jlist, &jlist_type);
    if (!out) {
        return nullptr;
    }
    out->tag = tag;
    new (&out->entries) std::vector<entry>(begin, end);
    if (tag == entry_tag::as_ob) {
        for (entry e : out->entries) {
            Py_INCREF(e.as_ob);
        }
        PyObject_GC_Track(out);
    }
    return out;
}

jlist* new_jlist(entry_tag tag) {
    jlist* out = PyObject_GC_New(jlist, &jlist_type);
    if (!out) {
        return nullptr;
    }
    out->tag = tag;
    new (&out->entries) std::vector<entry>;
    if (tag == entry_tag::as_ob) {
        PyObject_GC_Track(out);
    }
    return out;
}

void clear_helper(jlist& self) {
    if (self.tag == entry_tag::as_ob) {
        for (entry e : self.entries) {
            Py_DECREF(e.as_ob);
        }
    }
    self.entries.clear();
}
}  // namespace detail

PyObject* new_(PyTypeObject*, PyObject*, PyObject*) {
    return reinterpret_cast<PyObject*>(detail::new_jlist(entry_tag::unset));
}

int init(PyObject* _self, PyObject* args, PyObject* kwargs) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    if (kwargs && PyDict_Size(kwargs)) {
        PyErr_SetString(PyExc_TypeError, "jlist doesn't accept keywords");
        return -1;
    }

    // you can manually call `jlist.__init__` on an existing list, which should reset
    // the state
    detail::clear_helper(self);

    if (PyTuple_GET_SIZE(args) == 0) {
        return 0;
    }
    else if (PyTuple_GET_SIZE(args) == 1) {
        if (detail::extend_helper(self, PyTuple_GET_ITEM(args, 0))) {
            return -1;
        }
        return 0;
    }

    PyErr_SetString(PyExc_TypeError, "jlist accepts either 0 or 1 positional argument");
    return -1;

}

void deallocate(PyObject* _self) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    if (self.tag == entry_tag::as_ob) {
        PyObject_GC_UnTrack(_self);
        for (entry& e : self.entries) {
            Py_DECREF(e.as_ob);
        }
    }

    self.entries.~vector();
    PyObject_GC_Del(_self);
}

PyObject* repr(PyObject* _self) {
    Py_ssize_t rc = Py_ReprEnter(_self);
    if (rc != 0) {
        if (rc > 0) {
            return PyUnicode_FromString("jlist([...])");
        }
        return nullptr;
    }
    scope_guard repr([&] { Py_ReprLeave(_self); });

    jlist& self = *reinterpret_cast<jlist*>(_self);
    if (!self.size()) {
        return PyUnicode_FromString("jlist([])");
    }

    _PyUnicodeWriter writer;
    _PyUnicodeWriter_Init(&writer);
    scope_guard cleanup_writer([&] { _PyUnicodeWriter_Dealloc(&writer); });

    writer.overallocate = 1;
    writer.min_length = 7 + 1 + 1 + (2 + 1) * (self.size() - 1) + 1;
    if (_PyUnicodeWriter_WriteASCIIString(&writer, "jlist([", 7) < 0) {
        return nullptr;
    }

    Py_ssize_t ix = 0;

    switch (self.tag) {
    case entry_tag::as_ob:
        for (entry e : self.entries) {
            if (ix > 0) {
                if (_PyUnicodeWriter_WriteASCIIString(&writer, ", ", 2) < 0) {
                    return nullptr;
                }
            }

            PyObject* repr = PyObject_Repr(e.as_ob);
            if (!repr) {
                return nullptr;
            }
            int err = _PyUnicodeWriter_WriteStr(&writer, repr);
            Py_DECREF(repr);
            if (err < 0) {
                return nullptr;
            }
            ++ix;
        }
        break;
    case entry_tag::as_int:
        std::array<char, 20> buffer;
        for (entry e : self.entries) {
            if (ix > 0) {
                if (_PyUnicodeWriter_WriteASCIIString(&writer, ", ", 2) < 0) {
                    return nullptr;
                }
            }

            auto [p, ec] = std::to_chars(buffer.begin(), buffer.end(), e.as_int);
            if (_PyUnicodeWriter_WriteASCIIString(&writer,
                                                  buffer.begin(),
                                                  p - buffer.begin()) < 0) {
                return nullptr;
            }
            ++ix;
        }
        break;
    case entry_tag::as_double:
        for (entry e : self.entries) {
            if (ix > 0) {
                if (_PyUnicodeWriter_WriteASCIIString(&writer, ", ", 2) < 0) {
                    return nullptr;
                }
            }

            std::string str = std::to_string(e.as_double);
            if (_PyUnicodeWriter_WriteASCIIString(&writer, str.data(), str.size()) < 0) {
                return nullptr;
            }
            ++ix;
        }
        break;
    default:
        __builtin_unreachable();
    }

    writer.overallocate = 0;
    if (_PyUnicodeWriter_WriteASCIIString(&writer, "])", 2) < 0) {
        return nullptr;
    }

    cleanup_writer.dismiss();
    return _PyUnicodeWriter_Finish(&writer);
}

PyObject* richcompare(PyObject* _self, PyObject* _other, int cmp) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    if (!(cmp == Py_EQ || cmp == Py_NE)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    if (Py_TYPE(_other) != &jlist_type) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    jlist& other = *reinterpret_cast<jlist*>(_other);
    if (self.size() != other.size()) {
        return PyBool_FromLong(cmp == Py_NE);
    }

    if (!self.size()) {
        return PyBool_FromLong(cmp == Py_EQ);
    }

    auto box_lhs_loop = [&](auto type) -> PyObject* {
        using T = decltype(type);
        for (Py_ssize_t ix = 0; ix < self.size(); ++ix) {
            T unboxed_lhs = entry_value<T>(self.entries[ix]);
            PyObject* lhs = box_value(unboxed_lhs);
            if (!lhs) {
                return nullptr;
            }
            PyObject* rhs = other.entries[ix].as_ob;

            int r = PyObject_RichCompareBool(lhs, rhs, Py_EQ);
            Py_DECREF(lhs);
            if (r < 0) {
                return nullptr;
            }
            if (!r) {
                return PyBool_FromLong(cmp == Py_NE);
            }
        }

        return PyBool_FromLong(cmp == Py_EQ);
    };

    auto box_rhs_loop = [&](auto type) -> PyObject* {
        using T = decltype(type);
        for (Py_ssize_t ix = 0; ix < self.size(); ++ix) {
            PyObject* lhs = self.entries[ix].as_ob;
            T unboxed_rhs = entry_value<T>(other.entries[ix]);
            PyObject* rhs = box_value(unboxed_rhs);
            if (!rhs) {
                return nullptr;
            }

            int r = PyObject_RichCompareBool(lhs, rhs, Py_EQ);
            Py_DECREF(rhs);
            if (r < 0) {
                return nullptr;
            }
            if (!r) {
                return PyBool_FromLong(cmp == Py_NE);
            }
        }

        return PyBool_FromLong(cmp == Py_EQ);
    };

    auto prim_loop = [&](auto lhs_type, auto rhs_type) {
        using LHS = decltype(lhs_type);
        using RHS = decltype(rhs_type);
        for (Py_ssize_t ix = 0; ix < self.size(); ++ix) {
            LHS lhs = entry_value<LHS>(self.entries[ix]);
            RHS rhs = entry_value<RHS>(other.entries[ix]);

            if (lhs != rhs) {
                return PyBool_FromLong(cmp == Py_NE);
            }
        }

        return PyBool_FromLong(cmp == Py_EQ);
    };

    switch (self.tag) {
    case entry_tag::as_ob:
        switch (other.tag) {
        case entry_tag::as_ob:
            for (Py_ssize_t ix = 0; ix < self.size(); ++ix) {
                PyObject* lhs = self.entries[ix].as_ob;
                PyObject* rhs = other.entries[ix].as_ob;
                int r = PyObject_RichCompareBool(lhs, rhs, Py_EQ);
                if (r < 0) {
                    return nullptr;
                }
                if (!r) {
                    return PyBool_FromLong(cmp == Py_NE);
                }
            }
            return PyBool_FromLong(cmp == Py_EQ);
        case entry_tag::as_int:
            return box_rhs_loop(std::int64_t{});
        case entry_tag::as_double:
            return box_rhs_loop(double{});
        default:
            __builtin_unreachable();
        }
    case entry_tag::as_int:
        switch(other.tag) {
        case entry_tag::as_ob:
            return box_lhs_loop(std::int64_t{});
        case entry_tag::as_int:
            return prim_loop(std::int64_t{}, std::int64_t{});
        case entry_tag::as_double:
            return prim_loop(std::int64_t{}, double{});
        default:
            __builtin_unreachable();
        }
    case entry_tag::as_double:
        switch(other.tag) {
        case entry_tag::as_ob:
            return box_lhs_loop(double{});
        case entry_tag::as_int:
            return prim_loop(double{}, std::int64_t{});
        case entry_tag::as_double:
            return prim_loop(double{}, double{});
        default:
            __builtin_unreachable();
        }
    default:
        __builtin_unreachable();
    }

    __builtin_unreachable();
}

PyDoc_STRVAR(append_doc, "Append object to the end of the jlist.h");

PyObject* append(PyObject* _self, PyObject* ob) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    entry& e = self.entries.emplace_back();
    if (detail::setitem_helper(self, e, ob, false)) {
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyMethodDef append_method = {"append", append, METH_O, append_doc};

PyDoc_STRVAR(clear_doc, "Remove all items from self.");

PyObject* clear(PyObject* _self, PyObject*) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    detail::clear_helper(self);
    Py_RETURN_NONE;
}

PyMethodDef clear_method = {"clear", clear, METH_NOARGS, clear_doc};

PyDoc_STRVAR(count_doc, "Return the number of occurrences of value in self.");

PyObject* count(PyObject* _self, PyObject* value) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    if (!self.size()) {
        return PyLong_FromLong(0);
    }

    Py_ssize_t count = 0;

    auto boxing_count = [&](auto type) {
        using T = decltype(type);
        for (entry e : self.entries) {
            PyObject* boxed = box_value(entry_value<T>(e));
            if (!boxed) {
                return true;
            }
            int r = PyObject_RichCompareBool(boxed, value, Py_EQ);
            Py_DECREF(boxed);
            if (r < 0) {
                return true;
            }
            count += r;
        }

        return false;
    };

    switch (self.tag) {
    case entry_tag::as_ob:
        for (entry e : self.entries) {
            int r = PyObject_RichCompareBool(e.as_ob, value, Py_EQ);
            if (r < 0) {
                return nullptr;
            }
            count += r;
        }
        break;
    case entry_tag::as_int: {
        auto maybe_unboxed = maybe_unbox<std::int64_t>(value);
        if (!maybe_unboxed) {
            if (boxing_count(std::int64_t{})) {
                return nullptr;
            }
        }
        else {
            std::int64_t rhs = *maybe_unboxed;
            for (entry e : self.entries) {
                count += e.as_int == rhs;
            }
        }
        break;
    }
    case entry_tag::as_double: {
        auto maybe_unboxed = maybe_unbox<double>(value);
        if (!maybe_unboxed) {
            if (boxing_count(double{})) {
                return nullptr;
            }
        }
        else {
            std::int64_t rhs = *maybe_unboxed;
            for (entry e : self.entries) {
                count += e.as_double == rhs;
            }
        }
        break;
    }
    default:
        __builtin_unreachable();
    }

    return PyLong_FromSsize_t(count);
}

PyMethodDef count_method = {"count", count, METH_O, count_doc};

PyDoc_STRVAR(copy_doc, "Return a shallow copy of the jlist.");

PyObject* copy(PyObject* _self, PyObject*) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    return reinterpret_cast<PyObject*>(
        detail::new_jlist(self.tag, self.entries.begin(), self.entries.end()));
}

PyMethodDef copy_method = {"copy", copy, METH_NOARGS, copy_doc};

PyDoc_STRVAR(extend_doc, "Extend jlist by appending elements from the iterable.");

PyObject* extend(PyObject* _self, PyObject* ob) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    if (detail::extend_helper(self, ob)) {
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyMethodDef extend_method = {"extend", extend, METH_O, extend_doc};

PyDoc_STRVAR(index_doc, "Return the first index of value in self.");

namespace detail {
Py_ssize_t index_helper(jlist& self,
                        PyObject* value,
                        Py_ssize_t start = 0,
                        Py_ssize_t stop = 9223372036854775807L) {
    if (!self.size()) {
        return -1;
    }

    start = jl::detail::adjust_ix(start, self.size(), true);
    stop = jl::detail::adjust_ix(stop, self.size(), true);

    auto boxing_index = [&](auto type) -> Py_ssize_t {
        using T = decltype(type);
        // the comparison can cause the list to resize
        for (Py_ssize_t ix = start; ix < stop && ix < self.size(); ++ix) {
            PyObject* boxed = box_value(entry_value<T>(self.entries[ix]));
            if (!boxed) {
                return -2;
            }
            int r = PyObject_RichCompareBool(boxed, value, Py_EQ);
            Py_DECREF(boxed);
            if (r < 0) {
                return -2;
            }
            if (r) {
                return ix;
            }
        }

        return -1;
    };

    switch (self.tag) {
    case entry_tag::as_ob:
        // the comparison can cause the list to resize
        for (Py_ssize_t ix = start; ix < stop && ix < self.size(); ++ix) {
            int r = PyObject_RichCompareBool(self.entries[ix].as_ob, value, Py_EQ);
            if (r < 0) {
                return -2;
            }
            if (r) {
                return ix;
            }
        }
        return -1;
    case entry_tag::as_int: {
        auto maybe_unboxed = maybe_unbox<std::int64_t>(value);
        if (!maybe_unboxed) {
            return boxing_index(std::int64_t{});
        }
        else {
            std::int64_t rhs = *maybe_unboxed;
            for (Py_ssize_t ix = start; ix < stop; ++ix) {
                if (self.entries[ix].as_int == rhs) {
                    return ix;
                }
            }
            return -1;
        }
    }
    case entry_tag::as_double: {
        auto maybe_unboxed = maybe_unbox<double>(value);
        if (!maybe_unboxed) {
            return boxing_index(double{});
        }
        else {
            double rhs = *maybe_unboxed;
            for (Py_ssize_t ix = start; ix < stop; ++ix) {
                if (self.entries[ix].as_double == rhs) {
                    return ix;
                }
            }
            return -1;
        }
    }
    default:
        __builtin_unreachable();
    }

    __builtin_unreachable();
}
}  // namespace detail

PyObject* index(PyObject* _self, PyObject* args) {
    jlist& self = *reinterpret_cast<jlist*>(_self);
    PyObject* value = nullptr;
    Py_ssize_t start = 0;
    Py_ssize_t stop = self.size();

    auto clamp_bound = [&](PyObject* ob, Py_ssize_t& value) {
        value = PyNumber_AsSsize_t(ob, PyExc_OverflowError);
        if (value == -1) {
            if (PyErr_Occurred() == PyExc_OverflowError) {
                PyErr_Clear();
                PyObject* zero = PyLong_FromSsize_t(0);
                if (!zero) {
                    return true;
                }
                int r = PyObject_RichCompareBool(ob, zero, Py_LE);
                Py_DECREF(zero);
                if (r < 0) {
                    return true;
                }
                if (r) {
                    value = 0;
                }
                else {
                    value = self.size();
                }
            }
            else {
                return true;
            }
        }
        return false;
    };

    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    if (!nargs) {
        PyErr_SetString(PyExc_TypeError, "index() takes at least 1 argument (0 given)");
        return nullptr;
    }
    if (nargs >= 1) {
        value = PyTuple_GET_ITEM(args, 0);
    }
    if (nargs >= 2) {
        if (clamp_bound(PyTuple_GET_ITEM(args, 1), start)) {
            return nullptr;
        }
    }
    if (nargs >= 3) {
        if (clamp_bound(PyTuple_GET_ITEM(args, 2), stop)) {
            return nullptr;
        }
    }
    if (nargs > 3) {
        PyErr_Format(PyExc_TypeError,
                     "index() takes at most 3 arguments (%zd given)",
                     nargs);
        return nullptr;
    }

    Py_ssize_t ix = detail::index_helper(self, value, start, stop);
    if (ix == -2) {
        return nullptr;
    }
    else if (ix == -1) {
        PyErr_Format(PyExc_ValueError, "%R is not in jlist", value);
        return nullptr;
    }
    return PyLong_FromSsize_t(ix);
}

PyMethodDef index_method = {"index", index, METH_VARARGS, index_doc};

PyDoc_STRVAR(insert_doc, "Insert object before index into self.");

PyObject* insert(PyObject* _self, PyObject* args) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    if (PyTuple_GET_SIZE(args) != 2) {
        PyErr_Format(PyExc_TypeError,
                     "jlist.insert expects exactly 2 arguments, got: %zd",
                     PyTuple_GET_SIZE(args));
        return nullptr;
    }
    PyObject* index_ob = PyTuple_GET_ITEM(args, 0);
    PyObject* value = PyTuple_GET_ITEM(args, 1);

    Py_ssize_t index = PyNumber_AsSsize_t(index_ob, PyExc_IndexError);
    if (index == -1 && PyErr_Occurred()) {
        return nullptr;
    }

    index = jl::detail::adjust_ix(index, self.size(), true);

    if (index >= self.size()) {
        return append(_self, value);
    }

    auto it = self.entries.emplace(self.entries.begin() + index);
    if (detail::setitem_helper(self, *it, value, false)) {
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyMethodDef insert_method = {"insert", insert, METH_VARARGS, insert_doc};

PyDoc_STRVAR(pop_doc, "Remove and return item at index (default last).");

PyObject* pop(PyObject* _self, PyObject* args) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    Py_ssize_t ix = 0;
    if (nargs == 0) {
        ix = self.size() - 1;
    }
    else if (nargs == 1) {
        ix = PyNumber_AsSsize_t(PyTuple_GET_ITEM(args, 0), PyExc_IndexError);
        if (ix == -1 && PyErr_Occurred()) {
            return nullptr;
        }
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "pop() takes at most 1 argument (%zd given)",
                     nargs);
        return nullptr;
    }

    if (!self.size()) {
        PyErr_SetString(PyExc_IndexError, "pop from empty jlist");
        return nullptr;
    }

    entry* maybe_e = detail::get_entry(self, ix);
    if (!maybe_e) {
        PyErr_SetString(PyExc_IndexError, "pop index out of range");
        return nullptr;
    }
    entry& e = *maybe_e;

    PyObject* out;
    switch (self.tag) {
    case entry_tag::as_ob:
        out = e.as_ob;
        break;
    case entry_tag::as_int:
        out = box_value(e.as_int);
        break;
    case entry_tag::as_double:
        out = box_value(e.as_double);
        break;
    default:
        __builtin_unreachable();
    }

    self.entries.erase(self.entries.begin() + ix);
    return out;
}

PyMethodDef pop_method = {"pop", pop, METH_VARARGS, pop_doc};

PyDoc_STRVAR(remove_doc, "Remove first occurrence of value.");

PyObject* remove(PyObject* _self, PyObject* value) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    Py_ssize_t ix = detail::index_helper(self, value);
    if (ix == -2) {
        return nullptr;
    }
    else if (ix == -1) {
        PyErr_SetString(PyExc_ValueError, "jlist.remove(x): x not in list");
        return nullptr;
    }
    if (self.tag == entry_tag::as_ob) {
        Py_DECREF(self.entries[ix].as_ob);
    }
    self.entries.erase(self.entries.begin() + ix);
    Py_RETURN_NONE;
}

PyMethodDef remove_method = {"remove", remove, METH_O, remove_doc};

PyDoc_STRVAR(reverse_doc, "Reverse *IN PLACE*.");

PyObject* reverse(PyObject* _self, PyObject*) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    std::reverse(self.entries.begin(), self.entries.end());
    Py_RETURN_NONE;
}

PyMethodDef reverse_method = {"reverse", reverse, METH_NOARGS, reverse_doc};

PyDoc_STRVAR(sort_doc, "Stable sort *IN PLACE*.");

namespace detail {
bool sort_without_key(jlist& self) {
    try {
        switch (self.tag) {
        case entry_tag::as_ob:
            // Python builtin.list gives a stability contract here.
            std::stable_sort(self.entries.begin(),
                             self.entries.end(),
                             [](entry a, entry b) {
                                 int r =
                                     PyObject_RichCompareBool(a.as_ob, b.as_ob, Py_LT);
                                 if (r < 0) {
                                     throw std::runtime_error("bad compare");
                                 }
                                 return r;
                             });
            break;
        case entry_tag::as_int:
            // Python builtin.list gives a stability contract here, but since we are
            // erasing the identity of the stored ints, we can use a non-stable sort.
            std::sort(self.entries.begin(), self.entries.end(), [](entry a, entry b) {
                return a.as_int < b.as_int;
            });
            break;
        case entry_tag::as_double:
            // Python builtin.list gives a stability contract here, but since we are
            // erasing the identity of the stored doubles, we can use a non-stable sort.
            std::sort(self.entries.begin(), self.entries.end(), [](entry a, entry b) {
                return a.as_double < b.as_double;
            });
            break;
        default:
            __builtin_unreachable();
        }
    }
    catch (...) {
        return true;
    }

    return false;
}
bool sort_with_key(jlist& self, PyObject* key) {
    auto compare_objects = [&](PyObject* a, PyObject* b) {
        PyObject* lhs = PyObject_CallFunctionObjArgs(key, a, nullptr);
        if (!lhs) {
            throw std::runtime_error("bad compare");
        }
        PyObject* rhs = PyObject_CallFunctionObjArgs(key, b, nullptr);
        if (!rhs) {
            Py_DECREF(lhs);
            throw std::runtime_error("bad compare");
        }
        int r = PyObject_RichCompareBool(lhs, rhs, Py_LT);
        Py_DECREF(lhs);
        Py_DECREF(rhs);
        if (r < 0) {
            throw std::runtime_error("bad compare");
        }
        return r;
    };

    auto box_and_compare = [&](auto type, entry a, entry b) {
        using T = decltype(type);
        PyObject* lhs = box_value(entry_value<T>(a));
        if (!lhs) {
            throw std::runtime_error("bad compare");
        }
        PyObject* rhs = box_value(entry_value<T>(b));
        if (!rhs) {
            throw std::runtime_error("bad compare");
        }
        int out = compare_objects(lhs, rhs);
        Py_DECREF(lhs);
        Py_DECREF(rhs);
        return out;
    };

    try {
        switch (self.tag) {
        case entry_tag::as_ob:
            // Python builtin.list gives a stability contract here.
            std::stable_sort(self.entries.begin(),
                             self.entries.end(),
                             [&](entry a, entry b) {
                                 return compare_objects(a.as_ob, b.as_ob);
                             });
            break;
        case entry_tag::as_int:
            // Python builtin.list gives a stability contract here, but since we are
            // erasing the identity of the stored ints, we can use a non-stable sort.
            std::sort(self.entries.begin(), self.entries.end(), [&](entry a, entry b) {
                return box_and_compare(std::int64_t{}, a, b);
            });
            break;
        case entry_tag::as_double:
            // Python builtin.list gives a stability contract here, but since we are
            // erasing the identity of the stored doubles, we can use a non-stable sort.
            std::sort(self.entries.begin(), self.entries.end(), [&](entry a, entry b) {
                return box_and_compare(double{}, a, b);
            });
            break;
        default:
            __builtin_unreachable();
        }
    }
    catch (...) {
        return true;
    }

    return false;
}
}  // namespace detail

PyObject* sort(PyObject* _self, PyObject* args, PyObject* kwargs) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    if (PyTuple_GET_SIZE(args)) {
        PyErr_SetString(PyExc_TypeError, "sort() takes no positional arguments");
        return nullptr;
    }

    if (!self.size()) {
        Py_RETURN_NONE;
    }

    PyObject* key = nullptr;
    if (kwargs) {
        key = PyDict_GetItemString(kwargs, "key");
    }

    if (key) {
        if (detail::sort_with_key(self, key)) {
            return nullptr;
        }
    }
    else if (detail::sort_without_key(self)) {
        return nullptr;
    }

    Py_RETURN_NONE;
}

PyMethodDef sort_method = {"sort",
                           unsafe_cast_to_pycfunction(sort),
                           METH_VARARGS | METH_KEYWORDS,
                           sort_doc};

PyObject* reduce(PyObject* self, PyObject*) {
    PyObject* as_list = PySequence_List(self);
    if (!as_list) {
        return nullptr;
    }

    PyObject* out =
        Py_BuildValue("(O(O))", reinterpret_cast<PyObject*>(&jlist_type), as_list);
    Py_DECREF(as_list);
    return out;
}

PyMethodDef reduce_method = {"__reduce__", reduce, METH_NOARGS, nullptr};

PyMethodDef methods[] = {
    append_method,
    clear_method,
    copy_method,
    count_method,
    extend_method,
    index_method,
    insert_method,
    pop_method,
    remove_method,
    reverse_method,
    sort_method,
    reduce_method,
    {nullptr, nullptr, 0, nullptr},
};

Py_ssize_t length(PyObject* _self) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    return self.size();
}

PyObject* concat(PyObject* self, PyObject* ob) {
    PyObject* out = copy(self, nullptr);
    if (out) {
        jlist& out_ref = *reinterpret_cast<jlist*>(out);

        if (detail::extend_helper(out_ref, ob)) {
            Py_DECREF(out);
            return nullptr;
        }
    }
    return out;
}

PyObject* repeat(PyObject* _self, Py_ssize_t times) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    jlist* out = detail::new_jlist(self.tag);
    if (!out) {
        return nullptr;
    }
    if (times > 0) {
        out->entries.reserve(self.size() * times);
        for (Py_ssize_t ix = 0; ix < times; ++ix) {
            out->entries.insert(out->entries.end(), self.entries.begin(), self.entries.end());
            if (self.tag == entry_tag::as_ob) {
                for (entry e : self.entries) {
                    Py_INCREF(e.as_ob);
                }
            }
        }
    }

    return reinterpret_cast<PyObject*>(out);
}

PyObject* getitem(PyObject* _self, Py_ssize_t ix) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    entry* maybe_e = detail::get_entry(self, ix);
    if (!maybe_e) {
        PyErr_SetString(PyExc_IndexError, "jlist index out of range");
        return nullptr;
    }
    const entry& e = *maybe_e;

    switch (self.tag) {
    case entry_tag::as_ob:
        return e.as_ob;
    case entry_tag::as_int:
        return box_value(e.as_int);
    case entry_tag::as_double:
        return box_value(e.as_double);
    default:
        // we don't need to handle entry_tag::unset because that means the
        // size is 0 and we will have already returned `nullptr` in the bounds
        // check
        __builtin_unreachable();
    }
}

int setitem(PyObject* _self, Py_ssize_t ix, PyObject* ob) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    entry* maybe_e = detail::get_entry(self, ix);
    if (!maybe_e) {
        PyErr_SetString(PyExc_IndexError, "jlist index out of range");
        return -1;
    }

    if (!ob) {
        if (self.tag == entry_tag::as_ob) {
            Py_DECREF(maybe_e->as_ob);
        }
        self.entries.erase(self.entries.begin() + ix);
        return 0;
    }

    if (detail::setitem_helper(self, *maybe_e, ob, true)) {
        return -1;
    }
    return 0;
}

int contains(PyObject* _self, PyObject* ob) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    Py_ssize_t ix = detail::index_helper(self, ob);
    if (ix == -2) {
        return -1;
    }
    return ix > -1;
}

PyObject* inplace_concat(PyObject* _self, PyObject* ob) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    if (detail::extend_helper(self, ob)) {
        return nullptr;
    }
    Py_INCREF(_self);
    return _self;
}

PyObject* inplace_repeat(PyObject* _self, Py_ssize_t times) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    if (times <= 0) {
        detail::clear_helper(self);
    }
    else {
        Py_ssize_t original_size = self.size();
        self.entries.reserve(original_size * times);
        for (Py_ssize_t ix = 1; ix < times; ++ix) {
            self.entries.insert(self.entries.end(),
                                self.entries.begin(),
                                self.entries.begin() + original_size);
            if (self.tag == entry_tag::as_ob) {
                for (Py_ssize_t ix = 0; ix < original_size; ++ix) {
                    Py_INCREF(self.entries[ix].as_ob);
                }
            }
        }
    }

    Py_INCREF(_self);
    return _self;
}

PySequenceMethods sq_methods = {
    length,          // sq_length
    concat,          // sq_concat
    repeat,          // sq_repeat
    getitem,         // sq_item
    nullptr,         // sq_slice
    setitem,         // sq_ass_item
    nullptr,         // sq_ass_slice
    contains,        // sq_contains
    inplace_concat,  // sq_inplace_concat
    inplace_repeat,  // inplace_repeat
};

PyObject* subscript(PyObject* _self, PyObject* item) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    if (PyIndex_Check(item)) {
        Py_ssize_t ix = PyNumber_AsSsize_t(item, PyExc_IndexError);
        if (ix == -1 && PyErr_Occurred()) {
            return nullptr;
        }
        ix = jl::detail::adjust_ix(ix, self.size(), false);
        return getitem(_self, ix);
    }
    else if (!PySlice_Check(item)) {
        PyErr_Format(PyExc_TypeError,
                     "jlist indices must be integers or slices, not %.200s",
                     item->ob_type->tp_name);
        return nullptr;
    }

    Py_ssize_t start;
    Py_ssize_t stop;
    Py_ssize_t step;
    if (PySlice_Unpack(item, &start, &stop, &step) < 0) {
        return nullptr;
    }

    Py_ssize_t slicelength = PySlice_AdjustIndices(self.size(), &start, &stop, step);
    if (slicelength < 0) {
        return reinterpret_cast<PyObject*>(detail::new_jlist(entry_tag::unset));
    }
    else if (step == 1) {
        if (start > stop) {
            start = stop;
        }
        return reinterpret_cast<PyObject*>(
            detail::new_jlist(self.tag,
                              self.entries.begin() + start,
                              self.entries.begin() + stop));
    }

    jlist* out = detail::new_jlist(self.tag);
    if (!out) {
        return nullptr;
    }
    out->entries.reserve(slicelength);

    if (step > 0) {
        for (Py_ssize_t ix = start; ix < stop; ix += step) {
            out->entries.emplace_back(self.entries[ix]);
        }
    }
    else {
        for (Py_ssize_t ix = start; ix > stop; ix += step) {
            out->entries.emplace_back(self.entries[ix]);
        }
    }
    if (out->tag == entry_tag::as_ob) {
        for (entry e : out->entries) {
            Py_INCREF(e.as_ob);
        }
    }

    return reinterpret_cast<PyObject*>(out);
}

void delete_loop_ob(jlist& self,
                    Py_ssize_t start,
                    Py_ssize_t stop,
                    Py_ssize_t step,
                    Py_ssize_t slicelength) {
    std::vector<PyObject*> garbage(slicelength);

    std::size_t cur = start;
    for (Py_ssize_t ix = 0; cur < static_cast<std::size_t>(stop); cur += step, ++ix) {
        Py_ssize_t lim = step - 1;

        garbage[ix] = self.entries[cur].as_ob;

        if (cur + step >= self.entries.size()) {
            lim = self.size() - cur - 1;
        }

        std::copy_backward(self.entries.begin() + cur + 1,
                           self.entries.begin() + cur + 1 + lim,
                           self.entries.begin() + cur - ix + lim);
    }

    cur = start + slicelength * step;
    if (cur < self.entries.size()) {
        std::copy_backward(self.entries.begin() + cur,
                           self.entries.end(),
                           self.entries.begin() + self.size() - slicelength);
    }

    self.entries.erase(self.entries.end() - slicelength, self.entries.end());

    for (PyObject* p : garbage) {
        Py_DECREF(p);
    }
}

void delete_loop_prim(jlist& self,
                      Py_ssize_t start,
                      Py_ssize_t stop,
                      Py_ssize_t step,
                      Py_ssize_t slicelength) {
    std::size_t cur = start;
    for (Py_ssize_t ix = 0; cur < static_cast<std::size_t>(stop); cur += step, ix++) {
        Py_ssize_t lim = step - 1;

        if (cur + step >= self.entries.size()) {
            lim = self.size() - cur - 1;
        }

        std::copy_backward(self.entries.begin() + cur + 1,
                           self.entries.begin() + cur + 1 + lim,
                           self.entries.begin() + cur - ix + lim);
    }

    cur = start + slicelength * step;
    if (cur < self.entries.size()) {
        std::copy_backward(self.entries.begin() + cur,
                           self.entries.end(),
                           self.entries.begin() + self.size() - slicelength);
    }

    self.entries.erase(self.entries.end() - slicelength, self.entries.end());
}

namespace detail {
int delete_slice(jlist& self,
                 Py_ssize_t start,
                 Py_ssize_t stop,
                 Py_ssize_t step,
                 Py_ssize_t slicelength) {
    if (!slicelength) {
        return 0;
    }

    if (step == 1) {
        self.entries.erase(self.entries.begin() + start, self.entries.begin() + stop);
        if (self.tag == entry_tag::as_ob) {
            for (Py_ssize_t ix = start; ix < stop; ix += stop) {
                Py_DECREF(self.entries[ix].as_ob);
            }
        }
    }
    else {
        if (step < 0) {
            stop = start + 1;
            start = stop + step * (slicelength - 1) - 1;
            step = -step;
        }


        switch(self.tag) {
        case entry_tag::as_ob:
            delete_loop_ob(self, start, stop, step, slicelength);
            break;
        case entry_tag::as_int:
        case entry_tag::as_double:
            delete_loop_prim(self, start, stop, step, slicelength);
            break;
        default:
            __builtin_unreachable();
        }
    }
    return 0;
}

void set_loop_ob(jlist& self,
                 Py_ssize_t start,
                 Py_ssize_t step,
                 jlist& other) {

    std::vector<PyObject*> garbage(other.size());

    for (Py_ssize_t cur = start, ix = 0; ix < other.size(); cur += step, ix++) {
        garbage[ix] = self.entries[cur].as_ob;
        entry ins = other.entries[ix];

        Py_INCREF(ins.as_ob);
        self.entries[cur] = ins;
    }

    for (PyObject* p : garbage) {
        Py_DECREF(p);
    }
}

void set_loop_prim(jlist& self,
                   Py_ssize_t start,
                   Py_ssize_t step,
                   jlist& other) {
    for (Py_ssize_t cur = start, ix = 0; ix < other.size(); cur += step, ix++) {
        entry ins = other.entries[ix];
        self.entries[cur] = ins;
    }
}

int set_slice(jlist& self,
              Py_ssize_t start,
              Py_ssize_t step,
              Py_ssize_t slicelength,
              jlist* other) {

    if (&self == other) {
        other = new_jlist(self.tag, self.entries.begin(), self.entries.end());
    }
    else if (self.size() == 0) {
        self.set_tag(other->tag);
    }
    else if (other->size() == 0 && slicelength == 0) {
        return 0;
    }
    else if (self.tag != other->tag) {
        if (maybe_box_values(self)) {
            return -1;
        }
        if (other->tag != entry_tag::as_ob) {
            other = new_jlist(self.tag, other->entries.begin(), other->entries.end());
            if (!other || maybe_box_values(*other)) {
                return -1;
            }
        }
        else {
            Py_INCREF(other);
        }
    }
    else {
        Py_INCREF(other);
    }

    if (step == 1) {
        if (slicelength > other->size()) {
            if (self.tag == entry_tag::as_ob) {
                for (Py_ssize_t ix = other->size(); ix < slicelength; ++ix) {
                    Py_DECREF(self.entries[ix].as_ob);
                }
            }
            self.entries.erase(self.entries.begin() + other->size(),
                               self.entries.begin() + slicelength);
        }
        else if (slicelength > self.size() || other->size() > slicelength) {
            Py_ssize_t count = std::max(slicelength - self.size(),
                                        other->size() - slicelength);
            entry e;
            if (self.tag == entry_tag::as_ob) {
                e.as_ob = Py_None;
                for (Py_ssize_t ix = 0; ix < count; ++ix) {
                    Py_INCREF(Py_None);
                }
            }
            self.entries.insert(self.entries.begin() + start, count, e);
        }
    }
    else if (slicelength != other->size()) {
        PyErr_Format(PyExc_ValueError,
                     "attempt to assign sequence of "
                     "size %zd to extended slice of "
                     "size %zd",
                     other->size(),
                     slicelength);
        return -1;
    }

    if (other->size() == 0) {
        return 0;
    }

    switch(self.tag) {
    case entry_tag::as_ob:
        set_loop_ob(self, start, step, *other);
        break;
    case entry_tag::as_int:
    case entry_tag::as_double:
        set_loop_prim(self, start, step, *other);
        break;
    default:
        __builtin_unreachable();
    }

    Py_DECREF(other);
    return 0;
}

int set_slice(jlist& self,
              Py_ssize_t start,
              Py_ssize_t step,
              Py_ssize_t slicelength,
              PyObject* other) {
    jlist* rhs = new_jlist(entry_tag::unset);
    if (!rhs) {
        return -1;
    }

    if (extend_helper(*rhs, other)) {
        return -1;
    }

    int out = set_slice(self, start, step, slicelength, rhs);
    Py_DECREF(rhs);
    return out;
}
}  // namespace detail

int set_subscript(PyObject* _self, PyObject* item, PyObject* value) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    if (PyIndex_Check(item)) {
        Py_ssize_t ix = PyNumber_AsSsize_t(item, PyExc_IndexError);
        if (ix == -1 && PyErr_Occurred()) {
            return -1;
        }
        ix = jl::detail::adjust_ix(ix, self.size(), false);
        return setitem(_self, ix, value);
    }
    else if (!PySlice_Check(item)) {
        PyErr_Format(PyExc_TypeError,
                     "jlist indices must be integers or slices, not %.200s",
                     item->ob_type->tp_name);
        return -1;
    }

    Py_ssize_t start;
    Py_ssize_t stop;
    Py_ssize_t step;
    if (PySlice_Unpack(item, &start, &stop, &step) < 0) {
        return -1;
    }

    Py_ssize_t slicelength = PySlice_AdjustIndices(self.size(), &start, &stop, step);
    // Make sure s[5:2] = [..] inserts at the right place: before 5, not before 2.
    if ((step < 0 && start < stop) || (step > 0 && start > stop)) {
        stop = start;
    }

    if (!value) {
        return detail::delete_slice(self, start, stop, step, slicelength);
    }
    else if (Py_TYPE(value) == &jlist_type) {
        return detail::set_slice(self,
                                 start,
                                 step,
                                 slicelength,
                                 reinterpret_cast<jlist*>(value));
    }

    return detail::set_slice(self, start, step, slicelength, value);

}

PyMappingMethods as_mapping = {
    length,         // mp_length
    subscript,      // mp_subscript
    set_subscript,  // mp_ass_subscript
};

PyDoc_STRVAR(tag_doc, "The type tag for the sequence.");

PyMemberDef members[] = {
    {const_cast<char*>("tag"), T_BYTE, offsetof(jlist, tag), READONLY, tag_doc},
    {nullptr, 0, 0, 0, nullptr},
};

int traverse(PyObject* _self, visitproc visit, void* arg) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    if (self.tag == entry_tag::as_ob) {
        for (entry e : self.entries) {
            Py_VISIT(e.as_ob);
        }
    }

    return 0;
}

int gc_clear(PyObject* _self) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    detail::clear_helper(self);
    return 0;
}
}  // namespace detail

namespace iterobject {
struct jlist_iter {
    PyObject base;
    Py_ssize_t ix;
    jlist* list;
};

void deallocate(PyObject* _self) {
    jlist_iter& self = *reinterpret_cast<jlist_iter*>(_self);

    PyObject_GC_UnTrack(_self);
    Py_XDECREF(self.list);
    PyObject_GC_Del(_self);
}

PyObject* next(PyObject* _self) {
    jlist_iter& self = *reinterpret_cast<jlist_iter*>(_self);

    if (!self.list) {
        return nullptr;
    }

    if (self.ix >= self.list->size()) {
        Py_CLEAR(self.list);
        return nullptr;
    }

    return methods::getitem(reinterpret_cast<PyObject*>(self.list), self.ix++);
}

PyObject* length(PyObject* _self, PyObject*) {
    jlist_iter& self = *reinterpret_cast<jlist_iter*>(_self);

    if (!self.list) {
        return PyLong_FromSsize_t(0);
    }

    return PyLong_FromSsize_t(self.list->size() - self.ix);
}

PyMethodDef length_method = {"__length_hint__", length, METH_NOARGS, nullptr};

PyObject* reduce(PyObject* _self, PyObject*) {
    jlist_iter& self = *reinterpret_cast<jlist_iter*>(_self);

    PyObject* builtins = PyImport_ImportModule("builtins");
    if (!builtins) {
        return nullptr;
    }
    PyObject* iter = PyObject_GetAttrString(builtins, "iter");
    Py_DECREF(builtins);
    if (!iter) {
        return nullptr;
    }

    PyObject* list = reinterpret_cast<PyObject*>(self.list);
    if (!list) {
        if (!(list = PyList_New(0))) {
            return nullptr;
        }
    }
    else {
        Py_INCREF(list);
    }
    PyObject* out = Py_BuildValue("(O(O)n)", iter, list, self.ix);
    Py_DECREF(list);
    Py_DECREF(iter);
    return out;
}

PyMethodDef reduce_method = {"__reduce__", reduce, METH_NOARGS, nullptr};

PyObject* setstate(PyObject* _self, PyObject* _ix) {
    jlist_iter& self = *reinterpret_cast<jlist_iter*>(_self);

    Py_ssize_t ix = PyNumber_AsSsize_t(_ix, PyExc_TypeError);
    if (ix == -1 && PyErr_Occurred()) {
        return nullptr;
    }

    self.ix = ix;
    Py_RETURN_NONE;
}

PyMethodDef setstate_method = {"__setstate__", setstate, METH_O, nullptr};

PyMethodDef methods[] = {
    length_method,
    reduce_method,
    setstate_method,
    {nullptr, nullptr, 0, nullptr},
};

int traverse(PyObject* _self, visitproc visit, void* arg) {
    jlist_iter& self = *reinterpret_cast<jlist_iter*>(_self);
    if (self.list) {
        Py_VISIT(self.list);
    }
    return 0;
}

PyTypeObject type = {
    // clang-format: off
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    // clang-format: on
    "jlist.jlist_iterator",                   // tp_name
    sizeof(jlist),                            // tp_basicsize
    0,                                        // tp_itemsize
    deallocate,                               // tp_dealloc
    0,                                        // tp_print
    0,                                        // tp_getattr
    0,                                        // tp_setattr
    0,                                        // tp_reserved
    0,                                        // tp_repr
    0,                                        // tp_as_number
    0,                                        // tp_as_sequence
    0,                                        // tp_as_mapping
    0,                                        // tp_hash
    0,                                        // tp_call
    0,                                        // tp_str
    0,                                        // tp_getattro
    0,                                        // tp_setattro
    0,                                        // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,  // tp_flags
    0,                                        // tp_doc
    traverse,                                 // tp_traverse
    0,                                        // tp_clear
    0,                                        // tp_richcompare
    0,                                        // tp_weaklistoffset
    PyObject_SelfIter,                        // tp_iter
    next,                                     // tp_iternext
    methods,                                  // tp_methods,
};
}  // namespace iterobject

namespace methods {
PyObject* iter(PyObject* _self) {
    jlist& self = *reinterpret_cast<jlist*>(_self);

    iterobject::jlist_iter* out = PyObject_GC_New(iterobject::jlist_iter,
                                                  &iterobject::type);
    if (!out) {
        return nullptr;
    }

    Py_INCREF(_self);
    out->list = &self;
    out->ix = 0;

    PyObject_GC_Track(out);
    return reinterpret_cast<PyObject*>(out);
}
}  // namespace methods

PyTypeObject jlist_type = {
    // clang-format: off
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    // clang-format: on
    "jlist.jlist",                            // tp_name
    sizeof(jlist),                            // tp_basicsize
    0,                                        // tp_itemsize
    methods::deallocate,                      // tp_dealloc
    0,                                        // tp_print
    0,                                        // tp_getattr
    0,                                        // tp_setattr
    0,                                        // tp_reserved
    methods::repr,                            // tp_repr
    0,                                        // tp_as_number
    &methods::sq_methods,                     // tp_as_sequence
    &methods::as_mapping,                     // tp_as_mapping
    0,                                        // tp_hash
    0,                                        // tp_call
    0,                                        // tp_str
    0,                                        // tp_getattro
    0,                                        // tp_setattro
    0,                                        // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,  // tp_flags
    0,                                        // tp_doc
    methods::traverse,                        // tp_traverse
    methods::gc_clear,                        // tp_clear
    methods::richcompare,                     // tp_richcompare
    0,                                        // tp_weaklistoffset
    methods::iter,                            // tp_iter
    0,                                        // tp_iternext
    methods::methods,                         // tp_methods,
    methods::members,                         // tp_members
    0,                                        // tp_getset
    0,                                        // tp_base
    0,                                        // tp_dict
    0,                                        // tp_descr_get
    0,                                        // tp_descr_set
    0,                                        // tp_dictoffset
    methods::init,                            // tp_init
    0,                                        // tp_alloc
    methods::new_,                            // tp_new
};

PyModuleDef module = {
    PyModuleDef_HEAD_INIT,
    "jlist.jlist",
    nullptr,
    -1,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

PyMODINIT_FUNC PyInit_jlist() {
    if (PyType_Ready(&jlist_type) < 0) {
        return nullptr;
    }

    if (PyType_Ready(&iterobject::type) < 0) {
        return nullptr;
    }

    PyObject* m = PyModule_Create(&module);
    if (!m) {
        return nullptr;
    }
    if (PyObject_SetAttrString(m, "jlist", reinterpret_cast<PyObject*>(&jlist_type))) {
        Py_DECREF(m);
        return nullptr;
    }

    return m;
}
}  // namespace jl
