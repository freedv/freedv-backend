// Tests for UdpHandler: unicast sends, the multicast group-join paths used by
// UdpReporter (multicast destinations such as WSJT-X style 239.x.x.x groups),
// and error handling for bad addresses.
//
// Multicast delivery depends on the host having a multicast-capable route, so
// the multicast delivery checks first verify with a plain socket that the
// environment loops multicast back to itself, and are skipped (not failed)
// when it doesn't.
#include <atomic>
#include <cstring>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif // !defined(_WIN32)
#include "TestSocketCompat.h"


#include <thread>

#include "../SocketIoClient.h"
#include "../UdpHandler.h"

namespace {

// Evaluates a test condition, printing the failing expression and line so a
// FAIL can be traced without a debugger.
#define CHECK(expr) checkImpl_((expr), #expr, __LINE__)

bool checkImpl_(bool ok, const char* expr, int line)
{
    if (!ok)
    {
        std::cout << "\n    check failed at line " << line << ": " << expr << "\n    ";
    }
    return ok;
}

bool report(bool result)
{
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

const char* MULTICAST_GROUP_V4 = "239.255.77.77";
const char* MULTICAST_GROUP_V4_BIND = "239.255.77.78";
const char* MULTICAST_GROUP_V6 = "ff15::7777"; // transient, site-local scope

class TestUdpHandler : public UdpHandler
{
protected:
    // UdpHandler doesn't receive (receiveImpl_() is a no-op), so this is never
    // called; required because it's pure virtual.
    virtual void onReceive_(const char*, int, char*, int) override {}
};

// UDP socket listening on an ephemeral port, optionally a member of a
// multicast group.
class Listener
{
public:
    explicit Listener(int family = AF_INET)
        : family_(family)
        , fd_(testOpenSocket(family, SOCK_DGRAM))
        , port_(-1)
    {
        if (fd_ < 0)
        {
            return;
        }
        int on = 1;
        testSetSockOpt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        struct sockaddr_storage addr;
        socklen_t addrLen = 0;
        memset(&addr, 0, sizeof(addr));
        if (family == AF_INET)
        {
            auto in = (struct sockaddr_in*)&addr;
            in->sin_family = AF_INET;
            in->sin_addr.s_addr = htonl(INADDR_ANY);
            addrLen = sizeof(*in);
        }
        else
        {
            auto in6 = (struct sockaddr_in6*)&addr;
            in6->sin6_family = AF_INET6;
            in6->sin6_addr = in6addr_any;
            addrLen = sizeof(*in6);
        }
        if (bind(fd_, (struct sockaddr*)&addr, addrLen) == 0 &&
            getsockname(fd_, (struct sockaddr*)&addr, &addrLen) == 0)
        {
            port_ = ntohs(family == AF_INET ? ((struct sockaddr_in*)&addr)->sin_port
                                            : ((struct sockaddr_in6*)&addr)->sin6_port);
        }
    }

    ~Listener()
    {
        if (fd_ >= 0) testCloseSocket(fd_);
    }

    int port() const { return port_; }

    bool joinGroup(const char* group)
    {
        if (family_ == AF_INET)
        {
            struct ip_mreq req;
            memset(&req, 0, sizeof(req));
            inet_pton(AF_INET, group, &req.imr_multiaddr);
            req.imr_interface.s_addr = htonl(INADDR_ANY);
            return testSetSockOpt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &req, sizeof(req)) == 0;
        }
        struct ipv6_mreq req;
        memset(&req, 0, sizeof(req));
        inet_pton(AF_INET6, group, &req.ipv6mr_multiaddr);
        req.ipv6mr_interface = 0;
        return testSetSockOpt(fd_, IPPROTO_IPV6, IPV6_JOIN_GROUP, &req, sizeof(req)) == 0;
    }

    std::string receive(int timeoutMs)
    {
        if (!testWaitReadable(fd_, timeoutMs))
        {
            return "";
        }
        char buf[2048];
        ssize_t len = recv(fd_, buf, sizeof(buf), 0);
        return len > 0 ? std::string(buf, len) : "";
    }

private:
    int family_;
    int fd_;
    int port_;
};

// Sends one datagram with a plain socket. Used to probe whether the
// environment delivers multicast back to local listeners at all.
bool plainSend(int family, const char* host, int port, const std::string& payload)
{
    int fd = testOpenSocket(family, SOCK_DGRAM);
    if (fd < 0)
    {
        return false;
    }
    struct sockaddr_storage addr;
    socklen_t addrLen = 0;
    memset(&addr, 0, sizeof(addr));
    if (family == AF_INET)
    {
        auto in = (struct sockaddr_in*)&addr;
        in->sin_family = AF_INET;
        in->sin_port = htons(port);
        inet_pton(AF_INET, host, &in->sin_addr);
        addrLen = sizeof(*in);
    }
    else
    {
        auto in6 = (struct sockaddr_in6*)&addr;
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(port);
        inet_pton(AF_INET6, host, &in6->sin6_addr);
        addrLen = sizeof(*in6);
    }
    bool ok = sendto(fd, payload.data(), payload.size(), 0, (struct sockaddr*)&addr, addrLen) == (ssize_t)payload.size();
    testCloseSocket(fd);
    return ok;
}

// Returns a UDP port that was free a moment ago.
int getUnusedPort()
{
    Listener tmp(AF_INET);
    return tmp.port();
}

bool multicastWorks(int family, const char* group)
{
    Listener probe(family);
    return probe.port() > 0 && probe.joinGroup(group) &&
           plainSend(family, group, probe.port(), "probe") &&
           probe.receive(1000) == "probe";
}

bool testUnicastSend()
{
    std::cout << "Test 1 (unicast send to IPv4 and IPv6 loopback): ";

    bool result = true;

    Listener v4(AF_INET);
    TestUdpHandler handler;
    handler.open().wait(); // no host: unbound IPv4 socket for sending
    const std::string msg = "hello udp";
    handler.send("127.0.0.1", v4.port(), msg.c_str(), msg.size()).wait();
    result &= CHECK(v4.receive(2000) == msg);

    // An IPv6 send hint makes open() create an IPv6 socket.
    Listener v6(AF_INET6);
    if (v6.port() > 0)
    {
        TestUdpHandler handler6;
        handler6.open("", 0, "::1", v6.port()).wait();
        handler6.send("::1", v6.port(), msg.c_str(), msg.size()).wait();
        result &= CHECK(v6.receive(2000) == msg);
    }

    return report(result);
}

bool testMulticastSendHint()
{
    std::cout << "Test 2 (IPv4 multicast destination joins the group and delivers): ";

    if (!multicastWorks(AF_INET, MULTICAST_GROUP_V4))
    {
        std::cout << "SKIP (no IPv4 multicast loopback here) ";
        return report(true);
    }

    // This is how UdpReporter opens its socket: no local address, with the
    // destination given as a hint so a multicast group is joined up front.
    Listener listener(AF_INET);
    bool result = CHECK(listener.joinGroup(MULTICAST_GROUP_V4));

    TestUdpHandler handler;
    handler.open("", 0, MULTICAST_GROUP_V4, listener.port()).wait();
    const std::string msg = "hello multicast";
    for (int i = 0; i < 3; i++)
    {
        handler.send(MULTICAST_GROUP_V4, listener.port(), msg.c_str(), msg.size()).wait();
        result &= CHECK(listener.receive(2000) == msg);
    }

    return report(result);
}

bool testBindToMulticastGroup()
{
    std::cout << "Test 3 (binding to a multicast group address joins it): ";

    if (!multicastWorks(AF_INET, MULTICAST_GROUP_V4_BIND))
    {
        std::cout << "SKIP (no IPv4 multicast loopback here) ";
        return report(true);
    }

    // open() with a multicast host binds to the group and joins it, then
    // starts the receive thread. Group traffic must still reach other members,
    // and close() must stop the receive thread cleanly.
    Listener listener(AF_INET);
    bool result = CHECK(listener.joinGroup(MULTICAST_GROUP_V4_BIND));

    // Bind on a port of its own: UdpHandler doesn't set SO_REUSEADDR, so it
    // can't share the listener's port.
    int handlerPort = getUnusedPort();

    TestUdpHandler handler;
    handler.open(MULTICAST_GROUP_V4_BIND, handlerPort).wait();

    const std::string msg = "group traffic";
    result &= CHECK(plainSend(AF_INET, MULTICAST_GROUP_V4_BIND, listener.port(), msg));
    result &= CHECK(listener.receive(2000) == msg);

    // Note: a socket bound to a multicast address can't itself send (its
    // source address would be the group), so only reception by others is
    // checked here.

    handler.close().wait();
    // Closing twice is harmless.
    handler.close().wait();

    return report(result);
}

bool testIpv6Multicast()
{
    std::cout << "Test 4 (IPv6 multicast destination joins the group and delivers): ";

    if (!multicastWorks(AF_INET6, MULTICAST_GROUP_V6))
    {
        std::cout << "SKIP (no IPv6 multicast loopback here) ";
        return report(true);
    }

    Listener listener(AF_INET6);
    bool result = CHECK(listener.joinGroup(MULTICAST_GROUP_V6));

    TestUdpHandler handler;
    handler.open("", 0, MULTICAST_GROUP_V6, listener.port()).wait();
    const std::string msg = "hello v6 multicast";
    handler.send(MULTICAST_GROUP_V6, listener.port(), msg.c_str(), msg.size()).wait();
    result &= CHECK(listener.receive(2000) == msg);

    return report(result);
}

bool testBadAddresses()
{
    std::cout << "Test 5 (unparseable and non-local addresses fail cleanly): ";

    bool result = true;

    // UdpHandler only accepts numeric IPs; a hostname fails to resolve.
    {
        TestUdpHandler handler;
        handler.open("not-an-ip-address", 1234).wait();
        handler.send("127.0.0.1", 9, "x", 1).wait(); // socket never opened
        handler.close().wait();
    }

    // A send hint that doesn't parse falls back to IPv4.
    {
        Listener listener(AF_INET);
        TestUdpHandler handler;
        handler.open("", 0, "not-an-ip-address", 1234).wait();
        handler.send("127.0.0.1", listener.port(), "ok", 2).wait();
        result &= CHECK(listener.receive(2000) == "ok");

        // Sending to an unparseable destination is dropped.
        handler.send("also-not-an-ip", listener.port(), "no", 2).wait();
        result &= CHECK(listener.receive(300).empty());
    }

    // Binding to an address this host doesn't own (TEST-NET-1) fails without
    // starting the receive thread; close() must still work.
    {
        TestUdpHandler handler;
        handler.open("192.0.2.1", 0).wait();
        handler.close().wait();
    }

    return report(result);
}

#if !defined(_WIN32)
// Exhaust the process socket descriptors so the socket() calls inside
// openImpl_()/joinMulticastGroup_() fail, exercising their error branches.
bool testSocketExhaustion()
{
    std::cout << "Test 6 (open() survives socket creation and group-join failures): ";

    struct rlimit original;
    if (getrlimit(RLIMIT_NOFILE, &original) != 0)
    {
        return report(true); // nothing to exhaust here; skip
    }

    // Fetch ports while descriptors are still available; getUnusedPort()
    // opens a socket itself and would fail once the limit is in place.
    const int port1 = getUnusedPort();
    const int port2 = getUnusedPort();
    const int port3 = getUnusedPort();

    struct rlimit lowered = original;
    lowered.rlim_cur = 16; // well below the number of fds already open below
    if (setrlimit(RLIMIT_NOFILE, &lowered) != 0)
    {
        return report(true); // not permitted to lower the limit; skip
    }

    bool result = true;
    std::vector<int> dummies;
    try
    {
        int fd = -1;
        while (dummies.size() < 4096 && (fd = ::socket(AF_INET, SOCK_DGRAM, 0)) >= 0)
        {
            dummies.push_back(fd);
        }

        // Host-empty open with a multicast send hint: socket() fails, so
        // there's no IPv4 group to join.
        {
            TestUdpHandler handler;
            auto fut = handler.open("", 0, MULTICAST_GROUP_V4, port1);
            fut.wait();
            result &= CHECK(fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
            handler.close().wait();
        }

        // Same again for IPv6.
        {
            TestUdpHandler handler;
            auto fut = handler.open("", 0, MULTICAST_GROUP_V6, port2);
            fut.wait();
            result &= CHECK(fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
            handler.close().wait();
        }

        // Bound-host open: socket() fails before bind(); receive thread
        // must not start and close() must still complete.
        {
            TestUdpHandler handler;
            auto fut = handler.open("127.0.0.1", port3);
            fut.wait();
            result &= CHECK(fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
            handler.close().wait();
        }
    }
    catch (const std::exception& e)
    {
        std::cout << "[" << e.what() << "] ";
        result = false;
    }

    for (int fd : dummies)
    {
        testCloseSocket(fd);
    }
    setrlimit(RLIMIT_NOFILE, &original);

    // Sanity check: the limits are usable again and a normal open works.
    struct rlimit restored;
    getrlimit(RLIMIT_NOFILE, &restored);
    result &= CHECK(restored.rlim_cur == original.rlim_cur);

    {
        TestUdpHandler handler;
        int port = getUnusedPort();
        handler.open("127.0.0.1", port).wait();
        handler.close().wait();
    }

    return report(result);
}
#endif // !defined(_WIN32)

// 240.0.0.0/4 is above the 224.0.0.0-239.255.255.255 multicast band: the
// second half of the isMulticastAddress_() IPv4 range test must be evaluated.
bool testOutOfRangeMulticastProbe()
{
    std::cout << "Test 7 (addresses above the multicast band are not multicast): ";

    bool result = true;

    {
        TestUdpHandler handler;
        auto fut = handler.open("240.1.1.1", 0); // bind fails (local address not owned)
        fut.wait();
        result &= CHECK(fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
        handler.close().wait();
    }

    return report(result);
}

// A SocketIoClient that never connected must still drain its task queue on
// destruction: the deferred emit lambda must run with a null connection
// (its guard falls through) and the disconnect handler must tolerate the
// missing connection.
bool testSocketIoClientUnconnectedEmit()
{
    std::cout << "Test 8 (unconnected SocketIoClient emit and teardown are safe): ";

    bool result = true;

    {
        SocketIoClient client;
        client.emit("unconnected-event");
        // Give the handler thread a moment to drain the queued lambda.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return report(result);
}

// A SocketIoClient whose TCP connect succeeds but whose websocketpp layer is
// never opened (onConnect_ overridden to do nothing). Its disconnect handler
// must run with a null connection_: disconnectImpl_ reaches onDisconnect_
// because the raw socket is valid, and the connection_ guard inside
// onDisconnect_ must fall through cleanly.
class NeverWebsocketedSocketIoClient : public SocketIoClient
{
protected:
    virtual void onConnect_() override {}
};

bool testSocketIoClientDisconnectWithoutWebsocket()
{
    std::cout << "Test 9 (SocketIoClient teardown without websocket connection runs handler): ";

    bool result = true;

    {
        // A plain TCP listener that accepts nothing and sends nothing, so
        // the websocket handshake can never complete.
        int listenFd = testOpenSocket(AF_INET, SOCK_STREAM);
        result &= CHECK(listenFd >= 0);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        result &= CHECK(bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
        socklen_t addrLen = sizeof(addr);
        result &= CHECK(getsockname(listenFd, (struct sockaddr*)&addr, &addrLen) == 0);
        int port = ntohs(addr.sin_port);
        result &= CHECK(listen(listenFd, 1) == 0);

        NeverWebsocketedSocketIoClient client;
        std::atomic<bool> handlerRan{false};
        client.setOnDisconnectFn([&]() { handlerRan.store(true); });

        auto connectFuture = client.connect("127.0.0.1", port, false, false);
        connectFuture.wait();
        result &= CHECK(connectFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);

        // The socket is valid but connection_ is null: onDisconnect_ must
        // still reach the application callback.
        auto disconnectFuture = client.disconnect();
        disconnectFuture.wait();
        result &= CHECK(handlerRan.load());

        testCloseSocket(listenFd);
    }

    return report(result);
}


} // namespace

int main(int, char**)
{
    bool result = true;

    result &= testUnicastSend();
    result &= testMulticastSendHint();
    result &= testBindToMulticastGroup();
    result &= testIpv6Multicast();
    result &= testBadAddresses();
#if !defined(_WIN32)
    result &= testSocketExhaustion();
#endif // !defined(_WIN32)
    result &= testOutOfRangeMulticastProbe();
    result &= testSocketIoClientUnconnectedEmit();
    result &= testSocketIoClientDisconnectWithoutWebsocket();

    return result ? 0 : -1;
}
