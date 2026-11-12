#pragma once
#include "esp_err.h"

/* Starts (soft-AP must already be up):
 *   DNS   :53   every name -> this phone (captive portal)
 *   HTTP  :80   static Astro/Qwik site from the "www" SPIFFS partition, plus
 *     GET  /api/info   -> {"id","name","fw"}
 *     GET  /api/scan   -> [{"s":ssid,"r":rssi,"a":authmode}, ...]
 *     POST /api/wifi   <- {"ssid","pass","name"}  -> saves to NVS, reboots
 */
esp_err_t provision_start(void);
