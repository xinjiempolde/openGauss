#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/xlog.h"
#include "access/heapam.h"
#include "utils/postinit.h"
#include "storage/ipc.h"

#include "miscadmin.h"
#include "pgstat.h"

extern void ApplyLogWorkerThreadMain();
// 后台线程，从5556端口接受TaaS发来的写集，并进行回放落盘
void NeuApplyLogReceiverMain() {
  MemoryContext datawriter_context;
  t_thrd.role = NEU_APPLY_LOG_RECEIVER;

  /*
   * Create a resource owner to keep track of our resources (not clear that
   * we need this, but may as well have one).
   */
  t_thrd.utils_cxt.CurrentResourceOwner = ResourceOwnerCreate(NULL, "apply log", MEMORY_CONTEXT_STORAGE);

  /*
   * Create a memory context that we will do all our work in.  We do this so
   * that we can reset the context during error recovery and thereby avoid
   * possible memory leaks.  Formerly this code just ran in
   * t_thrd.top_mem_cxt, but resetting that would be a really bad idea.
   */
  datawriter_context = AllocSetContextCreate(t_thrd.top_mem_cxt,
                                            "apply log",
                                            ALLOCSET_DEFAULT_MINSIZE,
                                            ALLOCSET_DEFAULT_INITSIZE,
                                            ALLOCSET_DEFAULT_MAXSIZE);
  (void)MemoryContextSwitchTo(datawriter_context);
  t_thrd.proc_cxt.PostInit->SetDatabaseAndUser("tpcc", InvalidOid, "singheart");
  t_thrd.proc_cxt.PostInit->InitNeuApplyLogReceiver();
  ApplyLogWorkerThreadMain();
}