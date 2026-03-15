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

#include <algorithm>
#include <cmath>
#include "fpssync.h"
#include "log.h"

namespace libretrodroid {

unsigned FPSSync::advanceFrames() {
    if (timingMode == TimingMode::DisplayVsync) return 1;

    if (lastFrame == MIN_TIME) {
        start();
        return 1;
    }

    auto now = std::chrono::steady_clock::now();
    auto frames = std::clamp<long long>((now - lastFrame) / sampleInterval, 1, 2);
    lastFrame = lastFrame + sampleInterval * frames;

    return frames;
}

FPSSync::FPSSync(
    double contentRefreshRate,
    double screenRefreshRate,
    TimingMode timingMode,
    bool externallyPaced
) {
    this->contentRefreshRate = contentRefreshRate;
    this->screenRefreshRate = screenRefreshRate;
    this->timingMode = timingMode;
    this->externallyPaced = externallyPaced;
    this->sampleInterval = std::chrono::microseconds((long) ((1000000L / contentRefreshRate)));
    reset();
}

void FPSSync::start() {
    LOGI(
        "Starting game with fps %f on a screen with refresh rate %f. Timing mode: %d. Externally paced: %d",
        contentRefreshRate,
        screenRefreshRate,
        timingMode == TimingMode::DisplayVsync ? 0 : 1,
        externallyPaced
    );
    lastFrame = std::chrono::steady_clock::now();
}

void FPSSync::reset() {
    lastFrame = MIN_TIME;
}

double FPSSync::getTimeStretchFactor() {
    return timingMode == TimingMode::DisplayVsync ? contentRefreshRate / screenRefreshRate : 1.0;
}

void FPSSync::wait() {
    if (timingMode == TimingMode::DisplayVsync || externallyPaced) return;
    std::this_thread::sleep_until(lastFrame);
}

bool FPSSync::usesContentClock() const {
    return timingMode == TimingMode::ContentClock;
}

bool FPSSync::shouldUseDisplayVsync(double contentRefreshRate, double screenRefreshRate) {
    double relativeDifference = std::abs(contentRefreshRate - screenRefreshRate) /
        std::max(contentRefreshRate, screenRefreshRate);
    return relativeDifference < DISPLAY_VSYNC_RELATIVE_TOLERANCE;
}

} //namespace libretrodroid
