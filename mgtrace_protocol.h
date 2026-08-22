#ifndef MCI_MGTRACE_PROTOCOL_H
#define MCI_MGTRACE_PROTOCOL_H

#define MGTRACE_RPC_ID 0x80000F50
#define MGTRACE_RPC_FN_RUN 1
#define MGTRACE_STAGE_COUNT 4

/*
 * Small diagnostic RPC shared between the EE Inspector and the temporary IOP
 * tracer. It independently exercises the card-side F2/50..53 transform path
 * after a failed SecrDownloadGetKbit(). Nothing is written to the filesystem.
 */
typedef struct MgTracePacket {
    int port;
    int slot;
    unsigned char seed[8];

    int reset_ok;
    int reset_transfer_rc;
    unsigned int reset_stat6c;
    unsigned char reset_id;
    unsigned char reset_status;

    int stage_ok[MGTRACE_STAGE_COUNT];
    int transfer_rc[MGTRACE_STAGE_COUNT];
    unsigned int stat6c[MGTRACE_STAGE_COUNT];
    unsigned char response_id[MGTRACE_STAGE_COUNT];
    unsigned char response_status[MGTRACE_STAGE_COUNT];

    unsigned char transformed[8];
    unsigned char read_checksum;
    unsigned char calculated_checksum;
} MgTracePacket;

#endif /* MCI_MGTRACE_PROTOCOL_H */
