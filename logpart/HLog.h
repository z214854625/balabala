#pragma once

#include "spdlog/spdlog.h"
#include "spdlog/async.h" //support for async logging
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/daily_file_sink.h"
#include "spdlog/sinks/udp_sink.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

//日志名称
#define LOG_NAME "multi_sink"
// 封装宏，没有该宏无法输出文件名、行号等信息

#define LOG_TRACE(...)                                                         \
  SPDLOG_LOGGER_CALL(spdlog::get(LOG_NAME), spdlog::level::trace, __VA_ARGS__)
#define LOG_DEBUG(...)                                                         \
  SPDLOG_LOGGER_CALL(spdlog::get(LOG_NAME), spdlog::level::debug, __VA_ARGS__)
#define LOG_INFO(...)                                                          \
  SPDLOG_LOGGER_CALL(spdlog::get(LOG_NAME), spdlog::level::info, __VA_ARGS__)
#define LOG_WARN(...)                                                          \
  SPDLOG_LOGGER_CALL(spdlog::get(LOG_NAME), spdlog::level::warn, __VA_ARGS__)
#define LOG_ERROR(...)                                                         \
  SPDLOG_LOGGER_CALL(spdlog::get(LOG_NAME), spdlog::level::err, __VA_ARGS__)
#define LOG_CRITI(...)                                                         \
  SPDLOG_LOGGER_CALL(spdlog::get(LOG_NAME), spdlog::level::critical,           \
                     __VA_ARGS__)

class HLog {
public:
  //日志输出位置
  enum OutPosition {
    CONSOLE = 0x01, //控制台
    FILE = 0X02,    //文件
    UDP = 0X04,     //文件
    ALL = 0x07,     //控制台+文件
  };

  enum OutMode {
    SYNC,  //同步模式
    ASYNC, //异步模式
  };

  //日志输出等级
  enum OutLevel {
    LEVEL_TRACE = 0,
    LEVEL_DEBUG = 1,
    LEVEL_INFO = 2,
    LEVEL_WARN = 3,
    LEVEL_ERROR = 4,
    LEVEL_CRITI = 5,
    LEVEL_OFF = 6,
  };

public:
  HLog();
  ~HLog();

  /* func: 初始化日志通道
   * @para[in] nFileName    : 日志存储路径
   * （支持相对路径和绝对路径）
   * @para[in] nMaxFileSize : 日志文件最大存储大小	（默认1024*1024*10）
   * @para[in] nMaxFile     : 最多存储多少个日志文件
   * （默认10，超过最大值则循环覆盖）
   * @para[in] outMode      : 日志输出模式			（同步、异步）
   * @para[in] outPos       : 日志输出位置
   * （控制台、文件、控制台+文件）
   * @para[in] outLevel     : 日志输出等级
   * （只输出>=等级的日志消息）
   */
  bool Init(const char *nFileName, const int nMaxFileSize = 1024 * 1024 * 10,
            const int nMaxFile = 10, const OutMode outMode = SYNC,
            const OutPosition outPos = ALL,
            const OutLevel outLevel = LEVEL_TRACE);
  void UnInit();

private:
public:
  std::shared_ptr<spdlog::logger> m_pLogger;
  bool m_bInit;
};