// StereoRing.hpp - stereo history ring addressed by an absolute sample counter.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
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

private:
    int64_t cap_ = 1;
    std::vector<float> l_, r_;
};

} // namespace slowops::looper
