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

#include <cmath>
#include "fpssync.h"
#include "log.h"

namespace libretrodroid {

unsigned FPSSync::advanceFrames() {
    if (lastFrame == MIN_TIME) {
        start();
        lastFrame = lastFrame + sampleInterval;
        return 1;
    }

    auto now = std::chrono::steady_clock::now();

    if (useVSync) {
        // VSync 模式：允许 1/4 间隔的容差，避免因微小抖动导致跳帧
        auto tolerance = sampleInterval / 4;
        if (now + tolerance < lastFrame) {
            return 0;
        }
        // 仍以内容时钟为基准推进，但限制最大追赶帧数为 2，
        // 防止系统卡顿后一次性追赶过多帧
        auto frames = ((now - lastFrame) / sampleInterval) + 1;
        if (frames > 2) frames = 2;
        lastFrame = lastFrame + sampleInterval * frames;
        return frames;
    }

    auto frames = ((now - lastFrame) / sampleInterval) + 1;
    lastFrame = lastFrame + sampleInterval * frames;

    return frames;
}

FPSSync::FPSSync(double contentRefreshRate, double screenRefreshRate) {
    this->contentRefreshRate = contentRefreshRate;
    this->screenRefreshRate = screenRefreshRate;
    // 恢复智能 VSync 检测：仅当屏幕刷新率与内容帧率接近时启用 VSync
    this->useVSync = std::abs(contentRefreshRate - screenRefreshRate) < FPS_TOLERANCE;
    this->sampleInterval = std::chrono::microseconds((long) ((1000000L / contentRefreshRate)));
    reset();
}

void FPSSync::start() {
    LOGI("Starting game with fps %f on a screen with refresh rate %f. Using vsync: %d", contentRefreshRate, screenRefreshRate, useVSync);
    lastFrame = std::chrono::steady_clock::now();
}

void FPSSync::reset() {
    lastFrame = MIN_TIME;
}

double FPSSync::getTimeStretchFactor() {
    if (useVSync) {
        // VSync 模式下，音频需要匹配屏幕刷新率与内容帧率的比例
        return screenRefreshRate / contentRefreshRate;
    }
    return 1.0;
}

void FPSSync::wait() {
    if (useVSync) return;
    std::this_thread::sleep_until(lastFrame);
}

} //namespace libretrodroid
