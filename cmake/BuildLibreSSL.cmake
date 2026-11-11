set(LIBRESSL_VERSION "4.3.2")

# LibreSSL configuration options
set(LIBRESSL_SKIP_INSTALL ON)
set(LIBRESSL_APPS OFF)
set(LIBRESSL_TESTS OFF)
set(ENABLE_ASM OFF)
set(OPENSSLDIR "/etc/ssl")

# Newer clang releases (as shipped in recent llvm-mingw builds) ship a
# portable lib/clang/<ver>/include/endian.h shim so that #include <endian.h>
# always succeeds, even when targeting Windows. LibreSSL's own
# check_include_files(endian.h HAVE_ENDIAN_H) picks this up and concludes
# the platform has a native POSIX endian.h, which disables the WIN32
# byte-swap fallback macros in its include/compat/endian.h (they're only
# defined when HAVE_ENDIAN_H is unset) -- but clang's shim doesn't define
# be32toh()/htobe32()/etc. as functions, only as macros for non-Windows
# byte orders, so the LibreSSL build fails with "undeclared function"
# errors. Force the check to report false on Windows so LibreSSL uses its
# own winsock2-based conversions instead.
if(WIN32)
    set(HAVE_ENDIAN_H OFF CACHE BOOL "clang's portable endian.h shim isn't a real POSIX endian.h" FORCE)
endif()

include(FetchContent)
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.28.0")
    FetchContent_Declare(
        LibreSSL
        URL https://ftp.openbsd.org/pub/OpenBSD/LibreSSL/libressl-${LIBRESSL_VERSION}.tar.gz
        URL_HASH SHA256=edf01aee24c65d69e6a9efcb9d44bcda682ff9d4f3bbbd95e794e1dfa90847b5
        EXCLUDE_FROM_ALL
    )

    FetchContent_MakeAvailable(LibreSSL)
else()
    FetchContent_Declare(
        LibreSSL
        URL https://ftp.openbsd.org/pub/OpenBSD/LibreSSL/libressl-${LIBRESSL_VERSION}.tar.gz
        URL_HASH SHA256=edf01aee24c65d69e6a9efcb9d44bcda682ff9d4f3bbbd95e794e1dfa90847b5
    )
    FetchContent_GetProperties(LibreSSL)
    if(NOT LibreSSL_POPULATED)
        FetchContent_Populate(LibreSSL)
        add_subdirectory(${libressl_SOURCE_DIR} ${libressl_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif()
endif()

#target_include_directories(LibreSSL BEFORE PRIVATE ${LibreSSL_BINARY_DIR})
include_directories(${libressl_SOURCE_DIR}/include)

# In sanitizer builds, keep UndefinedBehaviorSanitizer out of LibreSSL. It
# deliberately relies on behaviour that's undefined by the letter of the
# standard but fine on the platforms it supports (e.g. unaligned word loads
# in crypto/modes/cbc128.c, free functions called through a generic
# void (*)(void*) in crypto/stack/stack.c), which UBSan reports on every TLS
# connection. AddressSanitizer stays on, as a memory error in LibreSSL would
# still be a real one. Only the Windows build compiles LibreSSL from source
# by default, so this is mostly for the Windows sanitizer CI job.
if(CMAKE_C_FLAGS MATCHES "-fsanitize=" AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    function(freedv_libressl_no_ubsan dir)
        get_property(_targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_target IN LISTS _targets)
            get_target_property(_type ${_target} TYPE)
            if(_type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
                target_compile_options(${_target} PRIVATE -fno-sanitize=undefined)
            endif()
        endforeach()
        get_property(_subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
        foreach(_subdir IN LISTS _subdirs)
            freedv_libressl_no_ubsan(${_subdir})
        endforeach()
    endfunction()
    freedv_libressl_no_ubsan(${libressl_SOURCE_DIR})
endif()
