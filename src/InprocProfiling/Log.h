#pragma once

#include "pch.h"
#include "Configuration.h"
#include "EnvironmentVariables.h"
#include "OpSysTools.h"

#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include "spdlog/sinks/stdout_color_sinks.h"
#include <spdlog/spdlog.h>

#include <regex>

class Log;

inline spdlog::level::level_enum GetLogLevel()
{
    // Default log level is info
    auto logLevel = spdlog::level::level_enum::info;
    std::string levelStr = Configuration::GetEnvironmentValue(EnvironmentVariables::LogLevel, std::string("info"));

    if (levelStr == "debug")
    {
        logLevel = spdlog::level::debug;
    }
    else if (levelStr == "info")
    {
        logLevel = spdlog::level::info;
    }
    else if (levelStr == "warn")
    {
        logLevel = spdlog::level::warn;
    }
    else if (levelStr == "error")
    {
        logLevel = spdlog::level::err;
    }

    return logLevel;
}

inline std::string SanitizeProcessName(std::string const& processName)
{
    const auto process_name_without_extension = processName.substr(0, processName.find_last_of("."));
    const std::regex dash_or_space_or_tab("-|\\s|\\t");
    return std::regex_replace(process_name_without_extension, dash_or_space_or_tab, "_");
}

inline std::string GetLogPathname(const std::string& loggerName)
{
    // the file name has the following format: "<logger name>-<process name>-<pid>.log"
    std::ostringstream oss;
    auto processName = SanitizeProcessName(OpSysTools::GetProcessName());

    oss << loggerName << "-" << processName << "-" << ::GetCurrentProcessId() << ".log";
    auto logFilename = oss.str();

    std::string logPathname;

    // look for env vars to configure the log file path
    auto logDirectory = Configuration::GetEnvironmentValue(EnvironmentVariables::LogDirectory, std::string(""));
    if (!logDirectory.empty())
    {
        logPathname = logDirectory + '\\' + logFilename;
    }
    else
    {
        // compute default log file path in Windows ProgramData folder
        // i.e. C:\ProgramData\Datadog Tracer\logs\<logger name>-<process name>-<pid>.log
        auto programDataPath = Configuration::GetEnvironmentValue("PROGRAMDATA", std::string("c:\\ProgramData"));
        logPathname = programDataPath + "\\Datadog Tracer\\logs\\" + logFilename;
    }

    // create the folders if needed
    const auto log_path = fs::path(logPathname);
    if (log_path.has_parent_path())
    {
        const auto parent_path = log_path.parent_path();

        if (!fs::exists(parent_path))
        {
            fs::create_directories(parent_path);
        }
    }

    return logPathname;
}

inline std::shared_ptr<Log> CreateLoggerInstance()
{
    // look for env vars to configure the logger:
    //  - EnvironmentVariables::LogDirectory: Tracer folder for the log file (if not set, use default folder in ProgramData)
    //  - EnvironmentVariables::LogLevel: set the log level (default is "info")
    //      #define SPDLOG_LEVEL_DEBUG 1
    //      #define SPDLOG_LEVEL_INFO 2
    //      #define SPDLOG_LEVEL_WARN 3
    //      #define SPDLOG_LEVEL_ERROR 4

    static const std::string loggerName = "DD-InprocProfiler";  // also used as log file name prefix

    // the logger will add a prefix to each log line
    static const std::string pattern = "[%Y-%m-%d %H:%M:%S.%e | %l | PId: %P | TId: %t] %v";
    // ex: [2025-06-13 11:49:13.616 | info | PId: 11916 | TId: 39072]

    auto logPathname = GetLogPathname(loggerName);
    auto logLevel = GetLogLevel();
    bool sendToConsole = Configuration::GetEnvironmentValue(EnvironmentVariables::LogToConsole, false);
    auto log = std::make_shared<Log>(loggerName, logPathname, pattern, logLevel, sendToConsole);
    return log;
}

template <class Period>
const char* time_unit_str()
{
    if constexpr (std::is_same_v<Period, std::nano>)
    {
        return "ns";
    }
    else if constexpr (std::is_same_v<Period, std::micro>)
    {
        return "us";
    }
    else if constexpr (std::is_same_v<Period, std::milli>)
    {
        return "ms";
    }
    else if constexpr (std::is_same_v<Period, std::ratio<1>>)
    {
        return "s";
    }

    return "<unknown unit of time>";
}

template <class Rep, class Period>
void WriteToStream(std::ostringstream& oss, std::chrono::duration<Rep, Period> const& x)
{
    oss << x.count() << time_unit_str<Period>();
}

template <class T>
void WriteToStream(std::ostringstream& oss, T const& x)
{
    oss << x;
}

template <typename... Args>
static std::string LogToString(Args const&... args)
{
    std::ostringstream oss;
    (WriteToStream(oss, args), ...);

    return oss.str();
}


class Log final
{
public:
    Log(const std::string& loggerName, std::string& logPathname, const std::string& pattern, spdlog::level::level_enum logLevel, bool sendToConsole)
    {

        spdlog::flush_every(std::chrono::seconds(3));
        std::shared_ptr<spdlog::logger> logger;
        try
        {
            logger =
                spdlog::rotating_logger_mt(loggerName, logPathname, 1048576 * 5, 10);
        }
        catch (...)
        {
            // By writing into the stderr was changing the behavior in a CI scenario.
            // There's not a good way to report errors when trying to create the log file.
            // But we never should be changing the normal behavior of an app.
            // std::cerr << "LoggerImpl Handler: Error creating native log file." << std::endl;
            logger = spdlog::null_logger_mt("LoggerImpl");
        }

        logger->set_level(logLevel);
        logger->set_pattern(pattern);
        logger->flush_on(spdlog::level::info);  // TODO: make this configurable if needed
        _internalLogger = std::move(logger);

        // TODO: we could customize to send warnigs/errors to stderr and info/debug to the console
        //    auto err_logger = spdlog::stderr_color_mt("stderr");
        _sendToConsole = sendToConsole;
        if (sendToConsole)
        {
            try
            {
                auto consoleLogger = spdlog::stdout_color_mt("console");
                _consoleLogger = std::move(consoleLogger);
                _consoleLogger->set_level(logLevel);
                _consoleLogger->set_pattern(pattern);
                _consoleLogger->flush_on(spdlog::level::info);  // TODO: make this configurable if needed

            }
            catch (...)
            {
                // If we can't create the console logger, we just ignore it.
            }
        }
    }

public:
    template <typename... Args>
    static inline void Debug(const Args&... args)
    {
        Instance->DebugImpl<Args...>(args...);
    }

    template <typename... Args>
    static void Info(const Args&... args)
    {
        Instance->InfoImpl<Args...>(args...);
    }

    template <typename... Args>
    static void Warn(const Args&... args)
    {
        Instance->WarnImpl<Args...>(args...);
    }

    template <typename... Args>
    static void Error(const Args&... args)
    {
        Instance->ErrorImpl<Args...>(args...);
    }

private:
    inline static std::shared_ptr<Log> const Instance = CreateLoggerInstance();

private:
    std::shared_ptr<spdlog::logger> _internalLogger;
    bool _sendToConsole = false;
    std::shared_ptr<spdlog::logger> _consoleLogger;

private:
    template <typename... Args>
    void DebugImpl(const Args&... args)
    {
        _internalLogger->debug(LogToString(args...));
        if (_sendToConsole && _consoleLogger)
        {
            _consoleLogger->debug(LogToString(args...));
        }
    }

    template <typename... Args>
    void InfoImpl(const Args&... args)
    {
        _internalLogger->info(LogToString(args...));
        if (_sendToConsole && _consoleLogger)
        {
            _consoleLogger->info(LogToString(args...));
        }
    }

    template <typename... Args>
    void WarnImpl(const Args&... args)
    {
        _internalLogger->warn(LogToString(args...));
        if (_sendToConsole && _consoleLogger)
        {
            _consoleLogger->warn(LogToString(args...));
        }
    }

    template <typename... Args>
    void ErrorImpl(const Args&... args)
    {
        _internalLogger->error(LogToString(args...));
        if (_sendToConsole && _consoleLogger)
        {
            _consoleLogger->error(LogToString(args...));
        }
    }
};

#define LogOnce(level, ...)                                                                                                \
    do                                                                                                                     \
    {                                                                                                                      \
        static std::once_flag UNIQUE_ONCE_FLAG_##__COUNTER__;                                                              \
        std::call_once(                                                                                                    \
            UNIQUE_ONCE_FLAG_##__COUNTER__, [](auto&&... args) { Log::level(std::forward<decltype(args)>(args)...); }, __VA_ARGS__); \
    } while (0) // NOLINT