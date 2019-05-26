jlist
=====

Your friend for homogeneous lists!

``jlist`` provides a new-drop in replacement for Python's ``list`` that provides
optimizations for the case where the list holds exclusively ``int`` or
``float``. While ``list`` *can* hold values of any type, almost all uses of
lists are homogeneous sequences. ``jlist`` takes advantage of this by storing
the data unboxed (as C data types) instead of Python objects when possible. To
comply with the ``list`` interface, attempting to add a value of a different
type to the list will cause it to box up all the existing members before adding
the new one.

Unboxed Types
-------------

``jlist`` can store in an unboxed form the ``int64`` and ``double``. A
``PyObject*``, ``int64`` and ``double`` all occupy the same space so we can use
the same array to store any of these values. The layout of a ``jlist`` is as
follows:

.. code-block:: C++

   enum class entry_tag : std::int8_t {
       as_ob = 0,
       as_int = 1,
       as_double = 2,
       unset = 3,
   };

   union entry {
       PyObject* as_ob;
       std::int64_t as_int;
       double as_double;
   };

   struct jlist {
       PyObject base;
       entry_tag tag;
       std::vector<entry> entries;
   };

The ``PyObject base`` member marks that we are a Python object. The ``tag``
member holds a value that indicates what sort of data we are storing in the
entries. ``entries`` is a vector that holds either ``int64``, ``double`` or
``PyObject*`` values depending on ``tag``.

``entry_tag::unset`` is a special tag that says that the ``jlist`` should infer
the type from the first value added to it. This is the tag for a newly
constructed ``jlist``. For ``tag`` to be ``entry_tag::unset``, the ``jlist``
must be empty; however, if you ``clear()`` an existing ``jlist``, you can get an
empty ``jlist`` with a different tag. ``clear()`` doesn't reset the tag because
if you are explicitly clearing, you may still just be storing a single type in
the list and we want future inserts to be fast.

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

   In [2]: %timeit jl.range(10000000)
   40.3 ms ± 183 µs per loop (mean ± std. dev. of 7 runs, 10 loops each)

   In [3]: %timeit jl.jlist(range(10000000))
   981 ms ± 7.52 ms per loop (mean ± std. dev. of 7 runs, 1 loop each)

   In [4]: %timeit list(range(10000000))
   325 ms ± 2.34 ms per loop (mean ± std. dev. of 7 runs, 1 loop each)

Operations
----------

``jlist`` also provides some optimized operations that can take advantage of the
potentially unboxed values.

.. code-block:: Python

   import jlist as jl

   jl.sum
   jl.any
   jl.all


Note: ``jl.sum`` for integers guards against overflow and will switch to summing
using Python ``int`` objects which have arbitrary precision.
