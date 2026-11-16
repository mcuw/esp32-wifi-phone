#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The setup page (web/dist), embedded into the app by tools/pack_web.py. */
typedef struct {
    const char    *uri;         /* "/index.html", "/build/q-xyz.js", ... */
    const char    *mime;
    const uint8_t *data;
    size_t         size;
    bool           gzip;        /* data is gzip: send Content-Encoding: gzip */
} web_file_t;

extern const web_file_t web_files[];   /* terminated by an entry with uri == NULL */
extern const size_t     web_files_count;
