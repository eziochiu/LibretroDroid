/*
 *     Copyright (C) 2019  Filippo Scognamiglio
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "log.h"

#include "audio.h"
#include <android/log.h>
#include <cmath>
#include <memory>

namespace libretrodroid {

Audio::Audio(int32_t sampleRate, double refreshRate, bool preferLowLatencyAudio) {
    LOGI("Audio initialization has been called with input sample rate %d", sampleRate);

    contentRefreshRate = refreshRate;
    inputSampleRate = sampleRate;
    audioLatencySettings = findBestLatencySettings(preferLowLatencyAudio);
    initializeStream();
}

bool Audio::initializeStream() {
    LOGI("Using low latency stream: %d", audioLatencySettings->useLowLatencyStream);

    int32_t audioBufferSize = computeAudioBufferSize();

    oboe::AudioStreamBuilder builder;
    builder.setChannelCount(2);
    builder.setDirection(oboe::Direction::Output);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setDataCallback(this);
    builder.setErrorCallback(this);

    if (audioLatencySettings->useLowLatencyStream) {
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    } else {
        builder.setFramesPerCallback(audioBufferSize / 10);
    }

    oboe::Result result = builder.openManagedStream(stream);
    if (result == oboe::Result::OK) {
        baseConversionFactor = (double) inputSampleRate / stream->getSampleRate();
        fifoBuffer = std::make_unique<oboe::FifoBuffer>(2, audioBufferSize);
        temporaryAudioBuffer = std::unique_ptr<int16_t[]>(new int16_t[audioBufferSize]);
        latencyTuner = std::make_unique<oboe::LatencyTuner>(*stream);
        maybeLogDiagnosticsLocked(
            "init",
            -1,
            -1,
            -1,
            -1,
            stream->getFramesPerBurst(),
            1.0,
            true
        );
        return true;
    } else {
        LOGE("Failed to create stream. Error: %s", oboe::convertToText(result));
        stream = nullptr;
        latencyTuner = nullptr;
        return false;
    }
}

std::unique_ptr<Audio::AudioLatencySettings> Audio::findBestLatencySettings(bool preferLowLatencyAudio) {
    if (oboe::AudioStreamBuilder::isAAudioRecommended() && preferLowLatencyAudio) {
        return std::make_unique<AudioLatencySettings>(LOW_LATENCY_SETTINGS);
    } else {
        return std::make_unique<AudioLatencySettings>(DEFAULT_LATENCY_SETTINGS);
    }
}

int32_t Audio::computeAudioBufferSize() {
    double maxLatency = computeMaximumLatency();
    LOGI("Average audio latency set to: %f ms", maxLatency * 0.5);
    double sampleRateDivisor = 500.0 / maxLatency;
    return roundToEven(inputSampleRate / sampleRateDivisor);
}

double Audio::computeMaximumLatency() const {
    double maxLatency = (audioLatencySettings->bufferSizeInVideoFrames / contentRefreshRate) * 1000;
    return std::max(maxLatency, 32.0);
}

void Audio::start() {
    startRequested = true;
    if (stream != nullptr)
        stream->requestStart();
}

void Audio::stop() {
    startRequested = false;
    if (stream != nullptr)
        stream->requestStop();
}

void Audio::write(const int16_t *data, size_t frames) {
    int32_t fifoFramesRequested = static_cast<int32_t>(frames * 2);
    if (audioSyncEnabled) {
        waitForSpace(fifoFramesRequested);
    }
    int32_t fifoFramesWritten = fifoBuffer->write(data, fifoFramesRequested);

    std::lock_guard<std::mutex> lock(diagnosticsMutex);
    totalWriteCalls++;
    totalWriteFramesRequested += fifoFramesRequested;
    totalWriteFramesAccepted += std::max(fifoFramesWritten, 0);
    maybeLogDiagnosticsLocked(
        "write",
        fifoFramesRequested,
        fifoFramesWritten,
        -1,
        -1,
        -1,
        -1.0
    );
}

void Audio::waitForSpace(size_t neededSamples) {
    std::unique_lock<std::mutex> lock(bufferMutex);
    bufferCondition.wait_for(lock, std::chrono::milliseconds(100), [this, neededSamples] {
        int32_t capacity = fifoBuffer->getBufferCapacityInFrames() * 2;
        int32_t available = fifoBuffer->getFullFramesAvailable() * 2;
        int32_t freeSpace = capacity - available;
        
        return freeSpace >= (int32_t)neededSamples && (available * 100 / capacity) < 75;
    });
}

void Audio::setAudioSyncEnabled(bool enabled) {
    audioSyncEnabled = enabled;
    LOGI("AudioSync %s", enabled ? "enabled" : "disabled");
}

void Audio::setPlaybackSpeed(const double newPlaybackSpeed) {
    playbackSpeed = newPlaybackSpeed;
}

oboe::DataCallbackResult Audio::onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) {
    double dynamicBufferFactor = computeDynamicBufferConversionFactor(0.001 * numFrames);
    double finalConversionFactor = baseConversionFactor * dynamicBufferFactor * playbackSpeed;

    // When using low-latency stream, numFrames is very low (~100) and the dynamic buffer scaling doesn't work with rounding.
    // By keeping track of the "fractional" frames we can keep the error smaller.
    framesToSubmit += numFrames * finalConversionFactor;
    int32_t currentFramesToSubmit = std::round(framesToSubmit);
    framesToSubmit -= currentFramesToSubmit;

    int32_t fifoFramesRequested = currentFramesToSubmit * 2;
    int32_t fifoFramesRead = fifoBuffer->readNow(temporaryAudioBuffer.get(), fifoFramesRequested);

    auto outputArray = reinterpret_cast<int16_t *>(audioData);
    resampler.resample(temporaryAudioBuffer.get(), currentFramesToSubmit, outputArray, numFrames);

    {
        std::lock_guard<std::mutex> lock(diagnosticsMutex);
        totalReadCallbacks++;
        totalReadFramesRequested += std::max(fifoFramesRequested, 0);
        totalReadFramesActual += std::max(fifoFramesRead, 0);
        maybeLogDiagnosticsLocked(
            "callback",
            -1,
            -1,
            fifoFramesRequested,
            fifoFramesRead,
            numFrames,
            dynamicBufferFactor
        );
    }

    latencyTuner->tune();

    // 通知可能在等待的写入线程：缓冲区有空间了
    if (audioSyncEnabled) {
        bufferCondition.notify_one();
    }

    return oboe::DataCallbackResult::Continue;
}

// To prevent audio buffer overruns or underruns we set up a PI controller. The idea is to run the
// audio slower when the buffer is empty and faster when it's full.
double Audio::computeDynamicBufferConversionFactor(double dt) {
    double framesCapacityInBuffer = fifoBuffer->getBufferCapacityInFrames();
    double framesAvailableInBuffer = fifoBuffer->getFullFramesAvailable();

    // Error is represented by normalized distance to half buffer utilization. Range [-1.0, 1.0]
    double errorMeasure = (framesCapacityInBuffer - 2.0f * framesAvailableInBuffer) / framesCapacityInBuffer;

    errorIntegral += errorMeasure * dt;

    // Wikipedia states that human ear resolution is around 3.6 Hz within the octave of 1000–2000 Hz.
    // This changes continuously, so we should try to keep it a very low value.
    double proportionalAdjustment = std::clamp(kp * errorMeasure, -maxp, maxp);

    // Ki is a lot lower, so it's safe if it exceeds the ear threshold. Hopefully convergence will
    // be slow enough to be not perceptible. We need to battle test this value.
    double integralAdjustment = std::clamp(ki * errorIntegral, -maxi, maxi);

    double finalAdjustment = proportionalAdjustment + integralAdjustment;

    LOGD("Audio speed adjustments (p: %f) (i: %f)", proportionalAdjustment, integralAdjustment);

    return 1.0 - (finalAdjustment);
}

void Audio::maybeLogDiagnosticsLocked(
    const char* event,
    int32_t writeRequestedFrames,
    int32_t writeAcceptedFrames,
    int32_t readRequestedFrames,
    int32_t readActualFrames,
    int32_t outputFrames,
    double dynamicBufferFactor,
    bool force
) {
    auto now = DiagnosticClock::now();
    bool partialWrite =
        writeRequestedFrames >= 0 &&
        writeAcceptedFrames >= 0 &&
        writeAcceptedFrames < writeRequestedFrames;
    bool underrun =
        readRequestedFrames >= 0 &&
        readActualFrames >= 0 &&
        readActualFrames < readRequestedFrames;
    bool periodic =
        lastDiagnosticsLogTime == DiagnosticClock::time_point::min() ||
        now - lastDiagnosticsLogTime >= std::chrono::seconds(1);

    if (!force && !partialWrite && !underrun && !periodic) {
        return;
    }

    lastDiagnosticsLogTime = now;

    uint32_t fifoCapacity = fifoBuffer ? fifoBuffer->getBufferCapacityInFrames() : 0;
    uint32_t fifoAvailable = fifoBuffer ? fifoBuffer->getFullFramesAvailable() : 0;
    int32_t streamSampleRate = stream ? stream->getSampleRate() : 0;
    int32_t framesPerBurst = stream ? stream->getFramesPerBurst() : 0;

    __android_log_print(
        ANDROID_LOG_INFO,
        MODULE_NAME,
        "AudioDiag[%s]: inputRate=%d streamRate=%d contentFps=%.3f baseFactor=%.6f dynFactor=%.6f playback=%.3f "
        "fifo=%u/%u framesPerBurst=%d writeReq=%d writeOk=%d readReq=%d readOk=%d outFrames=%d "
        "totals(writeCalls=%llu writeReq=%llu writeOk=%llu readCallbacks=%llu readReq=%llu readOk=%llu)",
        event,
        inputSampleRate,
        streamSampleRate,
        contentRefreshRate,
        baseConversionFactor,
        dynamicBufferFactor,
        playbackSpeed,
        fifoAvailable,
        fifoCapacity,
        framesPerBurst,
        writeRequestedFrames,
        writeAcceptedFrames,
        readRequestedFrames,
        readActualFrames,
        outputFrames,
        static_cast<unsigned long long>(totalWriteCalls),
        static_cast<unsigned long long>(totalWriteFramesRequested),
        static_cast<unsigned long long>(totalWriteFramesAccepted),
        static_cast<unsigned long long>(totalReadCallbacks),
        static_cast<unsigned long long>(totalReadFramesRequested),
        static_cast<unsigned long long>(totalReadFramesActual)
    );
}

int32_t Audio::roundToEven(int32_t x) {
    return (x / 2) * 2;
}

void Audio::onErrorAfterClose(oboe::AudioStream* oldStream, oboe::Result result) {
    AudioStreamErrorCallback::onErrorAfterClose(oldStream, result);
    LOGI("Stream error in oboe::onErrorAfterClose %s", oboe::convertToText(result));

    if (result != oboe::Result::ErrorDisconnected)
        return;

    initializeStream();
    if (startRequested) {
        start();
    }
}

} //namespace libretrodroid
