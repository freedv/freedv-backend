//=========================================================================
// Name:            TcpConnectionHandler.cpp
// Purpose:         Handler for TCP connections.
//
// Authors:         Mooneer Salem
// License:
//
//  All rights reserved.
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

#include <chrono>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <unistd.h>
#include <algorithm>
#include <sstream>

#include "TcpConnectionHandler.h"

#if defined(WIN32) || defined(__MINGW32__)

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif // !_WIN32_WINNT

#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2def.h>
#include <wincrypt.h>
#include <cryptuiapi.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#endif // defined(WIN32) || defined(__MINGW32__)

#include "logging/ulog.h"
#include "../os/os_interface.h"

using namespace std::chrono_literals;

#define RX_ATTEMPT_INTERVAL_MS (50)
#define RECONNECT_INTERVAL_MS (5000)
#define RX_BUFFER_SIZE (128 * 1024)

#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif // INVALID_SOCKET

// How long to wait in select() before checking whether a disconnect was requested.
constexpr long SELECT_SLICE_US = 100000; // 100ms

// How long to allow the TLS handshake before giving up and trying again later.
constexpr auto TLS_HANDSHAKE_TIMEOUT = 10s;

#if defined(ENABLE_TLS_SUPPORT)
static std::string GetSSLError_()
{
    //std::string ret = ERR_error_string(ERR_get_error(), NULL);
    BIO *bio = BIO_new(BIO_s_mem());
    ERR_print_errors(bio);
    char *buf;
    size_t len = BIO_get_mem_data(bio, &buf);
    std::string ret(buf, len);
    BIO_free(bio);
    return ret;
}
#endif // defined(ENABLE_TLS_SUPPORT)

TcpConnectionHandler::TcpConnectionHandler()
    : ThreadedObject("TcpConn")
    , enableReconnect_(false)
    , reconnectTimer_(RECONNECT_INTERVAL_MS, [&](ThreadedTimer&) {
        enqueue_(std::bind(&TcpConnectionHandler::connectImpl_, this));
    }, false)
    , socket_(-1)
    , cancelConnect_(false)
    , receiveBuffer_(RX_BUFFER_SIZE)
{
#if defined(WIN32)
    // Initialize Winsock in case it hasn't already been done.
    WSADATA wsaData;
    int result = 0;
    result = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (result != 0)
    {
        log_warn("Winsock could not be initialized: %d", result); 
    }
#endif // defined(WIN32)
}

TcpConnectionHandler::~TcpConnectionHandler()
{
    // Make sure we're disconnected before destroying.
    enableReconnect_.store(false, std::memory_order_relaxed);

    auto fut = disconnect();
    fut.wait();

    // Make absolutely sure there's nothing else in the queue before release.
    waitForAllTasksComplete_();
    
#if defined(WIN32)
    WSACleanup();
#endif // defined(WIN32)
}

std::future<void> TcpConnectionHandler::connect(const char* host, int port, bool enableReconnect, bool enableTLS)
{
    cancelConnect_.store(false, std::memory_order_relaxed);
    
    std::shared_ptr<std::promise<void>> prom = std::make_shared<std::promise<void> >();
    auto fut = prom->get_future();
    
    // The connection parameters are only changed on the worker thread, and
    // only while disconnected: the receive thread reads usingTLS_.
    std::string hostStr(host);
    enqueue_([&, prom, hostStr, port, enableReconnect, enableTLS]() {
        if (socket_.load(std::memory_order_relaxed) != INVALID_SOCKET)
        {
            log_warn("connect() called while already connected to %s port %d; ignoring", host_.c_str(), port_);
        }
        else
        {
            host_ = hostStr;
            port_ = port;
            usingTLS_ = enableTLS;
            enableReconnect_.store(enableReconnect, std::memory_order_relaxed);
            connectImpl_();
        }
        prom->set_value();
    });
    return fut;
}

std::future<void> TcpConnectionHandler::disconnect()
{
    // Cancel any pending connections
    cancelConnect_.store(true, std::memory_order_relaxed);
    
    std::shared_ptr<std::promise<void>> prom = std::make_shared<std::promise<void> >();
    auto fut = prom->get_future();
    
    enqueue_([&, prom]() {
        disconnectImpl_();
        cancelConnect_.store(false, std::memory_order_relaxed);
        prom->set_value();
    });
    return fut;
}

std::future<void> TcpConnectionHandler::send(const char* buf, int length)
{
    std::shared_ptr<std::promise<void>> prom = std::make_shared<std::promise<void> >();
    auto fut = prom->get_future();
    
    char* allocBuf = new char[length];
    assert(allocBuf != nullptr);
    memcpy(allocBuf, buf, length);
    
    enqueue_([&, prom, allocBuf, length]() { // NOLINT
        sendImpl_(allocBuf, length);
        delete[] allocBuf;
        prom->set_value();
    });
    return fut;
}

void TcpConnectionHandler::setOnRecvEndFn(OnRecvEndFn fn)
{
    enqueue_([this, fn = std::move(fn)]() {
        onRecvEndFn_ = fn;
    });
}

void TcpConnectionHandler::connectImpl_()
{
    // A connect() or reconnect attempt can be queued behind one that has
    // already succeeded. Starting over on a live connection would leak its
    // socket and TLS state and replace a running receive thread.
    if (socket_.load(std::memory_order_relaxed) != INVALID_SOCKET)
    {
        return;
    }

    // Convert port to string (needed by getaddrinfo() below).
    std::stringstream portStream;
    portStream << port_;
    
    log_info("Attempting connection to %s port %d", host_.c_str(), port_);
    
    // Happy Eyeballs v2.0:
    //     * Begin asynchronous DNS requests (IPv6 followed by IPv4)
    //     * If IPv6 received first: immediately attempt IPv6 connection
    //                         Else: wait 50 ms for IPv6 DNS to come back before attempting IPv4
    //     * Start new connection every 250ms until one succeeds or all fail.
    // Ref: https://datatracker.ietf.org/doc/html/rfc8305
    struct addrinfo* results[] = {nullptr, nullptr}; // [0] = IPv6, [1] = IPv4
    struct addrinfo* heads[] = {nullptr, nullptr};
    
    ipv4Complete_.store(false, std::memory_order_relaxed);
    ipv6Complete_.store(false, std::memory_order_relaxed);

    std::shared_ptr<std::promise<struct addrinfo*> > ipv6ResultPromise = 
        std::make_shared<std::promise<struct addrinfo*> >();
    std::shared_ptr<std::promise<struct addrinfo*> > ipv4ResultPromise = 
        std::make_shared<std::promise<struct addrinfo*> >();
    std::future<struct addrinfo*> ipv6ResultFuture = ipv6ResultPromise->get_future();
    std::future<struct addrinfo*> ipv4ResultFuture = ipv4ResultPromise->get_future();

    std::string portStr = portStream.str();
    std::thread ipv6ResolveThread([&, portStr, ipv6ResultPromise]() {
        SetThreadName("ipv6");

        struct addrinfo *result = nullptr;
        resolveAddresses_(AF_INET6, host_.c_str(), portStr.c_str(), &result);
        ipv6ResultPromise->set_value(result);
        ipv6Complete_.store(true, std::memory_order_relaxed);
    });
    
    std::thread ipv4ResolveThread([&, portStr, ipv4ResultPromise]() {
        SetThreadName("ipv4");

        struct addrinfo *result = nullptr;
        resolveAddresses_(AF_INET, host_.c_str(), portStr.c_str(), &result);
        ipv4ResultPromise->set_value(result);
        ipv4Complete_.store(true, std::memory_order_relaxed);
    });
    
    log_info("waiting for DNS");
    
    while (!cancelConnect_.load(std::memory_order_relaxed) && ipv4Complete_.load(std::memory_order_relaxed) == false && ipv6Complete_.load(std::memory_order_relaxed) == false)
    {
        std::this_thread::sleep_for(1ms);
    }
    
    log_info("some DNS results are available (v4 = %d, v6 = %d)", ipv4Complete_.load(std::memory_order_relaxed), ipv6Complete_.load(std::memory_order_relaxed));

    if (ipv6Complete_.load(std::memory_order_relaxed) == true)
    {
        heads[0] = ipv6ResultFuture.get();
        results[0] = heads[0];
    }

    if (ipv4Complete_.load(std::memory_order_relaxed) == true)
    {
        heads[1] = ipv4ResultFuture.get();
        results[1] = heads[1];
    }

    if (ipv6Complete_.load(std::memory_order_relaxed) == false)
    {
        log_info("waiting additional time for IPv6 DNS results");
        std::this_thread::sleep_for(50ms);
        if (ipv6Complete_.load(std::memory_order_relaxed) == true)
        {
            heads[0] = ipv6ResultFuture.get();
            results[0] = heads[0];
        }
    }
    // Check whether the IPv4 result has been collected rather than whether it's
    // complete: IPv4 DNS may finish after the check above, and testing
    // ipv4Complete_ here would then skip collecting it and leave us with no
    // addresses at all.
    if (ipv4ResultFuture.valid() && ipv6Complete_.load(std::memory_order_relaxed) == true && results[0] == nullptr)
    {
        log_info("no valid IPv6 results, need to wait for IPv4 before continuing.");
        heads[1] = ipv4ResultFuture.get(); // blocks until IPv4 DNS completes
        results[1] = heads[1];
    }

    // Choose based on the results we've actually collected rather than the
    // ipv*Complete_ flags: the resolver threads set those *after* publishing
    // their result, so a result we've already collected can still have its
    // flag reading false here.
    int whichIndex = 0;
    if (results[0] != nullptr)
    {
        log_info("starting with IPv6 connection");
    }
    else if (results[1] != nullptr)
    {
        log_info("starting with IPv4 connection");
        whichIndex = 1;
    }

#if !defined(WIN32)
    int flags = 0;
    std::vector<int> pendingSockets;
#else
    std::vector<SOCKET> pendingSockets;
#endif // !defined(WIN32)
    
    while (!cancelConnect_.load(std::memory_order_relaxed) && (results[0] || results[1]))
    {
        // At least one list is non-empty; make sure we're on one of those.
        if (results[whichIndex] == nullptr)
        {
            whichIndex = (whichIndex + 1) % 2;
        }
        struct addrinfo* current = results[whichIndex];
        int ret = 0;
#if defined(WIN32)
        u_long mode = 1;  // 1 to enable non-blocking socket
#endif // defined(WIN32)
        
        log_debug("create socket");
#if defined(WIN32)
        SOCKET fd = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (fd == INVALID_SOCKET)
        {
            log_warn("cannot open socket (err=%d)", WSAGetLastError());
            goto next_fd;
        }
#else
        int fd = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if(fd < 0)
        {
            log_warn("cannot open socket (err=%d)", errno);
            goto next_fd;
        }
#endif // defined(WIN32)

        pendingSockets.push_back(fd);
            
        // Set socket as non-blocking. Required to enable Happy Eyeballs behavior. 
        log_debug("set socket non-blocking"); 
#if defined(WIN32)
        if (ioctlsocket(fd, FIONBIO, &mode) != 0)
        {
            log_warn("cannot set socket as nonblocking (err=%d)", WSAGetLastError());
            closesocket(fd);
            pendingSockets.pop_back();
            goto next_fd;
        }
#else
        flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1)
        {
            log_warn("cannot get socket flags (err=%d)", errno);
            close(fd);
            pendingSockets.pop_back();
            goto next_fd;
        }
        
        flags |= O_NONBLOCK;
        if (fcntl(fd, F_SETFL, flags) != 0)
        {
            log_warn("cannot set socket as non-blocking (err=%d)", errno);
            close(fd);
            pendingSockets.pop_back();
            goto next_fd;
        }
#endif // defined(WIN32)
        
        // Start connection attempt
        char buf[256];
        getnameinfo(current->ai_addr, current->ai_addrlen, buf, sizeof(buf), nullptr, 0, NI_NUMERICHOST);

        log_info("attempting connection to %s", buf);
        ret = ::connect(fd, current->ai_addr, current->ai_addrlen);
        if (ret == 0)
        {
            // Connection succeeded immediately -- no need to attempt any other IPs
            socket_.store(fd, std::memory_order_relaxed);
            break;
        }
#if defined(WIN32)
        else if (WSAGetLastError() != WSAEWOULDBLOCK)
        {
            int lastErr = WSAGetLastError();
            log_warn("cannot start connection to %s (err=%d)", buf, lastErr);
            closesocket(fd);
            pendingSockets.pop_back();
            goto next_fd;
        }
#else
        else if (ret == -1 && errno != EINPROGRESS)
        {
            int err = errno;
            constexpr int ERROR_BUFFER_LEN = 1024;
            char tmpBuf[ERROR_BUFFER_LEN];
#if defined(__APPLE__) || ((_POSIX_C_SOURCE >= 200112L) && !_GNU_SOURCE)
            auto rv = strerror_r(err, tmpBuf, ERROR_BUFFER_LEN);
            if (rv != 0)
            {
                strncpy(tmpBuf, "(null)", 7);
            }
            log_warn("cannot start connection to %s (err=%d: %s)", buf, err, tmpBuf);
#else
            auto ptr = strerror_r(err, tmpBuf, ERROR_BUFFER_LEN);
            if (ptr == nullptr)
            {
                strncpy(tmpBuf, "(null)", 7);
            }
            else
            {
                memmove(tmpBuf, ptr, strlen(ptr) + 1);
            }
            log_warn("cannot start connection to %s (err=%d: %s)", buf, err, tmpBuf);
#endif // defined(__APPLE__) || ((_POSIX_C_SOURCE >= 200112L) && !_GNU_SOURCE)
            close(fd);
            pendingSockets.pop_back();
            goto next_fd;
        }
#endif // defined(WIN32)        

        // Check socket list to see if there have been any connections yet.
        checkConnections_(pendingSockets);
        if (socket_.load(std::memory_order_relaxed) != INVALID_SOCKET)
        {
            break;
        }
        
next_fd:
        results[whichIndex] = results[whichIndex]->ai_next;
        whichIndex = (whichIndex + 1) % 2;

        // See if we got any new DNS results. Only take results that are
        // already in: blocking on a slow lookup here would hold up trying
        // the other family's remaining addresses (and noticing that an
        // attempt already in flight has connected) until it finished.
        if (whichIndex == 0 && ipv6ResultFuture.valid() &&
            ipv6ResultFuture.wait_for(0s) == std::future_status::ready)
        {
            heads[0] = ipv6ResultFuture.get();
            results[0] = heads[0];
        }
        if (whichIndex == 1 && ipv4ResultFuture.valid() &&
            ipv4ResultFuture.wait_for(0s) == std::future_status::ready)
        {
            heads[1] = ipv4ResultFuture.get();
            results[1] = heads[1];
        }

        if (results[whichIndex] == nullptr)
        {
            whichIndex = (whichIndex + 1) % 2;
            
            if (results[whichIndex] == nullptr && (ipv4Complete_.load(std::memory_order_relaxed) == false || ipv6Complete_.load(std::memory_order_relaxed) == false))
            {
                // No more addresses to go through, so we *really* need to make sure
                // we're done with DNS before exiting the loop. Keep watching the
                // attempts already in flight meanwhile: one of them may connect
                // long before a slow lookup finishes.
                log_info("ran out of addresses to check but DNS requests are still pending");
                while ((ipv4Complete_.load(std::memory_order_relaxed) == false || ipv6Complete_.load(std::memory_order_relaxed) == false) &&
                       !cancelConnect_.load(std::memory_order_relaxed) &&
                       socket_.load(std::memory_order_relaxed) == INVALID_SOCKET)
                {
                    if (pendingSockets.empty())
                    {
                        std::this_thread::sleep_for(1ms);
                    }
                    else
                    {
                        checkConnections_(pendingSockets); // waits up to 250ms
                    }
                }
                if (socket_.load(std::memory_order_relaxed) != INVALID_SOCKET ||
                    cancelConnect_.load(std::memory_order_relaxed))
                {
                    break;
                }
                
                if (ipv4ResultFuture.valid())
                {
                    heads[1] = ipv4ResultFuture.get();
                    results[1] = heads[1];
                    whichIndex = 1;
                }
                
                if (ipv6ResultFuture.valid())
                {
                    heads[0] = ipv6ResultFuture.get();
                    results[0] = heads[0];
                    if (results[0] != nullptr)
                    {
                        // Prioritize IPv6 over IPv4 if we finally got results for the former.
                        whichIndex = 0;
                    }
                }
            }
        }
    }
    
    // Keep checking connections until one connects or we all time out.
    while (!cancelConnect_.load(std::memory_order_relaxed) && pendingSockets.size() > 0 && socket_.load(std::memory_order_relaxed) == INVALID_SOCKET)
    {
        checkConnections_(pendingSockets);
    }
    
    // Close any connections still pending.
    for (auto& sock : pendingSockets)
    {
        if (sock != socket_.load(std::memory_order_relaxed))
        {
#if defined(WIN32)
            closesocket(sock);
#else
            close(sock);
#endif // defined(WIN32)
        }
    }
    
    if (socket_.load(std::memory_order_relaxed) != INVALID_SOCKET)
    {
        bool connSucceeded = true;
#if defined(ENABLE_TLS_SUPPORT)
        sslCtx_.store(nullptr, std::memory_order_relaxed);
        ssl_.store(nullptr, std::memory_order_relaxed);

        // We have a valid socket. If the user wants to connect via TLS, attempt TLS negotiation now.
        // If we can't negotiate TLS for whatever reason, close socket and try again in a bit.
        if (usingTLS_)
        {
            const SSL_METHOD* method = TLS_client_method();
            sslCtx_.store(SSL_CTX_new(method), std::memory_order_relaxed);
            if (sslCtx_.load(std::memory_order_relaxed) == nullptr)
            {
                auto errStr = GetSSLError_();
                log_error("Unable to create SSL context: %s", errStr.c_str());
                disconnectImpl_(false);
                connSucceeded = false;
            }
            else
            {
                // Set up certificate validation
                SSL_CTX_set_verify(sslCtx_.load(std::memory_order_relaxed), SSL_VERIFY_PEER, nullptr);

                // Set up root certificate locations. Note that on Windows,
                // we use the Windows certificate store, so we need to manually
                // import those root certificates. For non-Windows platforms, we
                // use the default paths. On all platforms, we then also query
                // SSL_CERT_DIR/SSL_CERT_FILE from the environment and add those
                // locations as needed. (OpenSSL does this for us, but LibreSSL does not,
                // hence the need to manually query the environment here.)
#if defined(WIN32)
                {
                    HCERTSTORE hStore;
                    PCCERT_CONTEXT pContext = nullptr;
                    X509 *x509 = nullptr;
                    X509_STORE *store = SSL_CTX_get_cert_store(sslCtx_.load(std::memory_order_relaxed));
                    
                    if (store == nullptr)
                    {
                        store = X509_STORE_new();
                        SSL_CTX_set_cert_store(sslCtx_.load(std::memory_order_relaxed), store);
                    }

                    hStore = CertOpenSystemStoreW(NULL, L"ROOT");
                    if (!hStore)
                    {
                        log_warn("Could not open Windows certificate store");
                    }
                    else
                    {
                        while ((pContext = CertEnumCertificatesInStore(hStore, pContext)) != nullptr)
                        {
                            const unsigned char *encoded_cert = pContext->pbCertEncoded;
                            x509 = d2i_X509(NULL, &encoded_cert, pContext->cbCertEncoded);
                            if (x509)
                            {
                                if (X509_STORE_add_cert(store, x509) != 1)
                                {
                                   auto errStr = GetSSLError_();
                                   log_warn("Could not add certificate to internal store: %s", errStr.c_str());
                                }
                                X509_free(x509);
                            }
                        }
                        CertCloseStore(hStore, 0);
                    }
                }
#else
                if (!SSL_CTX_set_default_verify_paths(sslCtx_.load(std::memory_order_relaxed)))
                {
#if defined(__APPLE__) && defined(ENABLE_TLS_SUPPORT_WITH_OPENSSL)
                    log_info("Setting TLS certificate validation paths failed, but this is expected on macOS");
#else
                    auto errStr = GetSSLError_();
                    log_warn("Unable to set TLS certificate validation paths: %s", errStr.c_str());
#endif // defined(__APPLE__) && defined(ENABLE_TLS_SUPPORT_WITH_OPENSSL)
                }
#endif // defined(WIN32)

                auto sslCertDirEnv = getenv("SSL_CERT_DIR"); // NOLINT
                auto sslCertFileEnv = getenv("SSL_CERT_FILE"); // NOLINT
                if ((sslCertDirEnv != nullptr || sslCertFileEnv != nullptr) && !SSL_CTX_load_verify_locations(sslCtx_.load(std::memory_order_relaxed), sslCertFileEnv, sslCertDirEnv))
                {
                    auto errStr = GetSSLError_();
                    log_warn("Unable to set TLS certificate locations: %s", errStr.c_str());
                }

                // Force >= TLS 1.2
                if (!SSL_CTX_set_min_proto_version(sslCtx_.load(std::memory_order_relaxed), TLS1_2_VERSION)) 
                {
                    auto errStr = GetSSLError_();
                    log_warn("Unable to mandate minimum TLS version: %s", errStr.c_str());
                }

                ssl_.store(SSL_new(sslCtx_.load(std::memory_order_relaxed)), std::memory_order_relaxed);
                assert(ssl_.load(std::memory_order_relaxed) != nullptr);

                if (!SSL_set_fd(ssl_.load(std::memory_order_relaxed), (int)socket_.load(std::memory_order_relaxed)))
                {
                    auto errStr = GetSSLError_();
                    log_error("Unable to assign socket to SSL context: %s", errStr.c_str());
                    disconnectImpl_(false);
                    connSucceeded = false;
                }
                else
                {
                    // Set hostname for SSL negotiation
                    SSL_set_tlsext_host_name(ssl_.load(std::memory_order_relaxed), host_.c_str());
                    if (!SSL_set1_dnsname(ssl_.load(std::memory_order_relaxed), host_.c_str())) 
                    {
                        auto errStr = GetSSLError_();
                        log_warn("Unable to assign hostname for certificate validation: %s", errStr.c_str());
                    }

                    // Attempt SSL negotiation. The server may never respond, so give up
                    // if it takes too long or if a disconnect is requested; otherwise
                    // the disconnect waits behind us until the server closes the socket.
                    int sslRet = 0;
                    auto handshakeDeadline = std::chrono::steady_clock::now() + TLS_HANDSHAKE_TIMEOUT;
                    while ((sslRet = SSL_connect(ssl_.load(std::memory_order_relaxed))) != 1)
                    {
                        fd_set writeSet;
                        fd_set readSet;
                        FD_ZERO(&writeSet);
                        FD_ZERO(&readSet);
                        auto sslErr = SSL_get_error(ssl_.load(std::memory_order_relaxed), sslRet);
                        if (sslErr == SSL_ERROR_WANT_READ || sslErr == SSL_ERROR_WANT_WRITE)
                        {
                            if (cancelConnect_.load(std::memory_order_relaxed))
                            {
                                log_info("TLS negotiation cancelled");
                                disconnectImpl_(false);
                                connSucceeded = false;
                                break;
                            }
                            else if (std::chrono::steady_clock::now() >= handshakeDeadline)
                            {
                                log_error("TLS negotiation timed out");
                                disconnectImpl_(false);
                                connSucceeded = false;
                                break;
                            }

                            // Block until we're able to continue.
                            auto rawSock = socket_.load(std::memory_order_relaxed);
                            if (sslErr == SSL_ERROR_WANT_READ) FD_SET(rawSock, &readSet);
                            else FD_SET(rawSock, &writeSet);

                            struct timeval tv = {0, SELECT_SLICE_US};
                            select(socket_.load(std::memory_order_relaxed) + 1, &readSet, &writeSet, nullptr, &tv);
                        }
                        else
                        {
                            if (SSL_get_verify_result(ssl_.load(std::memory_order_relaxed)) != X509_V_OK)
                            {
                                log_error("Certificate validation error: %s", X509_verify_cert_error_string(SSL_get_verify_result(ssl_.load(std::memory_order_relaxed))));
                            }

                            auto errStr = GetSSLError_();
                            log_error("Unable to negotiate TLS connection: %s", errStr.c_str());
                            disconnectImpl_(false);
                            connSucceeded = false;
                            break;
                        }
                    }
                }
            }
        }
#endif // defined(ENABLE_TLS_SUPPORT)

        // Call connect handler (defined by child class).
        if (connSucceeded)
        {
            char buf[256];
            struct sockaddr_storage addr;
            socklen_t len = sizeof(addr);
            if (getpeername(socket_.load(std::memory_order_relaxed), (struct sockaddr*)&addr, &len) != 0)
            {
#if defined(WIN32)
                int err = WSAGetLastError();
#else
                int err = errno;
#endif // defined(WIN32)
                log_warn("could not get IP address of socket (err=%d)", err);
            }
            int nameErr = 0;
            if ((nameErr = getnameinfo((struct sockaddr*)&addr, len, buf, sizeof(buf), nullptr, 0, NI_NUMERICHOST)) != 0)
            {
#if defined(WIN32)
                nameErr = WSAGetLastError();
#endif // defined(WIN32)
                log_warn("could not get string representation of IP address (err=%d)", nameErr);
            }

            log_info("connection succeeded to %s", buf);
            enqueue_(std::bind(&TcpConnectionHandler::onConnect_, this));
        
            // Start receive thread
            receiveThread_ = std::thread(std::bind(&TcpConnectionHandler::receiveImpl_, this));
        }
    }
    else if (enableReconnect_.load(std::memory_order_relaxed))
    {
        // Attempt reconnect
        log_warn("connection failed, waiting to reconnect");
        reconnectTimer_.start();
    }
    
    // Free address info objects
    ipv6ResolveThread.detach();
    ipv4ResolveThread.detach();
    while (!ipv4Complete_.load(std::memory_order_relaxed) || !ipv6Complete_.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(1ms);
    }
    
    if (ipv4ResultFuture.valid())
    {
        heads[1] = ipv4ResultFuture.get();
    }
    if (ipv6ResultFuture.valid())
    {
        heads[0] = ipv6ResultFuture.get();
    }    
    if (heads[0] != nullptr)
    {
        freeaddrinfo(heads[0]);
    }
    if (heads[1] != nullptr)
    {
        freeaddrinfo(heads[1]);
    }
}

void TcpConnectionHandler::disconnectImpl_(bool callHandler)
{
    auto tmp = socket_.load(std::memory_order_relaxed);
    socket_.store(INVALID_SOCKET, std::memory_order_relaxed);
    if (tmp != INVALID_SOCKET)
    {
#if defined(ENABLE_TLS_SUPPORT)
        auto tmpSsl = ssl_.load(std::memory_order_relaxed);
        ssl_.store(nullptr, std::memory_order_relaxed);
        auto tmpCtx = sslCtx_.load(std::memory_order_relaxed);
        sslCtx_.store(nullptr, std::memory_order_relaxed);
#endif // defined(ENABLE_TLS_SUPPORT)

        if (receiveThread_.joinable())
        {
            receiveThread_.join();
        }

#if defined(ENABLE_TLS_SUPPORT)
        if (tmpSsl != nullptr) 
        {
            SSL_shutdown(tmpSsl);
            SSL_free(tmpSsl);
        }
        if (tmpCtx != nullptr)
        {
            SSL_CTX_free(tmpCtx);
        }
#endif // defined(ENABLE_TLS_SUPPORT)

#if defined(WIN32)
        closesocket(tmp);
#else
        close(tmp);
#endif // defined(WIN32)

        if (callHandler)
        {
            onDisconnect_();
        }

        if (enableReconnect_.load(std::memory_order_relaxed))
        {
            reconnectTimer_.start();
        }
    }
}

void TcpConnectionHandler::sendImpl_(const char* buf, int length)
{
    if (socket_.load(std::memory_order_relaxed) != INVALID_SOCKET)
    {
        // Simulate blocking socket for write. Most of the time this should
        // complete immediately anyway.
        while (length > 0)
        {
            fd_set writeSet;

            FD_ZERO(&writeSet);
            FD_SET(socket_.load(std::memory_order_relaxed), &writeSet);

            // Give up on the write if a disconnect is requested while we're waiting.
            struct timeval tv = {0, SELECT_SLICE_US};
            int rv = select(socket_.load(std::memory_order_relaxed) + 1, nullptr, &writeSet, nullptr, &tv);
            if (rv == 0 && cancelConnect_.load(std::memory_order_relaxed))
            {
                log_warn("write abandoned due to disconnect");
                break;
            }
            else if (rv > 0)
            {
                int numWritten = 0;
#if defined(ENABLE_TLS_SUPPORT)
                if (usingTLS_ && ssl_.load(std::memory_order_relaxed) != nullptr)
                {
                    int sslErr = SSL_ERROR_NONE;
                    while (
                        (ssl_.load(std::memory_order_relaxed) != nullptr) &&
                        (numWritten = sslWrite_(buf, length, sslErr)) < 0)
                    {
                        fd_set readSet;
                        FD_ZERO(&readSet);
                        if (sslErr == SSL_ERROR_WANT_WRITE)
                        {
                            // Can be handled by the top level loop. Not an error.
                            goto tryAgain;
                        }
                        else if (sslErr == SSL_ERROR_WANT_READ && socket_.load(std::memory_order_relaxed) != INVALID_SOCKET)
                        {
                            // Block until we're able to continue.
                            auto rawSock = socket_.load(std::memory_order_relaxed);
                            FD_SET(rawSock, &readSet);

                            struct timeval tv = {0, SELECT_SLICE_US};
                            if (select(socket_.load(std::memory_order_relaxed) + 1, &readSet, nullptr, nullptr, &tv) == 0 &&
                                cancelConnect_.load(std::memory_order_relaxed))
                            {
                                log_warn("write abandoned due to disconnect");
                                break;
                            }
                            continue;
                        }
                        else
                        {
                            auto errStr = GetSSLError_();
                            log_error("Unable to write to TLS connection: %s", errStr.c_str());
                            break;
                        }
                    }
                }
                else
#endif // defined(ENABLE_TLS_SUPPORT)
                {
#if defined(WIN32)
                    numWritten = ::send(socket_.load(std::memory_order_relaxed), buf, length, 0);
#else
                    numWritten = write(socket_.load(std::memory_order_relaxed), buf, length);
#endif // defined(WIN32)
                }
                if (numWritten > 0)
                {
                    buf += numWritten;
                    length -= numWritten;
                }
                else if (numWritten == 0)
                {
                    log_warn("write failed without error");
                    enqueue_([&]() {
                        disconnectImpl_();
                    });
                    break;
                }
                else if (numWritten < 0)
                {
#if defined(WIN32)
                    log_warn("write failed (errno=%d)", WSAGetLastError());
#else
                    log_warn("write failed (errno=%d)", errno);
#endif // defined(WIN32)
                    enqueue_([&]() {
                        disconnectImpl_();
                    });
                    break;
                }
            }
            else if (rv < 0)
            {
#if defined(WIN32)
                log_warn("write failed (errno=%d)", WSAGetLastError());
#else
                log_warn("write failed (errno=%d)", errno);
#endif // defined(WIN32)
                enqueue_([&]() {
                    disconnectImpl_();
                });
            }
            continue;
tryAgain:
            assert(usingTLS_); // to silence warning
        }
    }
}

#if defined(ENABLE_TLS_SUPPORT)
// SSL_write()/SSL_read() run on different threads (the ThreadedObject worker and
// receiveThread_), and an SSL object must not be used by two threads at once.
// Hold sslMutex_ only around the non-blocking calls themselves, never across
// select(), so neither side can stall the other.
int TcpConnectionHandler::sslWrite_(const char* buf, int length, int& sslErr)
{
    std::unique_lock<std::mutex> lk(sslMutex_);
    SSL* ssl = ssl_.load(std::memory_order_relaxed);
    if (ssl == nullptr)
    {
        sslErr = SSL_ERROR_SSL;
        return -1;
    }
    int rv = SSL_write(ssl, buf, length);
    sslErr = (rv <= 0) ? SSL_get_error(ssl, rv) : SSL_ERROR_NONE;
    return rv;
}

int TcpConnectionHandler::sslRead_(char* buf, int length, int& sslErr)
{
    std::unique_lock<std::mutex> lk(sslMutex_);
    SSL* ssl = ssl_.load(std::memory_order_relaxed);
    if (ssl == nullptr)
    {
        sslErr = SSL_ERROR_SSL;
        return -1;
    }
    int rv = SSL_read(ssl, buf, length);
    sslErr = (rv <= 0) ? SSL_get_error(ssl, rv) : SSL_ERROR_NONE;
    return rv;
}

int TcpConnectionHandler::sslPending_()
{
    std::unique_lock<std::mutex> lk(sslMutex_);
    SSL* ssl = ssl_.load(std::memory_order_relaxed);
    return ssl != nullptr ? SSL_pending(ssl) : 0;
}
#endif // defined(ENABLE_TLS_SUPPORT)

constexpr int READ_SIZE_BYTES = 1024;

void TcpConnectionHandler::receiveImpl_()
{
    char buf[READ_SIZE_BYTES];

    SetThreadName("TCPRx");

    while (socket_.load(std::memory_order_relaxed) != INVALID_SOCKET)
    {
        // If the receive buffer can't take another read, stop reading until
        // the handler has caught up. The data waits in the socket and TCP
        // flow control slows the sender down, rather than it being dropped.
        // (Everything already in the buffer has a dispatch task queued.)
        if (receiveBuffer_.numFree() < READ_SIZE_BYTES)
        {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        int rv = 1;
#if defined(ENABLE_TLS_SUPPORT)
        // Data TLS has already decrypted doesn't make the socket readable, so
        // read it without waiting (left over when the buffer filled up).
        if (!usingTLS_ || sslPending_() == 0)
#endif // defined(ENABLE_TLS_SUPPORT)
        {
            struct timeval tv = {0, 250000}; // 250ms
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(socket_.load(std::memory_order_relaxed), &readSet);

            rv = select(socket_.load(std::memory_order_relaxed) + 1, &readSet, nullptr, nullptr, &tv);
        }
        if (rv > 0 && socket_.load(std::memory_order_relaxed) != INVALID_SOCKET)
        {
            int numRead = 0;
            int numHaveRead = 0;
            bool bufferFull = false;
#if defined(ENABLE_TLS_SUPPORT)
            while(!usingTLS_ || ssl_.load(std::memory_order_relaxed) != nullptr)
#else
            while(true)
#endif // defined(ENABLE_TLS_SUPPORT)
            {
                if (receiveBuffer_.numFree() < READ_SIZE_BYTES)
                {
                    // Dispatch what we have; the top of the loop waits for room.
                    bufferFull = true;
                    break;
                }

#if defined(ENABLE_TLS_SUPPORT)
                if (usingTLS_ && ssl_.load(std::memory_order_relaxed) != nullptr)
                {
                    int sslErr = SSL_ERROR_NONE;
                    numRead = sslRead_(buf, READ_SIZE_BYTES, sslErr);
                    if (numRead < 0 && ssl_.load(std::memory_order_relaxed))
                    {
                        fd_set writeSet;
                        FD_ZERO(&writeSet);
                        if (sslErr == SSL_ERROR_WANT_READ)
                        {
                            // This case can be handled by the top-level select()
                            // loop. Not an error. If we already read data on this
                            // pass (e.g. it was an exact multiple of READ_SIZE_BYTES),
                            // dispatch it now rather than waiting for more to arrive.
                            if (numHaveRead > 0)
                            {
                                break;
                            }
                            goto tryAgain;
                        }
                        else if (sslErr == SSL_ERROR_WANT_WRITE && socket_.load(std::memory_order_relaxed) != INVALID_SOCKET)
                        {
                            // Wait until we're able to continue writing. Wait in
                            // slices so that a disconnect (which clears ssl_ and
                            // then joins this thread) isn't stuck behind a peer
                            // that has stopped reading.
                            auto rawSock = socket_.load(std::memory_order_relaxed);
                            FD_SET(rawSock, &writeSet);

                            struct timeval tv = {0, SELECT_SLICE_US};
                            select(rawSock + 1, nullptr, &writeSet, nullptr, &tv);
                            continue;
                        }
                        else
                        {
                            auto errStr = GetSSLError_();
                            log_error("Unable to read from TLS connection: %s", errStr.c_str());
                            break;
                        }
                    }
                }
                else
#endif // defined(ENABLE_TLS_SUPPORT)
                {
#if defined(WIN32)
                    numRead = recv(socket_.load(std::memory_order_relaxed), buf, READ_SIZE_BYTES, 0);
#else
                    numRead = read(socket_.load(std::memory_order_relaxed), buf, READ_SIZE_BYTES);
#endif // defined(WIN32)
                }

                if (numRead <= 0)
                {
                    break;
                }

                // Queue RX handler. This thread is the buffer's only writer
                // and we checked for room above, so this can't fail.
                numHaveRead += numRead;
                if (receiveBuffer_.write(buf, numRead) != 0)
                {
                    log_error("receive buffer overflow; dropped %d bytes", numRead);
                }

                if (numRead < READ_SIZE_BYTES)
                {
                    break;
                }
            }
            if (numHaveRead > 0 && socket_.load(std::memory_order_relaxed) != INVALID_SOCKET)
            {
                enqueue_([this]() {
                    char tmp[READ_SIZE_BYTES];
                    int toRead = std::min(receiveBuffer_.numUsed(), READ_SIZE_BYTES);
                    while (socket_.load(std::memory_order_relaxed) != INVALID_SOCKET && toRead > 0)
                    {
                        receiveBuffer_.read(tmp, toRead);
                        if (socket_.load(std::memory_order_relaxed) != INVALID_SOCKET) onReceive_(tmp, toRead);
                        toRead = std::min(receiveBuffer_.numUsed(), READ_SIZE_BYTES);
                    }
                    if (socket_.load(std::memory_order_relaxed) != INVALID_SOCKET && onRecvEndFn_)
                    {
                        onRecvEndFn_();
                    }
                });
            } 
            else if (bufferFull)
            {
                // Nothing read this pass; wait for room at the top of the loop.
            }
            else if (numRead == 0 && socket_.load(std::memory_order_relaxed) != INVALID_SOCKET)
            {
                log_warn("EOF received");
                enqueue_([this]() {
                    disconnectImpl_();
                });

                // Nothing more to read. Stop here rather than spinning (and
                // queueing more disconnects) until the disconnect runs.
                break;
            }
            else if (numRead < 0 && socket_.load(std::memory_order_relaxed) != INVALID_SOCKET)
            {
#if defined(WIN32)
                log_warn("read failed (errno=%d)", WSAGetLastError());
#else
                log_warn("read failed (errno=%d)", errno);
#endif // defined(WIN32)
                enqueue_([this]() {
                    disconnectImpl_();
                });
                break; // as above
            }
        }
        else if (rv < 0 && socket_.load(std::memory_order_relaxed) != INVALID_SOCKET)
        {
#if defined(WIN32)
            log_warn("read failed (errno=%d)", WSAGetLastError());
#else
            log_warn("read failed (errno=%d)", errno);
#endif // defined(WIN32)
            enqueue_([this]() {
                disconnectImpl_();
            });
            break; // as above
        }
        continue;
tryAgain:
        assert(usingTLS_); // silencing warning
    }
}

#if defined(WIN32)
void TcpConnectionHandler::checkConnections_(std::vector<SOCKET>& sockets)
#else
void TcpConnectionHandler::checkConnections_(std::vector<int>& sockets)
#endif // defined(WIN32)
{
    fd_set writeSet;
    // Windows reports a failed connect (e.g. refused) in the exception set
    // rather than the write set; elsewhere the socket becomes writable.
    fd_set exceptSet;
    struct timeval tv = {0, 250000}; // 250ms
    int err = 0;

    FD_ZERO(&writeSet);
    FD_ZERO(&exceptSet);
#if defined(WIN32)
    SOCKET maxSocket = INVALID_SOCKET;
#else
    int maxSocket = INVALID_SOCKET;
#endif // defined(WIN32)
    for (auto& sock : sockets)
    {
        FD_SET(sock, &writeSet);
#if defined(WIN32)
        FD_SET(sock, &exceptSet);
#endif // defined(WIN32)
        if (sock > maxSocket) maxSocket = sock;
    }
    
    if (select(maxSocket + 1, nullptr, &writeSet, &exceptSet, &tv) > 0)
    {
        int sockErrCode = 0;
        socklen_t resultLength = sizeof(sockErrCode);
        
        std::vector<int> socketsToDelete;
        for (auto& sock : sockets)
        {
            if (FD_ISSET(sock, &writeSet) || FD_ISSET(sock, &exceptSet))
            {
#if defined(WIN32)
                auto sockOptError = getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&sockErrCode, &resultLength);
                if (sockOptError != 0)
                {
                    err = WSAGetLastError();
                    goto socket_error;
                }
                else if (sockErrCode != 0)
                {
                    err = sockErrCode;
                    goto socket_error;
                }
#else
                auto sockOptError = getsockopt(sock, SOL_SOCKET, SO_ERROR, &sockErrCode, &resultLength);
                if (sockOptError < 0)
                {
                    err = errno;
                    goto socket_error;
                }
                else if (sockErrCode != 0 && sockErrCode != EINPROGRESS)
                {
                    err = sockErrCode;
                    goto socket_error;
                }
#endif // defined(WIN32)
                else if (sockErrCode == 0)
                {
                    // Connection succeeded.
                    socket_.store(sock, std::memory_order_relaxed);
                    break;
                }
                
socket_error:
                constexpr int ERROR_BUFFER_LEN = 1024;
                char tmpBuf[ERROR_BUFFER_LEN];
#if defined(WIN32)
                strerror_s(tmpBuf, ERROR_BUFFER_LEN,  err);
                log_warn("Got socket error %d (%s) while connecting", err, tmpBuf);
                closesocket(sock);
#else
#if defined(__APPLE__) || ((_POSIX_C_SOURCE >= 200112L) && !_GNU_SOURCE)
                auto rv = strerror_r(err, tmpBuf, ERROR_BUFFER_LEN);
                if (rv != 0)
                {
                    strncpy(tmpBuf, "(null)", 7);
                }
                log_warn("Got socket error %d (%s) while connecting", err, tmpBuf);
#else
                auto ptr = strerror_r(err, tmpBuf, ERROR_BUFFER_LEN);
                if (ptr == nullptr)
                {
                    strncpy(tmpBuf, "(null)", 7);
                }
                else
                {
                    memmove(tmpBuf, ptr, strlen(ptr) + 1);
                }
                log_warn("Got socket error %d (%s) while connecting", err, tmpBuf);
#endif // defined(__APPLE__) || ((_POSIX_C_SOURCE >= 200112L) && !_GNU_SOURCE)
                close(sock);
#endif // defined(WIN32)
                socketsToDelete.push_back(sock);
            }
        }
        
        for (auto& toDelete : socketsToDelete)
        {
            sockets.erase(std::find(sockets.begin(), sockets.end(), toDelete));
        }
    }
}

void TcpConnectionHandler::resolveAddresses_(int addressFamily, const char* host, const char* port, struct addrinfo** result)
{
    log_info("attempting to resolve %s:%s using family %d", host, port, addressFamily);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = addressFamily;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
#ifdef WIN32
    hints.ai_flags = AI_ADDRCONFIG;
#else
    hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV;
#endif // WIN32
    int err = getaddrinfo(host, port, &hints, result);
    if (err != 0) 
    {
        log_warn("cannot resolve %s:%s using family %d (err=%d)", host, port, addressFamily, err);
    }
    
    log_info("resolution of %s:%s using family %d complete", host, port, addressFamily);
}
