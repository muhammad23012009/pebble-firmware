/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "board/board.h"
#include "drivers/dma.h"
#include "drivers/flash.h"
#include "drivers/flash/flash_impl.h"
#include "drivers/flash/micron_n25q/flash_private.h"
#include "kernel/util/stop.h"
#include "process_management/worker_manager.h"
#include "services/common/analytics/analytics.h"
#include "os/mutex.h"
#include "kernel/util/delay.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/math.h"

#include "FreeRTOS.h"
#include "semphr.h"

extern void system_reset();

/*
 * Each peripheral has a dma channel / stream it works with
 * c.f. section 9.3.3 in stm32 reference manual
 */
/* RX DMA */
static DMA_Stream_TypeDef* FLASH_DMA_STREAM = DMA2_Stream0;
static const uint32_t FLASH_DMA_CHANNEL = DMA_Channel_3;
static const uint32_t FLASH_DMA_IRQn = DMA2_Stream0_IRQn;
static const uint32_t FLASH_DATA_REGISTER_ADDR = (uint32_t)&(SPI1->DR);
/* TX DMA */
static DMA_Stream_TypeDef* FLASH_TX_DMA_STREAM = DMA2_Stream3;
static const uint32_t FLASH_TX_DMA_CHANNEL = DMA_Channel_3;

static uint32_t s_write_protect_start = 0;
static uint32_t s_write_protect_end = 0;

static uint32_t s_flash_num_uses = 0;

struct FlashState {
  bool enabled;
  bool sleep_when_idle;
  bool deep_sleep;
  PebbleMutex * mutex;
  SemaphoreHandle_t dma_semaphore;
} s_flash_state;

static void enable_flash_dma_clock(void) {
  // TINTINHACK: Rather than update this file to use the new DMA driver, just rely on the fact that
  // this is the only consumer of DMA2.
  periph_config_enable(DMA2, RCC_AHB1Periph_DMA2);
}

static void disable_flash_dma_clock(void) {
  // TINTINHACK: Rather than update this file to use the new DMA driver, just rely on the fact that
  // this is the only consumer of DMA2.
  periph_config_disable(DMA2, RCC_AHB1Periph_DMA2);
}

static void setup_dma_read(uint8_t *buffer, int size) {
  DMA_InitTypeDef dma_config;

  DMA_DeInit(FLASH_DMA_STREAM);
  DMA_DeInit(FLASH_TX_DMA_STREAM);

  /* RX DMA config */
  DMA_StructInit(&dma_config);
  dma_config.DMA_Channel = FLASH_DMA_CHANNEL;
  dma_config.DMA_DIR = DMA_DIR_PeripheralToMemory;
  dma_config.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
  dma_config.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
  dma_config.DMA_Mode = DMA_Mode_Normal;
  dma_config.DMA_PeripheralBaseAddr = FLASH_DATA_REGISTER_ADDR;
  dma_config.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
  dma_config.DMA_MemoryInc = DMA_MemoryInc_Enable;
  dma_config.DMA_Priority = DMA_Priority_High;
  dma_config.DMA_FIFOMode = DMA_FIFOMode_Disable;
  dma_config.DMA_MemoryBurst = DMA_MemoryBurst_Single;
  dma_config.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;
  dma_config.DMA_Memory0BaseAddr = (uint32_t)buffer;
  dma_config.DMA_BufferSize = size;

  DMA_Init(FLASH_DMA_STREAM, &dma_config);

  /* TX DMA config */
  dma_config.DMA_Channel = FLASH_TX_DMA_CHANNEL;
  dma_config.DMA_DIR = DMA_DIR_MemoryToPeripheral;
  dma_config.DMA_PeripheralBaseAddr = FLASH_DATA_REGISTER_ADDR;
  dma_config.DMA_MemoryInc = DMA_MemoryInc_Disable;
  dma_config.DMA_Priority = DMA_Priority_High;
  dma_config.DMA_Memory0BaseAddr = (uint32_t)&FLASH_CMD_DUMMY;
  dma_config.DMA_BufferSize = size;

  DMA_Init(FLASH_TX_DMA_STREAM, &dma_config);

  /* Setup DMA interrupts */
  NVIC_InitTypeDef nvic_config;
  nvic_config.NVIC_IRQChannel = FLASH_DMA_IRQn;
  nvic_config.NVIC_IRQChannelPreemptionPriority = 0x0f;
  nvic_config.NVIC_IRQChannelSubPriority = 0x00;
  nvic_config.NVIC_IRQChannelCmd = ENABLE;
  NVIC_Init(&nvic_config);

  DMA_ITConfig(FLASH_DMA_STREAM, DMA_IT_TC, ENABLE);

  // enable the DMA stream to start the transfer
  SPI_I2S_DMACmd(FLASH_SPI, SPI_I2S_DMAReq_Tx | SPI_I2S_DMAReq_Rx, ENABLE);
}

static void do_dma_transfer(void) {
  xSemaphoreTake(s_flash_state.dma_semaphore, portMAX_DELAY);
  stop_mode_disable(InhibitorFlash);
  DMA_Cmd(FLASH_DMA_STREAM, ENABLE);
  DMA_Cmd(FLASH_TX_DMA_STREAM, ENABLE);
  xSemaphoreTake(s_flash_state.dma_semaphore, portMAX_DELAY);
  stop_mode_enable(InhibitorFlash);
  xSemaphoreGive(s_flash_state.dma_semaphore);
}

void DMA2_Stream0_IRQHandler(void) {
  if (DMA_GetITStatus(FLASH_DMA_STREAM, DMA_IT_TCIF3)) {
    DMA_ClearITPendingBit(FLASH_DMA_STREAM, DMA_IT_TCIF3);
    NVIC_DisableIRQ(FLASH_DMA_IRQn);
    signed portBASE_TYPE was_higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flash_state.dma_semaphore, &was_higher_priority_task_woken);
    portEND_SWITCHING_ISR(was_higher_priority_task_woken);
    return; //notreached
  }
}

status_t flash_impl_enter_low_power_mode(void) {
  flash_impl_use();

  if (!s_flash_state.deep_sleep) {
    flash_start_cmd();
    flash_send_and_receive_byte(FLASH_CMD_DEEP_SLEEP);
    flash_end_cmd();

    // guarantee we have actually transitioned to deep sleep
    delay_us(5);
    s_flash_state.deep_sleep = true;
  }
  flash_impl_release();

  return S_SUCCESS;
}

status_t flash_impl_exit_low_power_mode(void) {
  //assert_usable_state();

  flash_impl_use();
  if (s_flash_state.deep_sleep) {
    flash_start_cmd();
    flash_send_and_receive_byte(FLASH_CMD_WAKE);
    flash_end_cmd();

    // wait a sufficient amount of time to enter standby mode
    // It appears violating these timing conditions can lead to
    // random bit corruptions on flash writes!
    delay_us(100);
    s_flash_state.deep_sleep = false;
  }
  flash_impl_release();

  return S_SUCCESS;
}

void handle_sleep_when_idle_begin(void) {
  if (s_flash_state.sleep_when_idle) {
    flash_impl_exit_low_power_mode();
  }
}

FlashAddress flash_impl_get_sector_base_address(FlashAddress address) {
  return address & SECTOR_ADDR_MASK;
}

FlashAddress flash_impl_get_subsector_base_address(FlashAddress address) {
  return address & SUBSECTOR_ADDR_MASK;
}

// This simply issues a command to read a specific register
static uint8_t prv_flash_get_register(uint8_t command) {
  flash_start_cmd();
  flash_send_and_receive_byte(command);
  uint8_t reg = flash_read_next_byte();
  flash_end_cmd();
  return reg;
}

// This will read the flag status register and check it for the SectorLockStatus flag
void prv_check_protection_flag() {
  uint8_t flag_status_register = prv_flash_get_register(FLASH_CMD_READ_FLAG_STATUS_REG);
  // assert if we found the flag to be enabled
  PBL_ASSERTN(!(flag_status_register & N25QFlagStatusBit_SectorLockStatus));
}

// This will clear the protection flag error from a previous error.
// We call this because the error bits persist across reboots
static void prv_clear_flag_status_register(void) {
  flash_start_cmd();
  flash_send_and_receive_byte(FLASH_CMD_CLEAR_FLAG_STATUS_REG);
  flash_end_cmd();
}

// Public interface
// From here on down, make sure you're taking the s_flash_state.mutex before doing anything to the SPI peripheral.

/**
 * Write up to 1 page (256B) of data to flash. start_addr DOES NOT
 * need to be paged aligned. When writing into the middle of a page
 * (addr & 0xFFF > 0), overrunning the length of the page will cause
 * the write to "wrap around" and will modify (i.e. corrupt) data
 * stored before the starting address within the page.
 *
 */
int flash_impl_write_page_begin(const void* buffer, FlashAddress addr, size_t len) {
  // Ensure that we're not trying to write more data than a single page (256 bytes)
  PBL_LOG(LOG_LEVEL_ALWAYS, "Write data called! buffer is %p, address is %ld, size is %d", buffer, addr, len);
  flash_impl_use();

  uint8_t *data = (uint8_t*) buffer;
  size_t buffer_size = len > FLASH_PAGE_SIZE ? FLASH_PAGE_SIZE : len;
  len = buffer_size;

  PBL_LOG(LOG_LEVEL_ALWAYS, "Buffer size is now %d", buffer_size);

  // Writing a zero-length buffer is a no-op.
  if (buffer_size < 1) {
    return E_ERROR;
  }

  flash_write_enable();

  flash_start_cmd();

  flash_send_and_receive_byte(FLASH_CMD_PAGE_PROGRAM);
  flash_send_24b_address(addr);

  while (len != 0) {
    flash_send_and_receive_byte(*data);
    data++;
    len--;
  }

  flash_end_cmd();

  prv_check_protection_flag();

  flash_impl_release();

  PBL_LOG(LOG_LEVEL_ALWAYS, "Finished writing a page to flash!");
  return buffer_size;
}

status_t flash_impl_get_write_status(void) {
  flash_impl_use();

  uint8_t status_register = prv_flash_get_register(FLASH_CMD_READ_STATUS_REG);
  uint8_t flag_status_register = prv_flash_get_register(FLASH_CMD_READ_FLAG_STATUS_REG);
  flash_impl_release();

  if (status_register & N25QStatusBit_WriteInProgress)
    return E_BUSY;
  else if ((flag_status_register & N25QFlagStatusBit_ProgramSuspended))
    return E_AGAIN;
  else if (!(status_register & N25QStatusBit_WriteInProgress))
    return S_SUCCESS;
  else
    return E_ERROR;
}

status_t flash_impl_erase_suspend(FlashAddress address)
{
  flash_impl_use();

  // Check to see if we have a write in progress
  uint8_t status_register = prv_flash_get_register(FLASH_CMD_READ_STATUS_REG);
  if (!(status_register & N25QStatusBit_WriteInProgress))
    return S_NO_ACTION_REQUIRED;

  flash_start_cmd();
  flash_send_and_receive_byte(FLASH_CMD_ERASE_SUSPEND);
  flash_end_cmd();

  uint8_t flags = prv_flash_get_register(FLASH_CMD_READ_FLAG_STATUS_REG);

  flash_impl_release();

  if (flags & N25QFlagStatusBit_EraseSuspended)
    return S_SUCCESS;

  return E_ERROR;
}

status_t flash_impl_erase_resume(FlashAddress address)
{
  flash_impl_use();

  uint8_t status_register = prv_flash_get_register(FLASH_CMD_READ_FLAG_STATUS_REG);
  if (!(status_register & N25QFlagStatusBit_EraseSuspended))
    return S_NO_ACTION_REQUIRED;

  flash_start_cmd();
  flash_send_and_receive_byte(FLASH_CMD_ERASE_RESUME);
  flash_end_cmd();

  status_register = prv_flash_get_register(FLASH_CMD_READ_FLAG_STATUS_REG);
  flash_impl_release();

  if (!(status_register & N25QFlagStatusBit_EraseSuspended))
    return S_SUCCESS;

  return E_ERROR;
}

void flash_impl_enable_write_protection(void) {
  return;
}

status_t flash_impl_init(bool coredump_mode) {
  vSemaphoreCreateBinary(s_flash_state.dma_semaphore);

  flash_impl_use();

  prv_flash_start();

  s_flash_state.enabled = true;
  s_flash_state.sleep_when_idle = false;

  // Assume that last time we shut down we were asleep. Come back out.
  s_flash_state.deep_sleep = true;
  flash_impl_exit_low_power_mode();

  prv_clear_flag_status_register();

  flash_impl_release();

  if (!coredump_mode)
    flash_whoami();

  return S_SUCCESS;
}

status_t flash_impl_read_sync(void* buffer, FlashAddress start_addr, size_t len) {
  PBL_LOG(LOG_LEVEL_ALWAYS, "read sync called! %p, %ld, %d", buffer, start_addr, len);
  if (!len) {
    return E_ERROR;
  }


  if (!s_flash_state.enabled) {
    return E_ERROR;
  }

  uint8_t *data = buffer;
  power_tracking_start(PowerSystemFlashRead);

  flash_impl_use();
  //handle_sleep_when_idle_begin();

  flash_wait_for_write();
  PBL_LOG(LOG_LEVEL_ALWAYS, "Status register before starting was %d %d", prv_flash_get_register(FLASH_CMD_READ_STATUS_REG), prv_flash_get_register(FLASH_CMD_READ_FLAG_STATUS_REG));

  flash_start_cmd();

  flash_send_and_receive_byte(FLASH_CMD_READ);
  flash_send_24b_address(start_addr);

  flash_read_next_byte();
  PBL_LOG(LOG_LEVEL_ALWAYS, "Status register after writing address was %d %d", prv_flash_get_register(FLASH_CMD_READ_STATUS_REG), prv_flash_get_register(FLASH_CMD_READ_FLAG_STATUS_REG));
  // There is delay associated with setting up the stm32 dma, using FreeRTOS
  // sempahores, handling ISRs, etc. Thus for short reads, the cost of using
  // DMA is far more expensive than the read being performed. Reads greater
  // than 34 was empirically determined to be the point at which using the DMA
  // engine is advantageous
#if !defined(TARGET_QEMU)
  const uint32_t num_reads_dma_cutoff = 34;
#else
  // We are disabling DMA reads when running under QEMU for now because they are not reliable.
  const uint32_t num_reads_dma_cutoff = len + 1;
#endif
  if (len < num_reads_dma_cutoff) {
    while (len--) {
      *data = flash_read_next_byte();
      PBL_LOG(LOG_LEVEL_ALWAYS, "Printing status register %d %d", prv_flash_get_register(FLASH_CMD_READ_STATUS_REG), prv_flash_get_register(FLASH_CMD_READ_FLAG_STATUS_REG));
      PBL_LOG(LOG_LEVEL_ALWAYS, "Read a byte! %d", *data);
      data++;
      PBL_LOG(LOG_LEVEL_ALWAYS, "data pointer is now %p", data);
    }
  } else {
    enable_flash_dma_clock();
    setup_dma_read(buffer, len);
    do_dma_transfer();
    disable_flash_dma_clock();
  }

  PBL_LOG(LOG_LEVEL_ALWAYS, "read data from SPI, first two bytes are as follows: %d %d", *data, data[1]);

  flash_end_cmd();

  flash_impl_release();

  power_tracking_stop(PowerSystemFlashRead);

  return S_SUCCESS;
}

status_t flash_impl_erase_subsector_begin(FlashAddress subsector_addr) {
  PBL_LOG(LOG_LEVEL_ALWAYS, "Erasing subsector 0x%"PRIx32" (0x%"PRIx32" - 0x%"PRIx32")",
      subsector_addr,
      subsector_addr & SUBSECTOR_ADDR_MASK,
      (subsector_addr & SUBSECTOR_ADDR_MASK) + SUBSECTOR_SIZE_BYTES);

  if (!s_flash_state.enabled) {
    return E_ERROR;
  }

  analytics_inc(ANALYTICS_APP_METRIC_FLASH_SUBSECTOR_ERASE_COUNT, AnalyticsClient_CurrentTask);
  power_tracking_start(PowerSystemFlashErase);

  flash_impl_use();
  //handle_sleep_when_idle_begin();

  flash_write_enable();

  flash_start_cmd();
  flash_send_and_receive_byte(FLASH_CMD_ERASE_SUBSECTOR);
  flash_send_24b_address(subsector_addr);
  flash_end_cmd();

  flash_wait_for_write();

  prv_check_protection_flag();

  flash_impl_release();

  power_tracking_stop(PowerSystemFlashErase);

  return S_SUCCESS;
}

status_t flash_impl_erase_sector_begin(FlashAddress sector_addr) {
  PBL_LOG(LOG_LEVEL_DEBUG, "Erasing sector 0x%"PRIx32" (0x%"PRIx32" - 0x%"PRIx32")",
          sector_addr,
          sector_addr & SECTOR_ADDR_MASK,
          (sector_addr & SECTOR_ADDR_MASK) + SECTOR_SIZE_BYTES);

  if (prv_flash_sector_is_erased(sector_addr, false)) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Sector %#"PRIx32" already erased", sector_addr);
    return S_NO_ACTION_REQUIRED;
  }

  power_tracking_start(PowerSystemFlashErase);

  flash_impl_use();
  handle_sleep_when_idle_begin();

  flash_write_enable();

  flash_start_cmd();
  flash_send_and_receive_byte(FLASH_CMD_ERASE_SECTOR);
  flash_send_24b_address(sector_addr);
  flash_end_cmd();

  flash_wait_for_write();

  prv_check_protection_flag();

  flash_impl_release();

  power_tracking_stop(PowerSystemFlashErase);

  return S_SUCCESS;
}

status_t flash_impl_get_erase_status(void) {
  flash_impl_use();

  uint8_t status_register = prv_flash_get_register(FLASH_CMD_READ_FLAG_STATUS_REG);

  flash_impl_release();

  PBL_LOG(LOG_LEVEL_ALWAYS, "Here is flag status! %d %d %d", status_register, prv_flash_get_register(FLASH_CMD_READ_STATUS_REG), status_register & N25QFlagStatusBit_EraseStatus);

  if ((status_register & N25QFlagStatusBit_EraseStatus) == 0)
    return S_SUCCESS;

  else if ((status_register & N25QFlagStatusBit_DeviceReady) == 0)
    return E_BUSY;

  else if (status_register & N25QFlagStatusBit_EraseStatus)
    return E_ERROR;

  else if (status_register & N25QFlagStatusBit_EraseSuspended)
    return E_AGAIN;

  return E_ERROR;
}

status_t flash_impl_blank_check_subsector(FlashAddress subsector_addr) {
  // TODO: handle ongoing operations

  if (prv_flash_sector_is_erased(subsector_addr, true))
    return S_TRUE;

  return S_FALSE;
}

status_t flash_impl_blank_check_sector(FlashAddress sector_addr) {
  if (prv_flash_sector_is_erased(sector_addr, false))
    return S_TRUE;

  return S_FALSE;
}

// It is dangerous to leave this built in by default.
#if 0
void flash_erase_bulk(void) { 
  assert_usable_state();

  flash_lock();

  if (!s_flash_state.enabled) {
    flash_unlock();
    return;
  }

  flash_prf_set_protection(false);

  power_tracking_start(PowerSystemFlashErase);

  enable_flash_spi_clock();
  handle_sleep_when_idle_begin();

  flash_write_enable();

  flash_start_cmd();
  flash_send_and_receive_byte(FLASH_CMD_ERASE_BULK);
  flash_end_cmd();

  flash_wait_for_write();

  flash_prf_set_protection(true);

  disable_flash_spi_clock();

  power_tracking_stop(PowerSystemFlashErase);
  flash_unlock();
}
#endif

void debug_flash_dump_registers(void) {
#ifdef PBL_LOG_ENABLED
  flash_lock();

  if (!s_flash_state.enabled) {
    flash_unlock();
    return;
  }

  enable_flash_spi_clock();
  handle_sleep_when_idle_begin();

  uint8_t status_register = prv_flash_get_register(FLASH_CMD_READ_STATUS_REG);
  uint8_t lock_register = prv_flash_get_register(FLASH_CMD_READ_LOCK_REGISTER);
  uint8_t flag_status_register = prv_flash_get_register(FLASH_CMD_READ_FLAG_STATUS_REG);
  uint8_t nonvolatile_config_register =
      prv_flash_get_register(FLASH_CMD_READ_NONVOLATILE_CONFIG_REGISTER);
  uint8_t volatile_config_register =
      prv_flash_get_register(FLASH_CMD_READ_VOLATILE_CONFIG_REGISTER);

  disable_flash_spi_clock();
  flash_unlock();

  PBL_LOG(LOG_LEVEL_DEBUG, "Status Register: 0x%x", status_register);
  PBL_LOG(LOG_LEVEL_DEBUG, "Lock Register: 0x%x", lock_register);
  PBL_LOG(LOG_LEVEL_DEBUG, "Flag Status Register: 0x%x", flag_status_register);
  PBL_LOG(LOG_LEVEL_DEBUG, "Nonvolatile Configuration Register: 0x%x", nonvolatile_config_register);
  PBL_LOG(LOG_LEVEL_DEBUG, "Volatile Configuration Register: 0x%x", volatile_config_register);
#endif
}

/*void flash_prf_set_protection(bool do_protect) {
  assert_usable_state();

  flash_lock();

  if (!s_flash_state.enabled) {
    flash_unlock();
    return;
  }

  enable_flash_spi_clock();
  handle_sleep_when_idle_begin();

  flash_write_enable();

  const uint32_t start_addr = FLASH_REGION_SAFE_FIRMWARE_BEGIN;
  const uint32_t end_addr = FLASH_REGION_SAFE_FIRMWARE_END;
  const uint8_t lock_bits = do_protect ? N25QLockBit_SectorWriteLock : 0;
  for (uint32_t addr = start_addr; addr < end_addr; addr += SECTOR_SIZE_BYTES) {
    flash_start_cmd();
    flash_send_and_receive_byte(FLASH_CMD_WRITE_LOCK_REGISTER);
    flash_send_24b_address(addr);
    flash_send_and_receive_byte(lock_bits);
    flash_end_cmd();
  }

  disable_flash_spi_clock();

  flash_unlock();
}*/

status_t flash_impl_write_protect(FlashAddress start_addr, FlashAddress end_addr) {
  flash_impl_use();

  s_write_protect_start = start_addr;
  s_write_protect_end = end_addr;

  flash_write_enable();
  for (uint32_t addr = start_addr; addr < end_addr; addr += SECTOR_SIZE_BYTES) {
    flash_start_cmd();
    flash_send_and_receive_byte(FLASH_CMD_WRITE_LOCK_REGISTER);
    flash_send_24b_address(addr);
    flash_send_and_receive_byte(N25QLockBit_SectorWriteLock);
    flash_end_cmd();
  }

  flash_impl_release();

  return S_SUCCESS;
}

status_t flash_impl_unprotect(void) {
  PBL_ASSERT(s_write_protect_start != 0 && s_write_protect_end != 0, "FUCK");
  flash_impl_use();

  for (uint32_t addr = s_write_protect_start; addr < s_write_protect_end; addr += SECTOR_SIZE_BYTES) {
    flash_start_cmd();
    flash_send_and_receive_byte(FLASH_CMD_WRITE_LOCK_REGISTER);
    flash_send_24b_address(addr);
    flash_send_and_receive_byte(0);
    flash_end_cmd();
  }
  s_write_protect_start = s_write_protect_end = 0;
  flash_impl_release();

  return S_SUCCESS;
}

uint32_t flash_impl_get_typical_sector_erase_duration_ms(void) {
  return 150;
}

uint32_t flash_impl_get_typical_subsector_erase_duration_ms(void) {
  return 50;
}

// TODO: add support for idling GPIOs
void flash_impl_use(void) {
  if (s_flash_num_uses == 0) {
    enable_flash_spi_clock();
  }
  s_flash_num_uses++;
}

void flash_impl_release_many(uint32_t num_locks) {
  PBL_ASSERTN(s_flash_num_uses >= num_locks);
  s_flash_num_uses -= num_locks;
  if (s_flash_num_uses == 0) {
    disable_flash_spi_clock();
  }
}
void flash_impl_release(void) {
  flash_impl_release_many(1);
}