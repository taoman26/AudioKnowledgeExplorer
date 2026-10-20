#pragma once
// Test doubles shared by the job/UI tests: a decoder and a transcriber that "work" without any media or model.
#include <QString>
#include <QThread>
#include <atomic>
#include <memory>
#include <mutex>

#include "core/AnalysisPipeline.h"
#include "core/IDecoder.h"
#include "core/ITranscriber.h"

namespace fakes {
using namespace ake;

inline std::atomic<int> g_running{ 0 }; inline std::atomic<int> g_maxRunning{ 0 }, g_stepMs{ 5 }, g_factoryCalls{ 0 };
inline std::mutex g_mu;
inline TranscribeOptions g_lastOptions;
inline QString g_lastModel;

struct FakeDecoder : IDecoder {
    bool probe(const QString&, AudioInfo*, QString*) override { return true; }
    bool decode(const QString& path, std::vector<float>* pcm, AudioInfo* info, QString* err, const Progress& progress) override
    {
        if (path.contains("undecodable")) { *err = "cannot open: broken file"; return false; }
        for (int i = 1; i <= 4; ++i) {
            QThread::msleep(g_stepMs);
            if (progress && !progress(i / 4.0)) { *err = "cancelled"; return false; }
        }
        pcm->assign(16000 * 5, 0.1f);
        info->durationMs = 5000;
        info->sampleRate = 44100;
        return true;
    }
};

struct FakeTranscriber : ITranscriber {
    QString model;
    bool load(const QString& path, QString* err) override
    {
        if (path.contains("missing")) { *err = "model file not found: " + path; return false; }
        model = path;
        return true;
    }
    bool isLoaded() const override { return !model.isEmpty(); }
    QString modelName() const override { return "fake"; }
    bool transcribe(const std::vector<float>&, const TranscribeOptions& o, TranscriptResult* out, QString* err,
                    const IDecoder::Progress& progress) override
    {
        const int now = ++g_running;
        g_maxRunning = qMax(g_maxRunning.load(), now);
        { std::lock_guard<std::mutex> l(g_mu); g_lastOptions = o; g_lastModel = model; }
        bool cancelled = false;
        for (int i = 1; i <= 10 && !cancelled; ++i) {
            QThread::msleep(g_stepMs);
            if (progress && !progress(i / 10.0)) cancelled = true;
        }
        --g_running;
        if (cancelled) { *err = "cancelled"; return false; }
        out->language = o.language == "auto" ? "ja" : o.language;
        out->model = "fake";
        out->segments = { { 0, 2000, "こんにちは" }, { 2000, 5000, "テストです" } };
        return true;
    }
};

inline AnalysisPipeline::TranscriberFactory factory()
{
    return [] { ++g_factoryCalls; return std::unique_ptr<ITranscriber>(new FakeTranscriber); };
}


} // namespace fakes
