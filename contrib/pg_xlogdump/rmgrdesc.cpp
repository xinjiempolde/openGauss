/*
 * rmgrdesc.cpp
 *
 * pg_xlogdump resource managers definition
 *
 * contrib/pg_xlogdump/rmgrdesc.cpp
 */
#define FRONTEND 1
#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/clog.h"
#include "access/gin.h"
#include "access/gist_private.h"
#include "access/hash.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/nbtree.h"
#include "access/rmgr.h"
#include "access/spgist.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "catalog/storage_xlog.h"
#include "commands/dbcommands.h"
#include "commands/sequence.h"
#include "commands/tablespace.h"
#include "replication/slot.h"
#ifdef PGXC
#include "pgxc/barrier.h"
#endif
#include "rmgrdesc.h"
#include "storage/standby.h"
#include "utils/relmapper.h"
#include "replication/slot.h"
#ifdef ENABLE_MOT
#include "storage/mot/mot_xlog.h"
#endif

#define PG_RMGR(symname, name, redo, desc, startup, cleanup, safe_restartpoint) {name, desc},

const RmgrDescData RmgrDescTable[RM_MAX_ID + 1] = {
#include "access/rmgrlist.h"
};
