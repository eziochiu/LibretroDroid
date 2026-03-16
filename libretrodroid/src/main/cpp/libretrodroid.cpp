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

#include <jni.h>

#include <EGL/egl.h>

#include <string>
#include <utility>
#include <vector>
#include <unordered_set>
#include <algorithm>

#include "libretrodroid.h"
#include "utils/libretrodroidexception.h"
#include "log.h"
#include "core.h"
#include "audio.h"
#include "video.h"
#include "renderers/renderer.h"
#include "fpssync.h"
#include "input.h"
#include "rumble.h"
#include "shadermanager.h"
#include "utils/javautils.h"
#include "environment.h"
#include "renderers/es3/framebufferrenderer.h"
#include "renderers/es2/imagerendereres2.h"
#include "renderers/es3/imagerendereres3.h"
#include "utils/utils.h"
#include "utils/rect.h"
#include "errorcodes.h"
#include "vfs/vfs.h"

namespace libretrodroid {

uintptr_t LibretroDroid::callback_get_current_framebuffer() {
    return LibretroDroid::getInstance().handleGetCurrentFrameBuffer();
}

void LibretroDroid::callback_hw_video_refresh(
    const void *data,
    unsigned width,
    unsigned height,
    size_t pitch
) {
    LOGD("hw video refresh callback called %i %i", width, height);
    LibretroDroid::getInstance().handleVideoRefresh(data, width, height, pitch);
}

void LibretroDroid::callback_audio_sample(int16_t left, int16_t right) {
    LOGE("callback audio sample (left, right) has been called");
}

size_t LibretroDroid::callback_set_audio_sample_batch(const int16_t *data, size_t frames) {
    return LibretroDroid::getInstance().handleAudioCallback(data, frames);
}

void LibretroDroid::callback_retro_set_input_poll() {
    // Do nothing in here...
}

int16_t LibretroDroid::callback_set_input_state(
    unsigned int port,
    unsigned int device,
    unsigned int index,
    unsigned int id
) {
    return LibretroDroid::getInstance().handleSetInputState(port, device, index, id);
}

void LibretroDroid::updateAudioSampleRateMultiplier() {
    if (audio) {
        audio->setPlaybackSpeed(frameSpeed.load());
    }
}

void LibretroDroid::resetPerformanceDiagnostics() {
    performanceWindowStart = PerformanceClock::time_point::min();
    performanceStepCalls = 0;
    performanceRequestedRunFrames = 0;
    performanceExecutedRunFrames = 0;
    performanceSkippedRunFrames = 0;
    performanceSeparateRenderCalls = 0;
    performanceTotalStepDuration = PerformanceClock::duration::zero();
    performanceTotalRetroRunDuration = PerformanceClock::duration::zero();
    performanceTotalRenderDuration = PerformanceClock::duration::zero();
    performanceTotalWaitDuration = PerformanceClock::duration::zero();
    performanceMaxStepDuration = PerformanceClock::duration::zero();
}

void LibretroDroid::maybeLogPerformanceDiagnostics(
    unsigned requestedRunFrames,
    unsigned executedRunFrames,
    PerformanceClock::duration stepDuration,
    PerformanceClock::duration retroRunDuration,
    PerformanceClock::duration renderDuration,
    PerformanceClock::duration waitDuration,
    bool renderedOutsideRetroRun
) {
#if !DIAGNOSTIC_LOGGING
    (void) requestedRunFrames;
    (void) executedRunFrames;
    (void) stepDuration;
    (void) retroRunDuration;
    (void) renderDuration;
    (void) waitDuration;
    (void) renderedOutsideRetroRun;
    return;
#endif

    auto now = PerformanceClock::now();

    if (performanceWindowStart == PerformanceClock::time_point::min()) {
        performanceWindowStart = now;
    }

    performanceStepCalls++;
    performanceRequestedRunFrames += requestedRunFrames;
    performanceExecutedRunFrames += executedRunFrames;
    performanceSkippedRunFrames += requestedRunFrames > executedRunFrames
        ? (requestedRunFrames - executedRunFrames)
        : 0;
    performanceSeparateRenderCalls += renderedOutsideRetroRun ? 1 : 0;
    performanceTotalStepDuration += stepDuration;
    performanceTotalRetroRunDuration += retroRunDuration;
    performanceTotalRenderDuration += renderDuration;
    performanceTotalWaitDuration += waitDuration;
    performanceMaxStepDuration = std::max(performanceMaxStepDuration, stepDuration);

    auto elapsed = now - performanceWindowStart;
    if (elapsed < std::chrono::seconds(1)) {
        return;
    }

    auto toMilliseconds = [](PerformanceClock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    };

    double elapsedSeconds = std::chrono::duration<double>(elapsed).count();
    double drawFps = performanceStepCalls / elapsedSeconds;
    double requestedRunFps = performanceRequestedRunFrames / elapsedSeconds;
    double actualRunFps = performanceExecutedRunFrames / elapsedSeconds;
    unsigned currentFrameSpeed = frameSpeed.load();
    double targetRunFps = contentRefreshRate * currentFrameSpeed;
    double emulationSpeed = targetRunFps > 0.0 ? (actualRunFps / targetRunFps) * 100.0 : 0.0;
    double averageRetroRunMs = toMilliseconds(performanceTotalRetroRunDuration) / performanceStepCalls;
    double averageRenderMs = toMilliseconds(performanceTotalRenderDuration) / performanceStepCalls;
    double averageWaitMs = toMilliseconds(performanceTotalWaitDuration) / performanceStepCalls;
    double averageStepMs = toMilliseconds(performanceTotalStepDuration) / performanceStepCalls;
    double maxStepMs = toMilliseconds(performanceMaxStepDuration);

    __android_log_print(
        ANDROID_LOG_INFO,
        MODULE_NAME,
        "PerfDiag[step]: contentFps=%.3f screenFps=%.3f frameSpeed=%u drawFps=%.2f requestedRunFps=%.2f actualRunFps=%.2f emuSpeed=%.1f%% "
        "skippedRunFrames=%llu avgRetroRunMs=%.3f avgRenderMs=%.3f separateRenderCalls=%llu avgWaitMs=%.3f avgStepMs=%.3f maxStepMs=%.3f",
        contentRefreshRate,
        screenRefreshRate,
        currentFrameSpeed,
        drawFps,
        requestedRunFps,
        actualRunFps,
        emulationSpeed,
        static_cast<unsigned long long>(performanceSkippedRunFrames),
        averageRetroRunMs,
        averageRenderMs,
        static_cast<unsigned long long>(performanceSeparateRenderCalls),
        averageWaitMs,
        averageStepMs,
        maxStepMs
    );

    resetPerformanceDiagnostics();
}

// TODO... Do we really need this?
void LibretroDroid::resetGlobalVariables() {
    core = nullptr;
    audio = nullptr;
    video = nullptr;
    fpsSync = nullptr;
    input = nullptr;
    rumble = nullptr;
    resetPerformanceDiagnostics();
}

int LibretroDroid::availableDisks() {
    return Environment::getInstance().getRetroDiskControlCallback() != nullptr
           ? Environment::getInstance().getRetroDiskControlCallback()->get_num_images()
           : 0;
}

int LibretroDroid::currentDisk() {
    return Environment::getInstance().getRetroDiskControlCallback() != nullptr
           ? Environment::getInstance().getRetroDiskControlCallback()->get_image_index()
           : 0;
}

void LibretroDroid::changeDisk(unsigned int index) {
    if (Environment::getInstance().getRetroDiskControlCallback() == nullptr) {
        LOGE("Cannot swap disk. This platform does not support it.");
        return;
    }

    if (index < 0 || index >= Environment::getInstance().getRetroDiskControlCallback()->get_num_images()) {
        LOGE("Requested image index is not valid.");
        return;
    }

    if (Environment::getInstance().getRetroDiskControlCallback()->get_image_index() != index) {
        Environment::getInstance().getRetroDiskControlCallback()->set_eject_state(true);
        Environment::getInstance().getRetroDiskControlCallback()->set_image_index((unsigned) index);
        Environment::getInstance().getRetroDiskControlCallback()->set_eject_state(false);
    }
}

void LibretroDroid::updateVariable(const Variable& variable) {
    Environment::getInstance().updateVariable(variable.key, variable.value);
}

std::vector<Variable> LibretroDroid::getVariables() {
    return Environment::getInstance().getVariables();
}

std::vector<std::vector<struct Controller>> LibretroDroid::getControllers() {
    return Environment::getInstance().getControllers();
}

void LibretroDroid::setControllerType(unsigned int port, unsigned int type) {
    core->retro_set_controller_port_device(port, type);
}

bool LibretroDroid::unserializeState(int8_t *data, size_t size) {
    return core->retro_unserialize(data, size);
}

JNIEXPORT jboolean JNICALL LibretroDroid::unserializeSRAM(int8_t* data, size_t size) {
    size_t sramSize = core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    void *sramState = core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);

    if (sramState == nullptr) {
        LOGE("Cannot load SRAM: nullptr in retro_get_memory_data");
        return false;
    }

    if (size > sramSize) {
        LOGE("Cannot load SRAM: size mismatch");
        return false;
    }

    memcpy(sramState, data, size);

    return true;
}

std::pair<int8_t*, size_t> LibretroDroid::serializeSRAM() {
    size_t size = core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    auto* data = new int8_t[size];
    memcpy(data, (int8_t*) core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM), size);

    return std::pair(data, size);
}

void LibretroDroid::onSurfaceChanged(unsigned int width, unsigned int height) {
    LOGD("Performing libretrodroid onSurfaceChanged");
    video->updateScreenSize(width, height);
}

void LibretroDroid::onSurfaceCreated() {
    LOGD("Performing libretrodroid onSurfaceCreated");

    struct retro_system_av_info system_av_info {};
    core->retro_get_system_av_info(&system_av_info);

    video = nullptr;

    Video::RenderingOptions renderingOptions {
        Environment::getInstance().isUseHwAcceleration(),
        system_av_info.geometry.base_width,
        system_av_info.geometry.base_height,
        Environment::getInstance().isUseDepth(),
        Environment::getInstance().isUseStencil(),
        openglESVersion,
        Environment::getInstance().getPixelFormat()
    };

    auto newVideo = new Video(
        renderingOptions,
        fragmentShaderConfig,
        Environment::getInstance().isBottomLeftOrigin(),
        Environment::getInstance().getScreenRotation(),
        skipDuplicateFrames,
        immersiveModeEnabled,
        viewportRect,
        immersiveModeConfig
    );

    video = std::unique_ptr<Video>(newVideo);

    if (Environment::getInstance().getHwContextReset() != nullptr) {
        Environment::getInstance().getHwContextReset()();
    }
}

void LibretroDroid::onMotionEvent(
    unsigned int port,
    unsigned int source,
    float xAxis,
    float yAxis
) {
    LOGD("Received motion event: %d %.2f, %.2f", source, xAxis, yAxis);
    if (input) {
        input->onMotionEvent(port, source, xAxis, yAxis);
    }
}

void LibretroDroid::onTouchEvent(float xAxis, float yAxis) {
    LOGD("Received touch event: %.2f, %.2f", xAxis, yAxis);
    if (input && video) {
        auto [x, y] = video->getLayout().getRelativePosition(xAxis, yAxis);
        input->onMotionEvent(0, Input::MOTION_SOURCE_POINTER, x, y);
    }
}

void LibretroDroid::onKeyEvent(unsigned int port, int action, int keyCode) {
    LOGD("Received key event with action (%d) and keycode (%d)", action, keyCode);
    if (input) {
        input->onKeyEvent(port, action, keyCode);
    }
}

void LibretroDroid::create(
    unsigned int GLESVersion,
    const std::string& soFilePath,
    const std::string& systemDir,
    const std::string& savesDir,
    std::vector<Variable> variables,
    const ShaderManager::Config& shaderConfig,
    float refreshRate,
    bool lowLatencyAudio,
    bool enableVirtualFileSystem,
    bool enableMicrophone,
    bool duplicateFrames,
    std::optional<ImmersiveMode::Config> immersiveModeConfig,
    const std::string& language
) {
    LOGD("Performing libretrodroid create");

    resetGlobalVariables();

    Environment::getInstance().initialize(systemDir, savesDir, &callback_get_current_framebuffer);
    Environment::getInstance().setLanguage(language);
    Environment::getInstance().setEnableVirtualFileSystem(enableVirtualFileSystem);
    Environment::getInstance().setEnableMicrophone(enableMicrophone);

    openglESVersion = GLESVersion;
    screenRefreshRate = refreshRate;
    skipDuplicateFrames = duplicateFrames;
    immersiveModeEnabled = GLESVersion >= 3 && immersiveModeConfig.has_value();
    this->immersiveModeConfig = immersiveModeConfig.value_or(ImmersiveMode::Config{});
    audioEnabled.store(true);
    frameSpeed.store(1);
    useAsyncEmulation = false;
    pendingSoftwareFrameReady = false;
    pendingSoftwareFrameData.clear();

    core = std::make_unique<Core>(soFilePath);

    core->retro_set_video_refresh(&callback_hw_video_refresh);
    core->retro_set_environment(&Environment::callback_environment);
    core->retro_set_audio_sample(&callback_audio_sample);
    core->retro_set_audio_sample_batch(&callback_set_audio_sample_batch);
    core->retro_set_input_poll(&callback_retro_set_input_poll);
    core->retro_set_input_state(&callback_set_input_state);

    std::for_each(variables.begin(), variables.end(), [&](const Variable& v) {
        updateVariable(v);
    });

    core->retro_init();

    preferLowLatencyAudio = lowLatencyAudio;

    // HW accelerated cores are only supported on opengles 3.
    if (Environment::getInstance().isUseHwAcceleration() && openglESVersion < 3) {
        throw LibretroDroidError("OpenGL ES 3 is required for this Core", ERROR_GL_NOT_COMPATIBLE);
    }

    fragmentShaderConfig = shaderConfig;

    rumble = std::make_unique<Rumble>();
}

void LibretroDroid::loadGameFromPath(const std::string& gamePath) {
    LOGD("Performing libretrodroid loadGameFromPath");
    struct retro_system_info system_info {};
    core->retro_get_system_info(&system_info);

    struct retro_game_info game_info {};
    game_info.path = Utils::cloneToCString(gamePath);
    game_info.meta = nullptr;

    if (system_info.need_fullpath) {
        game_info.data = nullptr;
        game_info.size = 0;
    } else {
        struct Utils::ReadResult file = Utils::readFileAsBytes(gamePath);
        game_info.data = file.data;
        game_info.size = file.size;
    }

    bool result = core->retro_load_game(&game_info);
    if (!result) {
        LOGE("Cannot load game. Leaving.");
        throw std::runtime_error("Cannot load game");
    }

    afterGameLoad();
}

void LibretroDroid::loadGameFromBytes(const int8_t *data, size_t size) {
    LOGD("Performing libretrodroid loadGameFromBytes");

    struct retro_system_info system_info {};
    core->retro_get_system_info(&system_info);

    struct retro_game_info game_info {};
    game_info.path = nullptr;
    game_info.meta = nullptr;

    if (system_info.need_fullpath) {
        game_info.data = nullptr;
        game_info.size = 0;
    } else {
        game_info.data = data;
        game_info.size = size;
    }

    bool result = core->retro_load_game(&game_info);
    if (!result) {
        LOGE("Cannot load game. Leaving.");
        throw std::runtime_error("Cannot load game");
    }

    afterGameLoad();
}

void LibretroDroid::loadGameFromVirtualFiles(std::vector<VFSFile> virtualFiles) {
    LOGD("Performing libretrodroid loadGameFromVirtualFiles");
    struct retro_system_info system_info {};
    core->retro_get_system_info(&system_info);

    if (virtualFiles.empty()) {
        LOGE("Calling loadGameFromVirtualFiles without any file.");
        throw std::runtime_error("Calling loadGameFromVirtualFiles without any file.");
    }

    std::string firstFilePath = virtualFiles[0].getFileName();
    int firstFileFD = virtualFiles[0].getFD();

    bool loadUsingVFS = system_info.need_fullpath || virtualFiles.size() > 1;

    struct retro_game_info game_info {};
    game_info.path = Utils::cloneToCString(firstFilePath);
    game_info.meta = nullptr;

    if (loadUsingVFS) {
        VFS::getInstance().initialize(std::move(virtualFiles));
    }

    if (loadUsingVFS) {
        game_info.data = nullptr;
        game_info.size = 0;
    } else {
        struct Utils::ReadResult file = Utils::readFileAsBytes(firstFileFD);
        game_info.data = file.data;
        game_info.size = file.size;
    }

    bool result = core->retro_load_game(&game_info);
    if (!result) {
        LOGE("Cannot load game. Leaving.");
        throw std::runtime_error("Cannot load game");
    }

    afterGameLoad();
}

void LibretroDroid::destroy() {
    LOGD("Performing libretrodroid destroy");

    stopEmulationThread();

    if (Environment::getInstance().getHwContextDestroy() != nullptr) {
        Environment::getInstance().getHwContextDestroy()();
    }

    core->retro_unload_game();
    core->retro_deinit();

    video = nullptr;
    core = nullptr;
    rumble = nullptr;
    fpsSync = nullptr;
    audio = nullptr;

    Environment::getInstance().deinitialize();
    VFS::getInstance().deinitialize();
}

void LibretroDroid::resume() {
    LOGD("Performing libretrodroid resume");

    input = std::make_unique<Input>();

    fpsSync->reset();
    resetPerformanceDiagnostics();
    startEmulationThreadIfNeeded();
    audio->start();
    refreshAspectRatio();
}

void LibretroDroid::pause() {
    LOGD("Performing libretrodroid pause");
    {
        std::lock_guard<std::mutex> lock(emulationStateMutex);
        emulationThreadPaused = true;
    }
    audio->stop();

    input = nullptr;
}

void LibretroDroid::step() {
    LOGD("Stepping into retro_run()");

    if (useAsyncEmulation) {
        auto stepStart = PerformanceClock::now();
        auto renderDuration = PerformanceClock::duration::zero();

        uploadPendingSoftwareFrame();

        if (video) {
            auto renderStart = PerformanceClock::now();
            video->renderFrame(true);
            renderDuration = PerformanceClock::now() - renderStart;
        }

        {
            std::lock_guard<std::mutex> lock(coreMutex);
            if (rumble && rumbleEnabled) {
                rumble->fetchFromEnvironment();
            }

            if (video && Environment::getInstance().isGameGeometryUpdated()) {
                Environment::getInstance().clearGameGeometryUpdated();

                video->updateRendererSize(
                    Environment::getInstance().getGameGeometryWidth(),
                    Environment::getInstance().getGameGeometryHeight()
                );

                dirtyVideo = true;
            }

            if (video && Environment::getInstance().isScreenRotationUpdated()) {
                Environment::getInstance().clearScreenRotationUpdated();
                video->updateRotation(Environment::getInstance().getScreenRotation());
            }
        }

        auto stepDuration = PerformanceClock::now() - stepStart;
        maybeLogPerformanceDiagnostics(
            0,
            0,
            stepDuration,
            PerformanceClock::duration::zero(),
            renderDuration,
            PerformanceClock::duration::zero(),
            true
        );
        return;
    }

    auto stepStart = PerformanceClock::now();

    unsigned frames = 1;
    unsigned requestedFrames = 1;
    if (fpsSync) {
        requestedFrames = fpsSync->advanceFrames();

        // 允许追赶最多 2 帧，避免因抖动导致模拟器永远无法追赶
        frames = std::min(requestedFrames, 2u);
    }

    auto retroRunDuration = PerformanceClock::duration::zero();
    if (frames > 0) {
        auto retroRunStart = PerformanceClock::now();
        unsigned currentFrameSpeed = frameSpeed.load();
        for (size_t i = 0; i < frames * currentFrameSpeed; i++) {
            core->retro_run();
        }
        retroRunDuration = PerformanceClock::now() - retroRunStart;
    }

    auto renderDuration = PerformanceClock::duration::zero();
    bool renderedOutsideRetroRun = false;
    if (video && (!video->rendersInVideoCallback() || frames == 0)) {
        renderedOutsideRetroRun = true;
        auto renderStart = PerformanceClock::now();
        video->renderFrame(frames == 0);
        renderDuration = PerformanceClock::now() - renderStart;
    }

    auto waitDuration = PerformanceClock::duration::zero();
    if (fpsSync) {
        auto waitStart = PerformanceClock::now();
        fpsSync->wait();
        waitDuration = PerformanceClock::now() - waitStart;
    }

    if (rumble && rumbleEnabled) {
        rumble->fetchFromEnvironment();
    }

    // Some games override the core geometry at runtime. These fields get updated in retro_run().
    if (video && Environment::getInstance().isGameGeometryUpdated()) {
        Environment::getInstance().clearGameGeometryUpdated();

        video->updateRendererSize(
            Environment::getInstance().getGameGeometryWidth(),
            Environment::getInstance().getGameGeometryHeight()
        );

        dirtyVideo = true;
    }

    if (video && Environment::getInstance().isScreenRotationUpdated()) {
        Environment::getInstance().clearScreenRotationUpdated();

        video->updateRotation(Environment::getInstance().getScreenRotation());
    }

    auto stepDuration = PerformanceClock::now() - stepStart;
    unsigned currentFrameSpeed = frameSpeed.load();
    maybeLogPerformanceDiagnostics(
        requestedFrames * currentFrameSpeed,
        frames * currentFrameSpeed,
        stepDuration,
        retroRunDuration,
        renderDuration,
        waitDuration,
        renderedOutsideRetroRun
    );
}

float LibretroDroid::getAspectRatio() {
    float gameAspectRatio = Environment::getInstance().retrieveGameSpecificAspectRatio();
    return gameAspectRatio > 0 ? gameAspectRatio : defaultAspectRatio;
}

void LibretroDroid::refreshAspectRatio() {
    video->updateAspectRatio(getAspectRatio());
}

std::array<float, 4> LibretroDroid::getContentBounds() {
    if (video) {
        return video->getLayout().getRelativeForegroundBounds();
    }
    return {0.0f, 0.0f, 1.0f, 1.0f};
}

void LibretroDroid::setRumbleEnabled(bool enabled) {
    rumbleEnabled = enabled;
}

bool LibretroDroid::isRumbleEnabled() const {
    return rumbleEnabled;
}

void LibretroDroid::setFrameSpeed(unsigned int speed) {
    frameSpeed.store(speed);
    updateAudioSampleRateMultiplier();
}

void LibretroDroid::setAudioEnabled(bool enabled) {
    audioEnabled.store(enabled);
}

void LibretroDroid::setAudioSyncEnabled(bool enabled) {
    audioSyncEnabled = enabled;
    if (audio) {
        audio->setAudioSyncEnabled(enabled);
    }
    LOGI("LibretroDroid: AudioSync %s", enabled ? "enabled" : "disabled");
}

void LibretroDroid::setShaderConfig(ShaderManager::Config shaderConfig) {
    fragmentShaderConfig = std::move(shaderConfig);
    if (video) {
        video->updateShaderType(fragmentShaderConfig);
    }
}

void LibretroDroid::handleVideoRefresh(
    const void *data,
    unsigned int width,
    unsigned int height,
    size_t pitch
) {
    if (useAsyncEmulation) {
        if (data != nullptr) {
            size_t frameSize = pitch * height;
            std::lock_guard<std::mutex> lock(pendingSoftwareFrameMutex);
            pendingSoftwareFrameData.resize(frameSize);
            memcpy(pendingSoftwareFrameData.data(), data, frameSize);
            pendingSoftwareFrameWidth = width;
            pendingSoftwareFrameHeight = height;
            pendingSoftwareFramePitch = pitch;
            pendingSoftwareFrameReady = true;
        }
        return;
    }

    if (video) {
        video->onNewFrame(data, width, height, pitch);

        if (video->rendersInVideoCallback()) {
            video->renderFrame();
        }
    }
}

size_t LibretroDroid::handleAudioCallback(const int16_t *data, size_t frames) {
    if (audio && audioEnabled.load()) {
        audio->write(data, frames);
    }
    return frames;
}

int16_t LibretroDroid::handleSetInputState(
    unsigned int port,
    unsigned int device,
    unsigned int index,
    unsigned int id
) {
    if (input) {
        return input->getInputState(port, device, index, id);
    }
    return 0;
}

uintptr_t LibretroDroid::handleGetCurrentFrameBuffer() {
    if (video) {
        return video->getCurrentFramebuffer();
    }
    return 0;
}

void LibretroDroid::reset() {
    core->retro_reset();
}

std::pair<int8_t*, size_t> LibretroDroid::serializeState() {
    size_t size = core->retro_serialize_size();
    auto data = new int8_t[size];

    core->retro_serialize(data, size);

    return std::pair(data, size);
}

void LibretroDroid::resetCheat() {
    core->retro_cheat_reset();
}

void LibretroDroid::setCheat(unsigned index, bool enabled, const std::string& code) {
    core->retro_cheat_set(index, enabled, Utils::cloneToCString(code));
}

bool LibretroDroid::requiresVideoRefresh() const {
    return dirtyVideo;
}

void LibretroDroid::clearRequiresVideoRefresh() {
    dirtyVideo = false;
}

void LibretroDroid::afterGameLoad() {
    struct retro_system_av_info system_av_info {};
    core->retro_get_system_av_info(&system_av_info);

    contentRefreshRate = system_av_info.timing.fps;
    fpsSync = std::make_unique<FPSSync>(system_av_info.timing.fps, screenRefreshRate);
    useAsyncEmulation = shouldUseAsyncEmulation();
    resetPerformanceDiagnostics();

    double inputSampleRate = system_av_info.timing.sample_rate;

#if DIAGNOSTIC_LOGGING
    __android_log_print(
        ANDROID_LOG_INFO,
        MODULE_NAME,
        "AfterGameLoad: contentFps=%.6f screenRefresh=%.6f contentSampleRate=%.2f inputSampleRate=%.2f preferLowLatencyAudio=%d audioSyncEnabled=%d",
        system_av_info.timing.fps,
        screenRefreshRate,
        system_av_info.timing.sample_rate,
        inputSampleRate,
        preferLowLatencyAudio,
        audioSyncEnabled
    );
#endif

    audio = std::make_unique<Audio>(
        (int32_t) std::lround(inputSampleRate),
        system_av_info.timing.fps,
        preferLowLatencyAudio
    );

    // 应用 AudioSync 配置
    audio->setAudioSyncEnabled(audioSyncEnabled);

    updateAudioSampleRateMultiplier();

    defaultAspectRatio = findDefaultAspectRatio(system_av_info);
}

bool LibretroDroid::shouldUseAsyncEmulation() const {
    return fpsSync != nullptr &&
        fpsSync->usesVSync() &&
        !Environment::getInstance().isUseHwAcceleration() &&
        (screenRefreshRate - contentRefreshRate) > 1.0f;
}

void LibretroDroid::startEmulationThreadIfNeeded() {
    if (!useAsyncEmulation) {
        return;
    }

    {
        std::lock_guard<std::mutex> frameLock(pendingSoftwareFrameMutex);
        pendingSoftwareFrameReady = false;
    }

    if (!emulationThread.joinable()) {
        {
            std::lock_guard<std::mutex> lock(emulationStateMutex);
            emulationThreadShouldStop = false;
            emulationThreadPaused = false;
        }
        emulationThread = std::thread(&LibretroDroid::emulationThreadLoop, this);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(emulationStateMutex);
        emulationThreadPaused = false;
    }
    emulationStateCondition.notify_all();
}

void LibretroDroid::stopEmulationThread() {
    if (!emulationThread.joinable()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(emulationStateMutex);
        emulationThreadShouldStop = true;
        emulationThreadPaused = false;
    }
    emulationStateCondition.notify_all();
    emulationThread.join();

    {
        std::lock_guard<std::mutex> lock(emulationStateMutex);
        emulationThreadShouldStop = false;
        emulationThreadPaused = true;
    }
}

void LibretroDroid::emulationThreadLoop() {
    using EmulationClock = std::chrono::steady_clock;

    while (true) {
        {
            std::unique_lock<std::mutex> lock(emulationStateMutex);
            emulationStateCondition.wait(lock, [this] {
                return emulationThreadShouldStop || !emulationThreadPaused;
            });
            if (emulationThreadShouldStop) {
                return;
            }
        }

        auto nextTick = EmulationClock::now();

        while (true) {
            {
                std::lock_guard<std::mutex> lock(emulationStateMutex);
                if (emulationThreadShouldStop) {
                    return;
                }
                if (emulationThreadPaused) {
                    break;
                }
            }

            unsigned currentFrameSpeed = std::max(frameSpeed.load(), 1u);
            auto tickInterval = std::chrono::duration_cast<EmulationClock::duration>(
                std::chrono::duration<double>(1.0 / (contentRefreshRate * currentFrameSpeed))
            );

            auto now = EmulationClock::now();
            if (nextTick > now) {
                std::this_thread::sleep_until(nextTick);
                now = EmulationClock::now();
            }

            auto framesDue =
                std::max<int64_t>(
                    1,
                    ((now - nextTick) / tickInterval) + 1
                );
            auto maxFramesPerLoop = std::max<uint64_t>(3, 3ull * currentFrameSpeed);
            auto framesToRun = static_cast<unsigned>(std::min<uint64_t>(framesDue, maxFramesPerLoop));

            {
                std::lock_guard<std::mutex> coreLock(coreMutex);
                if (core) {
                    for (unsigned i = 0; i < framesToRun; i++) {
                        core->retro_run();
                    }
                }
            }

            nextTick += tickInterval * framesDue;
        }
    }
}

void LibretroDroid::uploadPendingSoftwareFrame() {
    if (!video) {
        return;
    }

    std::vector<uint8_t> frameData;
    unsigned width = 0;
    unsigned height = 0;
    size_t pitch = 0;

    {
        std::lock_guard<std::mutex> lock(pendingSoftwareFrameMutex);
        if (!pendingSoftwareFrameReady) {
            return;
        }
        frameData.swap(pendingSoftwareFrameData);
        width = pendingSoftwareFrameWidth;
        height = pendingSoftwareFrameHeight;
        pitch = pendingSoftwareFramePitch;
        pendingSoftwareFrameReady = false;
    }

    if (!frameData.empty()) {
        video->onNewFrame(frameData.data(), width, height, pitch);
    }
}

float LibretroDroid::findDefaultAspectRatio(const retro_system_av_info& system_av_info) {
    float result = system_av_info.geometry.aspect_ratio;
    if (result < 0) {
        result =
            (float) system_av_info.geometry.base_width / (float) system_av_info.geometry.base_height;
    }
    return result;
}

void LibretroDroid::handleRumbleUpdates(const std::function<void(int, float, float)> &handler) {
    if (rumble && rumbleEnabled) {
        rumble->handleRumbleUpdates(handler);
    }
}

void LibretroDroid::setViewport(Rect viewportRect) {
    this->viewportRect = viewportRect;

    if (video != nullptr) {
        video->updateViewportSize(viewportRect);
    }
}

// 获取当前FPS(从libretro core)
float LibretroDroid::getCurrentFPS() {
    if (core) {
        struct retro_system_av_info system_av_info {};
        core->retro_get_system_av_info(&system_av_info);
        return static_cast<float>(system_av_info.timing.fps);
    }
    return 0.0f;
}

// 获取内容刷新率(从libretro core)
float LibretroDroid::getContentRefreshRate() {
    if (core) {
        struct retro_system_av_info system_av_info {};
        core->retro_get_system_av_info(&system_av_info);
        return static_cast<float>(system_av_info.timing.fps);
    }
    return 0.0f;
}

} //namespace libretrodroid
