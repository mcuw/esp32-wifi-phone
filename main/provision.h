#pragma once
#include "esp_err.h"

/* Starts (soft-AP must already be up):
 *   DNS   :53   every name -> this phone (captive portal)
 *   HTTP  :80   static Astro/Qwik site from the "www" SPIFFS partition, plus
 *     GET  /api/info   -> {"id","name","fw"}
 *     GET  /api/scan   -> [{"s":ssid,"r":rssi,"a":authmode}, ...]
 *     POST /api/wifi   <- {"ssid","pass","name"}  -> saves to NVS, reboots
 *     GET  /api/coredump      -> {"present":false} or {"present":true,"size","reason",
 *                                "task","pc","ra","sp","mcause","mtval","elf_sha","same_fw"}
 *     GET  /api/coredump.bin  -> raw core dump image (decrypted), 404 if none
 *     DELETE /api/coredump    -> erases the stored core dump
 */
esp_err_t provision_start(void);
