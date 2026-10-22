#include "core/FFmpegDecoder.h"

#include <QFile>
#include <cstring>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/version.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

// FFmpeg 5.1 (libavutil 57.24) replaced the channel-layout integers by AVChannelLayout; FFmpeg 5.0 (libavformat 59) made the
// decoder returned by av_find_best_stream const. Ubuntu 22.04 ships 4.4, Haiku 6, current distributions 6 / 7: both are supported.
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100)
#define AKE_FF_NEW_CH_LAYOUT 1
#endif
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(59, 0, 0)
#define AKE_FF_CONST_CODEC 1
#endif

namespace ake {

namespace {

QString avErr(int code)
{
    // AVERROR(errno) values: use the OS wording ("No such file or directory") rather than FFmpeg's generic text.
    if (code < 0 && code > -200)
        return QString::fromLocal8Bit(std::strerror(-code));
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

struct FmtDeleter { void operator()(AVFormatContext* p) const { avformat_close_input(&p); } };
struct CodecDeleter { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct SwrDeleter { void operator()(SwrContext* p) const { swr_free(&p); } };
struct FrameDeleter { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using Fmt = std::unique_ptr<AVFormatContext, FmtDeleter>;
using Codec = std::unique_ptr<AVCodecContext, CodecDeleter>;
using Swr = std::unique_ptr<SwrContext, SwrDeleter>;
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;

bool fail(QString* out, const QString& msg)
{
    if (out) *out = msg;
    return false;
}

// Opens the container and returns the best audio stream index.
bool openInput(const QString& path, Fmt* fmt, int* streamIdx, const AVCodec** codec, QString* error)
{
    AVFormatContext* raw = nullptr;
    const QByteArray p = QFile::encodeName(path);
    int r = avformat_open_input(&raw, p.constData(), nullptr, nullptr);
    if (r < 0)
        return fail(error, QStringLiteral("cannot open: %1").arg(avErr(r)));
    fmt->reset(raw);
    r = avformat_find_stream_info(raw, nullptr);
    if (r < 0)
        return fail(error, QStringLiteral("cannot read stream info: %1").arg(avErr(r)));
#ifdef AKE_FF_CONST_CODEC
    const int idx = av_find_best_stream(raw, AVMEDIA_TYPE_AUDIO, -1, -1, codec, 0);
#else
    AVCodec* found = nullptr;
    const int idx = av_find_best_stream(raw, AVMEDIA_TYPE_AUDIO, -1, -1, &found, 0);
    *codec = found;
#endif
    if (idx < 0)
        return fail(error, QStringLiteral("no audio stream"));
    *streamIdx = idx;
    return true;
}

void fillInfo(const AVFormatContext* fmt, const AVStream* st, const AVCodec* codec, AudioInfo* info)
{
    info->durationMs = fmt->duration > 0 ? fmt->duration * 1000 / AV_TIME_BASE
                       : st->duration > 0 ? av_rescale_q(st->duration, st->time_base, AVRational{ 1, 1000 }) : 0;
    info->sampleRate = st->codecpar->sample_rate;
#ifdef AKE_FF_NEW_CH_LAYOUT
    info->channels = st->codecpar->ch_layout.nb_channels;
#else
    info->channels = st->codecpar->channels;
#endif
    info->codec = QString::fromUtf8(codec->name);
    auto tag = [&](const char* key) -> QString {
        const AVDictionaryEntry* e = av_dict_get(fmt->metadata, key, nullptr, 0);
        if (!e) e = av_dict_get(st->metadata, key, nullptr, 0);
        return e ? QString::fromUtf8(e->value) : QString();
    };
    info->title = tag("title");
    const QString ct = tag("creation_time");
    info->creationTime = ct.isEmpty() ? QDateTime() : QDateTime::fromString(ct, Qt::ISODate);
}

} // namespace

FFmpegDecoder::FFmpegDecoder()
{
    static const bool once = [] { av_log_set_level(AV_LOG_ERROR); return true; }();
    (void)once;
}

bool FFmpegDecoder::probe(const QString& path, AudioInfo* info, QString* error)
{
    Fmt fmt;
    int idx = -1;
    const AVCodec* codec = nullptr;
    if (!openInput(path, &fmt, &idx, &codec, error))
        return false;
    *info = {};
    fillInfo(fmt.get(), fmt->streams[idx], codec, info);
    return true;
}

bool FFmpegDecoder::decode(const QString& path, std::vector<float>* pcm, AudioInfo* info, QString* error,
                           const Progress& progress)
{
    pcm->clear();
    Fmt fmt;
    int idx = -1;
    const AVCodec* codec = nullptr;
    if (!openInput(path, &fmt, &idx, &codec, error))
        return false;
    AVStream* st = fmt->streams[idx];
    AudioInfo local;
    fillInfo(fmt.get(), st, codec, &local);

    Codec ctx(avcodec_alloc_context3(codec));
    if (!ctx) return fail(error, "out of memory");
    int r = avcodec_parameters_to_context(ctx.get(), st->codecpar);
    if (r < 0) return fail(error, QStringLiteral("codec parameters: %1").arg(avErr(r)));
    r = avcodec_open2(ctx.get(), codec, nullptr);
    if (r < 0) return fail(error, QStringLiteral("cannot open codec %1: %2").arg(local.codec, avErr(r)));

#ifdef AKE_FF_NEW_CH_LAYOUT
    // Some files (raw wav, ...) report an unspecified layout: fall back to the default one for the channel count.
    AVChannelLayout inLayout = {};
    if (ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || !av_channel_layout_check(&ctx->ch_layout))
        av_channel_layout_default(&inLayout, ctx->ch_layout.nb_channels > 0 ? ctx->ch_layout.nb_channels : 1);
    else
        av_channel_layout_copy(&inLayout, &ctx->ch_layout);
    AVChannelLayout outLayout = {};
    av_channel_layout_default(&outLayout, 1);

    SwrContext* rawSwr = nullptr;
    r = swr_alloc_set_opts2(&rawSwr, &outLayout, AV_SAMPLE_FMT_FLT, kDecodeSampleRate, &inLayout, ctx->sample_fmt,
                            ctx->sample_rate, 0, nullptr);
    Swr swr(rawSwr);
    auto releaseLayouts = [&] { av_channel_layout_uninit(&inLayout); av_channel_layout_uninit(&outLayout); };
#else
    int64_t inLayout = ctx->channel_layout ? int64_t(ctx->channel_layout)
                                           : av_get_default_channel_layout(ctx->channels > 0 ? ctx->channels : 1);
    SwrContext* rawSwr = swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_MONO, AV_SAMPLE_FMT_FLT, kDecodeSampleRate, inLayout,
                                            ctx->sample_fmt, ctx->sample_rate, 0, nullptr);
    r = rawSwr ? 0 : AVERROR(ENOMEM);
    Swr swr(rawSwr);
    auto releaseLayouts = [] {};
#endif
    // Normalize the downmix matrix: FFmpeg's default stereo->mono sums L and R at 0.707 each, which is louder than
    // either channel (and clips full-scale input). With maxval=1 the output can never exceed full scale.
    if (r >= 0 && swr)
        av_opt_set_double(swr.get(), "rematrix_maxval", 1.0, 0);
    if (r < 0 || !swr || (r = swr_init(swr.get())) < 0) {
        releaseLayouts();
        return fail(error, QStringLiteral("cannot set up resampler: %1").arg(avErr(r)));
    }
    releaseLayouts();

    Packet pkt(av_packet_alloc());
    Frame frame(av_frame_alloc());
    if (!pkt || !frame) return fail(error, "out of memory");

    const double totalSec = local.durationMs > 0 ? local.durationMs / 1000.0 : 0;
    bool cancelled = false;
    auto convert = [&](const AVFrame* f) -> bool { // f == nullptr flushes the resampler
        const int inSamples = f ? f->nb_samples : 0;
        const int maxOut = swr_get_out_samples(swr.get(), inSamples);
        if (maxOut < 0) return false;
        const size_t old = pcm->size();
        pcm->resize(old + size_t(maxOut));
        uint8_t* out = reinterpret_cast<uint8_t*>(pcm->data() + old);
        const int got = swr_convert(swr.get(), &out, maxOut, f ? const_cast<const uint8_t**>(f->extended_data) : nullptr,
                                    inSamples);
        if (got < 0) { pcm->resize(old); return false; }
        pcm->resize(old + size_t(got));
        return true;
    };
    auto drain = [&]() -> bool { // pull every available frame out of the decoder
        for (;;) {
            const int rr = avcodec_receive_frame(ctx.get(), frame.get());
            if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) return true;
            if (rr < 0) return false;
            const bool ok = convert(frame.get());
            av_frame_unref(frame.get());
            if (!ok) return false;
        }
    };

    int skipped = 0; // undecodable packets (corrupt data): tolerated, but not if nothing decodes at all
    while (!cancelled) {
        r = av_read_frame(fmt.get(), pkt.get());
        if (r < 0) break; // EOF or read error: keep what we have
        if (pkt->stream_index == idx) {
            const int sr = avcodec_send_packet(ctx.get(), pkt.get());
            if (sr < 0) skipped++;
            else if (!drain()) return fail(error, "decode error");
            if (progress && pkt->pts != AV_NOPTS_VALUE) {
                const double pos = pkt->pts * av_q2d(st->time_base);
                const double frac = totalSec > 0 ? qBound(0.0, pos / totalSec, 1.0) : -1.0;
                if (!progress(frac)) cancelled = true;
            }
        }
        av_packet_unref(pkt.get());
    }
    if (cancelled)
        return fail(error, "cancelled");

    avcodec_send_packet(ctx.get(), nullptr); // flush the decoder, then the resampler
    drain();
    convert(nullptr);

    if (pcm->empty())
        return fail(error, skipped ? "no decodable audio (corrupt file?)" : "file contains no audio samples");

    // The decoded length is authoritative; containers sometimes lie or omit the duration.
    local.durationMs = qint64(pcm->size()) * 1000 / kDecodeSampleRate;
    if (info) *info = local;
    if (progress) progress(1.0);
    return true;
}

} // namespace ake
