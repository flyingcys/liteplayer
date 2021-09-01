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

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "osal/os_thread.h"
#include "cutils/memory_helper.h"
#include "cutils/log_helper.h"
#include "liteplayer_manager.h"
#include "source_httpclient_wrapper.h"
#include "source_fatfs_wrapper.h"
#if defined(ENABLE_LINUX_ALSA)
#include "sink_alsa_wrapper.h"
#else
#include "sink_wave_wrapper.h"
#endif

#define TAG "playlist_demo"

#define PLAYLIST_FILE "liteplayermngr_demo.playlist"

#define PLAYLIST_DEMO_TASK_PRIO    (OS_THREAD_PRIO_NORMAL)
#define PLAYLIST_DEMO_TASK_STACK   (8192)
#define PLAYLIST_DEMO_THRESHOLD_MS (5000)

struct playlist_demo_priv {
    const char *url;
    liteplayermanager_handle_t mngr;
    enum liteplayer_state state;
    bool exit;
};

static int generate_playlist(const char *path)
{
    DIR *dir = NULL;
    if ((dir = opendir(path)) == NULL) {
        OS_LOGE(TAG, "Failed to open dir[%s]", path);
        return -1;
    } else {
        struct dirent *entry;
        char buffer[512];
        FILE *file = fopen(PLAYLIST_FILE, "wb+");
        if (file == NULL) {
            OS_LOGE(TAG, "Failed to open playlist file");
            closedir(dir);
            return -1;
        }

        while ((entry = readdir(dir)) != NULL) {
            OS_LOGD(TAG, "-->d_name=[%s], d_type=[%d]", entry->d_name, entry->d_type);
            if (entry->d_type == DT_REG &&
                (strstr(entry->d_name, ".mp3") != NULL ||
                 strstr(entry->d_name, ".m4a") != NULL ||
                 strstr(entry->d_name, ".wav") != NULL)) {
                snprintf(buffer, sizeof(buffer), "%s/%s\n", path, entry->d_name);
                fwrite(buffer, 1, strlen(buffer), file);
            }
        }
        buffer[0] = '\n';
        fwrite(buffer, 1, 1, file);

        fflush(file);
        fclose(file);
        closedir(dir);
        return 0;
    }

    return -1;
}

static int playlist_demo_state_callback(enum liteplayer_state state, int errcode, void *priv)
{
    struct playlist_demo_priv *demo = (struct playlist_demo_priv *)priv;
    bool state_sync = true;

    switch (state) {
    case LITEPLAYER_IDLE:
        OS_LOGI(TAG, "-->LITEPLAYER_IDLE");
        break;
    case LITEPLAYER_INITED:
        OS_LOGI(TAG, "-->LITEPLAYER_INITED");
        break;
    case LITEPLAYER_PREPARED:
        OS_LOGI(TAG, "-->LITEPLAYER_PREPARED");
        break;
    case LITEPLAYER_STARTED:
        OS_LOGI(TAG, "-->LITEPLAYER_STARTED");
        break;
    case LITEPLAYER_PAUSED:
        OS_LOGI(TAG, "-->LITEPLAYER_PAUSED");
        break;
    case LITEPLAYER_SEEKCOMPLETED:
        OS_LOGI(TAG, "-->LITEPLAYER_SEEKCOMPLETED");
        break;
    case LITEPLAYER_CACHECOMPLETED:
        OS_LOGI(TAG, "-->LITEPLAYER_CACHECOMPLETED");
        state_sync = false;
        break;
    case LITEPLAYER_NEARLYCOMPLETED:
        OS_LOGI(TAG, "-->LITEPLAYER_NEARLYCOMPLETED");
        state_sync = false;
        break;
    case LITEPLAYER_COMPLETED:
        OS_LOGI(TAG, "-->LITEPLAYER_COMPLETED");
        break;
    case LITEPLAYER_STOPPED:
        OS_LOGI(TAG, "-->LITEPLAYER_STOPPED");
        break;
    case LITEPLAYER_ERROR:
        OS_LOGE(TAG, "-->LITEPLAYER_ERROR: %d", errcode);
        break;
    default:
        OS_LOGE(TAG, "-->LITEPLAYER_UNKNOWN: %d", state);
        state_sync = false;
        break;
    }

    if (state_sync)
        demo->state = state;
    return 0;
}

static void *playlist_demo_thread(void *arg)
{
    struct playlist_demo_priv *demo = (struct playlist_demo_priv *)arg;

    demo->mngr = liteplayermanager_create();
    if (demo->mngr == NULL)
        return NULL;

    liteplayermanager_register_state_listener(demo->mngr, playlist_demo_state_callback, (void *)demo);

#if defined(ENABLE_LINUX_ALSA)
    struct sink_wrapper sink_ops = {
        .priv_data = NULL,
        .name = alsa_wrapper_name,
        .open = alsa_wrapper_open,
        .write = alsa_wrapper_write,
        .close = alsa_wrapper_close,
    };
#else
    struct sink_wrapper sink_ops = {
        .priv_data = NULL,
        .name = wave_wrapper_name,
        .open = wave_wrapper_open,
        .write = wave_wrapper_write,
        .close = wave_wrapper_close,
    };
#endif
    liteplayermanager_register_sink_wrapper(demo->mngr, &sink_ops);

    struct source_wrapper file_ops = {
        .async_mode = false,
        .ringbuf_size = 32*1024,
        .priv_data = NULL,
        .procotol = fatfs_wrapper_procotol,
        .open = fatfs_wrapper_open,
        .read = fatfs_wrapper_read,
        .filesize = fatfs_wrapper_filesize,
        .seek = fatfs_wrapper_seek,
        .close = fatfs_wrapper_close,
    };
    liteplayermanager_register_source_wrapper(demo->mngr, &file_ops);

    struct source_wrapper http_ops = {
        .async_mode = true,
        .ringbuf_size = 256*1024,
        .priv_data = NULL,
        .procotol = httpclient_wrapper_procotol,
        .open = httpclient_wrapper_open,
        .read = httpclient_wrapper_read,
        .filesize = httpclient_wrapper_filesize,
        .seek = httpclient_wrapper_seek,
        .close = httpclient_wrapper_close,
    };
    liteplayermanager_register_source_wrapper(demo->mngr, &http_ops);

    if (liteplayermanager_set_data_source(demo->mngr, demo->url, PLAYLIST_DEMO_THRESHOLD_MS) != 0) {
        OS_LOGE(TAG, "Failed to set data source");
        goto thread_exit;
    }

    if (liteplayermanager_prepare_async(demo->mngr) != 0) {
        OS_LOGE(TAG, "Failed to prepare player");
        goto thread_exit;
    }
    while (demo->state != LITEPLAYER_PREPARED && demo->state != LITEPLAYER_ERROR) {
        os_thread_sleep_msec(100);
    }
    if (demo->state == LITEPLAYER_ERROR) {
        OS_LOGE(TAG, "Failed to prepare player");
        goto thread_exit;
    }

    if (liteplayermanager_start(demo->mngr) != 0) {
        OS_LOGE(TAG, "Failed to start player");
        goto thread_exit;
    }
    OS_MEMORY_DUMP();
    while (demo->state != LITEPLAYER_COMPLETED && demo->state != LITEPLAYER_ERROR) {
        if (demo->state == LITEPLAYER_STOPPED || demo->state == LITEPLAYER_IDLE) {
            goto thread_exit;
        }
        os_thread_sleep_msec(100);
    }

    if (liteplayermanager_stop(demo->mngr) != 0) {
        OS_LOGE(TAG, "Failed to stop player");
        goto thread_exit;
    }
    while (demo->state != LITEPLAYER_STOPPED) {
        os_thread_sleep_msec(100);
    }

thread_exit:
    demo->exit = true;

    liteplayermanager_reset(demo->mngr);
    while (demo->state != LITEPLAYER_IDLE) {
        os_thread_sleep_msec(100);
    }
    liteplayermanager_destroy(demo->mngr);
    demo->mngr = NULL;

    os_thread_sleep_msec(100);
    OS_MEMORY_DUMP();

    OS_LOGD(TAG, "playlist demo thread leave");
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        OS_LOGI(TAG, "Usage: %s [url]", argv[0]);
        return 0;
    }

    struct os_thread_attr attr = {
        .name = "liteplayermanager_demo",
        .priority = PLAYLIST_DEMO_TASK_PRIO,
        .stacksize = PLAYLIST_DEMO_TASK_STACK,
        .joinable = true,
    };

    const char *filename = OS_STRDUP(argv[1]);
    const char *url = filename;
    if (strstr(filename, "http") == NULL) {
        struct stat statbuf;
        if (stat(filename, &statbuf) < 0) {
            OS_LOGE(TAG, "Failed to stat path[%s]", filename);
            goto demo_out;
        }

        if (S_ISDIR(statbuf.st_mode) && generate_playlist(filename) == 0) {
            url = PLAYLIST_FILE;
        }
    }

    struct playlist_demo_priv demo;
    memset(&demo, 0x0, sizeof(demo));
    demo.url = url;
    os_thread tid = os_thread_create(&attr, playlist_demo_thread, (void *)&demo);
    if (tid == NULL)
        goto demo_out;

    char input = 0;
    while (!demo.exit) {
        if (input != '\n') {
            OS_LOGW(TAG, "Waiting enter command:");
            OS_LOGW(TAG, "  Q|q: quit");
            OS_LOGW(TAG, "  P|p: pause");
            OS_LOGW(TAG, "  R|r: resume");
            OS_LOGW(TAG, "  S|s: seek");
            OS_LOGW(TAG, "  N|n: switch next");
            OS_LOGW(TAG, "  V|v: switch prev");
            OS_LOGW(TAG, "  O:   looping enable");
            OS_LOGW(TAG, "  o:   looping disable");
        }
        input = getc(stdin);

        if (input == 'Q' || input == 'q') {
           OS_LOGI(TAG, "Quit");
            if (demo.mngr)
                liteplayermanager_reset(demo.mngr);
            break;
        } else if (input == 'P' || input == 'p') {
           OS_LOGI(TAG, "Pause");
            if (demo.mngr)
                liteplayermanager_pause(demo.mngr);
        } else if (input == 'R' || input == 'r') {
           OS_LOGI(TAG, "Resume");
            if (demo.mngr)
                liteplayermanager_resume(demo.mngr);
        } else if (input == 'S' || input == 's') {
           OS_LOGI(TAG, "Seek 10s");
            if (demo.mngr) {
                int position = 0;
                if (liteplayermanager_get_position(demo.mngr, &position) == 0)
                    liteplayermanager_seek(demo.mngr, position+10000);
            }
        } else if (input == 'N' || input == 'n') {
           OS_LOGI(TAG, "Next");
            if (demo.mngr)
                liteplayermanager_next(demo.mngr);
        } else if (input == 'V' || input == 'v') {
           OS_LOGI(TAG, "Prev");
            if (demo.mngr)
                liteplayermanager_prev(demo.mngr);
        } else if (input == 'O') {
           OS_LOGI(TAG, "Enable looping");
            if (demo.mngr)
                liteplayermanager_set_single_looping(demo.mngr, true);
        } else if (input == 'o') {
           OS_LOGI(TAG, "Disable looping");
            if (demo.mngr)
                liteplayermanager_set_single_looping(demo.mngr, false);
        } else {
            if (input != '\n')
                OS_LOGW(TAG, "Unknown command: %c", input);
        }
    }

    os_thread_join(tid, NULL);

demo_out:
    OS_FREE(filename);
    OS_MEMORY_DUMP();
    OS_LOGD(TAG, "liteplayer main thread leave");
    return 0;
}
