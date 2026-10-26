if(WIN32)
    set(UTIL_WIN32_FILES logging/libfmemopen.c Win32COMObject.cpp)
endif(WIN32)

if(LINUX)
    set(UTIL_LINUX_FILES timespec.c)
endif(LINUX)

if(APPLE)
    set(THREADED_OBJECT_FILES ThreadedObject.mm)
else(APPLE)
    set(THREADED_OBJECT_FILES ThreadedObject.cpp)
endif(APPLE)

add_library(fdv_util STATIC
    DiagnosticCsvLogger.cpp
    LoudnessMeter.cpp
    Semaphore.cpp
    SocketIoClient.cpp
    ${THREADED_OBJECT_FILES}
    ThreadedTimer.cpp
    TcpConnectionHandler.cpp
    UdpHandler.cpp
    logging/ulog.c
    logging/ulog_async.cpp
    ${UTIL_LINUX_FILES}
    ${UTIL_WIN32_FILES}
)

# ULOG_ASYNC is also set via add_definitions() in ../CMakeLists.txt; repeated
# here as a PUBLIC usage requirement so it reaches downstream projects that
# link fdv_util through FetchContent (same rationale as ENABLE_TLS_SUPPORT
# below). It changes ulog_Event's layout, so it must be seen wherever ulog.h is.
target_compile_definitions(fdv_util PUBLIC ULOG_ASYNC)

find_package(Threads REQUIRED)

target_compile_options(fdv_util PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(fdv_util yyjson fdv_os_wrapper Threads::Threads ${LIBEBUR128})
if(WIN32)
    target_link_libraries(fdv_util iphlpapi)
endif(WIN32)

# Some toolchains turn std::atomic operations into libatomic calls rather
# than inline code (e.g. clang with libstdc++ leaves is_lock_free() as a call
# when not optimizing), so link libatomic when atomics don't link without it.
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
set(FDV_ATOMIC_TEST_SOURCE "
#include <atomic>
#include <cstdint>
std::atomic<int> i;
std::atomic<std::int64_t> l;
int main() { return (i.is_lock_free() && l.is_lock_free()) ? (int)++l : 0; }")
check_cxx_source_compiles("${FDV_ATOMIC_TEST_SOURCE}" FDV_ATOMICS_WITHOUT_LIBATOMIC)
if(NOT FDV_ATOMICS_WITHOUT_LIBATOMIC)
    cmake_push_check_state()
    list(APPEND CMAKE_REQUIRED_LIBRARIES atomic)
    check_cxx_source_compiles("${FDV_ATOMIC_TEST_SOURCE}" FDV_ATOMICS_WITH_LIBATOMIC)
    cmake_pop_check_state()
    if(FDV_ATOMICS_WITH_LIBATOMIC)
        target_link_libraries(fdv_util atomic)
    else(FDV_ATOMICS_WITH_LIBATOMIC)
        message(WARNING "std::atomic doesn't link with or without libatomic; the build may fail")
    endif(FDV_ATOMICS_WITH_LIBATOMIC)
endif(NOT FDV_ATOMICS_WITHOUT_LIBATOMIC)
if(NOT DISABLE_TLS_SUPPORT)
    target_compile_definitions(fdv_util PUBLIC ENABLE_TLS_SUPPORT)

    if (OpenSSL_FOUND)
        target_link_libraries(fdv_util OpenSSL::SSL)
	target_compile_definitions(fdv_util PUBLIC ENABLE_TLS_SUPPORT_WITH_OPENSSL)
    else (OpenSSL_FOUND)
        target_link_libraries(fdv_util tls)
    endif (OpenSSL_FOUND)

    if(WIN32)
        target_link_libraries(fdv_util crypt32 cryptui)
    endif(WIN32)
endif(NOT DISABLE_TLS_SUPPORT)

target_include_directories(fdv_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(BUILD_BACKEND_UNITTESTS)
macro(DefineUnitTest utName)
    add_executable(${utName} test/${utName}.cpp)
    target_link_libraries(${utName} PRIVATE fdv_util)

    add_test(NAME util_${utName} COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${utName})
    # Fail a hung test instead of letting it run until CI gives up.
    set_tests_properties(util_${utName} PROPERTIES TIMEOUT 600)
endmacro()

DefineUnitTest(ThreadedTimerTest)
DefineUnitTest(GenericFIFOTest)
DefineUnitTest(UlogAsyncTest)
DefineUnitTest(TcpConnectionHandlerTest)
DefineUnitTest(UdpHandlerTest)
DefineUnitTest(AudioSpinMutexTest)
DefineUnitTest(UlogConfigTest)
DefineUnitTest(ThreadedObjectTest)
DefineUnitTest(SemaphoreTest)
DefineUnitTest(EventHandlerTest)
endif(BUILD_BACKEND_UNITTESTS)

