#define _GNU_SOURCE
#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int g_shim_dump = 0;
static FILE *g_logfile = NULL;

static void ensure_logfile(void)
{
    static int tried = 0;
    const char *path;
    if (tried) return;
    tried = 1;
    /* Default to a fixed path so logging works even when the process was spawned
     * without our env (e.g. ls-hubd/dbus activation of PmBtEngine). */
    path = getenv("WEBOS_BT_SHIM_LOG");
    if (!path || !*path) path = "/var/log/btshim.log";
    g_logfile = fopen(path, "a");
}

static void vout(const char *level, const char *fmt, va_list ap)
{
    char buf[1024];
    int n;
    va_list ap2;

    n = snprintf(buf, sizeof(buf), "[btshim] %s: ", level);
    va_copy(ap2, ap);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap2);
    va_end(ap2);

    fprintf(stderr, "%s\n", buf);
    fflush(stderr);

    ensure_logfile();
    if (g_logfile) {
        fprintf(g_logfile, "%s\n", buf);
        fflush(g_logfile);
    }
}

void shim_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vout("info", fmt, ap);
    va_end(ap);
}

void shim_dbg(const char *fmt, ...)
{
    va_list ap;
    if (!g_shim_dump) return;
    va_start(ap, fmt);
    vout("dbg", fmt, ap);
    va_end(ap);
}

void shim_hexdump(const char *tag, const unsigned char *p, int len)
{
    char line[128];
    int i, col, off;
    if (!g_shim_dump) return;
    shim_dbg("%s (%d bytes):", tag, len);
    for (i = 0; i < len; i += 16) {
        off = snprintf(line, sizeof(line), "  %04x: ", i);
        for (col = 0; col < 16 && i + col < len; col++)
            off += snprintf(line + off, sizeof(line) - off, "%02x ", p[i + col]);
        shim_dbg("%s", line);
    }
}
