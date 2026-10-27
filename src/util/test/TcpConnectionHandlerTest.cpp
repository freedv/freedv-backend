#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(WIN32)
#include <sys/resource.h>
#endif // !defined(WIN32)


#include "../TcpConnectionHandler.h"
#include "LoopbackTcpServer.h"

#if defined(ENABLE_TLS_SUPPORT)
#include <openssl/ssl.h>
#include "TestCertificate.h"
#endif // defined(ENABLE_TLS_SUPPORT)
// ---------------------------------------------------------------------------
// --wrap fault-injection shims (Linux-only; see src/util/CMakeLists.txt).
// Each gate is armed by a test just before the targeted action, fires exactly
// once, and otherwise the wrapper passes straight through to __real_X. The
// shims call __real_X first wherever the real result leaves state the handler
// still needs (a genuinely connected socket, filled sockaddr/name buffers),
// except where the point is that the library call fails. Diagnostics go to
// stderr only, never log_* (ulog itself would recurse into wrapped calls).
// ---------------------------------------------------------------------------
#if !defined(_WIN32) && !defined(__APPLE__)
#include <cstdarg>

namespace {
std::atomic<int> fcntlFailMode_{0}; // 1: fail F_GETFL once, 2: fail F_SETFL once
std::atomic<int> connectEinprogressOnce_{0};
std::atomic<int> getpeernameFailOnce_{0};
std::atomic<int> getnameinfoFailOnce_{0}; // armed by the getpeername shim itself
#if defined(ENABLE_TLS_SUPPORT)
std::atomic<int> sslCtxNewFailOnce_{0};
std::atomic<int> sslDefaultPathsWarnOnce_{0};
std::atomic<int> sslMinProtoWarnOnce_{0};
std::atomic<int> sslSetFdFailOnce_{0};
std::atomic<int> sslSet1HostWarnOnce_{0};
#endif // defined(ENABLE_TLS_SUPPORT)
} // namespace

extern "C" {

int __real_fcntl(int fd, int cmd, ...);
int __wrap_fcntl(int fd, int cmd, ...)
{
    va_list args;
    va_start(args, cmd);
    int thirdArg = va_arg(args, int);
    va_end(args);

    int mode = fcntlFailMode_.load(std::memory_order_relaxed);
    if (mode == 1 && cmd == F_GETFL)
    {
        fcntlFailMode_.store(0);
        errno = EPERM;
        fprintf(stderr, "[fault] fcntl(F_GETFL) -> -1\n");
        return -1;
    }
    if (mode == 2 && cmd == F_SETFL)
    {
        fcntlFailMode_.store(0);
        errno = EPERM;
        fprintf(stderr, "[fault] fcntl(F_SETFL) -> -1\n");
        return -1;
    }
    return __real_fcntl(fd, cmd, thirdArg);
}

int __real_connect(int fd, const struct sockaddr* addr, socklen_t len);
int __wrap_connect(int fd, const struct sockaddr* addr, socklen_t len)
{
    int rv = __real_connect(fd, addr, len); // always run the real connect first, whichever way it goes
    if (connectEinprogressOnce_.exchange(0)) // consume the gate either way: a loopback connect may
    {                                        // legitimately return 0 or EINPROGRESS under load
        errno = EINPROGRESS;
        fprintf(stderr, "[fault] connect -> -1/EINPROGRESS\n");
        return -1;
    }
    return rv;
}

int __real_getpeername(int fd, struct sockaddr* addr, socklen_t* len);
int __wrap_getpeername(int fd, struct sockaddr* addr, socklen_t* len)
{
    int rv = __real_getpeername(fd, addr, len); // fill addr/len, then report failure
    if (rv == 0 && getpeernameFailOnce_.exchange(0))
    {
        getnameinfoFailOnce_.store(1); // arm the follow-up getnameinfo() in the same block
        errno = ENOTCONN;
        fprintf(stderr, "[fault] getpeername -> -1/ENOTCONN\n");
        return -1;
    }
    return rv;
}

int __real_getnameinfo(const struct sockaddr* sa, socklen_t salen, char* host, size_t hostlen,
                       char* serv, size_t servlen, int flags);
int __wrap_getnameinfo(const struct sockaddr* sa, socklen_t salen, char* host, size_t hostlen,
                       char* serv, size_t servlen, int flags)
{
    int rv = __real_getnameinfo(sa, salen, host, hostlen, serv, servlen, flags); // fill host first
    if (rv == 0 && getnameinfoFailOnce_.exchange(0))
    {
        fprintf(stderr, "[fault] getnameinfo -> EAI_NONAME\n");
        return EAI_NONAME;
    }
    return rv;
}

#if defined(ENABLE_TLS_SUPPORT)
SSL_CTX* __real_SSL_CTX_new(const SSL_METHOD* method);
SSL_CTX* __wrap_SSL_CTX_new(const SSL_METHOD* method)
{
    if (sslCtxNewFailOnce_.exchange(0))
    {
        SSL_CTX* ctx = __real_SSL_CTX_new(method);
        if (ctx != nullptr)
        {
            SSL_CTX_free(ctx); // keep the library consistent, hand the caller a failure
        }
        fprintf(stderr, "[fault] SSL_CTX_new -> nullptr\n");
        return nullptr;
    }
    return __real_SSL_CTX_new(method);
}

int __real_SSL_CTX_set_default_verify_paths(SSL_CTX* ctx);
int __wrap_SSL_CTX_set_default_verify_paths(SSL_CTX* ctx)
{
    if (sslDefaultPathsWarnOnce_.exchange(0))
    {
        fprintf(stderr, "[fault] SSL_CTX_set_default_verify_paths -> 0\n");
        return 0;
    }
    return __real_SSL_CTX_set_default_verify_paths(ctx);
}

// OpenSSL spells SSL_CTX_set_min_proto_version() as a macro around
// SSL_CTX_ctrl(), so the ctrl shim is what intercepts that call. Non-matching
// ctrls stay transparent and leave the gate armed for the intended call.
long __real_SSL_CTX_ctrl(SSL_CTX* ctx, int cmd, long larg, void* parg);
long __wrap_SSL_CTX_ctrl(SSL_CTX* ctx, int cmd, long larg, void* parg)
{
    if (cmd == SSL_CTRL_SET_MIN_PROTO_VERSION && sslMinProtoWarnOnce_.load())
    {
        sslMinProtoWarnOnce_.store(0);
        fprintf(stderr, "[fault] SSL_CTX_ctrl(SET_MIN_PROTO_VERSION) -> 0\n");
        return 0;
    }
    return __real_SSL_CTX_ctrl(ctx, cmd, larg, parg);
}

int __real_SSL_set_fd(SSL* ssl, int fd);
int __wrap_SSL_set_fd(SSL* ssl, int fd)
{
    if (sslSetFdFailOnce_.exchange(0))
    {
        fprintf(stderr, "[fault] SSL_set_fd -> 0\n");
        return 0;
    }
    return __real_SSL_set_fd(ssl, fd);
}

int __real_SSL_set1_host(SSL* ssl, const char* host);
int __wrap_SSL_set1_host(SSL* ssl, const char* host)
{
    if (sslSet1HostWarnOnce_.exchange(0))
    {
        fprintf(stderr, "[fault] SSL_set1_host -> 0\n");
        return 0;
    }
    return __real_SSL_set1_host(ssl, host);
}
#endif // defined(ENABLE_TLS_SUPPORT)

} // extern "C"
#endif // !defined(_WIN32) && !defined(__APPLE__)

using namespace std::chrono_literals;

// Lets tests observe and manipulate the handler's socket and TLS state (see
// TcpConnectionHandler.h).
class TcpConnectionHandlerTest
{
public:
    // The client's socket, so a test can manipulate it behind the handler's back.
    static int clientSocket(TcpConnectionHandler& conn)
    {
        return (int)conn.socket_.load(std::memory_order_relaxed);
    }

    // Queues closing the connection the way the receive thread does when
    // the peer hangs up: unlike disconnect(), without cancelling a
    // connection that's still being made.
    static void queueDrop(TcpConnectionHandler& conn)
    {
        conn.enqueue_([&conn]() { conn.disconnectImpl_(); });
    }

    // Waits for everything queued so far to run. (Not waitForAllTasksComplete_(),
    // which drops whatever's queued while it waits.)
    static void waitForQueue(TcpConnectionHandler& conn)
    {
        auto done = std::make_shared<std::promise<void>>();
        auto fut = done->get_future();
        conn.enqueue_([done]() { done->set_value(); });
        fut.wait();
    }
// Flips the stored socket number to INVALID_SOCKET without closing the
// underlying descriptor (the fd stays open, the fd number stays usable).
static void invalidateSocket(TcpConnectionHandler& conn)
{
    conn.socket_.store(-1, std::memory_order_relaxed); // INVALID_SOCKET
}

// Joins the receive thread after a test invalidated the socket behind the
// handler's back (disconnectImpl_ would skip its own join in that case).
static void joinRecvThread(TcpConnectionHandler& conn)
{
    if (conn.receiveThread_.joinable())
    {
        conn.receiveThread_.join();
    }
}

#if !defined(WIN32)
static void callCheckConnections(TcpConnectionHandler& conn, std::vector<int>& socks)
{
    conn.checkConnections_(socks);
}
#endif // !defined(WIN32)

#if defined(ENABLE_TLS_SUPPORT)
    // True if the client's last TLS operation is waiting to write.
    static bool sslWantsWrite(TcpConnectionHandler& conn)
    {
        std::unique_lock<std::mutex> lk(conn.sslMutex_);
        SSL* ssl = conn.ssl_.load(std::memory_order_relaxed);
        return ssl != nullptr && SSL_want_write(ssl);
    }

    // From now on, every TLS write the client attempts reports "try again
    // later" (as on a full socket buffer), however much room the OS has.
    static bool blockSslWrites(TcpConnectionHandler& conn)
    {
        std::unique_lock<std::mutex> lk(conn.sslMutex_);
        SSL* ssl = conn.ssl_.load(std::memory_order_relaxed);
        if (ssl == nullptr)
        {
            return false;
        }

        // The socket BIO (read and write share it). A callback that fails a
        // write before it happens leaves the BIO's ownership untouched.
        BIO_set_callback_ex(SSL_get_rbio(ssl), [](BIO* b, int oper, const char*, size_t, int, long, int ret, size_t*) -> long {
            if (oper == BIO_CB_WRITE)
            {
                BIO_clear_retry_flags(b);
                BIO_set_retry_write(b);
                return -1;
            }
            return ret;
        });
        return true;
    }
// Nulls out the SSL object while the connection still believes it has TLS.
static void nullSsl(TcpConnectionHandler& conn)
{
    conn.ssl_.store(nullptr, std::memory_order_relaxed);
}

// Flags the connection as TLS without ever negotiating TLS.
static void fakeUsingTls(TcpConnectionHandler& conn)
{
    conn.usingTLS_ = true;
}

static int callSslWrite(TcpConnectionHandler& conn, int& sslErr)
{
    return conn.sslWrite_("x", 1, sslErr);
}

static int callSslRead(TcpConnectionHandler& conn, int& sslErr)
{
    return conn.sslRead_((char*)"x", 1, sslErr);
}

static int callSslPending(TcpConnectionHandler& conn)
{
    return conn.sslPending_();
}
#endif // defined(ENABLE_TLS_SUPPORT)
};

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


constexpr int ACCEPT_TIMEOUT_MS = 5000;
constexpr int IO_TIMEOUT_MS = 5000;
constexpr auto EVENT_TIMEOUT = 5s;

// Enough data that a send to a peer that isn't reading can't complete,
// whatever the OS buffers on loopback (Windows takes well over 8 MB).
#if defined(_WIN32)
constexpr size_t STALLED_SEND_BYTES = 64 * 1024 * 1024;
#else
constexpr size_t STALLED_SEND_BYTES = 8 * 1024 * 1024;
#endif // defined(_WIN32)

// Records the handler callbacks so tests can wait on them from the main thread.
class TestConnection : public TcpConnectionHandler
{
public:
    // Makes the receive handler slow (1 ms per call), as if the application
    // were busy, so incoming data backs up.
    std::atomic<bool> slowReceive{false};

    virtual ~TestConnection()
    {
        // TcpConnectionHandler's destructor would call our onDisconnect_()
        // after this subclass is gone, so disconnect while we still exist.
        enableReconnect_.store(false, std::memory_order_relaxed);
        disconnect().wait();
        waitForAllTasksComplete_();
    }

    int connectCount()
    {
        std::unique_lock<std::mutex> lk(mutex_);
        return connectCount_;
    }

    int disconnectCount()
    {
        std::unique_lock<std::mutex> lk(mutex_);
        return disconnectCount_;
    }

    std::string received()
    {
        std::unique_lock<std::mutex> lk(mutex_);
        return received_;
    }

    void markRecvEnd()
    {
        std::unique_lock<std::mutex> lk(mutex_);
        recvEndCount_++;
        cv_.notify_all();
    }

    bool waitFor(std::function<bool()> const& pred, std::chrono::milliseconds timeout = EVENT_TIMEOUT)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        return cv_.wait_for(lk, timeout, pred);
    }

    bool waitForConnects(int count, std::chrono::milliseconds timeout = EVENT_TIMEOUT) { return waitFor([&]() { return connectCount_ >= count; }, timeout); }
    bool waitForDisconnects(int count) { return waitFor([&]() { return disconnectCount_ >= count; }); }
    bool waitForReceived(size_t length, std::chrono::milliseconds timeout = EVENT_TIMEOUT) { return waitFor([&]() { return received_.size() >= length; }, timeout); }
    bool waitForRecvEnd() { return waitFor([&]() { return recvEndCount_ > 0; }); }

protected:
    virtual void onConnect_() override
    {
        std::unique_lock<std::mutex> lk(mutex_);
        connectCount_++;
        cv_.notify_all();
    }

    virtual void onDisconnect_() override
    {
        std::unique_lock<std::mutex> lk(mutex_);
        disconnectCount_++;
        cv_.notify_all();
    }

    virtual void onReceive_(char* buf, int length) override
    {
        if (slowReceive.load())
        {
            std::this_thread::sleep_for(1ms);
        }
        std::unique_lock<std::mutex> lk(mutex_);
        received_.append(buf, length);
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int connectCount_ = 0;
    int disconnectCount_ = 0;
    int recvEndCount_ = 0;
    std::string received_;
};

std::string makePattern(size_t length)
{
    std::string result(length, '\0');
    for (size_t i = 0; i < length; i++)
    {
        result[i] = (char)('A' + (i * 7 + i / 251) % 26);
    }
    return result;
}

bool report(bool result)
{
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testConnectSendReceive()
{
    std::cout << "Test 1 (connect, send and receive over loopback): ";

    LoopbackTcpServer server;
    TestConnection conn;
    conn.setOnRecvEndFn([&]() { conn.markRecvEnd(); });

    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    // Client -> server.
    const std::string clientMsg = "hello from client";
    conn.send(clientMsg.c_str(), clientMsg.size());
    std::string gotClientMsg(clientMsg.size(), '\0');
    result &= CHECK(server.recvExact(&gotClientMsg[0], gotClientMsg.size(), IO_TIMEOUT_MS));
    result &= CHECK((gotClientMsg == clientMsg));

    // Server -> client, followed by the end-of-receive callback.
    const std::string serverMsg = "hello from server";
    result &= CHECK(server.sendAll(serverMsg));
    result &= CHECK(conn.waitForReceived(serverMsg.size()));
    result &= CHECK((conn.received() == serverMsg));
    result &= CHECK(conn.waitForRecvEnd());

    return report(result);
}

bool testLargeTransfers()
{
    std::cout << "Test 2 (multi-chunk transfers in both directions): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    // 4 MB outbound is larger than the loopback socket buffer, so sendImpl_()
    // has to loop on partial writes while the server drains the data.
    const std::string bigOut = makePattern(4 * 1024 * 1024);
    auto sendFuture = conn.send(bigOut.c_str(), bigOut.size());
    std::string gotOut(bigOut.size(), '\0');
    result &= CHECK(server.recvExact(&gotOut[0], gotOut.size(), IO_TIMEOUT_MS));
    result &= CHECK((sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready));
    result &= CHECK((gotOut == bigOut));

    // 64 KB inbound spans many 1 KB reads but fits in the 128 KB receive FIFO.
    const std::string bigIn = makePattern(64 * 1024);
    result &= CHECK(server.sendAll(bigIn));
    result &= CHECK(conn.waitForReceived(bigIn.size()));
    result &= CHECK((conn.received() == bigIn));

    return report(result);
}

bool testServerCloseTriggersDisconnect()
{
    std::cout << "Test 3 (server closing the socket fires onDisconnect_): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    server.closePeer();
    result &= CHECK(conn.waitForDisconnects(1));

    // Without reconnect enabled, nothing should try to connect again.
    result &= CHECK(!server.accept(500));
    result &= CHECK((conn.connectCount() == 1));

    // Sending after the disconnect is a silent no-op.
    conn.send("x", 1).wait();

    return report(result);
}

bool testClientDisconnect()
{
    std::cout << "Test 4 (client disconnect closes socket exactly once): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());

    // Disconnecting while not connected must not fire the handler.
    conn.disconnect().wait();
    result &= CHECK((conn.disconnectCount() == 0));

    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    conn.disconnect().wait();
    result &= CHECK((conn.disconnectCount() == 1));
    result &= CHECK(server.waitForPeerClose(IO_TIMEOUT_MS));

    // A second disconnect is a no-op.
    conn.disconnect().wait();
    result &= CHECK((conn.disconnectCount() == 1));

    return report(result);
}

bool testConnectionRefused()
{
    std::cout << "Test 5 (refused connection without reconnect): ";

    TestConnection conn;
    int port = LoopbackTcpServer::GetClosedPort();

    bool result = CHECK((port > 0));
    auto fut = conn.connect("127.0.0.1", port, false);
    result &= CHECK((fut.wait_for(EVENT_TIMEOUT) == std::future_status::ready));
    result &= CHECK((conn.connectCount() == 0));
    result &= CHECK((conn.disconnectCount() == 0));

    return report(result);
}

bool testReconnectAfterServerClose()
{
    std::cout << "Test 6 (reconnect timer re-establishes a dropped connection): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    // Drop the connection; the client should come back after the reconnect
    // interval (5 s).
    server.closePeer();
    result &= CHECK(conn.waitForDisconnects(1));
    result &= CHECK(server.accept(10000));
    result &= CHECK(conn.waitForConnects(2));

    return report(result);
}

bool testReconnectAfterRefusal()
{
    std::cout << "Test 7 (failed connect with reconnect enabled retries): ";

    // Connect to a port nobody is listening on yet, with reconnect enabled.
    // Then start listening on that port and wait for the retry to land.
    int port = LoopbackTcpServer::GetClosedPort();
    TestConnection conn;
    auto fut = conn.connect("127.0.0.1", port, true);

    bool result = CHECK((fut.wait_for(EVENT_TIMEOUT) == std::future_status::ready));
    result &= CHECK((conn.connectCount() == 0));

    int listenFd = testOpenSocket(AF_INET, SOCK_STREAM);
    int on = 1;
    testSetSockOpt(listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    bool listening = bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) == 0 && listen(listenFd, 1) == 0;
    if (!listening)
    {
        // Another process grabbed the port in the meantime; not a client bug.
        testCloseSocket(listenFd);
        std::cout << "SKIP (port reused) ";
        return report(true);
    }

    result &= CHECK(conn.waitForConnects(1, 10s));

    conn.disconnect().wait();
    testCloseSocket(listenFd);

    return report(result);
}

bool testHostnameResolution()
{
    std::cout << "Test 8 (\"localhost\" resolves and falls back to IPv4): ";

    // The server only listens on 127.0.0.1, so if "localhost" also resolves
    // to ::1 that attempt is refused and the IPv4 address must win.
    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    conn.connect("localhost", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    return report(result);
}

bool testUnresolvableHost()
{
    std::cout << "Test 9 (unresolvable hostname fails cleanly): ";

    // RFC 6761 reserves .invalid, so this can never resolve.
    TestConnection conn;
    auto fut = conn.connect("freedv-backend-test.invalid", 80, false);

    bool result = CHECK(fut.wait_for(30s) == std::future_status::ready);
    result &= CHECK((conn.connectCount() == 0));

    return report(result);
}

// Closes the socket with an RST instead of a FIN, so the peer's next send
// or read fails outright rather than being accepted into a buffer or
// reported as an orderly EOF.
void resetConnection(int fd)
{
    struct linger lingerOpt = {1, 0};
    testSetSockOpt(fd, SOL_SOCKET, SO_LINGER, &lingerOpt, sizeof(lingerOpt));
}

// Starts a send that can't complete because the server isn't reading, and
// returns its future. How much the OS buffers on loopback varies a lot
// (Windows took well over 128 MB), so keep sending `data` until a send is
// still pending after 300 ms, up to about 1 GB in all. On Linux and macOS the
// first send stalls.
std::future<void> startStalledSend(TestConnection& conn, const std::string& data)
{
    const size_t maxSends = std::max<size_t>(1, (1024u * 1024 * 1024) / data.size());
    std::future<void> sendFuture;
    for (size_t i = 0; i < maxSends; i++)
    {
        sendFuture = conn.send(data.c_str(), data.size());
        if (sendFuture.wait_for(300ms) == std::future_status::timeout)
        {
            break;
        }
    }
    return sendFuture;
}

bool testPlainSendToResetPeer()
{
    std::cout << "Test 22 (plain send to a reset connection fails and disconnects): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    // The server resets the connection while the client is mid-way through
    // a send it can't finish (nobody reads): write() must fail, not hang,
    // and the client must treat it as a disconnect.
    const std::string big = makePattern(STALLED_SEND_BYTES);
    auto sendFuture = startStalledSend(conn, big);
    std::this_thread::sleep_for(300ms);
    result &= CHECK(sendFuture.wait_for(0ms) == std::future_status::timeout);
    resetConnection(server.peerFd());
    server.closePeer();

    result &= CHECK(sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(conn.waitForDisconnects(1));
    result &= CHECK(conn.disconnectCount() == 1);

    return report(result);
}

bool testPlainReadFromResetPeer()
{
    std::cout << "Test 23 (reset of an idle plain connection fails the read and disconnects): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    // An RST rather than a FIN: read() fails (ECONNRESET) instead of
    // returning 0, which takes the read-error path rather than EOF.
    resetConnection(server.peerFd());
    server.closePeer();

    result &= CHECK(conn.waitForDisconnects(1));
    result &= CHECK(conn.disconnectCount() == 1);
    result &= CHECK(conn.received().empty());

    return report(result);
}

// Sends 1 MB, far more than the client's 128 KB receive buffer, to a client
// whose receive handler is slow: every byte must still arrive, in order.
bool checkBurstToSlowReceiver(TestConnection& conn, std::function<bool(const std::string&)> const& serverSend)
{
    conn.slowReceive = true;
    const std::string big = makePattern(1024 * 1024);
    bool result = CHECK(serverSend(big));
    result &= CHECK(conn.waitForReceived(big.size(), 30s));
    result &= CHECK(conn.received() == big);
    result &= CHECK(conn.disconnectCount() == 0);
    return result;
}

bool testPlainBurstToSlowReceiver()
{
    std::cout << "Test 25 (1 MB burst to a slow receiver arrives intact, plain TCP): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));
    result &= checkBurstToSlowReceiver(conn, [&](const std::string& data) { return server.sendAll(data); });

    return report(result);
}

bool testConnectWhileConnected()
{
    std::cout << "Test 26 (connect() while connected is ignored; the connection keeps working): ";

    LoopbackTcpServer server;
    LoopbackTcpServer otherServer;
    TestConnection conn;

    bool result = CHECK(server.valid() && otherServer.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    // Used to replace the running receive thread (std::terminate()).
    auto fut = conn.connect("127.0.0.1", otherServer.port(), false);
    result &= CHECK(fut.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(!otherServer.accept(500));
    result &= CHECK(conn.connectCount() == 1);

    const std::string msg = "still connected";
    conn.send(msg.c_str(), msg.size());
    std::string got(msg.size(), '\0');
    result &= CHECK(server.recvExact(&got[0], msg.size(), IO_TIMEOUT_MS) && got == msg);
    result &= CHECK(server.sendAll(msg));
    result &= CHECK(conn.waitForReceived(msg.size()));
    result &= CHECK(conn.received() == msg);
    result &= CHECK(conn.disconnectCount() == 0);

    return report(result);
}

// One address family's scripted DNS answer: how long the lookup takes and
// the (numeric) addresses it returns.
struct DnsAnswer
{
    std::chrono::milliseconds delay{0};
    std::vector<std::pair<std::string, int>> addresses; // address, port
};

// Answers the handler's DNS lookups from a script instead of the network, so
// tests can control which addresses each family returns and when.
class ScriptedDnsConnection : public TestConnection
{
public:
    ScriptedDnsConnection(DnsAnswer ipv6, DnsAnswer ipv4)
        : ipv6_(std::move(ipv6))
        , ipv4_(std::move(ipv4))
    {
    }

    virtual ~ScriptedDnsConnection()
    {
        // Lookups run on the handler's threads until its connect attempt
        // finishes, so finish it while the script still exists.
        enableReconnect_.store(false, std::memory_order_relaxed);
        disconnect().wait();
        waitForAllTasksComplete_();
    }

private:
    DnsAnswer ipv6_;
    DnsAnswer ipv4_;

    virtual void resolveAddresses_(int addressFamily, const char*, const char*, struct addrinfo** result) override
    {
        const DnsAnswer& answer = addressFamily == AF_INET6 ? ipv6_ : ipv4_;
        std::this_thread::sleep_for(answer.delay);

        // Chain one getaddrinfo() result per address, so the handler can
        // release the list with freeaddrinfo() as usual.
        *result = nullptr;
        struct addrinfo** tail = result;
        for (auto& address : answer.addresses)
        {
            struct addrinfo hints;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = addressFamily;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            hints.ai_flags = AI_NUMERICHOST;
            std::string port = std::to_string(address.second);
            if (getaddrinfo(address.first.c_str(), port.c_str(), &hints, tail) == 0)
            {
                while (*tail != nullptr)
                {
                    tail = &(*tail)->ai_next;
                }
            }
        }
    }
};

// Scripted hosts don't go to DNS, so the name is never looked up.
constexpr const char* SCRIPTED_HOST = "scripted.test";

// Variant whose host_ is a deliberately malformed DNS name, so the
// connection's SSL hostname pinning is exercised with an unpinnable value.
class BrokenHostScriptedDnsConnection : public ScriptedDnsConnection
{
public:
    BrokenHostScriptedDnsConnection(DnsAnswer ipv6, DnsAnswer ipv4)
        : ScriptedDnsConnection(std::move(ipv6), std::move(ipv4))
    {
        host_ = "bad host!!";
    }
};

// A connection whose resolver hands back deliberately broken address lists.
class BrokenDnsConnection : public TestConnection
{
public:
    enum class DnsTamper
    {
        WrongAddressFamily, // ai_family no address family knows
        TruncatedAddress,   // ai_addrlen too small to connect
        UdpOnly,            // a datagram socket number on a stream path
    };

    BrokenDnsConnection(DnsTamper tamperHead, std::vector<std::pair<std::string, int> > ipv4)
        : tamperHead_(tamperHead), ipv4_(ipv4)
    {
    }

    virtual ~BrokenDnsConnection()
    {
        enableReconnect_.store(false, std::memory_order_relaxed);
        disconnect().wait();
        waitForAllTasksComplete_();
    }

protected:
    virtual void resolveAddresses_(int addressFamily, const char* host, const char* port,
                                   struct addrinfo** result) override
    {
        if (addressFamily != AF_INET)
        {
            return;
        }
        struct addrinfo* head = nullptr;
        for (const auto& entry : ipv4_)
        {
            struct addrinfo* node = new struct addrinfo;
            memset(node, 0, sizeof(*node));
            struct sockaddr_in* addr = new struct sockaddr_in;
            memset(addr, 0, sizeof(*addr));
            addr->sin_family = AF_INET;
            addr->sin_port = htons(entry.second);
            inet_pton(AF_INET, entry.first.c_str(), &addr->sin_addr);
            node->ai_addr = (struct sockaddr*)addr;
            node->ai_addrlen = sizeof(*addr);
            node->ai_family = AF_INET;
            node->ai_socktype = SOCK_STREAM;
            node->ai_protocol = IPPROTO_TCP;
            node->ai_next = head;
            head = node;
        }
        if (head != nullptr)
        {
            switch (tamperHead_)
            {
            case DnsTamper::WrongAddressFamily:
                head->ai_family = 999; // connect() on this socket number must fail
                break;
            case DnsTamper::TruncatedAddress:
                head->ai_addrlen = 0; // connect() rejects it (EINVAL)
                break;
            case DnsTamper::UdpOnly:
                head->ai_socktype = SOCK_DGRAM;
                head->ai_protocol = 0; // socket() gives a datagram fd
                break;
            }
        }
        *result = head;
    }

private:
    DnsTamper tamperHead_;
    std::vector<std::pair<std::string, int> > ipv4_;
};
// Most of these tests need an IPv6 loopback address; skip them without one.
bool skipWithoutIpv6(LoopbackTcpServer& server6)
{
    if (!server6.valid())
    {
        std::cout << "SKIP (no IPv6 loopback)\n";
        return true;
    }
    return false;
}

bool testDualStackPrefersIpv6()
{
    std::cout << "Test 27 (both families resolve at once: IPv6 is used): ";

    LoopbackTcpServer server6(AF_INET6);
    if (skipWithoutIpv6(server6)) return true;
    LoopbackTcpServer server4;
    ScriptedDnsConnection conn({0ms, {{"::1", server6.port()}}}, {0ms, {{"127.0.0.1", server4.port()}}});

    bool result = CHECK(server4.valid());
    conn.connect(SCRIPTED_HOST, 0, false);
    result &= CHECK(server6.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));
    result &= CHECK(!server4.accept(300));

    return report(result);
}

bool testDualStackWaitsBrieflyForIpv6()
{
    std::cout << "Test 28 (IPv6 DNS answers just after IPv4: IPv6 is still used): ";

    // RFC 8305 gives IPv6 a short head start (50 ms here) once IPv4 answers.
    LoopbackTcpServer server6(AF_INET6);
    if (skipWithoutIpv6(server6)) return true;
    LoopbackTcpServer server4;
    ScriptedDnsConnection conn({5ms, {{"::1", server6.port()}}}, {0ms, {{"127.0.0.1", server4.port()}}});

    bool result = CHECK(server4.valid());
    conn.connect(SCRIPTED_HOST, 0, false);
    result &= CHECK(server6.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));
    result &= CHECK(!server4.accept(300));

    return report(result);
}

bool testSlowIpv6DnsDoesNotDelayIpv4()
{
    std::cout << "Test 29 (slow IPv6 DNS doesn't hold up an IPv4 connection): ";

    LoopbackTcpServer server4;
    ScriptedDnsConnection conn({2000ms, {}}, {0ms, {{"127.0.0.1", server4.port()}}});

    bool result = CHECK(server4.valid());
    conn.connect(SCRIPTED_HOST, 0, false);
    result &= CHECK(server4.accept(1000));
    result &= CHECK(conn.waitForConnects(1));

    return report(result);
}

bool testSlowIpv6DnsDoesNotDelayIpv4Fallback()
{
    std::cout << "Test 30 (slow IPv6 DNS doesn't hold up trying the next IPv4 address): ";

    // The first IPv4 address refuses the connection. The second must be
    // tried straight away, not once the IPv6 lookup finally finishes.
    LoopbackTcpServer server4;
    int closedPort = LoopbackTcpServer::GetClosedPort();
    ScriptedDnsConnection conn({2000ms, {}}, {0ms, {{"127.0.0.1", closedPort}, {"127.0.0.1", server4.port()}}});

    bool result = CHECK(server4.valid());
    conn.connect(SCRIPTED_HOST, 0, false);
    result &= CHECK(server4.accept(1000));
    result &= CHECK(conn.waitForConnects(1));

    return report(result);
}

bool testRefusedIpv6FallsBackToIpv4()
{
    std::cout << "Test 31 (IPv6 address refuses the connection: IPv4 is used): ";

    LoopbackTcpServer probe6(AF_INET6);
    if (skipWithoutIpv6(probe6)) return true;
    int closedPort6 = LoopbackTcpServer::GetClosedPort(AF_INET6);
    LoopbackTcpServer server4;
    ScriptedDnsConnection conn({0ms, {{"::1", closedPort6}}}, {0ms, {{"127.0.0.1", server4.port()}}});

    bool result = CHECK(server4.valid());
    conn.connect(SCRIPTED_HOST, 0, false);
    result &= CHECK(server4.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    return report(result);
}

bool testNoIpv6AnswerWaitsForIpv4()
{
    std::cout << "Test 32 (IPv6 DNS returns nothing: waits for the slower IPv4 answer): ";

    LoopbackTcpServer server4;
    ScriptedDnsConnection conn({0ms, {}}, {300ms, {{"127.0.0.1", server4.port()}}});

    bool result = CHECK(server4.valid());
    conn.connect(SCRIPTED_HOST, 0, false);
    result &= CHECK(server4.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    return report(result);
}

bool testNoAddressesFailsCleanly()
{
    std::cout << "Test 33 (neither family returns an address: the attempt ends without connecting): ";

    ScriptedDnsConnection conn({0ms, {}}, {0ms, {}});
    auto fut = conn.connect(SCRIPTED_HOST, 0, false);

    bool result = CHECK(fut.wait_for(2s) == std::future_status::ready);
    result &= CHECK(conn.connectCount() == 0);

    return report(result);
}

bool testUnansweredAddressDoesNotBlockNext()
{
    std::cout << "Test 34 (an address that never answers doesn't stop the next one being tried): ";

    // 192.0.2.1 (TEST-NET-1, RFC 5737) is never routed, so the attempt either
    // hangs or fails at once depending on the network. Either way the next
    // address must be tried within a moment (250 ms here), and the stalled
    // attempt dropped once that one connects.
    LoopbackTcpServer server4;
    ScriptedDnsConnection conn({0ms, {}}, {0ms, {{"192.0.2.1", 9}, {"127.0.0.1", server4.port()}}});

    bool result = CHECK(server4.valid());
    conn.connect(SCRIPTED_HOST, 0, false);
    result &= CHECK(server4.accept(2000));
    result &= CHECK(conn.waitForConnects(1));

    return report(result);
}

bool testLateIpv6AfterIpv4Fails()
{
    std::cout << "Test 36 (every IPv4 address fails, IPv6 DNS answers later: IPv6 is used): ";

    // The handler runs out of addresses while the IPv6 lookup is still
    // pending, so it must wait for that lookup and then try what it returns.
    LoopbackTcpServer server6(AF_INET6);
    if (skipWithoutIpv6(server6)) return true;
    int closedPort = LoopbackTcpServer::GetClosedPort();
    ScriptedDnsConnection conn({1000ms, {{"::1", server6.port()}}}, {0ms, {{"127.0.0.1", closedPort}}});

    conn.connect(SCRIPTED_HOST, 0, false);
    bool result = CHECK(server6.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    return report(result);
}

bool testDisconnectDuringDnsCancelsConnect()
{
    std::cout << "Test 35 (disconnect() during DNS lookups cancels the connection attempt): ";

    LoopbackTcpServer server4;
    ScriptedDnsConnection conn({500ms, {}}, {500ms, {{"127.0.0.1", server4.port()}}});

    bool result = CHECK(server4.valid());
    auto connectFut = conn.connect(SCRIPTED_HOST, 0, false);
    auto disconnectFut = conn.disconnect();
    result &= CHECK(connectFut.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(disconnectFut.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(!server4.accept(500));
    result &= CHECK(conn.connectCount() == 0);

    return report(result);
}

bool testDroppedBeforeOnConnectRuns()
{
    std::cout << "Test 37 (a connection closed before its onConnect_() runs doesn't get one): ";

    LoopbackTcpServer server4;
    // A moment's DNS lookup, so the connection is still being made when the
    // drop below is queued, and its onConnect_() is queued after that.
    ScriptedDnsConnection conn({200ms, {}}, {200ms, {{"127.0.0.1", server4.port()}}});

    bool result = CHECK(server4.valid());
    auto connectFut = conn.connect(SCRIPTED_HOST, 0, false);
    TcpConnectionHandlerTest::queueDrop(conn);
    result &= CHECK(connectFut.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(server4.accept(ACCEPT_TIMEOUT_MS));
    TcpConnectionHandlerTest::waitForQueue(conn);

    // The connection was made and then closed, and onConnect_() never ran
    // for it: SocketIoClient would otherwise start a websocket on the closed
    // connection that's never freed.
    result &= CHECK(conn.disconnectCount() == 1);
    result &= CHECK(conn.connectCount() == 0);

    return report(result);
}
bool testReconnectTimerIgnoredWhileConnected()
{
    std::cout << "Test 38 (reconnect timer while already connected is ignored): ";
    LoopbackTcpServer serverA;
    TestConnection conn;
    bool result = CHECK(serverA.valid());
    conn.connect("127.0.0.1", serverA.port(), true); // enableReconnect_
    result &= CHECK(serverA.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));
    conn.disconnect().wait();
    result &= CHECK(conn.waitForDisconnects(1)); // disconnect() reliably arms the reconnect timer
    conn.connect("127.0.0.1", serverA.port(), true); // reconnect before the timer fires
    result &= CHECK(serverA.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(2));

    // The timer's queued connectImpl_ must find a valid socket and bail out
    // without touching it, then the connection must still work.
    conn.send("still connected", 15);
    std::string got(15, '\0');
    result &= CHECK(serverA.recvExact(&got[0], got.size(), IO_TIMEOUT_MS) && got == "still connected");
    result &= CHECK(conn.waitForConnects(99, 5500ms) == false); // timer fired, connect ignored
    result &= CHECK(conn.connectCount() == 2);                 // the timer's attempt was ignored
    result &= CHECK(conn.disconnectCount() == 1);
    return report(result);
}

bool testUnusableAddressFamily()
{
    std::cout << "Test 39 (resolver hands back an address with a bogus family): ";
    BrokenDnsConnection conn(BrokenDnsConnection::DnsTamper::WrongAddressFamily, {});
    bool result = CHECK(conn.connect("broken-dns.test", 1, false)
                            .wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(conn.waitForConnects(1) == false);
    result &= CHECK(conn.connectCount() == 0);
    result &= CHECK(conn.disconnectCount() == 0);
    return report(result);
}

bool testTruncatedAddressRejected()
{
    std::cout << "Test 40 (first address has a truncated sockaddr; the second connects): ";
    LoopbackTcpServer server4;
    BrokenDnsConnection conn(BrokenDnsConnection::DnsTamper::TruncatedAddress,
                             {{"127.0.0.1", server4.port()}, {"127.0.0.1", 1}}); // head is port 1
    bool result = CHECK(server4.valid());
    result &= CHECK(conn.connect("broken-dns.test", 1, false)
                        .wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(conn.waitForConnects(1));
    result &= CHECK(conn.connectCount() == 1);
    result &= CHECK(conn.disconnectCount() == 0);
    return report(result);
}

bool testUdpAddressConnectsWithoutPeer()
{
    std::cout << "Test 41 (a datagram address 'connects' immediately with no peer): ";
    LoopbackTcpServer server4;
    BrokenDnsConnection conn(BrokenDnsConnection::DnsTamper::UdpOnly, {{"127.0.0.1", server4.port()}});
    bool result = CHECK(server4.valid());
    result &= CHECK(conn.connect("broken-dns.test", 1, false)
                        .wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(conn.waitForConnects(1));
    result &= CHECK(conn.connectCount() == 1);
    result &= CHECK(server4.accept(300) == false); // no TCP peer ever arrived
    return report(result);
}

bool testRanOutOfAddressesRecollectsLateResults()
{
    std::cout << "Test 42 (out of addresses: late DNS answers are recollected and tried): ";
    LoopbackTcpServer server4;
    int closed6 = LoopbackTcpServer::GetClosedPort(AF_INET6);
    int closed4 = LoopbackTcpServer::GetClosedPort();
    // IPv6 answers at once (refused); IPv4 arrives only after the wait loop
    // starts, so the loop must pick it up and connect through it.
    ScriptedDnsConnection conn({0ms, {{"::1", closed6}}}, {500ms, {{"127.0.0.1", closed4}, {"127.0.0.1", server4.port()}}});
    bool result = CHECK(server4.valid());
    auto connectFuture = conn.connect("scripted.test", 1, false);
    result &= CHECK(server4.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));
    result &= CHECK(conn.connectCount() == 1);
    result &= CHECK(conn.disconnectCount() == 0);
    return report(result);
}

bool testUnansweredAddressKeptUnderWatch()
{
    std::cout << "Test 43 (a blackholed address stays watched until it times out): ";
    ScriptedDnsConnection conn({800ms, {}}, {0ms, {{"192.0.2.1", 9}}}); // TEST-NET: SYN goes nowhere
    bool result = true;
    auto connectFuture = conn.connect("scripted.test", 1, false);
    result &= CHECK(connectFuture.wait_for(1200ms) == std::future_status::timeout); // still connecting
    conn.disconnect().wait(); // cancelConnect_ must end the watch loop
    result &= CHECK(conn.connectCount() == 0);
    result &= CHECK(conn.disconnectCount() == 0); // nothing connected: nothing disconnected
    return report(result);
}

bool testDisconnectDuringAddressWaitLoop()
{
    std::cout << "Test 44 (disconnect cancels the ran-out-of-addresses wait loop): ";
    int closed4 = LoopbackTcpServer::GetClosedPort();
    ScriptedDnsConnection conn({2500ms, {}}, {0ms, {{"127.0.0.1", closed4}}}); // both answers end up empty
    bool result = true;
    auto connectFuture = conn.connect("scripted.test", 1, false);
    std::this_thread::sleep_for(300ms); // while it's out of addresses, waiting for DNS
    auto disconnectFuture = conn.disconnect();
    result &= CHECK(connectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(disconnectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(conn.connectCount() == 0);
    result &= CHECK(conn.disconnectCount() == 0);
    return report(result);
}
bool testPendingConnectSucceedsWhileDnsPending()
{
    std::cout << "Test 45 (in-flight attempt connects while the other lookup is still pending): ";
    // A listener whose backlog stays full while the connect runs: its
    // addresses connect but the handler can only pick them up from its
    // watch loop (no DNS answer arrives to re-arm the attempt list).
    LoopbackTcpServer filled6(AF_INET6);
    bool result = !skipWithoutIpv6(filled6);
    if (!result)
    {
        return report(true);
    }

    std::vector<int> fillers;
    for (int i = 0; i < 6; i++)
    {
        int f = testOpenSocket(AF_INET6, SOCK_STREAM);
        testSetNonBlocking(f);
        struct sockaddr_in6 to;
        memset(&to, 0, sizeof(to));
        to.sin6_family = AF_INET6;
        to.sin6_port = htons(filled6.port());
        inet_pton(AF_INET6, "::1", &to.sin6_addr);
        ::connect(f, (struct sockaddr*)&to, sizeof(to));
        fillers.push_back(f);
    }

    ScriptedDnsConnection conn({1500ms, {{"::1", filled6.port()}}}, {4000ms, {}});
    auto connectFuture = conn.connect("scripted.test", 1, false);
    // Free one backlog slot only while DNS is still "pending", so the stored
    // in-flight attempt must be rescued by the watch loop.
    result &= CHECK(connectFuture.wait_for(1000ms) == std::future_status::timeout);
    result &= CHECK(filled6.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(connectFuture.wait_for(1000ms) == std::future_status::timeout);
    result &= CHECK(filled6.accept(ACCEPT_TIMEOUT_MS)); // the handler's own connect
    result &= CHECK(conn.waitForConnects(1, 4s));
    result &= CHECK(conn.disconnectCount() == 0);
    for (int f : fillers)
    {
        testCloseSocket(f);
    }
    return report(result);
}
bool testSendAfterSocketClosedReportsSelectError()
{
    std::cout << "Test 46 (send's wait loop sees select() fail on a closed socket number): ";
    LoopbackTcpServer server;
    TestConnection conn;
    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    const std::string big = makePattern(STALLED_SEND_BYTES);
    auto sendFuture = startStalledSend(conn, big); // peer never reads, so it stalls
    testCloseSocket(TcpConnectionHandlerTest::clientSocket(conn)); // number stays "valid", fd is gone

    result &= CHECK(sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(conn.disconnectCount() == 0); // nobody asked to disconnect
    conn.disconnect().wait();
    result &= CHECK(conn.waitForDisconnects(1));
    return report(result);
}

bool testReceiveAfterSocketClosedReportsSelectError()
{
    std::cout << "Test 47 (receive loop sees select() fail on a closed socket number): ";
    LoopbackTcpServer server;
    TestConnection conn;
    conn.slowReceive.store(true); // drain at 1 ms per 128 KB, so the socket buffer fills
    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    const std::string big = makePattern(200 * 1024);
    result &= CHECK(server.sendAll(big));
    std::this_thread::sleep_for(5ms); // inside a buffer-full select() wait
    testCloseSocket(TcpConnectionHandlerTest::clientSocket(conn)); // fd gone, number "valid"

    result &= CHECK(conn.waitForDisconnects(1)); // select() fails -> pass ends as an error
    result &= CHECK(conn.connectCount() == 1);
    return report(result);
}

bool testSelectWakeDiscardedWhenSocketInvalidated()
{
    std::cout << "Test 48 (select() wakes, but the socket was invalidated meanwhile): ";
    LoopbackTcpServer server;
    TestConnection conn;
    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));
    int clientFd = TcpConnectionHandlerTest::clientSocket(conn);
    result &= CHECK(clientFd >= 0);

    conn.send("go", 2); // wakes the receive pass
    result &= CHECK(conn.waitForReceived(0)); // trivially true; just a barrier
    // invalidateSocket() flips the stored number: the select() wake is
    // discarded and the pass ends without dispatching anything.
    TcpConnectionHandlerTest::invalidateSocket(conn);
    TcpConnectionHandlerTest::joinRecvThread(conn);
    testCloseSocket(clientFd); // we own the orphaned fd now
    result &= CHECK(conn.connectCount() == 1);
    result &= CHECK(conn.disconnectCount() == 0);
    return report(result);
}

bool testDispatchAbandonedWhenSocketInvalidated()
{
    std::cout << "Test 49 (buffer hand-over skipped when the socket was invalidated): ";
    LoopbackTcpServer server;
    TestConnection conn;
    conn.slowReceive.store(true); // dispatch 128 KB blocks ~1 ms per chunk
    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));
    int clientFd = TcpConnectionHandlerTest::clientSocket(conn);
    result &= CHECK(clientFd >= 0);

    result &= CHECK(server.sendAll(std::string(64 * 1024, 'x')));
    std::this_thread::sleep_for(5ms); // dispatch is mid-flight (socket still valid)
    TcpConnectionHandlerTest::invalidateSocket(conn); // the hand-over guard must notice
    TcpConnectionHandlerTest::joinRecvThread(conn); // disconnectImpl_ skipped its join
    testCloseSocket(clientFd);
    result &= CHECK(conn.connectCount() == 1);
    result &= CHECK(conn.disconnectCount() == 0);
    return report(result);
}
#if !defined(WIN32)
bool testCheckConnectionsSocketErrors()
{
    std::cout << "Test 50 (checkConnections_ on a dead socket number: getsockopt error deletes it): ";
    LoopbackTcpServer server;
    TestConnection conn;
    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    // A connect that will be refused, listed twice on the watch list: the
    // refusal cleans up the first entry, and the second entry then sees a
    // closed fd, which getsockopt() reports as an error.
    int closed4 = LoopbackTcpServer::GetClosedPort();
    int refused = testOpenSocket(AF_INET, SOCK_STREAM);
    testSetNonBlocking(refused);
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(closed4);
    to.sin_addr.s_addr = htonl(0x7F000001);
    ::connect(refused, (struct sockaddr*)&to, sizeof(to)); // will be refused

    int ownFd = TcpConnectionHandlerTest::clientSocket(conn);
    std::vector<int> socks{refused, refused};
    TcpConnectionHandlerTest::callCheckConnections(conn, socks);
    result &= CHECK(socks.empty());
    result &= CHECK(conn.disconnectCount() == 0);
    result &= CHECK(TcpConnectionHandlerTest::clientSocket(conn) == ownFd); // its own socket untouched
    return report(result);
}


bool testSocketExhaustionDuringConnect()
{
    std::cout << "Test 60 (socket() exhaustion mid-connect: every address is skipped, nothing connects): ";

    // Get the sanity listener first: it needs fds, which will be scarce.
    LoopbackTcpServer server4;
    bool result = CHECK(server4.valid());

    struct rlimit original;
    if (getrlimit(RLIMIT_NOFILE, &original) != 0)
    {
        return report(result); // no usable ceiling here; skip
    }
    struct rlimit lowered = original;
    lowered.rlim_cur = 16; // well below the number of fds already open
    if (lowered.rlim_cur > lowered.rlim_max)
    {
        lowered.rlim_cur = lowered.rlim_max;
    }
    if (setrlimit(RLIMIT_NOFILE, &lowered) != 0)
    {
        return report(result); // not permitted to lower the limit; skip
    }

    std::vector<int> dummies;
    while (dummies.size() < 4096)
    {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
        {
            break; // ceiling reached
        }
        dummies.push_back(fd);
    }

    bool survived = true;
    try
    {
        // Scripted DNS answers cost no file descriptors, so the ceiling
        // bites exactly at connectImpl_'s socket() call; every address must
        // be skipped through its error branch without disturbing the flow.
        ScriptedDnsConnection conn({0ms, {}}, {0ms, {{"127.0.0.1", 1}}});
        auto connectFuture = conn.connect(SCRIPTED_HOST, 1, false);
        survived &= connectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready;
        survived &= conn.connectCount() == 0;
        survived &= conn.disconnectCount() == 0;
    }
    catch (const std::exception&)
    {
        // EMFILE surfacing from the promise/thread machinery still counts
        // as surviving.
    }
    result &= CHECK(survived);

    for (int fd : dummies)
    {
        testCloseSocket(fd);
    }

    setrlimit(RLIMIT_NOFILE, &original);

    // With room again, a normal scripted connect works.
    ScriptedDnsConnection conn2({0ms, {}}, {0ms, {{"127.0.0.1", server4.port()}}});
    auto retryFuture = conn2.connect(SCRIPTED_HOST, 1, false);
    result &= CHECK(retryFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(server4.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn2.connectCount() == 1);
    return report(result);
}

bool testLateIpv6CollectedWhileIpv4AttemptInFlight()
{
    std::cout << "Test 61 (IPv6 answer lands while an IPv4 attempt is mid-flight: collected at the next_fd hop): ";

    LoopbackTcpServer server6(AF_INET6);
    if (skipWithoutIpv6(server6)) return true;

    // A listener whose single backlog slot is already taken: the IPv4 attempt
    // stays pending through the whole 250 ms check, which is long enough for
    // the (150 ms) IPv6 lookup to publish its answer before the hop back to
    // index 0, where the already-ready future must be collected without
    // blocking.
    int full4 = testOpenSocket(AF_INET, SOCK_STREAM);
    bool result = CHECK(full4 >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    result &= CHECK(bind(full4, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    socklen_t addrLen = sizeof(addr);
    result &= CHECK(getsockname(full4, (struct sockaddr*)&addr, &addrLen) == 0);
    result &= CHECK(listen(full4, 0) == 0);
    int filler = testOpenSocket(AF_INET, SOCK_STREAM);
    result &= CHECK(filler >= 0);
    result &= CHECK(::connect(filler, (struct sockaddr*)&addr, sizeof(addr)) == 0); // occupies the slot

    ScriptedDnsConnection conn({150ms, {{"::1", server6.port()}}},
                                {0ms, {{"127.0.0.1", ntohs(addr.sin_port)}}});
    conn.connect(SCRIPTED_HOST, 1, false);

    result &= CHECK(server6.accept(ACCEPT_TIMEOUT_MS)); // the IPv6 attempt connects once collected
    result &= CHECK(conn.waitForConnects(1));

    testCloseSocket(filler);
    testCloseSocket(full4);
    return report(result);
}

bool testStalledSendSelectErrorOnClosedSocket()
{
    std::cout << "Test 62 (stalled send's wait loop sees select() fail on a closed socket number): ";
    LoopbackTcpServer server;
    TestConnection conn;
    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    // Pin the peer's receive buffer so the big send truly stalls in the
    // write loop's select() waits instead of being absorbed by big buffers.
    int rcvBuf = 2048;
    result &= CHECK(setsockopt(server.peerFd(), SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf)) == 0);

    const std::string big = makePattern(STALLED_SEND_BYTES);
    auto sendFuture = startStalledSend(conn, big);
    result &= CHECK(sendFuture.wait_for(0ms) == std::future_status::timeout); // stalled for real

    testCloseSocket(TcpConnectionHandlerTest::clientSocket(conn)); // fd gone, number stays "valid"

    if (sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready)
    {
        // select() failed on the dead number and the pass ended as an error.
        return report(result);
    }

    std::cout << "SKIP (send loop kept waiting; a cancel ends it) ";
    auto disconnectFuture = conn.disconnect(); // cancelConnect_ must end the wait
    result &= CHECK(sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(disconnectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    return report(result);
}
#endif // !defined(WIN32)

#if defined(ENABLE_TLS_SUPPORT)

// Runs the server half of a TLS session on the accepted peer socket:
// handshake, read the client's message, reply with an echo.
class TlsServerSession
{
public:
    explicit TlsServerSession(TestCertificate& cert, bool tls12Only = false)
        : ctx_(SSL_CTX_new(TLS_server_method()))
        , ssl_(nullptr)
    {
        SSL_CTX_use_certificate(ctx_, cert.cert);
        SSL_CTX_use_PrivateKey(ctx_, cert.key);
        if (tls12Only)
        {
            SSL_CTX_set_max_proto_version(ctx_, TLS1_2_VERSION);
        }
    }

    ~TlsServerSession()
    {
        if (ssl_ != nullptr)
        {
            SSL_free(ssl_);
        }
        SSL_CTX_free(ctx_);
    }

    bool handshake(int fd)
    {
        ssl_ = SSL_new(ctx_);
        SSL_set_fd(ssl_, fd);
        return SSL_accept(ssl_) == 1;
    }

    bool readExact(std::string& out, size_t length)
    {
        out.resize(length);
        size_t offset = 0;
        while (offset < length)
        {
            int numRead = SSL_read(ssl_, &out[offset], length - offset);
            if (numRead <= 0)
            {
                return false;
            }
            offset += numRead;
        }
        return true;
    }

    bool write(const std::string& data)
    {
        return SSL_write(ssl_, data.data(), data.size()) == (int)data.size();
    }

    // Waits for the client's close_notify (or EOF).
    bool waitForShutdown()
    {
        char buf[64];
        return SSL_read(ssl_, buf, sizeof(buf)) <= 0;
    }

    // TLS 1.2 only: sends a HelloRequest, so the client has to start a new
    // handshake from inside SSL_read() (i.e. reading obliges it to write).
    // Returns false if this TLS library won't renegotiate.
    bool startRenegotiation()
    {
        if (SSL_renegotiate(ssl_) != 1)
        {
            return false;
        }
        int rv = SSL_do_handshake(ssl_);
        int err = SSL_get_error(ssl_, rv);
        return rv == 1 || err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE;
    }

    // As startRenegotiation(), but returns as soon as the HelloRequest is
    // sent instead of blocking until the client answers.
    bool startRenegotiationNonBlocking()
    {
        testSetNonBlocking(SSL_get_fd(ssl_));
        return startRenegotiation();
    }

#if defined(SSL_KEY_UPDATE_REQUESTED)
    // Asks the client to update its keys too (TLS 1.3), which it must answer
    // with a KeyUpdate of its own -- i.e. reading obliges it to write.
    bool requestKeyUpdate()
    {
        return SSL_key_update(ssl_, SSL_KEY_UPDATE_REQUESTED) == 1;
    }
#endif // defined(SSL_KEY_UPDATE_REQUESTED)

private:
    SSL_CTX* ctx_;
    SSL* ssl_;
};

void setPeerRecvTimeout(int fd, int seconds)
{
    testSetRecvTimeout(fd, seconds * 1000);
    testSetSendTimeout(fd, seconds * 1000);
}

bool testTlsRoundTrip(TestCertificate& cert)
{
    std::cout << "Test 10 (TLS handshake, verified certificate, data both ways): ";

    testSetEnv("SSL_CERT_FILE", cert.path.c_str());

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    conn.connect("localhost", server.port(), false, true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    setPeerRecvTimeout(server.peerFd(), 5);

    TlsServerSession tls(cert);
    result &= CHECK(tls.handshake(server.peerFd()));
    result &= CHECK(conn.waitForConnects(1));

    const std::string clientMsg = "hello over tls";
    conn.send(clientMsg.c_str(), clientMsg.size());
    std::string gotClientMsg;
    result &= CHECK(tls.readExact(gotClientMsg, clientMsg.size()));
    result &= CHECK((gotClientMsg == clientMsg));

    // Several SSL_read() calls on the client. An exact multiple of the 1 KB
    // read size also checks that data is dispatched when the last read hits
    // SSL_ERROR_WANT_READ rather than being held until more data arrives.
    const std::string serverMsg = makePattern(16 * 1024);
    result &= CHECK(tls.write(serverMsg));
    result &= CHECK(conn.waitForReceived(serverMsg.size()));
    result &= CHECK((conn.received() == serverMsg));

    conn.disconnect().wait();
    result &= CHECK((conn.disconnectCount() == 1));
    result &= CHECK(tls.waitForShutdown());

    return report(result);
}

bool testTlsUntrustedCertificate(TestCertificate& cert)
{
    std::cout << "Test 11 (TLS with untrusted certificate is rejected): ";

    // Point the client at an empty trust file so our self-signed cert fails
    // validation.
    std::string emptyTrustFile = testMakeTempFile("fdvempty");
    testSetEnv("SSL_CERT_FILE", emptyTrustFile.c_str());

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    auto fut = conn.connect("localhost", server.port(), false, true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    setPeerRecvTimeout(server.peerFd(), 5);

    TlsServerSession tls(cert);
    result &= CHECK(!tls.handshake(server.peerFd()));
    result &= CHECK((fut.wait_for(EVENT_TIMEOUT) == std::future_status::ready));
    result &= CHECK((conn.connectCount() == 0));

    std::remove(emptyTrustFile.c_str());
    testSetEnv("SSL_CERT_FILE", cert.path.c_str());

    return report(result);
}

bool testTlsGarbageFromServer()
{
    std::cout << "Test 12 (non-TLS reply during handshake is rejected): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    auto fut = conn.connect("localhost", server.port(), false, true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(server.sendAll(std::string("HTTP/1.1 400 Bad Request\r\n\r\n")));
    result &= CHECK((fut.wait_for(EVENT_TIMEOUT) == std::future_status::ready));
    result &= CHECK((conn.connectCount() == 0));

    return report(result);
}

bool testTlsHandshakeCancelled()
{
    std::cout << "Test 13 (disconnect cancels a stalled TLS handshake): ";

    // The server accepts but never answers the ClientHello, so the client sits
    // in its handshake loop until we ask it to disconnect.
    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    auto connectFuture = conn.connect("localhost", server.port(), false, true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    std::this_thread::sleep_for(300ms);
    result &= CHECK((connectFuture.wait_for(0ms) == std::future_status::timeout));

    auto start = std::chrono::steady_clock::now();
    auto disconnectFuture = conn.disconnect();
    result &= CHECK((connectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready));
    result &= CHECK((disconnectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready));
    result &= CHECK((std::chrono::steady_clock::now() - start < 5s));
    result &= CHECK((conn.connectCount() == 0));
    result &= CHECK(server.waitForPeerClose(IO_TIMEOUT_MS));

    return report(result);
}

bool testTlsHandshakeTimeout()
{
    std::cout << "Test 14 (stalled TLS handshake times out after ~10 s): ";

    // As in test 13 the server never answers, but nobody calls disconnect():
    // the client must give up on its own after TLS_HANDSHAKE_TIMEOUT (10 s)
    // and close the socket, rather than waiting forever.
    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    auto start = std::chrono::steady_clock::now();
    auto connectFuture = conn.connect("localhost", server.port(), false, true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));

    result &= CHECK(connectFuture.wait_for(20s) == std::future_status::ready);
    auto elapsed = std::chrono::steady_clock::now() - start;
    result &= CHECK(elapsed >= 9s);
    result &= CHECK(elapsed < 15s);
    result &= CHECK(conn.connectCount() == 0);
    result &= CHECK(server.waitForPeerClose(IO_TIMEOUT_MS));

    return report(result);
}

// Common setup for the tests below: a verified TLS connection to a loopback
// server whose SSL object the test drives directly.
struct TlsFixture
{
    LoopbackTcpServer server;
    TestConnection conn;
    TlsServerSession tls;
    bool ok = false;

    explicit TlsFixture(TestCertificate& cert, bool tls12Only = false)
        : tls(cert, tls12Only)
    {
        testSetEnv("SSL_CERT_FILE", cert.path.c_str());
        conn.connect("localhost", server.port(), false, true);
        ok = server.valid() && server.accept(ACCEPT_TIMEOUT_MS);
        if (ok)
        {
            setPeerRecvTimeout(server.peerFd(), 10);
            ok = tls.handshake(server.peerFd()) && conn.waitForConnects(1);
        }
    }
};

bool testTlsSendToSlowReader(TestCertificate& cert)
{
    std::cout << "Test 15 (TLS send blocks on a full socket buffer, then completes): ";

    // 8 MB is far more than the loopback socket buffers hold, so SSL_write()
    // keeps returning SSL_ERROR_WANT_WRITE until the server starts reading.
    TlsFixture f(cert);
    bool result = CHECK(f.ok);

    const std::string big = makePattern(STALLED_SEND_BYTES);
    auto sendFuture = f.conn.send(big.c_str(), big.size());
    std::this_thread::sleep_for(500ms);
    result &= CHECK(sendFuture.wait_for(0ms) == std::future_status::timeout);

    std::string got;
    result &= CHECK(f.tls.readExact(got, big.size()));
    result &= CHECK(got == big);
    result &= CHECK(sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(f.conn.disconnectCount() == 0);

    return report(result);
}

bool testBlockedSendAbandonedOnDisconnect(TestCertificate& cert)
{
    std::cout << "Test 16 (disconnect abandons a send blocked on a full buffer): ";

    bool result = true;

    // Plain TCP and TLS: the peer never reads, so the send can't finish; a
    // disconnect must give up on it promptly instead of waiting forever.
    for (bool useTls : {false, true})
    {
        std::unique_ptr<TlsFixture> tlsFixture;
        LoopbackTcpServer plainServer;
        TestConnection plainConn;
        TestConnection* conn = &plainConn;
        if (useTls)
        {
            tlsFixture = std::make_unique<TlsFixture>(cert);
            result &= CHECK(tlsFixture->ok);
            conn = &tlsFixture->conn;
        }
        else
        {
            plainConn.connect("127.0.0.1", plainServer.port(), false);
            result &= CHECK(plainServer.accept(ACCEPT_TIMEOUT_MS));
            result &= CHECK(plainConn.waitForConnects(1));
        }

        const std::string big = makePattern(STALLED_SEND_BYTES);
        auto sendFuture = startStalledSend(*conn, big);
        std::this_thread::sleep_for(300ms);
        result &= CHECK(sendFuture.wait_for(0ms) == std::future_status::timeout);

        auto start = std::chrono::steady_clock::now();
        auto disconnectFuture = conn->disconnect();
        result &= CHECK(sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
        result &= CHECK(disconnectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
        result &= CHECK(std::chrono::steady_clock::now() - start < 3s);
    }

    return report(result);
}

bool testTlsSendToResetPeer(TestCertificate& cert)
{
    std::cout << "Test 17 (TLS send to a reset connection fails and disconnects): ";

    TlsFixture f(cert);
    bool result = CHECK(f.ok);

    // The server resets the connection while the client is mid-way through a
    // large send: SSL_write() must fail (not hang) and the client must treat
    // it as a disconnect.
    const std::string big = makePattern(STALLED_SEND_BYTES);
    auto sendFuture = f.conn.send(big.c_str(), big.size());
    std::this_thread::sleep_for(300ms);
    resetConnection(f.server.peerFd());
    f.server.closePeer();

    result &= CHECK(sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(f.conn.waitForDisconnects(1));

    return report(result);
}

bool testTlsCorruptRecord(TestCertificate& cert)
{
    std::cout << "Test 18 (corrupted TLS record from the server disconnects): ";

    TlsFixture f(cert);
    bool result = CHECK(f.ok);

    // A well-formed TLS 1.2+ application-data record header followed by bytes
    // that won't decrypt, written straight to the socket behind SSL's back.
    std::string bogus = "\x17\x03\x03";
    bogus += (char)0x00;
    bogus += (char)0x40;
    bogus += std::string(0x40, '\xAA');
    result &= CHECK(f.server.sendAll(bogus));

    result &= CHECK(f.conn.waitForDisconnects(1));
    result &= CHECK(f.conn.received().empty());

    return report(result);
}

#if defined(SSL_KEY_UPDATE_REQUESTED)
bool testTlsKeyUpdateWhileSendBlocked(TestCertificate& cert)
{
    std::cout << "Test 19 (TLS 1.3 key update while the client's send is blocked): ";

    TlsFixture f(cert);
    bool result = CHECK(f.ok);

    // Request a key update while the client's send is stalled on a full
    // buffer. (OpenSSL queues the client's KeyUpdate response for its next
    // write rather than sending it from SSL_read().) Once the server drains
    // the connection, everything must complete with data intact and the
    // connection must keep working under the new keys.
    const std::string big = makePattern(STALLED_SEND_BYTES);
    auto sendFuture = f.conn.send(big.c_str(), big.size());
    std::this_thread::sleep_for(300ms);

    result &= CHECK(f.tls.requestKeyUpdate());
    const std::string msg = "after key update";
    result &= CHECK(f.tls.write(msg));
    std::this_thread::sleep_for(300ms);

    std::string got;
    result &= CHECK(f.tls.readExact(got, big.size()));
    result &= CHECK(got == big);
    result &= CHECK(sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(f.conn.waitForReceived(msg.size()));
    result &= CHECK(f.conn.received() == msg);

    // The connection must still work in both directions after the update.
    const std::string after = "still alive";
    f.conn.send(after.c_str(), after.size());
    result &= CHECK(f.tls.readExact(got, after.size()) && got == after);

    return report(result);
}

#endif // defined(SSL_KEY_UPDATE_REQUESTED)

// TLS 1.2 renegotiation while the client's send is stalled on a full buffer:
// the stalled SSL_write() picks up the new handshake and has to wait for the
// server's half of it (SSL_write() -> SSL_ERROR_WANT_READ) before it can
// continue sending application data.
bool testTlsRenegotiationWhileSendBlocked(TestCertificate& cert)
{
    std::cout << "Test 20 (TLS 1.2 renegotiation while the client's send is blocked): ";

    TlsFixture f(cert, true);
    bool result = CHECK(f.ok);

    const std::string big = makePattern(STALLED_SEND_BYTES);
    auto sendFuture = f.conn.send(big.c_str(), big.size());
    std::this_thread::sleep_for(300ms);

    if (!f.tls.startRenegotiation())
    {
        std::cout << "SKIP (TLS library won't renegotiate) ";
        std::string drain;
        f.tls.readExact(drain, big.size());
        return report(result);
    }
    std::this_thread::sleep_for(300ms);

    // Drain the client's data (SSL_read() on the server also completes the
    // renegotiation handshake), then check it arrived intact.
    std::string got;
    result &= CHECK(f.tls.readExact(got, big.size()));
    result &= CHECK(got == big);
    result &= CHECK(sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);

    // And the renegotiated connection works in both directions.
    const std::string msg = "after renegotiation";
    result &= CHECK(f.tls.write(msg));
    result &= CHECK(f.conn.waitForReceived(msg.size()));
    result &= CHECK(f.conn.received() == msg);
    f.conn.send(msg.c_str(), msg.size());
    result &= CHECK(f.tls.readExact(got, msg.size()) && got == msg);
    result &= CHECK(f.conn.disconnectCount() == 0);

    return report(result);
}

// TLS 1.2 renegotiation while the client can't write: its SSL_read() has to
// send a ClientHello and can't (SSL_read() -> SSL_ERROR_WANT_WRITE), and the
// socket isn't writable either. A disconnect must still complete promptly
// rather than waiting for the socket to drain.
bool testTlsReadWantsWriteThenDisconnect(TestCertificate& cert)
{
    std::cout << "Test 21 (disconnect while SSL_read() waits to write): ";

    TlsFixture f(cert, true);
    bool result = CHECK(f.ok);
    int clientFd = TcpConnectionHandlerTest::clientSocket(f.conn);
    result &= CHECK(clientFd >= 0);

    // Make the client's TLS writes fail as if its socket were full, then ask
    // it to renegotiate: SSL_read() has to send a ClientHello and can't.
    result &= CHECK(TcpConnectionHandlerTest::blockSslWrites(f.conn));
    if (!f.tls.startRenegotiationNonBlocking())
    {
        std::cout << "SKIP (TLS library won't renegotiate) ";
        return report(result);
    }

    auto waitStart = std::chrono::steady_clock::now();
    while (!TcpConnectionHandlerTest::sslWantsWrite(f.conn) && f.conn.disconnectCount() == 0 &&
           std::chrono::steady_clock::now() - waitStart < EVENT_TIMEOUT)
    {
        std::this_thread::sleep_for(10ms);
    }
    if (f.conn.disconnectCount() > 0)
    {
        std::cout << "SKIP (client refused to renegotiate) ";
        return report(result);
    }
    result &= CHECK(TcpConnectionHandlerTest::sslWantsWrite(f.conn));

    // Now really fill the client's socket buffer (with raw bytes the server
    // never reads, so the stream's integrity doesn't matter), so its receive
    // thread waits for the socket to become writable. Data keeps draining
    // into the server's receive buffer for a while, which the OS may also
    // grow, so keep topping up until nothing more fits for a few rounds.
    if (clientFd >= 0)
    {
        char junk[4096];
        memset(junk, 0, sizeof(junk));
        for (int quietRounds = 0; quietRounds < 3;)
        {
            bool wroteAny = false;
            for (int chunk : {(int)sizeof(junk), 1})
            {
                // The handler's socket is non-blocking, so this stops when full.
                while (::send(clientFd, junk, chunk, 0) > 0)
                {
                    wroteAny = true;
                }
            }
            quietRounds = wroteAny ? 0 : quietRounds + 1;
            std::this_thread::sleep_for(100ms);
        }
    }

    auto start = std::chrono::steady_clock::now();
    auto disconnectFuture = f.conn.disconnect();
    bool finished = disconnectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready;
    result &= CHECK(finished);
    result &= CHECK(std::chrono::steady_clock::now() - start < 3s);
    if (!finished)
    {
        // The handler can't be destroyed while its receive thread is stuck.
        std::cout << "FAIL (disconnect hung)" << std::endl;
        _exit(1);
    }

    return report(result);
}

// TLS 1.2 renegotiation the server never completes: the client sends its
// ClientHello and waits for the server's reply, so its next SSL_write() has
// to wait to read (SSL_write() -> SSL_ERROR_WANT_READ). A disconnect must
// abandon that send promptly.
bool testTlsSendWaitingToReadAbandonedOnDisconnect(TestCertificate& cert)
{
    std::cout << "Test 24 (disconnect abandons a send waiting on a stalled renegotiation): ";

    TlsFixture f(cert, true);
    bool result = CHECK(f.ok);

    if (!f.tls.startRenegotiationNonBlocking())
    {
        std::cout << "SKIP (TLS library won't renegotiate) ";
        return report(result);
    }
    std::this_thread::sleep_for(300ms);
    if (f.conn.disconnectCount() > 0)
    {
        std::cout << "SKIP (client refused to renegotiate) ";
        return report(result);
    }

    const std::string msg = "stuck behind the handshake";
    auto sendFuture = f.conn.send(msg.c_str(), msg.size());
    std::this_thread::sleep_for(300ms);
    result &= CHECK(sendFuture.wait_for(0ms) == std::future_status::timeout);

    auto start = std::chrono::steady_clock::now();
    auto disconnectFuture = f.conn.disconnect();
    bool finished = sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready &&
                    disconnectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready;
    result &= CHECK(finished);
    result &= CHECK(std::chrono::steady_clock::now() - start < 3s);
    if (!finished)
    {
        // The handler can't be destroyed while its send is stuck.
        std::cout << "FAIL (send never abandoned)" << std::endl;
        _exit(1);
    }

    return report(result);
}

bool testTlsBurstToSlowReceiver(TestCertificate& cert)
{
    std::cout << "Test 25b (1 MB burst to a slow receiver arrives intact, TLS): ";

    TlsFixture f(cert);
    bool result = CHECK(f.ok);
    result &= checkBurstToSlowReceiver(f.conn, [&](const std::string& data) { return f.tls.write(data); });

    return report(result);
}
// Removes an environment variable (testSetEnv can't; setting "" still counts
// as "present" for the handler's getenv() checks).
void testUnsetEnv(const char* name)
{
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif // defined(_WIN32)
}

bool testTlsCertDirEnv(TestCertificate& cert)
{
    std::cout << "Test 52 (SSL_CERT_DIR set alongside the certificate file: trust loads): ";
    std::string certDir = cert.path.substr(0, cert.path.find_last_of("/\\"));
    testSetEnv("SSL_CERT_DIR", certDir.c_str());
    testSetEnv("SSL_CERT_FILE", cert.path.c_str());

    LoopbackTcpServer server;
    TestConnection conn;
    bool result = CHECK(server.valid());
    auto connFut = conn.connect("localhost", server.port(), false, true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    setPeerRecvTimeout(server.peerFd(), 10);
    TlsServerSession tls(cert);
    result &= CHECK(tls.handshake(server.peerFd()));
    result &= CHECK(connFut.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(conn.waitForConnects(1));

    const std::string msg = "via cert dir";
    conn.send(msg.c_str(), msg.size());
    std::string got;
    result &= CHECK(tls.readExact(got, msg.size()) && got == msg);
    result &= CHECK(tls.write("ack"));
    result &= CHECK(conn.waitForReceived(3));
    result &= CHECK(conn.received() == "ack");
    result &= CHECK(conn.disconnectCount() == 0);

    testUnsetEnv("SSL_CERT_DIR"); // don't leak the setting into later tests
    return report(result);
}

bool testTlsWithoutCertEnvVars(TestCertificate& cert)
{
    std::cout << "Test 53 (no SSL_CERT_FILE/SSL_CERT_DIR: system store only, cert rejected): ";
    testUnsetEnv("SSL_CERT_FILE");
    testUnsetEnv("SSL_CERT_DIR");

    LoopbackTcpServer server;
    TestConnection conn;
    bool result = CHECK(server.valid());
    auto fut = conn.connect("localhost", server.port(), false, true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    setPeerRecvTimeout(server.peerFd(), 5);
    TlsServerSession tls(cert);
    result &= CHECK(!tls.handshake(server.peerFd())); // self-signed: system store rejects it
    result &= CHECK(fut.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(conn.connectCount() == 0);

    testSetEnv("SSL_CERT_FILE", cert.path.c_str()); // leave a sane state behind
    return report(result);
}

bool testTlsHandshakeWantsWrite(TestCertificate& cert)
{
    std::cout << "Test 55 (TLS handshake stalled wanting to write: disconnect cancels it): ";
    testSetEnv("SSL_CERT_FILE", cert.path.c_str());
    LoopbackTcpServer server;
    TestConnection conn;
    bool result = CHECK(server.valid());
    auto connectFuture = conn.connect("localhost", server.port(), false, true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS)); // TCP accepted, TLS never answered

    // Once the handshake's SSL object exists, make the client's TLS writes
    // report "try again", so SSL_connect() stalls wanting to write.
    auto start = std::chrono::steady_clock::now();
    while (!TcpConnectionHandlerTest::blockSslWrites(conn) &&
           std::chrono::steady_clock::now() - start < EVENT_TIMEOUT)
    {
        std::this_thread::sleep_for(10ms);
    }
    if (!TcpConnectionHandlerTest::sslWantsWrite(conn))
    {
        std::cout << "SKIP (handshake past its first write) ";
        conn.disconnect().wait();
        return report(result);
    }

    auto disconnectFuture = conn.disconnect(); // must break out of the loop
    result &= CHECK(connectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(disconnectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(conn.connectCount() == 0);
    return report(result);
}

bool testTlsFlaggedWithoutSslObject()
{
    std::cout << "Test 56 (TLS flagged but no SSL object: helpers bail out, plain path used): ";
    LoopbackTcpServer server;
    TestConnection conn;
    bool result = CHECK(server.valid());
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));

    int sslErr = 0;
    result &= CHECK(TcpConnectionHandlerTest::callSslWrite(conn, sslErr) == -1);
    result &= CHECK(TcpConnectionHandlerTest::callSslRead(conn, sslErr) == -1);
    result &= CHECK(TcpConnectionHandlerTest::callSslPending(conn) == 0);

    TcpConnectionHandlerTest::fakeUsingTls(conn); // claims TLS, never negotiated it
    const std::string msg = "plain path";
    conn.send(msg.c_str(), msg.size());
    std::string got(msg.size(), '\0');
    result &= CHECK(server.recvExact(&got[0], got.size(), IO_TIMEOUT_MS) && got == msg);

    // Flagged as TLS with nothing pending: the read loop never decrypts, so
    // every arrival (and every timeout) looks like the end of the stream.
    result &= CHECK(server.sendAll(std::string("ack")));
    result &= CHECK(conn.waitForDisconnects(1));
    result &= CHECK(conn.connectCount() == 1);
    result &= CHECK(conn.disconnectCount() == 1);
    result &= CHECK(conn.received().empty());
    return report(result);
}
bool testTlsSendLosesSslObjectMidWait(TestCertificate& cert)
{
    std::cout << "Test 57 (send waiting to read loses its SSL object mid-wait): ";
    TlsFixture f(cert, true);
    bool result = CHECK(f.ok);
    if (!f.tls.startRenegotiationNonBlocking())
    {
        std::cout << "SKIP (TLS library won't renegotiate) ";
        return report(result);
    }
    std::this_thread::sleep_for(300ms);
    if (f.conn.disconnectCount() > 0)
    {
        std::cout << "SKIP (client refused to renegotiate) ";
        return report(result);
    }

    const std::string msg = "stuck behind the handshake";
    auto sendFuture = f.conn.send(msg.c_str(), msg.size());
    result &= CHECK(sendFuture.wait_for(300ms) == std::future_status::timeout); // stalled SSL_write

    TcpConnectionHandlerTest::nullSsl(f.conn); // the write loop must notice and give up
    auto disconnectFuture = f.conn.disconnect();
    result &= CHECK(sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(disconnectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(f.conn.waitForDisconnects(1));
    result &= CHECK(f.conn.connectCount() == 1);
    result &= CHECK(f.conn.disconnectCount() == 1);
    return report(result);
}

bool testTlsSendWaitingOnInvalidSocket(TestCertificate& cert)
{
    std::cout << "Test 58 (send waiting to read on a socket invalidated behind its back): ";
    TlsFixture f(cert, true);
    bool result = CHECK(f.ok);
    int clientFd = TcpConnectionHandlerTest::clientSocket(f.conn);
    result &= CHECK(clientFd >= 0);
    if (!f.tls.startRenegotiationNonBlocking())
    {
        std::cout << "SKIP (TLS library won't renegotiate) ";
        return report(result);
    }
    std::this_thread::sleep_for(300ms);
    if (f.conn.disconnectCount() > 0)
    {
        std::cout << "SKIP (client refused to renegotiate) ";
        return report(result);
    }

    const std::string msg = "stuck behind the handshake";
    auto sendFuture = f.conn.send(msg.c_str(), msg.size());
    result &= CHECK(sendFuture.wait_for(300ms) == std::future_status::timeout); // stalled SSL_write

    TcpConnectionHandlerTest::invalidateSocket(f.conn); // the WANT_READ guard must notice
    auto disconnectFuture = f.conn.disconnect();
    result &= CHECK(sendFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(disconnectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    TcpConnectionHandlerTest::joinRecvThread(f.conn); // disconnectImpl_ skipped its join
    testCloseSocket(clientFd);
    result &= CHECK(f.conn.connectCount() == 1);
    result &= CHECK(f.conn.disconnectCount() == 0); // invalid socket: nothing to disconnect
    return report(result);
}

bool testTlsReadWantsWriteOnInvalidSocket(TestCertificate& cert)
{
    std::cout << "Test 59 (SSL_read waiting to write on an invalidated socket: pass ends, no dispatch): ";
    TlsFixture f(cert, true);
    bool result = CHECK(f.ok);
    int clientFd = TcpConnectionHandlerTest::clientSocket(f.conn);
    result &= CHECK(clientFd >= 0);

    result &= CHECK(f.tls.write(makePattern(64 * 1024))); // real data arrives first
    result &= CHECK(TcpConnectionHandlerTest::blockSslWrites(f.conn));
    if (!f.tls.startRenegotiationNonBlocking())
    {
        std::cout << "SKIP (TLS library won't renegotiate) ";
        return report(result);
    }

    // The read pass now holds the buffered data while SSL_read() tries to
    // send its ClientHello and can't (SSL_ERROR_WANT_WRITE).
    auto start = std::chrono::steady_clock::now();
    while (!TcpConnectionHandlerTest::sslWantsWrite(f.conn) && f.conn.disconnectCount() == 0 &&
           std::chrono::steady_clock::now() - start < EVENT_TIMEOUT)
    {
        std::this_thread::sleep_for(10ms);
    }
    if (f.conn.disconnectCount() > 0)
    {
        std::cout << "SKIP (client refused to renegotiate) ";
        return report(result);
    }

    TcpConnectionHandlerTest::invalidateSocket(f.conn); // the WANT_WRITE guard must notice
    auto disconnectFuture = f.conn.disconnect();
    result &= CHECK(disconnectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    TcpConnectionHandlerTest::joinRecvThread(f.conn);
    testCloseSocket(clientFd);
    // The buffered data was never dispatched: the pass ended on the dead
    // socket before its hand-over point.
    result &= CHECK(f.conn.connectCount() == 1);
    result &= CHECK(f.conn.disconnectCount() == 0);
    return report(result);
}


bool testTlsHelloRetryRequestStallsWrite(TestCertificate& cert)
{
    std::cout << "Test 63 (TLS server demands a key share via HelloRetryRequest: the follow-up write stalls wanting to write): ";

    // RFC 8446 section 4.4.1's HelloRetryRequest sentinel random value.
    static const unsigned char HrrRandom[32] = {
        0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
        0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C};

    testSetEnv("SSL_CERT_FILE", cert.path.c_str());
    LoopbackTcpServer server;
    TestConnection conn;
    bool result = CHECK(server.valid());
    auto connectFuture = conn.connect("localhost", server.port(), false, true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    int peer = server.peerFd();

    // Read the client's first full TLS record (its ClientHello).
    unsigned char hello[16384];
    int got = 0;
    bool wireOk = true;
    while (wireOk && got < 5)
    {
        ssize_t n = ::recv(peer, hello + got, sizeof(hello) - got, 0);
        if (n <= 0) wireOk = false; else got += int(n);
    }
    if (wireOk)
    {
        int recLen = (hello[3] << 8) | hello[4];
        while (wireOk && got < 5 + recLen && got < int(sizeof(hello)))
        {
            ssize_t n = ::recv(peer, hello + got, sizeof(hello) - got, 0);
            if (n <= 0) wireOk = false; else got += int(n);
        }
    }
    result &= CHECK(wireOk && got >= 6);
    if (!result)
    {
        conn.disconnect().wait();
        return report(result);
    }

    // Walk the ClientHello extensions for supported_groups (0x000A) and
    // key_share (0x0033), then pick a named group the client supports but
    // has no key share for yet.
    std::vector<int> supported, shared;
    int p = 9; // record header + handshake header
    p += 2 + 32; // legacy version + random
    if (p < got)
    {
        int sidLen = hello[p];
        p += 1 + sidLen; // session id
    }
    if (p + 2 <= got)
    {
        p += 2 + ((hello[p] << 8) | hello[p + 1]); // cipher suites length + list
    }
    if (p + 1 <= got)
    {
        p += 1 + hello[p]; // compression methods length + list
    }
    if (p + 2 <= got)
    {
        int extEnd = p + 2 + ((hello[p] << 8) | hello[p + 1]);
        p += 2;
        while (p + 4 <= got && p + 4 <= extEnd)
        {
            int type = (hello[p] << 8) | hello[p + 1];
            int len = (hello[p + 2] << 8) | hello[p + 3];
            p += 4;
            if (p + len > got) break;
            int q = p, listEnd = 0;
            if ((type == 0x000A || type == 0x0033) && q + 2 <= p + len)
            {
                listEnd = q + 2 + ((hello[q] << 8) | hello[q + 1]);
                q += 2;
            }
            if (type == 0x000A)
            {
                while (q + 2 <= listEnd && q + 2 <= p + len)
                {
                    supported.push_back((hello[q] << 8) | hello[q + 1]);
                    q += 2;
                }
            }
            else if (type == 0x0033)
            {
                while (q + 4 <= listEnd && q + 4 <= p + len)
                {
                    shared.push_back((hello[q] << 8) | hello[q + 1]);
                    q += 4 + ((hello[q + 2] << 8) | hello[q + 3]); // group + key length + key
                }
            }
            p += len;
        }
    }

    struct NamedGroup { int id; const char* name; };
    const NamedGroup candidates[] = {{0x0019, "P-521"}, {0x0018, "P-384"}, {0x0017, "P-256"}};
    const char* groupName = nullptr;
    for (const auto& candidate : candidates)
    {
        for (int g : supported)
        {
            if (g == candidate.id &&
                std::find(shared.begin(), shared.end(), g) == shared.end())
            {
                groupName = candidate.name;
                break;
            }
        }
        if (groupName != nullptr) break;
    }
    if (groupName == nullptr)
    {
        std::cout << "SKIP (client already offers a key share for every supported named group) ";
        conn.disconnect().wait();
        return report(result);
    }

    // Run the server half on a memory BIO pair, restricted to that group:
    // it must answer with a HelloRetryRequest before the client can proceed.
    SSL_CTX* sctx = SSL_CTX_new(TLS_server_method());
    result &= CHECK(sctx != nullptr);
    SSL_CTX_set_min_proto_version(sctx, TLS1_3_VERSION);
    result &= CHECK(SSL_CTX_use_certificate(sctx, cert.cert) == 1);
    result &= CHECK(SSL_CTX_use_PrivateKey(sctx, cert.key) == 1);
    result &= CHECK(SSL_CTX_set1_groups_list(sctx, groupName) == 1);
    SSL* srv = SSL_new(sctx);
    result &= CHECK(srv != nullptr);
    BIO* rbio = BIO_new(BIO_s_mem());
    BIO* wbio = BIO_new(BIO_s_mem());
    SSL_set_bio(srv, rbio, wbio); // the SSL object owns both BIOs now
    SSL_set_accept_state(srv);
    result &= CHECK(BIO_write(rbio, hello, got) == got);
    SSL_do_handshake(srv);
    unsigned char hrr[16384];
    int hrrLen = 0, chunk;
    while (hrrLen < int(sizeof(hrr)) && (chunk = BIO_read(wbio, hrr + hrrLen, sizeof(hrr) - hrrLen)) > 0)
    {
        hrrLen += chunk;
    }
    bool isHrr = false;
    for (int i = 0; i + 32 <= hrrLen; i++)
    {
        if (memcmp(hrr + i, HrrRandom, 32) == 0) { isHrr = true; break; }
    }
    if (!isHrr || hrrLen <= 0)
    {
        std::cout << "SKIP (server side did not produce a HelloRetryRequest) ";
        SSL_free(srv);
        SSL_CTX_free(sctx);
        conn.disconnect().wait();
        return report(result);
    }

    // The client can only continue by writing; from here on its TLS writes
    // report "try again", so the handshake loop stalls wanting to write and
    // arms its select() waits on the write side.
    result &= CHECK(TcpConnectionHandlerTest::blockSslWrites(conn));
    result &= CHECK((int)::send(peer, hrr, hrrLen, 0) == hrrLen);

    auto start = std::chrono::steady_clock::now();
    while (!TcpConnectionHandlerTest::sslWantsWrite(conn) &&
           std::chrono::steady_clock::now() - start < EVENT_TIMEOUT)
    {
        std::this_thread::sleep_for(10ms);
    }
    if (!TcpConnectionHandlerTest::sslWantsWrite(conn))
    {
        std::cout << "SKIP (client did not stall wanting to write) ";
        conn.disconnect().wait();
        SSL_free(srv);
        SSL_CTX_free(sctx);
        server.closePeer();
        return report(result);
    }

    auto disconnectFuture = conn.disconnect(); // must cancel the stalled handshake
    result &= CHECK(connectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(disconnectFuture.wait_for(EVENT_TIMEOUT) == std::future_status::ready);
    result &= CHECK(conn.connectCount() == 0);
    SSL_free(srv);
    SSL_CTX_free(sctx);
    server.closePeer();
    return report(result);
}
#endif // defined(ENABLE_TLS_SUPPORT)

// ---------------------------------------------------------------------------
// Linux-only fault-injection tests: the --wrap shims above drive the
// handler's OS/library failure arms deterministically. Each test arms its
// gate just before the connect attempt; each gate must read 0 afterwards,
// proving the shim fired exactly where intended.
// ---------------------------------------------------------------------------
#if !defined(_WIN32) && !defined(__APPLE__)

bool testConnectSocketFlagsGetFailure()
{
    std::cout << "Test 64 (fcntl(F_GETFL) failure: the attempt is dropped, the next address connects): ";

    // Two identical scripted addresses: the first attempt dies at the fcntl
    // shim before the server sees anything; the second must connect normally.
    LoopbackTcpServer server;
    ScriptedDnsConnection conn({0ms, {}}, {0ms, {{"127.0.0.1", server.port()}, {"127.0.0.1", server.port()}}});

    bool result = CHECK(server.valid());
    fcntlFailMode_.store(1);
    conn.connect(SCRIPTED_HOST, 0, false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));
    result &= CHECK((fcntlFailMode_.load() == 0));

    return report(result);
}

bool testConnectSocketFlagsSetFailure()
{
    std::cout << "Test 65 (fcntl(F_SETFL) failure: the attempt is dropped, the next address connects): ";

    LoopbackTcpServer server;
    ScriptedDnsConnection conn({0ms, {}}, {0ms, {{"127.0.0.1", server.port()}, {"127.0.0.1", server.port()}}});

    bool result = CHECK(server.valid());
    fcntlFailMode_.store(2);
    conn.connect(SCRIPTED_HOST, 0, false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));
    result &= CHECK((fcntlFailMode_.load() == 0));

    return report(result);
}

bool testConnectReportsEinprogress()
{
    std::cout << "Test 66 (connect() reporting EINPROGRESS: the SO_ERROR poll still completes the connection): ";

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    // The shim lets the real connect succeed, then reports -1/EINPROGRESS,
    // so the happy-eyeballs fall-through and checkConnections_() complete it.
    connectEinprogressOnce_.store(1);
    conn.connect("127.0.0.1", server.port(), false);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    result &= CHECK(conn.waitForConnects(1));
    result &= CHECK((connectEinprogressOnce_.load() == 0));

    return report(result);
}
#endif // !defined(_WIN32) && !defined(__APPLE__)

#if !defined(_WIN32) && !defined(__APPLE__) && defined(ENABLE_TLS_SUPPORT)

bool testTlsSslCtxAllocationFailure(TestCertificate& cert)
{
    std::cout << "Test 67 (SSL_CTX_new failure: the TLS attempt fails cleanly, a retry connects): ";

    testSetEnv("SSL_CERT_FILE", cert.path.c_str());

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    sslCtxNewFailOnce_.store(1);
    auto fut = conn.connect("localhost", server.port(), false, true);
    result &= CHECK((fut.wait_for(EVENT_TIMEOUT) == std::future_status::ready));
    result &= CHECK((sslCtxNewFailOnce_.load() == 0)); // the shim fired
    result &= CHECK((conn.connectCount() == 0));       // the failed attempt never reached onConnect_

    // With the shim spent, the same handler must complete a normal TLS session.
    LoopbackTcpServer server2;
    result &= CHECK(server2.valid());
    conn.connect("localhost", server2.port(), false, true);
    result &= CHECK(server2.accept(ACCEPT_TIMEOUT_MS));
    setPeerRecvTimeout(server2.peerFd(), 5);
    TlsServerSession tls(cert);
    result &= CHECK(tls.handshake(server2.peerFd()));
    result &= CHECK(conn.waitForConnects(1));
    conn.disconnect().wait();
    result &= CHECK(tls.waitForShutdown());

    return report(result);
}

bool testTlsSetFdFailure(TestCertificate& cert)
{
    std::cout << "Test 68 (SSL_set_fd failure: the attempt disconnects without onConnect_, a retry connects): ";

    testSetEnv("SSL_CERT_FILE", cert.path.c_str());

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    sslSetFdFailOnce_.store(1);
    auto fut = conn.connect("localhost", server.port(), false, true);
    result &= CHECK((fut.wait_for(EVENT_TIMEOUT) == std::future_status::ready));
    result &= CHECK((sslSetFdFailOnce_.load() == 0));
    result &= CHECK((conn.connectCount() == 0));

    LoopbackTcpServer server2;
    result &= CHECK(server2.valid());
    conn.connect("localhost", server2.port(), false, true);
    result &= CHECK(server2.accept(ACCEPT_TIMEOUT_MS));
    setPeerRecvTimeout(server2.peerFd(), 5);
    TlsServerSession tls(cert);
    result &= CHECK(tls.handshake(server2.peerFd()));
    result &= CHECK(conn.waitForConnects(1));
    conn.disconnect().wait();
    result &= CHECK(tls.waitForShutdown());

    return report(result);
}

bool testTlsWarnOnlySetupFailures(TestCertificate& cert)
{
    std::cout << "Test 69 (default_verify_paths / min_proto_version / set1_host failures are warn-only): ";

    testSetEnv("SSL_CERT_FILE", cert.path.c_str());

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    // The cert trust comes from SSL_CERT_FILE (loaded via SSL_CTX_load_verify
    // _locations), so losing the default paths, the version floor and the
    // hostname pin must all be survivable warnings.
    sslDefaultPathsWarnOnce_.store(1);
    sslMinProtoWarnOnce_.store(1);
    sslSet1HostWarnOnce_.store(1);
    conn.connect("localhost", server.port(), false, true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    setPeerRecvTimeout(server.peerFd(), 5);
    TlsServerSession tls(cert);
    result &= CHECK(tls.handshake(server.peerFd()));
    result &= CHECK(conn.waitForConnects(1));

    const std::string clientMsg = "warnings notwithstanding";
    conn.send(clientMsg.c_str(), clientMsg.size());
    std::string gotClientMsg;
    result &= CHECK(tls.readExact(gotClientMsg, clientMsg.size()));
    result &= CHECK((gotClientMsg == clientMsg));

    conn.disconnect().wait();
    result &= CHECK(tls.waitForShutdown());

    // All three shims must have fired during this one TLS setup.
    result &= CHECK((sslDefaultPathsWarnOnce_.load() == 0));
    result &= CHECK((sslMinProtoWarnOnce_.load() == 0));
    result &= CHECK((sslSet1HostWarnOnce_.load() == 0));

    return report(result);
}

bool testTlsPeerNameWarnings(TestCertificate& cert)
{
    std::cout << "Test 70 (getpeername/getnameinfo failures only cost a warning): ";

    testSetEnv("SSL_CERT_FILE", cert.path.c_str());

    LoopbackTcpServer server;
    TestConnection conn;

    bool result = CHECK(server.valid());
    // The getpeername shim arms the getnameinfo one, so the in-block follow-up
    // call (not the earlier address-logging one) is the one that reports EAI_NONAME.
    getpeernameFailOnce_.store(1);
    conn.connect("localhost", server.port(), false, true);
    result &= CHECK(server.accept(ACCEPT_TIMEOUT_MS));
    setPeerRecvTimeout(server.peerFd(), 5);
    TlsServerSession tls(cert);
    result &= CHECK(tls.handshake(server.peerFd()));
    result &= CHECK(conn.waitForConnects(1));

    const std::string clientMsg = "peer name lookup warnings are survivable";
    conn.send(clientMsg.c_str(), clientMsg.size());
    std::string gotClientMsg;
    result &= CHECK(tls.readExact(gotClientMsg, clientMsg.size()));
    result &= CHECK((gotClientMsg == clientMsg));

    conn.disconnect().wait();
    result &= CHECK(tls.waitForShutdown());
    result &= CHECK((getpeernameFailOnce_.load() == 0));
    result &= CHECK((getnameinfoFailOnce_.load() == 0));

    return report(result);
}
#endif // !defined(_WIN32) && !defined(__APPLE__) && defined(ENABLE_TLS_SUPPORT)

} // namespace

int main(int, char**)
{
    // The fake servers here write to clients that may already have hung up
    // (e.g. after rejecting a TLS certificate). Report that as a failed write
    // rather than letting SIGPIPE kill the test.
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif // !defined(_WIN32)

    bool result = true;

    result &= testConnectSendReceive();
    result &= testLargeTransfers();
    result &= testServerCloseTriggersDisconnect();
    result &= testClientDisconnect();
    result &= testConnectionRefused();
    result &= testReconnectAfterServerClose();
    result &= testReconnectAfterRefusal();
    result &= testHostnameResolution();
    result &= testUnresolvableHost();

#if defined(ENABLE_TLS_SUPPORT)
    TestCertificate cert;
    if (!cert.generate())
    {
        std::cout << "Could not generate test certificate\n";
        return -1;
    }
    result &= testTlsRoundTrip(cert);
    result &= testTlsUntrustedCertificate(cert);
    result &= testTlsGarbageFromServer();
    result &= testTlsHandshakeCancelled();
    result &= testTlsHandshakeTimeout();
    result &= testTlsSendToSlowReader(cert);
    result &= testBlockedSendAbandonedOnDisconnect(cert);
    result &= testTlsSendToResetPeer(cert);
    result &= testTlsCorruptRecord(cert);
#if defined(SSL_KEY_UPDATE_REQUESTED)
    result &= testTlsKeyUpdateWhileSendBlocked(cert);
#endif // defined(SSL_KEY_UPDATE_REQUESTED)
    result &= testTlsRenegotiationWhileSendBlocked(cert);
    result &= testTlsReadWantsWriteThenDisconnect(cert);
    result &= testTlsSendWaitingToReadAbandonedOnDisconnect(cert);
    result &= testTlsBurstToSlowReceiver(cert);
    result &= testTlsCertDirEnv(cert);
    result &= testTlsWithoutCertEnvVars(cert);
    result &= testTlsHandshakeWantsWrite(cert);
    result &= testTlsHelloRetryRequestStallsWrite(cert);
    result &= testTlsFlaggedWithoutSslObject();
    result &= testTlsSendLosesSslObjectMidWait(cert);
    result &= testTlsSendWaitingOnInvalidSocket(cert);
    result &= testTlsReadWantsWriteOnInvalidSocket(cert);
#if !defined(_WIN32) && !defined(__APPLE__)
    result &= testTlsSslCtxAllocationFailure(cert);
    result &= testTlsSetFdFailure(cert);
    result &= testTlsWarnOnlySetupFailures(cert);
    result &= testTlsPeerNameWarnings(cert);
#endif // !defined(_WIN32) && !defined(__APPLE__)
#endif // defined(ENABLE_TLS_SUPPORT)

    // Numbered after the TLS tests, which were written first.
    result &= testPlainSendToResetPeer();
    result &= testPlainReadFromResetPeer();
    result &= testPlainBurstToSlowReceiver();
    result &= testConnectWhileConnected();
    result &= testDualStackPrefersIpv6();
    result &= testDualStackWaitsBrieflyForIpv6();
    result &= testSlowIpv6DnsDoesNotDelayIpv4();
    result &= testSlowIpv6DnsDoesNotDelayIpv4Fallback();
    result &= testRefusedIpv6FallsBackToIpv4();
    result &= testNoIpv6AnswerWaitsForIpv4();
    result &= testNoAddressesFailsCleanly();
    result &= testUnansweredAddressDoesNotBlockNext();
    result &= testDisconnectDuringDnsCancelsConnect();
    result &= testLateIpv6AfterIpv4Fails();
    result &= testDroppedBeforeOnConnectRuns();
    result &= testReconnectTimerIgnoredWhileConnected();
    result &= testUnusableAddressFamily();
    result &= testTruncatedAddressRejected();
    result &= testUdpAddressConnectsWithoutPeer();
    result &= testRanOutOfAddressesRecollectsLateResults();
    result &= testUnansweredAddressKeptUnderWatch();
    result &= testDisconnectDuringAddressWaitLoop();
    result &= testPendingConnectSucceedsWhileDnsPending();
    result &= testSendAfterSocketClosedReportsSelectError();
    result &= testReceiveAfterSocketClosedReportsSelectError();
    result &= testSelectWakeDiscardedWhenSocketInvalidated();
    result &= testDispatchAbandonedWhenSocketInvalidated();
#if !defined(WIN32)
    result &= testCheckConnectionsSocketErrors();
    result &= testSocketExhaustionDuringConnect();
    result &= testLateIpv6CollectedWhileIpv4AttemptInFlight();
    result &= testStalledSendSelectErrorOnClosedSocket();
#endif // !defined(WIN32)
#if !defined(_WIN32) && !defined(__APPLE__)
    result &= testConnectSocketFlagsGetFailure();
    result &= testConnectSocketFlagsSetFailure();
    result &= testConnectReportsEinprogress();
#endif // !defined(_WIN32) && !defined(__APPLE__)

    return result ? 0 : -1;
}
