#include "core/SpeechGate.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "core/ITranscriber.h"

namespace ake {

namespace {

// In-place iterative radix-2 FFT (n a power of two)
void fft(std::vector<std::complex<float>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.f * float(M_PI) / float(len);
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.f, 0.f);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

} // namespace

double SpeechGate::Result::coverage(long long startMs, long long endMs) const
{
    if (speech.empty() || endMs <= startMs) return 1.0;
    const double w = windowSeconds() * 1000.0;
    const long long first = std::max<long long>(0, static_cast<long long>(startMs / w));
    const long long last = std::min<long long>(static_cast<long long>(speech.size()) - 1, static_cast<long long>((endMs - 1) / w));
    if (first > last) return 1.0;                        // beyond the analysed audio
    long long n = 0, hit = 0;
    for (long long i = first; i <= last; ++i) { ++n; hit += speech[size_t(i)] ? 1 : 0; }
    return double(hit) / double(n);
}

SpeechGate::Result SpeechGate::analyze(const std::vector<float>& pcm)
{
    Result r;
    const size_t frames = pcm.size() / kFrame;
    if (frames < size_t(kFramesPerWindow)) return r;     // too short to judge
    std::vector<float> hann(kFrame);
    for (int i = 0; i < kFrame; ++i) hann[size_t(i)] = 0.5f - 0.5f * std::cos(2.f * float(M_PI) * float(i) / float(kFrame - 1));
    const int lo = int(std::ceil(100.0 * kFrame / kSampleRate)), hi = int(4000.0 * kFrame / kSampleRate);
    std::vector<std::complex<float>> buf(kFrame);
    std::vector<float> db(frames), flat(frames);
    for (size_t f = 0; f < frames; ++f) {
        const float* x = &pcm[f * kFrame];
        double e = 0;
        for (int i = 0; i < kFrame; ++i) { buf[size_t(i)] = { x[i] * hann[size_t(i)], 0.f }; e += double(x[i]) * x[i]; }
        db[f] = 20.f * std::log10(float(std::sqrt(e / kFrame)) + 1e-9f);
        fft(buf);
        double logSum = 0, sum = 0;
        for (int k = lo; k <= hi; ++k) {
            const double p = double(std::norm(buf[size_t(k)])) + 1e-12;
            logSum += std::log(p);
            sum += p;
        }
        const int bins = hi - lo + 1;
        flat[f] = float(std::exp(logSum / bins) / (sum / bins));
    }
    const size_t windows = frames / kFramesPerWindow;
    std::vector<float> wdb(windows), wflat(windows);
    for (size_t w = 0; w < windows; ++w) {
        double d = 0, fl = 0;
        for (int k = 0; k < kFramesPerWindow; ++k) { d += db[w * kFramesPerWindow + size_t(k)]; fl += flat[w * kFramesPerWindow + size_t(k)]; }
        wdb[w] = float(d / kFramesPerWindow);
        wflat[w] = float(fl / kFramesPerWindow);
    }
    // "silence" is relative to how loud the recording is: a voice recorded at -40 dB must not count as silence
    std::vector<float> sorted(wdb);
    std::sort(sorted.begin(), sorted.end());
    const float loud = sorted[std::min(sorted.size() - 1, size_t(double(sorted.size()) * 0.95))];
    r.quietDb = std::clamp(loud - kQuietBelowLoudDb, kQuietFloorDb, kQuietDb);
    r.speech.resize(windows);
    size_t speechy = 0;
    for (size_t w = 0; w < windows; ++w) {
        const bool ok = wdb[w] > r.quietDb && wflat[w] < kMaxFlatness;
        r.speech[w] = ok;
        speechy += ok ? 1 : 0;
    }
    r.speechRatio = double(speechy) / double(windows);
    return r;
}

int SpeechGate::dropNonSpeech(const Result& r, QList<TranscriptSegment>* segments)
{
    int dropped = 0;
    for (auto it = segments->begin(); it != segments->end();) {
        if (r.coverage(it->startMs, it->endMs) < kMinCoverage) { it = segments->erase(it); ++dropped; }
        else ++it;
    }
    return dropped;
}

} // namespace ake
