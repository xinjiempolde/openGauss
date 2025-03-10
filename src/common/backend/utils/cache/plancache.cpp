/* -------------------------------------------------------------------------
 *
 * plancache.c
 *	  Plan cache management.
 *
 * The plan cache manager has two principal responsibilities: deciding when
 * to use a generic plan versus a custom (parameter-value-specific) plan,
 * and tracking whether cached plans need to be invalidated because of schema
 * changes in the objects they depend on.
 *
 * The logic for choosing generic or custom plans is in ChooseCustomPlan,
 * which see for comments.
 *
 * Cache invalidation is driven off sinval events.	Any CachedPlanSource
 * that matches the event is marked invalid, as is its generic CachedPlan
 * if it has one.  When (and if) the next demand for a cached plan occurs,
 * parse analysis and rewrite is repeated to build a new valid query tree,
 * and then planning is performed as normal.  We also force re-analysis and
 * re-planning if the active search_path is different from the previous time.
 *
 * Note that if the sinval was a result of user DDL actions, parse analysis
 * could throw an error, for example if a column referenced by the query is
 * no longer present.  Another possibility is for the query's output tupdesc
 * to change (for instance "SELECT *" might expand differently than before).
 * The creator of a cached plan can specify whether it is allowable for the
 * query to change output tupdesc on replan --- if so, it's up to the
 * caller to notice changes and cope with them.
 *
 * Currently, we track exactly the dependencies of plans on relations and
 * user-defined functions.	On relcache invalidation events or pg_proc
 * syscache invalidation events, we invalidate just those plans that depend
 * on the particular object being modified.  (Note: this scheme assumes
 * that any table modification that requires replanning will generate a
 * relcache inval event.)  We also watch for inval events on certain other
 * system catalogs, such as pg_namespace; but for them, our response is
 * just to invalidate all plans.  We expect updates on those catalogs to
 * be infrequent enough that more-detailed tracking is not worth the effort.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/plancache.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include <limits.h>

#include "access/transam.h"
#include "catalog/namespace.h"
#include "executor/executor.h"
#include "executor/lightProxy.h"
#include "executor/spi.h"
#include "executor/spi_priv.h"
#include "nodes/nodeFuncs.h"
#include "opfusion/opfusion.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/planner.h"
#include "optimizer/bucketpruning.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/hotkey.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/globalplancache.h"
#include "instruments/instr_unique_sql.h"
#include "instruments/instr_slow_query.h"

#ifdef ENABLE_MOT
#include "storage/mot/jit_exec.h"
#endif

#ifdef PGXC
#include "commands/prepare.h"
#include "pgxc/execRemote.h"
#include "pgxc/pgxc.h"

static void drop_datanode_statements(Plan* plannode);
#endif

/*
 * We must skip "overhead" operations that involve database access when the
 * cached plan's subject statement is a transaction control command.
 */
#define IsTransactionStmtPlan(plansource) \
    ((plansource)->raw_parse_tree && IsA((plansource)->raw_parse_tree, TransactionStmt))

static void ReleaseGenericPlan(CachedPlanSource* plansource);
static bool CheckCachedPlan(CachedPlanSource* plansource);
static CachedPlan* BuildCachedPlan(CachedPlanSource* plansource, List* qlist, ParamListInfo boundParams,
                                           bool isBuildingCustomPlan);
static bool ChooseCustomPlan(CachedPlanSource* plansource, ParamListInfo boundParams);
static bool IsForceCustomplan(CachedPlanSource *plansource);
static bool IsDeleteLimit(CachedPlanSource* plansource, ParamListInfo boundParams);
static double cached_plan_cost(CachedPlan* plan);
static void AcquireExecutorLocks(List* stmt_list, bool acquire);
static void AcquirePlannerLocks(List* stmt_list, bool acquire);
static void ScanQueryForLocks(Query* parsetree, bool acquire);
static bool ScanQueryWalker(Node* node, bool* acquire);
static TupleDesc PlanCacheComputeResultDesc(List* stmt_list);
#ifdef ENABLE_MULTIPLE_NODES
static void GPCCheckStreamPlan(CachedPlanSource *plansource, List* plist);
static bool check_stream_plan(Plan* plan);
#endif
static bool is_upsert_query_with_update_param(Node* raw_parse_tree);
static void GPCFillPlanCache(CachedPlanSource* plansource, bool isBuildingCustomPlan);

bool IsStreamSupport()
{
#ifdef ENABLE_MULTIPLE_NODES
    return u_sess->attr.attr_sql.enable_stream_operator;
#else
    return u_sess->opt_cxt.query_dop > 1;
#endif
}

/*
 * InitPlanCache: initialize module during InitPostgres.
 *
 * All we need to do is hook into inval.c's callback lists.
 */
void InitPlanCache(void)
{
    CacheRegisterRelcacheCallback(PlanCacheRelCallback, (Datum)0);
    CacheRegisterPartcacheCallback(PlanCacheRelCallback, (Datum)0);
    CacheRegisterSyscacheCallback(PROCOID, PlanCacheFuncCallback, (Datum)0);
    CacheRegisterSyscacheCallback(NAMESPACEOID, PlanCacheSysCallback, (Datum)0);
    CacheRegisterSyscacheCallback(OPEROID, PlanCacheSysCallback, (Datum)0);
    CacheRegisterSyscacheCallback(AMOPOPID, PlanCacheSysCallback, (Datum)0);
}

/*
 * CreateCachedPlan: initially create a plan cache entry.
 *
 * Creation of a cached plan is divided into two steps, CreateCachedPlan and
 * CompleteCachedPlan.	CreateCachedPlan should be called after running the
 * query through raw_parser, but before doing parse analysis and rewrite;
 * CompleteCachedPlan is called after that.  The reason for this arrangement
 * is that it can save one round of copying of the raw parse tree, since
 * the parser will normally scribble on the raw parse tree.  Callers would
 * otherwise need to make an extra copy of the parse tree to ensure they
 * still had a clean copy to present at plan cache creation time.
 *
 * All arguments presented to CreateCachedPlan are copied into a memory
 * context created as a child of the call-time CurrentMemoryContext, which
 * should be a reasonably short-lived working context that will go away in
 * event of an error.  This ensures that the cached plan data structure will
 * likewise disappear if an error occurs before we have fully constructed it.
 * Once constructed, the cached plan can be made longer-lived, if needed,
 * by calling SaveCachedPlan.
 *
 * raw_parse_tree: output of raw_parser()
 * query_string: original query text
 * commandTag: compile-time-constant tag for query, or NULL if empty query
 */
CachedPlanSource* CreateCachedPlan(Node* raw_parse_tree, const char* query_string,
#ifdef PGXC
    const char* stmt_name,
#endif
    const char* commandTag, bool enable_spi_gpc)
{
    CachedPlanSource* plansource = NULL;
    MemoryContext source_context;
    MemoryContext oldcxt;

    Assert(query_string != NULL); /* required as of 8.4 */
    bool enable_pbe_gpc = false;
    if (stmt_name != NULL && stmt_name[0] != '\0') {
#ifdef ENABLE_MULTIPLE_NODES
        /* TransactionStmt do not support shared plan */
        enable_pbe_gpc = (ENABLE_CN_GPC && raw_parse_tree && !IsA(raw_parse_tree, TransactionStmt)) || ENABLE_DN_GPC;
#else
        enable_pbe_gpc = ENABLE_GPC && raw_parse_tree && !IsA(raw_parse_tree, TransactionStmt);
#endif
    }

    if(!enable_pbe_gpc && !(ENABLE_CN_GPC && enable_spi_gpc)) {
       /*
        * Make a dedicated memory context for the CachedPlanSource and its
        * permanent subsidiary data.  It's probably not going to be large, but
        * just in case, use the default maxsize parameter.  Initially it's a
        * child of the caller's context (which we assume to be transient), so
        * that it will be cleaned up on error.
        */
        source_context = AllocSetContextCreate(CurrentMemoryContext,
                                            "CachedPlanSource",
                                            ALLOCSET_SMALL_MINSIZE,
                                            ALLOCSET_SMALL_INITSIZE,
                                            ALLOCSET_DEFAULT_MAXSIZE);
    } else {
        char *contextName = NULL;
        if (enable_pbe_gpc) {
            contextName =  "GPCCachedPlanSource";
        }
        if (ENABLE_CN_GPC && enable_spi_gpc) {
            contextName =  "SPI_GPCCachedPlanSource";
        }
        source_context = AllocSetContextCreate(GLOBAL_PLANCACHE_MEMCONTEXT,
                                               contextName,
                                               ALLOCSET_SMALL_MINSIZE,
                                               ALLOCSET_SMALL_INITSIZE,
                                               ALLOCSET_DEFAULT_MAXSIZE,
                                               SHARED_CONTEXT);
        ResourceOwnerEnlargeGMemContext(t_thrd.utils_cxt.TopTransactionResourceOwner);
        ResourceOwnerRememberGMemContext(t_thrd.utils_cxt.TopTransactionResourceOwner, source_context);
    }

    /*
     * Create and fill the CachedPlanSource struct within the new context.
     * Most fields are just left empty for the moment.
     */
    oldcxt = MemoryContextSwitchTo(source_context);

    plansource = (CachedPlanSource*)palloc0(sizeof(CachedPlanSource));
    plansource->magic = CACHEDPLANSOURCE_MAGIC;
    plansource->raw_parse_tree = (Node*)copyObject(raw_parse_tree);
    plansource->query_string = pstrdup(query_string);
    plansource->commandTag = commandTag;
    plansource->param_types = NULL;
    plansource->num_params = 0;
    plansource->parserSetup = NULL;
    plansource->parserSetupArg = NULL;
    plansource->cursor_options = 0;
    plansource->rewriteRoleId = InvalidOid;
    plansource->dependsOnRole = false;
    plansource->fixed_result = false;
    plansource->resultDesc = NULL;
    plansource->search_path = NULL;
    plansource->context = source_context;
#ifdef PGXC
    plansource->stmt_name = (stmt_name ? pstrdup(stmt_name) : NULL);
    plansource->stream_enabled = IsStreamSupport();
    plansource->cplan = NULL;
    plansource->single_exec_node = NULL;
    plansource->is_read_only = false;
    plansource->lightProxyObj = NULL;
    /* Initialize gplan_is_fqs is true, and set to false when find it's not fqs */
    plansource->gplan_is_fqs = true;
#endif

    plansource->query_list = NIL;
    plansource->relationOids = NIL;
    plansource->invalItems = NIL;
    plansource->query_context = NULL;
    plansource->gplan = NULL;
    plansource->is_oneshot = false;
    plansource->is_complete = false;
    plansource->is_saved = false;
    plansource->is_valid = false;
    plansource->generation = 0;
    plansource->next_saved = NULL;
    plansource->generic_cost = -1;
    plansource->total_custom_cost = 0;
    plansource->num_custom_plans = 0;
    plansource->opFusionObj = NULL;
    plansource->is_checked_opfusion = false;
    plansource->is_support_gplan = false;
    plansource->spi_signature = {(uint32)-1, 0, (uint32)-1, -1};

#ifdef ENABLE_MOT
    plansource->storageEngineType = SE_TYPE_UNSPECIFIED;
    plansource->mot_jit_context = NULL;
#endif

    if (enable_pbe_gpc) {
        plansource->gpc.status.ShareInit();
    } else if (ENABLE_CN_GPC && enable_spi_gpc) {
        Assert(u_sess->SPI_cxt._current->plan_id >= 0 && u_sess->SPI_cxt._current->visit_id >= 0);
        plansource->gpc.status.ShareInit();
        plansource->spi_signature.spi_key = u_sess->SPI_cxt._current->spi_hash_key;
        plansource->spi_signature.func_oid = u_sess->SPI_cxt._current->func_oid;
        plansource->spi_signature.spi_id = u_sess->SPI_cxt._current->visit_id;
        plansource->spi_signature.plansource_id = u_sess->SPI_cxt._current->plan_id;
    }

    MemoryContextSwitchTo(oldcxt);
    GPC_LOG("create plancache", plansource, plansource->stmt_name);

    return plansource;
}

/*
 * CreateOneShotCachedPlan: initially create a one-shot plan cache entry.
 *
 * This variant of CreateCachedPlan creates a plan cache entry that is meant
 * to be used only once.  No data copying occurs: all data structures remain
 * in the caller's memory context (which typically should get cleared after
 * completing execution).  The CachedPlanSource struct itself is also created
 * in that context.
 *
 * A one-shot plan cannot be saved or copied, since we make no effort to
 * preserve the raw parse tree unmodified.  There is also no support for
 * invalidation, so plan use must be completed in the current transaction,
 * and DDL that might invalidate the querytree_list must be avoided as well.
 *
 * raw_parse_tree: output of raw_parser()
 * query_string: original query text
 * commandTag: compile-time-constant tag for query, or NULL if empty query
 */
CachedPlanSource* CreateOneShotCachedPlan(Node* raw_parse_tree, const char* query_string, const char* commandTag)
{
    CachedPlanSource* plansource = NULL;

    Assert(query_string != NULL); /* required as of 8.4 */

    /*
     * Create and fill the CachedPlanSource struct within the caller's memory
     * context.  Most fields are just left empty for the moment.
     */
    plansource = (CachedPlanSource*)palloc0(sizeof(CachedPlanSource));
    plansource->magic = CACHEDPLANSOURCE_MAGIC;
    plansource->raw_parse_tree = raw_parse_tree;
    plansource->query_string = query_string;
    plansource->commandTag = commandTag;
    plansource->param_types = NULL;
    plansource->num_params = 0;
    plansource->parserSetup = NULL;
    plansource->parserSetupArg = NULL;
    plansource->cursor_options = 0;
    plansource->rewriteRoleId = InvalidOid;
    plansource->dependsOnRole = false;
    plansource->fixed_result = false;
    plansource->resultDesc = NULL;
    plansource->search_path = NULL;
    plansource->context = CurrentMemoryContext;
    plansource->query_list = NIL;
    plansource->relationOids = NIL;
    plansource->invalItems = NIL;
    plansource->query_context = NULL;
    plansource->gplan = NULL;
    plansource->is_oneshot = true;
    plansource->is_complete = false;
    plansource->is_saved = false;
    plansource->is_valid = false;
    plansource->generation = 0;
    plansource->next_saved = NULL;
    plansource->generic_cost = -1;
    plansource->total_custom_cost = 0;
    plansource->num_custom_plans = 0;
    plansource->spi_signature = {(uint32)-1, 0, (uint32)-1, -1};

#ifdef ENABLE_MOT
    plansource->storageEngineType = SE_TYPE_UNSPECIFIED;
    plansource->mot_jit_context = NULL;
#endif

#ifdef PGXC
    plansource->stream_enabled = IsStreamSupport();
    plansource->cplan = NULL;
    plansource->single_exec_node = NULL;
    plansource->is_read_only = false;
    plansource->lightProxyObj = NULL;
#endif

    return plansource;
}

/*
 * CompleteCachedPlan: second step of creating a plan cache entry.
 *
 * Pass in the analyzed-and-rewritten form of the query, as well as the
 * required subsidiary data about parameters and such.	All passed values will
 * be copied into the CachedPlanSource's memory, except as specified below.
 * After this is called, GetCachedPlan can be called to obtain a plan, and
 * optionally the CachedPlanSource can be saved using SaveCachedPlan.
 *
 * If querytree_context is not NULL, the querytree_list must be stored in that
 * context (but the other parameters need not be).	The querytree_list is not
 * copied, rather the given context is kept as the initial query_context of
 * the CachedPlanSource.  (It should have been created as a child of the
 * caller's working memory context, but it will now be reparented to belong
 * to the CachedPlanSource.)  The querytree_context is normally the context in
 * which the caller did raw parsing and parse analysis.  This approach saves
 * one tree copying step compared to passing NULL, but leaves lots of extra
 * cruft in the query_context, namely whatever extraneous stuff parse analysis
 * created, as well as whatever went unused from the raw parse tree.  Using
 * this option is a space-for-time tradeoff that is appropriate if the
 * CachedPlanSource is not expected to survive long.
 *
 * plancache.c cannot know how to copy the data referenced by parserSetupArg,
 * and it would often be inappropriate to do so anyway.  When using that
 * option, it is caller's responsibility that the referenced data remains
 * valid for as long as the CachedPlanSource exists.
 *
 * If the CachedPlanSource is a "oneshot" plan, then no querytree copying
 * occurs at all, and querytree_context is ignored; it is caller's
 * responsibility that the passed querytree_list is sufficiently long-lived.
 *
 * plansource: structure returned by CreateCachedPlan
 * querytree_list: analyzed-and-rewritten form of query (list of Query nodes)
 * querytree_context: memory context containing querytree_list,
 *					  or NULL to copy querytree_list into a fresh context
 * param_types: array of fixed parameter type OIDs, or NULL if none
 * num_params: number of fixed parameters
 * parserSetup: alternate method for handling query parameters
 * parserSetupArg: data to pass to parserSetup
 * cursor_options: options bitmask to pass to planner
 * fixed_result: TRUE to disallow future changes in query's result tupdesc
 */
void CompleteCachedPlan(CachedPlanSource* plansource, List* querytree_list, MemoryContext querytree_context,
    Oid* param_types, int num_params, ParserSetupHook parserSetup, void* parserSetupArg, int cursor_options,
    bool fixed_result, const char* stmt_name, ExecNodes* single_exec_node, bool is_read_only)
{
    MemoryContext source_context = plansource->context;
    MemoryContext oldcxt = CurrentMemoryContext;

    /* Assert caller is doing things in a sane order */
    Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
    Assert(!plansource->is_complete);

    /*
     * If caller supplied a querytree_context, reparent it underneath the
     * CachedPlanSource's context; otherwise, create a suitable context and
     * copy the querytree_list into it.  But no data copying should be done
     * for one-shot plans; for those, assume the passed querytree_list is
     * sufficiently long-lived.
     */
    if (plansource->is_oneshot) {
        querytree_context = CurrentMemoryContext;
        Assert (plansource->gpc.status.IsPrivatePlan());
    } else if (querytree_context != NULL) {
        if (plansource->gpc.status.IsPrivatePlan()) {
            MemoryContextSetParent(querytree_context, source_context);
        }
        MemoryContextSwitchTo(querytree_context);
    } else {
        if (!plansource->gpc.status.IsPrivatePlan()) {
		    querytree_context = AllocSetContextCreate(source_context,
    												  "GPCCachedPlanQuery",
    												  ALLOCSET_SMALL_MINSIZE,
    												  ALLOCSET_SMALL_INITSIZE,
    												  ALLOCSET_DEFAULT_MAXSIZE,
    												  SHARED_CONTEXT);
        } else {
    		/* Again, it's a good bet the querytree_context can be small */
    		querytree_context = AllocSetContextCreate(source_context,
    												  "CachedPlanQuery",
    												  ALLOCSET_SMALL_MINSIZE,
    												  ALLOCSET_SMALL_INITSIZE,
    												  ALLOCSET_DEFAULT_MAXSIZE);
        }
        MemoryContextSwitchTo(querytree_context);
        querytree_list = (List*)copyObject(querytree_list);
    }

    /*
     * Use the planner machinery to extract dependencies.  Data is saved in
     * query_context.  (We assume that not a lot of extra cruft is created by
     * this call.)	We can skip this for one-shot plans, and transaction
     * control commands have no such dependencies anyway.
     */
    if (!plansource->is_oneshot && !IsTransactionStmtPlan(plansource)) {
        extract_query_dependencies((Node*)querytree_list,
            &plansource->relationOids,
            &plansource->invalItems,
            &plansource->dependsOnRole,
            &plansource->force_custom_plan);

        /*
         * Also save the current search_path in the query_context.  (This
         * should not generate much extra cruft either, since almost certainly
         * the path is already valid.)  Again, we don't really need this for
         * one-shot plans; and we *must* skip this for transaction control
         * commands, because this could result in catalog accesses.
         */
        plansource->search_path = GetOverrideSearchPath(source_context);
    }

    /* Update RLS info as well. */
    plansource->rewriteRoleId = GetUserId();
    plansource->query_context = querytree_context;
    plansource->query_list = querytree_list;

    /*
     * Save the final parameter types (or other parameter specification data)
     * into the source_context, as well as our other parameters.  Also save
     * the result tuple descriptor.
     */
    MemoryContextSwitchTo(source_context);

    errno_t rc;
    if (num_params > 0) {
        plansource->param_types = (Oid*)palloc(num_params * sizeof(Oid));
        rc = memcpy_s(plansource->param_types, num_params * sizeof(Oid), param_types, num_params * sizeof(Oid));
        securec_check(rc, "", "");
    } else
        plansource->param_types = NULL;
    plansource->num_params = num_params;
    plansource->parserSetup = parserSetup;
    plansource->parserSetupArg = parserSetupArg;
    plansource->cursor_options = cursor_options;
    plansource->fixed_result = fixed_result;
#ifdef PGXC
    plansource->stmt_name = (*stmt_name ? pstrdup(stmt_name) : NULL);
#endif
    plansource->resultDesc = PlanCacheComputeResultDesc(querytree_list);
    MemoryContextSwitchTo(querytree_context);
    plansource->single_exec_node = (ExecNodes*)copyObject(single_exec_node);

    MemoryContextSwitchTo(oldcxt);

    plansource->is_complete = true;
    plansource->is_valid = true;
    plansource->is_read_only = is_read_only;
}

/*
 * SaveCachedPlan: save a cached plan permanently
 *
 * This function moves the cached plan underneath u_sess->cache_mem_cxt (making
 * it live for the life of the backend, unless explicitly dropped), and adds
 * it to the list of cached plans that are checked for invalidation when an
 * sinval event occurs.
 *
 * This is guaranteed not to throw error, except for the caller-error case
 * of trying to save a one-shot plan.  Callers typically depend on that
 * since this is called just before or just after adding a pointer to the
 * CachedPlanSource to some permanent data structure of their own.	Up until
 * this is done, a CachedPlanSource is just transient data that will go away
 * automatically on transaction abort.
 */
void SaveCachedPlan(CachedPlanSource* plansource)
{
    /* Assert caller is doing things in a sane order */
    Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
    Assert(plansource->is_complete);
    Assert(!plansource->is_saved);
    Assert(plansource->gpc.status.InShareTable() == false);
    /* This seems worth a real test, though */
    if (plansource->is_oneshot)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot save one-shot cached plan")));

    /*
     * In typical use, this function would be called before generating any
     * plans from the CachedPlanSource.  If there is a generic plan, moving it
     * into u_sess->cache_mem_cxt would be pretty risky since it's unclear
     * whether the caller has taken suitable care with making references
     * long-lived.	Best thing to do seems to be to discard the plan.
     */
    ReleaseGenericPlan(plansource);

    /*
     * Reparent the source memory context under u_sess->cache_mem_cxt so that it
     * will live indefinitely.	The query_context follows along since it's
     * already a child of the other one.
     */
    if (plansource->gpc.status.IsPrivatePlan()) {
        MemoryContextSetParent(plansource->context, u_sess->cache_mem_cxt);
    }

    START_CRIT_SECTION();
    ResourceOwnerForgetGMemContext(t_thrd.utils_cxt.TopTransactionResourceOwner, plansource->context);
    /*
     * Add the entry to the session's global list of cached plans.
     */
    plansource->next_saved = u_sess->pcache_cxt.first_saved_plan;
    u_sess->pcache_cxt.first_saved_plan = plansource;

    plansource->is_saved = true;
    END_CRIT_SECTION();
    
}

/*
 * DropCachedPlan: destroy a cached plan.
 *
 * Actually this only destroys the CachedPlanSource: any referenced CachedPlan
 * is released, but not destroyed until its refcount goes to zero.	That
 * handles the situation where DropCachedPlan is called while the plan is
 * still in use.
 */
void DropCachedPlan(CachedPlanSource* plansource)
{
    Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
    if (ENABLE_GPC && plansource->gpc.status.InShareTable())
        elog(PANIC, "should not drop shared plan");

    /* If it's been saved, remove it from the list */
    if (plansource->is_saved) {
        if (u_sess->pcache_cxt.first_saved_plan == plansource)
            u_sess->pcache_cxt.first_saved_plan = plansource->next_saved;
        else {
            CachedPlanSource* psrc = NULL;

            for (psrc = u_sess->pcache_cxt.first_saved_plan; psrc; psrc = psrc->next_saved) {
                if (psrc->next_saved == plansource) {
                    psrc->next_saved = plansource->next_saved;
                    break;
                }
            }
        }
        if (ENABLE_CN_GPC) {
            if (u_sess->pcache_cxt.ungpc_saved_plan == plansource) {
                u_sess->pcache_cxt.ungpc_saved_plan = plansource->next_saved;
            } else {
                CachedPlanSource* psrc = NULL;
                for (psrc = u_sess->pcache_cxt.ungpc_saved_plan; psrc; psrc = psrc->next_saved) {
                    if (psrc->next_saved == plansource) {
                        psrc->next_saved = plansource->next_saved;
                        break;
                    }
                }
            }
        }
        if (ENABLE_GPC)
            GPC_LOG("BEFORE DROP CACHE PLAN", plansource, plansource->stmt_name);
        plansource->is_saved = false;
    }
    plansource->next_saved = NULL;
    DropCachedPlanInternal(plansource);

    /* Mark it no longer valid */
    plansource->magic = 0;

    if (ENABLE_DN_GPC)
        GPC_LOG("DROP CACHE PLAN", plansource, 0);
    if (ENABLE_CN_GPC)
        CN_GPC_LOG("DROP CACHE PLAN", plansource, 0);

    /*
     * Remove the CachedPlanSource and all subsidiary data (including the
     * query_context if any).  But if it's a one-shot we can't free anything.
     */
    if (!plansource->is_oneshot)
        MemoryContextDelete(plansource->context);

}

/*
 * ReleaseGenericPlan: release a CachedPlanSource's generic plan, if any.
 */
static void ReleaseGenericPlan(CachedPlanSource* plansource)
{
    /* Be paranoid about the possibility that ReleaseCachedPlan fails */
    if (plansource->gplan || plansource->cplan) {
        CachedPlan* plan = NULL;

        /* custom plan and generic plan should not both exists */
        Assert(plansource->gplan == NULL || plansource->cplan == NULL);

        if (plansource->gplan)
            plan = plansource->gplan;
        else
            plan = plansource->cplan;

#ifdef PGXC
        /* Drop this plan on remote nodes */
        if (plan != NULL && !plan->isShared() && !u_sess->pcache_cxt.gpc_in_try_store) {
            ListCell* lc = NULL;

            /* Close any active planned Datanode statements */
            foreach (lc, plan->stmt_list) {
                Node* node = (Node*)lfirst(lc);

                if (IsA(node, PlannedStmt)) {
                    PlannedStmt* ps = (PlannedStmt*)node;
                    drop_datanode_statements(ps->planTree);
                }
            }
        }
#endif

        Assert(plan->magic == CACHEDPLAN_MAGIC);
        plansource->gplan = NULL;
        plansource->cplan = NULL;
        ReleaseCachedPlan(plan, false);
    }
}

/*
 * RevalidateCachedQuery: ensure validity of analyzed-and-rewritten query tree.
 *
 * What we do here is re-acquire locks and redo parse analysis if necessary.
 * On return, the query_list is valid and we have sufficient locks to begin
 * planning.
 *
 * If any parse analysis activity is required, the caller's memory context is
 * used for that work.
 *
 * The result value is the transient analyzed-and-rewritten query tree if we
 * had to do re-analysis, and NIL otherwise.  (This is returned just to save
 * a tree copying step in a subsequent BuildCachedPlan call.)
 */
List* RevalidateCachedQuery(CachedPlanSource* plansource, bool has_lp)
{
    bool snapshot_set = false;
    Node* rawtree = NULL;
    List* tlist = NIL; /* transient query-tree list */
    List* qlist = NIL; /* permanent query-tree list */
    TupleDesc resultDesc;
    MemoryContext querytree_context;
    MemoryContext oldcxt;
    bool need_reset_singlenode = false;
    /*
     * For one-shot plans, we do not support revalidation checking; it's
     * assumed the query is parsed, planned, and executed in one transaction,
     * so that no lock re-acquisition is necessary.
     */
    if (plansource->is_oneshot || IsTransactionStmtPlan(plansource)) {
        Assert(plansource->is_valid);
        return NIL;
    }
    /* if is shared plan, we should acquire plan lock for this transaction */
    if (plansource->gpc.status.InShareTable()) {
        Assert(plansource->is_valid);
        AcquirePlannerLocks(plansource->query_list, true);
        return NIL;
    }
    /*
     * If there were no parsetrees, we don't need to check the the plan is whether invalid or not cause we do nothing but call
     *  NullCommand() in the execute stage.
     */
    if (plansource->raw_parse_tree == NULL) {
        plansource->is_valid = true;
        return NIL;
    }

    /*
     * If the query is currently valid, we should have a saved search_path ---
     * check to see if that matches the current environment.  If not, we want
     * to force replan.
     */
    if (plansource->is_valid) {
        Assert(plansource->search_path != NULL);
        if (!OverrideSearchPathMatchesCurrent(plansource->search_path)) {
            /* Invalidate the querytree and generic plan */
            plansource->is_valid = false;
            if (plansource->gplan)
                plansource->gplan->is_valid = false;
        }
    }

    /*
     * If the query rewrite phase had a possible RLS dependency, we must redo
     * it if either the role setting has changed.
     */
    if (plansource->is_valid && plansource->dependsOnRole && (plansource->rewriteRoleId != GetUserId()))
        plansource->is_valid = false;

    /*
     * If the query is currently valid, acquire locks on the referenced
     * objects; then check again.  We need to do it this way to cover the race
     * condition that an invalidation message arrives before we get the locks.
     */
    if (plansource->is_valid) {
        AcquirePlannerLocks(plansource->query_list, true);

        /*
         * By now, if any invalidation has happened, the inval callback
         * functions will have marked the query invalid.
         */
        if (plansource->is_valid) {
            /* Successfully revalidated and locked the query. */
            return NIL;
        }

        /* Ooops, the race case happened.  Release useless locks. */
        AcquirePlannerLocks(plansource->query_list, false);
    }

    Assert(!plansource->gpc.status.InShareTable());

    /*
     * Discard the no-longer-useful query tree.  (Note: we don't want to do
     * this any earlier, else we'd not have been able to release locks
     * correctly in the race condition case.)
     */
    plansource->is_valid = false;
    plansource->query_list = NIL;
    plansource->relationOids = NIL;
    plansource->invalItems = NIL;
    plansource->search_path = NULL;
    if (plansource->single_exec_node) {
        need_reset_singlenode = true;
    }
    /*
     * Free the query_context.	We don't really expect MemoryContextDelete to
     * fail, but just in case, make sure the CachedPlanSource is left in a
     * reasonably sane state.  (The generic plan won't get unlinked yet, but
     * that's acceptable.)
     */
    if (plansource->query_context) {
        MemoryContext qcxt = plansource->query_context;

        plansource->query_context = NULL;
        MemoryContextDelete(qcxt);
    }

    /*
     * Now re-do parse analysis and rewrite.  This not incidentally acquires
     * the locks we need to do planning safely.
     */
    Assert(plansource->is_complete);

    /*
     * If a snapshot is already set (the normal case), we can just use that
     * for parsing/planning.  But if it isn't, install one.  Note: no point in
     * checking whether parse analysis requires a snapshot; utility commands
     * don't have invalidatable plans, so we'd not get here for such a
     * command.
     */
    snapshot_set = false;
    if (!ActiveSnapshotSet()) {
        PushActiveSnapshot(GetTransactionSnapshot(GTM_LITE_MODE));
        snapshot_set = true;
    }

    /*
     * Run parse analysis and rule rewriting.  The parser tends to scribble on
     * its input, so we must copy the raw parse tree to prevent corruption of
     * the cache.
     */
    rawtree = (Node*)copyObject(plansource->raw_parse_tree);
    if (plansource->parserSetup != NULL)
        tlist = pg_analyze_and_rewrite_params(
            rawtree, plansource->query_string, plansource->parserSetup, plansource->parserSetupArg);
    else
        tlist =
            pg_analyze_and_rewrite(rawtree, plansource->query_string, plansource->param_types, plansource->num_params);

    /* Release snapshot if we got one */
    if (snapshot_set)
        PopActiveSnapshot();

    /*
     * Check or update the result tupdesc.	XXX should we use a weaker
     * condition than equalTupleDescs() here?
     *
     * We assume the parameter types didn't change from the first time, so no
     * need to update that.
     */
    resultDesc = PlanCacheComputeResultDesc(tlist);
    if (resultDesc == NULL && plansource->resultDesc == NULL) {
        /* OK, doesn't return tuples */
    } else if (resultDesc == NULL || plansource->resultDesc == NULL ||
               !equalTupleDescs(resultDesc, plansource->resultDesc)) {
        /* can we give a better error message? */
        if (plansource->fixed_result)
            ereport(ERROR, (errcode(ERRCODE_INVALID_CACHE_PLAN), errmsg("cached plan must not change result type")));
        oldcxt = MemoryContextSwitchTo(plansource->context);
        if (resultDesc)
            resultDesc = CreateTupleDescCopy(resultDesc);
        if (plansource->resultDesc)
            FreeTupleDesc(plansource->resultDesc);
        plansource->resultDesc = resultDesc;
        MemoryContextSwitchTo(oldcxt);
    }

    if (!plansource->gpc.status.IsPrivatePlan()) {
    	querytree_context = AllocSetContextCreate(plansource->context,
    											  "GPCCachedPlanQuery",
    											  ALLOCSET_SMALL_MINSIZE,
    											  ALLOCSET_SMALL_INITSIZE,
    											  ALLOCSET_DEFAULT_MAXSIZE,
    											  SHARED_CONTEXT);
    } else {
    	/*
    	 * Allocate new query_context and copy the completed querytree into it.
    	 * It's transient until we complete the copying and dependency extraction.
    	 */
    	querytree_context = AllocSetContextCreate(u_sess->top_mem_cxt,
    											  "CachedPlanQuery",
    											  ALLOCSET_SMALL_MINSIZE,
    											  ALLOCSET_SMALL_INITSIZE,
    											  ALLOCSET_DEFAULT_MAXSIZE);
    }

    oldcxt = MemoryContextSwitchTo(querytree_context);

    qlist = (List*)copyObject(tlist);

    /*
     * Use the planner machinery to extract dependencies.  Data is saved in
     * query_context.  (We assume that not a lot of extra cruft is created by
     * this call.)
     */
    extract_query_dependencies((Node*)qlist,
        &plansource->relationOids,
        &plansource->invalItems,
        &plansource->dependsOnRole,
        &plansource->force_custom_plan);

    /* Update RLS info as well. */
    plansource->rewriteRoleId = GetUserId();

    /*
     * Also save the current search_path in the query_context.  (This should
     * not generate much extra cruft either, since almost certainly the path
     * is already valid.)
     */
    plansource->search_path = GetOverrideSearchPath(querytree_context);

    MemoryContextSwitchTo(oldcxt);

    if (plansource->gpc.status.IsPrivatePlan()) {
        /* Now reparent the finished query_context and save the links */
        MemoryContextSetParent(querytree_context, plansource->context);
    }

    plansource->query_context = querytree_context;
    plansource->query_list = qlist;

    /* Update ExecNodes for Light CN */
    if (need_reset_singlenode || has_lp) {
        ExecNodes* single_exec_node = NULL;

        /* should be only one query */
        if (list_length(qlist) == 1) {
            Query* query = (Query*)linitial(qlist);
            single_exec_node = lightProxy::checkLightQuery(query);

            /* only deal with single node */
            if (single_exec_node != NULL && list_length(single_exec_node->nodeList) +
                list_length(single_exec_node->primarynodelist) > 1) {
                FreeExecNodes(&single_exec_node);
            }
            CleanHotkeyCandidates(true);
        }

        oldcxt = MemoryContextSwitchTo(querytree_context);

        /* copy first in case memory error occurs */
        ExecNodes* tmp_en1 = (ExecNodes*)copyObject(single_exec_node);
        plansource->single_exec_node = tmp_en1;

        MemoryContextSwitchTo(oldcxt);
    }

    /* clean lightProxyObj if exists */
    if (plansource->lightProxyObj != NULL) {
        lightProxy* lp = (lightProxy*)plansource->lightProxyObj;
        lightProxy::tearDown(lp);
        plansource->lightProxyObj = NULL;
    }

    /* clean opFuisonObj if exists */
    if (plansource->opFusionObj != NULL) {
        OpFusion::tearDown((OpFusion*)plansource->opFusionObj);
        plansource->opFusionObj = NULL;
    }

#ifdef ENABLE_MOT
    /* clean JIT context if exists */
    if (plansource->mot_jit_context) {
        JitExec::DestroyJitContext(plansource->mot_jit_context);
        plansource->mot_jit_context = NULL;
    }
#endif

    /*
     * Note: we do not reset generic_cost or total_custom_cost, although we
     * could choose to do so.  If the DDL or statistics change that prompted
     * the invalidation meant a significant change in the cost estimates, it
     * would be better to reset those variables and start fresh; but often it
     * doesn't, and we're better retaining our hard-won knowledge about the
     * relative costs.
     */

    plansource->is_valid = true;
    plansource->gpc.status.SetStatus(GPC_VALID);

    /* Return transient copy of querytrees for possible use in planning */
    return tlist;
}

/*
 * CheckCachedPlan: see if the CachedPlanSource's generic plan is valid.
 *
 * Caller must have already called RevalidateCachedQuery to verify that the
 * querytree is up to date.
 *
 * On a "true" return, we have acquired the locks needed to run the plan.
 * (We must do this for the "true" result to be race-condition-free.)
 */
static bool CheckCachedPlan(CachedPlanSource* plansource)
{
    CachedPlan* plan = plansource->gplan;

    /* Assert that caller checked the querytree */
    Assert(plansource->is_valid);

    /* If there's no generic plan, just say "false" */
    if (plan == NULL) {
        if (plansource->gpc.status.InShareTable()) {
            elog(PANIC, "CheckCachedPlan no gplan for sharedplan %s", plansource->stmt_name);
        }
        return false;
    }

    /* If stream_operator alreadly change, need build plan again.*/
    if ((!plansource->gpc.status.InShareTable()) && plansource->stream_enabled != IsStreamSupport()) {
        return false;
    }

    if (plansource->gpc.status.InShareTable()) {
        return true;
    }

    Assert(plan->magic == CACHEDPLAN_MAGIC);
    /* Generic plans are never one-shot */
    Assert(!plan->is_oneshot);

    /* If plan isn't valid for current role, we can't use it. */
    if (plan->is_valid && plan->dependsOnRole && plan->planRoleId != GetUserId())
        plan->is_valid = false;

    /*
     * If it appears valid, acquire locks and recheck; this is much the same
     * logic as in RevalidateCachedQuery, but for a plan.
     */
    if (plan->is_valid) {
        /*
         * Plan must have positive refcount because it is referenced by
         * plansource; so no need to fear it disappears under us here.
         */
        Assert(plan->refcount > 0);

        AcquireExecutorLocks(plan->stmt_list, true);

        /*
         * If plan was transient, check to see if TransactionXmin has
         * advanced, and if so invalidate it.
         */
        if (plan->is_valid && TransactionIdIsValid(plan->saved_xmin) &&
            !TransactionIdEquals(plan->saved_xmin, u_sess->utils_cxt.TransactionXmin))
            plan->is_valid = false;

        /*
         * By now, if any invalidation has happened, the inval callback
         * functions will have marked the plan invalid.
         */
        if (plan->is_valid) {
            /* Successfully revalidated and locked the query. */
            return true;
        }

        /* Ooops, the race case happened.  Release useless locks. */
        AcquireExecutorLocks(plan->stmt_list, false);
    }
    Assert(!plansource->gpc.status.InShareTable());
    /*
     * Plan has been invalidated, so unlink it from the parent and release it.
     */
    ReleaseGenericPlan(plansource);

    return false;
}

static inline void ResetStream(bool outer_is_stream, bool outer_is_stream_support)
{
    if (IS_PGXC_COORDINATOR) {
        u_sess->opt_cxt.is_stream = outer_is_stream;
        u_sess->opt_cxt.is_stream_support = outer_is_stream_support;
    }
}

/*
 * BuildCachedPlan: construct a new CachedPlan from a CachedPlanSource.
 *
 * qlist should be the result value from a previous RevalidateCachedQuery,
 * or it can be set to NIL if we need to re-copy the plansource's query_list.
 *
 * To build a generic, parameter-value-independent plan, pass NULL for
 * boundParams.  To build a custom plan, pass the actual parameter values via
 * boundParams.  For best effect, the PARAM_FLAG_CONST flag should be set on
 * each parameter value; otherwise the planner will treat the value as a
 * hint rather than a hard constant.
 *
 * Planning work is done in the caller's memory context.  The finished plan
 * is in a child memory context, which typically should get reparented
 * (unless this is a one-shot plan, in which case we don't copy the plan).
 */
static CachedPlan* BuildCachedPlan(CachedPlanSource* plansource, List* qlist, ParamListInfo boundParams,
                                          bool isBuildingCustomPlan)
{
    CachedPlan* plan = NULL;
    List* plist = NIL;
    bool snapshot_set = false;
    bool spi_pushed = false;
    MemoryContext plan_context;
    MemoryContext oldcxt = CurrentMemoryContext;
    ListCell* lc = NULL;
    bool is_transient = false;
    PLpgSQL_execstate *saved_estate = plpgsql_estate;
    bool        outer_is_stream = false;
    bool        outer_is_stream_support = false;

    /*
     * NOTE: GetCachedPlan should have called RevalidateCachedQuery first, so
     * we ought to be holding sufficient locks to prevent any invalidation.
     * However, if we're building a custom plan after having built and
     * rejected a generic plan, it's possible to reach here with is_valid
     * false due to an invalidation while making the generic plan.	In theory
     * the invalidation must be a false positive, perhaps a consequence of an
     * sinval reset event or the CLOBBER_CACHE_ALWAYS debug code.
     *
     * We should not call RevalidateCachedQuery here for the above case, for
     * something has already been done when building a generic plan, for example
     * QueryRewriteCTAS has already created the table, and if we call
     * RevalidateCachedQuery again it will report an error 'table already exists'.
     * Also as the comments above, there is also no need to call it.
     */

    /*
     * If we don't already have a copy of the querytree list that can be
     * scribbled on by the planner, make one.  For a one-shot plan, we assume
     * it's okay to scribble on the original query_list.
     */
    if (qlist == NIL) {
        if (!plansource->is_oneshot)
            qlist = (List*)copyObject(plansource->query_list);
        else
            qlist = plansource->query_list;
    }

    /*
     * If a snapshot is already set (the normal case), we can just use that
     * for planning.  But if it isn't, and we need one, install one.
     */
    snapshot_set = false;
    if (!ActiveSnapshotSet() &&
#ifdef ENABLE_MOT
        !(plansource->storageEngineType == SE_TYPE_MOT) &&
#endif
        analyze_requires_snapshot(plansource->raw_parse_tree)) {
        PushActiveSnapshot(GetTransactionSnapshot(GTM_LITE_MODE));
        snapshot_set = true;
    }

    /*
     * The planner may try to call SPI-using functions, which causes a problem
     * if we're already inside one.  Rather than expect all SPI-using code to
     * do SPI_push whenever a replan could happen, it seems best to take care
     * of the case here.
     */
    spi_pushed = SPI_push_conditional();
    u_sess->pcache_cxt.query_has_params = (plansource->num_params > 0);
    /*
     * Generate the plan and we temporarily close enable_trigger_shipping as
     * we don't support cached shipping plan for trigger.
     */
    PG_TRY();
    {
        /* Save stream supported info since it will be reset when generate the plan. */
        outer_is_stream = u_sess->opt_cxt.is_stream;
        outer_is_stream_support = u_sess->opt_cxt.is_stream_support;
#ifndef ENABLE_MULTIPLE_NODES
        /* opengaussdb:jdbc to create vecplan */
        set_default_stream();
#endif
        plist = pg_plan_queries(qlist, plansource->cursor_options, boundParams);
    }
    PG_CATCH();
    {
        ResetStream(outer_is_stream, outer_is_stream_support);
        PG_RE_THROW();
    }
    PG_END_TRY();
    u_sess->pcache_cxt.query_has_params = false;

    ResetStream(outer_is_stream, outer_is_stream_support);

    /* Clean up SPI state */
    SPI_pop_conditional(spi_pushed);

    /* Release snapshot if we got one */
    if (snapshot_set)
        PopActiveSnapshot();

    /*
     * Normally we make a dedicated memory context for the CachedPlan and its
     * subsidiary data.  (It's probably not going to be large, but just in
     * case, use the default maxsize parameter.  It's transient for the
     * moment.)  But for a one-shot plan, we just leave it in the caller's
     * memory context.
     */
    if (!plansource->is_oneshot) {
        if (!plansource->gpc.status.IsPrivatePlan()) {
            plan_context = AllocSetContextCreate(GLOBAL_PLANCACHE_MEMCONTEXT,
    											 "GPCCachedPlan",
    											 ALLOCSET_SMALL_MINSIZE,
    											 ALLOCSET_SMALL_INITSIZE,
    											 ALLOCSET_DEFAULT_MAXSIZE,
    											 SHARED_CONTEXT);
        } else {
            plan_context = AllocSetContextCreate(u_sess->cache_mem_cxt,
    											 "CachedPlan",
    											 ALLOCSET_SMALL_MINSIZE,
    											 ALLOCSET_SMALL_INITSIZE,
    											 ALLOCSET_DEFAULT_MAXSIZE);
        }

        /* We must track shared memory context for handling exception */
        ResourceOwnerEnlargeGMemContext(t_thrd.utils_cxt.TopTransactionResourceOwner);
        ResourceOwnerRememberGMemContext(t_thrd.utils_cxt.TopTransactionResourceOwner, plan_context);
        
        /*
         * Copy plan into the new context.
         */
        MemoryContextSwitchTo(plan_context);

        plist = (List*)copyObject(plist);
    } else
        plan_context = CurrentMemoryContext;

#ifdef PGXC
    /*
     * If this plansource belongs to a named prepared statement, store the stmt
     * name for the Datanode queries.
     */
    bool is_named_prepare = (IS_PGXC_COORDINATOR && !IsConnFromCoord() && plansource->stmt_name);
    int stmt_num = 0;
    if (is_named_prepare) {
        int n;

        /*
         * Scan the plans and set the statement field for all found RemoteQuery
         * nodes so they use Datanode statements
         */
        n = 0;
        foreach (lc, plist) {
            Node* st = NULL;
            PlannedStmt* ps = NULL;

            st = (Node*)lfirst(lc);

            if (IsA(st, PlannedStmt)) {
                ps = (PlannedStmt*)st;
                n = SetRemoteStatementName(ps->planTree, plansource->stmt_name, plansource->num_params,
                                           plansource->param_types, n, isBuildingCustomPlan);
            }
        }
        stmt_num = n;
    }
#endif

    /*
     * Create and fill the CachedPlan struct within the new context.
     */
    plan = (CachedPlan*)palloc(sizeof(CachedPlan));
    plan->magic = CACHEDPLAN_MAGIC;
    plan->stmt_list = plist;
    plan->dn_stmt_num = stmt_num;

    /*
     * CachedPlan is dependent on role either if RLS affected the rewrite
     * phase or if a role dependency was injected during planning.  And it's
     * transient if any plan is marked so.
     */
    plan->planRoleId = GetUserId();
    plan->dependsOnRole = plansource->dependsOnRole;

    foreach (lc, plist) {
        PlannedStmt* plannedstmt = (PlannedStmt*)lfirst(lc);

        if (!IsA(plannedstmt, PlannedStmt))
            continue; /* Ignore utility statements */

        if (plannedstmt->transientPlan)
            is_transient = true;

        if (plannedstmt->dependsOnRole)
            plan->dependsOnRole = true;
    }

    if (is_transient) {
        Assert(TransactionIdIsNormal(u_sess->utils_cxt.TransactionXmin));
        plan->saved_xmin = u_sess->utils_cxt.TransactionXmin;
    } else
        plan->saved_xmin = InvalidTransactionId;
    plan->refcount = 0;
    plan->global_refcount = 0;
    plan->context = plan_context;
    plan->is_oneshot = plansource->is_oneshot;
    plan->is_saved = false;
    plan->is_valid = true;

    /* assign generation number to new plan */
    plan->generation = ++(plansource->generation);

    MemoryContextSwitchTo(oldcxt);

    /* Set plan real u_sess->attr.attr_sql.enable_stream_operator.*/
    plansource->stream_enabled = IsStreamSupport();
    //in shared hash table, we can not share the plan, we should throw error before this logic.

    plan->is_share = false;

    if (ENABLE_GPC) {
#ifdef ENABLE_MULTIPLE_NODES
        GPCCheckStreamPlan(plansource, plist);
#endif
        GPCFillPlanCache(plansource, isBuildingCustomPlan);
    }

    plpgsql_estate = saved_estate;

    return plan;
}

static void GPCFillPlanCache(CachedPlanSource* plansource, bool isBuildingCustomPlan)
{
    /* set flag is_support_gplan for plan not shared */
    if (!plansource->gpc.status.InShareTable()) {
        plansource->is_support_gplan = !isBuildingCustomPlan;
        if (ENABLE_CN_GPC)
            GPCReGplan(plansource);
        else if (ENABLE_DN_GPC && plansource->is_support_gplan && plansource->gpc.status.InPrepareStmt()) {
            /* add into first_saved_plan if not in */
            plansource->next_saved = u_sess->pcache_cxt.first_saved_plan;
            u_sess->pcache_cxt.first_saved_plan = plansource;
            plansource->is_saved = true;
            plansource->gpc.status.SetLoc(GPC_SHARE_IN_LOCAL_SAVE_PLAN_LIST);
        }
    }
    if (plansource->gpc.status.IsSharePlan() && !isBuildingCustomPlan) {
        pfree_ext(plansource->gpc.key);
        MemoryContext oldcxt = MemoryContextSwitchTo(plansource->context);
        plansource->gpc.key = (GPCKey*)palloc0(sizeof(GPCKey));
        plansource->gpc.key->query_string = plansource->query_string;
        plansource->gpc.key->query_length = strlen(plansource->query_string);
        plansource->gpc.key->spi_signature = plansource->spi_signature;
        GlobalPlanCache::EnvFill(&plansource->gpc.key->env, plansource->dependsOnRole);
        plansource->gpc.key->env.search_path = plansource->search_path;
        plansource->gpc.key->env.num_params = plansource->num_params;
        (void)MemoryContextSwitchTo(oldcxt);
    }
}

#ifdef ENABLE_MULTIPLE_NODES
/* If is stream plan, do not share it. 
 * We need different plannodeid for dn's stream consumer. */
static void GPCCheckStreamPlan(CachedPlanSource *plansource, List* plist)
{
    if (!ENABLE_CN_GPC)
        return;

    if (!plansource->gpc.status.IsSharePlan())
        return;

    ListCell* lc = NULL;
    bool is_stream_plan = false;
    foreach (lc, plist) {
        PlannedStmt* plannedstmt = (PlannedStmt*)lfirst(lc);
        if (IsA(plannedstmt, PlannedStmt)) {
            if (check_stream_plan(plannedstmt->planTree)) {
                is_stream_plan = true;
                break;
            }
        }
    }
    if (is_stream_plan)
        plansource->gpc.status.SetKind(GPC_UNSHARED);
}

static bool check_stream_plan(Plan* plan)
{
    if (plan == NULL)
        return false;

    if (IsA(plan, RemoteQuery) || IsA(plan, VecRemoteQuery)) {
        RemoteQuery* remote_query = (RemoteQuery*)plan;
        if (remote_query->is_simple)
            return true;
    }
    return check_stream_plan(plan->lefttree) || check_stream_plan(plan->righttree);
}

#endif

static bool IsDeleteLimit(CachedPlanSource* plansource, ParamListInfo boundParams)
{
    bool flag = false;
    if (IS_PGXC_COORDINATOR &&
        plansource->raw_parse_tree != NULL &&
        IsA(plansource->raw_parse_tree, DeleteStmt) &&
        ((DeleteStmt *)(plansource->raw_parse_tree))->limitClause != NULL &&
        boundParams != NULL) {
        flag = true;
    }
    return flag;
}


static bool IsForceCustomplan(CachedPlanSource *plansource) {
    bool flag = false;
    if (plansource->force_custom_plan) {
        if (IS_PGXC_DATANODE)
            flag = true;
        else if (IS_PGXC_COORDINATOR && !plansource->gplan_is_fqs)
            flag = true;
    }
    return flag;
}

static bool contain_ParamRef_walker(Node* node, void* context)
{
    if (node == NULL) {
        return false;
    }

    if (IsA(node, ParamRef)) {
        return true;
    }

    return raw_expression_tree_walker(node, (bool (*)())contain_ParamRef_walker, (void*)context);
}

static bool contain_ParamRef(Node* clause)
{
    return contain_ParamRef_walker(clause, NULL);
}

static bool is_upsert_query_with_update_param(Node* raw_parse_tree)
{
    if (raw_parse_tree != NULL && IsA(raw_parse_tree, InsertStmt)) {
        InsertStmt* stmt = (InsertStmt*)raw_parse_tree;
        if (stmt->upsertClause != NULL) {
            if (contain_ParamRef((Node*)(stmt->upsertClause->targetList))) {
                return true;
            }
        }
    }
    return false;
}

/*
 * ChooseCustomPlan: choose whether to use custom or generic plan
 *
 * This defines the policy followed by GetCachedPlan.
 */
static bool ChooseCustomPlan(CachedPlanSource* plansource, ParamListInfo boundParams)
{
    double avg_custom_cost;

    /*
     * Note: shared plancache need choose gplan, and shared plancache already has shared gplan.
     * DO NOT create cachedplan for shared plancache. Only create cachedplan for plancache not in GPC.
     */
    if (plansource->gpc.status.InShareTable()) {
        return false;
    }

    /* upsert with update query can't choose gplan */
    if (is_upsert_query_with_update_param(plansource->raw_parse_tree)) {
        return true;
    }

#ifdef ENABLE_MOT
    /* Don't choose custom plan if using pbe optimization and MOT engine. */
    if (u_sess->attr.attr_sql.enable_pbe_optimization && IsMOTEngineUsed()) {
        return false;
    }
#endif

    /* For PBE, such as col=$1+$2, generate cplan. */
    if (IsDeleteLimit(plansource, boundParams) == true) {
        return true;
    }

    /* One-shot plans will always be considered custom */
    if (plansource->is_oneshot)
        return true;

    /* Otherwise, never any point in a custom plan if there's no parameters */
    if (boundParams == NULL)
        return false;

    /* ... nor for transaction control statements */
    if (IsTransactionStmtPlan(plansource)) {
        return false;
    }

    /* See if caller wants to force the decision */
    if (plansource->cursor_options & CURSOR_OPT_GENERIC_PLAN)
        return false;
    if (plansource->cursor_options & CURSOR_OPT_CUSTOM_PLAN)
        return true;

    /* If we contains cstore table, always custom except fqs on CN */
    if (IsForceCustomplan(plansource) == true) {
        return true;
    }

    /* Don't choose custom plan if using pbe optimization */
    if (u_sess->attr.attr_sql.enable_pbe_optimization && plansource->gplan_is_fqs)
        return false;

    /* Let settings force the decision */
    if (unlikely(PLAN_CACHE_MODE_AUTO != u_sess->attr.attr_sql.g_planCacheMode)) {
        if (PLAN_CACHE_MODE_FORCE_GENERIC_PLAN == u_sess->attr.attr_sql.g_planCacheMode) {
            return false;
        }

        if (PLAN_CACHE_MODE_FORCE_CUSTOM_PLAN  == u_sess->attr.attr_sql.g_planCacheMode) {
            return true;
        }
    }

    /* Generate custom plans until we have done at least 5 (arbitrary) */
    if (plansource->num_custom_plans < 5)
        return true;

    avg_custom_cost = plansource->total_custom_cost / plansource->num_custom_plans;

    /*
     * Prefer generic plan if it's less than 10% more expensive than average
     * custom plan.  This threshold is a bit arbitrary; it'd be better if we
     * had some means of comparing planning time to the estimated runtime cost
     * differential.
     *
     * Note that if generic_cost is -1 (indicating we've not yet determined
     * the generic plan cost), we'll always prefer generic at this point.
     */
    if (plansource->generic_cost < avg_custom_cost * 1.1)
        return false;

    return true;
}

/*
 * cached_plan_cost: calculate estimated cost of a plan
 */
static double cached_plan_cost(CachedPlan* plan)
{
    double result = 0;
    ListCell* lc = NULL;

    foreach (lc, plan->stmt_list) {
        PlannedStmt* plannedstmt = (PlannedStmt*)lfirst(lc);

        if (!IsA(plannedstmt, PlannedStmt))
            continue; /* Ignore utility statements */

        result += plannedstmt->planTree->total_cost;
    }

    return result;
}

/*
 * GetCachedPlan: get a cached plan from a CachedPlanSource.
 *
 * This function hides the logic that decides whether to use a generic
 * plan or a custom plan for the given parameters: the caller does not know
 * which it will get.
 *
 * On return, the plan is valid and we have sufficient locks to begin
 * execution.
 *
 * On return, the refcount of the plan has been incremented; a later
 * ReleaseCachedPlan() call is expected.  The refcount has been reported
 * to the CurrentResourceOwner if useResOwner is true (note that that must
 * only be true if it's a "saved" CachedPlanSource).
 *
 * Note: if any replanning activity is required, the caller's memory context
 * is used for that work.
 */
CachedPlan* GetCachedPlan(CachedPlanSource* plansource, ParamListInfo boundParams, bool useResOwner)
{
    CachedPlan* plan = NULL;
    List* qlist = NIL;
    bool customplan = false;

    /* Assert caller is doing things in a sane order */
    Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
    Assert(plansource->is_complete);
    /* This seems worth a real test, though */
    if (useResOwner && !plansource->is_saved)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot apply ResourceOwner to non-saved cached plan")));

    /* Make sure the querytree list is valid and we have parse-time locks */
    qlist = RevalidateCachedQuery(plansource);

    /* Decide whether to use a custom plan */
    customplan = ChooseCustomPlan(plansource, boundParams);
    
    if (!customplan) {
        if (CheckCachedPlan(plansource)) {
            /* We want a generic plan, and we already have a valid one */
            plan = plansource->gplan;
            Assert(plan->magic == CACHEDPLAN_MAGIC);

            /* Update soft parse counter for Unique SQL */
            UniqueSQLStatCountSoftParse(1);
        } else {
            /* Whenever plan is rebuild, we need to drop the old one */
            ReleaseGenericPlan(plansource);
            /* Build a new generic plan */
            plan = BuildCachedPlan(plansource, qlist, NULL, customplan);
            Assert(!plan->isShared());


            /* Link the new generic plan into the plansource */
            plansource->gplan = plan;
            plan->refcount++;
            ResourceOwnerForgetGMemContext(t_thrd.utils_cxt.TopTransactionResourceOwner, plan->context);
            /* Immediately reparent into appropriate context */
            if (plansource->is_saved) {
                if (plansource->gpc.status.IsPrivatePlan()) {
                    /* saved plans all live under CacheMemoryContext */
                    MemoryContextSetParent(plan->context, u_sess->cache_mem_cxt);
                }
                plan->is_saved = true;
            } else {
                if (plansource->gpc.status.IsPrivatePlan()) {
                    /* otherwise, it should be a sibling of the plansource */
                    MemoryContextSetParent(plan->context,
                                           MemoryContextGetParent(plansource->context));
                }
            }
            /* Update generic_cost whenever we make a new generic plan */
            plansource->generic_cost = cached_plan_cost(plan);

            /*
             * Judge if gplan is single-node fqs, if so, we can use it when
             * enable_pbe_optimization is on
             */
            if (IS_PGXC_COORDINATOR && u_sess->attr.attr_sql.enable_pbe_optimization) {
                List* stmt_list = plan->stmt_list;
                bool plan_is_fqs = false;
                if (list_length(stmt_list) == 1) {
                    Node* pstmt = (Node*)linitial(stmt_list);
                    if (IsA(pstmt, PlannedStmt)) {
                        Plan* topPlan = ((PlannedStmt*)pstmt)->planTree;
                        if (IsA(topPlan, RemoteQuery)) {
                            RemoteQuery* rq = (RemoteQuery*)topPlan;
                            if (rq->exec_nodes &&
                                ((rq->exec_nodes->nodeList == NULL && rq->exec_nodes->en_expr != NULL) ||
                                    list_length(rq->exec_nodes->nodeList) == 1))
                                plan_is_fqs = true;
                        }
                    }
                }
                plansource->gplan_is_fqs = plan_is_fqs;
                ereport(
                    DEBUG2, (errmodule(MOD_OPT), errmsg("Custom plan is used for \"%s\"", plansource->query_string)));
            }

            /*
             * If, based on the now-known value of generic_cost, we'd not have
             * chosen to use a generic plan, then forget it and make a custom
             * plan.  This is a bit of a wart but is necessary to avoid a
             * glitch in behavior when the custom plans are consistently big
             * winners; at some point we'll experiment with a generic plan and
             * find it's a loser, but we don't want to actually execute that
             * plan.
             */
            customplan = ChooseCustomPlan(plansource, boundParams);

            /*
             * If we choose to plan again, we need to re-copy the query_list,
             * since the planner probably scribbled on it.	We can force
             * BuildCachedPlan to do that by passing NIL.
             */
            qlist = NIL;
        }
    }

    /* In function BuildCachedPlan, we deparse query to obtain sql_statement,
     * If we have generic plan, Sql_statement will have format parameter.
     * We can not send this statement to DN directly. So we should replace format
     * parameter with actual values. But, it will block pbe performance and
     * have side effects, such as unexpected query rewrite. So,we suggest that
     * u_sess->attr.attr_common.max_datanode_for_plan is 0 when testing performance.
     */
    if (u_sess->attr.attr_common.max_datanode_for_plan > 0 && !customplan && boundParams != NULL &&
        boundParams->params_need_process == true && IS_PGXC_COORDINATOR && !IsConnFromCoord()) {
        if (!plansource->gpc.status.InShareTable()) {
            /* we just replace params with virtual values and do not use plan. */
            CachedPlan* tmp_cplan = BuildCachedPlan(plansource, qlist, boundParams, customplan);
            /* Mark it no longer valid */
            tmp_cplan->magic = 0;
            /* One-shot plans do not own their context, so we can't free them */
            if (!tmp_cplan->is_oneshot) {
                ResourceOwnerForgetGMemContext(t_thrd.utils_cxt.TopTransactionResourceOwner, tmp_cplan->context);
                MemoryContextDelete(tmp_cplan->context);
            }
        } else {
            /* for shared plan , just copy a plansource for explain with values */
            CachedPlanSource* tmp_psrc = CopyCachedPlan(plansource, false);
            CachedPlan* tmp_cplan = BuildCachedPlan(tmp_psrc, NIL, boundParams, customplan);
            tmp_cplan->magic = 0;
            ResourceOwnerForgetGMemContext(t_thrd.utils_cxt.TopTransactionResourceOwner, tmp_cplan->context);
            MemoryContextDelete(tmp_cplan->context);
            tmp_psrc->magic = 0;
            ResourceOwnerForgetGMemContext(t_thrd.utils_cxt.TopTransactionResourceOwner, tmp_psrc->context);
            MemoryContextDelete(tmp_psrc->context);
        }
    }

    if (customplan) {
        /* Whenever plan is rebuild, we need to drop the old one */
        ReleaseGenericPlan(plansource);

        /* Build a custom plan */
        plan = BuildCachedPlan(plansource, qlist, boundParams, customplan);
        /* Link the new custome plan into the plansource */
        plansource->cplan = plan;
        plan->refcount++;
        ResourceOwnerForgetGMemContext(t_thrd.utils_cxt.TopTransactionResourceOwner, plan->context);
        
        if (plansource->is_saved) {
            if (plansource->gpc.status.IsPrivatePlan()) {
                /* saved plans all live under CacheMemoryContext */
                MemoryContextSetParent(plan->context, u_sess->cache_mem_cxt);
            }
            plan->is_saved = true;
        } else {
            if (plansource->gpc.status.IsPrivatePlan()) {
                /* otherwise, it should be a sibling of the plansource */
                MemoryContextSetParent(plan->context,
                                       MemoryContextGetParent(plansource->context));
            }
        }

        /* Accumulate total costs of custom plans, but 'ware overflow */
        if (plansource->num_custom_plans < INT_MAX) {
            plansource->total_custom_cost += cached_plan_cost(plan);
            plansource->num_custom_plans++;
        }

        ereport(DEBUG2, (errmodule(MOD_OPT), errmsg("Custom plan is used for \"%s\"", plansource->query_string)));
    } else {
        ereport(DEBUG2, (errmodule(MOD_OPT), errmsg("Generic plan is used for \"%s\"", plansource->query_string)));
    }

    /*
     * Validate the plansource again
     * The plansource may be invalidated by ResetPlanCache when handling invalid
     * messages in BuildCachedPlan.
     */
    plansource->is_valid = true;

    /* Flag the plan as in use by caller */
    if (useResOwner)
        ResourceOwnerEnlargePlanCacheRefs(t_thrd.utils_cxt.CurrentResourceOwner);
    plan->refcount++;
    if (useResOwner)
        ResourceOwnerRememberPlanCacheRef(t_thrd.utils_cxt.CurrentResourceOwner, plan);

    /*
     * Saved plans should be under u_sess->cache_mem_cxt so they will not go away
     * until their reference count goes to zero.  In the generic-plan cases we
     * already took care of that, but for a custom plan, do it as soon as we
     * have created a reference-counted link.
     */
    if (customplan && plansource->is_saved) {
	    if (plansource->gpc.status.IsPrivatePlan()) {
		    MemoryContextSetParent(plan->context, u_sess->cache_mem_cxt);
        }
        plan->is_saved = true;
    }

    if (!customplan) {
        setCachedPlanBucketId(plan, boundParams);
    }

#ifdef ENABLE_MOT
    /* set plan storageEngineType */
    plan->storageEngineType = plansource->storageEngineType;
    plan->mot_jit_context = plansource->mot_jit_context;
#endif

    ListCell *lc;
    foreach (lc, plan->stmt_list) {
        PlannedStmt* plannedstmt = (PlannedStmt*)lfirst(lc);

        if (!IsA(plannedstmt, PlannedStmt))
            continue; /* Ignore utility statements */

        check_gtm_free_plan(plannedstmt, u_sess->attr.attr_sql.explain_allow_multinode ? WARNING : ERROR);
    }

    return plan;
}

/*
 * Find and release all Datanode statements referenced by the plan node and subnodes
 */
#ifdef PGXC
static void drop_datanode_statements(Plan* plannode)
{
    if (IsA(plannode, RemoteQuery)) {
        RemoteQuery* step = (RemoteQuery*)plannode;

        if (step->statement)
            DropDatanodeStatement(step->statement);
    } else if (IsA(plannode, ModifyTable)) {
        ModifyTable* mt_plan = (ModifyTable*)plannode;
        /* For ModifyTable plan recurse into each of the plans underneath */
        ListCell* l = NULL;
        foreach (l, mt_plan->plans) {
            Plan* plan = (Plan*)lfirst(l);
            drop_datanode_statements(plan);
        }
    }

    if (innerPlan(plannode))
        drop_datanode_statements(innerPlan(plannode));

    if (outerPlan(plannode))
        drop_datanode_statements(outerPlan(plannode));
}
#endif

void ReleaseSharedCachedPlan(CachedPlan* plan, bool useResOwner)
{
    if (useResOwner) {
        Assert(plan->is_saved);
        ResourceOwnerForgetPlanCacheRef(t_thrd.utils_cxt.CurrentResourceOwner, plan);
    }
    Assert(plan->global_refcount > 0);
    /* we only delete the plan's context when global plancache is off or the plancache is private */
    if (pg_atomic_sub_fetch_u32((volatile uint32*)&plan->global_refcount, 1) == 0) {
        /* TopTransactionResourceOwner is NULL when thread exit */
        if (t_thrd.utils_cxt.TopTransactionResourceOwner)
            ResourceOwnerForgetGMemContext(t_thrd.utils_cxt.TopTransactionResourceOwner, plan->context);
        /* Mark it no longer valid */
        plan->magic = 0;
        MemoryContextUnSeal(plan->context);
        MemoryContextDelete(plan->context);
    }
}

/*
 * ReleaseCachedPlan: release active use of a cached plan.
 *
 * This decrements the reference count, and frees the plan if the count
 * has thereby gone to zero.  If useResOwner is true, it is assumed that
 * the reference count is managed by the CurrentResourceOwner.
 *
 * Note: useResOwner = false is used for releasing references that are in
 * persistent data structures, such as the parent CachedPlanSource or a
 * Portal.	Transient references should be protected by a resource owner.
 */
void ReleaseCachedPlan(CachedPlan* plan, bool useResOwner)
{
    Assert(plan != NULL);
    Assert(plan->magic == CACHEDPLAN_MAGIC);
    if (plan->isShared()) {
        ReleaseSharedCachedPlan(plan, useResOwner);
        return;
    }
    if (useResOwner) {
        Assert(plan->is_saved);
        ResourceOwnerForgetPlanCacheRef(t_thrd.utils_cxt.CurrentResourceOwner, plan);
    }
    Assert(plan->refcount > 0);
    plan->refcount--;
    /* we only delete the plan's context when global plancache is off or the plancache is private */
    if (plan->refcount == 0) {
        /* Mark it no longer valid */
        plan->magic = 0;

        /* One-shot plans do not own their context, so we can't free them */
        if (!plan->is_oneshot) {
            Assert (!plan->isShared());
            /* TopTransactionResourceOwner is NULL when thread exit */
            if (t_thrd.utils_cxt.TopTransactionResourceOwner)
                ResourceOwnerForgetGMemContext(t_thrd.utils_cxt.TopTransactionResourceOwner, plan->context);
            MemoryContextDelete(plan->context);
        }
    }
}

/*
 * CachedPlanSetParentContext: move a CachedPlanSource to a new memory context
 *
 * This can only be applied to unsaved plans; once saved, a plan always
 * lives underneath u_sess->cache_mem_cxt.
 */
void CachedPlanSetParentContext(CachedPlanSource* plansource, MemoryContext newcontext)
{
    /* Assert caller is doing things in a sane order */
    Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
    Assert(plansource->is_complete);

    /* These seem worth real tests, though */
    if (plansource->is_saved)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot move a saved cached plan to another context")));
    if (plansource->is_oneshot)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot move a one-shot cached plan to another context")));

    if (plansource->gpc.status.IsPrivatePlan()) {
        /* OK, let the caller keep the plan where he wishes */
        MemoryContextSetParent(plansource->context, newcontext);
    }

    /*
     * The query_context needs no special handling, since it's a child of
     * plansource->context.  But if there's a generic plan, it should be
     * maintained as a sibling of plansource->context.
     */
    if (plansource->gplan) {
        Assert(plansource->gplan->magic == CACHEDPLAN_MAGIC);
        if (plansource->gpc.status.IsPrivatePlan()) {
            MemoryContextSetParent(plansource->gplan->context, newcontext);
        }
    }

    if (plansource->cplan) {
        Assert(plansource->cplan->magic == CACHEDPLAN_MAGIC);
        if (plansource->gpc.status.IsPrivatePlan()) {
            MemoryContextSetParent(plansource->cplan->context, newcontext);
        }
    }
}

/*
 * CopyCachedPlan: make a copy of a CachedPlanSource
 *
 * This is a convenience routine that does the equivalent of
 * CreateCachedPlan + CompleteCachedPlan, using the data stored in the
 * input CachedPlanSource.	The result is therefore "unsaved" (regardless
 * of the state of the source), and we don't copy any generic plan either.
 * The result will be currently valid, or not, the same as the source.
 */
CachedPlanSource* CopyCachedPlan(CachedPlanSource* plansource, bool is_share)
{
    CachedPlanSource* newsource = NULL;
    MemoryContext source_context;
    MemoryContext querytree_context;
    MemoryContext oldcxt;

    Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
    Assert(plansource->is_complete);

    /*
     * One-shot plans can't be copied, because we haven't taken care that
     * parsing/planning didn't scribble on the raw parse tree or querytrees.
     */
    if (plansource->is_oneshot)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot copy a one-shot cached plan")));

    if (ENABLE_GPC && is_share == true) {
    	source_context = AllocSetContextCreate(GLOBAL_PLANCACHE_MEMCONTEXT,
    										   "GPCCachedPlanSource",
    										   ALLOCSET_SMALL_MINSIZE,
    										   ALLOCSET_SMALL_INITSIZE,
    										   ALLOCSET_DEFAULT_MAXSIZE,
    										   SHARED_CONTEXT);
    } else {
    	source_context = AllocSetContextCreate(u_sess->cache_mem_cxt,
    										   "CachedPlanSource",
    										   ALLOCSET_SMALL_MINSIZE,
    										   ALLOCSET_SMALL_INITSIZE,
    										   ALLOCSET_DEFAULT_MAXSIZE);
    }


    oldcxt = MemoryContextSwitchTo(source_context);

    newsource = (CachedPlanSource*)palloc0(sizeof(CachedPlanSource));
    newsource->magic = CACHEDPLANSOURCE_MAGIC;
    newsource->raw_parse_tree = (Node*)copyObject(plansource->raw_parse_tree);
    newsource->query_string = pstrdup(plansource->query_string);
    newsource->commandTag = plansource->commandTag;
    if (plansource->num_params > 0) {
        newsource->param_types = (Oid*)palloc(plansource->num_params * sizeof(Oid));
        errno_t rc = memcpy_s(newsource->param_types,
            plansource->num_params * sizeof(Oid),
            plansource->param_types,
            plansource->num_params * sizeof(Oid));
        securec_check(rc, "\0", "\0");
    } else
        newsource->param_types = NULL;
    newsource->num_params = plansource->num_params;
    newsource->parserSetup = plansource->parserSetup;
    newsource->parserSetupArg = plansource->parserSetupArg;
    newsource->cursor_options = plansource->cursor_options;
    newsource->rewriteRoleId = plansource->rewriteRoleId;
    newsource->dependsOnRole = plansource->dependsOnRole;
    newsource->fixed_result = plansource->fixed_result;
    if (plansource->resultDesc)
        newsource->resultDesc = CreateTupleDescCopy(plansource->resultDesc);
    else
        newsource->resultDesc = NULL;
    newsource->context = source_context;

    if (ENABLE_GPC && is_share == true) {
    	querytree_context = AllocSetContextCreate(source_context,
    											  "GPCCachedPlanQuery",
    											  ALLOCSET_SMALL_MINSIZE,
    											  ALLOCSET_SMALL_INITSIZE,
    											  ALLOCSET_DEFAULT_MAXSIZE,
    											  SHARED_CONTEXT);
    }
    else {
    	querytree_context = AllocSetContextCreate(source_context,
    											  "CachedPlanQuery",
    											  ALLOCSET_SMALL_MINSIZE,
    											  ALLOCSET_SMALL_INITSIZE,
    											  ALLOCSET_DEFAULT_MAXSIZE);
    }
    MemoryContextSwitchTo(querytree_context);
    newsource->query_list = (List*)copyObject(plansource->query_list);
    newsource->relationOids = (List*)copyObject(plansource->relationOids);
    newsource->invalItems = (List*)copyObject(plansource->invalItems);
    if (plansource->search_path) {
        newsource->search_path = CopyOverrideSearchPath(plansource->search_path);
    }
    newsource->query_context = querytree_context;
    newsource->gplan = NULL;

    newsource->is_oneshot = false;
    newsource->is_complete = true;
    newsource->is_saved = false;
    newsource->is_valid = plansource->is_valid;
    newsource->generation = plansource->generation;
    newsource->next_saved = NULL;

    /* We may as well copy any acquired cost knowledge */
    newsource->generic_cost = plansource->generic_cost;
    newsource->total_custom_cost = plansource->total_custom_cost;
    newsource->num_custom_plans = plansource->num_custom_plans;
    newsource->opFusionObj = NULL;
    newsource->is_checked_opfusion = false;
    newsource->spi_signature = plansource->spi_signature;

#ifdef ENABLE_MOT
    newsource->storageEngineType = SE_TYPE_UNSPECIFIED;
    newsource->mot_jit_context = NULL;
#endif

#ifdef PGXC
    newsource->stream_enabled = plansource->stream_enabled;
    if (!is_share) {
        plansource->cplan = NULL;
        plansource->single_exec_node = NULL;
        plansource->is_read_only = false;
        plansource->lightProxyObj = NULL;
    }
#endif

    MemoryContextSwitchTo(oldcxt);
    if (ENABLE_GPC && is_share)
        GPC_LOG("copy plan when recreate", newsource, 0);

    return newsource;
}

/*
 * CachedPlanIsValid: test whether the rewritten querytree within a
 * CachedPlanSource is currently valid (that is, not marked as being in need
 * of revalidation).
 *
 * This result is only trustworthy (ie, free from race conditions) if
 * the caller has acquired locks on all the relations used in the plan.
 */
bool CachedPlanIsValid(CachedPlanSource* plansource)
{
    Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
    return plansource->is_valid;
}

/*
 * CachedPlanGetTargetList: return tlist, if any, describing plan's output
 *
 * The result is guaranteed up-to-date.  However, it is local storage
 * within the cached plan, and may disappear next time the plan is updated.
 */
List* CachedPlanGetTargetList(CachedPlanSource* plansource)
{
    Node* pstmt = NULL;

    /* Assert caller is doing things in a sane order */
    Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);
    Assert(plansource->is_complete);

    /*
     * No work needed if statement doesn't return tuples (we assume this
     * feature cannot be changed by an invalidation)
     */
    if (plansource->resultDesc == NULL)
        return NIL;

    /* Make sure the querytree list is valid and we have parse-time locks */
    (void)RevalidateCachedQuery(plansource);

    /* Get the primary statement and find out what it returns */
    pstmt = PortalListGetPrimaryStmt(plansource->query_list);

    return FetchStatementTargetList(pstmt);
}

/*
 * AcquireExecutorLocks: acquire locks needed for execution of a cached plan;
 * or release them if acquire is false.
 */
static void AcquireExecutorLocks(List* stmt_list, bool acquire)
{
    ListCell* lc1 = NULL;

    foreach (lc1, stmt_list) {
        PlannedStmt* plannedstmt = (PlannedStmt*)lfirst(lc1);
        int rt_index;
        ListCell* lc2 = NULL;

        Assert(!IsA(plannedstmt, Query));
        if (!IsA(plannedstmt, PlannedStmt)) {
            /*
             * Ignore utility statements, except those (such as EXPLAIN) that
             * contain a parsed-but-not-planned query.	Note: it's okay to use
             * ScanQueryForLocks, even though the query hasn't been through
             * rule rewriting, because rewriting doesn't change the query
             * representation.
             */
            Query* query = UtilityContainsQuery((Node*)plannedstmt);

            if (query != NULL)
                ScanQueryForLocks(query, acquire);
            continue;
        }

        rt_index = 0;
        foreach (lc2, plannedstmt->rtable) {
            RangeTblEntry* rte = (RangeTblEntry*)lfirst(lc2);
            LOCKMODE lockmode;
            PlanRowMark* rc = NULL;

            rt_index++;

            if (rte->rtekind != RTE_RELATION)
                continue;

            /*
             * Acquire the appropriate type of lock on each relation OID. Note
             * that we don't actually try to open the rel, and hence will not
             * fail if it's been dropped entirely --- we'll just transiently
             * acquire a non-conflicting lock.
             */
            if (list_member_int(plannedstmt->resultRelations, rt_index))
                lockmode = RowExclusiveLock;
            else if ((rc = get_plan_rowmark(plannedstmt->rowMarks, rt_index)) != NULL &&
                     RowMarkRequiresRowShareLock(rc->markType))
                lockmode = RowShareLock;
            else
                lockmode = AccessShareLock;

            if (acquire)
                LockRelationOid(rte->relid, lockmode);
            else
                UnlockRelationOid(rte->relid, lockmode);
        }
    }
}

/*
 * AcquirePlannerLocks: acquire locks needed for planning of a querytree list;
 * or release them if acquire is false.
 *
 * Note that we don't actually try to open the relations, and hence will not
 * fail if one has been dropped entirely --- we'll just transiently acquire
 * a non-conflicting lock.
 */
static void AcquirePlannerLocks(List* stmt_list, bool acquire)
{
    ListCell* lc = NULL;

    foreach (lc, stmt_list) {
        Query* query = (Query*)lfirst(lc);

        Assert(IsA(query, Query));

        if (query->commandType == CMD_UTILITY) {
            /* Ignore utility statements, unless they contain a Query */
            query = UtilityContainsQuery(query->utilityStmt);
            if (query != NULL)
                ScanQueryForLocks(query, acquire);
            continue;
        }

        ScanQueryForLocks(query, acquire);
    }
}

/*
 * ScanQueryForLocks: recursively scan one Query for AcquirePlannerLocks.
 */
static void ScanQueryForLocks(Query* parsetree, bool acquire)
{
    ListCell* lc = NULL;
    int rt_index;

    /* Shouldn't get called on utility commands */
    Assert(parsetree->commandType != CMD_UTILITY);

    /*
     * First, process RTEs of the current query level.
     */
    rt_index = 0;
    foreach (lc, parsetree->rtable) {
        RangeTblEntry* rte = (RangeTblEntry*)lfirst(lc);
        LOCKMODE lockmode;

        rt_index++;
        switch (rte->rtekind) {
            case RTE_RELATION:
                /* Acquire or release the appropriate type of lock */
                if (rt_index == parsetree->resultRelation)
                    lockmode = RowExclusiveLock;
                else if (get_parse_rowmark(parsetree, rt_index) != NULL)
                    lockmode = RowShareLock;
                else
                    lockmode = AccessShareLock;
                if (acquire)
                    LockRelationOid(rte->relid, lockmode);
                else
                    UnlockRelationOid(rte->relid, lockmode);
                break;

            case RTE_SUBQUERY:
                /* Recurse into subquery-in-FROM */
                ScanQueryForLocks(rte->subquery, acquire);
                break;

            default:
                /* ignore other types of RTEs */
                break;
        }
    }

    /* Recurse into subquery-in-WITH */
    foreach (lc, parsetree->cteList) {
        CommonTableExpr* cte = (CommonTableExpr*)lfirst(lc);

        ScanQueryForLocks((Query*)cte->ctequery, acquire);
    }

    /*
     * Recurse into sublink subqueries, too.  But we already did the ones in
     * the rtable and cteList.
     */
    if (parsetree->hasSubLinks) {
        query_tree_walker(parsetree, (bool (*)())ScanQueryWalker, (void*)&acquire, QTW_IGNORE_RC_SUBQUERIES);
    }
}

/*
 * Walker to find sublink subqueries for ScanQueryForLocks
 */
static bool ScanQueryWalker(Node* node, bool* acquire)
{
    if (node == NULL)
        return false;
    if (IsA(node, SubLink)) {
        SubLink* sub = (SubLink*)node;

        /* Do what we came for */
        ScanQueryForLocks((Query*)sub->subselect, *acquire);
        /* Fall through to process lefthand args of SubLink */
    }

    /*
     * Do NOT recurse into Query nodes, because ScanQueryForLocks already
     * processed subselects of subselects for us.
     */
    return expression_tree_walker(node, (bool (*)())ScanQueryWalker, (void*)acquire);
}

/*
 * PlanCacheComputeResultDesc: given a list of analyzed-and-rewritten Queries,
 * determine the result tupledesc it will produce.	Returns NULL if the
 * execution will not return tuples.
 *
 * Note: the result is created or copied into current memory context.
 */
static TupleDesc PlanCacheComputeResultDesc(List* stmt_list)
{
    Query* query = NULL;

    switch (ChoosePortalStrategy(stmt_list)) {
        case PORTAL_ONE_SELECT:
        case PORTAL_ONE_MOD_WITH:
            query = (Query*)linitial(stmt_list);
            Assert(IsA(query, Query));
            return ExecCleanTypeFromTL(query->targetList, false);

        case PORTAL_ONE_RETURNING:
            query = (Query*)PortalListGetPrimaryStmt(stmt_list);
            Assert(IsA(query, Query));
            Assert(query->returningList);
            return ExecCleanTypeFromTL(query->returningList, false);

        case PORTAL_UTIL_SELECT:
            query = (Query*)linitial(stmt_list);
            Assert(IsA(query, Query));
            Assert(query->utilityStmt);
            return UtilityTupleDescriptor(query->utilityStmt);

        case PORTAL_MULTI_QUERY:
            /* will not return tuples */
            break;
        default:
            break;
    }
    return NULL;
}

/*
 * CheckRelDependency: go through the plansource's dependency list to see if it
 * depends on the given relation by ID.
 */
void
CheckRelDependency(CachedPlanSource *plansource, Oid relid)
{
	/*
	 * Check the dependency list for the rewritten querytree.
	 */
	if ((relid == InvalidOid) ? (plansource->relationOids != NIL) :
		list_member_oid(plansource->relationOids, relid))
	{
	    if (plansource->gpc.status.InShareTable()) {
            CN_GPC_LOG("invalid shared in CheckRelDependency", plansource, 0);
            plansource->gpc.status.SetStatus(GPC_INVALID);
            return;
        } else {
    		/* Invalidate the querytree and generic plan */
            CN_GPC_LOG("invalid in CheckRelDependency", plansource, 0);
    		plansource->is_valid = false;
    		if (plansource->gplan) {
    			plansource->gplan->is_valid = false;
            }
        }
	}

	/*
	 * The generic plan, if any, could have more dependencies than the
	 * querytree does, so we have to check it too.
	 */
	if (plansource->gplan && plansource->gplan->is_valid)
	{
		ListCell   *lc = NULL;

		foreach(lc, plansource->gplan->stmt_list)
		{
			PlannedStmt *plannedstmt = (PlannedStmt *) lfirst(lc);

			Assert(!IsA(plannedstmt, Query));
			if (!IsA(plannedstmt, PlannedStmt))
				continue;	/* Ignore utility statements */
			if ((relid == InvalidOid) ? (plannedstmt->relationOids != NIL) :
				list_member_oid(plannedstmt->relationOids, relid))
			{
        	    if (plansource->gpc.status.InShareTable()) {
                    plansource->gpc.status.SetStatus(GPC_INVALID);
                    return;
                } else {
    				/* Invalidate the generic plan only */
    				plansource->gplan->is_valid = false;
                }
				break;		/* out of stmt_list scan */
			}
		}
	}
}

/*
 * CheckInvalItemDependency: go through the plansource's dependency list to see if it
 * depends on the given object by ID.
 */
void
CheckInvalItemDependency(CachedPlanSource *plansource, int cacheid, uint32 hashvalue)
{
    ListCell   *lc = NULL;

	/*
	 * Check the dependency list for the rewritten querytree.
	 */
	foreach(lc, plansource->invalItems)
	{
		PlanInvalItem *item = (PlanInvalItem *) lfirst(lc);

		if (item->cacheId != cacheid)
			continue;
		if (hashvalue == 0 ||
			item->hashValue == hashvalue)
		{
    	    if (plansource->gpc.status.InShareTable()) {
                CN_GPC_LOG("invalid shared in CheckInvalItemDependency", plansource, 0);
                plansource->gpc.status.SetStatus(GPC_INVALID);
                return;
            } else {
    			/* Invalidate the querytree and generic plan */
                CN_GPC_LOG("invalid in CheckInvalItemDependency", plansource, 0);
    			plansource->is_valid = false;
    			if (plansource->gplan) {
    				plansource->gplan->is_valid = false;
                }
            }
			break;
		}
	}

	/*
	 * The generic plan, if any, could have more dependencies than the
	 * querytree does, so we have to check it too.
	 */
	if (plansource->gplan && plansource->gplan->is_valid)
	{
		foreach(lc, plansource->gplan->stmt_list)
		{
			PlannedStmt *plannedstmt = (PlannedStmt *) lfirst(lc);
			ListCell   *lc3 = NULL;

			Assert(!IsA(plannedstmt, Query));
			if (!IsA(plannedstmt, PlannedStmt))
				continue;	/* Ignore utility statements */
			foreach(lc3, plannedstmt->invalItems)
			{
				PlanInvalItem *item = (PlanInvalItem *) lfirst(lc3);

				if (item->cacheId != cacheid)
					continue;
				if (hashvalue == 0 ||
					item->hashValue == hashvalue)
				{
            	    if (plansource->gpc.status.InShareTable()) {
                        plansource->gpc.status.SetStatus(GPC_INVALID);
                        return;
                    } else {
    					/* Invalidate the generic plan only */
    					plansource->gplan->is_valid = false;
                    }
					break;	/* out of invalItems scan */
				}
			}
			if (!plansource->gplan->is_valid)
				break;		/* out of stmt_list scan */
		}
	}
}

/*
 * PlanCacheRelCallback
 *		Relcache inval callback function
 *
 * Invalidate all plans mentioning the given rel, or all plans mentioning
 * any rel at all if relid == InvalidOid.
 */
void PlanCacheRelCallback(Datum arg, Oid relid)
{
    CachedPlanSource* plansource = NULL;

    for (plansource = u_sess->pcache_cxt.first_saved_plan; plansource; plansource = plansource->next_saved) {
        Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);

        /* No work if it's already invalidated */
        if (!plansource->is_valid)
            continue;

        /* Never invalidate transaction control commands */
        if (IsTransactionStmtPlan(plansource)) {
            continue;
        }

        CheckRelDependency(plansource, relid);
    }

    if (ENABLE_CN_GPC) {
        for (plansource = u_sess->pcache_cxt.ungpc_saved_plan; plansource; plansource = plansource->next_saved) {
            Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);

            /* No work if it's already invalidated */
            if (!plansource->is_valid)
                continue;

            /* Never invalidate transaction control commands */
            if (IsTransactionStmtPlan(plansource)) {
                continue;
            }

            CheckRelDependency(plansource, relid);
        }
    }
}

/*
 * PlanCacheFuncCallback
 *		Syscache inval callback function for PROCOID cache
 *
 * Invalidate all plans mentioning the object with the specified hash value,
 * or all plans mentioning any member of this cache if hashvalue == 0.
 *
 * Note that the coding would support use for multiple caches, but right
 * now only user-defined functions are tracked this way.
 */
void PlanCacheFuncCallback(Datum arg, int cacheid, uint32 hashvalue)
{
    CachedPlanSource* plansource = NULL;

    for (plansource = u_sess->pcache_cxt.first_saved_plan; plansource; plansource = plansource->next_saved) {
        Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);

        /* No work if it's already invalidated */
        if (!plansource->is_valid)
            continue;

        /* Never invalidate transaction control commands */
        if (IsTransactionStmtPlan(plansource)) {
            continue;
        }
        CheckInvalItemDependency(plansource, cacheid, hashvalue);
    }
    if (ENABLE_CN_GPC) {
        for (plansource = u_sess->pcache_cxt.ungpc_saved_plan; plansource; plansource = plansource->next_saved) {
            Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);

            /* No work if it's already invalidated */
            if (!plansource->is_valid)
                continue;

            /* Never invalidate transaction control commands */
            if (IsTransactionStmtPlan(plansource)) {
                continue;
            }
            CheckInvalItemDependency(plansource, cacheid, hashvalue);
        }
    }
}

/*
 * PlanCacheSysCallback
 *		Syscache inval callback function for other caches
 *
 * Just invalidate everything...
 */
void PlanCacheSysCallback(Datum arg, int cacheid, uint32 hashvalue)
{
    ResetPlanCache();
}

/*
 * ResetPlanCache: invalidate a cached plans.
 */
void
ResetPlanCache(CachedPlanSource *plansource)
{
    ListCell   *lc = NULL;

	/*
	 * In general there is no point in invalidating utility statements
	 * since they have no plans anyway.  So invalidate it only if it
	 * contains at least one non-utility statement, or contains a utility
	 * statement that contains a pre-analyzed query (which could have
	 * dependencies.)
	 */
	foreach(lc, plansource->query_list)
	{
		Query	   *query = (Query *) lfirst(lc);

		Assert(IsA(query, Query));
		if (query->commandType != CMD_UTILITY ||
			UtilityContainsQuery(query->utilityStmt))
		{
		    /* non-utility statement, so invalidate */
    	    if (plansource->gpc.status.InShareTable()) {
                CN_GPC_LOG("invalid shared in ResetPlanCache", plansource, 0);
                plansource->gpc.status.SetStatus(GPC_INVALID);
                return;
            } else {
    			plansource->is_valid = false;
                CN_GPC_LOG("invalid in ResetPlanCache", plansource, 0);
    			if (plansource->gplan) {
    				plansource->gplan->is_valid = false;
                }
            }
			/* no need to look further */
			break;
		}
	}
}

/*
 * ResetPlanCache: invalidate all cached plans.
 */
void ResetPlanCache(void)
{
    CachedPlanSource* plansource = NULL;

    for (plansource = u_sess->pcache_cxt.first_saved_plan; plansource; plansource = plansource->next_saved) {
        Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);

        /* No work if it's already invalidated */
        if (!plansource->is_valid)
            continue;

        /*
         * We *must not* mark transaction control statements as invalid,
         * particularly not ROLLBACK, because they may need to be executed in
         * aborted transactions when we can't revalidate them (cf bug #5269).
         */
        if (IsTransactionStmtPlan(plansource)) {
            continue;
        }

        ResetPlanCache(plansource);
    }
    if (ENABLE_CN_GPC) {
        for (plansource = u_sess->pcache_cxt.ungpc_saved_plan; plansource; plansource = plansource->next_saved) {
            Assert(plansource->magic == CACHEDPLANSOURCE_MAGIC);

            /* No work if it's already invalidated */
            if (!plansource->is_valid)
                continue;

            /*
             * We *must not* mark transaction control statements as invalid,
             * particularly not ROLLBACK, because they may need to be executed in
             * aborted transactions when we can't revalidate them (cf bug #5269).
             */
            if (IsTransactionStmtPlan(plansource)) {
                continue;
            }

            ResetPlanCache(plansource);
        }
    }
}

/*
 * Drop lightProxyObj/gplan/cplan inside CachedPlanSource,
 * and drop prepared statements on DN.
 */
void DropCachedPlanInternal(CachedPlanSource* plansource)
{
#ifdef ENABLE_MOT
    /* MOT: clean any JIT context */
    if (plansource->mot_jit_context) {
        JitExec::DestroyJitContext(plansource->mot_jit_context);
        plansource->mot_jit_context = NULL;
    }
#endif

    if (plansource->lightProxyObj != NULL) {
        /* always use light proxy */
        Assert(plansource->gplan == NULL && plansource->cplan == NULL);

        lightProxy* lp = (lightProxy*)plansource->lightProxyObj;
        if (lp->m_cplan->stmt_name) {
            lp->m_entry = NULL;
            DropDatanodeStatement(lp->m_cplan->stmt_name);
        }
        if (lp->m_portalName == NULL || lightProxy::locateLightProxy(lp->m_portalName) == NULL) {
            lightProxy::tearDown(lp);
        }
        plansource->lightProxyObj = NULL;
    } else {
        if (plansource->opFusionObj != NULL) {
            OpFusion *opfusion = (OpFusion *)plansource->opFusionObj;
            if (opfusion->m_portalName == NULL || OpFusion::locateFusion(opfusion->m_portalName) == NULL) {
                OpFusion::tearDown(opfusion);
            } else {
                opfusion->m_psrc = NULL;
            }
            plansource->opFusionObj = NULL;
        }
        /* Decrement generic CachePlan's refcount and drop if no longer needed */
        ReleaseGenericPlan(plansource);
    }

#ifdef MEMORY_CONTEXT_CHECKING
    CachedPlanSource* cur_plansource = u_sess->pcache_cxt.first_saved_plan;
    while (cur_plansource != NULL) {
        Assert(cur_plansource->magic == CACHEDPLANSOURCE_MAGIC);
        cur_plansource = cur_plansource->next_saved;
    }
    if (ENABLE_CN_GPC) {
        cur_plansource = u_sess->pcache_cxt.ungpc_saved_plan;
        while (cur_plansource != NULL) {
            Assert(cur_plansource->magic == CACHEDPLANSOURCE_MAGIC);
            cur_plansource = cur_plansource->next_saved;
        }
    }
#endif
}
