#include "HLog.h"
#include <fstream>
#include <iostream>
#include <string>
 
HLog::HLog()
	:m_bInit(false)
{
 
}
 
HLog::~HLog()
{
	if (m_bInit)
	{
		this->UnInit();
	}
} 

bool HLog::Init(const char* nFileName, const int nMaxFileSize, const int nMaxFile,
				const OutMode outMode, const OutPosition outPos, const OutLevel outLevel)
{
	if (m_bInit)
	{
		//DBINFO("It's already initialized\n");
		return false;
	}
	m_bInit = true;
 
	try 
	{
		//[23/05/30 17:51:45.973] <p:1330 t:1330> [debug][Working:43] 
		const char* pFormat = "[%C/%m/%d %H:%M:%S.%e] <p:%P t:%t> [%^%l%$][%!:%#] %v";
		//sink容器
		std::vector<spdlog::sink_ptr> vecSink;
 
		//控制台
		if (outPos & CONSOLE)
		{
			auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
			console_sink->set_level(spdlog::level::trace);
			console_sink->set_pattern(pFormat);
			vecSink.push_back(console_sink);
		}
 
		//文件
		if (outPos & FILE)
		{
			auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(nFileName, nMaxFileSize, nMaxFile,false);
			file_sink->set_level(spdlog::level::trace);
			file_sink->set_pattern(pFormat);
			vecSink.push_back(file_sink);
		}

		//
		if (outPos & UDP)
		{
			spdlog::sinks::udp_sink_config cfg("127.0.0.1", 11091);
			auto udp_sink = std::make_shared<spdlog::sinks::udp_sink_mt>(cfg);
			udp_sink->set_level(spdlog::level::trace);
			udp_sink->set_pattern(pFormat);
			vecSink.push_back(udp_sink);
		}		
 
		//设置logger使用多个sink
		if (outMode == ASYNC)//异步
		{
			// spdlog::init_thread_pool(102400, 1);
			// auto tp = spdlog::thread_pool();
			auto tp = std::make_shared<spdlog::details::thread_pool>(102400, 1);			
			m_pLogger = std::make_shared<spdlog::async_logger>(LOG_NAME, begin(vecSink), end(vecSink), tp, spdlog::async_overflow_policy::block);
		}
		else//同步
		{
			m_pLogger = std::make_shared<spdlog::logger>(LOG_NAME, begin(vecSink), end(vecSink));
		}
		m_pLogger->set_level((spdlog::level::level_enum)outLevel);
 		
		//定时flush到文件，每5秒刷新一次
		spdlog::flush_every(std::chrono::seconds(1));
		//遇到warn级别，立即flush到文件
		spdlog::flush_on(spdlog::level::trace);
		spdlog::register_logger(m_pLogger);
	}

	catch(const spdlog::spdlog_ex& ex)
	{
		std::cout << "Log initialization failed: " << ex.what() << std::endl;
		return false;
	}
	return true;
}
 
void HLog::UnInit()
{
	spdlog::drop_all();
	spdlog::shutdown();
}