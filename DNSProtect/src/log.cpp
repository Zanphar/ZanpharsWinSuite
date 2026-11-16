// ============================================================================
//  log.cpp
// ============================================================================

#include "log.h"
#include "common.h"

namespace dzp::log {

namespace {

CRITICAL_SECTION g_lock;
bool             g_lockInit = false;
std::wstring     g_file;

void WriteLevel(const wchar_t* level, const std::wstring& message) {
    if (!g_lockInit) return;
    EnterCriticalSection(&g_lock);
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (FILE* f = _wfopen(g_file.c_str(), L"a, ccs=UTF-8")) {
        fwprintf(f, L"[%04u-%02u-%02u %02u:%02u:%02u] [%-5ls] %ls\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 level, message.c_str());
        fclose(f);
    }
    LeaveCriticalSection(&g_lock);
}

}  // namespace

void Init(const std::filesystem::path& file) {
    if (!g_lockInit) {
        InitializeCriticalSection(&g_lock);
        g_lockInit = true;
    }
    std::error_code ec;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    g_file = file.wstring();
}

void Info(const std::wstring& message) { WriteLevel(L"info", message); }
void Warn(const std::wstring& message) { WriteLevel(L"warn", message); }

}  // namespace dzp::log
