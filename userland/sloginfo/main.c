/*
 * sloginfo — dump QSOE system-log events.
 *
 * Opens /dev/slog read-only, drains events, prints them in a human-
 * readable form.  Mirrors QRV's sloginfo utility (clean-room, no
 * QRV code copied).
 *
 * Output format per event:
 *   [time_us]  SEV  major.minor  pid=N  text
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <qsoe-system.h>
#include <sys/slog.h>

static const char *sev_str(unsigned sev)
{
    static const char *names[] = {
        "SHTDN", "CRIT ", "ERROR", "WARN ", "NOTE ", "INFO ", "DBG1 ", "DBG2 "
    };
    return names[sev & 0x7];
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    int fd = open("/dev/slog", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "sloginfo: open(/dev/slog) failed: errno=%d\n",
                qsoe_errno);
        return 1;
    }

    /* Drain one batch (up to ~900 bytes per read). */
    unsigned char buf[928];
    long n = read(fd, buf, sizeof buf);
    if (n < 0) {
        fprintf(stderr, "sloginfo: read failed: errno=%d\n", qsoe_errno);
        close(fd);
        return 1;
    }
    if (n == 0) {
        printf("(slog ring is empty)\n");
        close(fd);
        return 0;
    }

    /* Walk events back-to-back. */
    unsigned long off = 0;
    while (off + sizeof(qsoe_slog_event_t) <= (unsigned long)n) {
        qsoe_slog_event_t *h = (qsoe_slog_event_t *)&buf[off];
        if (h->magic != QSOE_SLOG_MAGIC) {
            fprintf(stderr, "sloginfo: bad magic 0x%x at off=%lu\n",
                    (unsigned)h->magic, off);
            break;
        }
        unsigned major = _SLOG_GETMAJOR(h->code);
        unsigned minor = _SLOG_GETMINOR(h->code);
        printf("[%12lu us]  %s  %u.%u  pid=%u  ",
               (unsigned long)h->time_us, sev_str(h->severity),
               major, minor, (unsigned)h->pid);
        fflush(stdout);
        if ((h->flags & QSOE_SLOG_FLAG_TEXT) && h->paylen > 0) {
            char *text = (char *)(h + 1);
            write(1, text, h->paylen);
        } else if (h->paylen > 0) {
            printf("<%u bytes binary>", (unsigned)h->paylen);
        }
        write(1, "\n", 1);
        off += sizeof(qsoe_slog_event_t) + h->paylen;
    }

    close(fd);
    return 0;
}
