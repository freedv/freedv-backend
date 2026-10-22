cmake_minimum_required(VERSION 3.13)

# Avoid warning about DOWNLOAD_EXTRACT_TIMESTAMP in CMake 3.24.
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
        cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum OS X deployment version")

set(PROJECT_NAME freedv-backend)
set(PROJECT_VERSION 1.0.0)
set(PROJECT_DESCRIPTION "Common backend code shared by FreeDV projects")
set(PROJECT_HOMEPAGE_URL "https://freedv.org")

# Makes FreeDV overridden CMake platform available for ARM MinGW builds.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/")

if(APPLE)
    project(
        ${PROJECT_NAME}
        VERSION ${PROJECT_VERSION}
        DESCRIPTION ${PROJECT_DESCRIPTION}
        HOMEPAGE_URL ${PROJECT_HOMEPAGE_URL}
        LANGUAGES C CXX OBJCXX
    )
else()
    project(
        ${PROJECT_NAME}
        VERSION ${PROJECT_VERSION}
        DESCRIPTION ${PROJECT_DESCRIPTION}
        HOMEPAGE_URL ${PROJECT_HOMEPAGE_URL}
        LANGUAGES C CXX
    )
endif(APPLE)

# Build universal ARM64 and x86_64 binaries on Mac.
if(BUILD_OSX_UNIVERSAL)
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64")
endif(BUILD_OSX_UNIVERSAL)

# Mandate a minimum of C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include helpers
include(cmake/CheckLibraryUsable.cmake)

# Compile backend dependencies
include(cmake/BuildRADE.cmake)
include(cmake/BuildRNNoise.cmake)

if(NOT DISABLE_TLS_SUPPORT)
    message(STATUS "Enabling TLS support for FreeDV Reporter")

    # ENABLE_TLS_SUPPORT is applied via target_compile_definitions() on fdv_util/
    # fdv_reporting (not add_definitions() here) so it also propagates to
    # consumers outside this subproject, like FlexTcpTask in freedv-integrations.
    # TcpConnectionHandler's layout depends on this macro, so every TU that sees
    # it must agree, or subclasses silently corrupt adjacent members at runtime.

    if(NOT USE_STATIC_LIBRESSL)
        find_package(OpenSSL)
        if(OpenSSL_FOUND)
            CheckLibraryUsable(OPENSSL_USABLE
                SYMBOL SSL_CTX_new HEADER openssl/ssl.h
                LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
            if (OPENSSL_USABLE)
                message(STATUS "Using OpenSSL for TLS support.")
                include_directories(${OPENSSL_INCLUDE_DIR})

                # OpenSSL deprecates certain symbols after 4.0, but the suggested
				# replacements don't exist in LibreSSL. This ensures we use the old
				# calls in that case.
				string(REPLACE "." ";" OpenSSL_VERSION_COMPONENTS "${OPENSSL_VERSION}")
				list(GET OpenSSL_VERSION_COMPONENTS 0 VERSION_MAJOR)
				if (VERSION_MAJOR GREATER 3)
                    add_definitions(-DOPENSSL_USE_VER_4_DEPRECATED_FNS)
                endif()
            else()
                message(STATUS "OpenSSL is not usable, ignoring found library")
                set(OpenSSL_FOUND 0)
            endif()
        endif(OpenSSL_FOUND)
    endif(NOT USE_STATIC_LIBRESSL)
    if(USE_STATIC_LIBRESSL OR NOT OpenSSL_FOUND)
        message(STATUS "Will attempt static build of LibreSSL.")
        include(cmake/BuildLibreSSL.cmake)
    endif(USE_STATIC_LIBRESSL OR NOT OpenSSL_FOUND)
else(NOT DISABLE_TLS_SUPPORT)
    message(STATUS "Disabling TLS support; FreeDV Reporter will connect via non-TLS only.")
endif(NOT DISABLE_TLS_SUPPORT)

# Look for libebur128 (loudness measurement) 
if(NOT USE_STATIC_EBUR128)
    message(STATUS "Looking for ebur128...")
    find_library(LIBEBUR128 ebur128)
    find_path(LIBEBUR128_INCLUDE_DIR ebur128.h)
    message(STATUS "  ebur128 library: ${LIBEBUR128}")
    message(STATUS "  ebur128 headers: ${LIBEBUR128_INCLUDE_DIR}")
    if(LIBEBUR128 AND LIBEBUR128_INCLUDE_DIR)
        CheckLibraryUsable(LIBEBUR128_USABLE
            SYMBOL ebur128_init HEADER ebur128.h
            LIBRARIES ${LIBEBUR128} INCLUDE_DIRS ${LIBEBUR128_INCLUDE_DIR})
    endif(LIBEBUR128 AND LIBEBUR128_INCLUDE_DIR)
    if(LIBEBUR128_USABLE)
        list(APPEND FREEDV_LINK_LIBS ${LIBEBUR128})
        include_directories(${LIBEBUR128_INCLUDE_DIR})
    else(LIBEBUR128_USABLE)
        message(STATUS "Will attempt static build of ebur128.")
        include(cmake/BuildEbur128.cmake)
    endif(LIBEBUR128_USABLE)
else(NOT USE_STATIC_LIBEBUR128)
    message(STATUS "Will attempt static build of ebur128.")
    include(cmake/BuildEbur128.cmake)
endif(NOT USE_STATIC_EBUR128)

# Samplerate Library
#   
if(NOT USE_STATIC_SAMPLERATE) 
    message(STATUS "Looking for samplerate...")
    find_library(LIBSAMPLERATE samplerate)
    find_path(LIBSAMPLERATE_INCLUDE_DIR samplerate.h)
    message(STATUS "  samplerate library: ${LIBSAMPLERATE}")
    message(STATUS "  samplerate headers: ${LIBSAMPLERATE_INCLUDE_DIR}")
    if(LIBSAMPLERATE AND LIBSAMPLERATE_INCLUDE_DIR)
        CheckLibraryUsable(LIBSAMPLERATE_USABLE
            SYMBOL src_new HEADER samplerate.h
            LIBRARIES ${LIBSAMPLERATE} INCLUDE_DIRS ${LIBSAMPLERATE_INCLUDE_DIR})
    endif(LIBSAMPLERATE AND LIBSAMPLERATE_INCLUDE_DIR)
    if(LIBSAMPLERATE_USABLE)
        list(APPEND FREEDV_LINK_LIBS ${LIBSAMPLERATE})
        include_directories(${LIBSAMPLERATE_INCLUDE_DIR})
    else(LIBSAMPLERATE_USABLE)
        message(STATUS "Will attempt static build of samplerate.")
        include(cmake/BuildSamplerate.cmake)
    endif(LIBSAMPLERATE_USABLE)
else(NOT USE_STATIC_SAMPLERATE)
    message(STATUS "Will attempt static build of samplerate.")
    include(cmake/BuildSamplerate.cmake)
endif(NOT USE_STATIC_SAMPLERATE)

# Enable CTest tests for freedv-gui
option(BUILD_BACKEND_UNITTESTS "Build unit test binaries." OFF)
if(BUILD_BACKEND_UNITTESTS)
message(STATUS "Enabling unit tests.")
enable_testing()
endif(BUILD_BACKEND_UNITTESTS)

# Diagnostic-only CSV logging (AgcStep), written to
# ~/agc_diag.csv for tuning and evaluation. Off by default since it does
# blocking file I/O from the real-time audio thread.
option(ENABLE_AUDIO_DIAG_LOGGING "Enable diagnostic CSV logging in the audio leveler/limiter pipeline step. Not for production builds." OFF)
if(ENABLE_AUDIO_DIAG_LOGGING)
message(STATUS "Enabling audio diagnostic CSV logging.")
add_compile_definitions(FREEDV_ENABLE_AUDIO_DIAG_LOGGING)
endif(ENABLE_AUDIO_DIAG_LOGGING)

add_subdirectory(src)
