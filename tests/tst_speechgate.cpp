#include <QtTest>
#include <cmath>

#include "Fakes.h"
#include "core/AnalysisPipeline.h"
#include "core/SpeechGate.h"

using namespace ake;
using namespace fakes;

namespace {
constexpr int kSr = SpeechGate::kSampleRate;

// speech-like: a 150 Hz voice (harmonics 1..20, falling off), switched on and off about four times a second
std::vector<float> voiced(double seconds, float amp = 0.2f)
{
    std::vector<float> x(size_t(seconds * kSr));
    for (size_t i = 0; i < x.size(); ++i) {
        const double t = double(i) / kSr;
        double v = 0;
        for (int k = 1; k <= 20; ++k) v += std::sin(2 * M_PI * 150.0 * k * t + k) / k;
        const double env = 0.5 + 0.5 * std::sin(2 * M_PI * 4.0 * t);
        x[i] = float(amp * 0.25 * v * env);
    }
    return x;
}
std::vector<float> whiteNoise(double seconds, float amp = 0.3f)
{
    std::vector<float> x(size_t(seconds * kSr));
    unsigned s = 12345;
    for (float& v : x) { s = s * 1664525u + 1013904223u; v = amp * (float(s >> 8) / float(1 << 24) * 2.f - 1.f); }
    return x;
}
void append(std::vector<float>& a, const std::vector<float>& b) { a.insert(a.end(), b.begin(), b.end()); }

struct CannedDecoder : IDecoder {
    std::vector<float> pcm;
    bool probe(const QString&, AudioInfo*, QString*) override { return true; }
    bool decode(const QString&, std::vector<float>* out, AudioInfo* info, QString*, const Progress&) override
    {
        *out = pcm;
        info->durationMs = qint64(pcm.size() * 1000 / kSr);
        return true;
    }
};
} // namespace

class TstSpeechGate : public QObject {
    Q_OBJECT
private slots:
    void whiteNoiseIsNotSpeech()
    {
        const auto r = SpeechGate::analyze(whiteNoise(10));
        QVERIFY2(r.speechRatio < 0.01, qPrintable(QString::number(r.speechRatio)));
        QVERIFY(!r.hasSpeech());
        QVERIFY(!r.speech.empty());
    }
    void silenceAndVeryQuietSoundAreNotSpeech()
    {
        QVERIFY(!SpeechGate::analyze(std::vector<float>(10 * kSr, 0.f)).hasSpeech());
        QVERIFY(!SpeechGate::analyze(voiced(10, 0.0002f)).hasSpeech());          // a voice at about -75 dB: inaudible, not speech
        QVERIFY(SpeechGate::analyze(voiced(10, 0.05f)).hasSpeech());             // a quiet voice at about -30 dB still is
        QVERIFY(SpeechGate::analyze(voiced(10, 0.01f)).hasSpeech());             // so is one at about -60 dB (it would be silence by the old fixed threshold): silence is relative to the recording
    }
    void aVoiceIsSpeech()
    {
        const auto r = SpeechGate::analyze(voiced(10));
        QVERIFY2(r.speechRatio > 0.9, qPrintable(QString::number(r.speechRatio)));
        QVERIFY(r.hasSpeech());
    }
    void audioTooShortToJudgeIsLetThrough()
    {
        const auto r = SpeechGate::analyze(whiteNoise(0.3));
        QVERIFY(r.speech.empty());
        QVERIFY(r.hasSpeech());                                                  // we never throw away what we could not examine
        QVERIFY(SpeechGate::analyze({}).hasSpeech());
    }
    void noiseInTheMiddleOfSpeechIsLocated()
    {
        std::vector<float> x = voiced(10);
        append(x, whiteNoise(10));
        append(x, voiced(10));
        const auto r = SpeechGate::analyze(x);
        QVERIFY(r.hasSpeech());
        QVERIFY2(qAbs(r.speechRatio - 2.0 / 3.0) < 0.05, qPrintable(QString::number(r.speechRatio)));
        QVERIFY(r.coverage(0, 9000) > 0.9);
        QVERIFY2(r.coverage(11000, 19000) < 0.1, qPrintable(QString::number(r.coverage(11000, 19000))));
        QVERIFY(r.coverage(21000, 29000) > 0.9);
        QCOMPARE(r.coverage(500000, 600000), 1.0);                               // beyond the audio: unknown, not noise
        QCOMPARE(r.coverage(5000, 5000), 1.0);                                   // an empty span
        QList<TranscriptSegment> segs = { { 1000, 4000, "前の発言" }, { 12000, 15000, "雑音への幻覚" }, { 18000, 22000, "境目の発言" }, { 24000, 28000, "後の発言" } };
        const int dropped = SpeechGate::dropNonSpeech(r, &segs);
        QCOMPARE(dropped, 1);
        QStringList texts; for (const auto& s : segs) texts << s.text;
        QCOMPARE(texts, QStringList({ "前の発言", "境目の発言", "後の発言" }));            // the one that straddles a boundary stays
    }

    // ---- inside the pipeline
    void aRecordingOfNoiseIsNotTranscribedAtAll()
    {
        CannedDecoder dec; dec.pcm = whiteNoise(5);
        g_factoryCalls = 0;
        AnalysisPipeline p(dec, factory());
        TranscriptResult r; AudioInfo info; QString err;
        AnalysisConfig cfg; cfg.modelPath = "/models/ggml-small.bin"; cfg.language = "ja";
        std::atomic<bool> cancel{ false };
        QStringList stages;
        QVERIFY2(p.run("noise.wav", cfg, cancel, [&](const QString& st, double) { if (!stages.contains(st)) stages << st; }, &r, &info, &err), qPrintable(err));
        QVERIFY(r.segments.isEmpty());
        QCOMPARE(r.model, QString("ggml-small"));
        QCOMPARE(r.language, QString("ja"));
        QCOMPARE(int(g_factoryCalls), 0);                                        // the model was not even loaded
        cfg.language = "auto";
        QVERIFY(p.run("noise.wav", cfg, cancel, nullptr, &r, &info, &err));
        QVERIFY(r.language.isEmpty());
    }
    void theGateCanBeTurnedOffAndSpeechPassesThroughUntouched()
    {
        CannedDecoder dec; dec.pcm = whiteNoise(5);
        g_factoryCalls = 0;
        AnalysisPipeline p(dec, factory());
        TranscriptResult r; AudioInfo info; QString err;
        AnalysisConfig cfg; cfg.speechGate = false;
        std::atomic<bool> cancel{ false };
        QVERIFY(p.run("noise.wav", cfg, cancel, nullptr, &r, &info, &err));
        QCOMPARE(r.segments.size(), 2);                                          // without the gate whisper (here the fake) runs
        QCOMPARE(int(g_factoryCalls), 1);
        dec.pcm = voiced(5);
        cfg.speechGate = true;
        QVERIFY(p.run("voice.wav", cfg, cancel, nullptr, &r, &info, &err));
        QCOMPARE(r.segments.size(), 2);                                          // speech: both fake segments stay
    }
    void segmentsWrittenInNoiseAreDroppedByThePipeline()
    {
        CannedDecoder dec;
        dec.pcm = voiced(2);                                                     // the fake transcriber answers 0-2 s and 2-5 s
        append(dec.pcm, whiteNoise(3));
        AnalysisPipeline p(dec, factory());
        TranscriptResult r; AudioInfo info; QString err;
        std::atomic<bool> cancel{ false };
        QVERIFY(p.run("mixed.wav", {}, cancel, nullptr, &r, &info, &err));
        QCOMPARE(r.segments.size(), 1);
        QCOMPARE(r.segments[0].text, QString("こんにちは"));
    }
};

QTEST_MAIN(TstSpeechGate)
#include "tst_speechgate.moc"
