#include "Logging.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <strings.h> // strcasecmp
#include <unistd.h>

#include "Sha.hpp"

namespace
{

struct LogConfig {
    int         level     { LOGLEVEL_INFO };
    bool        color     { false };
    std::FILE*  file_sink { nullptr };
    std::mutex  mu;

    LogConfig() {
        if (const char* lv = std::getenv("WP_LOG_LEVEL")) {
            if      (::strcasecmp(lv, "trace") == 0) level = LOGLEVEL_TRACE;
            else if (::strcasecmp(lv, "debug") == 0) level = LOGLEVEL_DEBUG;
            else if (::strcasecmp(lv, "info")  == 0) level = LOGLEVEL_INFO;
            else if (::strcasecmp(lv, "warn")  == 0) level = LOGLEVEL_WARN;
            else if (::strcasecmp(lv, "error") == 0) level = LOGLEVEL_ERROR;
        }
        if (const char* col = std::getenv("WP_LOG_COLOR")) {
            color = !(std::strcmp(col, "0") == 0 ||
                      ::strcasecmp(col, "off") == 0 ||
                      ::strcasecmp(col, "false") == 0);
        } else {
            color = ::isatty(::fileno(stderr)) != 0;
        }
        if (const char* path = std::getenv("WP_LOG_FILE"); path && path[0] != '\0') {
            file_sink = std::fopen(path, "a");
            if (file_sink) std::setvbuf(file_sink, nullptr, _IOLBF, 0);
        }
    }

    ~LogConfig() {
        if (file_sink) std::fclose(file_sink);
    }
};

LogConfig& cfg() {
    static LogConfig c;
    return c;
}

const char* level_name(int level) {
    switch (level) {
    case LOGLEVEL_TRACE: return "TRACE";
    case LOGLEVEL_DEBUG: return "DEBUG";
    case LOGLEVEL_INFO:  return "INFO ";
    case LOGLEVEL_WARN:  return "WARN ";
    case LOGLEVEL_ERROR: return "ERROR";
    default:             return "?????";
    }
}

const char* level_color(int level) {
    switch (level) {
    case LOGLEVEL_TRACE: return "\033[90m"; // dim grey
    case LOGLEVEL_DEBUG: return "\033[36m"; // cyan
    case LOGLEVEL_INFO:  return "\033[32m"; // green
    case LOGLEVEL_WARN:  return "\033[33m"; // yellow
    case LOGLEVEL_ERROR: return "\033[31m"; // red
    default:             return "";
    }
}

void format_timestamp(char* buf, size_t bufsz) {
    using namespace std::chrono;
    auto now    = system_clock::now();
    auto secs   = system_clock::to_time_t(now);
    auto millis = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm;
    ::localtime_r(&secs, &tm);
    std::snprintf(buf,
                  bufsz,
                  "%02d:%02d:%02d.%03d",
                  tm.tm_hour,
                  tm.tm_min,
                  tm.tm_sec,
                  static_cast<int>(millis.count()));
}

} // namespace

void WallpaperLog(int level, const char* file, int line, const char* fmt, ...) {
    auto& c = cfg();
    if (level < c.level) return;

    char ts[16];
    format_timestamp(ts, sizeof(ts));

    char body[2048];
    {
        std::va_list args;
        va_start(args, fmt);
        std::vsnprintf(body, sizeof(body), fmt, args);
        va_end(args);
    }

    const bool       has_loc = file && file[0] != '\0';
    std::lock_guard  lock(c.mu);
    const char*      col_on  = c.color ? level_color(level) : "";
    const char*      col_off = c.color ? "\033[0m" : "";

    if (has_loc) {
        std::fprintf(stderr,
                     "%s%s%s %s [%s:%d] %s\n",
                     col_on,
                     level_name(level),
                     col_off,
                     ts,
                     file,
                     line,
                     body);
    } else {
        std::fprintf(stderr, "%s%s%s %s %s\n", col_on, level_name(level), col_off, ts, body);
    }
    std::fflush(stderr);

    if (c.file_sink) {
        if (has_loc) {
            std::fprintf(
                c.file_sink, "%s %s [%s:%d] %s\n", level_name(level), ts, file, line, body);
        } else {
            std::fprintf(c.file_sink, "%s %s %s\n", level_name(level), ts, body);
        }
    }
}

std::string logToTmpfileWithSha1(std::span<const char> in, const char* fmt, ...) {
    std::va_list          args;
    std::string           name   = utils::genSha1(in);
    std::filesystem::path fspath = std::filesystem::temp_directory_path() / name;
    std::string           path   = fspath.native();
    auto*                 file   = std::fopen(path.c_str(), "w+");
    if (! file) return path;
    {
        va_start(args, fmt);
        std::vfprintf(file, fmt, args);
        va_end(args);
    }
    std::fprintf(file, "\n");
    std::fclose(file);
    return path;
}
