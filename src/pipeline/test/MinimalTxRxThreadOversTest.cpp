// Two transmissions in a row through MinimalTxRxThread: when TX starts
// again after a gap, the thread must reset its pipeline and empty its input
// FIFO (so audio queued while not transmitting isn't sent), and each
// transmission must end with its own end-of-over carrying the callsign.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

extern "C"
{
#include "fargan.h"
#include "lpcnet.h"
#include "fargan_config.h"
}
#include "rade_api.h"

#include "MinimalTxRxThread.h"
#include "MinimalRealTimeHelper.h"
#include "paCallbackData.h"
#include "pipeline_defines.h"
#include "rade_text.h"
#include "PipelineTestCommon.h"
#include "SpeechSample.h"
#include "../util/GenericFIFO.h"

using namespace std::chrono_literals;

std::atomic<bool> g_tx(false);
std::atomic<bool> endingTx(false);
extern std::atomic<bool> g_eoo_enqueued; // defined in MinimalTxRxThread.cpp

std::string utTxFeatureFile;
std::string utRxFeatureFile;

namespace {

constexpr int SPEECH_RATE = RADE_SPEECH_SAMPLE_RATE; // 16 kHz
constexpr int MODEM_RATE = RADE_MODEM_SAMPLE_RATE;   // 8 kHz
constexpr int SPEECH_CHUNK = SPEECH_RATE / 50;       // 20 ms
constexpr int MODEM_CHUNK = MODEM_RATE / 50;

int CallsignsReceived = 0;

void drainInto(GenericFIFO<short>* fifo, std::vector<short>& out)
{
    int avail = fifo->numUsed();
    if (avail > 0)
    {
        size_t prev = out.size();
        out.resize(prev + avail);
        fifo->read(out.data() + prev, avail);
    }
}

// Writes all of `samples` into `fifo`, waiting for room, while draining
// `outFifo` into `out`.
void feed(GenericFIFO<short>* fifo, const short* samples, size_t count, int chunk,
          GenericFIFO<short>* outFifo, std::vector<short>& out)
{
    size_t pos = 0;
    while (pos < count)
    {
        int n = (int)std::min<size_t>({(size_t)chunk, count - pos, (size_t)fifo->numFree()});
        if (n > 0)
        {
            fifo->write(const_cast<short*>(samples) + pos, n);
            pos += n;
        }
        else
        {
            std::this_thread::sleep_for(5ms);
        }
        drainInto(outFifo, out);
    }
}

// One transmission: speech in, then end-of-over. Returns the modem audio.
std::vector<short> transmit(paCallBackData& cbData, const std::vector<short>& speech)
{
    std::vector<short> modem;

    // Starting TX makes the thread empty its input FIFO the next time it
    // runs, which wipes anything written before then. Queue a short marker
    // and wait for the FIFO to drain before writing the speech, so none of
    // it is caught by the wipe. The output FIFO isn't drained meanwhile:
    // with nothing to send, the thread transmits silence, and the small
    // output FIFO (see below) stops it after one block. If the wipe is
    // broken, the marker and stale audio are sent instead, a block at a
    // time, and the wait times out.
    std::vector<short> marker(SPEECH_CHUNK, 0);
    cbData.infifo1->write(marker.data(), (int)marker.size());
    endingTx.store(false, std::memory_order_release);
    g_tx.store(true, std::memory_order_release);
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (cbData.infifo1->numUsed() > 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(1ms);
    }
    feed(cbData.infifo1, speech.data(), speech.size(), SPEECH_CHUNK, cbData.outfifo1, modem);

    endingTx.store(true, std::memory_order_release);
    deadline = std::chrono::steady_clock::now() + 30s;
    while (!g_eoo_enqueued.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(10ms);
        drainInto(cbData.outfifo1, modem);
    }
    g_tx.store(false, std::memory_order_release);
    std::this_thread::sleep_for(200ms); // let the thread see TX end
    drainInto(cbData.outfifo1, modem);
    return modem;
}
// A helper whose mustStopWork() can be raised from the test, so we can
// watch the processing loops leave through the helper check instead of
// their FIFO guards.
class FlippableRealtimeHelper : public IRealtimeHelper
{
public:
    virtual void setHelperRealTime() override { /* empty */ }
    virtual void startRealTimeWork() override { /* empty */ }
    virtual void stopRealTimeWork(bool fastMode = false) override { (void)fastMode; }
    virtual void clearHelperRealTime() override { /* empty */ }
    virtual bool mustStopWork() FREEDV_NONBLOCKING override { return stopNow.load(std::memory_order_acquire); }

    std::atomic<bool> stopNow{false};
};


} // namespace

bool secondTransmissionStartsClean()
{
    auto speech = loadSpeech16k(10.0);
    if (speech.size() < (size_t)(10 * SPEECH_RATE))
    {
        std::cerr << "[could not load the speech sample]...";
        return false;
    }
    std::vector<short> firstOver(speech.begin(), speech.begin() + 5 * SPEECH_RATE);
    std::vector<short> secondOver(speech.begin() + 5 * SPEECH_RATE, speech.end());

    char modelFile[1] = {0};
    struct rade* rade = rade_open(modelFile, RADE_USE_C_ENCODER | RADE_USE_C_DECODER);
    LPCNetEncState* encState = lpcnet_encoder_create();
    FARGANState fargan;
    {
        float zeros[320] = {0};
        float inFeatures[5 * NB_TOTAL_FEATURES] = {0};
        fargan_init(&fargan);
        fargan_cont(&fargan, zeros, inFeatures);
    }
    rade_text_t radeText = rade_text_create();
    std::vector<float> eooSyms(rade_n_eoo_bits(rade));
    rade_text_generate_tx_string(radeText, "K6AQ", 4, eooSyms.data(), (int)eooSyms.size());
    rade_tx_set_eoo_bits(rade, eooSyms.data());
    CallsignsReceived = 0;
    rade_text_set_rx_callback(radeText, [](rade_text_t, const char* txt, int length, void*) {
        if (length == 4 && strncmp(txt, "K6AQ", 4) == 0) CallsignsReceived++;
    }, nullptr);

    paCallBackData cbData;
    // Room for a whole transmission, so its speech is queued at once and the
    // thread can never run dry mid-transmission (it would send silence,
    // making the transmission longer, on a slow or busy machine).
    cbData.infifo1 = new GenericFIFO<short>(8 * SPEECH_RATE);
    cbData.outfifo1 = nullptr; // sized once the TX thread knows its frame size
    cbData.infifo2 = new GenericFIFO<short>(MODEM_RATE);
    cbData.outfifo2 = new GenericFIFO<short>(SPEECH_RATE);

    auto txHelper = std::make_shared<MinimalRealtimeHelper>();
    auto rxHelper = std::make_shared<MinimalRealtimeHelper>();
    auto txThread = std::make_unique<MinimalTxRxThread>(true, SPEECH_RATE, MODEM_RATE, txHelper, rade, encState, &fargan, radeText, &cbData);
    auto rxThread = std::make_unique<MinimalTxRxThread>(false, MODEM_RATE, SPEECH_RATE, rxHelper, rade, encState, &fargan, radeText, &cbData);
    txThread->start(); rxThread->start();
    txThread->waitForReady(); rxThread->waitForReady();

    // Room for exactly one modem frame (the thread only runs when a whole
    // frame fits): it can't get more than a block ahead of what the test
    // drains, so how long the test takes to start feeding speech can't
    // change how much silence gets sent.
    cbData.outfifo1 = new GenericFIFO<short>(txThread->getTxNNomModemSamples() + 1);
    txThread->signalToStart(); rxThread->signalToStart();
    std::this_thread::sleep_for(100ms);

    auto modem1 = transmit(cbData, firstOver);

    // While not transmitting, 0.8 s of loud tone reaches the TX input (the
    // microphone is still live). Starting TX again must throw it away.
    std::vector<short> stale(SPEECH_RATE * 8 / 10);
    for (size_t n = 0; n < stale.size(); n++)
    {
        stale[n] = (short)(20000 * std::sin(2 * M_PI * 1000.0 * n / SPEECH_RATE));
    }
    cbData.infifo1->write(stale.data(), (int)stale.size());

    auto modem2 = transmit(cbData, secondOver);
    txThread->stop();

    // Receive both transmissions, with a second of silence between them.
    std::vector<short> modem = modem1;
    modem.insert(modem.end(), MODEM_RATE, 0);
    modem.insert(modem.end(), modem2.begin(), modem2.end());
    modem.insert(modem.end(), 2 * MODEM_RATE, 0); // flush the receiver
    std::vector<short> received;
    feed(cbData.infifo2, modem.data(), modem.size(), MODEM_CHUNK, cbData.outfifo2, received);
    auto deadline = std::chrono::steady_clock::now() + 60s;
    while (cbData.infifo2->numUsed() > 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(10ms);
        drainInto(cbData.outfifo2, received);
    }
    std::this_thread::sleep_for(500ms);
    drainInto(cbData.outfifo2, received);
    rxThread->stop();

    double seconds1 = (double)modem1.size() / MODEM_RATE;
    double seconds2 = (double)modem2.size() / MODEM_RATE;
    double receivedSeconds = (double)received.size() / SPEECH_RATE;
    // Each transmission is 5 s of speech plus the end-of-over burst.
    bool result = seconds1 >= 5.0 && std::abs(seconds2 - seconds1) < 0.3 && CallsignsReceived == 2 &&
                  receivedSeconds > 8.0 && receivedSeconds < 14.0;
    if (!result)
    {
        std::cerr << "[transmissions " << seconds1 << " s and " << seconds2 << " s of modem audio, "
                  << CallsignsReceived << " callsigns received, " << receivedSeconds << " s of speech back]...";
    }

    txThread.reset();
    rxThread.reset();
    delete cbData.infifo1; delete cbData.outfifo1;
    delete cbData.infifo2; delete cbData.outfifo2;
    rade_text_destroy(radeText);
    lpcnet_encoder_destroy(encState);
    rade_close(rade);
    return result;
}

bool threadGuardsStopOnHelperAndFullOutputFifo()
{
    char modelFile[1] = {0};
    struct rade* rade = rade_open(modelFile, RADE_USE_C_ENCODER | RADE_USE_C_DECODER);
    LPCNetEncState* encState = lpcnet_encoder_create();
    FARGANState fargan;
    {
        float zeros[320] = {0};
        float inFeatures[5 * NB_TOTAL_FEATURES] = {0};
        fargan_init(&fargan);
        fargan_cont(&fargan, zeros, inFeatures);
    }
    rade_text_t radeText = rade_text_create();

    paCallBackData cbData;
    cbData.infifo1 = new GenericFIFO<short>(SPEECH_RATE);      // stays quiet: TX sends silence
    cbData.outfifo1 = nullptr; // sized once the TX thread knows its frame size
    cbData.infifo2 = new GenericFIFO<short>(MODEM_RATE);
    // too small to ever hold a whole modem second of decode: RX must
    // run into its output-space guard
    cbData.outfifo2 = new GenericFIFO<short>(MODEM_RATE + 2000);
    auto txHelper = std::make_shared<FlippableRealtimeHelper>();
    auto rxHelper = std::make_shared<FlippableRealtimeHelper>();
    auto txThread = std::make_unique<MinimalTxRxThread>(true, SPEECH_RATE, MODEM_RATE, txHelper, rade, encState, &fargan, radeText, &cbData);
    auto rxThread = std::make_unique<MinimalTxRxThread>(false, MODEM_RATE, SPEECH_RATE, rxHelper, rade, encState, &fargan, radeText, &cbData);
    txThread->start(); rxThread->start();
    txThread->waitForReady(); rxThread->waitForReady();

    int txFrame = txThread->getTxNNomModemSamples();
    int rxFrame = rxThread->getRxNumSpeechSamples();
    std::vector<short> junk(SPEECH_RATE, 0);
    cbData.outfifo1 = new GenericFIFO<short>(txFrame + 1);     // room for exactly one modem frame

    txThread->signalToStart(); rxThread->signalToStart();
    endingTx.store(false, std::memory_order_release);
    g_tx.store(true, std::memory_order_release);

    // TX: silence flows until the one-frame output FIFO is blocked.
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (cbData.outfifo1->numUsed() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(1ms);
    }
    bool txBlocked = cbData.outfifo1->numUsed() > 0;

    // Emptying the FIFO restores room, but the helper must now hold the
    // loop before it processes anything, even with a full mic frame waiting.
    std::vector<short> drained;
    drainInto(cbData.outfifo1, drained);
    txHelper->stopNow.store(true, std::memory_order_release);
    cbData.infifo1->write(junk.data(), 2 * SPEECH_CHUNK);
    std::this_thread::sleep_for(500ms);
    bool txIdle = cbData.infifo1->numUsed() == 2 * SPEECH_CHUNK;
    g_tx.store(false, std::memory_order_release);

    // Nothing was queued during TX.  RX empties both its FIFOs once when
    // TX ends, so repeat the fill until one of them survives that reset:
    // outfifo2 is filled until RX's own speech frame no longer fits, and
    // the modem audio queued behind it must then stay unread.
    bool rxFrozen = false;
    for (int attempt = 0; attempt < 6 && !rxFrozen; attempt++)
    {
        cbData.infifo2->reset();
        cbData.outfifo2->reset();
        std::this_thread::sleep_for(100ms);
        cbData.outfifo2->write(junk.data(), MODEM_RATE + rxFrame - rxFrame / 2 - 1);
        cbData.infifo2->write(junk.data(), MODEM_CHUNK);
        std::this_thread::sleep_for(400ms);
        rxFrozen = cbData.infifo2->numUsed() == MODEM_CHUNK &&
                   cbData.outfifo2->numUsed() > 0;
    }

    // With room restored, only the helper stands between RX and the audio.
    rxHelper->stopNow.store(true, std::memory_order_release);
    drainInto(cbData.outfifo2, drained);
    std::this_thread::sleep_for(500ms);
    bool rxIdle = cbData.infifo2->numUsed() == MODEM_CHUNK && cbData.outfifo2->numUsed() == 0;

    txThread->stop();
    rxThread->stop();

    bool result = txBlocked && txIdle && rxFrozen && rxIdle;
    if (!result)
    {
        std::cerr << "[txBlocked=" << txBlocked << " txIdle=" << txIdle << " rxFrozen=" << rxFrozen
                  << " rxIdle=" << rxIdle << " infifo1=" << cbData.infifo1->numUsed()
                  << " outfifo1=" << cbData.outfifo1->numUsed() << " infifo2=" << cbData.infifo2->numUsed()
                  << " outfifo2=" << cbData.outfifo2->numUsed() << " (rxFrame=" << rxFrame << ")]...";
    }

    txThread.reset();
    rxThread.reset();
    delete cbData.infifo1; delete cbData.outfifo1;
    delete cbData.infifo2; delete cbData.outfifo2;
    rade_text_destroy(radeText);
    lpcnet_encoder_destroy(encState);
    rade_close(rade);
    return result;
}


int main()
{
    rade_initialize();
    TEST_CASE(secondTransmissionStartsClean);
    TEST_CASE(threadGuardsStopOnHelperAndFullOutputFifo);
    rade_finalize();
    return 0;
}
