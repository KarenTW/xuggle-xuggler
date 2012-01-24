/*
 * Westwood Studios AUD Format Demuxer
 * Copyright (c) 2003 The ffmpeg Project
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Westwood Studios AUD file demuxer
 * by Mike Melanson (melanson@pcisys.net)
 * for more information on the Westwood file formats, visit:
 *   http://www.pcisys.net/~melanson/codecs/
 *   http://www.geocities.com/SiliconValley/8682/aud3.txt
 *
 * Implementation note: There is no definite file signature for AUD files.
 * The demuxer uses a probabilistic strategy for content detection. This
 * entails performing sanity checks on certain header values in order to
 * qualify a file. Refer to wsaud_probe() for the precise parameters.
 */

#include "libavutil/intreadwrite.h"
#include "avformat.h"
#include "internal.h"

#define AUD_HEADER_SIZE 12
#define AUD_CHUNK_PREAMBLE_SIZE 8
#define AUD_CHUNK_SIGNATURE 0x0000DEAF

typedef struct WsAudDemuxContext {
    int audio_samplerate;
    int audio_channels;
    int audio_bits;
    enum CodecID audio_type;
    int audio_stream_index;
    int64_t audio_frame_counter;
} WsAudDemuxContext;

static int wsaud_probe(AVProbeData *p)
{
    int field;

    /* Probabilistic content detection strategy: There is no file signature
     * so perform sanity checks on various header parameters:
     *   8000 <= sample rate (16 bits) <= 48000  ==> 40001 acceptable numbers
     *   flags <= 0x03 (2 LSBs are used)         ==> 4 acceptable numbers
     *   compression type (8 bits) = 1 or 99     ==> 2 acceptable numbers
     *   first audio chunk signature (32 bits)   ==> 1 acceptable number
     * The number space contains 2^64 numbers. There are 40001 * 4 * 2 * 1 =
     * 320008 acceptable number combinations.
     */

    if (p->buf_size < AUD_HEADER_SIZE + AUD_CHUNK_PREAMBLE_SIZE)
        return 0;

    /* check sample rate */
    field = AV_RL16(&p->buf[0]);
    if ((field < 8000) || (field > 48000))
        return 0;

    /* enforce the rule that the top 6 bits of this flags field are reserved (0);
     * this might not be true, but enforce it until deemed unnecessary */
    if (p->buf[10] & 0xFC)
        return 0;

    /* note: only check for WS IMA (type 99) right now since there is no
     * support for type 1 */
    if (p->buf[11] != 99)
        return 0;

    /* read ahead to the first audio chunk and validate the first header signature */
    if (AV_RL32(&p->buf[16]) != AUD_CHUNK_SIGNATURE)
        return 0;

    /* return 1/2 certainty since this file check is a little sketchy */
    return AVPROBE_SCORE_MAX / 2;
}

static int wsaud_read_header(AVFormatContext *s,
                             AVFormatParameters *ap)
{
    WsAudDemuxContext *wsaud = s->priv_data;
    AVIOContext *pb = s->pb;
    AVStream *st;
    unsigned char header[AUD_HEADER_SIZE];

    if (avio_read(pb, header, AUD_HEADER_SIZE) != AUD_HEADER_SIZE)
        return AVERROR(EIO);
    wsaud->audio_samplerate = AV_RL16(&header[0]);
    if (header[11] == 99)
        wsaud->audio_type = CODEC_ID_ADPCM_IMA_WS;
    else
        return AVERROR_INVALIDDATA;

    /* flag 0 indicates stereo */
    wsaud->audio_channels = (header[10] & 0x1) + 1;
    /* flag 1 indicates 16 bit audio */
    wsaud->audio_bits = (((header[10] & 0x2) >> 1) + 1) * 8;

    /* initialize the audio decoder stream */
    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    avpriv_set_pts_info(st, 33, 1, wsaud->audio_samplerate);
    st->codec->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codec->codec_id = wsaud->audio_type;
    st->codec->codec_tag = 0;  /* no tag */
    st->codec->channels = wsaud->audio_channels;
    st->codec->sample_rate = wsaud->audio_samplerate;
    st->codec->bits_per_coded_sample = wsaud->audio_bits;
    st->codec->bit_rate = st->codec->channels * st->codec->sample_rate *
        st->codec->bits_per_coded_sample / 4;
    st->codec->block_align = st->codec->channels * st->codec->bits_per_coded_sample;

    wsaud->audio_stream_index = st->index;
    wsaud->audio_frame_counter = 0;

    return 0;
}

static int wsaud_read_packet(AVFormatContext *s,
                             AVPacket *pkt)
{
    WsAudDemuxContext *wsaud = s->priv_data;
    AVIOContext *pb = s->pb;
    unsigned char preamble[AUD_CHUNK_PREAMBLE_SIZE];
    unsigned int chunk_size;
    int ret = 0;

    if (avio_read(pb, preamble, AUD_CHUNK_PREAMBLE_SIZE) !=
        AUD_CHUNK_PREAMBLE_SIZE)
        return AVERROR(EIO);

    /* validate the chunk */
    if (AV_RL32(&preamble[4]) != AUD_CHUNK_SIGNATURE)
        return AVERROR_INVALIDDATA;

    chunk_size = AV_RL16(&preamble[0]);
    ret= av_get_packet(pb, pkt, chunk_size);
    if (ret != chunk_size)
        return AVERROR(EIO);
    pkt->stream_index = wsaud->audio_stream_index;
    pkt->pts = wsaud->audio_frame_counter;
    pkt->pts /= wsaud->audio_samplerate;

    /* 2 samples/byte, 1 or 2 samples per frame depending on stereo */
    wsaud->audio_frame_counter += (chunk_size * 2) / wsaud->audio_channels;

    return ret;
}

AVInputFormat ff_wsaud_demuxer = {
    .name           = "wsaud",
    .long_name      = NULL_IF_CONFIG_SMALL("Westwood Studios audio format"),
    .priv_data_size = sizeof(WsAudDemuxContext),
    .read_probe     = wsaud_probe,
    .read_header    = wsaud_read_header,
    .read_packet    = wsaud_read_packet,
};