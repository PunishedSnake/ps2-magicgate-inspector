#ifndef MCI_GUI_H
#define MCI_GUI_H

#include "card.h"
#include "magicgate.h"
#include "fmcb_install.h"
#include "settings.h"

typedef enum MciGuiPage {
    MCI_GUI_CARD = 0,
    MCI_GUI_MAGICGATE,
    MCI_GUI_FMCB,
    MCI_GUI_SETTINGS,
    MCI_GUI_PAGE_COUNT
} MciGuiPage;

typedef enum MciGuiTone {
    MCI_GUI_TONE_INFO = 0,
    MCI_GUI_TONE_SUCCESS,
    MCI_GUI_TONE_WARNING,
    MCI_GUI_TONE_DANGER
} MciGuiTone;

/*
 * 0.4 keeps a 640x224 logical UI but can map it onto the hardware-tested
 * display surfaces from fhdb-bootstrap-manager 0.4.3. Native is always the
 * recovery mode; progressive/HD modes are runtime-selectable from Settings.
 */
int MciGuiInit(void);
int MciGuiReady(void);
int MciGuiNeedsAnimation(void);
int MciGuiApplyVideoMode(MciVideoMode mode);
MciVideoMode MciGuiCurrentVideoMode(void);

void MciGuiRenderDashboard(int selected,
                           MciGuiPage page,
                           const CardReport cards[2],
                           const MagicGateReport magicgate[2],
                           const MagicGateIopStatus *mg_iop,
                           const FmcbMassBackendStatus *mass,
                           const FmcbPackageReport packages[2],
                           const MciSettings *settings,
                           int settings_row,
                           int last_video_rc,
                           int confirm_format,
                           int last_format_rc);

void MciGuiRenderMessage(const char *title,
                         const char *body,
                         const char *footer,
                         MciGuiTone tone);

void MciGuiRenderProgress(const char *title,
                          const char *action,
                          const char *detail,
                          int percent,
                          const char *footer,
                          MciGuiTone tone);

void MciGuiRenderFatal(const char *title,
                       const char *body,
                       int code);

#endif /* MCI_GUI_H */
