//
// Created by singheart on 2/27/25.
//

#ifndef SNOWFLAKE_UID_H
#define SNOWFLAKE_UID_H

#include <sys/time.h>
typedef unsigned long int uint64_t;
/**
* Twitter snowflake algorithm implementation:
* reference: https://pdai.tech/md/algorithm/alg-domain-id-snowflake.html
* 0 - 0000000000 0000000000 0000000000 0000000000 0 - 00000 - 00000 - 000000000000
* 1位标识，最高位是符号位，正数是0，负数是1，所以id一般是正数，最高位是0
* 41位时间截(毫秒级)，注意，41位时间截不是存储当前时间的时间截，而是存储时间截的差值（当前时间截 - 开始时间截)
* 得到的值），这里的的开始时间截，一般是我们的id生成器开始使用的时间，由我们程序来指定的（如下下面程序IdWorker类的startTime属性）。41位的时间截，可以使用69年，年T = (1L << 41) / (1000L * 60 * 60 * 24 * 365) = 69
* 10位的数据机器位，可以部署在1024个节点，包括5位datacenterId和5位workerId
* 12位序列，毫秒内的计数，12位的计数顺序号支持每个节点每毫秒(同一机器，同一时间截)产生4096个ID序号
* 加起来刚好64位，为一个Long型。
* SnowFlake的优点是，整体上按照时间自增排序，并且整个分布式系统内不会产生ID碰撞(由数据中心ID和机器ID作区分)，并且效率较高，经测试，SnowFlake每秒能够产生26万ID左右。
*/
class SnowflakeDistributeID {
 public:
  SnowflakeDistributeID(long workerId, long datacenterId) {
    if (workerId > maxWorkerId_ || workerId < 0) {
      return;
    }
    if (datacenterId > maxDatacenterId_ || datacenterId < 0) {
      return;
    }
    this->workerId_ = workerId;
    this->datacenterId_ = datacenterId;
  }

  uint64_t NextID() {
    long timestamp = TimeGen();
    // 如果当前时间小于上一次ID生成的时间戳，说明系统时钟回退过，这个时候应当抛出异常
    if (timestamp < lastTimestamp_) {
      return -1;
    }
    // 如果是同一时间生成的，则进行毫秒内序列
    if (timestamp == lastTimestamp_) {
      sequence_ = (sequence_ + 1) & sequenceMask;
      // 毫秒内序列溢出
      if (sequence_ == 0) {
        // 阻塞到下一个毫秒，获得新的时间戳
        timestamp = TilNextMillis(lastTimestamp_);
      }
    } else {
      // 时间戳改变，毫秒内序列重置
      sequence_ = 0L;
    }
    lastTimestamp_ = timestamp;
    return ((timestamp - startTimeStamp_) << timestampLeftShift_) |
           (datacenterId_ << datacenterIdShift_) |
           (workerId_ << workerIdBits_) | sequence_;
  }

 private:
  long TimeGen() {
    gettimeofday(&time_, nullptr);
    return time_.tv_sec * 1000 + time_.tv_usec / 1000;
  }

  long TilNextMillis(long lastTimestamp) {
    long timestamp = TimeGen();
    while (timestamp <= lastTimestamp) {
      timestamp = TimeGen();
    }
    return timestamp;
  }

 private:
  /* 用于获取当前的时间戳结构 */
  struct timeval time_ {};
  /* 从2015-01-01开始计算时间戳的差值 */
  const long startTimeStamp_ = 1420041600000L;
  /* 机器ID */
  long workerId_;
  /* 数据中心ID */
  long datacenterId_;
  /* 机器ID占用的位数 */
  const long workerIdBits_ = 5L;
  /* 数据中心ID占用的位数 */
  const long datacenterIdBits_ = 5L;
  /* 序列号占用的位数 */
  const long sequenceBits_ = 12L;
  /* 机器ID的最大值 */
  const long maxWorkerId_ = -1L ^ (-1L << workerIdBits_);
  /* 数据中心ID的最大值 */
  const long maxDatacenterId_ = -1L ^ (-1L << datacenterIdBits_);
  /* 时间戳的左移位数 */
  const long timestampLeftShift_ =
      sequenceBits_ + workerIdBits_ + datacenterIdBits_;
  /* 数据中心ID的左移位数 */
  const long datacenterIdShift_ = sequenceBits_ + workerIdBits_;
  /* 上次生成ID的时间截 */
  long lastTimestamp_ = -1;
  /* 毫秒内序列 */
  long sequence_ = 0L;
  /* 毫秒内序列的掩码 */
  long sequenceMask = -1L ^ (-1L << 12);
};
#endif //SNOWFLAKE_UID_H
