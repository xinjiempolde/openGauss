/* -------------------------------------------------------------------------
 *
 * buf_init.cpp
 *	  buffer manager initialization routines
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/gausskernel/storage/buffer/buf_init.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "storage/dfs/dfscache_mgr.h"

#include "postgres.h"
#include "knl/knl_variable.h"
#include "gs_bbox.h"
#include "storage/buf/bufmgr.h"
#include "storage/buf/buf_internals.h"
#include "storage/ipc.h"
#include "storage/cucache_mgr.h"
#include "pgxc/pgxc.h"
#include "postmaster/pagewriter.h"

const int PAGE_QUEUE_SLOT_MULTI_NBUFFERS = 5;

static void MemsetPageQueue(char *buffer, Size len)
{
    int rc;
    while (len > 0) {
        if (len < SECUREC_MEM_MAX_LEN) {
            rc = memset_s(buffer, len, 0, len);
            securec_check(rc, "", "");
            return;
        } else {
            rc = memset_s(buffer, SECUREC_MEM_MAX_LEN, 0, SECUREC_MEM_MAX_LEN);
            securec_check(rc, "", "");
            len -= SECUREC_MEM_MAX_LEN;
            buffer += SECUREC_MEM_MAX_LEN;
        }
    }
}

/*
 * Data Structures:
 *		buffers live in a freelist and a lookup data structure.
 *
 *
 * Buffer Lookup:
 *		Two important notes.  First, the buffer has to be
 *		available for lookup BEFORE an IO begins.  Otherwise
 *		a second process trying to read the buffer will
 *		allocate its own copy and the buffer pool will
 *		become inconsistent.
 *
 * Buffer Replacement:
 *		see freelist.c.  A buffer cannot be replaced while in
 *		use either by data manager or during IO.
 *
 *
 * Synchronization/Locking:
 *
 * IO_IN_PROGRESS -- this is a flag in the buffer descriptor.
 *		It must be set when an IO is initiated and cleared at
 *		the end of the IO.	It is there to make sure that one
 *		process doesn't start to use a buffer while another is
 *		faulting it in.  see WaitIO and related routines.
 *
 * refcount --	Counts the number of processes holding pins on a buffer.
 *		A buffer is pinned during IO and immediately after a BufferAlloc().
 *		Pins must be released before end of transaction.  For efficiency the
 *		shared refcount isn't increased if a individual backend pins a buffer
 *		multiple times. Check the PrivateRefCount infrastructure in bufmgr.c.
 */
/*
 * Initialize shared buffer pool
 *
 * This is called once during shared-memory initialization (either in the
 * postmaster, or in a standalone backend).
 */
void InitBufferPool(void)
{
    bool found_bufs = false;
    bool found_descs = false;
    bool found_buf_ckpt = false;
    uint64 buffer_size;

    t_thrd.storage_cxt.BufferDescriptors = (BufferDescPadded *)CACHELINEALIGN(
        ShmemInitStruct("Buffer Descriptors",
                        g_instance.attr.attr_storage.NBuffers * sizeof(BufferDescPadded) + PG_CACHE_LINE_SIZE,
                        &found_descs));

    /* Init candidate buffer list and candidate buffer free map */
    candidate_buf_init();

#ifdef __aarch64__
    buffer_size = g_instance.attr.attr_storage.NBuffers * (Size)BLCKSZ + PG_CACHE_LINE_SIZE;
    t_thrd.storage_cxt.BufferBlocks =
        (char *)CACHELINEALIGN(ShmemInitStruct("Buffer Blocks", buffer_size, &found_bufs));
#else
    buffer_size = g_instance.attr.attr_storage.NBuffers * (Size)BLCKSZ;
    t_thrd.storage_cxt.BufferBlocks = (char *)ShmemInitStruct("Buffer Blocks", buffer_size, &found_bufs);
#endif

    if (BBOX_BLACKLIST_SHARE_BUFFER) {
        bbox_blacklist_add(SHARED_BUFFER, t_thrd.storage_cxt.BufferBlocks, buffer_size);
    }

    /*
     * The array used to sort to-be-checkpointed buffer ids is located in
     * shared memory, to avoid having to allocate significant amounts of
     * memory at runtime. As that'd be in the middle of a checkpoint, or when
     * the checkpointer is restarted, memory allocation failures would be
     * painful.
     */
    g_instance.ckpt_cxt_ctl->CkptBufferIds =
        (CkptSortItem *)ShmemInitStruct("Checkpoint BufferIds",
                                        g_instance.attr.attr_storage.NBuffers * sizeof(CkptSortItem), &found_buf_ckpt);

    if (g_instance.attr.attr_storage.enableIncrementalCheckpoint && g_instance.ckpt_cxt_ctl->dirty_page_queue == NULL) {
        g_instance.ckpt_cxt_ctl->dirty_page_queue_size = g_instance.attr.attr_storage.NBuffers *
                                                         PAGE_QUEUE_SLOT_MULTI_NBUFFERS;
        MemoryContext oldcontext = MemoryContextSwitchTo(g_instance.increCheckPoint_context);

        Size queue_mem_size = g_instance.ckpt_cxt_ctl->dirty_page_queue_size * sizeof(DirtyPageQueueSlot);
        g_instance.ckpt_cxt_ctl->dirty_page_queue = (DirtyPageQueueSlot *)malloc(queue_mem_size);
        if (g_instance.ckpt_cxt_ctl->dirty_page_queue == NULL) {
            ereport(ERROR, (errmodule(MOD_INCRE_CKPT), errmsg("Memory allocation failed.\n")));
        }

        MemsetPageQueue((char*)g_instance.ckpt_cxt_ctl->dirty_page_queue, queue_mem_size);
        (void)MemoryContextSwitchTo(oldcontext);
    }

    if (found_descs || found_bufs || found_buf_ckpt) {
        /* both should be present or neither */
        Assert(found_descs && found_bufs && found_buf_ckpt);
        /* note: this path is only taken in EXEC_BACKEND case */
    } else {
        int i;

        /*
         * Initialize all the buffer headers.
         */
        for (i = 0; i < g_instance.attr.attr_storage.NBuffers; i++) {
            BufferDesc *buf = GetBufferDescriptor(i);
            CLEAR_BUFFERTAG(buf->tag);

            pg_atomic_init_u32(&buf->state, 0);
            buf->wait_backend_pid = 0;

            buf->buf_id = i;
            buf->io_in_progress_lock = LWLockAssign(LWTRANCHE_BUFFER_IO_IN_PROGRESS);
            buf->content_lock = LWLockAssign(LWTRANCHE_BUFFER_CONTENT);
            pg_atomic_init_u64(&buf->rec_lsn, InvalidXLogRecPtr);
            buf->dirty_queue_loc = PG_UINT64_MAX;
        }
    }

    /* Init other shared buffer-management stuff */
    StrategyInitialize(!found_descs);

    /* Init Vector Buffer management stuff */
    DataCacheMgr::NewSingletonInstance();

    /* Init Meta data cache management stuff */
    MetaCacheMgr::NewSingletonInstance();

    /* Initialize per-backend file flush context */
    WritebackContextInit(t_thrd.storage_cxt.BackendWritebackContext, &u_sess->attr.attr_common.backend_flush_after);
}

/*
 * BufferShmemSize
 *
 * compute the size of shared memory for the buffer pool including
 * data pages, buffer descriptors, hash tables, etc.
 */
Size BufferShmemSize(void)
{
    Size size = 0;

    /* size of buffer descriptors */
    size = add_size(size, mul_size(g_instance.attr.attr_storage.NBuffers, sizeof(BufferDescPadded)));
    size = add_size(size, PG_CACHE_LINE_SIZE);

    /* size of data pages */
    size = add_size(size, mul_size(g_instance.attr.attr_storage.NBuffers, BLCKSZ));
#ifdef __aarch64__
    size = add_size(size, PG_CACHE_LINE_SIZE);
#endif
    /* size of stuff controlled by freelist.c */
    size = add_size(size, StrategyShmemSize());

    /* size of checkpoint sort array in bufmgr.c */
    size = add_size(size, mul_size(g_instance.attr.attr_storage.NBuffers, sizeof(CkptSortItem)));

    /* size of candidate buffers */
    size = add_size(size, mul_size(g_instance.attr.attr_storage.NBuffers, sizeof(Buffer)));

    /* size of candidate free map */
    size = add_size(size, mul_size(g_instance.attr.attr_storage.NBuffers, sizeof(bool)));

    return size;
}
