#ifndef NEU_UTILS_H
#define NEU_UTILS_H

#include <vector>
#include <mutex>
#include <unordered_map>
#include <condition_variable>
#include <zmq.hpp>
#include <c.h>
#include "access/neu_utils/message.pb.h"
#include "blocking_concurrent_queue.hpp"

#include "storage/item/itemptr.h"

#define ENABLE_NEU_LOG 1
typedef uint64_t UniqueKey;
typedef uint64_t RID;

enum NeuTransactionState {
    STATE_INVALID = 0,
    STATE_COMMIT,
    STATE_ABORT
};

// 主要是为了线程之间的通信
class NeuTransactionManager {
public:
    NeuTransactionManager() = default;
    std::condition_variable cv_;
    NeuTransactionState txn_state_{STATE_INVALID};
};

// UniqueKey和TID的映射
// TODO(singheart): 目前使用的HashMap实现，存在与内存中，后续应该考虑持久化的问题
class KeyAndTidTranslator {
public:
    KeyAndTidTranslator() = default;
    RID TransformOtidToTid(ItemPointer otid) {
        RID rid = (RID)(otid->ip_blkid.bi_hi << 32 | otid->ip_blkid.bi_lo << 16 | otid->ip_posid);
        return rid;
    }
    void InsertKeyAndTid(Oid table_oid, UniqueKey key, ItemPointerData tid) {
        RID rid = TransformOtidToTid(&tid);
        fake_index_[table_oid][key] = tid;
        fake_index_reverse_[table_oid][rid] = key;
    }
    // 通过TID获取UniqueKey
    UniqueKey GetKeyWithTid(Oid table_oid, ItemPointerData tid) {
        RID rid = TransformOtidToTid(&tid);
        return fake_index_reverse_[table_oid][rid];
    }
    // 通过UniqueKey获取TID
    ItemPointerData GetTidWithKey(Oid table_oid, UniqueKey key) {
        return fake_index_[table_oid][key];
    }
private:
    std::unordered_map<Oid, std::unordered_map<UniqueKey, ItemPointerData>> fake_index_;
    std::unordered_map<Oid, std::unordered_map<RID, UniqueKey>> fake_index_reverse_;
};

extern moodycamel::BlockingConcurrentQueue<std::unique_ptr<zmq::message_t>> transaction_message_queue_;
extern moodycamel::BlockingConcurrentQueue<std::unique_ptr<proto::Message>> apply_log_message_queue_;
extern std::string taas_ipv4_addr;
extern thread_local std::vector<std::unique_ptr<proto::Row>> ReadWriteSetInTxn_;
extern std::mutex cv_mutex_;
extern std::unordered_map<TransactionId, std::shared_ptr<NeuTransactionManager>> cv_map_;
extern std::unordered_map<Oid, std::unordered_map<UniqueKey, ItemPointerData>> fake_index_;
extern KeyAndTidTranslator tid_translator_;

UniqueKey AllocateUniqueKey();
void NeuPrintLog(const char *format, ...);
void SendWorkerThreadMain();
void ResponseWorkerThreadMain();
void ApplyLogWorkerThreadMain();
std::string GetIPV4Address();
bool ApplyWriteSet(std::unique_ptr<proto::Message> log_message);
#endif //NEU_UTILS_H
