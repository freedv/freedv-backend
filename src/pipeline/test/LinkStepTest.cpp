// Tests for LinkStep, which carries audio from one pipeline (via its input
// step) to another (via its output step) through a shared FIFO -- in
// freedv-gui, equalized mic audio to the TX monitor output.

#include <memory>
#include <vector>

#include "LinkStep.h"
#include "PipelineTestCommon.h"

namespace {

std::vector<short> ramp(int count, short start)
{
    std::vector<short> result(count);
    for (int i = 0; i < count; i++)
    {
        result[i] = (short)(start + i);
    }
    return result;
}

bool write(IPipelineStep& input, std::vector<short> samples)
{
    int numOut = -1;
    short* out = input.execute(samples.data(), samples.size(), &numOut);
    // The input step is a sink: it produces nothing itself.
    return out == nullptr && numOut == 0;
}

std::vector<short> read(IPipelineStep& output, int requested)
{
    int numOut = -1;
    short* out = output.execute(nullptr, requested, &numOut);
    if (numOut <= 0)
    {
        return (out == nullptr && numOut == 0) ? std::vector<short>() : std::vector<short>(1, -1);
    }
    return std::vector<short>(out, out + numOut);
}

bool roundTrip()
{
    LinkStep link(8000);
    std::unique_ptr<IPipelineStep> input(link.getInputPipelineStep());
    std::unique_ptr<IPipelineStep> output(link.getOutputPipelineStep());

    bool result = link.getSampleRate() == 8000;
    result &= input->getInputSampleRate() == 8000 && input->getOutputSampleRate() == 8000;
    result &= output->getInputSampleRate() == 8000 && output->getOutputSampleRate() == 8000;

    result &= write(*input, ramp(160, 0));
    result &= write(*input, ramp(160, 160));
    result &= read(*output, 320) == ramp(320, 0);
    return result;
}

bool partialReads()
{
    LinkStep link(8000);
    std::unique_ptr<IPipelineStep> input(link.getInputPipelineStep());
    std::unique_ptr<IPipelineStep> output(link.getOutputPipelineStep());

    bool result = write(*input, ramp(300, 0));
    // Never returns more than requested, and preserves order across reads.
    result &= read(*output, 100) == ramp(100, 0);
    result &= read(*output, 100) == ramp(100, 100);
    // Asking for more than is buffered returns what's there.
    result &= read(*output, 500) == ramp(100, 200);
    // Empty: nothing, and a null pointer.
    result &= read(*output, 100).empty();
    return result;
}

bool drainAllWhenNoCountGiven()
{
    LinkStep link(8000);
    std::unique_ptr<IPipelineStep> input(link.getInputPipelineStep());
    std::unique_ptr<IPipelineStep> output(link.getOutputPipelineStep());

    // numInputSamples == 0 means "everything that's buffered".
    bool result = write(*input, ramp(500, 0));
    result &= read(*output, 0) == ramp(500, 0);
    result &= read(*output, 0).empty();
    return result;
}

bool outputNeverExceedsOneSecond()
{
    // The output step's buffer holds one second of audio, but the FIFO
    // defaults to 48000 samples regardless of rate. With more than a second
    // buffered (e.g. the monitor pipeline stalled), a drain-all or oversized
    // request must still be bounded by the buffer, with the rest returned by
    // later calls, in order.
    LinkStep link(8000);
    std::unique_ptr<IPipelineStep> input(link.getInputPipelineStep());
    std::unique_ptr<IPipelineStep> output(link.getOutputPipelineStep());

    bool result = true;
    for (int i = 0; i < 3; i++)
    {
        result &= write(*input, ramp(4000, (short)(i * 4000)));
    }

    auto first = read(*output, 0);
    result &= first.size() == 8000;
    result &= first == ramp(8000, 0);

    auto second = read(*output, 10000);
    result &= second == ramp(4000, 8000);
    return result;
}

bool fullFifoDropsWholeBlock()
{
    LinkStep link(8000, 1000);
    std::unique_ptr<IPipelineStep> input(link.getInputPipelineStep());
    std::unique_ptr<IPipelineStep> output(link.getOutputPipelineStep());

    // A block that doesn't fit is dropped entirely (never partially written),
    // so the reader never sees a torn block.
    bool result = write(*input, ramp(900, 0));
    result &= write(*input, ramp(200, 900));
    result &= read(*output, 0) == ramp(900, 0);
    return result;
}

bool clearFifoDiscards()
{
    LinkStep link(8000);
    std::unique_ptr<IPipelineStep> input(link.getInputPipelineStep());
    std::unique_ptr<IPipelineStep> output(link.getOutputPipelineStep());

    bool result = write(*input, ramp(400, 0));
    link.clearFifo();
    result &= read(*output, 0).empty();

    result &= write(*input, ramp(10, 1000));
    result &= read(*output, 0) == ramp(10, 1000);
    return result;
}

bool nullOrEmptyInputIgnored()
{
    LinkStep link(8000);
    std::unique_ptr<IPipelineStep> input(link.getInputPipelineStep());
    std::unique_ptr<IPipelineStep> output(link.getOutputPipelineStep());

    int numOut = -1;
    bool result = input->execute(nullptr, 160, &numOut) == nullptr && numOut == 0;
    short dummy = 0;
    result &= input->execute(&dummy, 0, &numOut) == nullptr && numOut == 0;
    result &= read(*output, 0).empty();
    return result;
}

} // namespace

int main()
{
    TEST_CASE(roundTrip);
    TEST_CASE(partialReads);
    TEST_CASE(drainAllWhenNoCountGiven);
    TEST_CASE(outputNeverExceedsOneSecond);
    TEST_CASE(fullFifoDropsWholeBlock);
    TEST_CASE(clearFifoDiscards);
    TEST_CASE(nullOrEmptyInputIgnored);
    return 0;
}
