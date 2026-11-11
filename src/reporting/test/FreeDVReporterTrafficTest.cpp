// End-to-end tests for FreeDVReporter: a real FreeDVReporter (and the
// SocketIoClient/TcpConnectionHandler stack beneath it) talks to an in-process
// fake FreeDV Reporter server over loopback. The fake speaks just enough of
// WebSocket (RFC 6455), engine.io v4 and socket.io v5 to drive the client.

#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <websocketpp/base64/base64.hpp>
#include <websocketpp/sha1/sha1.hpp>

#include "../FreeDVReporter.h"
#include "../../util/SocketIoClient.h"
#include "../../util/test/LoopbackTcpServer.h"

#if defined(ENABLE_TLS_SUPPORT)
#include <openssl/ssl.h>
#include "../../util/test/TestCertificate.h"
#endif // defined(ENABLE_TLS_SUPPORT)

using namespace std::chrono_literals;

// Grants the one piece of reporter state needed to observe the reporter's own
// disconnect cleanup. Kept at namespace scope to match the friend declaration
// in FreeDVReporter.h.
class FreeDVReporterTest
{
public:
    static bool fullyConnected(const FreeDVReporter& reporter) { return reporter.isFullyConnected_.load(std::memory_order_relaxed); }
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
// FreeDVReporter reconnects 5 s after a drop.
constexpr int RECONNECT_ACCEPT_TIMEOUT_MS = 10000;
constexpr auto EVENT_TIMEOUT = 5s;

const char* ENGINE_IO_OPEN =
    R"(0{"sid":"eio-test","upgrades":[],"pingInterval":25000,"pingTimeout":20000,"maxPayload":1000000})";

// Owned yyjson document; frees on destruction.
struct JsonDoc
{
    yyjson_doc* doc = nullptr;

    JsonDoc() = default;
    explicit JsonDoc(const std::string& text) : doc(yyjson_read(text.c_str(), text.size(), 0)) {}
    JsonDoc(JsonDoc&& other) noexcept : doc(other.doc) { other.doc = nullptr; }
    JsonDoc& operator=(JsonDoc&& other) noexcept { std::swap(doc, other.doc); return *this; }
    JsonDoc(const JsonDoc&) = delete;
    JsonDoc& operator=(const JsonDoc&) = delete;
    ~JsonDoc() { if (doc != nullptr) yyjson_doc_free(doc); }

    yyjson_val* root() const { return yyjson_doc_get_root(doc); }
};

std::string getStr(yyjson_val* obj, const char* key)
{
    auto val = yyjson_obj_get(obj, key);
    return yyjson_is_str(val) ? yyjson_get_str(val) : "<missing>";
}

// Fake FreeDV Reporter server: WebSocket server role on top of
// LoopbackTcpServer (optionally over TLS), plus socket.io helpers.
class FakeReporterServer
{
public:
    FakeReporterServer() = default;

#if defined(ENABLE_TLS_SUPPORT)
    // Serves over TLS with this certificate (for "localhost"), as the real
    // server does when freedv-gui uses a secure connection (its default).
    explicit FakeReporterServer(TestCertificate& cert)
        : sslCtx_(SSL_CTX_new(TLS_server_method()))
    {
        SSL_CTX_use_certificate(sslCtx_, cert.cert);
        SSL_CTX_use_PrivateKey(sslCtx_, cert.key);
    }
#endif // defined(ENABLE_TLS_SUPPORT)

    ~FakeReporterServer()
    {
        endTls_();
#if defined(ENABLE_TLS_SUPPORT)
        if (sslCtx_ != nullptr)
        {
            SSL_CTX_free(sslCtx_);
        }
#endif // defined(ENABLE_TLS_SUPPORT)
    }

    FakeReporterServer(const FakeReporterServer&) = delete;
    FakeReporterServer& operator=(const FakeReporterServer&) = delete;

    bool valid() const { return tcp_.valid(); }
    int port() const { return tcp_.port(); }

    // The TLS certificate is for "localhost" (which also tries ::1 first;
    // the server only listens on 127.0.0.1).
    std::string hostname() const
    {
#if defined(ENABLE_TLS_SUPPORT)
        if (sslCtx_ != nullptr)
        {
            return "localhost:" + std::to_string(port());
        }
#endif // defined(ENABLE_TLS_SUPPORT)
        return "127.0.0.1:" + std::to_string(port());
    }

    // Accepts the client and completes the WebSocket upgrade. Records the
    // request target so tests can check the engine.io query string.
    bool acceptWebSocket(int timeoutMs = ACCEPT_TIMEOUT_MS)
    {
        endTls_();
        if (!tcp_.accept(timeoutMs) || !startTls_())
        {
            return false;
        }

        std::string request;
        while (request.find("\r\n\r\n") == std::string::npos)
        {
            char c;
            if (!recvExact_(&c, 1, IO_TIMEOUT_MS) || request.size() > 8192)
            {
                return false;
            }
            request += c;
        }

        std::istringstream lines(request);
        std::string method, target;
        lines >> method >> target;
        requestTarget_ = target;

        std::string key;
        std::string line;
        while (std::getline(lines, line))
        {
            const std::string header = "sec-websocket-key:";
            std::string lower = line;
            for (auto& ch : lower) ch = (char)tolower(ch);
            if (lower.compare(0, header.size(), header) == 0)
            {
                key = line.substr(header.size());
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t\r") + 1);
            }
        }
        if (method != "GET" || key.empty())
        {
            return false;
        }

        std::string acceptSrc = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        unsigned char hash[20];
        websocketpp::sha1::calc(acceptSrc.data(), acceptSrc.size(), hash);

        std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + websocketpp::base64_encode(hash, sizeof(hash)) + "\r\n\r\n";
        return sendAll_(response);
    }

    const std::string& requestTarget() const { return requestTarget_; }

    // Sends one unmasked text frame (server-to-client frames are never masked).
    bool sendText(const std::string& payload)
    {
        std::string frame;
        frame += (char)0x81;
        if (payload.size() < 126)
        {
            frame += (char)payload.size();
        }
        else if (payload.size() < 65536)
        {
            frame += (char)126;
            frame += (char)((payload.size() >> 8) & 0xFF);
            frame += (char)(payload.size() & 0xFF);
        }
        else
        {
            frame += (char)127;
            for (int shift = 56; shift >= 0; shift -= 8)
            {
                frame += (char)(((uint64_t)payload.size() >> shift) & 0xFF);
            }
        }
        frame += payload;
        return sendAll_(frame);
    }

    // Sends one unmasked binary (opcode 2) frame; the client's websocket
    // layer must deliver it without disturbing the engine.io parser.
    bool sendBinary(const std::string& payload)
    {
        std::string frame;
        frame += (char)0x82;
        frame += (char)payload.size();
        frame += payload;
        return sendAll_(frame);
    }

    bool sendEvent(const std::string& name, const std::string& argsJson)
    {
        return sendText("42[\"" + name + "\"," + argsJson + "]");
    }

    // Receives the next text frame from the client, unmasking it. Returns
    // false on timeout, EOF, or a WebSocket close frame.
    bool recvText(std::string& payload, int timeoutMs = IO_TIMEOUT_MS)
    {
        while (true)
        {
            unsigned char header[2];
            if (!recvExact_(header, 2, timeoutMs))
            {
                return false;
            }
            int opcode = header[0] & 0x0F;
            bool masked = (header[1] & 0x80) != 0;
            uint64_t length = header[1] & 0x7F;
            if (length == 126 || length == 127)
            {
                unsigned char ext[8];
                int extLen = (length == 126) ? 2 : 8;
                if (!recvExact_(ext, extLen, timeoutMs))
                {
                    return false;
                }
                length = 0;
                for (int i = 0; i < extLen; i++)
                {
                    length = (length << 8) | ext[i];
                }
            }
            unsigned char mask[4] = {0, 0, 0, 0};
            if (masked && !recvExact_(mask, 4, timeoutMs))
            {
                return false;
            }
            std::string data(length, '\0');
            if (length > 0 && !recvExact_(&data[0], length, timeoutMs))
            {
                return false;
            }
            for (size_t i = 0; i < data.size(); i++)
            {
                data[i] ^= mask[i % 4];
            }

            if (opcode == 0x8)
            {
                return false; // close
            }
            if (opcode == 0x1)
            {
                payload = data;
                return true;
            }
            // Ignore ping/pong/continuation frames; the client doesn't send them.
        }
    }

    // Reads socket.io event packets until one named `name` arrives, returning
    // its argument (or a null document for argument-less events). Packets for
    // other events are kept in `skipped` for tests that care about them.
    bool expectEvent(const std::string& name, JsonDoc& args, int timeoutMs = IO_TIMEOUT_MS)
    {
        std::string payload;
        while (recvText(payload, timeoutMs))
        {
            if (payload.compare(0, 2, "42") != 0)
            {
                skipped.push_back(payload);
                continue;
            }
            JsonDoc event(payload.substr(2));
            yyjson_val* eventName = yyjson_arr_get(event.root(), 0);
            if (yyjson_is_str(eventName) && name == yyjson_get_str(eventName))
            {
                yyjson_val* eventArgs = yyjson_arr_get(event.root(), 1);
                args = eventArgs != nullptr ? JsonDoc(writeJson_(eventArgs)) : JsonDoc();
                return true;
            }
            skipped.push_back(payload);
        }
        return false;
    }

    bool expectEvent(const std::string& name)
    {
        JsonDoc ignored;
        return expectEvent(name, ignored);
    }

    // engine.io open, then read the socket.io CONNECT packet ("40{auth}")
    // and return its auth object.
    bool openAndReadAuth(JsonDoc& auth, const char* engineIoOpen = ENGINE_IO_OPEN)
    {
        std::string payload;
        if (!sendText(engineIoOpen) || !recvText(payload) || payload.compare(0, 2, "40") != 0)
        {
            return false;
        }
        auth = JsonDoc(payload.substr(2));
        return auth.doc != nullptr;
    }

    // Full happy-path handshake up to socket.io CONNECT acknowledgement.
    bool completeHandshake(JsonDoc& auth, int acceptTimeoutMs = ACCEPT_TIMEOUT_MS)
    {
        return acceptWebSocket(acceptTimeoutMs) &&
               openAndReadAuth(auth) &&
               sendText(R"(40{"sid":"sio-test"})");
    }

    bool waitForClientClose(int timeoutMs = IO_TIMEOUT_MS)
    {
#if defined(ENABLE_TLS_SUPPORT)
        if (ssl_ != nullptr)
        {
            // Discard data until close_notify or EOF.
            testSetRecvTimeout(tcp_.peerFd(), timeoutMs);
            char buf[1024];
            int numRead;
            while ((numRead = SSL_read(ssl_, buf, sizeof(buf))) > 0)
            {
            }
            return SSL_get_error(ssl_, numRead) == SSL_ERROR_ZERO_RETURN || !testLastRecvTimedOut();
        }
#endif // defined(ENABLE_TLS_SUPPORT)
        return tcp_.waitForPeerClose(timeoutMs);
    }

    void dropConnection()
    {
        endTls_();
        tcp_.closePeer();
    }

    std::vector<std::string> skipped;

private:
    LoopbackTcpServer tcp_;
    std::string requestTarget_;
#if defined(ENABLE_TLS_SUPPORT)
    SSL_CTX* sslCtx_ = nullptr;
    SSL* ssl_ = nullptr;
#endif // defined(ENABLE_TLS_SUPPORT)

    // TLS handshake on a newly accepted client (no-op without TLS).
    bool startTls_()
    {
#if defined(ENABLE_TLS_SUPPORT)
        if (sslCtx_ != nullptr)
        {
            testSetRecvTimeout(tcp_.peerFd(), IO_TIMEOUT_MS);
            testSetSendTimeout(tcp_.peerFd(), IO_TIMEOUT_MS);
            ssl_ = SSL_new(sslCtx_);
            SSL_set_fd(ssl_, tcp_.peerFd());
            return SSL_accept(ssl_) == 1;
        }
#endif // defined(ENABLE_TLS_SUPPORT)
        return true;
    }

    void endTls_()
    {
#if defined(ENABLE_TLS_SUPPORT)
        if (ssl_ != nullptr)
        {
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
#endif // defined(ENABLE_TLS_SUPPORT)
    }

    bool recvExact_(void* data, size_t length, int timeoutMs)
    {
#if defined(ENABLE_TLS_SUPPORT)
        if (ssl_ != nullptr)
        {
            testSetRecvTimeout(tcp_.peerFd(), timeoutMs);
            char* ptr = static_cast<char*>(data);
            while (length > 0)
            {
                int numRead = SSL_read(ssl_, ptr, (int)length);
                if (numRead <= 0)
                {
                    return false;
                }
                ptr += numRead;
                length -= numRead;
            }
            return true;
        }
#endif // defined(ENABLE_TLS_SUPPORT)
        return tcp_.recvExact(data, length, timeoutMs);
    }

    bool sendAll_(const std::string& data)
    {
#if defined(ENABLE_TLS_SUPPORT)
        if (ssl_ != nullptr)
        {
            return SSL_write(ssl_, data.data(), (int)data.size()) == (int)data.size();
        }
#endif // defined(ENABLE_TLS_SUPPORT)
        return tcp_.sendAll(data);
    }

    static std::string writeJson_(yyjson_val* val)
    {
        char* text = yyjson_val_write(val, 0, nullptr);
        std::string result = text != nullptr ? text : "";
        free(text);
        return result;
    }
};

// Thread-safe log of reporter callbacks, so the main thread can wait for them.
class CallbackLog
{
public:
    void add(const std::string& entry)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        entries_.push_back(entry);
        cv_.notify_all();
    }

    bool waitFor(const std::string& entry, std::chrono::milliseconds timeout = EVENT_TIMEOUT)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        return cv_.wait_for(lk, timeout, [&]() {
            for (auto& e : entries_)
            {
                if (e == entry) return true;
            }
            return false;
        });
    }

    int count(const std::string& prefix)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        int result = 0;
        for (auto& e : entries_)
        {
            if (e.compare(0, prefix.size(), prefix) == 0) result++;
        }
        return result;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::string> entries_;
};

void registerCallbacks(FreeDVReporter& reporter, CallbackLog& log)
{
    reporter.setOnReporterConnectFn([&]() { log.add("reporter_connect"); });
    reporter.setOnReporterDisconnectFn([&]() { log.add("reporter_disconnect"); });
    reporter.setConnectionSuccessfulFn([&]() { log.add("connection_successful"); });
    reporter.setAboutToShowSelfFn([&]() { log.add("about_to_show_self"); });
    reporter.setRecvEndFn([&]() { log.add("recv_end"); });

    reporter.setOnUserConnectFn([&](std::string sid, std::string, std::string callsign, std::string grid,
                                    std::string version, bool rxOnly, std::string connectTime) {
        log.add("user_connect " + sid + " " + callsign + " " + grid + " " + version + " " +
                (rxOnly ? "rx_only" : "tx") + " " + connectTime);
    });
    reporter.setOnUserDisconnectFn([&](std::string sid, std::string, std::string callsign, std::string,
                                       std::string, bool, std::string) {
        log.add("user_disconnect " + sid + " " + callsign);
    });
    reporter.setOnFrequencyChangeFn([&](std::string sid, std::string, std::string callsign, std::string,
                                        uint64_t freq) {
        log.add("freq_change " + sid + " " + callsign + " " + std::to_string(freq));
    });
    reporter.setOnTransmitUpdateFn([&](std::string sid, std::string, std::string, std::string,
                                       std::string mode, bool tx, std::string lastTx) {
        log.add("tx_report " + sid + " " + mode + " " + (tx ? "tx" : "rx") + " [" + lastTx + "]");
    });
    reporter.setOnReceiveUpdateFn([&](std::string sid, std::string, std::string receiver, std::string,
                                      std::string sender, float snr, std::string mode) {
        log.add("rx_report " + sid + " " + receiver + " " + sender + " " + std::to_string((int)snr) + " " + mode);
    });
    reporter.setMessageUpdateFn([&](std::string sid, std::string, std::string message) {
        log.add("message_update " + sid + " " + message);
    });
    reporter.setOnQSYRequestFn([&](std::string callsign, uint64_t freq, std::string message) {
        log.add("qsy_request " + callsign + " " + std::to_string(freq) + " " + message);
    });
}

bool report(bool result)
{
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testHandshakeAndInitialReport()
{
    std::cout << "Test 1 (handshake, auth payload, state replay on connection_successful): ";

    FakeReporterServer server;
    CallbackLog log;
    FreeDVReporter reporter(server.hostname(), "N1DQ", "CN98", "FreeDV Test", false);
    registerCallbacks(reporter, log);

    // State set before connecting must be replayed once the server says
    // the connection is fully established.
    reporter.freqChange(14236000);
    reporter.transmit("RADEV1", false);
    reporter.updateMessage("hello from test");

    bool result = CHECK(server.valid());
    reporter.connect();
    result &= CHECK(log.waitFor("reporter_connect"));

    JsonDoc auth;
    result &= CHECK(server.acceptWebSocket());
    result &= CHECK((server.requestTarget() == "/socket.io/?EIO=4&transport=websocket"));
    result &= CHECK(server.openAndReadAuth(auth));
    result &= CHECK((getStr(auth.root(), "role") == "report"));
    result &= CHECK((getStr(auth.root(), "callsign") == "N1DQ"));
    result &= CHECK((getStr(auth.root(), "grid_square") == "CN98"));
    result &= CHECK((getStr(auth.root(), "version") == "FreeDV Test"));
    result &= CHECK(yyjson_is_false(yyjson_obj_get(auth.root(), "rx_only")));
    result &= CHECK((getStr(auth.root(), "os") != "<missing>"));
    result &= CHECK((yyjson_get_int(yyjson_obj_get(auth.root(), "protocol_version")) == 2));

    result &= CHECK(server.sendText(R"(40{"sid":"sio-test"})"));
    result &= CHECK(server.sendEvent("connection_successful", "{}"));
    result &= CHECK(log.waitFor("connection_successful"));

    JsonDoc args;
    result &= CHECK(server.expectEvent("freq_change", args));
    result &= CHECK((yyjson_get_uint(yyjson_obj_get(args.root(), "freq")) == 14236000ULL));
    result &= CHECK(server.expectEvent("tx_report", args));
    result &= CHECK((getStr(args.root(), "mode") == "RADEV1"));
    result &= CHECK(yyjson_is_false(yyjson_obj_get(args.root(), "transmitting")));
    result &= CHECK(server.expectEvent("message_update", args));
    result &= CHECK((getStr(args.root(), "message") == "hello from test"));

    return report(result);
}

bool testInboundEvents()
{
    std::cout << "Test 2 (server events reach the reporter callbacks): ";

    FakeReporterServer server;
    CallbackLog log;
    FreeDVReporter reporter(server.hostname(), "N1DQ", "CN98", "FreeDV Test", false);
    registerCallbacks(reporter, log);

    bool result = CHECK(server.valid());
    reporter.connect();
    JsonDoc auth;
    result &= CHECK(server.completeHandshake(auth));

    result &= CHECK(server.sendEvent("new_connection",
        R"({"sid":"s1","last_update":"u","callsign":"K6AQ","grid_square":"DM12","version":"2.0",)"
        R"("rx_only":true,"connect_time":"t0"})"));
    result &= CHECK(server.sendEvent("tx_report",
        R"({"sid":"s1","last_update":"u","callsign":"K6AQ","grid_square":"DM12","mode":"RADEV1",)"
        R"("transmitting":true,"last_tx":null})"));
    result &= CHECK(server.sendEvent("rx_report",
        R"({"sid":"s1","last_update":"u","receiver_callsign":"K6AQ","receiver_grid_square":"DM12",)"
        R"("callsign":"N1DQ","snr":7,"mode":"RADEV1"})"));
    result &= CHECK(server.sendEvent("freq_change",
        R"({"sid":"s1","last_update":"u","callsign":"K6AQ","grid_square":"DM12","freq":7177000})"));
    result &= CHECK(server.sendEvent("message_update", R"({"sid":"s1","last_update":"u","message":"CQ CQ"})"));
    result &= CHECK(server.sendEvent("qsy_request", R"({"callsign":"K6AQ","frequency":14236000,"message":"QSY?"})"));
    result &= CHECK(server.sendEvent("remove_connection",
        R"({"sid":"s1","last_update":"u","callsign":"K6AQ","grid_square":"DM12","version":"2.0","rx_only":true})"));

    result &= CHECK(log.waitFor("user_connect s1 K6AQ DM12 2.0 rx_only t0"));
    result &= CHECK(log.waitFor("tx_report s1 RADEV1 tx []"));
    result &= CHECK(log.waitFor("rx_report s1 K6AQ N1DQ 7 RADEV1"));
    result &= CHECK(log.waitFor("freq_change s1 K6AQ 7177000"));
    result &= CHECK(log.waitFor("message_update s1 CQ CQ"));
    result &= CHECK(log.waitFor("qsy_request K6AQ 14236000 QSY?"));
    result &= CHECK(log.waitFor("user_disconnect s1 K6AQ"));
    result &= CHECK(log.waitFor("recv_end"));

    // bulk_update replays a batch of events through the same handlers.
    result &= CHECK(server.sendEvent("bulk_update",
        R"([["new_connection",{"sid":"s2","last_update":"u","callsign":"VK3TPM","grid_square":"QF22",)"
        R"("version":"2.0","rx_only":false,"connect_time":"t1"}],)"
        R"(["freq_change",{"sid":"s2","last_update":"u","callsign":"VK3TPM","grid_square":"QF22","freq":14236000}]])"));
    result &= CHECK(log.waitFor("user_connect s2 VK3TPM QF22 2.0 tx t1"));
    result &= CHECK(log.waitFor("freq_change s2 VK3TPM 14236000"));

    // Junk the client should shrug off: unknown event, unparseable JSON,
    // non-string event name, and socket.io/engine.io packet types it ignores.
    result &= CHECK(server.sendEvent("no_such_event", "{}"));
    result &= CHECK(server.sendText("42this is not json"));
    result &= CHECK(server.sendText("42[123]"));
    result &= CHECK(server.sendText("43[]"));
    result &= CHECK(server.sendText("6"));

    // The connection must still be healthy afterwards.
    result &= CHECK(server.sendEvent("message_update", R"({"sid":"s3","last_update":"u","message":"still here"})"));
    result &= CHECK(log.waitFor("message_update s3 still here"));
    result &= CHECK((log.count("reporter_disconnect") == 0));

    return report(result);
}

bool testOutboundReports()
{
    std::cout << "Test 3 (reporter updates are emitted to the server): ";

    FakeReporterServer server;
    CallbackLog log;
    FreeDVReporter reporter(server.hostname(), "N1DQ", "CN98", "FreeDV Test", false);
    registerCallbacks(reporter, log);

    bool result = CHECK(server.valid());
    reporter.connect();
    JsonDoc auth;
    result &= CHECK(server.completeHandshake(auth));
    result &= CHECK(server.sendEvent("connection_successful", "{}"));
    result &= CHECK(log.waitFor("connection_successful"));
    result &= CHECK(server.expectEvent("message_update")); // end of the initial replay
    server.skipped.clear();

    JsonDoc args;
    reporter.freqChange(7177000);
    result &= CHECK(server.expectEvent("freq_change", args));
    result &= CHECK((yyjson_get_uint(yyjson_obj_get(args.root(), "freq")) == 7177000ULL));

    reporter.transmit("RADEV1", true);
    result &= CHECK(server.expectEvent("tx_report", args));
    result &= CHECK(yyjson_is_true(yyjson_obj_get(args.root(), "transmitting")));

    reporter.updateMessage("testing 1 2 3");
    result &= CHECK(server.expectEvent("message_update", args));
    result &= CHECK((getStr(args.root(), "message") == "testing 1 2 3"));

    reporter.addReceiveRecord("K6AQ", "RADEV1", 7177000, -3);
    result &= CHECK(server.expectEvent("rx_report", args));
    result &= CHECK((getStr(args.root(), "callsign") == "K6AQ"));
    result &= CHECK((getStr(args.root(), "mode") == "RADEV1"));
    result &= CHECK((yyjson_get_int(yyjson_obj_get(args.root(), "snr")) == -3));

    reporter.requestQSY("sid-9", 14236000, "meet me on 20m");
    result &= CHECK(server.expectEvent("qsy_request", args));
    result &= CHECK((getStr(args.root(), "dest_sid") == "sid-9"));
    result &= CHECK((getStr(args.root(), "message") == "meet me on 20m"));
    result &= CHECK((yyjson_get_uint(yyjson_obj_get(args.root(), "frequency")) == 14236000ULL));

    // Analog mode hides us; going back to digital shows us and re-reports state.
    reporter.inAnalogMode(true);
    result &= CHECK(server.expectEvent("hide_self", args));
    result &= CHECK((args.doc == nullptr));

    reporter.inAnalogMode(false);
    result &= CHECK(log.waitFor("about_to_show_self"));
    result &= CHECK(server.expectEvent("show_self"));
    result &= CHECK(server.expectEvent("freq_change", args));
    result &= CHECK((yyjson_get_uint(yyjson_obj_get(args.root(), "freq")) == 7177000ULL));
    result &= CHECK(server.expectEvent("tx_report"));
    result &= CHECK(server.expectEvent("message_update", args));
    result &= CHECK((getStr(args.root(), "message") == "testing 1 2 3"));

    // Nothing unexpected should have been sent along the way.
    result &= CHECK(server.skipped.empty());

    return report(result);
}

bool testPingPong()
{
    std::cout << "Test 4 (engine.io ping is answered with pong): ";

    FakeReporterServer server;
    CallbackLog log;
    FreeDVReporter reporter(server.hostname(), "N1DQ", "CN98", "FreeDV Test", false);
    registerCallbacks(reporter, log);

    bool result = CHECK(server.valid());
    reporter.connect();
    JsonDoc auth;
    result &= CHECK(server.completeHandshake(auth));

    std::string payload;
    for (int i = 0; i < 3; i++)
    {
        result &= CHECK(server.sendText("2"));
        result &= CHECK(server.recvText(payload));
        result &= CHECK((payload == "3"));
    }

    return report(result);
}

bool testReconnectWhileHidden()
{
    std::cout << "Test 5 (server close, reconnect, hidden state restored): ";

    FakeReporterServer server;
    CallbackLog log;
    FreeDVReporter reporter(server.hostname(), "N1DQ", "CN98", "FreeDV Test", false);
    registerCallbacks(reporter, log);

    bool result = CHECK(server.valid());
    reporter.connect();
    JsonDoc auth;
    result &= CHECK(server.completeHandshake(auth));
    result &= CHECK(server.sendEvent("connection_successful", "{}"));
    result &= CHECK(server.expectEvent("message_update")); // end of the initial replay
    server.skipped.clear();

    reporter.hideFromView();
    result &= CHECK(server.expectEvent("hide_self"));

    // engine.io CLOSE from the server: the client must disconnect...
    result &= CHECK(server.sendText("1"));
    result &= CHECK(log.waitFor("reporter_disconnect"));
    result &= CHECK(server.waitForClientClose());

    // ...and come back on its own, re-authenticating.
    result &= CHECK(server.completeHandshake(auth, RECONNECT_ACCEPT_TIMEOUT_MS));
    result &= CHECK((getStr(auth.root(), "callsign") == "N1DQ"));

    // Because we were hidden, only hide_self is replayed (no freq/tx/message).
    result &= CHECK(server.sendEvent("connection_successful", "{}"));
    result &= CHECK(server.expectEvent("hide_self"));
    result &= CHECK((log.count("connection_successful") == 2));
    result &= CHECK(server.skipped.empty());

    return report(result);
}

bool testServerInitiatedDisconnects()
{
    std::cout << "Test 6 (namespace error, invalid data and dropped socket disconnect): ";

    bool result = true;

    // socket.io CONNECT_ERROR ("44").
    {
        FakeReporterServer server;
        CallbackLog log;
        FreeDVReporter reporter(server.hostname(), "N1DQ", "CN98", "FreeDV Test", false);
        registerCallbacks(reporter, log);
        reporter.connect();
        JsonDoc auth;
        result &= CHECK(server.acceptWebSocket());
        result &= CHECK(server.openAndReadAuth(auth));
        result &= CHECK(server.sendText(R"(44{"message":"not authorized"})"));
        result &= CHECK(log.waitFor("reporter_disconnect"));
    }

    // engine.io packet that isn't a digit.
    {
        FakeReporterServer server;
        CallbackLog log;
        FreeDVReporter reporter(server.hostname(), "N1DQ", "CN98", "FreeDV Test", false);
        registerCallbacks(reporter, log);
        reporter.connect();
        JsonDoc auth;
        result &= CHECK(server.completeHandshake(auth));
        result &= CHECK(server.sendText("garbage"));
        result &= CHECK(log.waitFor("reporter_disconnect"));
    }

    // TCP connection dropped without any WebSocket/engine.io close.
    {
        FakeReporterServer server;
        CallbackLog log;
        FreeDVReporter reporter(server.hostname(), "N1DQ", "CN98", "FreeDV Test", false);
        registerCallbacks(reporter, log);
        reporter.connect();
        JsonDoc auth;
        result &= CHECK(server.completeHandshake(auth));
        server.dropConnection();
        result &= CHECK(log.waitFor("reporter_disconnect"));
    }

    return report(result);
}

bool testPingTimeout()
{
    std::cout << "Test 7 (missing server pings time out the connection): ";

    FakeReporterServer server;
    CallbackLog log;
    FreeDVReporter reporter(server.hostname(), "N1DQ", "CN98", "FreeDV Test", false);
    registerCallbacks(reporter, log);

    bool result = CHECK(server.valid());
    reporter.connect();
    JsonDoc auth;
    result &= CHECK(server.acceptWebSocket());
    // Ping interval + timeout of 300 ms, after which we never ping.
    result &= CHECK(server.openAndReadAuth(auth,
        R"(0{"sid":"eio-test","upgrades":[],"pingInterval":200,"pingTimeout":100,"maxPayload":1000000})"));
    result &= CHECK(server.sendText(R"(40{"sid":"sio-test"})"));

    auto start = std::chrono::steady_clock::now();
    result &= CHECK(log.waitFor("reporter_disconnect"));
    result &= CHECK((std::chrono::steady_clock::now() - start < 3s));

    return report(result);
}

bool testAlternateRoles()
{
    std::cout << "Test 8 (view-only and write-only auth roles): ";

    bool result = true;

    // No callsign/grid: connects as a viewer and sends no station details.
    {
        FakeReporterServer server;
        FreeDVReporter reporter(server.hostname(), "", "", "FreeDV Test", false);
        reporter.connect();
        JsonDoc auth;
        result &= CHECK(server.acceptWebSocket());
        result &= CHECK(server.openAndReadAuth(auth));
        result &= CHECK((getStr(auth.root(), "role") == "view"));
        result &= CHECK((yyjson_obj_get(auth.root(), "callsign") == nullptr));
        result &= CHECK((yyjson_get_int(yyjson_obj_get(auth.root(), "protocol_version")) == 2));
    }

    // writeOnly reporters use the report_wo role; rx_only is passed through.
    {
        FakeReporterServer server;
        FreeDVReporter reporter(server.hostname(), "N1DQ", "CN98", "FreeDV Test", true, true);
        reporter.connect();
        JsonDoc auth;
        result &= CHECK(server.acceptWebSocket());
        result &= CHECK(server.openAndReadAuth(auth));
        result &= CHECK((getStr(auth.root(), "role") == "report_wo"));
        result &= CHECK(yyjson_is_true(yyjson_obj_get(auth.root(), "rx_only")));
    }

    return report(result);
}

#if defined(ENABLE_TLS_SUPPORT)
bool testSecureConnection(TestCertificate& cert)
{
    std::cout << "Test 9 (secure connection: handshake and events over TLS): ";

    // freedv-gui connects securely by default.
    testSetEnv("SSL_CERT_FILE", cert.path.c_str());
    FakeReporterServer server(cert);
    CallbackLog log;
    FreeDVReporter reporter(server.hostname(), "N1DQ", "CN98", "FreeDV Test", false, false, true);
    registerCallbacks(reporter, log);
    reporter.freqChange(14236000);

    bool result = CHECK(server.valid());
    reporter.connect();
    JsonDoc auth;
    result &= CHECK(server.completeHandshake(auth));
    result &= CHECK((getStr(auth.root(), "callsign") == "N1DQ"));

    result &= CHECK(server.sendEvent("connection_successful", "{}"));
    result &= CHECK(log.waitFor("connection_successful"));
    JsonDoc args;
    result &= CHECK(server.expectEvent("freq_change", args));
    result &= CHECK((yyjson_get_uint(yyjson_obj_get(args.root(), "freq")) == 14236000ULL));

    result &= CHECK(server.sendEvent("message_update", R"({"sid":"s1","last_update":"u","message":"over TLS"})"));
    result &= CHECK(log.waitFor("message_update s1 over TLS"));

    // A large event (several TLS records) arrives intact.
    std::string longMessage(20000, 'm');
    result &= CHECK(server.sendEvent("message_update", R"({"sid":"s2","last_update":"u","message":")" + longMessage + R"("})"));
    result &= CHECK(log.waitFor("message_update s2 " + longMessage));

    return report(result);
}

bool testSecureConnectionRejectsUntrustedServer(TestCertificate& cert)
{
    std::cout << "Test 10 (secure connection refuses a server it doesn't trust): ";

    // Trust nothing: the server's certificate can't be verified, so the
    // reporter must not get as far as sending its WebSocket request.
    std::string emptyTrustFile = testMakeTempFile("fdvnone");
    testSetEnv("SSL_CERT_FILE", emptyTrustFile.c_str());
    FakeReporterServer server(cert);
    CallbackLog log;
    {
        FreeDVReporter reporter(server.hostname(), "N1DQ", "CN98", "FreeDV Test", false, false, true);
        registerCallbacks(reporter, log);

        bool result = CHECK(server.valid());
        reporter.connect();
        result &= CHECK(!server.acceptWebSocket());
        result &= CHECK(log.count("connection_successful") == 0);
        std::remove(emptyTrustFile.c_str());
        testSetEnv("SSL_CERT_FILE", cert.path.c_str());
        return report(result);
    }
}
#endif // defined(ENABLE_TLS_SUPPORT)
bool testDropWithoutReporterDisconnectCallback()
{
    std::cout << "Test 11 (dropped socket clears the connected gate with no reporter callback): ";

    FakeReporterServer server;
    CallbackLog log;
    FreeDVReporter reporter(server.hostname(), "N1DQ", "CN98", "FreeDV Test", false);

    // Deliberately no setOnReporterDisconnectFn(): the reporter's own cleanup
    // has to run whether or not the application listens for it.
    reporter.setConnectionSuccessfulFn([&]() { log.add("connection_successful"); });

    bool result = CHECK(server.valid());
    reporter.connect();
    JsonDoc auth;
    result &= CHECK(server.completeHandshake(auth));
    result &= CHECK(server.sendEvent("connection_successful", "{}"));
    result &= CHECK(log.waitFor("connection_successful"));
    result &= CHECK(FreeDVReporterTest::fullyConnected(reporter));

    server.dropConnection();

    // The only witness is the gate the internal handler drops.
    bool gateCleared = false;
    for (int i = 0; i < 50 && !gateCleared; i++)
    {
        gateCleared = !FreeDVReporterTest::fullyConnected(reporter);
        if (!gateCleared) std::this_thread::sleep_for(20ms);
    }
    result &= CHECK(gateCleared);
    result &= CHECK((log.count("reporter_disconnect") == 0));

    return report(result);
}

bool testNonTextWebsocketFrameIgnored()
{
    std::cout << "Test 12 (binary websocket frame shrugged off, connection stays healthy): ";

    FakeReporterServer server;
    CallbackLog log;
    FreeDVReporter reporter(server.hostname(), "N1DQ", "CN98", "FreeDV Test", false);
    registerCallbacks(reporter, log);

    bool result = CHECK(server.valid());
    reporter.connect();
    JsonDoc auth;
    result &= CHECK(server.completeHandshake(auth));
    result &= CHECK(server.sendEvent("connection_successful", "{}"));
    result &= CHECK(log.waitFor("connection_successful"));

    // A non-text (binary, opcode 2) frame arrives on the websocket layer.
    // The client must not feed it into the engine.io parser and must not
    // disconnect.
    result &= CHECK(server.sendBinary("bin\x01\x02junk"));

    // engine.io pings are still answered (skipping any status frames the
    // reporter pushes on its own schedule)...
    std::string payload;
    bool pongSeen = false;
    result &= CHECK(server.sendText("2"));
    for (int i = 0; i < 8 && !pongSeen; i++)
    {
        result &= CHECK(server.recvText(payload));
        pongSeen = (payload == "3");
    }
    result &= CHECK(pongSeen);

    // ...and events still reach the reporter callbacks.
    result &= CHECK(server.sendEvent("message_update", R"({"sid":"s12","last_update":"u","message":"after binary"})"));
    result &= CHECK(log.waitFor("message_update s12 after binary"));
    result &= CHECK((log.count("reporter_disconnect") == 0));

    return report(result);
}

bool testRawClientNoConnectFnAndNamelessEvent()
{
    std::cout << "Test 13 (raw SocketIoClient with no onConnect callback; '42[]' skipped): ";

    FakeReporterServer server;
    bool result = CHECK(server.valid());

    SocketIoClient client;
    std::atomic<bool> disconnectHappened{false};
    std::atomic<bool> eventSeen{false};
    client.setOnDisconnectFn([&]() { disconnectHappened.store(true); });
    client.on("probe_event", [&](yyjson_val*) { eventSeen.store(true); });

    // Minimal auth payload so the CONNECT packet parses; deliberately no
    // setOnConnectFn(): the socket.io CONNECT ack ("40{...}") must be
    // handled with a null onConnectFn_.
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* authJson = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, authJson);
    yyjson_mut_obj_add_str(doc, authJson, "role", "report");
    client.setAuthDictionary(doc);

    client.connect("127.0.0.1", server.port(), false, false);

    JsonDoc gotAuth;
    result &= CHECK(server.acceptWebSocket());
    result &= CHECK(server.openAndReadAuth(gotAuth));
    result &= CHECK((getStr(gotAuth.root(), "role") == "report"));
    result &= CHECK(server.sendText(R"(40{"sid":"sio-test"})"));

    // The client is alive despite the missing callback: engine.io pings get
    // answered.
    std::string payload;
    result &= CHECK(server.sendText("2"));
    result &= CHECK(server.recvText(payload));
    result &= CHECK((payload == "3"));

    // A socket.io event with no name at all: '42[]' parses, but the first
    // array entry is null; the client must skip it and keep serving events.
    result &= CHECK(server.sendText("42[]"));
    result &= CHECK(server.sendEvent("probe_event", "{}"));

    bool seen = false;
    for (int i = 0; i < 250 && !seen; i++)
    {
        seen = eventSeen.load();
        if (!seen) std::this_thread::sleep_for(20ms);
    }
    result &= CHECK(seen);
    result &= CHECK((disconnectHappened.load() == false));

    return report(result);
}


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

    result &= testHandshakeAndInitialReport();
    result &= testInboundEvents();
    result &= testOutboundReports();
    result &= testPingPong();
    result &= testReconnectWhileHidden();
    result &= testServerInitiatedDisconnects();
    result &= testPingTimeout();
    result &= testAlternateRoles();

#if defined(ENABLE_TLS_SUPPORT)
    TestCertificate cert;
    if (!cert.generate())
    {
        std::cout << "Could not generate test certificate\n";
        return -1;
    }
    result &= testSecureConnection(cert);
    result &= testSecureConnectionRejectsUntrustedServer(cert);
#endif // defined(ENABLE_TLS_SUPPORT)

    result &= testDropWithoutReporterDisconnectCallback();
    result &= testNonTextWebsocketFrameIgnored();
    result &= testRawClientNoConnectFnAndNamelessEvent();

    return result ? 0 : -1;
}
