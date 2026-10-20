#pragma once
#include <mutex>

#include "core/ITranscriber.h"

struct whisper_context;

namespace ake {

// whisper.cpp backend (CPU). Calls are serialized by an internal mutex: one inference at a time per instance.
class WhisperTranscriber : public ITranscriber {
public:
    WhisperTranscriber();
    ~WhisperTranscriber() override;
    WhisperTranscriber(const WhisperTranscriber&) = delete;
    WhisperTranscriber& operator=(const WhisperTranscriber&) = delete;

    bool load(const QString& modelPath, QString* error = nullptr) override;
    bool isLoaded() const override { return m_ctx != nullptr; }
    QString modelName() const override { return m_modelName; }

    bool transcribe(const std::vector<float>& pcm, const TranscribeOptions& options, TranscriptResult* out,
                    QString* error = nullptr, const IDecoder::Progress& progress = {}) override;

    static int defaultThreads();

private:
    std::mutex m_mutex;
    whisper_context* m_ctx = nullptr;
    QString m_modelName;
};

} // namespace ake
