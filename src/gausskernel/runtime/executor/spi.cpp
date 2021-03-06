/* -------------------------------------------------------------------------
 *
 * spi.cpp
 * 				Server Programming Interface
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * 	  src/gausskernel/runtime/executor/spi.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/printtup.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/heap.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/spi_priv.h"
#include "miscadmin.h"
#include "parser/parser.h"
#include "pgxc/pgxc.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/rel_gs.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "utils/elog.h"

THR_LOCAL uint32 SPI_processed = 0;
THR_LOCAL SPITupleTable *SPI_tuptable = NULL;
THR_LOCAL int SPI_result;

static Portal SPI_cursor_open_internal(const char *name, SPIPlanPtr plan, ParamListInfo paramLI, bool read_only);

static void _SPI_prepare_plan(const char *src, SPIPlanPtr plan);
#ifdef PGXC
static void _SPI_pgxc_prepare_plan(const char *src, List *src_parsetree, SPIPlanPtr plan);
#endif

static void _SPI_prepare_oneshot_plan(const char *src, SPIPlanPtr plan);

static int _SPI_execute_plan(SPIPlanPtr plan, ParamListInfo paramLI, Snapshot snapshot, Snapshot crosscheck_snapshot,
    bool read_only, bool fire_triggers, long tcount, bool from_lock = false);

static ParamListInfo _SPI_convert_params(int nargs, Oid *argtypes, Datum *Values, const char *Nulls,
    Cursor_Data *cursor_data = NULL);

static int _SPI_pquery(QueryDesc *queryDesc, bool fire_triggers, long tcount, bool from_lock = false);

static void _SPI_error_callback(void *arg);

static void _SPI_cursor_operation(Portal portal, FetchDirection direction, long count, DestReceiver *dest);

static SPIPlanPtr _SPI_make_plan_non_temp(SPIPlanPtr plan);
static SPIPlanPtr _SPI_save_plan(SPIPlanPtr plan);

static int _SPI_begin_call(bool execmem);
static MemoryContext _SPI_execmem(void);
static MemoryContext _SPI_procmem(void);
static bool _SPI_checktuples(void);
extern void ClearVacuumStmt(VacuumStmt *stmt);
static void CopySPI_Plan(SPIPlanPtr newplan, SPIPlanPtr plan, MemoryContext plancxt);

/* =================== interface functions =================== */
int SPI_connect(CommandDest dest, void (*spiCallbackfn)(void *), void *clientData)
{
    int new_depth;
    /*
     * When procedure called by Executor u_sess->SPI_cxt._curid expected to be equal to
     * u_sess->SPI_cxt._connected
     */
    if (u_sess->SPI_cxt._curid != u_sess->SPI_cxt._connected) {
        return SPI_ERROR_CONNECT;
    }

    if (u_sess->SPI_cxt._stack == NULL) {
        if (u_sess->SPI_cxt._connected != -1 || u_sess->SPI_cxt._stack_depth != 0) {
            ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("SPI stack corrupted when connect SPI, %s",
                u_sess->SPI_cxt._connected != -1 ? "init level is not -1." : "stack depth is not zero.")));
        }
        new_depth = 16;
        u_sess->SPI_cxt._stack =
            (_SPI_connection *)MemoryContextAlloc(u_sess->top_transaction_mem_cxt, new_depth * sizeof(_SPI_connection));
        u_sess->SPI_cxt._stack_depth = new_depth;
    } else {
        if (u_sess->SPI_cxt._stack_depth <= 0 || u_sess->SPI_cxt._stack_depth <= u_sess->SPI_cxt._connected) {
            ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                errmsg("SPI stack corrupted when connect SPI, stack depth %d", u_sess->SPI_cxt._stack_depth)));
        }
        if (u_sess->SPI_cxt._stack_depth == u_sess->SPI_cxt._connected + 1) {
            new_depth = u_sess->SPI_cxt._stack_depth * 2;
            u_sess->SPI_cxt._stack =
                (_SPI_connection *)repalloc(u_sess->SPI_cxt._stack, new_depth * sizeof(_SPI_connection));
            u_sess->SPI_cxt._stack_depth = new_depth;
        }
    }

    /*
     * We're entering procedure where u_sess->SPI_cxt._curid == u_sess->SPI_cxt._connected - 1
     */
    u_sess->SPI_cxt._connected++;
    Assert(u_sess->SPI_cxt._connected >= 0 && u_sess->SPI_cxt._connected < u_sess->SPI_cxt._stack_depth);

    u_sess->SPI_cxt._current = &(u_sess->SPI_cxt._stack[u_sess->SPI_cxt._connected]);
    u_sess->SPI_cxt._current->processed = 0;
    u_sess->SPI_cxt._current->lastoid = InvalidOid;
    u_sess->SPI_cxt._current->tuptable = NULL;
    u_sess->SPI_cxt._current->procCxt = NULL; /* in case we fail to create 'em */
    u_sess->SPI_cxt._current->execCxt = NULL;
    u_sess->SPI_cxt._current->connectSubid = GetCurrentSubTransactionId();
    u_sess->SPI_cxt._current->dest = dest;
    u_sess->SPI_cxt._current->spiCallback = (void (*)(void *))spiCallbackfn;
    u_sess->SPI_cxt._current->clientData = clientData;

    /*
     * Create memory contexts for this procedure
     *
     * XXX it would be better to use t_thrd.mem_cxt.portal_mem_cxt as the parent context, but
     * we may not be inside a portal (consider deferred-trigger execution).
     * Perhaps t_thrd.mem_cxt.cur_transaction_mem_cxt would do?	For now it doesn't matter
     * because we clean up explicitly in AtEOSubXact_SPI().
     */
    u_sess->SPI_cxt._current->procCxt = AllocSetContextCreate(u_sess->top_transaction_mem_cxt, "SPI Proc",
        ALLOCSET_DEFAULT_MINSIZE, ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);
    u_sess->SPI_cxt._current->execCxt = AllocSetContextCreate(u_sess->top_transaction_mem_cxt, "SPI Exec",
        ALLOCSET_DEFAULT_MINSIZE, ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);
    /* ... and switch to procedure's context */
    u_sess->SPI_cxt._current->savedcxt = MemoryContextSwitchTo(u_sess->SPI_cxt._current->procCxt);

    return SPI_OK_CONNECT;
}

int SPI_finish(void)
{
    int res;

    res = _SPI_begin_call(false); /* live in procedure memory */
    if (res < 0) {
        return res;
    }
    /* Restore memory context as it was before procedure call */
    (void)MemoryContextSwitchTo(u_sess->SPI_cxt._current->savedcxt);

    /* Release memory used in procedure call */
    MemoryContextDelete(u_sess->SPI_cxt._current->execCxt);
    u_sess->SPI_cxt._current->execCxt = NULL;
    MemoryContextDelete(u_sess->SPI_cxt._current->procCxt);
    u_sess->SPI_cxt._current->procCxt = NULL;

    /*
     * Reset result variables, especially SPI_tuptable which is probably
     * pointing at a just-deleted tuptable
     */
    SPI_processed = 0;
    u_sess->SPI_cxt.lastoid = InvalidOid;
    SPI_tuptable = NULL;

    /*
     * After _SPI_begin_call u_sess->SPI_cxt._connected == u_sess->SPI_cxt._curid. Now we are closing
     * connection to SPI and returning to upper Executor and so u_sess->SPI_cxt._connected
     * must be equal to u_sess->SPI_cxt._curid.
     */
    u_sess->SPI_cxt._connected--;
    u_sess->SPI_cxt._curid--;
    if (u_sess->SPI_cxt._connected == -1) {
        u_sess->SPI_cxt._current = NULL;
    } else {
        u_sess->SPI_cxt._current = &(u_sess->SPI_cxt._stack[u_sess->SPI_cxt._connected]);
    }

    return SPI_OK_FINISH;
}

/*
 * Clean up SPI state at transaction commit or abort.
 */
void AtEOXact_SPI(bool isCommit)
{
    /*
     * Note that memory contexts belonging to SPI stack entries will be freed
     * automatically, so we can ignore them here.  We just need to restore our
     * static variables to initial state.
     */
    if (isCommit && u_sess->SPI_cxt._connected != -1) {
        ereport(WARNING, (errcode(ERRCODE_WARNING), errmsg("transaction left non-empty SPI stack"),
            errhint("Check for missing \"SPI_finish\" calls.")));
    }

    u_sess->SPI_cxt._current = u_sess->SPI_cxt._stack = NULL;
    u_sess->SPI_cxt._stack_depth = 0;
    u_sess->SPI_cxt._connected = u_sess->SPI_cxt._curid = -1;
    SPI_processed = 0;
    u_sess->SPI_cxt.lastoid = InvalidOid;
    SPI_tuptable = NULL;
}

/*
 * Clean up SPI state at subtransaction commit or abort.
 *
 * During commit, there shouldn't be any unclosed entries remaining from
 * the current subtransaction; we emit a warning if any are found.
 */
void AtEOSubXact_SPI(bool isCommit, SubTransactionId mySubid)
{
    bool found = false;

    while (u_sess->SPI_cxt._connected >= 0) {
        _SPI_connection *connection = &(u_sess->SPI_cxt._stack[u_sess->SPI_cxt._connected]);

        if (connection->connectSubid != mySubid) {
            break; /* couldn't be any underneath it either */
        }

        found = true;
        /*
         * Release procedure memory explicitly (see note in SPI_connect)
         */
        if (connection->execCxt) {
            MemoryContextDelete(connection->execCxt);
            connection->execCxt = NULL;
        }
        if (connection->procCxt) {
            MemoryContextDelete(connection->procCxt);
            connection->procCxt = NULL;
        }

        /*
         * Pop the stack entry and reset global variables.	Unlike
         * SPI_finish(), we don't risk switching to memory contexts that might
         * be already gone.
         */
        u_sess->SPI_cxt._connected--;
        u_sess->SPI_cxt._curid = u_sess->SPI_cxt._connected;
        if (u_sess->SPI_cxt._connected == -1) {
            u_sess->SPI_cxt._current = NULL;
        } else {
            u_sess->SPI_cxt._current = &(u_sess->SPI_cxt._stack[u_sess->SPI_cxt._connected]);
        }
        SPI_processed = 0;
        u_sess->SPI_cxt.lastoid = InvalidOid;
        SPI_tuptable = NULL;
    }

    if (found && isCommit) {
        ereport(WARNING, (errcode(ERRCODE_WARNING), errmsg("subtransaction left non-empty SPI stack"),
            errhint("Check for missing \"SPI_finish\" calls.")));
    }

    /*
     * If we are aborting a subtransaction and there is an open SPI context
     * surrounding the subxact, clean up to prevent memory leakage.
     */
    if (u_sess->SPI_cxt._current && !isCommit) {
        /* free Executor memory the same as _SPI_end_call would do */
        MemoryContextResetAndDeleteChildren(u_sess->SPI_cxt._current->execCxt);
        /* throw away any partially created tuple-table */
        SPI_freetuptable(u_sess->SPI_cxt._current->tuptable);
        u_sess->SPI_cxt._current->tuptable = NULL;
    }
}

/* Pushes SPI stack to allow recursive SPI calls */
void SPI_push(void)
{
    u_sess->SPI_cxt._curid++;
}

/* Pops SPI stack to allow recursive SPI calls */
void SPI_pop(void)
{
    u_sess->SPI_cxt._curid--;
}

/* Conditional push: push only if we're inside a SPI procedure */
bool SPI_push_conditional(void)
{
    bool pushed = (u_sess->SPI_cxt._curid != u_sess->SPI_cxt._connected);

    if (pushed) {
        u_sess->SPI_cxt._curid++;
        /* We should now be in a state where SPI_connect would succeed */
        Assert(u_sess->SPI_cxt._curid == u_sess->SPI_cxt._connected);
    }
    return pushed;
}

/* Conditional pop: pop only if SPI_push_conditional pushed */
void SPI_pop_conditional(bool pushed)
{
    /* We should be in a state where SPI_connect would succeed */
    Assert(u_sess->SPI_cxt._curid == u_sess->SPI_cxt._connected);
    if (pushed) {
        u_sess->SPI_cxt._curid--;
    }
}

/* Restore state of SPI stack after aborting a subtransaction */
void SPI_restore_connection(void)
{
    Assert(u_sess->SPI_cxt._connected >= 0);
    u_sess->SPI_cxt._curid = u_sess->SPI_cxt._connected - 1;
}

#ifdef PGXC
/* SPI_execute_direct:
 * Runs the 'remote_sql' query string on the node 'nodename'
 * Create the ExecDirectStmt parse tree node using remote_sql, and then prepare
 * and execute it using SPI interface.
 * This function is essentially used for making internal exec-direct operations;
 * and this should not require super-user privileges. We cannot run EXEC-DIRECT
 * query because it is meant only for superusers. So this function needs to
 * bypass the parse stage. This is achieved here by calling
 * _SPI_pgxc_prepare_plan which accepts a parse tree.
 */
int SPI_execute_direct(const char *remote_sql, char *nodename)
{
    _SPI_plan plan;
    int res;
    ExecDirectStmt *stmt = makeNode(ExecDirectStmt);
    StringInfoData execdirect;

    initStringInfo(&execdirect);

    /* This string is never used. It is just passed to fill up spi_err_context.arg */
    appendStringInfo(&execdirect, "EXECUTE DIRECT ON (%s) '%s'", nodename, remote_sql);

    stmt->node_names = list_make1(makeString(nodename));
    stmt->query = pstrdup(remote_sql);

    res = _SPI_begin_call(true);
    if (res < 0) {
        return res;
    }

    errno_t errorno = memset_s(&plan, sizeof(_SPI_plan), '\0', sizeof(_SPI_plan));
    securec_check(errorno, "\0", "\0");
    plan.magic = _SPI_PLAN_MAGIC;
    plan.cursor_options = 0;

    /* Now pass the ExecDirectStmt parsetree node */
    _SPI_pgxc_prepare_plan(execdirect.data, list_make1(stmt), &plan);

    res = _SPI_execute_plan(&plan, NULL, InvalidSnapshot, InvalidSnapshot, false, true, 0, true);

    _SPI_end_call(true);
    return res;
}
#endif

/* Parse, plan, and execute a query string */
int SPI_execute(const char *src, bool read_only, long tcount)
{
    _SPI_plan plan;
    int res;

    if (src == NULL || tcount < 0) {
        return SPI_ERROR_ARGUMENT;
    }

    res = _SPI_begin_call(true);
    if (res < 0) {
        return res;
    }
    errno_t errorno = memset_s(&plan, sizeof(_SPI_plan), '\0', sizeof(_SPI_plan));
    securec_check(errorno, "\0", "\0");
    plan.magic = _SPI_PLAN_MAGIC;
    plan.cursor_options = 0;

    _SPI_prepare_oneshot_plan(src, &plan);

    res = _SPI_execute_plan(&plan, NULL, InvalidSnapshot, InvalidSnapshot, read_only, true, tcount);

    _SPI_end_call(true);
    return res;
}

/* Obsolete version of SPI_execute */
int SPI_exec(const char *src, long tcount)
{
    return SPI_execute(src, false, tcount);
}

/* Execute a previously prepared plan */
int SPI_execute_plan(SPIPlanPtr plan, Datum *Values, const char *Nulls, bool read_only, long tcount)
{
    int res;

    if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC || tcount < 0) {
        return SPI_ERROR_ARGUMENT;
    }

    if (plan->nargs > 0 && Values == NULL)
        return SPI_ERROR_PARAM;

    res = _SPI_begin_call(true);
    if (res < 0) {
        return res;
    }
    res = _SPI_execute_plan(plan, _SPI_convert_params(plan->nargs, plan->argtypes, Values, Nulls), InvalidSnapshot,
        InvalidSnapshot, read_only, true, tcount);

    _SPI_end_call(true);
    return res;
}

/* Obsolete version of SPI_execute_plan */
int SPI_execp(SPIPlanPtr plan, Datum *Values, const char *Nulls, long tcount)
{
    return SPI_execute_plan(plan, Values, Nulls, false, tcount);
}

/* Execute a previously prepared plan */
int SPI_execute_plan_with_paramlist(SPIPlanPtr plan, ParamListInfo params, bool read_only, long tcount)
{
    int res;

    if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC || tcount < 0) {
        return SPI_ERROR_ARGUMENT;
    }

    res = _SPI_begin_call(true);
    if (res < 0) {
        return res;
    }
    res = _SPI_execute_plan(plan, params, InvalidSnapshot, InvalidSnapshot, read_only, true, tcount);

    _SPI_end_call(true);
    return res;
}

/*
 * SPI_execute_snapshot -- identical to SPI_execute_plan, except that we allow
 * the caller to specify exactly which snapshots to use, which will be
 * registered here.  Also, the caller may specify that AFTER triggers should be
 * queued as part of the outer query rather than being fired immediately at the
 * end of the command.
 *
 * This is currently not documented in spi.sgml because it is only intended
 * for use by RI triggers.
 *
 * Passing snapshot == InvalidSnapshot will select the normal behavior of
 * fetching a new snapshot for each query.
 */
int SPI_execute_snapshot(SPIPlanPtr plan, Datum *Values, const char *Nulls, Snapshot snapshot,
    Snapshot crosscheck_snapshot, bool read_only, bool fire_triggers, long tcount)
{
    int res;

    if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC || tcount < 0) {
        return SPI_ERROR_ARGUMENT;
    }

    if (plan->nargs > 0 && Values == NULL) {
        return SPI_ERROR_PARAM;
    }

    res = _SPI_begin_call(true);
    if (res < 0) {
        return res;
    }
    res = _SPI_execute_plan(plan, _SPI_convert_params(plan->nargs, plan->argtypes, Values, Nulls), snapshot,
        crosscheck_snapshot, read_only, fire_triggers, tcount);

    _SPI_end_call(true);
    return res;
}

/*
 * SPI_execute_with_args -- plan and execute a query with supplied arguments
 *
 * This is functionally equivalent to SPI_prepare followed by
 * SPI_execute_plan.
 */
int SPI_execute_with_args(const char *src, int nargs, Oid *argtypes, Datum *Values, const char *Nulls, bool read_only,
    long tcount, Cursor_Data *cursor_data)
{
    int res;
    _SPI_plan plan;
    ParamListInfo param_list_info;

    if (src == NULL || nargs < 0 || tcount < 0) {
        return SPI_ERROR_ARGUMENT;
    }

    if (nargs > 0 && (argtypes == NULL || Values == NULL)) {
        return SPI_ERROR_PARAM;
    }

    res = _SPI_begin_call(true);
    if (res < 0) {
        return res;
    }
    errno_t errorno = memset_s(&plan, sizeof(_SPI_plan), '\0', sizeof(_SPI_plan));
    securec_check(errorno, "\0", "\0");
    plan.magic = _SPI_PLAN_MAGIC;
    plan.cursor_options = 0;
    plan.nargs = nargs;
    plan.argtypes = argtypes;
    plan.parserSetup = NULL;
    plan.parserSetupArg = NULL;

    param_list_info = _SPI_convert_params(nargs, argtypes, Values, Nulls, cursor_data);

    _SPI_prepare_oneshot_plan(src, &plan);

    res = _SPI_execute_plan(&plan, param_list_info, InvalidSnapshot, InvalidSnapshot, read_only, true, tcount);

    _SPI_end_call(true);
    return res;
}

SPIPlanPtr SPI_prepare(const char *src, int nargs, Oid *argtypes)
{
    return SPI_prepare_cursor(src, nargs, argtypes, 0);
}

SPIPlanPtr SPI_prepare_cursor(const char *src, int nargs, Oid *argtypes, int cursorOptions)
{
    _SPI_plan plan;
    SPIPlanPtr result;

    if (src == NULL || nargs < 0 || (nargs > 0 && argtypes == NULL)) {
        SPI_result = SPI_ERROR_ARGUMENT;
        return NULL;
    }

    SPI_result = _SPI_begin_call(true);
    if (SPI_result < 0) {
        return NULL;
    }

    errno_t errorno = memset_s(&plan, sizeof(_SPI_plan), '\0', sizeof(_SPI_plan));
    securec_check(errorno, "\0", "\0");
    plan.magic = _SPI_PLAN_MAGIC;
    plan.cursor_options = cursorOptions;
    plan.nargs = nargs;
    plan.argtypes = argtypes;
    plan.parserSetup = NULL;
    plan.parserSetupArg = NULL;

    _SPI_prepare_plan(src, &plan);

    /* copy plan to procedure context */
    result = _SPI_make_plan_non_temp(&plan);

    _SPI_end_call(true);

    return result;
}

SPIPlanPtr SPI_prepare_params(const char *src, ParserSetupHook parserSetup, void *parserSetupArg, int cursorOptions)
{
    _SPI_plan plan;
    SPIPlanPtr result;

    if (src == NULL) {
        SPI_result = SPI_ERROR_ARGUMENT;
        return NULL;
    }

    SPI_result = _SPI_begin_call(true);
    if (SPI_result < 0) {
        return NULL;
    }

    errno_t errorno = memset_s(&plan, sizeof(_SPI_plan), '\0', sizeof(_SPI_plan));
    securec_check(errorno, "\0", "\0");
    plan.magic = _SPI_PLAN_MAGIC;
    plan.cursor_options = cursorOptions;
    plan.nargs = 0;
    plan.argtypes = NULL;
    plan.parserSetup = parserSetup;
    plan.parserSetupArg = parserSetupArg;

    _SPI_prepare_plan(src, &plan);

    /* copy plan to procedure context */
    result = _SPI_make_plan_non_temp(&plan);

    _SPI_end_call(true);

    return result;
}

int SPI_keepplan(SPIPlanPtr plan)
{
    ListCell *lc = NULL;

    if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC || plan->saved || plan->oneshot) {
        return SPI_ERROR_ARGUMENT;
    }

    /*
     * Mark it saved, reparent it under u_sess->cache_mem_cxt, and mark all the
     * component CachedPlanSources as saved.  This sequence cannot fail
     * partway through, so there's no risk of long-term memory leakage.
     */
    plan->saved = true;
    MemoryContextSetParent(plan->plancxt, u_sess->cache_mem_cxt);

    foreach (lc, plan->plancache_list) {
        CachedPlanSource *plansource = (CachedPlanSource *)lfirst(lc);

        SaveCachedPlan(plansource);
    }

    return 0;
}

SPIPlanPtr SPI_saveplan(SPIPlanPtr plan)
{
    SPIPlanPtr new_plan = NULL;

    if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC) {
        SPI_result = SPI_ERROR_ARGUMENT;
        return NULL;
    }

    SPI_result = _SPI_begin_call(false); /* don't change context */
    if (SPI_result < 0) {
        return NULL;
    }

    new_plan = _SPI_save_plan(plan);

    SPI_result = _SPI_end_call(false);

    return new_plan;
}

int SPI_freeplan(SPIPlanPtr plan)
{
    ListCell *lc = NULL;

    if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC) {
        return SPI_ERROR_ARGUMENT;
    }


    /* Release the plancache entries */
    foreach (lc, plan->plancache_list) {
        CachedPlanSource *plansource = (CachedPlanSource *)lfirst(lc);
        DropCachedPlan(plansource);
    }

    /* Now get rid of the _SPI_plan and subsidiary data in its plancxt */
    MemoryContextDelete(plan->plancxt);

    return 0;
}

HeapTuple SPI_copytuple(HeapTuple tuple)
{
    MemoryContext old_ctx = NULL;
    HeapTuple c_tuple;

    if (tuple == NULL) {
        SPI_result = SPI_ERROR_ARGUMENT;
        return NULL;
    }

    if (u_sess->SPI_cxt._curid + 1 == u_sess->SPI_cxt._connected) { /* connected */
        if (u_sess->SPI_cxt._current != &(u_sess->SPI_cxt._stack[u_sess->SPI_cxt._curid + 1])) {
            ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                errmsg("SPI stack corrupted when copy tuple, connected level: %d", u_sess->SPI_cxt._connected)));
        }

        old_ctx = MemoryContextSwitchTo(u_sess->SPI_cxt._current->savedcxt);
    }

    c_tuple = heap_copytuple(tuple);

    if (old_ctx) {
        (void)MemoryContextSwitchTo(old_ctx);
    }

    return c_tuple;
}

HeapTupleHeader SPI_returntuple(HeapTuple tuple, TupleDesc tupdesc)
{
    MemoryContext old_ctx = NULL;
    HeapTupleHeader d_tup;

    if (tuple == NULL || tupdesc == NULL) {
        SPI_result = SPI_ERROR_ARGUMENT;
        return NULL;
    }

    /* For RECORD results, make sure a typmod has been assigned */
    if (tupdesc->tdtypeid == RECORDOID && tupdesc->tdtypmod < 0) {
        assign_record_type_typmod(tupdesc);
    }

    if (u_sess->SPI_cxt._curid + 1 == u_sess->SPI_cxt._connected) { /* connected */
        if (u_sess->SPI_cxt._current != &(u_sess->SPI_cxt._stack[u_sess->SPI_cxt._curid + 1])) {
            ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                errmsg("SPI stack corrupted when return tuple, connected level: %d", u_sess->SPI_cxt._connected)));
        }

        old_ctx = MemoryContextSwitchTo(u_sess->SPI_cxt._current->savedcxt);
    }

    d_tup = (HeapTupleHeader)palloc(tuple->t_len);
    errno_t rc = memcpy_s((char *)d_tup, tuple->t_len, (char *)tuple->t_data, tuple->t_len);
    securec_check(rc, "\0", "\0");

    HeapTupleHeaderSetDatumLength(d_tup, tuple->t_len);
    HeapTupleHeaderSetTypeId(d_tup, tupdesc->tdtypeid);
    HeapTupleHeaderSetTypMod(d_tup, tupdesc->tdtypmod);

    if (old_ctx) {
        (void)MemoryContextSwitchTo(old_ctx);
    }

    return d_tup;
}

HeapTuple SPI_modifytuple(Relation rel, HeapTuple tuple, int natts, int *attnum, Datum *Values, const char *Nulls)
{
    MemoryContext old_ctx = NULL;
    HeapTuple m_tuple = NULL;
    int num_of_attr;
    Datum *v = NULL;
    bool *n = NULL;
    int i;

    if (rel == NULL || tuple == NULL || natts < 0 || attnum == NULL || Values == NULL) {
        SPI_result = SPI_ERROR_ARGUMENT;
        return NULL;
    }

    if (u_sess->SPI_cxt._curid + 1 == u_sess->SPI_cxt._connected) { /* connected */
        if (u_sess->SPI_cxt._current != &(u_sess->SPI_cxt._stack[u_sess->SPI_cxt._curid + 1])) {
            ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                errmsg("SPI stack corrupted when modify tuple, connected level: %d", u_sess->SPI_cxt._connected)));
        }

        old_ctx = MemoryContextSwitchTo(u_sess->SPI_cxt._current->savedcxt);
    }
    SPI_result = 0;
    num_of_attr = rel->rd_att->natts;
    v = (Datum *)palloc(num_of_attr * sizeof(Datum));
    n = (bool *)palloc(num_of_attr * sizeof(bool));

    /* fetch old values and nulls */
    heap_deform_tuple(tuple, rel->rd_att, v, n);

    /* replace values and nulls */
    for (i = 0; i < natts; i++) {
        if (attnum[i] <= 0 || attnum[i] > num_of_attr) {
            break;
        }
        v[attnum[i] - 1] = Values[i];
        n[attnum[i] - 1] = (Nulls && Nulls[i] == 'n') ? true : false;
    }

    if (i == natts) {
        /* no errors in *attnum */
        m_tuple = heap_form_tuple(rel->rd_att, v, n);

        /*
         * copy the identification info of the old tuple: t_ctid, t_self, and
         * OID (if any)
         */
        m_tuple->t_data->t_ctid = tuple->t_data->t_ctid;
        m_tuple->t_self = tuple->t_self;
        m_tuple->t_tableOid = tuple->t_tableOid;
        m_tuple->t_bucketId = tuple->t_bucketId;
        HeapTupleCopyBase(m_tuple, tuple);
#ifdef PGXC
        m_tuple->t_xc_node_id = tuple->t_xc_node_id;
#endif

        if (rel->rd_att->tdhasoid) {
            HeapTupleSetOid(m_tuple, HeapTupleGetOid(tuple));
        }
    } else {
        m_tuple = NULL;
        SPI_result = SPI_ERROR_NOATTRIBUTE;
    }

    pfree_ext(v);
    pfree_ext(n);

    if (old_ctx) {
        (void)MemoryContextSwitchTo(old_ctx);
    }
    return m_tuple;
}

int SPI_fnumber(TupleDesc tupdesc, const char *fname)
{
    int res;
    Form_pg_attribute sys_att;

    for (res = 0; res < tupdesc->natts; res++) {
        if (namestrcmp(&tupdesc->attrs[res]->attname, fname) == 0) {
            return res + 1;
        }
    }

    sys_att = SystemAttributeByName(fname, true /* "oid" will be accepted */);
    if (sys_att != NULL) {
        return sys_att->attnum;
    }

    /* SPI_ERROR_NOATTRIBUTE is different from all sys column numbers */
    return SPI_ERROR_NOATTRIBUTE;
}

char *SPI_fname(TupleDesc tupdesc, int fnumber)
{
    Form_pg_attribute attr;
    SPI_result = 0;

    if (fnumber > tupdesc->natts || fnumber == 0 || fnumber <= FirstLowInvalidHeapAttributeNumber) {
        SPI_result = SPI_ERROR_NOATTRIBUTE;
        return NULL;
    }

    if (fnumber > 0) {
        attr = tupdesc->attrs[fnumber - 1];
    } else {
        attr = SystemAttributeDefinition(fnumber, true, false);
    }

    return pstrdup(NameStr(attr->attname));
}

char *SPI_getvalue(HeapTuple tuple, TupleDesc tupdesc, int fnumber)
{
    char *result = NULL;
    Datum orig_val, val;
    bool is_null = false;
    Oid typoid, foutoid;
    bool typo_is_varlen = false;

    SPI_result = 0;

    if (fnumber > tupdesc->natts || fnumber == 0 || fnumber <= FirstLowInvalidHeapAttributeNumber) {
        SPI_result = SPI_ERROR_NOATTRIBUTE;
        return NULL;
    }

    orig_val = heap_getattr(tuple, (unsigned int)fnumber, tupdesc, &is_null);
    if (is_null) {
        return NULL;
    }

    if (fnumber > 0) {
        typoid = tupdesc->attrs[fnumber - 1]->atttypid;
    } else {
        typoid = (SystemAttributeDefinition(fnumber, true, false))->atttypid;
    }

    getTypeOutputInfo(typoid, &foutoid, &typo_is_varlen);

    /*
     * If we have a toasted datum, forcibly detoast it here to avoid memory
     * leakage inside the type's output routine.
     */
    if (typo_is_varlen) {
        val = PointerGetDatum(PG_DETOAST_DATUM(orig_val));
    } else {
        val = orig_val;
    }

    result = OidOutputFunctionCall(foutoid, val);

    /* Clean up detoasted copy, if any */
    if (val != orig_val) {
        pfree(DatumGetPointer(val));
    }

    return result;
}

Datum SPI_getbinval(HeapTuple tuple, TupleDesc tupdesc, int fnumber, bool *isnull)
{
    SPI_result = 0;

    if (fnumber > tupdesc->natts || fnumber == 0 || fnumber <= FirstLowInvalidHeapAttributeNumber) {
        SPI_result = SPI_ERROR_NOATTRIBUTE;
        *isnull = true;
        return (Datum)NULL;
    }

    return heap_getattr(tuple, (unsigned int)fnumber, tupdesc, isnull);
}

char *SPI_gettype(TupleDesc tupdesc, int fnumber)
{
    Oid typoid;
    HeapTuple type_tuple = NULL;
    char *result = NULL;
    SPI_result = 0;

    if (fnumber > tupdesc->natts || fnumber == 0 || fnumber <= FirstLowInvalidHeapAttributeNumber) {
        SPI_result = SPI_ERROR_NOATTRIBUTE;
        return NULL;
    }

    if (fnumber > 0) {
        typoid = tupdesc->attrs[fnumber - 1]->atttypid;
    } else {
        typoid = (SystemAttributeDefinition(fnumber, true, false))->atttypid;
    }

    type_tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typoid));
    if (!HeapTupleIsValid(type_tuple)) {
        SPI_result = SPI_ERROR_TYPUNKNOWN;
        return NULL;
    }

    result = pstrdup(NameStr(((Form_pg_type)GETSTRUCT(type_tuple))->typname));
    ReleaseSysCache(type_tuple);
    return result;
}

Oid SPI_gettypeid(TupleDesc tupdesc, int fnumber)
{
    SPI_result = 0;

    if (fnumber > tupdesc->natts || fnumber == 0 || fnumber <= FirstLowInvalidHeapAttributeNumber) {
        SPI_result = SPI_ERROR_NOATTRIBUTE;
        return InvalidOid;
    }

    if (fnumber > 0) {
        return tupdesc->attrs[fnumber - 1]->atttypid;
    } else {
        return (SystemAttributeDefinition(fnumber, true, false))->atttypid;
    }
}

char *SPI_getrelname(Relation rel)
{
    return pstrdup(RelationGetRelationName(rel));
}

char *SPI_getnspname(Relation rel)
{
    return get_namespace_name(RelationGetNamespace(rel), true);
}

void *SPI_palloc(Size size)
{
    MemoryContext old_ctx = NULL;
    void *pointer = NULL;

    if (u_sess->SPI_cxt._curid + 1 == u_sess->SPI_cxt._connected) { /* connected */
        if (u_sess->SPI_cxt._current != &(u_sess->SPI_cxt._stack[u_sess->SPI_cxt._curid + 1])) {
            ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                errmsg("SPI stack corrupted when allocate, connected level: %d", u_sess->SPI_cxt._connected)));
        }

        old_ctx = MemoryContextSwitchTo(u_sess->SPI_cxt._current->savedcxt);
    }

    pointer = palloc(size);

    if (old_ctx) {
        (void)MemoryContextSwitchTo(old_ctx);
    }

    return pointer;
}

void *SPI_repalloc(void *pointer, Size size)
{
    /* No longer need to worry which context chunk was in... */
    return repalloc(pointer, size);
}

void SPI_pfree_ext(void *pointer)
{
    /* No longer need to worry which context chunk was in... */
    pfree_ext(pointer);
}

void SPI_freetuple(HeapTuple tuple)
{
    /* No longer need to worry which context tuple was in... */
    heap_freetuple_ext(tuple);
}

void SPI_freetuptable(SPITupleTable *tuptable)
{
    if (tuptable != NULL) {
        MemoryContextDelete(tuptable->tuptabcxt);
    }
}

/*
 * SPI_cursor_open
 *
 * 	Open a prepared SPI plan as a portal
 */
Portal SPI_cursor_open(const char *name, SPIPlanPtr plan, Datum *Values, const char *Nulls, bool read_only)
{
    Portal portal;
    ParamListInfo param_list_info;

    /* build transient ParamListInfo in caller's context */
    param_list_info = _SPI_convert_params(plan->nargs, plan->argtypes, Values, Nulls);

    portal = SPI_cursor_open_internal(name, plan, param_list_info, read_only);

    /* done with the transient ParamListInfo */
    if (param_list_info) {
        pfree_ext(param_list_info);
    }

    return portal;
}

/*
 * SPI_cursor_open_with_args
 *
 * Parse and plan a query and open it as a portal.
 */
Portal SPI_cursor_open_with_args(const char *name, const char *src, int nargs, Oid *argtypes, Datum *Values,
    const char *Nulls, bool read_only, int cursorOptions)
{
    Portal result;
    _SPI_plan plan;
    ParamListInfo param_list_info;
    errno_t errorno = EOK;

    if (src == NULL || nargs < 0) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("open cursor with args has invalid arguments, %s",
            src == NULL ? "query string is NULL" : "argument number is less than zero.")));
    }

    if (nargs > 0 && (argtypes == NULL || Values == NULL)) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("open cursor with args has invalid arguments, %s",
            argtypes == NULL ? "argument type is NULL" : "value is NULL")));
    }

    SPI_result = _SPI_begin_call(true);
    if (SPI_result < 0) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_CURSOR_STATE),
            errmsg("SPI stack is corrupted when open cursor with args, current level: %d, connected level: %d",
            u_sess->SPI_cxt._curid, u_sess->SPI_cxt._connected)));
    }

    errorno = memset_s(&plan, sizeof(_SPI_plan), '\0', sizeof(_SPI_plan));
    securec_check(errorno, "\0", "\0");

    plan.magic = _SPI_PLAN_MAGIC;
    plan.cursor_options = cursorOptions;
    plan.nargs = nargs;
    plan.argtypes = argtypes;
    plan.parserSetup = NULL;
    plan.parserSetupArg = NULL;

    /* build transient ParamListInfo in executor context */
    param_list_info = _SPI_convert_params(nargs, argtypes, Values, Nulls);

    _SPI_prepare_plan(src, &plan);

    /* We needn't copy the plan; SPI_cursor_open_internal will do so */
    /* Adjust stack so that SPI_cursor_open_internal doesn't complain */
    u_sess->SPI_cxt._curid--;

    result = SPI_cursor_open_internal(name, &plan, param_list_info, read_only);

    /* And clean up */
    u_sess->SPI_cxt._curid++;
    _SPI_end_call(true);

    return result;
}

/*
 * SPI_cursor_open_with_paramlist
 *
 * 	Same as SPI_cursor_open except that parameters (if any) are passed
 * 	as a ParamListInfo, which supports dynamic parameter set determination
 */
Portal SPI_cursor_open_with_paramlist(const char *name, SPIPlanPtr plan, ParamListInfo params, bool read_only)
{
    return SPI_cursor_open_internal(name, plan, params, read_only);
}

/*
 * SPI_cursor_open_internal
 *
 * 	Common code for SPI_cursor_open variants
 */
static Portal SPI_cursor_open_internal(const char *name, SPIPlanPtr plan, ParamListInfo paramLI, bool read_only)
{
    CachedPlanSource *plansource = NULL;
    CachedPlan *cplan = NULL;
    List *stmt_list = NIL;
    char *query_string = NULL;
    Snapshot snapshot;
    MemoryContext old_ctx;
    Portal portal;
    ErrorContextCallback spi_err_context;

    /*
     * Check that the plan is something the Portal code will special-case as
     * returning one tupleset.
     */
    if (!SPI_is_cursor_plan(plan, paramLI)) {
        /* try to give a good error message */
        if (list_length(plan->plancache_list) != 1) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_CURSOR_DEFINITION), errmsg("cannot open multi-query plan as cursor")));
        }

        plansource = (CachedPlanSource *)linitial(plan->plancache_list);
        ereport(ERROR, (errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
            /* translator: %s is name of a SQL command, eg INSERT */
            errmsg("cannot open %s query as cursor", plansource->commandTag)));
    }

    Assert(list_length(plan->plancache_list) == 1);
    plansource = (CachedPlanSource *)linitial(plan->plancache_list);

    /* Push the SPI stack */
    if (_SPI_begin_call(true) < 0) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_CURSOR_STATE),
            errmsg("SPI stack is corrupted when open cursor, current level: %d, connected level: %d",
            u_sess->SPI_cxt._curid, u_sess->SPI_cxt._connected)));
    }

    /* Reset SPI result (note we deliberately don't touch lastoid) */
    SPI_processed = 0;
    SPI_tuptable = NULL;
    u_sess->SPI_cxt._current->processed = 0;
    u_sess->SPI_cxt._current->tuptable = NULL;

    /* Create the portal */
    if (name == NULL || name[0] == '\0') {
        /* Use a random nonconflicting name */
        portal = CreateNewPortal();
    } else {
        /* In this path, error if portal of same name already exists */
        portal = CreatePortal(name, false, false);
    }

    /* Copy the plan's query string into the portal */
    query_string = MemoryContextStrdup(PortalGetHeapMemory(portal), plansource->query_string);

    /*
     * Setup error traceback support for ereport(), in case GetCachedPlan
     * throws an error.
     */
    spi_err_context.callback = _SPI_error_callback;
    spi_err_context.arg = (void *)plansource->query_string;
    spi_err_context.previous = t_thrd.log_cxt.error_context_stack;
    t_thrd.log_cxt.error_context_stack = &spi_err_context;

    /*
     * Note: for a saved plan, we mustn't have any failure occur between
     * GetCachedPlan and PortalDefineQuery; that would result in leaking our
     * plancache refcount.
     */
    /* Replan if needed, and increment plan refcount for portal */
    cplan = GetCachedPlan(plansource, paramLI, false);
    stmt_list = cplan->stmt_list;

    /* Pop the error context stack */
    t_thrd.log_cxt.error_context_stack = spi_err_context.previous;

    if (!plan->saved) {
        /*
         * We don't want the portal to depend on an unsaved CachedPlanSource,
         * so must copy the plan into the portal's context.  An error here
         * will result in leaking our refcount on the plan, but it doesn't
         * matter because the plan is unsaved and hence transient anyway.
         */
        old_ctx = MemoryContextSwitchTo(PortalGetHeapMemory(portal));
        stmt_list = (List *)copyObject(stmt_list);
        (void)MemoryContextSwitchTo(old_ctx);
        ReleaseCachedPlan(cplan, false);
        cplan = NULL; /* portal shouldn't depend on cplan */
    }

    /*
     * Set up the portal.
     */
    PortalDefineQuery(portal, NULL, /* no statement name */
        query_string, plansource->commandTag, stmt_list, cplan);

    /*
     * Set up options for portal.  Default SCROLL type is chosen the same way
     * as PerformCursorOpen does it.
     */
    portal->cursorOptions = plan->cursor_options;
    if (!(portal->cursorOptions & (CURSOR_OPT_SCROLL | CURSOR_OPT_NO_SCROLL))) {
        if (list_length(stmt_list) == 1 && IsA((Node *)linitial(stmt_list), PlannedStmt) &&
            ((PlannedStmt *)linitial(stmt_list))->rowMarks == NIL &&
            ExecSupportsBackwardScan(((PlannedStmt *)linitial(stmt_list))->planTree)) {
            portal->cursorOptions |= CURSOR_OPT_SCROLL;
        }

        else {
            portal->cursorOptions |= CURSOR_OPT_NO_SCROLL;
        }
    }

    /*
     * Disallow SCROLL with SELECT FOR UPDATE.	This is not redundant with the
     * check in transformDeclareCursorStmt because the cursor options might
     * not have come through there.
     */
    if (portal->cursorOptions & CURSOR_OPT_SCROLL) {
        if (list_length(stmt_list) == 1 && IsA((Node *)linitial(stmt_list), PlannedStmt) &&
            ((PlannedStmt *)linitial(stmt_list))->rowMarks != NIL) {
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("DECLARE SCROLL CURSOR ... FOR UPDATE/SHARE is not supported"),
                errdetail("Scrollable cursors must be READ ONLY.")));
        }
    }

    /*
     * If told to be read-only, we'd better check for read-only queries. This
     * can't be done earlier because we need to look at the finished, planned
     * queries.  (In particular, we don't want to do it between GetCachedPlan
     * and PortalDefineQuery, because throwing an error between those steps
     * would result in leaking our plancache refcount.)
     */
    if (read_only) {
        ListCell *lc = NULL;

        foreach (lc, stmt_list) {
            Node *pstmt = (Node *)lfirst(lc);

            if (!CommandIsReadOnly(pstmt)) {
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    /* translator: %s is a SQL statement name */
                    errmsg("%s is not allowed in a non-volatile function", CreateCommandTag(pstmt)),
                    errhint("You can change function definition.")));
            }
        }
    }

    /* Set up the snapshot to use. */
    if (read_only) {
        snapshot = GetActiveSnapshot();
    }

    else {
        CommandCounterIncrement();
        snapshot = GetTransactionSnapshot();
    }

    /*
     * If the plan has parameters, copy them into the portal.  Note that this
     * must be done after revalidating the plan, because in dynamic parameter
     * cases the set of parameters could have changed during re-parsing.
     */
    if (paramLI) {
        old_ctx = MemoryContextSwitchTo(PortalGetHeapMemory(portal));
        paramLI = copyParamList(paramLI);
        (void)MemoryContextSwitchTo(old_ctx);
    }

    /*
     * Start portal execution.
     */
    PortalStart(portal, paramLI, 0, snapshot);

    Assert(portal->strategy != PORTAL_MULTI_QUERY);

    /* Pop the SPI stack */
    _SPI_end_call(true);

    /* Return the created portal */
    return portal;
}

/*
 * SPI_cursor_find
 *
 * 	Find the portal of an existing open cursor
 */
Portal SPI_cursor_find(const char *name)
{
    return GetPortalByName(name);
}

/*
 * SPI_cursor_fetch
 *
 * 	Fetch rows in a cursor
 */
void SPI_cursor_fetch(Portal portal, bool forward, long count)
{
    _SPI_cursor_operation(portal, forward ? FETCH_FORWARD : FETCH_BACKWARD, count, CreateDestReceiver(DestSPI));
    /* we know that the DestSPI receiver doesn't need a destroy call */
}

/*
 * SPI_cursor_move
 *
 * 	Move in a cursor
 */
void SPI_cursor_move(Portal portal, bool forward, long count)
{
    _SPI_cursor_operation(portal, forward ? FETCH_FORWARD : FETCH_BACKWARD, count, None_Receiver);
}

/*
 * SPI_scroll_cursor_fetch
 *
 * 	Fetch rows in a scrollable cursor
 */
void SPI_scroll_cursor_fetch(Portal portal, FetchDirection direction, long count)
{
    _SPI_cursor_operation(portal, direction, count, CreateDestReceiver(DestSPI));
    /* we know that the DestSPI receiver doesn't need a destroy call */
}

/*
 * SPI_scroll_cursor_move
 *
 * 	Move in a scrollable cursor
 */
void SPI_scroll_cursor_move(Portal portal, FetchDirection direction, long count)
{
    _SPI_cursor_operation(portal, direction, count, None_Receiver);
}

/*
 * SPI_cursor_close
 *
 * 	Close a cursor
 */
void SPI_cursor_close(Portal portal)
{
    if (!PortalIsValid(portal)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_CURSOR_STATE), errmsg("invalid portal in SPI cursor close operation")));
    }


    PortalDrop(portal, false);
}

/*
 * Returns the Oid representing the type id for argument at argIndex. First
 * parameter is at index zero.
 */
Oid SPI_getargtypeid(SPIPlanPtr plan, int argIndex)
{
    if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC || argIndex < 0 || argIndex >= plan->nargs) {
        SPI_result = SPI_ERROR_ARGUMENT;
        return InvalidOid;
    }
    return plan->argtypes[argIndex];
}

/*
 * Returns the number of arguments for the prepared plan.
 */
int SPI_getargcount(SPIPlanPtr plan)
{
    if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC) {
        SPI_result = SPI_ERROR_ARGUMENT;
        return -1;
    }
    return plan->nargs;
}

/*
 * Returns true if the plan contains exactly one command
 * and that command returns tuples to the caller (eg, SELECT or
 * INSERT ... RETURNING, but not SELECT ... INTO). In essence,
 * the result indicates if the command can be used with SPI_cursor_open
 *
 * Parameters
 * 	  plan: A plan previously prepared using SPI_prepare
 * 	 paramLI: hook function and possibly data values for plan
 */
bool SPI_is_cursor_plan(SPIPlanPtr plan, ParamListInfo paramLI)
{
    CachedPlanSource *plan_source = NULL;

    if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC) {
        SPI_result = SPI_ERROR_ARGUMENT;
        return false;
    }

    if (list_length(plan->plancache_list) != 1) {
        SPI_result = 0;
        return false; /* not exactly 1 pre-rewrite command */
    }
    plan_source = (CachedPlanSource *)linitial(plan->plancache_list);

    /*
     * We used to force revalidation of the cached plan here, but that seems
     * unnecessary: invalidation could mean a change in the rowtype of the
     * tuples returned by a plan, but not whether it returns tuples at all.
     */
    SPI_result = 0;

    /* Does it return tuples? */
    if (plan_source->resultDesc) {
        CachedPlan *cplan = NULL;
        cplan = GetCachedPlan(plan_source, paramLI, false);
        PortalStrategy strategy = ChoosePortalStrategy(cplan->stmt_list);
        if (strategy == PORTAL_MULTI_QUERY) {
            return false;
        } else {
            return true;
        }
    }

    return false;
}

/*
 * SPI_plan_is_valid --- test whether a SPI plan is currently valid
 * (that is, not marked as being in need of revalidation).
 *
 * See notes for CachedPlanIsValid before using this.
 */
bool SPI_plan_is_valid(SPIPlanPtr plan)
{
    ListCell *lc = NULL;

    Assert(plan->magic == _SPI_PLAN_MAGIC);

    foreach (lc, plan->plancache_list) {
        CachedPlanSource *plansource = (CachedPlanSource *)lfirst(lc);

        if (!CachedPlanIsValid(plansource)) {
            return false;
        }
    }
    return true;
}

/*
 * SPI_result_code_string --- convert any SPI return code to a string
 *
 * This is often useful in error messages.	Most callers will probably
 * only pass negative (error-case) codes, but for generality we recognize
 * the success codes too.
 */
const char *SPI_result_code_string(int code)
{
    char *buf = u_sess->SPI_cxt.buf;

    switch (code) {
        case SPI_ERROR_CONNECT:
            return "SPI_ERROR_CONNECT";
        case SPI_ERROR_COPY:
            return "SPI_ERROR_COPY";
        case SPI_ERROR_OPUNKNOWN:
            return "SPI_ERROR_OPUNKNOWN";
        case SPI_ERROR_UNCONNECTED:
            return "SPI_ERROR_UNCONNECTED";
        case SPI_ERROR_ARGUMENT:
            return "SPI_ERROR_ARGUMENT";
        case SPI_ERROR_PARAM:
            return "SPI_ERROR_PARAM";
        case SPI_ERROR_TRANSACTION:
            return "SPI_ERROR_TRANSACTION";
        case SPI_ERROR_NOATTRIBUTE:
            return "SPI_ERROR_NOATTRIBUTE";
        case SPI_ERROR_NOOUTFUNC:
            return "SPI_ERROR_NOOUTFUNC";
        case SPI_ERROR_TYPUNKNOWN:
            return "SPI_ERROR_TYPUNKNOWN";
        case SPI_OK_CONNECT:
            return "SPI_OK_CONNECT";
        case SPI_OK_FINISH:
            return "SPI_OK_FINISH";
        case SPI_OK_FETCH:
            return "SPI_OK_FETCH";
        case SPI_OK_UTILITY:
            return "SPI_OK_UTILITY";
        case SPI_OK_SELECT:
            return "SPI_OK_SELECT";
        case SPI_OK_SELINTO:
            return "SPI_OK_SELINTO";
        case SPI_OK_INSERT:
            return "SPI_OK_INSERT";
        case SPI_OK_DELETE:
            return "SPI_OK_DELETE";
        case SPI_OK_UPDATE:
            return "SPI_OK_UPDATE";
        case SPI_OK_CURSOR:
            return "SPI_OK_CURSOR";
        case SPI_OK_INSERT_RETURNING:
            return "SPI_OK_INSERT_RETURNING";
        case SPI_OK_DELETE_RETURNING:
            return "SPI_OK_DELETE_RETURNING";
        case SPI_OK_UPDATE_RETURNING:
            return "SPI_OK_UPDATE_RETURNING";
        case SPI_OK_REWRITTEN:
            return "SPI_OK_REWRITTEN";
        default:
            break;
    }
    /* Unrecognized code ... return something useful ... */
    errno_t sret = sprintf_s(buf, BUFLEN, "Unrecognized SPI code %d", code);
    securec_check_ss(sret, "\0", "\0");

    return buf;
}

/*
 * SPI_plan_get_plan_sources --- get a SPI plan's underlying list of
 * CachedPlanSources.
 *
 * This is exported so that pl/pgsql can use it (this beats letting pl/pgsql
 * look directly into the SPIPlan for itself).  It's not documented in
 * spi.sgml because we'd just as soon not have too many places using this.
 */
List *SPI_plan_get_plan_sources(SPIPlanPtr plan)
{
    Assert(plan->magic == _SPI_PLAN_MAGIC);
    return plan->plancache_list;
}

/*
 * SPI_plan_get_cached_plan --- get a SPI plan's generic CachedPlan,
 * if the SPI plan contains exactly one CachedPlanSource.  If not,
 * return NULL.  Caller is responsible for doing ReleaseCachedPlan().
 *
 * This is exported so that pl/pgsql can use it (this beats letting pl/pgsql
 * look directly into the SPIPlan for itself).  It's not documented in
 * spi.sgml because we'd just as soon not have too many places using this.
 */
CachedPlan *SPI_plan_get_cached_plan(SPIPlanPtr plan)
{
    CachedPlanSource *plan_source = NULL;
    CachedPlan *cplan = NULL;
    ErrorContextCallback spi_err_context;

    Assert(plan->magic == _SPI_PLAN_MAGIC);

    /* Can't support one-shot plans here */
    if (plan->oneshot) {
        return NULL;
    }

    /* Must have exactly one CachedPlanSource */
    if (list_length(plan->plancache_list) != 1) {
        return NULL;
    }
    plan_source = (CachedPlanSource *)linitial(plan->plancache_list);

    /* Setup error traceback support for ereport() */
    spi_err_context.callback = _SPI_error_callback;
    spi_err_context.arg = (void *)plan_source->query_string;
    spi_err_context.previous = t_thrd.log_cxt.error_context_stack;
    t_thrd.log_cxt.error_context_stack = &spi_err_context;

    /* Get the generic plan for the query */
    cplan = GetCachedPlan(plan_source, NULL, plan->saved);
    Assert(cplan == plan_source->gplan);

    /* Pop the error context stack */
    t_thrd.log_cxt.error_context_stack = spi_err_context.previous;

    return cplan;
}

/* =================== private functions =================== */
static void spi_check_connid()
{
    /*
     * When called by Executor u_sess->SPI_cxt._curid expected to be equal to
     * u_sess->SPI_cxt._connected
     */
    if (u_sess->SPI_cxt._curid != u_sess->SPI_cxt._connected || u_sess->SPI_cxt._connected < 0) {
        ereport(ERROR, (errcode(ERRORCODE_SPI_IMPROPER_CALL),
            errmsg("SPI stack level is corrupted when checking SPI id, current level: %d, connected level: %d",
            u_sess->SPI_cxt._curid, u_sess->SPI_cxt._connected)));
    }

    if (u_sess->SPI_cxt._current != &(u_sess->SPI_cxt._stack[u_sess->SPI_cxt._curid])) {
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("SPI stack is corrupted when checking SPI id.")));
    }
}

/*
 * spi_dest_startup
 * 		Initialize to receive tuples from Executor into SPITupleTable
 * 		of current SPI procedure
 */
void spi_dest_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
    SPITupleTable *tuptable = NULL;
    MemoryContext old_ctx;
    MemoryContext tup_tab_cxt;

    spi_check_connid();

    if (u_sess->SPI_cxt._current->tuptable != NULL) {
        ereport(ERROR,
            (errcode(ERRORCODE_SPI_IMPROPER_CALL), errmsg("SPI tupletable is not cleaned when initializing SPI.")));
    }

    old_ctx = _SPI_procmem(); /* switch to procedure memory context */

    tup_tab_cxt = AllocSetContextCreate(CurrentMemoryContext, "SPI TupTable", ALLOCSET_DEFAULT_MINSIZE,
        ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);
    (void)MemoryContextSwitchTo(tup_tab_cxt);

    u_sess->SPI_cxt._current->tuptable = tuptable = (SPITupleTable *)palloc(sizeof(SPITupleTable));
    tuptable->tuptabcxt = tup_tab_cxt;

    if (DestSPI == self->mydest) {
        tuptable->alloced = tuptable->free = 128;
    } else {
        tuptable->alloced = tuptable->free = DEFAULT_SAMPLE_ROWCNT;
    }

    tuptable->vals = (HeapTuple *)palloc(tuptable->alloced * sizeof(HeapTuple));
    tuptable->tupdesc = CreateTupleDescCopy(typeinfo);

    (void)MemoryContextSwitchTo(old_ctx);
}

/*
 * spi_printtup
 * 		store tuple retrieved by Executor into SPITupleTable
 * 		of current SPI procedure
 */
void spi_printtup(TupleTableSlot *slot, DestReceiver *self)
{
    SPITupleTable *tuptable = NULL;
    MemoryContext old_ctx;
    HeapTuple tuple;

    spi_check_connid();
    tuptable = u_sess->SPI_cxt._current->tuptable;
    if (tuptable == NULL) {
        ereport(ERROR, (errcode(ERRORCODE_SPI_IMPROPER_CALL), errmsg("tuple is NULL when store to SPI tupletable.")));
    }

    old_ctx = MemoryContextSwitchTo(tuptable->tuptabcxt);

    if (tuptable->free == 0) {
        /* Double the size of the pointer array */
        tuptable->free = tuptable->alloced;
        tuptable->alloced += tuptable->free;
        tuptable->vals = (HeapTuple *)repalloc(tuptable->vals, tuptable->alloced * sizeof(HeapTuple));
    }

    tuple = ExecCopySlotTuple(slot);
    /* check ExecCopySlotTuple result */
    if (tuple == NULL) {
        ereport(WARNING, (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
            (errmsg("slot tuple copy failed, unexpexted return value. Maybe the slot tuple is invalid or tuple "
            "data is null "))));
    }

    tuptable->vals[tuptable->alloced - tuptable->free] = tuple;

    (tuptable->free)--;

    (void)MemoryContextSwitchTo(old_ctx);
}

/*
 * Static functions
 */
/*
 * Parse and analyze a querystring.
 *
 * At entry, plan->argtypes and plan->nargs (or alternatively plan->parserSetup
 * and plan->parserSetupArg) must be valid, as must plan->cursor_options.
 *
 * Results are stored into *plan (specifically, plan->plancache_list).
 * Note that the result data is all in CurrentMemoryContext or child contexts
 * thereof; in practice this means it is in the SPI executor context, and
 * what we are creating is a "temporary" SPIPlan.  Cruft generated during
 * parsing is also left in CurrentMemoryContext.
 */
static void _SPI_prepare_plan(const char *src, SPIPlanPtr plan)
{
#ifdef PGXC
    _SPI_pgxc_prepare_plan(src, NULL, plan);
}

/*
 * _SPI_pgxc_prepare_plan: Optionally accepts a parsetree which allows it to
 * bypass the parse phase, and directly analyse, rewrite and plan. Meant to be
 * called for internally executed execute-direct statements that are
 * transparent to the user.
 */
static void _SPI_pgxc_prepare_plan(const char *src, List *src_parsetree, SPIPlanPtr plan)
{
#endif
    List *raw_parsetree_list = NIL;
    List *plancache_list = NIL;
    ListCell *list_item = NULL;
    ErrorContextCallback spi_err_context;

    /*
     * Setup error traceback support for ereport()
     */
    spi_err_context.callback = _SPI_error_callback;
    spi_err_context.arg = (void *)src;
    spi_err_context.previous = t_thrd.log_cxt.error_context_stack;
    t_thrd.log_cxt.error_context_stack = &spi_err_context;

    /*
     * Parse the request string into a list of raw parse trees.
     */
#ifdef PGXC
    /* Parse it only if there isn't an already parsed tree passed */
    if (src_parsetree != NIL)
        raw_parsetree_list = src_parsetree;
    else
#endif
        raw_parsetree_list = pg_parse_query(src);
    /*
     * Do parse analysis and rule rewrite for each raw parsetree, storing the
     * results into unsaved plancache entries.
     */
    plancache_list = NIL;

    foreach (list_item, raw_parsetree_list) {
        Node *parsetree = (Node *)lfirst(list_item);
        List *stmt_list = NIL;
        CachedPlanSource *plansource = NULL;

        /*
         * Create the CachedPlanSource before we do parse analysis, since it
         * needs to see the unmodified raw parse tree.
         */
        plansource = CreateCachedPlan(parsetree, src,
#ifdef PGXC
            NULL,
#endif
            CreateCommandTag(parsetree));

        /*
         * Parameter datatypes are driven by parserSetup hook if provided,
         * otherwise we use the fixed parameter list.
         */
        if (plan->parserSetup != NULL) {
            Assert(plan->nargs == 0);
            stmt_list = pg_analyze_and_rewrite_params(parsetree, src, plan->parserSetup, plan->parserSetupArg);
        } else {
            stmt_list = pg_analyze_and_rewrite(parsetree, src, plan->argtypes, plan->nargs);
        }

        /* Finish filling in the CachedPlanSource */
        CompleteCachedPlan(plansource, stmt_list, NULL, plan->argtypes, plan->nargs, plan->parserSetup,
            plan->parserSetupArg, plan->cursor_options, false, /* not fixed result */
            "");

        plancache_list = lappend(plancache_list, plansource);
    }

    plan->plancache_list = plancache_list;
    plan->oneshot = false;

    /*
     * Pop the error context stack
     */
    t_thrd.log_cxt.error_context_stack = spi_err_context.previous;
}

/*
 * Parse, but don't analyze, a querystring.
 *
 * This is a stripped-down version of _SPI_prepare_plan that only does the
 * initial raw parsing.  It creates "one shot" CachedPlanSources
 * that still require parse analysis before execution is possible.
 *
 * The advantage of using the "one shot" form of CachedPlanSource is that
 * we eliminate data copying and invalidation overhead.  Postponing parse
 * analysis also prevents issues if some of the raw parsetrees are DDL
 * commands that affect validity of later parsetrees.  Both of these
 * attributes are good things for SPI_execute() and similar cases.
 *
 * Results are stored into *plan (specifically, plan->plancache_list).
 * Note that the result data is all in CurrentMemoryContext or child contexts
 * thereof; in practice this means it is in the SPI executor context, and
 * what we are creating is a "temporary" SPIPlan.  Cruft generated during
 * parsing is also left in CurrentMemoryContext.
 */
static void _SPI_prepare_oneshot_plan(const char *src, SPIPlanPtr plan)
{
    List *raw_parsetree_list = NIL;
    List *plancache_list = NIL;
    ListCell *list_item = NULL;
    ErrorContextCallback spi_err_context;
    List *query_string_locationlist = NIL;
    int stmt_num = 0;
    /*
     * Setup error traceback support for ereport()
     */
    spi_err_context.callback = _SPI_error_callback;
    spi_err_context.arg = (void *)src;
    spi_err_context.previous = t_thrd.log_cxt.error_context_stack;
    t_thrd.log_cxt.error_context_stack = &spi_err_context;

    /*
     * Parse the request string into a list of raw parse trees.
     */
    raw_parsetree_list = pg_parse_query(src, &query_string_locationlist);

    /*
     * Construct plancache entries, but don't do parse analysis yet.
     */
    plancache_list = NIL;
    char **query_string_single = NULL;

    foreach (list_item, raw_parsetree_list) {
        Node *parsetree = (Node *)lfirst(list_item);
        CachedPlanSource *plansource = NULL;
        if (IS_PGXC_COORDINATOR && PointerIsValid(query_string_locationlist) &&
            list_length(query_string_locationlist) > 1) {
            query_string_single = get_next_snippet(query_string_single, src, query_string_locationlist, &stmt_num);
            plansource =
                CreateOneShotCachedPlan(parsetree, query_string_single[stmt_num - 1], CreateCommandTag(parsetree));
        } else {
            plansource = CreateOneShotCachedPlan(parsetree, src, CreateCommandTag(parsetree));
        }

        plancache_list = lappend(plancache_list, plansource);
    }

    plan->plancache_list = plancache_list;
    plan->oneshot = true;

    /*
     * Pop the error context stack
     */
    t_thrd.log_cxt.error_context_stack = spi_err_context.previous;
}

/*
 * Execute the given plan with the given parameter values
 *
 * snapshot: query snapshot to use, or InvalidSnapshot for the normal
 * 		behavior of taking a new snapshot for each query.
 * crosscheck_snapshot: for RI use, all others pass InvalidSnapshot
 * read_only: TRUE for read-only execution (no CommandCounterIncrement)
 * fire_triggers: TRUE to fire AFTER triggers at end of query (normal case);
 * 		FALSE means any AFTER triggers are postponed to end of outer query
 * tcount: execution tuple-count limit, or 0 for none
 */
static int _SPI_execute_plan(SPIPlanPtr plan, ParamListInfo paramLI, Snapshot snapshot, Snapshot crosscheck_snapshot,
    bool read_only, bool fire_triggers, long tcount, bool from_lock)
{
    int my_res = 0;
    uint32 my_processed = 0;
    Oid my_lastoid = InvalidOid;
    SPITupleTable *my_tuptable = NULL;
    int res = 0;
    bool pushed_active_snap = false;
    ErrorContextCallback spi_err_context;
    CachedPlan *cplan = NULL;
    ListCell *lc1 = NULL;
    bool tmp_enable_light_proxy = u_sess->attr.attr_sql.enable_light_proxy;

    /* not allow Light CN */
    u_sess->attr.attr_sql.enable_light_proxy = false;

    /*
     * Setup error traceback support for ereport()
     */
    spi_err_context.callback = _SPI_error_callback;
    spi_err_context.arg = NULL; /* we'll fill this below */
    spi_err_context.previous = t_thrd.log_cxt.error_context_stack;
    t_thrd.log_cxt.error_context_stack = &spi_err_context;

    /*
     * We support four distinct snapshot management behaviors:
     *
     * snapshot != InvalidSnapshot, read_only = true: use exactly the given
     * snapshot.
     *
     * snapshot != InvalidSnapshot, read_only = false: use the given snapshot,
     * modified by advancing its command ID before each querytree.
     *
     * snapshot == InvalidSnapshot, read_only = true: use the entry-time
     * ActiveSnapshot, if any (if there isn't one, we run with no snapshot).
     *
     * snapshot == InvalidSnapshot, read_only = false: take a full new
     * snapshot for each user command, and advance its command ID before each
     * querytree within the command.
     *
     * In the first two cases, we can just push the snap onto the stack once
     * for the whole plan list.
     */
    if (snapshot != InvalidSnapshot) {
        if (read_only) {
            PushActiveSnapshot(snapshot);
            pushed_active_snap = true;
        } else {
            /* Make sure we have a private copy of the snapshot to modify */
            PushCopiedSnapshot(snapshot);
            pushed_active_snap = true;
        }
    }

    foreach (lc1, plan->plancache_list) {
        CachedPlanSource *plansource = (CachedPlanSource *)lfirst(lc1);
        List *stmt_list = NIL;
        ListCell *lc2 = NULL;

        spi_err_context.arg = (void *)plansource->query_string;

        /*
         * If this is a one-shot plan, we still need to do parse analysis.
         */
        if (plan->oneshot) {
            Node *parsetree = plansource->raw_parse_tree;
            const char *src = plansource->query_string;
            List *statement_list = NIL;

            /*
             * Parameter datatypes are driven by parserSetup hook if provided,
             * otherwise we use the fixed parameter list.
             */
            if (plan->parserSetup != NULL) {
                Assert(plan->nargs == 0);
                statement_list = pg_analyze_and_rewrite_params(parsetree, src, plan->parserSetup, plan->parserSetupArg);
            } else {
                statement_list = pg_analyze_and_rewrite(parsetree, src, plan->argtypes, plan->nargs);
            }

            /* Finish filling in the CachedPlanSource */
            CompleteCachedPlan(plansource, statement_list, NULL, plan->argtypes, plan->nargs, plan->parserSetup,
                plan->parserSetupArg, plan->cursor_options, false, /* not fixed result */
                "");
        }

        /*
         * Replan if needed, and increment plan refcount.  If it's a saved
         * plan, the refcount must be backed by the CurrentResourceOwner.
         */
        cplan = GetCachedPlan(plansource, paramLI, plan->saved);
        stmt_list = cplan->stmt_list;

        /*
         * In the default non-read-only case, get a new snapshot, replacing
         * any that we pushed in a previous cycle.
         */
        if (snapshot == InvalidSnapshot && !read_only) {
            if (pushed_active_snap) {
                PopActiveSnapshot();
            }
            PushActiveSnapshot(GetTransactionSnapshot());
            pushed_active_snap = true;
        }

        foreach (lc2, stmt_list) {
            Node *stmt = (Node *)lfirst(lc2);
            bool canSetTag = false;
            DestReceiver *dest = NULL;

            u_sess->SPI_cxt._current->processed = 0;
            u_sess->SPI_cxt._current->lastoid = InvalidOid;
            u_sess->SPI_cxt._current->tuptable = NULL;

            if (IsA(stmt, PlannedStmt)) {
                canSetTag = ((PlannedStmt *)stmt)->canSetTag;
            } else {
                /* utilities are canSetTag if only thing in list */
                canSetTag = (list_length(stmt_list) == 1);

                if (IsA(stmt, CopyStmt)) {
                    CopyStmt *cstmt = (CopyStmt *)stmt;

                    if (cstmt->filename == NULL) {
                        my_res = SPI_ERROR_COPY;
                        goto fail;
                    }
                } else if (IsA(stmt, TransactionStmt)) {
                    my_res = SPI_ERROR_TRANSACTION;
                    goto fail;
                }
            }

            if (read_only && !CommandIsReadOnly(stmt)) {
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    /* translator: %s is a SQL statement name */
                    errmsg("%s is not allowed in a non-volatile function", CreateCommandTag(stmt))));
            }

            /*
             * If not read-only mode, advance the command counter before each
             * command and update the snapshot.
             */
            if (!read_only) {
                CommandCounterIncrement();
                UpdateActiveSnapshotCommandId();
            }

            dest = CreateDestReceiver(canSetTag ? u_sess->SPI_cxt._current->dest : DestNone);

            if (IsA(stmt, PlannedStmt) && ((PlannedStmt *)stmt)->utilityStmt == NULL) {
                QueryDesc *qdesc = NULL;
                Snapshot snap;

                if (ActiveSnapshotSet()) {
                    snap = GetActiveSnapshot();
                } else {
                    snap = InvalidSnapshot;
                }

                qdesc = CreateQueryDesc((PlannedStmt *)stmt, plansource->query_string, snap, crosscheck_snapshot, dest,
                    paramLI, 0);
                res = _SPI_pquery(qdesc, fire_triggers, canSetTag ? tcount : 0, from_lock);
                FreeQueryDesc(qdesc);
            } else {
                char completionTag[COMPLETION_TAG_BUFSIZE];

                /*
                 * Reset schema name for analyze in stored procedure.
                 * When analyze has error, there is no time for schema name to be reseted.
                 * It will be kept in the plan for stored procedure and the result
                 * is uncertain.
                 */
                if (IsA(stmt, VacuumStmt)) {
                    ClearVacuumStmt((VacuumStmt *)stmt);
                }
                if (IsA(stmt, CreateSeqStmt)) {
                    ClearCreateSeqStmtUUID((CreateSeqStmt *)stmt);
                }
                if (IsA(stmt, CreateStmt)) {
                    ClearCreateStmtUUIDS((CreateStmt *)stmt);
                }

                if (IsA(stmt, CreateRoleStmt) || IsA(stmt, AlterRoleStmt)) {
                    stmt = (Node *)copyObject(stmt);
                }

                ProcessUtility(stmt, plansource->query_string, paramLI, false, /* not top level */
                    dest,
#ifdef PGXC
                    false,
#endif /* PGXC */
                    completionTag);

                /* Update "processed" if stmt returned tuples */
                if (u_sess->SPI_cxt._current->tuptable) {
                    u_sess->SPI_cxt._current->processed =
                        u_sess->SPI_cxt._current->tuptable->alloced - u_sess->SPI_cxt._current->tuptable->free;
                }

                /*
                 * CREATE TABLE AS is a messy special case for historical
                 * reasons.  We must set u_sess->SPI_cxt._current->processed even though
                 * the tuples weren't returned to the caller, and we must
                 * return a special result code if the statement was spelled
                 * SELECT INTO.
                 */
                if (IsA(stmt, CreateTableAsStmt)) {
                    Assert(strncmp(completionTag, "SELECT ", 7) == 0);
                    u_sess->SPI_cxt._current->processed = strtoul(completionTag + 7, NULL, 10);
                    if (((CreateTableAsStmt *)stmt)->is_select_into) {
                        res = SPI_OK_SELINTO;
                    } else {
                        res = SPI_OK_UTILITY;
                    }
                } else {
                    res = SPI_OK_UTILITY;
                }

                if (IsA(stmt, CreateRoleStmt) || IsA(stmt, AlterRoleStmt)) {
                    pfree_ext(stmt);
                }
            }

            /*
             * The last canSetTag query sets the status values returned to the
             * caller.	Be careful to free any tuptables not returned, to
             * avoid intratransaction memory leak.
             */
            if (canSetTag) {
                my_processed = u_sess->SPI_cxt._current->processed;
                my_lastoid = u_sess->SPI_cxt._current->lastoid;
                SPI_freetuptable(my_tuptable);
                my_tuptable = u_sess->SPI_cxt._current->tuptable;
                my_res = res;
            } else {
                SPI_freetuptable(u_sess->SPI_cxt._current->tuptable);
                u_sess->SPI_cxt._current->tuptable = NULL;
            }
            /* we know that the receiver doesn't need a destroy call */
            if (res < 0) {
                my_res = res;
                goto fail;
            }
        }

        /* Done with this plan, so release refcount */
        ReleaseCachedPlan(cplan, plan->saved);
        cplan = NULL;

        /*
         * If not read-only mode, advance the command counter after the last
         * command.  This ensures that its effects are visible, in case it was
         * DDL that would affect the next CachedPlanSource.
         */
        if (!read_only) {
            CommandCounterIncrement();
        }
    }

fail:

    /* Pop the snapshot off the stack if we pushed one */
    if (pushed_active_snap) {
        PopActiveSnapshot();
    }

    /* We no longer need the cached plan refcount, if any */
    if (cplan != NULL) {
        ReleaseCachedPlan(cplan, plan->saved);
    }

    /*
     * When plan->plancache_list > 1 means it's a multi query and  have been malloc memory
     * through get_next_snippet, so we need free them here.
     */
    if (IS_PGXC_COORDINATOR && PointerIsValid(plan->plancache_list) && list_length(plan->plancache_list) > 1) {
        ListCell *list_item = NULL;

        foreach (list_item, plan->plancache_list) {
            CachedPlanSource *PlanSource = (CachedPlanSource *)lfirst(list_item);
            if (PlanSource->query_string != NULL) {
                pfree_ext((PlanSource->query_string));
                PlanSource->query_string = NULL;
            }
        }
    }

    /*
     * Pop the error context stack
     */
    t_thrd.log_cxt.error_context_stack = spi_err_context.previous;

    /* Save results for caller */
    SPI_processed = my_processed;
    u_sess->SPI_cxt.lastoid = my_lastoid;
    SPI_tuptable = my_tuptable;

    /* tuptable now is caller's responsibility, not SPI's */
    u_sess->SPI_cxt._current->tuptable = NULL;

    /*
     * If none of the queries had canSetTag, return SPI_OK_REWRITTEN. Prior to
     * 8.4, we used return the last query's result code, but not its auxiliary
     * results, but that's confusing.
     */
    if (my_res == 0) {
        my_res = SPI_OK_REWRITTEN;
    }

    u_sess->attr.attr_sql.enable_light_proxy = tmp_enable_light_proxy;

    return my_res;
}

/*
 * Convert arrays of query parameters to form wanted by planner and executor
 */
static ParamListInfo _SPI_convert_params(int nargs, Oid *argtypes, Datum *Values, const char *Nulls,
    Cursor_Data *cursor_data)
{
    ParamListInfo param_list_info;

    if (nargs > 0) {
        int i;

        param_list_info = (ParamListInfo)palloc(offsetof(ParamListInfoData, params) + nargs * sizeof(ParamExternData));
        /* we have static list of params, so no hooks needed */
        param_list_info->paramFetch = NULL;
        param_list_info->paramFetchArg = NULL;
        param_list_info->parserSetup = NULL;
        param_list_info->parserSetupArg = NULL;
        param_list_info->params_need_process = false;
        param_list_info->numParams = nargs;

        for (i = 0; i < nargs; i++) {
            ParamExternData *prm = &param_list_info->params[i];

            prm->value = Values[i];
            prm->isnull = (Nulls && Nulls[i] == 'n');
            prm->pflags = PARAM_FLAG_CONST;
            prm->ptype = argtypes[i];
            if (cursor_data != NULL) {
                CopyCursorInfoData(&prm->cursor_data, &cursor_data[i]);
            }
        }
    } else {
        param_list_info = NULL;
    }
    return param_list_info;
}

static int _SPI_pquery(QueryDesc *queryDesc, bool fire_triggers, long tcount, bool from_lock)
{
    int operation = queryDesc->operation;
    int eflags;
    int res;

    switch (operation) {
        case CMD_SELECT:
            Assert(queryDesc->plannedstmt->utilityStmt == NULL);
            if (queryDesc->dest->mydest != DestSPI) {
                /* Don't return SPI_OK_SELECT if we're discarding result */
                res = SPI_OK_UTILITY;
            } else
                res = SPI_OK_SELECT;
            break;
        case CMD_INSERT:
            if (queryDesc->plannedstmt->hasReturning)
                res = SPI_OK_INSERT_RETURNING;
            else
                res = SPI_OK_INSERT;
            break;
        case CMD_DELETE:
            if (queryDesc->plannedstmt->hasReturning)
                res = SPI_OK_DELETE_RETURNING;
            else
                res = SPI_OK_DELETE;
            break;
        case CMD_UPDATE:
            if (queryDesc->plannedstmt->hasReturning)
                res = SPI_OK_UPDATE_RETURNING;
            else
                res = SPI_OK_UPDATE;
            break;
        case CMD_MERGE:
            res = SPI_OK_MERGE;
            break;
        default:
            return SPI_ERROR_OPUNKNOWN;
    }

#ifdef SPI_EXECUTOR_STATS
    if (ShowExecutorStats)
        ResetUsage();
#endif

    /* Select execution options */
    if (fire_triggers) {
        eflags = 0; /* default run-to-completion flags */
    } else {
        eflags = EXEC_FLAG_SKIP_TRIGGERS;
    }

    ExecutorStart(queryDesc, eflags);

    bool forced_control = !from_lock && IS_PGXC_COORDINATOR &&
        (t_thrd.wlm_cxt.parctl_state.simple == 1 || u_sess->wlm_cxt->is_active_statements_reset) &&
        ENABLE_WORKLOAD_CONTROL;
    Qid stroedproc_qid = { 0, 0, 0 };
    unsigned char stroedproc_parctl_state_except = 0;
    WLMStatusTag stroedproc_g_collectInfo_status = WLM_STATUS_RESERVE;
    bool stroedproc_is_active_statements_reset = false;
    errno_t rc;
    if (forced_control) {
        if (!u_sess->wlm_cxt->is_active_statements_reset && !u_sess->attr.attr_resource.enable_transaction_parctl) {
            u_sess->wlm_cxt->stroedproc_rp_reserve = t_thrd.wlm_cxt.parctl_state.rp_reserve;
            u_sess->wlm_cxt->stroedproc_rp_release = t_thrd.wlm_cxt.parctl_state.rp_release;
            u_sess->wlm_cxt->stroedproc_release = t_thrd.wlm_cxt.parctl_state.release;
        }

        /* Retain the parameters of the main statement */
        if (!IsQidInvalid(&u_sess->wlm_cxt->wlm_params.qid)) {
            rc = memcpy_s(&stroedproc_qid, sizeof(Qid), &u_sess->wlm_cxt->wlm_params.qid, sizeof(Qid));
            securec_check(rc, "\0", "\0");
        }
        stroedproc_parctl_state_except = t_thrd.wlm_cxt.parctl_state.except;
        stroedproc_g_collectInfo_status = t_thrd.wlm_cxt.collect_info->status;
        stroedproc_is_active_statements_reset = u_sess->wlm_cxt->is_active_statements_reset;

        t_thrd.wlm_cxt.parctl_state.subquery = 1;
        WLMInitQueryPlan(queryDesc);
        dywlm_client_manager(queryDesc);
    }

    ExecutorRun(queryDesc, ForwardScanDirection, tcount);

    if (forced_control) {
        t_thrd.wlm_cxt.parctl_state.except = 0;
        if (g_instance.wlm_cxt->dynamic_workload_inited && (t_thrd.wlm_cxt.parctl_state.simple == 0)) {
            dywlm_client_release(&t_thrd.wlm_cxt.parctl_state);
        } else {
            // only release resource pool count
            if (IS_PGXC_COORDINATOR && !IsConnFromCoord() &&
                (u_sess->wlm_cxt->parctl_state_exit || IsQueuedSubquery())) {
                WLMReleaseGroupActiveStatement();
            }
        }

        WLMSetCollectInfoStatus(WLM_STATUS_FINISHED);
        t_thrd.wlm_cxt.parctl_state.subquery = 0;
        t_thrd.wlm_cxt.parctl_state.except = stroedproc_parctl_state_except;
        t_thrd.wlm_cxt.collect_info->status = stroedproc_g_collectInfo_status;
        u_sess->wlm_cxt->is_active_statements_reset = stroedproc_is_active_statements_reset;
        if (!IsQidInvalid(&stroedproc_qid)) {
            rc = memcpy_s(&u_sess->wlm_cxt->wlm_params.qid, sizeof(Qid), &stroedproc_qid, sizeof(Qid));
            securec_check(rc, "\0", "\0");
        }

        /* restore state condition if guc para is off since it contains unreleased count */
        if (!u_sess->attr.attr_resource.enable_transaction_parctl && (u_sess->wlm_cxt->reserved_in_active_statements ||
            u_sess->wlm_cxt->reserved_in_group_statements || u_sess->wlm_cxt->reserved_in_group_statements_simple)) {
            t_thrd.wlm_cxt.parctl_state.rp_reserve = u_sess->wlm_cxt->stroedproc_rp_reserve;
            t_thrd.wlm_cxt.parctl_state.rp_release = u_sess->wlm_cxt->stroedproc_rp_release;
            t_thrd.wlm_cxt.parctl_state.release = u_sess->wlm_cxt->stroedproc_release;
        }
    }

    u_sess->SPI_cxt._current->processed = queryDesc->estate->es_processed;
    u_sess->SPI_cxt._current->lastoid = queryDesc->estate->es_lastoid;

    if ((res == SPI_OK_SELECT || queryDesc->plannedstmt->hasReturning) && queryDesc->dest->mydest == DestSPI) {
        if (_SPI_checktuples()) {
            ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                errmsg("consistency check on SPI tuple count failed when execute plan, %s",
                u_sess->SPI_cxt._current->tuptable == NULL ? "tupletable is NULL." :
                                                             "processed tuples is not matched.")));
        }
    }

    ExecutorFinish(queryDesc);
    ExecutorEnd(queryDesc);
    /* FreeQueryDesc is done by the caller */
#ifdef SPI_EXECUTOR_STATS
    if (ShowExecutorStats)
        ShowUsage("SPI EXECUTOR STATS");
#endif

    return res;
}

/*
 * _SPI_error_callback
 *
 * Add context information when a query invoked via SPI fails
 */
static void _SPI_error_callback(void *arg)
{
    /* We can't expose query when under analyzing with tablesample. */
    if (u_sess->analyze_cxt.is_under_analyze) {
        return;
    }

    const char *query = (const char *)arg;
    int syntax_err_pos;

    if (query == NULL) { /* in case arg wasn't set yet */
        return;
    }

    char *mask_string = maskPassword(query);
    if (mask_string == NULL) {
        mask_string = (char *)query;
    }

    /*
     * If there is a syntax error position, convert to internal syntax error;
     * otherwise treat the query as an item of context stack
     */
    syntax_err_pos = geterrposition();
    if (syntax_err_pos > 0) {
        errposition(0);
        internalerrposition(syntax_err_pos);
        internalerrquery(mask_string);
    } else {
        errcontext("SQL statement \"%s\"", mask_string);
    }

    if (mask_string != query) {
        pfree(mask_string);
    }
}

/*
 * _SPI_cursor_operation
 *
 * 	Do a FETCH or MOVE in a cursor
 */
static void _SPI_cursor_operation(Portal portal, FetchDirection direction, long count, DestReceiver *dest)
{
    long n_fetched;

    /* Check that the portal is valid */
    if (!PortalIsValid(portal)) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_CURSOR_STATE), errmsg("invalid portal in SPI cursor operation")));
    }

    /* Push the SPI stack */
    if (_SPI_begin_call(true) < 0) {
        ereport(ERROR, (errcode(ERRCODE_SPI_CONNECTION_FAILURE),
            errmsg("SPI stack is corrupted when perform cursor operation, current level: %d, connected level: %d",
            u_sess->SPI_cxt._curid, u_sess->SPI_cxt._connected)));
    }

    /* Reset the SPI result (note we deliberately don't touch lastoid) */
    SPI_processed = 0;
    SPI_tuptable = NULL;
    u_sess->SPI_cxt._current->processed = 0;
    u_sess->SPI_cxt._current->tuptable = NULL;

    /* Run the cursor */
    n_fetched = PortalRunFetch(portal, direction, count, dest);

    /*
     * Think not to combine this store with the preceding function call. If
     * the portal contains calls to functions that use SPI, then SPI_stack is
     * likely to move around while the portal runs.  When control returns,
     * u_sess->SPI_cxt._current will point to the correct stack entry... but the pointer
     * may be different than it was beforehand. So we must be sure to re-fetch
     * the pointer after the function call completes.
     */
    u_sess->SPI_cxt._current->processed = n_fetched;

    if (dest->mydest == DestSPI && _SPI_checktuples()) {
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("consistency check on SPI tuple count failed, %s",
            u_sess->SPI_cxt._current->tuptable == NULL ? "tupletable is NULL." : "processed tuples is not matched.")));
    }

    /* Put the result into place for access by caller */
    SPI_processed = u_sess->SPI_cxt._current->processed;
    SPI_tuptable = u_sess->SPI_cxt._current->tuptable;

    /* tuptable now is caller's responsibility, not SPI's */
    u_sess->SPI_cxt._current->tuptable = NULL;

    /* Pop the SPI stack */
    _SPI_end_call(true);
}

static MemoryContext _SPI_execmem(void)
{
    return MemoryContextSwitchTo(u_sess->SPI_cxt._current->execCxt);
}

static MemoryContext _SPI_procmem(void)
{
    return MemoryContextSwitchTo(u_sess->SPI_cxt._current->procCxt);
}

/*
 * _SPI_begin_call: begin a SPI operation within a connected procedure
 */
static int _SPI_begin_call(bool execmem)
{
    if (u_sess->SPI_cxt._curid + 1 != u_sess->SPI_cxt._connected)
        return SPI_ERROR_UNCONNECTED;
    u_sess->SPI_cxt._curid++;
    if (u_sess->SPI_cxt._current != &(u_sess->SPI_cxt._stack[u_sess->SPI_cxt._curid])) {
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("SPI stack corrupted when begin SPI operation.")));
    }

    if (execmem) { /* switch to the Executor memory context */
        _SPI_execmem();
    }

    return 0;
}

/*
 * _SPI_end_call: end a SPI operation within a connected procedure
 *
 * Note: this currently has no failure return cases, so callers don't check
 */
int _SPI_end_call(bool procmem)
{
    /*
     * We're returning to procedure where u_sess->SPI_cxt._curid == u_sess->SPI_cxt._connected - 1
     */
    u_sess->SPI_cxt._curid--;

    if (procmem) {
        /* switch to the procedure memory context */
        _SPI_procmem();
        /* and free Executor memory */
        MemoryContextResetAndDeleteChildren(u_sess->SPI_cxt._current->execCxt);
    }

    return 0;
}

static bool _SPI_checktuples(void)
{
    uint64 processed = u_sess->SPI_cxt._current->processed;
    SPITupleTable *tuptable = u_sess->SPI_cxt._current->tuptable;
    bool failed = false;

    if (tuptable == NULL) { /* spi_dest_startup was not called */
        failed = true;
    }

    else if (processed != (tuptable->alloced - tuptable->free)) {
        failed = true;
    }

    return failed;
}

/*
 * Convert a "temporary" SPIPlan into an "unsaved" plan.
 *
 * The passed _SPI_plan struct is on the stack, and all its subsidiary data
 * is in or under the current SPI executor context.  Copy the plan into the
 * SPI procedure context so it will survive _SPI_end_call().  To minimize
 * data copying, this destructively modifies the input plan, by taking the
 * plancache entries away from it and reparenting them to the new SPIPlan.
 */
static SPIPlanPtr _SPI_make_plan_non_temp(SPIPlanPtr plan)
{
    SPIPlanPtr newplan = NULL;
    MemoryContext parentcxt = u_sess->SPI_cxt._current->procCxt;
    MemoryContext plancxt = NULL;
    MemoryContext oldcxt = NULL;
    ListCell *lc = NULL;

    /* Assert the input is a temporary SPIPlan */
    Assert(plan->magic == _SPI_PLAN_MAGIC);
    Assert(plan->plancxt == NULL);
    /* One-shot plans can't be saved */
    Assert(!plan->oneshot);

    /*
     * Create a memory context for the plan, underneath the procedure context.
     * We don't expect the plan to be very large, so use smaller-than-default
     * alloc parameters.
     */
    plancxt = AllocSetContextCreate(parentcxt, "SPI Plan", ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE,
        ALLOCSET_SMALL_MAXSIZE);
    oldcxt = MemoryContextSwitchTo(plancxt);
    /* Copy the SPI_plan struct and subsidiary data into the new context */
    newplan = (SPIPlanPtr)palloc(sizeof(_SPI_plan));
    CopySPI_Plan(newplan, plan, plancxt);

    /*
     * Reparent all the CachedPlanSources into the procedure context.  In
     * theory this could fail partway through due to the pallocs, but we don't
     * care too much since both the procedure context and the executor context
     * would go away on error.
     */
    foreach (lc, plan->plancache_list) {
        CachedPlanSource *plansource = (CachedPlanSource *)lfirst(lc);

        CachedPlanSetParentContext(plansource, parentcxt);

        /* Build new list, with list cells in plancxt */
        newplan->plancache_list = lappend(newplan->plancache_list, plansource);
    }

    (void)MemoryContextSwitchTo(oldcxt);

    /* For safety, unlink the CachedPlanSources from the temporary plan */
    plan->plancache_list = NIL;

    return newplan;
}

void CopySPI_Plan(SPIPlanPtr newplan, SPIPlanPtr plan, MemoryContext plancxt)
{
    newplan->magic = _SPI_PLAN_MAGIC;
    newplan->saved = false;
    newplan->oneshot = false;
    newplan->plancache_list = NIL;
    newplan->plancxt = plancxt;
    newplan->cursor_options = plan->cursor_options;
    newplan->nargs = plan->nargs;
    if (plan->nargs > 0) {
        newplan->argtypes = (Oid *)palloc(plan->nargs * sizeof(Oid));
        errno_t rc = memcpy_s(newplan->argtypes, plan->nargs * sizeof(Oid), plan->argtypes, plan->nargs * sizeof(Oid));
        securec_check(rc, "\0", "\0");
    } else {
        newplan->argtypes = NULL;
    }
    newplan->parserSetup = plan->parserSetup;
    newplan->parserSetupArg = plan->parserSetupArg;
}

/*
 * Make a "saved" copy of the given plan.
 */
static SPIPlanPtr _SPI_save_plan(SPIPlanPtr plan)
{
    SPIPlanPtr newplan = NULL;
    MemoryContext plancxt = NULL;
    MemoryContext oldcxt = NULL;
    ListCell *lc = NULL;

    /* One-shot plans can't be saved */
    Assert(!plan->oneshot);

    /*
     * Create a memory context for the plan.  We don't expect the plan to be
     * very large, so use smaller-than-default alloc parameters.  It's a
     * transient context until we finish copying everything.
     */
    plancxt = AllocSetContextCreate(CurrentMemoryContext, "SPI Plan", ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE,
        ALLOCSET_SMALL_MAXSIZE);
    oldcxt = MemoryContextSwitchTo(plancxt);

    /* Copy the SPI plan into its own context */
    newplan = (SPIPlanPtr)palloc(sizeof(_SPI_plan));
    CopySPI_Plan(newplan, plan, plancxt);

    /* Copy all the plancache entries */
    foreach (lc, plan->plancache_list) {
        CachedPlanSource *plansource = (CachedPlanSource *)lfirst(lc);
        CachedPlanSource *newsource = NULL;

        newsource = CopyCachedPlan(plansource, false);
        newplan->plancache_list = lappend(newplan->plancache_list, newsource);
    }

    (void)MemoryContextSwitchTo(oldcxt);

    /*
     * Mark it saved, reparent it under u_sess->cache_mem_cxt, and mark all the
     * component CachedPlanSources as saved.  This sequence cannot fail
     * partway through, so there's no risk of long-term memory leakage.
     */
    newplan->saved = true;
    MemoryContextSetParent(newplan->plancxt, u_sess->cache_mem_cxt);

    foreach (lc, newplan->plancache_list) {
        CachedPlanSource *plansource = (CachedPlanSource *)lfirst(lc);

        SaveCachedPlan(plansource);
    }

    return newplan;
}

/*
 * spi_dest_shutdownAnalyze: We receive 30000 samples each time and callback to process when analyze for table sample,
 * 					if the num of last batch less than 30000, we should callback to process in this.
 *
 * Parameters:
 * 	@in self: a base type for destination-specific local state.
 *
 * Returns: void
 */
static void spi_dest_shutdownAnalyze(DestReceiver *self)
{
    SPITupleTable *tuptable = NULL;

    spi_check_connid();
    tuptable = u_sess->SPI_cxt._current->tuptable;
    if (tuptable == NULL) {
        ereport(ERROR, (errcode(ERRCODE_SPI_ERROR), errmsg("SPI tupletable is NULL when shutdown SPI for analyze.")));
    }

    if ((tuptable->free < tuptable->alloced) && (u_sess->SPI_cxt._current->spiCallback)) {
        SPI_tuptable = tuptable;
        SPI_processed = tuptable->alloced - tuptable->free;
        u_sess->SPI_cxt._current->spiCallback(u_sess->SPI_cxt._current->clientData);
    }
}

/*
 * spi_dest_destroyAnalyze: pfree the state for receiver.
 *
 * Parameters:
 * 	@in self: a base type for destination-specific local state.
 *
 * Returns: void
 */
static void spi_dest_destroyAnalyze(DestReceiver *self)
{
    pfree_ext(self);
}

/*
 * spi_dest_printTupleAnalyze: Receive sample tuples each time and callback to process
 * 							when analyze for table sample.
 *
 * Parameters:
 * 	@in slot: the struct which executor stores tuples.
 * 	@in self: a base type for destination-specific local state.
 *
 * Returns: void
 */
static void spi_dest_printTupleAnalyze(TupleTableSlot *slot, DestReceiver *self)
{
    SPITupleTable *tuptable = NULL;
    MemoryContext oldcxt = NULL;

    spi_check_connid();
    tuptable = u_sess->SPI_cxt._current->tuptable;
    if (tuptable == NULL) {
        ereport(ERROR,
            (errcode(ERRCODE_SPI_ERROR), errmsg("SPI tupletable is NULL when store tuple to it for analyze.")));
    }

    oldcxt = MemoryContextSwitchTo(tuptable->tuptabcxt);

    if (tuptable->free == 0) {
        SPI_processed = tuptable->alloced - tuptable->free;
        SPI_tuptable = tuptable;

        /* Callback process tuples we have received.  */
        if (u_sess->SPI_cxt._current->spiCallback) {
            u_sess->SPI_cxt._current->spiCallback(u_sess->SPI_cxt._current->clientData);
        }

        for (uint32 i = 0; i < SPI_processed; i++) {
            pfree_ext(tuptable->vals[i]);
        }

        pfree_ext(tuptable->vals);
        tuptable->free = tuptable->alloced;
        tuptable->vals = (HeapTuple *)palloc0(tuptable->alloced * sizeof(HeapTuple));
    }

    tuptable->vals[tuptable->alloced - tuptable->free] = ExecCopySlotTuple(slot);
    (tuptable->free)--;

    (void)MemoryContextSwitchTo(oldcxt);
}

/*
 * createAnalyzeSPIDestReceiver: create a DestReceiver for printtup of SPI when analyze for table sample..
 *
 * Parameters:
 * 	@in dest: identify the desired destination, results sent to SPI manager.
 *
 * Returns: DestReceiver*
 */
DestReceiver *createAnalyzeSPIDestReceiver(CommandDest dest)
{
    DestReceiver *spi_dst_receiver = (DestReceiver *)palloc0(sizeof(DestReceiver));

    spi_dst_receiver->rStartup = spi_dest_startup;
    spi_dst_receiver->receiveSlot = spi_dest_printTupleAnalyze;
    spi_dst_receiver->rShutdown = spi_dest_shutdownAnalyze;
    spi_dst_receiver->rDestroy = spi_dest_destroyAnalyze;
    spi_dst_receiver->mydest = dest;

    return spi_dst_receiver;
}

/*
 * spi_exec_with_callback: this is a helper method that executes a SQL statement using
 * 					the SPI interface. It optionally calls a callback function with result pointer.
 *
 * Parameters:
 * 	@in dest: indentify execute the plan using oreitation-row or column
 * 	@in src: SQL string
 * 	@in read_only: is it a read-only call?
 * 	@in tcount: execution tuple-count limit, or 0 for none
 * 	@in spec: the sample info of special attribute for compute statistic
 * 	@in callbackFn: callback function to be executed once SPI is done.
 * 	@in clientData: argument to call back function (usually pointer to data-structure
 * 				that the callback function populates).
 *
 * Returns: void
 */
void spi_exec_with_callback(CommandDest dest, const char *src, bool read_only, long tcount, bool direct_call,
    void (*callbackFn)(void *), void *clientData)
{
    bool connected = false;
    int ret = 0;

    PG_TRY();
    {
        if (SPI_OK_CONNECT != SPI_connect(dest, callbackFn, clientData)) {
            ereport(ERROR, (errcode(ERRCODE_SPI_CONNECTION_FAILURE),
                errmsg("Unable to connect to execute internal query, current level: %d, connected level: %d",
                u_sess->SPI_cxt._curid, u_sess->SPI_cxt._connected)));
        }
        connected = true;

        elog(DEBUG1, "Executing SQL: %s", src);

        /* Do the query. */
        ret = SPI_execute(src, read_only, tcount);
        Assert(ret > 0);

        if (direct_call && callbackFn != NULL) {
            callbackFn(clientData);
        }

        connected = false;
        (void)SPI_finish();
    }
    /* Clean up in case of error. */
    PG_CATCH();
    {
        if (connected) {
            SPI_finish();
        }

        /* Carry on with error handling. */
        PG_RE_THROW();
    }
    PG_END_TRY();
}
