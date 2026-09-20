#ifndef MCI_CARD_RAW_SESSION_H
#define MCI_CARD_RAW_SESSION_H

typedef struct MciRawCardSessionStatus {
    int sio2_rc;
    int pad_rc;
    int mcman_rc;
    int mcserv_rc;
    int iomanx_rc;
    int filexio_module_rc;
    int usbd_rc;
    int usbhdfsd_rc;
    int mcinit_rc;
    int mcinfo_issue_rc;
    int mcinfo_sync_rc;
    int mcinfo_result;
    int card_type;
    int free_clusters;
    int formatted;
    int filexio_init_rc;
    int ready;
} MciRawCardSessionStatus;

void MciRawCardSessionReset(MciRawCardSessionStatus *status);
int MciRawCardSessionStart(MciRawCardSessionStatus *status);
void MciRawCardSessionStop(MciRawCardSessionStatus *status);

#endif /* MCI_CARD_RAW_SESSION_H */
