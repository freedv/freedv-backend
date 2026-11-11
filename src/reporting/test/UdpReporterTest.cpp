#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include "../../util/test/TestSocketCompat.h"

#include "../UdpReporter.h"
#include "../../3rdparty/yyjson/yyjson.h"

namespace {

// Creates a UDP socket bound to an ephemeral port on loopback, ready to
// receive whatever UdpReporter sends to that port. Returns the fd and
// writes the chosen port to outPort (-1 on failure).
int makeLoopbackReceiver(int& outPort)
{
    int fd = testOpenSocket(AF_INET, SOCK_DGRAM);
    if (fd < 0)
    {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // let the OS pick an ephemeral port

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        testCloseSocket(fd);
        return -1;
    }

    socklen_t addrLen = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &addrLen) < 0)
    {
        testCloseSocket(fd);
        return -1;
    }
    outPort = ntohs(addr.sin_port);

    // Never block forever if a datagram is unexpectedly missing.
    testSetRecvTimeout(fd, 2000);

    return fd;
}

bool isIso8601Utc(const std::string& value)
{
    // "YYYY-MM-DDTHH:MM:SSZ"
    if (value.size() != 20)
    {
        return false;
    }
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    return isDigit(value[0]) && isDigit(value[1]) && isDigit(value[2]) && isDigit(value[3]) &&
           value[4] == '-' &&
           isDigit(value[5]) && isDigit(value[6]) &&
           value[7] == '-' &&
           isDigit(value[8]) && isDigit(value[9]) &&
           value[10] == 'T' &&
           isDigit(value[11]) && isDigit(value[12]) &&
           value[13] == ':' &&
           isDigit(value[14]) && isDigit(value[15]) &&
           value[16] == ':' &&
           isDigit(value[17]) && isDigit(value[18]) &&
           value[19] == 'Z';
}

// Receives one datagram and validates the full fdv_callsign JSON schema.
bool receiveAndValidateRecord(int fd, const char* expectCallsign, const char* expectMode,
                             int expectSnr, uint64_t expectFreqHz)
{
    char buf[4096];
    ssize_t len = recv(fd, buf, sizeof(buf) - 1, 0);
    if (len <= 0)
    {
        return false;
    }
    buf[len] = '\0';

    yyjson_doc* doc = yyjson_read(buf, static_cast<size_t>(len), 0);
    if (doc == nullptr)
    {
        return false;
    }

    yyjson_val* root = yyjson_doc_get_root(doc);
    bool result = true;

    auto type = yyjson_obj_get(root, "type");
    auto version = yyjson_obj_get(root, "version");
    auto timestamp = yyjson_obj_get(root, "timestamp");
    auto callsign = yyjson_obj_get(root, "callsign");
    auto mode = yyjson_obj_get(root, "mode");
    auto snr = yyjson_obj_get(root, "snr");
    auto freq = yyjson_obj_get(root, "frequency_hz");

    result &= yyjson_is_str(type) && std::strcmp(yyjson_get_str(type), "fdv_callsign") == 0;
    result &= yyjson_is_int(version) && yyjson_get_int(version) == 1;
    result &= yyjson_is_str(timestamp) && isIso8601Utc(yyjson_get_str(timestamp));
    result &= yyjson_is_str(callsign) && std::strcmp(yyjson_get_str(callsign), expectCallsign) == 0;
    result &= yyjson_is_str(mode) && std::strcmp(yyjson_get_str(mode), expectMode) == 0;
    result &= yyjson_is_int(snr) && yyjson_get_int(snr) == expectSnr;
    result &= yyjson_is_uint(freq) && yyjson_get_uint(freq) == expectFreqHz;

    yyjson_doc_free(doc);
    return result;
}

bool testReceiveRecordProducesValidJson()
{
    std::cout << "Test 1 (addReceiveRecord sends valid fdv_callsign JSON): ";

    int port = 0;
    int fd = makeLoopbackReceiver(port);
    bool result = (fd >= 0);

    if (result)
    {
        {
            UdpReporter reporter("127.0.0.1", port);
            result &= receiveAndValidateRecord(fd, "N1DQ", "1600X", -5, 14236000ULL) == false; // nothing sent yet
            reporter.addReceiveRecord("N1DQ", "1600X", 14236000, -5);
            result &= receiveAndValidateRecord(fd, "N1DQ", "1600X", -5, 14236000ULL);
        }
        testCloseSocket(fd);
    }

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testNonRecordMethodsSendNothing()
{
    std::cout << "Test 2 (freqChange/transmit/send/inAnalogMode send no datagrams): ";

    int port = 0;
    int fd = makeLoopbackReceiver(port);
    bool result = (fd >= 0);

    if (result)
    {
        {
            UdpReporter reporter("127.0.0.1", port);
            reporter.freqChange(7100000);
            reporter.transmit("1600X", true);
            reporter.inAnalogMode(true);
            reporter.send();

            char buf[4096];
            ssize_t len = recv(fd, buf, sizeof(buf), 0); // SO_RCVTIMEO bounds the wait
            result &= (len < 0) && testLastRecvTimedOut();
        }
        testCloseSocket(fd);
    }

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testRecordFrequencyFollowsArgumentNotFreqChange()
{
    std::cout << "Test 3 (record uses its own frequency, not freqChange's): ";

    int port = 0;
    int fd = makeLoopbackReceiver(port);
    bool result = (fd >= 0);

    if (result)
    {
        {
            UdpReporter reporter("127.0.0.1", port);
            reporter.freqChange(7100000);
            reporter.addReceiveRecord("K6AQ", "8PSK1250", 7074000, 12);
            result &= receiveAndValidateRecord(fd, "K6AQ", "8PSK1250", 12, 7074000ULL);
        }
        testCloseSocket(fd);
    }

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}
bool testRecordWithInvalidUtf8IsNotSent()
{
    std::cout << "Test 4 (a record that cannot be serialised is dropped, not sent): ";

    int port = 0;
    int fd = makeLoopbackReceiver(port);
    bool result = (fd >= 0);

    if (result)
    {
        // Shorter than the harness default: this test only waits for a
        // datagram that must not arrive.
        testSetRecvTimeout(fd, 500);
        {
            UdpReporter reporter("127.0.0.1", port);

            // 0xFF 0xFE is not valid UTF-8, so yyjson refuses to serialise
            // the record; UdpReporter must drop it rather than emit a
            // malformed or empty datagram.
            reporter.addReceiveRecord("\xff\xfe", "1600X", 14236000, 3);

            char buf[4096];
            ssize_t len = recv(fd, buf, sizeof(buf), 0);
            result &= (len < 0) && testLastRecvTimedOut();

            // The same reporter still delivers a well formed record.
            reporter.addReceiveRecord("N1DQ", "1600X", 14236000, -5);
            result &= receiveAndValidateRecord(fd, "N1DQ", "1600X", -5, 14236000ULL);
        }
        testCloseSocket(fd);
    }

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}


} // namespace

int main(int argc, char** argv)
{
    bool result = true;

    result &= testReceiveRecordProducesValidJson();
    result &= testNonRecordMethodsSendNothing();
    result &= testRecordFrequencyFollowsArgumentNotFreqChange();
    result &= testRecordWithInvalidUtf8IsNotSent();

    return result ? 0 : -1;
}
