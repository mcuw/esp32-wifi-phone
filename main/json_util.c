#include "json_util.h"
#include <stdio.h>
#include <string.h>

void json_escape(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in && o + 7 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c < 0x20)         { o += snprintf(out + o, n - o, "\\u%04x", c); }
        else                       { out[o++] = c; }
    }
    out[o] = 0;
}

/* Minimal extractor for flat {"key":"string"} objects. */
bool json_get_str(const char *json, const char *key, char *out, size_t n)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    while (*++p == ' ') {}
    if (*p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < n) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) { case 'n': out[o++] = '\n'; break; case 't': out[o++] = '\t'; break;
                          case 'u': out[o++] = '?'; p += 4; break;       /* \uXXXX unsupported */
                          default:  out[o++] = *p; }
            p++;
        } else out[o++] = *p++;
    }
    out[o] = 0;
    return *p == '"';
}
