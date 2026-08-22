#include <types.h>
#include <irx.h>
#include <sifman.h>
#include <sifcmd.h>
#include <thbase.h>
#include <sysclib.h>
#include <sio2man.h>

#include "../../mgtrace_protocol.h"

IRX_ID("mgtrace", 1, 0);

static SifRpcDataQueue_t TraceQueue;
static SifRpcServerData_t TraceServer;
static int TraceThreadId;
static unsigned char TraceServerBuffer[256] __attribute__((aligned(16)));

static unsigned char TraceChecksum(const unsigned char *buffer, int length)
{
    unsigned char checksum = 0;
    int i;

    for (i = 0; i < length; i++)
        checksum ^= buffer[i];
    return checksum;
}

static int TraceStatusOk(unsigned int stat6c)
{
    return (((stat6c >> 13) & 1) == 0) && (((stat6c >> 14) & 3) == 0);
}

static void SetupPacket(sio2_transfer_data_t *td, int port,
                        unsigned char *wrbuf, unsigned char *rdbuf,
                        int size, unsigned int regdata)
{
    memset(td, 0, sizeof(*td));
    memset(wrbuf, 0, size);
    memset(rdbuf, 0, size);

    td->in = wrbuf;
    td->out = rdbuf;
    td->port_ctrl1[port] = 0xFF060505;
    td->port_ctrl2[port] = 0x0003FFFF;
    td->regdata[0] = (port & 3) | regdata;
    td->regdata[1] = 0;
    td->in_size = size;
    td->out_size = size;
    td->in_dma.addr = NULL;
    td->out_dma.addr = NULL;
}

static int RunReset(int port, MgTracePacket *packet)
{
    sio2_transfer_data_t td;
    unsigned char wrbuf[5];
    unsigned char rdbuf[5];
    int rc;

    SetupPacket(&td, port, wrbuf, rdbuf, sizeof(wrbuf), 0x00140540);
    wrbuf[0] = 0x81;
    wrbuf[1] = 0xF3;

    sio2_mc_transfer_init();
    rc = sio2_transfer(&td);

    packet->reset_transfer_rc = rc;
    packet->reset_stat6c = td.stat6c;
    packet->reset_id = rdbuf[3];
    packet->reset_status = rdbuf[4];
    packet->reset_ok = (rc != 0 && TraceStatusOk(td.stat6c) &&
                        rdbuf[3] == 0x2B && rdbuf[4] != 0x66);
    return packet->reset_ok;
}

static int RunSimple(int port, int index, unsigned char command,
                     MgTracePacket *packet)
{
    sio2_transfer_data_t td;
    unsigned char wrbuf[5];
    unsigned char rdbuf[5];
    int rc;

    SetupPacket(&td, port, wrbuf, rdbuf, sizeof(wrbuf), 0x00140540);
    wrbuf[0] = 0x81;
    wrbuf[1] = 0xF2;
    wrbuf[2] = command;

    sio2_mc_transfer_init();
    rc = sio2_transfer(&td);

    packet->transfer_rc[index] = rc;
    packet->stat6c[index] = td.stat6c;
    packet->response_id[index] = rdbuf[3];
    packet->response_status[index] = rdbuf[4];
    packet->stage_ok[index] = (rc != 0 && TraceStatusOk(td.stat6c) &&
                               rdbuf[3] == 0x2B && rdbuf[4] != 0x66);
    return packet->stage_ok[index];
}

static int RunWrite(int port, int index, const unsigned char seed[8],
                    MgTracePacket *packet)
{
    sio2_transfer_data_t td;
    unsigned char wrbuf[14];
    unsigned char rdbuf[14];
    int rc;
    int i;

    SetupPacket(&td, port, wrbuf, rdbuf, sizeof(wrbuf), 0x00380E40);
    wrbuf[0] = 0x81;
    wrbuf[1] = 0xF2;
    wrbuf[2] = 0x51;
    for (i = 0; i < 8; i++)
        wrbuf[3 + i] = seed[7 - i];
    wrbuf[11] = TraceChecksum(seed, 8);

    sio2_mc_transfer_init();
    rc = sio2_transfer(&td);

    packet->transfer_rc[index] = rc;
    packet->stat6c[index] = td.stat6c;
    packet->response_id[index] = rdbuf[12];
    packet->response_status[index] = rdbuf[13];
    packet->stage_ok[index] = (rc != 0 && TraceStatusOk(td.stat6c) &&
                               rdbuf[12] == 0x2B && rdbuf[13] != 0x66);
    return packet->stage_ok[index];
}

static int RunRead(int port, int index, MgTracePacket *packet)
{
    sio2_transfer_data_t td;
    unsigned char wrbuf[14];
    unsigned char rdbuf[14];
    int rc;
    int i;

    SetupPacket(&td, port, wrbuf, rdbuf, sizeof(wrbuf), 0x00380E40);
    wrbuf[0] = 0x81;
    wrbuf[1] = 0xF2;
    wrbuf[2] = 0x53;

    sio2_mc_transfer_init();
    rc = sio2_transfer(&td);

    packet->transfer_rc[index] = rc;
    packet->stat6c[index] = td.stat6c;
    packet->response_id[index] = rdbuf[3];
    packet->response_status[index] = rdbuf[13];
    packet->read_checksum = rdbuf[12];
    packet->calculated_checksum = TraceChecksum(&rdbuf[4], 8);
    for (i = 0; i < 8; i++)
        packet->transformed[7 - i] = rdbuf[4 + i];

    packet->stage_ok[index] = (rc != 0 && TraceStatusOk(td.stat6c) &&
                               rdbuf[3] == 0x2B && rdbuf[13] != 0x66 &&
                               packet->read_checksum == packet->calculated_checksum);
    return packet->stage_ok[index];
}

static void *TraceRpcHandler(int function, void *buffer, int nbytes)
{
    MgTracePacket *packet = (MgTracePacket *)buffer;
    int i;

    (void)nbytes;
    if (function != MGTRACE_RPC_FN_RUN)
        return buffer;

    packet->reset_ok = -1;
    packet->reset_transfer_rc = -1;
    packet->reset_stat6c = 0;
    packet->reset_id = 0;
    packet->reset_status = 0;
    packet->read_checksum = 0;
    packet->calculated_checksum = 0;
    memset(packet->transformed, 0, sizeof(packet->transformed));
    for (i = 0; i < MGTRACE_STAGE_COUNT; i++) {
        packet->stage_ok[i] = -1;
        packet->transfer_rc[i] = -1;
        packet->stat6c[i] = 0;
        packet->response_id[i] = 0;
        packet->response_status[i] = 0;
    }

    if (packet->port < 0 || packet->port > 1 || packet->slot != 0)
        return buffer;

    /* Reset the card authentication state, then independently reproduce the
     * F2/50 -> F2/51 -> F2/52 -> F2/53 card-side transform used by SECRMAN. */
    if (!RunReset(packet->port, packet))
        return buffer;
    if (!RunSimple(packet->port, 0, 0x50, packet))
        return buffer;
    if (!RunWrite(packet->port, 1, packet->seed, packet))
        return buffer;
    if (!RunSimple(packet->port, 2, 0x52, packet))
        return buffer;
    RunRead(packet->port, 3, packet);

    return buffer;
}

static void TraceServerThread(void *arg)
{
    (void)arg;

    if (!sceSifCheckInit())
        sceSifInit();
    sceSifInitRpc(0);
    sceSifSetRpcQueue(&TraceQueue, GetThreadId());
    sceSifRegisterRpc(&TraceServer, MGTRACE_RPC_ID, TraceRpcHandler,
                      TraceServerBuffer, NULL, NULL, &TraceQueue);
    sceSifRpcLoop(&TraceQueue);
}

int _start(int argc, char *argv[])
{
    iop_thread_t thread;

    (void)argc;
    (void)argv;

    memset(&thread, 0, sizeof(thread));
    thread.attr = TH_C;
    thread.thread = TraceServerThread;
    thread.priority = 0x27;
    thread.stacksize = 0x1000;

    TraceThreadId = CreateThread(&thread);
    if (TraceThreadId <= 0)
        return MODULE_NO_RESIDENT_END;
    if (StartThread(TraceThreadId, NULL) < 0)
        return MODULE_NO_RESIDENT_END;

    return MODULE_RESIDENT_END;
}
