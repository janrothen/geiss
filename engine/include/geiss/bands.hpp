// This file is part of the Geiss macOS port.
//                           ___ ___ ___ ___ ___
//                          / __| __|_ _/ __/ __|
//                         | (_ | _| | |\__ \__ \
//                          \___|___|___|___/___/
// 3-Clause BSD License
//
// Copyright (c) 1998-2022 Ryan Geiss (@geissomatik)
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#pragma once

namespace geiss {

// Splits a frame's rows into bands that may run at the same time. The host supplies `run`,
// which must call `band(job, i)` once for each `i` in [0, count) and return only when every
// call has finished. Without a host, the bands run in order on the calling thread, so the
// engine needs no threads of its own. Every band writes its own rows, so the result is the
// same however they are scheduled.
struct BandRunner {
    using Band = void (*)(void* job, int index);
    using Run = void (*)(void* host, int count, void* job, Band band);

    Run run = nullptr;
    void* host = nullptr;

    void operator()(int count, void* job, Band band) const {
        if (run != nullptr) {
            run(host, count, job, band);
            return;
        }
        for (int index = 0; index < count; ++index) {
            band(job, index);
        }
    }
};

// How many bands a frame of `height` rows is split into: enough to fill the performance
// and efficiency cores of an Apple Silicon Mac, and never more than the rows.
constexpr int bandCount(int height) {
    constexpr int kBands = 16;
    return height < kBands ? (height > 0 ? height : 1) : kBands;
}

// The rows [first, last) of band `index` out of `count`.
constexpr int bandFirstRow(int index, int count, int height) { return index * height / count; }

}  // namespace geiss
