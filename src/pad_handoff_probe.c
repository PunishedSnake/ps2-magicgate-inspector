/* SPDX-License-Identifier: MIT */
/*
 * PAD handoff forensic probe.
 *
 * Intentionally does NOT reboot the IOP and does NOT load PADMAN/SIO2MAN.
 * It binds to the controller service inherited from the launcher so we can
 * compare FMCB handoff state between installations without first erasing it.
 */

#include <tamtypes.h>
#include <kernel.h>
#include <delaythread.h>
#include <sifrpc.h>
#include <libpad.h>
#include <debug.h>
#include <stdio.h>
#include <string.h>

static unsigned char PadBuf[256] __attribute__((aligned(64)));

typedef struct PadSnapshot {
    int pad_init_rc;
    int mod_version;
    int port_max;
    int slot_max;
    int open_rc;
    int state;
    int mode_count;
    int mode_cur_id;
    int mode_cur_exid;
    int mode_cur_offs;
    int press_mode;
    int button_mask;
    int mode_table[8];
    int read_rc;
    unsigned int buttons;
    unsigned int pad_mode_byte;
} PadSnapshot;

static int wait_ready(int port, int slot, unsigned int max_tries)
{
    unsigned int i;
    int state = -1;

    for (i = 0; i < max_tries; i++) {
        state = padGetState(port, slot);
        if (state == PAD_STATE_STABLE ||
            state == PAD_STATE_FINDCTP1 ||
            state == PAD_STATE_DISCONN)
            return state;
        DelayThread(1000);
    }
    return state;
}

static void take_snapshot(PadSnapshot *s, int already_open)
{
    struct padButtonStatus buttons;
    int i;

    memset(s, 0, sizeof(*s));
    s->pad_init_rc = 1;
    s->mod_version = padGetModVersion();
    s->port_max = padGetPortMax();
    s->slot_max = padGetSlotMax(0);
    s->open_rc = already_open;

    s->state = wait_ready(0, 0, 2000);
    s->mode_count = padInfoMode(0, 0, PAD_MODETABLE, -1);
    s->mode_cur_id = padInfoMode(0, 0, PAD_MODECURID, 0);
    s->mode_cur_exid = padInfoMode(0, 0, PAD_MODECUREXID, 0);
    s->mode_cur_offs = padInfoMode(0, 0, PAD_MODECUROFFS, 0);
    s->press_mode = padInfoPressMode(0, 0);
    s->button_mask = padGetButtonMask(0, 0);

    for (i = 0; i < 8; i++)
        s->mode_table[i] = (i < s->mode_count)
                               ? padInfoMode(0, 0, PAD_MODETABLE, i)
                               : -1;

    memset(&buttons, 0, sizeof(buttons));
    s->read_rc = padRead(0, 0, &buttons);
    s->buttons = 0xffffu ^ buttons.btns;
    s->pad_mode_byte = buttons.mode;
}

static void draw(const PadSnapshot *s, const char *last_action)
{
    int i;

    scr_clear();
    scr_setXY(0, 0);
    scr_printf("PAD HANDOFF FORENSICS - NO IOP RESET\n");
    scr_printf("-----------------------------------\n");
    scr_printf("Launch this ELF DIRECTLY from FMCB on both cards.\n");
    scr_printf("Do not launch it through wLaunchELF for the baseline.\n\n");

    scr_printf("padInit      : %d\n", s->pad_init_rc);
    scr_printf("PADMAN ver   : 0x%08X\n", s->mod_version);
    scr_printf("port/slot max: %d / %d\n", s->port_max, s->slot_max);
    scr_printf("padPortOpen  : %d\n", s->open_rc);
    scr_printf("state        : 0x%02X\n", s->state);
    scr_printf("mode count   : %d\n", s->mode_count);
    scr_printf("mode cur id  : %d\n", s->mode_cur_id);
    scr_printf("mode cur exid: %d\n", s->mode_cur_exid);
    scr_printf("mode cur offs: %d\n", s->mode_cur_offs);
    scr_printf("press mode   : %d\n", s->press_mode);
    scr_printf("button mask  : 0x%08X\n", s->button_mask);
    scr_printf("padRead      : %d  mode byte: 0x%02X\n",
               s->read_rc, s->pad_mode_byte);
    scr_printf("buttons      : 0x%04X\n", s->buttons & 0xffffu);

    scr_printf("mode table   :");
    for (i = 0; i < s->mode_count && i < 8; i++)
        scr_printf(" %d", s->mode_table[i]);
    scr_printf("\n\n");

    scr_printf("CROSS  refresh snapshot\n");
    scr_printf("L1+R1+SQUARE  normalize to DIGITAL + UNLOCK + exit pressure\n");
    scr_printf("RESET/power off to leave probe\n\n");
    scr_printf("Last action: %s\n", last_action != NULL ? last_action : "baseline");
}

static int normalize_pad(void)
{
    int rc1, rc2;

    rc1 = padExitPressMode(0, 0);
    wait_ready(0, 0, 2000);
    rc2 = padSetMainMode(0, 0, PAD_MMODE_DIGITAL, PAD_MMODE_UNLOCK);
    wait_ready(0, 0, 2000);

    return (rc1 == 1 && rc2 == 1) ? 1 : 0;
}

int main(int argc, char **argv)
{
    PadSnapshot snap;
    struct padButtonStatus buttons;
    unsigned int old_buttons = 0;
    unsigned int now_buttons;
    unsigned int pressed;
    int pad_init_rc;
    int open_rc;
    char action[96];

    (void)argc;
    (void)argv;

    init_scr();
    SifInitRpc(0);

    pad_init_rc = padInit(0);
    if (pad_init_rc <= 0) {
        scr_printf("PAD HANDOFF FORENSICS\n\npadInit failed: %d\n", pad_init_rc);
        scr_printf("This probe deliberately does not load/reset PADMAN.\n");
        scr_printf("The inherited launcher environment has no usable PAD RPC server.\n");
        SleepThread();
    }

    memset(PadBuf, 0, sizeof(PadBuf));
    open_rc = padPortOpen(0, 0, PadBuf);
    if (open_rc == 0) {
        scr_printf("PAD HANDOFF FORENSICS\n\npadPortOpen(0,0) failed.\n");
        SleepThread();
    }

    wait_ready(0, 0, 2000);
    take_snapshot(&snap, open_rc);
    snap.pad_init_rc = pad_init_rc;
    snprintf(action, sizeof(action), "baseline captured without IOP reset");
    draw(&snap, action);

    for (;;) {
        memset(&buttons, 0, sizeof(buttons));
        if (padRead(0, 0, &buttons) != 0)
            now_buttons = 0xffffu ^ buttons.btns;
        else
            now_buttons = 0;

        pressed = now_buttons & ~old_buttons;
        old_buttons = now_buttons;

        if (pressed & PAD_CROSS) {
            take_snapshot(&snap, open_rc);
            snap.pad_init_rc = pad_init_rc;
            snprintf(action, sizeof(action), "snapshot refreshed");
            draw(&snap, action);
        }

        if ((pressed & PAD_SQUARE) &&
            (now_buttons & PAD_L1) &&
            (now_buttons & PAD_R1)) {
            int rc = normalize_pad();
            take_snapshot(&snap, open_rc);
            snap.pad_init_rc = pad_init_rc;
            snprintf(action, sizeof(action),
                     "normalize rc=%d (exit pressure, DIGITAL, UNLOCK)", rc);
            draw(&snap, action);
        }

        DelayThread(16000);
    }

    return 0;
}
