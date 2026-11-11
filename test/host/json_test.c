#include <stdio.h>
#include <string.h>
#include "json_util.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main(void)
{
    char ssid[33], pass[65], name[25], out[80];

    /* exactly what JSON.stringify produces for tricky input */
    const char *j = "{\"ssid\":\"Caf\xc3\xa9 \\\"X\\\"\",\"pass\":\"p\\\\w\\\"d\",\"name\":\"Anna\"}";
    CHECK(json_get_str(j, "ssid", ssid, sizeof ssid));
    CHECK(json_get_str(j, "pass", pass, sizeof pass));
    CHECK(json_get_str(j, "name", name, sizeof name));
    CHECK(!strcmp(ssid, "Caf\xc3\xa9 \"X\""));
    CHECK(!strcmp(pass, "p\\w\"d"));
    CHECK(!strcmp(name, "Anna"));

    CHECK(!json_get_str("{\"a\":\"b\"}", "ssid", ssid, sizeof ssid));      /* missing key */
    CHECK(!json_get_str("{\"ssid\":12}", "ssid", ssid, sizeof ssid));      /* not a string */
    CHECK(!json_get_str("{\"ssid\":\"abc", "ssid", ssid, sizeof ssid));    /* unterminated */

    char small[4];
    json_get_str("{\"k\":\"abcdefgh\"}", "k", small, sizeof small);        /* must not overflow */
    CHECK(strlen(small) == 3);

    json_escape("a\"b\\c\n", out, sizeof out);
    CHECK(!strcmp(out, "a\\\"b\\\\c\\u000a"));

    printf("json: %s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
