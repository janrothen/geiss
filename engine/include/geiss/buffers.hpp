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

#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

namespace geiss {

// Alignment and size granule of the engine's frame buffers: the 16 KB page of Apple Silicon.
// A host can then hand the same memory to a GPU without copying it (Metal's
// newBufferWithBytesNoCopy wants page-aligned memory and a length in whole pages). Elsewhere
// it only costs a little padding.
constexpr std::size_t kBufferPage = 16384;

// `bytes` rounded up to whole pages.
constexpr std::size_t pageRounded(std::size_t bytes) { return (bytes + kBufferPage - 1) / kBufferPage * kBufferPage; }

// Allocates page-aligned memory in whole pages, so a vector's data can be shared with a GPU.
template <typename T>
struct PageAllocator {
    using value_type = T;

    PageAllocator() = default;
    template <typename U>
    explicit PageAllocator(const PageAllocator<U>& /*other*/) {}

    T* allocate(std::size_t count) {
        void* memory = std::aligned_alloc(kBufferPage, pageRounded(count * sizeof(T)));
        if (memory == nullptr) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(memory);
    }
    void deallocate(T* pointer, std::size_t /*count*/) { std::free(pointer); }

    template <typename U>
    bool operator==(const PageAllocator<U>& /*other*/) const {
        return true;
    }
};

template <typename T>
using PageVector = std::vector<T, PageAllocator<T>>;

}  // namespace geiss
