#include <stdio.h>
#include <string.h>
#include "version.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main(void)
{
    fw_version_t v;
    CHECK(version_parse("v0.2.0", &v) && v.major == 0 && v.minor == 2 && v.patch == 0 && !v.pre && !v.ahead);
    CHECK(version_parse("1.10.3", &v) && v.major == 1 && v.minor == 10 && v.patch == 3);
    CHECK(version_parse("v0.2.0-3-gabc1234", &v) && v.ahead == 3 && !v.pre);
    CHECK(version_parse("v0.2.0-3-gabc1234-dirty", &v) && v.ahead == 3);
    CHECK(version_parse("v0.2.0-dirty", &v) && v.ahead == 1 && !v.pre);
    CHECK(version_parse("v0.2.0-rc1", &v) && v.pre);
    CHECK(version_parse("v0.2.0+build5", &v) && !v.pre && !v.ahead);
    CHECK(!version_parse("abc1234", &v));
    CHECK(!version_parse("v1.2", &v));
    CHECK(!version_parse("v1.2.", &v));
    CHECK(!version_parse("v1.2.3x", &v));
    CHECK(!version_parse("", &v));
    CHECK(!version_parse(NULL, &v));
    CHECK(!version_parse("v99999999999.0.0", &v));

    /* newer release offered */
    CHECK(version_is_update("v0.1.0", "v0.2.0"));
    CHECK(version_is_update("v0.1.0", "v0.1.1"));
    CHECK(version_is_update("v0.9.9", "v1.0.0"));
    CHECK(version_is_update("v0.1.9", "v0.1.10"));         /* numeric, not lexical */
    CHECK(version_is_update("v0.2.0-rc1", "v0.2.0"));      /* pre-release -> its release */
    CHECK(version_is_update("v0.1.0-4-gdead", "v0.2.0"));  /* dev build after v0.1.0 */
    CHECK(version_is_update("abc1234", "v0.1.0"));         /* build without tags */
    /* not newer */
    CHECK(!version_is_update("v0.2.0", "v0.2.0"));
    CHECK(!version_is_update("v0.2.0", "v0.1.9"));
    CHECK(!version_is_update("v0.2.0-1-gbeef", "v0.2.0")); /* dev build already past it */
    CHECK(!version_is_update("v0.2.0-dirty", "v0.2.0"));
    /* never offered */
    CHECK(!version_is_update("v0.1.0", "v0.3.0-rc1"));
    CHECK(!version_is_update("v0.1.0", "v0.3.0-2-gabc"));
    CHECK(!version_is_update("v0.1.0", "latest"));
    CHECK(!version_is_update("v0.1.0", NULL));

    CHECK(!strcmp(version_display("v0.2.0"), "0.2.0"));
    CHECK(!strcmp(version_display("v0.2.0-3-gabc"), "0.2.0-3-gabc"));
    CHECK(!strcmp(version_display("abc1234"), "abc1234"));
    CHECK(!strcmp(version_display("vx"), "vx"));
    CHECK(!strcmp(version_display(NULL), ""));

    if (failures) { printf("version_test: %d failure(s)\n", failures); return 1; }
    printf("version_test: ok\n");
    return 0;
}
