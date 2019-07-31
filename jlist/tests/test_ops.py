import math
import random
from unittest import TestCase

import jlist as jl


class SumTestCase(TestCase):
    RANDOM_SEED = int.from_bytes(b'ayy lmao', 'little')

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.random = random.Random(cls.RANDOM_SEED)

    def test_int_overflow_box_first(self):
        # a list which contains a max int, we will sum with an initial value
        # of 1, which should switch over to boxing on the first element
        list_ints = [2 ** 63 - 1, 1]

        builtin_sum_list_ints = sum(list_ints, 1)

        # this result cannot fit in a 64 bit signed integer
        self.assertGreaterEqual(math.log2(builtin_sum_list_ints), 63)

        jl_sum_list_ints = jl.sum(list_ints, 1)
        self.assertEqual(jl_sum_list_ints, builtin_sum_list_ints)

        jlist_ints = jl.jlist(list_ints)
        jl_sum_jlist_ints = jl.sum(jlist_ints, 1)
        self.assertEqual(jl_sum_jlist_ints, builtin_sum_list_ints)

    def test_int_overflow_box_last(self):
        # a list which when summed will overflow a signed 64 bit integer on
        # the last element
        list_ints = [1, 2 ** 63 - 1]

        builtin_sum_list_ints = sum(list_ints)

        # this result cannot fit in a 64 bit signed integer
        self.assertGreaterEqual(math.log2(builtin_sum_list_ints), 63)

        jl_sum_list_ints = jl.sum(list_ints)
        self.assertEqual(jl_sum_list_ints, builtin_sum_list_ints)

        jlist_ints = jl.jlist(list_ints)
        jl_sum_jlist_ints = jl.sum(jlist_ints)
        self.assertEqual(jl_sum_jlist_ints, builtin_sum_list_ints)

    def test_int_overflow_box_middle(self):
        # make a long list of potentially large that are < 64 bits but should
        # sum to a number that cannot be stored
        list_ints = [self.random.randrange(2 ** 60) for _ in range(1000)]
        builtin_sum_list_ints = sum(list_ints)

        # this result cannot fit in a 64 bit signed integer
        self.assertGreaterEqual(math.log2(builtin_sum_list_ints), 63)

        # make sure that the tripping point is somewhere in the middle of the
        # list, not the first or last element which are tested in different
        # tests
        self.assertGreater(math.log2(sum(list_ints[:-1])), 63)
        self.assertLess(math.log2(list_ints[0]), 63)

        jl_sum_list_ints = jl.sum(list_ints)
        self.assertEqual(jl_sum_list_ints, builtin_sum_list_ints)

        jlist_ints = jl.jlist(list_ints)
        builtin_sum_jlist_ints = sum(jlist_ints)

        self.assertEqual(builtin_sum_jlist_ints, builtin_sum_list_ints)
        jl_sum_jlist_ints = jl.sum(jlist_ints)
        self.assertEqual(jl_sum_jlist_ints, builtin_sum_list_ints)
