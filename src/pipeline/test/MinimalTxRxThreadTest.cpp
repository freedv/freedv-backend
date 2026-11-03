//==========================================================================
// Name:            MinimalTxRxThreadTest.cpp
//
// Purpose:         End-to-end test of MinimalTxRxThread TX and RX paths
//                  across a variety of sample-rate configurations.
//
//                  For each test case the WAV at rade_src/wav/all.wav
//                  (16 kHz, 16-bit PCM) is:
//                    1. Resampled from 16 kHz to the configured speech rate.
//                    2. Piped through a TX instance of MinimalTxRxThread
//                       (speech rate in → modem rate out).
//                    3. The modem-rate output is piped through an RX instance
//                       (modem rate in → speech rate out).
//                  Every combination must deliver the EOO callsign and
//                  bring back speech of about the right length and level.
//                  Where Python is configured, feature files captured via
//                  utTxFeatureFile / utRxFeatureFile are also scored by
//                  radae's loss.py with --loss_test 0.15; without it (e.g.
//                  on Windows) only the first 10 s of speech is used, to
//                  keep the run short.
//
//                  Valid speech rates  (TX input / RX output): 16000, 22050,
//                    24000, 32000, 44100, 48000 Hz.
//                  Valid modem rate   (TX output / RX input):  8000 Hz.
//
// Created:         May 2026
// Authors:         Mooneer Salem
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// - Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// - Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
// OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//==========================================================================

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <memory>

#include <cmath>

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
#include "ResampleStep.h"
#include "../util/GenericFIFO.h"
#include "../util/logging/ulog.h"
#include "../../util/test/TestSocketCompat.h" // testMakeTempFile()
#include "SpeechSample.h"

// ---------------------------------------------------------------------------
// Globals required by MinimalTxRxThread
// ---------------------------------------------------------------------------
std::atomic<bool> g_tx(false);
std::atomic<bool> endingTx(false);
extern std::atomic<bool> g_eoo_enqueued; // defined in MinimalTxRxThread.cpp

// ---------------------------------------------------------------------------
// Globals required by RADETransmitStep and RADEReceiveStep (unit-test hooks)
// ---------------------------------------------------------------------------
std::string utTxFeatureFile;
std::string utRxFeatureFile;

// ---------------------------------------------------------------------------
// Run loss.py on the two feature files.
// Returns true if loss.py outputs "PASS" (and not "FAIL").
// ---------------------------------------------------------------------------
#if defined(PYTHON_EXECUTABLE)
static bool runLossCheck(const char* txFeat, const char* rxFeat)
{
    std::string cmd =
        std::string("PYTHONPATH=") + RADE_SRC_DIR + " " +
        PYTHON_EXECUTABLE + " " +
        RADE_SRC_DIR + "/loss.py " +
        txFeat + " " +
        rxFeat +
        " --loss_test 0.0895 --clip_start 100 --clip_end 300 2>&1";

    log_info("Running: %s", cmd.c_str());

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe)
    {
        log_error("popen() failed – cannot run loss.py");
        return false;
    }

    std::string output;
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe))
    {
        output += buf;
        fputs(buf, stdout);
        fflush(stdout);
    }
    pclose(pipe);

    return output.find("PASS") != std::string::npos &&
           output.find("FAIL") == std::string::npos;
}
#endif // defined(PYTHON_EXECUTABLE)

static double rmsOf(const std::vector<short>& samples, size_t start)
{
    double sum = 0;
    for (size_t i = start; i < samples.size(); i++)
        sum += (double)samples[i] * samples[i];
    return samples.size() > start ? std::sqrt(sum / (samples.size() - start)) : 0;
}

// ---------------------------------------------------------------------------
// Run one TX → RX pipeline test for a given (speechRate, modemRate) pair.
//
// wavAudio16k   – entire all.wav audio at 16 kHz (read-only)
// speechRate    – TX input rate / RX output rate
// modemRate     – TX output rate / RX input rate
//
// utTxFeatureFile / utRxFeatureFile must be set by the caller before this
// function is called, as they are read by RADETransmitStep / RADEReceiveStep
// during pipeline construction.
//
// Returns true if the pipeline ran without error.  The caller should then
// invoke runLossCheck() on the captured feature files.
// ---------------------------------------------------------------------------
static bool runPipeline(
    const std::vector<short>& wavAudio16k,
    int speechRate,
    int modemRate,
    struct rade*      rade,
    LPCNetEncState*   encState,
    FARGANState*      fargan,
    rade_text_t       radeText,
    std::vector<short>& rxSpeech)
{
    // Reset per-run global state
    g_tx.store(true, std::memory_order_release);
    endingTx.store(false, std::memory_order_release);
    g_eoo_enqueued = false;

    // ----- FIFO sizing -----
    // Give each FIFO enough headroom for several seconds of audio at the
    // appropriate sample rate so the pipeline never blocks.
    const int SPEECH_FIFO = speechRate * 1;  // 1 s of speech
    const int MODEM_FIFO  = modemRate  * 1;  // 1 s of modem

    paCallBackData cbData;

    // TX thread: infifo1 (speech in), outfifo1 (modem out)
    cbData.infifo1  = new GenericFIFO<short>(SPEECH_FIFO);
    cbData.outfifo1 = new GenericFIFO<short>(MODEM_FIFO);

    // RX thread: infifo2 (modem in), outfifo2 (speech out)
    cbData.infifo2  = new GenericFIFO<short>(MODEM_FIFO);
    cbData.outfifo2 = new GenericFIFO<short>(SPEECH_FIFO);

    // Create TX thread (speechRate in → modemRate out)
    auto txHelper = std::make_shared<MinimalRealtimeHelper>();
    auto txThread  = std::make_unique<MinimalTxRxThread>(
        true, speechRate, modemRate,
        txHelper, rade, encState, fargan, radeText, &cbData);
    txThread->disableProcessing();

    // Create RX thread (modemRate in → speechRate out)
    auto rxHelper = std::make_shared<MinimalRealtimeHelper>();
    auto rxThread  = std::make_unique<MinimalTxRxThread>(
        false, modemRate, speechRate,
        rxHelper, rade, encState, fargan, radeText, &cbData);
    rxThread->disableProcessing();

    // Start both threads and let them initialise
    txThread->start(); rxThread->start();
    txThread->waitForReady(); rxThread->waitForReady();
    txThread->signalToStart(); rxThread->signalToStart();

    // Let both threads finish their initial clearFifos_() before we write
    // any audio so the data is not immediately wiped.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ----- Resampler: 16 kHz WAV → speechRate -----
    // ResampleStep pre-warms its r8brain state so the first output sample
    // corresponds to the first input sample.
    std::unique_ptr<ResampleStep> resampler;
    if (speechRate != RADE_SPEECH_SAMPLE_RATE)
        resampler = std::make_unique<ResampleStep>(RADE_SPEECH_SAMPLE_RATE, speechRate);

    // Buffer to accumulate all modem-rate samples from the TX thread
    std::vector<short> modemBuffer;
    modemBuffer.reserve(static_cast<size_t>(modemRate) * 60);

    // ----- TX phase -----
    // Feed WAV audio to the TX thread in 20 ms chunks (at 16 kHz = 320 samples).
    // If a resampler is active, convert each chunk to speechRate first.
    const int WAV_CHUNK = RADE_SPEECH_SAMPLE_RATE * FRAME_DURATION_MS / MS_TO_SEC; // 320

    size_t wavPos   = 0;
    const size_t wavTotal = wavAudio16k.size();

    auto drainTxOut = [&]()
    {
        int avail = cbData.outfifo1->numUsed();
        if (avail > 0)
        {
            size_t prev = modemBuffer.size();
            modemBuffer.resize(prev + static_cast<size_t>(avail));
            cbData.outfifo1->read(modemBuffer.data() + prev, avail);
        }
    };

    while (wavPos < wavTotal)
    {
        // Build one 16 kHz chunk (may be shorter at the end of the file)
        int rawLen = static_cast<int>(
            std::min(static_cast<size_t>(WAV_CHUNK), wavTotal - wavPos));
        const short* rawPtr = wavAudio16k.data() + wavPos;
        wavPos += static_cast<size_t>(rawLen);

        // Optionally resample to speechRate
        const short* speechPtr = rawPtr;
        int          speechLen = rawLen;
        std::vector<short> speechBuf;

        if (resampler)
        {
            int nout = 0;
            short* rsOut = resampler->execute(
                const_cast<short*>(rawPtr), rawLen, &nout);
            speechBuf.assign(rsOut, rsOut + nout);
            speechPtr = speechBuf.data();
            speechLen = nout;
        }

        // Write resampled chunk to TX infifo1, waiting for space if full
        int written = 0;
        while (written < speechLen)
        {
            int space = cbData.infifo1->numFree();
            if (space > 0)
            {
                int n = std::min(speechLen - written, space);
                cbData.infifo1->write(const_cast<short*>(speechPtr) + written, n);
                written += n;
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            drainTxOut();
        }
    }

    // Signal end of TX so the thread sends the RADE EOO burst
    endingTx.store(true, std::memory_order_release);

    // Wait until the TX thread signals EOO, draining modem output continuously
    while (!g_eoo_enqueued.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        drainTxOut();
    }
    g_tx.store(false, std::memory_order_release);

    // Final drain window
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    drainTxOut();

    log_info("TX done: %zu modem samples (speechRate=%d modemRate=%d)",
             modemBuffer.size(), speechRate, modemRate);

    // Stop TX thread; this flushes and closes the TX feature file
    txThread->stop();

    // ----- RX phase -----
    // Feed the collected modem samples into the RX thread in 20 ms chunks.
    const int MODEM_CHUNK = modemRate * FRAME_DURATION_MS / MS_TO_SEC;
    const size_t totalModem = modemBuffer.size();
    size_t rxPos = 0;

    auto drainRxOut = [&]()
    {
        int avail = cbData.outfifo2->numUsed();
        if (avail > 0)
        {
            size_t prev = rxSpeech.size();
            rxSpeech.resize(prev + static_cast<size_t>(avail));
            cbData.outfifo2->read(rxSpeech.data() + prev, avail);
            return true;
        }
        return false;
    };

    while (rxPos < totalModem)
    {
        int space = cbData.infifo2->numFree();
        if (space > 0)
        {
            int n = std::min({static_cast<int>(totalModem - rxPos), space, MODEM_CHUNK});
            cbData.infifo2->write(modemBuffer.data() + rxPos, n);
            rxPos += static_cast<size_t>(n);
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        drainRxOut();
    }

    log_info("All modem samples fed to RX (speechRate=%d modemRate=%d)",
             speechRate, modemRate);

    short *silence = new short[MODEM_CHUNK];
    memset(silence, 0, sizeof(short) * MODEM_CHUNK);

    // Wait up to 5 minutes for the RX thread to drain infifo2.
    // RADE neural-network inference on CPU can be slower than real-time.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
    while (cbData.infifo2->numUsed() > 0)
    {
        // Write silence to force processing of anything still in the buffer.
        cbData.infifo2->write(silence, MODEM_CHUNK);

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!drainRxOut()) break;

        if (std::chrono::steady_clock::now() > deadline)
        {
            log_error("Timeout waiting for RX to drain (speechRate=%d modemRate=%d)",
                      speechRate, modemRate);
            break;
        }
    }

    delete[] silence;

    // Extra settling time for the RX pipeline's last frame
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Stop RX thread; this flushes and closes the RX feature file
    rxThread->stop();

    // Delete FIFOs after threads are fully stopped
    delete cbData.infifo1; delete cbData.outfifo1;
    delete cbData.infifo2; delete cbData.outfifo2;

    return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main()
{
    // Load the source WAV once; all pipeline runs reuse this buffer
#if defined(PYTHON_EXECUTABLE)
    const double speechSeconds = 3600; // the whole file, for loss.py
#else
    const double speechSeconds = 10;
#endif // defined(PYTHON_EXECUTABLE)
    const std::vector<short> wavAudio16k = loadSpeech16k(speechSeconds);
    if (wavAudio16k.empty())
    {
        printf("Could not load the speech sample (rade_src/wav/all.wav)\n");
        return 1;
    }
    log_info("Loaded WAV: %zu samples at 16000 Hz", wavAudio16k.size());
    const double inputSeconds = (double)wavAudio16k.size() / RADE_SPEECH_SAMPLE_RATE;
    const double inputRms = rmsOf(wavAudio16k, 0);

    // Global RADE init / finalise (called only once for the process)
    rade_initialize();

    // Test matrix – {speechRate, modemRate}
    //   speechRate – TX input  / RX output sample rate
    //   modemRate  – TX output / RX input  sample rate 
    struct TestCase { int speechRate; int modemRate; };
    const int ValidModemRates[] = {8000, 16000, 22050, 24000, 32000, 44100, 48000};
    const int ValidSpeechRates[] = {16000, 22050, 24000, 32000, 44100, 48000};
    TestCase cases[(sizeof(ValidModemRates) / sizeof(int)) * (sizeof(ValidSpeechRates) / sizeof(int))];

    int numCases = 0;
    for (int i = 0; i < (sizeof(ValidSpeechRates) / sizeof(int)); i++) {
        for (int j = 0; j < (sizeof(ValidModemRates) / sizeof(int)); j++) {
            cases[numCases++] = {ValidSpeechRates[i], ValidModemRates[j]};
        }
    }

    bool allPassed = true;

    for (int ci = 0; ci < numCases; ++ci)
    {
        const int speechRate = cases[ci].speechRate;
        const int modemRate  = cases[ci].modemRate;

        printf("\n==========================================================\n");
        printf("Test %d/%d: speechRate=%d  modemRate=%d\n",
               ci + 1, numCases, speechRate, modemRate);
        printf("==========================================================\n");
        fflush(stdout);

        // Temporary feature files for this configuration
        std::string txFeatFile = testMakeTempFile("mtxtx");
        std::string rxFeatFile = testMakeTempFile("mtxrx");
        assert(!txFeatFile.empty() && !rxFeatFile.empty());
        const char* txFeatPath = txFeatFile.c_str();
        const char* rxFeatPath = rxFeatFile.c_str();

        // Point the RADETransmitStep / RADEReceiveStep hooks at the new files.
        // Must be set before MinimalTxRxThread objects (and thus their internal
        // pipeline steps) are constructed inside runPipeline().
        utTxFeatureFile = txFeatPath;
        utRxFeatureFile = rxFeatPath;

        log_info("TX features: %s", txFeatPath);
        log_info("RX features: %s", rxFeatPath);

        // Fresh RADE / LPCNet / FARGAN state per test to avoid any
        // carry-over between configurations.
        char modelFile[1] = {0};
        struct rade* rade = rade_open(modelFile, RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_MODE_V2);
        assert(rade != nullptr);

        LPCNetEncState* encState = lpcnet_encoder_create();
        assert(encState != nullptr);

        FARGANState fargan;
        {
            float zeros[320]                         = {0};
            float in_features[5 * NB_TOTAL_FEATURES] = {0};
            fargan_init(&fargan);
            fargan_cont(&fargan, zeros, in_features);
        }

        rade_text_t radeText = rade_text_create();
        assert(radeText != nullptr);

        rade_text_generate_tx_string(radeText, "K6AQ", 4);

        // Must start false: it used to start true, so the check passed even
        // if the callsign never arrived.
        bool radeTextReceived = false;
        rade_text_set_rx_callback(radeText, [](rade_text_t, const char *txt_ptr, int length, void *state) {
            bool* pass = (bool*)state;
            *pass = (length == 4 && strncmp(txt_ptr, "K6AQ", length) == 0);
        }, &radeTextReceived);

        // Run the full TX → RX pipeline
        std::vector<short> rxSpeech;
        bool pipelineOk = runPipeline(wavAudio16k, speechRate, modemRate,
                                      rade, encState, &fargan, radeText, rxSpeech);

        // Tear down per-iteration RADE state now that threads are stopped
        // (feature files are fully written at this point)
        rade_text_destroy(radeText);
        rade_close(rade);
        lpcnet_encoder_destroy(encState);

        // The received speech must be about as long as what was sent, and
        // (past the first second, while RX acquires) at about its level.
        double rxSeconds = (double)rxSpeech.size() / speechRate;
        double levelDb = 20 * std::log10(rmsOf(rxSpeech, speechRate) / inputRms);
        bool speechOk = rxSeconds > 0.8 * inputSeconds && rxSeconds < 1.2 * inputSeconds + 1 &&
                        std::fabs(levelDb) < 6;
        printf("rx speech: %.1f s (sent %.1f s), level %+.1f dB\n", rxSeconds, inputSeconds, levelDb);

#if defined(PYTHON_EXECUTABLE)
        // Evaluate quality with loss.py
        bool lossOk = runLossCheck(txFeatPath, rxFeatPath);
#else
        bool lossOk = true;
        printf("loss.py: skipped (no Python configured)\n");
#endif // defined(PYTHON_EXECUTABLE)
        bool passed = pipelineOk && radeTextReceived && speechOk && lossOk;

        // Remove temp feature files
        std::remove(txFeatPath);
        std::remove(rxFeatPath);

        printf("callsign rx=%d\n", radeTextReceived);
        printf("speechRate=%-5d  modemRate=%-5d : %s\n\n",
               speechRate, modemRate, passed ? "PASS" : "FAIL");
        fflush(stdout);

        allPassed = allPassed && passed;
        if (!allPassed) break;
    }

    rade_finalize();

    printf("==========================================================\n");
    printf("Overall result: %s\n", allPassed ? "PASS" : "FAIL");
    printf("==========================================================\n");

    return allPassed ? 0 : 1;
}
