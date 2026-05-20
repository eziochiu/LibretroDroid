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
    if (useVSync) return 1;

    if (firstFrame) {
        start();
        return 1;
    }

    frameAccumulator += framesPerScreenRefresh;

    if (frameAccumulator >= 1.0) {
        frameAccumulator -= 1.0;
        return 1;
    }

    return 0;
}

FPSSync::FPSSync(double contentRefreshRate, double screenRefreshRate) {
    this->contentRefreshRate = contentRefreshRate;
    this->screenRefreshRate = screenRefreshRate;
    double speedAdjustment = std::abs(screenRefreshRate / contentRefreshRate - 1.0);
    this->useVSync = speedAdjustment <= MAX_VSYNC_SPEED_ADJUSTMENT;
    this->framesPerScreenRefresh = contentRefreshRate / screenRefreshRate;
    reset();
}

void FPSSync::start() {
    LOGI("Starting game with fps %f on a screen with refresh rate %f. Using vsync: %d", contentRefreshRate, screenRefreshRate, useVSync);
    firstFrame = false;
}

void FPSSync::reset() {
    firstFrame = true;
    frameAccumulator = 0.0;
}

double FPSSync::getTimeStretchFactor() {
    return useVSync ? contentRefreshRate / screenRefreshRate : 1.0;
}

void FPSSync::wait() {
    // The GL thread is already paced by EGL buffer swaps. Sleeping here can miss
    // the next display refresh and cause visible judder on mismatched rates.
}

} //namespace libretrodroid
