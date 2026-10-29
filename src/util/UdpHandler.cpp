//=========================================================================
// Name:            UdpHandler.cpp
// Purpose:         Handler for UDP sockets.
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

#if defined(WIN32) || defined(__MINGW32__)

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif // !_WIN32_WINNT

#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2def.h>
#include <iphlpapi.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#endif // defined(WIN32) || defined(__MINGW32__)

#include "UdpHandler.h"
#include "logging/ulog.h"
#include "../os/os_interface.h"

#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif // INVALID_SOCKET

UdpHandler::UdpHandler()
    : ThreadedObject("UdpHandler")
    , socket_(-1)
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

UdpHandler::~UdpHandler()
{
    auto fut = close();
    fut.wait();

#if defined(WIN32)
    WSACleanup();
#endif // defined(WIN32)
}

std::future<void> UdpHandler::open(const char* host, int port, const char* sendIp, int sendPort)
{
    host_ = host;
    port_ = port;
    
    std::shared_ptr<std::promise<void>> prom = std::make_shared<std::promise<void> >();
    auto fut = prom->get_future();
    
    enqueue_([&, prom, sendIp, sendPort]() {
        openImpl_(sendIp, sendPort);
        prom->set_value();
    });
    return fut;
}

std::future<void> UdpHandler::close()
{
    std::shared_ptr<std::promise<void>> prom = std::make_shared<std::promise<void> >();
    auto fut = prom->get_future();
    
    enqueue_([&, prom]() {
        closeImpl_();
        prom->set_value();
    });
    return fut;
}

std::future<void> UdpHandler::send(const char* host, int port, const char* buf, int length)
{
    std::shared_ptr<std::promise<void>> prom = std::make_shared<std::promise<void> >();
    auto fut = prom->get_future();
    
    enqueue_([&, prom, host, port, buf, length]() {
        sendImpl_(host, port, buf, length);
        prom->set_value();
    });
    return fut;
}

void UdpHandler::openImpl_(const char* sendIp, int sendPort)
{
    std::stringstream portStream;
    portStream << port_;
    
    log_info("Opening UDP socket (host %s port %d)", host_.c_str(), port_);
    
    // If empty hostname, see if we can infer the socket type from the provided
    // example send port. If not, default to INADDR_ANY.
    if (host_ == "")
    {
        auto addressType = AF_INET;
        struct addrinfo* result = nullptr;
        if (sendIp != nullptr)
        {
            result = resolveIpAddress_(sendIp, sendPort);
            if (result != nullptr)
            {
                addressType = result->ai_family;

            }
        }

#if defined(WIN32)
        socket_.store(socket(addressType, SOCK_DGRAM, IPPROTO_UDP), std::memory_order_relaxed);
        if (socket_.load(std::memory_order_relaxed) == INVALID_SOCKET)
        {
            log_warn("cannot open socket (err=%d)", WSAGetLastError());
        }
#else
        socket_.store(socket(addressType, SOCK_DGRAM, IPPROTO_UDP), std::memory_order_relaxed);
        if(socket_.load(std::memory_order_relaxed) < 0)
        {
            log_warn("cannot open socket (err=%d)", errno);
        }
#endif // defined(WIN32)
        if (socket_.load(std::memory_order_relaxed) == INVALID_SOCKET)
        {
            // Nothing to join a multicast group with.
            if (result != nullptr)
            {
                freeaddrinfo(result);
            }
            return;
        }

        // XXX (Windows): if the send address is multicast, we need to join
        // the associated multicast group in order to not get "unreachable" errors.
        // This theoretically shouldn't be necessary per Microsoft's own documentation
        // but apparently Windows will use localhost for the multicast interface otherwise,
        // causing the issue.
        // See https://github.com/python/cpython/issues/102590 for details.
        if (result != nullptr)
        {
            if (isMulticastAddress_(result))
            {
                joinMulticastGroup_(result);
            }
            freeaddrinfo(result);
        }
    }
    else
    {
        struct addrinfo* result = resolveIpAddress_(host_.c_str(), port_);

        if (result != nullptr)
        {
#if defined(WIN32)
            socket_.store(socket(result->ai_family, result->ai_socktype, result->ai_protocol), std::memory_order_relaxed);
            if (socket_.load(std::memory_order_relaxed) == INVALID_SOCKET)
            {
                log_warn("cannot open socket (err=%d)", WSAGetLastError());
            }
#else
            socket_.store(socket(result->ai_family, result->ai_socktype, result->ai_protocol), std::memory_order_relaxed);
            if(socket_.load(std::memory_order_relaxed) < 0)
            {
                log_warn("cannot open socket (err=%d)", errno);
            }
#endif // defined(WIN32)
            if (socket_.load(std::memory_order_relaxed) == INVALID_SOCKET)
            {
                // Nothing to bind, receive on or join a multicast group with.
                freeaddrinfo(result);
                return;
            }

            auto err = ::bind(socket_.load(std::memory_order_relaxed), result->ai_addr, result->ai_addrlen);
            if (err != 0)
            {
#if defined(WIN32)
                log_warn("cannot bind socket (err=%d)", WSAGetLastError());
#else
                log_warn("cannot bind socket (err=%d)", errno);
#endif // defined(WIN32)
            }
            else
            {                
                // Start receive thread
                receiveThread_ = std::thread(std::bind(&UdpHandler::receiveImpl_, this));
            }

            // Join multicast group.
            if (isMulticastAddress_(result))
            {
                joinMulticastGroup_(result);
            }
            
            freeaddrinfo(result);           /* No longer needed */
        }
    }
}

void UdpHandler::closeImpl_()
{
    auto tmp = socket_.load(std::memory_order_relaxed);
    if (tmp != INVALID_SOCKET)
    {
        socket_.store(INVALID_SOCKET, std::memory_order_relaxed);

#if defined(WIN32)
        closesocket(tmp);
#else
        ::close(tmp);
#endif // defined(WIN32)

        if (receiveThread_.joinable())
        {
            receiveThread_.join();
        }
    }
}

void UdpHandler::sendImpl_(const char* host, int port, const char* buf, int length)
{
    struct addrinfo* result = resolveIpAddress_(host, port);
    if (result != nullptr)
    {
        auto rv = sendto(socket_.load(std::memory_order_relaxed), buf, length, 0, result->ai_addr, result->ai_addrlen);
        if (rv < 0)
        {
#if defined(WIN32)
            log_warn("cannot send to host %s port %d (err=%d)", host, port, WSAGetLastError());
#else
            log_warn("cannot send to host %s port %d (err=%d)", host, port, errno);
#endif // defined(WIN32)
        }
        
        freeaddrinfo(result);
    }
}

void UdpHandler::receiveImpl_()
{
    // TBD - not handling RX for now. This class is mainly for WSJT-X style logging, so we 
    // shouldn't *need* to handle anything being received
}

struct addrinfo* UdpHandler::resolveIpAddress_(const char* host, int port)
{
    log_info("Attempting DNS resolution of %s:%d", host, port);
    
    std::stringstream portStream;
    portStream << port;
    
    struct addrinfo* result = nullptr;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
#ifdef WIN32
    hints.ai_flags = AI_NUMERICHOST;
#else
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
#endif // WIN32
    int err = getaddrinfo(host, portStream.str().c_str(), &hints, &result);
    if (err != 0) 
    {
        log_warn("cannot resolve %s:%s (err=%d)", host, portStream.str().c_str(), err);
    }
    
    return result;
}

bool UdpHandler::isMulticastAddress_(struct addrinfo* addr)
{
    if (addr->ai_family == AF_INET6)
    {
        auto ipv6Addr = (struct sockaddr_in6*)(addr->ai_addr);
        return ipv6Addr->sin6_addr.s6_addr[0] == 0xFF; // ff00::/8
    }
    else  // resolveIpAddress_ hints AF_INET/AF_INET6 only
    {
        auto ipv4Addr = (struct sockaddr_in*)(addr->ai_addr);
        auto addrAsHost = ntohl(ipv4Addr->sin_addr.s_addr);
        return addrAsHost >= 0xE0000000 && addrAsHost <= 0xEFFFFFFF; // 224.0.0.0-239.255.255.255            
    }
}

void UdpHandler::joinMulticastGroup_(struct addrinfo* addr)
{
    log_info("Joining multicast group");

    if (addr->ai_family == AF_INET6)
    {
        struct ipv6_mreq multicastRequest;  /* Multicast address join structure */

        /* Specify the multicast group */
        memcpy(&multicastRequest.ipv6mr_multiaddr,
                &((struct sockaddr_in6*)(addr->ai_addr))->sin6_addr,
                sizeof(multicastRequest.ipv6mr_multiaddr));

        /* Accept multicast from any interface */
        multicastRequest.ipv6mr_interface = 0;

        /* Join the multicast address */
        if (setsockopt(socket_.load(std::memory_order_relaxed), IPPROTO_IPV6, IPV6_JOIN_GROUP, (char*) &multicastRequest, sizeof(multicastRequest)) != 0) 
        {
#if defined(WIN32)
            log_warn("Cannot join multicast group (err=%d)", WSAGetLastError());
#else
            log_warn("Cannot join multicast group (err=%d)", errno);
#endif // defined(WIN32)
            }
    }
    else if (addr->ai_family == AF_INET)
    {
        struct ip_mreq multicastRequest;  /* Multicast address join structure */

        /* Specify the multicast group */
        memcpy(&multicastRequest.imr_multiaddr,
                &((struct sockaddr_in*)(addr->ai_addr))->sin_addr,
                sizeof(multicastRequest.imr_multiaddr));

        /* Accept multicast from any interface */
            multicastRequest.imr_interface.s_addr = htonl(INADDR_ANY);

#if defined(WIN32)
        // XXX (Windows): INADDR_ANY is insufficient to have IP_ADD_MEMBERSHIP do the right
        // thing (join multicast group on user's local LAN). We need to manually retrieve
        // the correct interface address to use and populate it here if possible.
        // Note: This is only needed for IPv4; IPv6 works properly using the same logic as
        // other supported platforms.
        //
        // Source: https://github.com/ntop/n2n/pull/576
        DWORD ifIndex = 0;
        auto rv = GetBestInterface(*(IPAddr*)addr->ai_addr, &ifIndex);
        if (rv != NO_ERROR)
        {
            log_warn("Could not get best interface for multicast address (rv=%d)", (int)rv);
        }
        else
        {
            IP_ADAPTER_INFO* ifInfo = (IP_ADAPTER_INFO*)malloc(sizeof(IP_ADAPTER_INFO));
            assert(ifInfo != nullptr);
            ULONG ifInfoLen = 0;

            rv = GetAdaptersInfo(ifInfo, &ifInfoLen);
            if (rv == ERROR_BUFFER_OVERFLOW)
            {
                ifInfo = (IP_ADAPTER_INFO*)realloc(ifInfo, ifInfoLen);
                assert(ifInfo != nullptr);
            }

            rv = GetAdaptersInfo(ifInfo, &ifInfoLen);
            if (rv != NO_ERROR)
            {
                log_warn("Could not get list of network interfaces (rv=%d)", (int)rv);
            }
            else
            {
                auto ptr = ifInfo;
                while (ptr != nullptr)
                {
                    if (ptr->Index == ifIndex)
                    {
                        log_info("Found best interface at address %s", ptr->IpAddressList.IpAddress.String);
                        auto ifAddr = inet_addr(ptr->IpAddressList.IpAddress.String);
                        multicastRequest.imr_interface.s_addr = ifAddr;

                        // We also need to specify IP_MULTICAST_IF too.
                        if (setsockopt(socket_.load(std::memory_order_relaxed), IPPROTO_IP, IP_MULTICAST_IF, (char*) &ifAddr, sizeof(ifAddr)) != 0) 
                        {
                            log_warn("Cannot join multicast group (err=%d)", WSAGetLastError());
                        }
                        break;
                    }
                    
                    ptr = ptr->Next;
                }
            }

            free(ifInfo);
        }
#endif // defined(WIN32)

        /* Join the multicast address */
        if (setsockopt(socket_.load(std::memory_order_relaxed), IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*) &multicastRequest, sizeof(multicastRequest)) != 0) 
        {
#if defined(WIN32)
            log_warn("Cannot join multicast group (err=%d)", WSAGetLastError());
#else
            log_warn("Cannot join multicast group (err=%d)", errno);
#endif // defined(WIN32)
        }
    }
    else
    {
        assert(0); // should not reach here
    }
}
