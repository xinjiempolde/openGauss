#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/xlog.h"
#include "access/heapam.h"
#include "utils/postinit.h"
#include "storage/ipc.h"

#include "miscadmin.h"
#include "pgstat.h"

extern void ResponseWorkerThreadMain();
// 后台线程，从5552端口从TaaS获取事务状态(Commit or Abort)
void NeuResponseReceiverMain() {
  MemoryContext datawriter_context;
  t_thrd.role = NEU_RESPONSE_RECEIVER;

  /*
   * Create a resource owner to keep track of our resources (not clear that
   * we need this, but may as well have one).
   */
  t_thrd.utils_cxt.CurrentResourceOwner = ResourceOwnerCreate(NULL, "response receiver", MEMORY_CONTEXT_STORAGE);

  /*
   * Create a memory context that we will do all our work in.  We do this so
   * that we can reset the context during error recovery and thereby avoid
   * possible memory leaks.  Formerly this code just ran in
   * t_thrd.top_mem_cxt, but resetting that would be a really bad idea.
   */
  datawriter_context = AllocSetContextCreate(t_thrd.top_mem_cxt,
                                            "response receiver",
                                            ALLOCSET_DEFAULT_MINSIZE,
                                            ALLOCSET_DEFAULT_INITSIZE,
                                            ALLOCSET_DEFAULT_MAXSIZE);
  (void)MemoryContextSwitchTo(datawriter_context);
  t_thrd.proc_cxt.PostInit->SetDatabaseAndUser("learn", InvalidOid, "singheart");
  t_thrd.proc_cxt.PostInit->InitNeuResponseReceiver();
  ResponseWorkerThreadMain();
}