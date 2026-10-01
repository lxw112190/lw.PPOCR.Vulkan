#include "service.hpp"
#include <csignal>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif
namespace lwvk::http {
#ifdef _WIN32
static SERVICE_STATUS_HANDLE status_handle = nullptr;
static SERVICE_STATUS status{};
static fs::path config_file;
static int exit_code = 0;
static void report(DWORD state, DWORD error = 0) {
    static std::mutex reporting;
    std::lock_guard<std::mutex> lock(reporting);
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = error;
    status.dwServiceSpecificExitCode = error == ERROR_SERVICE_SPECIFIC_ERROR ? 1 : 0;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    status.dwWaitHint = state == SERVICE_START_PENDING ? 120000 : state == SERVICE_STOP_PENDING ? 120000 : 0;
    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING)
        ++status.dwCheckPoint;
    else
        status.dwCheckPoint = 0;
    SetServiceStatus(status_handle, &status);
}
static void WINAPI control(DWORD command) {
    if (command == SERVICE_CONTROL_STOP || command == SERVICE_CONTROL_SHUTDOWN) {
        report(SERVICE_STOP_PENDING);
        stop();
    }
}
static void WINAPI service_main(DWORD, wchar_t**) {
    status_handle = RegisterServiceCtrlHandlerW(L"lw.PPOCR.Vulkan", control);
    if (!status_handle)
        return;
    report(SERVICE_START_PENDING);
    // SCM running means service host is alive; /health verifies model/HTTP readiness.
    report(SERVICE_RUNNING);
    try {
        exit_code = run(config_file);
    } catch (...) {
        exit_code = 1;
    }
    report(SERVICE_STOPPED, exit_code ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR);
}
static BOOL WINAPI console_control(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
        event == CTRL_SHUTDOWN_EVENT) {
        stop();
        return TRUE;
    }
    return FALSE;
}
#endif
int host_main(int argc, char** argv) {
    fs::path file = executable_path().parent_path() / "http-service.json";
    bool service = false, check_config = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc)
            file = fs::u8path(argv[++i]);
        else if (arg == "--check-config")
            check_config = true;
        else if (arg == "--service")
            service = true;
        else if (arg == "--help") {
            std::cout << "lw-ppocr-vulkan-http-service [--config file] [--check-config] [--service (Windows)]\n";
            return 0;
        } else
            throw std::runtime_error("unknown/missing argument: " + arg);
    }
    if (check_config) {
        load_config(file);
        std::cout << "Configuration valid (GPU not initialized)\n";
        return 0;
    }
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    if (service) {
        config_file = fs::absolute(file);
        SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t*>(L"lw.PPOCR.Vulkan"), service_main}, {nullptr, nullptr}};
        if (!StartServiceCtrlDispatcherW(table))
            throw std::runtime_error("SCM dispatcher failed; use install-service.bat");
        return exit_code;
    }
    SetConsoleCtrlHandler(console_control, TRUE);
#else
    if (service)
        throw std::runtime_error("--service is Windows-only; use systemd scripts");
#endif
    std::signal(SIGINT, [](int) { stop(); });
    std::signal(SIGTERM, [](int) { stop(); });
    return run(file);
}
} // namespace lwvk::http
static int entry(int argc, char** argv) {
    try {
        return lwvk::http::host_main(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "lw.PPOCR.Vulkan HTTP service: " << e.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> values;
    std::vector<char*> pointers;
    values.reserve(argc);
    pointers.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        int size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string value(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, value.data(), size, nullptr, nullptr);
        values.push_back(std::move(value));
    }
    for (auto& value : values)
        pointers.push_back(value.data());
    return entry(argc, pointers.data());
}
#else
int main(int argc, char** argv) {
    return entry(argc, argv);
}
#endif
