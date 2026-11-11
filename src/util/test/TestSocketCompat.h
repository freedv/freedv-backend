//=========================================================================
// Name:            TestSocketCompat.h
// Purpose:         Lets the unit tests use one set of socket (and a few
//                  other POSIX) calls on Linux, macOS and Windows.
//
// Socket handles are kept in plain ints: Winsock's SOCKET handles fit, and
// INVALID_SOCKET converts to -1, so "fd < 0" checks work everywhere.
//
// License:
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2.1,
//  as published by the Free Software Foundation.  This program is
//  distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
//  License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, see <http://www.gnu.org/licenses/>.
//
//=========================================================================

#ifndef TEST_SOCKET_COMPAT_H
#define TEST_SOCKET_COMPAT_H

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <windows.h>

// wincrypt.h (pulled in by windows.h unless something earlier included it
// without WIN32_LEAN_AND_MEAN) defines these as macros, which breaks
// OpenSSL/LibreSSL headers included afterwards.
#undef X509_NAME
#undef X509_EXTENSIONS
#undef PKCS7_ISSUER_AND_SERIAL
#undef PKCS7_SIGNER_INFO
#undef OCSP_REQUEST
#undef OCSP_RESPONSE

// Winsock must be initialized before any socket call, including the ones a
// test makes before constructing a handler (which would otherwise do it).
inline const bool TestWinsockInitialized = []() {
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
}();

#else

#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#endif // defined(_WIN32)

inline int testOpenSocket(int family, int type)
{
    return (int)::socket(family, type, 0);
}

inline int testAccept(int listenFd)
{
    return (int)::accept(listenFd, nullptr, nullptr);
}

inline void testCloseSocket(int fd)
{
#if defined(_WIN32)
    ::closesocket((SOCKET)fd);
#else
    ::close(fd);
#endif // defined(_WIN32)
}

inline int testSetSockOpt(int fd, int level, int name, const void* value, int length)
{
    return ::setsockopt(fd, level, name, (const char*)value, length);
}

// Waits up to timeoutMs for fd to become readable.
inline bool testWaitReadable(int fd, int timeoutMs)
{
    if (fd < 0)
    {
        return false;
    }
#if defined(_WIN32)
    WSAPOLLFD pfd;
    pfd.fd = (SOCKET)fd;
    pfd.events = POLLRDNORM;
    pfd.revents = 0;
    return WSAPoll(&pfd, 1, timeoutMs) > 0;
#else
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    return poll(&pfd, 1, timeoutMs) > 0;
#endif // defined(_WIN32)
}

// Bounds how long a blocking recv() on fd waits.
inline void testSetRecvTimeout(int fd, int timeoutMs)
{
#if defined(_WIN32)
    DWORD timeout = timeoutMs;
#else
    struct timeval timeout;
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
#endif // defined(_WIN32)
    testSetSockOpt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

// Bounds how long a blocking send() on fd waits.
inline void testSetSendTimeout(int fd, int timeoutMs)
{
#if defined(_WIN32)
    DWORD timeout = timeoutMs;
#else
    struct timeval timeout;
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
#endif // defined(_WIN32)
    testSetSockOpt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

// True if the last recv() failed because its SO_RCVTIMEO timeout expired.
inline bool testLastRecvTimedOut()
{
#if defined(_WIN32)
    return WSAGetLastError() == WSAETIMEDOUT;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif // defined(_WIN32)
}

inline void testSetNonBlocking(int fd)
{
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket((SOCKET)fd, FIONBIO, &mode);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#endif // defined(_WIN32)
}

// Whether the socket becomes writable (e.g. a non-blocking connect
// completes) within timeoutMs.
inline bool testWaitWritable(int fd, int timeoutMs)
{
    fd_set writeSet;
    FD_ZERO(&writeSet);
#if defined(_WIN32)
    FD_SET((SOCKET)fd, &writeSet);
#else
    FD_SET(fd, &writeSet);
#endif // defined(_WIN32)
    struct timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    return ::select(fd + 1, nullptr, &writeSet, nullptr, &tv) > 0;
}

inline void testSetEnv(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif // defined(_WIN32)
}

// Creates an empty temporary file and returns its path ("" on failure).
inline std::string testMakeTempFile(const char* prefix)
{
#if defined(_WIN32)
    char dir[MAX_PATH];
    char path[MAX_PATH];
    if (GetTempPathA(sizeof(dir), dir) == 0 || GetTempFileNameA(dir, prefix, 0, path) == 0)
    {
        return "";
    }
    return path;
#else
    std::string path = std::string("/tmp/") + prefix + "_XXXXXX";
    int fd = mkstemp(&path[0]);
    if (fd < 0)
    {
        return "";
    }
    close(fd);
    return path;
#endif // defined(_WIN32)
}

#endif // TEST_SOCKET_COMPAT_H
