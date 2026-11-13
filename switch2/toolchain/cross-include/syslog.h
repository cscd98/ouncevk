/* SPDX-License-Identifier: MIT */

/*
 * Stub <syslog.h> for Horizon.
 * Horizon has no system log daemon. Every call does nothing.
 */

#ifndef SWITCH_CROSS_SYSLOG_H
#define SWITCH_CROSS_SYSLOG_H

/* Priorities. */
#define LOG_EMERG 0
#define LOG_ALERT 1
#define LOG_CRIT 2
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_NOTICE 5
#define LOG_INFO 6
#define LOG_DEBUG 7

/* Options for openlog. */
#define LOG_PID 1
#define LOG_CONS 2
#define LOG_ODELAY 4
#define LOG_NDELAY 8
#define LOG_NOWAIT 0x10
#define LOG_PERROR 0x20

/* Facilities. */
#define LOG_KERN 0
#define LOG_USER (1 << 3)
#define LOG_DAEMON (3 << 3)
#define LOG_LOCAL0 (16 << 3)

#ifdef __cplusplus
extern "C" {
#endif

static inline void openlog(const char *ident, int option, int facility)
{
    (void)ident;
    (void)option;
    (void)facility;
}

static inline void syslog(int priority, const char *format, ...)
{
    (void)priority;
    (void)format;
}

static inline void closelog(void)
{
}

#ifdef __cplusplus
}
#endif

#endif /* SWITCH_CROSS_SYSLOG_H */
