#include <cmath>
#include <cstdio>

import owe.audio_response;
import rstd;

using namespace rstd::prelude;

namespace
{

owe::audio::PcmWindow tones(float scale) {
    owe::audio::PcmWindow window {};
    window.generation     = 1;
    window.sequence       = 1;
    window.sample_rate_hz = owe::audio::kSampleRate;
    window.channels       = owe::audio::kChannels;
    window.frames         = static_cast<rstd::uint32_t>(owe::audio::kWindowFrames);
    const rstd::array<float, 7> frequencies {
        200.0f, 400.0f, 800.0f, 1600.0f, 3200.0f, 6400.0f, 12800.0f,
    };
    for (rstd::size_t frame = 0; frame < owe::audio::kWindowFrames; ++frame) {
        float       sample = 0.0f;
        const float time = static_cast<float>(frame) / static_cast<float>(owe::audio::kSampleRate);
        for (float frequency : frequencies) {
            sample += scale * std::sin(2.0f * f32::consts::PI.to_primitive() * frequency * time);
        }
        window.samples[usize(frame * 2)]     = sample;
        window.samples[usize(frame * 2 + 1)] = sample;
    }
    return window;
}

owe::audio::PcmWindow aligned_tones(bool include_second) {
    owe::audio::PcmWindow window {};
    window.generation     = 1;
    window.sequence       = 1;
    window.sample_rate_hz = owe::audio::kSampleRate;
    window.channels       = owe::audio::kChannels;
    window.frames         = static_cast<rstd::uint32_t>(owe::audio::kWindowFrames);
    for (rstd::size_t frame = 0; frame < owe::audio::kWindowFrames; ++frame) {
        const float time = static_cast<float>(frame) / static_cast<float>(owe::audio::kSampleRate);
        float sample = 0.05f * std::sin(2.0f * f32::consts::PI.to_primitive() * 3093.75f * time);
        if (include_second) {
            sample += 0.05f * std::sin(2.0f * f32::consts::PI.to_primitive() * 3164.0625f * time);
        }
        window.samples[usize(frame * 2)]     = sample;
        window.samples[usize(frame * 2 + 1)] = sample;
    }
    return window;
}

owe::audio::PcmWindow impulse() {
    owe::audio::PcmWindow window {};
    window.generation     = 1;
    window.sequence       = 1;
    window.sample_rate_hz = owe::audio::kSampleRate;
    window.channels       = owe::audio::kChannels;
    window.frames         = static_cast<rstd::uint32_t>(owe::audio::kWindowFrames);
    window.samples[usize((owe::audio::kWindowFrames - 1) * owe::audio::kChannels)] = 1.0f;
    return window;
}

rstd::size_t peak_near(const rstd::array<float, 64>& values, rstd::size_t center) {
    auto peak = center - 1;
    for (auto index = center; index <= center + 1; ++index) {
        if (values[usize(index)] > values[usize(peak)]) peak = index;
    }
    return peak;
}

rstd::size_t peak(const rstd::array<float, 64>& values) {
    rstd::size_t result = 0;
    for (rstd::size_t index = 1; index < 64; ++index) {
        if (values[usize(index)] > values[usize(result)]) result = index;
    }
    return result;
}

} // namespace

int main() {
    owe::audio::ResponseEngine engine;
    owe::audio::ResponseFrame  first {};
    auto                       input = tones(0.02f);
    if (! engine.analyze(input, first)) return 1;
    const rstd::array<rstd::size_t, 7> expected {
        rstd::size_t(8),  rstd::size_t(16), rstd::size_t(30), rstd::size_t(36),
        rstd::size_t(43), rstd::size_t(51), rstd::size_t(61),
    };
    for (auto band : expected) {
        const auto actual = peak_near(first.left, band);
        if (actual != band) {
            std::fprintf(stderr, "expected peak %zu, got %zu\n", band, actual);
            return 2;
        }
    }
    if (engine.analyze(input, first)) return 3;

    input.sequence         = 2;
    input.end_sample_frame = owe::audio::kWindowFrames;
    for (auto& sample : input.samples) sample = sample * 2.0f;
    owe::audio::ResponseFrame second {};
    if (! engine.analyze(input, second)) return 4;
    for (auto band : expected) {
        const float ratio = second.left[usize(band)] / first.left[usize(band)];
        if (std::abs(ratio - 2.0f) > 1.0e-4f) return 5;
    }
    if (second.left[usize(61)] <= 1.0f) return 6;

    engine.end();
    input.sequence = 1;
    if (! engine.analyze(input, first)) return 7;

    owe::audio::PcmWindow stereo {};
    stereo.generation     = 2;
    stereo.sequence       = 1;
    stereo.sample_rate_hz = owe::audio::kSampleRate;
    stereo.channels       = owe::audio::kChannels;
    stereo.frames         = static_cast<rstd::uint32_t>(owe::audio::kWindowFrames);
    for (rstd::size_t frame = 0; frame < owe::audio::kWindowFrames; ++frame) {
        const float time = static_cast<float>(frame) / static_cast<float>(owe::audio::kSampleRate);
        stereo.samples[usize(frame * 2)] =
            0.45f * std::sin(2.0f * f32::consts::PI.to_primitive() * 110.0f * time);
        stereo.samples[usize(frame * 2 + 1)] =
            0.45f * std::sin(2.0f * f32::consts::PI.to_primitive() * 1760.0f * time);
    }
    owe::audio::ResponseFrame split {};
    if (! engine.analyze(stereo, split)) return 8;
    if (peak(split.left) != 4 || peak(split.right) != 37) return 9;
    if (split.left[usize(4)] <= split.right[usize(4)] ||
        split.right[usize(37)] <= split.left[usize(37)])
        return 10;
    if (std::abs(split.left[usize(4)] - 1.05f) > 0.2f ||
        std::abs(split.right[usize(37)] - 4.3f) > 0.5f) {
        std::fprintf(
            stderr, "split response: %.6f %.6f\n", split.left[usize(4)], split.right[usize(37)]);
        return 11;
    }

    owe::audio::ResponseEngine aligned_engine;
    auto                       aligned_input = aligned_tones(false);
    owe::audio::ResponseFrame  single {};
    if (! aligned_engine.analyze(aligned_input, single)) return 12;
    aligned_input          = aligned_tones(true);
    aligned_input.sequence = 2;
    owe::audio::ResponseFrame dual {};
    if (! aligned_engine.analyze(aligned_input, dual)) return 13;
    const float peak_ratio = dual.left[usize(43)] / single.left[usize(43)];
    if (std::abs(peak_ratio - 1.160275f) > 2.0e-4f) {
        std::fprintf(stderr, "dual tone peak ratio: %.9f\n", peak_ratio);
        return 14;
    }

    auto expired = aligned_tones(false);
    for (rstd::size_t frame = owe::audio::kWindowFrames / 2; frame < owe::audio::kWindowFrames;
         ++frame) {
        expired.samples[usize(frame * 2)]     = 0.0f;
        expired.samples[usize(frame * 2 + 1)] = 0.0f;
    }
    owe::audio::ResponseEngine expired_engine;
    owe::audio::ResponseFrame  expired_response {};
    if (! expired_engine.analyze(expired, expired_response)) return 15;
    for (float value : expired_response.left) {
        if (value != 0.0f) return 16;
    }
    owe::audio::ResponseEngine impulse_engine;
    owe::audio::ResponseFrame  impulse_response {};
    auto                       impulse_input = impulse();
    if (! impulse_engine.analyze(impulse_input, impulse_response)) return 17;
    if (std::abs(impulse_response.left[usize(0)] - 0.003480064f) > 1.0e-5f ||
        std::abs(impulse_response.left[usize(29)] - 0.006702113f) > 1.0e-5f ||
        std::abs(impulse_response.left[usize(63)] - 0.077817120f) > 1.0e-5f) {
        std::fprintf(stderr,
                     "impulse response: %.9f %.9f %.9f\n",
                     impulse_response.left[usize(0)],
                     impulse_response.left[usize(29)],
                     impulse_response.left[usize(63)]);
        return 18;
    }
    for (float value : impulse_response.right) {
        if (value != 0.0f) return 19;
    }

    return 0;
}
