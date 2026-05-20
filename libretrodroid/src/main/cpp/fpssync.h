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

#ifndef LIBRETRODROID_FPSSYNC_H
#define LIBRETRODROID_FPSSYNC_H

namespace libretrodroid {

class FPSSync {
public:
    FPSSync(double contentRefreshRate, double screenRefreshRate);
    ~FPSSync() { }

    void reset();
    unsigned advanceFrames();
    void wait();
    double getTimeStretchFactor();
private:

    double screenRefreshRate;
    double contentRefreshRate;
    bool useVSync;
    const double MAX_VSYNC_SPEED_ADJUSTMENT = 0.005;

    void start();

    bool firstFrame = true;
    double frameAccumulator = 0.0;
    double framesPerScreenRefresh = 1.0;
};

}


#endif //LIBRETRODROID_FPSSYNC_H
