/*
spdlog
  1）丰富的日志输出格式，自定义
  2）多种输出文件日志类型，支持控制台
  3）支持单多线程、支持同步异步
  4）清晰的代码结构，便于读者阅读
  5）写日志非常的快，效果很好
*/

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/daily_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include "spdlog/sinks/callback_sink.h"
#include <iostream>

#include <iostream>
#include <vector>
//#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include "HLog.h"

using namespace std;

#include <unordered_map>

int main(void)
{
    HLog log;
    log.Init(".log/test.log", 1024*1024*1000, 10, HLog::SYNC, HLog::ALL, HLog::LEVEL_INFO);

    //输出不同级别的日志
    spdlog::info("Hello, {}!", "World");
    spdlog::info("Welcome to spdlog!");
    spdlog::error("Some error message with arg: {}", 1);
    
    spdlog::warn("Easy padding in numbers like {:08d}", 12);
    spdlog::critical("Support for int: {0:d};  hex: {0:x};  oct: {0:o}; bin: {0:b}", 42);
    spdlog::info("Support for floats {:03.2f}", 1.23456);
    spdlog::info("Positional args are {1} {0}..", "too", "supported");
    spdlog::info("This is an info log with multiple parameters: string={0}, integer={1}, float={2:03.2f}",   
                 "example", 123, 45.678);  
    spdlog::info("{:<30}", "left aligned");
    

    LOG_TRACE("test {}",1);
    LOG_DEBUG("test {:.2f}", 1.0000);
    LOG_INFO("test {}", 1.23456789);
    LOG_WARN("test {0} {1} {2}", 1, 2, 'A');
    LOG_WARN("test {} {} {}", 1, 2, 'A');
    LOG_ERROR("test {}", "ABC");
    LOG_CRITI("test {}", std::string("abc"));

    int i =0;
    while (i < 30)
    {
        LOG_WARN("Async message #{}", i);
        i++;
        sleep(1);
    }
}

// int main() {
//     auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
//     auto logger1 = std::make_shared<spdlog::logger>("logger1", sink);
//     auto logger2 = std::make_shared<spdlog::logger>("logger2", sink);
//     auto logger3 = std::make_shared<spdlog::logger>("logger3", sink);
 
//     logger1->info("hello world ");
//     logger2->info("hello world ");
//     logger3->info("hello world ");
//     return 0;
// }

// int main() {
//     auto logger = spdlog::callback_logger_mt("custom_callback_logger", [](const spdlog::details::log_msg & msg) {
//         std::cout << msg.payload.data() << std::endl;
//     });
 
//     logger->info("123");
//     return 0;
// }

/*日志等级
SPDLOG_ACTIVE_LEVEL 宏
如果SPDLOG_ACTIVE_LEVEL被定义，并且指向了一个特定的日志级别（如spdlog::level::info），那么编译器可能会优化掉所有低于该级别的日志记录代码。
这意味着，如果你将SPDLOG_ACTIVE_LEVEL设置为spdlog::level::info，那么所有trace和debug级别的日志记录调用都可能在预处理阶段被完全移除
spdlog::set_level(spdlog::level::debug); // Set global log level to debug
*/
//  int main()
//  {
//    spdlog::info("{:<30}", "left aligned");
//     spdlog::warn("Easy padding in numbers like {:08d}", 12);
//     spdlog::error("Some error message with arg: {}", 1);
//     spdlog::critical("Support for int: {0:d};  hex: {0:x};  oct: {0:o}; bin: {0:b}", 42);

//     spdlog::set_level(spdlog::level::debug); // Set global log level to debug
//     spdlog::debug("This message should be displayed..");

//     // change log pattern
//     spdlog::error("log level: {}", SPDLOG_ACTIVE_LEVEL);
//     //spdlog::set_pattern("[%H:%M:%S %z] [%n] [%^---%L---%$] [thread %t] %v");
//     spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e %z] [%^%l%$][thread %t](%@): %v");

//     // Compile time log levels
//     // define SPDLOG_ACTIVE_LEVEL to desired level,if not define, don't show
//     SPDLOG_TRACE("1111Some trace message with param {}", 42);
//     SPDLOG_DEBUG("2222Some debug message");
//     return 0;
//  }


// int main()
// {
//     // create color multi threaded logger
//     auto console = spdlog::stdout_color_mt("console");  
//     auto err_logger = spdlog::stderr_color_mt("stderr");    
//     spdlog::get("console")->info("loggers can be retrieved from a global registry using the spdlog::get(logger_name)");
//     return 0;
// }
// void basic_logfile_example()
// {
//     //简单日志文件示例
//     try
//     {
//         spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e %z] [%^%l%$][thread %t](%s): %v");
//         auto logger = spdlog::basic_logger_mt("basic_logger", "logs/basic-log.txt");
//         spdlog::get("basic_logger")->info("log file basic_logger created {}",1);
//     }
//     catch (const spdlog::spdlog_ex &ex)
//     {
//         std::cout << "Log init failed: " << ex.what() << std::endl;
//     }
// }

// void rotating_example()
// {
//     // Create a file rotating logger with 5mb size max and 3 rotated files
//     //auto max_size = 1024*1024 * 5;
//     auto max_size = 256;
//     auto max_files = 3;
//     auto logger = spdlog::rotating_logger_mt("some_logger_name", "logs/rotating.txt", max_size, max_files);
//     for (int i=0; i<10000; i++) {
//         logger->info("{} * {} equals {:>10}",i, i, i*i);
//     }
// }

// void daily_example()
// {
//     // Create a daily logger - a new file is created every day on 2:30am
//     auto logger = spdlog::daily_logger_mt("daily_logger", "logs/daily.txt", 2, 30);
// }

// int main() {
//         basic_logfile_example();
//         //rotating_example();
//         //daily_example();
//         return 0;
// }