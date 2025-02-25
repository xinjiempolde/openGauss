#ifndef NEU_UTILS_H
#define NEU_UTILS_H

#include <zmq.hpp>
#include "blocking_concurrent_queue.hpp"
#include "concurrent_queue.hpp"

#define ENABLE_NEU_LOG 1
extern moodycamel::BlockingConcurrentQueue<std::unique_ptr<zmq::message_t>> transaction_message_queue_;
extern std::string taas_ipv4_addr;

void NeuPrintLog(const char *format, ...);
void SendWorkerThreadMain();
void ResponseWorkerThreadMain();
void ApplyLogWorkerThreadMain();
#endif //NEU_UTILS_H
