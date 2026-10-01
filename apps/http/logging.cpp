#include "service.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <ctime>
#include <iomanip>
#include <sstream>
namespace lwvk::http {
std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::ostringstream s;
    s << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms << 'Z';
    return s.str();
}
struct Logs::Impl {
    std::shared_ptr<spdlog::logger> runtime, access;
};
Logs::Logs(const Config& c) : impl(new Impl) {
    if (!c.logging)
        return;
    fs::create_directories(c.logs);
    auto make = [&](const char* name) {
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            (c.logs / (std::string(name) + ".log")).native(), c.log_bytes, c.log_files);
        return std::make_shared<spdlog::logger>(name, sink);
    };
    impl->runtime = make("runtime");
    impl->runtime->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
    if (c.access) {
        impl->access = make("access");
        impl->access->set_pattern("%v");
    }
}
Logs::~Logs() = default;
void Logs::runtime(const std::string& value, bool error) {
    if (impl->runtime) {
        if (error)
            impl->runtime->error("{}", value);
        else
            impl->runtime->info("{}", value);
        impl->runtime->flush();
    }
}
void Logs::request(const json& value) {
    if (impl->access) {
        impl->access->info("{}", value.dump());
        impl->access->flush();
    }
}
} // namespace lwvk::http
