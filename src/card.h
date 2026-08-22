#ifndef MCI_CARD_H
#define MCI_CARD_H

/*
 * Ordinary memory-card filesystem diagnostics.
 *
 * These APIs run only against the normal Sony ROM X memory-card personality.
 * They are intentionally independent of the temporary MagicGate/SECR session
 * so a security-backend experiment cannot redefine basic filesystem health.
 */

typedef enum CardHealth {
    CARD_HEALTH_UNKNOWN = 0,
    CARD_HEALTH_NO_CARD,
    CARD_HEALTH_NOT_PS2,
    CARD_HEALTH_UNFORMATTED,
    CARD_HEALTH_FILESYSTEM_ERROR,
    CARD_HEALTH_RW_ERROR,
    CARD_HEALTH_HEALTHY
} CardHealth;

typedef enum CardRwStage {
    CARD_RW_NOT_RUN = 0,
    CARD_RW_CREATE,
    CARD_RW_WRITE,
    CARD_RW_FLUSH,
    CARD_RW_REOPEN,
    CARD_RW_READ,
    CARD_RW_COMPARE,
    CARD_RW_DELETE,
    CARD_RW_VERIFY_DELETE,
    CARD_RW_DONE
} CardRwStage;

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
    CardRwStage rw_stage;
    CardHealth health;
} CardReport;

void CardInspect(int port, CardReport *report);
int CardFormat(int port, CardReport *report);
const char *CardResultText(int result);
const char *CardTypeText(int type);
const char *CardHealthText(CardHealth health);
const char *CardRwStageText(CardRwStage stage);

#endif /* MCI_CARD_H */
