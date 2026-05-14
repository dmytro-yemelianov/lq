/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * qsh_error_t — explicit error propagation.
 *
 * v0.6.3 replaces upstream mksh's setjmp/longjmp-based control flow
 * with explicit return codes.  Every function that can fail or
 * non-locally exit returns a `qsh_error_t`; callers check the value
 * (typically via the QSH_TRY() macro) and propagate non-OK values up
 * the call stack until a designated handler turns them back into a
 * shell-visible event (re-prompt, exit, etc.).
 *
 * Why no setjmp/longjmp:
 *  - seL4 has no portable jmp_buf semantics that interact correctly
 *    with our pthread-less environment.
 *  - musl provides setjmp/longjmp but they don't compose with our
 *    multi-thread (main + signal thread) layout.
 *  - Cooperative-polling for SIGINT (signal thread sets a flag, main
 *    thread reads it at safe points) is incompatible with longjmp's
 *    cross-frame teleportation.
 *
 * Migration from the mksh unwind() codes (LBREAK / LCONTIN / LERROR
 * / LEXIT / LINTR / LLEAVE / LRETURN / LSHELL / LERREXT) is in
 * progress; see doc/tmp/qsh.longjmp-map.md.
 */
#ifndef QSH_ERROR_H
#define QSH_ERROR_H

typedef enum {
    QSH_OK = 0,             /* no error; command completed normally */

    /* Parse / lex layer */
    QSH_PARSE_ERR,          /* syntax error during yylex/compile */

    /* Word-expansion layer */
    QSH_EXPAND_ERR,         /* substitute / glob / arithmetic failed */
    QSH_REDIR_ERR,          /* I/O redirection setup failed */

    /* Execution layer */
    QSH_EXEC_ERR,           /* command not found / exec failed */
    QSH_BUILTIN_ERR,        /* builtin returned non-zero status */

    /* Control flow (used to be LBREAK / LCONTIN / LRETURN) */
    QSH_BREAK,              /* break N — see qsh_break_depth */
    QSH_CONTINUE,           /* continue N — see qsh_break_depth */
    QSH_RETURN,             /* return from function */

    /* Top-level events */
    QSH_QUIT,               /* exit / EOF on tty (LEXIT) */
    QSH_RESTART,            /* re-enter main prompt loop (LSHELL) */
    QSH_FATAL               /* unrecoverable; clean up and abort */
} qsh_error_t;

/* For BREAK / CONTINUE codes, the loop depth is carried in a side
 * channel rather than encoded in the enum value.  The producer sets
 * it; the loop driver consumes it.  Single-threaded main thread, so
 * a plain global is fine. */
extern unsigned int qsh_break_depth;

/* Standard pattern: propagate non-OK from a nested call.
 *
 *     QSH_TRY(expand_one_word(w, &result));
 *
 * is equivalent to
 *
 *     {
 *         qsh_error_t _e = expand_one_word(w, &result);
 *         if (_e != QSH_OK) return _e;
 *     }
 */
#define QSH_TRY(expr)                       \
    do {                                    \
        qsh_error_t _qsh_try_e = (expr);    \
        if (_qsh_try_e != QSH_OK)           \
            return _qsh_try_e;              \
    } while (0)

/*
 * NOTE on signals/interrupts:
 * QSOE delivers signals as pulses to a per-process system thread
 * that calls the registered handler directly.  Main-thread polling
 * (`QSH_INTERRUPTED`, `QSH_CHECK_INTR`, qsh_interrupt_pending) is
 * NOT needed in v0.6.3:
 *   - Ctrl-C is a keystroke handled by edit.c (line discipline
 *     lives in qsh, not the driver).
 *   - All other long blocks live in MsgReceive on a channel; the
 *     handler in the system thread can wake them via pulse if a
 *     specific case ever needs it (not yet).
 *   - Short CPU-bound work doesn't need interruption.
 * If a future use case appears, polling can be added as a focused
 * follow-up without changing the qsh_error_t taxonomy.
 */

/* Convert an internal qsh_error_t to a shell exit-status integer.
 * The mapping is informational; the top-level loop uses it to set
 * $? after each command. */
int qsh_error_to_status(qsh_error_t e);

#endif /* QSH_ERROR_H */
