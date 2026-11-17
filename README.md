# freedv-backend

This repository contains a series of static libraries used by other FreeDV repositories (such as [freedv-integrations](https://github.com/freedv/freedv-integrations)) to support various functions:

1. RADE encode and decode.
2. Audio processing (e.g. resampling, AGC and RNNoise, among other actions).
3. FreeDV Reporter/PSK Reporter handling (TCP/IP, callsign encoding/decoding in RADE transmissions)

This is intended to be used as a Git submodule, i.e.

```sh
project/src$ git submodule add https://github.com/freedv/freedv-backend backend
...

# inside src/CMakeLists.txt
add_subdirectory(backend)
```

with required libraries linked in as needed.

## Compiling standalone

This project can be compiled standalone by running `cmake`:

```sh
mkdir build
cd build
cmake ..
make
```

To enable unit tests, you can pass `-DBUILD_BACKEND_UNITTESTS=1` to `cmake`:

```sh
cmake -DBUILD_BACKEND_UNITTESTS=1 ..
make
ctest -V
```

It's highly recommended to also set up a Python venv so that the "loss" tests can be executed
(see [RADE README](https://github.com/drowe67/radae/blob/main/README.md#verifying-rade-integration)):

```sh
python3 -m venv rade-venv
. ./rade-venv/bin/activate
pip install torch matplotlib
mkdir build
cd build
cmake -DRADAE_PYTHON_EXECUTABLE=$(pwd)/../rade-venv/bin/python3 -DBUILD_BACKEND_UNITTESTS=1 ..
make
ctest -V
```

Tests take some time. A good result ends like this:

```
100% tests passed, 0 tests failed out of 10

Total Test time (real) = 198.79 sec
```

TLS support (mainly for FreeDV Reporter) is enabled by default, but can be disabled (i.e. for platforms that can't do TLS) by passing `-DDISABLE_TLS_SUPPORT=1` to `cmake`.
Additionally, LibreSSL can be statically linked instead of the system's copy of OpenSSL by passing in `-DUSE_STATIC_LIBRESSL=1`.

By default, `rade_c` is downloaded and built automatically as part of this project's build. To link against your own local build of `rade_c` instead, pass both `-DRADE_C_SOURCE_DIR=<path to rade_c checkout>` and `-DRADE_C_BINARY_DIR=<path to that checkout's build directory>` to `cmake`. The referenced build must have been produced using the same CMake options this project would otherwise pass to `rade_c` (see `cmake/BuildRADE.cmake`).

## Getting Support

Please create a GitHub issue if you find a problem with this repository.
