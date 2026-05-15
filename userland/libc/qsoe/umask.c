/*
 * umask.c — POSIX umask().
 *
 * The mask lives per-process in taskman's tm_process_t.umask.
 * Wire op TM_REQ_UMASK is "atomic exchange": MR0 holds the new
 * mask (or -1 for query-only); reply MR0 holds the old mask.
 * POSIX has no "get current" entry point, so the GNU /
 * (and de-facto common) practice is `umask(022); umask(old);`
 * round-trips; we expose it as a single IPC by passing the
 * caller's intended set value through unchanged.
 */

#include <sys/stat.h>
#include <qsoe/qrv.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include <sel4_types.h>
#include <qsoe_invoke.h>

mode_t umask(mode_t mask)
{
    seL4_Word mr0 = (seL4_Word)(mask & 0777u);
    seL4_Word mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_UMASK, 0, 0, 1);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    /* POSIX defines no failure mode for umask; if taskman returns
     * an error (process unknown — shouldn't happen) we fall back to
     * the input value so callers see "no-op" rather than a stale
     * value.  errno isn't set per spec. */
    if (err != 0) return mask;
    return (mode_t)(mr0 & 0777u);
}
