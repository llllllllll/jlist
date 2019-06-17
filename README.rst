jlist
=====

Your friend for homogeneous lists!

``jlist`` provides a new-drop in replacement for Python's ``list`` that provides
optimizations for the case where the list holds exclusively ``int`` or
``float``, or values of a single Python type (not including subclasses). While
``list`` *can* hold values of any type, almost all uses of lists are homogeneous
sequences. ``jlist`` takes advantage of this by storing the data unboxed (as C
data types) instead of Python objects when possible. Also, by remembering the
single homogeneous type, many operations can be sped up by reducing the amount
of dispatching required. To comply with the ``list`` interface, attempting to
add a value of a different type to the list will cause it to box up all the
existing members  before adding the new one.

``jlist`` can be made to replace Python's builtin ``list`` object, either in
particular function or globally. See patching_ for more information.

Unboxed Types
-------------

``jlist`` can store in an unboxed form the ``int64`` and ``double``. A
``PyObject*``, ``int64`` and ``double`` all occupy the same space so we can use
the same array to store any of these values.

Homogeneous ``PyObject*``
-------------------------

If we know that all of the objects are of the exact same Python type, then we
can do some optimizations by reducing the amount of virtual function table
lookups. For example, consider the case of ``all`` or ``any``. ``bool(x)`` is
defined in the C API as ``PyObject_IsTrue``, which looks like:

.. code-block:: C

   int
   PyObject_IsTrue(PyObject *v)
   {
       Py_ssize_t res;
       if (v == Py_True)
           return 1;
       if (v == Py_False)
           return 0;
       if (v == Py_None)
           return 0;
       else if (v->ob_type->tp_as_number != NULL &&
                v->ob_type->tp_as_number->nb_bool != NULL)
           res = (*v->ob_type->tp_as_number->nb_bool)(v);
       else if (v->ob_type->tp_as_mapping != NULL &&
                v->ob_type->tp_as_mapping->mp_length != NULL)
           res = (*v->ob_type->tp_as_mapping->mp_length)(v);
       else if (v->ob_type->tp_as_sequence != NULL &&
                v->ob_type->tp_as_sequence->sq_length != NULL)
           res = (*v->ob_type->tp_as_sequence->sq_length)(v);
       else
           return 1;
       /* if it is negative, it should be either -1 or -2 */
       return (res > 0) ? 1 : Py_SAFE_DOWNCAST(res, Py_ssize_t, int);
   }

There is a lot of checking to see how truthiness is defined for the given
type. If we know that the list holds only a single type, we can look up the
proper method for determining truthiness just once, which can be a dramatic
performance improvement.

Construction
------------

A ``jlist`` can be constructed from any Python iterable, just like ``list``.

.. code-block:: Python

   >>> import jlist as jl
   >>> jlist([])
   >>> jl.jlist([1, 2, 3])
   jlist([1, 2, 3])
   >>> jl.jlist(range(6))
   jlist([0, 1, 2, 3, 4, 5])

``jlist`` also has an optimized version of ``jl.jlist(range(...))`` which
doesn't round trip through the Python iterator protocol:

.. code-block:: Python

   In [1]: import jlist as jl

   In [2]: %timeit list(range(10000000))
   337 ms ± 6.57 ms per loop (mean ± std. dev. of 7 runs, 1 loop each)

   In [3]: %timeit jl.jlist(range(10000000))
   40.7 ms ± 146 µs per loop (mean ± std. dev. of 7 runs, 10 loops each)

   In [4]: %timeit jl.range(10000000)
   40.3 ms ± 183 µs per loop (mean ± std. dev. of 7 runs, 10 loops each)


``jl.range`` is purely a convenience when creating eager ranges.

There is also a helper for creating a list of all zero, which exists only as a
convenience over ``jl.jlist([0]) * n``:

.. code-block:: Python

   In [1]: import jlist as jl

   In [2]: %timeit jl.zeros(10000000)
   35 ms ± 216 µs per loop (mean ± std. dev. of 7 runs, 10 loops each)

   In [3]: %timeit jl.jlist([0]) * 10000000
   33.4 ms ± 202 µs per loop (mean ± std. dev. of 7 runs, 10 loops each)

   In [4]: %timeit [0] * 10000000
   51.5 ms ± 487 µs per loop (mean ± std. dev. of 7 runs, 10 loops each)


Operations
----------

``jlist`` also provides optimized operations that can take advantage of the
potentially unboxed values.

``jlist`` specific
~~~~~~~~~~~~~~~~~~

``jlist`` aims to be a replacement for ``list``; however, there are a few things
that are not exactly the same.

``tag``
```````

``jlist`` objects have an extra ``tag`` attribute which can be used to check
what state it is in.

.. code-block:: Python

   In [1]: import jlist as jl

   In [2]: jl.jlist().tag
   Out[2]: 'unset'

   In [3]: jl.jlist([0]).tag
   Out[3]: 'int'

   In [4]: jl.jlist([0.5]).tag
   Out[4]: 'double'

   In [5]: jl.jlist(['a']).tag
   Out[5]: 'homogeneous_ob'

   In [6]: jl.jlist(['a', None]).tag
   Out[6]: 'heterogeneous_ob'



Identity
````````

Because ``jlist`` stores ``int`` and ``float`` unboxed, object identity is not
preserved for these objects. This means that if you put an ``int`` in a
``jlist``, the value you get back may be a different Python object with the same
value. Given that ``int`` and ``float`` are immutable, this should likely not
matter. The CPython test suite doesn't even test this property for ``list``.

.. code-block:: Python

   In [1]: import jlist as jl

   In [2]: jlist = jl.jlist()

   In [3]: value = 9001

   In [4]: jlist.append(value)

   In [5]: jlist[0] is value
   Out[5]: False

   In [6]: jlist[0] == value
   Out[6]: True


List Methods
~~~~~~~~~~~~

Slicing
```````
.. code-block:: Python

   In [1]: import jlist as jl

   In [2]: regular = list(jlist)

   In [3]: %timeit regular[:100000 // 2]
   145 µs ± 1.28 µs per loop (mean ± std. dev. of 7 runs, 10000 loops each)

   In [4]: jlist = jl.jlist(regular)

   In [5]: %timeit jlist[:100000 // 2]
   14.3 µs ± 28.9 ns per loop (mean ± std. dev. of 7 runs, 100000 loops each)

   In [6]: %timeit regular[::2]
   310 µs ± 4.07 µs per loop (mean ± std. dev. of 7 runs, 1000 loops each)

   In [7]: %timeit jlist[::2]
   202 µs ± 1.23 µs per loop (mean ± std. dev. of 7 runs, 1000 loops each)

Containment
```````````
.. code-block:: Python

   In [1]: import jlist as jl

   In [2]: regular = list(range(100000))

   In [3]: -1 in regular
   Out[3]: False

   In [4]: %timeit -- -1 in regular
   926 µs ± 10.4 µs per loop (mean ± std. dev. of 7 runs, 1000 loops each)

   In [5]: jlist = jl.jlist(regular)

   In [6]: %timeit -- -1 in jlist
   34 µs ± 201 ns per loop (mean ± std. dev. of 7 runs, 10000 loops each)

   In [7]: %timeit regular.index(100000 // 2)
   540 µs ± 2.96 µs per loop (mean ± std. dev. of 7 runs, 1000 loops each)

   In [8]: %timeit jlist.index(100000 // 2)
   17.8 µs ± 775 ns per loop (mean ± std. dev. of 7 runs, 100000 loops each)

   In [9]: import string; random

   In [10]: regular_list = [
       ...:     ''.join(map(
       ...:         chr, (
       ...:         random.randint(ord('a'), ord('z'))
       ...:         for _ in range(random.randint(3, 10)))
       ...:     ))
       ...:     for _ in range(100000)
       ...: ]

   In [11]: search = 'a' * 10  # not in the sequence

   In [12]: %timeit search in regular_list
   1.3 ms ± 10.9 µs per loop (mean ± std. dev. of 7 runs, 1000 loops each)

   In [13]: jlist = jl.jlist(regular_list)

   In [14]: %timeit search in jlist
   905 µs ± 16.1 µs per loop (mean ± std. dev. of 7 runs, 1000 loops each)


Copy
````

.. code-block:: Python

   In [1]: import jlist as jl

   In [2]: regular = list(range(100000))

   In [3]: %timeit regular.copy()
   448 µs ± 60.5 µs per loop (mean ± std. dev. of 7 runs, 1000 loops each)

   In [4]: jlist = jl.jlist(regular)

   In [5]: %timeit jlist.copy()
   29.9 µs ± 371 ns per loop (mean ± std. dev. of 7 runs, 10000 loops each)


Sorting
```````
Note: we copy before sorting because ``sort()`` is in-place (just like list).

.. code-block:: Python

   In [1]: import jlist as jl; import random

   In [2]: regular = [random.random() for _ in range(100000)]

   In [3]: %timeit regular.copy().sort()
   15.8 ms ± 236 µs per loop (mean ± std. dev. of 7 runs, 100 loops each)

   In [4]: jlist = jl.jlist(regular)

   In [5]: %timeit jlist.copy().sort()
   6.88 ms ± 27 µs per loop (mean ± std. dev. of 7 runs, 100 loops each)


Built-in Free Functions
~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: Python

   In [1]: import jlist as jl

   In [2]: regular_list = list(range(10000000))

   In [3]: %timeit sum(regular_list)
   56.5 ms ± 351 µs per loop (mean ± std. dev. of 7 runs, 10 loops each)

   In [4]: jlist = jl.jlist(regular_list)

   In [5]: %timeit jl.sum(jlist)
   6.43 ms ± 242 µs per loop (mean ± std. dev. of 7 runs, 100 loops each)

   In [6]: regular_list = [0 for _ in range(10000000)]

   In [7]: %timeit any(regular_list)
   45.2 ms ± 231 µs per loop (mean ± std. dev. of 7 runs, 10 loops each)

   In [8]: jlist = jl.jlist(regular_list)

   In [9]: %timeit jl.any(jlist)
   6.31 ms ± 42.7 µs per loop (mean ± std. dev. of 7 runs, 100 loops each)

   In [10]: regular_list = [1 for _ in range(10000000)]

   In [11]: %timeit all(regular_list)
   40.5 ms ± 304 µs per loop (mean ± std. dev. of 7 runs, 10 loops each)

   In [12]: jlist = jl.jlist(regular_list)

   In [13]: %timeit jl.all(jlist)
   6.26 ms ± 28.7 µs per loop (mean ± std. dev. of 7 runs, 100 loops each)

Note: ``jl.sum`` for integers guards against overflow and will switch to summing
using Python ``int`` objects which have arbitrary precision.

.. _patching:

Patching
--------

``jlist`` can be made to replace Python's builtin ``list`` object, either in
particular function or globally. This behavior depends on the `codetransformer
<https://github.com/llllllllll/codetransformer>`_ module, and is not installed
nor enabled by default.

To make ``jlist`` replace ``list`` literals in a particular function,
``jlist.overloaded_literals`` may be used as a function decorator:

.. code-block:: Python

   import jlist as jl


   @jl.overloaded_literals
   def f():
       return [1, 2, 3]

   print(f())  # jlist([1, 2, 3])


Overloaded literals also supports list comprehensions:

.. code-block:: Python

   import jlist as jl

   @jl.overloaded_literals
   def f():
       return [x * 2 for x in range(5)]

   print(f())  # jlist([0, 2, 4, 6, 8])


To replace ``list`` literals with ``jlist`` literals in the entire process, you
may use: ``jlist.patch_literals``.

.. warning::

   This might have strange side-effects. While we would like to be a total drop
   in replacement, some code may actually require a ``builtins.list``
   object. This is especially true for code that calls into C extension modules,
   including Cython.

``jlist.patch_literals`` does not change ``builtins.list`` to be
``jlist.jlist``. This allows you to still check against a real list. If you
would like to replace ``builtins.list`` with ``jlist.jlist``, which will make the
name ``list`` resolve to ``jlist.jlist``, you may use ``jlist.patch_builtins``.
``jlist.patch_builtins`` will also replace the builtin free functions like
``any`` and ``all`` with their ``jlist`` equivalents. The ``jlist`` versions
fall back to the builtins if the input is not a ``jlist.jlist``.

``jlist.patch_all`` is a helper that calls both ``jlist.patch_literals`` and
``jlist.patch_builtins``.
