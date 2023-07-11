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
#include <esp32/rom/ets_sys.h>

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
static char * mc_filename; // Contains the file name
static uint32_t addr_range[MC_BUFFER_BLOCK_CNT]={0}; // Holds which cache line is mapped to which addresses.
#define MC_ADDR_RANGE_COMPARE_MASK (0xFFFFF000)
// Addresses are byte-addressed out of 128KB space, which is 17 bits to store, but since the address ranges are 4KB each, there are only 32 possible values
// Time/memory tradeoff: We can either store a 32bit address or store an 8bit address and left-shift it 12 places each time
// For code simplicity, I'll go with the latter
static enum mc_fetch_state_t {MC_FETCH_FINISHED=0, MC_FETCHING, MC_FETCH_FAILED} mc_fetch_state = MC_FETCH_FINISHED;
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
    if (stat(mc_filename, &st) != 0) {
        printf("# %s: No Memory Card on FS. Creating...\n", __FUNCTION__);
        ret = mc_store(mc_filename);
    }
    FILE *file = fopen(mc_filename, "rb");
    if(wired_adapter.system_id==DC) fseek(file, (0b0011<<17), SEEK_SET); //If it's the dreamcast, read from the last memcard to cause cache misses for debugging.
    if (file == NULL) {
        printf("# %s: failed to open file for reading\n", __FUNCTION__);
    }
    else {
        uint32_t count = 0;
        for (uint32_t i = 0; i < MC_BUFFER_BLOCK_CNT; i++) {
            count += fread((void *)mc_buffer[i], MC_BUFFER_BLOCK_SIZE, 1, file);
            addr_range[i] = i*MC_BUFFER_BLOCK_SIZE;
            if(wired_adapter.system_id==DC) addr_range[i] |= (0b0011<<17); //Record that this is a read from memcard 3
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
// Only creates a new file with the size MC_BUFFER_BLOCK_CNT: For Dreamcast, we want it to create a file 4x that.
// Writing back with this for a DC memcard will no longer be a valid writeback -- We should change this function to only initialize a memory card
// TODO Rename to mc_create and zero-fill instead of putting in junk data
static int32_t mc_store(char *filename) {
    int32_t ret = -1;

    FILE *file = fopen(filename, "wb");
    if (file == NULL) {
        printf("# %s: failed to open file for writing\n", __FUNCTION__);
    }
    else {
        uint32_t target_count = MC_BUFFER_BLOCK_CNT;
        if(wired_adapter.system_id==DC) target_count = 4*MC_BUFFER_BLOCK_CNT;
        uint32_t count = 0;
        for (uint32_t i = 0; i < target_count; i++) {
            count += fwrite((void *)mc_buffer[i&(MC_BUFFER_BLOCK_CNT-1)], MC_BUFFER_BLOCK_SIZE, 1, file);
        }
        fclose(file);
    
        if (count == target_count) {
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
    if (block) {
        uint32_t count = 0;
        block -= 1;
        FILE *file = fopen(mc_filename, "r+b");
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

        printf("# %s: cache line %ld addr_range %lx written back cnt: %ld\n", __FUNCTION__, block, addr_range[block], count);

    }
    return ret;
}

static inline void mc_store_cb(void *arg) {
    uint32_t count = 0;

    if(mc_fetch_state==MC_FETCHING){
        //check for free slots
        uint32_t block = __builtin_ffs(~mc_block_state);
        if(!block) { // all cache lines have yet to be written back
            (void)mc_store_spread();
            block = __builtin_ffs(~mc_block_state);
        }
        //Select line to fetch -- for now let's use ffs as above -- block = index of first 0 bit + 1
        if(!block){
            //writeback failed, don't overrwrite any data
            //set a flag saying the fetch failed so we can send an 0xFC (resend last packet AKA you're going too fast)
            atomic_set(&mc_fetch_state,MC_FETCH_FAILED);
        }
        else{
        block-=1;

        FILE *file = fopen(mc_filename, "rb");
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
        addr_range[block] = mc_fetch_addr & MC_ADDR_RANGE_COMPARE_MASK;
        atomic_clear(&mc_fetch_state);
        printf("# %s: fetched addr %lx to block %ld\n", __FUNCTION__, mc_fetch_addr,block);
        }
    }
    else (void)mc_store_spread();
    
    if (mc_block_state) {
        mc_start_update_timer(20000);
    }
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
        mc_filename=VMU_0_FILE;
    }
    else {
        mc_filename=N64_MEMORY_CARD_FILE;
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


int mc_read_multicard(uint32_t addr, uint8_t *data, uint32_t size) {
    struct raw_fb fb_data = {0};
    //Search for the block in cache
    for (uint32_t i=0; i<MC_BUFFER_BLOCK_CNT;i++){
        if(addr_range[i] == (addr & MC_ADDR_RANGE_COMPARE_MASK)) {
            memcpy(data, mc_buffer[i] + (addr & 0xFFF), size);
            //Later, update the LRU counter here if we implement that
            return 0;
        }
    }
    //If not found in cache, we'll need to fetch it.
    mc_fetch_addr = addr;
    atomic_set(&mc_fetch_state,MC_FETCHING);
    //push out feedback and wait
    fb_data.header.wired_id = 0;
    fb_data.header.type = FB_TYPE_MEM_WRITEBACK;
    fb_data.header.data_len = 0;
    adapter_q_fb(&fb_data);

    bool timed_out = true;
    for(int timeout=30; timeout>0; timeout--){
        //check if mc_fetch is false to signal fetch completion
        if (mc_fetch_state != MC_FETCHING) {
        if (mc_fetch_state != MC_FETCHING) {
            timed_out = false;
            break;
        }
        delay_us(1000);
    }
    
    if(timed_out){
        ets_printf("%s: reached timeout on wait for fetch.\n",__FUNCTION__);
        atomic_clear(&mc_fetch_state); //if it didn't fetch in 30ms it's not gonna finish.
        return -1;
        }
    else if (mc_fetch_state == MC_FETCH_FAILED){
        ets_printf("%s: fetch failed.\n",__FUNCTION__);
        atomic_clear(&mc_fetch_state); // Clear the fetch flag. I could leave this set so it can keep retrying the fetch
        // But instead I'll try to fetch at most once per r/w command
        return -1;
    }
    else {
            //Search for the block in cache
        for (uint8_t i=0; i<MC_BUFFER_BLOCK_CNT;i++){
            if(addr_range[i] == (addr & MC_ADDR_RANGE_COMPARE_MASK)) {
                memcpy(data, mc_buffer[i] + (addr & 0xFFF), size);
                //Later, update the LRU counter here if we implement that
                return 0;
            }
        }
        //If we got here, we claim to have completed fetch yet nothing is matching the correct table entry. 
        ets_printf("%s fetch succeeded but no corresponding block found: searching for address %lx\n",__FUNCTION__,addr);
        return -1;
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

int mc_write_multicard(uint32_t addr, uint8_t *data, uint32_t size) {
    struct raw_fb fb_data = {0};
    //Search for the block in cache
    for (uint32_t i=0; i<MC_BUFFER_BLOCK_CNT;i++){
        if(addr_range[i] == (addr & MC_ADDR_RANGE_COMPARE_MASK)) {
            memcpy(mc_buffer[i] + (addr & 0xFFF), data, size);
            atomic_set_bit(&mc_block_state, i);

            fb_data.header.wired_id = 0;
            fb_data.header.type = FB_TYPE_MEM_WRITE;
            fb_data.header.data_len = 0;
            adapter_q_fb(&fb_data);
            //Later, update the LRU counter here if we implement that
            return 0;
        }
    }
    //If not found in cache, we'll need to fetch it.
    mc_fetch_addr = addr;
    atomic_set(&mc_fetch_state,MC_FETCHING);
    //push out feedback and wait
    fb_data.header.wired_id = 0;
    fb_data.header.type = FB_TYPE_MEM_WRITEBACK;
    fb_data.header.data_len = 0;
    adapter_q_fb(&fb_data);

    bool timed_out = true;
    for(int timeout=30; timeout>0; timeout--){
        //check if mc_fetch is false to signal fetch completion
        if (mc_fetch_state != MC_FETCHING) {
            timed_out=false;
            break;
        }
        delay_us(1000);
    }
    
    if(timed_out){
        ets_printf("%s: reached timeout on wait for fetch.\n",__FUNCTION__);
        atomic_clear(&mc_fetch_state); //if it didn't fetch in 30ms it's not gonna finish.
        return -1;
        }
    else if (mc_fetch_state == MC_FETCH_FAILED){
        ets_printf("%s: fetch failed.\n",__FUNCTION__);
        atomic_clear(&mc_fetch_state); // Clear the fetch flag. I could leave this set so it can keep retrying the fetch, but instead I'll try to fetch at most once per r/w command
        return -1;
    }
    else {
            //Search for the block in cache
        for (uint32_t i=0; i<MC_BUFFER_BLOCK_CNT;i++){
            if(addr_range[i] == (addr & MC_ADDR_RANGE_COMPARE_MASK)) {
                memcpy(mc_buffer[i] + (addr & 0xFFF), data, size);
                atomic_set_bit(&mc_block_state, i);
                atomic_set_bit(&mc_block_state, i);

                fb_data.header.wired_id = 0;
                fb_data.header.type = FB_TYPE_MEM_WRITE;
                fb_data.header.data_len = 0;
                adapter_q_fb(&fb_data);
                //Later, update the LRU counter here if we implement that
                return 0;
            }
        }
    //Fetch reports success, but write to buffer still fails? This should never happen: if it does, we know the fetch logic fetched the wrong data.
    ets_printf("%s fetch succeeded but no corresponding block found: searching for address %lx\n",__FUNCTION__,addr);
    return -1;
    }
}

uint8_t *mc_get_ptr(uint32_t addr) {
    return mc_buffer[addr >> 12] + (addr & 0xFFF);
}
