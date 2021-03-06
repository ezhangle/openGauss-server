/* -------------------------------------------------------------------------
 *
 * walsender.cpp
 *
 * The WAL sender process (walsender) is new as of Postgres 9.0. It takes
 * care of sending XLOG from the primary server to a single recipient.
 * (Note that there can be more than one walsender process concurrently.)
 * It is started by the postmaster when the walreceiver of a standby server
 * connects to the primary server and requests XLOG streaming replication.
 * It attempts to keep reading XLOG records from the disk and sending them
 * to the standby server, as long as the connection is alive (i.e., like
 * any backend, there is a one-to-one relationship between a connection
 * and a walsender process).
 *
 * Normal termination is by SIGTERM, which instructs the walsender to
 * close the connection and exit(0) at next convenient moment. Emergency
 * termination is by SIGQUIT; like any backend, the walsender will simply
 * abort and exit on SIGQUIT. A close of the connection and a FATAL error
 * are treated as not a crash but approximately normal termination;
 * the walsender will exit quickly without sending any more XLOG records.
 *
 * If the server is shut down, postmaster sends us SIGUSR2 after all
 * regular backends have exited and the shutdown checkpoint has been written.
 * This instruct walsender to send any outstanding WAL, including the
 * shutdown checkpoint record, wait for it to be replicated to the standby,
 * and then exit.
 *
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 2010-2012, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/gausskernel/storage/replication/walsender.cpp
 *
 * -------------------------------------------------------------------------
 */
#include <math.h>
#include "postgres.h"
#include "knl/knl_variable.h"

#include <signal.h>
#include <unistd.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <arpa/inet.h>
#ifndef WIN32
#include <syscall.h>
#endif
#include <sys/stat.h>

#include "access/cbmparsexlog.h"
#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "funcapi.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/replnodes.h"
#include "pgstat.h"
#include "replication/basebackup.h"
#include "replication/catchup.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/slot.h"
#include "replication/snapbuild.h"
#include "replication/syncrep.h"
#include "replication/walprotocol.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "replication/datasender.h"
#include "replication/dataqueue.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"
#include "auditfuncs.h"
#include "gssignal/gs_signal.h"
#include "postmaster/postmaster.h"
#include "alarm/alarm.h"
#include "utils/distribute_test.h"

#define CRC_LEN 11

extern void* internal_load_library(const char* libname);
extern char* expand_dynamic_library_name(const char* name);
extern bool PMstateIsRun(void);
extern void PerformMotCheckpointFetch();

/* Are we there yet? */
static bool WalSndCaughtUp = false;
#define NAPTIME_PER_CYCLE 100 /* max sleep time between cycles (100ms) */
bool WalSegmemtRemovedhappened = false;
volatile bool bSyncStat = false;
volatile bool bSyncStatStatBefore = false;

#define AmWalSenderToDummyStandby() (t_thrd.walsender_cxt.MyWalSnd->sendRole == SNDROLE_PRIMARY_DUMMYSTANDBY)
#define AmWalSenderOnDummyStandby() (t_thrd.walsender_cxt.MyWalSnd->sendRole == SNDROLE_DUMMYSTANDBY_STANDBY)

/* Statistics for log control */
static const int MICROSECONDS_PER_SECONDS = 1000000;
static const int MILLISECONDS_PER_SECONDS = 1000;
static const int MILLISECONDS_PER_MICROSECONDS = 1000;
static const int INIT_CONTROL_REPLY = 1;
static const int MAX_CONTROL_REPLY = 3000;
static const int FAULT_TOLERANCE = 10;
static const int SLEEP_MORE = 200;
static const int SLEEP_LESS = 200;
static const int NODENAMELEN = 1024;
static const int SHIFT_SPEED = 3;

/*
 * This is set while we are streaming. When not set, SIGUSR2 signal will be
 * handled like SIGTERM. When set, the main loop is responsible for checking
 * t_thrd.walsender_cxt.walsender_ready_to_stop and terminating when it's set (after streaming any
 * remaining WAL).
 */
static volatile sig_atomic_t replication_active = false;

/* Signal handlers */
static void WalSndSigHupHandler(SIGNAL_ARGS);
static void WalSndShutdownHandler(SIGNAL_ARGS);
static void WalSndQuickDieHandler(SIGNAL_ARGS);
static void WalSndXLogSendHandler(SIGNAL_ARGS);
static void WalSndLastCycleHandler(SIGNAL_ARGS);

static bool HandleWalReplicationCommand(const char* cmd_string);
typedef void (*WalSndSendDataCallback)(void);
static int WalSndLoop(WalSndSendDataCallback send_data);
static void InitWalSnd(void);
static void WalSndHandshake(void);
static void WalSndKill(int code, Datum arg);
static void XLogSendPhysical(void);
static void XLogSendLogical(void);
static void IdentifySystem(void);
static void IdentifyVersion(void);
static void IdentifyConsistence(IdentifyConsistenceCmd* cmd);
static void IdentifyChannel(IdentifyChannelCmd* cmd);
static void CreateReplicationSlot(const CreateReplicationSlotCmd* cmd);
static void DropReplicationSlot(DropReplicationSlotCmd* cmd);
static void StartReplication(StartReplicationCmd* cmd);
static void StartLogicalReplication(StartReplicationCmd* cmd);
static void ProcessStandbyMessage(void);
static void ProcessStandbyReplyMessage(void);
static void ProcessStandbyHSFeedbackMessage(void);
static void ProcessStandbySwitchRequestMessage(void);
static void ProcessRepliesIfAny(void);
static void LogCtrlCountSleepLimit(void);
static void LogCtrlSleep(void);
static void LogCtrlCalculateCurrentRTO(StandbyReplyMessage* reply);
static void LogCtrlCalculateSleepTime(void);
static void WalSndKeepalive(bool requestReply);
static void WalSndRmXLog(bool requestReply);
static void WalSndSyncDummyStandbyDone(bool requestReply);
static void WalSndKeepaliveIfNecessary(TimestampTz now);
static void WalSndResponseSwitchover(char* msgbuf);
static void SetHaWalSenderChannel(void);
static void SetReplWalSender(void);
static bool SendConfigFile(char* path);
static void ProcessStandbyFileTimeMessage(void);

static long WalSndComputeSleeptime(TimestampTz now);
static void WalSndCheckTimeOut(TimestampTz now);
static void WalSndWriteLogicalAdvanceXLog(TimestampTz now);

static void WalSndPrepareWrite(LogicalDecodingContext* ctx, XLogRecPtr lsn, TransactionId xid, bool last_write);
static void WalSndWriteData(LogicalDecodingContext* ctx, XLogRecPtr lsn, TransactionId xid, bool last_write);
static XLogRecPtr WalSndWaitForWal(XLogRecPtr loc);

static void XLogRead(char* buf, XLogRecPtr startptr, Size count);

static void SetWalSndPeerMode(ServerMode mode);
static void SetWalSndPeerDbstate(DbState state);

static void ChooseStartPointForDummyStandby(void);
static bool WalSndCaughtup(void);
static bool WalSndDummyLEStandby(void);
static void StopAliveBuildSender(void);
static void WalSndShutdown(void) __attribute__((noreturn));

static bool UpdateHaWalSenderChannel(int ha_remote_listen_port);
static bool IsWalSenderToBuild(void);
static void WalSndSetPercentCountStartLsn(XLogRecPtr startLsn);
static void WalSndRefreshPercentCountStartLsn(XLogRecPtr currentMaxLsn, XLogRecPtr currentDoneLsn);

char* DataDir = ".";

/* Main entry point for walsender process */
int WalSenderMain(void)
{
    MemoryContext walsnd_context;
    int nRet = 0;

    t_thrd.proc_cxt.MyProgName = "WalSender";
    if (RecoveryInProgress()) {
        t_thrd.role = WAL_STANDBY_SENDER;
    }

    if (g_threadPoolControler) {
        ereport(INFO, (errmsg("Try to bind walsender thread to available CPUs in threadpool.")));
        g_threadPoolControler->BindThreadToAllAvailCpu(t_thrd.proc_cxt.MyProcPid);
    }

    /* Create a per-walsender data structure in shared memory */
    InitWalSnd();

    ereport(LOG, (errmsg("walsender thread started")));
    /*
     * Create a memory context that we will do all our work in.  We do this so
     * that we can reset the context during error recovery and thereby avoid
     * possible memory leaks.  Formerly this code just ran in
     * t_thrd.top_mem_cxt, but resetting that would be a really bad idea.
     *
     * XXX: we don't actually attempt error recovery in walsender, we just
     * close the connection and exit.
     */
    walsnd_context = AllocSetContextCreate(t_thrd.top_mem_cxt,
        "Wal Sender",
        ALLOCSET_DEFAULT_MINSIZE,
        ALLOCSET_DEFAULT_INITSIZE,
        ALLOCSET_DEFAULT_MAXSIZE);
    (void)MemoryContextSwitchTo(walsnd_context);

    /* Set up resource owner */
    t_thrd.utils_cxt.CurrentResourceOwner = ResourceOwnerCreate(NULL, "walsender top-level resource owner");

    /*
     * Let postmaster know that we're streaming. Once we've declared us as a
     * WAL sender process, postmaster will let us outlive the bgwriter and
     * kill us last in the shutdown sequence, so we get a chance to stream all
     * remaining WAL at shutdown, including the shutdown checkpoint. Note that
     * there's no going back, and we mustn't write any WAL records after this.
     */
    MarkPostmasterChildWalSender();
    SendPostmasterSignal(PMSIGNAL_ADVANCE_STATE_MACHINE);

    /* Unblock signals (they were blocked when the postmaster forked us) */
    gs_signal_setmask(&t_thrd.libpq_cxt.UnBlockSig, NULL);

    /*
     * Use the recovery target timeline ID during recovery
     */
    if (AM_WAL_STANDBY_SENDER)
        t_thrd.xlog_cxt.ThisTimeLineID = GetRecoveryTargetTLI();

    if (dummyStandbyMode) {
        ShutdownWalRcv();
        t_thrd.xlog_cxt.ThisTimeLineID = GetRecoveryTargetTLI();
        t_thrd.xlog_cxt.recoveryTargetTLI = GetRecoveryTargetTLI();
        ereport(LOG, (errmsg("ThisTimeLineID: %u", t_thrd.xlog_cxt.ThisTimeLineID)));
    }

    /* Tell the standby that walsender is ready for receiving commands */
    ReadyForQuery_noblock(DestRemote, u_sess->attr.attr_storage.wal_sender_timeout);

    if (t_thrd.postmaster_cxt.HaShmData)
        t_thrd.walsender_cxt.server_run_mode = t_thrd.postmaster_cxt.HaShmData->current_mode;

    SetHaWalSenderChannel();
    /* Handle handshake messages before streaming */
    WalSndHandshake();

    /* Initialize shared memory status */
    {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;

        SpinLockAcquire(&walsnd->mutex);
        walsnd->pid = t_thrd.proc_cxt.MyProcPid;
#ifndef WIN32
        walsnd->lwpId = syscall(SYS_gettid);
#else
        walsnd->lwpId = (int)t_thrd.proc_cxt.MyProcPid
#endif

        if (AM_WAL_DB_SENDER) {
            /* logical replication */
            walsnd->sentPtr = t_thrd.slot_cxt.MyReplicationSlot->data.restart_lsn;
        } else {
            /* physical replication */
            walsnd->sentPtr = t_thrd.walsender_cxt.sentPtr;
        }

        SpinLockRelease(&walsnd->mutex);

        if (walsnd->sendRole == SNDROLE_PRIMARY_DUMMYSTANDBY) {
            pgstat_report_appname("WalSender to Secondary");
        } else if (walsnd->sendRole == SNDROLE_PRIMARY_BUILDSTANDBY) {
            pgstat_report_appname("WalSender to Build");
        } else if (walsnd->sendRole == SNDROLE_PRIMARY_STANDBY) {
            pgstat_report_appname("WalSender to Standby");
        }
    }

    SyncRepInitConfig();

    if (t_thrd.proc_cxt.DataDir) {
        nRet = snprintf_s(
            t_thrd.walsender_cxt.gucconf_file, MAXPGPATH, MAXPGPATH - 1, "%s/postgresql.conf", t_thrd.proc_cxt.DataDir);
        securec_check_ss(nRet, "\0", "\0");

        nRet = snprintf_s(t_thrd.walsender_cxt.gucconf_lock_file,
            MAXPGPATH,
            MAXPGPATH - 1,
            "%s/postgresql.conf.lock",
            t_thrd.proc_cxt.DataDir);
        securec_check_ss(nRet, "\0", "\0");
    } else {
        ereport(ERROR,
            (errcode(ERRCODE_PROTOCOL_VIOLATION),
                errmsg_internal("cannot find GAUSSDATA: %s", t_thrd.walsender_cxt.gucconf_file)));
    }

    /* init the dummy standby data num to write in wal streaming. */
    if (g_instance.attr.attr_storage.enable_mix_replication && dummyStandbyMode)
        InitWSDataNumOnDummyStandby();

    if (g_instance.attr.attr_storage.enable_mix_replication && !u_sess->attr.attr_storage.enable_cbm_tracking)
        ereport(PANIC, (errmsg("enable_cbm_tracking must be turn on when enable_mix_replication is on!")));

    /* Main loop of walsender */
    if (AM_WAL_DB_SENDER)
        return WalSndLoop(XLogSendLogical);
    else
        return WalSndLoop(XLogSendPhysical);
}

/* check PMstate and RecoveryInProgress */
void CheckPMstateAndRecoveryInProgress(void)
{
    if (!PMstateIsRun() || RecoveryInProgress()) {
        ereport(ERROR,
            (errcode(ERRCODE_LOGICAL_DECODE_ERROR),
                errmsg("can't decode in pmState is not run or recovery in progress.")));
    }
}

/*
 * Execute commands from walreceiver, until we enter streaming mode.
 */
static void WalSndHandshake(void)
{
    StringInfoData input_message;
    bool replication_started = false;
    int sleeptime = 0;
    int timeout = 0;
    const int TIMEOUT_TIMES = 4;
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;

    initStringInfo(&input_message);

    if (walsnd->sendRole == SNDROLE_PRIMARY_BUILDSTANDBY)
        timeout = TIMEOUT_TIMES * u_sess->attr.attr_storage.wal_sender_timeout;
    else
        timeout = u_sess->attr.attr_storage.wal_sender_timeout;

    while (!replication_started) {
        int firstchar;

        WalSndSetState(WALSNDSTATE_STARTUP);
        set_ps_display("idle", false);
        if (t_thrd.walsender_cxt.walsender_ready_to_stop || t_thrd.walsender_cxt.walsender_shutdown_requested) {
            ereport(LOG, (errmsg("caught ready to stop or shutdown request")));
            proc_exit(0);
        }
        /* Wait for some data to arrive */
        if (!pq_select(NAPTIME_PER_CYCLE)) {
            sleeptime += NAPTIME_PER_CYCLE;

            /*
             * not yet data available without blocking,
             * check if it is under maximum timeout
             * period
             */
            if (timeout > 0 && sleeptime >= timeout) {
                ereport(COMMERROR,
                    (errcode(ERRCODE_PROTOCOL_VIOLATION), errmsg("No message received from standby for maximum time")));
                proc_exit(0);
            }
            continue;
        }

        sleeptime = 0;

        /*
         * Since select has indicated that data is available to read,
         * then we can call blocking function itself, as there must be
         * some data to get.
         */
        firstchar = pq_getbyte();

        /*
         * Emergency bailout if postmaster has died.  This is to avoid the
         * necessity for manual cleanup of all postmaster children.
         */
        if (!PostmasterIsAlive())
            gs_thread_exit(1);

        /*
         * Check for any other interesting events that happened while we
         * slept.
         */
        if (t_thrd.walsender_cxt.got_SIGHUP) {
            t_thrd.walsender_cxt.got_SIGHUP = false;
            ProcessConfigFile(PGC_SIGHUP);
        }

        if (firstchar != EOF) {
            /*
             * Read the message contents. This is expected to be done without
             * blocking because we've been able to get message type code.
             */
            if (pq_getmessage(&input_message, 0))
                firstchar = EOF; /* suitable message already logged */
        }

        /* Handle the very limited subset of commands expected in this phase */
        switch (firstchar) {
            case 'Q': /* Query message */
            {
                const char* query_string = NULL;

                query_string = pq_getmsgstring(&input_message);
                pq_getmsgend(&input_message);

                if (HandleWalReplicationCommand(query_string)) {
                    replication_started = true;
                }
            } break;

            case 'X':
                /* standby is closing the connection */
                proc_exit(0);
                /* fall-through */
            case EOF:
                /* standby disconnected unexpectedly */
                ereport(
                    COMMERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION), errmsg("unexpected EOF on standby connection")));
                proc_exit(0);
                /* fall-through */
            default:
                ereport(FATAL,
                    (errcode(ERRCODE_PROTOCOL_VIOLATION),
                        errmsg("invalid standby handshake message type %d", firstchar)));
        }
    }
}

/*
 * IDENTIFY_SYSTEM
 */
static void IdentifySystem(void)
{
    StringInfoData buf;
    char sysid[32];
    char tli[11];
    char xpos[MAXFNAMELEN];
    XLogRecPtr logptr;
    int rc = 0;
    char* dbname = NULL;

    /*
     * Reply with a result set with one row, four columns. First col is system
     * ID, second is timeline ID, third is current xlog location and the fourth
     * contains the database name if we are connected to one.
     */
    rc = snprintf_s(sysid, sizeof(sysid), sizeof(sysid) - 1, UINT64_FORMAT, GetSystemIdentifier());
    securec_check_ss(rc, "\0", "\0");

    rc = snprintf_s(tli, sizeof(tli), sizeof(tli) - 1, "%u", t_thrd.xlog_cxt.ThisTimeLineID);
    securec_check_ss(rc, "\0", "\0");

    logptr = AM_WAL_STANDBY_SENDER ? GetStandbyFlushRecPtr(NULL) : GetFlushRecPtr();

    rc = snprintf_s(xpos, sizeof(xpos), sizeof(xpos) - 1, "%X/%X", (uint32)(logptr >> 32), (uint32)logptr);
    securec_check_ss(rc, "\0", "\0");

    if (u_sess->proc_cxt.MyDatabaseId != InvalidOid) {
        MemoryContext cur = CurrentMemoryContext;

        /* syscache access needs a transaction env. */
        StartTransactionCommand();
        /* make dbname live outside TX context */
        (void)MemoryContextSwitchTo(cur);
        dbname = get_database_name(u_sess->proc_cxt.MyDatabaseId);
        if (dbname == NULL) {
            ereport(ERROR, (errcode(ERRCODE_UNDEFINED_DATABASE),
                        errmsg("database with OID %u does not exist",
                            u_sess->proc_cxt.MyDatabaseId)));
        }
        CommitTransactionCommand();
        /* CommitTransactionCommand switches to t_thrd.top_mem_cxt */
        (void)MemoryContextSwitchTo(cur);
    }

    /* Send a RowDescription message */
    pq_beginmessage(&buf, 'T');
    pq_sendint16(&buf, 4); /* 4 fields */

    /* first field */
    pq_sendstring(&buf, "systemid"); /* col name */
    pq_sendint32(&buf, 0);           /* table oid */
    pq_sendint16(&buf, 0);           /* attnum */
    pq_sendint32(&buf, TEXTOID);     /* type oid */
    pq_sendint16(&buf, UINT16_MAX);  /* typlen */
    pq_sendint32(&buf, 0);           /* typmod */
    pq_sendint16(&buf, 0);           /* format code */

    /* second field */
    pq_sendstring(&buf, "timeline"); /* col name */
    pq_sendint32(&buf, 0);           /* table oid */
    pq_sendint16(&buf, 0);           /* attnum */
    pq_sendint32(&buf, INT4OID);     /* type oid */
    pq_sendint16(&buf, 4);           /* typlen */
    pq_sendint32(&buf, 0);           /* typmod */
    pq_sendint16(&buf, 0);           /* format code */

    /* third field */
    pq_sendstring(&buf, "xlogpos"); /* col name */
    pq_sendint32(&buf, 0);          /* table oid */
    pq_sendint16(&buf, 0);          /* attnum */
    pq_sendint32(&buf, TEXTOID);    /* type oid */
    pq_sendint16(&buf, UINT16_MAX); /* typlen */
    pq_sendint32(&buf, 0);          /* typmod */
    pq_sendint16(&buf, 0);          /* format code */

    /* fourth field */
    pq_sendstring(&buf, "dbname");  /* col name */
    pq_sendint32(&buf, 0);          /* table oid */
    pq_sendint16(&buf, 0);          /* attnum */
    pq_sendint32(&buf, TEXTOID);    /* type oid */
    pq_sendint16(&buf, UINT16_MAX); /* typlen */
    pq_sendint32(&buf, 0);          /* typmod */
    pq_sendint16(&buf, 0);          /* format code */
    pq_endmessage_noblock(&buf);

    /* Send a DataRow message */
    pq_beginmessage(&buf, 'D');
    pq_sendint16(&buf, 4);             /* # of columns */
    pq_sendint32(&buf, strlen(sysid)); /* col1 len */
    pq_sendbytes(&buf, (char*)sysid, strlen(sysid));
    pq_sendint32(&buf, strlen(tli)); /* col2 len */
    pq_sendbytes(&buf, (char*)tli, strlen(tli));
    pq_sendint32(&buf, strlen(xpos)); /* col3 len */
    pq_sendbytes(&buf, (char*)xpos, strlen(xpos));
    /* send NULL if not connected to a database */
    if (dbname != NULL) {
        pq_sendint32(&buf, strlen(dbname)); /* col4 len */
        pq_sendbytes(&buf, (char*)dbname, strlen(dbname));
    } else {
        pq_sendint32(&buf, UINT32_MAX); /* col4 len, NULL */
    }
    pq_endmessage_noblock(&buf);

    /* Send CommandComplete and ReadyForQuery messages */
    EndCommand_noblock("SELECT", DestRemote);
    ReadyForQuery_noblock(DestRemote, u_sess->attr.attr_storage.wal_sender_timeout);
    /* ReadyForQuery did pq_flush_if_available for us */
}

/*
 * IDENTIFY_VERSION
 */
static void IdentifyVersion(void)
{
    StringInfoData buf;
    char pg_sversion[11] = {0};
    char pg_pversion[32] = {0};
    char term[11] = {0};
    uint32 sys_version = PG_VERSION_NUM;
    int nRet = 0;
    errno_t rc = EOK;

    nRet = snprintf_s(pg_sversion, sizeof(pg_sversion), sizeof(pg_sversion) - 1, "%u", sys_version);
    securec_check_ss(nRet, "\0", "\0");

    rc = strncpy_s(pg_pversion, sizeof(pg_pversion), PG_PROTOCOL_VERSION, strlen(PG_PROTOCOL_VERSION));
    securec_check(rc, "\0", "\0");
    nRet = snprintf_s(term, sizeof(term), sizeof(term) - 1, "%u", g_instance.comm_cxt.localinfo_cxt.term);
    securec_check_ss(nRet, "\0", "\0");
    pg_pversion[strlen(PG_PROTOCOL_VERSION)] = '\0';

    /* Send a RowDescription message */
    pq_beginmessage(&buf, 'T');
    pq_sendint16(&buf, 3); /* 3 fields */

    /* first field */
    pq_sendstring(&buf, "sversion"); /* col name */
    pq_sendint32(&buf, 0);           /* table oid */
    pq_sendint16(&buf, 0);           /* attnum */
    pq_sendint32(&buf, INT4OID);     /* type oid */
    pq_sendint16(&buf, 4);           /* typlen */
    pq_sendint32(&buf, 0);           /* typmod */
    pq_sendint16(&buf, 0);           /* format code */

    /* second field */
    pq_sendstring(&buf, "pversion"); /* col name */
    pq_sendint32(&buf, 0);           /* table oid */
    pq_sendint16(&buf, 0);           /* attnum */
    pq_sendint32(&buf, TEXTOID);     /* type oid */
    pq_sendint16(&buf, UINT16_MAX);  /* typlen */
    pq_sendint32(&buf, 0);           /* typmod */
    pq_sendint16(&buf, 0);           /* format code */

    /* first field */
    pq_sendstring(&buf, "term"); /* col name */
    pq_sendint32(&buf, 0);       /* table oid */
    pq_sendint16(&buf, 0);       /* attnum */
    pq_sendint32(&buf, INT4OID); /* type oid */
    pq_sendint16(&buf, 4);       /* typlen */
    pq_sendint32(&buf, 0);       /* typmod */
    pq_sendint16(&buf, 0);       /* format code */
    pq_endmessage_noblock(&buf);

    /* Send a DataRow message */
    pq_beginmessage(&buf, 'D');
    pq_sendint16(&buf, 3);                   /* # of columns */
    pq_sendint32(&buf, strlen(pg_sversion)); /* col1 len */
    pq_sendbytes(&buf, (char*)pg_sversion, strlen(pg_sversion));
    pq_sendint32(&buf, strlen(pg_pversion)); /* col2 len */
    pq_sendbytes(&buf, (char*)pg_pversion, strlen(pg_pversion));
    pq_sendint32(&buf, strlen(term)); /* col2 len */
    pq_sendbytes(&buf, (char*)term, strlen(term));
    pq_endmessage_noblock(&buf);

    /* Send CommandComplete and ReadyForQuery messages */
    EndCommand_noblock("SELECT", DestRemote);
    ReadyForQuery_noblock(DestRemote, u_sess->attr.attr_storage.wal_sender_timeout);
    /* ReadyForQuery did pq_flush_if_available for us */
}

/*
 * IDENTIFY_MODE extern  for datasender
 */
void IdentifyMode(void)
{
    StringInfoData buf;
    char smode[11];
    volatile HaShmemData* hashmdata = t_thrd.postmaster_cxt.HaShmData;
    int nRet = 0;

    SpinLockAcquire(&hashmdata->mutex);
    nRet = snprintf_s(smode, sizeof(smode), sizeof(smode) - 1, "%d", hashmdata->current_mode);
    securec_check_ss(nRet, "\0", "\0");
    SpinLockRelease(&hashmdata->mutex);

    /* Send a RowDescription message */
    pq_beginmessage(&buf, 'T');
    pq_sendint16(&buf, 1); /* 1 fields */

    /* first field */
    pq_sendstring(&buf, "smode"); /* col name */
    pq_sendint32(&buf, 0);        /* table oid */
    pq_sendint16(&buf, 0);        /* attnum */
    pq_sendint32(&buf, INT4OID);  /* type oid */
    pq_sendint16(&buf, 4);        /* typlen */
    pq_sendint32(&buf, 0);        /* typmod */
    pq_sendint16(&buf, 0);        /* format code */
    pq_endmessage_noblock(&buf);

    /* Send a DataRow message */
    pq_beginmessage(&buf, 'D');
    pq_sendint16(&buf, 1);             /* # of columns */
    pq_sendint32(&buf, strlen(smode)); /* col1 len */
    pq_sendbytes(&buf, (char*)smode, strlen(smode));
    pq_endmessage_noblock(&buf);

    /* Send CommandComplete and ReadyForQuery messages */
    EndCommand_noblock("SELECT", DestRemote);
    ReadyForQuery_noblock(DestRemote, u_sess->attr.attr_storage.wal_sender_timeout);
    /* ReadyForQuery did pq_flush_if_available for us */
}

/*
 * IDENTIFY_MAXLSN
 * This LSN contains two part,node name and XLogRecPtr
 * One case is CN build DN and get current latest flushed LSN here
 */
static void IdentifyMaxLsn(void)
{
    int nRet = 0;
    StringInfoData buf;
    char str[MAXFNAMELEN];
    char recptr[MAXFNAMELEN];

    XLogRecPtr ptr = GetFlushRecPtr();

    nRet = snprintf_s(recptr, sizeof(recptr), sizeof(recptr) - 1, "%X/%X", (uint32)(ptr >> 32), (uint32)ptr);
    securec_check_ss(nRet, "\0", "\0");
    nRet = snprintf_s(str, sizeof(str), sizeof(str) - 1, "%s|%s", g_instance.attr.attr_common.PGXCNodeName, recptr);
    securec_check_ss(nRet, "\0", "\0");

    pq_beginmessage(&buf, 'T'); /* RowDescription */
    pq_sendint16(&buf, 1);      /* 1 field */

    /* Field header */
    pq_sendstring(&buf, "recptr");
    pq_sendint32(&buf, 0);       /* table oid */
    pq_sendint16(&buf, 0);       /* attnum */
    pq_sendint32(&buf, TEXTOID); /* type oid */
    pq_sendint16(&buf, UINT16_MAX);
    pq_sendint32(&buf, 0);
    pq_sendint16(&buf, 0);
    pq_endmessage_noblock(&buf);

    /* Data row */
    pq_beginmessage(&buf, 'D');
    pq_sendint16(&buf, 1);           /* number of columns */
    pq_sendint32(&buf, strlen(str)); /* length */
    pq_sendbytes(&buf, str, strlen(str));
    pq_endmessage_noblock(&buf);

    /* Send CommandComplete and ReadyForQuery messages */
    EndCommand_noblock("SELECT", DestRemote);
    ReadyForQuery_noblock(DestRemote, u_sess->attr.attr_storage.wal_sender_timeout);
    /* ReadyForQuery did pq_flush_if_available for us */
}

/*
 * IDENTIFY_CONSISTENCE
 * identify consistence of primary and standby
 */
static void IdentifyConsistence(IdentifyConsistenceCmd* cmd)
{
    StringInfoData buf;
    char        crc[CRC_LEN] = {0};
    char        maxLsnCrcStr[CRC_LEN] = {0};
    pg_crc32    requestRecCrc = 0;
    pg_crc32    localMaxLsnCrc = 0;
    bool        crcValid = false;
    XLogRecPtr  localMaxPtr = InvalidXLogRecPtr;;
    char        strMaxPtr[MAXFNAMELEN] = {0};
    int         nRet = 0;
    char        msgBuf[XLOG_READER_MAX_MSGLENTH] = {0};

    requestRecCrc = GetXlogRecordCrc(cmd->recordptr, crcValid);

    /* To support grayupgrade, msg with 1 row of 2 or 3 colums is used
     * according to working version number. Will remove later.
     */ 
    if (t_thrd.proc && t_thrd.proc->workingVersionNum >= 92060) {
        /* Don't care max xlog when check with building process */
        if (IsWalSenderToBuild() == false) {
            if (dummyStandbyMode) {
                localMaxPtr = FindMaxLSN(t_thrd.proc_cxt.DataDir, msgBuf, XLOG_READER_MAX_MSGLENTH, &localMaxLsnCrc);
            } else {
                if (AM_WAL_STANDBY_SENDER) {
                    (void)GetXLogReplayRecPtr(NULL, &localMaxPtr);
                    localMaxLsnCrc = GetXlogRecordCrc(localMaxPtr, crcValid);
                } else {
                    localMaxPtr =
                        FindMaxLSN(t_thrd.proc_cxt.DataDir, msgBuf, XLOG_READER_MAX_MSGLENTH, &localMaxLsnCrc);
                }
            }

            ereport(LOG, (errmsg("remote request lsn/crc: [%X/%X, %u] "
                "local max lsn/crc: [%X/%X, %u]",
                (uint32)(cmd->recordptr >> 32), (uint32)cmd->recordptr,
                (uint32)requestRecCrc, (uint32)(localMaxPtr >> 32),
                (uint32)localMaxPtr, (uint32)localMaxLsnCrc)));
        }

        if (requestRecCrc == NONE_REC_CRC && WalSndCaughtup()) {
            requestRecCrc = IGNORE_REC_CRC;
        }

        nRet = snprintf_s(crc, sizeof(crc), sizeof(crc) - 1, "%X", requestRecCrc);
        securec_check_ss(nRet, "\0", "\0");

        nRet = snprintf_s(strMaxPtr, sizeof(strMaxPtr), sizeof(strMaxPtr) - 1, "%X/%X", (uint32)(localMaxPtr >> 32),
            (uint32)localMaxPtr);
        securec_check_ss(nRet, "\0", "\0");

        nRet = snprintf_s(maxLsnCrcStr, sizeof(maxLsnCrcStr), sizeof(maxLsnCrcStr) - 1, "%X", localMaxLsnCrc);
        securec_check_ss(nRet, "\0", "\0");

        /* Send a RowDescription message */
        pq_beginmessage(&buf, 'T');
        pq_sendint16(&buf, 3); /* 1 fields */

        /* first field */
        pq_sendstring(&buf, "requestRemoteCrc"); /* col name */
        pq_sendint32(&buf, 0); /* table oid */
        pq_sendint16(&buf, 0); /* attnum */
        pq_sendint32(&buf, TEXTOID); /* type oid */
        pq_sendint16(&buf, UINT16_MAX); /* typlen */
        pq_sendint32(&buf, 0); /* typmod */
        pq_sendint16(&buf, 0); /* format code */

        /* second field */
        pq_sendstring(&buf, "localMaxLsn"); /* col name */
        pq_sendint32(&buf, 0); /* table oid */
        pq_sendint16(&buf, 1); /* attnum */
        pq_sendint32(&buf, TEXTOID); /* type oid */
        pq_sendint16(&buf, UINT16_MAX); /* typlen */
        pq_sendint32(&buf, 0); /* typmod */
        pq_sendint16(&buf, 0); /* format code */

        /* third field */
        pq_sendstring(&buf, "localMaxLsnCrc"); /* col name */
        pq_sendint32(&buf, 0); /* table oid */
        pq_sendint16(&buf, 2); /* attnum */
        pq_sendint32(&buf, TEXTOID); /* type oid */
        pq_sendint16(&buf, UINT16_MAX); /* typlen */
        pq_sendint32(&buf, 0); /* typmod */
        pq_sendint16(&buf, 0); /* format code */

        pq_endmessage_noblock(&buf);

        /* Send a DataRow message */
        pq_beginmessage(&buf, 'D');
        pq_sendint16(&buf, 3); /* # of columns */
        pq_sendint32(&buf, strlen(crc)); /* col1 len */
        pq_sendbytes(&buf, (char *) crc, strlen(crc));
        pq_sendint32(&buf, strlen(strMaxPtr)); /* col2 len */
        pq_sendbytes(&buf, (char *) strMaxPtr, strlen(strMaxPtr));
        pq_sendint32(&buf, strlen(maxLsnCrcStr)); /* col3 len */
        pq_sendbytes(&buf, (char *) maxLsnCrcStr, strlen(maxLsnCrcStr));
        pq_endmessage_noblock(&buf);
    } else {
        char havexlog[8] = {0};
        if (dummyStandbyMode) {
            if (crcValid) {
                havexlog[0] = '1';
            } else {
                localMaxPtr = FindMaxLSN(t_thrd.proc_cxt.DataDir, msgBuf, XLOG_READER_MAX_MSGLENTH, &localMaxLsnCrc);
                if (XLByteLT(localMaxPtr, cmd->recordptr) || XLByteEQ(localMaxPtr, InvalidXLogRecPtr)) {
                    havexlog[0] = '0';
                } else {
                    havexlog[0] = '1';
                }
            }
            ereport(LOG,
                    (errmsg("standby rec: %x/%x, havexlog: %s, crc:%u",
                            (uint32) (cmd->recordptr >> 32), (uint32) cmd->recordptr,
                            havexlog, (uint32)requestRecCrc)));
        } else {
            havexlog[0] = '1';  /* have xlog be true if in primary mode */
        }

        nRet = snprintf_s(crc, sizeof(crc), sizeof(crc) - 1, "%X", requestRecCrc);
        securec_check_ss(nRet, "\0", "\0");
        
        /* Send a RowDescription message */
        pq_beginmessage(&buf, 'T');
        pq_sendint16(&buf, 2);      /* 1 fields */
        
        /* first field */
        pq_sendstring(&buf, "reccrc");  /* col name */
        pq_sendint32(&buf, 0);      /* table oid */
        pq_sendint16(&buf, 0);      /* attnum */
        pq_sendint32(&buf, TEXTOID);        /* type oid */
        pq_sendint16(&buf, UINT16_MAX); /* typlen */
        pq_sendint32(&buf, 0);      /* typmod */
        pq_sendint16(&buf, 0);      /* format code */
        
        /* sencond field */
        pq_sendstring(&buf, "havexlog");    /* col name */
        pq_sendint32(&buf, 0);      /* table oid */
        pq_sendint16(&buf, 0);      /* attnum */
        pq_sendint32(&buf, INT4OID);        /* type oid */
        pq_sendint16(&buf, 4);      /* typlen */
        pq_sendint32(&buf, 0);      /* typmod */
        pq_sendint16(&buf, 0);      /* format code */
        pq_endmessage_noblock(&buf);
        
        /* Send a DataRow message */
        pq_beginmessage(&buf, 'D');
        pq_sendint16(&buf, 2);      /* # of columns */
        pq_sendint32(&buf, strlen(crc));    /* col1 len */
        pq_sendbytes(&buf, (char *) crc, strlen(crc));
        pq_sendint32(&buf, strlen(havexlog));   /* col2 len */
        pq_sendbytes(&buf, (char *) havexlog, strlen(havexlog));
        pq_endmessage_noblock(&buf);
    }

    /* Send CommandComplete and ReadyForQuery messages */
    EndCommand_noblock("SELECT", DestRemote);
    ReadyForQuery_noblock(DestRemote, u_sess->attr.attr_storage.wal_sender_timeout);
    /* ReadyForQuery did pq_flush_if_available for us */
}

/*
 * IDENTIFY_CHANNEL
 * get channel identifier from standby
 */
static void IdentifyChannel(IdentifyChannelCmd* cmd)
{
    StringInfoData buf;

    t_thrd.walsender_cxt.remotePort = cmd->channel_identifier;
    bool is_success = UpdateHaWalSenderChannel(t_thrd.walsender_cxt.remotePort);

    const char *result = is_success ? "t" : "f";
    size_t result_len = strlen(result);

    /* Send a RowDescription message */
    pq_beginmessage(&buf, 'T');
    pq_sendint16(&buf, 1); /* 1 fields */

    /* first field */
    pq_sendstring(&buf, "identifier"); /* col name */
    pq_sendint32(&buf, 0);             /* table oid */
    pq_sendint16(&buf, 0);             /* attnum */
    pq_sendint32(&buf, BOOLOID);       /* type oid */
    pq_sendint16(&buf, 1);             /* typlen */
    pq_sendint32(&buf, 0);             /* typmod */
    pq_sendint16(&buf, 0);             /* format code */
    pq_endmessage_noblock(&buf);

    /* Send a DataRow message */
    pq_beginmessage(&buf, 'D');
    pq_sendint16(&buf, 1);          /* # of columns */
    pq_sendint32(&buf, result_len); /* col1 len */
    pq_sendbytes(&buf, result, result_len);
    pq_endmessage_noblock(&buf);

    /* Send CommandComplete and ReadyForQuery messages */
    EndCommand_noblock("SELECT", DestRemote);
    ReadyForQuery_noblock(DestRemote, u_sess->attr.attr_storage.wal_sender_timeout);
}

/*
 * START_REPLICATION
 */
static void StartReplication(StartReplicationCmd* cmd)
{
    StringInfoData buf;

    /*
     * When promoting a cascading standby, postmaster sends SIGUSR2 to any
     * cascading walsenders to kill them. But there is a corner-case where
     * such walsender fails to receive SIGUSR2 and survives a standby
     * promotion unexpectedly. This happens when postmaster sends SIGUSR2
     * before the walsender marks itself as a WAL sender, because postmaster
     * sends SIGUSR2 to only the processes marked as a WAL sender.
     *
     * To avoid this corner-case, if recovery is NOT in progress even though
     * the walsender is cascading one, we do the same thing as SIGUSR2 signal
     * handler does, i.e., set t_thrd.walsender_cxt.walsender_ready_to_stop to true. Which causes
     * the walsender to end later.
     *
     * When terminating cascading walsenders, usually postmaster writes the
     * log message announcing the terminations. But there is a race condition
     * here. If there is no walsender except this process before reaching
     * here, postmaster thinks that there is no walsender and suppresses that
     * log message. To handle this case, we always emit that log message here.
     * This might cause duplicate log messages, but which is less likely to
     * happen, so it's not worth writing some code to suppress them.
     */
    if (AM_WAL_STANDBY_SENDER && !RecoveryInProgress()) {
        ereport(LOG,
            (errmsg("terminating walsender process to force cascaded standby "
                    "to update timeline and reconnect")));
        t_thrd.walsender_cxt.walsender_ready_to_stop = true;
    }

    /*
     * We assume here that we're logging enough information in the WAL for
     * log-shipping, since this is checked in PostmasterMain().
     *
     * NOTE: wal_level can only change at shutdown, so in most cases it is
     * difficult for there to be WAL data that we can still see that was
     * written at wal_level='minimal'.
     */
    if (cmd->slotname) {
        ReplicationSlotAcquire(cmd->slotname, AmWalSenderToDummyStandby() ? true : false);
        if (t_thrd.slot_cxt.MyReplicationSlot->data.database != InvalidOid)
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                    (errmsg("cannot use a logical replication slot for physical replication"))));
    }

    /*
     * When we first start replication the standby will be behind the primary.
     * For some applications, for example, synchronous replication, it is
     * important to have a clear state for this initial catchup mode, so we
     * can trigger actions when we change streaming state later. We may stay
     * in this state for a long time, which is exactly why we want to be able
     * to monitor whether or not we are still here.
     */
    WalSndSetState(WALSNDSTATE_CATCHUP);

    /* Send a CopyBothResponse message, and start streaming */
    pq_beginmessage(&buf, 'W');
    pq_sendbyte(&buf, 0);
    pq_sendint16(&buf, 0);
    pq_endmessage_noblock(&buf);
    pq_flush_timedwait(u_sess->attr.attr_storage.wal_sender_timeout);

    /*
     * Initialize position to the received one, then the xlog records begin to
     * be shipped from that position
     */
    if (AmWalSenderToDummyStandby())
        ChooseStartPointForDummyStandby();
    else {
        t_thrd.walsender_cxt.sentPtr = cmd->startpoint;
        WalSndSetPercentCountStartLsn(cmd->startpoint);
    }
}

/*
 * read_page callback for logical decoding contexts, as a walsender process.
 *
 * Inside the walsender we can do better than logical_read_local_xlog_page,
 * which has to do a plain sleep/busy loop, because the walsender's latch gets
 * set everytime WAL is flushed.
 */
static int logical_read_xlog_page(XLogReaderState* state, XLogRecPtr targetPagePtr, int reqLen, XLogRecPtr targetRecPtr,
    char* cur_page, TimeLineID* pageTLI)
{
    XLogRecPtr flushptr;
    int count;

    /* make sure we have enough WAL available */
    flushptr = WalSndWaitForWal(targetPagePtr + reqLen);
    /* fail if not (implies we are going to shut down) */
    if (flushptr < targetPagePtr + reqLen)
        return -1;

    if (targetPagePtr + XLOG_BLCKSZ <= flushptr)
        count = XLOG_BLCKSZ; /* more than one block available */
    else
        count = flushptr - targetPagePtr; /* part of the page available */

    /* now actually read the data, we know it's there */
    XLogRead(cur_page, targetPagePtr, XLOG_BLCKSZ);

    return count;
}

/*
 * Create a new replication slot.
 */
static void CreateReplicationSlot(const CreateReplicationSlotCmd* cmd)
{
    const char* slot_name = NULL;
    StringInfoData buf;
    bool isDummyStandby = false;
    const char* snapshot_name = NULL;
    char xpos[MAXFNAMELEN];
    int rc = 0;

    Assert(!t_thrd.slot_cxt.MyReplicationSlot);

    /* setup state for XLogReadPage */
    isDummyStandby = AmWalSenderToDummyStandby() ? true : false;

    if (cmd->kind == REPLICATION_KIND_LOGICAL) {
        CheckPMstateAndRecoveryInProgress();
        CheckLogicalDecodingRequirements(u_sess->proc_cxt.MyDatabaseId);
        /*
         * Initially create the slot as ephemeral - that allows us to nicely
         * handle errors during initialization because it'll get dropped if
         * this transaction fails. We'll make it persistent at the end.
         */
        ReplicationSlotCreate(
            cmd->slotname, RS_EPHEMERAL, isDummyStandby, u_sess->proc_cxt.MyDatabaseId, InvalidXLogRecPtr);
    } else {
        /*
         * physical slot save init value if exist
         */
        ReplicationSlotCreate(cmd->slotname, RS_PERSISTENT, isDummyStandby, InvalidOid, cmd->init_slot_lsn);
    }
    slot_name = NameStr(t_thrd.slot_cxt.MyReplicationSlot->data.name);

    if (cmd->kind == REPLICATION_KIND_LOGICAL) {
        (void)ValidateName(cmd->slotname);
        (void)ValidateName(cmd->plugin);
        char* fullname = NULL;
        fullname = expand_dynamic_library_name(cmd->plugin);

        /* Load the shared library, unless we already did */
        (void)internal_load_library(fullname);

        LogicalDecodingContext* ctx = NULL;

        ctx = CreateInitDecodingContext(
            cmd->plugin, NIL, true, logical_read_xlog_page, WalSndPrepareWrite, WalSndWriteData);

        /* build initial snapshot, might take a while */
        DecodingContextFindStartpoint(ctx);

        /*
         * Export a plain (not of the snapbuild.c type) snapshot to the user
         * that can be imported into another session.
         */
        snapshot_name = SnapBuildExportSnapshot(ctx->snapshot_builder);

        /* don't need the decoding context anymore */
        FreeDecodingContext(ctx);

        ReplicationSlotPersist();

        // write xlog
        log_slot_create(&t_thrd.slot_cxt.MyReplicationSlot->data);
    }

    rc = snprintf_s(xpos,
        sizeof(xpos),
        sizeof(xpos) - 1,
        "%X/%X",
        (uint32)(t_thrd.slot_cxt.MyReplicationSlot->data.confirmed_flush >> 32),
        (uint32)t_thrd.slot_cxt.MyReplicationSlot->data.confirmed_flush);
    securec_check_ss(rc, "\0", "\0");

    /*
     * It may seem somewhat pointless to send back the same slot name the
     * client just requested and nothing else, but logical replication
     * will add more fields here.  (We could consider removing the slot
     * name from what's sent back, though, since the client has specified
     * that.)
     */
    pq_beginmessage(&buf, 'T');
    pq_sendint16(&buf, 4); /* 4 field */

    /* first field: slot name */
    pq_sendstring(&buf, "slot_name"); /* col name */
    pq_sendint32(&buf, 0);            /* table oid */
    pq_sendint16(&buf, 0);            /* attnum */
    pq_sendint32(&buf, TEXTOID);      /* type oid */
    pq_sendint16(&buf, UINT16_MAX);   /* typlen */
    pq_sendint32(&buf, 0);            /* typmod */
    pq_sendint16(&buf, 0);            /* format code */

    /* second field: LSN at which we became consistent */
    pq_sendstring(&buf, "consistent_point"); /* col name */
    pq_sendint32(&buf, 0);                   /* table oid */
    pq_sendint16(&buf, 0);                   /* attnum */
    pq_sendint32(&buf, TEXTOID);             /* type oid */
    pq_sendint16(&buf, UINT16_MAX);          /* typlen */
    pq_sendint32(&buf, 0);                   /* typmod */
    pq_sendint16(&buf, 0);                   /* format code */

    /* third field: exported snapshot's name */
    pq_sendstring(&buf, "snapshot_name"); /* col name */
    pq_sendint32(&buf, 0);                /* table oid */
    pq_sendint16(&buf, 0);                /* attnum */
    pq_sendint32(&buf, TEXTOID);          /* type oid */
    pq_sendint16(&buf, UINT16_MAX);       /* typlen */
    pq_sendint32(&buf, 0);                /* typmod */
    pq_sendint16(&buf, 0);                /* format code */

    /* fourth field: output plugin */
    pq_sendstring(&buf, "output_plugin"); /* col name */
    pq_sendint32(&buf, 0);                /* table oid */
    pq_sendint16(&buf, 0);                /* attnum */
    pq_sendint32(&buf, TEXTOID);          /* type oid */
    pq_sendint16(&buf, UINT16_MAX);       /* typlen */
    pq_sendint32(&buf, 0);                /* typmod */
    pq_sendint16(&buf, 0);                /* format code */
    pq_endmessage_noblock(&buf);

    /* Send a DataRow message */
    pq_beginmessage(&buf, 'D');
    pq_sendint16(&buf, 4); /* # of columns */

    /* slot_name */
    pq_sendint32(&buf, strlen(slot_name)); /* col1 len */
    pq_sendbytes(&buf, slot_name, strlen(slot_name));

    /* consistent wal location */
    pq_sendint32(&buf, strlen(xpos)); /* col2 len */
    pq_sendbytes(&buf, xpos, strlen(xpos));

    /* snapshot name */
    if (snapshot_name != NULL) {
        pq_sendint32(&buf, strlen(snapshot_name)); /* col3 len */
        pq_sendbytes(&buf, snapshot_name, strlen(snapshot_name));
    } else {
        pq_sendint32(&buf, UINT32_MAX); /* col3 len, NULL */
    }

    /* plugin */
    if (cmd->plugin != NULL) {
        pq_sendint32(&buf, strlen(cmd->plugin)); /* col4 len */
        pq_sendbytes(&buf, cmd->plugin, strlen(cmd->plugin));
    } else
        pq_sendint32(&buf, UINT32_MAX); /* col4 len, NULL */
    pq_endmessage_noblock(&buf);

    /* Send CommandComplete and ReadyForQuery messages */
    EndCommand_noblock("SELECT", DestRemote);
    ReadyForQuery_noblock(DestRemote, u_sess->attr.attr_storage.wal_sender_timeout);
    /* ReadyForQuery did pq_flush_if_available for us
     *
     * release active status again, START_REPLICATION will reacquire it
     */
    ReplicationSlotRelease();
}

/* Determine if it is a logical slot */
bool IsLogicalSlot(const char* name)
{
    bool ret = false;

    LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
    for (int i = 0; i < g_instance.attr.attr_storage.max_replication_slots; i++) {
        ReplicationSlot* s = &t_thrd.slot_cxt.ReplicationSlotCtl->replication_slots[i];

        if (s->in_use && strcmp(name, NameStr(s->data.name)) == 0 && s->data.database != InvalidOid) {
            ret = true;
            break;
        }
    }
    LWLockRelease(ReplicationSlotControlLock);

    return ret;
}

/*
 * Get rid of a replication slot that is no longer wanted.
 */
static void DropReplicationSlot(DropReplicationSlotCmd* cmd)
{
    if (IsLogicalSlot(cmd->slotname)) {
        CheckPMstateAndRecoveryInProgress();
        ReplicationSlotDrop(cmd->slotname);
        log_slot_drop(cmd->slotname);
    } else {
        ReplicationSlotDrop(cmd->slotname);
    }

    EndCommand_noblock("DROP_REPLICATION_SLOT", DestRemote);
    EndCommand_noblock("SELECT", DestRemote);
    ReadyForQuery_noblock(DestRemote, u_sess->attr.attr_storage.wal_sender_timeout);
}

/*
 * Load previously initiated logical slot and prepare for sending data (via
 * WalSndLoop).
 */
static void StartLogicalReplication(StartReplicationCmd* cmd)
{
    StringInfoData buf;

    /* make sure that our requirements are still fulfilled */
    CheckLogicalDecodingRequirements(u_sess->proc_cxt.MyDatabaseId);

    Assert(!t_thrd.slot_cxt.MyReplicationSlot);

    ReplicationSlotAcquire(cmd->slotname, AmWalSenderToDummyStandby() ? true : false);

    /*
     * Force a disconnect, so that the decoding code doesn't need to care
     * about a eventual switch from running in recovery, to running in a
     * normal environment. Client code is expected to handle reconnects.
     */
    if (AM_WAL_STANDBY_SENDER && !RecoveryInProgress()) {
        ereport(LOG, (errmsg("terminating walsender process after promotion")));
        t_thrd.walsender_cxt.walsender_ready_to_stop = true;
    }

    {
        /*
         * Rebuild snap dir
         */
        char snappath[MAXPGPATH];
        struct stat st;
        int rc = 0;

        rc = snprintf_s(snappath,
            MAXPGPATH,
            MAXPGPATH - 1,
            "pg_replslot/%s/snap",
            NameStr(t_thrd.slot_cxt.MyReplicationSlot->data.name));
        securec_check_ss(rc, "\0", "\0");

        if (stat(snappath, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (!rmtree(snappath, true))
                ereport(ERROR, (errcode_for_file_access(), errmsg("could not remove directory \"%s\"", snappath)));
        }
        if (mkdir(snappath, S_IRWXU) < 0)
            ereport(ERROR, (errcode_for_file_access(), errmsg("could not create directory \"%s\": %m", snappath)));
    }

    WalSndSetState(WALSNDSTATE_CATCHUP);

    /* Send a CopyBothResponse message, and start streaming */
    pq_beginmessage(&buf, 'W');
    pq_sendbyte(&buf, 0);
    pq_sendint(&buf, 0, 2);
    pq_endmessage(&buf);
    pq_flush();

    /*
     * Initialize position to the last ack'ed one, then the xlog records begin
     * to be shipped from that position.
     */
    t_thrd.walsender_cxt.logical_decoding_ctx = CreateDecodingContext(
        cmd->startpoint, cmd->options, false, logical_read_xlog_page, WalSndPrepareWrite, WalSndWriteData);

    /* Start reading WAL from the oldest required WAL. */
    t_thrd.walsender_cxt.logical_startptr = t_thrd.slot_cxt.MyReplicationSlot->data.restart_lsn;

    /*
     * Report the location after which we'll send out further commits as the
     * current sentPtr.
     */
    t_thrd.walsender_cxt.sentPtr = t_thrd.slot_cxt.MyReplicationSlot->data.confirmed_flush;

    /* Also update the sent position status in shared memory */
    {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;

        SpinLockAcquire(&walsnd->mutex);
        walsnd->sentPtr = t_thrd.slot_cxt.MyReplicationSlot->data.restart_lsn;
        SpinLockRelease(&walsnd->mutex);
    }

    replication_active = true;

    SyncRepInitConfig();

    /* Main loop of walsender */
    WalSndLoop(XLogSendLogical);

    FreeDecodingContext(t_thrd.walsender_cxt.logical_decoding_ctx);
    ReplicationSlotRelease();

    replication_active = false;
    if (t_thrd.walsender_cxt.walsender_ready_to_stop)
        proc_exit(0);
    WalSndSetState(WALSNDSTATE_STARTUP);

    /* Get out of COPY mode (CommandComplete). */
    EndCommand("COPY 0", DestRemote);
}

/*
 * LogicalDecodingContext 'prepare_write' callback.
 *
 * Prepare a write into a StringInfo.
 *
 * Don't do anything lasting in here, it's quite possible that nothing will done
 * with the data.
 */
static void WalSndPrepareWrite(LogicalDecodingContext* ctx, XLogRecPtr lsn, TransactionId xid, bool last_write)
{
    /* can't have sync rep confused by sending the same LSN several times */
    if (!last_write)
        lsn = InvalidXLogRecPtr;

    resetStringInfo(ctx->out);

    pq_sendbyte(ctx->out, 'w');
    pq_sendint64(ctx->out, lsn); /* dataStart */
    pq_sendint64(ctx->out, lsn); /* walEnd */
    /*
     * Fill out the sendtime later, just as it's done in XLogSendPhysical, but
     * reserve space here.
     */
    pq_sendint64(ctx->out, 0); /* sendtime */
}

/*
 * LogicalDecodingContext 'write' callback.
 *
 * Actually write out data previously prepared by WalSndPrepareWrite out to
 * the network. Take as long as needed, but process replies from the other
 * side and check timeouts during that.
 */
static void WalSndWriteData(LogicalDecodingContext* ctx, XLogRecPtr lsn, TransactionId xid, bool last_write)
{
    errno_t rc;

    /* output previously gathered data in a CopyData packet */
    pq_putmessage_noblock('d', ctx->out->data, ctx->out->len);

    /*
     * Fill the send timestamp last, so that it is taken as late as
     * possible. This is somewhat ugly, but the protocol's set as it's already
     * used for several releases by streaming physical replication.
     */
    resetStringInfo(t_thrd.walsender_cxt.tmpbuf);
    pq_sendint64(t_thrd.walsender_cxt.tmpbuf, GetCurrentTimestamp());
    rc = memcpy_s(&(ctx->out->data[1 + sizeof(int64) + sizeof(int64)]),
        ctx->out->len,
        t_thrd.walsender_cxt.tmpbuf->data,
        sizeof(int64));
    securec_check(rc, "\0", "\0");

    /* fast path */
    /* Try to flush pending output to the client */
    if (pq_flush_if_writable() != 0)
        WalSndShutdown();

    if (!pq_is_send_pending())
        return;

    for (;;) {
        int wakeEvents;
        long sleeptime;
        TimestampTz now;

        /*
         * Emergency bailout if postmaster has died.  This is to avoid the
         * necessity for manual cleanup of all postmaster children.
         */
        if (!PostmasterIsAlive())
            exit(1);

        /* Process any requests or signals received recently */
        if (t_thrd.walsender_cxt.got_SIGHUP) {
            t_thrd.walsender_cxt.got_SIGHUP = false;
            ProcessConfigFile(PGC_SIGHUP);
            SyncRepInitConfig();
        }

        /* Check for input from the client */
        ProcessRepliesIfAny();

        /* Clear any already-pending wakeups */
        ResetLatch(&t_thrd.walsender_cxt.MyWalSnd->latch);

        /* Try to flush pending output to the client */
        if (pq_flush_if_writable() != 0)
            WalSndShutdown();

        /* If we finished clearing the buffered data, we're done here. */
        if (!pq_is_send_pending())
            break;

        now = GetCurrentTimestamp();

        sleeptime = WalSndComputeSleeptime(now);

        wakeEvents = WL_LATCH_SET | WL_POSTMASTER_DEATH | WL_SOCKET_WRITEABLE | WL_SOCKET_READABLE | WL_TIMEOUT;

        /* Sleep until something happens or we time out */
        t_thrd.int_cxt.ImmediateInterruptOK = true;
        CHECK_FOR_INTERRUPTS();
        WaitLatchOrSocket(
            &t_thrd.walsender_cxt.MyWalSnd->latch, wakeEvents, u_sess->proc_cxt.MyProcPort->sock, sleeptime);
        t_thrd.int_cxt.ImmediateInterruptOK = false;
    }

    /* reactivate latch so WalSndLoop knows to continue */
    SetLatch(&t_thrd.walsender_cxt.MyWalSnd->latch);
}

/*
 * Wait till WAL < loc is flushed to disk so it can be safely read.
 */
static XLogRecPtr WalSndWaitForWal(XLogRecPtr loc)
{
    int wakeEvents;
    static XLogRecPtr RecentFlushPtr = InvalidXLogRecPtr;

    /*
     * Fast path to avoid acquiring the spinlock in the we already know we
     * have enough WAL available. This is particularly interesting if we're
     * far behind.
     */
    if (!XLogRecPtrIsInvalid(RecentFlushPtr) && XLByteLE(loc, RecentFlushPtr))
        return RecentFlushPtr;

    /* Get a more recent flush pointer. */
    if (!RecoveryInProgress())
        RecentFlushPtr = GetFlushRecPtr();
    else
        RecentFlushPtr = GetXLogReplayRecPtr(NULL);

    for (;;) {
        long sleeptime;
        TimestampTz now;

        /*
         * Emergency bailout if postmaster has died.  This is to avoid the
         * necessity for manual cleanup of all postmaster children.
         */
        if (!PostmasterIsAlive())
            exit(1);

        /* Process any requests or signals received recently */
        if (t_thrd.walsender_cxt.got_SIGHUP) {
            t_thrd.walsender_cxt.got_SIGHUP = false;
            ProcessConfigFile(PGC_SIGHUP);
            SyncRepInitConfig();
        }

        /* Check for input from the client */
        ProcessRepliesIfAny();

        /* Clear any already-pending wakeups */
        ResetLatch(&t_thrd.walsender_cxt.MyWalSnd->latch);

        /* Update our idea of the currently flushed position. */
        if (!RecoveryInProgress())
            RecentFlushPtr = GetFlushRecPtr();
        else
            RecentFlushPtr = GetXLogReplayRecPtr(NULL);

        /*
         * If postmaster asked us to stop, don't wait here anymore. This will
         * cause the xlogreader to return without reading a full record, which
         * is the fastest way to reach the mainloop which then can quit.
         *
         * It's important to do this check after the recomputation of
         * RecentFlushPtr, so we can send all remaining data before shutting
         * down.
         */
        if (t_thrd.walsender_cxt.walsender_ready_to_stop)
            break;

        /*
         * We only send regular messages to the client for full decoded
         * transactions, but a synchronous replication and walsender shutdown
         * possibly are waiting for a later location. So we send pings
         * containing the flush location every now and then.
         */
        if (XLByteLT(t_thrd.walsender_cxt.MyWalSnd->flush, t_thrd.walsender_cxt.sentPtr) &&
            !t_thrd.walsender_cxt.waiting_for_ping_response) {
            WalSndKeepalive(false);
            t_thrd.walsender_cxt.waiting_for_ping_response = true;
        }

        /* check whether we're done */
        if (XLByteLE(loc, RecentFlushPtr))
            break;

        /* Waiting for new WAL. Since we need to wait, we're now caught up. */
        WalSndCaughtUp = true;

        /*
         * Try to flush pending output to the client. Also wait for the socket
         * becoming writable, if there's still pending output after an attempt
         * to flush. Otherwise we might just sit on output data while waiting
         * for new WAL being generated.
         */
        if (pq_flush_if_writable() != 0)
            WalSndShutdown();

        now = GetCurrentTimestamp();

        sleeptime = WalSndComputeSleeptime(now);

        wakeEvents = WL_LATCH_SET | WL_POSTMASTER_DEATH | WL_SOCKET_READABLE | WL_TIMEOUT;

        if (pq_is_send_pending())
            wakeEvents |= WL_SOCKET_WRITEABLE;

        /* Sleep until something happens or we time out */
        t_thrd.int_cxt.ImmediateInterruptOK = true;
        CHECK_FOR_INTERRUPTS();
        WaitLatchOrSocket(
            &t_thrd.walsender_cxt.MyWalSnd->latch, wakeEvents, u_sess->proc_cxt.MyProcPort->sock, sleeptime);
        t_thrd.int_cxt.ImmediateInterruptOK = false;
    }

    /* reactivate latch so WalSndLoop knows to continue */
    SetLatch(&t_thrd.walsender_cxt.MyWalSnd->latch);
    return RecentFlushPtr;
}

/*
 * Execute an incoming replication command.
 */
static bool HandleWalReplicationCommand(const char* cmd_string)
{
    bool replication_started = false;
    int parse_rc;
    Node* cmd_node = NULL;
    MemoryContext cmd_context;
    MemoryContext old_context;
    replication_scanner_yyscan_t yyscanner = NULL;

    /*
     * CREATE_REPLICATION_SLOT ... LOGICAL exports a snapshot until the next
     * command arrives. Clean up the old stuff if there's anything.
     */
    SnapBuildClearExportedSnapshot();

    ereport(LOG, (errmsg("received wal replication command: %s", cmd_string)));

    cmd_context = AllocSetContextCreate(CurrentMemoryContext,
        "Replication command context",
        ALLOCSET_DEFAULT_MINSIZE,
        ALLOCSET_DEFAULT_INITSIZE,
        ALLOCSET_DEFAULT_MAXSIZE);

    yyscanner = replication_scanner_init(cmd_string);
    parse_rc = replication_yyparse(yyscanner);
    replication_scanner_finish(yyscanner);

    if (parse_rc != 0) {
        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), 
            (errmsg_internal("replication command parser returned %d", parse_rc))));
    }

    old_context = MemoryContextSwitchTo(cmd_context);

    cmd_node = t_thrd.replgram_cxt.replication_parse_result;

    switch (cmd_node->type) {
        case T_IdentifySystemCmd:
            IdentifySystem();
            break;

        case T_IdentifyVersionCmd:
            IdentifyVersion();
            break;

        case T_IdentifyModeCmd:
            IdentifyMode();
            break;

        case T_IdentifyMaxLsnCmd:
            IdentifyMaxLsn();
            break;

        case T_IdentifyConsistenceCmd:
            IdentifyConsistence((IdentifyConsistenceCmd*)cmd_node);
            SetReplWalSender();
            break;

        case T_IdentifyChannelCmd:
            IdentifyChannel((IdentifyChannelCmd*)cmd_node);
            break;

        case T_BaseBackupCmd:
            MarkPostmasterChildNormal();
            SetWalSndPeerMode(STANDBY_MODE);
            SetWalSndPeerDbstate(BUILDING_STATE);

            if (!IS_PGXC_COORDINATOR && IS_DN_DUMMY_STANDYS_MODE())
                StopAliveBuildSender();
            SendBaseBackup((BaseBackupCmd*)cmd_node);

            /* Send CommandComplete and ReadyForQuery messages */
            EndCommand_noblock("SELECT", DestRemote);
            ReadyForQuery_noblock(DestRemote, u_sess->attr.attr_storage.wal_sender_timeout);
            /* ReadyForQuery did pq_flush for us */
            /* Audit database recovery */
            pgaudit_system_recovery_ok();
            break;

        case T_CreateReplicationSlotCmd:
            CreateReplicationSlot((CreateReplicationSlotCmd*)cmd_node);
            break;

        case T_DropReplicationSlotCmd:
            DropReplicationSlot((DropReplicationSlotCmd*)cmd_node);
            break;

        case T_StartReplicationCmd: {
            StartReplicationCmd* cmd = (StartReplicationCmd*)cmd_node;
            if (cmd->kind == REPLICATION_KIND_PHYSICAL) {
                StartReplication(cmd);
                /* break out of the loop */
                replication_started = true;
            } else {
                CheckPMstateAndRecoveryInProgress();
                StartLogicalReplication(cmd);
            }
            break;
        }

        case T_FetchMotCheckpointCmd:
            PerformMotCheckpointFetch();
            /* Send CommandComplete and ReadyForQuery messages */
            EndCommand_noblock("SELECT", DestRemote);
            ReadyForQuery_noblock(DestRemote, u_sess->attr.attr_storage.wal_sender_timeout);
            break;

        default:
            ereport(
                FATAL, (errcode(ERRCODE_PROTOCOL_VIOLATION), errmsg("invalid standby query string: %s", cmd_string)));
    }

    /* done */
    (void)MemoryContextSwitchTo(old_context);
    MemoryContextDelete(cmd_context);

    return replication_started;
}

/*
 * Check if the remote end has closed the connection.
 */
static void ProcessRepliesIfAny(void)
{
    unsigned char firstchar;
    int r;
    bool received = false;

    for (;;) {
        r = pq_getbyte_if_available(&firstchar);
        if (r < 0) {
            /* unexpected error or EOF */
            ereport(COMMERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION), errmsg("unexpected EOF on standby connection")));
            proc_exit(0);
        }
        if (r == 0) {
            /* no data available without blocking */
            break;
        }

        /* Handle the very limited subset of commands expected in this phase */
        switch (firstchar) {
                /*
                 * 'd' means a standby reply wrapped in a CopyData packet.
                 */
            case 'd':
                ProcessStandbyMessage();
                received = true;
                break;

                /*
                 * 'X' means that the standby is closing down the socket.
                 */
            case 'X':
                proc_exit(0);
                /* fall-through */
            default:
                ereport(FATAL,
                    (errcode(ERRCODE_PROTOCOL_VIOLATION), errmsg("invalid standby message type \"%c\"", firstchar)));
        }
    }

    /*
     * Save the last reply timestamp if we've received at least one reply.
     */
    if (received) {
        t_thrd.walsender_cxt.last_reply_timestamp = GetCurrentTimestamp();
        t_thrd.walsender_cxt.waiting_for_ping_response = false;
    } else {
        TimestampTz now = GetCurrentTimestamp();
        /* Check for replication timeout. */
        WalSndCheckTimeOut(now);
        /* Send keepalive if the time has come. */
        WalSndKeepaliveIfNecessary(now);
    }
}

/*
 * Process a status update message received from standby.
 */
static void ProcessStandbyMessage(void)
{
    char msgtype;

    resetStringInfo(t_thrd.walsender_cxt.reply_message);

    /*
     * Read the message contents.
     */
    if (pq_getmessage(t_thrd.walsender_cxt.reply_message, 0)) {
        ereport(COMMERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION), errmsg("unexpected EOF on standby connection")));
        proc_exit(0);
    }

    /*
     * Check message type from the first byte.
     */
    msgtype = pq_getmsgbyte(t_thrd.walsender_cxt.reply_message);

    switch (msgtype) {
        case 'r':
            ProcessStandbyReplyMessage();
            break;

        case 'h':
            ProcessStandbyHSFeedbackMessage();
            break;

        case 's':
            ProcessStandbySwitchRequestMessage();
            break;

        case 'A':
            ProcessStandbyFileTimeMessage();
            break;

        default:
            ereport(
                COMMERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION), errmsg("unexpected message type \"%d\"", msgtype)));
            proc_exit(0);
    }
}

/*
 * Remember that a walreceiver just confirmed receipt of lsn `lsn`.
 */
static void PhysicalConfirmReceivedLocation(XLogRecPtr lsn)
{
    bool changed = false;
    /* use volatile pointer to prevent code rearrangement */
    volatile ReplicationSlot* slot = t_thrd.slot_cxt.MyReplicationSlot;

    Assert(!XLByteEQ(lsn, InvalidXLogRecPtr));
    /* not update in boundary */
    if (lsn % XLogSegSize == 0) {
        return;
    }
    SpinLockAcquire(&slot->mutex);
    if (!XLByteEQ(slot->data.restart_lsn, lsn)) {
        changed = true;
        slot->data.restart_lsn = lsn;
    }
    SpinLockRelease(&slot->mutex);

    if (changed) {
        ReplicationSlotMarkDirty();
        ReplicationSlotsComputeRequiredLSN(NULL);
    }

    /*
     * One could argue that the slot should saved to disk now, but that'd be
     * energy wasted - the worst lost information can do here is give us wrong
     * information in a statistics view - we'll just potentially be more
     * conservative in removing files.
     */
}

/*
 * Regular request from standby to send config file.
 */
static void ProcessStandbyFileTimeMessage(void)
{
    ConfigModifyTimeMessage reply_modify_file_time;
    struct stat statbuf;

    pq_copymsgbytes(
        t_thrd.walsender_cxt.reply_message, (char*)&reply_modify_file_time, sizeof(ConfigModifyTimeMessage));
    if (lstat(t_thrd.walsender_cxt.gucconf_file, &statbuf) != 0) {
        if (errno != ENOENT)
            ereport(ERROR,
                (errcode_for_file_access(),
                    errmsg("could not stat file or directory \"%s\": %m", t_thrd.walsender_cxt.gucconf_file)));
    }
    if (reply_modify_file_time.config_modify_time != statbuf.st_mtime) {
        ereport(LOG, (errmsg("the config file has been modified, so send it to the standby")));
        (void)SendConfigFile(t_thrd.walsender_cxt.gucconf_file);
    } else
        ereport(LOG, (errmsg("the config file has no change")));
}

char* remote_role_to_string(int role)
{
    switch (role) {
        case SNDROLE_PRIMARY_STANDBY:
            return "STANDBY";
            break;
        case SNDROLE_PRIMARY_BUILDSTANDBY:
            return "BUILD_STANDBY";
            break;
        case SNDROLE_PRIMARY_DUMMYSTANDBY:
            return "DUMMYSTANDBY";
            break;
        case SNDROLE_DUMMYSTANDBY_STANDBY:
            return "DSTANDBY";
            break;
        default:
            break;
    }

    return "UNKNOW";
}

/*
 * Regular reply from standby advising of WAL positions on standby server.
 */
static void ProcessStandbyReplyMessage(void)
{
    StandbyReplyMessage reply;
    XLogRecPtr sndFlush = InvalidXLogRecPtr;
    int rc;
    pq_copymsgbytes(t_thrd.walsender_cxt.reply_message, (char*)&reply, sizeof(StandbyReplyMessage));

    ereport(DEBUG2,
        (errmsg("receive %X/%X write %X/%X flush %X/%X apply %X/%X",
            (uint32)(reply.receive >> 32),
            (uint32)reply.receive,
            (uint32)(reply.write >> 32),
            (uint32)reply.write,
            (uint32)(reply.flush >> 32),
            (uint32)reply.flush,
            (uint32)(reply.apply >> 32),
            (uint32)reply.apply)));

    /* send a reply if the standby requested one */
    if (reply.replyRequested) {
        WalSndKeepalive(false);
    }
    /* use volatile pointer to prevent code rearrangement */
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;
    if (IS_PGXC_DATANODE) {
        char* standby_name = (char*)(g_instance.rto_cxt.rto_standby_data[walsnd->index].id);
        rc = strncpy_s(standby_name,
            NODENAMELEN,
            u_sess->attr.attr_common.application_name,
            strlen(u_sess->attr.attr_common.application_name));
        securec_check(rc, "\0", "\0");
        if (u_sess->attr.attr_storage.target_rto == 0) {
            g_instance.rto_cxt.rto_standby_data[walsnd->index].current_sleep_time = 0;
        } else {
            g_instance.rto_cxt.rto_standby_data[walsnd->index].current_sleep_time = walsnd->log_ctrl.sleep_time;
        }
        if (g_instance.rto_cxt.rto_standby_data[walsnd->index].target_rto != u_sess->attr.attr_storage.target_rto) {
            ereport(LOG,
                (errmodule(MOD_RTO),
                    errmsg("target_rto changes to %d, previous target_rto is %d, current the sleep time is %ld",
                        u_sess->attr.attr_storage.target_rto,
                        g_instance.rto_cxt.rto_standby_data[walsnd->index].target_rto,
                        g_instance.rto_cxt.rto_standby_data[walsnd->index].current_sleep_time)));

            g_instance.rto_cxt.rto_standby_data[walsnd->index].target_rto = u_sess->attr.attr_storage.target_rto;
        }
    }

    /*
     * Update shared state for this WalSender process based on reply data from
     * standby.
     */
    {
        SpinLockAcquire(&walsnd->mutex);
        walsnd->receive = reply.receive;
        walsnd->write = reply.write;
        walsnd->flush = reply.flush;
        walsnd->apply = reply.apply;
        walsnd->peer_role = reply.peer_role;
        walsnd->peer_state = reply.peer_state;
        SpinLockRelease(&walsnd->mutex);
    }

    /*
     * calculate RTO every sleep_count_limit.
     */
    if (IS_PGXC_DATANODE && walsnd->log_ctrl.sleep_count % walsnd->log_ctrl.sleep_count_limit == 0) {
        LogCtrlCalculateCurrentRTO(&reply);
        walsnd->log_ctrl.prev_reply_time = reply.sendTime;
        walsnd->log_ctrl.prev_flush = reply.flush;
        walsnd->log_ctrl.prev_apply = reply.apply;
    }

    /* try to control log sent rate so that standby can flush and apply log under RTO seconds */
    if (walsnd->state == WALSNDSTATE_STREAMING && u_sess->attr.attr_storage.target_rto > 0 && IS_PGXC_DATANODE) {
        if (walsnd->log_ctrl.sleep_count % walsnd->log_ctrl.sleep_count_limit == 0) {
            LogCtrlCalculateSleepTime();
            LogCtrlCountSleepLimit();
        }
        LogCtrlSleep();
    }
    walsnd->log_ctrl.sleep_count++;

    if (!AM_WAL_STANDBY_SENDER)
        SyncRepReleaseWaiters();

    /*
     * Advance our local xmin horizon when the client confirmed a flush.
     */
    if (t_thrd.slot_cxt.MyReplicationSlot && (!XLByteEQ(reply.flush, InvalidXLogRecPtr))) {
        if (t_thrd.slot_cxt.MyReplicationSlot->data.database != InvalidOid) {
            LogicalConfirmReceivedLocation(reply.flush);
        } else {
            PhysicalConfirmReceivedLocation(reply.flush);
        }
    }

    if (AM_WAL_STANDBY_SENDER) {
        sndFlush = GetFlushRecPtr();
        WalSndRefreshPercentCountStartLsn(sndFlush, reply.flush);
    }
}

/* compute new replication slot xmin horizon if needed */
static void PhysicalReplicationSlotNewXmin(TransactionId feedbackXmin)
{
    bool changed = false;
    volatile ReplicationSlot* slot = t_thrd.slot_cxt.MyReplicationSlot;

    SpinLockAcquire(&slot->mutex);
    t_thrd.pgxact->xmin = InvalidTransactionId;
    /*
     * For physical replication we don't need the the interlock provided
     * by xmin and effective_xmin since the consequences of a missed increase
     * are limited to query cancellations, so set both at once.
     */
    if (!TransactionIdIsNormal(slot->data.xmin) || !TransactionIdIsNormal(feedbackXmin) ||
        TransactionIdPrecedes(slot->data.xmin, feedbackXmin)) {
        changed = true;
        slot->data.xmin = feedbackXmin;
        slot->effective_xmin = feedbackXmin;
    }
    SpinLockRelease(&slot->mutex);

    if (changed) {
        ReplicationSlotMarkDirty();
        ReplicationSlotsComputeRequiredXmin(false);
    }
}

/*
 * Hot Standby feedback
 */
static void ProcessStandbyHSFeedbackMessage(void)
{
    StandbyHSFeedbackMessage reply;
    TransactionId nextXid;

    /* Decipher the reply message */
    pq_copymsgbytes(t_thrd.walsender_cxt.reply_message, (char*)&reply, sizeof(StandbyHSFeedbackMessage));

    ereport(DEBUG2, (errmsg("hot standby feedback xmin " XID_FMT, reply.xmin)));

    /* Ignore invalid xmin (can't actually happen with current walreceiver) */
    if (!TransactionIdIsNormal(reply.xmin)) {
        if (t_thrd.slot_cxt.MyReplicationSlot != NULL)
            PhysicalReplicationSlotNewXmin(reply.xmin);
        return;
    }
    nextXid = ReadNewTransactionId();
    if (!TransactionIdPrecedesOrEquals(reply.xmin, nextXid)) {
        /* epoch OK, but it's wrapped around */
        return;
    }

    /*
     * Set the WalSender's xmin equal to the standby's requested xmin, so that
     * the xmin will be taken into account by GetOldestXmin.  This will hold
     * back the removal of dead rows and thereby prevent the generation of
     * cleanup conflicts on the standby server.
     *
     * There is a small window for a race condition here: although we just
     * checked that reply.xmin precedes nextXid, the nextXid could have gotten
     * advanced between our fetching it and applying the xmin below, perhaps
     * far enough to make reply.xmin wrap around.  In that case the xmin we
     * set here would be "in the future" and have no effect.  No point in
     * worrying about this since it's too late to save the desired data
     * anyway.	Assuming that the standby sends us an increasing sequence of
     * xmins, this could only happen during the first reply cycle, else our
     * own xmin would prevent nextXid from advancing so far.
     *
     * We don't bother taking the ProcArrayLock here.  Setting the xmin field
     * is assumed atomic, and there's no real need to prevent a concurrent
     * GetOldestXmin.  (If we're moving our xmin forward, this is obviously
     * safe, and if we're moving it backwards, well, the data is at risk
     * already since a VACUUM could have just finished calling GetOldestXmin.)
     */
    /* If we're using a replication slot we reserve the xmin via that,
     * otherwise via the walsender's PGXACT entry.

     * XXX: It might make sense to introduce ephemeral slots and always use
     * the slot mechanism.
     */
    /* XXX: persistency configurable? */
    if (t_thrd.slot_cxt.MyReplicationSlot != NULL) {
        PhysicalReplicationSlotNewXmin(reply.xmin);
    } else {
        t_thrd.pgxact->xmin = reply.xmin;
    }
}

/*
 * process message from standby request primary server making switchover.
 */
static void ProcessStandbySwitchRequestMessage(void)
{
    int i;
    StandbySwitchRequestMessage message;

    pq_copymsgbytes(t_thrd.walsender_cxt.reply_message, (char*)&message, sizeof(StandbySwitchRequestMessage));

    if (message.demoteMode < SmartDemote || message.demoteMode > FastDemote) {
        ereport(
            WARNING, (errmsg("invalid switchover mode in standby switchover request message: %d", message.demoteMode)));
        return;
    }

    SpinLockAcquire(&t_thrd.walsender_cxt.WalSndCtl->mutex);

    /*
     * If the catchup thread is alive, we must stop the demoting process
     * at once. There will be some risk of losting data when catchup can't send the data pages.
     */
    if (catchup_online) {
        volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;
        ClusterNodeState old_mode = walsnd->node_state;

        walsnd->node_state = NODESTATE_PRIMARY_DEMOTING_WAIT_CATCHUP;
        WalSndResponseSwitchover(t_thrd.walsender_cxt.output_xlog_message);
        walsnd->node_state = old_mode;

        SpinLockRelease(&t_thrd.walsender_cxt.WalSndCtl->mutex);
        ereport(NOTICE, (errmsg("could not continuing switchover process when catchup is alive.")));
        return;
    }

    if (t_thrd.walsender_cxt.WalSndCtl->demotion > NoDemote &&
        t_thrd.walsender_cxt.Demotion != t_thrd.walsender_cxt.WalSndCtl->demotion) {
        SpinLockRelease(&t_thrd.walsender_cxt.WalSndCtl->mutex);
        ereport(NOTICE, (errmsg("master is doing switchover,\
						 probably another standby already requested switchover.")));
        return;
    } else if (message.demoteMode <= t_thrd.walsender_cxt.Demotion) {
        SpinLockRelease(&t_thrd.walsender_cxt.WalSndCtl->mutex);
        ereport(NOTICE,
            (errmsg("the standby already requested %s switchover, so ignore new %s switchover request.",
                DemoteModeDesc(t_thrd.walsender_cxt.Demotion),
                DemoteModeDesc(message.demoteMode))));
        return;
    }

    t_thrd.walsender_cxt.WalSndCtl->demotion = (DemoteMode)message.demoteMode;

    /* update the demote state */
    for (i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
        volatile WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];

        if (walsnd->pid == 0) {
            continue;
        }

        walsnd->node_state = NODESTATE_PRIMARY_DEMOTING;
    }
    t_thrd.walsender_cxt.MyWalSnd->node_state = (ClusterNodeState)message.demoteMode;

    SpinLockRelease(&t_thrd.walsender_cxt.WalSndCtl->mutex);

    t_thrd.walsender_cxt.Demotion = (DemoteMode)message.demoteMode;
    ereport(
        LOG, (errmsg("received %s switchover request from standby", DemoteModeDesc(t_thrd.walsender_cxt.Demotion))));

    SendPostmasterSignal(PMSIGNAL_DEMOTE_PRIMARY);
}

/*
 * Count the limit for sleep_count, it is based on sleep time.
 */
static void LogCtrlCountSleepLimit(void)
{
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;
    int64 sleep_count_limit_count;
    if (walsnd->log_ctrl.sleep_time == 0) {
        sleep_count_limit_count = MAX_CONTROL_REPLY;
    } else {
        sleep_count_limit_count = INIT_CONTROL_REPLY * MICROSECONDS_PER_SECONDS / walsnd->log_ctrl.sleep_time;
        sleep_count_limit_count =
            (sleep_count_limit_count > MAX_CONTROL_REPLY) ? MAX_CONTROL_REPLY : sleep_count_limit_count;
    }
    if (sleep_count_limit_count <= 0) {
        sleep_count_limit_count = INIT_CONTROL_REPLY;
    }
    walsnd->log_ctrl.sleep_count_limit = sleep_count_limit_count;
}

/*
 * Update the sleep time for primary.
 */
static void LogCtrlSleep(void)
{
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;
    if (walsnd->log_ctrl.sleep_time > 0 && walsnd->log_ctrl.sleep_time <= MICROSECONDS_PER_SECONDS) {
        pg_usleep(walsnd->log_ctrl.sleep_time);
    } else if (walsnd->log_ctrl.sleep_time > MICROSECONDS_PER_SECONDS) {
        pg_usleep(MICROSECONDS_PER_SECONDS);
        walsnd->log_ctrl.sleep_time = MICROSECONDS_PER_SECONDS;
    }
    g_instance.rto_cxt.rto_standby_data[walsnd->index].current_sleep_time = walsnd->log_ctrl.sleep_time;
}

static inline uint64 LogCtrlCountBigSpeed(uint64 originSpeed, uint64 curSpeed)
{
    uint64 updateSpeed = (((originSpeed << SHIFT_SPEED) - originSpeed) >> SHIFT_SPEED) + curSpeed;
    return updateSpeed;
}

/*
 * Estimate the time standby need to flush and apply log.
 */
static void LogCtrlCalculateCurrentRTO(StandbyReplyMessage* reply)
{
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;
    long sec_to_time;
    int microsec_to_time;
    if (XLByteLT(reply->receive, reply->flush) || XLByteLT(reply->flush, reply->apply) ||
        XLByteLT(reply->flush, walsnd->log_ctrl.prev_flush) || XLByteLT(reply->apply, walsnd->log_ctrl.prev_apply)) {
        return;
    }

    uint64 part1 = reply->receive - reply->flush;
    uint64 part2 = reply->flush - reply->apply;
    uint64 part1_diff = reply->flush - walsnd->log_ctrl.prev_flush;
    uint64 part2_diff = reply->apply - walsnd->log_ctrl.prev_apply;
    if (walsnd->log_ctrl.prev_reply_time == 0) {
        return;
    }

    TimestampDifference(walsnd->log_ctrl.prev_reply_time, reply->sendTime, &sec_to_time, &microsec_to_time);
    long millisec_time_diff = sec_to_time * MILLISECONDS_PER_SECONDS + microsec_to_time / MILLISECONDS_PER_MICROSECONDS;
    if (millisec_time_diff <= 0) {
        return;
    }

    /*
     * consumeRatePart1 and consumeRatePart2 is based on 7/8 previous_speed(walsnd->log_ctrl.pre_rate1) and 1/8
     * speed_now(part1_diff / millisec_time_diff). To be more precise and keep more decimal point, we expand speed_now
     * by multiply first then divide, which is (8 * previous_speed * 7/8 + speed_now) / 8.
     */
    if (walsnd->log_ctrl.pre_rate1 != 0) {
        walsnd->log_ctrl.pre_rate1 =
            LogCtrlCountBigSpeed(walsnd->log_ctrl.pre_rate1, (uint64)(part1_diff / millisec_time_diff));
    } else {
        walsnd->log_ctrl.pre_rate1 = ((part1_diff / (uint64)millisec_time_diff) << SHIFT_SPEED);
    }
    if (walsnd->log_ctrl.pre_rate2 != 0) {
        walsnd->log_ctrl.pre_rate2 =
            LogCtrlCountBigSpeed(walsnd->log_ctrl.pre_rate2, (uint64)(part2_diff / millisec_time_diff));
    } else {
        walsnd->log_ctrl.pre_rate2 = ((uint64)(part2_diff / millisec_time_diff) << SHIFT_SPEED);
    }

    uint64 consumeRatePart1 = (walsnd->log_ctrl.pre_rate1 >> SHIFT_SPEED);
    uint64 consumeRatePart2 = (walsnd->log_ctrl.pre_rate2 >> SHIFT_SPEED);
    if (consumeRatePart1 == 0 || consumeRatePart2 == 0) {
        return;
    }

    uint64 sec_RTO_part1 = (part1 / consumeRatePart1) / MILLISECONDS_PER_SECONDS;
    uint64 sec_RTO_part2 = ((part1 + part2) / consumeRatePart2) / MILLISECONDS_PER_SECONDS;
    uint64 sec_RTO = sec_RTO_part1 > sec_RTO_part2 ? sec_RTO_part1 : sec_RTO_part2;
    walsnd->log_ctrl.prev_RTO = walsnd->log_ctrl.current_RTO;
    walsnd->log_ctrl.current_RTO = sec_RTO;
    g_instance.rto_cxt.rto_standby_data[walsnd->index].current_rto = walsnd->log_ctrl.current_RTO;
    ereport(DEBUG4,
        (errmodule(MOD_RTO),
            errmsg("The RTO estimated is = : %lu seconds. reply->reveive is %lu, reply->flush is %lu, "
                   "reply->apply is %lu, pre_flush is %lu, pre_apply is %lu, TimestampDifference is %ld, "
                   "consumeRatePart1 is %lu, consumeRatePart2 is %lu",
                sec_RTO,
                reply->receive,
                reply->flush,
                reply->apply,
                walsnd->log_ctrl.prev_flush,
                walsnd->log_ctrl.prev_apply,
                millisec_time_diff,
                consumeRatePart1,
                consumeRatePart2)));
}

/*
 * If current RTO is less than target_rto, primary need less sleep.
 * If current RTO is more than target_rto, primary need more sleep.
 * If current RTO equals to target_rto, primary will sleep according to balance_sleep to maintain rto.
 */
static void LogCtrlCalculateSleepTime(void)
{
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;
    if (walsnd->log_ctrl.prev_RTO < 0) {
        walsnd->log_ctrl.prev_RTO = walsnd->log_ctrl.current_RTO;
        return;
    }

    int targetRTO = u_sess->attr.attr_storage.target_rto;
    int64 currentRTO = walsnd->log_ctrl.current_RTO;
    int64 gap = currentRTO - targetRTO;
    int64 prev_gap = walsnd->log_ctrl.prev_RTO - targetRTO;
    int64 gap_diff = gap - prev_gap;
    int64 sleepTime = walsnd->log_ctrl.sleep_time;
    /* use for rto log */
    int64 pre_time = walsnd->log_ctrl.sleep_time;

    /* mark balance sleep time */
    if (abs(gap_diff) <= (targetRTO / FAULT_TOLERANCE)) {
        walsnd->log_ctrl.balance_sleep_time = sleepTime;
        ereport(DEBUG4,
            (errmodule(MOD_RTO),
                errmsg("The balance time for log control is : %ld microseconds", walsnd->log_ctrl.balance_sleep_time)));
    }

    /* rto balance, currentRTO close to targetRTO */
    if (abs(gap) <= (targetRTO / FAULT_TOLERANCE)) {
        if (walsnd->log_ctrl.balance_sleep_time != 0) {
            walsnd->log_ctrl.sleep_time = walsnd->log_ctrl.balance_sleep_time;
        } else {
            sleepTime -= SLEEP_LESS;
            walsnd->log_ctrl.sleep_time = (sleepTime >= 0) ? sleepTime : 0;
        }
    }

    /* need more sleep, currentRTO larger than targetRTO
     *  get bigger, but no more than 1s
     */
    if (gap > (targetRTO / FAULT_TOLERANCE)) {
        sleepTime += SLEEP_MORE;
        walsnd->log_ctrl.sleep_time = (sleepTime < 1 * MICROSECONDS_PER_SECONDS) ? sleepTime : MICROSECONDS_PER_SECONDS;
    }

    /* need less sleep, currentRTO less than targetRTO */
    if (gap < -(targetRTO / FAULT_TOLERANCE)) {
        sleepTime -= SLEEP_LESS;
        walsnd->log_ctrl.sleep_time = (sleepTime >= 0) ? sleepTime : 0;
    }
    /* log control take effect */
    if (pre_time == 0 && walsnd->log_ctrl.sleep_time != 0) {
        ereport(LOG,
            (errmodule(MOD_RTO),
                errmsg("Log control take effect, target_rto is %d, current_rto is %ld, current the sleep time is %ld "
                       "microseconds",
                    u_sess->attr.attr_storage.target_rto,
                    walsnd->log_ctrl.current_RTO,
                    walsnd->log_ctrl.sleep_time)));
    }
    /* log control take does not effect */
    if (pre_time != 0 && walsnd->log_ctrl.sleep_time == 0) {
        ereport(LOG,
            (errmodule(MOD_RTO),
                errmsg("Log control does not take effect, target_rto is %d, current_rto is %ld, current the sleep time "
                       "is %ld microseconds",
                    u_sess->attr.attr_storage.target_rto,
                    walsnd->log_ctrl.current_RTO,
                    walsnd->log_ctrl.sleep_time)));
    }
    ereport(DEBUG4,
        (errmodule(MOD_RTO),
            errmsg("The sleep time for log control is : %ld microseconds", walsnd->log_ctrl.sleep_time)));
}

static void ChooseStartPointForDummyStandby(void)
{
    XLogRecPtr initSentPtr;

    if (!XLByteEQ(t_thrd.walsender_cxt.sentPtr, InvalidXLogRecPtr)) {
        ereport(DEBUG1,
            (errmsg("use current sentPtr %X/%X as sync secondary startpoint",
                (uint32)(t_thrd.walsender_cxt.sentPtr >> 32),
                (uint32)t_thrd.walsender_cxt.sentPtr)));
        return;
    }

    t_thrd.xlog_cxt.server_mode = PRIMARY_MODE;
    ReplicationSlotsComputeRequiredLSN(NULL);
    initSentPtr = XLogGetReplicationSlotMaximumLSN();

    ReplicationSlotReportRestartLSN();
    ereport(LOG,
        (errmsg("init sentPtr at %X/%X, latestValidRecord is %X/%X",
            (uint32)(initSentPtr >> 32),
            (uint32)initSentPtr,
            (uint32)(latestValidRecord >> 32),
            (uint32)latestValidRecord)));

    if (XLByteEQ(initSentPtr, InvalidXLogRecPtr))
        initSentPtr = latestValidRecord;

    /*
     * Ref RequestXLogStreaming()
     * We always start at the beginning of the segment. That prevents a broken
     * segment (i.e., with no records in the first half of a segment) from
     * being created by XLOG streaming, which might cause trouble later on if
     * the segment is e.g archived.
     * Prev the requested segment if request xlog from the beginning of a segment.
     */
    if (initSentPtr % XLogSegSize != 0) {
        initSentPtr -= initSentPtr % XLogSegSize;
    } else {
        XLogSegNo _logSeg;
        XLByteToSeg(initSentPtr, _logSeg);
        _logSeg--;
        initSentPtr = _logSeg * XLogSegSize;
    }

    t_thrd.walsender_cxt.sentPtr = MAX_XLOG_RECORD(t_thrd.walsender_cxt.sentPtr, initSentPtr);

    ereport(DEBUG2,
        (errmsg(
            "In ChooseStartPointForDummyStandby(): initSentPtr is %X/%X, latestValidRecord is %X/%X, sentPtr is %X/%X.",
            (uint32)(initSentPtr >> 32),
            (uint32)initSentPtr,
            (uint32)(latestValidRecord >> 32),
            (uint32)latestValidRecord,
            (uint32)(t_thrd.walsender_cxt.sentPtr >> 32),
            (uint32)t_thrd.walsender_cxt.sentPtr)));
}
static int WSXLogPageRead(XLogReaderState* xlogreader, XLogRecPtr targetPagePtr, int reqLen, XLogRecPtr targetRecPtr,
    char* readBuf, TimeLineID* pageTLI)
{
    WSXLogPageReadPrivate* ws_private = (WSXLogPageReadPrivate*)xlogreader->private_data;
    uint32 targetPageOff;
    int nRetCode = 0;
    char xlogfile[MAXFNAMELEN];
    char xlogfpath[MAXPGPATH];

    if (ws_private == NULL) {
        Assert(false);
        ereport(WARNING, (errmsg("The WAL Streaming XLog Reader Private Info is NULL.")));
        return -1;
    }

    if (ws_private->xlogreadfd >= 0 && !XLByteInSeg(targetPagePtr, ws_private->xlogreadlogsegno)) {
        (void)close(ws_private->xlogreadfd);
        ws_private->xlogreadfd = -1;
    }

    XLByteToSeg(targetPagePtr, ws_private->xlogreadlogsegno);

    if (ws_private->xlogreadfd < 0) {
        XLogFileName(xlogfile, ws_private->tli, ws_private->xlogreadlogsegno);

        nRetCode = snprintf_s(xlogfpath, MAXPGPATH, MAXPGPATH - 1, XLOGDIR "/%s", xlogfile);
        securec_check_ss(nRetCode, "\0", "\0");

        ws_private->xlogreadfd = BasicOpenFile(xlogfpath, O_RDONLY | PG_BINARY, 0);

        if (ws_private->xlogreadfd < 0) {
            ereport(DEBUG2, (errmsg("could not open the xlog file %s: %s.", xlogfpath, gs_strerror(errno))));
            return -1;
        }
    }

    targetPageOff = targetPagePtr % XLogSegSize;

    /* Read the requested page */
    if (lseek(ws_private->xlogreadfd, (off_t)targetPageOff, SEEK_SET) < 0) {
        Assert(false);
        ereport(WARNING,
            (errmsg("could not seek %u bytes in the file %s: %s.", targetPageOff, xlogfpath, gs_strerror(errno))));
        return -1;
    }

    if (read(ws_private->xlogreadfd, readBuf, XLOG_BLCKSZ) != XLOG_BLCKSZ) {
        Assert(false);
        ereport(WARNING,
            (errmsg("could not read the request %d bytes in the xlog file %s: %s.",
                reqLen,
                xlogfpath,
                gs_strerror(errno))));
        return -1;
    }

    *pageTLI = ws_private->tli;
    return XLOG_BLCKSZ;
}

static void InitWalSndXLogReader()
{
    WSXLogPageReadPrivate* ws_private = NULL;
    errno_t rc = 0;

    if (t_thrd.walsender_cxt.ws_xlog_reader) {
        if (t_thrd.walsender_cxt.ws_xlog_reader->private_data) {
            pfree((WSXLogPageReadPrivate*)t_thrd.walsender_cxt.ws_xlog_reader->private_data);
            t_thrd.walsender_cxt.ws_xlog_reader->private_data = NULL;
        }

        XLogReaderFree(t_thrd.walsender_cxt.ws_xlog_reader);
        t_thrd.walsender_cxt.ws_xlog_reader = NULL;
    }

    /*
     * Allocate the xlogreader used for xlog parsing.
     */
    ws_private = (WSXLogPageReadPrivate*)palloc(sizeof(WSXLogPageReadPrivate));

    /* Set up XLOG reader facility */
    rc = memset_s(ws_private, sizeof(WSXLogPageReadPrivate), 0, sizeof(WSXLogPageReadPrivate));
    securec_check(rc, "\0", "\0");
    ws_private->xlogreadfd = -1;
    ws_private->tli = t_thrd.xlog_cxt.ThisTimeLineID;

    t_thrd.walsender_cxt.ws_xlog_reader = XLogReaderAllocate(&WSXLogPageRead, ws_private);

    if (!t_thrd.walsender_cxt.ws_xlog_reader)
        ereport(ERROR, (errcode(ERRCODE_INVALID_STATUS), errmsg("Failed to init the xlog reader for the wal sender.")));
    else
        ereport(LOG, (errmsg("Succeeded to init the xlog reader for the wal sender.")));

    return;
}

static void WSDataSendInit()
{
    /*
     * Allocate buffer that will be used for each output message.  We do this
     * just once to reduce palloc overhead.  The buffer must be made large
     * enough for maximum-sized messages.
     */
    if (!g_instance.attr.attr_storage.enable_mix_replication)
        t_thrd.walsender_cxt.output_xlog_message =
            (char*)palloc(1 + sizeof(WalDataMessageHeader) + (int)WS_MAX_SEND_SIZE);
    else {
        t_thrd.walsender_cxt.output_xlog_msg_prefix_len =
            1 + sizeof(WalDataMessageHeader) + sizeof(uint32) + 1 + sizeof(XLogRecPtr);
        t_thrd.walsender_cxt.output_xlog_message =
            (char*)palloc(t_thrd.walsender_cxt.output_xlog_msg_prefix_len + (int)WS_MAX_SEND_SIZE);
        t_thrd.walsender_cxt.output_data_message = (char*)palloc(
            1 + sizeof(WalDataPageMessageHeader) + sizeof(uint32) + 1 + sizeof(XLogRecPtr) * 2 + (int)WS_MAX_SEND_SIZE);
        t_thrd.walsender_cxt.output_data_msg_cur_len = 0;
        t_thrd.walsender_cxt.load_cu_buffer = (char*)palloc(t_thrd.walsender_cxt.load_cu_buffer_size);

        InitWalSndXLogReader();

        t_thrd.walsender_cxt.wsXLogJustSendRegion->start_ptr = InvalidXLogRecPtr;
        t_thrd.walsender_cxt.wsXLogJustSendRegion->end_ptr = InvalidXLogRecPtr;
    }

    return;
}

/* Main loop of walsender process */
static int WalSndLoop(WalSndSendDataCallback send_data)
{
    bool first_startup = true;
    bool sync_config_needed = false;
    bool marked_stream_replication = true;
    TimestampTz last_syncconf_timestamp;

    WSDataSendInit();

    /*
     * Allocate buffer that will be used for processing reply messages.  As
     * above, do this just once to reduce palloc overhead.
     */
    initStringInfo(t_thrd.walsender_cxt.reply_message);
    initStringInfo(t_thrd.walsender_cxt.tmpbuf);

    /* Initialize the last reply timestamp */
    t_thrd.walsender_cxt.last_reply_timestamp = GetCurrentTimestamp();
    last_syncconf_timestamp = GetCurrentTimestamp();
    t_thrd.walsender_cxt.last_logical_xlog_advanced_timestamp = GetCurrentTimestamp();
    t_thrd.walsender_cxt.waiting_for_ping_response = false;

    /* Loop forever, unless we get an error */
    for (;;) {
        TimestampTz now;

        /* Clear any already-pending wakeups */
        ResetLatch(&t_thrd.walsender_cxt.MyWalSnd->latch);
#ifdef ENABLE_DISTRIBUTE_TEST
        if (TEST_STUB(DN_WALSEND_MAINLOOP, stub_sleep_emit)) {
            ereport(get_distribute_test_param()->elevel,
                (errmsg("sleep_emit happen during WalSndLoop  time:%ds, stub_name:%s",
                    get_distribute_test_param()->sleep_time,
                    get_distribute_test_param()->test_stub_name)));
        }
#endif

        pgstat_report_activity(STATE_RUNNING, NULL);

        /*
         * Emergency bailout if postmaster has died.  This is to avoid the
         * necessity for manual cleanup of all postmaster children.
         */
        if (!PostmasterIsAlive())
            gs_thread_exit(1);

        /* Process any requests or signals received recently */
        if (t_thrd.walsender_cxt.got_SIGHUP) {
            t_thrd.walsender_cxt.got_SIGHUP = false;
            marked_stream_replication = u_sess->attr.attr_storage.enable_stream_replication;
            ProcessConfigFile(PGC_SIGHUP);
            SyncRepInitConfig();
            sync_config_needed = true;
        }

        /* switchover is forbidden when catchup thread in progress */
        if (catchup_online && t_thrd.walsender_cxt.WalSndCtl->demotion > NoDemote) {
            volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;
            SpinLockAcquire(&t_thrd.walsender_cxt.WalSndCtl->mutex);

            walsnd->node_state = NODESTATE_PRIMARY_DEMOTING_WAIT_CATCHUP;
            WalSndResponseSwitchover(t_thrd.walsender_cxt.output_xlog_message);

            /* recover node_state and demotion state */
            walsnd->node_state = NODESTATE_NORMAL;
            t_thrd.walsender_cxt.WalSndCtl->demotion = NoDemote;
            t_thrd.walsender_cxt.Demotion = NoDemote;

            SpinLockRelease(&t_thrd.walsender_cxt.WalSndCtl->mutex);

            ereport(PANIC,
                (errmsg("walsender stop switchover process for catchup is alive, the process need to be restart")));
        }

        /* Normal exit from the walsender is here */
        if ((t_thrd.walsender_cxt.walsender_shutdown_requested &&
                !t_thrd.walsender_cxt.response_switchover_requested) ||
            t_thrd.walsender_cxt.MyWalSnd->node_state == NODESTATE_STANDBY_REDIRECT) {
            /* Inform the standby that XLOG streaming is done */
            if (!sync_config_needed) {
                pq_puttextmessage('C', "COPY 0");
                (void)pq_flush();

                proc_exit(0);
            }
        }

        /* if changed to stream replication, request for catchup. */
        if (u_sess->attr.attr_storage.enable_stream_replication && !marked_stream_replication) {
            marked_stream_replication = u_sess->attr.attr_storage.enable_stream_replication;
            WalSndSetState(WALSNDSTATE_CATCHUP);
        }

        if (t_thrd.walsender_cxt.response_switchover_requested) {
            if (t_thrd.walsender_cxt.MyWalSnd->peer_role != STANDBY_MODE) {
                ereport(LOG, (errmsg("walsender closed because of switchover.")));
                proc_exit(0);
            }
        }

        /* Check for input from the client */
        ProcessRepliesIfAny();

        /* Walsender first startup, send a keepalive to standby, no need reply. */
        if (first_startup) {
            WalSndKeepalive(false);
            first_startup = false;
        }

        /* send switchover response message to standby if requested */
        if (t_thrd.walsender_cxt.response_switchover_requested) {
            XLogRecPtr WriteRqstPtr;
            uint32 XLogPageOffSet;

            WriteRqstPtr = GetXLogInsertEndRecPtr();
            XLogPageOffSet = WriteRqstPtr % XLOG_BLCKSZ;

            ereport(LOG, (errmsg("The WAL sender in primary is ready to do the switchover.")));

            ereport(
                LOG, (errmsg("the latest WAL insert at %X/%X", (uint32)(WriteRqstPtr >> 32), (uint32)WriteRqstPtr)));

            /*
             * Check whether the write requestptr points to the end of new
             * page header, we try to flush to the end of last page instead
             * of the new page header.
             */
            if (SizeOfXLogLongPHD == XLogPageOffSet || SizeOfXLogShortPHD == WriteRqstPtr % XLogSegSize) {
                WriteRqstPtr -= XLogPageOffSet;
                ereport(LOG,
                    (errmsg("the latest WAL insert back off to %X/%X",
                        (uint32)(WriteRqstPtr >> 32),
                        (uint32)WriteRqstPtr)));
            }

            /*
             * Do a last xlog flush; then, if XLogNeedsFlush() found useful
             * work to do, continue to loop.
             */
            if (XLogNeedsFlush(WriteRqstPtr)) {
                XLogFlush(WriteRqstPtr);
                ereport(LOG,
                    (errmsg("the latest WAL flush to %X/%X.", (uint32)(WriteRqstPtr >> 32), (uint32)WriteRqstPtr)));
            } else {
                XLogRecPtr SendRqstPtr;
                SendRqstPtr = AM_WAL_STANDBY_SENDER ? GetStandbyFlushRecPtr(NULL) : GetFlushRecPtr();
                /* Quick exit if nothing to do */
                if (XLByteLE(SendRqstPtr, t_thrd.walsender_cxt.MyWalSnd->flush) &&
                    !t_thrd.walsender_cxt.wal_send_completed) {
                    t_thrd.walsender_cxt.wal_send_completed = true;
                    ereport(LOG,
                        (errmsg("the latest WAL complete at %X/%X", (uint32)(SendRqstPtr >> 32), (uint32)SendRqstPtr)));
                } else
                    ereport(LOG,
                        (errmsg("the latest WAL flush at %X/%X the latest standby flush at %X/%X",
                            (uint32)(SendRqstPtr >> 32),
                            (uint32)SendRqstPtr,
                            (uint32)(t_thrd.walsender_cxt.MyWalSnd->flush >> 32),
                            (uint32)t_thrd.walsender_cxt.MyWalSnd->flush)));

                if (!DataSndInProgress(SNDROLE_PRIMARY_STANDBY | SNDROLE_PRIMARY_DUMMYSTANDBY) &&
                    !WalSndInProgress(SNDROLE_PRIMARY_DUMMYSTANDBY | SNDROLE_PRIMARY_STANDBY) &&
                    t_thrd.walsender_cxt.wal_send_completed) {
                    t_thrd.walsender_cxt.response_switchover_requested = false;
                    WalSndResponseSwitchover(t_thrd.walsender_cxt.output_xlog_message);
                    ereport(LOG,
                        (errmsg(
                            "The WAL sender in primary has done the switchover waiting for the standby's promotion.")));
                }
            }
        }

        if (sync_config_needed) {
            if (t_thrd.walsender_cxt.walsender_shutdown_requested) {
                if (!SendConfigFile(t_thrd.walsender_cxt.gucconf_file))
                    ereport(LOG, (errmsg("failed to send config to the peer when walsender shutdown.")));
                sync_config_needed = false;
            } else {
                TimestampTz nowtime = GetCurrentTimestamp();
                if (TimestampDifferenceExceeds(last_syncconf_timestamp, nowtime, 1000) ||
                    last_syncconf_timestamp > nowtime) {
                    sync_config_needed = false;
                    /* begin send file to standby */
                    if (t_thrd.walsender_cxt.MyWalSnd && t_thrd.walsender_cxt.MyWalSnd->peer_state != BUILDING_STATE) {
                        if (!SendConfigFile(t_thrd.walsender_cxt.gucconf_file))
                            sync_config_needed = true;
                        else
                            last_syncconf_timestamp = nowtime;
                    } else {
                        sync_config_needed = false;
                        ereport(LOG, (errmsg("receive sigup,but the peer is building!")));
                    }
                }
            }
        }

        if (AmWalSenderToDummyStandby()) {
            /*
             * If i am sender to dummy and streaming to standby is online, do not
             * send WAL to dummy. Especially, set WalSndCaughtUp to true, if the dummy
             * sender is "out of work".
             */
            if (WalSndCaughtup()) {
                WalSndCaughtUp = true;
                t_thrd.walsender_cxt.sentPtr = InvalidXLogRecPtr;

                /* Close open wal file */
                if (t_thrd.walsender_cxt.sendFile >= 0) {
                    (void)close(t_thrd.walsender_cxt.sendFile);
                    t_thrd.walsender_cxt.sendFile = -1;
                }

                if (u_sess->attr.attr_storage.HaModuleDebug) {
                    ereport(LOG,
                        (errmsg("standby is steaming, "
                                "stop sync to walsender, recycle local data.")));
                }

                /* Notify dummy to cleanup WAL. False means not need response. */
                if (WalSndDummyLEStandby()) {
                    WalSndRmXLog(false);
                }

                /* Set dummy standby replication slot lsn invalid */
                if (g_instance.attr.attr_storage.max_replication_slots > 0)
                    SetDummyStandbySlotLsnInvalid();
            } else {
                ChooseStartPointForDummyStandby();

                if (!pq_is_send_pending())
                    send_data();
                else
                    WalSndCaughtUp = false;

                /* Send DummyStandby end message */
                if (WalSndCaughtUp) {
                    /* Try to flush pending output to the client */
                    if (pq_flush_if_writable() != 0)
                        break;

                    if (!pq_is_send_pending())
                        WalSndSyncDummyStandbyDone(false);
                }
            }
        } else {
            /*
             * If we don't have any pending data in the output buffer, try to send
             * some more.  If there is some, we don't bother to call XLogSend
             * again until we've flushed it ... but we'd better assume we are not
             * caught up.
             */
            if (!pq_is_send_pending())
                send_data();
            else
                WalSndCaughtUp = false;

            if (WalSndCaughtUp && dummyStandbyMode) {
                if (!pq_is_send_pending()) {
                    WalSndSyncDummyStandbyDone(false);
                    (void)pq_flush();
                    ereport(LOG,
                        (errmsg("dummystandby wal data replication completed at %X/%X",
                            (uint32)(t_thrd.walsender_cxt.sentPtr >> 32),
                            (uint32)t_thrd.walsender_cxt.sentPtr)));
                }
            }
        }

        /* Try to flush pending output to the client */
        if (pq_flush_if_writable() != 0) {
            ereport(LOG, (errmsg("flush return not zero !\n")));
            break;
        }

        /* If nothing remains to be sent right now ... */
        if (WalSndCaughtUp && !pq_is_send_pending()) {
            /*
             * If we're in catchup state, move to streaming.  This is an
             * important state change for users to know about, since before
             * this point data loss might occur if the primary dies and we
             * need to failover to the standby. The state change is also
             * important for synchronous replication, since commits that
             * started to wait at that point might wait for some time.
             */
            if (t_thrd.walsender_cxt.MyWalSnd->state == WALSNDSTATE_CATCHUP) {
                ereport(DEBUG1,
                    (errmsg(
                        "standby \"%s\" has now caught up with primary", u_sess->attr.attr_common.application_name)));
                WalSndSetState(WALSNDSTATE_STREAMING);
                /* Refresh new state to peer */
                WalSndKeepalive(true);
            }

            t_thrd.walsender_cxt.catchup_threshold = 0;

            /*
             * When SIGUSR2 arrives, we send any outstanding logs up to the
             * shutdown checkpoint record (i.e., the latest record), wait
             * for them to be replicated to the standby, and exit.
             * This may be a normal termination at shutdown, or a promotion,
             * the walsender is not sure which.
             */
            if (t_thrd.walsender_cxt.walsender_ready_to_stop) {
                /*
                 * Let's just be real sure we're caught up. For dummy sender,
                 * during shutting down, if the sender to standby is in progress,
                 * skip to send outstanding logs.
                 */
                if (AmWalSenderToDummyStandby() && WalSndInProgress(SNDROLE_PRIMARY_STANDBY))
                    ; /* nothing to do */
                else
                    send_data();

                if (WalSndCaughtUp && !pq_is_send_pending()) {
                    if (dummyStandbyMode ||
                        XLByteEQ(t_thrd.walsender_cxt.sentPtr, t_thrd.walsender_cxt.MyWalSnd->flush))
                        t_thrd.walsender_cxt.walsender_shutdown_requested = true;
                }
            }
        } else {
            if (t_thrd.walsender_cxt.MyWalSnd->state == WALSNDSTATE_STREAMING &&
                !XLByteLT(t_thrd.walsender_cxt.catchup_threshold,
                    INT2UINT64(g_instance.attr.attr_storage.MaxSendSize) * 1024)) {
                ereport(DEBUG1,
                    (errmsg(
                        "standby \"%s\" has now caught up with primary", u_sess->attr.attr_common.application_name)));
                WalSndSetState(WALSNDSTATE_CATCHUP);
                t_thrd.walsender_cxt.catchup_threshold = 0;
            }
        }

        now = GetCurrentTimestamp();

        if (u_sess->proc_cxt.MyDatabaseId != InvalidOid)
            WalSndWriteLogicalAdvanceXLog(now);

        /*
         * We don't block if not caught up, unless there is unsent data
         * pending in which case we'd better block until the socket is
         * write-ready.  This test is only needed for the case where XLogSend
         * loaded a subset of the available data but then pq_flush_if_writable
         * flushed it all --- we should immediately try to send more.
         */
        if (WalSndCaughtUp || pq_is_send_pending()) {
            long sleeptime;
            int wakeEvents;

            wakeEvents = WL_LATCH_SET | WL_POSTMASTER_DEATH | WL_SOCKET_READABLE | WL_TIMEOUT;

            sleeptime = WalSndComputeSleeptime(now);

            if (pq_is_send_pending())
                wakeEvents |= WL_SOCKET_WRITEABLE;
            else if (first_startup) {
                /* Walsender first startup, send a keepalive to standby, no need reply. */
                WalSndKeepalive(false);
                first_startup = false;
            }

            /*
             * if requested to response switchover, walsender need not to wait for new xlog data.
             * if requested to shutdown, walsender need not to wait for new xlog data.
             */
            if (t_thrd.walsender_cxt.response_switchover_requested || t_thrd.walsender_cxt.walsender_shutdown_requested)
                sleeptime = 100; /* 0.1s */

            /* Sleep until something happens or we time out */
            pgstat_report_activity(STATE_IDLE, NULL);
            t_thrd.int_cxt.ImmediateInterruptOK = true;
            CHECK_FOR_INTERRUPTS();

            if (sleeptime > u_sess->attr.attr_storage.wal_sender_timeout / 2)
                sleeptime = u_sess->attr.attr_storage.wal_sender_timeout / 2;

            WaitLatchOrSocket(
                &t_thrd.walsender_cxt.MyWalSnd->latch, wakeEvents, u_sess->proc_cxt.MyProcPort->sock, sleeptime);
            t_thrd.int_cxt.ImmediateInterruptOK = false;
        }

        if (!bSyncStat && !dummyStandbyMode) {
            if (XLByteEQ(GetFlushRecPtr(), t_thrd.walsender_cxt.sentPtr) && SyncRepRequested() &&
                most_available_sync == false) {
                bSyncStat = true;
                ereport(LOG, (errmsg("The primary and standby reached syncstat in WalSndLoop.")));
            }
        }
    }

    WalSndShutdown();
    return 1; /* keep the compiler quiet */
}

/*
 * Compute how long send/receive loops should sleep.
 *
 * If wal_sender_timeout is enabled we want to wake up in time to send
 * keepalives and to abort the connection if wal_sender_timeout has been
 * reached.
 */
static long WalSndComputeSleeptime(TimestampTz now)
{
    /*
     * Formally, sleep time is set according to wal sender timeout.
     * Time is too long and sender can only be waked up when latch
     * is set, resulting in poor performance. Here reduced to 1s.
     */
    long sleeptime = 1000;

    if (u_sess->attr.attr_storage.wal_sender_timeout > 0 && t_thrd.walsender_cxt.last_reply_timestamp > 0) {
        TimestampTz wakeup_time;
        long sec_to_timeout;
        int microsec_to_timeout;

        /*
         * At the latest stop sleeping once wal_sender_timeout has been
         * reached.
         */
        wakeup_time = TimestampTzPlusMilliseconds(
            t_thrd.walsender_cxt.last_reply_timestamp, u_sess->attr.attr_storage.wal_sender_timeout);

        /*
         * If no ping has been sent yet, wakeup when it's time to do so.
         * DataSndKeepaliveIfNecessary() wants to send a keepalive once half of
         * the timeout passed without a response.
         */
        if (!t_thrd.walsender_cxt.waiting_for_ping_response)
            wakeup_time = TimestampTzPlusMilliseconds(
                t_thrd.walsender_cxt.last_reply_timestamp, u_sess->attr.attr_storage.wal_sender_timeout / 2);

        /* Compute relative time until wakeup. */
        TimestampDifference(now, wakeup_time, &sec_to_timeout, &microsec_to_timeout);

        sleeptime = sec_to_timeout * 1000 + microsec_to_timeout / 1000;
    }

    return sleeptime;
}

/*
 * Check if time since last write xlog of logical slot advancing has reached the limit.
 * If reached, write a new xlog.
 */
static void WalSndWriteLogicalAdvanceXLog(TimestampTz now)
{
    TimestampTz timegap;
    if (t_thrd.walsender_cxt.last_logical_xlog_advanced_timestamp <= 0)
        return;

    timegap = TimestampTzPlusMilliseconds(
        t_thrd.walsender_cxt.last_logical_xlog_advanced_timestamp, t_thrd.walsender_cxt.logical_xlog_advanced_timeout);
    if (t_thrd.walsender_cxt.logical_xlog_advanced_timeout > 0 && now >= timegap) {
        ereport(LOG, (errmsg("write xlog of logical slot advanced")));
        log_slot_advance(&t_thrd.slot_cxt.MyReplicationSlot->data);
        t_thrd.walsender_cxt.last_logical_xlog_advanced_timestamp = now;
    }
}

static TimestampTz GetHeartbeatLastReplyTimestamp()
{
    int replindex;
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;
    SpinLockAcquire(&walsnd->mutex);
    replindex = walsnd->channel_get_replc;
    SpinLockRelease(&walsnd->mutex);

    return get_last_reply_timestamp(replindex);
}

/* return timeout time */
static inline TimestampTz CalculateTimeout(TimestampTz last_reply_time)
{
    const int MULTIPLE_TIME = 4;
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;

    if (walsnd->sendRole == SNDROLE_PRIMARY_BUILDSTANDBY) {
        return TimestampTzPlusMilliseconds(
            last_reply_time, MULTIPLE_TIME * u_sess->attr.attr_storage.wal_sender_timeout);
    } else {
        return TimestampTzPlusMilliseconds(last_reply_time, u_sess->attr.attr_storage.wal_sender_timeout);
    }
}

/*
 * Check if time since last receive from standby has reached the
 * configured limit.
 */
static void WalSndCheckTimeOut(TimestampTz now)
{
    TimestampTz timeout;

    /* don't bail out if we're doing something that doesn't require timeouts */
    if (u_sess->attr.attr_storage.wal_sender_timeout <= 0 || t_thrd.walsender_cxt.last_reply_timestamp <= 0) {
        return;
    }

    /*
     * Use static last_reply_time to avoid call GetHeartbeatLastReplyTimestamp frequently
     * when t_thrd.walsender_cxt.last_reply_timestamp has meet the timeout condition
     * but last heartbeat time doesn't.
     */
    static TimestampTz last_reply_time = t_thrd.walsender_cxt.last_reply_timestamp;
    /* t_thrd.walsender_cxt.last_reply_timestamp newer */
    if (timestamptz_cmp_internal(t_thrd.walsender_cxt.last_reply_timestamp, last_reply_time) > 0) {
        last_reply_time = t_thrd.walsender_cxt.last_reply_timestamp;
    }

    timeout = CalculateTimeout(last_reply_time);
    if (now < timeout) {
        return;
    }

    TimestampTz heartbeat = GetHeartbeatLastReplyTimestamp();
    /* If heartbeat newer, use heartbeat to recalculate timeout. */
    if (timestamptz_cmp_internal(heartbeat, last_reply_time) > 0) {
        last_reply_time = heartbeat;
        timeout = CalculateTimeout(last_reply_time);
    }

    if (heartbeat != 0 && now >= timeout) {
        /*
         * Since typically expiration of replication timeout means
         * communication problem, we don't send the error message to the
         * standby.
         */
        ereport(ERROR,
            (errmsg("terminating Walsender process due to replication timeout."
                    "now time(%s) timeout time(%s) last recv time(%s) heartbeat time(%s)",
                timestamptz_to_str(now),
                timestamptz_to_str(timeout),
                timestamptz_to_str(last_reply_time),
                timestamptz_to_str(heartbeat))));
        WalSndShutdown();
    }
}

/* Initialize a per-walsender data structure for this walsender process */
static void InitWalSnd(void)
{
    int i;
    errno_t rc = 0;

    /*
     * WalSndCtl should be set up already (we inherit this by fork() or
     * EXEC_BACKEND mechanism from the postmaster).
     */
    Assert(t_thrd.walsender_cxt.WalSndCtl != NULL);
    Assert(t_thrd.walsender_cxt.MyWalSnd == NULL);

    /*
     * Find a free walsender slot and reserve it. If this fails, we must be
     * out of WalSnd structures.
     */
    for (i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];

        SpinLockAcquire(&walsnd->mutex);

        if (walsnd->pid != 0) {
            SpinLockRelease(&walsnd->mutex);
            continue;
        } else {
            /*
             * Found a free slot. Reserve it for us.
             */
            walsnd->pid = t_thrd.proc_cxt.MyProcPid;
            rc = memset_s((XLogRecPtr* )&walsnd->sentPtr, sizeof(XLogRecPtr), 0, sizeof(XLogRecPtr));
            securec_check(rc, "\0", "\0");
            walsnd->state = WALSNDSTATE_STARTUP;
            walsnd->node_state = NODESTATE_NORMAL;
            if (dummyStandbyMode) {
                walsnd->sendRole = SNDROLE_DUMMYSTANDBY_STANDBY;
            } else if (t_thrd.postmaster_cxt.senderToDummyStandby) {
                walsnd->sendRole = SNDROLE_PRIMARY_DUMMYSTANDBY;
            } else if (t_thrd.postmaster_cxt.senderToBuildStandby) {
                walsnd->sendRole = SNDROLE_PRIMARY_BUILDSTANDBY;
            } else {
                walsnd->sendRole = SNDROLE_PRIMARY_STANDBY;
            }

            walsnd->sendKeepalive = true;
            walsnd->replSender = false;
            walsnd->peer_role = UNKNOWN_MODE;
            walsnd->peer_state = NORMAL_STATE;
            walsnd->channel_get_replc = 0;
            rc = memset_s((XLogRecPtr*)&walsnd->receive, sizeof(XLogRecPtr), 0, sizeof(XLogRecPtr));
            securec_check(rc, "\0", "\0");
            rc = memset_s((XLogRecPtr*)&walsnd->write, sizeof(XLogRecPtr), 0, sizeof(XLogRecPtr));
            securec_check(rc, "\0", "\0");
            rc = memset_s((XLogRecPtr*)&walsnd->flush, sizeof(XLogRecPtr), 0, sizeof(XLogRecPtr));
            securec_check(rc, "\0", "\0");
            rc = memset_s((XLogRecPtr*)&walsnd->apply, sizeof(XLogRecPtr), 0, sizeof(XLogRecPtr));
            securec_check(rc, "\0", "\0");
            rc = memset_s((XLogRecPtr*)&walsnd->data_flush, sizeof(XLogRecPtr), 0, sizeof(XLogRecPtr));
            securec_check(rc, "\0", "\0");
            rc = memset_s((XLogRecPtr*)&walsnd->wal_sender_channel, sizeof(ReplConnInfo), 0, sizeof(ReplConnInfo));
            securec_check(rc, "\0", "\0");
            walsnd->sync_standby_priority = 0;
            walsnd->index = i;
            walsnd->log_ctrl.sleep_time = 0;
            walsnd->log_ctrl.balance_sleep_time = 0;
            walsnd->log_ctrl.prev_RTO = -1;
            walsnd->log_ctrl.current_RTO = -1;
            walsnd->log_ctrl.sleep_count = 0;
            walsnd->log_ctrl.sleep_count_limit = MAX_CONTROL_REPLY;
            walsnd->log_ctrl.prev_flush = 0;
            walsnd->log_ctrl.prev_apply = 0;
            walsnd->log_ctrl.prev_reply_time = 0;
            walsnd->log_ctrl.pre_rate1 = 0;
            walsnd->log_ctrl.pre_rate2 = 0;
            SpinLockRelease(&walsnd->mutex);
            /* don't need the lock anymore */
            OwnLatch((Latch*)&walsnd->latch);
            t_thrd.walsender_cxt.MyWalSnd = (WalSnd*)walsnd;

            break;
        }
    }
    if (t_thrd.walsender_cxt.MyWalSnd == NULL)
        ereport(FATAL,
            (errcode(ERRCODE_TOO_MANY_CONNECTIONS),
                errmsg("number of requested standby connections "
                       "exceeds max_wal_senders (currently %d)",
                    g_instance.attr.attr_storage.max_wal_senders)));

    /* Arrange to clean up at walsender exit */
    on_shmem_exit(WalSndKill, 0);
}

/* Mark WalSnd struct no longer in use. */
static void WalSndReset(WalSnd *walsnd)
{
    errno_t	rc = 0;

    SpinLockAcquire(&walsnd->mutex);
    walsnd->pid = 0;
    walsnd->lwpId = 0;
    walsnd->peer_role = UNKNOWN_MODE;
    walsnd->replSender = false;
    walsnd->wal_sender_channel.localport = 0;
    walsnd->wal_sender_channel.localservice = 0;
    walsnd->wal_sender_channel.remoteport = 0;
    walsnd->wal_sender_channel.remoteservice = 0;
    walsnd->channel_get_replc = 0;
    rc = memset_s(walsnd->wal_sender_channel.localhost, sizeof(walsnd->wal_sender_channel.localhost), 0,
        sizeof(walsnd->wal_sender_channel.localhost));
    securec_check_c(rc, "\0", "\0");
    rc = memset_s(walsnd->wal_sender_channel.remotehost, sizeof(walsnd->wal_sender_channel.remotehost), 0,
        sizeof(walsnd->wal_sender_channel.remotehost));
    securec_check_c(rc, "\0", "\0");
    SpinLockRelease(&walsnd->mutex);
}

/* Destroy the per-walsender data structure for this walsender process */
static void WalSndKill(int code, Datum arg)
{
    WalSnd *walsnd = t_thrd.walsender_cxt.MyWalSnd;

    Assert(walsnd != NULL);

    /*
     * Clear MyWalSnd first; then disown the latch.  This is so that signal
     * handlers won't try to touch the latch after it's no longer ours.
     */
    t_thrd.walsender_cxt.MyWalSnd = NULL;

    DisownLatch(&walsnd->latch);

    if (code > 0) {
        /* Sleep at least 0.1 second to wait for reporting the error to the client */
        pg_usleep(100000L);
    }

    if (dummyStandbyMode) {
        set_failover_host_conninfo_for_dummy(walsnd->wal_sender_channel.remotehost,
            t_thrd.walsender_cxt.remotePort);
        t_thrd.walsender_cxt.remotePort = 0;
    }

    /* Mark WalSnd struct no longer in use. */
    WalSndReset(walsnd);

    /*
     * Here one standby is going down, then check if it was synchronous
     * standby and also there is no more synchronous standby, if yes
     * then wake all waiting transaction and also change the master
     * mode to standalone. Should check lock is held or not already to
     * prevent deadlock. (e.g., fatal occurs when lock held and then
     * re-acquire the same lock when process exits)
     */
    if (LWLockHeldByMe(SyncRepLock)) {
        LWLockRelease(SyncRepLock);
    }
    LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
    SyncRepCheckSyncStandbyAlive();
    LWLockRelease(SyncRepLock);

    /* Close open wal file */
    if (t_thrd.walsender_cxt.sendFile >= 0) {
        (void)close(t_thrd.walsender_cxt.sendFile);
        t_thrd.walsender_cxt.sendFile = -1;
    }

    t_thrd.walsender_cxt.wsXLogJustSendRegion->start_ptr = InvalidXLogRecPtr;
    t_thrd.walsender_cxt.wsXLogJustSendRegion->end_ptr = InvalidXLogRecPtr;

    ereport(LOG, (errmsg("walsender thread shut down")));
}

/*
 * Handle a client's connection abort in an orderly manner.
 */
static void WalSndShutdown(void)
{
    /*
     * Reset whereToSendOutput to prevent ereport from attempting to send any
     * more messages to the standby.
     */
    ereport(LOG, (errmsg("wal send shut down !\n")));
    if (t_thrd.postgres_cxt.whereToSendOutput == DestRemote)
        t_thrd.postgres_cxt.whereToSendOutput = DestNone;

    proc_exit(0);
    abort(); /* keep the compiler quiet */
}

/*
 * Read 'count' bytes from WAL into 'buf', starting at location 'startptr'.
 * XXX probably this should be improved to suck data directly from the
 * WAL buffers when possible. Will open, and keep open, one WAL segment 
 * stored in the global file descriptor sendFile. This means if XLogRead is used
 * once, there will always be one descriptor left open until the process ends, but never
 * more than one.
 */
static void XLogRead(char* buf, XLogRecPtr startptr, Size count)
{
    char* p = NULL;
    XLogRecPtr recptr;
    Size nbytes;
    XLogSegNo segno;

retry:
    p = buf;
    recptr = startptr;
    nbytes = count;

    while (nbytes > 0) {
        uint32 startoff;
        int segbytes;
        int readbytes;

        startoff = recptr % XLogSegSize;

        /* Do we need to switch to a different xlog segment? */
        if (t_thrd.walsender_cxt.sendFile < 0 || !XLByteInSeg(recptr, t_thrd.walsender_cxt.sendSegNo)) {
            char path[MAXPGPATH];

            if (t_thrd.walsender_cxt.sendFile >= 0) {
                (void)close(t_thrd.walsender_cxt.sendFile);
            }

            XLByteToSeg(recptr, t_thrd.walsender_cxt.sendSegNo);
            XLogFilePath(path, t_thrd.xlog_cxt.ThisTimeLineID, t_thrd.walsender_cxt.sendSegNo);

            t_thrd.walsender_cxt.sendFile = BasicOpenFile(path, O_RDONLY | PG_BINARY, 0);
            if (t_thrd.walsender_cxt.sendFile < 0) {
                /*
                 * If the file is not found, assume it's because the standby
                 * asked for a too old WAL segment that has already been
                 * removed or recycled.
                 */
                if (errno == ENOENT) {
                    /* we suppose wal segments removed happend when we can't open the xlog file. */
                    WalSegmemtRemovedhappened = true;
                    ereport(ERROR,
                        (errcode_for_file_access(),
                            errmsg("requested WAL segment %s has already been removed",
                                XLogFileNameP(t_thrd.xlog_cxt.ThisTimeLineID, t_thrd.walsender_cxt.sendSegNo))));
                } else {
                    ereport(ERROR,
                        (errcode_for_file_access(),
                            errmsg("could not open file \"%s\" (log segment %s): %m",
                                path,
                                XLogFileNameP(t_thrd.xlog_cxt.ThisTimeLineID, t_thrd.walsender_cxt.sendSegNo))));
                }
            }
            t_thrd.walsender_cxt.sendOff = 0;
        }

        /* Need to seek in the file? */
        if (t_thrd.walsender_cxt.sendOff != startoff) {
            if (lseek(t_thrd.walsender_cxt.sendFile, (off_t)startoff, SEEK_SET) < 0) {
                (void)close(t_thrd.walsender_cxt.sendFile);
                t_thrd.walsender_cxt.sendFile = -1;
                ereport(ERROR,
                    (errcode_for_file_access(),
                        errmsg("could not seek in log segment %s to offset %u: %m",
                            XLogFileNameP(t_thrd.xlog_cxt.ThisTimeLineID, t_thrd.walsender_cxt.sendSegNo),
                            startoff)));
            }
            t_thrd.walsender_cxt.sendOff = startoff;
        }

        /* How many bytes are within this segment? */
        if (nbytes > (XLogSegSize - startoff)) {
            segbytes = XLogSegSize - startoff;
        } else {
            segbytes = nbytes;
        }

        pgstat_report_waitevent(WAIT_EVENT_WAL_READ);
        readbytes = read(t_thrd.walsender_cxt.sendFile, p, segbytes);
        pgstat_report_waitevent(WAIT_EVENT_END);
        if (readbytes <= 0) {
            (void)close(t_thrd.walsender_cxt.sendFile);
            t_thrd.walsender_cxt.sendFile = -1;
            ereport(ERROR,
                (errcode_for_file_access(),
                    errmsg("could not read from log segment %s, offset %u, length %lu: %m",
                        XLogFileNameP(t_thrd.xlog_cxt.ThisTimeLineID, t_thrd.walsender_cxt.sendSegNo),
                        t_thrd.walsender_cxt.sendOff,
                        INT2ULONG(segbytes))));
        }

        /* Update state for read */
        XLByteAdvance(recptr, readbytes);

        t_thrd.walsender_cxt.sendOff += readbytes;
        nbytes -= readbytes;
        p += readbytes;
    }

    /*
     * After reading into the buffer, check that what we read was valid. We do
     * this after reading, because even though the segment was present when we
     * opened it, it might get recycled or removed while we read it. The
     * read() succeeds in that case, but the data we tried to read might
     * already have been overwritten with new WAL records.
     */
    XLByteToSeg(startptr, segno);
    CheckXLogRemoved(segno, t_thrd.xlog_cxt.ThisTimeLineID);

    /*
     * During recovery, the currently-open WAL file might be replaced with the
     * file of the same name retrieved from archive. So we always need to
     * check what we read was valid after reading into the buffer. If it's
     * invalid, we try to open and read the file again.
     */
    if (AM_WAL_STANDBY_SENDER) {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;
        bool reload = false;

        SpinLockAcquire(&walsnd->mutex);
        reload = walsnd->needreload;
        walsnd->needreload = false;
        SpinLockRelease(&walsnd->mutex);

        if (reload && t_thrd.walsender_cxt.sendFile >= 0) {
            (void)close(t_thrd.walsender_cxt.sendFile);
            t_thrd.walsender_cxt.sendFile = -1;

            goto retry;
        }
    }

    /* we open the xlog file success. it seems we are in good status. */
    WalSegmemtRemovedhappened = false;
}

/*
 * Stream out logically decoded data.
 */
static void XLogSendLogical(void)
{
    CheckPMstateAndRecoveryInProgress();
    XLogRecord* record = NULL;
    char* errm = NULL;

    /*
     * Don't know whether we've caught up yet. We'll set it to true in
     * WalSndWaitForWal, if we're actually waiting. We also set to true if
     * XLogReadRecord() had to stop reading but WalSndWaitForWal didn't wait -
     * i.e. when we're shutting down.
     */
    WalSndCaughtUp = false;

    record =
        XLogReadRecord(t_thrd.walsender_cxt.logical_decoding_ctx->reader, t_thrd.walsender_cxt.logical_startptr, &errm);
    t_thrd.walsender_cxt.logical_startptr = InvalidXLogRecPtr;

    /* xlog record was invalid */
    if (errm != NULL)
        ereport(ERROR,
            (errcode(ERRCODE_LOGICAL_DECODE_ERROR),
                errmsg("Stopped to parse any valid XLog Record at %X/%X: %s.",
                    (uint32)(t_thrd.walsender_cxt.logical_decoding_ctx->reader->EndRecPtr >> 32),
                    (uint32)t_thrd.walsender_cxt.logical_decoding_ctx->reader->EndRecPtr,
                    errm)));

    if (record != NULL) {
        LogicalDecodingProcessRecord(
            t_thrd.walsender_cxt.logical_decoding_ctx, t_thrd.walsender_cxt.logical_decoding_ctx->reader);

        t_thrd.walsender_cxt.sentPtr = t_thrd.walsender_cxt.logical_decoding_ctx->reader->EndRecPtr;
    } else {
        /*
         * If the record we just wanted read is at or beyond the flushed point,
         * then we're caught up.
         */
        if (t_thrd.walsender_cxt.logical_decoding_ctx->reader->EndRecPtr >= GetFlushRecPtr())
            WalSndCaughtUp = true;
    }

    /* Update shared memory status */
    {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;

        SpinLockAcquire(&walsnd->mutex);
        walsnd->sentPtr = t_thrd.walsender_cxt.sentPtr;
        SpinLockRelease(&walsnd->mutex);
    }
}

/*
 * Read up to MAX_SEND_SIZE bytes of WAL that's been flushed to disk,
 * but not yet sent to the client, and buffer it in the libpq output buffer.
 *
 * If there is no unsent WAL remaining, *caughtup is set to true, otherwise
 * *caughtup is set to false.
 */
static void XLogSendPhysical(void)
{
    XLogRecPtr SendRqstPtr = InvalidXLogRecPtr;
    XLogRecPtr startptr = InvalidXLogRecPtr;
    XLogRecPtr endptr = InvalidXLogRecPtr;
    Size nbytes = 0;
    WalDataMessageHeader msghdr;
    ServerMode local_role;
    volatile HaShmemData* hashmdata = t_thrd.postmaster_cxt.HaShmData;
    errno_t errorno = EOK;

    t_thrd.walsender_cxt.catchup_threshold = 0;

    /*
     * Attempt to send all data that's already been written out and fsync'd to
     * disk.  We cannot go further than what's been written out given the
     * current implementation of XLogRead().  And in any case it's unsafe to
     * send WAL that is not securely down to disk on the master: if the master
     * subsequently crashes and restarts, slaves must not have applied any WAL
     * that gets lost on the master.
     */
    if (AM_WAL_STANDBY_SENDER) {
        TimeLineID currentTargetTLI;
        SendRqstPtr = GetStandbyFlushRecPtr(&currentTargetTLI);

        /*
         * If the recovery target timeline changed, bail out. It's a bit
         * unfortunate that we have to just disconnect, but there is no way
         * to tell the client that the timeline changed. We also don't know
         * exactly where the switch happened, so we cannot safely try to send
         * up to the switchover point before disconnecting.
         */
        if (currentTargetTLI != t_thrd.xlog_cxt.ThisTimeLineID) {
            if (!t_thrd.walsender_cxt.walsender_ready_to_stop)
                ereport(LOG,
                    (errmsg("terminating walsender process to force cascaded standby "
                            "to update timeline and reconnect")));
            t_thrd.walsender_cxt.walsender_ready_to_stop = true;
            WalSndCaughtUp = true;
            return;
        }
    } else if (dummyStandbyMode)
        SendRqstPtr = GetWalRcvWriteRecPtr(NULL);
    else
        SendRqstPtr = GetFlushRecPtr();

    /* Quick exit if nothing to do */
    if (!u_sess->attr.attr_storage.enable_stream_replication || XLByteLE(SendRqstPtr, t_thrd.walsender_cxt.sentPtr)) {
        WalSndCaughtUp = true;
        return;
    }

    /*
     * Figure out how much to send in one message. If there's no more than
     * MAX_SEND_SIZE bytes to send, send everything. Otherwise send
     * MAX_SEND_SIZE bytes, but round back to logfile or page boundary.
     *
     * The rounding is not only for performance reasons. Walreceiver relies on
     * the fact that we never split a WAL record across two messages. Since a
     * long WAL record is split at page boundary into continuation records,
     * page boundary is always a safe cut-off point. We also assume that
     * SendRqstPtr never points to the middle of a WAL record.
     */
    startptr = t_thrd.walsender_cxt.sentPtr;
    endptr = startptr;
    XLByteAdvance(endptr, g_instance.attr.attr_storage.MaxSendSize * 1024);

    /* if we went beyond SendRqstPtr, back off */
    if (XLByteLE(SendRqstPtr, endptr)) {
        endptr = SendRqstPtr;
        WalSndCaughtUp = true;
    } else {
        /* round down to page boundary. */
        endptr -= (endptr % XLOG_BLCKSZ);
        WalSndCaughtUp = false;
        t_thrd.walsender_cxt.catchup_threshold = XLByteDifference(SendRqstPtr, endptr);
    }

    nbytes = endptr - startptr;
    Assert(nbytes <= (Size)g_instance.attr.attr_storage.MaxSendSize * 1024);

    if (nbytes == 0)
        ereport(NOTICE,
            (errmsg("streaming body is empty, "
                    "request send: %X/%X, already sent: %X/%X",
                (uint32)(SendRqstPtr >> 32),
                (uint32)SendRqstPtr,
                (uint32)(t_thrd.walsender_cxt.sentPtr >> 32),
                (uint32)t_thrd.walsender_cxt.sentPtr)));

    /*
     * OK to read and send the slice.
     */
    t_thrd.walsender_cxt.output_xlog_message[0] = 'w';

    /*
     * Read the log directly into the output buffer to avoid extra memcpy
     * calls.
     */
    XLogRead(t_thrd.walsender_cxt.output_xlog_message + 1 + sizeof(WalDataMessageHeader), startptr, nbytes);
    ereport(DEBUG5,
        (errmsg("conninfo:(%s,%d) start: %X/%X, end: %X/%X, %lu bytes",
            t_thrd.walsender_cxt.MyWalSnd->wal_sender_channel.localhost,
            t_thrd.walsender_cxt.MyWalSnd->wal_sender_channel.localport,
            (uint32)(startptr >> 32),
            (uint32)startptr,
            (uint32)(endptr >> 32),
            (uint32)endptr,
            nbytes)));

    /*
     * We fill the message header last so that the send timestamp is taken as
     * late as possible.
     */
    msghdr.dataStart = startptr;
    msghdr.walEnd = SendRqstPtr;
    msghdr.sendTime = GetCurrentTimestamp();
    msghdr.sender_sent_location = endptr;
    msghdr.catchup = (t_thrd.walsender_cxt.MyWalSnd->state == WALSNDSTATE_CATCHUP && !WalSndCaughtUp);
    SpinLockAcquire(&hashmdata->mutex);
    local_role = hashmdata->current_mode;
    SpinLockRelease(&hashmdata->mutex);
    if (local_role == PRIMARY_MODE) {
        /* Local role is a primary */
        msghdr.sender_flush_location = GetFlushRecPtr();
        msghdr.sender_replay_location = msghdr.sender_flush_location;
        msghdr.sender_write_location = GetXLogWriteRecPtr();
    } else {
        /* Local role is not a primary */
        msghdr.sender_write_location = GetWalRcvWriteRecPtr(NULL);
        msghdr.sender_flush_location = GetStandbyFlushRecPtr(NULL);
        msghdr.sender_replay_location = GetXLogReplayRecPtr(NULL);
    }

    errorno = memcpy_s(t_thrd.walsender_cxt.output_xlog_message + 1,
        sizeof(WalDataMessageHeader) + g_instance.attr.attr_storage.MaxSendSize * 1024,
        &msghdr,
        sizeof(WalDataMessageHeader));
    securec_check(errorno, "\0", "\0");
    (void)pq_putmessage_noblock(
        'd', t_thrd.walsender_cxt.output_xlog_message, 1 + sizeof(WalDataMessageHeader) + nbytes);

    t_thrd.walsender_cxt.sentPtr = endptr;

    /* Update shared memory status */
    {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;

        SpinLockAcquire(&walsnd->mutex);
        walsnd->sentPtr = t_thrd.walsender_cxt.sentPtr;
        SpinLockRelease(&walsnd->mutex);
    }

    /* Report progress of XLOG streaming in PS display */
    if (u_sess->attr.attr_common.update_process_title) {
        char activitymsg[50];
        int rc = 0;

        rc = snprintf_s(activitymsg,
            sizeof(activitymsg),
            sizeof(activitymsg) - 1,
            "streaming %X/%X",
            (uint32)(t_thrd.walsender_cxt.sentPtr >> 32),
            (uint32)t_thrd.walsender_cxt.sentPtr);
        securec_check_ss(rc, "\0", "\0");

        set_ps_display(activitymsg, false);
    }

    return;
}

/*
 * Request walsenders to reload the currently-open WAL file
 */
void WalSndRqstFileReload(void)
{
    int i;

    for (i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];

        if (walsnd->pid == 0)
            continue;

        SpinLockAcquire(&walsnd->mutex);
        walsnd->needreload = true;
        SpinLockRelease(&walsnd->mutex);
    }
}

/* SIGHUP: set flag to re-read config file at next convenient time */
static void WalSndSigHupHandler(SIGNAL_ARGS)
{
    int save_errno = errno;

    t_thrd.walsender_cxt.got_SIGHUP = true;
    if (t_thrd.walsender_cxt.MyWalSnd)
        SetLatch(&t_thrd.walsender_cxt.MyWalSnd->latch);

    errno = save_errno;
}

/* SIGTERM: set flag to shut down */
static void WalSndShutdownHandler(SIGNAL_ARGS)
{
    int save_errno = errno;

    t_thrd.walsender_cxt.walsender_shutdown_requested = true;
    if (t_thrd.walsender_cxt.MyWalSnd)
        SetLatch(&t_thrd.walsender_cxt.MyWalSnd->latch);

    /*
     * Set the standard (non-walsender) state as well, so that we can abort
     * things like do_pg_stop_backup().
     */
    InterruptPending = true;
    t_thrd.int_cxt.ProcDiePending = true;

    errno = save_errno;
}

/*
 * WalSndQuickDieHandler() occurs when signalled SIGQUIT by the postmaster.
 *
 * Some backend has bought the farm,
 * so we need to stop what we're doing and exit.
 */
static void WalSndQuickDieHandler(SIGNAL_ARGS)
{
    gs_signal_setmask(&t_thrd.libpq_cxt.BlockSig, NULL);

    /*
     * We DO NOT want to run proc_exit() callbacks -- we're here because
     * shared memory may be corrupted, so we don't want to try to clean up our
     * transaction.  Just nail the windows shut and get out of town.  Now that
     * there's an atexit callback to prevent third-party code from breaking
     * things by calling exit() directly, we have to reset the callbacks
     * explicitly to make this work as intended.
     */
    on_exit_reset();

    /*
     * Note we do exit(2) not exit(0).	This is to force the postmaster into a
     * system reset cycle if some idiot DBA sends a manual SIGQUIT to a random
     * backend.  This is necessary precisely because we don't clean up our
     * shared memory state.  (The "dead man switch" mechanism in pmsignal.c
     * should ensure the postmaster sees this as a crash, too, but no harm in
     * being doubly sure.)
     */
    exit(2);
}

/* SIGUSR1: set flag to send WAL records */
static void WalSndXLogSendHandler(SIGNAL_ARGS)
{
    int save_errno = errno;

    latch_sigusr1_handler();

    errno = save_errno;
}

/* SIGUSR2: set flag to do a last cycle and shut down afterwards */
static void WalSndLastCycleHandler(SIGNAL_ARGS)
{
    int save_errno = errno;

    t_thrd.walsender_cxt.walsender_ready_to_stop = true;
    if (t_thrd.walsender_cxt.MyWalSnd)
        SetLatch(&t_thrd.walsender_cxt.MyWalSnd->latch);

    if (IS_DN_DUMMY_STANDYS_MODE()) {
        if (t_thrd.walsender_cxt.MyWalSnd && !AmWalSenderToDummyStandby() &&
            (t_thrd.walsender_cxt.MyWalSnd->node_state == NODESTATE_PROMOTE_APPROVE ||
                t_thrd.walsender_cxt.MyWalSnd->node_state == NODESTATE_STANDBY_REDIRECT))
            t_thrd.walsender_cxt.response_switchover_requested = true;
    } else {
        if (t_thrd.walsender_cxt.MyWalSnd && t_thrd.walsender_cxt.MyWalSnd->node_state == NODESTATE_PROMOTE_APPROVE)
            t_thrd.walsender_cxt.response_switchover_requested = true;
    }

    errno = save_errno;
}

/* Set up signal handlers */
void WalSndSignals(void)
{
    /* Set up signal handlers */
    (void)gspqsignal(SIGHUP, WalSndSigHupHandler);    /* set flag to read config file */
    (void)gspqsignal(SIGINT, SIG_IGN);                /* not used */
    (void)gspqsignal(SIGTERM, WalSndShutdownHandler); /* request shutdown */
    (void)gspqsignal(SIGQUIT, WalSndQuickDieHandler); /* hard crash time */
    (void)gspqsignal(SIGALRM, handle_sig_alarm);
    (void)gspqsignal(SIGPIPE, SIG_IGN);
    (void)gspqsignal(SIGUSR1, WalSndXLogSendHandler);  /* request WAL sending */
    (void)gspqsignal(SIGUSR2, WalSndLastCycleHandler); /* request a last cycle and shutdown */

    /* Reset some signals that are accepted by postmaster but not here */
    (void)gspqsignal(SIGCHLD, SIG_DFL);
    (void)gspqsignal(SIGTTIN, SIG_DFL);
    (void)gspqsignal(SIGTTOU, SIG_DFL);
    (void)gspqsignal(SIGCONT, SIG_DFL);
    (void)gspqsignal(SIGWINCH, SIG_DFL);
}

/* Report shared-memory space needed by WalSndShmemInit */
Size WalSndShmemSize(void)
{
    Size size = 0;

    size = offsetof(WalSndCtlData, walsnds);
    size = add_size(size, mul_size(g_instance.attr.attr_storage.max_wal_senders, sizeof(WalSnd)));

    return size;
}

/* Allocate and initialize walsender-related shared memory */
void WalSndShmemInit(void)
{
    bool found = false;
    errno_t rc = 0;
    int i;

    t_thrd.walsender_cxt.WalSndCtl = (WalSndCtlData*)ShmemInitStruct("Wal Sender Ctl", WalSndShmemSize(), &found);

    if (!found) {
        /* First time through, so initialize */
        rc = memset_s(t_thrd.walsender_cxt.WalSndCtl, WalSndShmemSize(), 0, WalSndShmemSize());
        securec_check(rc, "\0", "\0");

        for (i = 0; i < NUM_SYNC_REP_WAIT_MODE; i++)
            SHMQueueInit(&(t_thrd.walsender_cxt.WalSndCtl->SyncRepQueue[i]));

        for (i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
            WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];

            walsnd->sendKeepalive = true;
            SpinLockInit(&walsnd->mutex);
            InitSharedLatch(&walsnd->latch);
        }
        t_thrd.walsender_cxt.WalSndCtl->most_available_sync = false;
        t_thrd.walsender_cxt.WalSndCtl->sync_master_standalone = false;
        t_thrd.walsender_cxt.WalSndCtl->demotion = NoDemote;
        SpinLockInit(&t_thrd.walsender_cxt.WalSndCtl->mutex);
    }
}

/* Wake up all walsenders */
void WalSndWakeup(void)
{
    int i;

    for (i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++)
        SetLatch(&t_thrd.walsender_cxt.WalSndCtl->walsnds[i].latch);
}

/* return true if any standby(except dummy standby) caught up primary */
static bool WalSndCaughtup(void)
{
    int i;

    for (i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];

        SpinLockAcquire(&walsnd->mutex);

        if (walsnd->pid != 0 && walsnd->sendRole == SNDROLE_PRIMARY_STANDBY && walsnd->state == WALSNDSTATE_STREAMING) {
            SpinLockRelease(&walsnd->mutex);

            return true;
        }

        SpinLockRelease(&walsnd->mutex);
    }

    return false;
}

/* return true if standby has flush more xlog than dummy standby */
static bool WalSndDummyLEStandby(void)
{
    XLogRecPtr flushDummy = InvalidXLogRecPtr;
    XLogRecPtr flushStandby = InvalidXLogRecPtr;
    int i;

    for (i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];

        SpinLockAcquire(&walsnd->mutex);

        if (walsnd->pid != 0 && walsnd->sendRole == SNDROLE_PRIMARY_STANDBY) {
            flushStandby = walsnd->flush;
        } else if (walsnd->pid != 0 && walsnd->sendRole == SNDROLE_PRIMARY_DUMMYSTANDBY) {
            flushDummy = walsnd->flush;
        }

        SpinLockRelease(&walsnd->mutex);
    }

    if (XLByteEQ(flushDummy, InvalidXLogRecPtr) || XLByteEQ(flushStandby, InvalidXLogRecPtr))
        return true;

    return XLByteLE(flushDummy, flushStandby);
}

/* check if there is any wal sender alive. */
bool WalSndInProgress(int type)
{
    int i;

    for (i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];

        SpinLockAcquire(&walsnd->mutex);

        if (walsnd->pid != 0 && walsnd->pid != t_thrd.proc_cxt.MyProcPid &&
            ((walsnd->sendRole & type) == walsnd->sendRole)) {
            SpinLockRelease(&walsnd->mutex);

            return true;
        }

        SpinLockRelease(&walsnd->mutex);
    }

    return false;
}

/* check if there is quorum wal sender in type status. */
bool WalSndQuorumInProgress(int type)
{
    int i;
    int num = 0;
    int num_sync = t_thrd.syncrep_cxt.SyncRepConfig->num_sync;

    for (i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];
        SpinLockAcquire(&walsnd->mutex);
        if (walsnd->pid != 0 && walsnd->pid != t_thrd.proc_cxt.MyProcPid &&
            ((walsnd->sendRole & type) == walsnd->sendRole)) {
            num++;
        }
        SpinLockRelease(&walsnd->mutex);
    }
    if (num_sync <= num) {
        return true;
    } else {
        return false;
    }
}

/* check if there is all wal sender in type status. */
bool WalSndAllInProgress(int type)
{
    int i;
    int num = 0;
    int all_num = t_thrd.syncrep_cxt.SyncRepConfig->nmembers;

    for (i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];
        SpinLockAcquire(&walsnd->mutex);
        if (walsnd->pid != 0 && walsnd->pid != t_thrd.proc_cxt.MyProcPid &&
            ((walsnd->sendRole & type) == walsnd->sendRole)) {
            num++;
        }
        SpinLockRelease(&walsnd->mutex);
    }
    if (num >= all_num) {
        return true;
    } else {
        return false;
    }
}

/*
 * Check if there is any standby or secondeary alive through walsnd.
 */
void StandbyOrSecondaryIsAlive(void)
{
    /*
     * When the standby promote to primary wrongfully, and the old primary is
     * alive, the new primary maybe overwrite the same name file on the dfs;
     * At this moment, only one primary that the secondary is connected can
     * commit transaction, so another primary that no standby or secondary is
     * connected will report an ERROR to rollback transaction.
     */
    if (!(t_thrd.postmaster_cxt.HaShmData->current_mode == NORMAL_MODE ||
            WalSndInProgress(SNDROLE_PRIMARY_STANDBY | SNDROLE_PRIMARY_DUMMYSTANDBY)))
        ereport(LOG,
            (errmsg("No standby or secondary is connected, a new dfs file "
                    "can not be created")));
}

/* Set state for current walsender (only called in walsender) */
void WalSndSetState(WalSndState state)
{
    /* use volatile pointer to prevent code rearrangement */
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;

    Assert(AM_WAL_SENDER);

    if (walsnd->state == state)
        return;

    SpinLockAcquire(&walsnd->mutex);
    walsnd->state = state;
    if (state == WALSNDSTATE_CATCHUP)
        walsnd->catchupTime[0] = GetCurrentTimestamp();
    else if (state == WALSNDSTATE_STREAMING)
        walsnd->catchupTime[1] = GetCurrentTimestamp();
    SpinLockRelease(&walsnd->mutex);
}

/*
 * Return a string constant representing the state. This is used
 * in system views, and should *not* be translated.
 */
static const char* WalSndGetStateString(WalSndState state)
{
    switch (state) {
        case WALSNDSTATE_STARTUP:
            return "Startup";
        case WALSNDSTATE_BACKUP:
            return "Backup";
        case WALSNDSTATE_CATCHUP:
            return "Catchup";
        case WALSNDSTATE_STREAMING:
            return "Streaming";
    }
    return "Unknown";
}

/*
 * Build tuple desc and store for the caller result
 * return the tuple store, the tupdesc would be return by pointer.
 */
Tuplestorestate* BuildTupleResult(FunctionCallInfo fcinfo, TupleDesc* tupdesc)
{
    ReturnSetInfo* rsinfo = (ReturnSetInfo*)fcinfo->resultinfo;
    Tuplestorestate* tupstore = NULL;

    MemoryContext per_query_ctx;
    MemoryContext oldcontext;

    /* check to see if caller supports returning a tuplestore */
    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo)) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("set-valued function called in context that cannot accept a set")));
    }

    if (!(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("materialize mode required, but it is not "
                       "allowed in this context")));

    /* Build a tuple descriptor for our result type */
    if (get_call_result_type(fcinfo, NULL, tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("return type must be a row type")));

    per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    oldcontext = MemoryContextSwitchTo(per_query_ctx);

    tupstore = tuplestore_begin_heap(true, false, u_sess->attr.attr_memory.work_mem);
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult = tupstore;
    rsinfo->setDesc = *tupdesc;

    (void)MemoryContextSwitchTo(oldcontext);

    return tupstore;
}

/*
 * Returns activity of walsenders, including pids and xlog locations sent to
 * standby servers.
 */
Datum pg_stat_get_wal_senders(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_WAL_SENDERS_COLS 21

    TupleDesc tupdesc;
    Tuplestorestate* tupstore = NULL;
    int* sync_priority = NULL;
    int i = 0;
    volatile HaShmemData* hashmdata = t_thrd.postmaster_cxt.HaShmData;
    List* sync_standbys = NIL;

    tupstore = BuildTupleResult(fcinfo, &tupdesc);

    if (IS_DN_DUMMY_STANDYS_MODE()) {
        /*
         * Get the priorities of sync standbys all in one go, to minimise lock
         * acquisitions and to allow us to evaluate who is the current sync
         * standby. This code must match the code in SyncRepReleaseWaiters().
         */
        sync_priority = (int*)palloc(sizeof(int) * g_instance.attr.attr_storage.max_wal_senders);
        for (i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
            /* use volatile pointer to prevent code rearrangement */
            volatile WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];

            if (walsnd->pid != 0) {
                /*
                 * Treat a standby such as a pg_basebackup background process
                 * which always returns an invalid flush location, as an
                 * asynchronous standby.
                 */
                sync_priority[i] = XLogRecPtrIsInvalid(walsnd->flush) ? 0 : walsnd->sync_standby_priority;
            }
        }
    }

    /*
     * Get the currently active synchronous standbys.
     */
    LWLockAcquire(SyncRepLock, LW_SHARED);
    sync_standbys = SyncRepGetSyncStandbys(NULL);
    LWLockRelease(SyncRepLock);

    for (i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];
        char location[MAXFNAMELEN] = {0};
        XLogRecPtr sentRecPtr;
        XLogRecPtr local_write;
        XLogRecPtr flush;
        XLogRecPtr apply;
        WalSndState state;
        XLogRecPtr sndWrite;
        XLogRecPtr sndFlush;
        XLogRecPtr sndReplay;
        XLogRecPtr RcvReceived;
        XLogRecPtr syncStart;

        int sync_percent = 0;
        ServerMode peer_role;
        SndRole snd_role;
        DbState peer_state;
        ServerMode local_role;
        char localip[IP_LEN] = {0};
        char remoteip[IP_LEN] = {0};
        TimestampTz catchup_time[2];
        int localport = 0;
        int remoteport = 0;
        Datum values[PG_STAT_GET_WAL_SENDERS_COLS];
        bool nulls[PG_STAT_GET_WAL_SENDERS_COLS];
        int j = 0;
        errno_t rc = 0;
        int ret = 0;
        int priority = 0;

        SpinLockAcquire(&hashmdata->mutex);
        local_role = hashmdata->current_mode;
        if (walsnd->pid == 0) {
            SpinLockRelease(&hashmdata->mutex);
            continue;
        }
        SpinLockRelease(&hashmdata->mutex);
        SpinLockAcquire(&walsnd->mutex);

        localport = walsnd->wal_sender_channel.localport;
        remoteport = walsnd->wal_sender_channel.remoteport;
        rc = strncpy_s(localip, IP_LEN, (char*)walsnd->wal_sender_channel.localhost, IP_LEN - 1);
        securec_check(rc, "\0", "\0");
        rc = strncpy_s(remoteip, IP_LEN, (char*)walsnd->wal_sender_channel.remotehost, IP_LEN - 1);
        securec_check(rc, "\0", "\0");
        localip[IP_LEN - 1] = '\0';
        remoteip[IP_LEN - 1] = '\0';
        peer_role = walsnd->peer_role;
        snd_role = walsnd->sendRole;
        peer_state = walsnd->peer_state;
        state = walsnd->state;

        sentRecPtr = walsnd->sentPtr;
        local_write = walsnd->write;
        flush = walsnd->flush;
        apply = walsnd->apply;
        RcvReceived = walsnd->receive;
        syncStart = walsnd->syncPercentCountStart;
        catchup_time[0] = walsnd->catchupTime[0];
        catchup_time[1] = walsnd->catchupTime[1];
        if (IS_DN_MULTI_STANDYS_MODE())
            priority = walsnd->sync_standby_priority;
        SpinLockRelease(&walsnd->mutex);

        if (local_role == PRIMARY_MODE) {
            sndWrite = GetXLogWriteRecPtr();
            sndFlush = GetFlushRecPtr();
            sndReplay = sndFlush;
        } else {
            sndWrite = GetWalRcvWriteRecPtr(NULL);
            sndFlush = GetStandbyFlushRecPtr(NULL);
            sndReplay = GetXLogReplayRecPtr(NULL);
        }

        rc = memset_s(nulls, sizeof(nulls), 0, sizeof(nulls));
        securec_check(rc, "\0", "\0");

        values[j++] = Int64GetDatum(walsnd->pid);
        values[j++] = Int32GetDatum(walsnd->lwpId);

        if (!superuser()) {
            /*
             * Only superusers can see details. Other users only get the pid
             * value to know it's a walsender, but no details.
             */
            rc = memset_s(&nulls[j], PG_STAT_GET_WAL_SENDERS_COLS - j, true, PG_STAT_GET_WAL_SENDERS_COLS - j);
            securec_check(rc, "\0", "\0");
        } else {
            /* local_role */
            values[j++] = CStringGetTextDatum(wal_get_role_string(local_role));

            /* peer_role */
            if (snd_role == SNDROLE_PRIMARY_DUMMYSTANDBY)
                values[j++] = CStringGetTextDatum("Secondary");
            else
                values[j++] = CStringGetTextDatum(wal_get_role_string(peer_role));

            /* peer_state */
            values[j++] = CStringGetTextDatum(wal_get_db_state_string(peer_state));

            /* state */
            values[j++] = CStringGetTextDatum(WalSndGetStateString(state));

            /* catchup time */
            if (catchup_time[0] != 0)
                values[j++] = TimestampTzGetDatum(catchup_time[0]);
            else
                nulls[j++] = true;
            if (catchup_time[1] != 0 && (state != WALSNDSTATE_CATCHUP))
                values[j++] = TimestampTzGetDatum(catchup_time[1]);
            else
                nulls[j++] = true;

            /* sender_sent_location */
            ret = snprintf_s(location,
                sizeof(location),
                sizeof(location) - 1,
                "%X/%X",
                (uint32)(sentRecPtr >> 32),
                (uint32)sentRecPtr);
            securec_check_ss(ret, "\0", "\0");
            values[j++] = CStringGetTextDatum(location);

            /* sender_write_location */
            if (sndWrite == 0)
                SETXLOGLOCATION(sndWrite, sentRecPtr)
            ret = snprintf_s(
                location, sizeof(location), sizeof(location) - 1, "%X/%X", (uint32)(sndWrite >> 32), (uint32)sndWrite);
            securec_check_ss(ret, "\0", "\0");
            values[j++] = CStringGetTextDatum(location);

            /* sender_flush_location */
            if (sndFlush == 0)
                SETXLOGLOCATION(sndFlush, sentRecPtr)
            ret = snprintf_s(
                location, sizeof(location), sizeof(location) - 1, "%X/%X", (uint32)(sndFlush >> 32), (uint32)sndFlush);
            securec_check_ss(ret, "\0", "\0");
            values[j++] = CStringGetTextDatum(location);

            /* sender_replay_location */
            if (sndReplay == 0)
                SETXLOGLOCATION(sndReplay, sentRecPtr)
            ret = snprintf_s(location,
                sizeof(location),
                sizeof(location) - 1,
                "%X/%X",
                (uint32)(sndReplay >> 32),
                (uint32)sndReplay);
            securec_check_ss(ret, "\0", "\0");
            values[j++] = CStringGetTextDatum(location);

            /* receiver_received_location */
            if (RcvReceived == 0)
                SETXLOGLOCATION(RcvReceived, sentRecPtr)
            ret = snprintf_s(location,
                sizeof(location),
                sizeof(location) - 1,
                "%X/%X",
                (uint32)(RcvReceived >> 32),
                (uint32)RcvReceived);
            securec_check_ss(ret, "\0", "\0");
            values[j++] = CStringGetTextDatum(location);

            /* receiver_write_location */
            if (local_write == 0)
                SETXLOGLOCATION(local_write, sentRecPtr)
            ret = snprintf_s(location,
                sizeof(location),
                sizeof(location) - 1,
                "%X/%X",
                (uint32)(local_write >> 32),
                (uint32)local_write);
            securec_check_ss(ret, "\0", "\0");
            values[j++] = CStringGetTextDatum(location);

            /* receiver_flush_location */
            if (flush == 0)
                SETXLOGLOCATION(flush, sentRecPtr)
            ret = snprintf_s(
                location, sizeof(location), sizeof(location) - 1, "%X/%X", (uint32)(flush >> 32), (uint32)flush);
            securec_check_ss(ret, "\0", "\0");
            values[j++] = CStringGetTextDatum(location);

            /* receiver_replay_location */
            if (apply == 0)
                SETXLOGLOCATION(apply, sentRecPtr)
            ret = snprintf_s(
                location, sizeof(location), sizeof(location) - 1, "%X/%X", (uint32)(apply >> 32), (uint32)apply);
            securec_check_ss(ret, "\0", "\0");
            values[j++] = CStringGetTextDatum(location);

            /* sync_percent */
            sync_percent = GetSyncPercent(syncStart, sndFlush, flush);
            ret = snprintf_s(location, sizeof(location), sizeof(location) - 1, "%d%%", sync_percent);
            securec_check_ss(ret, "\0", "\0");
            values[j++] = CStringGetTextDatum(location);

            if (IS_DN_DUMMY_STANDYS_MODE()) {
                /* sync_state and sync_prority */
                if (!SyncRepRequested()) {
                    values[j++] = CStringGetTextDatum("Async");
                    values[j++] = Int32GetDatum(0);
                } else {
                    values[j++] = CStringGetTextDatum("Sync");
                    values[j++] = Int32GetDatum(sync_priority[i]);
                }
            } else {
                /*
                 * Treat a standby such as a pg_basebackup background process
                 * which always returns an invalid flush location, as an
                 * asynchronous standby.
                 */
                priority = XLogRecPtrIsInvalid(walsnd->flush) ? 0 : priority;
                /*
                 * More easily understood version of standby state. This is purely
                 * informational.
                 * In quorum-based sync replication, the role of each standby
                 * listed in synchronous_standby_names can be changing very
                 * frequently. Any standbys considered as "sync" at one moment can
                 * be switched to "potential" ones at the next moment. So, it's
                 * basically useless to report "sync" or "potential" as their sync
                 * states. We report just "quorum" for them.
                 */
                if (priority == 0)
                    values[j++] = CStringGetTextDatum("Async");
                else if (list_member_int(sync_standbys, i)) {
                    values[j++] = t_thrd.syncrep_cxt.SyncRepConfig->syncrep_method == SYNC_REP_PRIORITY ?
                        CStringGetTextDatum("Sync") :
                        CStringGetTextDatum("Quorum");
                } else
                    values[j++] = CStringGetTextDatum("Potential");
                values[j++] = Int32GetDatum(priority);
            }

            if (most_available_sync)
                values[j++] = CStringGetTextDatum("On");
            else
                values[j++] = CStringGetTextDatum("Off");

            /* channel */
            ret = snprintf_s(location,
                sizeof(location),
                sizeof(location) - 1,
                "%s:%d-->%s:%d",
                localip,
                localport,
                remoteip,
                remoteport);
            securec_check_ss(ret, "\0", "\0");
            values[j++] = CStringGetTextDatum(location);
        }

        tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }
    list_free(sync_standbys);
    if (sync_priority != NULL) {
        pfree(sync_priority);
        sync_priority = NULL;
    }

    /* clean up and return the tuplestore */
    tuplestore_donestoring(tupstore);

    return (Datum)0;
}

/*
 * This function is used to send keepalive message to standby.
 * If requestReply is set, sets a flag in the message requesting the standby
 * to send a message back to us, for heartbeat purposes.
 */
static void WalSndKeepalive(bool requestReply)
{
    PrimaryKeepaliveMessage keepalive_message;
    volatile HaShmemData* hashmdata = t_thrd.postmaster_cxt.HaShmData;
    errno_t errorno = EOK;

    /* Construct a new message */
    SpinLockAcquire(&hashmdata->mutex);
    keepalive_message.peer_role = hashmdata->current_mode;
    SpinLockRelease(&hashmdata->mutex);
    keepalive_message.peer_state = get_local_dbstate();
    keepalive_message.walEnd = t_thrd.walsender_cxt.sentPtr;
    keepalive_message.sendTime = GetCurrentTimestamp();
    keepalive_message.replyRequested = requestReply;
    keepalive_message.catchup = (t_thrd.walsender_cxt.MyWalSnd->state == WALSNDSTATE_CATCHUP);

    ereport(DEBUG2, (errmsg("sending wal replication keepalive")));

    /* Prepend with the message type and send it. */
    t_thrd.walsender_cxt.output_xlog_message[0] = 'k';
    errorno = memcpy_s(t_thrd.walsender_cxt.output_xlog_message + 1,
        sizeof(WalDataMessageHeader) + g_instance.attr.attr_storage.MaxSendSize * 1024,
        &keepalive_message,
        sizeof(PrimaryKeepaliveMessage));
    securec_check(errorno, "\0", "\0");
    (void)pq_putmessage_noblock('d', t_thrd.walsender_cxt.output_xlog_message, sizeof(PrimaryKeepaliveMessage) + 1);

    /* Flush the keepalive message to standby immediately. */
    if (pq_flush_if_writable() != 0)
        WalSndShutdown();
}

/*
 * This function is used to send rm_xlog message to  xlogreceiver.
 * If requestReply is set, sets a flag in the message requesting the standby
 * to send a message back to us, for heartbeat purposes.
 */
static void WalSndRmXLog(bool requestReply)
{
    RmXLogMessage rmXLogMessage;
    volatile HaShmemData* hashmdata = t_thrd.postmaster_cxt.HaShmData;
    errno_t errorno = EOK;

    /* Construct a new message */
    SpinLockAcquire(&hashmdata->mutex);
    rmXLogMessage.peer_role = hashmdata->current_mode;
    SpinLockRelease(&hashmdata->mutex);
    rmXLogMessage.peer_state = get_local_dbstate();
    rmXLogMessage.sendTime = GetCurrentTimestamp();
    rmXLogMessage.replyRequested = requestReply;

    ereport(DEBUG2, (errmsg("sending dummystandby rm xlog message")));

    /* Prepend with the message type and send it. */
    t_thrd.walsender_cxt.output_xlog_message[0] = 'x';
    errorno = memcpy_s(t_thrd.walsender_cxt.output_xlog_message + 1,
        sizeof(WalDataMessageHeader) + WS_MAX_SEND_SIZE,
        &rmXLogMessage,
        sizeof(RmXLogMessage));
    securec_check(errorno, "\0", "\0");
    (void)pq_putmessage_noblock('d', t_thrd.walsender_cxt.output_xlog_message, sizeof(RmXLogMessage) + 1);
}

/*
 * This function is used to send rm_xlog message to  xlogreceiver.
 * If requestReply is set, sets a flag in the message requesting the standby
 * to send a message back to us, for heartbeat purposes.
 */
static void WalSndSyncDummyStandbyDone(bool requestReply)
{
    EndXLogMessage endXLogMessage;
    volatile HaShmemData* hashmdata = t_thrd.postmaster_cxt.HaShmData;
    errno_t errorno = EOK;

    /* Construct a new message */
    SpinLockAcquire(&hashmdata->mutex);
    endXLogMessage.peer_role = hashmdata->current_mode;
    SpinLockRelease(&hashmdata->mutex);
    endXLogMessage.peer_state = get_local_dbstate();
    endXLogMessage.sendTime = GetCurrentTimestamp();
    endXLogMessage.percent = SYNC_DUMMY_STANDBY_END;

    ereport(dummyStandbyMode ? LOG : DEBUG2, (errmsg("send Secondary Standby xlog done")));

    /* Prepend with the message type and send it. */
    t_thrd.walsender_cxt.output_xlog_message[0] = 'e';
    errorno = memcpy_s(t_thrd.walsender_cxt.output_xlog_message + 1,
        sizeof(WalDataMessageHeader) + g_instance.attr.attr_storage.MaxSendSize * 1024,
        &endXLogMessage,
        sizeof(EndXLogMessage));
    securec_check(errorno, "\0", "\0");
    (void)pq_putmessage_noblock('d', t_thrd.walsender_cxt.output_xlog_message, sizeof(EndXLogMessage) + 1);
}

static void WalSndKeepaliveIfNecessary(TimestampTz now)
{
    TimestampTz ping_time;

    /*
     * Don't send keepalive messages if timeouts are globally disabled or
     * we're doing something not partaking in timeouts.
     */
    if (u_sess->attr.attr_storage.wal_sender_timeout <= 0 || t_thrd.walsender_cxt.last_reply_timestamp <= 0)
        return;

    if (t_thrd.walsender_cxt.waiting_for_ping_response)
        return;

    /*
     * If half of wal_sender_timeout has lapsed without receiving any reply
     * from the standby, send a keep-alive message to the standby requesting
     * an immediate reply.
     */
    ping_time = TimestampTzPlusMilliseconds(
        t_thrd.walsender_cxt.last_reply_timestamp, u_sess->attr.attr_storage.wal_sender_timeout / 2);
    if (now >= ping_time) {
        WalSndKeepalive(true);
        t_thrd.walsender_cxt.waiting_for_ping_response = true;
    }
}

/*
 * send switchover response message
 */
static void WalSndResponseSwitchover(char* msgbuf)
{
    PrimarySwitchResponseMessage response_message;
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;
    errno_t errorno = EOK;

    if (walsnd == NULL)
        return;

    switch (walsnd->node_state) {
        case NODESTATE_PROMOTE_APPROVE:
            response_message.switchResponse = SWITCHOVER_PROMOTE_REQUEST;
            /* clean view data. */
            int rc;
            rc = memset_s(&(g_instance.rto_cxt), sizeof(knl_g_rto_context), 0, sizeof(knl_g_rto_context));
            securec_check(rc, "\0", "\0");
            break;
        case NODESTATE_PRIMARY_DEMOTING_WAIT_CATCHUP:
            response_message.switchResponse = SWITCHOVER_DEMOTE_CATCHUP_EXIST;
            break;
        case NODESTATE_STANDBY_REDIRECT:
        case NODESTATE_DEMOTE_FAILED:
        default:
            return;
    }

    response_message.walEnd = t_thrd.walsender_cxt.sentPtr;
    response_message.sendTime = GetCurrentTimestamp();

    ereport(LOG,
        (errmsg("sending switchover response message%s",
            walsnd->node_state == NODESTATE_PRIMARY_DEMOTING_WAIT_CATCHUP ? ", meeting alive catchup" : ".")));

    /* Prepend with the message type and send it. */
    msgbuf[0] = 'p';
    errorno = memcpy_s(msgbuf + 1,
        sizeof(WalDataMessageHeader) + g_instance.attr.attr_storage.MaxSendSize * 1024,
        &response_message,
        sizeof(PrimarySwitchResponseMessage));
    securec_check(errorno, "\0", "\0");
    (void)pq_putmessage_noblock('d', msgbuf, sizeof(PrimarySwitchResponseMessage) + 1);
}

/*
 * This isn't currently used for anything. Monitoring tools might be
 * interested in the future, and we'll need something like this in the
 * future for synchronous replication.
 */
#ifdef NOT_USED
/*
 * Returns the oldest Send position among walsenders. Or InvalidXLogRecPtr
 * if none.
 */
XLogRecPtr GetOldestWALSendPointer(void)
{
    XLogRecPtr oldest = 0;
    int i;
    bool found = false;

    for (i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];
        XLogRecPtr recptr;

        if (walsnd->pid == 0)
            continue;

        SpinLockAcquire(&walsnd->mutex);
        recptr = walsnd->sentPtr;
        SpinLockRelease(&walsnd->mutex);

        if (recptr == 0)
            continue;

        if (!found || XLByteLT(recptr, oldest))
            oldest = recptr;
        found = true;
    }
    return oldest;
}

#endif

/*
 * Save the current connect channel of the walsender in walsnd.
 */
static void SetHaWalSenderChannel()
{
    struct sockaddr* laddr = (struct sockaddr*)&(u_sess->proc_cxt.MyProcPort->laddr.addr);
    struct sockaddr* raddr = (struct sockaddr*)&(u_sess->proc_cxt.MyProcPort->raddr.addr);
    char local_ip[IP_LEN] = {0};
    char remote_ip[IP_LEN] = {0};
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;
    char* result = NULL;
    errno_t rc = 0;

    if (laddr->sa_family == AF_INET6) {
        result = inet_net_ntop(AF_INET6, &((struct sockaddr_in*)laddr)->sin_addr, 128, local_ip, IP_LEN);
        if (result == NULL) {
            ereport(WARNING, (errmsg("inet_net_ntop failed")));
        }
    } else if (laddr->sa_family == AF_INET) {
        result = inet_net_ntop(AF_INET, &((struct sockaddr_in*)laddr)->sin_addr, 32, local_ip, IP_LEN);
        if (result == NULL) {
            ereport(WARNING, (errmsg("inet_net_ntop failed")));
        }
    }

    if (raddr->sa_family == AF_INET6) {
        result = inet_net_ntop(AF_INET6, &((struct sockaddr_in*)raddr)->sin_addr, 128, remote_ip, IP_LEN);
        if (result == NULL) {
            ereport(WARNING, (errmsg("inet_net_ntop failed")));
        }
    } else if (raddr->sa_family == AF_INET) {
        result = inet_net_ntop(AF_INET, &((struct sockaddr_in*)raddr)->sin_addr, 32, remote_ip, IP_LEN);
        if (result == NULL) {
            ereport(WARNING, (errmsg("inet_net_ntop failed")));
        }
    }

    SpinLockAcquire(&walsnd->mutex);
    rc = strncpy_s((char*)walsnd->wal_sender_channel.localhost, IP_LEN, local_ip, IP_LEN - 1);
    securec_check(rc, "\0", "\0");
    walsnd->wal_sender_channel.localhost[IP_LEN - 1] = '\0';
    walsnd->wal_sender_channel.localport = ntohs(((struct sockaddr_in*)laddr)->sin_port);
    rc = strncpy_s((char*)walsnd->wal_sender_channel.remotehost, IP_LEN, remote_ip, IP_LEN - 1);
    securec_check(rc, "\0", "\0");
    walsnd->wal_sender_channel.remotehost[IP_LEN - 1] = '\0';
    walsnd->wal_sender_channel.remoteport = ntohs(((struct sockaddr_in*)raddr)->sin_port);
    SpinLockRelease(&walsnd->mutex);

    /* for log control view. */
    if (IS_PGXC_DATANODE) {
        g_instance.rto_cxt.rto_standby_data[walsnd->index].source_port = walsnd->wal_sender_channel.localport;
        g_instance.rto_cxt.rto_standby_data[walsnd->index].dest_port = walsnd->wal_sender_channel.remoteport;

        char* localip = (char*)(g_instance.rto_cxt.rto_standby_data[walsnd->index].source_ip);
        rc = strncpy_s(localip, IP_LEN, local_ip, IP_LEN - 1);
        securec_check(rc, "\0", "\0");
        g_instance.rto_cxt.rto_standby_data[walsnd->index].source_ip[IP_LEN - 1] = '\0';

        char* remoteip = (char*)(g_instance.rto_cxt.rto_standby_data[walsnd->index].dest_ip);
        rc = strncpy_s(remoteip, IP_LEN, remote_ip, IP_LEN - 1);
        securec_check(rc, "\0", "\0");
        g_instance.rto_cxt.rto_standby_data[walsnd->index].dest_ip[IP_LEN - 1] = '\0';
    }
}

static bool UpdateHaWalSenderChannel(int ha_remote_listen_port)
{
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;
    bool is_found = false;
    int i = 0;

    for (i = 1; i < MAX_REPLNODE_NUM; i++) {
        ReplConnInfo* replconninfo = t_thrd.postmaster_cxt.ReplConnArray[i];
        if (replconninfo == NULL)
            continue;

        if (strncmp((char*)replconninfo->remotehost, (char*)walsnd->wal_sender_channel.remotehost, IP_LEN) == 0 &&
            replconninfo->remoteport == ha_remote_listen_port) {
            SpinLockAcquire(&walsnd->mutex);
            walsnd->channel_get_replc = i;
            walsnd->wal_sender_channel.localservice = replconninfo->localservice;
            walsnd->wal_sender_channel.remoteservice = replconninfo->remoteservice;
            SpinLockRelease(&walsnd->mutex);
            is_found = true;
            break;
        }
    }

    if (is_found)
        ereport(LOG, (errmsg("current repl of walsender is %d.", i)));

    return is_found;
}

static void SetReplWalSender(void)
{
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;

    SpinLockAcquire(&walsnd->mutex);
    walsnd->replSender = true;
    SpinLockRelease(&walsnd->mutex);
}

/* Set walsnd peer_mode */
static void SetWalSndPeerMode(ServerMode mode)
{
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;

    SpinLockAcquire(&walsnd->mutex);
    walsnd->peer_role = mode;
    SpinLockRelease(&walsnd->mutex);
}

/* Set walsnd peer_state */
static void SetWalSndPeerDbstate(DbState state)
{
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;

    SpinLockAcquire(&walsnd->mutex);
    walsnd->peer_state = state;
    SpinLockRelease(&walsnd->mutex);
}

/* send config file to standby */
static bool SendConfigFile(char* path)
{
    FILE* fp = NULL;
    char* buf = NULL;
    int nread = 0;
    int len = 0;
    struct stat statbuf;
    ConfFileLock filelock = {NULL, 0};
    ConfigModifyTimeMessage msgConfigTime;
    errno_t errorno = EOK;

    if (AmWalSenderToDummyStandby() || AmWalSenderOnDummyStandby())
        return true;

    if (lstat(path, &statbuf) < 0 || statbuf.st_size == 0) {
        ereport(LOG, (errcode_for_file_access(), errmsg("could not stat file or directory \"%s\": %m", path)));
        return false;
    }
    if (get_file_lock(t_thrd.walsender_cxt.gucconf_lock_file, &filelock) != CODE_OK) {
        ereport(LOG, (errmsg("get lock failed when send gaussdb config file to the peer.")));
        return false;
    }
    if ((fp = fopen(path, "rb")) == NULL) {
        release_file_lock(&filelock);
        ereport(LOG, (errmsg("could not open file or directory \"%s\": %m", path)));
        return false;
    }
    len = 1 + sizeof(ConfigModifyTimeMessage) + statbuf.st_size;
    buf = (char*)palloc0(len);
    buf[0] = 'm';
    msgConfigTime.config_modify_time = statbuf.st_mtime;
    errorno = memcpy_s(
        buf + 1, sizeof(ConfigModifyTimeMessage) + statbuf.st_size, &msgConfigTime, sizeof(ConfigModifyTimeMessage));
    securec_check(errorno, "\0", "\0");
    nread = (int)fread(buf + 1 + sizeof(ConfigModifyTimeMessage), 1, statbuf.st_size, fp);
    if (nread != statbuf.st_size) {
        release_file_lock(&filelock);
        (void)fclose(fp);
        pfree(buf);
        buf = NULL;
        ereport(LOG, (errcode_for_file_access(), errmsg("could not read gaussdb config file: %m")));
        return false;
    }
    if (fclose(fp) != 0)
        ereport(LOG, (errmsg("walsender close the file or directory error\"%s\": %m", path)));
    release_file_lock(&filelock);
    /* Send the chunk as a CopyData message */
    (void)pq_putmessage_noblock('d', buf, 1 + sizeof(ConfigModifyTimeMessage) + nread);
    pfree(buf);
    buf = NULL;
    ereport(LOG, (errmsg("walsender send config file size :%d", (int)(1 + sizeof(ConfigModifyTimeMessage) + nread))));
    return true;
}

AlarmCheckResult WalSegmentsRemovedChecker(Alarm* alarm, AlarmAdditionalParam* additionalParam)
{
    if (WalSegmemtRemovedhappened == true) {
        // fill the alarm message
        WriteAlarmAdditionalInfo(additionalParam,
            g_instance.attr.attr_common.PGXCNodeName,
            "",
            "",
            alarm,
            ALM_AT_Fault,
            g_instance.attr.attr_common.PGXCNodeName);
        return ALM_ACR_Abnormal;
    } else {
        // fill the alarm message
        WriteAlarmAdditionalInfo(
            additionalParam, g_instance.attr.attr_common.PGXCNodeName, "", "", alarm, ALM_AT_Resume);
        return ALM_ACR_Normal;
    }
}

static void StopAliveBuildSender(void)
{
    for (int i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];
        SndRole sndrole;

        if (walsnd->pid == 0 || t_thrd.walsender_cxt.MyWalSnd == walsnd)
            continue;

        SpinLockAcquire(&walsnd->mutex);
        sndrole = walsnd->sendRole;
        SpinLockRelease(&walsnd->mutex);

        /* skip the last cycle using SIGTERM */
        if (sndrole == SNDROLE_PRIMARY_BUILDSTANDBY)
            (void)gs_signal_send(walsnd->pid, SIGTERM);
    }
}

void GetFastestReplayStandByServiceAddress(
    char* fastest_remote_address, char* second_fastest_remote_address, size_t address_len)
{
    if (fastest_remote_address == NULL || second_fastest_remote_address == NULL || address_len == 0 ||
        t_thrd.walsender_cxt.WalSndCtl == NULL)
        return;

    int fastest = 0;
    int second_fastest = 0;
    XLogRecPtr fastest_replay = InvalidXLogRecPtr;
    XLogRecPtr second_fastest_replay = InvalidXLogRecPtr;

    errno_t rc = EOK;

    for (int i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];

        if (walsnd->pid == 0 || !walsnd->replSender)
            continue;

        SpinLockAcquire(&walsnd->mutex);
        XLogRecPtr walsnd_replay = walsnd->apply;
        DbState peer_state = walsnd->peer_state;
        SpinLockRelease(&walsnd->mutex);

        /* remote can accept connections */
        if (peer_state != NORMAL_STATE && peer_state != CATCHUP_STATE)
            continue;

        if (XLByteLT(second_fastest_replay, walsnd_replay)) {
            if (XLByteLT(fastest_replay, walsnd_replay)) {
                /* walsnd_replay is larger than fastest_replay */
                second_fastest = fastest;
                second_fastest_replay = fastest_replay;

                fastest = i;
                fastest_replay = walsnd_replay;
            } else {
                /* walsnd_replay is in the range (second_fastest_replay, fastest_replay] */
                second_fastest = i;
                second_fastest_replay = walsnd_replay;
            }
        }
    }

    /* find fastest replay standby */
    if (!XLogRecPtrIsInvalid(fastest_replay)) {
        volatile WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[fastest];
        SpinLockAcquire(&walsnd->mutex);
        rc = snprintf_s(fastest_remote_address,
            address_len,
            (address_len - 1),
            "%s:%d",
            walsnd->wal_sender_channel.remotehost,
            walsnd->wal_sender_channel.remoteservice);
        SpinLockRelease(&walsnd->mutex);

        securec_check_ss(rc, "", "");
    }

    /* find second fastest replay standby */
    if (!XLogRecPtrIsInvalid(second_fastest_replay)) {
        volatile WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[second_fastest];
        SpinLockAcquire(&walsnd->mutex);
        rc = snprintf_s(second_fastest_remote_address,
            address_len,
            (address_len - 1),
            "%s:%d",
            walsnd->wal_sender_channel.remotehost,
            walsnd->wal_sender_channel.remoteservice);
        SpinLockRelease(&walsnd->mutex);

        securec_check_ss(rc, "\0", "\0");
    }
}

bool IsPrimaryStandByReadyToRemoteRead(void)
{
    /* only using  IS_DN_DUMMY_STANDYS_MODE && PRIMARY_MODE */
    bool can_remte_read = false;

    for (int i = 0; i < g_instance.attr.attr_storage.max_wal_senders; i++) {
        /* use volatile pointer to prevent code rearrangement */
        volatile WalSnd* walsnd = &t_thrd.walsender_cxt.WalSndCtl->walsnds[i];

        if (walsnd->pid == 0 || t_thrd.walsender_cxt.MyWalSnd == walsnd)
            continue;

        SpinLockAcquire(&walsnd->mutex);
        SndRole sndrole = walsnd->sendRole;
        DbState peer_state = walsnd->peer_state;
        SpinLockRelease(&walsnd->mutex);

        if (sndrole == SNDROLE_PRIMARY_STANDBY) {
            if (peer_state == NORMAL_STATE || peer_state == CATCHUP_STATE) {
                can_remte_read = true;
            }

            break;
        }
    }

    return can_remte_read;
}

static bool IsWalSenderToBuild(void)
{
    WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;
    bool isWalToBuild = false;
    if (walsnd == NULL) {
        return false;
    }
    SpinLockAcquire(&walsnd->mutex);
    SndRole sndrole = walsnd->sendRole;
    ThreadId pid = walsnd->pid;
    if (sndrole == SNDROLE_PRIMARY_BUILDSTANDBY && pid != 0) {
        isWalToBuild = true;
    }
    SpinLockRelease(&walsnd->mutex);

    return isWalToBuild;
}

/* Set start send lsn for current walsender (only called in walsender) */
static void WalSndSetPercentCountStartLsn(XLogRecPtr startLsn)
{
    /* use volatile pointer to prevent code rearrangement */
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;

    SpinLockAcquire(&walsnd->mutex);
    walsnd->syncPercentCountStart = startLsn;
    SpinLockRelease(&walsnd->mutex);
}

/* Set start send lsn for current walsender (only called in walsender) */
static void WalSndRefreshPercentCountStartLsn(XLogRecPtr currentMaxLsn, XLogRecPtr currentDoneLsn)
{
    uint64 coundWindow = ((uint64)WalGetSyncCountWindow() * XLOG_SEG_SIZE);
    volatile WalSnd* walsnd = t_thrd.walsender_cxt.MyWalSnd;
    XLogRecPtr baseStartLsn = InvalidXLogRecPtr;

    if (!walsnd) {
        return;
    }

    /* don't refresh during catching up. */
    if (walsnd->state == WALSNDSTATE_CATCHUP) {
        return;
    }

    if (XLogDiff(currentMaxLsn, currentDoneLsn) < coundWindow) {
        WalSndSetPercentCountStartLsn(InvalidXLogRecPtr);
    } else {
        SpinLockAcquire(&walsnd->mutex);
        baseStartLsn = walsnd->syncPercentCountStart;
        SpinLockRelease(&walsnd->mutex);
        if (!XLByteEQ(baseStartLsn, InvalidXLogRecPtr)) {
            return;
        }
        WalSndSetPercentCountStartLsn(currentDoneLsn);
    }
}

XLogSegNo WalGetSyncCountWindow(void)
{
    return (XLogSegNo)(uint32)u_sess->attr.attr_storage.wal_keep_segments;
}
