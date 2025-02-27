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

#define ENABLE_NEU_LOG 1

enum NeuTransactionState {
    STATE_INVALID = 0,
    STATE_COMMIT,
    STATE_ABORT
};

// 主要是为了线程之间的通信
class NeuTransactionManager {
public:
    NeuTransactionManager() {}
    std::condition_variable cv_;
    NeuTransactionState txn_state_{STATE_INVALID};
};

extern moodycamel::BlockingConcurrentQueue<std::unique_ptr<zmq::message_t>> transaction_message_queue_;
extern moodycamel::BlockingConcurrentQueue<std::unique_ptr<proto::Message>> apply_log_message_queue_;
extern std::string taas_ipv4_addr;
extern thread_local std::vector<std::unique_ptr<proto::Row>> ReadWriteSetInTxn_;
extern std::mutex cv_mutex_;
extern std::unordered_map<TransactionId, std::shared_ptr<NeuTransactionManager>> cv_map_;

uint64_t AllocateUniqueKey();
void NeuPrintLog(const char *format, ...);
void SendWorkerThreadMain();
void ResponseWorkerThreadMain();
void ApplyLogWorkerThreadMain();
std::string GetIPV4Address();
bool ApplyWriteSet(std::unique_ptr<proto::Message> log_message);
#endif //NEU_UTILS_H
