// 大部分的对openGauss的更改放到这个文件中
#include <cstdarg>
#include <cstdio>

#include "access/neu_utils/message.pb.h"
#include "access/neu_utils/neu_utils.h"
#include "utils/elog.h"

//=== NEU全局变量定义开始 ===//
moodycamel::BlockingConcurrentQueue<std::unique_ptr<zmq::message_t>>
    transaction_message_queue_;
std::string taas_ipv4_addr = "219.216.64.135";
std::atomic<bool> system_run_enable_{true};
//=== NEU全局变量定义结束 ===//

// 将事务(或者说读写集)发送给TaaS
void SendWorkerThreadMain() {
  std::string zmq_remote_port = "5551";
  std::string zmq_remote_addr =
      "tcp://" + taas_ipv4_addr + ":" + zmq_remote_port;
  zmq::context_t context(1);
  zmq::socket_t send_socket(context, ZMQ_PUSH);
  zmq::send_flags zmq_flags(zmq::send_flags::none);
  std::unique_ptr<zmq::message_t> zmq_message;
  send_socket.connect(zmq_remote_addr);
  NeuPrintLog("connect to remote TaaS, address: %s\n", zmq_remote_addr.c_str());

  // 从并发队列中取出Transaction发送给TaaS
  while (system_run_enable_.load()) {
    transaction_message_queue_.wait_dequeue(zmq_message);
    //     fprintf(stderr, "zmq message size is %lu\n", zmq_message->size());
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
  std::unique_ptr<zmq::message_t> zmq_message =
      std::make_unique<zmq::message_t>();
  listen_socket.setsockopt(ZMQ_RCVHWM, &zmq_queue_len, sizeof(zmq_queue_len));
  listen_socket.bind(zmq_bind_addr);
  fprintf(stderr, "bind address %s\n", zmq_bind_addr.c_str());
  while (system_run_enable_.load()) {
    listen_socket.recv(zmq_message.get());
    // fprintf(stderr, "recv from TaaS in 5552, data len is %lu\n", zmq_message->size());
    // 将TaaS反馈来的结果反序列化
    google::protobuf::io::ArrayInputStream input_stream(zmq_message->data(),
                                                        zmq_message->size());
    protobuf_deserialize_result =
        taas_result_message.ParseFromZeroCopyStream(&input_stream);
    if (!protobuf_deserialize_result) {
      fprintf(stderr, "failed to deserialize result from taas\n");
      system_run_enable_.store(false);
    }
    if (taas_result_message.type_case() ==
        proto::Message::TypeCase::kReplyTxnResultToClient) {
      auto reply_result = taas_result_message.reply_txn_result_to_client();
      fprintf(stderr, "ReplyTxnResultToClient, csn %lu\n",
              reply_result.client_txn_id());
      if (reply_result.txn_state() == proto::TxnState::Commit) {
        // fprintf(stderr, "tell me commit\n");
      } else if (reply_result.txn_state() == proto::TxnState::Abort) {
        fprintf(stderr, "opps, abort\n");
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(20));
  }
}

// 接受来自TaaS发来的日志，将日志进行重发
void ApplyLogWorkerThreadMain() {
  const std::string log_listen_port = "5556";
  const std::string zmq_log_listen_addr =
      "tcp://" + taas_ipv4_addr + ":" + log_listen_port;
  zmq::context_t context(1);
  zmq::socket_t listen_socket(context, ZMQ_SUB);
  // 记得初始化内存
  std::unique_ptr<zmq::message_t> zmq_message =
      std::make_unique<zmq::message_t>();
  std::unique_ptr<proto::Message> log_message =
      std::make_unique<proto::Message>();
  int zmq_queue_len = 0;
  bool proto_deserialize_result = false;
  listen_socket.setsockopt(ZMQ_SUBSCRIBE, "", 0);
  listen_socket.setsockopt(ZMQ_RCVHWM, &zmq_queue_len, sizeof(zmq_queue_len));
  listen_socket.connect(zmq_log_listen_addr);
  fprintf(stderr, "connect to storage log service, address is %s\n",
          zmq_log_listen_addr.c_str());
  while (system_run_enable_.load()) {
    listen_socket.recv(zmq_message.get());
    // fprintf(stderr, "received storage log from taas, data len is %lu\n", zmq_message->size());
    // 使用protobuf反序列化
    google::protobuf::io::ArrayInputStream input_stream(zmq_message->data(),
                                                        zmq_message->size());
    proto_deserialize_result =
        log_message->ParseFromZeroCopyStream(&input_stream);
    if (!proto_deserialize_result) {
      fprintf(stderr, "failed to deserialize log message from taas\n");
      system_run_enable_.store(false);
    }
    if (log_message->type_case() ==
        proto::Message::TypeCase::kStoragePullResponse) {
      fprintf(stderr, "received storage pull response\n");
    } else if (log_message->type_case() ==
               proto::Message::TypeCase::kStoragePushResponse) {
      const proto::StoragePushResponse& push_response =
          log_message->storage_push_response();
      fprintf(stderr, "received storage push response, txn size is %d\n",
              push_response.txns_size());
      for (int i = 0; i < push_response.txns_size(); ++i) {
        const proto::Transaction& txn = push_response.txns(i);
        // fprintf(stderr, "txn csn is %lu", txn.csn());
      }
    } else {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

// 打印日志
void NeuPrintLog(const char* format, ...) {
#ifdef ENABLE_NEU_LOG
  va_list args;
  va_start(args, format);
  ereport(INFO, (errmsg(format, args)));
  va_end(args);
#endif
}