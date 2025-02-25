/* -------------------------------------------------------------------------
 *
 * nodeIndexonlyscan.cpp
 *	  Routines to support index-only scans
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/gausskernel/runtime/executor/nodeIndexonlyscan.cpp
 *
 * -------------------------------------------------------------------------
 *
 * INTERFACE ROUTINES
 *		ExecIndexOnlyScan			scans an index
 *		IndexOnlyNext				retrieve next tuple
 *		ExecInitIndexOnlyScan		creates and initializes state info.
 *		ExecReScanIndexOnlyScan		rescans the indexed relation.
 *		ExecEndIndexOnlyScan		releases all storage.
 *		ExecIndexOnlyMarkPos		marks scan position.
 *		ExecIndexOnlyRestrPos		restores scan position.
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/relscan.h"
#include "access/visibilitymap.h"
#include "access/tableam.h"
#include "catalog/pg_partition_fn.h"
#include "executor/execdebug.h"
#include "executor/nodeIndexonlyscan.h"
#include "executor/nodeIndexscan.h"
#include "storage/buf/bufmgr.h"
#include "storage/predicate.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/rel_gs.h"
#include "nodes/makefuncs.h"
#include "optimizer/pruning.h"


static TupleTableSlot* IndexOnlyNext(IndexOnlyScanState* node);
static void ExecInitNextIndexPartitionForIndexScanOnly(IndexOnlyScanState* node);

/* ----------------------------------------------------------------
 *		ReleaseNodeVMBuffer
 *
 *		Release VM buffer pin, if any.
 * ----------------------------------------------------------------
 */
static void ReleaseNodeVMBuffer(IndexOnlyScanState* node)
{
    if (node->ioss_VMBuffer != InvalidBuffer) {
        ReleaseBuffer(node->ioss_VMBuffer);
        node->ioss_VMBuffer = InvalidBuffer;
    }
}

/* ----------------------------------------------------------------
 *		ExecGPIGetNextPartRelation
 * ----------------------------------------------------------------
 */
static bool ExecGPIGetNextPartRelation(IndexOnlyScanState* node, IndexScanDesc indexScan)
{
    if (IndexScanNeedSwitchPartRel(indexScan)) {
        /* Release VM buffer pin, if any. */
        ReleaseNodeVMBuffer(node);
        /* Change the heapRelation in indexScanDesc to Partition Relation of current index */
        if (!GPIGetNextPartRelation(indexScan->xs_gpi_scan, CurrentMemoryContext, AccessShareLock)) {
            return false;
        }
        indexScan->heapRelation = indexScan->xs_gpi_scan->fakePartRelation;
    }

    return true;
}

/* ----------------------------------------------------------------
 *		IndexOnlyNext
 *
 *		Retrieve a tuple from the IndexOnlyScan node's index.
 * ----------------------------------------------------------------
 */
static TupleTableSlot* IndexOnlyNext(IndexOnlyScanState* node)
{
    EState* estate = NULL;
    ExprContext* econtext = NULL;
    ScanDirection direction;
    IndexScanDesc scandesc;
    TupleTableSlot* slot = NULL;
    ItemPointer tid;

    /*
     * extract necessary information from index scan node
     */
    estate = node->ss.ps.state;
    direction = estate->es_direction;
    /* flip direction if this is an overall backward scan */
    if (ScanDirectionIsBackward(((IndexOnlyScan*)node->ss.ps.plan)->indexorderdir)) {
        if (ScanDirectionIsForward(direction))
            direction = BackwardScanDirection;
        else if (ScanDirectionIsBackward(direction))
            direction = ForwardScanDirection;
    }
    scandesc = node->ioss_ScanDesc;
    econtext = node->ss.ps.ps_ExprContext;
    slot = node->ss.ss_ScanTupleSlot;

    /*
     * OK, now that we have what we need, fetch the next tuple.
     */
    while ((tid = scan_handler_idx_getnext_tid(scandesc, direction)) != NULL) {
        HeapTuple tuple = NULL;
        IndexScanDesc indexScan = GetIndexScanDesc(scandesc);

        /*
         * We can skip the heap fetch if the TID references a heap page on
         * which all tuples are known visible to everybody.  In any case,
         * we'll use the index tuple not the heap tuple as the data source.
         *
         * Note on Memory Ordering Effects: visibilitymap_test does not lock
         * the visibility map buffer, and therefore the result we read here
         * could be slightly stale.  However, it can't be stale enough to
         * matter.	It suffices to show that (1) there is a read barrier
         * between the time we read the index TID and the time we test the
         * visibility map; and (2) there is a write barrier between the time
         * some other concurrent process clears the visibility map bit and the
         * time it inserts the index TID.  Since acquiring or releasing a
         * LWLock interposes a full barrier, this is easy to show: (1) is
         * satisfied by the release of the index buffer content lock after
         * reading the TID; and (2) is satisfied by the acquisition of the
         * buffer content lock in order to insert the TID.
         */
        if (!ExecGPIGetNextPartRelation(node, indexScan)) {
            continue;
        }
        if (!visibilitymap_test(indexScan->heapRelation, ItemPointerGetBlockNumber(tid), &node->ioss_VMBuffer)) {
            /*
             * Rats, we have to visit the heap to check visibility.
             */
            node->ioss_HeapFetches++;
            tuple = scan_handler_idx_fetch_heap(scandesc);
            if (tuple == NULL)
                continue; /* no visible tuple, try next index entry */

            /*
             * Only MVCC snapshots are supported here, so there should be no
             * need to keep following the HOT chain once a visible entry has
             * been found.	If we did want to allow that, we'd need to keep
             * more state to remember not to call index_getnext_tid next time.
             */
            if (indexScan->xs_continue_hot)
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("non-MVCC snapshots are not supported in index-only scans")));

            /*
             * Note: at this point we are holding a pin on the heap page, as
             * recorded in scandesc->xs_cbuf.  We could release that pin now,
             * but it's not clear whether it's a win to do so.	The next index
             * entry might require a visit to the same heap page.
             */
        }

        /*
         * Fill the scan tuple slot with data from the index.
         */
        StoreIndexTuple(slot, indexScan->xs_itup, indexScan->xs_itupdesc);

        /*
         * If the index was lossy, we have to recheck the index quals.
         * (Currently, this can never happen, but we should support the case
         * for possible future use, eg with GiST indexes.)
         */
        if (indexScan->xs_recheck) {
            econtext->ecxt_scantuple = slot;
            ResetExprContext(econtext);
            if (!ExecQual(node->indexqual, econtext, false)) {
                /* Fails recheck, so drop it and loop back for another */
                InstrCountFiltered2(node, 1);
                continue;
            }
        }

        /*
         * Predicate locks for index-only scans must be acquired at the page
         * level when the heap is not accessed, since tuple-level predicate
         * locks need the tuple's xmin value.  If we had to visit the tuple
         * anyway, then we already have the tuple-level lock and can skip the
         * page lock.
         */
        if (tuple == NULL)
            PredicateLockPage(indexScan->heapRelation, ItemPointerGetBlockNumber(tid), estate->es_snapshot);

        return slot;
    }

    /*
     * if we get here it means the index scan failed so we are at the end of
     * the scan..
     */
    return ExecClearTuple(slot);
}

/*
 * StoreIndexTuple
 *		Fill the slot with data from the index tuple.
 *
 * At some point this might be generally-useful functionality, but
 * right now we don't need it elsewhere.
 */
void StoreIndexTuple(TupleTableSlot* slot, IndexTuple itup, TupleDesc itupdesc)
{
    int nindexatts = itupdesc->natts;
    Datum* values = slot->tts_values;
    bool* isnull = slot->tts_isnull;
    int i;

    /*
     * Note: we must use the tupdesc supplied by the AM in index_getattr, not
     * the slot's tupdesc, in case the latter has different datatypes (this
     * happens for btree name_ops in particular).  They'd better have the same
     * number of columns though, as well as being datatype-compatible which is
     * something we can't so easily check.
     */
    Assert(slot->tts_tupleDescriptor->natts == nindexatts);

    (void)ExecClearTuple(slot);
    for (i = 0; i < nindexatts; i++)
        values[i] = index_getattr(itup, i + 1, itupdesc, &isnull[i]);
    ExecStoreVirtualTuple(slot);
}

/*
 * IndexOnlyRecheck -- access method routine to recheck a tuple in EvalPlanQual
 *
 * This can't really happen, since an index can't supply CTID which would
 * be necessary data for any potential EvalPlanQual target relation.  If it
 * did happen, the EPQ code would pass us the wrong data, namely a heap
 * tuple not an index tuple.  So throw an error.
 */
static bool IndexOnlyRecheck(IndexOnlyScanState* node, TupleTableSlot* slot)
{
    ereport(ERROR,
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("EvalPlanQual recheck is not supported in index-only scans")));
    return false; /* keep compiler quiet */
}

/* ----------------------------------------------------------------
 *		ExecIndexOnlyScan(node)
 * ----------------------------------------------------------------
 */
TupleTableSlot* ExecIndexOnlyScan(IndexOnlyScanState* node)
{
    /*
     * If we have runtime keys and they've not already been set up, do it now.
     */
    if (node->ioss_NumRuntimeKeys != 0 && !node->ioss_RuntimeKeysReady) {
        if (node->ss.isPartTbl) {
            if (PointerIsValid(node->ss.partitions)) {
                node->ss.ss_ReScan = true;

                ExecReScan((PlanState*)node);
            }
        } else {
            ExecReScan((PlanState*)node);
        }
    }

    return ExecScan(&node->ss, (ExecScanAccessMtd)IndexOnlyNext, (ExecScanRecheckMtd)IndexOnlyRecheck);
}

/* ----------------------------------------------------------------
 *		ExecReScanIndexOnlyScan(node)
 *
 *		Recalculates the values of any scan keys whose value depends on
 *		information known at runtime, then rescans the indexed relation.
 *
 *		Updating the scan key was formerly done separately in
 *		ExecUpdateIndexScanKeys. Integrating it into ReScan makes
 *		rescans of indices and relations/general streams more uniform.
 * ----------------------------------------------------------------
 */
void ExecReScanIndexOnlyScan(IndexOnlyScanState* node)
{
    /*
     * For recursive-stream rescan, if number of RuntimeKeys not euqal zero,
     * just return without rescan.
     *
     * If we are doing runtime key calculations (ie, any of the index key
     * values weren't simple Consts), compute the new key values.  But first,
     * reset the context so we don't leak memory as each outer tuple is
     * scanned.  Note this assumes that we will recalculate *all* runtime keys
     * on each call.
     */
    if (node->ioss_NumRuntimeKeys != 0) {
        if (node->ss.ps.state->es_recursive_next_iteration) {
            node->ioss_RuntimeKeysReady = false;
            return;
        }
        ExprContext* econtext = node->ioss_RuntimeContext;

        ResetExprContext(econtext);
        ExecIndexEvalRuntimeKeys(econtext, node->ioss_RuntimeKeys, node->ioss_NumRuntimeKeys);
    }
    node->ioss_RuntimeKeysReady = true;

    /*
     * deal with partitioned table
     */
    if (node->ss.isPartTbl) {
        /*
         * if node->ss.ss_ReScan = true, just do rescaning as non-partitioned
         * table; else switch to next partition for scaning.
         */
        if (node->ss.ss_ReScan) {
            /* reset the rescan falg */
            node->ss.ss_ReScan = false;
        } else {
            /*
             * switch to the next partition for scaning
             */
            if (!PointerIsValid(node->ss.partitions)) {
                return;
            }
            Assert(PointerIsValid(node->ioss_ScanDesc));
            scan_handler_idx_endscan(node->ioss_ScanDesc);

            /* initialize to scan the next partition */
            ExecInitNextIndexPartitionForIndexScanOnly(node);
            ExecScanReScan(&node->ss);
            /*
             * give up rescaning the index if there is no partition to scan
             */
            return;
        }
    }

    /* reset index scan */
    scan_handler_idx_rescan(node->ioss_ScanDesc,
        node->ioss_ScanKeys,
        node->ioss_NumScanKeys,
        node->ioss_OrderByKeys,
        node->ioss_NumOrderByKeys);

    ExecScanReScan(&node->ss);
}

/* ----------------------------------------------------------------
 *		ExecEndIndexOnlyScan
 * ----------------------------------------------------------------
 */
void ExecEndIndexOnlyScan(IndexOnlyScanState* node)
{
    Relation indexRelationDesc;
    IndexScanDesc idxScanDesc;
    Relation relation;

    /*
     * extract information from the node
     */
    indexRelationDesc = node->ioss_RelationDesc;
    idxScanDesc = node->ioss_ScanDesc;
    relation = node->ss.ss_currentRelation;

    /* Release VM buffer pin, if any. */
    if (node->ioss_VMBuffer != InvalidBuffer) {
        ReleaseBuffer(node->ioss_VMBuffer);
        node->ioss_VMBuffer = InvalidBuffer;
    }

    /*
     * Free the exprcontext(s) ... now dead code, see ExecFreeExprContext
     */
#ifdef NOT_USED
    ExecFreeExprContext(&node->ss.ps);
    if (node->ioss_RuntimeContext)
        FreeExprContext(node->ioss_RuntimeContext, true);
#endif

    /*
     * clear out tuple table slots
     */
    (void)ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    (void)ExecClearTuple(node->ss.ss_ScanTupleSlot);

    /*
     * close the index relation (no-op if we didn't open it)
     */
    if (idxScanDesc)
        scan_handler_idx_endscan(idxScanDesc);
    /*
     * close the index relation (no-op if we didn't open it)
     * close the index relation if the relation is non-partitioned table
     * close the index partitions and table partitions if the relation is
     * non-partitioned table
     */
    if (node->ss.isPartTbl) {
        if (PointerIsValid(node->ioss_IndexPartitionList)) {
            Assert(PointerIsValid(indexRelationDesc));
            Assert(PointerIsValid(node->ss.partitions));
            Assert(node->ss.partitions->length == node->ioss_IndexPartitionList->length);

            Assert(PointerIsValid(node->ioss_CurrentIndexPartition));
            releaseDummyRelation(&(node->ioss_CurrentIndexPartition));

            Assert(PointerIsValid(node->ss.ss_currentPartition));
            releaseDummyRelation(&(node->ss.ss_currentPartition));

            /* close index partition */
            releasePartitionList(node->ioss_RelationDesc, &(node->ioss_IndexPartitionList), NoLock);

            /* close table partition */
            releasePartitionList(node->ss.ss_currentRelation, &(node->ss.partitions), NoLock);
        }
    }

    if (indexRelationDesc)
        index_close(indexRelationDesc, NoLock);

    /*
     * close the heap relation.
     */
    ExecCloseScanRelation(relation);
}

/* ----------------------------------------------------------------
 *		ExecIndexOnlyMarkPos
 * ----------------------------------------------------------------
 */
void ExecIndexOnlyMarkPos(IndexOnlyScanState* node)
{
    scan_handler_idx_markpos(node->ioss_ScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecIndexOnlyRestrPos
 * ----------------------------------------------------------------
 */
void ExecIndexOnlyRestrPos(IndexOnlyScanState* node)
{
    scan_handler_idx_restrpos(node->ioss_ScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecInitIndexOnlyScan
 *
 *		Initializes the index scan's state information, creates
 *		scan keys, and opens the base and index relations.
 *
 *		Note: index scans have 2 sets of state information because
 *			  we have to keep track of the base relation and the
 *			  index relation.
 * ----------------------------------------------------------------
 */
IndexOnlyScanState* ExecInitIndexOnlyScan(IndexOnlyScan* node, EState* estate, int eflags)
{
    IndexOnlyScanState* indexstate = NULL;
    Relation currentRelation;
    bool relistarget = false;
    TupleDesc tupDesc;

    /*
     * create state structure
     */
    indexstate = makeNode(IndexOnlyScanState);
    indexstate->ss.ps.plan = (Plan*)node;
    indexstate->ss.ps.state = estate;
    indexstate->ioss_HeapFetches = 0;

    /* inherit essential info about partition data from IndexOnlyScan */
    indexstate->ss.isPartTbl = node->scan.isPartTbl;
    indexstate->ss.partScanDirection = node->indexorderdir;
    indexstate->ss.currentSlot = 0;

    /*
     * Miscellaneous initialization
     *
     * create expression context for node
     */
    ExecAssignExprContext(estate, &indexstate->ss.ps);

    indexstate->ss.ps.ps_TupFromTlist = false;

    /*
     * initialize child expressions
     *
     * Note: we don't initialize all of the indexorderby expression, only the
     * sub-parts corresponding to runtime keys (see below).
     */
    indexstate->ss.ps.targetlist = (List*)ExecInitExpr((Expr*)node->scan.plan.targetlist, (PlanState*)indexstate);
    indexstate->ss.ps.qual = (List*)ExecInitExpr((Expr*)node->scan.plan.qual, (PlanState*)indexstate);
    indexstate->indexqual = (List*)ExecInitExpr((Expr*)node->indexqual, (PlanState*)indexstate);


    /*
     * open the base relation and acquire appropriate lock on it.
     */
    currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid);

    indexstate->ss.ss_currentRelation = currentRelation;
    indexstate->ss.ss_currentScanDesc = NULL; /* no heap scan here */

    /*
     * tuple table initialization
     */
    ExecInitResultTupleSlot(estate, &indexstate->ss.ps, currentRelation->rd_tam_type);
    ExecInitScanTupleSlot(estate, &indexstate->ss, currentRelation->rd_tam_type);

    /*
     * Build the scan tuple type using the indextlist generated by the
     * planner.  We use this, rather than the index's physical tuple
     * descriptor, because the latter contains storage column types not the
     * types of the original datums.  (It's the AM's responsibility to return
     * suitable data anyway.)
     */
    tupDesc = ExecTypeFromTL(node->indextlist, false, false, TAM_HEAP);
    ExecAssignScanType(&indexstate->ss, tupDesc);

    /*
     * Initialize result tuple type and projection info.
     */
    ExecAssignResultTypeFromTL(
            &indexstate->ss.ps,
            indexstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor->tdTableAmType);

    ExecAssignScanProjectionInfo(&indexstate->ss);

    Assert(indexstate->ss.ps.ps_ResultTupleSlot->tts_tupleDescriptor->tdTableAmType != TAM_INVALID);

    /*
     * If we are just doing EXPLAIN (ie, aren't going to run the plan), stop
     * here.  This allows an index-advisor plugin to EXPLAIN a plan containing
     * references to nonexistent indexes.
     */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
        return indexstate;

    /*
     * Open the index relation.
     *
     * If the parent table is one of the target relations of the query, then
     * InitPlan already opened and write-locked the index, so we can avoid
     * taking another lock here.  Otherwise we need a normal reader's lock.
     */
    relistarget = ExecRelationIsTargetRelation(estate, node->scan.scanrelid);
    indexstate->ioss_RelationDesc = index_open(node->indexid, relistarget ? NoLock : AccessShareLock);

    if (!IndexIsUsable(indexstate->ioss_RelationDesc->rd_index)) {
        ereport(ERROR,
            (errcode(ERRCODE_INDEX_CORRUPTED),
                errmsg("can't initialize index-only scans using unusable index \"%s\"",
                    RelationGetRelationName(indexstate->ioss_RelationDesc))));
    }
    /*
     * Initialize index-specific scan state
     */
    indexstate->ioss_RuntimeKeysReady = false;
    indexstate->ioss_RuntimeKeys = NULL;
    indexstate->ioss_NumRuntimeKeys = 0;

    /*
     * build the index scan keys from the index qualification
     */
    ExecIndexBuildScanKeys((PlanState*)indexstate,
        indexstate->ioss_RelationDesc,
        node->indexqual,
        false,
        &indexstate->ioss_ScanKeys,
        &indexstate->ioss_NumScanKeys,
        &indexstate->ioss_RuntimeKeys,
        &indexstate->ioss_NumRuntimeKeys,
        NULL, /* no ArrayKeys */
        NULL);

    /*
     * any ORDER BY exprs have to be turned into scankeys in the same way
     */
    ExecIndexBuildScanKeys((PlanState*)indexstate,
        indexstate->ioss_RelationDesc,
        node->indexorderby,
        true,
        &indexstate->ioss_OrderByKeys,
        &indexstate->ioss_NumOrderByKeys,
        &indexstate->ioss_RuntimeKeys,
        &indexstate->ioss_NumRuntimeKeys,
        NULL, /* no ArrayKeys */
        NULL);

    /*
     * If we have runtime keys, we need an ExprContext to evaluate them. The
     * node's standard context won't do because we want to reset that context
     * for every tuple.  So, build another context just like the other one...
     * -tgl 7/11/00
     */
    if (indexstate->ioss_NumRuntimeKeys != 0) {
        ExprContext* stdecontext = indexstate->ss.ps.ps_ExprContext;

        ExecAssignExprContext(estate, &indexstate->ss.ps);
        indexstate->ioss_RuntimeContext = indexstate->ss.ps.ps_ExprContext;
        indexstate->ss.ps.ps_ExprContext = stdecontext;
    } else {
        indexstate->ioss_RuntimeContext = NULL;
    }

    /*
     * Initialize scan descriptor.
     * If the index is an non-partitioned index, initialize the table realtion that the index
     * is on. If the is an partitioned index, make the corresponding relation by the
     * partitioned index relation, partitioned table realtion and first table partititon and
     * index partition, then initialize the dummy relation
     */
    if (node->scan.isPartTbl) {
        indexstate->ioss_CurrentIndexPartition = NULL;
        indexstate->ss.ss_currentPartition = NULL;
        indexstate->ioss_ScanDesc = NULL;

        if (node->scan.itrs > 0) {
            Partition currentpartition = NULL;
            Partition currentindex = NULL;

            /* Initialize table partition list and index partition list for following scan*/
            ExecInitPartitionForIndexOnlyScan(indexstate, estate);

            if (indexstate->ss.partitions != NIL) {
                /* construct a dummy table relation with the first table partition for following scan */
                currentpartition = (Partition)list_nth(indexstate->ss.partitions, 0);
                indexstate->ss.ss_currentPartition =
                    partitionGetRelation(indexstate->ss.ss_currentRelation, currentpartition);

                /* construct a dummy index relation with the first table partition for following scan */
                currentindex = (Partition)list_nth(indexstate->ioss_IndexPartitionList, 0);
                indexstate->ioss_CurrentIndexPartition =
                    partitionGetRelation(indexstate->ioss_RelationDesc, currentindex);

                indexstate->ioss_ScanDesc = scan_handler_idx_beginscan(indexstate->ss.ss_currentPartition,
                    indexstate->ioss_CurrentIndexPartition,
                    estate->es_snapshot,
                    indexstate->ioss_NumScanKeys,
                    indexstate->ioss_NumOrderByKeys,
                    (ScanState*)indexstate);
            }
        }
    } else {
        /*
         * Initialize scan descriptor.
         */
        indexstate->ioss_ScanDesc = scan_handler_idx_beginscan(currentRelation,
            indexstate->ioss_RelationDesc,
            estate->es_snapshot,
            indexstate->ioss_NumScanKeys,
            indexstate->ioss_NumOrderByKeys,
            (ScanState*)indexstate);
    }

    /*
     * If is Partition table, if ( 0 == node->scan.itrs), scan_desc is NULL.
     */
    if (PointerIsValid(indexstate->ioss_ScanDesc)) {
        /* Set it up for index-only scan */
        GetIndexScanDesc(indexstate->ioss_ScanDesc)->xs_want_itup = true;
        indexstate->ioss_VMBuffer = InvalidBuffer;

        /*
         * If no run-time keys to calculate, go ahead and pass the scankeys to the
         * index AM.
         */
        if (indexstate->ioss_NumRuntimeKeys == 0)
            scan_handler_idx_rescan_local(indexstate->ioss_ScanDesc,
                indexstate->ioss_ScanKeys,
                indexstate->ioss_NumScanKeys,
                indexstate->ioss_OrderByKeys,
                indexstate->ioss_NumOrderByKeys);
    } else {
        indexstate->ss.ps.stubType = PST_Scan;
    }

    /*
     * all done.
     */
    return indexstate;
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: construt a dummy relation with the next partition and the partitiobed
 *			: table for the following IndexOnlyScan, and swith the scaning relation
 *			: to the dummy relation
 * Description	:
 * Input		:
 * Output	:
 * Notes		:
 */
static void ExecInitNextIndexPartitionForIndexScanOnly(IndexOnlyScanState* node)
{
    Partition currentpartition = NULL;
    Relation currentpartitionrel = NULL;
    Partition currentindexpartition = NULL;
    Relation currentindexpartitionrel = NULL;
    IndexOnlyScan* plan = NULL;
    int paramno = -1;
    ParamExecData* param = NULL;

    if (BufferIsValid(node->ioss_VMBuffer)) {
        ReleaseBuffer(node->ioss_VMBuffer);
        node->ioss_VMBuffer = InvalidBuffer;
    }

    plan = (IndexOnlyScan*)(node->ss.ps.plan);

    /* get partition sequnce number */
    paramno = plan->scan.plan.paramno;
    param = &(node->ss.ps.state->es_param_exec_vals[paramno]);
    node->ss.currentSlot = (int)param->value;

    /* construct a dummy table relation with the next table partition*/
    currentpartition = (Partition)list_nth(node->ss.partitions, node->ss.currentSlot);
    currentpartitionrel = partitionGetRelation(node->ss.ss_currentRelation, currentpartition);

    /* update scan-related table partition with the relation construced before */
    Assert(PointerIsValid(node->ss.ss_currentPartition));
    releaseDummyRelation(&(node->ss.ss_currentPartition));
    node->ss.ss_currentPartition = currentpartitionrel;

    /* construct a dummy index relation with the next index partition*/
    currentindexpartition = (Partition)list_nth(node->ioss_IndexPartitionList, node->ss.currentSlot);
    currentindexpartitionrel = partitionGetRelation(node->ioss_RelationDesc, currentindexpartition);

    /* update scan-related index partition with the relation construced before */
    Assert(PointerIsValid(node->ioss_CurrentIndexPartition));
    releaseDummyRelation(&(node->ioss_CurrentIndexPartition));
    node->ioss_CurrentIndexPartition = currentindexpartitionrel;

    /* Initialize scan descriptor. */
    node->ioss_ScanDesc = scan_handler_idx_beginscan(node->ss.ss_currentPartition,
        node->ioss_CurrentIndexPartition,
        node->ss.ps.state->es_snapshot,
        node->ioss_NumScanKeys,
        node->ioss_NumOrderByKeys,
        (ScanState*)node);
    GetIndexScanDesc(node->ioss_ScanDesc)->xs_want_itup = true;
    scan_handler_idx_rescan_local(node->ioss_ScanDesc,
        node->ioss_ScanKeys,
        node->ioss_NumScanKeys,
        node->ioss_OrderByKeys,
        node->ioss_NumOrderByKeys);

}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: get index partitions list and table partitions list for the
 *			: the following IndexOnliScan
 * Description	:
 * Input		:
 * Output	:
 * Notes		:
 */
void ExecInitPartitionForIndexOnlyScan(IndexOnlyScanState* indexstate, EState* estate)
{
    IndexOnlyScan* plan = NULL;
    Relation currentRelation = NULL;
    Partition tablepartition = NULL;
    Partition indexpartition = NULL;

    indexstate->ss.partitions = NIL;
    indexstate->ss.ss_currentPartition = NULL;
    indexstate->ioss_IndexPartitionList = NIL;
    indexstate->ioss_CurrentIndexPartition = NULL;

    plan = (IndexOnlyScan*)indexstate->ss.ps.plan;
    currentRelation = indexstate->ss.ss_currentRelation;

    if (plan->scan.itrs > 0) {
        Oid indexid = plan->indexid;
        bool relistarget = false;
        LOCKMODE lock;

        /*
         * get relation's lockmode that hangs on whether
         * it's one of the target relations of the query
         */
        relistarget = ExecRelationIsTargetRelation(estate, plan->scan.scanrelid);
        lock = (relistarget ? RowExclusiveLock : AccessShareLock);
        indexstate->ss.lockMode = lock;
        indexstate->lockMode = lock;

        PruningResult* resultPlan = NULL;
        if (plan->scan.pruningInfo->expr) {
            resultPlan = GetPartitionInfo(plan->scan.pruningInfo, estate, currentRelation);
        } else {
            resultPlan = plan->scan.pruningInfo;
        }
        if (resultPlan->ls_rangeSelectedPartitions != NULL) {
            indexstate->part_id = resultPlan->ls_rangeSelectedPartitions->length;
        } else {
            indexstate->part_id = 0;
        }
        
        ListCell* cell = NULL;
        List* part_seqs = resultPlan->ls_rangeSelectedPartitions;
        foreach (cell, part_seqs) {
            Oid indexpartitionid = InvalidOid;
            List* partitionIndexOidList = NIL;
            Oid tablepartitionid = InvalidOid;
            int partSeq = lfirst_int(cell);

            /* get table partition and add it to a list for following scan */
            tablepartitionid = getPartitionOidFromSequence(currentRelation, partSeq);
            tablepartition = partitionOpen(currentRelation, tablepartitionid, lock);
            indexstate->ss.partitions = lappend(indexstate->ss.partitions, tablepartition);

            /* get index partition and add it to a list for following scan */
            partitionIndexOidList = PartitionGetPartIndexList(tablepartition);
            Assert(PointerIsValid(partitionIndexOidList));
            if (!PointerIsValid(partitionIndexOidList)) {
                ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                        errmsg("no local indexes found for partition %s", PartitionGetPartitionName(tablepartition))));
            }
            indexpartitionid = searchPartitionIndexOid(indexid, partitionIndexOidList);
            list_free_ext(partitionIndexOidList);

            indexpartition = partitionOpen(indexstate->ioss_RelationDesc, indexpartitionid, lock);
            if (indexpartition->pd_part->indisusable == false) {
                ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("can't initialize index-only scans using unusable local index \"%s\"",
                            PartitionGetPartitionName(indexpartition))));
            }
            indexstate->ioss_IndexPartitionList = lappend(indexstate->ioss_IndexPartitionList, indexpartition);
        }
    }
}
