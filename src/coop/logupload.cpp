// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "logupload.h"

#include "log.h"
#include "version.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace coop::logupload {

namespace {

constexpr wchar_t kLogName[] = L"PTDE-FPS-Unlock.log";
constexpr wchar_t kPrevName[] = L"PTDE-FPS-Unlock.log.prev";  // the FPS Unlock's log_init keeps the last run's
// The start of the last log sent at a close (its first line's time), so that each log is sent once.
constexpr wchar_t kSentName[] = L"PTDE-Coop-Log-Sent.txt";
// A last run's log not sent when it closed is moved out of log_init's way at the start, before anything is sent, as
// PTDE-Coop-Unsent-<stamp>.log, and deleted once it is sent: log_init replaces the .prev at every start, so a run that
// died while its start was still sending the one before (a crash loop at startup) lost that log. The newest few kept.
constexpr wchar_t kUnsentPrefix[] = L"PTDE-Coop-Unsent-";
constexpr size_t kUnsentKept = 5;
constexpr DWORD kMaxBytes = 31u << 20;  // the drop takes up to 32 MiB a file: of a longer log, its end
constexpr DWORD kTimeoutMs = 5000;
constexpr DWORD kExitWaitMs = 12000;    // the game's close waits this long for it at most

std::wstring g_dir;   // our DLL's folder, with its backslash
std::wstring g_to;    // UploadLogsTo
std::wstring g_name;  // the folder there: UploadLogsName, else the PC's name
volatile LONG g_exit_sent = 0;
using ExitProcessFn = VOID(WINAPI*)(UINT);
using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);
ExitProcessFn g_exit_process = nullptr;
TerminateProcessFn g_terminate_process = nullptr;

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    std::wstring out(s.size(), L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0],
                                      static_cast<int>(out.size()));
    out.resize(n > 0 ? n : 0);
    return out;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    std::string out(w.size() * 3, '\0');
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0],
                                      static_cast<int>(out.size()), nullptr, nullptr);
    out.resize(n > 0 ? n : 0);
    return out;
}

// A folder name the drop takes: letters, digits, - and _, 40 at most.
std::wstring folder_name(const std::wstring& in) {
    std::wstring out;
    for (wchar_t c : in) {
        if (out.size() >= 40) break;
        const bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' ||
                        c == L'_';
        out += ok ? c : L'_';
    }
    return out;
}

// Up to `max` bytes of the file: its start (`tail` false) or its end.
bool read_file(const std::wstring& path, std::vector<char>* out, DWORD max, bool tail) {
    out->clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) != 0;
    if (ok) {
        const LONGLONG want = size.QuadPart > static_cast<LONGLONG>(max) ? static_cast<LONGLONG>(max) : size.QuadPart;
        if (tail && size.QuadPart > want) {
            LARGE_INTEGER at{};
            at.QuadPart = size.QuadPart - want;
            SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
        }
        out->resize(static_cast<size_t>(want));
        DWORD got = 0;
        if (want > 0) ok = ReadFile(f, out->data(), static_cast<DWORD>(want), &got, nullptr) != 0;
        out->resize(ok ? got : 0);
    }
    CloseHandle(f);
    return ok;
}

// The time a log was started, from its first line ("[2026-11-05 18:33:12.123] ..."), as 2026-11-05_18-33-12; empty
// when the file is not there or not a log of ours.
std::wstring stamp_of(const std::wstring& path) {
    std::vector<char> head;
    if (!read_file(path, &head, 32, false) || head.size() < 21 || head[0] != '[') return std::wstring();
    const char* p = head.data() + 1;  // YYYY-MM-DD HH:MM:SS
    for (int i = 0; i < 19; ++i) {
        const char want = i == 4 || i == 7 ? '-' : i == 10 ? ' ' : i == 13 || i == 16 ? ':' : 0;
        if (want ? p[i] != want : (p[i] < '0' || p[i] > '9')) return std::wstring();
    }
    std::string s(p, 19);
    s[10] = '_';
    s[13] = '-';
    s[16] = '-';
    return widen(s);
}

std::wstring sent_stamp() {
    std::vector<char> v;
    if (!read_file(g_dir + kSentName, &v, 64, false)) return std::wstring();
    std::string s(v.begin(), v.end());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return widen(s);
}

void record_sent(const std::wstring& stamp) {
    HANDLE f = CreateFileW((g_dir + kSentName).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const std::string s = narrow(stamp) + "\n";
    DWORD wrote = 0;
    WriteFile(f, s.data(), static_cast<DWORD>(s.size()), &wrote, nullptr);
    CloseHandle(f);
}

std::string base64(const std::string& in) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = static_cast<uint8_t>(in[i]) << 16 | static_cast<uint8_t>(in[i + 1]) << 8 |
                           static_cast<uint8_t>(in[i + 2]);
        out += t[v >> 18 & 63];
        out += t[v >> 12 & 63];
        out += t[v >> 6 & 63];
        out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        const uint32_t v = static_cast<uint8_t>(in[i]) << 16;
        out += t[v >> 18 & 63];
        out += t[v >> 12 & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const uint32_t v = static_cast<uint8_t>(in[i]) << 16 | static_cast<uint8_t>(in[i + 1]) << 8;
        out += t[v >> 18 & 63];
        out += t[v >> 12 & 63];
        out += t[v >> 6 & 63];
        out += '=';
    }
    return out;
}

struct Target {
    std::wstring host, user, password, path;
    INTERNET_PORT port = 0;
    bool https = false;
};

bool crack(const std::wstring& url, Target* t) {
    wchar_t host[256] = {}, user[128] = {}, pass[128] = {}, path[1024] = {};
    URL_COMPONENTS u{};
    u.dwStructSize = sizeof(u);
    u.lpszHostName = host;
    u.dwHostNameLength = 256;
    u.lpszUserName = user;
    u.dwUserNameLength = 128;
    u.lpszPassword = pass;
    u.dwPasswordLength = 128;
    u.lpszUrlPath = path;
    u.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, ICU_DECODE, &u) || !host[0]) return false;
    if (u.nScheme != INTERNET_SCHEME_HTTPS && u.nScheme != INTERNET_SCHEME_HTTP) return false;
    t->host = host;
    t->user = user;
    t->password = pass;
    t->path = path;
    if (t->path.empty() || t->path.back() != L'/') t->path += L'/';
    t->port = u.nPort;
    t->https = u.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

// The log at `path` to <UploadLogsTo>/<name>/<stamp>.log. The HTTP status (201: there), or 0 with `why`.
int send(const std::wstring& path, const std::wstring& stamp, std::string* why) {
    Target t;
    if (!crack(g_to, &t)) {
        *why = "UploadLogsTo is not an http(s) address";
        return 0;
    }
    std::vector<char> body;
    if (!read_file(path, &body, kMaxBytes, true) || body.empty()) {
        *why = "the log could not be read";
        return 0;
    }
    const std::wstring object = t.path + g_name + L"/" + stamp + L".log";
    const std::wstring agent = widen("PrepareToCoop-LogUpload/" PREPARE_TO_COOP_VERSION);
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        *why = "WinHttpOpen " + std::to_string(GetLastError());
        return 0;
    }
    WinHttpSetTimeouts(session, kTimeoutMs, kTimeoutMs, kTimeoutMs * 2, kTimeoutMs * 2);
    int status = 0;
    HINTERNET connect = WinHttpConnect(session, t.host.c_str(), t.port, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"PUT", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, t.https ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    if (!request) {
        *why = "connect " + std::to_string(GetLastError());
    } else {
        std::wstring headers = L"Content-Type: text/plain; charset=utf-8\r\n";
        if (!t.user.empty()) {
            headers += L"Authorization: Basic " + widen(base64(narrow(t.user) + ":" + narrow(t.password))) + L"\r\n";
        }
        const DWORD n = static_cast<DWORD>(body.size());
        if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1), body.data(), n, n, 0) ||
            !WinHttpReceiveResponse(request, nullptr)) {
            *why = "send " + std::to_string(GetLastError());
        } else {
            DWORD code = 0, size = sizeof(code);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
            status = static_cast<int>(code);
            if (status != 201 && status != 200) *why = "the server refused it";
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return status;
}

// Sends the log at `path` as <stamp>.log; `what` says which, for our own log. The HTTP status (0: no answer).
int send_logged(const std::wstring& path, const std::wstring& stamp, const char* what) {
    std::string why;
    const int status = send(path, stamp, &why);
    const bool sent = status == 201 || status == 200;
    LOG_INFO("[coop] Log upload: %s %s as %s/%s.log (%d%s%s)", what, sent ? "sent" : "NOT sent", narrow(g_name).c_str(),
             narrow(stamp).c_str(), status, why.empty() ? "" : ", ", why.c_str());
    return status;
}

// Sends the log at `path` unless it was sent already, and records it once sent.
void send_once(const std::wstring& path, const char* what) {
    const std::wstring stamp = stamp_of(path);
    if (stamp.empty() || stamp == sent_stamp()) return;
    const int status = send_logged(path, stamp, what);
    if (status == 201 || status == 200) record_sent(stamp);
}

// The logs waiting to be sent (kUnsentPrefix), oldest first: their names hold their start, which sorts by time.
std::vector<std::wstring> unsent_logs() {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((g_dir + kUnsentPrefix + L"*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

// At the start, before any send: the last run's log, if it was not sent when it closed, is moved where the next
// start's log_init does not replace it. Only the newest kUnsentKept wait.
void keep_unsent() {
    const std::wstring prev = g_dir + kPrevName;
    const std::wstring stamp = stamp_of(prev);
    if (!stamp.empty() && stamp != sent_stamp()) {
        const std::wstring to = g_dir + kUnsentPrefix + stamp + L".log";
        if (!MoveFileExW(prev.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            LOG_INFO("[coop] Log upload: the last run's log could not be kept aside (%lu)", GetLastError());
        }
    }
    std::vector<std::wstring> waiting = unsent_logs();
    for (size_t i = 0; i + kUnsentKept < waiting.size(); ++i) DeleteFileW((g_dir + waiting[i]).c_str());
}

DWORD WINAPI start_thread(void*) {
    const size_t prefix = std::wcslen(kUnsentPrefix);
    for (const std::wstring& name : unsent_logs()) {
        const std::wstring path = g_dir + name;
        std::wstring stamp = stamp_of(path);
        if (stamp.empty()) stamp = name.substr(prefix, name.size() - prefix - 4);  // its name without the ".log"
        // No answer (no network, the server down): this one and the rest wait for the next start. One the server
        // answered and refused would be refused again, and is not kept.
        if (send_logged(path, stamp, "an earlier run's log (not sent when it closed)") == 0) {
            LOG_INFO("[coop] Log upload: the logs not sent yet wait for the next start");
            break;
        }
        DeleteFileW(path.c_str());
    }
    return 0;
}

DWORD WINAPI exit_thread(void* what) {
    send_once(g_dir + kLogName, static_cast<const char*>(what));
    return 0;
}

// On the game's way out, once: on a thread of its own, so that a slow or absent network holds the close kExitWaitMs at
// most.
void send_at_exit(const char* what) {
    if (InterlockedExchange(&g_exit_sent, 1) != 0) return;
    HANDLE t = CreateThread(nullptr, 0, exit_thread, const_cast<char*>(what), 0, nullptr);
    if (!t) return;
    WaitForSingleObject(t, kExitWaitMs);
    CloseHandle(t);
}

VOID WINAPI hk_exit_process(UINT code) {
    send_at_exit("this run's log (the game closed)");
    g_exit_process(code);
}

BOOL WINAPI hk_terminate_process(HANDLE process, UINT code) {
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId()) {
        send_at_exit("this run's log (the game ended itself)");
    }
    return g_terminate_process(process, code);
}

// The game's import of KERNEL32!`name` points at `hook` (the old target in `*orig`).
bool patch_import(const char* name, void* hook, void** orig) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "kernel32.dll") != 0 || !imp->OriginalFirstThunk) {
            continue;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), name) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) return false;
            *orig = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
            return *orig != nullptr;
        }
    }
    return false;
}

}  // namespace

void install(const wchar_t* dll_path, bool on, const char* to, const char* name) {
    if (!on) {
        LOG_INFO("[coop] Log upload: off (UploadLogs)");
        return;
    }
    Target t;
    if (!to || !*to || !crack(widen(to), &t)) {
        LOG_INFO("[coop] Log upload: UploadLogs is on, but UploadLogsTo is %s: nothing is sent",
                 to && *to ? "not an http(s) address" : "empty");
        return;
    }
    g_to = widen(to);
    g_dir = dll_path;
    const size_t slash = g_dir.find_last_of(L"\\/");
    g_dir = slash == std::wstring::npos ? std::wstring() : g_dir.substr(0, slash + 1);
    std::wstring who = name && *name ? widen(name) : std::wstring();
    if (who.empty()) {
        wchar_t pc[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
        if (GetComputerNameW(pc, &n)) who = pc;
    }
    g_name = folder_name(who);
    if (g_name.empty()) g_name = L"unnamed";
    void* o = nullptr;
    const bool exit_hook = patch_import("ExitProcess", reinterpret_cast<void*>(&hk_exit_process), &o);
    if (exit_hook) g_exit_process = reinterpret_cast<ExitProcessFn>(o);
    const bool term_hook = patch_import("TerminateProcess", reinterpret_cast<void*>(&hk_terminate_process), &o);
    if (term_hook) g_terminate_process = reinterpret_cast<TerminateProcessFn>(o);
    // The address without its user and password, for the log.
    LOG_INFO("[coop] Log upload: on: this log goes to %s://%s%s%s/ when the game closes (its exit %s)",
             t.https ? "https" : "http", narrow(t.host).c_str(), narrow(t.path).c_str(), narrow(g_name).c_str(),
             exit_hook ? "watched" : "NOT watched: it goes at the next start");
    keep_unsent();
    if (HANDLE h = CreateThread(nullptr, 0, start_thread, nullptr, 0, nullptr)) CloseHandle(h);
}

}  // namespace coop::logupload
