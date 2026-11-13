// TMX Browser: HTTP client (WinHTTP) with two lanes, a thumbnail disk cache and decoding off the game thread.
// See tmx_net.h.
#include "tmx_net.h"

#include <windows.h>
#include <winhttp.h>

#include <d3d9.h>

#include <algorithm>
#include <chrono>

#include "tmx_logic.h"

namespace tmx {

namespace {

uint64_t nowMs() { return GetTickCount64(); }

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring queryHeader(HINTERNET req, DWORD what) {
    DWORD len = 0;
    WinHttpQueryHeaders(req, what, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &len, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0 || len > 8192) return std::wstring();
    std::wstring v(len / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(req, what, WINHTTP_HEADER_NAME_BY_INDEX, &v[0], &len, WINHTTP_NO_HEADER_INDEX)) return std::wstring();
    v.resize(len / sizeof(wchar_t));
    return v;
}

bool hostOfUrl(const std::wstring& url, std::wstring& host, bool& https) {
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t h[256] = {};
    uc.lpszHostName = h;
    uc.dwHostNameLength = 255;
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) return false;
    host.assign(h, uc.dwHostNameLength);
    https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

bool readFileBytes(const std::string& path, std::string& out, size_t maxBytes) {
    HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && static_cast<unsigned long long>(sz.QuadPart) <= maxBytes;
    if (ok) {
        out.resize(static_cast<size_t>(sz.QuadPart));
        DWORD n = 0;
        ok = ReadFile(f, &out[0], static_cast<DWORD>(out.size()), &n, nullptr) && n == out.size();
    }
    CloseHandle(f);
    return ok;
}

// Written to <file>.tmp first, then renamed: a half-written cache file never counts.
void writeCache(const std::string& path, const std::string& bytes) {
    const std::string tmp = path + ".tmp";
    if (!createDirs(path) || !writeFile(tmp, bytes.data(), bytes.size())) return;
    if (!MoveFileExW(widen(tmp).c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(widen(tmp).c_str());
}

} // namespace

void Net::configure(const std::string& userAgent, int apiGapMs, int imageGapMs) {
    std::lock_guard<std::mutex> lock(m_);
    ua_ = userAgent;
    gapMs_[0] = apiGapMs < 0 ? 0 : apiGapMs;
    gapMs_[1] = imageGapMs < 0 ? 0 : imageGapMs;
}

void Net::submit(const NetJob& job) {
    if (sync_) {
        NetResult r = run(job);
        std::lock_guard<std::mutex> lock(m_);
        done_.push_back(std::move(r));
        return;
    }
    std::lock_guard<std::mutex> lock(m_);
    if (threads_.empty()) {
        stopping_ = false;
        if (!session_) {
            const std::wstring ua = widen(ua_);
            HINTERNET s = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!s) s = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (s) {
                WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
                DWORD prot = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
                if (!WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &prot, sizeof(prot))) {
                    prot = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
                    WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &prot, sizeof(prot));
                }
            }
            session_ = s;
        }
        threads_.emplace_back(&Net::worker, this, Lane::Api);
        for (int i = 0; i < 3; i++) threads_.emplace_back(&Net::worker, this, Lane::Image);
    }
    NetJob j = job;
    (void)j;
    q_[static_cast<int>(job.lane)].push_back(job);
    cv_.notify_all();
}

bool Net::poll(NetResult& out) {
    std::lock_guard<std::mutex> lock(m_);
    if (done_.empty()) return false;
    out = std::move(done_.front());
    done_.pop_front();
    return true;
}

void Net::cancelAll() {
    std::lock_guard<std::mutex> lock(m_);
    q_[0].clear();
    q_[1].clear();
    done_.clear();
    gen_++;
}

size_t Net::queued() const {
    std::lock_guard<std::mutex> lock(m_);
    return q_[0].size() + q_[1].size();
}

void Net::stop() {
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(m_);
        stopping_ = true;
        q_[0].clear();
        q_[1].clear();
        gen_++;
        threads.swap(threads_);
        // closing the session makes the running requests fail at once
        if (session_) {
            WinHttpCloseHandle(static_cast<HINTERNET>(session_));
            session_ = nullptr;
        }
    }
    cv_.notify_all();
    for (std::thread& t : threads)
        if (t.joinable()) t.join();
    std::lock_guard<std::mutex> lock(m_);
    done_.clear();
}

void Net::worker(Lane lane) {
    const int li = static_cast<int>(lane);
    for (;;) {
        NetJob job;
        unsigned gen = 0;
        {
            std::unique_lock<std::mutex> lock(m_);
            for (;;) {
                if (stopping_) return;
                if (!q_[li].empty()) {
                    const uint64_t now = nowMs();
                    if (now >= nextStart_[li]) break;
                    cv_.wait_for(lock, std::chrono::milliseconds(nextStart_[li] - now));
                    continue;
                }
                cv_.wait(lock);
            }
            job = std::move(q_[li].front());
            q_[li].pop_front();
            gen = gen_;
            // a cache hit is not a request: the next start is moved on only when the network was used (run())
        }
        NetResult r = run(job);
        std::lock_guard<std::mutex> lock(m_);
        if (gen == gen_ && !stopping_) done_.push_back(std::move(r));
    }
}

NetResult Net::run(const NetJob& job) {
    NetResult res;
    res.id = job.id;
    if (!job.sendLink.empty()) {
        res.r.status = sendLink(job.sendLink, job.window) ? 200 : 0;
        return res;
    }
    std::string bytes;
    if (job.decode && !job.cacheFile.empty() && readFileBytes(job.cacheFile, bytes, job.maxBytes)) {
        res.fromCache = true;
        res.r.status = 200;
    } else {
        if (!sync_) {
            std::lock_guard<std::mutex> lock(m_);
            const int li = static_cast<int>(job.lane);
            const uint64_t now = nowMs();
            nextStart_[li] = std::max(nextStart_[li], now) + static_cast<uint64_t>(gapMs_[li]);
        }
        res.r = fetch(job.url, job.maxBytes, job.follow);
        bytes = std::move(res.r.body);
        res.r.body.clear();
    }
    if (job.decode) {
        if (res.r.status == 200 && isPlaceholderImage(bytes.data(), bytes.size())) {
            res.placeholder = true;
        } else if (res.r.status == 200 && decodeThumb(bytes.data(), bytes.size(), res.rgba, res.w, res.h, 256)) {
            res.decoded = true;
            if (!res.fromCache && !job.cacheFile.empty()) writeCache(job.cacheFile, bytes);
        } else if (res.fromCache) {
            DeleteFileW(widen(job.cacheFile).c_str()); // unreadable cache file: fetched again next time
        }
    } else {
        res.r.body = std::move(bytes);
    }
    return res;
}

NetResponse Net::fetch(const std::string& url, size_t maxBytes, bool follow) {
    if (fetcher_) return fetcher_(url, maxBytes, follow);
    NetResponse out;
    const std::wstring wurl = widen(url);
    std::wstring host;
    bool https = false;
    if (!hostOfUrl(wurl, host, https) || !https || !allowedHost(narrow(host))) {
        out.status = -3;
        return out;
    }
    HINTERNET session = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_);
        session = static_cast<HINTERNET>(session_);
    }
    if (!session) return out;
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc)) return out;
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    HINTERNET con = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                        : nullptr;
    if (req) {
        DWORD policy = follow ? WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, len = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
            out.status = static_cast<int>(status);
            std::string type = narrow(queryHeader(req, WINHTTP_QUERY_CONTENT_TYPE));
            const size_t semi = type.find(';');
            if (semi != std::string::npos) type.resize(semi);
            out.type = type;
            out.location = narrow(queryHeader(req, WINHTTP_QUERY_LOCATION));
            // after redirects the final host must still be an exchange
            DWORD ulen = 0;
            WinHttpQueryOption(req, WINHTTP_OPTION_URL, nullptr, &ulen);
            if (ulen > 0 && ulen < 8192) {
                std::wstring fin(ulen / sizeof(wchar_t) + 1, L'\0');
                if (WinHttpQueryOption(req, WINHTTP_OPTION_URL, &fin[0], &ulen)) {
                    fin.resize(ulen / sizeof(wchar_t));
                    std::wstring fh;
                    bool fs = false;
                    if (!hostOfUrl(fin, fh, fs) || !fs || !allowedHost(narrow(fh))) out.status = -3;
                }
            }
            if (out.status == 200) {
                char buf[16384];
                for (;;) {
                    DWORD n = 0;
                    if (!WinHttpReadData(req, buf, sizeof(buf), &n)) {
                        out.status = GetLastError() == ERROR_WINHTTP_TIMEOUT ? -2 : 0;
                        break;
                    }
                    if (n == 0) break;
                    if (out.body.size() + n > maxBytes) {
                        out.status = -3;
                        break;
                    }
                    out.body.append(buf, n);
                }
                if (out.status != 200) out.body.clear();
            }
        } else {
            out.status = GetLastError() == ERROR_WINHTTP_TIMEOUT ? -2 : 0;
        }
        WinHttpCloseHandle(req);
    }
    if (con) WinHttpCloseHandle(con);
    return out;
}

// Hands a tmtp:// link to the game the way a second TmForever.exe started with /singleinst does it: the
// game's window gets WM_COPYDATA with dwData 'LRUC' and the link as an 8-bit string (the receiver opens it
// like a clicked link). Sent from this worker thread, so the game handles it in its own message loop.
bool Net::sendLink(const std::string& link, void* window) {
    if (linkSender_) return linkSender_(link);
    HWND w = static_cast<HWND>(window ? window : gameWindow(nullptr));
    if (!w) return false;
    COPYDATASTRUCT cd = {};
    cd.dwData = 0x4355524c; // 'LRUC'
    cd.cbData = static_cast<DWORD>(link.size() + 1);
    cd.lpData = const_cast<char*>(link.c_str());
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(w, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 15000, &result) != 0;
}

namespace {
struct FindWin {
    DWORD pid;
    HWND best;
    long long area;
};
BOOL CALLBACK enumWin(HWND w, LPARAM lp) {
    FindWin* f = reinterpret_cast<FindWin*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != f->pid || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    RECT r;
    if (!GetClientRect(w, &r)) return TRUE;
    const long long a = static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
    if (a > f->area) {
        f->area = a;
        f->best = w;
    }
    return TRUE;
}
} // namespace

void* gameWindow(void* d3dDevice) {
    if (d3dDevice) {
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        if (SUCCEEDED(static_cast<IDirect3DDevice9*>(d3dDevice)->GetCreationParameters(&cp)) && cp.hFocusWindow) return cp.hFocusWindow;
    }
    FindWin f = {GetCurrentProcessId(), nullptr, 0};
    EnumWindows(&enumWin, reinterpret_cast<LPARAM>(&f));
    return f.best;
}

void pruneCache(const std::string& folder, size_t keep) {
    struct F {
        std::wstring name;
        FILETIME t;
    };
    std::vector<F> files;
    WIN32_FIND_DATAW fd;
    const std::wstring dir = widen(folder);
    HANDLE h = FindFirstFileW((dir + L"*.img").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(F{fd.cFileName, fd.ftLastWriteTime});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (files.size() <= keep) return;
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return CompareFileTime(&a.t, &b.t) < 0; });
    for (size_t i = 0; i + keep < files.size(); i++) DeleteFileW((dir + files[i].name).c_str());
}

} // namespace tmx
