#include <unistd.h>

/* PS2SDK v1.0 does not export DelayThread(), while newer code often uses it.
 * Keep the standalone inspector source portable by providing the tiny shim here. */
int DelayThread(int usec)
{
    return usleep(usec);
}
