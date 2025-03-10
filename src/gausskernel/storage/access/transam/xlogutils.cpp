/* -------------------------------------------------------------------------
 *
 * xlogutils.cpp
 *
 * PostgreSQL transaction log manager utility routines
 *
 * This file contains support routines that are used by XLOG replay functions.
 * None of this code is used during normal system operation.
 *
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *        src/gausskernel/storage/access/transam/xlogutils.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/nbtree.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "access/xlog_internal.h"
#include "access/transam.h"
#include "commands/tablespace.h"
#include "access/xlogproc.h"
#include "access/multi_redo_api.h"
#include "access/parallel_recovery/dispatcher.h"
#include "catalog/catalog.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/catchup.h"
#include "replication/datasender.h"
#include "replication/walsender.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/rel.h"
#include "utils/rel_gs.h"

#include "commands/dbcommands.h"

/*
 * During XLOG replay, we may see XLOG records for incremental updates of
 * pages that no longer exist, because their relation was later dropped or
 * truncated.  (Note: this is only possible when full_page_writes = OFF,
 * since when it's ON, the first reference we see to a page should always
 * be a full-page rewrite not an incremental update.)  Rather than simply
 * ignoring such records, we make a note of the referenced page, and then
 * complain if we don't actually see a drop or truncate covering the page
 * later in replay.
 */
typedef struct xl_invalid_page_key {
    RelFileNode node;  /* the relation */
    ForkNumber forkno; /* the fork number */
    BlockNumber blkno; /* the page */
} xl_invalid_page_key;

typedef struct xl_invalid_page {
    xl_invalid_page_key key; /* hash key ... must be first */
    bool present;            /* page existed but contained zeroes */
} xl_invalid_page;

/* Report a reference to an invalid page */
static void report_invalid_page(int elevel, const RelFileNode &node, ForkNumber forkno, BlockNumber blkno, bool present)
{
    char *path = relpathperm(node, forkno);

    if (present)
        ereport(elevel, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                         errmsg("page %u of relation %s is uninitialized", blkno, path)));
    else
        ereport(elevel, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                         errmsg("page %u of relation %s does not exist", blkno, path)));
    pfree(path);
}

/* Close the XLog that opened by XLogRead when the thread exits. */
void closeXLogRead()
{
    if (t_thrd.xlog_cxt.sendFile < 0)
        return;

        /*
         * WAL segment files will not be re-read in normal operation, so we advise
         * the OS to release any cached pages. But do not do so if WAL archiving
         * or streaming is active, because archiver and walsender process could
         * use the cache to read the WAL segment.
         */
#if defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
    if (!XLogIsNeeded())
        (void)posix_fadvise(t_thrd.xlog_cxt.sendFile, 0, 0, POSIX_FADV_DONTNEED);
#endif

    if (close(t_thrd.xlog_cxt.sendFile))
        ereport(PANIC, (errcode_for_file_access(), errmsg("could not close log file %u, segment %lu: %m",
                                                          t_thrd.xlog_cxt.sendId, t_thrd.xlog_cxt.sendSegNo)));
    t_thrd.xlog_cxt.sendFile = -1;
}

/* Log a reference to an invalid page */
void log_invalid_page(const RelFileNode &node, ForkNumber forkno, BlockNumber blkno, bool present)
{
    xl_invalid_page_key key;
    xl_invalid_page *hentry = NULL;
    bool found = false;
    errno_t errorno = EOK;

    MemoryContext oldCtx = NULL;
    if (IsMultiThreadRedoRunning()) {
        oldCtx = MemoryContextSwitchTo(g_instance.comm_cxt.predo_cxt.parallelRedoCtx);
    }
    /*
     * Log references to invalid pages at DEBUG1 level.  This allows some
     * tracing of the cause (note the ereport context mechanism will tell us
     * something about the XLOG record that generated the reference).
     */
    if (log_min_messages <= DEBUG1 || client_min_messages <= DEBUG1)
        report_invalid_page(LOG, node, forkno, blkno, present);

    if (t_thrd.xlog_cxt.invalid_page_tab == NULL) {
        /* create hash table when first needed */
        HASHCTL ctl;

        errorno = memset_s(&ctl, sizeof(ctl), 0, sizeof(ctl));
        securec_check(errorno, "", "");

        ctl.keysize = sizeof(xl_invalid_page_key);
        ctl.entrysize = sizeof(xl_invalid_page);
        ctl.hash = tag_hash;
        int flag = HASH_ELEM | HASH_FUNCTION;
        if (IsMultiThreadRedoRunning()) {
            ctl.hcxt = g_instance.comm_cxt.predo_cxt.parallelRedoCtx;
            flag |= HASH_SHRCTX;
        }
        t_thrd.xlog_cxt.invalid_page_tab = hash_create("XLOG invalid-page table", 100, &ctl, flag);
    }

    /* we currently assume xl_invalid_page_key contains no padding */
    key.node = node;
    key.forkno = forkno;
    key.blkno = blkno;
    hentry = (xl_invalid_page *)hash_search(t_thrd.xlog_cxt.invalid_page_tab, (void *)&key, HASH_ENTER, &found);

    if (!found) {
        /* hash_search already filled in the key */
        hentry->present = present;
    } else {
        /* repeat reference ... leave "present" as it was */
    }

    if (IsMultiThreadRedoRunning()) {
        (void)MemoryContextSwitchTo(oldCtx);
    }
}

/* Forget any invalid pages >= minblkno, because they've been dropped */
static void forget_invalid_pages(const RelFileNode &node, ForkNumber forkno, BlockNumber minblkno)
{
    HASH_SEQ_STATUS status;
    xl_invalid_page *hentry = NULL;

    if (t_thrd.xlog_cxt.invalid_page_tab == NULL)
        return; /* nothing to do */

    MemoryContext oldCtx = NULL;
    if (IsMultiThreadRedoRunning()) {
        oldCtx = MemoryContextSwitchTo(g_instance.comm_cxt.predo_cxt.parallelRedoCtx);
    }

    hash_seq_init(&status, t_thrd.xlog_cxt.invalid_page_tab);

    while ((hentry = (xl_invalid_page *)hash_seq_search(&status)) != NULL) {
        if (BucketRelFileNodeEquals(node, hentry->key.node) && hentry->key.forkno == forkno &&
            hentry->key.blkno >= minblkno) {
            if (log_min_messages <= DEBUG2 || client_min_messages <= DEBUG2) {
                char *path = relpathperm(hentry->key.node, forkno);

                ereport(DEBUG2, (errmsg("page %u of relation %s has been dropped", hentry->key.blkno, path)));

                pfree(path);
            }

            if (hash_search(t_thrd.xlog_cxt.invalid_page_tab, (void *)&hentry->key, HASH_REMOVE, NULL) == NULL)
                ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("hash table corrupted")));
        }
    }

    if (IsMultiThreadRedoRunning()) {
        (void)MemoryContextSwitchTo(oldCtx);
    }
}

/* Forget any invalid pages in a whole database */
static void forget_invalid_pages_db(Oid dbid)
{
    HASH_SEQ_STATUS status;
    xl_invalid_page *hentry = NULL;

    if (t_thrd.xlog_cxt.invalid_page_tab == NULL)
        return; /* nothing to do */
    MemoryContext oldCtx = NULL;
    if (IsMultiThreadRedoRunning()) {
        oldCtx = MemoryContextSwitchTo(g_instance.comm_cxt.predo_cxt.parallelRedoCtx);
    }

    hash_seq_init(&status, t_thrd.xlog_cxt.invalid_page_tab);

    while ((hentry = (xl_invalid_page *)hash_seq_search(&status)) != NULL) {
        if (hentry->key.node.dbNode == dbid) {
            if (log_min_messages <= DEBUG2 || client_min_messages <= DEBUG2) {
                char *path = relpathperm(hentry->key.node, hentry->key.forkno);

                ereport(DEBUG2, (errmsg("page %u of relation %s has been dropped", hentry->key.blkno, path)));
                pfree(path);
            }

            if (hash_search(t_thrd.xlog_cxt.invalid_page_tab, (void *)&hentry->key, HASH_REMOVE, NULL) == NULL)
                ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("hash table corrupted")));
        }
    }

    if (IsMultiThreadRedoRunning()) {
        (void)MemoryContextSwitchTo(oldCtx);
    }
}

void PrintInvalidPage()
{
    if (t_thrd.xlog_cxt.invalid_page_tab != NULL && hash_get_num_entries(t_thrd.xlog_cxt.invalid_page_tab) > 0) {
        HASH_SEQ_STATUS status;
        xl_invalid_page *hentry = NULL;
        hash_seq_init(&status, t_thrd.xlog_cxt.invalid_page_tab);
        while ((hentry = (xl_invalid_page *)hash_seq_search(&status)) != NULL) {
            report_invalid_page(LOG, hentry->key.node, hentry->key.forkno, hentry->key.blkno, hentry->present);
        }
    }
}

/* Are there any unresolved references to invalid pages? */
bool XLogHaveInvalidPages(void)
{
    if (t_thrd.xlog_cxt.invalid_page_tab != NULL && hash_get_num_entries(t_thrd.xlog_cxt.invalid_page_tab) > 0) {
#ifdef USE_ASSERT_CHECKING
        int printLevel = WARNING;
#else
        int printLevel = DEBUG1;
#endif
        if (log_min_messages <= printLevel) {
            PrintInvalidPage();
        }
        return true;
    }
    return false;
}

typedef struct InvalidPagesState {
    HTAB *invalid_page_tab;
} InvalidPagesState;

void *XLogGetInvalidPages()
{
    InvalidPagesState *state = (InvalidPagesState *)palloc(sizeof(InvalidPagesState));
    state->invalid_page_tab = t_thrd.xlog_cxt.invalid_page_tab;
    t_thrd.xlog_cxt.invalid_page_tab = NULL;
    return state;
}

static bool XLogCheckInvalidPages_ForSingle(void)
{
    HASH_SEQ_STATUS status;
    xl_invalid_page *hentry = NULL;
    bool foundone = false;

    if (t_thrd.xlog_cxt.invalid_page_tab == NULL)
        return foundone; /* nothing to do */

    MemoryContext oldCtx = NULL;
    if (IsMultiThreadRedoRunning()) {
        oldCtx = MemoryContextSwitchTo(g_instance.comm_cxt.predo_cxt.parallelRedoCtx);
    }
    hash_seq_init(&status, t_thrd.xlog_cxt.invalid_page_tab);

    /*
     * Our strategy is to emit WARNING messages for all remaining entries and
     * only PANIC after we've dumped all the available info.
     */
    while ((hentry = (xl_invalid_page *)hash_seq_search(&status)) != NULL) {
        report_invalid_page(WARNING, hentry->key.node, hentry->key.forkno, hentry->key.blkno, hentry->present);
        t_thrd.xlog_cxt.invaildPageCnt++;
        foundone = true;
    }

    hash_destroy(t_thrd.xlog_cxt.invalid_page_tab);
    t_thrd.xlog_cxt.invalid_page_tab = NULL;

    if (IsMultiThreadRedoRunning()) {
        (void)MemoryContextSwitchTo(oldCtx);
    }

    return foundone;
}

static void CollectInvalidPagesStates(uint32 *nstates_ptr, InvalidPagesState ***state_array_ptr)
{
    *nstates_ptr = GetRedoWorkerCount();
    *state_array_ptr = (InvalidPagesState **)GetXLogInvalidPagesFromWorkers();

    ereport(LOG,
            (errmodule(MOD_REDO), errcode(ERRCODE_LOG), errmsg("CollectInvalidPagesStates: nstates:%u", *nstates_ptr)));
    return;
}

/* Complain about any remaining invalid-page entries */
void XLogCheckInvalidPages(void)
{
    bool foundone = false;
    if (t_thrd.xlog_cxt.forceFinishHappened) {
        ereport(WARNING,
                (errmodule(MOD_REDO), errcode(ERRCODE_LOG),
                 errmsg("[REDO_LOG_TRACE]XLogCheckInvalidPages happen:%u", t_thrd.xlog_cxt.forceFinishHappened)));
    }
    if (IsMultiThreadRedoRunning()) {
        MemoryContext old = MemoryContextSwitchTo(g_instance.comm_cxt.predo_cxt.parallelRedoCtx);
        /* for parallel redo, trxn thread also may have invalidpages */
        foundone = XLogCheckInvalidPages_ForSingle();

        if (GetRedoWorkerCount() > 0) {
            uint32 nstates;
            InvalidPagesState **state_array = NULL;
            HASH_SEQ_STATUS status;
            xl_invalid_page *hentry = NULL;

            CollectInvalidPagesStates(&nstates, &state_array);

            for (uint32 i = 0; ((i < nstates) && (state_array != NULL)); i++) {
                if (state_array[i] == NULL) {
                    continue;
                } else if (state_array[i]->invalid_page_tab == NULL)
                    continue; /* nothing to do */

                hash_seq_init(&status, state_array[i]->invalid_page_tab);

                /*
                 * Our strategy is to emit WARNING messages for all remaining entries and
                 * only PANIC after we've dumped all the available info.
                 */
                while ((hentry = (xl_invalid_page *)hash_seq_search(&status)) != NULL) {
                    report_invalid_page(WARNING, hentry->key.node, hentry->key.forkno, hentry->key.blkno,
                                        hentry->present);
                    t_thrd.xlog_cxt.invaildPageCnt++;
                    foundone = true;
                }
                hash_destroy(state_array[i]->invalid_page_tab);
            }
        }
        (void)MemoryContextSwitchTo(old);
    } else {
        foundone = XLogCheckInvalidPages_ForSingle();
    }

    if (foundone) {
        /* can't use t_thrd.xlog_cxt.isIgoreCleanup to judge here, because of scenario: */
        /* after XLogCheckInvalidPages finishes with isIgoreCleanup on, then DN crash and restart, */
        /* and then we get here again with isIgoreCleanup off, we have to ignore again. */
        if (!force_finish_enabled()) {
            ereport(PANIC, (errmodule(MOD_REDO), errcode(ERRCODE_LOG),
                            errmsg("[REDO_LOG_TRACE]WAL contains references to invalid pages, count:%u",
                                   t_thrd.xlog_cxt.invaildPageCnt)));
        } else {
            ereport(WARNING, (errmodule(MOD_REDO), errcode(ERRCODE_LOG),
                              errmsg("[REDO_LOG_TRACE]WAL contains references to invalid pages, "
                                     "and invalid pages are ignored, happen:%u, count:%u",
                                     t_thrd.xlog_cxt.forceFinishHappened, t_thrd.xlog_cxt.invaildPageCnt)));
        }
    }
}

/*
 * XLogReadBufferForRedo
 *		Read a page during XLOG replay
 *
 * Reads a block referenced by a WAL record into shared buffer cache, and
 * determines what needs to be done to redo the changes to it.  If the WAL
 * record includes a full-page image of the page, it is restored.
 *
 * 'lsn' is the LSN of the record being replayed.  It is compared with the
 * page's LSN to determine if the record has already been replayed.
 * 'block_id' is the ID number the block was registered with, when the WAL
 * record was created.
 *
 * Returns one of the following:
 *
 *	BLK_NEEDS_REDO	- changes from the WAL record need to be applied
 *	BLK_DONE		- block doesn't need replaying
 *	BLK_RESTORED	- block was restored from a full-page image included in
 *					  the record
 *	BLK_NOTFOUND	- block was not found (because it was truncated away by
 *					  an operation later in the WAL stream)
 *
 * On return, the buffer is locked in exclusive-mode, and returned in *buf.
 * Note that the buffer is locked and returned even if it doesn't need
 * replaying.  (Getting the buffer lock is not really necessary during
 * single-process crash recovery, but some subroutines such as MarkBufferDirty
 * will complain if we don't have the lock.  In hot standby mode it's
 * definitely necessary.)
 *
 * Note: when a backup block is available in XLOG, we restore it
 * unconditionally, even if the page in the database appears newer.  This is
 * to protect ourselves against database pages that were partially or
 * incorrectly written during a crash.  We assume that the XLOG data must be
 * good because it has passed a CRC check, while the database page might not
 * be.  This will force us to replay all subsequent modifications of the page
 * that appear in XLOG, rather than possibly ignoring them as already
 * applied, but that's not a huge drawback.
 */
XLogRedoAction XLogReadBufferForRedo(XLogReaderState *record, uint8 block_id, RedoBufferInfo *bufferinfo)
{
    return XLogReadBufferForRedoExtended(record, block_id, RBM_NORMAL, false, bufferinfo);
}

/*
 * Pin and lock a buffer referenced by a WAL record, for the purpose of
 * re-initializing it.
 */
void XLogInitBufferForRedo(XLogReaderState *record, uint8 block_id, RedoBufferInfo *bufferinfo)
{
    XLogReadBufferForRedoExtended(record, block_id, RBM_ZERO_AND_LOCK, false, bufferinfo);
}

/*
 * XLogReadBufferForRedoExtended
 *     Like XLogReadBufferForRedo, but with extra options.
 *
 * IN RBM_ZERO_XXXX modes, if the page doesn't exist, the relation is extended
 * with all-zeros pages up to the referenced block number. In
 * RBM_ZERO_AND_LOCK and RBM_ZERO_AND_CLEANUP_LOCK modes,  the return
 * value is always BLK_NEEDS_REDO.
 *
 * (The RBM_ZERO_AND_CLEANUP_LOCK mode is redundant with the get_cleanup_lock
 * parameter. Do not use an inconsistent combination!)
 *
 * If 'get_cleanup_lock' is true, a "cleanup lock" is acquired on the buffer
 * using LockBufferForCleanup(), instead of a regular exclusive lock.
 */
XLogRedoAction XLogReadBufferForRedoBlockExtend(RedoBufferTag *redoblock, ReadBufferMode mode, 
    bool get_cleanup_lock, RedoBufferInfo *redobufferinfo, XLogRecPtr xloglsn, ReadBufferMethod readmethod)
{
    bool pageisvalid = false;
    Page page;
    Buffer buf;
    Size pagesize;
    if (readmethod == WITH_OUT_CACHE) {
        buf = XLogReadBufferExtendedWithoutBuffer(redoblock->rnode, redoblock->forknum, redoblock->blkno, mode);
        XLogRedoBufferIsValidFunc(buf, &pageisvalid);
        XLogRedoBufferGetPageFunc(buf, &page);
        pagesize = (Size)BLCKSZ;
    } else {
        if (readmethod == WITH_LOCAL_CACHE)
            buf = XLogReadBufferExtendedWithLocalBuffer(redoblock->rnode, redoblock->forknum, redoblock->blkno, mode);
        else
            buf = XLogReadBufferExtended(redoblock->rnode, redoblock->forknum, redoblock->blkno, mode);
        pageisvalid = BufferIsValid(buf);
        if (pageisvalid) {
            if (readmethod != WITH_LOCAL_CACHE) {
                if (mode != RBM_ZERO_AND_LOCK && mode != RBM_ZERO_AND_CLEANUP_LOCK) {
                    if (get_cleanup_lock)
                        LockBufferForCleanup(buf);
                    else
                        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
                }
            }
            page = BufferGetPage(buf);
            pagesize = BufferGetPageSize(buf);
        }
    }

    redobufferinfo->lsn = xloglsn;
    redobufferinfo->blockinfo = *redoblock;
    if (pageisvalid) {
        redobufferinfo->buf = buf;
        redobufferinfo->pageinfo.page = page;
        redobufferinfo->pageinfo.pagesize = pagesize;
        if (XLByteLE(xloglsn, PageGetLSN(page)))
            return BLK_DONE;
        else {
            return BLK_NEEDS_REDO;
        }
    } else {
        redobufferinfo->buf = InvalidBuffer;
    }
    return BLK_NOTFOUND;
}

XLogRedoAction XLogReadBufferForRedoExtended(XLogReaderState *record, uint8 block_id, ReadBufferMode mode,
                                             bool get_cleanup_lock, RedoBufferInfo *bufferinfo,
                                             ReadBufferMethod readmethod)
{
    bool zeromode = false;
    bool willinit = false;
    RedoBufferTag blockinfo;
    XLogRedoAction redoaction;
    bool xloghasblockimage = false;

    if (!XLogRecGetBlockTag(record, block_id, &(blockinfo.rnode), &(blockinfo.forknum), &(blockinfo.blkno))) {
        /* Caller specified a bogus block_id */
        ereport(PANIC, (errmsg("failed to locate backup block with ID %d", block_id)));
    }

    /*
     * Make sure that if the block is marked with WILL_INIT, the caller is
     * going to initialize it. And vice versa.
     */
    zeromode = (mode == RBM_ZERO_AND_LOCK || mode == RBM_ZERO_AND_CLEANUP_LOCK);
    willinit = (record->blocks[block_id].flags & BKPBLOCK_WILL_INIT) != 0;
    if (willinit && !zeromode)
        ereport(PANIC, (errmsg("block with WILL_INIT flag in WAL record must be zeroed by redo routine")));
    if (!willinit && zeromode)
        ereport(PANIC,
                (errmsg(
                    "block to be initialized in redo routine must be marked with WILL_INIT flag in the WAL record")));

    xloghasblockimage = XLogRecHasBlockImage(record, block_id);
    if (xloghasblockimage) {
        mode = get_cleanup_lock ? RBM_ZERO_AND_CLEANUP_LOCK : RBM_ZERO_AND_LOCK;
    }
    redoaction = XLogReadBufferForRedoBlockExtend(&blockinfo, mode, get_cleanup_lock, bufferinfo, record->EndRecPtr,
                                                  readmethod);
    if (redoaction == BLK_NOTFOUND) {
        return BLK_NOTFOUND;
    }
    /* If it's a full-page image, restore it. */
    if (xloghasblockimage) {
        char *imagedata;
        uint16 hole_offset;
        uint16 hole_length;
        imagedata = XLogRecGetBlockImage(record, block_id, &hole_offset, &hole_length);
        if (NULL == imagedata)
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                            errmsg("XLogReadBufferForRedoExtended failed to restore block image")));
        RestoreBlockImage(imagedata, hole_offset, hole_length, (char *)bufferinfo->pageinfo.page);
        XlogUpdateFullPageWriteLsn(bufferinfo->pageinfo.page, bufferinfo->lsn);
        PageSetJustAfterFullPageWrite(bufferinfo->pageinfo.page);
        if (readmethod == WITH_NORMAL_CACHE) {
            MarkBufferDirty(bufferinfo->buf);
            if (bufferinfo->blockinfo.forknum == INIT_FORKNUM)
                FlushOneBuffer(bufferinfo->buf);
        }
        return BLK_RESTORED;
    } else if (BLK_NEEDS_REDO == redoaction) {
        if (EnalbeWalLsnCheck && bufferinfo->blockinfo.forknum == MAIN_FORKNUM) {
            XLogRecPtr lastLsn;
            if (!XLogRecGetBlockLastLsn(record, block_id, &lastLsn)) {
                ereport(PANIC, (errmsg("can not get xlog lsn from record page block %u lsn %lu", block_id, lastLsn)));
            }
            DoLsnCheck(bufferinfo, willinit, lastLsn);
        }
        PageClearJustAfterFullPageWrite(bufferinfo->pageinfo.page);
    }
    return redoaction;
}
Buffer XLogReadBufferExtendedWithLocalBuffer(RelFileNode rnode, ForkNumber forknum, BlockNumber blkno,
                                             ReadBufferMode mode)
{
    BlockNumber lastblock;
    Buffer buffer;
    Page page;
    bool hit = false;
    SMgrRelation smgr;

    Assert(blkno != P_NEW);

    smgr = smgropen(rnode, InvalidBackendId);
    smgrcreate(smgr, forknum, true);
    lastblock = smgrnblocks(smgr, forknum);
    if (blkno < lastblock) {
        buffer = ReadBuffer_common_for_localbuf(rnode, RELPERSISTENCE_PERMANENT, forknum, blkno, mode, NULL, &hit);
    } else {
        if (mode == RBM_NORMAL) {
            log_invalid_page(rnode, forknum, blkno, false);
            return InvalidBuffer;
        }
        if (mode == RBM_NORMAL_NO_LOG)
            return InvalidBuffer;

        Assert(t_thrd.xlog_cxt.InRecovery);
        buffer = InvalidBuffer;
        LockRelFileNodeForExtension(rnode, ExclusiveLock);
        do {
            if (buffer != InvalidBuffer) {
                ReleaseBuffer(buffer);
            }
            buffer = ReadBuffer_common_for_localbuf(rnode, RELPERSISTENCE_PERMANENT, forknum, P_NEW, mode, NULL, &hit);
        } while (BufferGetBlockNumber(buffer) < blkno);
        UnlockRelFileNodeForExtension(rnode, ExclusiveLock);

        if (BufferGetBlockNumber(buffer) != blkno) {
            ReleaseBuffer(buffer);
            buffer = ReadBuffer_common_for_localbuf(rnode, RELPERSISTENCE_PERMANENT, forknum, blkno, mode, NULL, &hit);
        }
    }
    page = BufferGetPage(buffer);
    if (mode == RBM_NORMAL) {
        /*
         * The page may be uninitialized. If so, we can't set the LSN because
         * that would corrupt the page.
         */
        if (PageIsNew(page)) {
            Assert(!PageIsLogical(page));
            ReleaseBuffer(buffer);
            log_invalid_page(rnode, forknum, blkno, true);
            return InvalidBuffer;
        }
    }
    if (t_thrd.xlog_cxt.startup_processing && t_thrd.xlog_cxt.server_mode == STANDBY_MODE && PageIsLogical(page))
        PageClearLogical(page);
    return buffer;
}

Buffer XLogReadBufferExtendedWithoutBuffer(RelFileNode rnode, ForkNumber forknum, BlockNumber blkno,
                                           ReadBufferMode mode)
{
    BlockNumber lastblock;
    Page page;
    SMgrRelation smgr;
    Buffer buffer;
    BlockNumber curblknum;

    Assert(blkno != P_NEW);
    smgr = smgropen(rnode, InvalidBackendId);
    /*
     * At the end of crash recovery the init forks of unlogged relations
     * are copied, without going through shared buffers. So we need to
     * force the on-disk state of init forks to always be in sync with the
     * state in shared buffers.
     */
    smgrcreate(smgr, forknum, true);

    lastblock = smgrnblocks(smgr, forknum);

    if (blkno < lastblock) {
        buffer = ReadBuffer_common_for_direct(rnode, RELPERSISTENCE_PERMANENT, forknum, blkno, mode);
    } else {
        if (mode == RBM_NORMAL) {
            log_invalid_page(rnode, forknum, blkno, false);
            return InvalidBuffer;
        }
        if (mode == RBM_NORMAL_NO_LOG)
            return InvalidBuffer;

        Assert(t_thrd.xlog_cxt.InRecovery);
        buffer = InvalidBuffer;
        LockRelFileNodeForExtension(rnode, ExclusiveLock);
        do {
            if (buffer != InvalidBuffer) {
                /* not local the buffer content , so don't need call LockBuffer unlock */
                XLogRedoBufferReleaseFunc(buffer);
            }
            buffer = ReadBuffer_common_for_direct(rnode, RELPERSISTENCE_PERMANENT, forknum, P_NEW, mode);
            XLogRedoBufferGetBlkNumberFunc(buffer, &curblknum);
        } while (curblknum < blkno);
        UnlockRelFileNodeForExtension(rnode, ExclusiveLock);

        XLogRedoBufferGetBlkNumberFunc(buffer, &curblknum);
        if (curblknum != blkno) {
            XLogRedoBufferReleaseFunc(buffer);
            buffer = ReadBuffer_common_for_direct(rnode, RELPERSISTENCE_PERMANENT, forknum, blkno, mode);
        }
    }

    XLogRedoBufferGetPageFunc(buffer, &page);
    if (mode == RBM_NORMAL) {
        if (PageIsNew(page)) {
            Assert(!PageIsLogical(page));
            XLogRedoBufferReleaseFunc(buffer);
            log_invalid_page(rnode, forknum, blkno, true);
            return InvalidBuffer;
        }
    }
    if (t_thrd.xlog_cxt.startup_processing && t_thrd.xlog_cxt.server_mode == STANDBY_MODE && PageIsLogical(page))
        PageClearLogical(page);
    return buffer;
}

/*
 * XLogReadBufferExtended
 *		Read a page during XLOG replay
 *
 * This is functionally comparable to ReadBufferExtended. There's some
 * differences in the behavior wrt. the "mode" argument:
 *
 * In RBM_NORMAL mode, if the page doesn't exist, or contains all-zeroes, we
 * return InvalidBuffer. In this case the caller should silently skip the
 * update on this page. (In this situation, we expect that the page was later
 * dropped or truncated. If we don't see evidence of that later in the WAL
 * sequence, we'll complain at the end of WAL replay.)
 *
 * In RBM_ZERO* modes, if the page doesn't exist, the relation is extended
 * with all-zeroes pages up to the given block number.
 *
 * In RBM_NORMAL_NO_LOG mode, we return InvalidBuffer if the page doesn't
 * exist, and we don't check for all-zeroes.  Thus, no log entry is made
 * to imply that the page should be dropped or truncated later.
 *
 * NB: A redo function should normally not call this directly. To get a page
 * to modify, use XLogReadBufferForRedoExtended instead. It is important that
 * all pages modified by a WAL record are registered in the WAL records, or
 * they will be invisible to tools that that need to know which pages are
 * modified.
 */
Buffer XLogReadBufferExtended(const RelFileNode &rnode, ForkNumber forknum, BlockNumber blkno, ReadBufferMode mode)
{
    BlockNumber lastblock;
    Buffer buffer;
    Page page;
    SMgrRelation smgr;

    Assert(blkno != P_NEW);

    /* Open the relation at smgr level */
    smgr = smgropen(rnode, InvalidBackendId);

    /*
     * Create the target file if it doesn't already exist.  This lets us cope
     * if the replay sequence contains writes to a relation that is later
     * deleted.  (The original coding of this routine would instead suppress
     * the writes, but that seems like it risks losing valuable data if the
     * filesystem loses an inode during a crash.  Better to write the data
     * until we are actually told to delete the file.)
     */
    smgrcreate(smgr, forknum, true);

    lastblock = smgrnblocks(smgr, forknum);
    if (blkno < lastblock) {
        /* page exists in file */
        buffer = ReadBufferWithoutRelcache(rnode, forknum, blkno, mode, NULL);
    } else {
        /* hm, page doesn't exist in file */
        if (mode == RBM_NORMAL) {
            log_invalid_page(rnode, forknum, blkno, false);
            return InvalidBuffer;
        }
        if (mode == RBM_NORMAL_NO_LOG)
            return InvalidBuffer;

        /*
         * OK to extend the file
         * Data replication writer maybe conflicts with us. lock relation extension first.
         */
        Assert(t_thrd.xlog_cxt.InRecovery);
        buffer = InvalidBuffer;

        LockRelFileNodeForExtension(rnode, ExclusiveLock);
        do {
            if (buffer != InvalidBuffer) {
                if (mode == RBM_ZERO_AND_LOCK || mode == RBM_ZERO_AND_CLEANUP_LOCK)
                    LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
                ReleaseBuffer(buffer);
            }
            buffer = ReadBufferWithoutRelcache(rnode, forknum, P_NEW, mode, NULL);
        } while (BufferGetBlockNumber(buffer) < blkno);
        UnlockRelFileNodeForExtension(rnode, ExclusiveLock);

        /* Handle the corner case that P_NEW returns non-consecutive pages */
        if (BufferGetBlockNumber(buffer) != blkno) {
            if (mode == RBM_ZERO_AND_LOCK || mode == RBM_ZERO_AND_CLEANUP_LOCK)
                LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
            ReleaseBuffer(buffer);
            buffer = ReadBufferWithoutRelcache(rnode, forknum, blkno, mode, NULL);
        }
    }

    page = BufferGetPage(buffer);
    if (mode == RBM_NORMAL) {
        /* check that page has been initialized
         *
         * We assume that PageIsNew is safe without a lock. During recovery,
         * there should be no other backends that could modify the buffer at
         * the same time.
         */
        if (PageIsNew(page)) {
            Assert(!PageIsLogical(page));
            ReleaseBuffer(buffer);
            log_invalid_page(rnode, forknum, blkno, true);
            return InvalidBuffer;
        }
    }

    if (t_thrd.xlog_cxt.startup_processing && t_thrd.xlog_cxt.server_mode == STANDBY_MODE && PageIsLogical(page))
        PageClearLogical(page);

    return buffer;
}

/*
 * Struct actually returned by XLogFakeRelcacheEntry, though the declared
 * return type is Relation.
 */
typedef struct {
    RelationData reldata; /* Note: this must be first */
    FormData_pg_class pgc;
} FakeRelCacheEntryData;

typedef FakeRelCacheEntryData *FakeRelCacheEntry;

/*
 * Create a fake relation cache entry for a physical relation
 *
 * It's often convenient to use the same functions in XLOG replay as in the
 * main codepath, but those functions typically work with a relcache entry.
 * We don't have a working relation cache during XLOG replay, but this
 * function can be used to create a fake relcache entry instead. Only the
 * fields related to physical storage, like rd_rel, are initialized, so the
 * fake entry is only usable in low-level operations like ReadBuffer().
 *
 * Caller must free the returned entry with FreeFakeRelcacheEntry().
 */
Relation CreateFakeRelcacheEntry(const RelFileNode &rnode)
{
    return CreateCUReplicationRelation(rnode,
                                       /* We will never be working with temp rels during recovery */
                                       InvalidBackendId,
                                       /* It must be a permanent table if we're in recovery. */
                                       RELPERSISTENCE_PERMANENT,
                                       /* fill relation name following */
                                       NULL);
}

/* Create a fake relation for CU replication.
 *
 * All these arguments are needed during CU replication.
 * These fake relation will be passed to CStoreCUReplication().
 * Now SET TABLESPACE and REWRITE COLUMN RELATION must create new CU Replication relation by calling
 * this method becuase of new tablespace and new relfilenode, which is different from existing tablespace
 * and relfilenode.
 * For COPY FROM and BULK INSERT, current heap relation is used for data replication.
 */
Relation CreateCUReplicationRelation(const RelFileNode &rnode, int BackendId, char relpersistence, const char *relname)
{
    FakeRelCacheEntry fakeentry = NULL;
    Relation rel = NULL;
    MemoryContext oldcxt = NULL;

    /*
     * switch to the cache context to create the fake relcache entry.
     */
    oldcxt = MemoryContextSwitchTo(u_sess->cache_mem_cxt);

    /* Allocate the Relation struct and all related space in one block. */
    fakeentry = (FakeRelCacheEntry)palloc0(sizeof(FakeRelCacheEntryData));
    rel = (Relation)fakeentry;

    /* fill all these necessary info for this relation */
    rel->rd_rel = &fakeentry->pgc;
    rel->rd_node = rnode;
    rel->rd_backend = BackendId;
    rel->rd_rel->relpersistence = relpersistence;

    if (relname != NULL) {
        /* fill relation name. */
        int len = (int)strlen(relname);
        len = Min(len, (NAMEDATALEN - 1));
        int rc = strncpy_s(RelationGetRelationName(rel), NAMEDATALEN, relname, len);
        securec_check_c(rc, "\0", "\0");
    } else {
        /* We don't know the name of the relation; use relfilenode instead */
        int rc = sprintf_s(RelationGetRelationName(rel), NAMEDATALEN, "%u/%u/%u", rnode.spcNode, rnode.dbNode,
                           rnode.relNode);
        securec_check_ss(rc, "\0", "\0");
    }

    /*
     * We set up the lockRelId in case anything tries to lock the dummy
     * relation.  Note that this is fairly bogus since relNode may be
     * different from the relation's OID.  It shouldn't really matter though,
     * since we are presumably running by ourselves and can't have any lock
     * conflicts ...
     */
    rel->rd_lockInfo.lockRelId.dbId = rnode.dbNode;
    rel->rd_lockInfo.lockRelId.relId = rnode.relNode;
    rel->rd_lockInfo.lockRelId.bktId = (Oid)(rnode.bucketNode + 1);

    /* at default it's closed and null */
    rel->rd_smgr = NULL;
    rel->rd_bucketkey = NULL;
    rel->rd_bucketoid = InvalidOid;

    (void)MemoryContextSwitchTo(oldcxt);

    return rel;
}

/*
 * Free a fake relation cache entry.
 */
void FreeFakeRelcacheEntry(Relation fakerel)
{
    /* make sure the fakerel is not referenced by the SmgrRelation anymore */
    if (fakerel->rd_smgr != NULL)
        smgrclearowner(&fakerel->rd_smgr, fakerel->rd_smgr);

    pfree(fakerel);
}

void XlogDropRowReation(RelFileNode rnode)
{
    for (int fork = 0; fork <= MAX_FORKNUM; fork++)
        XLogDropRelation(rnode, fork);
    RelFileNodeBackend rbnode;
    /* close the relnode */
    rbnode.node = rnode;
    rbnode.backend = InvalidBackendId;
    smgrclosenode(rbnode);
}

void XLogForgetDDLRedo(XLogRecParseState *redoblockstate)
{
    XLogBlockDdlParse *ddlrecparse = &(redoblockstate->blockparse.extra_rec.blockddlrec);
    if (ddlrecparse->blockddltype == BLOCK_DDL_DROP_RELNODE) {
        if (redoblockstate->blockparse.blockhead.forknum <= MAX_FORKNUM) {
            RelFileNode relNode;
            relNode.spcNode = redoblockstate->blockparse.blockhead.spcNode;
            relNode.dbNode = redoblockstate->blockparse.blockhead.dbNode;
            relNode.relNode = redoblockstate->blockparse.blockhead.relNode;
            relNode.bucketNode = redoblockstate->blockparse.blockhead.bucketNode;
            XlogDropRowReation(relNode);
        }
    } else if (ddlrecparse->blockddltype == BLOCK_DDL_TRUNCATE_RELNODE) {
        RelFileNode relNode;
        relNode.spcNode = redoblockstate->blockparse.blockhead.spcNode;
        relNode.dbNode = redoblockstate->blockparse.blockhead.dbNode;
        relNode.relNode = redoblockstate->blockparse.blockhead.relNode;
        relNode.bucketNode = redoblockstate->blockparse.blockhead.bucketNode;
        XLogTruncateRelation(relNode, redoblockstate->blockparse.blockhead.forknum,
                             redoblockstate->blockparse.blockhead.blkno);
    }
}

/*
 * Drop a relation during XLOG replay
 *
 * This is called when the relation is about to be deleted; we need to remove
 * any open "invalid-page" records for the relation.
 */
void XLogDropRelation(const RelFileNode &rnode, ForkNumber forknum)
{
    forget_invalid_pages(rnode, forknum, 0);
}

bool IsDataBaseDrop(XLogReaderState *record)
{
    return (XLogRecGetRmid(record) == RM_DBASE_ID && (XLogRecGetInfo(record) & ~XLR_INFO_MASK) == XLOG_DBASE_DROP);
}

bool IsDataBaseCreate(XLogReaderState *record)
{
    return (XLogRecGetRmid(record) == RM_DBASE_ID && (XLogRecGetInfo(record) & ~XLR_INFO_MASK) == XLOG_DBASE_CREATE);
}

bool IsTableSpaceDrop(XLogReaderState *record)
{
    return (XLogRecGetRmid(record) == RM_TBLSPC_ID && (XLogRecGetInfo(record) & ~XLR_INFO_MASK) == XLOG_TBLSPC_DROP);
}

bool IsTableSpaceCreate(XLogReaderState *record)
{
    return (XLogRecGetRmid(record) == RM_TBLSPC_ID &&
            ((XLogRecGetInfo(record) & ~XLR_INFO_MASK) == XLOG_TBLSPC_CREATE ||
             (XLogRecGetInfo(record) & ~XLR_INFO_MASK) == XLOG_TBLSPC_RELATIVE_CREATE));
}

/*
 * Drop a whole database during XLOG replay
 *
 * As above, but for DROP DATABASE instead of dropping a single rel
 */
void XLogDropDatabase(Oid dbid)
{
    /*
     * This is unnecessarily heavy-handed, as it will close SMgrRelation
     * objects for other databases as well. DROP DATABASE occurs seldom enough
     * that it's not worth introducing a variant of smgrclose for just this
     * purpose. XXX: Or should we rather leave the smgr entries dangling?
     */
    smgrcloseall();

    forget_invalid_pages_db(dbid);
}

/*
 * Truncate a relation during XLOG replay
 *
 * We need to clean up any open "invalid-page" records for the dropped pages.
 */
void XLogTruncateRelation(XLogReaderState *record, const RelFileNode &rnode, ForkNumber forkNum, BlockNumber nblocks)
{
    forget_invalid_pages(rnode, forkNum, nblocks);
}

void XLogTruncateRelation(RelFileNode rnode, ForkNumber forkNum, BlockNumber nblocks)
{
    forget_invalid_pages(rnode, forkNum, nblocks);
}

/*
 * Read 'count' bytes from WAL into 'buf', starting at location 'startptr'
 * in timeline 'tli'. Will open, and keep open, one WAL segment stored in the static file
 * descriptor 'sendFile'. This means if XLogRead is used once, there will
 * always be one descriptor left open until the process ends, but never
 * more than one. This is very similar to pg_waldump's XLogDumpXLogRead and to XLogRead
 * in walsender.c but for small differences (such as lack of ereport() in
 * front-end). Probably these should be merged at some point.
 */
static void XLogRead(char *buf, TimeLineID tli, XLogRecPtr startptr, Size count)
{
    char *p = NULL;
    XLogRecPtr recptr;
    Size nbytes;
    errno_t errorno = EOK;

    p = buf;
    recptr = startptr;
    nbytes = count;

    while (nbytes > 0) {
        uint32 startoff;
        int segbytes;
        int readbytes;

        startoff = recptr % XLogSegSize;

        /* Do we need to switch to a different xlog segment? */
        if (t_thrd.xlog_cxt.sendFile < 0 || !XLByteInSeg(recptr, t_thrd.xlog_cxt.sendSegNo) ||
            t_thrd.xlog_cxt.sendTLI != tli) {
            char path[MAXPGPATH];

            if (t_thrd.xlog_cxt.sendFile >= 0)
                close(t_thrd.xlog_cxt.sendFile);

            XLByteToSeg(recptr, t_thrd.xlog_cxt.sendSegNo);

            errorno = snprintf_s(path, MAXPGPATH, MAXPGPATH - 1, XLOGDIR "/%08X%08X%08X", tli,
                                 (uint32)((t_thrd.xlog_cxt.sendSegNo) / XLogSegmentsPerXLogId),
                                 (uint32)((t_thrd.xlog_cxt.sendSegNo) % XLogSegmentsPerXLogId));
            securec_check_ss(errorno, "", "");

            t_thrd.xlog_cxt.sendFile = BasicOpenFile(path, O_RDONLY | PG_BINARY, 0);

            if (t_thrd.xlog_cxt.sendFile < 0) {
                if (errno == ENOENT)
                    ereport(ERROR, (errcode_for_file_access(),
                                    errmsg("requested WAL segment %s has already been removed", path)));
                else
                    ereport(ERROR, (errcode_for_file_access(), errmsg("could not open file \"%s\": %m", path)));
            }
            t_thrd.xlog_cxt.sendOff = 0;
            t_thrd.xlog_cxt.sendTLI = tli;
        }

        /* Need to seek in the file? */
        if (t_thrd.xlog_cxt.sendOff != startoff) {
            if (lseek(t_thrd.xlog_cxt.sendFile, (off_t)startoff, SEEK_SET) < 0) {
                char path[MAXPGPATH];

                errorno = snprintf_s(path, MAXPGPATH, MAXPGPATH - 1, XLOGDIR "/%08X%08X%08X", tli,
                                     (uint32)((t_thrd.xlog_cxt.sendSegNo) / XLogSegmentsPerXLogId),
                                     (uint32)((t_thrd.xlog_cxt.sendSegNo) % XLogSegmentsPerXLogId));
                securec_check_ss(errorno, "", "");
                (void)close(t_thrd.xlog_cxt.sendFile);
                t_thrd.xlog_cxt.sendFile = -1;
                ereport(ERROR, (errcode_for_file_access(),
                                errmsg("could not seek in log segment %s to offset %u: %m", path, startoff)));
            }
            t_thrd.xlog_cxt.sendOff = startoff;
        }

        /* How many bytes are within this segment? */
        if (nbytes > (XLogSegSize - startoff))
            segbytes = (int)(XLogSegSize - startoff);
        else
            segbytes = (int)nbytes;

        pgstat_report_waitevent(WAIT_EVENT_WAL_READ);
        readbytes = (int)read(t_thrd.xlog_cxt.sendFile, p, segbytes);
        pgstat_report_waitevent(WAIT_EVENT_END);
        if (readbytes <= 0) {
            char path[MAXPGPATH];

            errorno = snprintf_s(path, MAXPGPATH, MAXPGPATH - 1, XLOGDIR "/%08X%08X%08X", tli,
                                 (uint32)((t_thrd.xlog_cxt.sendSegNo) / XLogSegmentsPerXLogId),
                                 (uint32)((t_thrd.xlog_cxt.sendSegNo) % XLogSegmentsPerXLogId));
            securec_check_ss(errorno, "", "");

            (void)close(t_thrd.xlog_cxt.sendFile);
            t_thrd.xlog_cxt.sendFile = -1;
            ereport(ERROR, (errcode_for_file_access(),
                            errmsg("could not read from log segment %s, offset %u, length %d, readbytes %d: %m", path,
                                   t_thrd.xlog_cxt.sendOff, segbytes, readbytes)));
        }

        /* Update state for read */
        XLByteAdvance(recptr, readbytes);

        t_thrd.xlog_cxt.sendOff += readbytes;
        nbytes -= readbytes;
        p += readbytes;
    }
}

/*
 * read_page callback for reading local xlog files
 *
 * Public because it would likely be very helpful for someone writing another
 * output method outside walsender, e.g. in a bgworker.
 *
 * description: The walsender has its own version of this, but it relies on the
 * walsender's latch being set whenever WAL is flushed. No such infrastructure
 * exists for normal backends, so we have to do a check/sleep/repeat style of
 * loop for now.
 */
int read_local_xlog_page(XLogReaderState *state, XLogRecPtr targetPagePtr, int reqLen, XLogRecPtr targetRecPtr,
                         char *cur_page, TimeLineID *pageTLI)
{
    XLogRecPtr read_upto, loc, loc_page;
    int count;

    loc = targetPagePtr;
    XLByteAdvance(loc, reqLen);

    while (true) {
        /*
         * description: we're going to have to do something more intelligent about
         * timelines on standbys. Use readTimeLineHistory() and
         * tliOfPointInHistory() to get the proper LSN? For now we'll catch
         * that case earlier, but the code and description is left in here for when
         * that changes.
         */
        if (!RecoveryInProgress()) {
            *pageTLI = t_thrd.xlog_cxt.ThisTimeLineID;
            read_upto = GetFlushRecPtr();
        } else
            read_upto = GetXLogReplayRecPtr(pageTLI);

        if (XLByteLE(loc, read_upto))
            break;

        CHECK_FOR_INTERRUPTS();
        pg_usleep(1000L);
    }

    loc_page = targetPagePtr;
    XLByteAdvance(loc_page, XLOG_BLCKSZ);

    if (XLByteLE(loc_page, read_upto)) {
        /*
         * more than one block available; read only that block, have caller
         * come back if they need more.
         */
        count = XLOG_BLCKSZ;
    } else if (XLByteLT(read_upto, loc)) {
        /* not enough data there */
        return -1;
    } else {
        /* enough bytes available to satisfy the request */
        count = (int)(read_upto - targetPagePtr);
    }

    /*
     * Even though we just determined how much of the page can be validly read
     * as 'count', read the whole page anyway. It's guaranteed to be
     * zero-padded up to the page boundary if it's incomplete.
     */
    XLogRead(cur_page, *pageTLI, targetPagePtr, XLOG_BLCKSZ);

    /* number of valid bytes in the buffer */
    return count;
}
