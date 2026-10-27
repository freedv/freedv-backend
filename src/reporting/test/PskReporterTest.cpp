#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <sys/resource.h>

#include "../../util/test/TestSocketCompat.h"

#include "../pskreporter.h"

// Grants access to PskReporter's private encoding helpers so the wire format
// can be verified without depending on network reachability of the real
// PSK Reporter server. Kept at namespace scope to match the friend
// declaration in pskreporter.h.
class PskReporterTest
{
public:
    static int rxDataSize(PskReporter& reporter) { return reporter.getRxDataSize_(); }
    static int txDataSize(PskReporter& reporter) { return reporter.getTxDataSize_(); }
    static void encodeRx(PskReporter& reporter, char* buf) { reporter.encodeReceiverRecord_(buf); }
    static void encodeTx(PskReporter& reporter, char* buf) { reporter.encodeSenderRecords_(buf); }
    static bool reportCommon(PskReporter& reporter) { return reporter.reportCommon_(); }
    static void clearRecords(PskReporter& reporter) { reporter.recordList_.clear(); }
    static size_t recordCount(PskReporter& reporter)
    {
        std::unique_lock<std::mutex> lock(reporter.recordListMutex_);
        return reporter.recordList_.size();
    }
    static void setServer(PskReporter& reporter, const std::string& host, int port)
    {
        reporter.serverHostname_ = host;
        reporter.serverPort_ = std::to_string(port);
    }
    static void setServerName(PskReporter& reporter, const std::string& host, const std::string& port)
    {
        reporter.serverHostname_ = host;
        reporter.serverPort_ = port;
    }
};

namespace {

// UDP socket on an ephemeral 127.0.0.1 port standing in for the PSK Reporter
// server, so tests that send reports never touch the network.
class FakePskServer
{
public:
    FakePskServer()
        : fd_(testOpenSocket(AF_INET, SOCK_DGRAM))
        , port_(-1)
    {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t addrLen = sizeof(addr);
        if (fd_ >= 0 &&
            bind(fd_, (struct sockaddr*)&addr, sizeof(addr)) == 0 &&
            getsockname(fd_, (struct sockaddr*)&addr, &addrLen) == 0)
        {
            port_ = ntohs(addr.sin_port);
        }
    }

    ~FakePskServer()
    {
        if (fd_ >= 0) testCloseSocket(fd_);
    }

    int port() const { return port_; }

    // Returns the next datagram, or an empty string if none arrives in time.
    std::string receive(int timeoutMs)
    {
        if (!testWaitReadable(fd_, timeoutMs))
        {
            return "";
        }
        char buf[65536];
        ssize_t len = recv(fd_, buf, sizeof(buf), 0);
        return len > 0 ? std::string(buf, len) : "";
    }

private:
    int fd_;
    int port_;
};

size_t countOccurrences(const std::string& haystack, const std::string& needle)
{
    size_t count = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1))
    {
        count++;
    }
    return count;
}

uint32_t readU32(const std::string& data, size_t offset)
{
    return ((uint32_t)(unsigned char)data[offset] << 24) | ((uint32_t)(unsigned char)data[offset + 1] << 16) |
           ((uint32_t)(unsigned char)data[offset + 2] << 8) | (uint32_t)(unsigned char)data[offset + 3];
}

// Checks the IPFIX message header: version 0x000A and a length field that
// matches the datagram.
bool validHeader(const std::string& datagram)
{
    return datagram.size() > 16 &&
           datagram[0] == 0x00 && datagram[1] == 0x0A &&
           (size_t)(((unsigned char)datagram[2] << 8) | (unsigned char)datagram[3]) == datagram.size();
}

uint64_t readBigEndian(const char* p, int bytes)
{
    uint64_t value = 0;
    for (int i = 0; i < bytes; i++)
    {
        value = (value << 8) | static_cast<unsigned char>(p[i]);
    }
    return value;
}

bool testSenderRecordEncoding()
{
    std::cout << "Test 1 (SenderRecord size and wire encoding): ";

    uint64_t freqHz = 14236000;
    SenderRecord record("N1DQ", freqHz, -5);
    bool result = (record.recordSize() == 23);

    char buf[64];
    memset(buf, 0, sizeof(buf));
    record.encode(buf);

    // callsign: length-prefixed
    result &= (static_cast<unsigned char>(buf[0]) == 4);
    result &= (memcmp(buf + 1, "N1DQ", 4) == 0);

    // frequency: 5-byte big-endian
    result &= (readBigEndian(buf + 5, 5) == freqHz);

    // SNR: one signed byte
    result &= (buf[10] == static_cast<char>(-5));

    // mode: length-prefixed "FREEDV"
    result &= (static_cast<unsigned char>(buf[11]) == 6);
    result &= (memcmp(buf + 12, "FREEDV", 6) == 0);

    // info source
    result &= (static_cast<unsigned char>(buf[18]) == 1);

    // flow start time: 4-byte big-endian, close to now
    uint32_t flowTime = static_cast<uint32_t>(readBigEndian(buf + 19, 4));
    uint64_t now = static_cast<uint64_t>(time(nullptr));
    result &= (flowTime <= now + 2) && (flowTime + 10 >= now);

    // Frequencies above 32 bits must use all 5 frequency bytes.
    SenderRecord big("W1AW", 10000000000ULL, 0);
    memset(buf, 0, sizeof(buf));
    big.encode(buf);
    result &= (readBigEndian(buf + 5, 5) == 10000000000ULL);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testRxDataSizePadding()
{
    std::cout << "Test 2 (RX record data size pads to 4-byte boundary): ";

    // 4 + (1+4) + (1+6) + (1+10) = 27 -> padded to 28.
    PskReporter aligned("K6AQ", "DM12kw", "FreeDV 1.3");
    bool result = (PskReporterTest::rxDataSize(aligned) == 28);

    // 4 + (1+4) + (1+6) + (1+6) = 23 -> padded to 24.
    PskReporter padded("K6AQ", "DM12kw", "FreeDV");
    result &= (PskReporterTest::rxDataSize(padded) == 24);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testReceiverRecordEncoding()
{
    std::cout << "Test 3 (receiver record header and fields): ";

    PskReporter reporter("K6AQ", "DM12kw", "FreeDV 1.3");
    int size = PskReporterTest::rxDataSize(reporter);
    bool result = (size == 28);

    char buf[64];
    memset(buf, 0, sizeof(buf));
    PskReporterTest::encodeRx(reporter, buf);

    result &= (static_cast<unsigned char>(buf[0]) == 0x99);
    result &= (static_cast<unsigned char>(buf[1]) == 0x92);
    result &= (readBigEndian(buf + 2, 2) == static_cast<uint64_t>(size));

    result &= (static_cast<unsigned char>(buf[4]) == 4);
    result &= (memcmp(buf + 5, "K6AQ", 4) == 0);
    result &= (static_cast<unsigned char>(buf[9]) == 6);
    result &= (memcmp(buf + 10, "DM12kw", 6) == 0);
    result &= (static_cast<unsigned char>(buf[16]) == 10);
    result &= (memcmp(buf + 17, "FreeDV 1.3", 10) == 0);

    // Padding bytes up to the aligned size must be zero.
    result &= (buf[27] == 0);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testSenderRecordsEncoding()
{
    std::cout << "Test 4 (TX record list size and encoding): ";

    PskReporter reporter("K6AQ", "DM12kw", "FreeDV 1.3");
    bool result = (PskReporterTest::txDataSize(reporter) == 0);

    uint64_t freqHz = 14236000;
    reporter.addReceiveRecord("N1DQ", "1600X", freqHz, -5);

    // 4 (header+size) + 23 (one record) = 27 -> padded to 28.
    int size = PskReporterTest::txDataSize(reporter);
    result &= (size == 28);

    char buf[64];
    memset(buf, 0, sizeof(buf));
    PskReporterTest::encodeTx(reporter, buf);

    result &= (static_cast<unsigned char>(buf[0]) == 0x99);
    result &= (static_cast<unsigned char>(buf[1]) == 0x93);
    result &= (readBigEndian(buf + 2, 2) == static_cast<uint64_t>(size));

    // The single sender record follows the SenderRecord layout from Test 1.
    result &= (static_cast<unsigned char>(buf[4]) == 4);
    result &= (memcmp(buf + 5, "N1DQ", 4) == 0);
    result &= (readBigEndian(buf + 9, 5) == freqHz);
    result &= (buf[14] == static_cast<char>(-5));
    result &= (static_cast<unsigned char>(buf[15]) == 6);
    result &= (memcmp(buf + 16, "FREEDV", 6) == 0);
    result &= (static_cast<unsigned char>(buf[22]) == 1);

    // Clear before destruction so ~PskReporter doesn't fire a duplicate
    // network report.
    PskReporterTest::clearRecords(reporter);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testReportCommonClearsPendingRecords()
{
    std::cout << "Test 5 (reportCommon_ drains the pending record list): ";

    FakePskServer server;
    PskReporter reporter("TEST1", "DM12kw", "FreeDV Test");
    PskReporterTest::setServer(reporter, "127.0.0.1", server.port());
    reporter.addReceiveRecord("TEST2", "1600X", 14236000, 5);
    reporter.addReceiveRecord("TEST3", "1600X", 14236000, 6);
    bool result = (PskReporterTest::recordCount(reporter) == 2);

    result &= PskReporterTest::reportCommon(reporter);
    result &= (PskReporterTest::recordCount(reporter) == 0);

    std::string datagram = server.receive(2000);
    result &= validHeader(datagram);
    result &= (countOccurrences(datagram, "TEST2") == 1);
    result &= (countOccurrences(datagram, "TEST3") == 1);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testAutoFlushThresholdBoundary()
{
    std::cout << "Test 6 (49 records do not trigger an early flush): ";

    PskReporter reporter("TEST1", "DM12kw", "FreeDV Test");
    bool result = true;
    for (int i = 0; i < 49; i++)
    {
        reporter.addReceiveRecord("TEST2", "1600X", 14236000, static_cast<signed char>(i % 50));
    }
    result &= (PskReporterTest::recordCount(reporter) == 49);

    // Clear before destruction to avoid a network send from the destructor.
    PskReporterTest::clearRecords(reporter);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testAutoFlushAtFiftyRecords()
{
    std::cout << "Test 7 (50th record triggers a background flush of all 50): ";

    FakePskServer server;
    PskReporter reporter("TEST1", "DM12kw", "FreeDV Test");
    PskReporterTest::setServer(reporter, "127.0.0.1", server.port());

    bool result = (server.port() > 0);
    for (int i = 0; i < 49; i++)
    {
        reporter.addReceiveRecord("AUTO1", "1600X", 14236000, 5);
    }
    result &= server.receive(300).empty();

    reporter.addReceiveRecord("AUTO1", "1600X", 14236000, 5);
    std::string datagram = server.receive(2000);
    result &= validHeader(datagram);
    result &= (countOccurrences(datagram, "AUTO1") == 50);
    result &= (PskReporterTest::recordCount(reporter) == 0);

    // Records after the flush start a new batch.
    reporter.addReceiveRecord("AUTO2", "1600X", 14236000, 5);
    result &= (PskReporterTest::recordCount(reporter) == 1);
    reporter.send();
    std::string second = server.receive(2000);
    result &= validHeader(second);
    result &= (countOccurrences(second, "AUTO2") == 1);
    result &= (countOccurrences(second, "AUTO1") == 0);

    // Sequence numbers advance between reports (header offset 8).
    result &= (readU32(second, 8) == readU32(datagram, 8) + 1);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testDestroyRightAfterAutoFlush()
{
    std::cout << "Test 8 (destroying the reporter waits for an in-flight flush): ";

    FakePskServer server;
    bool result = (server.port() > 0);

    // Destroy the reporter immediately after the 50th record starts a
    // background send. The send must complete (and must not touch the freed
    // reporter), and the destructor must not report the same records again.
    {
        PskReporter reporter("TEST1", "DM12kw", "FreeDV Test");
        PskReporterTest::setServer(reporter, "127.0.0.1", server.port());
        for (int i = 0; i < 50; i++)
        {
            reporter.addReceiveRecord("GONE1", "1600X", 14236000, 5);
        }
    }

    std::string datagram = server.receive(2000);
    result &= validHeader(datagram);
    result &= (countOccurrences(datagram, "GONE1") == 50);
    result &= server.receive(300).empty();

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testDestructorFlushesPendingRecords()
{
    std::cout << "Test 9 (destructor reports records that were never sent): ";

    FakePskServer server;
    bool result = (server.port() > 0);
    {
        PskReporter reporter("TEST1", "DM12kw", "FreeDV Test");
        PskReporterTest::setServer(reporter, "127.0.0.1", server.port());
        reporter.addReceiveRecord("LAST1", "1600X", 7177000, -3);
    }

    std::string datagram = server.receive(2000);
    result &= validHeader(datagram);
    result &= (countOccurrences(datagram, "LAST1") == 1);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}
bool testRxDataSizeAlignedNoPadding()
{
    std::cout << "Test 10 (RX record data size is used as-is when already 4-byte aligned): ";

    // 4 + (1+4) + (1+6) + (1+7) = 24 -> already aligned, nothing added.
    PskReporter aligned("K6AQ", "DM12kw", "FreeDV1");
    bool result = (PskReporterTest::rxDataSize(aligned) == 24);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testEncodeSenderRecordsWithEmptyList()
{
    std::cout << "Test 11 (encodeSenderRecords_ leaves the buffer alone for an empty list): ";

    PskReporter reporter("K6AQ", "DM12kw", "FreeDV 1.3");
    char buf[32];
    memset(buf, 0x5A, sizeof(buf));

    bool result = (PskReporterTest::txDataSize(reporter) == 0);
    PskReporterTest::encodeTx(reporter, buf);

    bool untouched = true;
    for (size_t i = 0; i < sizeof(buf); i++)
    {
        untouched &= (buf[i] == (char)0x5A);
    }
    result &= untouched;

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testReportCommonWithoutPendingRecords()
{
    std::cout << "Test 12 (reportCommon_ omits the TX format block when nothing is pending): ";

    FakePskServer server;
    PskReporter reporter("TEST1", "DM12kw", "FreeDV Test");
    PskReporterTest::setServer(reporter, "127.0.0.1", server.port());

    bool result = (PskReporterTest::recordCount(reporter) == 0);
    result &= PskReporterTest::reportCommon(reporter);

    std::string datagram = server.receive(2000);
    result &= validHeader(datagram);
    result &= (countOccurrences(datagram, "TEST1") == 1);
    // The SenderRecord format header (0x99 0x93) is only present with TX data.
    result &= (countOccurrences(datagram, std::string("\x99\x93", 2)) == 0);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testReportCommonUnresolvableServer()
{
    std::cout << "Test 13 (reportCommon_ fails when the server name cannot be resolved): ";

    PskReporter reporter("TEST1", "DM12kw", "FreeDV Test");
    reporter.addReceiveRecord("TEST2", "1600X", 14236000, 5);

    // getaddrinfo is called with AI_NUMERICSERV, so a non-numeric service is
    // rejected locally without any name lookup.
    PskReporterTest::setServerName(reporter, "127.0.0.1", "psk-reporter");
    bool result = (PskReporterTest::reportCommon(reporter) == false);

    // Pending records are dropped by the failed attempt rather than retried.
    result &= (PskReporterTest::recordCount(reporter) == 0);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testReportCommonSendFailure()
{
    std::cout << "Test 14 (reportCommon_ fails when the datagram cannot be sent): ";

    PskReporter reporter("TEST1", "DM12kw", "FreeDV Test");
    reporter.addReceiveRecord("TEST2", "1600X", 14236000, 5);

    // Broadcasting to 255.255.255.255 needs SO_BROADCAST, which the reporter
    // never sets, so sendto() fails with EACCES.
    PskReporterTest::setServer(reporter, "255.255.255.255", 39000);
    bool result = (PskReporterTest::reportCommon(reporter) == false);
    result &= (PskReporterTest::recordCount(reporter) == 0);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testReportCommonSocketFailure()
{
    std::cout << "Test 15 (reportCommon_ fails when no descriptor is available): ";

    PskReporter reporter("TEST1", "DM12kw", "FreeDV Test");
    reporter.addReceiveRecord("TEST2", "1600X", 14236000, 5);
    PskReporterTest::setServer(reporter, "127.0.0.1", 39000);

    // Cap the descriptor limit at the three stdio descriptors so socket()
    // returns EMFILE, then put the original limit back.
    struct rlimit original;
    bool result = (getrlimit(RLIMIT_NOFILE, &original) == 0);
    struct rlimit lowered = original;
    lowered.rlim_cur = 3;
    result &= (setrlimit(RLIMIT_NOFILE, &lowered) == 0);

    bool sendFailed = (PskReporterTest::reportCommon(reporter) == false);

    result &= (setrlimit(RLIMIT_NOFILE, &original) == 0);
    result &= sendFailed;

    // A normal report works again once the limit is restored.
    FakePskServer server;
    PskReporterTest::setServer(reporter, "127.0.0.1", server.port());
    reporter.addReceiveRecord("RESTORED", "1600X", 14236000, 5);
    result &= PskReporterTest::reportCommon(reporter);
    result &= (countOccurrences(server.receive(2000), "RESTORED") == 1);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}


} // namespace

int main(int argc, char** argv)
{
    bool result = true;

    result &= testSenderRecordEncoding();
    result &= testRxDataSizePadding();
    result &= testReceiverRecordEncoding();
    result &= testSenderRecordsEncoding();
    result &= testReportCommonClearsPendingRecords();
    result &= testAutoFlushThresholdBoundary();
    result &= testAutoFlushAtFiftyRecords();
    result &= testDestroyRightAfterAutoFlush();
    result &= testDestructorFlushesPendingRecords();
    result &= testRxDataSizeAlignedNoPadding();
    result &= testEncodeSenderRecordsWithEmptyList();
    result &= testReportCommonWithoutPendingRecords();
    result &= testReportCommonUnresolvableServer();
    result &= testReportCommonSendFailure();
    result &= testReportCommonSocketFailure();

    return result ? 0 : -1;
}
