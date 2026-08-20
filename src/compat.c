/* Compatibility helpers for the old PS2DEV v1.0 EE toolchain. */
int DelayThread(int usec)
{
    volatile unsigned int loops;

    if (usec <= 0)
        return 0;

    /* UI throttling only. No memory-card protocol timing depends on this. */
    loops = (unsigned int)usec * 24u;
    while (loops-- != 0u)
        __asm__ __volatile__("nop");

    return 0;
}
