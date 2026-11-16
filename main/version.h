#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Firmware versions come from the git tag (esp_app_desc_t.version, set by
 * ESP-IDF from version.txt or `git describe`): "v0.2.0", or between two
 * releases "v0.2.0-3-gabc1234" (3 commits after v0.2.0), maybe "-dirty".
 * Pure C, no ESP-IDF - tested on the host (test/host/version_test.c). */

typedef struct {
    int  major, minor, patch;
    bool pre;                   /* "-rc1" etc.: a pre-release of major.minor.patch */
    int  ahead;                 /* "-3-gabc": commits after the tag, 0 for a release */
} fw_version_t;

/* Parses "[v]MAJOR.MINOR.PATCH[-...]". false if no X.Y.Z at the start. */
bool version_parse(const char *s, fw_version_t *v);

/* <0, 0, >0 like strcmp. A pre-release sorts before its release, a build
 * after a tag ("-3-gabc") after that release. */
int  version_cmp(const fw_version_t *a, const fw_version_t *b);

/* True if 'offered' is a strictly newer release than 'running'. Pre-releases
 * and unparseable strings are never offered. */
bool version_is_update(const char *running, const char *offered);

/* "v0.2.0" -> "0.2.0" (for display and /api/info). */
const char *version_display(const char *s);
