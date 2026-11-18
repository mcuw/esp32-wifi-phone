#include "version.h"
#include <ctype.h>
#include <stdlib.h>

static bool num(const char **p, int *out)
{
    if (!isdigit((unsigned char)**p)) return false;
    long n = 0;
    while (isdigit((unsigned char)**p)) {
        n = n * 10 + (**p - '0');
        if (n > 1000000) return false;
        (*p)++;
    }
    *out = (int)n;
    return true;
}

bool version_parse(const char *s, fw_version_t *v)
{
    if (!s || !v) return false;
    *v = (fw_version_t){ 0 };
    const char *p = s;
    if (*p == 'v' || *p == 'V') p++;
    if (!num(&p, &v->major) || *p++ != '.') return false;
    if (!num(&p, &v->minor) || *p++ != '.') return false;
    if (!num(&p, &v->patch)) return false;
    if (*p == 0) return true;
    if (*p != '-' && *p != '+') return false;
    if (*p == '+') return true;                        /* build metadata only */

    /* "-<n>-g<hash>" (git describe) or a pre-release label ("-rc1") */
    const char *q = p + 1;
    int n;
    if (num(&q, &n) && q[0] == '-' && q[1] == 'g') { v->ahead = n; return true; }
    if (p[1] == 'd' && p[2] == 'i' && p[3] == 'r' && p[4] == 't' && p[5] == 'y' && p[6] == 0) {
        v->ahead = 1;                                  /* "v1.2.3-dirty": local changes */
        return true;
    }
    v->pre = true;
    return true;
}

int version_cmp(const fw_version_t *a, const fw_version_t *b)
{
    if (a->major != b->major) return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    if (a->pre != b->pre) return a->pre ? -1 : 1;
    if (a->ahead != b->ahead) return a->ahead < b->ahead ? -1 : 1;
    return 0;
}

bool version_is_update(const char *running, const char *offered)
{
    fw_version_t r, o;
    if (!version_parse(offered, &o) || o.pre || o.ahead) return false;
    /* An unparseable running version (e.g. a bare commit hash from a build
     * without tags) accepts any release. */
    if (!version_parse(running, &r)) return true;
    return version_cmp(&o, &r) > 0;
}

const char *version_display(const char *s)
{
    if (s && (s[0] == 'v' || s[0] == 'V') && isdigit((unsigned char)s[1])) return s + 1;
    return s ? s : "";
}
