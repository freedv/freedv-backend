// End-to-end tests of the RADE steps without Python: real speech through
// RADETransmitStep and straight into RADEReceiveStep must sync, report a
// high SNR and come out as speech at a sensible level; noise alone must not
// sync or produce audio; and once sync is lost the SNR stops changing.

#include <cmath>
#include <memory>
#include <random>
#include <vector>

#include "RADETransmitStep.h"
#include "RADEReceiveStep.h"
#include "PipelineTestCommon.h"
#include "SpeechSample.h"

std::string utTxFeatureFile;
std::string utRxFeatureFile;

namespace {

constexpr int SPEECH_BLOCK = RADE_SPEECH_SAMPLE_RATE / 50; // 20 ms
constexpr int MODEM_BLOCK = RADE_MODEM_SAMPLE_RATE / 50;

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

double rms(const std::vector<short>& samples, size_t start = 0)
{
    double sum = 0;
    for (size_t i = start; i < samples.size(); i++)
    {
        sum += (double)samples[i] * samples[i];
    }
    return samples.size() > start ? std::sqrt(sum / (samples.size() - start)) : 0;
}

std::vector<short> makeNoise(size_t count, double level, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> dist(0, level);
    std::vector<short> out(count);
    for (auto& s : out)
    {
        s = (short)std::max(-32767.0, std::min(32767.0, dist(rng)));
    }
    return out;
}

// Feeds modem audio to the receiver in 20 ms blocks, collecting its speech
// output. Returns whether it was in sync at the end.
bool receive(RADEReceiveStep& rx, std::vector<short>& modem, std::vector<short>& speechOut)
{
    for (size_t pos = 0; pos + MODEM_BLOCK <= modem.size(); pos += MODEM_BLOCK)
    {
        int numOut = 0;
        short* result = rx.execute(&modem[pos], MODEM_BLOCK, &numOut);
        speechOut.insert(speechOut.end(), result, result + numOut);
    }
    return rx.getSync() != 0;
}

} // namespace

bool speechLoopsBack()
{
    auto speech = loadSpeech16k(8.0);
    if (speech.empty())
    {
        std::cerr << "[SKIP: speech sample not found]...";
        return true;
    }

    RadeSession session;
    std::unique_ptr<RADETransmitStep> tx(session.makeTx());
    std::unique_ptr<RADEReceiveStep> rx(session.makeRx());

    std::vector<short> modem;
    for (size_t pos = 0; pos + SPEECH_BLOCK <= speech.size(); pos += SPEECH_BLOCK)
    {
        int numOut = 0;
        short* result = tx->execute(&speech[pos], SPEECH_BLOCK, &numOut);
        modem.insert(modem.end(), result, result + numOut);
    }

    std::vector<short> received;
    bool inSync = receive(*rx, modem, received);
    int snr = rx->getSnr();

    // Allow two seconds for acquisition before comparing levels.
    double speechDb = 20 * std::log10(rms(speech, 2 * RADE_SPEECH_SAMPLE_RATE));
    double receivedDb = 20 * std::log10(rms(received, received.size() > (size_t)RADE_SPEECH_SAMPLE_RATE ? RADE_SPEECH_SAMPLE_RATE : 0));
    bool result = inSync && snr >= 15 &&
                  received.size() > speech.size() / 2 &&
                  std::abs(receivedDb - speechDb) < 10;
    if (!result)
    {
        std::cerr << "[sync=" << inSync << " SNR=" << snr << " dB, speech level " << speechDb << " dB in, "
                  << receivedDb << " dB out over " << received.size() << " samples]...";
    }
    return result;
}

bool noiseDoesNotSync()
{
    RadeSession session;
    std::unique_ptr<RADEReceiveStep> rx(session.makeRx());

    auto noise = makeNoise(10 * RADE_MODEM_SAMPLE_RATE, 3000, 7);
    std::vector<short> received;
    bool everSynced = false;
    for (size_t pos = 0; pos + MODEM_BLOCK <= noise.size(); pos += MODEM_BLOCK)
    {
        int numOut = 0;
        short* result = rx->execute(&noise[pos], MODEM_BLOCK, &numOut);
        received.insert(received.end(), result, result + numOut);
        everSynced |= rx->getSync() != 0;
    }
    if (everSynced || !received.empty())
    {
        std::cerr << "[noise synced=" << everSynced << ", " << received.size() << " samples out]...";
        return false;
    }
    return true;
}

bool snrHeldOnceSyncIsLost()
{
    auto speech = loadSpeech16k(6.0);
    if (speech.empty())
    {
        std::cerr << "[SKIP: speech sample not found]...";
        return true;
    }

    RadeSession session;
    std::unique_ptr<RADETransmitStep> tx(session.makeTx());
    std::unique_ptr<RADEReceiveStep> rx(session.makeRx());

    std::vector<short> modem;
    for (size_t pos = 0; pos + SPEECH_BLOCK <= speech.size(); pos += SPEECH_BLOCK)
    {
        int numOut = 0;
        short* result = tx->execute(&speech[pos], SPEECH_BLOCK, &numOut);
        modem.insert(modem.end(), result, result + numOut);
    }
    std::vector<short> received;
    bool synced = receive(*rx, modem, received);
    int snrInSync = rx->getSnr();

    // The signal stops (no end-of-over): only noise from now on. The
    // receiver holds sync for a moment, measuring the noise, then drops
    // it. From then on the SNR must stay at the last value measured in sync
    // rather than tracking a signal that isn't there.
    auto noise = makeNoise(10 * RADE_MODEM_SAMPLE_RATE, 300, 11);
    int snrAtLoss = 0;
    bool lostSync = false;
    bool snrChangedAfterLoss = false;
    for (size_t pos = 0; pos + MODEM_BLOCK <= noise.size(); pos += MODEM_BLOCK)
    {
        int numOut = 0;
        rx->execute(&noise[pos], MODEM_BLOCK, &numOut);
        if (!lostSync && rx->getSync() == 0)
        {
            lostSync = true;
            snrAtLoss = rx->getSnr();
        }
        else if (lostSync && rx->getSnr() != snrAtLoss)
        {
            snrChangedAfterLoss = true;
        }
    }
    if (!synced || !lostSync || snrChangedAfterLoss)
    {
        std::cerr << "[synced=" << synced << " (SNR " << snrInSync << " dB), lost sync=" << lostSync
                  << ", SNR changed after losing sync=" << snrChangedAfterLoss << "]...";
        return false;
    }
    return true;
}

int main()
{
    rade_initialize();

    TEST_CASE(speechLoopsBack);
    TEST_CASE(noiseDoesNotSync);
    TEST_CASE(snrHeldOnceSyncIsLost);

    rade_finalize();
    return 0;
}
