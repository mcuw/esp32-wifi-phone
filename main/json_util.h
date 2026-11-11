#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Escapes '"', '\\' and control characters for use inside a JSON string. */
void json_escape(const char *in, char *out, size_t n);

/* Minimal extractor for flat {"key":"string"} objects (what the setup page sends). */
bool json_get_str(const char *json, const char *key, char *out, size_t n);
