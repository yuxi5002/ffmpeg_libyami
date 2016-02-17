/*
 * libyami.cpp -- h264 decoder uses libyami
 *
 *  Copyright (C) 2014 Intel Corporation
 *    Author: Zhao Halley<halley.zhao@intel.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <deque>

extern "C" {
#include "avcodec.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "internal.h"
}
#include "VideoDecoderHost.h"
#include "VideoEncoderHost.h"


using namespace YamiMediaCodec;
#ifndef VA_FOURCC_I420
#define VA_FOURCC_I420 VA_FOURCC('I','4','2','0')
#endif

#ifndef VA_FOURCC_NV12
#define VA_FOURCC_NV12 VA_FOURCC('N','V','1','2')
#endif

#define DECODE_TRACE(format, ...)  av_log(avctx, AV_LOG_VERBOSE, "# decode # line:%4d " format, __LINE__, ##__VA_ARGS__)

#define ENCODE_TRACE(format, ...)  av_log(avctx, AV_LOG_VERBOSE, "< encode > line:%4d " format, __LINE__, ##__VA_ARGS__)

typedef enum {
    DECODE_THREAD_NOT_INIT = 0,
    DECODE_THREAD_RUNING,
    DECODE_THREAD_GOT_EOS,
    DECODE_THREAD_EXIT,
} DecodeThreadStatus;

typedef enum {
    ENCODE_THREAD_NOT_INIT = 0,
    ENCODE_THREAD_RUNING,
    ENCODE_THREAD_GOT_EOS,
    ENCODE_THREAD_EXIT,
} EncodeThreadStatus;

#define DEBUG_LIBYAMI 0

#define QUEUE_MAX_SIZE 8
#define QUEUE_MIN_SIZE 8

struct YamiDecContext {
    AVCodecContext *avctx;
    pthread_mutex_t mutex_; // mutex for decoder->getOutput() and YamiContext itself update (decode_status, etc)

    IVideoDecoder *decoder;
    VideoDataMemoryType output_type;
    const VideoFormatInfo *format_info;
    pthread_t decode_thread_id;
    std::deque<VideoDecodeBuffer *> *in_queue;
    pthread_mutex_t in_mutex; // mutex for in_queue
    pthread_cond_t in_cond;   // decode thread condition wait
    DecodeThreadStatus decode_status;

    // debug use
    int decode_count;
    int decode_count_yami;
    int render_count;
};

#include <fcntl.h>
#include <unistd.h>
#include <va/va_drm.h>

static VADisplay createVADisplay(void)
{
    static VADisplay vadisplay = NULL;


    if (!vadisplay) {
        int fd = open("/dev/dri/card0", O_RDWR);
        if (fd < 0) {
            av_log(NULL, AV_LOG_ERROR, "open card0 failed");
            return NULL;
        }
        vadisplay = vaGetDisplayDRM(fd);
        int majorVersion, minorVersion;
        VAStatus vaStatus = vaInitialize(vadisplay, &majorVersion, &minorVersion);
        if (vaStatus != VA_STATUS_SUCCESS) {
            av_log(NULL, AV_LOG_ERROR, "va init failed, status =  %d", vaStatus);
            close(fd);
            vadisplay = NULL;
            return NULL;
        }
        return vadisplay;
    } else {
        return vadisplay;
    }
}

static av_cold int yami_dec_init(AVCodecContext *avctx)
{
    YamiDecContext *s = (YamiDecContext *)avctx->priv_data;
    Decode_Status status;

    s->decoder = NULL;
    enum AVPixelFormat pix_fmts[4] =
        {
            AV_PIX_FMT_YAMI,
            AV_PIX_FMT_NV12,
            AV_PIX_FMT_YUV420P,
            AV_PIX_FMT_NONE
        };
    if (avctx->pix_fmt == AV_PIX_FMT_NONE) {
        int ret = ff_get_format(avctx, pix_fmts);
        if (ret < 0)
            return ret;

        avctx->pix_fmt      = (AVPixelFormat)ret;
    }
    VADisplay m_display = createVADisplay();
    if (!m_display) {
        av_log(avctx, AV_LOG_ERROR, "\nfail to create libyami h264 display\n");
        return -1;
    }
    av_log(avctx, AV_LOG_VERBOSE, "yami_dec_init\n");
    s->decoder = createVideoDecoder(YAMI_MIME_H264);
    if (!s->decoder) {
        av_log(avctx, AV_LOG_ERROR, "fail to create libyami h264 decoder\n");
        return -1;
    }

    NativeDisplay native_display;
    native_display.type = NATIVE_DISPLAY_VA;

    native_display.handle = (intptr_t)m_display;
    s->decoder->setNativeDisplay(&native_display);

    VideoConfigBuffer config_buffer;
    memset(&config_buffer, 0, sizeof(VideoConfigBuffer));
    if (avctx->extradata && avctx->extradata_size && avctx->extradata[0] == 1) {
        config_buffer.data = avctx->extradata;
        config_buffer.size = avctx->extradata_size;
    }
    config_buffer.profile = VAProfileNone;
    status = s->decoder->start(&config_buffer);
    if (status != DECODE_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "yami h264 decoder fail to start\n");
        return -1;
    }

    /* XXX: now used this as default, maybe change this with private options */
    s->output_type = VIDEO_DATA_MEMORY_TYPE_RAW_POINTER;

    s->in_queue = new std::deque<VideoDecodeBuffer*>;
    pthread_mutex_init(&s->mutex_, NULL);
    pthread_mutex_init(&s->in_mutex, NULL);
    pthread_cond_init(&s->in_cond, NULL);
    s->decode_status = DECODE_THREAD_NOT_INIT;

    s->decode_count = 0;
    s->decode_count_yami = 0;
    s->render_count = 0;

    return 0;
}

static void *decodeThread(void *arg)
{
    AVCodecContext *avctx = (AVCodecContext *)arg;
    YamiDecContext *s = (YamiDecContext *)avctx->priv_data;

    while (1) {
        VideoDecodeBuffer *in_buffer = NULL;
        // deque one input buffer
        DECODE_TRACE("decode thread runs one cycle start ... \n");
        pthread_mutex_lock(&s->in_mutex);
        if (s->in_queue->empty()) {
            if (s->decode_status == DECODE_THREAD_GOT_EOS) {
                pthread_mutex_unlock(&s->in_mutex);
                break;
            } else {
                DECODE_TRACE("decode thread wait because s->in_queue is empty\n");
                pthread_cond_wait(&s->in_cond, &s->in_mutex); // wait if no todo frame is available
            }
        }

        if (s->in_queue->empty()) { // may wake up from EOS/Close
            pthread_mutex_unlock(&s->in_mutex);
            continue;
        }

        DECODE_TRACE("s->in_queue->size()=%ld\n", s->in_queue->size());
        in_buffer = s->in_queue->front();

        pthread_mutex_unlock(&s->in_mutex);

        // decode one input buffer
        DECODE_TRACE("try to process one input buffer, in_buffer->data=%p, in_buffer->size=%d\n", in_buffer->data, in_buffer->size);
        Decode_Status status = s->decoder->decode(in_buffer);
        DECODE_TRACE("decode() status=%d, decode_count_yami=%d render_count %d\n", status, s->decode_count_yami, s->render_count);

        if (DECODE_FORMAT_CHANGE == status) {
            s->format_info = s->decoder->getFormatInfo();
            DECODE_TRACE("decode format change %dx%d\n", s->format_info->width,s->format_info->height);
            // resend the buffer to decoder
            status = s->decoder->decode(in_buffer);
            DECODE_TRACE("decode() status=%d\n",status);
            avctx->width = s->format_info->width;
            avctx->height = s->format_info->height;

        }
        if (status < 0){
            av_log(avctx, AV_LOG_ERROR, "decode error %d\n", status);
        }
        s->decode_count_yami++;
        s->in_queue->pop_front();
        av_free(in_buffer->data);
        av_free(in_buffer);
    }

    DECODE_TRACE("decode thread exit\n");
    pthread_mutex_lock(&s->mutex_);
    s->decode_status = DECODE_THREAD_EXIT;
    pthread_mutex_unlock(&s->mutex_);
    return NULL;
}

static void yami_recycle_frame(void *opaque, uint8_t *data)
{
    AVCodecContext *avctx = (AVCodecContext *)opaque;
    YamiDecContext *s = (YamiDecContext *)avctx->priv_data;
    VideoFrameRawData *yami_frame = (VideoFrameRawData *)data;

    if (!s || !s->decoder || !yami_frame) // XXX, use shared pointer for s
        return;
    pthread_mutex_lock(&s->mutex_);
    s->decoder->renderDone(yami_frame);
    /* XXX: should I delete frame buffer?? */
    av_free(yami_frame);
    pthread_mutex_unlock(&s->mutex_);
    av_log(avctx, AV_LOG_DEBUG, "recycle previous frame: %p\n", yami_frame);
}

static int yami_dec_frame(AVCodecContext *avctx, void *data,
                          int *got_frame, AVPacket *avpkt)
{
    YamiDecContext *s = (YamiDecContext *)avctx->priv_data;
    if (!s || !s->decoder)
        return -1;
    VideoDecodeBuffer *in_buffer = NULL;
    Decode_Status status = RENDER_NO_AVAILABLE_FRAME;
    VideoFrameRawData *yami_frame = NULL;
    AVFrame *frame = (AVFrame *)data;

    av_log(avctx, AV_LOG_VERBOSE, "yami_dec_frame\n");

    // append avpkt to input buffer queue
    in_buffer = (VideoDecodeBuffer *)av_mallocz(sizeof(VideoDecodeBuffer));
    if (!in_buffer)
        return AVERROR(ENOMEM);
    /* avoid avpkt free and data is pointer */
    in_buffer->data = (uint8_t *)av_mallocz(avpkt->size);
    if (!in_buffer->data)
        return AVERROR(ENOMEM);
    memcpy(in_buffer->data, avpkt->data, avpkt->size);
    in_buffer->size = avpkt->size;
    in_buffer->timeStamp = avpkt->pts;

    if (avctx->extradata && avctx->extradata_size && avctx->extradata[0] == 1)
        in_buffer->flag |= IS_AVCC;

    while (s->decode_status < DECODE_THREAD_GOT_EOS) { // we need enque eos buffer more than once
        pthread_mutex_lock(&s->in_mutex);
        if (s->in_queue->size() < QUEUE_MAX_SIZE) {
            s->in_queue->push_back(in_buffer);
            av_log(avctx, AV_LOG_VERBOSE, "wakeup decode thread ...\n");
            pthread_cond_signal(&s->in_cond);
            pthread_mutex_unlock(&s->in_mutex);
            break;
        }
        pthread_mutex_unlock(&s->in_mutex);

        av_log(avctx, AV_LOG_DEBUG, "s->in_queue->size()=%ld, s->decode_count=%d, s->decode_count_yami=%d, too many buffer are under decoding, wait ...\n",
        s->in_queue->size(), s->decode_count, s->decode_count_yami);
        usleep(1000);
    };
    s->decode_count++;

    // decode thread status update
    pthread_mutex_lock(&s->mutex_);
    switch (s->decode_status) {
    case DECODE_THREAD_NOT_INIT:
    case DECODE_THREAD_EXIT:
        if (avpkt->data && avpkt->size) {
            s->decode_status = DECODE_THREAD_RUNING;
            pthread_create(&s->decode_thread_id, NULL, &decodeThread, avctx);
        }
        break;
    case DECODE_THREAD_RUNING:
        if (!avpkt->data || ! avpkt->size)
            s->decode_status = DECODE_THREAD_GOT_EOS; // call releaseLock for seek
        break;
    case DECODE_THREAD_GOT_EOS:
        if (s->in_queue->empty())
            s->decode_status = DECODE_THREAD_NOT_INIT;
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&s->mutex_);

    // get an output buffer from yami
    yami_frame = (VideoFrameRawData *)av_mallocz(sizeof(VideoFrameRawData));
    if (!yami_frame)
        return AVERROR(ENOMEM);
    if (avctx->pix_fmt == AV_PIX_FMT_YAMI)
        yami_frame->memoryType = VIDEO_DATA_MEMORY_TYPE_SURFACE_ID;
    else
        yami_frame->memoryType = VIDEO_DATA_MEMORY_TYPE_RAW_POINTER;
    /* FIXME */
    if (avctx->pix_fmt == AV_PIX_FMT_NV12)
        yami_frame->fourcc = VA_FOURCC_NV12;
    else
        yami_frame->fourcc = VA_FOURCC_I420;

    do {
        if (!s->format_info) {
            usleep(10000);
            continue;
        }

        status = s->decoder->getOutput(yami_frame, false);
        av_log(avctx, AV_LOG_DEBUG, "getoutput() status=%d\n", status);
        if (status == RENDER_SUCCESS) {
            break;
        }

        *got_frame = 0;
        av_free(yami_frame);
        return avpkt->size;
    } while (s->decode_status == DECODE_THREAD_RUNING);

    if (status != RENDER_SUCCESS) {
        av_log(avctx, AV_LOG_VERBOSE, "after processed EOS, return\n");
        return avpkt->size;
    }

    // process the output frame
    if (avctx->pix_fmt == AV_PIX_FMT_YAMI) {
        frame->pts = yami_frame->timeStamp;

        frame->width = avctx->width;
        frame->height = avctx->height;

        frame->format = AV_PIX_FMT_YAMI; /* FIXME */
        frame->extended_data = NULL;

        *(AVFrame *)data = *frame;
        ((AVFrame *)data)->extended_data = ((AVFrame *)data)->data;
        /* XXX: put the surface id to data[3] */
        frame->data[3] = reinterpret_cast<uint8_t *>(yami_frame);

        frame->buf[0] = av_buffer_create((uint8_t *)frame->data[3],
                                         sizeof(VideoFrameRawData),
                                         yami_recycle_frame, avctx, 0);
    } else {
        int src_linesize[4];
        uint8_t *src_data[4];
        if (avctx->pix_fmt == AV_PIX_FMT_YUV420P) {
            src_linesize[0] = yami_frame->pitch[0];
            src_linesize[1] = yami_frame->pitch[1];
            src_linesize[2] = yami_frame->pitch[2];
            uint8_t* yami_data = reinterpret_cast<uint8_t*>(yami_frame->handle);
            src_data[0] = yami_data + yami_frame->offset[0];
            src_data[1] = yami_data + yami_frame->offset[1];
            src_data[2] = yami_data + yami_frame->offset[2];

            frame->data[0] = reinterpret_cast<uint8_t*>(src_data[0]);
            frame->data[1] = reinterpret_cast<uint8_t*>(src_data[1]);
            frame->data[2] = reinterpret_cast<uint8_t*>(src_data[2]);
            frame->linesize[0] = src_linesize[0];
            frame->linesize[1] = src_linesize[1];
            frame->linesize[2] = src_linesize[2];
        } else {
            src_linesize[0] = yami_frame->pitch[0];
            src_linesize[1] = yami_frame->pitch[1];
            src_linesize[2] = yami_frame->pitch[2];
            uint8_t* yami_data = reinterpret_cast<uint8_t*>(yami_frame->handle);
            src_data[0] = yami_data + yami_frame->offset[0];
            src_data[1] = yami_data + yami_frame->offset[1];

            frame->data[0] = reinterpret_cast<uint8_t*>(src_data[0]);
            frame->data[1] = reinterpret_cast<uint8_t*>(src_data[1]);
            frame->linesize[0] = src_linesize[0];
            frame->linesize[1] = src_linesize[1];
        }

        frame->pkt_pts = AV_NOPTS_VALUE;
        frame->pkt_dts = yami_frame->timeStamp;
        frame->pts = AV_NOPTS_VALUE;

        frame->width = avctx->width;
        frame->height = avctx->height;

        frame->format = avctx->pix_fmt;
        frame->extended_data = NULL;

        frame->extended_data = frame->data;


        frame->buf[0] = av_buffer_create((uint8_t *) yami_frame,
                                                     sizeof(VideoFrameRawData),
                                                     yami_recycle_frame, avctx, 0);

    }

    *got_frame = 1;

    s->render_count++;
    assert(data->buf[0] || !*got_frame);
    av_log(avctx, AV_LOG_VERBOSE,
           "decode_count_yami=%d, decode_count=%d, render_count=%d\n",
           s->decode_count_yami, s->decode_count, s->render_count);

    return avpkt->size;
}

static av_cold int yami_dec_close(AVCodecContext *avctx)
{
    YamiDecContext *s = (YamiDecContext *)avctx->priv_data;

    pthread_mutex_lock(&s->mutex_);
    while (s->decode_status != DECODE_THREAD_EXIT) {
        // potential race condition on s->decode_status
        s->decode_status = DECODE_THREAD_GOT_EOS;
        pthread_mutex_unlock(&s->mutex_);
        pthread_cond_signal(&s->in_cond);
        usleep(10000);
        pthread_mutex_lock(&s->mutex_);
    }
    pthread_mutex_unlock(&s->mutex_);

    if (s->decoder) {
        s->decoder->stop();
        releaseVideoDecoder(s->decoder);
        s->decoder = NULL;
    }

    pthread_mutex_destroy(&s->in_mutex);
    pthread_cond_destroy(&s->in_cond);
    while (!s->in_queue->empty()) {
        VideoDecodeBuffer *in_buffer = s->in_queue->front();
        s->in_queue->pop_front();
        av_free(in_buffer->data);
        av_free(in_buffer);
    }
    delete s->in_queue;
    av_log(avctx, AV_LOG_VERBOSE, "yami_close\n");

    return 0;
}

AVCodec ff_libyami_h264_decoder = {
    .name                  = "libyami_h264",
    .long_name             = NULL_IF_CONFIG_SMALL("libyami H.264 decoder"),
    .type                  = AVMEDIA_TYPE_VIDEO,
    .id                    = AV_CODEC_ID_H264,
    .capabilities          = CODEC_CAP_DELAY, // it is not necessary to support multi-threads
    .supported_framerates  = NULL,
    .pix_fmts              = (const enum AVPixelFormat[]) { AV_PIX_FMT_YAMI ,
                                                            AV_PIX_FMT_NV12 ,
                                                            AV_PIX_FMT_YUV420P,
                                                            AV_PIX_FMT_NONE},
    .supported_samplerates = NULL,
    .sample_fmts           = NULL,
    .channel_layouts       = NULL,
#if FF_API_LOWRES
    .max_lowres            = 0,
#endif
    .priv_class            = NULL,
    .profiles              = NULL,
    .priv_data_size        = sizeof(YamiDecContext),
    .next                  = NULL,
    .init_thread_copy      = NULL,
    .update_thread_context = NULL,
    .defaults              = NULL,
    .init_static_data      = NULL,
    .init                  = yami_dec_init,
    .encode_sub            = NULL,
    .encode2               = NULL,
    .decode                = yami_dec_frame,
    .close                 = yami_dec_close,
    .flush                 = NULL, // TODO, add it
    .caps_internal         = FF_CODEC_CAP_SETS_PKT_DTS
};

struct YamiEncContext {
    AVCodecContext *avctx;

    pthread_mutex_t mutex_; // mutex for decoder->getOutput() and YamiContext itself update (decode_status, etc)
    IVideoEncoder *encoder;
    VideoEncOutputBuffer outputBuffer;

    pthread_t encode_thread_id;
    std::deque<AVFrame *> *in_queue;
    pthread_mutex_t in_mutex; // mutex for in_queue
    pthread_cond_t in_cond;   // decode thread condition wait
    EncodeThreadStatus encode_status;

    uint8_t *m_buffer;
    uint32_t m_frameSize;
    /***video param*****/
    uint32_t cqp;
    uint32_t frame_rate;
    char *rcmod;
    uint32_t gop;
    char *level;
    char *profile;
    /*******************/

    uint32_t maxOutSize;

    // debug use
    int encode_count;
    int encode_count_yami;
    int render_count;
};

#define OFFSET(x) offsetof(YamiEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "profile",       "Set profile restrictions ", OFFSET(profile),       AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE},
    { "level",         "Specify level (as defined by Annex A)", OFFSET(level), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, VE},
    { "rcmode",        "rate control mode", OFFSET(rcmod), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, VE},
    { "qp",            "Constant quantization parameter rate control method",OFFSET(cqp),        AV_OPT_TYPE_INT,    { .i64 = 26 }, 1, 52, VE },
//    { "cavlc",            NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 },  INT_MIN, INT_MAX, VE, "coder" },
//    { "cabac",            NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 },  INT_MIN, INT_MAX, VE, "coder" },
    { NULL },
};

static const AVClass yami_enc_264_class = {
    .class_name = "libyami_h264",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVCodecDefault yami_enc_264_defaults[] = {
    { (uint8_t *)("b"),                (uint8_t *)("2M") },
    { (uint8_t *)("g"),                (uint8_t *)("30") },
    { NULL },
};


static bool
getPlaneResolution(uint32_t fourcc, uint32_t pixelWidth, uint32_t pixelHeight, uint32_t byteWidth[3], uint32_t byteHeight[3],  uint32_t& planes)
{
    int w = pixelWidth;
    int h = pixelHeight;
    uint32_t *width = byteWidth;
    uint32_t *height = byteHeight;

    switch (fourcc) {
    case VA_FOURCC_NV12:
    case VA_FOURCC_I420:
    case VA_FOURCC_YV12:
        width[0] = w;
        height[0] = h;
        if (fourcc == VA_FOURCC_NV12) {
            width[1]  = w + (w & 1);
            height[1] = (h + 1) >> 1;
            planes = 2;
        } else {
            width[1] = width[2] = (w + 1) >> 1;
            height[1] = height[2] = (h + 1) >> 1;
            planes = 3;
        }
        break;
    case VA_FOURCC_YUY2:
    case VA_FOURCC_UYVY:
        width[0] = w * 2;
        height[0] = h;
        planes = 1;
        break;
    case VA_FOURCC_RGBX:
    case VA_FOURCC_RGBA:
    case VA_FOURCC_BGRX:
    case VA_FOURCC_BGRA:
        width[0] = w * 4;
        height[0] = h;
        planes = 1;
        break;
    default:
        assert(0 && "do not support this format");
        planes = 0;
        return false;
    }
    return true;
}

static bool
fillFrameRawData(VideoFrameRawData *frame, uint32_t fourcc, uint32_t width, uint32_t height, uint8_t *data)
{
    memset(frame, 0, sizeof(*frame));
    uint32_t planes;
    uint32_t w[3], h[3];

    if (!getPlaneResolution(fourcc, width, height, w, h, planes))
        return false;
    frame->fourcc = fourcc;
    frame->width  = width;
    frame->height = height;
    frame->handle = reinterpret_cast<intptr_t>(data);

    frame->memoryType = VIDEO_DATA_MEMORY_TYPE_RAW_POINTER;
    uint32_t offset = 0;
    for (uint32_t i = 0; i < planes; i++) {
        frame->pitch[i] = w[i];
        frame->offset[i] = offset;//reinterpret_cast<intptr_t>(data->data[i]);
        offset += w[i] * h[i];
    }
    return true;
}

static void *encodeThread(void *arg)
{
    AVCodecContext *avctx = (AVCodecContext *)arg;
    YamiEncContext *s = (YamiEncContext *)avctx->priv_data;

    while (1) {
        AVFrame *frame;
        VideoFrameRawData *in_buffer = NULL;
        // deque one input buffer
        ENCODE_TRACE("encode thread runs one cycle start ... \n");
        pthread_mutex_lock(&s->in_mutex);
        if (s->in_queue->empty()) {
            if (s->encode_status == ENCODE_THREAD_GOT_EOS) {
                pthread_mutex_unlock(&s->in_mutex);
                break;
            } else {
                ENCODE_TRACE("encode thread wait because s->in_queue is empty\n");
                pthread_cond_wait(&s->in_cond, &s->in_mutex); // wait if no todo frame is available
            }
        }

        if (s->in_queue->empty()) { // may wake up from EOS/Close
            pthread_mutex_unlock(&s->in_mutex);
            continue;
        }

        ENCODE_TRACE("s->in_queue->size()=%ld\n", s->in_queue->size());
        frame = s->in_queue->front();
        pthread_mutex_unlock(&s->in_mutex);

        // encode one input in_buffer
        Decode_Status status;
        if (frame->format != AV_PIX_FMT_YAMI) { /* non zero-copy mode */
            uint32_t src_linesize[4];
            const uint8_t *src_data[4];

            uint8_t *dst_data[4];

            in_buffer = (VideoFrameRawData *)av_malloc(sizeof(VideoFrameRawData));

            in_buffer->width = avctx->width;
            in_buffer->height = avctx->height;
#if 0
            /* FIXME there is risk here, I need another yami interface */
            if (avctx->pix_fmt == AV_PIX_FMT_YUV420P){
                in_buffer->pitch[0] = frame->linesize[0];
                in_buffer->pitch[1] = frame->linesize[1];
                in_buffer->pitch[2] = frame->linesize[2];

                in_buffer->handle = reinterpret_cast<intptr_t>(frame->data[2]);
                in_buffer->offset[0] = reinterpret_cast<intptr_t>(frame->data[0]) - in_buffer->handle;
                in_buffer->offset[1] = reinterpret_cast<intptr_t>(frame->data[1]) - in_buffer->handle;
                in_buffer->offset[2] = reinterpret_cast<intptr_t>(frame->data[2]) - in_buffer->handle;
                in_buffer->fourcc = VA_FOURCC_I420;
            } else {
                src_linesize[0] = in_buffer->pitch[0] = frame->linesize[0];
                src_linesize[1] = in_buffer->pitch[1] = frame->linesize[1];
                in_buffer->handle = reinterpret_cast<intptr_t>(frame->data[0]);
                in_buffer->offset[0] = reinterpret_cast<intptr_t>(frame->data[0]) - in_buffer->handle;
                in_buffer->offset[1] = reinterpret_cast<intptr_t>(frame->data[1]) - in_buffer->handle;
                in_buffer->fourcc = VA_FOURCC_NV12;

            }
#else
            src_linesize[0] = in_buffer->pitch[0] = frame->linesize[0];
            src_linesize[1] = in_buffer->pitch[1] = frame->linesize[1];
            src_linesize[2] = in_buffer->pitch[2] = frame->linesize[2];
            uint8_t *yamidata = reinterpret_cast<uint8_t *>(s->m_buffer);

            dst_data[0] = yamidata;
            dst_data[1] = yamidata + avctx->width * avctx->height;
            dst_data[2] = dst_data[1] + avctx->width * avctx->height / 4;

            src_data[0] = frame->data[0];
            src_data[1] = frame->data[1];
            src_data[2] = frame->data[2];

            av_image_copy(dst_data, (int *)in_buffer->pitch, src_data,
                          (int *)src_linesize, avctx->pix_fmt, avctx->width,
                          avctx->height);

            if (avctx->pix_fmt == AV_PIX_FMT_YUV420P)
                fillFrameRawData(in_buffer, VA_FOURCC_I420, avctx->width,
                                 avctx->height, s->m_buffer);
            else if (avctx->pix_fmt == AV_PIX_FMT_NV12)
                fillFrameRawData(in_buffer, VA_FOURCC_NV12, avctx->width,
                                 avctx->height, s->m_buffer);
#endif
            /* handle decoder busy case */
            do {
                 status = s->encoder->encode(in_buffer);
            } while (status == ENCODE_IS_BUSY);

            ENCODE_TRACE("encode() status=%d, encode_count_yami=%d\n", status, s->encode_count_yami);
            av_free(in_buffer);
        } else { /* zero-copy mode */
            SharedPtr < VideoFrame > yami_frame;
            in_buffer = (VideoFrameRawData *)frame->data[3];

            if (frame) {
                yami_frame.reset(new VideoFrame);
                yami_frame->surface = (intptr_t)in_buffer->internalID; /* XXX: get decoded surface */
                yami_frame->timeStamp = in_buffer->timeStamp;
                yami_frame->crop.x = 0;
                yami_frame->crop.y = 0;
                yami_frame->crop.width = avctx->width;
                yami_frame->crop.height = avctx->height;
                yami_frame->flags = 0;
            }

            /* handle decoder busy case */
            do {
                 status = s->encoder->encode(yami_frame);
            } while (status == ENCODE_IS_BUSY);

            ENCODE_TRACE("encode() status=%d, encode_count_yami=%d\n", status, s->encode_count_yami);
        }

        if (status < 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "encode error %d frame %d\n", status , s->encode_count_yami);
        }
        s->encode_count_yami++;
        s->in_queue->pop_front();
        av_frame_free(&frame);
    }

    ENCODE_TRACE("encode thread exit\n");
    pthread_mutex_lock(&s->mutex_);
    s->encode_status = ENCODE_THREAD_EXIT;
    pthread_mutex_unlock(&s->mutex_);

    return NULL;
}

static bool
createOutputBuffer(VideoEncOutputBuffer *outputBuffer, int maxOutSize)
{
    outputBuffer->data = static_cast<uint8_t *>(malloc(maxOutSize));
    if (!outputBuffer->data)
        return false;
    outputBuffer->bufferSize = maxOutSize;
    outputBuffer->format = OUTPUT_EVERYTHING;

    return true;
}

static bool destroyOutputBuffer(VideoEncOutputBuffer *outputBuffer)
{
    if (outputBuffer->data)
        free(outputBuffer->data);

    return true;
}

static av_cold int yami_enc_init(AVCodecContext *avctx)
{
    YamiEncContext *s = (YamiEncContext *) avctx->priv_data;
    Decode_Status status;

    enum AVPixelFormat pix_fmts[4] =
        {
            AV_PIX_FMT_YAMI,
            AV_PIX_FMT_NV12,
            AV_PIX_FMT_YUV420P,
            AV_PIX_FMT_NONE
        };

    if (avctx->pix_fmt == AV_PIX_FMT_NONE) {
        int ret = ff_get_format(avctx, pix_fmts);
        if (ret < 0)
            return ret;

        avctx->pix_fmt      = (AVPixelFormat)ret;
    }

    av_log(avctx, AV_LOG_VERBOSE, "yami_init h264 encoder\n");
    s->encoder = createVideoEncoder(YAMI_MIME_H264);
    if (!s->encoder) {
        av_log(avctx, AV_LOG_ERROR, "fail to create libyami h264 encoder\n");
        return -1;
    }

    NativeDisplay native_display;
    native_display.type = NATIVE_DISPLAY_VA;
    VADisplay m_display = createVADisplay();
    native_display.handle = (intptr_t)m_display;
    s->encoder->setNativeDisplay(&native_display);

    // configure encoding parameters
    VideoParamsCommon encVideoParams;
    encVideoParams.size = sizeof(VideoParamsCommon);
    s->encoder->getParameters(VideoParamsTypeCommon, &encVideoParams);

    encVideoParams.resolution.width  = avctx->width;
    encVideoParams.resolution.height = avctx->height;

    // frame rate parameters.
    if (avctx->framerate.den > 0 && avctx->framerate.num > 0) {
        encVideoParams.frameRate.frameRateDenom = avctx->framerate.den;
        encVideoParams.frameRate.frameRateNum = avctx->framerate.num;
    } else {
        encVideoParams.frameRate.frameRateNum = avctx->time_base.den;
        encVideoParams.frameRate.frameRateDenom = avctx->time_base.num;
    }

    // picture type and bitrate
    encVideoParams.intraPeriod = av_clip(avctx->gop_size, 1, 250);
    encVideoParams.ipPeriod = !avctx->max_b_frames ? 1 : 3;
    if (s->rcmod){
        if (!strcmp(s->rcmod, "CQP"))
            encVideoParams.rcMode = RATE_CONTROL_CQP;
        else if (!strcmp(s->rcmod, "VBR")){
            encVideoParams.rcMode = RATE_CONTROL_VBR;
            encVideoParams.rcParams.bitRate = avctx->bit_rate;
        } else {
            encVideoParams.rcMode = RATE_CONTROL_CBR;
            encVideoParams.rcParams.bitRate = avctx->bit_rate;
        }
    } else {
        encVideoParams.rcMode = RATE_CONTROL_CQP;
    }

    encVideoParams.rcParams.initQP = av_clip(s->cqp,1,52);

    if (s->level){
        encVideoParams.level = atoi(s->level);
    } else {
        encVideoParams.level = 40;
    }
    /*libyami only support h264 main now*/
//    if (s->profile){
//        if (!strcmp(s->profile , "high"))
//            encVideoParams.profile = VAProfileH264High;
//        else if(!strcmp(s->profile , "main")){
//            encVideoParams.profile = VAProfileH264Main;
//        } else {
//            encVideoParams.profile = VAProfileH264Baseline;
//        }
//    } else {
//        encVideoParams.profile = VAProfileH264High;
//    }
    // s->encoder->setEncoderParameters(&encVideoParams);
    encVideoParams.size = sizeof(VideoParamsCommon);
    s->encoder->setParameters(VideoParamsTypeCommon, &encVideoParams);

    VideoConfigAVCStreamFormat streamFormat;
    streamFormat.size = sizeof(VideoConfigAVCStreamFormat);
    streamFormat.streamFormat = AVC_STREAM_FORMAT_ANNEXB;
    s->encoder->setParameters(VideoConfigTypeAVCStreamFormat, &streamFormat);

    status = s->encoder->start();
    assert(status == ENCODE_SUCCESS);

    // init output buffer
    s->encoder->getMaxOutSize(&(s->maxOutSize));

    if (!createOutputBuffer(&s->outputBuffer, s->maxOutSize)) {
        av_log(avctx, AV_LOG_ERROR,  "fail to create output\n");
        return -1;
    }
    s->m_frameSize = avctx->width * avctx->height * 3 / 2;
    s->m_buffer = static_cast<uint8_t *>(av_mallocz(s->m_frameSize));

    s->in_queue = new std::deque<AVFrame *>;
    pthread_mutex_init(&s->mutex_, NULL);
    pthread_mutex_init(&s->in_mutex, NULL);
    pthread_cond_init(&s->in_cond, NULL);
    s->encode_status = ENCODE_THREAD_NOT_INIT;

    s->encode_count = 0;
    s->encode_count_yami = 0;
    s->render_count = 0;

    return 0;
}

static int yami_enc_frame(AVCodecContext *avctx, AVPacket *pkt,
                          const AVFrame *frame, int *got_packet)
{
    YamiEncContext *s = (YamiEncContext *)avctx->priv_data;
    Encode_Status status;
    int ret;

    if(!s->encoder)
        return -1;
    if (frame) {
        AVFrame *qframe = av_frame_alloc();
        if (!qframe) {
            return AVERROR(ENOMEM);
        }

        /* av_frame_ref the src frame and av_frame_unref in encode thread */
        ret = av_frame_ref(qframe, frame);
        if (ret < 0)
            return ret;

        while (s->encode_status < ENCODE_THREAD_GOT_EOS) { // we need enque eos buffer more than once
            pthread_mutex_lock(&s->in_mutex);
            if (s->in_queue->size() < QUEUE_MIN_SIZE) {
                s->in_queue->push_back(qframe);
                av_log(avctx, AV_LOG_VERBOSE, "wakeup encode thread ...\n");
                pthread_cond_signal(&s->in_cond);
                pthread_mutex_unlock(&s->in_mutex);
                break;
            }
            pthread_mutex_unlock(&s->in_mutex);

            av_log(avctx, AV_LOG_DEBUG,
                   "s->in_queue->size()=%ld, s->encode_count=%d, s->encode_count_yami=%d, too many buffer are under encoding, wait ...\n",
                   s->in_queue->size(), s->encode_count, s->encode_count_yami);
            usleep(1000);
        };
        s->encode_count++;
    }

    // decode thread status update
    pthread_mutex_lock(&s->mutex_);
    switch (s->encode_status) {
    case ENCODE_THREAD_NOT_INIT:
    case ENCODE_THREAD_EXIT:
        if (frame) {
            s->encode_status = ENCODE_THREAD_RUNING;
            pthread_create(&s->encode_thread_id, NULL, &encodeThread, avctx);
        }
        break;
    case ENCODE_THREAD_RUNING:
        if (!frame) {
            s->encode_status = ENCODE_THREAD_GOT_EOS; // call releaseLock for seek
        }
        break;
    case ENCODE_THREAD_GOT_EOS:
        if (s->in_queue->empty())
            s->encode_status = ENCODE_THREAD_NOT_INIT;
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&s->mutex_);

    do {
        status = s->encoder->getOutput(&s->outputBuffer, true);
    } while (!frame && status != ENCODE_SUCCESS && s->in_queue->size() > 0);
    if (status != ENCODE_SUCCESS)
        return 0;

    s->render_count++;

    if ((ret = ff_alloc_packet2(avctx, pkt, s->outputBuffer.dataSize, 0)) < 0)
        return ret;
    void *p = pkt->data;
    memcpy(p, s->outputBuffer.data, s->outputBuffer.dataSize);
    *got_packet = 1;

    return 0;
}

static av_cold int yami_enc_close(AVCodecContext *avctx)
{
    YamiEncContext *s = (YamiEncContext *)avctx->priv_data;

    destroyOutputBuffer(&s->outputBuffer);
    pthread_mutex_lock(&s->mutex_);
    while (s->encode_status == ENCODE_THREAD_RUNING) {
        // potential race condition on s->encode_status
        s->encode_status = ENCODE_THREAD_GOT_EOS;
        pthread_mutex_unlock(&s->mutex_);
        pthread_cond_signal(&s->in_cond);
        usleep(10000);
        pthread_mutex_lock(&s->mutex_);
    }
    pthread_mutex_unlock(&s->mutex_);

    if (s->encoder) {
        s->encoder->stop();
        releaseVideoEncoder(s->encoder);
        s->encoder = NULL;
    }
    pthread_mutex_destroy(&s->in_mutex);
    pthread_cond_destroy(&s->in_cond);
    while (!s->in_queue->empty()) {
        AVFrame *in_buffer = s->in_queue->front();
        s->in_queue->pop_front();
        av_frame_free(&in_buffer);
    }
    delete s->in_queue;

    av_free(s->m_buffer);
    s->m_frameSize = 0;

    av_log(avctx, AV_LOG_VERBOSE, "yami_enc_close\n");

    return 0;
}

AVCodec ff_libyami_h264_encoder = {
    .name                  = "libyami_h264",
    .long_name             = NULL_IF_CONFIG_SMALL("libyami H.264 encoder"),
    .type                  = AVMEDIA_TYPE_VIDEO,
    .id                    = AV_CODEC_ID_H264,
    .capabilities          = CODEC_CAP_DELAY, // it is not necessary to support multi-threads
    .supported_framerates  = NULL,
    .pix_fmts              = (const enum AVPixelFormat[]) { AV_PIX_FMT_YAMI,
                                                            AV_PIX_FMT_NV12,
                                                            AV_PIX_FMT_YUV420P,
                                                            AV_PIX_FMT_NONE},
    .supported_samplerates = NULL,
    .sample_fmts           = NULL,
    .channel_layouts       = NULL,
#if FF_API_LOWRES
    .max_lowres            = 0,
#endif
    .priv_class            = &yami_enc_264_class,
    .profiles              = NULL,
    .priv_data_size        = sizeof(YamiEncContext),
    .next                  = NULL,
    .init_thread_copy      = NULL,
    .update_thread_context = NULL,
    .defaults              = yami_enc_264_defaults,
    .init_static_data      = NULL,
    .init                  = yami_enc_init,
    .encode_sub            = NULL,
    .encode2               = yami_enc_frame,
    .decode                = NULL,
    .close                 = yami_enc_close,
    .flush                 = NULL, // TODO, add it
};