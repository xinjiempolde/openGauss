// 大部分的对openGauss的更改放到这个文件中
#include <cstdarg>
#include <cstdio>

#include <ifaddrs.h>
#include <arpa/inet.h>

#include "access/neu_utils/message.pb.h"
#include "access/neu_utils/neu_utils.h"
#include "access/neu_utils/snowflake_uid.h"
#include "utils/elog.h"

//=== NEU全局变量定义开始 ===//

// 用于存放将要发送的读写集
moodycamel::BlockingConcurrentQueue<std::unique_ptr<zmq::message_t>> transaction_message_queue_;
// 用于存放从5556端口接收的Apply Log
moodycamel::BlockingConcurrentQueue<std::unique_ptr<proto::Message>> apply_log_message_queue_;
std::string taas_ipv4_addr = "219.216.64.135";
std::atomic<bool> system_run_enable_{true};
// 用于缓存当前事务的读写集，线程私有变量，在CommitTransaction的时候才能确认所有的读写集收集完毕
// 由于是线程私有变量，只有当前线程能够插入数据，所以不用考虑线程并发问题，是线程安全的
thread_local std::vector<std::unique_ptr<proto::Row>> ReadWriteSetInTxn_{};
// 使用SnowFlake算法生成分布式UID
SnowflakeDistributeID uid_generator_{1, 1};
// TODO(singheart):这么做并发hashmap可能有性能问题
// 每个事务都对应一个条件变量，当CommitTransaction的时候，在自己的条件变量上等待
// 直到TaaS从5552端口将结果返回，调用notify_all唤醒阻塞的事务，继续完成Commit操作
std::mutex cv_mutex_;
std::unordered_map<TransactionId, std::shared_ptr<NeuTransactionManager>> cv_map_;
//=== NEU全局变量定义结束 ===//

// 将事务(或者说读写集)发送给TaaS
void SendWorkerThreadMain() {
    std::string zmq_remote_port = "5551";
    std::string zmq_remote_addr = "tcp://" + taas_ipv4_addr + ":" + zmq_remote_port;
    zmq::context_t context(1);
    zmq::socket_t send_socket(context, ZMQ_PUSH);
    zmq::send_flags zmq_flags(zmq::send_flags::none);
    std::unique_ptr<zmq::message_t> zmq_message;
    send_socket.connect(zmq_remote_addr);
    NeuPrintLog("connect to remote TaaS, address: %s\n", zmq_remote_addr.c_str());

    // 从并发队列中取出Transaction发送给TaaS
    while (system_run_enable_.load()) {
        transaction_message_queue_.wait_dequeue(zmq_message);
        // fprintf(stderr, "zmq message size is %lu\n", zmq_message->size());
        send_socket.send(*zmq_message, zmq_flags);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// 接受来自TaaS发来的消息，告诉我们提交还是回滚
void ResponseWorkerThreadMain() {
    std::string zmq_remote_port = "5552";
    std::string zmq_bind_addr = "tcp://*:" + zmq_remote_port;
    zmq::context_t context(1);
    zmq::socket_t listen_socket(context, ZMQ_PULL);
    int zmq_queue_len = 0;
    bool protobuf_deserialize_result;
    proto::Message taas_result_message;
    std::unique_ptr<zmq::message_t> zmq_message = std::make_unique<zmq::message_t>();
    listen_socket.setsockopt(ZMQ_RCVHWM, &zmq_queue_len, sizeof(zmq_queue_len));
    listen_socket.bind(zmq_bind_addr);
    fprintf(stderr, "bind address %s\n", zmq_bind_addr.c_str());
    while (system_run_enable_.load()) {
        listen_socket.recv(zmq_message.get());
        // fprintf(stderr, "recv from TaaS in 5552, data len is %lu\n", zmq_message->size());
        // 将TaaS反馈来的结果反序列化
        google::protobuf::io::ArrayInputStream input_stream(zmq_message->data(), zmq_message->size());
        protobuf_deserialize_result = taas_result_message.ParseFromZeroCopyStream(&input_stream);
        if (!protobuf_deserialize_result) {
            fprintf(stderr, "failed to deserialize result from taas\n");
            system_run_enable_.store(false);
        }
        if (taas_result_message.type_case() ==
            proto::Message::TypeCase::kReplyTxnResultToClient) {
            auto reply_result = taas_result_message.reply_txn_result_to_client();
            TransactionId xid = reply_result.client_txn_id();
            fprintf(stderr, "ReplyTxnResultToClient, csn %lu\n", xid);
            cv_mutex_.lock();
            auto txn_manager_iter = cv_map_.find(xid);
            if (txn_manager_iter == cv_map_.end()) {
                NeuPrintLog("failed to find xid: %lu", xid);
                continue;
            }

            // 找到了当前的TransactionID，设置NeuTransactionManager的相关状态
            auto txn_manager = txn_manager_iter->second;
            if (reply_result.txn_state() == proto::Commit) {
                txn_manager->txn_state_ = STATE_COMMIT;
            } else if (reply_result.txn_state() == proto::Abort) {
                txn_manager->txn_state_ = STATE_ABORT;
            }

            // 唤醒之前阻塞的事务
            txn_manager->cv_.notify_all();

            // TODO(singheart)：别忘记放锁，可用guard_lock替换？
            cv_mutex_.unlock();
            if (reply_result.txn_state() == proto::TxnState::Commit) {
                fprintf(stderr, "tell me commit\n");
            } else if (reply_result.txn_state() == proto::TxnState::Abort) {
                fprintf(stderr, "opps, abort\n");
            }
        }
        // TODO(singheart): Delete this
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
}

// 接受来自TaaS发来的日志，将日志进行重发
void ApplyLogWorkerThreadMain() {
    const std::string log_listen_port = "5556";
    const std::string zmq_log_listen_addr = "tcp://" + taas_ipv4_addr + ":" + log_listen_port;
    zmq::context_t context(1);
    zmq::socket_t listen_socket(context, ZMQ_SUB);
    // 记得初始化内存
    std::unique_ptr<zmq::message_t> zmq_message = std::make_unique<zmq::message_t>();
    std::unique_ptr<proto::Message> log_message = std::make_unique<proto::Message>();
    int zmq_queue_len = 0;
    bool proto_deserialize_result = false;
    listen_socket.setsockopt(ZMQ_SUBSCRIBE, "", 0);
    listen_socket.setsockopt(ZMQ_RCVHWM, &zmq_queue_len, sizeof(zmq_queue_len));
    listen_socket.connect(zmq_log_listen_addr);
    fprintf(stderr, "connect to storage log service, address is %s\n", zmq_log_listen_addr.c_str());
    while (system_run_enable_.load()) {
        // ZeroMQ从5556端口接收数据(Apply Log)
        listen_socket.recv(zmq_message.get());
        // fprintf(stderr, "received storage log from taas, data len is %lu\n", zmq_message->size());

        // 使用protobuf反序列化
        google::protobuf::io::ArrayInputStream input_stream(zmq_message->data(), zmq_message->size());
        proto_deserialize_result = log_message->ParseFromZeroCopyStream(&input_stream);
        if (!proto_deserialize_result) {
            NeuPrintLog("failed to deserialize log message from taas\n");
            system_run_enable_.store(false);
        }

        // 将日志重放
        ApplyWriteSet(std::move(log_message));

        // TODO(singheart): Delete this
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// 打印日志
void NeuPrintLog(const char* format, ...) {
#ifdef ENABLE_NEU_LOG
    va_list args;
    va_start(args, format);
    ereport(LOG, (errmsg(format, args)));
    va_end(args);
#endif
}

// 使用SnowFlake算法分配分布式全局位移的UID
uint64_t AllocateUniqueKey() {
    return uid_generator_.NextID();
}

// 获取当前机器的IPV4地址，只获取ens8f0网卡的地址
std::string GetIPV4Address() {
    struct ifaddrs *ifaddr, *ifa;
    static std::string ip(INET_ADDRSTRLEN, '\0');
    if (ip[0] != '\0') return ip;
    if (getifaddrs(&ifaddr) == -1) return "";
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            void* addr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
            inet_ntop(AF_INET, addr, const_cast<char*>(ip.c_str()), INET_ADDRSTRLEN);
            if (strcmp(ifa->ifa_name, "ens8f0") == 0) break;
        }
    }
    freeifaddrs(ifaddr);
    // 去除'\0'
    while (!ip.empty() && ip.back() == '\0') {
        ip.pop_back();
    }
    return ip;
}