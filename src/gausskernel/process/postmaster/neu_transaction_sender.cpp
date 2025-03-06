// add by singheart
#include "postgres.h"
#include "knl/knl_variable.h"
#include "utils/postinit.h"

#include "miscadmin.h"
#include "pgstat.h"

extern void SendWorkerThreadMain();

// 后台线程，将存储的读写集通过端口5551发送给TaaS
void NeuTransactionSenderMain() {
    MemoryContext datawriter_context;
    t_thrd.role = NEU_TRANSACTION_SENDER;

    /*
     * Create a resource owner to keep track of our resources (not clear that
     * we need this, but may as well have one).
     */
    t_thrd.utils_cxt.CurrentResourceOwner =
        ResourceOwnerCreate(NULL, "txn sender", MEMORY_CONTEXT_STORAGE);

    /*
     * Create a memory context that we will do all our work in.  We do this so
     * that we can reset the context during error recovery and thereby avoid
     * possible memory leaks.  Formerly this code just ran in
     * t_thrd.top_mem_cxt, but resetting that would be a really bad idea.
     */
    datawriter_context = AllocSetContextCreate(
        t_thrd.top_mem_cxt, "txn sender", ALLOCSET_DEFAULT_MINSIZE,
        ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);
    (void)MemoryContextSwitchTo(datawriter_context);
    t_thrd.proc_cxt.PostInit->SetDatabaseAndUser("tpcc", InvalidOid,
                                                 "singheart");
    t_thrd.proc_cxt.PostInit->InitNeuTransactionSender();
    SendWorkerThreadMain();
}