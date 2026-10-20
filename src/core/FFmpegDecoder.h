#pragma once
#include "core/IDecoder.h"

namespace ake {

// libavformat/libavcodec/libswresample backend (needs FFmpeg >= 5.1 for AVChannelLayout).
class FFmpegDecoder : public IDecoder {
public:
    FFmpegDecoder();
    bool probe(const QString& path, AudioInfo* info, QString* error = nullptr) override;
    bool decode(const QString& path, std::vector<float>* pcm, AudioInfo* info, QString* error = nullptr,
                const Progress& progress = {}) override;
};

} // namespace ake
