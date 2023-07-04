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

#define MC_MAX_FILE_CNT (4)

/* Related to bare metal hack, something goes writing there. */
/* Workaround: Remove region from heap pool until I figure it out */
SOC_RESERVE_MEMORY_REGION(0x3FFE7D98, 0x3FFE7E28, bad_region);

static uint8_t *mc_buffer[MC_BUFFER_BLOCK_CNT] = {0};
static esp_timer_handle_t mc_timer_hdl = NULL;
static int32_t mc_block_state = 0; // Holds which cache line is not yet written back to flash
static uint8_t num_files = 1;
static char * filename_list[MC_MAX_FILE_CNT]; // List of files which the memory card contains
static uint8_t vmu_number[MC_BUFFER_BLOCK_CNT]={0}; // Holds which cache line is mapped to which file; index is the cache line, value is the index in filename_list
static uint32_t addr_range[MC_BUFFER_BLOCK_CNT]={0}; // Holds which cache line is mapped to which addresses.
#define MC_ADDR_RANGE_COMPARE_MASK (0xFFFFF000)
// Addresses are byte-addressed out of 128KB space, which is 17 bits to store, but since the address ranges are 4KB each, there are only 32 possible values
// Time/memory tradeoff: We can either store a 32bit address or store an 8bit address and left-shift it 12 places each time
// For code simplicity, I'll go with the latter
static bool mc_fetch = 0;
static uint8_t mc_fetch_card_num = 0;
static uint32_t mc_fetch_addr = NULL;


static int32_t mc_restore();
static int32_t mc_store(char *filename);
static inline void mc_store_cb(void *arg);

static void mc_start_update_timer(uint64_t timeout_us) {
    if (mc_timer_hdl) {
        if (esp_timer_is_active(mc_timer_hdl)) {
            esp_timer_stop(mc_timer_hdl);
        }
        esp_timer_start_once(mc_timer_hdl, timeout_us);
    }
}


static int32_t mc_restore() {
    struct stat st;
    int32_t ret = -1;
    for (uint8_t i = 0; i<num_files;i++){
        if (stat(filename_list[i], &st) != 0) {
            printf("# %s: No Memory Card on FS. Creating...\n", __FUNCTION__);
            ret = mc_store(filename_list[i]);
        }
    }
        // Read last file in filename_list into memory
        FILE *file = fopen(filename_list[num_files-1], "rb");
        if (file == NULL) {
            printf("# %s: failed to open file for reading\n", __FUNCTION__);
        }
        else {
            uint32_t count = 0;
            for (uint32_t i = 0; i < MC_BUFFER_BLOCK_CNT; i++) {
                count += fread((void *)mc_buffer[i], MC_BUFFER_BLOCK_SIZE, 1, file);
                vmu_number[i] = num_files-1;
                addr_range[i] = i*MC_BUFFER_BLOCK_SIZE;
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
    return ret;
}

// Writes the entire memory card buffer back to flash
// Currently only used to create a new memory card and fill it with effectively junk data
static int32_t mc_store(char *filename) {
    int32_t ret = -1;

    FILE *file = fopen(filename, "wb");
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

static int32_t mc_store_spread() {
    int32_t ret = -1;
    uint32_t block = __builtin_ffs(mc_block_state);
    //TODO: Pull this if statement out to before the file open since we need to check the block number, vmu_block, and vmu_number to figure out which file to open.
    if (block) {
        uint32_t count = 0;
        block -= 1;
        FILE *file = fopen(filename_list[vmu_number[block]], "r+b");
        if (file == NULL) {
            printf("# %s: failed to open file for writing\n", __FUNCTION__);
            return ret;
        }


        fseek(file, addr_range[block], SEEK_SET);
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
    return ret;
}

static inline void mc_store_cb(void *arg) {
    uint32_t count = 0;

    if(mc_fetch){
        //check for free slots
        uint32_t block = __builtin_ffs(~mc_block_state);
        if(!block) { // all cache lines have yet to be written back
            (void)mc_store_spread();
            block = __builtin_ffs(~mc_block_state);
        }
        //Select line to fetch -- for now let's use ffs as above -- block = index of first 0 bit + 1
        block-=1;

        FILE *file = fopen(filename_list[mc_fetch_card_num], "rb");
        if (file == NULL) {
            printf("# %s: failed to open file for reading\n", __FUNCTION__);
        }        
        fseek(file, mc_fetch_addr & MC_ADDR_RANGE_COMPARE_MASK, SEEK_SET); //fetch the data, aligned on block size
        count = fread((void *)mc_buffer[block], MC_BUFFER_BLOCK_SIZE, 1, file);
        fclose(file);

        if(count==0){
            printf("%s: failed to read file during fetch\n",__FUNCTION__);
        }

        //Update cache table
        vmu_number[block] = mc_fetch_card_num;
        addr_range[block] = mc_fetch_addr & MC_ADDR_RANGE_COMPARE_MASK;
        mc_fetch = false;
        printf("# %s: fetched addr %lx from card %d to block %ld\n", __FUNCTION__, mc_fetch_addr, mc_fetch_card_num,block);
        
    }
    else (void)mc_store_spread();
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

        if (mc_buffer[i] == NULL) {
            printf("# %s mc_buffer[%ld] alloc fail\n", __FUNCTION__, i);
            heap_caps_dump_all();
            goto exit;
        }
    }

    esp_timer_create(&mc_timer_args, &mc_timer_hdl);

    if(wired_adapter.system_id==DC) {
        num_files=4;
        filename_list[0]=VMU_0_FILE;
        filename_list[1]=VMU_1_FILE;
        filename_list[2]=VMU_2_FILE;
        filename_list[3]=VMU_3_FILE;
    }
    else {
        num_files=1;
        filename_list[0]=N64_MEMORY_CARD_FILE;
    }
    ret = mc_restore();

    return ret;

exit:
    return -1;
}

void mc_storage_update(void) {
    mc_start_update_timer(1000000);
}

void mc_storage_update_instant(void) {
    mc_start_update_timer(1);
}

/* Assume r/w size will never cross blocks boundary */
void mc_read(uint32_t addr, uint8_t *data, uint32_t size) {
    memcpy(data, mc_buffer[addr >> 12] + (addr & 0xFFF), size);
}

void mc_read_multicard(uint32_t addr, uint8_t *data, uint32_t size, uint8_t memcard_no) {
    struct raw_fb fb_data = {0};
    //Search for the block in cache
    for (uint8_t i=0; i<MC_BUFFER_BLOCK_CNT;i++){
        if((addr_range[i] == (addr & MC_ADDR_RANGE_COMPARE_MASK)) && vmu_number[i] == memcard_no) {
            memcpy(data, mc_buffer[i] + (addr & 0xFFF), size);
            //Later, update the LRU counter here if we implement that
            return;
        }
    }
    //If not found in cache, we'll need to fetch it.
    mc_fetch = true;
    mc_fetch_addr = addr;
    mc_fetch_card_num = memcard_no;
    //push out feedback and wait
    fb_data.header.wired_id = 0;
    fb_data.header.type = FB_TYPE_MEM_WRITEBACK;
    fb_data.header.data_len = 0;
    adapter_q_fb(&fb_data);

    bool fetch_success = false;
    for(int timeout=30; timeout>0; timeout--){
        //check if mc_fetch is false to signal fetch completion
        if (!mc_fetch) {
            fetch_success=true;
            break;
        }
        delay_us(1000);
    }
    
    if(!fetch_success) printf("%s: reached timeout on wait for fetch.\n",__FUNCTION__);
    else {
            //Search for the block in cache
        for (uint8_t i=0; i<MC_BUFFER_BLOCK_CNT;i++){
            if((addr_range[i] == (addr & MC_ADDR_RANGE_COMPARE_MASK)) && vmu_number[i] == memcard_no) {
                memcpy(data, mc_buffer[i] + (addr & 0xFFF), size);
                //Later, update the LRU counter here if we implement that
                return;
            }
        }
    }
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

void mc_write_multicard(uint32_t addr, uint8_t *data, uint32_t size, uint8_t memcard_no) {
    struct raw_fb fb_data = {0};
    uint32_t block = addr >> 12;
    //Search for the block in cache
    for (uint8_t i=0; i<MC_BUFFER_BLOCK_CNT;i++){
        if((addr_range[i] == (addr & MC_ADDR_RANGE_COMPARE_MASK)) && vmu_number[i] == memcard_no) {
            memcpy(mc_buffer[i] + (addr & 0xFFF), data, size);
            atomic_set_bit(&mc_block_state, block);

            fb_data.header.wired_id = 0;
            fb_data.header.type = FB_TYPE_MEM_WRITE;
            fb_data.header.data_len = 0;
            adapter_q_fb(&fb_data);
            //Later, update the LRU counter here if we implement that
            return;
        }
    }
    //If not found in cache, we'll need to fetch it.
    mc_fetch = true;
    mc_fetch_addr = addr;
    mc_fetch_card_num = memcard_no;
    //push out feedback and wait
    fb_data.header.wired_id = 0;
    fb_data.header.type = FB_TYPE_MEM_WRITEBACK;
    fb_data.header.data_len = 0;
    adapter_q_fb(&fb_data);

    bool fetch_success = false;
    for(int timeout=30; timeout>0; timeout--){
        //check if mc_fetch is false to signal fetch completion
        if (!mc_fetch) {
            fetch_success=true;
            break;
        }
        delay_us(1000);
    }
    
    if(!fetch_success) printf("%s: reached timeout on wait for fetch.\n",__FUNCTION__);
    else {
            //Search for the block in cache
        for (uint8_t i=0; i<MC_BUFFER_BLOCK_CNT;i++){
            if((addr_range[i] == (addr & MC_ADDR_RANGE_COMPARE_MASK)) && vmu_number[i] == memcard_no) {
                memcpy(mc_buffer[i] + (addr & 0xFFF), data, size);
                atomic_set_bit(&mc_block_state, block);

                fb_data.header.wired_id = 0;
                fb_data.header.type = FB_TYPE_MEM_WRITE;
                fb_data.header.data_len = 0;
                adapter_q_fb(&fb_data);
                //Later, update the LRU counter here if we implement that
                return;
            }
        }
    }
}

uint8_t *mc_get_ptr(uint32_t addr) {
    return mc_buffer[addr >> 12] + (addr & 0xFFF);
}
