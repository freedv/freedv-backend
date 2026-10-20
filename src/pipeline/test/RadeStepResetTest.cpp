// Tests that reset() on the RADE transmit and receive steps discards the
// state those steps own: buffered input samples, partially collected
// features and queued EOO (end-of-over) samples.
//
// The RADE, LPCNet and FARGAN state objects are owned by the caller and
// passed in, so reset() can't (and shouldn't) reset them. The tests are built
// so those objects are untouched where outputs are compared bit-exactly:
// samples that stay buffered inside the step (fewer than one frame) never
// reach RADE/LPCNet, so after reset() the step must behave exactly like a
// fresh one on equally fresh RADE state.

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "RADETransmitStep.h"
#include "RADEReceiveStep.h"
#include "PipelineTestCommon.h"

// Unit-test hooks read by the RADE steps; empty disables feature dumps.
std::string utTxFeatureFile;
std::string utRxFeatureFile;

namespace {

// One independent set of RADE/LPCNet/FARGAN state, as MinimalTxRxThread sets
// up per mode.
struct RadeSession
{
    struct rade* dv = nullptr;
    LPCNetEncState* encState = nullptr;
    FARGANState fargan;

    RadeSession()
    {
        char modelFile[1] = {0};
        dv = rade_open(modelFile, RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_MODE_V2);
        assert(dv != nullptr);
        encState = lpcnet_encoder_create();
        assert(encState != nullptr);

        float zeros[320] = {0};
        float inFeatures[5 * NB_TOTAL_FEATURES] = {0};
        fargan_init(&fargan);
        fargan_cont(&fargan, zeros, inFeatures);
    }

    ~RadeSession()
    {
        lpcnet_encoder_destroy(encState);
        rade_close(dv);
    }

    RADETransmitStep* makeTx() { return new RADETransmitStep(dv, encState); }

    RADEReceiveStep* makeRx()
    {
        return new RADEReceiveStep(
            dv, &fargan, nullptr,
            +[](RADEReceiveStep*) FREEDV_NONBLOCKING {},
            +[]() FREEDV_NONBLOCKING { return 0.0f; });
    }
};

// Speech-band test signal: a few harmonics with slow amplitude modulation
// plus a little noise, so the encoder has something voice-like to work on.
std::vector<short> makeSpeechLike(int sampleRate, float seconds)
{
    std::vector<short> result((size_t)(sampleRate * seconds));
    unsigned state = 12345;
    for (size_t n = 0; n < result.size(); n++)
    {
        float t = (float)n / sampleRate;
        float envelope = 0.5f + 0.5f * sinf(2.0f * (float)M_PI * 3.0f * t);
        float v = 0;
        for (int h = 1; h <= 5; h++)
        {
            v += sinf(2.0f * (float)M_PI * 150.0f * h * t) / h;
        }
        state = state * 1664525u + 1013904223u;
        float noise = ((float)(state >> 16) / 65535.0f - 0.5f) * 0.05f;
        result[n] = (short)(6000.0f * envelope * v + 6000.0f * noise);
    }
    return result;
}

// Feeds the signal through the step in 10 ms chunks. Returns everything the
// step produced; per-chunk output counts go to `counts` if given.
std::vector<short> run(IPipelineStep& step, const std::vector<short>& input, std::vector<int>* counts = nullptr)
{
    std::vector<short> output;
    int chunk = step.getInputSampleRate() / 100;
    std::vector<short> buf(chunk);
    for (size_t offset = 0; offset + chunk <= input.size(); offset += chunk)
    {
        std::copy(input.begin() + offset, input.begin() + offset + chunk, buf.begin());
        int numOut = 0;
        short* out = step.execute(buf.data(), chunk, &numOut);
        output.insert(output.end(), out, out + numOut);
        if (counts) counts->push_back(numOut);
    }
    return output;
}

bool sameSamples(const std::vector<short>& actual, const std::vector<short>& expected)
{
    if (actual.size() != expected.size())
    {
        std::cerr << "[" << actual.size() << " samples vs " << expected.size() << " from a fresh step]...";
        return false;
    }
    for (size_t i = 0; i < actual.size(); i++)
    {
        if (actual[i] != expected[i])
        {
            std::cerr << "[first difference at sample " << i << " of " << actual.size() << "]...";
            return false;
        }
    }
    return true;
}

const std::vector<short>& speech()
{
    static std::vector<short> signal = makeSpeechLike(RADE_SPEECH_SAMPLE_RATE, 3.0f);
    return signal;
}

bool txResetDiscardsBufferedSamples()
{
    std::vector<short> expected;
    {
        RadeSession session;
        std::unique_ptr<RADETransmitStep> fresh(session.makeTx());
        expected = run(*fresh, speech());
    }

    RadeSession session;
    std::unique_ptr<RADETransmitStep> step(session.makeTx());

    // Less than one LPCNet frame: stays in the step's input FIFO and never
    // reaches the encoder.
    std::vector<short> partial(LPCNET_FRAME_SIZE / 2, 20000);
    int numOut = -1;
    step->execute(partial.data(), partial.size(), &numOut);
    bool result = numOut == 0;

    step->reset();
    result &= !expected.empty();
    result &= sameSamples(run(*step, speech()), expected);
    return result;
}

bool txResetDiscardsPartialFeatures()
{
    std::vector<int> expectedCounts;
    {
        RadeSession session;
        std::unique_ptr<RADETransmitStep> fresh(session.makeTx());
        run(*fresh, speech(), &expectedCounts);
    }

    RadeSession session;
    std::unique_ptr<RADETransmitStep> step(session.makeTx());

    // A few whole frames: features are collected but not yet enough for a
    // RADE modem frame, so nothing is output.
    std::vector<short> frames(LPCNET_FRAME_SIZE * 3, 1000);
    int numOut = -1;
    step->execute(frames.data(), frames.size(), &numOut);
    bool result = numOut == 0;

    // After reset() the partial feature set must be gone: modem frames come
    // out after exactly as much input as for a fresh step. (Sample values
    // differ because the shared LPCNet encoder state has advanced.)
    step->reset();
    std::vector<int> counts;
    run(*step, speech(), &counts);
    result &= counts == expectedCounts;
    if (counts != expectedCounts)
    {
        size_t i = 0;
        while (i < counts.size() && i < expectedCounts.size() && counts[i] == expectedCounts[i]) i++;
        std::cerr << "[output timing differs from a fresh step at chunk " << i << "]...";
    }
    return result;
}

bool txResetDiscardsQueuedEoo()
{
    RadeSession session;
    std::unique_ptr<RADETransmitStep> step(session.makeTx());

    // restartVocoder() queues the EOO burst; a zero-length execute() drains it.
    step->restartVocoder();
    int numOut = -1;
    step->execute(nullptr, 0, &numOut);
    bool result = numOut == step->eooLengthInSamples() && numOut > 0;

    // Queued but reset before being drained: must be discarded.
    step->restartVocoder();
    step->reset();
    step->execute(nullptr, 0, &numOut);
    result &= numOut == 0;
    return result;
}

bool rxResetDiscardsBufferedSamples()
{
    // Modem signal from a separate transmitter session.
    std::vector<short> modem;
    {
        RadeSession session;
        std::unique_ptr<RADETransmitStep> tx(session.makeTx());
        modem = run(*tx, makeSpeechLike(RADE_SPEECH_SAMPLE_RATE, 6.0f));
    }

    std::vector<short> expected;
    int expectedSync = 0;
    {
        RadeSession session;
        std::unique_ptr<RADEReceiveStep> fresh(session.makeRx());
        expected = run(*fresh, modem);
        expectedSync = fresh->getSync();
    }

    RadeSession session;
    std::unique_ptr<RADEReceiveStep> step(session.makeRx());

    // Fewer samples than RADE needs for a frame: stays in the step's input
    // FIFO, so the shared RADE state is untouched.
    std::vector<short> junk(100, 15000);
    int numOut = -1;
    step->execute(junk.data(), junk.size(), &numOut);
    bool result = numOut == 0;

    step->reset();
    auto actual = run(*step, modem);

    // The fresh receiver must actually have decoded speech, or the
    // comparison below proves nothing.
    result &= expectedSync != 0 && !expected.empty();
    result &= sameSamples(actual, expected);
    result &= step->getSync() == expectedSync;
    return result;
}

bool rxStateAccessors()
{
    RadeSession session;
    std::unique_ptr<RADEReceiveStep> step(session.makeRx());

    static std::atomic<int> rxState(0);
    step->setRxStateFn(+[]() FREEDV_NONBLOCKING { return &rxState; });
    bool result = step->getRxStateFn()() == &rxState;

    int obj = 0;
    step->setStateObj(&obj);
    result &= step->getStateObj() == &obj;
    result &= step->getInputSampleRate() == RADE_MODEM_SAMPLE_RATE;
    result &= step->getOutputSampleRate() == RADE_SPEECH_SAMPLE_RATE;
    return result;
}

} // namespace

int main()
{
    rade_initialize();

    TEST_CASE(txResetDiscardsBufferedSamples);
    TEST_CASE(txResetDiscardsPartialFeatures);
    TEST_CASE(txResetDiscardsQueuedEoo);
    TEST_CASE(rxResetDiscardsBufferedSamples);
    TEST_CASE(rxStateAccessors);

    rade_finalize();
    return 0;
}
