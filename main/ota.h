#pragma once
#include <stdbool.h>

/* Firmware updates from the release manifest (CONFIG_PHONE_OTA_MANIFEST_URL),
 * with rollback (issue #16).
 *
 * Manifest: {"version":"v1.2.3","url":"https://.../c6_phone.bin"}
 * Checked CONFIG_PHONE_OTA_FIRST_CHECK_S after the first Wi-Fi connection,
 * then every CONFIG_PHONE_OTA_CHECK_INTERVAL_H hours, and on ota_check().
 * Installed only on ota_install() (tap on the display), never during a call. */

typedef enum {
    OTA_IDLE,           /* nothing checked yet */
    OTA_CHECKING,
    OTA_UP_TO_DATE,
    OTA_AVAILABLE,      /* version = offered release */
    OTA_INSTALLING,     /* percent 0..100 */
    OTA_REBOOTING,
    OTA_FAILED,         /* msg = reason (ASCII, for the display) */
} ota_state_t;

/* Runs on the OTA task: must only hand the values on (no blocking). */
typedef void (*ota_cb_t)(ota_state_t st, const char *version, int percent, const char *msg);

/* At boot, before anything else that may fail: starts the verify timer if
 * this is a freshly installed firmware, and the OTA task. */
void ota_init(ota_cb_t cb);

/* The firmware works (Wi-Fi + signalling up, or setup mode started): cancel
 * the rollback. No-op for a firmware that is already valid. */
void ota_app_ok(void);

void ota_online(void);      /* Wi-Fi connected: arms the periodic check */
void ota_check(void);       /* check now (button) */
void ota_install(void);     /* install the offered version (button) */

/* Running version for display, without a leading 'v' ("0.2.0"). */
const char *ota_version(void);
