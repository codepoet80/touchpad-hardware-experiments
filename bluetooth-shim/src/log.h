/*
 * log.h -- tiny logger.  Writes to stderr (captured by PmBtEngine's upstart log)
 * and, if WEBOS_BT_SHIM_LOG names a path, appends there too.  All output is
 * prefixed [btshim] so it is greppable next to the stock [HIDH] lines.
 *
 * Verbosity:
 *   shim_log()  -- always (init, device create/destroy, errors)
 *   shim_dbg()  -- only when WEBOS_BT_SHIM_DUMP=1 (descriptor + per-report dumps)
 */
#ifndef WEBOS_BT_SHIM_LOG_H
#define WEBOS_BT_SHIM_LOG_H

extern int  g_shim_dump;                 /* set from WEBOS_BT_SHIM_DUMP */
void shim_log(const char *fmt, ...);
void shim_dbg(const char *fmt, ...);
void shim_hexdump(const char *tag, const unsigned char *p, int len);

#endif
