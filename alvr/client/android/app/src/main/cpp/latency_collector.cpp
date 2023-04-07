#include "latency_collector.h"
#ifndef ALXR_CLIENT
    #include "utils.h"
    #include "bindings.h"
#else
#include <chrono>

namespace {
#if 0//def __linux__
    struct XrSteadyClock : std::chrono::steady_clock
    {
        using time_point = std::chrono::time_point<XrSteadyClock, duration>;
        constexpr static const bool is_steady = true;
        static inline time_point now() noexcept
        {
            using namespace std::chrono;
            struct timespec ts;
            clock_gettime(CLOCK_BOOTTIME, &ts);
            return time_point(seconds(ts.tv_sec) + nanoseconds(ts.tv_nsec));
        }
    };
#else
    using XrSteadyClock = std::chrono::steady_clock;
#endif
    constexpr const std::uint64_t USECS_IN_SEC = 1000 * 1000;

    inline std::uint64_t getTimestampUs() {
        using namespace std::chrono;
        using ClockType = XrSteadyClock;
        static_assert(ClockType::is_steady);
        using microsecondsU64 = duration<std::uint64_t, microseconds::period>;
        return duration_cast<microsecondsU64>(ClockType::now().time_since_epoch()).count();
    }
    inline void FrameLog(...) {}
}
#endif

LatencyCollector LatencyCollector::m_Instance;

LatencyCollector::LatencyCollector()
: m_StatisticsTime(getTimestampUs() / USECS_IN_SEC) {
}

LatencyCollector::FrameTimestamp &LatencyCollector::getFrame(uint64_t frameIndex) {
    std::scoped_lock<std::mutex> lock(m_framesMutex);
    if (m_Frames.size() > MAX_FRAMES) {
        m_Frames.erase(m_Frames.cbegin());
    }
    auto &frame = m_Frames[frameIndex];
    frame.frameIndex = frameIndex;
    return frame;
}

void LatencyCollector::setTotalLatency(uint32_t latency) {
    if (latency < 2e5)
        m_ServerTotalLatency.store(latency * 0.05 + m_ServerTotalLatency.load() * 0.95);
}
void LatencyCollector::tracking(uint64_t frameIndex) {
    getFrame(frameIndex).tracking = getTimestampUs();
}
void LatencyCollector::estimatedSent(uint64_t frameIndex, uint64_t offset) {
    getFrame(frameIndex).estimatedSent = getTimestampUs() + offset;
}
void LatencyCollector::received(uint64_t frameIndex) {
    getFrame(frameIndex).received = getTimestampUs(); // Round trip
}
void LatencyCollector::receivedFirst(uint64_t frameIndex) {
    getFrame(frameIndex).receivedFirst = getTimestampUs();
}
void LatencyCollector::receivedLast(uint64_t frameIndex) {
    getFrame(frameIndex).receivedLast = getTimestampUs();
}
void LatencyCollector::decoderInput(uint64_t frameIndex) {
    getFrame(frameIndex).decoderInput = getTimestampUs();
}
void LatencyCollector::decoderOutput(uint64_t frameIndex) {
    getFrame(frameIndex).decoderOutput = getTimestampUs();
}
void LatencyCollector::rendered1(uint64_t frameIndex) {
    getFrame(frameIndex).rendered1 = getTimestampUs();
}
void LatencyCollector::rendered2(uint64_t frameIndex) {
    getFrame(frameIndex).rendered2 = getTimestampUs();
}

void LatencyCollector::submit(uint64_t frameIndex) {
    FrameTimestamp timestamp = getFrame(frameIndex);
    timestamp.submit = getTimestampUs();

    m_Latency[0] = timestamp.submit - timestamp.tracking;
    if (timestamp.decoderInput >= timestamp.decoderOutput)
        m_Latency[2] = 0;
    else
        m_Latency[2] = timestamp.decoderOutput - timestamp.decoderInput;
    if (timestamp.received) {
        m_Latency[3] = (timestamp.received - timestamp.tracking) / 2;
        m_Latency[1] = timestamp.receivedLast - timestamp.receivedFirst + m_Latency[3];
    } else {
        m_Latency[3] = 0;
        m_Latency[1] = timestamp.receivedLast - timestamp.receivedFirst;
    }
    if (timestamp.decoderOutput >= timestamp.rendered2)
        m_Latency[4] = 0;
    else
        m_Latency[4] = timestamp.rendered2 - timestamp.decoderOutput;

    submitNewFrame();

    m_FramesInSecond = 1000000.0 / (timestamp.submit - m_LastSubmit);
    m_LastSubmit = timestamp.submit;
#ifndef NDEBUG
    FrameLog(frameIndex, "totalLatency=%.1f transportLatency=%.1f decodeLatency=%.1f renderLatency1=%.1f renderLatency2=%.1f"
            , m_Latency[0] / 1000.0, m_Latency[1] / 1000.0, m_Latency[2] / 1000.0
            , (timestamp.rendered2 - timestamp.decoderOutput) / 1000.0
            , (timestamp.submit - timestamp.rendered2) / 1000.0);
#endif
}

void LatencyCollector::resetAll() {
    m_PacketsLostTotal = 0;
    m_PacketsLostInSecond = 0;
    m_PacketsLostPrevious = 0;

    m_FecFailureTotal = 0;
    m_FecFailureInSecond = 0;
    m_FecFailurePrevious = 0;

    m_FramesInSecond = 0;
    m_LastSubmit = 0;

    for(int i = 0; i < 5; i++) {
        m_Latency[i] = 0;
    }

    {
        std::scoped_lock l(m_framesMutex);
        m_Frames.clear();
    }
    m_ServerTotalLatency.store(0);

    m_StatisticsTime = getTimestampUs() / USECS_IN_SEC;
}

void LatencyCollector::resetSecond(){
    m_PacketsLostPrevious = m_PacketsLostInSecond;
    m_PacketsLostInSecond = 0;

    m_FecFailurePrevious = m_FecFailureInSecond;
    m_FecFailureInSecond = 0;
}

void LatencyCollector::checkAndResetSecond() {
    uint64_t current = getTimestampUs() / USECS_IN_SEC;
    if(m_StatisticsTime != current){
        m_StatisticsTime = current;
        resetSecond();
    }
}

void LatencyCollector::packetLoss(int64_t lost) {
    checkAndResetSecond();

    m_PacketsLostTotal += lost;
    m_PacketsLostInSecond += lost;
}

void LatencyCollector::fecFailure() {
    checkAndResetSecond();

    m_FecFailureTotal++;
    m_FecFailureInSecond++;
}

void LatencyCollector::submitNewFrame() {
    checkAndResetSecond();
}

uint64_t LatencyCollector::getTrackingPredictionLatency() const {
    const auto predictionLatency = m_ServerTotalLatency.load();
    return predictionLatency > 2e5 ? 2e5 : predictionLatency;
}

uint64_t LatencyCollector::getLatency(uint32_t i) const {
    return m_Latency[i];
}
uint64_t LatencyCollector::getPacketsLostTotal() const {
    return m_PacketsLostTotal;
}
uint64_t LatencyCollector::getPacketsLostInSecond() const {
    return m_PacketsLostPrevious;
}
uint64_t LatencyCollector::getFecFailureTotal() const {
    return m_FecFailureTotal;
}
uint64_t LatencyCollector::getFecFailureInSecond() const {
    return m_FecFailurePrevious;
}
float LatencyCollector::getFramesInSecond() const {
    return m_FramesInSecond;
}

LatencyCollector &LatencyCollector::Instance() {
    return m_Instance;
}

void decoderInput(long long frameIndex) {
    LatencyCollector::Instance().decoderInput((uint64_t)frameIndex);
}

void decoderOutput(long long frameIndex) {
    LatencyCollector::Instance().decoderOutput((uint64_t)frameIndex);
}
