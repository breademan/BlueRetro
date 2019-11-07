#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "adapter.h"
#include "config.h"

#define CONFIG_MAGIC 0xA5A5A5A5
#define CONFIG_FILE "/sd/config.bin"

struct config config;

static void config_init_struct(struct config *data);
static int32_t config_load_from_file(struct config *data);
static int32_t config_store_on_file(struct config *data);

static void config_init_struct(struct config *data) {
    data->magic = CONFIG_MAGIC;
    data->multitap_conf = 0x00;

    for (uint32_t i = 0; i < WIRED_MAX_DEV; i++) {
        data->output_conf[i].dev_mode = 0x00;
        data->output_conf[i].map_size = KB_MAX;
        for (uint32_t j = 0; j < KB_MAX; j++) {
            data->output_conf[i].map_conf[j].src_btn = j;
            data->output_conf[i].map_conf[j].dst_btn = j;
            data->output_conf[i].map_conf[j].dst_id = i;
            data->output_conf[i].map_conf[j].turbo = 0;
            data->output_conf[i].map_conf[j].algo = 0;
            data->output_conf[i].map_conf[j].perc_max = 100;
            data->output_conf[i].map_conf[j].perc_threshold = 25;
            data->output_conf[i].map_conf[j].perc_deadzone = 10;
        }
    }
}

static int32_t config_load_from_file(struct config *data) {
    struct stat st;
    int32_t ret = -1;

    if (stat(CONFIG_FILE, &st) != 0) {
        printf("%s: No config on SD. Creating...\n", __FUNCTION__);
        config_init_struct(data);
        ret = config_store_on_file(data);
    }
    else {
        FILE *file = fopen(CONFIG_FILE, "rb");
        if (file == NULL) {
            printf("%s: failed to open file for reading\n", __FUNCTION__);
        }
        else {
            fread((void *)data, sizeof(*data), 1, file);
            fclose(file);
            ret = 0;
        }
    }
    if (data->magic != CONFIG_MAGIC) { /* TODO use CRC32 */
        printf("%s: Bad magic, reset config\n", __FUNCTION__);
        config_init_struct(data);
        ret = config_store_on_file(data);
    }
    return ret;
}

static int32_t config_store_on_file(struct config *data) {
    int32_t ret = -1;

    FILE *file = fopen(CONFIG_FILE, "wb");
    if (file == NULL) {
        printf("%s: failed to open file for writing\n", __FUNCTION__);
    }
    else {
        fwrite((void *)data, sizeof(*data), 1, file);
        fclose(file);
        ret = 0;
    }
    return ret;
}

void config_init(void) {
    config_load_from_file(&config);
}

void config_update(void) {
    config_store_on_file(&config);
}