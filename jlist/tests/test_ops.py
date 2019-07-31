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

    def test_int_overflow_box(self):
        # make a long list of potentially large that are < 64 bits but should
        # sum to a number that cannot be stored
        list_ints = [self.random.randrange(2 ** 60) for _ in range(1000)]
        builtin_sum_list_ints = sum(list_ints)

        # this result cannot fit in 64 bits
        self.assertGreater(math.log2(builtin_sum_list_ints), 64)

        jl_sum_list_ints = jl.sum(list_ints)
        self.assertEqual(jl_sum_list_ints, builtin_sum_list_ints)

        jlist_ints = jl.jlist(list_ints)
        builtin_sum_jlist_ints = sum(jlist_ints)

        self.assertEqual(builtin_sum_jlist_ints, builtin_sum_list_ints)
        jl_sum_jlist_ints = jl.sum(jlist_ints)
        self.assertEqual(jl_sum_jlist_ints, builtin_sum_list_ints)
