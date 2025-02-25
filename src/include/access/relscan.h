/* -------------------------------------------------------------------------
 *
 * relscan.h
 *	  POSTGRES relation scan descriptor definitions.
 *
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/relscan.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef RELSCAN_H
#define RELSCAN_H

#include "access/genam.h"
#include "access/heapam.h"
#include "access/itup.h"
#include "access/tupdesc.h"

#define PARALLEL_SCAN_GAP 100

/* ----------------------------------------------------------------
 *				 Scan State Information
 * ----------------------------------------------------------------
 */
#define IsValidScanDesc(sd)  (sd != NULL)

typedef struct HeapScanDescData {
    TableScanDescData rs_base;  /* AM independent part of the descriptor */

    /* scan parameters */
    bool rs_allow_strat;  /* allow or disallow use of access strategy */

    /* scan current state */
    TupleDesc rs_tupdesc;  /* heap tuple descriptor for rs_ctup */
    /* NB: if rs_cbuf is not InvalidBuffer, we hold a pin on that buffer */
    ItemPointerData rs_mctid; /* marked scan position, if any */

    /* these fields only used in page-at-a-time mode and for bitmap scans */
    int rs_mindex;                                   /* marked tuple's saved index */
    int dop;                                         /* scan parallel degree */
    /* put decompressed tuple data into rs_ctbuf be careful  , when malloc memory  should give extra mem for
     *xs_ctbuf_hdr. t_bits which is varlength arr
     */
    HeapTupleData rs_ctup; /* current tuple in scan, if any */
    HeapTupleHeaderData rs_ctbuf_hdr;
} HeapScanDescData;

#define SizeofHeapScanDescData (offsetof(HeapScanDescData, rs_ctbuf_hdr) + SizeofHeapTupleHeader)

struct ScanState;

/*
 * The ScanDescData for hash-bucket table
 */
typedef struct HBktTblScanDescData {
    // !! rs_rd MUST BE FIRST MEMBER !!
    Relation rs_rd;

    struct ScanState* scanState;
    List* hBktList;  /* hash bucket list that used to scan */
    int      curr_slot;
    Relation currBktRel;
    TableScanDesc currBktScan;
} HBktTblScanDescData;

typedef struct HBktTblScanDescData* HBktTblScanDesc;

struct IndexFetchTableData;
/*
 * We use the same IndexScanDescData structure for both amgettuple-based
 * and amgetbitmap-based index scans.  Some fields are only relevant in
 * amgettuple-based scans.
 */
typedef struct IndexScanDescData {
    /* scan parameters */    
    // !! heapRelation MUST BE FIRST MEMBER !!
    Relation heapRelation;  /* heap relation descriptor, or NULL */

    Relation indexRelation; /* index relation descriptor */
    GPIScanDesc xs_gpi_scan; /* global partition index scan use information */
    Snapshot xs_snapshot;   /* snapshot to see */
    int numberOfKeys;       /* number of index qualifier conditions */
    int numberOfOrderBys;   /* number of ordering operators */
    ScanKey keyData;        /* array of index qualifier descriptors */
    ScanKey orderByData;    /* array of ordering op descriptors */
    bool xs_want_itup;      /* caller requests index tuples */
    bool xs_want_ext_oid;    /* global partition index need partition oid */

    /* signaling to index AM about killing index tuples */
    bool kill_prior_tuple;      /* last-returned tuple is dead */
    bool ignore_killed_tuples;  /* do not return killed entries */
    bool xactStartedInRecovery; /* prevents killing/seeing killed
                                 * tuples */

    /* index access method's private state */
    void* opaque; /* access-method-specific info */

    /* in an index-only scan, this is valid after a successful amgettuple */
    IndexTuple xs_itup;    /* index tuple returned by AM */
    TupleDesc xs_itupdesc; /* rowtype descriptor of xs_itup */

    /* xs_ctup/xs_cbuf/xs_recheck are valid after a successful index_getnext */
    HeapTupleData xs_ctup; /* current heap tuple, if any */
    Buffer xs_cbuf;        /* current heap buffer in scan, if any */
    /* NB: if xs_cbuf is not InvalidBuffer, we hold a pin on that buffer */
    bool xs_recheck; /* T means scan keys must be rechecked */

    /* state data for traversing HOT chains in index_getnext */
    bool xs_continue_hot; /* T if must keep walking HOT chain */
    IndexFetchTableData *xs_heapfetch;
    /* put decompressed heap tuple data into xs_ctbuf_hdr be careful! when malloc memory  should give extra mem for
     *xs_ctbuf_hdr. t_bits which is varlength arr
     */
    HeapTupleHeaderData xs_ctbuf_hdr;
    /* DO NOT add any other members here. xs_ctbuf_hdr must be the last one. */
} IndexScanDescData;

#define SizeofIndexScanDescData (offsetof(IndexScanDescData, xs_ctbuf_hdr) + SizeofHeapTupleHeader)

/* Get partition heap oid for bitmap index scan */
#define IndexScanGetPartHeapOid(scan)                                                                           \
    ((scan)->indexRelation != NULL                                                                              \
            ? (RelationIsPartition((scan)->indexRelation) ? (scan)->indexRelation->rd_partHeapOid : InvalidOid) \
            : InvalidOid)

/*
 * When the global partition index is used for index scanning,
 * checks whether the partition table needs to be
 * switched each time an indextuple is obtained.
 */
#define IndexScanNeedSwitchPartRel(scan) \
    ((scan)->xs_want_ext_oid && GPIScanCheckPartOid((scan)->xs_gpi_scan, (scan)->heapRelation->rd_id))

typedef struct HBktIdxScanDescData {
    // !! rs_rd MUST BE FIRST MEMBER !!
    Relation rs_rd;  /* heap relation descriptor */

    Relation idx_rd;  /* index relation descriptor */
    struct ScanState* scanState;
    List* hBktList;
    int      curr_slot;
    Relation currBktHeapRel;
    Relation currBktIdxRel;
    IndexScanDescData* currBktIdxScan;
} HBktIdxScanDescData;

typedef HBktIdxScanDescData* HBktIdxScanDesc;

/* Struct for heap-or-index scans of system tables */
typedef struct SysScanDescData {
    Relation heap_rel;   /* catalog being scanned */
    Relation irel;       /* NULL if doing heap scan */
    HeapScanDesc scan;   /* only valid in heap-scan case */
    IndexScanDesc iscan; /* only valid in index-scan case */
} SysScanDescData;

#endif /* RELSCAN_H */
