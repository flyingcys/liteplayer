/*
 * Copyright 2019-2020 LUOYUN <sysu.zqlong@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "msgutils/os_logger.h"
#include "esp_adf/audio_element.h"
#include "esp_adf/audio_common.h"
#include "audio_extractor/wav_extractor.h"
#include "audio_decoder/wav_decoder.h"
#include "dr_libs/dr_wav.h"

#define TAG "[liteplayer]WAV_DEC"

#define WAV_DECODER_INPUT_TIMEOUT_MAX  200

#define WAV_MAX_NCHANS                 (2)
#define WAV_MAX_NSAMP                  (128)
//For IEEE 64-bit floating point: MAX_NSAMP*MAX_NCHAN*sizeof(uint64_t)
#define WAV_DECODER_INPUT_BUFFER_SIZE  (WAV_MAX_NSAMP*WAV_MAX_NCHANS*sizeof(uint64_t))
#define WAV_DECODER_OUTPUT_BUFFER_SIZE (WAV_MAX_NSAMP*WAV_MAX_NCHANS*sizeof(short))

struct wav_buf_in {
    char data[WAV_DECODER_INPUT_BUFFER_SIZE];
    int  bytes_want;     // bytes that want to read
    int  bytes_read;     // bytes that have read
    bool eof;            // if end of stream
};

struct wav_buf_out {
    char data[WAV_DECODER_OUTPUT_BUFFER_SIZE];
    int  bytes_remain;   // bytes that remained to write
    int  bytes_written;  // bytes that have written
};

struct wav_decoder {
    audio_element_handle_t  el;
    drwav                   drwav;
    bool                    drwav_inited;
    int                     drwav_offset;
    int                     block_align;
    struct wav_buf_in       buf_in;
    struct wav_buf_out      buf_out;
    bool                    parsed_header;
};
typedef struct wav_decoder *wav_decoder_handle_t;

static void *drwav_on_malloc(size_t sz, void* pUserData)
{
    return audio_malloc(sz);
}

static void *drwav_on_realloc(void *p, size_t sz, void *pUserData)
{
    return audio_realloc(p, sz);
}

static void drwav_on_free(void *p, void *pUserData)
{
    audio_free(p);
}

static drwav_allocation_callbacks drwav_allocation = {
    .pUserData = NULL,
    .onMalloc = drwav_on_malloc,
    .onRealloc = drwav_on_realloc,
    .onFree = drwav_on_free,
};

static size_t drwav_on_read(void *pUserData, void *pBufferOut, size_t bytesToRead)
{
    wav_decoder_handle_t wav = (wav_decoder_handle_t)pUserData;
    if (bytesToRead > wav->buf_in.bytes_read) {
        OS_LOGW(TAG, "Insufficient data: %d/%d", bytesToRead, wav->buf_in.bytes_read);
        bytesToRead = wav->buf_in.bytes_read;
    }
    if (bytesToRead > 0) {
        memcpy(pBufferOut, &wav->buf_in.data[wav->drwav_offset], bytesToRead);
        wav->drwav_offset += bytesToRead;
        wav->buf_in.bytes_read -= bytesToRead;
    }
    return bytesToRead;
}

static drwav_bool32 drwav_on_seek(void *pUserData, int offset, drwav_seek_origin origin)
{
    wav_decoder_handle_t wav = (wav_decoder_handle_t)pUserData;
    if (origin != drwav_seek_origin_current) {
        OS_LOGE(TAG, "Unsupported seek mode");
        return DRWAV_FALSE;
    }
    if (offset > wav->buf_in.bytes_read) {
        OS_LOGE(TAG, "Offset overflow: %d:%d", offset, wav->buf_in.bytes_read);
        return DRWAV_FALSE;
    }
    wav->drwav_offset += offset;
    wav->buf_in.bytes_read -= offset;
    return DRWAV_TRUE;
}

static int drwav_run(wav_decoder_handle_t wav)
{
    struct wav_buf_in *in = &wav->buf_in;
    int ret = AEL_IO_OK;

    if (in->eof && in->bytes_read < wav->block_align) {
        OS_LOGV(TAG, "WAV frame end");
        return AEL_IO_DONE;
    }

    if (in->bytes_read < wav->block_align) {
        if (in->bytes_read > 0) {
            OS_LOGD(TAG, "Refill data with remaining bytes:%d, offset:%d",
                    in->bytes_read, wav->drwav_offset);
            memmove(in->data, in->data+wav->drwav_offset, in->bytes_read);
            in->bytes_want = WAV_DECODER_INPUT_BUFFER_SIZE - in->bytes_read;
            wav->drwav_offset = in->bytes_read;
        }
        else {
            in->bytes_want = WAV_DECODER_INPUT_BUFFER_SIZE;
            wav->drwav_offset = 0;
        }
        ret = audio_element_input_chunk(wav->el, in->data+wav->drwav_offset, in->bytes_want);
        if (ret == in->bytes_want) {
            in->bytes_read += ret;
            wav->drwav_offset = 0;
        }
        else if (ret == AEL_IO_OK || ret == AEL_IO_DONE || ret == AEL_IO_ABORT) {
            in->eof = true;
            return AEL_IO_DONE;
        }
        else if (ret < 0) {
            OS_LOGW(TAG, "Read chunk error: %d/%d", ret, in->bytes_want);
            return ret;
        }
        else {
            in->eof = true;
            in->bytes_read += ret;
            wav->drwav_offset = 0;
        }
    }

    if (!wav->drwav_inited) {
        if (drwav_init_ex(&wav->drwav,
                          drwav_on_read, drwav_on_seek, NULL,
                          (void *)wav, NULL, DRWAV_SEQUENTIAL,
                          &drwav_allocation) != DRWAV_TRUE) {
            OS_LOGE(TAG, "Failed to init drwav decoder");
            return AEL_PROCESS_FAIL;
        }

        if (!wav->parsed_header) {
            audio_element_info_t info = {0};
            info.out_samplerate = wav->drwav.sampleRate;
            info.out_channels   = wav->drwav.channels;
            info.bits           = 16;
            audio_element_setinfo(wav->el, &info);
            audio_element_report_info(wav->el);

            OS_LOGV(TAG,"Found wav header: SR=%d, CH=%d", info.out_samplerate, info.out_channels);
            wav->parsed_header = true;
        }
        wav->block_align = wav->drwav.channels*wav->drwav.bitsPerSample/8;
        wav->drwav_inited = true;
    }

    drwav_int16 *out = (drwav_int16 *)(wav->buf_out.data);
    drwav_uint64 in_frames = (WAV_MAX_NSAMP > in->bytes_read/wav->block_align) ?
                             in->bytes_read/wav->block_align : WAV_MAX_NSAMP;
    drwav_uint64 out_frames = drwav_read_pcm_frames_s16le(&wav->drwav, in_frames, out);
    if (out_frames == 0) {
        OS_LOGW(TAG, "WAVDecode dummy data, AEL_IO_DONE");
        return AEL_IO_DONE;
    }
    OS_LOGV(TAG, "WAVDecode out_frames: %d", out_frames);
    wav->buf_out.bytes_remain = out_frames * wav->drwav.channels * sizeof(short);
    return 0;
}

static esp_err_t wav_decoder_destroy(audio_element_handle_t self)
{
    wav_decoder_handle_t wav = (wav_decoder_handle_t)audio_element_getdata(self);
    OS_LOGV(TAG, "Destroy wav decoder");
    audio_free(wav);
    return ESP_OK;
}

static esp_err_t wav_decoder_open(audio_element_handle_t self)
{
    OS_LOGV(TAG, "Open wav decoder");
    return ESP_OK;
}

static esp_err_t wav_decoder_close(audio_element_handle_t self)
{
    wav_decoder_handle_t wav = (wav_decoder_handle_t)audio_element_getdata(self);

    if (AEL_STATE_PAUSED != audio_element_get_state(self)) {
        if (wav->drwav_inited) {
            OS_LOGV(TAG, "Close drwav decoder");
            drwav_uninit(&wav->drwav);
            wav->drwav_inited = false;
        }
        wav->parsed_header = false;

        audio_element_info_t info = {0};
        audio_element_getinfo(self, &info);
        info.byte_pos = 0;
        info.total_bytes = 0;
        audio_element_setinfo(self, &info);
    }
    return ESP_OK;
}

static int wav_decoder_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    int byte_write = 0;
    int ret = AEL_IO_FAIL;
    wav_decoder_handle_t wav = (wav_decoder_handle_t)audio_element_getdata(self);

    if (wav->buf_out.bytes_remain > 0) {
        /* Output buffer have remain data */
        byte_write = audio_element_output(self,
                        wav->buf_out.data+wav->buf_out.bytes_written,
                        wav->buf_out.bytes_remain);
    } else {
        /* More data need to be wrote */
        ret = drwav_run(wav);
        if (ret < 0) {
            if (ret == AEL_IO_TIMEOUT) {
                OS_LOGW(TAG, "drwav_run AEL_IO_TIMEOUT");
            }
            else if (ret != AEL_IO_DONE) {
                OS_LOGE(TAG, "drwav_run failed:%d", ret);
            }
            return ret;
        }

        //OS_LOGV(TAG, "ret=%d, bytes_remain=%d", ret, wav->buf_out.bytes_remain);
        wav->buf_out.bytes_written = 0;
        byte_write = audio_element_output(self,
                        wav->buf_out.data,
                        wav->buf_out.bytes_remain);
    }

    if (byte_write > 0) {
        wav->buf_out.bytes_remain -= byte_write;
        wav->buf_out.bytes_written += byte_write;

        audio_element_info_t audio_info = {0};
        audio_element_getinfo(self, &audio_info);
        audio_info.byte_pos += byte_write;
        audio_element_setinfo(self, &audio_info);
    }

    return byte_write;
}

static esp_err_t wav_decoder_seek(audio_element_handle_t self, long long offset)
{
    //wav_decoder_handle_t wav = (wav_decoder_handle_t)audio_element_getdata(self);
    //wav->parsed_header = true;
    return ESP_OK;
}

audio_element_handle_t wav_decoder_init(struct wav_decoder_cfg *config)
{
    OS_LOGV(TAG, "Init wav decoder");

    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.destroy     = wav_decoder_destroy;
    cfg.open        = wav_decoder_open;
    cfg.close       = wav_decoder_close;
    cfg.process     = wav_decoder_process;
    cfg.seek        = wav_decoder_seek;
    cfg.buffer_len  = WAV_DECODER_BUFFER_SIZE;

    cfg.task_stack  = config->task_stack;
    cfg.task_prio   = config->task_prio;
    cfg.out_rb_size = config->out_rb_size;
    if (cfg.task_stack == 0)
        cfg.task_stack = WAV_DECODER_TASK_STACK;
    cfg.tag = "wav_dec";

    wav_decoder_handle_t wav = audio_calloc(1, sizeof(struct wav_decoder));
    if (wav == NULL)
        return NULL;

    audio_element_handle_t el = audio_element_init(&cfg);
    AUDIO_MEM_CHECK(TAG, el, goto wav_init_error);
    wav->el = el;
    wav->block_align = WAV_MAX_CHANNEL_COUNT * sizeof(uint64_t);
    audio_element_setdata(el, wav);

    audio_element_info_t info = { 0 };
    memset(&info, 0x0, sizeof(info));
    audio_element_setinfo(el, &info);

    audio_element_set_input_timeout(el, WAV_DECODER_INPUT_TIMEOUT_MAX);
    return el;

wav_init_error:
    audio_free(wav);
    return NULL;
}
