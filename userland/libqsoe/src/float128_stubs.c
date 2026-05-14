/*
 * float128_stubs.c — placeholders for the libgcc soft-float
 * routines (v0.5.1).
 *
 * QSOE builds with `-mabi=lp64` (soft-float). The cross-compiler's
 * /usr/lib/gcc-cross/riscv64-linux-gnu/.../libgcc.a is built for
 * `-mabi=lp64d` (double-float) and refuses to link into our binaries.
 *
 * The good news: musl's vfprintf statically references TF (float128
 * = long double) helpers, but only the %f / %Lf / %g / %e code paths
 * actually call them. printf("%d", n) or printf("%s", str) never
 * touches a TF op at runtime.
 *
 * So we provide empty aliases that satisfy the linker. If a binary
 * accidentally pulls a %f format, the trap below catches it instead
 * of corrupting memory. A real soft-float libgcc replacement is
 * v0.6+ work (compile compiler-rt's builtins for lp64, or switch
 * the whole project to lp64d once we want hardware FPU access).
 */

__attribute__((noreturn))
static void qsoe_softfloat_unimpl(void)
{
    /* %f / %Lf in printf is not supported in v0.5.1. If reached,
     * spin so the symptom is "hang" rather than a memory smash. */
    while (1) ;
}

/* The alias must inherit qsoe_softfloat_unimpl's noreturn attribute
 * explicitly — gcc warns under -Wmissing-attributes otherwise. */
#define STUB(name) \
    __attribute__((noreturn)) \
    void name(void) __attribute__((weak, alias("qsoe_softfloat_unimpl")))

STUB(__addtf3);
STUB(__subtf3);
STUB(__multf3);
STUB(__divtf3);
STUB(__netf2);
STUB(__eqtf2);
STUB(__gttf2);
STUB(__lttf2);
STUB(__getf2);
STUB(__letf2);
STUB(__fixtfsi);
STUB(__fixtfdi);
STUB(__fixunstfsi);
STUB(__fixunstfdi);
STUB(__floatsitf);
STUB(__floatditf);
STUB(__floatunsitf);
STUB(__floatunditf);
STUB(__extenddftf2);
STUB(__extendsftf2);
STUB(__trunctfdf2);
STUB(__trunctfsf2);
STUB(__unordtf2);
