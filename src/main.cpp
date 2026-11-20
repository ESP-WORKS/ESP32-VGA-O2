#include "fw.h"
#include "vga_host.h"
#include "menu.h"
#include "audio_host.h"

#include <Arduino.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "esp_bt.h"
#include <WiFi.h>

static TaskHandle_t s_emu = nullptr;

void setup() {
  const esp_partition_t *otadata = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
  if (otadata)
    esp_partition_erase_range(otadata, 0, otadata->size);

  setCpuFrequencyMhz(240);
  Serial.begin(115200);
  delay(50);
  Serial.println(FW_BANNER);

  WiFi.mode(WIFI_OFF);
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);

  /* VGA ISR starveia IDLE1. SD.begin no core 0 starveia IDLE0 (~10 s abort
   * no FW 1). Desliga o TWDT nas duas IDLES e no driver. */
  disableCore0WDT();
  disableCore1WDT();
  TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCPU(0);
  if (idle0)
    esp_task_wdt_delete(idle0);
  TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCPU(1);
  if (idle1)
    esp_task_wdt_delete(idle1);
  esp_task_wdt_deinit();

  /* VGA ISR lives on this core (Arduino setup = core 1). Allocate AFTER setResolution. */
  vgaInit();

  /* 16 KB: FAT/SD + String no parse de bootl.rc estouravam 8 KB (backtrace
   * CORRUPTED). Scan de ROM continua dentro do emuTask, depois do VGA. */
  /* 12 KB: leaves ~4 KB more heap for the 250-line VDC slab vs 16 KB. */
  xTaskCreatePinnedToCore(emuTask, "emu", 12288, nullptr, 5, &s_emu, 0);
}

void loop() {
  /* Core 1: empty. VGA ISR only. No blit, no SD, no emu. */
  vTaskDelay(pdMS_TO_TICKS(1000));
}
