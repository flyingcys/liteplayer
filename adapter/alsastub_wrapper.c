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
#include <string.h>

#include "esp_adf/esp_log.h"
#include "esp_adf/audio_common.h"
#include "audio_extractor/wav_extractor.h"
#include "alsastub_wrapper.h"

#define TAG "alsawrapper"

#define ALSA_OUT_FILE "alsa_out.wav"

struct alsastub_priv {
    FILE *file;
    int samplerate;
    int channels;
    long offset;
};

alsa_handle_t alsastub_wrapper_open(int samplerate, int channels, void *alsa_priv)
{
    struct alsastub_priv *priv = audio_calloc(1, sizeof(struct alsastub_priv));
    if (priv == NULL)
        return NULL;

    priv->file = fopen(ALSA_OUT_FILE, "wb+");
    if (priv->file == NULL) {
        audio_free(priv);
        return NULL;
    }

    priv->samplerate = samplerate;
    priv->channels = channels;
    priv->offset = 0;

    wav_header_t header;
    memset(&header, 0x0, sizeof(wav_header_t));
    fwrite(&header, 1, sizeof(header), priv->file);

    return priv;
}

int alsastub_wrapper_write(alsa_handle_t handle, char *buffer, int size)
{
    struct alsastub_priv *priv = (struct alsastub_priv *)handle;
    size_t bytes_written = fwrite(buffer, 1, size, priv->file);
    if (bytes_written > 0)
        priv->offset += bytes_written;
    return bytes_written;
}

void alsastub_wrapper_close(alsa_handle_t handle)
{
    struct alsastub_priv *priv = (struct alsastub_priv *)handle;

    wav_header_t header;
    memset(&header, 0x0, sizeof(wav_header_t));
    wav_build_header(&header, priv->samplerate, 16, priv->channels, (int)priv->offset);
    fseek(priv->file, 0, SEEK_SET);
    fwrite(&header, 1, sizeof(header), priv->file);

    fflush(priv->file);
    fclose(priv->file);
    audio_free(priv);
}
