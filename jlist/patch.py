import jlist as jl


def patch_builtins(*, include_type=False):
    """Replace ``builtins.all`` with ``jlist.all`` and ``builtins.any`` with
    ``jlist.any``.

    Parameters
    ----------
    include_type : bool, optional
        Also replace ``builtins.list`` with ``jlist.jlist``.

    Examples
    --------
    >>> list
    list

    >>> from jlist.patch import patch_builtins
    >>> patch_builtins()

    >>> list
    <class 'jlist.jlist'>
    """
    import builtins

    if include_type:
        builtins.list = jl.jlist

    builtins.all = jl.all
    builtins.any = jl.any


try:
    import codetransformer
except ImportError as e:
    HAVE_CODETRANSFORMER = False

    def _f(name, e=e):
        def f():
            raise ImportError(f'{name} requires codetransformer') from e

        return f

    overloaded_literals = _f('overloaded_literals')
    patch_literals = _f('patch_literals')
    patch_all = _f('patch_all')

    del _f
else:
    import ast
    from functools import wraps
    import operator
    import sys

    from codetransformer import pattern, instructions as instrs

    HAVE_CODETRANSFORMER = True

    class JListLiterals(codetransformer.CodeTransformer):
        IN_COMPREHENSION = 'in_comprehension'

        @pattern(
            instrs.BUILD_LIST,
            codetransformer.matchany[codetransformer.var],
            instrs.LIST_APPEND
        )
        def _list_append(self, *instrs):
            self.begin(self.IN_COMPREHENSION)
            yield from self.patterndispatcher(instrs)

        @pattern(
            instrs.BUILD_LIST,
            instrs.LOAD_FAST,
            startcodes=(IN_COMPREHENSION,),
        )
        def _build_list_in_comprehension(self, build_instr, load_instr):
            yield instrs.LOAD_CONST(jl.jlist).steal(build_instr)
            yield instrs.CALL_FUNCTION(0)
            yield instrs.DUP_TOP()
            yield instrs.DUP_TOP()
            # TOS  = <jlist>
            # TOS1 = <jlist>
            # TOS2 = <jlist>

            yield instrs.LOAD_ATTR('append')
            yield instrs.STORE_FAST('.append')
            # TOS  = <jlist>
            # TOS1 = <jlist>

            yield load_instr
            # TOS  = .0
            # TOS1 = <jlist>
            # TOS2 = <jlist>

            yield instrs.DUP_TOP()
            # TOS  = .0
            # TOS1 = .0
            # TOS2 = <jlist>
            # TOS3 = <jlist>

            yield instrs.ROT_THREE()
            # TOS  =  .0
            # TOS1 = <jlist>
            # TOS2 = .0
            # TOS3 = <jlist>

            yield instrs.LOAD_CONST(operator.length_hint)
            yield instrs.ROT_TWO()
            yield instrs.CALL_FUNCTION(1)
            # TOS  = <length_hint>
            # TOS1 = <jlist>
            # TOS2 = .0
            # TOS3 = <jlist>

            yield instrs.ROT_TWO()
            # TOS  = <jlist>
            # TOS1 = <length_hint>
            # TOS2 = .0
            # TOS3 = <jlist>

            if sys.version_info >= (3, 7):
                yield instrs.LOAD_METHOD('_reserve')
                # TOS  = <jlist._reserve>
                # TOS1 = <length_hint>
                # TOS2 = .0
                # TOS3 = <jlist>
                yield instrs.ROT_TWO()
                # TOS  = <length_hint>
                # TOS1 = <jlist._reserve>
                # TOS2 = .0
                # TOS3 = <jlist>
                yield instrs.CALL_METHOD(1)
                # TOS  = None
                # TOS1 = .0
                # TOS3 = <jlist>
            else:
                yield instrs.LOAD_ATTR('_reserve')
                # TOS  = <jlist._reserve>
                # TOS1 = <length_hint>
                # TOS2 = .0
                # TOS3 = <jlist>
                yield instrs.ROT_TWO()
                # TOS  = <length_hint>
                # TOS1 = <jlist._reserve>
                # TOS2 = .0
                # TOS3 = <jlist>
                yield instrs.CALL_FUNCTION(1)
                # TOS  = None
                # TOS1 = .0
                # TOS3 = <jlist>

            yield instrs.POP_TOP()
            # TOS  = .0
            # TOS1 = <jlist>

        @pattern(instrs.LIST_APPEND, startcodes=(IN_COMPREHENSION,))
        def _list_append_in_comprehension(self, instr):
            yield instrs.LOAD_FAST('.append').steal(instr)
            yield instrs.ROT_TWO()
            yield instrs.CALL_FUNCTION(1)
            yield instrs.POP_TOP()

        @pattern(instrs.BUILD_LIST)
        def _build_list(self, instr):
            if instr.arg == 0:
                yield instrs.LOAD_CONST(jl.jlist).steal(instr)
                yield instrs.CALL_FUNCTION(0)
            elif instr.arg == 1:
                yield instrs.LOAD_CONST(jl.jlist._from_starargs).steal(instr)
                yield instrs.ROT_TWO()
                yield instrs.CALL_FUNCTION(1)
            elif instr.arg == 2:
                yield instrs.LOAD_CONST(jl.jlist._from_starargs).steal(instr)
                yield instrs.ROT_THREE()
                yield instrs.CALL_FUNCTION(2)
            else:
                yield instr
                yield instrs.LOAD_CONST(jl.jlist)
                yield instrs.ROT_TWO()
                yield instrs.CALL_FUNCTION(1)

    overloaded_literals = JListLiterals()

    _original_compile = compile

    @wraps(_original_compile)
    def _patched_compile(source,
                         filename,
                         mode,
                         flags=0,
                         dont_inherit=False,
                         optimize=-1):
        if flags & ast.PyCF_ONLY_AST:
            return _original_compile(
                source,
                filename,
                mode,
                flags,
                dont_inherit,
                optimize,
            )
        code = codetransformer.Code.from_pycode(_original_compile(
            source,
            filename,
            mode,
            flags,
            dont_inherit,
            optimize,
        ))
        return overloaded_literals.transform(code).to_pycode()

    def patch_literals():
        """Convert list literals into ``jlist.jlist`` objects.

        Examples
        --------
        >>> [1, 2, 3]
        [1, 2, 3]
        >>> [x for x in range(3)]
        [1, 2, 3]

        >>> from jlist.patch import patch_literals
        >>> patch_literals()

        >>> [1, 2, 3]
        >>> jlist([1, 2, 3])
        >>> [x for x in range(3)]
        >>> jlist([1, 2, 3])
        """
        import builtins

        builtins.compile = _patched_compile

    def patch_all():
        """Patch ``builtins.list`` and list literal objects.

        See Also
        --------
        jlist.patch.patch_builtins
        jlist.patch.patch_literals
        """
        patch_builtins(include_type=True)
        patch_literals()
