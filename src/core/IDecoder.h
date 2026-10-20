#pragma once
#include <QDateTime>
#include <QString>
#include <functional>
#include <vector>

namespace ake {

struct AudioInfo {
    qint64 durationMs = 0;   // 0 = unknown
    int sampleRate = 0;      // of the source stream
    int channels = 0;
    QString codec;
    QString title;           // container tag, if any
    QDateTime creationTime;  // container tag "creation_time", if any (invalid otherwise)
};

// whisper.cpp wants 16 kHz mono float samples in [-1, 1].
constexpr int kDecodeSampleRate = 16000;

// Decoding backend. Implementations must be safe to call concurrently from several threads.
class IDecoder {
public:
    virtual ~IDecoder() = default;

    // Reads container metadata only (fast).
    virtual bool probe(const QString& path, AudioInfo* info, QString* error = nullptr) = 0;

    // Decodes the whole file into 16 kHz mono float PCM. `progress` receives 0..1 (or -1 when the duration is
    // unknown); returning false cancels (decode() then returns false with *error == "cancelled").
    using Progress = std::function<bool(double)>;
    virtual bool decode(const QString& path, std::vector<float>* pcm, AudioInfo* info, QString* error = nullptr,
                        const Progress& progress = {}) = 0;
};

} // namespace ake
