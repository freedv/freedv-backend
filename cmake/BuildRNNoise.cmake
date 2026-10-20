message(STATUS "Will build RNNoise")

# RNNoise builds via its own autotools ./configure && make invocation
# below, which is a separate build system CMake just shells out to -- it
# never sees CMAKE_C_COMPILER_LAUNCHER/CMAKE_CXX_COMPILER_LAUNCHER (e.g.
# ccache), since those only apply to CMake's own compile rules. Bake the
# launcher into CC/CXX instead so RNNoise's build gets cached the same way
# as the rest of the project.
if(CMAKE_C_COMPILER_LAUNCHER)
    set(RNNOISE_CC "${CMAKE_C_COMPILER_LAUNCHER} ${CMAKE_C_COMPILER}")
else()
    set(RNNOISE_CC "${CMAKE_C_COMPILER}")
endif()

if(CMAKE_CXX_COMPILER_LAUNCHER)
    set(RNNOISE_CXX "${CMAKE_CXX_COMPILER_LAUNCHER} ${CMAKE_CXX_COMPILER}")
else()
    set(RNNOISE_CXX "${CMAKE_CXX_COMPILER}")
endif()

# --enable-rtcd builds RNNoise's DNN kernels for more than one instruction set and
# picks the fastest one the CPU supports at runtime: SSE4.1/AVX2 on x86 and the dot
# product instructions on AArch64 (it's a no-op anywhere else).
set(CONFIGURE_COMMAND ./autogen.sh && ./configure --with-pic --disable-examples --disable-doc --disable-shared --enable-rtcd CC=${RNNOISE_CC} CXX=${RNNOISE_CXX})

if (CMAKE_CROSSCOMPILING)
set(CONFIGURE_COMMAND ${CONFIGURE_COMMAND} --host=${CMAKE_C_COMPILER_TARGET} --target=${CMAKE_C_COMPILER_TARGET})
endif (CMAKE_CROSSCOMPILING)

# Our fork of xiph/rnnoise, which adds on top of upstream:
# - AArch64 runtime CPU detection, so generic AArch64 builds (Linux, Windows on
#   Arm) use the dot product instructions on CPUs that have them (-11 to -14%
#   rnnoise_process_frame() time), behind a single --enable-rtcd option that also
#   covers x86 (--enable-x86-rtcd is kept as an alias).
# - A faster NEON sparse_cgemv8x4() (four accumulators instead of two; -7% on an
#   Apple M1, bit-identical output).
# - The NEON sparse_sgemv8x4() from xiph/rnnoise#234. (This has no effect in the
#   default build, which compiles out the float weights it would be used with.)
# Pinned to a commit so builds don't change underneath us.
set(RNNOISE_REPO https://github.com/freedv/rnnoise.git)
set(RNNOISE_GIT_TAG 8b12912d0e4a4503ca7d83fd1111f00f7e8298e8)

include(ExternalProject)

if(APPLE)
set(RNNOISE_APPLE_MIN_BUILD -mmacosx-version-min=11.0)
# autoconf's compiler-works check invokes CC (which CMake may resolve to the
# Xcode toolchain's raw absolute cc path rather than /usr/bin/cc or `xcrun
# cc`) without going through xcrun, so it doesn't auto-discover the default
# SDK and fails with "ld: library 'System' not found". Pass -isysroot
# explicitly so RNNoise's ./configure can actually link its test program.
if(CMAKE_OSX_SYSROOT)
    set(RNNOISE_APPLE_FLAGS -isysroot\ ${CMAKE_OSX_SYSROOT}\ ${RNNOISE_APPLE_MIN_BUILD})
else()
    set(RNNOISE_APPLE_FLAGS ${RNNOISE_APPLE_MIN_BUILD})
endif(CMAKE_OSX_SYSROOT)
endif(APPLE)

if(APPLE AND BUILD_OSX_UNIVERSAL)
# RNNoise ./configure doesn't behave properly when built as a universal binary;
# build it twice and use lipo to create a universal librnnoise.a instead.
ExternalProject_Add(build_rnnoise_x86
    DOWNLOAD_EXTRACT_TIMESTAMP NO
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${CONFIGURE_COMMAND} --host=x86_64-apple-darwin --target=x86_64-apple-darwin CFLAGS=-arch\ x86_64\ -O2\ ${RNNOISE_APPLE_FLAGS}
    BUILD_COMMAND $(MAKE)
    INSTALL_COMMAND ""
    GIT_REPOSITORY ${RNNOISE_REPO}
    GIT_TAG ${RNNOISE_GIT_TAG}
    UPDATE_DISCONNECTED 1
)
ExternalProject_Add(build_rnnoise_arm
    DOWNLOAD_EXTRACT_TIMESTAMP NO
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${CONFIGURE_COMMAND} --host=aarch64-apple-darwin --target=aarch64-apple-darwin CFLAGS=-arch\ arm64\ -O2\ ${RNNOISE_APPLE_FLAGS}
    BUILD_COMMAND $(MAKE)
    INSTALL_COMMAND ""
    GIT_REPOSITORY ${RNNOISE_REPO}
    GIT_TAG ${RNNOISE_GIT_TAG}
    UPDATE_DISCONNECTED 1
)

ExternalProject_Get_Property(build_rnnoise_arm BINARY_DIR)
ExternalProject_Get_Property(build_rnnoise_arm SOURCE_DIR)
set(RNNOISE_ARM_BINARY_DIR ${BINARY_DIR})
ExternalProject_Get_Property(build_rnnoise_x86 BINARY_DIR)
set(RNNOISE_X86_BINARY_DIR ${BINARY_DIR})

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/librnnoise${CMAKE_STATIC_LIBRARY_SUFFIX}
    COMMAND lipo ${RNNOISE_ARM_BINARY_DIR}/.libs/librnnoise${CMAKE_STATIC_LIBRARY_SUFFIX} ${RNNOISE_X86_BINARY_DIR}/.libs/librnnoise${CMAKE_STATIC_LIBRARY_SUFFIX} -output ${CMAKE_CURRENT_BINARY_DIR}/librnnoise${CMAKE_STATIC_LIBRARY_SUFFIX} -create
    DEPENDS build_rnnoise_arm build_rnnoise_x86)

add_custom_target(
    librnnoise.a
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/librnnoise${CMAKE_STATIC_LIBRARY_SUFFIX})

include_directories(${SOURCE_DIR}/include)

add_library(rnnoise STATIC IMPORTED)
add_dependencies(rnnoise librnnoise.a)
set_target_properties(rnnoise PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/librnnoise${CMAKE_STATIC_LIBRARY_SUFFIX}"
)

add_library(rnnoise_inc INTERFACE)
target_include_directories(rnnoise_inc INTERFACE ${SOURCE_DIR}/include)

else(APPLE AND BUILD_OSX_UNIVERSAL)

# RNNoise's configure defaults to -g -O2. With gcc on x86-64, -O3 vectorizes
# its plain C FFT and pitch code, which -O2 mostly doesn't: rnnoise_process_frame()
# went from 69.8 to 65.0 us per frame (-7%) with bit-identical output (gcc 16,
# AMD EPYC-Milan). Not applied elsewhere: clang (macOS) showed no difference,
# gcc on AArch64 showed no gain on a Raspberry Pi 4 and changed the output
# slightly, and the Windows builds (clang via llvm-mingw) haven't been measured.
if(${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86" AND NOT APPLE AND NOT WIN32)
set(CONFIGURE_COMMAND ${CONFIGURE_COMMAND} CFLAGS=-g\ -O3)
endif(${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86" AND NOT APPLE AND NOT WIN32)

if(APPLE)
set(CONFIGURE_COMMAND ${CONFIGURE_COMMAND} CFLAGS=-O2\ ${RNNOISE_APPLE_FLAGS})
endif(APPLE)

ExternalProject_Add(build_rnnoise
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${CONFIGURE_COMMAND}
    BUILD_COMMAND $(MAKE)
    INSTALL_COMMAND ""
    GIT_REPOSITORY ${RNNOISE_REPO}
    GIT_TAG ${RNNOISE_GIT_TAG}
    UPDATE_DISCONNECTED 1
)

ExternalProject_Get_Property(build_rnnoise BINARY_DIR)
ExternalProject_Get_Property(build_rnnoise SOURCE_DIR)
add_library(rnnoise STATIC IMPORTED)
add_dependencies(rnnoise build_rnnoise)

add_library(rnnoise_inc INTERFACE)
target_include_directories(rnnoise_inc INTERFACE ${SOURCE_DIR}/include)

set_target_properties(rnnoise PROPERTIES
    IMPORTED_LOCATION "${BINARY_DIR}/.libs/librnnoise${CMAKE_STATIC_LIBRARY_SUFFIX}"
    IMPORTED_IMPLIB   "${BINARY_DIR}/.libs/librnnoise${CMAKE_STATIC_LIBRARY_SUFFIX}"
)

include_directories(${SOURCE_DIR}/include)
endif(APPLE AND BUILD_OSX_UNIVERSAL)
