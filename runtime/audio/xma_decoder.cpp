#include "xma_decoder.h"

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

namespace {

// The 34-byte XMA2 "extension" extradata ffmpeg's AV_CODEC_ID_XMA2 decoder expects
// (the WAVEFORMATEX-stripped tail, little-endian): wNumStreams, dwChannelMask,
// dwSamplesEncoded, dwBytesPerBlock, dwPlayBegin/Length, dwLoopBegin/Length,
// bLoopCount, bEncoderVersion, wBlockCount.
//
// Only three fields matter to the decoder: the stream count, the channel mask (it
// derives the layout from it) and the encoder version. The rest describe playback
// bounds the HARDWARE would honour; we drive those from the context's own fields.
void BuildXma2Extradata(uint8_t* ed /*34*/, int channels, uint32_t bytesPerBlock)
{
    memset(ed, 0, 34);
    auto wr16 = [&](int off, uint16_t v) { ed[off] = v & 0xFF; ed[off + 1] = v >> 8; };
    auto wr32 = [&](int off, uint32_t v) {
        ed[off] = v; ed[off + 1] = v >> 8; ed[off + 2] = v >> 16; ed[off + 3] = v >> 24;
    };
    wr16(0, 1);                                         // wNumStreams
    wr32(2, channels == 2 ? 0x3u : 0x4u);               // dwChannelMask (L|R / centre)
    wr32(6, 0);                                         // dwSamplesEncoded
    wr32(10, bytesPerBlock ? bytesPerBlock : 0x10000u); // dwBytesPerBlock
    wr32(14, 0);                                        // dwPlayBegin
    wr32(18, 0);                                        // dwPlayLength
    wr32(22, 0);                                        // dwLoopBegin
    wr32(26, 0);                                        // dwLoopLength
    ed[30] = 0;                                         // bLoopCount
    ed[31] = 4;                                         // bEncoderVersion
    wr16(32, 1);                                        // wBlockCount
}

void AppendFrame(AVFrame* f, int channels, std::vector<float>& out)
{
    const int n = f->nb_samples;
    if (av_sample_fmt_is_planar((AVSampleFormat)f->format))
    {
        for (int i = 0; i < n; i++)
            for (int c = 0; c < channels; c++)
            {
                const float* p = reinterpret_cast<const float*>(f->data[c]);
                out.push_back(p ? p[i] : 0.f);
            }
    }
    else
    {
        const float* p = reinterpret_cast<const float*>(f->data[0]);
        for (int i = 0; i < n * channels; i++)
            out.push_back(p[i]);
    }
}

} // namespace

struct XmaDecoder
{
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    int channels = 2;
};

bool Xma_CodecAvailable()
{
    return avcodec_find_decoder(AV_CODEC_ID_XMA2) != nullptr;
}

XmaDecoder* Xma_Create(int channels, int sampleRate)
{
    XmaDecoder* d = new XmaDecoder();
    d->channels = channels;
    d->codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    if (!d->codec)
    {
        fprintf(stderr, "[xma] no XMA2 decoder in libavcodec\n");
        delete d;
        return nullptr;
    }
    d->ctx = avcodec_alloc_context3(d->codec);
    d->ctx->sample_rate = sampleRate;
    av_channel_layout_default(&d->ctx->ch_layout, channels);
    d->ctx->extradata = (uint8_t*)av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE);
    d->ctx->extradata_size = 34;
    BuildXma2Extradata(d->ctx->extradata, channels, 0x10000);
    if (avcodec_open2(d->ctx, d->codec, nullptr) < 0)
    {
        fprintf(stderr, "[xma] avcodec_open2 failed (ch=%d rate=%d)\n", channels, sampleRate);
        avcodec_free_context(&d->ctx);
        delete d;
        return nullptr;
    }
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    return d;
}

void Xma_Destroy(XmaDecoder* d)
{
    if (!d) return;
    if (d->frame) av_frame_free(&d->frame);
    if (d->pkt) av_packet_free(&d->pkt);
    if (d->ctx) avcodec_free_context(&d->ctx);
    delete d;
}

int Xma_DecodePacket(XmaDecoder* d, const uint8_t* packet, size_t size,
                     std::vector<float>& out)
{
    if (!d || !d->ctx) return -1;
    d->pkt->data = const_cast<uint8_t*>(packet);
    d->pkt->size = (int)size;
    const size_t before = out.size();
    if (avcodec_send_packet(d->ctx, d->pkt) < 0)
        return -1;
    while (avcodec_receive_frame(d->ctx, d->frame) == 0)
        AppendFrame(d->frame, d->ctx->ch_layout.nb_channels, out);
    return (int)(out.size() - before);
}

bool Xma_Validate(const uint8_t* data, size_t size, int channels, int sampleRate)
{
    XmaDecoder* d = Xma_Create(channels, sampleRate);
    if (!d) return false;
    std::vector<float> out;
    size_t pkts = 0;
    for (size_t off = 0; off + 2048 <= size; off += 2048)
    {
        Xma_DecodePacket(d, data + off, 2048, out);
        if (++pkts >= 16) break;
    }
    // Flush, or the tail frames the decoder is holding never appear and a short
    // input reads as "decoded nothing".
    d->pkt->data = nullptr;
    d->pkt->size = 0;
    avcodec_send_packet(d->ctx, d->pkt);
    while (avcodec_receive_frame(d->ctx, d->frame) == 0)
        AppendFrame(d->frame, d->ctx->ch_layout.nb_channels, out);

    double sum = 0;
    float peak = 0;
    for (float f : out)
    {
        sum += double(f) * f;
        const float a = f < 0 ? -f : f;
        if (a > peak) peak = a;
    }
    const double rms = out.empty() ? 0 : std::sqrt(sum / out.size());
    fprintf(stderr,
            "[xma] validate: fed %zu pkts -> %zu float samples, decoder ch=%d rate=%d "
            "fmt=%s rms=%.4f peak=%.4f\n",
            pkts, out.size(), d->ctx->ch_layout.nb_channels, d->ctx->sample_rate,
            av_get_sample_fmt_name(d->ctx->sample_fmt), rms, peak);
    Xma_Destroy(d);
    return !out.empty() && peak > 0;
}
