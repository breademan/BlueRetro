/*
 * Copyright (c) 2021, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <soc/soc_memory_layout.h>
#include "xtensa/core-macros.h"
#include "sdkconfig.h"
#include "system/fs.h"
#include "memory_card.h"
#include "system/delay.h"

#define MC_BUFFER_SIZE (128 * 1024)
#define MC_BUFFER_BLOCK_SIZE (4 * 1024)
#define MC_BUFFER_BLOCK_CNT (MC_BUFFER_SIZE / MC_BUFFER_BLOCK_SIZE)

/* Related to bare metal hack, something goes writing there. */
/* Workaround: Remove region from heap pool until I figure it out */
SOC_RESERVE_MEMORY_REGION(0x3FFE7D98, 0x3FFE7E28, bad_region);


static const uint8_t vmu_init_system_area[] = {
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x27, 0x11, 0x98, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xFF, 0x00, 0xFE, 0x00, 0xFF, 0x00, 0xFD, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0D,
    0x00, 0x1F, 0x00, 0xC8, 0x00, 0x80, 0x00, 0x00,
    };


static const uint8_t fat_data[32] = {
    0xff, 0xfa, 0xff, 0xfc, 0x00, 0xf2, 0x00, 0xf1, 0x00, 0xf4, 0x00, 0xf3, 0x00, 0xf6, 0x00, 0xf5,
    0x00, 0xf8, 0x00, 0xf7, 0x00, 0xfa, 0x00, 0xf9, 0x00, 0xfc, 0x00, 0xfb, 0xff, 0xfa, 0xff, 0xfa
};


static uint8_t *mc_buffer[MC_BUFFER_BLOCK_CNT] = {0};
static esp_timer_handle_t mc_timer_hdl = NULL;
static int32_t mc_block_state = 0;
static char * mc_filename;

static int32_t mc_restore(void);
static int32_t mc_store(void);
static inline void mc_store_cb(void *arg);

static int32_t create_vmu_file(void) {
    int32_t ret = -1;
    FILE *file = fopen(mc_filename, "wb");

    if (file == NULL) {
        printf("# %s: failed to open file for writing\n", __FUNCTION__);
    }
    else {
        uint32_t zero_count = 0;
        uint32_t data_count = 0;
        // Write blocks 00-FD -- initialize with zeros.
        // the first block of mc_buffer will be our zero source.
        memset((void *)mc_buffer[0], 0, MC_BUFFER_BLOCK_SIZE);
        for (uint32_t i = 0; i < MC_BUFFER_BLOCK_CNT; i++) {
            zero_count += fwrite((void *)mc_buffer[0], MC_BUFFER_BLOCK_SIZE, 1, file);
        }
        //Seek to block FE
        fseek(file, (512*0xFE), SEEK_SET);
        //Write block FE
        const uint8_t fat_unallocated[4] = {0xff, 0xfc, 0xff, 0xfc};
        for (uint32_t i = 0; i < 120; i++) {
            data_count = fwrite(fat_unallocated, 4, 1, file); // I'm not 100% confident the endianness on this will work like I expect. TODO: Check this.
        }
        data_count += fwrite(fat_data, 4, 8, file);

        // Write block FF
        data_count += fwrite(vmu_init_system_area, 4, sizeof(vmu_init_system_area), file);
        // Pad the rest with zeros
        for (uint32_t i = 0; i < (128 - sizeof(vmu_init_system_area)); i++) {
            data_count += fwrite((void *)mc_buffer[0], 4, 1, file);
        }

        if (zero_count == MC_BUFFER_BLOCK_CNT) {
            ret = 0;
            mc_block_state = 0;
        }
        fclose(file);


        printf("# %s: file created. zero_cnt(should be one): %ld. Words of data written: %ld\n", __FUNCTION__, zero_count, data_count);
    }
    return ret;
}

static void mc_start_update_timer(uint64_t timeout_us) {
    if (mc_timer_hdl) {
        if (esp_timer_is_active(mc_timer_hdl)) {
            esp_timer_stop(mc_timer_hdl);
        }
        esp_timer_start_once(mc_timer_hdl, timeout_us);
    }
}

static int32_t mc_restore(void) {
    struct stat st;
    int32_t ret = -1;
    if (stat(mc_filename, &st) != 0) {
        printf("# %s: No Memory Card on FS. Creating...\n", __FUNCTION__);
        ret = create_vmu_file();
    }
    else {
        FILE *file = fopen(mc_filename, "rb");
        if (file == NULL) {
            printf("# %s: failed to open file for reading\n", __FUNCTION__);
        }
        else {
            uint32_t count = 0;
            for (uint32_t i = 0; i < MC_BUFFER_BLOCK_CNT; i++) {
                count += fread((void *)mc_buffer[i], MC_BUFFER_BLOCK_SIZE, 1, file);
            }
            fclose(file);

            if (count == MC_BUFFER_BLOCK_CNT) {
                ret = 0;
                printf("# %s: restore sucessful!\n", __FUNCTION__);
            }
            else {
                printf("# %s: restore failed! cnt: %ld File size:%ld\n", __FUNCTION__, count, st.st_size);
            }
        }
    }
    return ret;
}

static int32_t mc_store_spread(void) {
    int32_t ret = -1;

    FILE *file = fopen(mc_filename, "r+b");
    if (file == NULL) {
        printf("# %s: failed to open file for writing\n", __FUNCTION__);
    }
    else {
        uint32_t block = __builtin_ffs(mc_block_state);

        if (block) {
            uint32_t count = 0;
            block -= 1;

            fseek(file, block * MC_BUFFER_BLOCK_SIZE, SEEK_SET);
            count = fwrite((void *)mc_buffer[block], MC_BUFFER_BLOCK_SIZE, 1, file);
            fclose(file);

            if (count == 1) {
                ret = 0;
                atomic_clear_bit(&mc_block_state, block);
            }

            printf("# %s: block %ld updated cnt: %ld\n", __FUNCTION__, block, count);

            if (mc_block_state) {
                mc_start_update_timer(20000);
            }
        }
    }
    return ret;
}

static inline void mc_store_cb(void *arg) {
    (void)mc_store_spread();
}

int32_t mc_init(void) {
    int32_t ret = -1;
    const esp_timer_create_args_t mc_timer_args = {
        .callback = &mc_store_cb,
        .arg = (void *)NULL,
        .name = "mc_timer"
    };

    for (uint32_t i = 0; i < MC_BUFFER_BLOCK_CNT; i++) {
        mc_buffer[i] = malloc(MC_BUFFER_BLOCK_SIZE);
        memset((void *)mc_buffer[i],0,MC_BUFFER_BLOCK_SIZE);

        if (mc_buffer[i] == NULL) {
            printf("# %s mc_buffer[%ld] alloc fail\n", __FUNCTION__, i);
            heap_caps_dump_all();
            goto exit;
        }
    }

    esp_timer_create(&mc_timer_args, &mc_timer_hdl);
    
    while(wired_adapter.system_id==WIRED_AUTO){
        printf("Waiting for wired_adapter.system_id to be decided\n");
        delay_us(200);
    }

    if (wired_adapter.system_id==DC) mc_filename = DC_MEMORY_CARD_FILE;
    else mc_filename = N64_MEMORY_CARD_FILE;

    ret = mc_restore();

    return ret;

exit:
    return -1;
}

void mc_storage_update(void) {
    mc_start_update_timer(1000000);
}

/* Assume r/w size will never cross blocks boundary */
void mc_read(uint32_t addr, uint8_t *data, uint32_t size) {
    memcpy(data, mc_buffer[addr >> 12] + (addr & 0xFFF), size);
}

void mc_write(uint32_t addr, uint8_t *data, uint32_t size) {
    struct raw_fb fb_data = {0};
    uint32_t block = addr >> 12;

    memcpy(mc_buffer[block] + (addr & 0xFFF), data, size);
    atomic_set_bit(&mc_block_state, block);

    fb_data.header.wired_id = 0;
    fb_data.header.type = FB_TYPE_MEM_WRITE;
    fb_data.header.data_len = 0;
    adapter_q_fb(&fb_data);
}

uint8_t *mc_get_ptr(uint32_t addr) {
    return mc_buffer[addr >> 12] + (addr & 0xFFF);
}
