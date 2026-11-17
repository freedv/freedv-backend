if(CMAKE_CROSSCOMPILING)
    set(RADE_CMAKE_ARGS ${RADE_CMAKE_ARGS} -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})

    # build_rade is a genuinely separate `cmake` invocation (ExternalProject),
    # so it re-runs the toolchain file above and re-triggers CMake's own
    # platform-default population of CMAKE_C_STANDARD_LIBRARIES/CMAKE_CXX_
    # STANDARD_LIBRARIES (e.g. "-lkernel32 -luser32 ..."), clobbering
    # anything the toolchain file itself tried to add there (see the
    # comment in freedv-gui's cross-compile/freedv-mingw-gcc-x86_64.cmake).
    # By the time this file runs, freedv-gui's own top-level CMakeLists.txt
    # has already appended its extra libraries (e.g. -lssp/-lucrt for
    # Ubuntu's gcc-mingw-w64) to these same variables in *this* (parent)
    # configure, so forward the already-correct values through explicitly
    # rather than relying on build_rade's own toolchain-file processing to
    # reproduce them.
    set(RADE_CMAKE_ARGS ${RADE_CMAKE_ARGS} -DCMAKE_C_STANDARD_LIBRARIES=${CMAKE_C_STANDARD_LIBRARIES} -DCMAKE_CXX_STANDARD_LIBRARIES=${CMAKE_CXX_STANDARD_LIBRARIES})
endif()

set(RADE_CMAKE_ARGS ${RADE_CMAKE_ARGS} -DBUILD_OSX_UNIVERSAL=${BUILD_OSX_UNIVERSAL} -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DOPUS_URL=https://github.com/xiph/opus/archive/940d4e5af64351ca8ba8390df3f555484c567fbb.zip)

set(RADE_C_SOURCE_DIR "" CACHE PATH "Path to an already-built local rade_c checkout to link against instead of downloading and building rade_c. Must have been configured/built with this same project's RADE_CMAKE_ARGS (see BuildRADE.cmake). Requires RADE_C_BINARY_DIR to also be set.")
set(RADE_C_BINARY_DIR "" CACHE PATH "Path to the build directory produced by building the rade_c checkout referenced by RADE_C_SOURCE_DIR (contains librade and the bundled opus/fargan build artifacts). Requires RADE_C_SOURCE_DIR to also be set.")

if(RADE_C_SOURCE_DIR OR RADE_C_BINARY_DIR)
    if(NOT RADE_C_SOURCE_DIR OR NOT RADE_C_BINARY_DIR)
        message(FATAL_ERROR "RADE_C_SOURCE_DIR and RADE_C_BINARY_DIR must both be set to link a local rade_c build.")
    endif()
    if(NOT EXISTS "${RADE_C_SOURCE_DIR}/src")
        message(FATAL_ERROR "RADE_C_SOURCE_DIR (${RADE_C_SOURCE_DIR}) does not look like a rade_c checkout (missing src/ directory).")
    endif()
    message(STATUS "Linking local rade_c build (source: ${RADE_C_SOURCE_DIR}, binary: ${RADE_C_BINARY_DIR}) instead of downloading and building rade_c.")
    set(SOURCE_DIR ${RADE_C_SOURCE_DIR})
    set(BINARY_DIR ${RADE_C_BINARY_DIR})
    add_library(rade SHARED IMPORTED)
else()
    # rade_c is itself a CMake project, built here as a nested ExternalProject
    # configure/build rather than via add_subdirectory(). That nested cmake
    # invocation gets its own fresh CMakeCache.txt and doesn't inherit the
    # parent project's CMAKE_C_COMPILER_LAUNCHER/CMAKE_CXX_COMPILER_LAUNCHER
    # (e.g. ccache) automatically, so forward it explicitly. Note this only
    # covers rade_c's own sources -- Opus is built inside rade_c via its own
    # nested ExternalProject_Add (cmake/BuildOpus.cmake in that repo), which
    # has the same gap but isn't addressed here.
    if(CMAKE_C_COMPILER_LAUNCHER)
        set(RADE_CMAKE_ARGS ${RADE_CMAKE_ARGS} -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER})
    endif()
    if(CMAKE_CXX_COMPILER_LAUNCHER)
        set(RADE_CMAKE_ARGS ${RADE_CMAKE_ARGS} -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER})
    endif()
    
    include(ExternalProject)
    ExternalProject_Add(build_rade
       SOURCE_DIR rade_src
       BINARY_DIR rade_build
       GIT_REPOSITORY https://github.com/freedv/rade_c
       GIT_TAG main
       GIT_SUBMODULES ""
       GIT_SUBMODULES_RECURSE NO
       CMAKE_ARGS ${RADE_CMAKE_ARGS}
       CMAKE_CACHE_ARGS -DCMAKE_BUILD_TYPE:STRING=Release -DCMAKE_OSX_DEPLOYMENT_TARGET:STRING=${CMAKE_OSX_DEPLOYMENT_TARGET} -DCMAKE_OSX_ARCHITECTURES:STRING=${CMAKE_OSX_ARCHITECTURES}
       INSTALL_COMMAND ""
    )

    ExternalProject_Get_Property(build_rade BINARY_DIR)
    ExternalProject_Get_Property(build_rade SOURCE_DIR)
    add_library(rade SHARED IMPORTED)
    add_dependencies(rade build_rade)
endif()

include_directories(${SOURCE_DIR}/src)
target_include_directories(rade INTERFACE ${SOURCE_DIR}/src)

set_target_properties(rade PROPERTIES
    IMPORTED_LOCATION "${BINARY_DIR}/src/librade${CMAKE_SHARED_LIBRARY_SUFFIX}"
    IMPORTED_IMPLIB   "${BINARY_DIR}/src/librade${CMAKE_IMPORT_LIBRARY_SUFFIX}"
)
list(APPEND FREEDV_PACKAGE_SEARCH_PATHS ${BINARY_DIR}/src)
set(rade_BINARY_DIR ${BINARY_DIR})
set(rade_SOURCE_DIR ${SOURCE_DIR})

add_library(opus STATIC IMPORTED)
if(TARGET build_rade)
    add_dependencies(opus build_rade)
endif()
set(FARGAN_ARM_CONFIG_H_FILE "${BINARY_DIR}/build_opus_arm-prefix/src/build_opus_arm/config.h")
set(FARGAN_X86_CONFIG_H_FILE "${BINARY_DIR}/build_opus_x86-prefix/src/build_opus_x86/config.h")

if(APPLE AND BUILD_OSX_UNIVERSAL)
include_directories(SYSTEM
    ${BINARY_DIR}/build_opus_arm-prefix/src/build_opus_arm
    ${BINARY_DIR}/build_opus_arm-prefix/src/build_opus_arm/dnn
    ${BINARY_DIR}/build_opus_arm-prefix/src/build_opus_arm/celt
    ${BINARY_DIR}/build_opus_arm-prefix/src/build_opus_arm/silk
    ${BINARY_DIR}/build_opus_arm-prefix/src/build_opus_arm/include)
target_include_directories(opus INTERFACE
    ${BINARY_DIR}/build_opus_arm-prefix/src/build_opus_arm
    ${BINARY_DIR}/build_opus_arm-prefix/src/build_opus_arm/dnn
    ${BINARY_DIR}/build_opus_arm-prefix/src/build_opus_arm/celt
    ${BINARY_DIR}/build_opus_arm-prefix/src/build_opus_arm/silk
    ${BINARY_DIR}/build_opus_arm-prefix/src/build_opus_arm/include)
set_target_properties(opus PROPERTIES
    IMPORTED_LOCATION "${BINARY_DIR}/libopus${CMAKE_STATIC_LIBRARY_SUFFIX}"
)

set(FARGAN_CONFIG_H_FILE "${BINARY_DIR}/build_opus_arm-prefix/src/build_opus_arm/config.h")
else(APPLE AND BUILD_OSX_UNIVERSAL)
target_include_directories(opus INTERFACE
    ${BINARY_DIR}/build_opus-prefix/src/build_opus
    ${BINARY_DIR}/build_opus-prefix/src/build_opus/dnn
    ${BINARY_DIR}/build_opus-prefix/src/build_opus/celt
    ${BINARY_DIR}/build_opus-prefix/src/build_opus/silk
    ${BINARY_DIR}/build_opus-prefix/src/build_opus/include)
include_directories(SYSTEM
    ${BINARY_DIR}/build_opus-prefix/src/build_opus
    ${BINARY_DIR}/build_opus-prefix/src/build_opus/dnn
    ${BINARY_DIR}/build_opus-prefix/src/build_opus/celt
    ${BINARY_DIR}/build_opus-prefix/src/build_opus/silk
    ${BINARY_DIR}/build_opus-prefix/src/build_opus/include)
set_target_properties(opus PROPERTIES
    IMPORTED_LOCATION "${BINARY_DIR}/build_opus-prefix/src/build_opus/.libs/libopus${CMAKE_STATIC_LIBRARY_SUFFIX}"
)
set(FARGAN_CONFIG_H_FILE "${BINARY_DIR}/build_opus-prefix/src/build_opus/config.h")
set(FARGAN_ARM_CONFIG_H_FILE "${FARGAN_CONFIG_H_FILE}")
set(FARGAN_X86_CONFIG_H_FILE "${FARGAN_CONFIG_H_FILE}")
endif(APPLE AND BUILD_OSX_UNIVERSAL)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/fargan_config.h.in" "${CMAKE_CURRENT_BINARY_DIR}/fargan_config.h")
target_include_directories(rade INTERFACE ${CMAKE_CURRENT_BINARY_DIR})
include_directories("${CMAKE_CURRENT_BINARY_DIR}")
