#ifndef MCI_CARD_H
#define MCI_CARD_H

/*
 * Ordinary memory-card diagnostics run against the normal Sony ROM X stack.
 * MagicGate/KELF capability is intentionally reported through a separate API:
 * a card may have a healthy PS2 filesystem without implementing CardAuth.
 */

typedef enum CardHealth {
    CARD_UNKNOWN = 0,
    CARD_OK,
    CARD_FULL,
    CARD_UNFORMATTED,
    CARD_FILESYSTEM_BROKEN,
    CARD_IO_FAILURE,
    CARD_AUTH_FAILURE,
    CARD_DETECT_FAILURE,
    CARD_NO_CARD
} CardHealth;

typedef enum RwStage {
    RW_NOT_RUN = 0,
    RW_FIND_NAME,
    RW_VERIFY_CARD,
    RW_OPEN_WRITE,
    RW_WRITE,
    RW_FLUSH,
    RW_CLOSE_WRITE,
    RW_OPEN_READ,
    RW_READ,
    RW_CLOSE_READ,
    RW_COMPARE,
    RW_DELETE,
    RW_VERIFY_DELETE,
    RW_DONE
} RwStage;

typedef struct CardReport {
    int port;
    int info_rc;
    int type;
    int free_clusters;
    int formatted;
    int root_rc;
    int rw_rc;
    int cleanup_rc;
    int format_allowed;
    unsigned int test_bytes;
    CardHealth health;
    RwStage rw_stage;
} CardReport;

/* Historical 4 KiB test retained as a convenience wrapper. */
void CardInspect(int port, CardReport *report);
/* 0.4 can exercise more media without allocating a matching EE buffer. The
 * implementation streams deterministic 4 KiB chunks and verifies each chunk
 * after a flush/close/reopen boundary. */
void CardInspectSized(int port, CardReport *report, unsigned int test_bytes);
int CardFormat(int port, CardReport *report);

const char *CardHealthText(CardHealth health);
const char *CardTypeText(int type);
const char *CardRwStageText(RwStage stage);
const char *CardResultText(int rc);

#endif /* MCI_CARD_H */
