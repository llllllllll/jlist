#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <Python.h>

namespace jl {
enum class entry_tag : std::int8_t {
    as_ob = 0,
    as_int = 1,
    as_double = 2,
    unset = 3,
};

namespace detail {
template<typename T>
struct entry_tag_for_type;

template<>
struct entry_tag_for_type<PyObject*> {
    static constexpr entry_tag value = entry_tag::as_ob;
};

template<>
struct entry_tag_for_type<std::int64_t> {
    static constexpr entry_tag value = entry_tag::as_int;
};

template<>
struct entry_tag_for_type<double> {
    static constexpr entry_tag value = entry_tag::as_double;
};
}  // namespace detail

template<typename T>
constexpr entry_tag entry_tag_for_type = detail::entry_tag_for_type<T>::value;

union entry {
    PyObject* as_ob;
    std::int64_t as_int;
    double as_double;
};

template<typename T>
T& entry_value(entry& e) {
    if constexpr(std::is_same_v<T, PyObject*>) {
        return e.as_ob;
    }
    else if constexpr (std::is_same_v<T, std::int64_t>) {
        return e.as_int;
    }
    else if constexpr (std::is_same_v<T, double>) {
        return e.as_double;
    }
    else {
        __builtin_unreachable();
    }
}

template<typename T>
const T& entry_value(const entry& e) {
    if constexpr(std::is_same_v<T, PyObject*>) {
        return e.as_ob;
    }
    else if constexpr (std::is_same_v<T, std::int64_t>) {
        return e.as_int;
    }
    else if constexpr (std::is_same_v<T, double>) {
        return e.as_double;
    }
    else {
        __builtin_unreachable();
    }
}

inline PyObject* box_value(PyObject* ob) {
    Py_INCREF(ob);
    return ob;
}

inline PyObject* box_value(std::int64_t unboxed) {
    return PyLong_FromLongLong(unboxed);
}

inline PyObject* box_value(double unboxed) {
    return PyFloat_FromDouble(unboxed);
}

template<typename T>
T unbox_value(PyObject* boxed);

template<>
std::int64_t unbox_value(PyObject* boxed) {
    return PyLong_AsLongLong(boxed);
}

template<>
double unbox_value(PyObject* boxed) {
    return PyFloat_AsDouble(boxed);
}

template<typename T>
std::optional<T> maybe_unbox(PyObject*);

template<>
std::optional<std::int64_t> maybe_unbox(PyObject* ob) {
    if (!PyLong_CheckExact(ob)) {
        return std::nullopt;
    }

    int overflow = 0;
    std::int64_t value = PyLong_AsLongLongAndOverflow(ob, &overflow);
    if (overflow) {
        return std::nullopt;
    }
    return value;
}

template<>
std::optional<double> maybe_unbox(PyObject* ob) {
    if (!PyFloat_CheckExact(ob)) {
        return std::nullopt;
    }

    return PyFloat_AsDouble(ob);
}

struct jlist {
    PyObject base;
    entry_tag tag;
    std::vector<entry> entries;

    Py_ssize_t size() const {
        return static_cast<Py_ssize_t>(entries.size());
    }

    void set_tag(entry_tag new_tag) {
        if (tag != entry_tag::as_ob && new_tag == entry_tag::as_ob) {
            PyObject_GC_Track(this);
        }
        else if (tag == entry_tag::as_ob && new_tag != entry_tag::as_ob) {
            PyObject_GC_UnTrack(this);
        }
        tag = new_tag;
    }
};

template<typename F>
PyCFunction unsafe_cast_to_pycfunction(F&& f) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
    return reinterpret_cast<PyCFunction>(f);
#pragma GCC diagnostic pop
}
}  // namespace jl
