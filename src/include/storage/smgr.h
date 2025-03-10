/* -------------------------------------------------------------------------
 *
 * smgr.h
 *	  storage manager switch public interface declarations.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/smgr.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef SMGR_H
#define SMGR_H

#include "fmgr.h"
#include "lib/ilist.h"
#include "storage/buf/block.h"
#include "storage/relfilenode.h"

#include "utils/rel.h"
#include "utils/rel_gs.h"
#include "vecexecutor/vectorbatch.h"
#include "nodes/bitmapset.h"


/*
 * smgr.c maintains a table of SMgrRelation objects, which are essentially
 * cached file handles.  An SMgrRelation is created (if not already present)
 * by smgropen(), and destroyed by smgrclose().  Note that neither of these
 * operations imply I/O, they just create or destroy a hashtable entry.
 * (But smgrclose() may release associated resources, such as OS-level file
 * descriptors.)
 *
 * An SMgrRelation may have an "owner", which is just a pointer to it from
 * somewhere else; smgr.c will clear this pointer if the SMgrRelation is
 * closed.	We use this to avoid dangling pointers from relcache to smgr
 * without having to make the smgr explicitly aware of relcache.  There
 * can't be more than one "owner" pointer per SMgrRelation, but that's
 * all we need.
 *
 * SMgrRelations that do not have an "owner" are considered to be transient,
 * and are deleted at end of transaction.
 */
typedef struct SMgrRelationData {
    /* rnode is the hashtable lookup key, so it must be first! */
    RelFileNodeBackend smgr_rnode; /* relation physical identifier */

    /* pointer to owning pointer, or NULL if none */
    struct SMgrRelationData** smgr_owner;

    /*
     * These next three fields are not actually used or manipulated by smgr,
     * except that they are reset to InvalidBlockNumber upon a cache flush
     * event (in particular, upon truncation of the relation).	Higher levels
     * store cached state here so that it will be reset when truncation
     * happens.  In all three cases, InvalidBlockNumber means "unknown".
     */
    BlockNumber smgr_targblock;   /* current insertion target block */
    BlockNumber smgr_fsm_nblocks; /* last known size of fsm fork */
    BlockNumber smgr_vm_nblocks;  /* last known size of vm fork */

    int smgr_bcmarry_size;
    BlockNumber* smgr_bcm_nblocks; /* last known size of bcm fork */

    /* additional public fields may someday exist here */

    /*
     * Fields below here are intended to be private to smgr.c and its
     * submodules.	Do not touch them from elsewhere.
     */
     int smgr_which; /* storage manager selector */


    /* for md.c; NULL for forks that are not open */
    int md_fdarray_size;
    struct _MdfdVec** md_fd;

    /* hash table storing specific bucket node's smgr pointer */
    HTAB* bucketnodes_smgrhash;
    
    /* if unowned, list link in list of all unowned SMgrRelations */
    dlist_node node;
} SMgrRelationData;


typedef SMgrRelationData* SMgrRelation;

#define SmgrIsTemp(smgr) RelFileNodeBackendIsTemp((smgr)->smgr_rnode)

extern void smgrinit(void);
extern SMgrRelation smgropen(const RelFileNode& rnode, BackendId backend, int col = 0, const oidvector* bucketlist  = NULL);
extern bool smgrexists(SMgrRelation reln, ForkNumber forknum);
extern void smgrsetowner(SMgrRelation* owner, SMgrRelation reln);
extern void smgrclearowner(SMgrRelation* owner, SMgrRelation reln);
extern void smgrclose(SMgrRelation reln);
extern void smgrcloseall(void);
extern void smgrclosenode(const RelFileNodeBackend& rnode);
extern void smgrcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo );
extern void smgrdounlink(SMgrRelation reln, bool isRedo);
extern void smgrdounlinkfork(SMgrRelation reln, ForkNumber forknum, bool isRedo);
extern void smgrextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, const char* buffer, bool skipFsync);
extern void smgrprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum);
extern void smgrread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, char* buffer);
extern void smgrwrite(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, const char* buffer, bool skipFsync);
extern void smgrwriteback(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, BlockNumber nblocks);
extern BlockNumber smgrnblocks(SMgrRelation reln, ForkNumber forknum);
extern void smgrtruncatefunc(SMgrRelation reln, ForkNumber forknum, BlockNumber nblocks);
extern void smgrtruncate(SMgrRelation reln, ForkNumber forknum, BlockNumber nblocks);
extern void smgrimmedsync(SMgrRelation reln, ForkNumber forknum);
extern void smgrpreckpt(void);
extern void smgrsync(void);
extern void smgrpostckpt(void);

extern void AtEOXact_SMgr(void);

/* internals: move me elsewhere -- ay 7/94 */

/* in md.c */
extern void mdinit(void);
extern void mdclose(SMgrRelation reln, ForkNumber forknum);
extern void mdcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo);
extern void smgrcreatebuckets(SMgrRelation reln, ForkNumber forknum, bool isRedo);

extern bool mdexists(SMgrRelation reln, ForkNumber forknum);
extern void mdunlink(const RelFileNodeBackend& rnode, ForkNumber forknum, bool isRedo);
extern void mdextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, const char* buffer, bool skipFsync);
extern void mdprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum);
extern void mdread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, char* buffer);
extern void mdwrite(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, const char* buffer, bool skipFsync);
extern void mdwriteback(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, BlockNumber nblocks);
extern BlockNumber mdnblocks(SMgrRelation reln, ForkNumber forknum);
extern void mdtruncate(SMgrRelation reln, ForkNumber forknum, BlockNumber nblocks);
extern void mdimmedsync(SMgrRelation reln, ForkNumber forknum);
extern void mdpreckpt(void);
extern void mdsync(void);
extern void mdpostckpt(void);
extern char* mdsegpath(const RelFileNode& rnode, ForkNumber forknum, BlockNumber blkno);

extern void SetForwardFsyncRequests(void);
extern void RememberFsyncRequest(const RelFileNode& rnode, ForkNumber forknum, BlockNumber segno);
extern void ForgetRelationFsyncRequests(const RelFileNode& rnode, ForkNumber forknum);
extern void ForgetDatabaseFsyncRequests(Oid dbid);
extern void smgrsync_for_dw(void);
extern void smgrsync_with_absorption(void);

/* smgrtype.c */
extern Datum smgrout(PG_FUNCTION_ARGS);
extern Datum smgrin(PG_FUNCTION_ARGS);
extern Datum smgreq(PG_FUNCTION_ARGS);
extern Datum smgrne(PG_FUNCTION_ARGS);

extern void partition_create_new_storage(Relation rel, Partition part, const RelFileNodeBackend& filenode);
extern ScalarToDatum GetTransferFuncByTypeOid(Oid attTypeOid);
#endif /* SMGR_H */
