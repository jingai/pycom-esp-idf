// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <sys/param.h>

#include "esp_attr.h"
#include "esp_log.h"

#include "rom/cache.h"
#include "rom/efuse.h"
#include "rom/ets_sys.h"
#include "rom/spi_flash.h"
#include "rom/crc.h"
#include "rom/rtc.h"
#include "rom/uart.h"
#include "rom/gpio.h"
#include "rom/secure_boot.h"

#include "soc/soc.h"
#include "soc/cpu.h"
#include "soc/rtc.h"
#include "soc/dport_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/efuse_reg.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/timer_group_reg.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"

#include "sdkconfig.h"
#include "esp_image_format.h"
#include "esp_secure_boot.h"
#include "esp_flash_encrypt.h"
#include "esp_flash_partitions.h"
#include "bootloader_flash.h"
#include "bootloader_random.h"
#include "bootloader_config.h"

#include "flash_qio_mode.h"

extern int _bss_start;
extern int _bss_end;

static const char* TAG = "boot";
/*
We arrive here after the bootloader finished loading the program from flash. The hardware is mostly uninitialized,
flash cache is down and the app CPU is in reset. We do have a stack, so we can do the initialization in C.
*/


void bootloader_main();
static void unpack_load_app(const esp_partition_pos_t *app_node);
void print_flash_info(const esp_image_header_t* pfhdr);
static void set_cache_and_start_app(uint32_t drom_addr,
    uint32_t drom_load_addr,
    uint32_t drom_size,
    uint32_t irom_addr,
    uint32_t irom_load_addr,
    uint32_t irom_size,
    uint32_t entry_addr);
static void update_flash_config(const esp_image_header_t* pfhdr);
static void clock_configure(void);
static void uart_console_configure(void);
static void wdt_reset_check(void);

void IRAM_ATTR call_start_cpu0()
{
    cpu_configure_region_protection();

    //Clear bss
    memset(&_bss_start, 0, (&_bss_end - &_bss_start) * sizeof(_bss_start));

    /* completely reset MMU for both CPUs
       (in case serial bootloader was running) */
    Cache_Read_Disable(0);
    Cache_Read_Disable(1);
    Cache_Flush(0);
    Cache_Flush(1);
    mmu_init(0);
    DPORT_REG_SET_BIT(DPORT_APP_CACHE_CTRL1_REG, DPORT_APP_CACHE_MMU_IA_CLR);
    mmu_init(1);
    DPORT_REG_CLR_BIT(DPORT_APP_CACHE_CTRL1_REG, DPORT_APP_CACHE_MMU_IA_CLR);
    /* (above steps probably unnecessary for most serial bootloader
       usage, all that's absolutely needed is that we unmask DROM0
       cache on the following two lines - normal ROM boot exits with
       DROM0 cache unmasked, but serial bootloader exits with it
       masked. However can't hurt to be thorough and reset
       everything.)

       The lines which manipulate DPORT_APP_CACHE_MMU_IA_CLR bit are
       necessary to work around a hardware bug.
    */
    DPORT_REG_CLR_BIT(DPORT_PRO_CACHE_CTRL1_REG, DPORT_PRO_CACHE_MASK_DROM0);
    DPORT_REG_CLR_BIT(DPORT_APP_CACHE_CTRL1_REG, DPORT_APP_CACHE_MASK_DROM0);

    bootloader_main();
}


/** @brief Load partition table
 *
 *  Parse partition table, get useful data such as location of
 *  OTA data partition, factory app partition, and test app partition.
 *
 *  @param         bs     bootloader state structure used to save read data
 *  @return        return true if the partition table was succesfully loaded and MD5 checksum is valid.
 */
bool load_partition_table(bootloader_state_t* bs)
{
    const esp_partition_info_t *partitions;
    const int ESP_PARTITION_TABLE_DATA_LEN = 0xC00; /* length of actual data (signature is appended to this) */
    char *partition_usage;
    esp_err_t err;
    int num_partitions;

#ifdef CONFIG_SECURE_BOOT_ENABLED
    if(esp_secure_boot_enabled()) {
        ESP_LOGI(TAG, "Verifying partition table signature...");
        err = esp_secure_boot_verify_signature(ESP_PARTITION_TABLE_ADDR, ESP_PARTITION_TABLE_DATA_LEN);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to verify partition table signature.");
            return false;
        }
        ESP_LOGD(TAG, "Partition table signature verified");
    }
#endif

    partitions = bootloader_mmap(ESP_PARTITION_TABLE_ADDR, ESP_PARTITION_TABLE_DATA_LEN);
    if (!partitions) {
            ESP_LOGE(TAG, "bootloader_mmap(0x%x, 0x%x) failed", ESP_PARTITION_TABLE_ADDR, ESP_PARTITION_TABLE_DATA_LEN);
            return false;
    }
    ESP_LOGD(TAG, "mapped partition table 0x%x at 0x%x", ESP_PARTITION_TABLE_ADDR, (intptr_t)partitions);

    err = esp_partition_table_basic_verify(partitions, true, &num_partitions);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to verify partition table");
        return false;
    }

    ESP_LOGI(TAG, "Partition Table:");
    ESP_LOGI(TAG, "## Label            Usage          Type ST Offset   Length");

    for(int i = 0; i < num_partitions; i++) {
        const esp_partition_info_t *partition = &partitions[i];
        ESP_LOGD(TAG, "load partition table entry 0x%x", (intptr_t)partition);
        ESP_LOGD(TAG, "type=%x subtype=%x", partition->type, partition->subtype);
        partition_usage = "unknown";

        /* valid partition table */
        switch(partition->type) {
        case PART_TYPE_APP: /* app partition */
            switch(partition->subtype) {
            case PART_SUBTYPE_FACTORY: /* factory binary */
                bs->factory = partition->pos;
                partition_usage = "factory app";
                break;
            case PART_SUBTYPE_TEST: /* test binary */
                bs->test = partition->pos;
                partition_usage = "test app";
                break;
            default:
                /* OTA binary */
                if ((partition->subtype & ~PART_SUBTYPE_OTA_MASK) == PART_SUBTYPE_OTA_FLAG) {
                    bs->ota[partition->subtype & PART_SUBTYPE_OTA_MASK] = partition->pos;
                    ++bs->app_count;
                    partition_usage = "OTA app";
                }
                else {
                    partition_usage = "Unknown app";
                }
                break;
            }
            break; /* PART_TYPE_APP */
        case PART_TYPE_DATA: /* data partition */
            switch(partition->subtype) {
            case PART_SUBTYPE_DATA_OTA: /* ota data */
                bs->ota_info = partition->pos;
                partition_usage = "OTA data";
                break;
            case PART_SUBTYPE_DATA_RF:
                partition_usage = "RF data";
                break;
            case PART_SUBTYPE_DATA_WIFI:
                partition_usage = "WiFi data";
                break;
            default:
                partition_usage = "Unknown data";
                break;
            }
            break; /* PARTITION_USAGE_DATA */
        default: /* other partition type */
            break;
        }

        /* print partition type info */
        ESP_LOGI(TAG, "%2d %-16s %-16s %02x %02x %08x %08x", i, partition->label, partition_usage,
                 partition->type, partition->subtype,
                 partition->pos.offset, partition->pos.size);
    }

    bootloader_munmap(partitions);

    ESP_LOGI(TAG,"End of partition table");
    return true;
}

static uint32_t ota_select_crc(const esp_ota_select_entry_t *s)
{
  return crc32_le(UINT32_MAX, (uint8_t*)&s->ota_seq, 4);
}

static bool ota_select_valid(const esp_ota_select_entry_t *s)
{
  return s->ota_seq != UINT32_MAX && s->crc == ota_select_crc(s);
}

/**
 *  @function :     bootloader_main
 *  @description:   entry function of 2nd bootloader
 *
 *  @inputs:        void
 */

void bootloader_main()
{
    clock_configure();
    uart_console_configure();
    wdt_reset_check();
    ESP_LOGI(TAG, "ESP-IDF %s 2nd stage bootloader", IDF_VER);
#if defined(CONFIG_SECURE_BOOT_ENABLED) || defined(CONFIG_FLASH_ENCRYPTION_ENABLED)
    esp_err_t err;
#endif
    esp_image_header_t fhdr;
    bootloader_state_t bs;
    esp_rom_spiflash_result_t spiRet1,spiRet2;
    esp_ota_select_entry_t sa,sb;
    const esp_ota_select_entry_t *ota_select_map;

    memset(&bs, 0, sizeof(bs));

    ESP_LOGI(TAG, "compile time " __TIME__ );
    ets_set_appcpu_boot_addr(0); 

    /* disable watch dog here */
    REG_CLR_BIT( RTC_CNTL_WDTCONFIG0_REG, RTC_CNTL_WDT_FLASHBOOT_MOD_EN );
    REG_CLR_BIT( TIMG_WDTCONFIG0_REG(0), TIMG_WDT_FLASHBOOT_MOD_EN );

#ifndef CONFIG_SPI_FLASH_ROM_DRIVER_PATCH
    const uint32_t spiconfig = ets_efuse_get_spiconfig();
    if(spiconfig != EFUSE_SPICONFIG_SPI_DEFAULTS && spiconfig != EFUSE_SPICONFIG_HSPI_DEFAULTS) {
        ESP_LOGE(TAG, "SPI flash pins are overridden. \"Enable SPI flash ROM driver patched functions\" must be enabled in menuconfig");
        return;
    }
#endif

    esp_rom_spiflash_unlock();

    ESP_LOGI(TAG, "Enabling RNG early entropy source...");
    bootloader_random_enable();

#if CONFIG_FLASHMODE_QIO || CONFIG_FLASHMODE_QOUT
    bootloader_enable_qio_mode();
#endif

    if(esp_image_load_header(0x1000, true, &fhdr) != ESP_OK) {
        ESP_LOGE(TAG, "failed to load bootloader header!");
        return;
    }

    print_flash_info(&fhdr);

    update_flash_config(&fhdr);

    if (!load_partition_table(&bs)) {
        ESP_LOGE(TAG, "load partition table error!");
        return;
    }

    esp_partition_pos_t load_part_pos;

    if (bs.ota_info.offset != 0) {              // check if partition table has OTA info partition
        //ESP_LOGE("OTA info sector handling is not implemented");
        if (bs.ota_info.size < 2 * SPI_SEC_SIZE) {
            ESP_LOGE(TAG, "ERROR: ota_info partition size %d is too small (minimum %d bytes)", bs.ota_info.size, sizeof(esp_ota_select_entry_t));
            return;
        }
        ota_select_map = bootloader_mmap(bs.ota_info.offset, bs.ota_info.size);
        if (!ota_select_map) {
            ESP_LOGE(TAG, "bootloader_mmap(0x%x, 0x%x) failed", bs.ota_info.offset, bs.ota_info.size);
            return;
        }
        memcpy(&sa, ota_select_map, sizeof(esp_ota_select_entry_t));
        memcpy(&sb, (uint8_t *)ota_select_map + SPI_SEC_SIZE, sizeof(esp_ota_select_entry_t));
        bootloader_munmap(ota_select_map);
        ESP_LOGD(TAG, "OTA sequence values A 0x%08x B 0x%08x", sa.ota_seq, sb.ota_seq);
        if(sa.ota_seq == 0xFFFFFFFF && sb.ota_seq == 0xFFFFFFFF) {
            ESP_LOGD(TAG, "OTA sequence numbers both empty (all-0xFF");
            // init status flash
            if (bs.factory.offset != 0) {        // if have factory bin,boot factory bin
                ESP_LOGD(TAG, "Defaulting to factory image");
                load_part_pos = bs.factory;
            } else {
                ESP_LOGD(TAG, "No factory image, defaulting to OTA 0");
                load_part_pos = bs.ota[0];
                sa.ota_seq = 0x01;
                sa.crc = ota_select_crc(&sa);
                sb.ota_seq = 0x00;
                sb.crc = ota_select_crc(&sb);

                Cache_Read_Disable(0);
                spiRet1 = esp_rom_spiflash_erase_sector(bs.ota_info.offset/0x1000);
                spiRet2 = esp_rom_spiflash_erase_sector(bs.ota_info.offset/0x1000+1);
                if (spiRet1 != ESP_ROM_SPIFLASH_RESULT_OK || spiRet2 != ESP_ROM_SPIFLASH_RESULT_OK ) {
                    ESP_LOGE(TAG, SPI_ERROR_LOG);
                    return;
                }
                spiRet1 = esp_rom_spiflash_write(bs.ota_info.offset,(uint32_t *)&sa,sizeof(esp_ota_select_entry_t));
                spiRet2 = esp_rom_spiflash_write(bs.ota_info.offset + 0x1000,(uint32_t *)&sb,sizeof(esp_ota_select_entry_t));
                if (spiRet1 != ESP_ROM_SPIFLASH_RESULT_OK || spiRet2 != ESP_ROM_SPIFLASH_RESULT_OK ) {
                    ESP_LOGE(TAG, SPI_ERROR_LOG);
                    return;
                }
                Cache_Read_Enable(0);
            }
            //TODO:write data in ota info
        } else  {
            if(ota_select_valid(&sa) && ota_select_valid(&sb)) {
                ESP_LOGD(TAG, "Both OTA sequence valid, using OTA slot %d", MAX(sa.ota_seq, sb.ota_seq)-1);
                load_part_pos = bs.ota[(MAX(sa.ota_seq, sb.ota_seq) - 1)%bs.app_count];
            } else if(ota_select_valid(&sa)) {
                ESP_LOGD(TAG, "Only OTA sequence A is valid, using OTA slot %d", sa.ota_seq - 1);
                load_part_pos = bs.ota[(sa.ota_seq - 1) % bs.app_count];
            } else if(ota_select_valid(&sb)) {
                ESP_LOGD(TAG, "Only OTA sequence B is valid, using OTA slot %d", sa.ota_seq - 1);
                load_part_pos = bs.ota[(sb.ota_seq - 1) % bs.app_count];
            } else if (bs.factory.offset != 0) {
                ESP_LOGE(TAG, "ota data partition invalid, falling back to factory");
                load_part_pos = bs.factory;
            } else {
                ESP_LOGE(TAG, "ota data partition invalid and no factory, can't boot");
                return;
            }
        }
    } else if (bs.factory.offset != 0) {        // otherwise, look for factory app partition
        load_part_pos = bs.factory;
    } else if (bs.test.offset != 0) {           // otherwise, look for test app parition
        load_part_pos = bs.test;
    } else {                                    // nothing to load, bail out
        ESP_LOGE(TAG, "nothing to load");
        return;
    }

#ifdef CONFIG_SECURE_BOOT_ENABLED
    /* Generate secure digest from this bootloader to protect future
       modifications */
    ESP_LOGI(TAG, "Checking secure boot...");
    err = esp_secure_boot_permanently_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bootloader digest generation failed (%d). SECURE BOOT IS NOT ENABLED.", err);
        /* Allow booting to continue, as the failure is probably
           due to user-configured EFUSEs for testing...
        */
    }
#endif

#ifdef CONFIG_FLASH_ENCRYPTION_ENABLED
    /* encrypt flash */
    ESP_LOGI(TAG, "Checking flash encryption...");
    bool flash_encryption_enabled = esp_flash_encryption_enabled();
    err = esp_flash_encrypt_check_and_update();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Flash encryption check failed (%d).", err);
      return;
    }

    if (!flash_encryption_enabled && esp_flash_encryption_enabled()) {
      /* Flash encryption was just enabled for the first time,
         so issue a system reset to ensure flash encryption
         cache resets properly */
      ESP_LOGI(TAG, "Resetting with flash encryption enabled...");
      REG_WRITE(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
      return;
    }
#endif

    ESP_LOGI(TAG, "Disabling RNG early entropy source...");
    bootloader_random_disable();

    // copy loaded segments to RAM, set up caches for mapped segments, and start application
    ESP_LOGI(TAG, "Loading app partition at offset %08x", load_part_pos);
    unpack_load_app(&load_part_pos);
}

static void unpack_load_app(const esp_partition_pos_t* partition)
{
    esp_err_t err;
    esp_image_header_t image_header;
    uint32_t image_length;

    /* TODO: verify the app image as part of OTA boot decision, so can have fallbacks */
    err = esp_image_basic_verify(partition->offset, true, &image_length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to verify app image @ 0x%x (%d)", partition->offset, err);
        return;
    }

#ifdef CONFIG_SECURE_BOOT_ENABLED
    if (esp_secure_boot_enabled()) {
        ESP_LOGI(TAG, "Verifying app signature @ 0x%x (length 0x%x)", partition->offset, image_length);
        err = esp_secure_boot_verify_signature(partition->offset, image_length);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "App image @ 0x%x failed signature verification (%d)", partition->offset, err);
            return;
        }
        ESP_LOGD(TAG, "App signature is valid");
    }
#endif

    if (esp_image_load_header(partition->offset, true, &image_header) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load app image header @ 0x%x", partition->offset);
        return;
    }

    uint32_t drom_addr = 0;
    uint32_t drom_load_addr = 0;
    uint32_t drom_size = 0;
    uint32_t irom_addr = 0;
    uint32_t irom_load_addr = 0;
    uint32_t irom_size = 0;

    /* Reload the RTC memory segments whenever a non-deepsleep reset
       is occurring */
    bool load_rtc_memory = rtc_get_reset_reason(0) != DEEPSLEEP_RESET;

    ESP_LOGD(TAG, "bin_header: %u %u %u %u %08x", image_header.magic,
             image_header.segment_count,
             image_header.spi_mode,
             image_header.spi_size,
             (unsigned)image_header.entry_addr);

    /* Important: From here on this function cannot access any global data (bss/data segments),
       as loading the app image may overwrite these.
    */
    for (int segment = 0; segment < image_header.segment_count; segment++) {
        esp_image_segment_header_t segment_header;
        uint32_t data_offs;
        if(esp_image_load_segment_header(segment, partition->offset,
                                         &image_header, true,
                                         &segment_header, &data_offs) != ESP_OK) {
            ESP_LOGE(TAG, "failed to load segment header #%d", segment);
            return;
        }

        const uint32_t address = segment_header.load_addr;
        bool load = true;
        bool map = false;
        if (address == 0x00000000) {        // padding, ignore block
            load = false;
        }
        if (address == 0x00000004) {
            load = false;                   // md5 checksum block
            // TODO: actually check md5
        }

        if (address >= SOC_DROM_LOW && address < SOC_DROM_HIGH) {
            ESP_LOGD(TAG, "found drom segment, map from %08x to %08x", data_offs,
                      segment_header.load_addr);
            drom_addr = data_offs;
            drom_load_addr = segment_header.load_addr;
            drom_size = segment_header.data_len + sizeof(segment_header);
            load = false;
            map = true;
        }

        if (address >= SOC_IROM_LOW && address < SOC_IROM_HIGH) {
            ESP_LOGD(TAG, "found irom segment, map from %08x to %08x", data_offs,
                      segment_header.load_addr);
            irom_addr = data_offs;
            irom_load_addr = segment_header.load_addr;
            irom_size = segment_header.data_len + sizeof(segment_header);
            load = false;
            map = true;
        }

        if (!load_rtc_memory && address >= SOC_RTC_IRAM_LOW && address < SOC_RTC_IRAM_HIGH) {
            ESP_LOGD(TAG, "Skipping RTC code segment at %08x\n", data_offs);
            load = false;
        }

        if (!load_rtc_memory && address >= SOC_RTC_DATA_LOW && address < SOC_RTC_DATA_HIGH) {
            ESP_LOGD(TAG, "Skipping RTC data segment at %08x\n", data_offs);
            load = false;
        }

        ESP_LOGI(TAG, "segment %d: paddr=0x%08x vaddr=0x%08x size=0x%05x (%6d) %s", segment, data_offs - sizeof(esp_image_segment_header_t),
                 segment_header.load_addr, segment_header.data_len, segment_header.data_len, (load)?"load":(map)?"map":"");

        if (load) {
            intptr_t sp, start_addr, end_addr;
            ESP_LOGV(TAG, "bootloader_mmap data_offs=%08x data_len=%08x", data_offs, segment_header.data_len);

            start_addr = segment_header.load_addr;
            end_addr = start_addr + segment_header.data_len;

            /* Before loading segment, check it doesn't clobber
               bootloader RAM... */

            if (end_addr < 0x40000000) {
                if (end_addr > 0x3FFE0000) {
                    /* Temporary workaround for an ugly crash, until we allow >192KB of static DRAM */
                    ESP_LOGE(TAG, "DRAM segment %d (start 0x%08x end 0x%08x) too large for IDF to boot",
                             segment, start_addr, end_addr);
                    return;
                }

                sp = (intptr_t)get_sp();
                if (end_addr > sp) {
                    ESP_LOGE(TAG, "Segment %d end address %08x overlaps bootloader stack %08x - can't load",
                         segment, end_addr, sp);
                    return;
                }
                if (end_addr > sp - 256) {
                    /* We don't know for sure this is the stack high water mark, so warn if
                       it seems like we may overflow.
                    */
                    ESP_LOGW(TAG, "Segment %d end address %08x close to stack pointer %08x",
                             segment, end_addr, sp);
                }
            }

            const void *data = bootloader_mmap(data_offs, segment_header.data_len);
            if(!data) {
                ESP_LOGE(TAG, "bootloader_mmap(0x%xc, 0x%x) failed",
                         data_offs, segment_header.data_len);
                return;
            }
            memcpy((void *)segment_header.load_addr, data, segment_header.data_len);
            bootloader_munmap(data);
        }
    }

    set_cache_and_start_app(drom_addr,
        drom_load_addr,
        drom_size,
        irom_addr,
        irom_load_addr,
        irom_size,
        image_header.entry_addr);
}

static void set_cache_and_start_app(
    uint32_t drom_addr,
    uint32_t drom_load_addr,
    uint32_t drom_size,
    uint32_t irom_addr,
    uint32_t irom_load_addr,
    uint32_t irom_size,
    uint32_t entry_addr)
{
    ESP_LOGD(TAG, "configure drom and irom and start");
    Cache_Read_Disable( 0 );
    Cache_Flush( 0 );
    uint32_t drom_page_count = (drom_size + 64*1024 - 1) / (64*1024); // round up to 64k
    ESP_LOGV(TAG, "d mmu set paddr=%08x vaddr=%08x size=%d n=%d", drom_addr & 0xffff0000, drom_load_addr & 0xffff0000, drom_size, drom_page_count );
    int rc = cache_flash_mmu_set( 0, 0, drom_load_addr & 0xffff0000, drom_addr & 0xffff0000, 64, drom_page_count );
    ESP_LOGV(TAG, "rc=%d", rc );
    rc = cache_flash_mmu_set( 1, 0, drom_load_addr & 0xffff0000, drom_addr & 0xffff0000, 64, drom_page_count );
    ESP_LOGV(TAG, "rc=%d", rc );
    uint32_t irom_page_count = (irom_size + 64*1024 - 1) / (64*1024); // round up to 64k
    ESP_LOGV(TAG, "i mmu set paddr=%08x vaddr=%08x size=%d n=%d", irom_addr & 0xffff0000, irom_load_addr & 0xffff0000, irom_size, irom_page_count );
    rc = cache_flash_mmu_set( 0, 0, irom_load_addr & 0xffff0000, irom_addr & 0xffff0000, 64, irom_page_count );
    ESP_LOGV(TAG, "rc=%d", rc );
    rc = cache_flash_mmu_set( 1, 0, irom_load_addr & 0xffff0000, irom_addr & 0xffff0000, 64, irom_page_count );
    ESP_LOGV(TAG, "rc=%d", rc );
    DPORT_REG_CLR_BIT( DPORT_PRO_CACHE_CTRL1_REG, (DPORT_PRO_CACHE_MASK_IRAM0) | (DPORT_PRO_CACHE_MASK_IRAM1 & 0) | (DPORT_PRO_CACHE_MASK_IROM0 & 0) | DPORT_PRO_CACHE_MASK_DROM0 | DPORT_PRO_CACHE_MASK_DRAM1 );
    DPORT_REG_CLR_BIT( DPORT_APP_CACHE_CTRL1_REG, (DPORT_APP_CACHE_MASK_IRAM0) | (DPORT_APP_CACHE_MASK_IRAM1 & 0) | (DPORT_APP_CACHE_MASK_IROM0 & 0) | DPORT_APP_CACHE_MASK_DROM0 | DPORT_APP_CACHE_MASK_DRAM1 );
    Cache_Read_Enable( 0 );

    // Application will need to do Cache_Flush(1) and Cache_Read_Enable(1)

    ESP_LOGD(TAG, "start: 0x%08x", entry_addr);
    typedef void (*entry_t)(void);
    entry_t entry = ((entry_t) entry_addr);

    // TODO: we have used quite a bit of stack at this point.
    // use "movsp" instruction to reset stack back to where ROM stack starts.
    (*entry)();
}

static void update_flash_config(const esp_image_header_t* pfhdr)
{
    uint32_t size;
    switch(pfhdr->spi_size) {
        case ESP_IMAGE_FLASH_SIZE_1MB:
            size = 1;
            break;
        case ESP_IMAGE_FLASH_SIZE_2MB:
            size = 2;
            break;
        case ESP_IMAGE_FLASH_SIZE_4MB:
            size = 4;
            break;
        case ESP_IMAGE_FLASH_SIZE_8MB:
            size = 8;
            break;
        case ESP_IMAGE_FLASH_SIZE_16MB:
            size = 16;
            break;
        default:
            size = 2;
    }
    Cache_Read_Disable( 0 );
    // Set flash chip size
    esp_rom_spiflash_config_param(g_rom_flashchip.device_id, size * 0x100000, 0x10000, 0x1000, 0x100, 0xffff);
    // TODO: set mode
    // TODO: set frequency
    Cache_Flush(0);
    Cache_Read_Enable( 0 );
}

void print_flash_info(const esp_image_header_t* phdr)
{
#if (BOOT_LOG_LEVEL >= BOOT_LOG_LEVEL_NOTICE)

    ESP_LOGD(TAG, "magic %02x", phdr->magic );
    ESP_LOGD(TAG, "segments %02x", phdr->segment_count );
    ESP_LOGD(TAG, "spi_mode %02x", phdr->spi_mode );
    ESP_LOGD(TAG, "spi_speed %02x", phdr->spi_speed );
    ESP_LOGD(TAG, "spi_size %02x", phdr->spi_size );

    const char* str;
    switch ( phdr->spi_speed ) {
    case ESP_IMAGE_SPI_SPEED_40M:
        str = "40MHz";
        break;
    case ESP_IMAGE_SPI_SPEED_26M:
        str = "26.7MHz";
        break;
    case ESP_IMAGE_SPI_SPEED_20M:
        str = "20MHz";
        break;
    case ESP_IMAGE_SPI_SPEED_80M:
        str = "80MHz";
        break;
    default:
        str = "20MHz";
        break;
    }
    ESP_LOGI(TAG, "SPI Speed      : %s", str );

    /* SPI mode could have been set to QIO during boot already,
       so test the SPI registers not the flash header */
    uint32_t spi_ctrl = REG_READ(SPI_CTRL_REG(0));
    if (spi_ctrl & SPI_FREAD_QIO) {
        str = "QIO";
    } else if (spi_ctrl & SPI_FREAD_QUAD) {
        str = "QOUT";
    } else if (spi_ctrl & SPI_FREAD_DIO) {
        str = "DIO";
    } else if (spi_ctrl & SPI_FREAD_DUAL) {
        str = "DOUT";
    } else if (spi_ctrl & SPI_FASTRD_MODE) {
        str = "FAST READ";
    } else {
        str = "SLOW READ";
    }
    ESP_LOGI(TAG, "SPI Mode       : %s", str );

    switch ( phdr->spi_size ) {
    case ESP_IMAGE_FLASH_SIZE_1MB:
        str = "1MB";
        break;
    case ESP_IMAGE_FLASH_SIZE_2MB:
        str = "2MB";
        break;
    case ESP_IMAGE_FLASH_SIZE_4MB:
        str = "4MB";
        break;
    case ESP_IMAGE_FLASH_SIZE_8MB:
        str = "8MB";
        break;
    case ESP_IMAGE_FLASH_SIZE_16MB:
        str = "16MB";
        break;
    default:
        str = "2MB";
        break;
    }
    ESP_LOGI(TAG, "SPI Flash Size : %s", str );
#endif
}


static void clock_configure(void)
{
    /* Set CPU to 80MHz. Keep other clocks unmodified. */
    rtc_cpu_freq_t cpu_freq = RTC_CPU_FREQ_80M;

    /* On ESP32 rev 0, switching to 80MHz if clock was previously set to
     * 240 MHz may cause the chip to lock up (see section 3.5 of the errata
     * document). For rev. 0, switch to 240 instead if it was chosen in
     * menuconfig.
     */
    uint32_t chip_ver_reg = REG_READ(EFUSE_BLK0_RDATA3_REG);
    if ((chip_ver_reg & EFUSE_RD_CHIP_VER_REV1_M) == 0 &&
            CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ == 240) {
        cpu_freq = RTC_CPU_FREQ_240M;
    }

    uart_tx_wait_idle(0);
    rtc_clk_config_t clk_cfg = RTC_CLK_CONFIG_DEFAULT();
    clk_cfg.xtal_freq = CONFIG_ESP32_XTAL_FREQ;
    clk_cfg.cpu_freq = cpu_freq;
    clk_cfg.slow_freq = rtc_clk_slow_freq_get();
    clk_cfg.fast_freq = rtc_clk_fast_freq_get();
    rtc_clk_init(clk_cfg);
    /* As a slight optimization, if 32k XTAL was enabled in sdkconfig, we enable
     * it here. Usually it needs some time to start up, so we amortize at least
     * part of the start up time by enabling 32k XTAL early.
     * App startup code will wait until the oscillator has started up.
     */
    if (!rtc_clk_32k_enabled()) {
        rtc_clk_32k_bootstrap();
    }
}

static void uart_console_configure(void)
{
#if CONFIG_CONSOLE_UART_NONE
    ets_install_putc1(NULL);
    ets_install_putc2(NULL);
#else // CONFIG_CONSOLE_UART_NONE
    const int uart_num = CONFIG_CONSOLE_UART_NUM;

    uartAttach();
    ets_install_uart_printf();

    // ROM bootloader may have put a lot of text into UART0 FIFO.
    // Wait for it to be printed.
    uart_tx_wait_idle(0);

#if CONFIG_CONSOLE_UART_CUSTOM
    // Some constants to make the following code less upper-case
    const int uart_tx_gpio = CONFIG_CONSOLE_UART_TX_GPIO;
    const int uart_rx_gpio = CONFIG_CONSOLE_UART_RX_GPIO;
    // Switch to the new UART (this just changes UART number used for
    // ets_printf in ROM code).
    uart_tx_switch(uart_num);
    // If console is attached to UART1 or if non-default pins are used,
    // need to reconfigure pins using GPIO matrix
    if (uart_num != 0 || uart_tx_gpio != 1 || uart_rx_gpio != 3) {
        // Change pin mode for GPIO1/3 from UART to GPIO
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_U0RXD_GPIO3);
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_U0TXD_GPIO1);
        // Route GPIO signals to/from pins
        // (arrays should be optimized away by the compiler)
        const uint32_t tx_idx_list[3] = { U0TXD_OUT_IDX, U1TXD_OUT_IDX, U2TXD_OUT_IDX };
        const uint32_t rx_idx_list[3] = { U0RXD_IN_IDX, U1RXD_IN_IDX, U2RXD_IN_IDX };
        const uint32_t tx_idx = tx_idx_list[uart_num];
        const uint32_t rx_idx = rx_idx_list[uart_num];
        gpio_matrix_out(uart_tx_gpio, tx_idx, 0, 0);
        gpio_matrix_in(uart_rx_gpio, rx_idx, 0);
    }
#endif // CONFIG_CONSOLE_UART_CUSTOM

    // Set configured UART console baud rate
    const int uart_baud = CONFIG_CONSOLE_UART_BAUDRATE;
    uart_div_modify(uart_num, (rtc_clk_apb_freq_get() << 4) / uart_baud);

#endif // CONFIG_CONSOLE_UART_NONE
}

static void wdt_reset_cpu0_info_enable(void)
{
    //We do not reset core1 info here because it didn't work before cpu1 was up. So we put it into call_start_cpu1.
    DPORT_REG_SET_BIT(DPORT_PRO_CPU_RECORD_CTRL_REG, DPORT_PRO_CPU_PDEBUG_ENABLE | DPORT_PRO_CPU_RECORD_ENABLE);
    DPORT_REG_CLR_BIT(DPORT_PRO_CPU_RECORD_CTRL_REG, DPORT_PRO_CPU_RECORD_ENABLE);
}

static void wdt_reset_info_dump(int cpu)
{
    uint32_t inst = 0, pid = 0, stat = 0, data = 0, pc = 0,
             lsstat = 0, lsaddr = 0, lsdata = 0, dstat = 0;
    char *cpu_name = cpu ? "APP" : "PRO";

    if (cpu == 0) {
        stat    = DPORT_REG_READ(DPORT_PRO_CPU_RECORD_STATUS_REG);
        pid     = DPORT_REG_READ(DPORT_PRO_CPU_RECORD_PID_REG);
        inst    = DPORT_REG_READ(DPORT_PRO_CPU_RECORD_PDEBUGINST_REG);
        dstat   = DPORT_REG_READ(DPORT_PRO_CPU_RECORD_PDEBUGSTATUS_REG);
        data    = DPORT_REG_READ(DPORT_PRO_CPU_RECORD_PDEBUGDATA_REG);
        pc      = DPORT_REG_READ(DPORT_PRO_CPU_RECORD_PDEBUGPC_REG);
        lsstat  = DPORT_REG_READ(DPORT_PRO_CPU_RECORD_PDEBUGLS0STAT_REG);
        lsaddr  = DPORT_REG_READ(DPORT_PRO_CPU_RECORD_PDEBUGLS0ADDR_REG);
        lsdata  = DPORT_REG_READ(DPORT_PRO_CPU_RECORD_PDEBUGLS0DATA_REG);

    } else {
        stat    = DPORT_REG_READ(DPORT_APP_CPU_RECORD_STATUS_REG);
        pid     = DPORT_REG_READ(DPORT_APP_CPU_RECORD_PID_REG);
        inst    = DPORT_REG_READ(DPORT_APP_CPU_RECORD_PDEBUGINST_REG);
        dstat   = DPORT_REG_READ(DPORT_APP_CPU_RECORD_PDEBUGSTATUS_REG);
        data    = DPORT_REG_READ(DPORT_APP_CPU_RECORD_PDEBUGDATA_REG);
        pc      = DPORT_REG_READ(DPORT_APP_CPU_RECORD_PDEBUGPC_REG);
        lsstat  = DPORT_REG_READ(DPORT_APP_CPU_RECORD_PDEBUGLS0STAT_REG);
        lsaddr  = DPORT_REG_READ(DPORT_APP_CPU_RECORD_PDEBUGLS0ADDR_REG);
        lsdata  = DPORT_REG_READ(DPORT_APP_CPU_RECORD_PDEBUGLS0DATA_REG);
    }
    if (DPORT_RECORD_PDEBUGINST_SZ(inst) == 0 &&
        DPORT_RECORD_PDEBUGSTATUS_BBCAUSE(dstat) == DPORT_RECORD_PDEBUGSTATUS_BBCAUSE_WAITI) {
        ESP_LOGW(TAG, "WDT reset info: %s CPU PC=0x%x (waiti mode)", cpu_name, pc);
    } else {
        ESP_LOGW(TAG, "WDT reset info: %s CPU PC=0x%x", cpu_name, pc);
    }
    ESP_LOGD(TAG, "WDT reset info: %s CPU STATUS        0x%08x", cpu_name, stat);
    ESP_LOGD(TAG, "WDT reset info: %s CPU PID           0x%08x", cpu_name, pid);
    ESP_LOGD(TAG, "WDT reset info: %s CPU PDEBUGINST    0x%08x", cpu_name, inst);
    ESP_LOGD(TAG, "WDT reset info: %s CPU PDEBUGSTATUS  0x%08x", cpu_name, dstat);
    ESP_LOGD(TAG, "WDT reset info: %s CPU PDEBUGDATA    0x%08x", cpu_name, data);
    ESP_LOGD(TAG, "WDT reset info: %s CPU PDEBUGPC      0x%08x", cpu_name, pc);
    ESP_LOGD(TAG, "WDT reset info: %s CPU PDEBUGLS0STAT 0x%08x", cpu_name, lsstat);
    ESP_LOGD(TAG, "WDT reset info: %s CPU PDEBUGLS0ADDR 0x%08x", cpu_name, lsaddr);
    ESP_LOGD(TAG, "WDT reset info: %s CPU PDEBUGLS0DATA 0x%08x", cpu_name, lsdata);
}

static void wdt_reset_check(void)
{
    int wdt_rst = 0;
    RESET_REASON rst_reas[2];

    rst_reas[0] = rtc_get_reset_reason(0);
    rst_reas[1] = rtc_get_reset_reason(1);
    if (rst_reas[0] == RTCWDT_SYS_RESET || rst_reas[0] == TG0WDT_SYS_RESET || rst_reas[0] == TG1WDT_SYS_RESET ||
        rst_reas[0] == TGWDT_CPU_RESET  || rst_reas[0] == RTCWDT_CPU_RESET) {
        ESP_LOGW(TAG, "PRO CPU has been reset by WDT.");
        wdt_rst = 1;
    }
    if (rst_reas[1] == RTCWDT_SYS_RESET || rst_reas[1] == TG0WDT_SYS_RESET || rst_reas[1] == TG1WDT_SYS_RESET ||
        rst_reas[1] == TGWDT_CPU_RESET  || rst_reas[1] == RTCWDT_CPU_RESET) {
        ESP_LOGW(TAG, "APP CPU has been reset by WDT.");
        wdt_rst = 1;
    }
    if (wdt_rst) {
        // if reset by WDT dump info from trace port
        wdt_reset_info_dump(0);
        wdt_reset_info_dump(1);
    }
    wdt_reset_cpu0_info_enable();
}
