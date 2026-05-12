/*
 * taskman — central system server (QSOE).
 *
 * v0.1: this is the seed. Currently it is loaded by the seL4 kernel as the
 * rootserver and immediately enters an idle loop. Subsequent iterations will
 * grow this into the full process/memory/path manager.
 */

int main(void)
{
    for (;;) {
        __asm__ volatile("nop");
    }
    return 0;
}
