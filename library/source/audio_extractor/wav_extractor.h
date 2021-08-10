/*
 * Copyright (c) 2019-2021 Qinglong <sysu.zqlong@gmail.com>
 *
 * This file is part of Liteplayer
 * (see https://github.com/sepnic/liteplayer_priv).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 2.1 of the License, or
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

#ifndef _WAV_EXTRACTOR_H_
#define _WAV_EXTRACTOR_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//RIFF block
typedef struct {
    uint32_t ChunkID;           //chunk id;"RIFF",0X46464952
    uint32_t ChunkSize ;        //file length - 8
    uint32_t Format;            //WAVE,0X45564157
} __attribute__((packed)) ChunkRIFF;

//fmt block
typedef struct {
    uint32_t ChunkID;           //chunk id;"fmt ",0X20746D66
    uint32_t ChunkSize;         //Size of this fmt block (Not include ID and Size);16 or 18 or 40 bytes.
    uint16_t AudioFormat;       //Format;0X01:linear PCM;0X11:IMA ADPCM
    uint16_t NumOfChannels;     //Number of channel;1: 1 channel;2: 2 channels;
    uint32_t SampleRate;        //sample rate;0X1F40 = 8Khz
    uint32_t ByteRate;          //Byte rate;
    uint16_t BlockAlign;        //align with byte;
    uint16_t BitsPerSample;     //Bit lenght per sample point,4 ADPCM
//  uint16_t ByteExtraData;     //Exclude in linear PCM format(0~22)
} __attribute__((packed)) ChunkFMT;

//fact block
typedef struct {
    uint32_t ChunkID;           //chunk id;"fact",0X74636166;
    uint32_t ChunkSize;         //Size(Not include ID and Size);4 byte
    uint32_t NumOfSamples;      //number of sample
} __attribute__((packed)) ChunkFACT;

//LIST block
typedef struct {
    uint32_t ChunkID;            //chunk id 0X5453494C;
    uint32_t ChunkSize;          //Size
} __attribute__((packed)) ChunkLIST;

//PEAK block
typedef struct {
    uint32_t ChunkID;            //chunk id; 0X4B414550
    uint32_t ChunkSize;          //Size
} __attribute__((packed)) ChunkPeak;

//data block
typedef struct {
    uint32_t ChunkID;           //chunk id;"data",0X5453494C
    uint32_t ChunkSize;         //Size of data block(Not include ID and Size)
} __attribute__((packed)) ChunkDATA;

//wav block
typedef struct {
    ChunkRIFF riff;             //riff
    ChunkFMT fmt;               //fmt
    //ChunkFACT fact;             //fact,Exclude in linear PCM format
    ChunkDATA data;             //data
} __attribute__((packed)) wav_header_t;

//wav control struct
struct wav_info {
    uint16_t audioFormat;         //format;0X01,linear PCM;0X11 IMA ADPCM
    uint32_t sampleRate;          //sample rate
    uint16_t channels;            //Number of channel; 1: 1 channel; 2: 2 channels
    uint16_t bits;                //bit length 16bit,24bit,32bit
    uint32_t byteRate;            //byte sample rate
    uint16_t blockAlign;          //align
    uint32_t dataSize;            //size of data
    uint32_t dataOffset;          //offset of data
    uint8_t *header_buff;
    uint32_t header_size;
};

//wav that dr_wav.h supported:
/*Tested and supported internal formats include the following:
  - Unsigned 8-bit PCM
  - Signed 12-bit PCM
  - Signed 16-bit PCM
  - Signed 24-bit PCM
  - Signed 32-bit PCM
  - IEEE 32-bit floating point
  - IEEE 64-bit floating point
  - A-law and u-law
  - Microsoft ADPCM
  - IMA ADPCM (DVI, format code 0x11)
 */
enum wav_format {
    WAV_FMT_PCM = 0x0001,
    WAV_FMT_ADPCM = 0x0002,
    WAV_FMT_IEEE_FLOAT = 0x0003,
    WAV_FMT_DVI_ADPCM = 0x0011,
};

#define WAV_MAX_CHANNEL_COUNT 2

// Return the data size obtained
typedef int (*wav_fetch_cb)(char *buf, int wanted_size, long offset, void *fetch_priv);

void wav_build_header(wav_header_t *header, int samplerate, int bits, int channels, enum wav_format format, long datasize);

int wav_parse_header(char *buf, int buf_size, struct wav_info *info);

int wav_extractor(wav_fetch_cb fetch_cb, void *fetch_priv, struct wav_info *info);

#ifdef __cplusplus
}
#endif

#endif
