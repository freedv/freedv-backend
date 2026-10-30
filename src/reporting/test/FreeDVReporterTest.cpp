#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "../FreeDVReporter.h"

// Grants access to FreeDVReporter's private event handlers and state so the
// socket.io message plumbing can be exercised without a live server. Kept at
// namespace scope to match the friend declaration in FreeDVReporter.h.
class FreeDVReporterTest
{
public:
    static void fireNewConnection(FreeDVReporter& reporter, yyjson_val* params) { reporter.onFreeDVReporterNewConnection_(params); }
    static void fireConnectionSuccessful(FreeDVReporter& reporter) { reporter.onFreeDVReporterConnectionSuccessful_(nullptr); }
    static void fireRemoveConnection(FreeDVReporter& reporter, yyjson_val* params) { reporter.onFreeDVReporterRemoveConnection_(params); }
    static void fireTxReport(FreeDVReporter& reporter, yyjson_val* params) { reporter.onFreeDVReporterTransmitReport_(params); }
    static void fireRxReport(FreeDVReporter& reporter, yyjson_val* params) { reporter.onFreeDVReporterReceiveReport_(params); }
    static void fireFreqChange(FreeDVReporter& reporter, yyjson_val* params) { reporter.onFreeDVReporterFrequencyChange_(params); }
    static void fireMessageUpdate(FreeDVReporter& reporter, yyjson_val* params) { reporter.onFreeDVReporterMessageUpdate_(params); }
    static void fireQsyRequest(FreeDVReporter& reporter, yyjson_val* params) { reporter.onFreeDVReporterQsyRequest_(params); }
    static void fireBulkUpdate(FreeDVReporter& reporter, yyjson_val* params) { reporter.onFreeDVReporterBulkUpdate_(params); }

    static uint64_t lastFrequency(const FreeDVReporter& reporter) { return reporter.lastFrequency_; }
    static std::string mode(const FreeDVReporter& reporter) { return reporter.mode_; }
    static bool tx(const FreeDVReporter& reporter) { return reporter.tx_; }
    static std::string message(const FreeDVReporter& reporter) { return reporter.message_; }
    static bool hidden(const FreeDVReporter& reporter) { return reporter.hidden_; }
    static bool fullyConnected(const FreeDVReporter& reporter) { return reporter.isFullyConnected_.load(std::memory_order_relaxed); }
    static std::string hostname(const FreeDVReporter& reporter) { return reporter.hostname_; }
};

namespace {
// Parses a JSON payload, hands the root value to the handler under test,
// then frees the document. Handlers must not retain yyjson_val pointers.
template <typename HandlerFn>
void withParsedJson(const char* json, HandlerFn&& handler)
{
    yyjson_doc* doc = yyjson_read(json, strlen(json), 0);
    if (doc == nullptr)
    {
        return;
    }
    handler(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
}

bool testValidForReportingGating()
{
    std::cout << "Test 1 (isValidForReporting gates all updates): ";

    FreeDVReporter valid("localhost", "N1DQ", "CN98", "FreeDV Test", false);
    FreeDVReporter noCallsign("localhost", "", "CN98", "FreeDV Test", false);
    FreeDVReporter noGrid("localhost", "N1DQ", "", "FreeDV Test", false);

    bool result = valid.isValidForReporting();
    result &= !noCallsign.isValidForReporting();
    result &= !noGrid.isValidForReporting();

    // With an invalid identity, public update methods must be inert.
    noCallsign.freqChange(14236000);
    noCallsign.transmit("1600X", true);
    noCallsign.updateMessage("ignored");
    noCallsign.inAnalogMode(true);
    result &= (FreeDVReporterTest::lastFrequency(noCallsign) == 0);
    result &= (FreeDVReporterTest::mode(noCallsign) == "");
    result &= (FreeDVReporterTest::message(noCallsign) == "");
    result &= (FreeDVReporterTest::hidden(noCallsign) == false);

    bool aboutToShowSelfCalled = false;
    noCallsign.setAboutToShowSelfFn([&]() { aboutToShowSelfCalled = true; });
    noCallsign.hideFromView();
    result &= (FreeDVReporterTest::hidden(noCallsign) == false);
    noCallsign.showOurselves();
    result &= !aboutToShowSelfCalled;
    noCallsign.requestQSY("sid", 7100000, "ignored");
    noCallsign.addReceiveRecord("K6AQ", "1600X", 14236000, 5);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testNewConnectionHandler()
{
    std::cout << "Test 2 (new_connection dispatch and type validation): ";

    FreeDVReporter reporter("localhost", "N1DQ", "CN98", "FreeDV Test", false);

    int calls = 0;
    std::string gotSid, gotLastUpdate, gotCallsign, gotGrid, gotVersion, gotConnectTime;
    bool gotRxOnly = false;
    reporter.setOnUserConnectFn([&](std::string sid, std::string lastUpdate, std::string callsign,
                                    std::string grid, std::string version, bool rxOnly, std::string connectTime) {
        calls++;
        gotSid = sid; gotLastUpdate = lastUpdate; gotCallsign = callsign; gotGrid = grid;
        gotVersion = version; gotRxOnly = rxOnly; gotConnectTime = connectTime;
    });

    bool result = true;

    withParsedJson(
        R"({"sid":"abc123","last_update":"2026-10-08 12:00:00","callsign":"K6AQ",)"
        R"("grid_square":"DM12kw","version":"1.4","rx_only":true,"connect_time":"2026-10-08 11:00:00"})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireNewConnection(reporter, root); });

    result &= (calls == 1);
    result &= (gotSid == "abc123");
    result &= (gotLastUpdate == "2026-10-08 12:00:00");
    result &= (gotCallsign == "K6AQ");
    result &= (gotGrid == "DM12kw");
    result &= (gotVersion == "1.4");
    result &= (gotRxOnly == true);
    result &= (gotConnectTime == "2026-10-08 11:00:00");

    // Wrong type for rx_only: handler must not fire.
    withParsedJson(
        R"({"sid":"abc123","last_update":"u","callsign":"K6AQ","grid_square":"DM12kw",)"
        R"("version":"1.4","rx_only":"yes","connect_time":"t"})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireNewConnection(reporter, root); });
    result &= (calls == 1);

    // Missing connect_time: handler must not fire.
    withParsedJson(
        R"({"sid":"abc123","last_update":"u","callsign":"K6AQ","grid_square":"DM12kw",)"
        R"("version":"1.4","rx_only":false})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireNewConnection(reporter, root); });
    result &= (calls == 1);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testRemoveConnectionHandler()
{
    std::cout << "Test 3 (remove_connection dispatch, empty connect_time): ";

    FreeDVReporter reporter("localhost", "N1DQ", "CN98", "FreeDV Test", false);

    int calls = 0;
    std::string gotConnectTime = "unset";
    reporter.setOnUserDisconnectFn([&](std::string sid, std::string, std::string,
                                       std::string, std::string, bool, std::string connectTime) {
        calls++;
        gotConnectTime = connectTime;
    });

    bool result = true;

    withParsedJson(
        R"({"sid":"abc123","last_update":"u","callsign":"K6AQ","grid_square":"DM12kw",)"
        R"("version":"1.4","rx_only":false})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireRemoveConnection(reporter, root); });
    result &= (calls == 1);
    result &= (gotConnectTime == "");

    // sid with the wrong type: handler must not fire.
    withParsedJson(
        R"({"sid":123,"last_update":"u","callsign":"K6AQ","grid_square":"DM12kw",)"
        R"("version":"1.4","rx_only":false})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireRemoveConnection(reporter, root); });
    result &= (calls == 1);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testTxReportHandler()
{
    std::cout << "Test 4 (tx_report dispatch, null last_tx tolerated): ";

    FreeDVReporter reporter("localhost", "N1DQ", "CN98", "FreeDV Test", false);

    int calls = 0;
    std::string gotMode, gotLastTx;
    bool gotTransmitting = false;
    reporter.setOnTransmitUpdateFn([&](std::string, std::string, std::string, std::string,
                                       std::string mode, bool transmitting, std::string lastTx) {
        calls++;
        gotMode = mode; gotTransmitting = transmitting; gotLastTx = lastTx;
    });

    bool result = true;

    withParsedJson(
        R"({"sid":"s","last_update":"u","callsign":"K6AQ","grid_square":"DM12kw",)"
        R"("mode":"1600X","transmitting":true,"last_tx":"2026-10-08 12:00:00"})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireTxReport(reporter, root); });
    result &= (calls == 1);
    result &= (gotMode == "1600X");
    result &= (gotTransmitting == true);
    result &= (gotLastTx == "2026-10-08 12:00:00");

    // last_tx may legitimately be null (station has never transmitted).
    withParsedJson(
        R"({"sid":"s","last_update":"u","callsign":"K6AQ","grid_square":"DM12kw",)"
        R"("mode":"1600X","transmitting":false,"last_tx":null})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireTxReport(reporter, root); });
    result &= (calls == 2);
    result &= (gotLastTx == "");
    result &= (gotTransmitting == false);

    // transmitting with the wrong type: handler must not fire.
    withParsedJson(
        R"({"sid":"s","last_update":"u","callsign":"K6AQ","grid_square":"DM12kw",)"
        R"("mode":"1600X","transmitting":"yes","last_tx":null})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireTxReport(reporter, root); });
    result &= (calls == 2);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testRxReportHandler()
{
    std::cout << "Test 5 (rx_report dispatch, integer and float SNR): ";

    FreeDVReporter reporter("localhost", "N1DQ", "CN98", "FreeDV Test", false);

    int calls = 0;
    float gotSnr = 0;
    std::string gotReceiverCallsign, gotSenderCallsign, gotMode;
    reporter.setOnReceiveUpdateFn([&](std::string, std::string, std::string receiverCallsign,
                                      std::string, std::string senderCallsign, float snr, std::string mode) {
        calls++;
        gotReceiverCallsign = receiverCallsign; gotSenderCallsign = senderCallsign;
        gotSnr = snr; gotMode = mode;
    });

    bool result = true;

    withParsedJson(
        R"({"sid":"s","last_update":"u","receiver_callsign":"K6AQ","receiver_grid_square":"DM12kw",)"
        R"("callsign":"N1DQ","snr":5,"mode":"1600X"})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireRxReport(reporter, root); });
    result &= (calls == 1);
    result &= (gotReceiverCallsign == "K6AQ");
    result &= (gotSenderCallsign == "N1DQ");
    result &= (std::fabs(gotSnr - 5.0f) < 1e-6f);
    result &= (gotMode == "1600X");

    // SNR as a JSON real must also be accepted.
    withParsedJson(
        R"({"sid":"s","last_update":"u","receiver_callsign":"K6AQ","receiver_grid_square":"DM12kw",)"
        R"("callsign":"N1DQ","snr":-2.5,"mode":"1600X"})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireRxReport(reporter, root); });
    result &= (calls == 2);
    result &= (std::fabs(gotSnr + 2.5f) < 1e-6f);

    // SNR as a string: handler must not fire.
    withParsedJson(
        R"({"sid":"s","last_update":"u","receiver_callsign":"K6AQ","receiver_grid_square":"DM12kw",)"
        R"("callsign":"N1DQ","snr":"5","mode":"1600X"})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireRxReport(reporter, root); });
    result &= (calls == 2);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testFreqChangeHandler()
{
    std::cout << "Test 6 (freq_change dispatch, non-uint freq rejected): ";

    FreeDVReporter reporter("localhost", "N1DQ", "CN98", "FreeDV Test", false);

    int calls = 0;
    uint64_t gotFreq = 0;
    reporter.setOnFrequencyChangeFn([&](std::string, std::string, std::string, std::string, uint64_t freq) {
        calls++;
        gotFreq = freq;
    });

    bool result = true;

    withParsedJson(
        R"({"sid":"s","last_update":"u","callsign":"K6AQ","grid_square":"DM12kw","freq":14236000})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireFreqChange(reporter, root); });
    result &= (calls == 1);
    result &= (gotFreq == 14236000ULL);

    withParsedJson(
        R"({"sid":"s","last_update":"u","callsign":"K6AQ","grid_square":"DM12kw","freq":-100})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireFreqChange(reporter, root); });
    result &= (calls == 1);

    withParsedJson(
        R"({"sid":"s","last_update":"u","callsign":"K6AQ","grid_square":"DM12kw","freq":14236000.5})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireFreqChange(reporter, root); });
    result &= (calls == 1);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testMessageAndQsyHandlers()
{
    std::cout << "Test 7 (message_update and qsy_request dispatch): ";

    FreeDVReporter reporter("localhost", "N1DQ", "CN98", "FreeDV Test", false);

    int messageCalls = 0;
    std::string gotMessage;
    reporter.setMessageUpdateFn([&](std::string, std::string, std::string message) {
        messageCalls++;
        gotMessage = message;
    });

    int qsyCalls = 0;
    std::string gotQsyCallsign, gotQsyMessage;
    uint64_t gotQsyFreq = 0;
    reporter.setOnQSYRequestFn([&](std::string callsign, uint64_t freq, std::string message) {
        qsyCalls++;
        gotQsyCallsign = callsign; gotQsyFreq = freq; gotQsyMessage = message;
    });

    bool result = true;

    withParsedJson(R"({"sid":"s","last_update":"u","message":"CQ test"})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireMessageUpdate(reporter, root); });
    result &= (messageCalls == 1);
    result &= (gotMessage == "CQ test");

    withParsedJson(R"({"sid":"s","last_update":"u","message":42})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireMessageUpdate(reporter, root); });
    result &= (messageCalls == 1);

    withParsedJson(R"({"callsign":"K6AQ","frequency":7100000,"message":"QSY please"})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireQsyRequest(reporter, root); });
    result &= (qsyCalls == 1);
    result &= (gotQsyCallsign == "K6AQ");
    result &= (gotQsyFreq == 7100000ULL);
    result &= (gotQsyMessage == "QSY please");

    withParsedJson(R"({"callsign":"K6AQ","frequency":"7100000","message":"QSY please"})",
        [&](yyjson_val* root) { FreeDVReporterTest::fireQsyRequest(reporter, root); });
    result &= (qsyCalls == 1);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testBulkUpdateToleratesMalformedEntries()
{
    std::cout << "Test 8 (bulk_update survives malformed entries and unknown events): ";

    FreeDVReporter reporter("localhost", "N1DQ", "CN98", "FreeDV Test", false);
    bool result = true;

    // No event handlers are registered (connect() was never called), so every
    // dispatch is a no-op; the point is that parsing/dispatch must not crash
    // on non-array entries or non-string event names.
    withParsedJson(R"([["new_connection",{"sid":"s"}], "garbage", [42], []])",
        [&](yyjson_val* root) { FreeDVReporterTest::fireBulkUpdate(reporter, root); });

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testStateSavedBeforeConnectionAndReplayedOnSuccess()
{
    std::cout << "Test 9 (state saved pre-connect, connection_successful flips gate): ";

    FreeDVReporter reporter("localhost", "N1DQ", "CN98", "FreeDV Test", false);

    bool connectionSuccessfulCalled = false;
    reporter.setConnectionSuccessfulFn([&]() { connectionSuccessfulCalled = true; });

    bool result = !FreeDVReporterTest::fullyConnected(reporter);

    // Before connecting, updates must be saved for replay but not emitted.
    reporter.freqChange(14236000);
    reporter.transmit("1600X", true);
    reporter.updateMessage("CQ test");
    result &= (FreeDVReporterTest::lastFrequency(reporter) == 14236000ULL);
    result &= (FreeDVReporterTest::mode(reporter) == "1600X");
    result &= (FreeDVReporterTest::tx(reporter) == true);
    result &= (FreeDVReporterTest::message(reporter) == "CQ test");

    // addReceiveRecord is gated on full connection: must be inert here.
    reporter.addReceiveRecord("K6AQ", "1600X", 14236000, 5);

    FreeDVReporterTest::fireConnectionSuccessful(reporter);
    result &= FreeDVReporterTest::fullyConnected(reporter);
    result &= connectionSuccessfulCalled;

    // Re-report path ran through the (unguarded-by-connection) emit calls
    // without crashing; saved state must be intact.
    result &= (FreeDVReporterTest::lastFrequency(reporter) == 14236000ULL);
    result &= (FreeDVReporterTest::mode(reporter) == "1600X");

    // Now fully connected: addReceiveRecord takes the emit path.
    reporter.addReceiveRecord("K6AQ", "1600X", 14236000, 5);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testHideShowAnalogMode()
{
    std::cout << "Test 10 (inAnalogMode hides in analog, shows in digital): ";

    FreeDVReporter reporter("localhost", "N1DQ", "CN98", "FreeDV Test", false);

    bool aboutToShowSelfCalled = false;
    reporter.setAboutToShowSelfFn([&]() { aboutToShowSelfCalled = true; });

    bool result = !FreeDVReporterTest::hidden(reporter);

    reporter.inAnalogMode(true);
    result &= FreeDVReporterTest::hidden(reporter);
    result &= !aboutToShowSelfCalled;

    reporter.inAnalogMode(false);
    result &= !FreeDVReporterTest::hidden(reporter);
    result &= aboutToShowSelfCalled;

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

// A server event, how to deliver it, and a payload the handler accepts.
struct ServerEvent
{
    const char* name;
    void (*fire)(FreeDVReporter&, yyjson_val*);
    const char* validJson;
    std::vector<std::string> optionalFields; // may be missing or of any type
};

const std::vector<ServerEvent>& serverEvents()
{
    static const std::vector<ServerEvent> events = {
        {"new_connection", FreeDVReporterTest::fireNewConnection,
         R"({"sid":"s","last_update":"u","callsign":"K6AQ","grid_square":"DM12kw",)"
         R"("version":"1.4","rx_only":true,"connect_time":"t"})", {}},
        {"remove_connection", FreeDVReporterTest::fireRemoveConnection,
         R"({"sid":"s","last_update":"u","callsign":"K6AQ","grid_square":"DM12kw",)"
         R"("version":"1.4","rx_only":false})", {}},
        {"tx_report", FreeDVReporterTest::fireTxReport,
         R"({"sid":"s","last_update":"u","callsign":"K6AQ","grid_square":"DM12kw",)"
         R"("mode":"1600X","transmitting":true,"last_tx":"2026-10-08 12:00:00"})", {"last_tx"}},
        {"rx_report", FreeDVReporterTest::fireRxReport,
         R"({"sid":"s","last_update":"u","receiver_callsign":"K6AQ","receiver_grid_square":"DM12kw",)"
         R"("callsign":"N1DQ","snr":5,"mode":"1600X"})", {}},
        {"freq_change", FreeDVReporterTest::fireFreqChange,
         R"({"sid":"s","last_update":"u","callsign":"K6AQ","grid_square":"DM12kw","freq":14236000})", {}},
        {"message_update", FreeDVReporterTest::fireMessageUpdate,
         R"({"sid":"s","last_update":"u","message":"CQ test"})", {}},
        {"qsy_request", FreeDVReporterTest::fireQsyRequest,
         R"({"callsign":"K6AQ","frequency":7100000,"message":"QSY please"})", {}},
    };
    return events;
}

// Registers a handler for every server event that counts its calls.
void countAllEvents(FreeDVReporter& reporter, int& calls)
{
    reporter.setOnUserConnectFn([&](std::string, std::string, std::string, std::string, std::string, bool, std::string) { calls++; });
    reporter.setOnUserDisconnectFn([&](std::string, std::string, std::string, std::string, std::string, bool, std::string) { calls++; });
    reporter.setOnTransmitUpdateFn([&](std::string, std::string, std::string, std::string, std::string, bool, std::string) { calls++; });
    reporter.setOnReceiveUpdateFn([&](std::string, std::string, std::string, std::string, std::string, float, std::string) { calls++; });
    reporter.setOnFrequencyChangeFn([&](std::string, std::string, std::string, std::string, uint64_t) { calls++; });
    reporter.setMessageUpdateFn([&](std::string, std::string, std::string) { calls++; });
    reporter.setOnQSYRequestFn([&](std::string, uint64_t, std::string) { calls++; });
}

// Delivers the event's valid payload with one field removed (wrongValue
// null) or replaced by wrongValue, and returns how many handlers ran.
int fireWithFieldChanged(FreeDVReporter& reporter, int& calls, const ServerEvent& event,
                         const char* field, const char* wrongValue)
{
    yyjson_doc* doc = yyjson_read(event.validJson, strlen(event.validJson), 0);
    yyjson_mut_doc* mut = yyjson_doc_mut_copy(doc, nullptr);
    yyjson_doc_free(doc);
    yyjson_mut_val* root = yyjson_mut_doc_get_root(mut);
    if (wrongValue == nullptr)
    {
        yyjson_mut_obj_remove_key(root, field);
    }
    else
    {
        yyjson_doc* valueDoc = yyjson_read(wrongValue, strlen(wrongValue), 0);
        yyjson_mut_obj_replace(root, yyjson_mut_strcpy(mut, field),
                               yyjson_val_mut_copy(mut, yyjson_doc_get_root(valueDoc)));
        yyjson_doc_free(valueDoc);
    }
    char* json = yyjson_mut_write(mut, 0, nullptr);
    yyjson_mut_doc_free(mut);

    int before = calls;
    withParsedJson(json, [&](yyjson_val* root) { event.fire(reporter, root); });
    free(json);
    return calls - before;
}

bool testEveryFieldValidated()
{
    std::cout << "Test 11 (every server event ignores a missing or wrongly typed field): ";

    FreeDVReporter reporter("localhost", "N1DQ", "CN98", "FreeDV Test", false);
    int calls = 0;
    countAllEvents(reporter, calls);

    bool result = true;
    for (auto& event : serverEvents())
    {
        // The valid payload itself must get through.
        int before = calls;
        withParsedJson(event.validJson, [&](yyjson_val* root) { event.fire(reporter, root); });
        if (calls != before + 1)
        {
            std::cout << "\n    " << event.name << ": valid payload not delivered\n    ";
            result = false;
        }

        yyjson_doc* doc = yyjson_read(event.validJson, strlen(event.validJson), 0);
        size_t idx, max;
        yyjson_val *key, *value;
        yyjson_obj_foreach(yyjson_doc_get_root(doc), idx, max, key, value)
        {
            std::string field = yyjson_get_str(key);
            bool optional = std::find(event.optionalFields.begin(), event.optionalFields.end(), field) != event.optionalFields.end();
            int expected = optional ? 1 : 0;

            // A value of some other type: a number for a string field, a
            // string for anything else.
            const char* wrongValue = yyjson_is_str(value) ? "42" : "\"wrong\"";
            const char* variants[] = {nullptr, wrongValue, "null"};
            for (const char* variant : variants)
            {
                int got = fireWithFieldChanged(reporter, calls, event, field.c_str(), variant);
                if (got != expected)
                {
                    std::cout << "\n    " << event.name << ": " << field << " "
                              << (variant ? std::string("= ") + variant : std::string("missing"))
                              << " -> " << got << " calls, expected " << expected << "\n    ";
                    result = false;
                }
            }
        }
        yyjson_doc_free(doc);
    }

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testNonObjectPayloadsIgnored()
{
    std::cout << "Test 12 (every server event ignores a payload that isn't an object): ";

    FreeDVReporter reporter("localhost", "N1DQ", "CN98", "FreeDV Test", false);
    int calls = 0;
    countAllEvents(reporter, calls);

    for (auto& event : serverEvents())
    {
        for (const char* json : {"null", "42", "\"text\"", "[1,2,3]", "{}"})
        {
            withParsedJson(json, [&](yyjson_val* root) { event.fire(reporter, root); });
        }
        event.fire(reporter, nullptr); // event sent without arguments
    }
    bool result = (calls == 0);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testTxReportWithoutLastTx()
{
    std::cout << "Test 13 (tx_report with last_tx missing or not a string reports it as empty): ";

    // Used to construct a std::string from a null pointer (a crash).
    FreeDVReporter reporter("localhost", "N1DQ", "CN98", "FreeDV Test", false);
    int calls = 0;
    std::string lastTx = "unset";
    reporter.setOnTransmitUpdateFn([&](std::string, std::string, std::string, std::string, std::string, bool, std::string tx) {
        calls++;
        lastTx = tx;
    });

    bool result = true;
    for (const char* lastTxJson : {"", R"(,"last_tx":12345)", R"(,"last_tx":null)"})
    {
        std::string json = std::string(R"({"sid":"s","last_update":"u","callsign":"K6AQ","grid_square":"DM12kw",)"
                                       R"("mode":"1600X","transmitting":true)") + lastTxJson + "}";
        lastTx = "unset";
        withParsedJson(json.c_str(), [&](yyjson_val* root) { FreeDVReporterTest::fireTxReport(reporter, root); });
        result &= (lastTx == "");
    }
    result &= (calls == 3);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testEventsWithoutHandlersIgnored()
{
    std::cout << "Test 14 (server events with no handler registered are ignored): ";

    // freedv-gui registers only some handlers (e.g. none while the FreeDV
    // Reporter window is closed); the rest must be skipped safely.
    FreeDVReporter reporter("localhost", "N1DQ", "CN98", "FreeDV Test", false);
    for (auto& event : serverEvents())
    {
        withParsedJson(event.validJson, [&](yyjson_val* root) { event.fire(reporter, root); });
    }
    FreeDVReporterTest::fireConnectionSuccessful(reporter);
    bool result = FreeDVReporterTest::fullyConnected(reporter);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}
bool testDefaultHostnameWhenEmpty()
{
    std::cout << "Test 15 (empty hostname falls back to the default reporter server): ";

    FreeDVReporter emptyHostname("", "N1DQ", "CN98", "FreeDV Test", false);
    FreeDVReporter explicitHostname("127.0.0.1:8080", "N1DQ", "CN98", "FreeDV Test", false);

    bool result = (FreeDVReporterTest::hostname(emptyHostname) == FREEDV_REPORTER_DEFAULT_HOSTNAME);
    result &= (FreeDVReporterTest::hostname(explicitHostname) == "127.0.0.1:8080");

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testDeleteThroughReporterInterface()
{
    std::cout << "Test 16 (reporter heap-allocated and deleted through IReporter*): ";

    IReporter* reporter = new FreeDVReporter("127.0.0.1:8080", "N1DQ", "CN98", "FreeDV Test", false);
    bool result = (reporter != nullptr);

    // Calls through the base pointer prove the object is a live reporter
    // before the deleting destructor runs.
    reporter->freqChange(14236000);
    reporter->send();
    delete reporter;

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testShowOurselvesWithoutCallback()
{
    std::cout << "Test 17 (showOurselves_ clears hidden even with no about_to_show_self callback): ";

    FreeDVReporter reporter("127.0.0.1:8080", "N1DQ", "CN98", "FreeDV Test", false);
    bool called = false;
    reporter.setAboutToShowSelfFn([&]() { called = true; });
    reporter.hideFromView();

    bool result = FreeDVReporterTest::hidden(reporter);
    reporter.showOurselves();
    result &= called;
    result &= !FreeDVReporterTest::hidden(reporter);

    // Same sequence with the callback left unset: no crash, state still updated.
    FreeDVReporter noCallback("127.0.0.1:8080", "N1DQ", "CN98", "FreeDV Test", false);
    noCallback.hideFromView();
    result &= FreeDVReporterTest::hidden(noCallback);
    noCallback.showOurselves();
    result &= !FreeDVReporterTest::hidden(noCallback);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testBulkUpdateWithNonArrayPayloads()
{
    std::cout << "Test 18 (bulk_update ignores payloads that are not arrays of event entries): ";

    FreeDVReporter reporter("127.0.0.1:8080", "N1DQ", "CN98", "FreeDV Test", false);
    int calls = 0;
    countAllEvents(reporter, calls);

    bool result = true;

    // A whole payload that isn't an array at all.
    for (const char* payload : {"42", "\"bulk\"", "{}", "null", "[]", "true"})
    {
        withParsedJson(payload, [&](yyjson_val* root) { FreeDVReporterTest::fireBulkUpdate(reporter, root); });
    }

    // Entries inside the payload that aren't arrays.
    withParsedJson(R"([ "garbage", 42, null, {}, [], ["freq_change"] ])",
        [&](yyjson_val* root) { FreeDVReporterTest::fireBulkUpdate(reporter, root); });

    // No payload at all: the server sent the event with no arguments.
    FreeDVReporterTest::fireBulkUpdate(reporter, nullptr);

    // Nothing reached the application callbacks and no state was touched.
    result &= (calls == 0);
    result &= (FreeDVReporterTest::mode(reporter) == "");
    result &= !FreeDVReporterTest::fullyConnected(reporter);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testHostnameWithoutPortDefaultsToServerPort()
{
    std::cout << "Test 19 (hostname with no port falls back to the server port): ";

    // "127.0.0.1:" carries no port field, so connect_() keeps its default and
    // still starts the (loopback) connection attempt. Nothing here waits on
    // the handshake: the gate must simply still be closed on return.
    FreeDVReporter noPort("127.0.0.1:", "N1DQ", "CN98", "FreeDV Test", false);
    bool result = (FreeDVReporterTest::hostname(noPort) == "127.0.0.1:");
    result &= !FreeDVReporterTest::fullyConnected(noPort);

    noPort.connect();
    result &= !FreeDVReporterTest::fullyConnected(noPort);

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}


} // namespace

int main(int argc, char** argv)
{
    bool result = true;

    result &= testValidForReportingGating();
    result &= testNewConnectionHandler();
    result &= testRemoveConnectionHandler();
    result &= testTxReportHandler();
    result &= testRxReportHandler();
    result &= testFreqChangeHandler();
    result &= testMessageAndQsyHandlers();
    result &= testBulkUpdateToleratesMalformedEntries();
    result &= testStateSavedBeforeConnectionAndReplayedOnSuccess();
    result &= testHideShowAnalogMode();
    result &= testEveryFieldValidated();
    result &= testNonObjectPayloadsIgnored();
    result &= testTxReportWithoutLastTx();
    result &= testEventsWithoutHandlersIgnored();
    result &= testDefaultHostnameWhenEmpty();
    result &= testDeleteThroughReporterInterface();
    result &= testShowOurselvesWithoutCallback();
    result &= testBulkUpdateWithNonArrayPayloads();
    result &= testHostnameWithoutPortDefaultsToServerPort();

    return result ? 0 : -1;
}
