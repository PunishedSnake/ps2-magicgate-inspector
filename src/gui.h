#ifndef MCI_GUI_H
#define MCI_GUI_H

#include "card.h"
#include "magicgate.h"
#include "fmcb_install.h"

typedef enum MciGuiPage {
    MCI_GUI_CARD = 0,
    MCI_GUI_MAGICGATE,
    MCI_GUI_FMCB,
    MCI_GUI_PAGE_COUNT
} MciGuiPage;

typedef enum MciGuiTone {
    MCI_GUI_TONE_INFO = 0,
    MCI_GUI_TONE_SUCCESS,
    MCI_GUI_TONE_WARNING,
    MCI_GUI_TONE_DANGER
} MciGuiTone;

/*
 * Native 640x224 GS frontend.
 *
 * The renderer deliberately follows the hardware-validated 0.4.0
 * fhdb-bootstrap-manager frontend: libdebug establishes the CRT/read-circuit
 * state once through init_scr(), then all application pixels are submitted
 * through libdraw/GIF DMA. 0.3.0 intentionally has no video-mode selector.
 */
int MciGuiInit(void);
int MciGuiReady(void);

void MciGuiRenderDashboard(int selected,
                           MciGuiPage page,
                           const CardReport cards[2],
                           const MagicGateReport magicgate[2],
                           const MagicGateIopStatus *mg_iop,
                           const FmcbMassBackendStatus *mass,
                           const FmcbPackageReport packages[2],
                           int confirm_format,
                           int last_format_rc);

void MciGuiRenderMessage(const char *title,
                         const char *body,
                         const char *footer,
                         MciGuiTone tone);

void MciGuiRenderFatal(const char *title,
                       const char *body,
                       int code);

#endif /* MCI_GUI_H */
