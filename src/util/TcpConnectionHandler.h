//=========================================================================
// Name:            TcpConnectionHandler.h
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

#ifndef TCP_CONNECTION_HANDLER_H
#define TCP_CONNECTION_HANDLER_H

#include "ThreadedObject.h"
#include "ThreadedTimer.h"
#include "GenericFIFO.h"

#include <vector>
#include <future>
#include <atomic>
#include <mutex>

#if defined(ENABLE_TLS_SUPPORT)
#include <openssl/ssl.h>
#include <openssl/err.h> 
#endif // defined(ENABLE_TLS_SUPPORT)

class TcpConnectionHandler : public ThreadedObject
{
public:
    using OnRecvEndFn = std::function<void()>;

    TcpConnectionHandler();
    virtual ~TcpConnectionHandler();
    
    // Ignored (with a warning) if already connected; disconnect() first to
    // connect somewhere else.
    std::future<void> connect(const char* host, int port, bool enableReconnect, bool enableTLS = false);
    std::future<void> disconnect();
    
    std::future<void> send(const char* buf, int length);

    void setOnRecvEndFn(OnRecvEndFn fn);
    
protected:
    std::string host_;
    int port_;
    bool usingTLS_;
    // Reconnect-policy flag. Carries no payload, so relaxed ordering is enough;
    // it just needs to be read/written atomically.
    std::atomic<bool> enableReconnect_;
    
    virtual void onConnect_() = 0;
    virtual void onDisconnect_() = 0;
    virtual void onReceive_(char* buf, int length) = 0;
    
private:
    friend class TcpConnectionHandlerTest; // grants unit tests access to TLS state

    std::thread receiveThread_;
    ThreadedTimer reconnectTimer_;
    // socket_ / ssl_ / sslCtx_ are accessed with relaxed ordering. They are only
    // ever handed to receiveImpl_() via the receiveThread_ std::thread creation and
    // torn down after receiveThread_.join() (see disconnectImpl_()), so the thread
    // create/join edges provide all the required happens-before; the atomics just
    // guarantee non-torn reads and stop the compiler caching the value across
    // receiveImpl_()'s loop. connectImpl_()/sendImpl_()/disconnectImpl_() all run
    // on the same ThreadedObject worker thread (program order). This stays valid as
    // long as receiveThread_ is re-created per connection rather than long-lived.
#if defined(WIN32)
    std::atomic<SOCKET> socket_;
#else
    std::atomic<int> socket_;
#endif // defined(WIN32)
    // DNS-completion flags, polled in sleep loops. The resolved addrinfo is handed
    // over via std::future (its own synchronisation), so these carry no payload:
    // relaxed ordering.
    std::atomic<bool> ipv4Complete_;
    std::atomic<bool> ipv6Complete_;
    // Cancellation flag polled by connectImpl_(); no payload -> relaxed.
    std::atomic<bool> cancelConnect_;
    // Changes with every connection made and closed, so that an onConnect_()
    // queued for a connection that's closed before it runs can tell and not
    // run. Only used on the worker thread.
    uint64_t connectionGeneration_;
    GenericFIFO<char> receiveBuffer_;
    OnRecvEndFn onRecvEndFn_;
#if defined(ENABLE_TLS_SUPPORT)
    std::atomic<SSL_CTX*> sslCtx_;
    std::atomic<SSL*> ssl_;
    // Serializes SSL_read()/SSL_write() between receiveThread_ and the worker.
    std::mutex sslMutex_;

    int sslWrite_(const char* buf, int length, int& sslErr);
    int sslRead_(char* buf, int length, int& sslErr);
    int sslPending_(); // decrypted bytes SSL_read() can return without the socket
#endif // defined(ENABLE_TLS_SUPPORT)

    void connectImpl_();
    void disconnectImpl_(bool callHandler = true);
    void sendImpl_(const char* buf, int length);
    void receiveImpl_();
    
    // Virtual so unit tests can script DNS answers and timing. The result
    // is released with freeaddrinfo().
    virtual void resolveAddresses_(int addressFamily, const char* host, const char* port, struct addrinfo** result);
#if defined(WIN32)
    void checkConnections_(std::vector<SOCKET>& sockets);
#else
    void checkConnections_(std::vector<int>& sockets);
#endif // defined(WIN32)
};

#endif // TCP_CONNECTION_HANDLER_H
