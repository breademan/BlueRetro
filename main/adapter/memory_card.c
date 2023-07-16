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

#define MC_BUFFER_SIZE (128 * 1024)
#define MC_BUFFER_BLOCK_SIZE (4 * 1024)
#define MC_BUFFER_BLOCK_CNT (MC_BUFFER_SIZE / MC_BUFFER_BLOCK_SIZE)

/* Related to bare metal hack, something goes writing there. */
/* Workaround: Remove region from heap pool until I figure it out */
SOC_RESERVE_MEMORY_REGION(0x3FFE7D98, 0x3FFE7E28, bad_region);

// This is what the formatter writes to a VMU
// It includes a date/time stamp
//Init area is 512Bytes/1block -- Word 21 is the last word that contains non-zero data.
//This is checked in accordance with traces from a real DC, but not data sent to a real VMU
//If the DC was just reflecting my mediainfo replies, which are guesses, they may not be true to a real VMU.
static const uint8_t vmu_init_system_area[] = {
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, //16 bytes, 4 words
    0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //w4-7 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //w8-11 (6,7,8,9,10,11 contain nothing used?)
    0x27, 0x11, 0x98, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //w12-15 (w12,w13 contain date-timestamp)
    0x00, 0x00, 0x00, 0xFF, 0x00, 0xFE, 0x00, 0xFF, 0x00, 0xFD, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0D, //w16-19 (w18 is unknown but differs)
    0x00, 0x1F, 0x00, 0xC8, 0x00, 0x80, 0x00, 0x00, //w20-21 (20 -- some saves zero out the 1F in w20)
    };

/*static const uint8_t fat_block[512] = {
    // Offset 0x0001FC00 to 0x0001FDFF
    //Based of a presumably non-empty save file
    0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 
    0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 
    0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC,
    0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 
    0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 
    0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC,
    0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 
    0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 
    0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC,
    0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 
    0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 
    0x00, 0x58, 0xFF, 0xFA, 0x00, 0x5A, 0x00, 0x59, 0x00, 0x5C, 0x00, 0x5B, 0x00, 0x5E, 0x00, 0x5D,
    0x00, 0x60, 0x00, 0x5F, 0x00, 0x62, 0x00, 0x61, 0x00, 0x64, 0x00, 0x63, 0x00, 0x66, 0x00, 0x65, 
    0x00, 0x68, 0x00, 0x67, 0x00, 0x6A, 0x00, 0x69, 0x00, 0x6C, 0x00, 0x6B, 0x00, 0x6E, 0x00, 0x6D, 
    0x00, 0x70, 0x00, 0x6F, 0x00, 0x72, 0x00, 0x71, 0x00, 0x74, 0xFF, 0xFA, 0x00, 0x76, 0x00, 0x75,
    0x00, 0x78, 0x00, 0x77, 0x00, 0x7A, 0x00, 0x79, 0x00, 0x7C, 0x00, 0x7B, 0x00, 0x7E, 0xFF, 0xFA,
    0x00, 0x80, 0xFF, 0xFA, 0x00, 0x82, 0xFF, 0xFA, 0x00, 0x84, 0x00, 0x83, 0x00, 0x86, 0x00, 0x85, 
    0x00, 0x88, 0x00, 0x87, 0x00, 0x8A, 0x00, 0x89, 0x00, 0x8C, 0x00, 0x8B, 0x00, 0x8E, 0x00, 0x8D,
    0x00, 0x90, 0x00, 0x8F, 0x00, 0x92, 0x00, 0x91, 0x00, 0x94, 0x00, 0x93, 0x00, 0x96, 0x00, 0x95,
    0x00, 0x98, 0x00, 0x97, 0x00, 0x9A, 0x00, 0x99, 0x00, 0x9C, 0x00, 0x9B, 0x00, 0x9E, 0x00, 0x9D, 
    0x00, 0xA0, 0x00, 0x9F, 0x00, 0xA2, 0x00, 0xA1, 0x00, 0xA4, 0x00, 0xA3, 0x00, 0xA6, 0x00, 0xA5,
    0x00, 0xA8, 0x00, 0xA7, 0x00, 0xAA, 0x00, 0xA9, 0x00, 0xAC, 0x00, 0xAB, 0x00, 0xAE, 0x00, 0xAD,
    0x00, 0xB0, 0x00, 0xAF, 0x00, 0xB2, 0x00, 0xB1, 0x00, 0xB4, 0x00, 0xB3, 0x00, 0xB6, 0xFF, 0xFA, 
    0x00, 0xB8, 0x00, 0xB7, 0x00, 0xBA, 0x00, 0xB9, 0x00, 0xBC, 0x00, 0xBB, 0x00, 0xBE, 0x00, 0xBD,
    0x00, 0xC0, 0x00, 0xBF, 0x00, 0xC2, 0x00, 0xC1, 0x00, 0xC4, 0x00, 0xC3, 0x00, 0xC6, 0x00, 0xC5,
    0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 
    0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC,
    0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 
    0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC,
    0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC,
    0xFF, 0xFA, 0xFF, 0xFC, 0x00, 0xF2, 0x00, 0xF1, 0x00, 0xF4, 0x00, 0xF3, 0x00, 0xF6, 0x00, 0xF5,
    0x00, 0xF8, 0x00, 0xF7, 0x00, 0xFA, 0x00, 0xF9, 0x00, 0xFC, 0x00, 0xFB, 0xFF, 0xFA, 0xFF, 0xFA
};*/

// Some memory cards have the first word as 0x00fdfffc, and the second word as 0x00f2fffa. If there's some trouble, try this.
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
        data_count += fwrite(vmu_init_system_area, 4, __sizeof__(vmu_init_system_area), file);
        // Pad the rest with zeros
        for (uint32_t i = 0; i < (128 - __sizeof__(vmu_init_system_area)); i++) {
            data_count += fwrite((void *)mc_buffer[0], 4, 1, file);
        }

        if (zero_count == MC_BUFFER_BLOCK_CNT) {
            ret = 0;
            mc_block_state = 0;
        }
        fclose(file);


        printf("# %s: file created. zero_cnt(should be one): %ld\n", __FUNCTION__, zero_count);
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
        //ret = mc_store();
        ret = create_vmu_file(); // N64 was previously initialized with junk, so it shouldn't cause any problems if we replace it with different junk.
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

static int32_t mc_store(void) {
    int32_t ret = -1;

    FILE *file = fopen(mc_filename, "wb");
    if (file == NULL) {
        printf("# %s: failed to open file for writing\n", __FUNCTION__);
    }
    else {
        uint32_t count = 0;
        for (uint32_t i = 0; i < MC_BUFFER_BLOCK_CNT; i++) {
            count += fwrite((void *)mc_buffer[i], MC_BUFFER_BLOCK_SIZE, 1, file);
        }
        fclose(file);

        if (count == MC_BUFFER_BLOCK_CNT) {
            ret = 0;
            mc_block_state = 0;
        }
        printf("# %s: file updated cnt: %ld\n", __FUNCTION__, count);
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
