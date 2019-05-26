import os
from setuptools import Extension, find_packages, setup

optlevel = os.environ.get('JLIST_OPTLEVEL', '3')

cxxflags = [
    '-Wall',
    '-Wextra',
    '-Wno-missing-field-initializers',
    '-std=gnu++17',
    '-march=native',
    '-mtune=native',
    '-O' + optlevel,
]

if optlevel == '0':
    cxxflags.append('-g')

if not os.environ.get('JLIST_ALL_COMPILE_ERRORS', False):
    cxxflags.append('-fmax-errors=15')


def extension(name, sources, depends=None):
    return Extension(
        name,
        sources,
        include_dirs=['.'],
        language='c++',
        extra_compile_args=cxxflags,
        depends=depends or [],
    )


setup(
    name='jlist',
    version='0.1.0',
    description='A list type that can store unboxed primitive values.',
    author='Joe Jevnik',
    author_email='joejev@gmail.com',
    packages=find_packages(),
    include_package_data=True,
    url='https://github.com/llllllllll/jlist',
    license='Apache 2.0',
    classifiers=[
        'Development Status :: 3 - Alpha',
        'License :: OSI Approved :: Apache Software License',
        'Intended Audience :: Developers',
        'Natural Language :: English',
        'Programming Language :: Python :: 3 :: Only',
        'Programming Language :: Python :: 3.6',
        'Programming Language :: Python :: 3.7',
        'Programming Language :: Python :: Implementation :: CPython',
        'Programming Language :: C++',
        'Operating System :: POSIX',
        'Topic :: Software Development',
        'Topic :: Utilities',
    ],
    ext_modules=[
        extension(
            'jlist.jlist',
            ['jlist/jlist.cc'],
            depends=['jlist/jlist.h'],
        ),
        extension(
            'jlist.ops',
            ['jlist/ops.cc'],
            depends=['jlist/jlist.h'],
        ),
    ],
)
