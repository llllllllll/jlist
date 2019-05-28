#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include <Python.h>

namespace jl {
enum class entry_tag : std::int8_t {
    as_homogeneous_ob = 0,
    as_heterogeneous_ob = 1,
    as_int = 2,
    as_double = 3,
    unset = 4,
};

inline bool is_object_tag(entry_tag tag) {
    return tag == entry_tag::as_homogeneous_ob || tag == entry_tag::as_heterogeneous_ob;
}

union entry {
    PyObject* as_ob;
    std::int64_t as_int;
    double as_double;
};

template<typename T>
constexpr bool is_entry_type = std::is_same_v<T, PyObject*> ||
                               std::is_same_v<T, std::int64_t> ||
                               std::is_same_v<T, double>;

template<typename T>
void static_assert_is_entry_type() {
    static_assert(is_entry_type<T>, "T must be PyObject*, std::int64_t, or double");
}

namespace detail {
template<typename T>
struct entry_pytype;

template<>
struct entry_pytype<std::int64_t> {
    constexpr static PyTypeObject* value = &PyLong_Type;
};

template<>
struct entry_pytype<double> {
    constexpr static PyTypeObject* value = &PyFloat_Type;
};
}  // namespace detail

template<typename T>
constexpr PyTypeObject* entry_pytype = detail::entry_pytype<T>::value;

template<typename T>
T& entry_value(entry& e) {
    static_assert_is_entry_type<T>();

    if constexpr(std::is_same_v<T, PyObject*>) {
        return e.as_ob;
    }
    else if constexpr (std::is_same_v<T, std::int64_t>) {
        return e.as_int;
    }
    else if constexpr (std::is_same_v<T, double>) {
        return e.as_double;
    }
}

template<typename T>
const T& entry_value(const entry& e) {
    static_assert_is_entry_type<T>();

    if constexpr(std::is_same_v<T, PyObject*>) {
        return e.as_ob;
    }
    else if constexpr (std::is_same_v<T, std::int64_t>) {
        return e.as_int;
    }
    else if constexpr (std::is_same_v<T, double>) {
        return e.as_double;
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

namespace detail {
class tagged_type_pointer {
private:
    static_assert(alignof(PyTypeObject) == 8,
                  "PyTypeObject isn't 8 byte aligned, so we can't store the tag in the "
                  "low order 3 bits");

    constexpr static std::intptr_t tag_mask = 7;
    std::intptr_t m_value;

public:
    tagged_type_pointer(entry_tag tag, PyTypeObject* ptr)
        : m_value(static_cast<std::uint8_t>(tag) & reinterpret_cast<std::intptr_t>(ptr)) {
    }

    inline entry_tag tag() const {
        return static_cast<entry_tag>(m_value & tag_mask);
    }

    inline void tag(entry_tag tag) {
        m_value &= ~tag_mask;
        m_value |= static_cast<std::uint8_t>(tag);
    }

    inline PyTypeObject* ptr() const {
        return reinterpret_cast<PyTypeObject*>(m_value);
    }

    inline void ptr(PyTypeObject* ptr) {
        m_value &= tag_mask;
        m_value |= reinterpret_cast<std::intptr_t>(ptr);
    }
};
}  // namespace detail

struct jlist {
    PyObject base;
    detail::tagged_type_pointer tagged_ptr;
    std::vector<entry> entries;

    entry_tag tag() const {
        return tagged_ptr.tag();
    }

    void tag(entry_tag tag) {
        tagged_ptr.tag(tag);
    }

    bool boxed() const {
        return is_object_tag(tag());
    }

    PyTypeObject* homogeneous_type_ptr() const {
        return tagged_ptr.ptr();
    }

    void homogeneous_type_ptr(PyTypeObject* ptr) {
        tagged_ptr.tag(entry_tag::as_homogeneous_ob);
        tagged_ptr.ptr(ptr);
    }

    Py_ssize_t size() const {
        return static_cast<Py_ssize_t>(entries.size());
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
