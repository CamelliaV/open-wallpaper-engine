module;

#include <cmath>
#include <complex>

module owe.audio_response;

import rstd;

namespace owe::audio
{

using namespace rstd::prelude;

namespace
{

constexpr rstd::size_t kFftFrames         = 2089;
constexpr rstd::size_t kAnalysisFrames    = 1392;
constexpr rstd::size_t kSpectrumBins      = 640;
constexpr rstd::size_t kConvolutionFrames = 8192;
constexpr float        kBandExponent      = 0.25f;
constexpr float        kWeightBase        = 0.5009999871253967f;
constexpr float        kInputScale        = 127.0f;
constexpr float        kOutputScale       = 0.001f * 640.0f / (2089.0f * 0.5f);

void fft(std::complex<float>* values) {
    rstd::size_t swap_index = 0;
    for (rstd::size_t index = 1; index < kConvolutionFrames; ++index) {
        auto bit = kConvolutionFrames >> 1;
        while ((swap_index & bit) != 0) {
            swap_index ^= bit;
            bit >>= 1;
        }
        swap_index ^= bit;
        if (index < swap_index) {
            const auto value   = values[index];
            values[index]      = values[swap_index];
            values[swap_index] = value;
        }
    }
    for (rstd::size_t length = 2; length <= kConvolutionFrames; length <<= 1) {
        const float angle = -2.0f * f32::consts::PI.to_primitive() / static_cast<float>(length);
        const std::complex<float> root(std::cos(angle), std::sin(angle));
        for (rstd::size_t base = 0; base < kConvolutionFrames; base += length) {
            std::complex<float> weight(1.0f, 0.0f);
            for (rstd::size_t offset = 0; offset < length / 2; ++offset) {
                const auto even                    = values[base + offset];
                const auto odd                     = values[base + offset + length / 2] * weight;
                values[base + offset]              = even + odd;
                values[base + offset + length / 2] = even - odd;
                weight *= root;
            }
        }
    }
}

void inverse_fft(std::complex<float>* values) {
    for (rstd::size_t index = 0; index < kConvolutionFrames; ++index)
        values[index] = std::conj(values[index]);
    fft(values);
    const float normalization = 1.0f / static_cast<float>(kConvolutionFrames);
    for (rstd::size_t index = 0; index < kConvolutionFrames; ++index)
        values[index] = std::conj(values[index]) * normalization;
}

struct BluesteinPlan {
    BluesteinPlan() {
        for (rstd::size_t index = 0; index < kFftFrames; ++index) {
            const double position = static_cast<double>(index);
            const double angle    = f64::consts::PI.to_primitive() * position * position /
                                    static_cast<double>(kFftFrames);
            chirp[usize(index)]   = std::complex<float>(static_cast<float>(std::cos(angle)),
                                                        static_cast<float>(-std::sin(angle)));
            const auto kernel     = std::conj(chirp[usize(index)]);
            kernel_spectrum[usize(index)] = kernel;
            if (index != 0) kernel_spectrum[usize(kConvolutionFrames - index)] = kernel;
        }
        fft(kernel_spectrum.data());
    }

    void transform(rstd::array<std::complex<float>, kConvolutionFrames>& values) const {
        for (rstd::size_t index = 0; index < kFftFrames; ++index)
            values[usize(index)] *= chirp[usize(index)];
        fft(values.data());
        for (rstd::size_t index = 0; index < kConvolutionFrames; ++index)
            values[usize(index)] *= kernel_spectrum[usize(index)];
        inverse_fft(values.data());
        for (rstd::size_t index = 0; index < kFftFrames; ++index)
            values[usize(index)] *= chirp[usize(index)];
    }

    rstd::array<std::complex<float>, kFftFrames>         chirp {};
    rstd::array<std::complex<float>, kConvolutionFrames> kernel_spectrum {};
};

const BluesteinPlan& Plan() {
    static const BluesteinPlan plan;
    return plan;
}

void analyze_channel(const PcmWindow& window, rstd::size_t channel,
                     rstd::array<float, kResponseBins>& output) {
    rstd::array<std::complex<float>, kConvolutionFrames> spectrum {};
    // Bands exclude DC; remove the constant baseline before our FFT to avoid leakage.
    constexpr float baseline_reciprocal = 1.0f / kInputScale;
    constexpr auto  first_frame         = kWindowFrames - kAnalysisFrames;
    for (rstd::size_t frame = 0; frame < kAnalysisFrames; ++frame) {
        const auto  sample_frame  = first_frame + frame;
        const float sample        = window.samples[usize(sample_frame * kChannels + channel)];
        const float finite_sample = std::isfinite(sample) ? sample : 0.0f;
        const float value         = finite_sample * kInputScale + kInputScale;
        const float reciprocal    = value == 0.0f ? baseline_reciprocal : 1.0f / value;
        spectrum[usize(frame)] =
            std::complex<float>(finite_sample * kInputScale, reciprocal - baseline_reciprocal);
    }
    Plan().transform(spectrum);

    rstd::size_t band = 0;
    for (rstd::size_t bin = 1; bin < kSpectrumBins; ++bin) {
        const float coordinate =
            static_cast<float>(bin - 1) / static_cast<float>(kSpectrumBins - 1);
        const auto candidate = static_cast<rstd::size_t>(std::pow(coordinate, kBandExponent) *
                                                         static_cast<float>(kResponseBins));
        band                 = rstd::cmp::min(candidate, band + 1);
        const float angle    = f32::consts::PI.to_primitive() * coordinate;
        const float weight   = std::sqrt(kWeightBase - (1.0f - kWeightBase) * std::cos(angle));
        output[usize(band)]  = rstd::cmp::max(
            output[usize(band)], std::abs(spectrum[usize(bin)]) * weight * kOutputScale);
    }
}

} // namespace

bool ResponseEngine::analyze(const PcmWindow& window, ResponseFrame& output) {
    if (window.sample_rate_hz != kSampleRate || window.channels != kChannels ||
        window.frames != kWindowFrames || window.sequence == 0)
        return false;
    if (primed_ && window.generation == generation_ && window.sequence <= sequence_) return false;
    if (primed_ && window.generation < generation_) return false;

    ResponseFrame response {};
    response.generation       = window.generation;
    response.sequence         = window.sequence;
    response.captured_at_ns   = window.captured_at_ns;
    response.end_sample_frame = window.end_sample_frame;
    analyze_channel(window, 0, response.left);
    analyze_channel(window, 1, response.right);
    output      = response;
    generation_ = window.generation;
    sequence_   = window.sequence;
    primed_     = true;
    return true;
}

void ResponseEngine::end() {
    generation_ = 0;
    sequence_   = 0;
    primed_     = false;
}

} // namespace owe::audio
