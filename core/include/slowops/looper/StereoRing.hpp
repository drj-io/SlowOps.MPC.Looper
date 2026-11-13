// StereoRing.hpp - stereo history ring addressed by an absolute sample counter.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace slowops::looper {

class StereoRing
{
public:
    // Allocates. Call from prepare() only.
    void allocate(int64_t frames)
    {
        cap_ = frames < 1 ? 1 : frames;
        l_.assign((size_t)cap_, 0.0f);
        r_.assign((size_t)cap_, 0.0f);
    }
    int64_t capacity() const { return cap_; }

    void write(int64_t t, float l, float r)
    {
        const size_t i = (size_t)(t % cap_);
        l_[i] = l; r_[i] = r;
    }
    float left(int64_t t) const  { return l_[(size_t)(t % cap_)]; }
    float right(int64_t t) const { return r_[(size_t)(t % cap_)]; }

    // Copy frames [t, t + n) into dst (at most two contiguous segments).
    void copyOut(int64_t t, int64_t n, float* dstL, float* dstR) const
    {
        while (n > 0)
        {
            const int64_t i = t % cap_;
            const int64_t seg = (cap_ - i) < n ? (cap_ - i) : n;
            std::memcpy(dstL, l_.data() + i, sizeof(float) * (size_t)seg);
            std::memcpy(dstR, r_.data() + i, sizeof(float) * (size_t)seg);
            dstL += seg; dstR += seg; t += seg; n -= seg;
        }
    }

private:
    int64_t cap_ = 1;
    std::vector<float> l_, r_;
};

} // namespace slowops::looper
