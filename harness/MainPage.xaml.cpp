#include "pch.h"
#include "MainPage.xaml.h"
#if !defined(APOTHEOSIS_XAML_CODEGEN)
#include "MainPage.g.hpp"
#endif
#include "WebCoreDriver.h"
#include "JitProbe.h"
#include "GpuProbe.h"

#include <robuffer.h>
#include <wrl.h>
#include <windows.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <algorithm>
#include <cwctype>
#include <cstdlib>
#include <ppltasks.h>
#include <collection.h>

using namespace Harness;
using namespace Platform;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Media::Imaging;
using namespace Windows::UI::Core;
using namespace Microsoft::WRL;

// ===== 字符串/路径辅助 =====
static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string ToUtf8(Platform::String^ s)
{
    return s ? WideToUtf8(std::wstring(s->Data())) : std::string{};
}
static std::wstring ToWide(const char* s) { return s ? Utf8ToWide(std::string(s)) : std::wstring{}; }

static std::wstring LocalStateDir()
{
    using namespace Windows::Storage;
    try { return std::wstring(ApplicationData::Current->LocalFolder->Path->Data()); }
    catch (...) { return {}; }
}
static std::wstring InstallDir()
{
    using namespace Windows::ApplicationModel;
    try { return std::wstring(Package::Current->InstalledLocation->Path->Data()); }
    catch (...) { return {}; }
}

// Apotheosis (M4): same device-side opt-in as the engine perf log (LocalState\perf.txt). Set in
// SetupRuntimeEnv so the live-tick path can read it without touching the file system per tick.
static bool g_perfLogEnabled = false;
static unsigned g_memTickCount = 0;

// ===== 运行期配置:fontconfig(含 SimHei CJK 回退)+ CA blob =====
static void SetupRuntimeEnv()
{
    try {
        std::string installDir = WideToUtf8(InstallDir());
        std::string localDir = WideToUtf8(LocalStateDir());
        if (installDir.empty() || localDir.empty())
            return;

        std::string fontsDir = installDir + "\\Assets\\fonts";
        std::string cacheDir = localDir + "\\fontconfig-cache";
        std::string confPath = localDir + "\\fonts.conf";
        std::ofstream conf(confPath, std::ios::binary | std::ios::trunc);
        if (conf) {
            conf << "<?xml version=\"1.0\"?>\n<fontconfig>\n";
            conf << "  <dir>" << fontsDir << "</dir>\n";
            conf << "  <cachedir>" << cacheDir << "</cachedir>\n";
            conf << "  <match target=\"pattern\"><test name=\"family\"><string>sans-serif</string></test>"
                    "<edit name=\"family\" mode=\"prepend\" binding=\"strong\"><string>Segoe UI</string><string>Arial</string><string>SimHei</string></edit></match>\n";
            conf << "  <match target=\"pattern\"><test name=\"family\"><string>serif</string></test>"
                    "<edit name=\"family\" mode=\"prepend\" binding=\"strong\"><string>Times New Roman</string><string>SimHei</string></edit></match>\n";
            conf << "  <match target=\"pattern\"><test name=\"family\"><string>monospace</string></test>"
                    "<edit name=\"family\" mode=\"prepend\" binding=\"strong\"><string>Courier New</string><string>SimHei</string></edit></match>\n";
            // 兜底:任何字族缺字形 → Segoe UI 再 → SimHei(CJK)。
            conf << "  <match target=\"pattern\"><edit name=\"family\" mode=\"append\" binding=\"weak\"><string>Segoe UI</string><string>SimHei</string></edit></match>\n";
            conf << "</fontconfig>\n";
            conf.close();
            _putenv_s("FONTCONFIG_FILE", confPath.c_str());
        }

        // cookie 持久化:SQLite 真实文件 open() 在这个 ARM32 UWP App Container 构建里会崩(2026-07-03
        // 真机验证,VFS 层空函数指针,见项目记忆 cookie-persistence),故不用 WebCoreSetCookieJarPath。
        // 改走引擎自己的 JSON Lines 旁路快照:jar 仍是稳定的 ":memory:",这个文件只是启动时读回 /
        // 切后台时写出的持久化数据,不经过 SQLite 的真实文件 I/O。
        WebCoreSetCookieJsonPath((localDir + "\\cookies.jsonl").c_str());

        // Apotheosis: crash log. Always on (not opt-in like perf.txt): a crash with no
        // WER dump — which is every fast-fail/trap termination on Windows 10 Mobile — is
        // exactly the case we cannot reproduce on the build machine. The engine appends
        // reason + stack frames to LocalState\crash.txt; pull it with WDP after a crash.
        WebCoreSetCrashLogPath((localDir + "\\crash.txt").c_str());

        // Apotheosis (M4): per-phase timing. Device-side opt-in exactly like imedebug.txt —
        // only when the tester dropped LocalState\perf.txt (via WDP, effective after restart)
        // does the engine time the load/paint phases and append them to LocalState\perf.csv.
        // 未放该文件时引擎侧零开销(每个探针只剩一个分支)。SetupRuntimeEnv 跑在引擎线程,符合 ABI 要求。
        if (GetFileAttributesW((LocalStateDir() + L"\\perf.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
            WebCoreSetPerfLogPath((localDir + "\\perf.csv").c_str());
            g_perfLogEnabled = true;   // Apotheosis (M4): same switch also arms the memory tick log
        }

        // CA 根证书:内存 blob 注入(绕 App Container 文件式加载限制)。
        std::string srcCa = installDir + "\\cacert.pem";
        std::vector<uint8_t> caBytes;
        std::ifstream in(srcCa, std::ios::binary | std::ios::ate);
        if (in) {
            std::streamsize n = in.tellg();
            if (n > 0) {
                caBytes.resize((size_t)n);
                in.seekg(0);
                in.read(reinterpret_cast<char*>(caBytes.data()), n);
            }
        }
        if (!caBytes.empty())
            WebCoreSetCACertBlob(caBytes.data(), (int)caBytes.size());
    } catch (...) {}
}

static void WriteStage(const char* stage)
{
    try {
        std::wstring d = LocalStateDir();
        if (d.empty()) return;
        std::ofstream f(WideToUtf8(d) + "\\stage.txt", std::ios::binary | std::ios::trunc);
        if (f) f << stage << "\n";
    } catch (...) {}
}

// Apotheosis (M4): UWP enforces a per-app memory cap; on a Lumia 950 the app is terminated with no
// crash dump once it is exceeded (github.com ≈620 MB → ntv.de). One compact snapshot of the OS view
// of our own working set, appended to the stage lines so WDP can pull the numbers back.
// 这三个 API 在 15254 上都有;取不到就返回 "mem=n/a",绝不影响调用点。
static std::string MemSnapshot()
{
    try {
        unsigned long long used = Windows::System::MemoryManager::AppMemoryUsage;
        unsigned long long limit = Windows::System::MemoryManager::AppMemoryUsageLimit;
        auto lvl = Windows::System::MemoryManager::AppMemoryUsageLevel;
        const char* lvlName = "Unknown";
        switch (lvl) {
        case Windows::System::AppMemoryUsageLevel::Low:       lvlName = "Low"; break;
        case Windows::System::AppMemoryUsageLevel::Medium:    lvlName = "Medium"; break;
        case Windows::System::AppMemoryUsageLevel::High:      lvlName = "High"; break;
        case Windows::System::AppMemoryUsageLevel::OverLimit: lvlName = "OverLimit"; break;
        default: break;
        }
        const unsigned long long kMB = 1024ULL * 1024ULL;
        int pct = (limit > 0) ? (int)((used * 100ULL) / limit) : 0;
        return "mem=" + std::to_string(used / kMB) + "/" + std::to_string(limit / kMB)
             + "MB(" + std::to_string(pct) + "%) lvl=" + std::string(lvlName);
    } catch (...) { return std::string("mem=n/a"); }
}

// Apotheosis (M4): stage.txt is truncated on every write (it is the "where are we now" marker), so
// the memory numbers get their own append-only log: LocalState\mem.txt, one line per event with a
// local HH:mm:ss prefix. 这些行很稀疏(每次导航几行 / 事件级),每次开关文件的开销可以忽略。
static void WriteMemLog(const std::string& line)
{
    try {
        std::wstring d = LocalStateDir();
        if (d.empty()) return;
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        char ts[16] = "";
        ts[0] = (char)('0' + (st.wHour / 10) % 10);   ts[1] = (char)('0' + st.wHour % 10);   ts[2] = ':';
        ts[3] = (char)('0' + (st.wMinute / 10) % 10); ts[4] = (char)('0' + st.wMinute % 10); ts[5] = ':';
        ts[6] = (char)('0' + (st.wSecond / 10) % 10); ts[7] = (char)('0' + st.wSecond % 10); ts[8] = ' ';
        ts[9] = '\0';
        std::ofstream f(WideToUtf8(d) + "\\mem.txt", std::ios::binary | std::ios::app);
        if (f) f << ts << line << "\n";
    } catch (...) {}
}

// Apotheosis (MEMORY-PLAN.md §3 change 2): the engine's own view, appended to the OS view above.
// MemSnapshot() only says *that* we are at 620 MB, never which of the eight buckets moved.
// ENGINE THREAD ONLY — WebCoreGetMemoryStats() walks the MemoryCache and locks the JSC VM.
static std::string EngineMemStats()
{
    WebCoreMemoryStats st = {};
    st.structSize = (int)sizeof st;
    try {
        if (WebCoreGetMemoryStats(&st) != 0) return std::string(" eng=n/a");
    } catch (...) { return std::string(" eng=n/a"); }
    const unsigned long long kMB = 1024ULL * 1024ULL;
    auto mb = [kMB](unsigned long long v) { return std::to_string((v + kMB / 2) / kMB); };
    return " eng jsc=" + mb(st.jscHeapSize) + "/" + mb(st.jscHeapCapacity) + "MB"
         + " extra=" + mb(st.jscExtraMemory) + "MB obj=" + std::to_string(st.jscObjectCount)
         + " mc=" + mb(st.cacheTotal) + "/" + mb(st.cacheLive) + "MB dec=" + mb(st.cacheDecoded) + "MB"
         + " cap=" + mb(st.cacheCapacity) + "MB"
         + " img=" + std::to_string(st.imagesCount) + "/" + mb(st.imagesSize) + "MB/"
                   + mb(st.imagesDecoded) + "MB"
         + " css=" + mb(st.cssSize) + "MB js=" + mb(st.scriptsSize) + "MB font=" + mb(st.fontsSize) + "MB"
         + " tex=" + mb(st.texBytes) + "MB/" + std::to_string(st.texCount)
         + " pool=" + mb(st.poolBytes) + "MB/" + std::to_string(st.poolCount)
         + " plvl=" + std::to_string(st.pressureLevel);
}

// Apotheosis (MEMORY-PLAN.md §3 change 3 / §4): the memory-pressure level we last pushed into
// WebCore. Only ever read/written on the engine thread — the UI-thread MemoryManager handlers
// post into the engine instead of touching it. The OS raises AppMemoryUsageIncreased only on a
// level boundary and never before the silent kill, so our own sampling is the primary source.
static int g_engMemPressure = 0;

// ENGINE THREAD ONLY.
static void ApplyMemoryPressure(int level, const std::string& why)
{
    if (level == g_engMemPressure) return;
    const int previous = g_engMemPressure;
    g_engMemPressure = level;
    try { WebCoreSetMemoryPressure(level); } catch (...) {}
    // One line per transition, so a death after the fact is attributable.
    WriteMemLog("mem-pressure " + std::to_string(previous) + "->" + std::to_string(level)
                + " (" + why + ") " + MemSnapshot());
}

// ENGINE THREAD ONLY. Two property reads per call, so it is cheap enough for every live tick;
// the engine is only touched on an actual transition. Hysteresis: up at 65 %/80 %, down at
// 60 %/75 %, and never straight from 2 back to 0.
static void SampleMemoryPressure()
{
    unsigned long long used = 0, limit = 0;
    try {
        used = Windows::System::MemoryManager::AppMemoryUsage;
        limit = Windows::System::MemoryManager::AppMemoryUsageLimit;
    } catch (...) { return; }
    if (!limit) return;
    const int pct = (int)((used * 100ULL) / limit);
    const int cur = g_engMemPressure;
    int want = cur;
    if (cur <= 0) {
        if (pct >= 80) want = 2;
        else if (pct >= 65) want = 1;
    } else if (cur == 1) {
        if (pct >= 80) want = 2;
        else if (pct < 60) want = 0;
    } else {
        if (pct < 75) want = 1;
    }
    if (want != cur)
        ApplyMemoryPressure(want, "sample pct=" + std::to_string(pct));
}

// IME 诊断日志开关:仅当 LocalState\imedebug.txt 已存在(测试者经 WDP 放置,重启生效)才追加记录 ——
// 对齐 autodiag.txt 的"设备侧显式开启"模式。此前每敲一键 UI/引擎线程各开写一次文件,是打字延迟的
// 固定开销,且日志跨会话无限增长。
static bool ImeDebugEnabled()
{
    static bool enabled = [] {
        std::wstring d = LocalStateDir();
        return !d.empty() && GetFileAttributesW((d + L"\\imedebug.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
    }();
    return enabled;
}

static std::wstring g_lang = L"zh";   // 界面语言:zh(默认)/ en;首启 OOBE 选定,存 settings.ini
// 代码里动态设置的中/英文案(按当前语言返回)。静态 XAML 串由 TranslateNode 树遍历翻译;
// 这个给"运行期才赋值、会盖掉翻译"的标签/toast 用(收藏状态、UA 状态等)。
static Platform::String^ L8(const wchar_t* zh, const wchar_t* en) {
    return ref new Platform::String(g_lang == L"en" ? en : zh);
}
// Apotheosis: same choice for the two other string flavours the harness builds text in —
//   std::wstring (titles, status lines handed to the engine thread) and UTF-8 char* (the
//   built-in home/error pages, which are HTML source).
static std::wstring W8(const wchar_t* zh, const wchar_t* en) {
    return std::wstring(g_lang == L"en" ? en : zh);
}
static const char* U8(const char* zh, const char* en) {
    return g_lang == L"en" ? en : zh;
}

static std::string MakeErrorHtml(const std::string& url, const char* err)
{
    std::string e = err ? err : "";
    return "<html><head><meta charset='utf-8'></head>"
        "<body style='margin:0;background:#fff;font-family:sans-serif'>"
        "<div style='background:#d93025;color:#fff;padding:32px 24px'><h1 style='margin:0;font-size:38px'>"
        + std::string(U8("无法访问此页面", "Can&rsquo;t reach this page")) + "</h1></div>"
        "<div style='padding:24px;color:#333;font-size:24px'><p style='word-break:break-all;color:#1a73e8'>" + url + "</p>"
        "<p style='color:#d93025;font-size:22px;word-break:break-all'>" + e + "</p></div></body></html>";
}

// 设置:搜索引擎前缀 + 主页(默认值;LoadSettings 从 settings.ini 覆盖)。free 函数 NormalizeUrl/构造用,故放全局。
static std::wstring g_searchPrefix = L"https://www.qwant.com/?q=";
static std::wstring g_homeUrl = L"about:home";
// Apotheosis (PRIVACY-AUDIT.md recommended action 3): index 4 = Qwant, the default for a fresh
//   install - it is the only one of these that states it does not track or profile its users, and
//   the previous default (cn.bing.com, Bing China) was a poor fit outside China. Bing now goes to
//   www.bing.com. Existing users keep whatever settings.ini already stores, so the indices below
//   must never be renumbered - new engines are appended.
static std::wstring SearchPrefixFor(int idx)
{
    switch (idx) {
        case 0: return L"https://www.bing.com/search?q=";
        case 1: return L"https://www.google.com/search?q=";
        case 2: return L"https://duckduckgo.com/?q=";
        case 3: return L"https://www.baidu.com/s?wd=";
        default: return L"https://www.qwant.com/?q=";   // 4 = Qwant, also the fallback for a bad index
    }
}

static std::string HtmlEscape(const std::string& s)
{
    std::string out; out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}
static std::string HostOfU8(const std::string& u)
{
    size_t p = u.find("://");
    size_t s = (p == std::string::npos) ? 0 : p + 3;
    size_t e = u.find('/', s);
    return u.substr(s, (e == std::string::npos) ? std::string::npos : e - s);
}

// 动态新标签页:书签优先、历史补足的速拨磁贴(最多 8)。磁贴=<a>,经渲染时链接提取→点击导航。
static std::string BuildHomeHtml(const std::vector<Harness::Entry>& bookmarks, const std::vector<Harness::Entry>& history)
{
    std::vector<Harness::Entry> tiles;
    std::vector<std::wstring> seen;
    auto add = [&](const std::vector<Harness::Entry>& src) {
        for (const auto& e : src) {
            if (tiles.size() >= 8) break;
            if (e.url.empty() || e.url == L"about:home") continue;
            if (std::find(seen.begin(), seen.end(), e.url) != seen.end()) continue;
            seen.push_back(e.url);
            tiles.push_back(e);
        }
    };
    add(bookmarks);
    add(history);

    std::string h;
    h += "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><style>";
    h += "*{box-sizing:border-box}body{margin:0;background:#f5f6f8;font-family:sans-serif;color:#202124}";
    h += ".hero{background:linear-gradient(135deg,#00aa77,#0088cc);color:#fff;padding:46px 26px 38px}";
    h += ".hero h1{margin:0;font-size:46px;letter-spacing:-1px}.hero p{margin:10px 0 0;font-size:20px;opacity:.92}";
    h += ".wrap{padding:24px}.sec{font-size:17px;color:#5f6368;margin:0 0 14px}";
    h += ".grid{display:grid;grid-template-columns:repeat(2,1fr);gap:14px}";
    h += "a.tile{display:block;text-decoration:none;background:#fff;border-radius:16px;padding:18px 18px 20px;box-shadow:0 2px 10px rgba(0,0,0,.08);color:#202124}";
    h += ".tile .t{font-size:20px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}";
    h += ".tile .u{font-size:15px;color:#80868b;margin-top:7px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}";
    h += "</style></head><body>";
    h += "<div class='hero'><h1>EdgeHTML Reborn</h1><p>";
    h += U8("\xE7\x8E\xB0\xE4\xBB\xA3\xE6\xB5\x8F\xE8\xA7\x88\xE5\x99\xA8\xE5\xBC\x95\xE6\x93\x8E", "A modern browser engine");
    h += " &middot; Windows 10 Mobile &middot; ARM32</p></div>";
    h += "<div class='wrap'>";
    if (tiles.empty()) {
        h += "<p class='sec'>";
        h += U8("\xE5\x9C\xA8\xE4\xB8\x8A\xE6\x96\xB9\xE5\x9C\xB0\xE5\x9D\x80\xE6\xA0\x8F\xE8\xBE\x93\xE5\x85\xA5\xE7\xBD\x91\xE5\x9D\x80\xE8\xAE\xBF\xE9\x97\xAE\xE7\xBD\x91\xE9\xA1\xB5\xE3\x80\x82",
                "Type a URL in the address bar above to open a page.");
        h += "</p><div class='grid'>";
        const char* defs[][2] = { {"https://example.com","example.com"}, {"https://github.com","github.com"}, {"https://www.bing.com","bing.com"}, {"https://en.wikipedia.org","wikipedia.org"} };
        for (auto& d : defs) { h += "<a class='tile' href='"; h += d[0]; h += "'><div class='t'>"; h += d[1]; h += "</div><div class='u'>"; h += d[0]; h += "</div></a>"; }
        h += "</div>";
    } else {
        h += "<p class='sec'>";
        h += U8("\xE5\xB8\xB8\xE7\x94\xA8\xE7\xAB\x99\xE7\x82\xB9", "Frequently visited");
        h += "</p><div class='grid'>";
        for (const auto& e : tiles) {
            std::string href = HtmlEscape(WideToUtf8(e.url));
            std::string title = HtmlEscape(WideToUtf8(e.title.empty() ? e.url : e.title));
            std::string host = HtmlEscape(HostOfU8(WideToUtf8(e.url)));
            h += "<a class='tile' href='" + href + "'><div class='t'>" + title + "</div><div class='u'>" + host + "</div></a>";
        }
        h += "</div>";
    }
    h += "</div></body></html>";
    return h;
}

static Platform::String^ NormalizeUrl(Platform::String^ raw)
{
    std::wstring s = raw ? std::wstring(raw->Data()) : L"";
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    if (s.empty())
        return ref new String(L"about:home");
    // 含空格或没有点且不像域名 → 当作搜索词交给默认搜索引擎(g_searchPrefix，设置里选)。
    bool looksUrl = (s.find(L"://") != std::wstring::npos) || (s.find(L'.') != std::wstring::npos && s.find(L' ') == std::wstring::npos);
    if (s.rfind(L"about:", 0) == 0)
        return ref new String(s.c_str());
    if (!looksUrl) {
        std::wstring q;
        for (wchar_t c : s) { if (c == L' ') q += L"%20"; else q += c; }
        return ref new String((g_searchPrefix + q).c_str());
    }
    if (s.find(L"://") == std::wstring::npos)
        s = L"https://" + s;
    return ref new String(s.c_str());
}

// ===== 单一引擎线程:WebCore/JSC 严格单线程,所有引擎调用串行其上 =====
class WebEngine {
public:
    static WebEngine& instance() { static WebEngine e; return e; }
    void post(std::function<void()> job)
    {
        { std::lock_guard<std::mutex> lk(m_mtx); m_q.push_back(std::move(job)); }
        m_cv.notify_one();
    }
private:
    WebEngine() { std::thread([this] { loop(); }).detach(); }
    void loop()
    {
        SetupRuntimeEnv();   // 一次,在引擎线程,字体 + CA,必须在首次加载前。
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cv.wait(lk, [this] { return !m_q.empty(); });
                job = std::move(m_q.front());
                m_q.pop_front();
            }
            try { job(); } catch (...) {}
        }
    }
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_q;
};

// App::OnSuspending 的落地点(见 App.xaml.cpp):cookie JSON 落盘转给引擎线程串行执行,写完才
// Complete deferral——UWP 挂起到进程被冻结/可能被系统直接终止之间只给系统定的几秒钟,这是唯一
// 有时间保证的落盘时机(Window::VisibilityChanged 触发的是不等结果的 fire-and-forget,曾实测
// 切后台重开后 cookie 没保住,应是没跑完就被冻结)。
void MainPage::FlushCookiesForSuspend(Windows::ApplicationModel::SuspendingDeferral^ deferral)
{
    WebEngine::instance().post([deferral]() {
        try { WebCoreFlushCookiesToDisk(); } catch (...) {}
        // Apotheosis (M4): 同理落盘性能日志 —— 环形缓冲平时只在导航完成时写盘,挂起后进程可能被
        // 系统直接终止,未落盘的行就丢了。关闭时为 no-op。
        try { WebCorePerfFlush(); } catch (...) {}
        deferral->Complete();
    });
}

// GPU 直呈现模式:引擎已 swapBuffers 到可见 GpuPanel,无需把 rgba blit 进 WriteableBitmap(RenderImage 已隐藏)。
//   置位后 BlitToBitmap 直接返回,省掉每帧 3MB 的 RGBA→BGRA 拷贝(冲 60fps)。引擎线程与 UI 线程都可能读,用 atomic。
static std::atomic<bool> g_directPresent { false };

// ===== 渲染缓冲 → WriteableBitmap(RGBA→BGRA)=====
// Raw 版无条件贴图:标签切换快照要在直呈现模式下也贴得出来(那时它盖在透明的 GpuPanel 下面)。
static void BlitToBitmapRaw(WriteableBitmap^ wb, const std::vector<uint8_t>& rgba, int W, int H)
{
    ComPtr<Windows::Storage::Streams::IBufferByteAccess> bba;
    reinterpret_cast<IInspectable*>(wb->PixelBuffer)->QueryInterface(IID_PPV_ARGS(&bba));
    byte* dst = nullptr;
    bba->Buffer(&dst);
    // 按 32 位字交换 R/B:比逐字节拷贝少 ~4 倍内存访问(每帧 3MB 的热路径,ARM32 上可观)。
    // 两侧缓冲均 4 字节对齐(vector 堆块 / XAML 像素缓冲)。LE 下 RGBA 内存序 = A<<24|B<<16|G<<8|R,
    // BGRA 需 A<<24|R<<16|G<<8|B → 保留 G/A 字节,交换 R/B 字节。
    const size_t n = (size_t)W * H;
    const uint32_t* src = reinterpret_cast<const uint32_t*>(rgba.data());
    uint32_t* d32 = reinterpret_cast<uint32_t*>(dst);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t v = src[i];
        d32[i] = (v & 0xFF00FF00u) | ((v >> 16) & 0xFFu) | ((v & 0xFFu) << 16);
    }
}

static void BlitToBitmap(WriteableBitmap^ wb, const std::vector<uint8_t>& rgba, int W, int H)
{
    if (g_directPresent.load()) return;   // 直呈现:跳过软件 blit(GpuPanel 已由引擎呈现)
    BlitToBitmapRaw(wb, rgba, W, H);
}

// 把 RGBA(上→下)写成 32 位 BMP(BGRA,自下而上)——供自动诊断把 GPU readback 的实际帧落盘,
// 经 WDP 拉回当"截图"看(无 UI、无 PNG 编码器依赖;ARM 上头部用 memcpy 避免非对齐写)。
static void WriteBmp32(const std::string& path, const uint8_t* rgba, int w, int h)
{
    const uint32_t dataSize = (uint32_t)w * h * 4;
    uint8_t fh[54] = {0};
    fh[0] = 'B'; fh[1] = 'M';
    auto put32 = [&](int off, uint32_t v) { memcpy(fh + off, &v, 4); };
    auto put16 = [&](int off, uint16_t v) { memcpy(fh + off, &v, 2); };
    put32(2, 54 + dataSize); put32(10, 54);
    put32(14, 40); put32(18, (uint32_t)w); put32(22, (uint32_t)h);   // 正高=自下而上
    put16(26, 1); put16(28, 32); put32(30, 0); put32(34, dataSize);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write((char*)fh, 54);
    std::vector<uint8_t> row((size_t)w * 4);
    for (int fy = 0; fy < h; ++fy) {
        const uint8_t* src = rgba + (size_t)(h - 1 - fy) * w * 4;
        for (int x = 0; x < w; ++x) {
            row[x * 4 + 0] = src[x * 4 + 2]; // B
            row[x * 4 + 1] = src[x * 4 + 1]; // G
            row[x * 4 + 2] = src[x * 4 + 0]; // R
            row[x * 4 + 3] = src[x * 4 + 3]; // A
        }
        f.write((char*)row.data(), (std::streamsize)w * 4);
    }
}

static const int kW = 720, kH = 1080;

// Apotheosis (M4): 页面缩放边界不能超出引擎侧 —— port\WebCoreDriver.cpp 的 WebCoreSetPageScale
//   自己把 scale 钳到 [0.5, 6.0]；harness 若用更宽的上下界，超界的捏合会被引擎悄悄改成别的值，
//   harness 记的 m_pageScale 就和 Page::pageScaleFactor() 对不上（下次捏合基准错）。子区间是安全的。
// Apotheosis: the committed minimum is the engine's own minimum, 0.5 — pinching out past 1:1
//   and letting go now really zooms out, so a long page can be surveyed at a glance.
//   NOTE what "below 1:1" does and does not do: layout still happens at the engine viewport
//   width (kW), so zooming out does NOT reflow a wider desktop layout into the extra room —
//   the same fit-to-width page is simply drawn smaller from the top-left and the right side
//   stays blank. A real wide-viewport "desktop layout" mode (lay out at, say, 980 px and let
//   the page scale be the fit-to-width ratio) is a separate feature; see PERF-OPTIONS.md.
static const float kMinPageScale = 0.5f;
static const float kMaxPageScale = 6.0f;
// The live preview never needs to go below what we can commit any more.
static const float kMinLiveScale = 0.5f;
// Spring-back animation on the preview transform. Short enough to feel like a release, long
//   enough to read as a movement rather than a jump; runs on the composition thread.
static const int kZoomSpringMs = 180;
// Apotheosis: snap band around 1:1. Generous (±33 %, package 7 feedback widened it from ±20 %) on
//   purpose — 1:1 is the one scale that matters (fit-to-width, crisp text), a pinch is an
//   accumulating product of float deltas, and without a wide band the page ends up parked at 0.94
//   or 1.07 forever, with the error growing on every gesture. Below 0.67 (and above 1.33) the user
//   clearly wants a different scale, so the real value is committed. The 180 ms spring animates the
//   preview across the snap gap so it reads as a release rather than a jump (SpringBackZoom).
static const float kPageScaleSnapTol = 0.33f;

// 钳到引擎接受的区间，并把接近 1:1 的结果吸附成精确 1.0。
static float SnapAndClampPageScale(float s)
{
    if (!(s > 0.0f)) s = 1.0f;
    if (s < kMinPageScale) s = kMinPageScale;
    if (s > kMaxPageScale) s = kMaxPageScale;
    if (s > 1.0f - kPageScaleSnapTol && s < 1.0f + kPageScaleSnapTol) s = 1.0f;
    return s;
}

// Apotheosis (package 7 feedback: "clamp the live preview to the document edges like
//   Safari/Chrome — content never leaves the viewport at >=1, centred below 1, small rubber-band
//   allowed"): how far the live pinch preview is allowed to drift from a clean anchor-only scale,
//   in DIP, before ClampZoomAxis below starts pulling it back. Eased to 0 by SpringBackZoom on
//   release, exactly like the ±33% page-scale snap band above is eased.
static const double kZoomRubberBandDip = 40.0;

// One axis of the live-pinch clamp (ApplyLiveZoom). Works entirely in engine px (WebCoreGetScrollState
//   units), like ClampPanRemainder/InstantPanBy — the caller converts the result to DIP.
//
//   ScaleTransform(center=anchor, scale=live) with no translate shows document range
//   [scrollPos + anchor*(1-1/live), scrollPos + anchor + (viewSize-anchor)/live] (T=0 baseline;
//   see the derivation in ApplyLiveZoom's comment). For live>=1 that range is provably a SUBSET of
//   what the anchor-only scale already covers for any anchor position — scaling a rect up about an
//   interior point never shrinks it — so this only has to catch the case the pure geometry cannot
//   see: the *document* itself runs out (scrollPos/contentSize say so) before the raster does, e.g.
//   already scrolled to an edge. For live<1 there is no way to avoid a gap (the image is genuinely
//   smaller than the viewport), so instead of leaving it wherever the anchor happens to sit
//   (asymmetric, package-7's "right side never fills"), just recentre it.
static double ClampZoomAxis(double scrollPos, double contentSize, double anchor, double viewSize,
                             double live, double rubberBandPx, bool haveScroll)
{
    if (!(viewSize > 1.0) || !(live > 0.0))
        return 0.0;
    if (live < 1.0)   // overview: centre the shrunk frame instead of tracking the document
        return (viewSize / 2.0 - anchor) * (1.0 - live);
    if (!haveScroll || !(contentSize > 0.0))
        return 0.0;   // no WebCoreGetScrollState answer yet this gesture: nothing to clamp against
    const double lo = scrollPos + anchor * (1.0 - 1.0 / live);
    const double hi = scrollPos + anchor + (viewSize - anchor) / live;
    double corrDoc = 0.0;
    if (lo < -rubberBandPx) corrDoc = (-rubberBandPx) - lo;                        // pull the window forward
    else if (hi > contentSize + rubberBandPx) corrDoc = (contentSize + rubberBandPx) - hi;  // pull it back
    if (corrDoc == 0.0)
        return 0.0;
    return -corrDoc * live;   // a screen translate T shifts the shown doc window by -T/live
}

// 取一块引擎渲染输出缓冲(kW*kH*4)。直呈现模式:UI 从不读这块 RGBA(BlitToBitmap 空转、各回调按
// m_gpuPresent 跳过贴图),且引擎线程严格串行 → 全程复用同一块,免去热路径(实时 tick/拖拽滚动/
// 逐键重绘)每帧 3MB 的分配+清零。软件模式必须每次新分配:UI 线程可能还拿着上一帧在读。
static std::shared_ptr<std::vector<uint8_t>> AcquireEngineBuffer(bool present)
{
    if (present) {
        static std::shared_ptr<std::vector<uint8_t>> s_buf =
            std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4);
        return s_buf;
    }
    return std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
}

// ============================================================================
MainPage::MainPage()
{
    InitializeComponent();

    // Apotheosis: focus sink for DismissKeyboardForOverlay() — Grid (RootGrid) has no IsTabStop
    //   property (that's Control-only), so the page itself takes focus instead; set here in code
    //   rather than in MainPage.xaml since Page IS a Control (via UserControl).
    try { this->IsTabStop = true; } catch (...) {}

    // Apotheosis: MainPage.xaml only wires ContentArea's ManipulationDelta/ManipulationCompleted
    // (see MainPage.xaml) — ManipulationStarted has no markup hook, so it is subscribed here in
    // code instead of touching the XAML. Kicks off the nested-scroll hit test (WebCoreIsScrollableAt)
    // as early as possible, before the first ManipulationDelta of the gesture arrives.
    ContentArea->ManipulationStarted += ref new Windows::UI::Xaml::Input::ManipulationStartedEventHandler(
        this, &MainPage::OnImageManipStarted);

    // 分享:注册一次 DataRequested(原生分享契约,App Container/1607 起可用)。分享当前页 URL+标题。
    try {
        auto dtm = Windows::ApplicationModel::DataTransfer::DataTransferManager::GetForCurrentView();
        dtm->DataRequested += ref new Windows::Foundation::TypedEventHandler<
            Windows::ApplicationModel::DataTransfer::DataTransferManager^,
            Windows::ApplicationModel::DataTransfer::DataRequestedEventArgs^>(
            [this](Windows::ApplicationModel::DataTransfer::DataTransferManager^,
                   Windows::ApplicationModel::DataTransfer::DataRequestedEventArgs^ e) {
                if (m_currentUrl.empty() || m_currentUrl == L"about:home") {
                    e->Request->FailWithDisplayText(L8(L"无可分享内容", L"Nothing to share"));
                    return;
                }
                auto req = e->Request;
                req->Data->Properties->Title = ref new Platform::String(
                    m_currentTitle.empty() ? m_currentUrl.c_str() : m_currentTitle.c_str());
                req->Data->Properties->Description = ref new Platform::String(m_currentUrl.c_str());
                try {
                    req->Data->SetWebLink(ref new Windows::Foundation::Uri(ref new Platform::String(m_currentUrl.c_str())));
                } catch (...) {
                    req->Data->SetText(ref new Platform::String(m_currentUrl.c_str()));
                }
            });
    } catch (...) {}

    // 一次性可执行内存探针(JIT 可行性),结果写 LocalState\jitresult.txt 供 WDP 拉取。
    try {
        std::string jit = RunJitProbe();
        std::wstring d = LocalStateDir();
        if (!d.empty()) {
            std::ofstream jf(WideToUtf8(d) + "\\jitresult.txt", std::ios::binary | std::ios::trunc);
            if (jf) jf.write(jit.data(), jit.size());
        }
    } catch (...) {}
    LoadData();
    LoadSettings();   // 搜索引擎/主页/默认UA/缩放/标签模式(在首次导航前应用)
    // OOBE:从没存过 lang(全新安装)→ 弹首启选语言浮层;否则按已选语言应用(英文则翻译整个界面)。
    if (!m_langSet) { if (OobePanel) OobePanel->Visibility = Windows::UI::Xaml::Visibility::Visible; }
    else { ApplyLanguage(); }
    // 初始化标签集合:活动标签的实时状态用全局成员表示,此处占位 1 个(首次导航填充其全局状态)。
    { Tab t0; t0.currentUrl = g_homeUrl; m_tabs.push_back(t0); m_activeTab = 0; }
    UpdateTabCount();
    // GPU 崩溃环路保护:上次自动开 GPU 没干净返回(标记残留)→ 这次别再默认开,并持久化关掉(避免每次启动即崩)。
    {
        std::wstring d = LocalStateDir();
        if (!d.empty() && GetFileAttributesW((d + L"\\gpu-crash.flag").c_str()) != INVALID_FILE_ATTRIBUTES) {
            m_gpuDefault = false;
            try { DeleteFileW((d + L"\\gpu-crash.flag").c_str()); } catch (...) {}
            SaveSettings();
        }
    }
    // 前后台切换:后台暂停实时渲染(省电、避免后台跑引擎被 PLM 冻结时堆积)。
    Window::Current->VisibilityChanged += ref new Windows::UI::Xaml::WindowVisibilityChangedEventHandler(
        [this](Platform::Object^, Windows::UI::Core::VisibilityChangedEventArgs^ e) {
            m_appForeground = e->Visible;
            if (e->Visible) StartLiveMode();
            else {
                StopLiveMode();
                // cookie 落盘(JSON Lines 快照):UWP 挂起的应用可能被系统直接终止、不会再回调任何
                // 生命周期事件,切后台这一刻是最后的安全落盘时机。引擎线程异步(线程铁律:UI 线程
                // 绝不同步 wait 引擎),不等它做完就返回——反正马上要挂起,没有下一步依赖它的操作。
                WebEngine::instance().post([]() { try { WebCoreFlushCookiesToDisk(); } catch (...) {} });
            }
        });
    // Apotheosis (PRIVACY-AUDIT.md): "Wi-Fi only" prefetch has to follow the connection. The event
    //   arrives on a worker thread, so hop to the UI thread first (m_prefetch lives there) and let
    //   ApplyPrefetchSetting post the engine call - never call the engine from here.
    try {
        Windows::Networking::Connectivity::NetworkInformation::NetworkStatusChanged +=
            ref new Windows::Networking::Connectivity::NetworkStatusChangedEventHandler(
                [this](Platform::Object^) {
                    try {
                        Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
                            ref new DispatchedHandler([this]() { ApplyPrefetchSetting(); }));
                    } catch (...) {}
                });
    } catch (...) {}
    // 实体返回键(Win10M 硬件 Back):接管系统返回事件 → 先关浮层/再浏览器后退/否则交系统。
    try {
        Windows::UI::Core::SystemNavigationManager::GetForCurrentView()->BackRequested +=
            ref new Windows::Foundation::EventHandler<Windows::UI::Core::BackRequestedEventArgs^>(this, &MainPage::OnHardwareBack);
    } catch (...) {}
    // 内存压力(防 OOM):UWP 报应用内存到高水位/超限 → 让引擎一把放缓存(后退页面缓存已默认关)。
    // 走引擎线程异步,绝不在 UI 线程同步 wait 引擎(线程铁律)。
    try {
        Windows::System::MemoryManager::AppMemoryUsageIncreased +=
            ref new Windows::Foundation::EventHandler<Platform::Object^>(
                [](Platform::Object^, Platform::Object^) {
                    auto lvl = Windows::System::MemoryManager::AppMemoryUsageLevel;
                    // Apotheosis (M4): the OS only raises this when we cross a level boundary (rare),
                    // so logging it is free and it is the last breadcrumb before a silent kill.
                    WriteMemLog("mem-event increased " + MemSnapshot());
                    // Apotheosis (MEMORY-PLAN.md §3 change 3): Medium is the level we actually
                    // spend our time in and it used to do nothing at all. Push every level into
                    // WebCore — on the engine thread, never from here.
                    int want = 0;
                    if (lvl == Windows::System::AppMemoryUsageLevel::Medium) want = 1;
                    else if (lvl == Windows::System::AppMemoryUsageLevel::High) want = 2;
                    else if (lvl == Windows::System::AppMemoryUsageLevel::OverLimit) want = 2;
                    WebEngine::instance().post([want]() { try { ApplyMemoryPressure(want, "mem-event"); } catch (...) {} });
                    if (lvl == Windows::System::AppMemoryUsageLevel::High || lvl == Windows::System::AppMemoryUsageLevel::OverLimit) {
                        int crit = (lvl == Windows::System::AppMemoryUsageLevel::OverLimit) ? 1 : 0;
                        // Unconditional: ApplyMemoryPressure() above is a no-op once the level is
                        // already 2, and OverLimit is the last breadcrumb before the kill.
                        WebEngine::instance().post([crit]() { try { WebCoreReleaseMemory(crit); } catch (...) {} });
                    }
                });
        // Apotheosis (M4): the cap itself moves (another app in the foreground, PLM). The args carry
        // the old/new limit — that is exactly the number we are missing when the app dies silently.
        Windows::System::MemoryManager::AppMemoryUsageLimitChanging +=
            ref new Windows::Foundation::EventHandler<Windows::System::AppMemoryUsageLimitChangingEventArgs^>(
                [](Platform::Object^, Windows::System::AppMemoryUsageLimitChangingEventArgs^ e) {
                    const unsigned long long kMB = 1024ULL * 1024ULL;
                    unsigned long long oldLimit = 0, newLimit = 0;
                    try { oldLimit = e->OldLimit; newLimit = e->NewLimit; } catch (...) {}
                    WriteMemLog("mem-event limit-changing old=" + std::to_string(oldLimit / kMB)
                                + "MB new=" + std::to_string(newLimit / kMB) + "MB "
                                + MemSnapshot());
                    // 新上限已低于当前用量 → 立刻按临界级别放缓存,别等 AppMemoryUsageIncreased。
                    if (newLimit > 0 && Windows::System::MemoryManager::AppMemoryUsage >= newLimit)
                        WebEngine::instance().post([]() {
                            try { ApplyMemoryPressure(2, "limit-changing"); } catch (...) {}
                            try { WebCoreReleaseMemory(1); } catch (...) {}
                        });
                });
    } catch (...) {}
    // 软键盘遮挡:底栏在屏幕底部,键盘弹出会盖住地址栏。仅当地址栏聚焦时把导航栏(标签数/地址胶囊/
    //   菜单键那一行,NavBarShift)上移键盘高度;细状态行(标题/加载点,在它上面那一行)刻意不受
    //   影响的话,键盘再高也推不出屏幕。网页表单输入(ImeBox)不上移——引擎自管把聚焦框滚进视口。
    try {
        auto ip = Windows::UI::ViewManagement::InputPane::GetForCurrentView();
        ip->Showing += ref new Windows::Foundation::TypedEventHandler<
            Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^>(
            [this](Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^ e) {
                if (m_urlFocused && NavBarShift) {
                    NavBarShift->Y = -e->OccludedRect.Height;
                    // Apotheosis (review 2026-09-03): the suggestion dropdown is anchored to the
                    //   bottom of the *content* row, i.e. it floats right on top of the nav bar.
                    //   Shifting only the nav bar left it behind the keyboard while typing, so it
                    //   rides along by the same amount (its own transform — different subtree).
                    ShiftSuggestPanel(-e->OccludedRect.Height);
                    e->EnsuredFocusedElementInView = true;   // 已自行让位,系统勿再额外滚动
                }
            });
        ip->Hiding += ref new Windows::Foundation::TypedEventHandler<
            Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^>(
            [this](Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^ e) {
                if (NavBarShift && NavBarShift->Y != 0) {   // 仅当我们上移过才复位+认领(设置页文本框靠系统自身滚动恢复,别干扰)
                    NavBarShift->Y = 0;
                    ShiftSuggestPanel(0.0);
                    e->EnsuredFocusedElementInView = true;
                }
            });
    } catch (...) {}
    // Apotheosis (review 2026-09-03): DISPLAY toggle "Hide navigation bar". Wired here rather than
    //   with a Toggled="" attribute in the XAML: the hand-rolled code-behind generator
    //   (port\gen-xaml-codebehind.ps1) knows no Toggled event, and an event attribute it cannot
    //   resolve would make XamlReader::Load throw on that fallback path.
    if (SetHideNavBarSwitch)
        SetHideNavBarSwitch->Toggled += ref new Windows::UI::Xaml::RoutedEventHandler(
            this, &MainPage::OnHideNavBarToggled);
    // Apotheosis: status bar like Edge — extend the app under it (translucent, page content may
    //   run behind it), but keep our own chrome clear of the shell's own furniture.
    //   Review 2026-09-03: SetDesiredBoundsMode(UseCoreWindow) extends the window under the
    //   *software navigation bar* as well, and StatusBar::OccludedRect reports an empty rect once
    //   BackgroundOpacity is 0 — so the old OccludedRect padding never fired and the address row
    //   sat under the back/home/search buttons. The insets now come from ApplicationView's
    //   VisibleBounds (what the shell leaves usable) against the CoreWindow bounds; see
    //   ApplyViewInsets(). VisibleBoundsChanged fires for rotation, status-bar and nav-bar changes.
    try {
        auto view = Windows::UI::ViewManagement::ApplicationView::GetForCurrentView();
        view->SetDesiredBoundsMode(Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
        view->VisibleBoundsChanged += ref new Windows::Foundation::TypedEventHandler<
            Windows::UI::ViewManagement::ApplicationView^, Platform::Object^>(
            [this](Windows::UI::ViewManagement::ApplicationView^, Platform::Object^) { ApplyViewInsets(); });
    } catch (...) {}
    try {
        if (Windows::Foundation::Metadata::ApiInformation::IsTypePresent(L"Windows.UI.ViewManagement.StatusBar")) {
            auto sb = Windows::UI::ViewManagement::StatusBar::GetForCurrentView();
            sb->BackgroundOpacity = 0.0;
            sb->ForegroundColor = Windows::UI::ColorHelper::FromArgb(0xFF, 0xF4, 0xF7, 0xF8);   // TxtHi:时钟在深色 chrome 上仍可读
            // Fallback only: on a shell that does not move VisibleBounds, showing/hiding the bar is
            //   still the moment the usable top edge changes — re-measure, do not trust OccludedRect.
            sb->Showing += ref new Windows::Foundation::TypedEventHandler<
                Windows::UI::ViewManagement::StatusBar^, Platform::Object^>(
                [this](Windows::UI::ViewManagement::StatusBar^, Platform::Object^) { ApplyViewInsets(); });
            sb->Hiding += ref new Windows::Foundation::TypedEventHandler<
                Windows::UI::ViewManagement::StatusBar^, Platform::Object^>(
                [this](Windows::UI::ViewManagement::StatusBar^, Platform::Object^) { ApplyViewInsets(); });
        }
    } catch (...) {}
    ApplyViewInsets();
    // 测试钩子:若 LocalState\testurl.txt 存在,启动直接导航到它(供 WDP 远程自动化测试,免 UI 输入)。
    std::wstring testUrl;
    try {
        std::wstring d = LocalStateDir();
        if (!d.empty()) {
            std::ifstream f(WideToUtf8(d) + "\\testurl.txt", std::ios::binary);
            if (f) { std::string s; std::getline(f, s); testUrl = Utf8ToWide(s); }
            while (!testUrl.empty() && (testUrl.back() == L'\r' || testUrl.back() == L'\n' || testUrl.back() == L' ' || testUrl.back() == L'\t'))
                testUrl.pop_back();
        }
    } catch (...) {}
    // 自动诊断钩子:若 LocalState\autodiag.txt 存在(每行一个 URL,# 开头忽略),启动后在引擎线程
    //   自动 GpuInit(离屏)+ 逐个 GPU 合成加载 + dump 各页 diag/层树到 autodump.txt,供 WDP 全自动抓取
    //   (免 UI 点按 GPU 两下)。用于定位"某些页 GPU 合成全白"——对比能渲染的页与全白页的层树差异。
    std::vector<std::string> diagUrls;
    try {
        std::wstring d = LocalStateDir();
        if (!d.empty()) {
            std::ifstream f(WideToUtf8(d) + "\\autodiag.txt", std::ios::binary);
            std::string line;
            while (std::getline(f, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
                    line.pop_back();
                if (!line.empty() && line[0] != '#')
                    diagUrls.push_back(line);
            }
        }
    } catch (...) {}
    // (诊断用)autodiag.txt 存在时才跑;正常浏览不受影响。需要再抓时临时放 autodiag.txt(每行一个 URL,DESKTOP: 前缀=桌面 UA)。
    if (!diagUrls.empty()) {
        CoreDispatcher^ disp = this->Dispatcher;
        Platform::Agile<MainPage^> self(this);
        WebEngine::instance().post([disp, self, diagUrls]() {
            std::string dump;
            int gi = -999;
            try { gi = WebCoreGpuInit(nullptr, kW, kH); } catch (...) { gi = -1000; }
            dump += "WebCoreGpuInit(offscreen) rc=" + std::to_string(gi) + "\n\n";
            auto rgba = std::vector<uint8_t>((size_t)kW * kH * 4, 0);
            std::wstring dd = LocalStateDir();
            int idx = 0;
            for (const auto& rawUrl : diagUrls) {
                std::string url = rawUrl;
                bool desktop = false;
                if (url.rfind("DESKTOP:", 0) == 0) { desktop = true; url = url.substr(8); }
                try { WebCoreSetUserAgentMobile(desktop ? 0 : 1); } catch (...) {}
                dump += "########## URL: " + url + (desktop ? " [desktop UA]" : " [mobile UA]") + " ##########\n";
                int lrc = -999;
                try { lrc = WebCoreSessionLoad(url.c_str(), kW, kH, rgba.data()); } catch (...) { lrc = -1000; }
                dump += "SessionLoad rc=" + std::to_string(lrc) + "\n";
                std::vector<char> dg(4096, 0);
                try { WebCoreGetDiag(dg.data(), (int)dg.size()); } catch (...) {}
                dump += "diag: " + std::string(dg.data()) + "\n";
                std::vector<char> li(65536, 0);
                try { WebCoreGpuLayerInfo(li.data(), (int)li.size()); } catch (...) {}
                dump += std::string(li.data());
                dump += "\n\n";
                // cookie 持久化调试:document.cookie 快照(诊断 jar 是否收到/带上了 Set-Cookie)。
                std::vector<char> ck(2048, 0);
                try { WebCoreEvalJS("document.cookie", ck.data(), (int)ck.size()); } catch (...) {}
                dump += "document.cookie: [" + std::string(ck.data()) + "]\n\n";
                // 落盘这页 GPU readback 的实际帧(BMP),供 WDP 拉回当截图看
                try { if (!dd.empty()) WriteBmp32(WideToUtf8(dd) + "\\shot_" + std::to_string(idx) + ".bmp", rgba.data(), kW, kH); } catch (...) {}
                ++idx;
            }
            // cookie 持久化调试:主动落盘(平时靠切后台 VisibilityChanged 触发;autodiag 不经 UI 生命周期,
            // 这里显式补一次,让"设 cookie→跑 autodiag→杀进程→重跑另一份 autodiag 验证读回"这套测试闭环成立)。
            try { WebCoreFlushCookiesToDisk(); dump += "WebCoreFlushCookiesToDisk() done.\n"; } catch (...) {}
            // Apotheosis (M4): autodiag 不经 UI 生命周期,显式把性能日志环形缓冲落到 perf.csv,
            // 供 WDP 一并拉回(仅当 LocalState\perf.txt 开了开关时才有内容)。
            try { WebCorePerfFlush(); dump += "WebCorePerfFlush() done.\n"; } catch (...) {}
            try {
                std::wstring d2 = LocalStateDir();
                if (!d2.empty()) {
                    std::ofstream f(WideToUtf8(d2) + "\\autodump.txt", std::ios::binary | std::ios::trunc);
                    if (f) f.write(dump.data(), dump.size());
                }
            } catch (...) {}
            try {
                disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    s->TitleText->Text = ref new Platform::String(L"AUTODIAG DONE");
                }));
            } catch (...) {}
        });
    } else {
        std::wstring firstUrl = testUrl.empty() ? g_homeUrl : testUrl;   // 主页(设置可改)/ 测试钩子
        // Apotheosis (M4): 启动只加载一次页面。原流程 = 软件加载首页 → OnNavDone 自动 EnableGpu →
        //   EnableGpu 成功后重载同一页(引擎侧 setAcceleratedCompositingEnabled/setForceCompositingMode
        //   只在 buildSession 里按 g_gpuActive 生效,见 port\WebCoreDriver.cpp:1357-1358,故会话建好后
        //   无法追加合成),真机上每次启动白花 ~14 s / ~300 MB。
        //   改为:面板就绪 → 先 WebCoreGpuInit → 再发第一次导航,首个会话就带合成。GpuInit 失败时
        //   g_gpuActive 仍为 false → 同一次导航照旧走 Cairo 软件路径(零回归)。
        //   about:home 不走这条:它无会话/无合成,且直呈现下 PresentSoftwareFrame 会跳过贴图 → 保持原行为。
        if (m_gpuDefault && !firstUrl.empty() && firstUrl != L"about:home") {
            m_pendingFirstNav = firstUrl;
            m_pendingFirstNavPush = true;
            // 真机实测(第一版):GpuPanel 的 Loaded 从未到达 → 启动落到兜底定时器 → 又变回两次加载。
            //   原因:Visibility=Collapsed 的元素不参与 measure/arrange,既不 Loaded 也不 SizeChanged。
            //   故这里就把面板设为可见(第一帧 GPU 内容前它是透明的,盖在 RenderImage 上不影响软件路径)。
            //   面板可见/挂 SizeChanged/等真实尺寸都交给 HookGpuPanelForStartup(2026-09-03 崩溃修复,
            //   见其定义处的注释)——不再在这里直接起 GPU。
            // 触发源取先到者(StartupGpuThenNav 自带去重):页面 Loaded 一定会来(Page 是可见根),
            //   面板首次非零 SizeChanged / 面板 Loaded 通常更早,但都要先确认面板尺寸是真的。
            this->Loaded += ref new Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnPageLoadedForGpu);
            // 兜底:6 s 内一个触发源都没来 → 照原路发导航(软件首屏,GPU 仍由 OnNavDone 的自动开关
            // 接手 = 老的两次加载)——绝不让启动停在"没有任何页面"。
            ArmStartupNavTimer();
            HookGpuPanelForStartup();
        } else {
            NavigateTo(ref new String(firstUrl.c_str()), true);
        }
    }
}

// Apotheosis (review 2026-09-03): DISPLAY toggle "Hide navigation bar". On a phone the software
//   back/Windows/search bar owns the bottom strip of the screen; SuppressSystemOverlays hands that
//   strip to the app (the user swipes up from the bottom edge to get the bar back for a moment).
//   Not TryEnterFullScreenMode — that would also take away the status bar with the clock, which is
//   exactly what the Edge-style top treatment above wants to keep.
//   The property is phone-only (and was added after 10240), so it is both ApiInformation-guarded
//   and wrapped: on a desktop/build without it the toggle simply does nothing.
//   Hiding/showing the bar moves ApplicationView::VisibleBounds → VisibleBoundsChanged → the bottom
//   inset ApplyViewInsets() computes drops to 0 on its own (and comes back while the bar is up).
//   ApplyViewInsets() is still called here so the freed strip is used in the same frame.
//   UI THREAD ONLY.
void MainPage::ApplyHideNavBarSetting()
{
    try {
        if (!Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                L"Windows.UI.ViewManagement.ApplicationView", L"SuppressSystemOverlays"))
            return;
        auto view = Windows::UI::ViewManagement::ApplicationView::GetForCurrentView();
        if (view == nullptr) return;
        // C4973: the SDK marks SuppressSystemOverlays deprecated in favour of TryEnterFullScreenMode.
        //   Not applicable here — full-screen mode would also take the status bar (clock) away, and
        //   keeping that is the whole point of the Edge-style top treatment. Deliberate, so silenced.
#pragma warning(push)
#pragma warning(disable: 4973)
        view->SuppressSystemOverlays = m_hideNavBar;
#pragma warning(pop)
    } catch (...) { return; }
    ApplyViewInsets();
}

// The switch takes effect while the settings page is still open (the bar disappears under it), so
//   the user can see what the toggle does. HideSettings() re-reads it anyway and persists it.
void MainPage::OnHideNavBarToggled(Platform::Object^, RoutedEventArgs^)
{
    if (!SetHideNavBarSwitch) return;
    if (m_hideNavBar == SetHideNavBarSwitch->IsOn) return;
    m_hideNavBar = SetHideNavBarSwitch->IsOn;
    ApplyHideNavBarSetting();
}

// Apotheosis (review 2026-09-03): move the URL suggestion dropdown together with the nav bar when
//   the soft keyboard comes up. SuggestPanel lives in the content row (bottom-anchored) while
//   NavBarShift only covers the bottom chrome, so it needs its own translation by the same Y.
//   Created lazily and kept on the element — y == 0 restores the resting position.
//   UI THREAD ONLY.
void MainPage::ShiftSuggestPanel(double y)
{
    if (!SuggestPanel) return;
    auto t = dynamic_cast<Windows::UI::Xaml::Media::TranslateTransform^>(SuggestPanel->RenderTransform);
    if (t == nullptr) {
        if (y == 0.0) return;   // nothing to restore
        t = ref new Windows::UI::Xaml::Media::TranslateTransform();
        SuggestPanel->RenderTransform = t;
    }
    t->Y = y;
}

// Apotheosis (review 2026-09-03): with ApplicationViewBoundsMode::UseCoreWindow our window covers
//   the whole screen, so the shell's status bar (top) and software navigation bar (bottom) sit on
//   top of our chrome. ApplicationView::VisibleBounds is what the shell leaves usable; the gap
//   against CoreWindow::Bounds is the inset we owe on each edge. Both rects are in the same DIP
//   space, so plain arithmetic is enough.
//   bottom → RootGrid padding, so the status line + address/nav row ride above the buttons;
//   top    → top margin/padding of everything anchored to the top edge (loading bar, find bar) and
//            of the full-screen overlays, whose own headers would otherwise sit under the clock.
//   UI THREAD ONLY. Cheap and idempotent — safe to call from every event that may change the edges.
void MainPage::ApplyViewInsets()
{
    double top = 0.0, bottom = 0.0;
    try {
        auto view = Windows::UI::ViewManagement::ApplicationView::GetForCurrentView();
        auto win = Windows::UI::Core::CoreWindow::GetForCurrentThread();
        if (view == nullptr || win == nullptr) return;
        Windows::Foundation::Rect vb = view->VisibleBounds;
        Windows::Foundation::Rect wb = win->Bounds;
        if (!(vb.Height > 0.0f) || !(wb.Height > 0.0f)) return;
        top = (double)vb.Y - (double)wb.Y;
        bottom = ((double)wb.Y + (double)wb.Height) - ((double)vb.Y + (double)vb.Height);
    } catch (...) { return; }
    if (!(top > 0.0)) top = 0.0;
    if (!(bottom > 0.0)) bottom = 0.0;
    Windows::UI::Xaml::Thickness topPad(0, top, 0, 0);
    if (Progress) Progress->Margin = topPad;
    if (FindBar) FindBar->Margin = topPad;
    if (Drawer) Drawer->Padding = topPad;
    if (SettingsPage) SettingsPage->Padding = topPad;
    if (TabSwitcher) TabSwitcher->Padding = topPad;
    if (OobePanel) OobePanel->Padding = topPad;
    if (RootGrid) RootGrid->Padding = Windows::UI::Xaml::Thickness(0, 0, 0, bottom);
}

// ---- 持久化 ----
static std::vector<Entry> ReadEntries(const std::wstring& path)
{
    std::vector<Entry> out;
    std::ifstream f(WideToUtf8(path), std::ios::binary);
    if (!f) return out;
    std::stringstream ss; ss << f.rdbuf();
    std::string all = ss.str();
    std::wstring w = Utf8ToWide(all);
    std::wstringstream ws(w);
    std::wstring line;
    while (std::getline(ws, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        Entry e;
        size_t t1 = line.find(L'\t');
        size_t t2 = (t1 == std::wstring::npos) ? std::wstring::npos : line.find(L'\t', t1 + 1);
        if (t1 == std::wstring::npos) { e.url = line; }
        else {
            e.url = line.substr(0, t1);
            if (t2 == std::wstring::npos) e.title = line.substr(t1 + 1);
            else { e.title = line.substr(t1 + 1, t2 - t1 - 1); e.extra = line.substr(t2 + 1); }
        }
        out.push_back(e);
    }
    return out;
}
static void WriteEntries(const std::wstring& path, const std::vector<Entry>& v)
{
    std::wstring w;
    for (auto& e : v) { w += e.url; w += L'\t'; w += e.title; w += L'\t'; w += e.extra; w += L'\n'; }
    std::ofstream f(WideToUtf8(path), std::ios::binary | std::ios::trunc);
    if (f) { std::string u = WideToUtf8(w); f.write(u.data(), u.size()); }
}

void MainPage::LoadData()
{
    std::wstring d = LocalStateDir();
    if (d.empty()) return;
    m_bookmarks = ReadEntries(d + L"\\bookmarks.tsv");
    m_historyList = ReadEntries(d + L"\\history.tsv");
    m_downloads = ReadEntries(d + L"\\downloads.tsv");
}
void MainPage::SaveBookmarks() { std::wstring d = LocalStateDir(); if (!d.empty()) WriteEntries(d + L"\\bookmarks.tsv", m_bookmarks); }
void MainPage::SaveHistory()   { std::wstring d = LocalStateDir(); if (!d.empty()) WriteEntries(d + L"\\history.tsv", m_historyList); }
void MainPage::SaveDownloads() { std::wstring d = LocalStateDir(); if (!d.empty()) WriteEntries(d + L"\\downloads.tsv", m_downloads); }

void MainPage::AddHistory(const std::wstring& url, const std::wstring& title)
{
    if (url.empty() || url == L"about:home") return;
    m_historyList.erase(std::remove_if(m_historyList.begin(), m_historyList.end(),
        [&](const Entry& e) { return e.url == url; }), m_historyList.end());
    Entry e; e.url = url; e.title = title.empty() ? url : title;
    m_historyList.insert(m_historyList.begin(), e);
    if (m_historyList.size() > 300) m_historyList.resize(300);
    SaveHistory();
}
bool MainPage::IsBookmarked(const std::wstring& url)
{
    for (auto& b : m_bookmarks) if (b.url == url) return true;
    return false;
}

// ---- 导航 ----
void MainPage::UpdateNavButtons()
{
    BackBtn->IsEnabled = (m_navIndex > 0);
    FwdBtn->IsEnabled = (m_navIndex >= 0 && m_navIndex < (int)m_navStack.size() - 1);
}
void MainPage::SetLoading(bool loading)
{
    m_loading = loading;
    Progress->IsIndeterminate = loading;
    Progress->Visibility = loading ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    UpdateUrlActionGlyph();   // 加载态切到 ✕ 停止 / 结束回 → 或 ⟳
}

void MainPage::NavigateTo(Platform::String^ url, bool pushHistory)
{
    if (m_loading) return;

    std::wstring wurl = url ? std::wstring(url->Data()) : L"about:home";
    const bool isHome = wurl.empty() || wurl == L"about:home";

    // Apotheosis (M4): GPU 优先 —— 本次会话的第一次网络导航,先起 GPU 再加载,首个会话就带合成
    //   (引擎侧 setAcceleratedCompositingEnabled/setForceCompositingMode 只在 buildSession 里按
    //   g_gpuActive 生效,见 port\WebCoreDriver.cpp:1357-1358 → 会话建好后无法追加合成,原流程只能
    //   "软件加载一遍 + 开 GPU 后重载一遍",真机每次 ~14 s / ~300 MB 白工)。
    //   这里拦截而不是只在启动时拦截:主页是 about:home 时,第一次网络导航来自用户输入/点击。
    //   about:home 保持软件路径(无会话/无合成)。GpuInit 失败 → g_gpuActive 仍 false → 同一次导航
    //   照旧走 Cairo(零回归)。EnableGpu 的回调(成功或失败)负责把这次导航发出去。
    if (!isHome && m_gpuDefault && !m_gpuOn && !m_gpuAutoTried && !m_gpuStartupBegun) {
        m_pendingFirstNav = wurl;
        m_pendingFirstNavPush = pushHistory;
        HideSuggestions();
        SetLoading(true);      // GpuInit 期间(几百 ms)显示进度条,并挡住重复点/回车(m_loading 早退)
        // Apotheosis (2026-09-03 崩溃修复): 这条路径是新装后"首次输入网址回车"的实际触发点——此时
        //   about:home 从未进过上面构造期的 GPU-first 分支,GpuPanel 还是 Collapsed/从未 arrange 过。
        //   原代码在这里直接把面板设 Visible 就同步 StartupGpuThenNav() 起 GPU,面板此刻尺寸仍是
        //   0x0(真机 mem.txt: "startup gpu-first (panel 0x0) ... panelLoaded=1"),ANGLE 绑定的
        //   SwapChainPanel 还没走完首次 arrange;紧接着面板真正的第一次 SizeChanged 就会在
        //   libGLESv2.dll 里空指针崩溃(SEH fault=0,UI 线程内 XAML SwapChainPanel 回调 → ANGLE)。
        //   改走 HookGpuPanelForStartup:等面板报出真实非零尺寸再起 GPU,~2s 等不到才照老行为起。
        HookGpuPanelForStartup();
        return;
    }

    m_currentUrl = isHome ? L"about:home" : wurl;

    // M4:导航=新页面,引擎 pageScaleFactor 复位 1.0 → harness 缩放状态/显示变换同步复位(否则下次捏合基准错)。
    m_pinching = false; m_liveScale = 1.0f; m_pageScale = 1.0f;
    m_zoomTransform = nullptr;
    // Apotheosis (instant pan): a new document invalidates the cached scroll position and bounds,
    // and any pan remainder belongs to the page we are leaving.
    m_panRemX = 0; m_panRemY = 0;
    if (m_panTranslate != nullptr) { m_panTranslate->X = 0.0; m_panTranslate->Y = 0.0; }
    if (m_panSnapTimer) m_panSnapTimer->Stop();
    m_scrollStateValid = false;
    ++m_scrollStateGen;
    if (m_presentGroup != nullptr) m_presentGroup->Children->Clear();
    if (GpuPanel) GpuPanel->RenderTransform = nullptr;
    if (RenderImage) RenderImage->RenderTransform = nullptr;

    if (pushHistory) {
        if (m_navIndex >= 0 && m_navIndex < (int)m_navStack.size() - 1)
            m_navStack.erase(m_navStack.begin() + m_navIndex + 1, m_navStack.end());
        m_navStack.push_back(wurl);
        m_navIndex = (int)m_navStack.size() - 1;
    }
    UpdateNavButtons();
    UpdateLockIcon();
    HideSuggestions();
    m_urlSyncing = true;
    UrlBox->Text = isHome ? ref new String(L"") : url;
    m_urlSyncing = false;
    TitleText->Text = isHome ? L8(L"主页", L"Home") : ref new String(((g_lang == L"en" ? L"Loading  " : L"加载中  ") + wurl).c_str());
    SetLoading(true);

    // 加载看门狗:即使完成回调因 dispatcher 断开/低内存而丢失,40s 后也强制复位 m_loading,
    // 避免导航永久锁死(代码审查确认的真实 hang)。引擎侧 30s 看门狗保证 job 必返回,UI 侧 40s 兜底。
    if (!m_loadWatchdog) {
        m_loadWatchdog = ref new Windows::UI::Xaml::DispatcherTimer();
        Windows::Foundation::TimeSpan ts; ts.Duration = 40LL * 10000000LL;   // 40s(100ns 单位)
        m_loadWatchdog->Interval = ts;
        m_loadWatchdog->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(this, &MainPage::OnLoadWatchdog);
    }
    m_loadWatchdog->Start();

    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    std::string surl = ToUtf8(url);
    unsigned long long mySeq = ++m_opSeq;
    // 主页:用当前书签/历史动态生成新标签页(速拨磁贴=<a>,渲染时提取进链接表→点击导航)。
    std::string homeHtml = isHome ? BuildHomeHtml(m_bookmarks, m_historyList) : std::string();

    WebEngine::instance().post([disp, self, surl, isHome, homeHtml, mySeq]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        bool loadOk = false;   // 网络加载是否真成功(区别于错误页渲染成功),决定是否进历史
        bool sessionActive = false;   // 是否建立了引擎常驻会话(决定点击转发/翻页按钮)
        int comp = 0;                 // 本页合成是否在跑(根图层已附)→ UI 侧据此选 GPU 面/软件面
        std::wstring title;
        try {
            if (isHome) {
                WebCoreCloseSession();   // 离开网络页:销毁会话,释放 Page + 取消在途加载
                rc = WebCoreRenderHtml(homeHtml.c_str(), kW, kH, rgba->data());
                loadOk = (rc == 0);
                title = W8(L"主页", L"Home");
            } else {
                WriteStage(("before-load " + surl).c_str());
                WriteMemLog("before-load url=" + surl + " " + MemSnapshot() + EngineMemStats());   // Apotheosis (M4)
                int netRc = WebCoreSessionLoad(surl.c_str(), kW, kH, rgba->data());   // 常驻会话加载
                // Apotheosis (M4): peak right after the load, still before title/diag/compositing
                // queries — if the OS kills us in those, mem.txt already carries the number.
                WriteMemLog("mem-loading url=" + surl + " " + MemSnapshot());
                char t[512] = ""; WebCoreGetTitle(t, sizeof t);
                char diag[4096] = ""; WebCoreGetDiag(diag, sizeof diag);
                char err[512] = ""; WebCoreGetLastError(err, sizeof err);   // curl 错误码+描述(失败时)
                try { comp = WebCoreEnableCompositing(); } catch (...) {}   // M1 验证:合成是否在跑(根图层已附)
                // 失败原因也写进 stage.txt(原来只进错误页,拉不到)→ 远程诊断"加载失败"必看。
                WriteStage(("after-load url=" + surl + " rc=" + std::to_string(netRc)
                            + " compositing=" + std::to_string(comp)
                            + "\nERR: " + err + "\ndiag: " + diag).c_str());
                WriteMemLog("after-load url=" + surl + " rc=" + std::to_string(netRc)
                            + " " + MemSnapshot() + EngineMemStats());   // Apotheosis (M4)
                SampleMemoryPressure();   // Apotheosis: a load is where the level actually moves
                if (netRc == 0) {
                    rc = 0;
                    loadOk = true;
                    sessionActive = true;
                    title = ToWide(t);
                    if (title.empty()) title = Utf8ToWide(surl);
                } else {
                    std::string eh = MakeErrorHtml(surl, err);
                    rc = WebCoreRenderHtml(eh.c_str(), kW, kH, rgba->data());   // 渲染错误页(会话已被引擎清理)
                    loadOk = false;
                    title = W8(L"加载失败", L"Load failed");
                }
            }
        } catch (...) { rc = -1000; loadOk = false; title = W8(L"渲染异常", L"Render error"); }

        // 取链接命中表(渲染时已提取到驱动 g_links,这里在引擎线程读出)。
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        try {
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) {
                int lx = 0, ly = 0, lw = 0, lh = 0; char lu[1200] = "";
                if (WebCoreGetLink(i, &lx, &ly, &lw, &lh, lu, sizeof lu)) {
                    Harness::PageLink pl; pl.x = lx; pl.y = ly; pl.w = lw; pl.h = lh; pl.url = Utf8ToWide(lu);
                    links->push_back(std::move(pl));
                }
            }
        } catch (...) {}

        auto titleCopy = std::make_shared<std::wstring>(title);
        bool ok = (rc == 0);   // 渲染是否成功(决定是否贴图)
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, rgba, titleCopy, ok, loadOk, sessionActive, comp, links, mySeq]() {
                    MainPage^ s = self.Get();
                    if (!s) return;
                    if (s->m_opSeq != mySeq) return;   // 已被更新操作/看门狗取代,丢弃此迟到回调
                    if (ok) {
                        // Apotheosis (M4): 主页/错误页是纯软件渲染(无会话 → 无合成图层树,paintToRGBA
                        //   落回 cairo 并填满 rgba)。直呈现模式下 PresentSoftwareFrame 自跳过贴图,画面
                        //   就会停在没有内容的 GpuPanel 上 —— GPU 优先启动后首屏本身就可能是错误页(离线
                        //   启动),故按本帧的会话状态切显示面:无会话→软件面(RenderImage),有会话→GPU 面。
                        //   引擎侧不受影响(g_directPresent 只 gate harness 的 BlitToBitmap)。
                        //   判据用本页实际合成状态(comp=根图层已附),而不是"有会话":GpuInit 恰好在
                        //   这次加载途中完成时,会话是无合成建起来的 → 引擎按 cairo 填了 rgba,必须走软件面。
                        //   ★ GpuInit 成功后绝不再 Collapse GpuPanel:折叠 = 面板变 0×0,ANGLE 会经面板
                        //   dispatcher 重建/缩放交换链,按作者的线程约定那条路会 std::terminate(真机上
                        //   开第二个标签即崩)。改用 Opacity=0 隐藏 —— 面板留在树里、尺寸不变、不绘制,
                        //   下层 RenderImage 直接透出来(GpuPanel 本来就 IsHitTestVisible=False,不挡点击)。
                        if (s->m_gpuOn && (sessionActive && comp != 0) != s->m_gpuPresent) {
                            bool present = (sessionActive && comp != 0);
                            s->m_gpuPresent = present;
                            g_directPresent.store(present);
                            s->GpuPanel->Opacity = present ? 1.0 : 0.0;
                            s->GpuPanel->IsHitTestVisible = false;
                            s->RenderImage->Visibility = present
                                ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible;
                        }
                        s->PresentSoftwareFrame(rgba);
                        s->m_pageLinks = *links;   // 存当前页链接表供点击命中
                    }
                    s->m_sessionActive = sessionActive;
                    s->UpdateScrollFab();
                    // 实时渲染:有会话则启动(让动画动、SPA 渐进挂载);无会话(主页/错误页)停。
                    s->m_lastFrameHash = 0;
                    if (sessionActive) s->StartLiveMode(); else s->StopLiveMode();
                    s->OnNavDone(ref new String(titleCopy->c_str()), ok, loadOk);
                }));
        } catch (...) {
            // RunAsync 抛了(dispatcher 断开/低内存):OnNavDone 不会跑,m_loading 靠 UI 看门狗复位。
        }
    });
}

void MainPage::OnNavDone(Platform::String^ finalTitle, bool ok, bool loadOk)
{
    if (m_loadWatchdog) m_loadWatchdog->Stop();
    // Apotheosis: 新会话的第一帧已经呈现(GPU 面已 swapBuffers / 软件面已贴图,见调用点),
    //   切换快照的占位使命结束 —— 上面的切面逻辑已按本页真实状态设好 m_gpuPresent。
    HideTabSnapshot();
    m_currentTitle = finalTitle ? std::wstring(finalTitle->Data()) : L"";
    TitleText->Text = (m_currentTitle.empty() ? ref new String(L"EdgeHTML Reborn") : finalTitle);
    if (loadOk && m_currentUrl != L"about:home")   // 仅真正加载成功才记历史,失败不污染
        AddHistory(m_currentUrl, m_currentTitle);
    if (m_currentUrl != L"about:home") {
        m_urlSyncing = true;
        UrlBox->Text = ref new String(m_currentUrl.c_str());
        m_urlSyncing = false;
    }
    UpdateLockIcon();
    SetLoading(false);
    UpdateNavButtons();
    // 启动静默自检更新:首个网络页加载成功后跑一次(此时 CA blob 已注入,WebCoreDownload 才能过 TLS);
    //   有新版才提示,无更新/失败静默。manual 检查在设置里按钮。
    if (m_updateAuto && !m_updateAutoChecked && loadOk && m_currentUrl != L"about:home") {
        m_updateAutoChecked = true;
        CheckForUpdate(false);
    }
    // 默认 GPU:首个网络页加载完、开关开 → 自动开 GPU(EnableGpu 成功会重载本页,届时再走一遍 OnNavDone)。
    if (m_gpuDefault && !m_gpuOn && !m_gpuAutoTried && m_sessionActive && m_currentUrl != L"about:home") {
        m_gpuAutoTried = true;
        EnableGpu();
        return;   // 缩放在重载后的 OnNavDone 应用,避免双重栅格
    }
    // 默认缩放:有会话且默认非 100% 时,按新尺度重栅格(复用捏合提交路径)。
    if (m_sessionActive && m_defaultZoom != 100)
        PinchCommit(m_defaultZoom / 100.0f, kW / 2, kH / 2);
    (void)ok;
}

void MainPage::OnLoadWatchdog(Platform::Object^, Platform::Object^)
{
    if (m_loadWatchdog) m_loadWatchdog->Stop();
    HideTabSnapshot();   // Apotheosis: 完成回调丢了也不能让切换快照永久占着画面
    if (m_loading || m_interacting) {   // 完成回调丢失,强制复位以恢复导航/交互
        ++m_opSeq;                      // 作废这次超时操作的迟到回调,使其回 UI 时被丢弃
        m_interacting = false;
        // 会话状态不可知(加载可能半途):退到无会话,隐藏翻页按钮。下次点击若引擎仍有会话会自洽;
        // 没有则返回 -12/-14,已处理。避免停在"以为有会话"却点不动的状态。
        m_sessionActive = false;
        ScrollFab->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        TitleText->Text = L8(L"加载超时", L"Load timed out");
        SetLoading(false);
        UpdateNavButtons();
    }
}

// 网页点击:软件帧与 GPU surface 都会铺满 ContentArea，故统一由显示坐标映回固定引擎视口。
//  有会话(网络页):转发到引擎 WebCoreClickAt,经真实命中测试 + 默认动作(链接/表单/按钮 onclick/SPA)。
//  无会话(主页/错误页):退回链接命中表导航。
// 把内容区显示坐标(DIP)映回引擎像素空间(kW×kH)。
//
// Apotheosis: map against the layer that actually SHOWS the engine output, not against the one
//   that receives the gesture. The caller hands us ContentArea DIPs, but in direct-present mode
//   the 720x1080 surface is stretched over GpuPanel, and GpuPanel spans the whole content row
//   while ContentArea sits 6 DIP inside it (Border Margin="6,6,6,0"). Mapping ContentArea DIPs
//   with ContentArea's own size therefore misses on both counts — a missing 6 DIP origin shift
//   and a scale factor off by 12/width — for a hit about 1.7 % away from the finger, growing
//   towards the right and bottom edges. Small, but it is exactly what makes a link at the edge
//   of a dense page refuse to open. PresentLayer() is the same layer SetPinchAnchor pins to, so
//   taps and the pinch anchor now agree on one coordinate space.
void MainPage::MapTapToEngine(double dipX, double dipY, int& outPx, int& outPy)
{
    if (dipX < 0.0 || dipY < 0.0) {
        outPx = -1;
        outPy = -1;
        return;
    }
    double lx = dipX, ly = dipY;
    double aw = ContentArea->ActualWidth, ah = ContentArea->ActualHeight;
    auto layer = PresentLayer();
    if (layer != nullptr && layer != static_cast<Windows::UI::Xaml::FrameworkElement^>(ContentArea)) {
        try {
            auto tv = ContentArea->TransformToVisual(layer);
            auto p = tv->TransformPoint(Windows::Foundation::Point((float)dipX, (float)dipY));
            lx = p.X; ly = p.Y;
            if (layer->ActualWidth > 1.0 && layer->ActualHeight > 1.0) {
                aw = layer->ActualWidth; ah = layer->ActualHeight;
            }
        } catch (...) { lx = dipX; ly = dipY; }   // no visual relation yet: fall back to ContentArea
    }
    dipX = lx; dipY = ly;
    if (aw > 1.0 && ah > 1.0) {
        outPx = static_cast<int>(dipX * static_cast<double>(kW) / aw + 0.5);
        outPy = static_cast<int>(dipY * static_cast<double>(kH) / ah + 0.5);
    } else {
        outPx = static_cast<int>(dipX + 0.5);
        outPy = static_cast<int>(dipY + 0.5);
    }
}

void MainPage::OnPageTapped(Platform::Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    HideSuggestions();   // 点页面即收起地址栏建议下拉(否则只能靠导航/清空关 → "关不掉")
    if (m_loading || m_interacting) return;
    // 取相对 ContentArea(承接手势/点击的层,始终参与布局)的坐标。★ 不能用 RenderImage:直呈现模式下它被
    //   Collapsed(让位给 GpuPanel),对已塌缩元素 GetPosition 坐标无效 → 点击错位(滚动后点底部却命中顶部)。
    //   ContentArea 左上角 = 渲染视口原点,故二者在软件模式下等价,直呈现模式下正确。
    auto pt = e->GetPosition(ContentArea);
    int px, py; MapTapToEngine(pt.X, pt.Y, px, py);
    if (px < 0 || py < 0 || px >= kW || py >= kH) return;

    // 有会话:一律转发引擎真实点击。引擎命中测试是权威的——正确处理弹窗/遮罩层(z-order)、按钮、表单、
    // 以及链接(锚点默认动作=导航)。链接表感知不到模态层覆盖,故不再"链接表优先"(否则点模态关闭按钮
    // 会被误判成点中被它盖住的下层链接 → 弹窗关不掉)。ForwardClickToEngine 内含链接表兜底。
    if (m_sessionActive) {
        ForwardClickToEngine(px, py);
        return;
    }
    // 无会话(主页/错误页):链接表命中导航。
    for (auto it = m_pageLinks.rbegin(); it != m_pageLinks.rend(); ++it) {
        const PageLink& l = *it;
        if (px >= l.x && px < l.x + l.w && py >= l.y && py < l.y + l.h) {
            NavigateTo(ref new String(l.url.c_str()), true);
            return;
        }
    }
}

// 把 (px,py) 点击转发给引擎活会话。引擎派发真实鼠标事件并处理默认动作;若触发了会话内导航
// (URL 变化),回 UI 后同步地址栏/前进后退栈/历史。引擎点击失败但命中了链接表 → 退回经典导航。
void MainPage::ForwardClickToEngine(int px, int py)
{
    if (m_interacting) return;
    m_interacting = true;
    SetLoading(true);
    if (m_loadWatchdog) m_loadWatchdog->Start();   // 兜底:若引擎/回调卡死,40s 强制复位
    TitleText->Text = L8(L"处理中…", L"Working…");

    // 链接表命中(供引擎点击失败时退回经典导航)
    auto linkHit = std::make_shared<std::wstring>();
    for (auto it = m_pageLinks.rbegin(); it != m_pageLinks.rend(); ++it) {
        const PageLink& l = *it;
        if (px >= l.x && px < l.x + l.w && py >= l.y && py < l.y + l.h) { *linkHit = l.url; break; }
    }

    std::wstring prevUrl = m_currentUrl;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = ++m_opSeq;

    WebEngine::instance().post([disp, self, px, py, linkHit, prevUrl, mySeq]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        unsigned hashBefore = WebCoreGetFrameHash();
        try { rc = WebCoreClickAt(px, py, rgba->data()); } catch (...) { rc = -1000; }
        unsigned hashAfter = (rc == 0) ? WebCoreGetFrameHash() : hashBefore;
        bool changed = (hashAfter != hashBefore);   // 引擎点击是否改变了画面(区分模态关闭/按钮 vs 死链接)
        int editable = 0;
        try { if (rc == 0) editable = WebCoreFocusedEditable(); } catch (...) {}   // 点中的是不是输入框

        std::wstring navUrl, title;
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        if (rc == 0) {
            char t[512] = ""; WebCoreGetTitle(t, sizeof t); title = ToWide(t);
            char u[1024] = ""; WebCoreGetUrl(u, sizeof u);
            std::wstring newUrl = ToWide(u);
            if (!newUrl.empty() && newUrl != prevUrl) navUrl = newUrl;   // 会话内发生了导航
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) {
                int lx=0,ly=0,lw=0,lh=0; char lu[1200]="";
                if (WebCoreGetLink(i,&lx,&ly,&lw,&lh,lu,sizeof lu)) {
                    Harness::PageLink pl; pl.x=lx; pl.y=ly; pl.w=lw; pl.h=lh; pl.url=Utf8ToWide(lu);
                    links->push_back(std::move(pl));
                }
            }
        }
        auto titleW = std::make_shared<std::wstring>(title);
        auto navW = std::make_shared<std::wstring>(navUrl);
        int rcCopy = rc;
        bool changedCopy = changed;
        int editableCopy = editable;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, rgba, titleW, navW, links, rcCopy, changedCopy, editableCopy, linkHit, mySeq]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    if (s->m_opSeq != mySeq) return;   // 已被取代/看门狗复位,丢弃迟到回调
                    s->m_interacting = false;
                    if (s->m_loadWatchdog) s->m_loadWatchdog->Stop();
                    if (rcCopy == 0) {
                        Platform::String^ title = ref new String(titleW->c_str());
                        Platform::String^ navUrl = navW->empty() ? nullptr : ref new String(navW->c_str());
                        // 引擎点击没导航、画面也没变、却命中了链接表 → 引擎可能没触发锚点默认动作,经典导航兜底。
                        // 模态关闭/按钮等会改变画面(changedCopy=true)→ 不兜底,信任引擎结果。
                        if (navW->empty() && !changedCopy && !linkHit->empty()) {
                            s->SetLoading(false);
                            s->NavigateTo(ref new String(linkHit->c_str()), true);
                        } else {
                            s->ApplyEngineFrame(rgba, title, navUrl, links);
                            s->SetLoading(false);
                            // 未导航(in-page):点中可编辑元素则唤起键盘,否则收起。导航了则收起。
                            if (navW->empty()) { if (editableCopy) s->OpenKeyboard(); else s->CloseKeyboard(); }
                            else s->CloseKeyboard();
                        }
                    } else if (!linkHit->empty()) {
                        s->SetLoading(false);   // 先清 m_loading 否则 NavigateTo 早退
                        s->NavigateTo(ref new String(linkHit->c_str()), true);   // 退回经典导航
                    } else {
                        // 会话丢失(-12 无会话 / -14 帧丢失):清状态、隐藏翻页按钮,避免后续点击空转
                        if (rcCopy == -12 || rcCopy == -14) {
                            s->m_sessionActive = false;
                            s->ScrollFab->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                        }
                        s->SetLoading(false);
                        s->TitleText->Text = ref new String(s->m_currentTitle.empty() ? L"EdgeHTML Reborn" : s->m_currentTitle.c_str());
                    }
                }));
        } catch (...) {}
    });
}

// 软件模式:把引擎 RGBA 帧贴上 RenderImage。WriteableBitmap 双缓冲复用 —— 原来每帧 ref new 一块
// 3MB XAML 位图(实时 5fps + 拖拽滚动 + 逐键重绘)是 UI 线程分配大头;交替写 A/B 两块,避免 XAML
// 还在上传上一帧纹理时就地改写同一块。直呈现模式(GpuPanel 已由引擎 swapBuffers)无事可做。
void MainPage::PresentSoftwareFrame(const std::shared_ptr<std::vector<uint8_t>>& rgba)
{
    if (m_gpuPresent) return;
    WriteableBitmap^ wb = m_frameBmpFlip ? m_frameBmpB : m_frameBmpA;
    if (!wb) {
        wb = ref new WriteableBitmap(kW, kH);
        if (m_frameBmpFlip) m_frameBmpB = wb; else m_frameBmpA = wb;
    }
    m_frameBmpFlip = !m_frameBmpFlip;
    BlitToBitmap(wb, *rgba, kW, kH);
    wb->Invalidate();
    RenderImage->Source = wb;
}

// 把一帧引擎渲染结果贴到位图 + 同步标题/链接表;navUrl 非空 = 会话内发生导航(同步地址栏/栈/历史)。
void MainPage::ApplyEngineFrame(const std::shared_ptr<std::vector<uint8_t>>& rgba,
                               Platform::String^ title, Platform::String^ navUrl,
                               const std::shared_ptr<std::vector<PageLink>>& links)
{
    PresentSoftwareFrame(rgba);   // present 模式内部自跳过(引擎已 swapBuffers 到 GpuPanel)
    m_pageLinks = *links;
    m_lastFrameHash = 0;        // 强制下一实时帧重贴(交互改了画面)
    StartLiveMode();            // 交互后重启实时(可能触发了动画/SPA 更新)
    if (title && title->Length() > 0) {
        m_currentTitle = std::wstring(title->Data());
        TitleText->Text = title;
    }
    if (navUrl != nullptr) {
        std::wstring nu = std::wstring(navUrl->Data());
        m_currentUrl = nu;
        m_urlSyncing = true;
        UrlBox->Text = navUrl;
        m_urlSyncing = false;
        UpdateLockIcon();
        UpdateUrlActionGlyph();
        // 仅当与当前栈顶不同才压栈,避免会话内重定向链/同页微变产生相邻重复项(导致"后退无反应")。
        bool dup = (m_navIndex >= 0 && m_navIndex < (int)m_navStack.size() && m_navStack[m_navIndex] == nu);
        if (!dup) {
            if (m_navIndex >= 0 && m_navIndex < (int)m_navStack.size() - 1)
                m_navStack.erase(m_navStack.begin() + m_navIndex + 1, m_navStack.end());
            m_navStack.push_back(nu);
            m_navIndex = (int)m_navStack.size() - 1;
            UpdateNavButtons();
        }
        AddHistory(m_currentUrl, m_currentTitle);
    }
}

// 引擎滚动(触发懒加载图片/查看下方内容)。dy>0 向下。滚动后位图即新视口,native 滚动复位顶。
void MainPage::EngineScroll(int dy)
{
    if (!m_sessionActive || m_loading || m_interacting) return;
    m_interacting = true;
    SetLoading(true);
    if (m_loadWatchdog) m_loadWatchdog->Start();

    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = ++m_opSeq;
    WebEngine::instance().post([disp, self, dy, mySeq]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        try { rc = WebCoreScrollBy(0, dy, rgba->data()); } catch (...) { rc = -1000; }
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        if (rc == 0) {
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) {
                int lx=0,ly=0,lw=0,lh=0; char lu[1200]="";
                if (WebCoreGetLink(i,&lx,&ly,&lw,&lh,lu,sizeof lu)) {
                    Harness::PageLink pl; pl.x=lx; pl.y=ly; pl.w=lw; pl.h=lh; pl.url=Utf8ToWide(lu);
                    links->push_back(std::move(pl));
                }
            }
        }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, rgba, links, rcCopy, mySeq]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    if (s->m_opSeq != mySeq) return;   // 已被取代/看门狗复位,丢弃迟到回调
                    s->m_interacting = false;
                    if (s->m_loadWatchdog) s->m_loadWatchdog->Stop();
                    s->SetLoading(false);
                    if (rcCopy == 0) {
                        s->PresentSoftwareFrame(rgba);
                        s->m_pageLinks = *links;
                        s->m_lastFrameHash = 0;
                        s->StartLiveMode();   // 滚动后重启实时(新视口的懒加载/动画)
                    }
                }));
        } catch (...) {}
    });
}
// Apotheosis (PRIVACY-AUDIT.md recommended action 4): is the connection we are on unmetered?
//   Anything that is not explicitly Unrestricted (Fixed / Variable / Unknown, or no profile at all)
//   counts as metered, so "Wi-Fi only" errs towards not spending the user's data plan.
static bool ConnectionIsUnmetered()
{
    using namespace Windows::Networking::Connectivity;
    try {
        ConnectionProfile^ profile = NetworkInformation::GetInternetConnectionProfile();
        if (!profile) return false;
        ConnectionCost^ cost = profile->GetConnectionCost();
        if (!cost) return false;
        return cost->NetworkCostType == NetworkCostType::Unrestricted;
    } catch (...) { return false; }
}

// Apotheosis: speculation-rules prefetch = Off / Wi-Fi only / Always (Settings -> PRIVACY).
//   Called from ApplySettings and whenever the network changes; the engine call goes through the
//   engine thread (thread rule: the UI thread never calls WebCore directly).
void MainPage::ApplyPrefetchSetting()
{
    if (m_prefetch < 0 || m_prefetch > 2) m_prefetch = 0;
    int en = (m_prefetch == 2 || (m_prefetch == 1 && ConnectionIsUnmetered())) ? 1 : 0;
    WebEngine::instance().post([en]() { try { WebCoreSetSpeculativePrefetch(en); } catch (...) {} });
}

// Apotheosis: the floating page up/down buttons are a developer aid (they were there to trigger
//   lazy loading before touch scrolling worked). Off unless Settings → DEVELOPER turns them on.
void MainPage::UpdateScrollFab()
{
    if (!ScrollFab) return;
    ScrollFab->Visibility = (m_sessionActive && m_showScrollFab)
        ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
}

void MainPage::OnScrollUp(Platform::Object^, RoutedEventArgs^)   { EngineScroll(-900); }
void MainPage::OnScrollDown(Platform::Object^, RoutedEventArgs^) { EngineScroll(900); }

// ---- 自由滚动:指针拖拽 / 滚轮 → 累积位移 → 合并成引擎滚动(无 spinner,跟手)----
void MainPage::FreeScrollBy(int dx, int dy)
{
    if (!m_sessionActive || (dx == 0 && dy == 0)) return;
    m_scrollAccumX += dx;
    m_scrollAccum += dy;
    if (!m_scrollBusy) PumpScroll();
}
void MainPage::PumpScroll()
{
    if ((m_scrollAccum == 0 && m_scrollAccumX == 0) || !m_sessionActive) { m_scrollBusy = false; return; }
    int dy = m_scrollAccum; m_scrollAccum = 0;
    int dx = m_scrollAccumX; m_scrollAccumX = 0;
    m_scrollBusy = true;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = m_opSeq;   // 不自增:被动滚动不作废点击/导航令牌,但被它们作废(导航后丢弃迟到滚动帧)
    bool present = m_gpuPresent;
    // ★ 提速:滚动期间不再每帧跨 FFI 拷贝链接表(引擎侧也跳过了 extractLinks),present 模式连 WriteableBitmap
    //   都不建(引擎已直呈现到 GpuPanel,BlitToBitmap 本就空转)。链接表在滚动停止后由 SyncLinksAfterScroll 一次性补。
    WebEngine::instance().post([disp, self, dx, dy, mySeq, present]() {
        auto rgba = AcquireEngineBuffer(present);
        int rc = -999;
        try { rc = WebCoreScrollBy(dx, dy, rgba->data()); } catch (...) { rc = -1000; }
        // Apotheosis (instant pan): read back where the engine ended up, in the same engine-thread
        // hop that just scrolled and presented. This is the "a frame was presented" point the pan
        // preview reduces itself at; the engine clamps at the document edges, so what it actually
        // applied is generally NOT the delta we sent — only the position tells the truth. Cheap:
        // WebCoreGetScrollState does no layout and no paint.
        int sx = 0, sy = 0, cw = 0, ch = 0, vw = 0, vh = 0;
        bool haveState = false;
        if (rc == 0) {
            int src = -1;
            try { src = WebCoreGetScrollState(&sx, &sy, &cw, &ch, &vw, &vh); } catch (...) { src = -1; }
            haveState = (src == 0);
        }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rgba, rcCopy, mySeq, present, dx, dy, sx, sy, cw, ch, vw, vh, haveState]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_opSeq != mySeq) { s->m_scrollBusy = false; s->m_scrollAccum = 0; s->m_scrollAccumX = 0; s->InstantPanReset(); return; }   // 被导航/点击取代,丢弃迟到帧+全部残留位移
                if (rcCopy == 0) {
                    if (!present) s->PresentSoftwareFrame(rgba);
                    s->m_lastFrameHash = 0;
                    if (haveState) { s->m_contentW = cw; s->m_contentH = ch; s->m_viewW = vw; s->m_viewH = vh; }
                    s->InstantPanApplied(sx, sy, haveState, dx, dy);
                }
                s->m_scrollBusy = false;
                if (s->m_scrollAccum != 0 || s->m_scrollAccumX != 0) s->PumpScroll();   // 拖拽期间又攒了位移(含纯横向),继续冲刷
                else { s->SyncLinksAfterScroll(); s->StartLiveMode(); }  // 滚动停了 → 补链接表 + 重启实时(新视口懒加载/动画)
            }));
        } catch (...) {}
    });
}

// Apotheosis (d982774): nested-scroll sibling of FreeScrollBy/PumpScroll above — same accumulate-
// then-flush shape (m_nestedScrollBusy gates one in-flight engine post at a time), but the flush
// dispatches through WebCoreWheelAt first so an overflow:auto container/modal/iframe under the
// dispatch point scrolls itself. dx/dy accumulate across deltas the same way; the dispatch point
// (px,py) is refreshed on every call, i.e. tracks the finger's current position, not the gesture's
// starting point.
void MainPage::NestedScrollBy(int px, int py, int dx, int dy)
{
    if (!m_sessionActive || (dx == 0 && dy == 0)) return;
    m_nestedAccumX += dx;
    m_nestedAccumY += dy;
    m_nestedPx = px; m_nestedPy = py;
    if (!m_nestedScrollBusy) PumpNestedScroll();
}

void MainPage::PumpNestedScroll()
{
    if ((m_nestedAccumX == 0 && m_nestedAccumY == 0) || !m_sessionActive) { m_nestedScrollBusy = false; return; }
    int dx = m_nestedAccumX; m_nestedAccumX = 0;
    int dy = m_nestedAccumY; m_nestedAccumY = 0;
    int px = m_nestedPx, py = m_nestedPy;
    m_nestedScrollBusy = true;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = m_opSeq;   // 被动滚动:不作废点击/导航令牌,但被它们作废(同 PumpScroll)
    bool present = m_gpuPresent;
    WebEngine::instance().post([disp, self, px, py, dx, dy, mySeq, present]() {
        // Apotheosis: acquire the buffer up front (same as PumpScroll) — WebCoreWheelAt now presents
        // this delta itself on a consumed (1) return, so it needs a real target even when the
        // WebCoreScrollBy fallback below never runs.
        auto rgba = AcquireEngineBuffer(present);
        int wrc = -999;
        // Apotheosis: dx/dy here are scroll-offset deltas (finger up -> content moves down -> positive
        // dy), the same convention WebCoreScrollBy takes below. WebCoreWheelAt instead builds a
        // PlatformWheelEvent from its deltaX/deltaY unchanged (WebCoreDriver.cpp:3131) and WebCore's
        // wheel convention is the opposite of that: positive deltaY means "wheel notch away from the
        // user" == content scrolls up. Passing the offset-delta straight through inverted the nested
        // scroller on device (banner moved opposite the finger) -> negate both axes only for this call,
        // so a finger-up pan still scrolls nested content down, matching the main-frame fast path below.
        try { wrc = WebCoreWheelAt(px, py, (float)-dx, (float)-dy, 2 /* changed */, rgba->data()); } catch (...) { wrc = -1000; }
        // wrc == 1: consumed by a nested scroller, main-frame position guaranteed untouched, and the
        // frame already composited/presented into rgba by WebCoreWheelAt itself — done, no
        // WebCoreScrollBy for this delta. Any other value (0 = not consumed, or a driver exception):
        // main-frame scroll position is left unchanged either way, so WebCoreScrollBy is safe to call
        // unconditionally for the same delta (ordering preserved: wheel first).
        int rc = 0;
        if (wrc != 1) {
            try { rc = WebCoreScrollBy(dx, dy, rgba->data()); } catch (...) { rc = -1000; }
        }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rgba, rcCopy, mySeq, present, wrc]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_opSeq != mySeq) { s->m_nestedScrollBusy = false; s->m_nestedAccumX = 0; s->m_nestedAccumY = 0; return; }   // 被导航/点击取代
                if (wrc == 1 || rcCopy == 0) {
                    if (!present) s->PresentSoftwareFrame(rgba);   // present 模式:WebCoreWheelAt/ScrollBy 已经直呈现到 GpuPanel
                    s->m_lastFrameHash = 0;
                }
                s->m_nestedScrollBusy = false;
                if (s->m_nestedAccumX != 0 || s->m_nestedAccumY != 0) s->PumpNestedScroll();   // 拖拽期间又攒了位移,继续冲刷
                else { s->SyncLinksAfterScroll(); s->StartLiveMode(); }
            }));
        } catch (...) {}
    });
}

// ===========================================================================
// Apotheosis (drag as pointer events): the WebCoreDragAt route.
//
// Google Maps, OpenStreetMap/Leaflet and canvas apps pan by handling
// pointerdown/mousedown and moving their own content — no scrollable box is
// involved anywhere, so translating the gesture into scrolling (either route
// above) leaves the widget frozen and drags the document behind it instead.
// WebCoreWantsDragAt, asked once at ManipulationStarted, says whether the point
// belongs to such a widget; if it does, the whole gesture is replayed into the
// page as a left-button mouse drag and NOTHING scrolls.
//
// Shape: like PumpNestedScroll, one engine post in flight at a time. Unlike it,
// the phases are ordered rather than summed — the press has to land first,
// because its return value is what decides whether this gesture is the page's at
// all, and moves must never overtake it. Moves coalesce to the latest position
// (an absolute point, not a delta: "the finger is here now").
// ===========================================================================

// The finger moved to (px,py) in engine viewport px. fallbackDx/fallbackDy are the
// same scroll-offset deltas the other routes take — kept only until the press is
// answered, so that a gesture the page turns out NOT to want can still be handed
// to the scroll path with nothing lost.
void MainPage::DragMoveTo(int px, int py, int fallbackDx, int fallbackDy)
{
    if (!m_sessionActive) return;
    // First movement of the gesture: queue the press at the touch-down point. Doing it here rather
    // than at ManipulationStarted means a gesture that never moves never presses — no stray
    // mousedown/mouseup pair, and taps keep going through Tapped/WebCoreClickAt as before.
    if (!m_dragActive && !m_dragPressPending && !m_dragPressSent) {
        m_dragPressPending = true;
        m_dragPressX = m_dragStartPx; m_dragPressY = m_dragStartPy;
        m_dragMoveX = m_dragStartPx; m_dragMoveY = m_dragStartPy;
    }
    m_dragMoveX = px; m_dragMoveY = py;
    m_dragMovePending = true;
    if (!m_dragActive) { m_dragFallbackDx += fallbackDx; m_dragFallbackDy += fallbackDy; }
    PumpDrag();
}

void MainPage::DragReset()
{
    // NOTE: this only forgets the gesture on the harness side. If a press is still in flight when a
    // navigation supersedes it, the engine's own teardownSession() drops its drag flag, so no
    // half-pressed state survives into the next page either.
    m_dragBusy = false;
    m_dragActive = false;
    m_dragPressPending = false;
    m_dragPressSent = false;
    m_dragMovePending = false;
    m_dragReleasePending = false;
    m_dragCancel = false;
    m_dragFallbackDx = 0; m_dragFallbackDy = 0;
}

void MainPage::PumpDrag()
{
    if (m_dragBusy) return;
    if (!m_sessionActive) { DragReset(); return; }
    int phase, px, py;
    if (m_dragPressPending) {
        phase = 0; px = m_dragPressX; py = m_dragPressY;
        m_dragPressPending = false; m_dragPressSent = true;
    } else if (!m_dragActive) {
        // No press was consumed (or none was ever sent): the gesture is not the page's, so anything
        // still queued for it is dropped rather than dispatched into a document that saw no mousedown.
        m_dragMovePending = false; m_dragReleasePending = false;
        return;
    } else if (m_dragMovePending) {
        phase = 1; px = m_dragMoveX; py = m_dragMoveY;
        m_dragMovePending = false;
    } else if (m_dragReleasePending) {
        phase = m_dragCancel ? 3 : 2; px = m_dragMoveX; py = m_dragMoveY;
        m_dragReleasePending = false;
        m_dragActive = false;   // the gesture ends with this post; nothing more may be queued for it
    } else
        return;

    m_dragBusy = true;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = m_opSeq;      // 同 PumpScroll:不作废点击/导航令牌,但被它们作废
    unsigned long long myGen = m_dragGen;    // per gesture; a late answer from the previous one is dropped
    bool present = m_gpuPresent;
    WebEngine::instance().post([disp, self, phase, px, py, mySeq, myGen, present]() {
        auto rgba = AcquireEngineBuffer(present);
        int rc = -999;
        try { rc = WebCoreDragAt(phase, px, py, rgba->data()); } catch (...) { rc = -1000; }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rgba, rcCopy, phase, mySeq, myGen, present]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_opSeq != mySeq) { s->DragReset(); return; }   // 被导航/点击取代
                if (s->m_dragGen != myGen) return;                     // answer belongs to a finished gesture
                if (phase == 0) {
                    s->m_dragActive = (rcCopy == 1);
                    if (!s->m_dragActive) {
                        // Nothing took the press even though WebCoreWantsDragAt liked the point: this is
                        // an ordinary pan after all. Hand the gesture back to the scroll path, including
                        // the movement that happened while the press was in flight.
                        s->m_nestedScrollState = NestedScrollState::No;
                        s->m_dragMovePending = false;
                        s->m_dragReleasePending = false;
                        s->m_dragBusy = false;
                        int fdx = s->m_dragFallbackDx, fdy = s->m_dragFallbackDy;
                        s->m_dragFallbackDx = 0; s->m_dragFallbackDy = 0;
                        if (fdx != 0 || fdy != 0) { s->InstantPanBy(fdx, fdy); s->FreeScrollBy(fdx, fdy); }
                        return;
                    }
                    s->m_dragFallbackDx = 0; s->m_dragFallbackDy = 0;   // the page owns it now
                }
                if (rcCopy == 1) {
                    // WebCoreDragAt already composited/presented this frame (direct swap in present
                    // mode), same contract as WebCoreWheelAt — only the software path needs the blit.
                    if (!present) s->PresentSoftwareFrame(rgba);
                    s->m_lastFrameHash = 0;
                }
                s->m_dragBusy = false;
                if (s->m_dragMovePending || s->m_dragReleasePending) s->PumpDrag();
                else if (!s->m_dragActive) { s->SyncLinksAfterScroll(); s->StartLiveMode(); }
            }));
        } catch (...) {}
    });
}

// 滚动停止后一次性刷新链接命中表(滚动期间为提速跳过了引擎 extractLinks)。点击走引擎实时命中测试(权威),
// 故此刷新主要服务点击兜底/主页路径;陈旧窗口仅限"刚停手到这帧返回"之间,无碍。
void MainPage::SyncLinksAfterScroll()
{
    if (!m_sessionActive) return;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = m_opSeq;
    WebEngine::instance().post([disp, self, mySeq]() {
        int rc = -999;
        try { rc = WebCoreSyncLinks(); } catch (...) { rc = -1000; }
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        if (rc == 0) {
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) { int lx=0,ly=0,lw=0,lh=0; char lu[1200]=""; if (WebCoreGetLink(i,&lx,&ly,&lw,&lh,lu,sizeof lu)) { Harness::PageLink pl; pl.x=lx; pl.y=ly; pl.w=lw; pl.h=lh; pl.url=Utf8ToWide(lu); links->push_back(std::move(pl)); } }
        }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Low, ref new DispatchedHandler([self, links, rcCopy, mySeq]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_opSeq != mySeq) return;   // 被新操作取代
                if (rcCopy == 0) s->m_pageLinks = *links;
            }));
        } catch (...) {}
    });
}
// Apotheosis (d982774): gesture start — fire-and-forget hit test (WebCoreIsScrollableAt) at the
// touch-down point, posted to the engine thread, never blocking the UI thread. The answer lands
// later via RunAsync; m_nestedScrollGen guards against a gesture that has already ended (or been
// replaced by a new one) applying a stale answer.
//
// Apotheosis (review 2026-09-03): deltas arriving before the answer are now BUFFERED
// (m_pendingPan*) rather than run down the main-frame fast path — that first delta moved the page
// behind a cookie overlay by a visible jerk before the route switched. ReplayPendingPan() below
// sends them through whichever path the answer selects. Because Unknown no longer means "fast
// path", the two early returns here must set the state explicitly: no hit test is posted on them,
// so nothing would ever resolve Unknown and the whole gesture would buffer forever.
void MainPage::OnImageManipStarted(Platform::Object^, Windows::UI::Xaml::Input::ManipulationStartedRoutedEventArgs^ e)
{
    unsigned long long gen = ++m_nestedScrollGen;
    m_nestedScrollState = NestedScrollState::Unknown;
    m_pendingPanX = 0; m_pendingPanY = 0;
    // Apotheosis (drag as pointer events): a new gesture — drop whatever the previous one left
    // behind before the probe below can answer Drag for this one.
    ++m_dragGen;
    DragReset();
    // Apotheosis (instant pan): a new gesture starts from the frame that is on screen — drop any
    // remainder the previous one left behind, then ask the engine where it is so the very first
    // delta can already be clamped to the document.
    InstantPanReset();
    m_scrollStateValid = false;
    RequestScrollState();
    if (!m_sessionActive) { m_nestedScrollState = NestedScrollState::No; return; }
    int px, py; MapTapToEngine(e->Position.X, e->Position.Y, px, py);
    if (px < 0 || py < 0 || px >= kW || py >= kH) {
        m_nestedScrollState = NestedScrollState::No;   // off-viewport touch-down: main-frame fast path
        return;
    }
    m_dragStartPx = px; m_dragStartPy = py;   // the press, if this turns out to be a drag, goes here

    Platform::Agile<MainPage^> self(this);
    CoreDispatcher^ disp = this->Dispatcher;
    // Apotheosis (drag as pointer events): two probes, cheapest routing decision first. A point that
    // belongs to something which drags itself (map/canvas/touch-action widget) wins over "there is a
    // scrollable ancestor", because such widgets are routinely nested inside a scrollable container
    // and scrolling that container is exactly the wrong answer. WebCoreIsScrollableAt is then not
    // even asked, which keeps the added cost of this feature at zero for the Drag case.
    bool probeDrag = m_dragPointer;
    WebEngine::instance().post([disp, self, px, py, gen, probeDrag]() {
        int drag = 0;
        if (probeDrag) { try { drag = WebCoreWantsDragAt(px, py); } catch (...) { drag = 0; } }
        int r = 0;
        if (!drag) { try { r = WebCoreIsScrollableAt(px, py); } catch (...) { r = 0; } }
        try {
            disp->RunAsync(CoreDispatcherPriority::High, ref new DispatchedHandler([self, r, drag, gen]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_nestedScrollGen != gen) return;   // gesture already ended / superseded: drop the answer
                s->m_nestedScrollState = drag ? NestedScrollState::Drag
                                              : (r ? NestedScrollState::Yes : NestedScrollState::No);
                s->ReplayPendingPan();   // the answer is in: send what the finger did while waiting
            }));
        } catch (...) {}
    });
}

// Apotheosis (review 2026-09-03): flush the deltas buffered while the nested-scroll answer was
// still Unknown, down the path that answer chose. Called from the hit-test callback and from
// OnImageManipCompleted (belt and braces: if the answer never arrives — engine post dropped, or
// the gesture outlived it — the movement is still applied rather than silently lost, and by then
// the state is whatever it ended up being, defaulting to the main-frame fast path).
void MainPage::ReplayPendingPan()
{
    int dx = m_pendingPanX, dy = m_pendingPanY;
    m_pendingPanX = 0; m_pendingPanY = 0;
    if ((dx == 0 && dy == 0) || !m_sessionActive)
        return;
    if (m_nestedScrollState == NestedScrollState::Drag)
        DragMoveTo(m_pendingPanPx, m_pendingPanPy, dx, dy);   // presses at the touch-down point first
    else if (m_nestedScrollState == NestedScrollState::Yes)
        NestedScrollBy(m_pendingPanPx, m_pendingPanPy, dx, dy);
    else {
        InstantPanBy(dx, dy);
        FreeScrollBy(dx, dy);
    }
}

// ===========================================================================
// Apotheosis: instant pan — "the last frame follows the finger".
//
// The problem: a touch pan posts a coalesced WebCoreScrollBy to the engine thread and the screen
// only moves once the engine has scrolled, composited and presented. While the engine is busy
// (JS, layout, image decode) the finger runs ahead of the picture and the page reads as stuck.
//
// The fix: the pixels the engine already presented are exactly the pixels one pan delta away from
// what the finger wants, so the UI thread moves them itself — a TranslateTransform on the same
// presenting element the pinch preview scales (PresentLayer(): GpuPanel in direct-present mode,
// RenderImage in software mode). The engine keeps getting the identical coalesced WebCoreScrollBy
// it got before; nothing about the engine path changes.
//
// Bookkeeping, all on the UI thread:
//   m_panRemX/Y  finger-requested engine px the engine has NOT applied yet. Grows on every delta
//                (InstantPanBy), shrinks by the distance the engine really moved every time an
//                engine frame lands (InstantPanApplied, from PumpScroll's completion). At 0 the
//                transform is identity and the engine frame alone is on screen.
//   m_scroll*/m_content*/m_view*  last scroll position and document bounds the engine reported
//                (WebCoreGetScrollState). Two jobs: measure what the engine actually applied
//                (which is NOT the delta we sent — the engine clamps at the document edges), and
//                clamp the preview so a fling at the bottom of a page cannot slide the content off
//                its own end.
//
// Uncovered area revealed by the translation shows the panel background; that is accepted, and the
// translation is capped at one screen so it can never be more than a screen of it.
// ===========================================================================

// The finger asked for dx/dy more engine px. Show them now.
void MainPage::InstantPanBy(int dx, int dy)
{
    // Off by setting, no session, or a pinch owns the presenting layer right now: leave the
    // transform alone entirely (a pinch preview must not be fought over — see SetPinchAnchor).
    if (!m_instantPan || !m_sessionActive || m_pinching || m_zoomSpring != nullptr)
        return;
    if (dx == 0 && dy == 0)
        return;
    m_panRemX += dx;
    m_panRemY += dy;
    ClampPanRemainder();
    ApplyPanTransform();
    RestartPanSnapTimer();
}

// An engine frame landed. newScrollX/Y is where the engine now is (WebCoreGetScrollState, read on
// the engine thread in the same hop as the WebCoreScrollBy). haveScrollState=false means the call
// failed — then fall back to "the engine applied the delta we dispatched", which is right except
// at a document edge, where the snap timer cleans up.
void MainPage::InstantPanApplied(int newScrollX, int newScrollY, bool haveScrollState,
                                 int fallbackDx, int fallbackDy)
{
    if (!haveScrollState) {
        m_scrollStateValid = false;
        if (m_panRemX || m_panRemY) {
            m_panRemX -= fallbackDx;
            m_panRemY -= fallbackDy;
            ClampPanRemainder();
            ApplyPanTransform();
        }
        return;
    }
    if (m_scrollStateValid) {
        m_panRemX -= (newScrollX - m_scrollX);
        m_panRemY -= (newScrollY - m_scrollY);
    } else {
        // First frame of this gesture and the seed from RequestScrollState() never arrived: the
        // dispatched delta is the best estimate we have.
        m_panRemX -= fallbackDx;
        m_panRemY -= fallbackDy;
    }
    m_scrollX = newScrollX;
    m_scrollY = newScrollY;
    m_scrollStateValid = true;
    ClampPanRemainder();
    ApplyPanTransform();
}

void MainPage::InstantPanReset()
{
    m_panRemX = 0;
    m_panRemY = 0;
    if (m_panSnapTimer) m_panSnapTimer->Stop();
    ApplyPanTransform();
}

// Keep the previewed position inside the document, and never show more than one screen of
// background. m_scroll* + m_panRem* is the position the user is looking at.
void MainPage::ClampPanRemainder()
{
    if (m_scrollStateValid) {
        int maxX = m_contentW - m_viewW; if (maxX < 0) maxX = 0;
        int maxY = m_contentH - m_viewH; if (maxY < 0) maxY = 0;
        const int loX = -m_scrollX, hiX = maxX - m_scrollX;
        const int loY = -m_scrollY, hiY = maxY - m_scrollY;
        if (m_panRemX < loX) m_panRemX = loX;
        if (m_panRemX > hiX) m_panRemX = hiX;
        if (m_panRemY < loY) m_panRemY = loY;
        if (m_panRemY > hiY) m_panRemY = hiY;
    }
    if (m_panRemX >  kW) m_panRemX =  kW;
    if (m_panRemX < -kW) m_panRemX = -kW;
    if (m_panRemY >  kH) m_panRemY =  kH;
    if (m_panRemY < -kH) m_panRemY = -kH;
}

// Engine px → presenting-layer DIPs. Same DIP↔px factor as MapTapToEngine/SetPinchAnchor
// (c5731be): the kW×kH surface is stretched over the whole presenting layer, so the factor is that
// layer's own size — GpuPanel spans the content row while ContentArea sits 6 DIP inside it.
// A positive remainder means "the engine still owes us a scroll down", i.e. the content must move
// UP on screen → negative translation.
void MainPage::ApplyPanTransform()
{
    // Apotheosis (review 2026-09-03): during a pinch (and its spring-back) m_panTranslate belongs
    //   to the zoom path — ApplyLiveZoom writes the clamp/recentre offset into the very same
    //   transform (see the comment at m_panTranslate's reuse there). A late engine frame
    //   (InstantPanApplied) or the async WebCoreGetScrollState answer (RequestScrollState) landing
    //   mid-gesture would come in here with a zero remainder, write X/Y = 0 and detach the
    //   translation — wiping the clamp and letting the preview jump past the document edge.
    //   InstantPanBy already refuses to run while pinching; do the same for every other caller.
    if (m_pinching || m_zoomSpring != nullptr)
        return;
    if (m_panRemX == 0 && m_panRemY == 0) {
        if (m_panTranslate != nullptr) { m_panTranslate->X = 0.0; m_panTranslate->Y = 0.0; }
        ApplyPresentTransform();
        return;
    }
    if (!GpuPanel || !RenderImage || !ContentArea)
        return;
    auto layer = PresentLayer();
    double lw = (layer != nullptr) ? layer->ActualWidth : 0.0;
    double lh = (layer != nullptr) ? layer->ActualHeight : 0.0;
    if (!(lw > 1.0)) lw = ContentArea->ActualWidth;
    if (!(lh > 1.0)) lh = ContentArea->ActualHeight;
    const double fx = (lw > 1.0) ? lw / (double)kW : 1.0;
    const double fy = (lh > 1.0) ? lh / (double)kH : 1.0;
    if (m_panTranslate == nullptr) m_panTranslate = ref new Windows::UI::Xaml::Media::TranslateTransform();
    m_panTranslate->X = -(double)m_panRemX * fx;
    m_panTranslate->Y = -(double)m_panRemY * fy;
    ApplyPresentTransform();
}

// Seed the scroll/bounds cache at gesture start so the very first delta can already be clamped.
// Fire-and-forget on the engine thread, exactly like the WebCoreIsScrollableAt hit test — the UI
// thread never waits on the engine. A late answer for a gesture that has been superseded is
// dropped via m_scrollStateGen, and an answer that arrives after a real engine frame must not
// overwrite the position that frame reported, so only the bounds are refreshed in that case.
void MainPage::RequestScrollState()
{
    if (!m_instantPan || !m_sessionActive) return;
    const unsigned long long gen = ++m_scrollStateGen;
    Platform::Agile<MainPage^> self(this);
    CoreDispatcher^ disp = this->Dispatcher;
    WebEngine::instance().post([disp, self, gen]() {
        int sx = 0, sy = 0, cw = 0, ch = 0, vw = 0, vh = 0, rc = -1;
        try { rc = WebCoreGetScrollState(&sx, &sy, &cw, &ch, &vw, &vh); } catch (...) { rc = -1; }
        if (rc != 0) return;
        try {
            disp->RunAsync(CoreDispatcherPriority::High, ref new DispatchedHandler([self, gen, sx, sy, cw, ch, vw, vh]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_scrollStateGen != gen) return;
                s->m_contentW = cw; s->m_contentH = ch; s->m_viewW = vw; s->m_viewH = vh;
                if (!s->m_scrollStateValid) {
                    s->m_scrollX = sx; s->m_scrollY = sy;
                    s->m_scrollStateValid = true;
                }
                s->ClampPanRemainder();
                s->ApplyPanTransform();
            }));
        } catch (...) {}
    });
}

// The engine wins after ~1 s. Restarted on every finger delta, so the deadline is measured from
// the last movement — during a gesture (and its inertia) it simply never fires, and after the
// gesture it bounds how long a stale offset can sit on screen if the engine never catches up
// (a load stole the engine thread, the scroll was clamped by something we cannot see, ...).
void MainPage::RestartPanSnapTimer()
{
    if (m_panRemX == 0 && m_panRemY == 0) {
        if (m_panSnapTimer) m_panSnapTimer->Stop();
        return;
    }
    if (!m_panSnapTimer) {
        m_panSnapTimer = ref new Windows::UI::Xaml::DispatcherTimer();
        m_panSnapTimer->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(this, &MainPage::OnPanSnapTick);
    }
    Windows::Foundation::TimeSpan ts; ts.Duration = 10000000LL;   // 1 s (100 ns units)
    m_panSnapTimer->Stop();
    m_panSnapTimer->Interval = ts;
    m_panSnapTimer->Start();
}

void MainPage::OnPanSnapTick(Platform::Object^, Platform::Object^)
{
    InstantPanReset();
}

// Apotheosis: one place that decides what sits on the presenting element. The pinch preview scale
// and the instant-pan translation are composed in a TransformGroup with the scale FIRST, so the
// translation stays in final screen DIPs instead of being scaled by the preview. In practice the
// two never overlap (a pinch cancels the pan, see SetPinchAnchor), but a stray combination must
// still compose rather than have one silently replace the other.
// Both elements are cleared and the group emptied before anything is (re)attached: a XAML Transform
// may only have one parent, and re-appending one that still has its old parent throws.
void MainPage::ApplyPresentTransform()
{
    // ApplySettings() can reach this before the XAML tree exists (settings.ini with instantpan=0
    // is read in the constructor), so both elements are checked like NavigateTo does.
    if (!GpuPanel || !RenderImage)
        return;
    const bool haveZoom = (m_zoomTransform != nullptr);
    const bool havePan = (m_panTranslate != nullptr) && (m_panTranslate->X != 0.0 || m_panTranslate->Y != 0.0);
    GpuPanel->RenderTransform = nullptr;
    RenderImage->RenderTransform = nullptr;
    if (m_presentGroup != nullptr) m_presentGroup->Children->Clear();
    if (!haveZoom && !havePan)
        return;
    Windows::UI::Xaml::Media::Transform^ t;
    if (haveZoom && havePan) {
        if (m_presentGroup == nullptr) m_presentGroup = ref new Windows::UI::Xaml::Media::TransformGroup();
        m_presentGroup->Children->Append(m_zoomTransform);
        m_presentGroup->Children->Append(m_panTranslate);
        t = m_presentGroup;
    } else
        t = haveZoom ? static_cast<Windows::UI::Xaml::Media::Transform^>(m_zoomTransform)
                     : static_cast<Windows::UI::Xaml::Media::Transform^>(m_panTranslate);
    if (m_gpuPresent) GpuPanel->RenderTransform = t;
    else              RenderImage->RenderTransform = t;
}

// Apotheosis (OFFTHREAD-RASTER-LOG.md): DEVELOPER toggle "Threaded raster". Engine-thread call.
void MainPage::ApplyThreadedRasterSetting()
{
    const int en = m_threadedRaster ? 1 : 0;
    WebEngine::instance().post([en]() { try { WebCoreSetThreadedRaster(en); } catch (...) {} });
}

// 自由滚动:内容区 ManipulationDelta(去掉 ScrollViewer 后,触摸不再被吞)。单指拖拽的累计 ΔY → 引擎滚动。
// TranslateInertia 让松手后继续惯性滚(ManipulationDelta 在惯性期持续触发)。点击经 Tapped 走(手势识别器
// 区分点按 vs 拖拽,小位移=Tapped、越阈值=Manipulation,不会冲突)。
void MainPage::OnImageManipDelta(Platform::Object^, Windows::UI::Xaml::Input::ManipulationDeltaRoutedEventArgs^ e)
{
    if (!m_sessionActive) return;
    // M4 捏合缩放:本次增量 Scale≠1(或已进入捏合)→ 捏合模式:只对显示层做实时 ScaleTransform(零引擎调用,
    //   丝滑),不走引擎滚动;松手(OnImageManipCompleted)再把累计缩放提交给引擎按新尺度重栅格。
    float ds = e->Delta.Scale;
    if (m_pinching || (ds > 0.0f && (ds > 1.002f || ds < 0.998f))) {
        // Apotheosis: the anchor is taken ONCE, at the first pinch delta, and then frozen for the
        //   whole gesture. It used to follow e->Position on every delta, and the centroid of a
        //   two-finger manipulation becomes the position of the remaining finger the moment the
        //   first one leaves the glass — re-centring the transform there shifts the content by
        //   (Fnew − Fold)·(1 − live), which is exactly the "it snaps to the other finger" jump on
        //   release (worst when zooming in, where 1 − live is largest).
        if (!m_pinching) {
            // Apotheosis (review 2026-09-03): this gesture turns out to be a pinch, not a pan —
            // drop anything buffered for the (never-resolved) nested-scroll route so it cannot be
            // replayed as a scroll on top of the zoom.
            m_pendingPanX = 0; m_pendingPanY = 0;
            // Apotheosis (drag as pointer events): the page is holding a mouse button we pressed —
            // end it as a cancel before the zoom takes over, or the page keeps dragging its content
            // for the rest of the gesture and never sees a mouseup.
            if (m_dragActive || m_dragPressPending || m_dragBusy) {
                m_dragCancel = true;
                m_dragMovePending = false;
                m_dragReleasePending = true;
                PumpDrag();
            }
            m_nestedScrollState = NestedScrollState::No;
            // Apotheosis (instant pan): a pinch and a pan must not fight over the presenting
            // element's transform — drop the pan preview before the anchor is taken (SetPinchAnchor
            // needs an untransformed TransformToVisual).
            // Order matters: ApplyPanTransform now refuses to touch the transform while m_pinching
            // is set (it belongs to the zoom clamp then), so this reset must run before the flag.
            InstantPanReset();
            m_pinching = true;
            SetPinchAnchor(e->Position.X, e->Position.Y);
        }
        if (ds > 0.0f) m_liveScale *= ds;
        float total = m_pageScale * m_liveScale;          // 钳总缩放到 [kMinLiveScale,kMaxPageScale]
        if (m_pageScale > 0.0f) {
            if (total < kMinLiveScale) m_liveScale = kMinLiveScale / m_pageScale;
            if (total > kMaxPageScale) m_liveScale = kMaxPageScale / m_pageScale;
        }
        ApplyLiveZoom();
        return;
    }
    // Apotheosis: ManipulationDelta 的位移是内容区显示坐标(DIP),而 FreeScrollBy/WebCoreScrollBy 要的是
    //   固定引擎视口像素(kW×kH)。此前直接把 DIP 当引擎像素传 → 页面只跟手约一半。与 MapTapToEngine 同一换算。
    //   不要再除 m_pageScale:表面无论缩放都铺满同一块屏幕矩形。
    double sx = ContentArea->ActualWidth  > 1.0 ? (double)kW / ContentArea->ActualWidth  : 1.0;
    double sy = ContentArea->ActualHeight > 1.0 ? (double)kH / ContentArea->ActualHeight : 1.0;
    double dx = -e->Delta.Translation.X * sx;        // 手指左移(ΔX<0)→ 内容右滚(dx>0)
    double dy = -e->Delta.Translation.Y * sy;        // 手指上移(ΔY<0)→ 内容下滚(dy>0)
    int idx = (int)(dx < 0 ? dx - 0.5 : dx + 0.5);
    int idy = (int)(dy < 0 ? dy - 0.5 : dy + 0.5);
    if (idx == 0 && idy == 0) return;
    // Apotheosis (d982774): this gesture started over a nested scroller (cookie overlay/modal/
    // iframe) -> route through WebCoreWheelAt instead of the main-frame fast path. Covers inertia
    // too: OnImageManipDelta keeps firing translation deltas during TranslateInertia, and
    // m_nestedScrollState is only reset (to Unknown) in OnImageManipCompleted, so inertia deltas
    // of a gesture that started nested stay on this path all the way to rest.
    if (m_nestedScrollState == NestedScrollState::Unknown) {
        // Apotheosis (review 2026-09-03): the hit test has not answered yet. Buffer instead of
        // guessing — running these through the main-frame fast path scrolled the page behind the
        // overlay by one visible jerk on every gesture that turned out to be nested. Nothing is
        // lost by waiting: WebCoreIsScrollableAt sits in the same engine-thread queue a
        // WebCoreScrollBy would, so the frame could not have been produced any earlier anyway.
        int px, py; MapTapToEngine(e->Position.X, e->Position.Y, px, py);
        if (px < 0) px = 0; else if (px >= kW) px = kW - 1;
        if (py < 0) py = 0; else if (py >= kH) py = kH - 1;
        m_pendingPanPx = px; m_pendingPanPy = py;
        m_pendingPanX += idx; m_pendingPanY += idy;
        return;
    }
    // Apotheosis (drag as pointer events): the gesture started over a map/canvas — hand the finger
    // to the page as a mouse drag. Absolute position, not a delta: the page moves its own content
    // from where the pointer is, so a coalesced move simply means "the finger is here now".
    // Deliberately no InstantPanBy and no WebCoreScrollBy on this path: the document must not move.
    if (m_nestedScrollState == NestedScrollState::Drag) {
        int px, py; MapTapToEngine(e->Position.X, e->Position.Y, px, py);
        if (px < 0) px = 0; else if (px >= kW) px = kW - 1;
        if (py < 0) py = 0; else if (py >= kH) py = kH - 1;
        DragMoveTo(px, py, idx, idy);
        return;
    }
    if (m_nestedScrollState == NestedScrollState::Yes) {
        int px, py; MapTapToEngine(e->Position.X, e->Position.Y, px, py);
        if (px < 0) px = 0; else if (px >= kW) px = kW - 1;
        if (py < 0) py = 0; else if (py >= kH) py = kH - 1;
        NestedScrollBy(px, py, idx, idy);
        return;
    }
    // No:现有主帧快路径不变。
    // Apotheosis (instant pan): move the presented frame with the finger right now; the engine
    // still gets the same coalesced WebCoreScrollBy it always did.
    InstantPanBy(idx, idy);
    FreeScrollBy(idx, idy);
}

// Apotheosis: the element that actually shows the engine output — in direct-present mode the
//   engine composites straight into GpuPanel, otherwise the software frame sits on RenderImage.
//   Note GpuPanel spans the whole content row while ContentArea sits 6 DIP inside it (Border
//   Margin="6,6,6,0"), so the two are NOT the same coordinate space.
Windows::UI::Xaml::FrameworkElement^ MainPage::PresentLayer()
{
    return m_gpuPresent ? static_cast<Windows::UI::Xaml::FrameworkElement^>(GpuPanel)
                        : static_cast<Windows::UI::Xaml::FrameworkElement^>(RenderImage);
}

// Apotheosis: fix the pinch anchor from a manipulation position (ContentArea DIPs).
//
// Preview and engine must keep the SAME point of the page pinned, otherwise the engine frame
// that replaces the preview lands somewhere else and the content visibly jumps on release.
// Both sides pin a point, but they name it in different spaces:
//
//   preview  ScaleTransform(live, centre = A) on the presenting layer maps a layer point p to
//            A + (p − A)·live, i.e. the layer point A is the fixed point.
//   engine   WebCoreSetPageScale(scale, focalX, focalY) — port\WebCoreDriver.cpp — takes the
//            focal in ENGINE VIEWPORT PIXELS (0..kW × 0..kH), not in page/CSS coordinates and
//            not in DIPs. It computes the content point c = scroll + focal/oldScale and sets
//            the new scroll to c − focal/newScale, i.e. the viewport pixel `focal` is the fixed
//            point. Scroll offset and the current page scale therefore need no term of their
//            own here — the engine reads both itself; passing the anchor is enough.
//
// So the committed focal is simply the preview's transform centre expressed in engine pixels:
//
//     A     = TransformToVisual(ContentArea → presenting layer) · position    [layer DIPs]
//     focal = A · (kW / layer.ActualWidth, kH / layer.ActualHeight)           [engine px]
//
// (The engine surface is created at kW×kH and stretched over the whole presenting layer, so the
// DIP→px factor is the layer's own size — the same kW/ActualWidth idea as MapTapToEngine, but
// against the layer that carries the transform instead of against ContentArea.)
void MainPage::SetPinchAnchor(double dipX, double dipY)
{
    auto layer = PresentLayer();
    double lx = dipX, ly = dipY;
    // A spring-back may still be running (pinch again within kZoomSpringMs): stop it, keep the
    //   scale it was heading for, and drop its pending commit — this gesture will commit instead.
    if (m_zoomSpring != nullptr) {
        try { m_zoomSpring->Stop(); } catch (...) {}   // Stop() does not raise Completed
        m_zoomSpring = nullptr;
        m_liveScale = m_springTargetLive;
    }
    // TransformToVisual would fold in a RenderTransform still sitting on the layer, so drop it
    //   first. A fresh transform per gesture also keeps a finished animation from holding its
    //   value on the old one (FillBehavior=HoldEnd would swallow later writes to ScaleX).
    m_zoomTransform = nullptr;
    GpuPanel->RenderTransform = nullptr;
    RenderImage->RenderTransform = nullptr;
    if (m_presentGroup != nullptr) m_presentGroup->Children->Clear();   // release the old parents too
    if (layer != nullptr && layer != static_cast<Windows::UI::Xaml::FrameworkElement^>(ContentArea)) {
        try {
            auto tv = ContentArea->TransformToVisual(layer);
            auto p = tv->TransformPoint(Windows::Foundation::Point((float)dipX, (float)dipY));
            lx = p.X; ly = p.Y;
        } catch (...) {}
    }
    m_focalX = lx; m_focalY = ly;                     // presenting-layer DIPs = ScaleTransform centre
    double lw = (layer != nullptr) ? layer->ActualWidth : 0.0;
    double lh = (layer != nullptr) ? layer->ActualHeight : 0.0;
    if (!(lw > 1.0)) lw = ContentArea->ActualWidth;
    if (!(lh > 1.0)) lh = ContentArea->ActualHeight;
    double px = (lw > 1.0) ? lx * (double)kW / lw : lx;
    double py = (lh > 1.0) ? ly * (double)kH / lh : ly;
    if (px < 0.0) px = 0.0; if (px > (double)kW) px = (double)kW;
    if (py < 0.0) py = 0.0; if (py > (double)kH) py = (double)kH;
    m_focalPx = (int)(px + 0.5);
    m_focalPy = (int)(py + 0.5);
}

// 实时缩放变换:把 ScaleTransform(以焦点为中心)挂到当前显示层(present=GpuPanel,readback=RenderImage)。
//   只变换已渲染像素 → 捏合期间 60fps 丝滑,不调引擎。
void MainPage::ApplyLiveZoom()
{
    // Apotheosis: one transform per gesture, kept as a member — SpringBackZoom animates it.
    if (m_zoomTransform == nullptr) m_zoomTransform = ref new Windows::UI::Xaml::Media::ScaleTransform();
    m_zoomTransform->ScaleX = m_liveScale; m_zoomTransform->ScaleY = m_liveScale;
    m_zoomTransform->CenterX = m_focalX; m_zoomTransform->CenterY = m_focalY;

    // Apotheosis (package 7): clamp/centre the preview against the document edges (ClampZoomAxis).
    //   Reuses m_panTranslate — InstantPanBy() never touches it while m_pinching is set, and
    //   ApplyPresentTransform already composes it after the scale ("145b29f", scale first so the
    //   translation stays screen-space) — so a pinch can borrow it for the whole gesture and
    //   PinchCommit()/SpringBackZoom already know how to zero it back out on release.
    auto layer = PresentLayer();
    double lw = (layer != nullptr) ? layer->ActualWidth : 0.0;
    double lh = (layer != nullptr) ? layer->ActualHeight : 0.0;
    if (!(lw > 1.0)) lw = ContentArea->ActualWidth;
    if (!(lh > 1.0)) lh = ContentArea->ActualHeight;
    const double fx = (lw > 1.0) ? lw / (double)kW : 1.0;
    const double fy = (lh > 1.0) ? lh / (double)kH : 1.0;
    const double viewW = (m_scrollStateValid && m_viewW > 0) ? (double)m_viewW : (double)kW;
    const double viewH = (m_scrollStateValid && m_viewH > 0) ? (double)m_viewH : (double)kH;
    const double rbX = kZoomRubberBandDip / (fx > 0.0001 ? fx : 1.0);
    const double rbY = kZoomRubberBandDip / (fy > 0.0001 ? fy : 1.0);
    const double tx = ClampZoomAxis((double)m_scrollX, (double)m_contentW, (double)m_focalPx, viewW,
                                     (double)m_liveScale, rbX, m_scrollStateValid);
    const double ty = ClampZoomAxis((double)m_scrollY, (double)m_contentH, (double)m_focalPy, viewH,
                                     (double)m_liveScale, rbY, m_scrollStateValid);
    if (m_panTranslate == nullptr) m_panTranslate = ref new Windows::UI::Xaml::Media::TranslateTransform();
    m_panTranslate->X = tx * fx;
    m_panTranslate->Y = ty * fy;

    ApplyPresentTransform();   // composes m_zoomTransform (scale) + m_panTranslate (clamp) in order
}

// Apotheosis: ease the preview from the scale the fingers left it at to the scale we are about to
//   commit, then commit. Two cases produce a gap: pinching out below 1:1 (overview — the engine
//   never renders below fit-to-width, so the preview must come back up) and the ±6 % snap to 1.0.
//   Committing straight away would show that gap as a jump; a short ease-out reads as a release.
//   The animation targets a RenderTransform, so it runs on the composition thread — the UI thread
//   is free and the engine is called exactly once, when the animation is done.
void MainPage::SpringBackZoom(float targetLive, float commitScale)
{
    using namespace Windows::UI::Xaml::Media::Animation;
    m_springTargetLive = targetLive;
    auto sb = ref new Storyboard();
    Windows::Foundation::TimeSpan ts; ts.Duration = (long long)kZoomSpringMs * 10000;   // 100 ns units
    for (int i = 0; i < 2; ++i) {
        auto a = ref new DoubleAnimation();
        a->To = ref new Platform::Box<double>((double)targetLive);   // IReference<double>

        a->Duration = Windows::UI::Xaml::Duration(ts);
        a->EnableDependentAnimation = true;
        auto ease = ref new QuadraticEase();
        ease->EasingMode = EasingMode::EaseOut;
        a->EasingFunction = ease;
        Storyboard::SetTarget(a, m_zoomTransform);
        Storyboard::SetTargetProperty(a, i == 0 ? "ScaleX" : "ScaleY");
        sb->Children->Append(a);
    }
    // Apotheosis (package 7 rubber-band): ease whatever ClampZoomAxis left on m_panTranslate back
    //   to 0 in the same spring — the committed frame lands with no translate at all (PinchCommit
    //   zeroes it), so anything left over here must be gone before that swap or the release reads
    //   as a jump instead of a settle.
    if (m_panTranslate != nullptr && (m_panTranslate->X != 0.0 || m_panTranslate->Y != 0.0)) {
        for (int i = 0; i < 2; ++i) {
            auto a = ref new DoubleAnimation();
            a->To = ref new Platform::Box<double>(0.0);
            a->Duration = Windows::UI::Xaml::Duration(ts);
            a->EnableDependentAnimation = true;
            auto ease = ref new QuadraticEase();
            ease->EasingMode = EasingMode::EaseOut;
            a->EasingFunction = ease;
            Storyboard::SetTarget(a, m_panTranslate);
            Storyboard::SetTargetProperty(a, i == 0 ? "X" : "Y");
            sb->Children->Append(a);
        }
    }
    m_zoomSpring = sb;
    Platform::Agile<MainPage^> self(this);
    sb->Completed += ref new Windows::Foundation::EventHandler<Platform::Object^>(
        [self, targetLive, commitScale](Platform::Object^, Platform::Object^) {
            MainPage^ s = self.Get(); if (!s) return;
            if (s->m_zoomSpring == nullptr) return;         // stopped by a new pinch
            try { s->m_zoomSpring->Stop(); } catch (...) {} // release HoldEnd on ScaleX/ScaleY/X/Y
            s->m_zoomSpring = nullptr;
            s->m_liveScale = 1.0f;
            if (s->m_zoomTransform != nullptr) {            // Stop() snapped it back — hold the target
                s->m_zoomTransform->ScaleX = targetLive;
                s->m_zoomTransform->ScaleY = targetLive;
            }
            if (s->m_panTranslate != nullptr) {             // same — the rubber-band always ends at 0
                s->m_panTranslate->X = 0.0;
                s->m_panTranslate->Y = 0.0;
            }
            s->PinchCommit(commitScale, s->m_focalPx, s->m_focalPy);
        });
    try { sb->Begin(); } catch (...) {                      // no animation? commit right away
        m_zoomSpring = nullptr;
        m_liveScale = 1.0f;
        PinchCommit(commitScale, m_focalPx, m_focalPy);
    }
}

// 捏合结束:把累计缩放提交给引擎(WebCoreSetPageScale 按新尺度重栅格 → 文字清晰),回 UI 后复位变换 + 显示清晰帧。
void MainPage::OnImageManipCompleted(Platform::Object^, Windows::UI::Xaml::Input::ManipulationCompletedRoutedEventArgs^)
{
    // Apotheosis (d982774): reset the nested-scroll gesture state regardless of pinch/pan — bumping
    // the generation also drops any WebCoreIsScrollableAt answer for this gesture that is still in
    // flight when it lands (OnImageManipStarted checks it against m_nestedScrollGen).
    // Apotheosis (review 2026-09-03): flush first — the answer may never have arrived, and buffered
    // finger movement must reach the page rather than vanish with the gesture.
    ReplayPendingPan();
    // Apotheosis (drag as pointer events): finger left the glass — the page must get its mouseup,
    // whether or not the press has even been answered yet (PumpDrag orders it after the press).
    if (m_dragActive || m_dragPressPending || m_dragBusy) {
        m_dragReleasePending = true;
        PumpDrag();
    }
    ++m_nestedScrollGen;
    m_nestedScrollState = NestedScrollState::Unknown;
    if (!m_pinching) return;
    m_pinching = false;
    float live = m_liveScale;
    // 钳到引擎区间 + 吸附 1:1（见 SnapAndClampPageScale）。没有吸附时，捏回去总差百分之几，
    //   页面永远停在“差不多但不是原始大小”的状态上，且误差每次捏合继续累积。
    float newScale = SnapAndClampPageScale(m_pageScale * live);
    // The focal was converted to engine pixels once, when the anchor was fixed (SetPinchAnchor):
    //   it is the very point the preview transform is centred on, so the engine frame lands
    //   exactly where the preview showed it.
    float targetLive = (m_pageScale > 0.0f) ? newScale / m_pageScale : 1.0f;
    float gap = (targetLive > live) ? (targetLive - live) : (live - targetLive);
    if (m_zoomTransform != nullptr && gap > 0.005f) {
        SpringBackZoom(targetLive, newScale);   // commits when the animation is done
        return;
    }
    m_liveScale = 1.0f;
    PinchCommit(newScale, m_focalPx, m_focalPy);
}

// 把缩放提交给引擎线程:WebCoreSetPageScale → 新清晰帧;回 UI 后更新已提交尺度 + 复位 RenderTransform + 显示。
void MainPage::PinchCommit(float newScale, int focalX, int focalY)
{
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = ++m_opSeq;
    bool present = m_gpuPresent;
    WebEngine::instance().post([disp, self, newScale, focalX, focalY, mySeq, present]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        try { rc = WebCoreSetPageScale(newScale, focalX, focalY, rgba->data()); } catch (...) { rc = -1000; }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rgba, rcCopy, newScale, mySeq, present]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_opSeq != mySeq) return;            // 被新操作取代,丢弃迟到帧
                if (rcCopy == 0) {
                    s->m_pageScale = newScale;
                    s->PresentSoftwareFrame(rgba);   // present 模式:引擎已 swapBuffers 到 GpuPanel,内部自跳过
                }
                // 复位实时变换(新帧已是按新尺度渲染的清晰图;变换归一,避免叠加二次缩放)。
                // Apotheosis (instant pan): the page scale changed, so the cached scroll bounds are
                // stale — and the pan remainder was already dropped when the pinch started.
                s->m_zoomTransform = nullptr;
                s->m_panRemX = 0; s->m_panRemY = 0;
                if (s->m_panTranslate != nullptr) { s->m_panTranslate->X = 0.0; s->m_panTranslate->Y = 0.0; }
                s->m_scrollStateValid = false;
                if (s->m_presentGroup != nullptr) s->m_presentGroup->Children->Clear();
                s->GpuPanel->RenderTransform = nullptr;
                s->RenderImage->RenderTransform = nullptr;
                s->StartLiveMode();
            }));
        } catch (...) {}
    });
}

// ---- GPU 路径1 探针 ----
void MainPage::OnGpuPanelLoaded(Platform::Object^, RoutedEventArgs^)
{
    static bool s_done = false;
    if (s_done) return;   // 只跑一次
    s_done = true;
    // Apotheosis (M4): 走"GPU 优先启动"(m_pendingFirstNav 非空)时不跑三角形探针 —— 探针在同一个
    //   SwapChainPanel 上自建 EGL 窗口表面并常驻(GpuProbe.cpp 末尾故意不销毁),会和紧随其后的
    //   WebCoreGpuInit 窗口表面抢 ISwapChainPanelNative;原流程两者相隔十几秒才不打架。
    m_gpuPanelLoadedSeen = true;   // 诊断:真机上这个事件到底来不来(见 startup 日志行)
    if (m_pendingFirstNav.empty()) {
        try { RunGpuProbe(GpuPanel, ref new String(LocalStateDir().c_str())); } catch (...) {}
        return;
    }
    // Apotheosis (2026-09-03 崩溃修复): Loaded 到达不代表面板已经过 arrange——真机上这个事件带着
    //   ActualWidth/Height 仍是 0 到达过,是崩溃根因之一(见 HookGpuPanelForStartup 注释)。只有此刻
    //   尺寸已经是真的才在这里直接起 GPU;否则交给 SizeChanged 或 2s 兜底定时器。
    if (GpuPanel && GpuPanel->ActualWidth > 0.0 && GpuPanel->ActualHeight > 0.0) {
        if (m_gpuSizeWaitTimer) m_gpuSizeWaitTimer->Stop();
        StartupGpuThenNav();
    }
}

// Apotheosis (M4): 页面 Loaded —— 可视树已建,GpuPanel 已进树,是"起 GPU"的保底触发源
// (面板 Loaded/SizeChanged 万一不来也有它)。
void MainPage::OnPageLoadedForGpu(Platform::Object^, RoutedEventArgs^)
{
    m_pageLoadedSeen = true;
    if (m_pendingFirstNav.empty()) return;
    // Apotheosis (2026-09-03 崩溃修复): 同 OnGpuPanelLoaded——页面 Loaded 也不能证明 GpuPanel 已经
    //   arrange 过(它是页面里的一个子元素),同样先查真实尺寸,查不到就让 SizeChanged / 2s 兜底接手。
    if (GpuPanel && GpuPanel->ActualWidth > 0.0 && GpuPanel->ActualHeight > 0.0) {
        if (m_gpuSizeWaitTimer) m_gpuSizeWaitTimer->Stop();
        StartupGpuThenNav();
    }
}

// Apotheosis (M4): 面板拿到非零尺寸 = ANGLE 可以在它上面建窗口表面 → 起 GPU,再发第一次导航。
void MainPage::OnGpuPanelSizeChanged(Platform::Object^, Windows::UI::Xaml::SizeChangedEventArgs^ e)
{
    if (m_pendingFirstNav.empty()) return;   // 导航已发出(GPU 路径或兜底)
    if (e->NewSize.Width <= 0.0f || e->NewSize.Height <= 0.0f) return;
    if (m_gpuSizeWaitTimer) m_gpuSizeWaitTimer->Stop();   // 真实尺寸到了,2s 兜底不用再等
    StartupGpuThenNav();
}

// ============================================================================
// Apotheosis (2026-09-03 崩溃修复): GPU-first 起 GPU 前的尺寸门槛,两处触发点共用
// (构造期 GPU-first 启动块 + NavigateTo 里"首次输入网址"拦截块)。
//
// 根因:两处触发点都曾经在把 GpuPanel 设为 Visible 后立刻(或靠 Loaded 事件立刻)调
// StartupGpuThenNav()→EnableGpu()→WebCoreGpuInit(GpuPanel 包成的 EGLNativeWindowTypeProperty,...),
// 完全不管此刻 GpuPanel->ActualWidth/ActualHeight 是不是还是 0(Loaded 触发时面板可能还没走完首次
// arrange;真机 mem.txt 记录过 "startup gpu-first (panel 0x0) ... panelLoaded=1")。ANGLE 就在一个
// 从未 arrange 过的 SwapChainPanel 上建好了窗口表面;紧接着面板真正完成布局、触发它*第一次*真实的
// SizeChanged 时,XAML 内部重建/resize 该面板的合成 swapchain,libGLESv2.dll 里引用的还是那个没建
// 完整的原生窗口状态 → SEH access violation fault=0(crash.txt: pc 落在 libGLESv2.dll,栈上是
// Windows.UI.Xaml SwapChainPanel 的事件回调)。
//
// 修法:只在 GpuPanel 报出真实(非零)尺寸后才起 GPU。已经有尺寸就立即起;否则挂 SizeChanged
// (只挂一次,m_gpuSizeHandlerWired 去重)等它到来。~2s 内还没等到 → 按老行为起 GPU(不然万一某些
// 布局路径永远不给非零尺寸,启动会卡死),但至少把"用 0x0 面板起 GPU"的窗口从"必然"降到"少见兜底"。
// ============================================================================
void MainPage::HookGpuPanelForStartup()
{
    if (!GpuPanel) { StartupGpuThenNav(); return; }   // 没有面板可等,直接走老路(理论上不会发生)
    if (GpuPanel->ActualWidth > 0.0 && GpuPanel->ActualHeight > 0.0) {
        // 尺寸已经是真的(例如面板早被别的路径 arrange 过)——不用等,直接起。
        StartupGpuThenNav();
        return;
    }
    GpuPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;   // 先可见,才谈得上 arrange/SizeChanged
    if (!m_gpuSizeHandlerWired) {
        m_gpuSizeHandlerWired = true;
        GpuPanel->SizeChanged += ref new Windows::UI::Xaml::SizeChangedEventHandler(this, &MainPage::OnGpuPanelSizeChanged);
    }
    if (!m_gpuSizeWaitTimer) {
        m_gpuSizeWaitTimer = ref new Windows::UI::Xaml::DispatcherTimer();
        Windows::Foundation::TimeSpan sts; sts.Duration = 20000000LL;   // 2s(100ns 单位)
        m_gpuSizeWaitTimer->Interval = sts;
        m_gpuSizeWaitTimer->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(this, &MainPage::OnGpuSizeWaitTimer);
    }
    m_gpuSizeWaitTimer->Stop();
    m_gpuSizeWaitTimer->Start();
}

// 2s 到点还没等到 GpuPanel 的真实尺寸 —— 别把启动卡死。
//
// Apotheosis (review 2026-09-03): this used to call StartupGpuThenNav() unconditionally, which is
// exactly the crash HookGpuPanelForStartup exists to prevent — WebCoreGpuInit on a panel that is
// still 0×0, whose first real SizeChanged then makes XAML rebuild the swapchain under ANGLE's
// half-built native window (SEH fault=0 inside libGLESv2.dll). The 2 s fallback reintroduced it
// on every startup where the size never arrived. It must not: if the panel still has no size,
// send the pending navigation on the software path instead (StartPendingFirstNav). GPU is not
// lost, only deferred — m_gpuAutoTried is untouched on this path, so OnNavDone's "default GPU"
// branch enables it once that first page has loaded and the panel has long since been arranged.
void MainPage::OnGpuSizeWaitTimer(Platform::Object^, Platform::Object^)
{
    if (m_gpuSizeWaitTimer) m_gpuSizeWaitTimer->Stop();
    if (m_pendingFirstNav.empty()) return;   // 导航已经用别的路径发出去了
    bool sized = false;
    try { sized = GpuPanel && GpuPanel->ActualWidth > 0.0 && GpuPanel->ActualHeight > 0.0; } catch (...) { sized = false; }
    if (sized) {
        StartupGpuThenNav();   // size did arrive, just no SizeChanged for it — the normal GPU-first path
        return;
    }
    WriteMemLog("startup gpu-wait timeout (panel " + GpuPanelSizeStr() + ") -> software first paint"
                " pageLoaded=" + std::string(m_pageLoadedSeen ? "1" : "0")
                + " panelLoaded=" + (m_gpuPanelLoadedSeen ? "1" : "0")
                + " url=" + WideToUtf8(m_pendingFirstNav));
    StartPendingFirstNav();
}

// ---- 输入法/屏幕键盘 ----
void MainPage::OpenKeyboard()
{
    m_imeOpen = true;
    m_imeSyncing = true;
    ImeBox->Text = ref new String(L"");
    m_lastImeText.clear();
    m_imeSyncing = false;
    ImeBox->Focus(Windows::UI::Xaml::FocusState::Programmatic);   // 聚焦隐藏 TextBox → 唤起屏幕键盘
}
void MainPage::CloseKeyboard()
{
    m_imeOpen = false;
    // 地址栏键盘并不经过 ImeBox，不能因 m_imeOpen=false 而漏掉 TryHide；否则菜单关闭后会重新露出。
    try { Windows::UI::ViewManagement::InputPane::GetForCurrentView()->TryHide(); } catch (...) {}
}

// Apotheosis: TryHide() alone is not enough — a still-focused UrlBox can bring the keyboard right
//   back on the next unrelated tap (any control regaining focus re-evaluates the input pane). Move
//   focus onto the page itself (IsTabStop set true in the constructor — Grid has no such property,
//   so this is done as a plain Control member instead of a MainPage.xaml attribute) so nothing
//   editable is focused while the action menu / settings / tab switcher sit on top of it.
void MainPage::DismissKeyboardForOverlay()
{
    m_urlFocused = false;
    SetUrlEditingChrome(false);   // 焦点没真的离开 UrlBox → LostFocus 不会触发,手动还原刷新/停止键
    CloseKeyboard();
    try { this->Focus(Windows::UI::Xaml::FocusState::Programmatic); } catch (...) {}
}
void MainPage::OnImeTextChanged(Platform::Object^, Windows::UI::Xaml::Controls::TextChangedEventArgs^)
{
    // 诊断埋点(imedebug.txt 存在才记,见 ImeDebugEnabled):记录本回调是否触发 + 门控状态 + 文本长度。
    // 若打字后此文件无新记录 → OnImeTextChanged 没触发 → 隐藏 ImeBox 收不到 IME 文本(UI 层问题);
    // 若有记录但输入框没字 → 引擎层(看 SendKeyToEngine 写的 rc:kErrNoDocument=canEdit 丢焦点)。
    if (ImeDebugEnabled()) {
        try {
            std::wstring d = LocalStateDir();
            if (!d.empty()) {
                std::ofstream f(WideToUtf8(d) + "\\imedebug.txt", std::ios::app | std::ios::binary);
                if (f) { std::string s = "TC open=" + std::to_string(m_imeOpen) + " sess=" + std::to_string(m_sessionActive)
                    + " sync=" + std::to_string(m_imeSyncing) + " textLen=" + std::to_string(ImeBox->Text ? ImeBox->Text->Length() : 0) + "\n"; f.write(s.data(), s.size()); }
            }
        } catch (...) {}
    }
    if (m_imeSyncing || !m_imeOpen || !m_sessionActive) return;
    std::wstring cur = ImeBox->Text ? std::wstring(ImeBox->Text->Data()) : L"";
    std::wstring prev = m_lastImeText;
    if (cur == prev) return;
    if (cur.size() > prev.size() && cur.compare(0, prev.size(), prev) == 0) {
        SendKeyToEngine(0, ref new String(cur.substr(prev.size()).c_str()));   // 末尾追加
    } else if (cur.size() < prev.size() && prev.compare(0, cur.size(), cur) == 0) {
        int n = static_cast<int>(prev.size() - cur.size());
        for (int i = 0; i < n; ++i) SendKeyToEngine(2, nullptr);   // 末尾退格
    } else {
        // 复杂编辑/IME 重排:简化为整体替换(退完旧的再插新的)。
        for (size_t i = 0; i < prev.size(); ++i) SendKeyToEngine(2, nullptr);
        if (!cur.empty()) SendKeyToEngine(0, ref new String(cur.c_str()));
    }
    m_lastImeText = cur;
}
void MainPage::OnImeKeyDown(Platform::Object^, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e)
{
    if (!m_imeOpen) return;
    if (e->Key == Windows::System::VirtualKey::Enter) {
        e->Handled = true;
        SendKeyToEngine(1, nullptr);   // 回车(可能触发表单提交导航)
        m_imeSyncing = true; ImeBox->Text = ref new String(L""); m_imeSyncing = false; m_lastImeText.clear();
    } else if (e->Key == Windows::System::VirtualKey::Back && m_lastImeText.empty()) {
        SendKeyToEngine(2, nullptr);   // 缓冲空时退格(TextChanged 不触发)
    }
}
void MainPage::SendKeyToEngine(int kind, Platform::String^ text)
{
    if (!m_sessionActive) return;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    std::string utf8 = (kind == 0 && text) ? ToUtf8(text) : std::string();
    // 回车可能触发表单提交导航 → 像点击一样领新操作令牌(作废在途实时/滚动帧);普通键只读令牌。
    // 回调检查令牌:期间发生导航/切标签/看门狗复位后,迟到的按键帧被丢弃,不再盖掉新页面(此前无防护)。
    unsigned long long mySeq = (kind == 1) ? ++m_opSeq : m_opSeq;
    bool present = m_gpuPresent;
    WebEngine::instance().post([disp, self, kind, utf8, mySeq, present]() {
        auto rgba = AcquireEngineBuffer(present);
        int rc = -999;
        try {
            if (kind == 0) rc = WebCoreTypeText(utf8.c_str(), rgba->data());
            else if (kind == 1) rc = WebCoreKeyAction(1, rgba->data());
            else rc = WebCoreKeyAction(0, rgba->data());
        } catch (...) { rc = -1000; }
        // 诊断(imedebug.txt 存在才记):rc=-6/kErrNoDocument → 打字时 canEdit 为 false=丢了可编辑焦点。
        if (ImeDebugEnabled()) {
            char edbg[256] = ""; try { WebCoreEditDebug(edbg, sizeof edbg); } catch (...) {}
            try { std::wstring dd = LocalStateDir(); if (!dd.empty()) { std::ofstream f(WideToUtf8(dd) + "\\imedebug.txt", std::ios::app | std::ios::binary); if (f) { std::string s = "  SK kind=" + std::to_string(kind) + " rc=" + std::to_string(rc) + " [" + edbg + "]\n"; f.write(s.data(), s.size()); } } } catch (...) {}
        }
        std::wstring navUrl, title;
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        if (rc == 0 && kind == 1) {   // 回车可能导航 → 取新 url/title/链接
            char t[512] = ""; WebCoreGetTitle(t, sizeof t); title = ToWide(t);
            char u[1024] = ""; WebCoreGetUrl(u, sizeof u); navUrl = ToWide(u);
            int lc = WebCoreGetLinkCount();
            for (int i = 0; i < lc; ++i) { int lx=0,ly=0,lw=0,lh=0; char lu[1200]=""; if (WebCoreGetLink(i,&lx,&ly,&lw,&lh,lu,sizeof lu)) { Harness::PageLink pl; pl.x=lx; pl.y=ly; pl.w=lw; pl.h=lh; pl.url=Utf8ToWide(lu); links->push_back(std::move(pl)); } }
        }
        auto navW = std::make_shared<std::wstring>(navUrl); auto titleW = std::make_shared<std::wstring>(title);
        int rcCopy = rc; int kindCopy = kind;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rgba, rcCopy, kindCopy, navW, titleW, links, mySeq]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_opSeq != mySeq) return;   // 被导航/点击/看门狗取代,丢弃迟到按键帧
                if (rcCopy != 0) return;
                if (kindCopy == 1 && !navW->empty() && std::wstring(navW->c_str()) != s->m_currentUrl) {
                    s->ApplyEngineFrame(rgba, ref new String(titleW->c_str()), ref new String(navW->c_str()), links);   // 回车导航:同步地址栏/历史
                    s->CloseKeyboard();
                } else {
                    s->PresentSoftwareFrame(rgba);
                    s->m_lastFrameHash = 0;
                    s->StartLiveMode();   // 打字后重启实时循环 → 后续帧把输入内容再合成/呈现一次(防单帧合成漏掉新文字)
                }
            }));
        } catch (...) {}
    });
}

// ---- 实时渲染循环:低帧率驱动引擎推进动画/SPA 渐进挂载 ----
void MainPage::StartLiveMode()
{
    if (!m_sessionActive || !m_appForeground) return;
    if (Drawer->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    if (ActionMenu->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    if (SettingsPage->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    if (TabSwitcher->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    m_liveBusy = false;        // 重置(若上次 RunAsync 抛/后台早退卡住,这里恢复)
    m_liveBusyAge = 0;
    m_liveStaticTicks = 0;
    m_liveTotalTicks = 0;
    if (!m_liveTimer) {
        m_liveTimer = ref new Windows::UI::Xaml::DispatcherTimer();
        m_liveTimer->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(this, &MainPage::OnLiveTick);
    }
    // 每次启动都恢复快帧率(撤销之前永久动画的降速)。
    Windows::Foundation::TimeSpan ts; ts.Duration = 2000000LL;   // 200ms(100ns 单位)≈ 5fps
    m_liveTimer->Interval = ts;
    m_liveTimer->Start();
}
void MainPage::StopLiveMode()
{
    if (m_liveTimer) m_liveTimer->Stop();
}
void MainPage::OnLiveTick(Platform::Object^, Platform::Object^)
{
    if (!m_sessionActive || !m_appForeground || m_loading || m_interacting) return;
    if (Drawer->Visibility == Windows::UI::Xaml::Visibility::Visible
        || ActionMenu->Visibility == Windows::UI::Xaml::Visibility::Visible
        || SettingsPage->Visibility == Windows::UI::Xaml::Visibility::Visible
        || TabSwitcher->Visibility == Windows::UI::Xaml::Visibility::Visible) { StopLiveMode(); return; }
    if (m_liveBusy) {                      // 上一帧引擎任务还没回
        // Apotheosis (M4): perf.csv showed a live tick costing 1-4 s on github.com. With the
        // old 5-tick (~1 s) self-heal a new tick was queued on the engine thread every second
        // while the previous one was still running, so the FIFO grew without bound and every
        // navigation/scroll queued behind minutes of ticks ("load timeout", eventually OOM).
        // Only self-heal after 30 s - that is a genuinely lost RunAsync, not a slow frame.
        if (++m_liveBusyAge < 150) return;
        m_liveBusy = false;                // 卡过久 → RunAsync 很可能丢了,自愈不死循环
    }
    m_liveBusyAge = 0;
    m_liveBusy = true;
    unsigned long long mySeq = m_opSeq;    // 只读不自增:实时帧是被动的,绝不能作废正在进行的真操作
    bool present = m_gpuPresent;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    WebEngine::instance().post([disp, self, mySeq, present]() {
        MainPage^ s0 = self.Get();
        if (!s0 || !s0->m_appForeground) return;   // 已切后台:别在 PLM 冻结风险下跑 JS+绘制(m_liveBusy 由恢复时 StartLiveMode 清)
        auto rgba = AcquireEngineBuffer(present);
        int rc = -999; unsigned hash = 0; int pending = 0;
        try { rc = WebCoreLiveTick(rgba->data()); if (rc == 0) { hash = WebCoreGetFrameHash(); pending = WebCoreGetPendingResourceCount(); } } catch (...) { rc = -1000; }
        // Apotheosis (MEMORY-PLAN.md §3/§4): sample here, on the engine thread, not in the UI
        // continuation below — WebCoreGetMemoryStats()/WebCoreSetMemoryPressure() must never be
        // called from the UI thread, and the UI thread must never wait on the engine.
        SampleMemoryPressure();
        if (g_perfLogEnabled && (++g_memTickCount % 50) == 0)
            WriteMemLog("mem-tick n=" + std::to_string(g_memTickCount) + " " + MemSnapshot() + EngineMemStats());
        int rcCopy = rc; unsigned hashCopy = hash; int pendingCopy = pending;
        try {
            disp->RunAsync(CoreDispatcherPriority::Low,
                ref new DispatchedHandler([self, rgba, rcCopy, hashCopy, pendingCopy, mySeq, present]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    s->m_liveBusy = false;
                    if (s->m_opSeq != mySeq) return;   // 期间发生了导航/滚动/点击/超时 → 丢弃这帧旧像素(防闪回旧页)
                    if (!s->m_sessionActive || s->m_loading || s->m_interacting || !s->m_appForeground) return;
                    if (rcCopy != 0) {
                        if (rcCopy == -12 || rcCopy == -14) {   // 会话没了:停帧 + 收起按钮
                            s->m_sessionActive = false;
                            s->ScrollFab->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                            s->StopLiveMode();
                        }
                        return;
                    }
                    // Apotheosis (M4): the mem-tick line moved to the engine-thread part of this
                    // tick, where the engine-side numbers can be read (EngineMemStats()).
                    if (hashCopy == s->m_lastFrameHash) {        // 画面没变:连续静止则停帧省电
                        if (pendingCopy > 0) {
                            s->m_liveStaticTicks = 0;            // 仍有图片/子资源在途:继续 tick,等待完成回调和解码
                            int ticks = ++s->m_liveTotalTicks;
                            if (ticks == 150 && s->m_liveTimer) {
                                Windows::Foundation::TimeSpan slow; slow.Duration = 10000000LL;   // 1s ≈ 1fps
                                s->m_liveTimer->Interval = slow;
                            }
                            if (ticks >= 300)
                                s->StopLiveMode();
                            return;
                        }
                        if (++s->m_liveStaticTicks >= 40) s->StopLiveMode();   // ~8s 静止 → 停,给慢图片/解码留余量
                        return;
                    }
                    s->m_liveStaticTicks = 0;
                    s->m_lastFrameHash = hashCopy;
                    s->PresentSoftwareFrame(rgba);
                    // 永久动画防失控:连续动画超 ~150 帧(30s)无交互 → 降到 ~1fps(不硬停,免得动画卡死);
                    // 任何交互/导航/滚动都经 StartLiveMode 重置计数并恢复 200ms。
                    if (++s->m_liveTotalTicks == 150 && s->m_liveTimer) {
                        Windows::Foundation::TimeSpan slow; slow.Duration = 10000000LL;   // 1s ≈ 1fps
                        s->m_liveTimer->Interval = slow;
                    }
                }));
        } catch (...) {}
    });
}

// ---- 工具栏事件 ----
void MainPage::OnGo(Platform::Object^, RoutedEventArgs^) { NavigateTo(NormalizeUrl(UrlBox->Text), true); }
void MainPage::OnUrlKeyDown(Platform::Object^, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e)
{
    if (e->Key == Windows::System::VirtualKey::Enter) NavigateTo(NormalizeUrl(UrlBox->Text), true);
}
void MainPage::OnHome(Platform::Object^, RoutedEventArgs^) { NavigateTo(ref new String(g_homeUrl.c_str()), true); }
void MainPage::OnBack(Platform::Object^, RoutedEventArgs^)
{
    if (m_loading || m_navIndex <= 0) return;
    --m_navIndex;
    NavigateTo(ref new String(m_navStack[m_navIndex].c_str()), false);
}
void MainPage::OnForward(Platform::Object^, RoutedEventArgs^)
{
    if (m_loading || m_navIndex >= (int)m_navStack.size() - 1) return;
    ++m_navIndex;
    NavigateTo(ref new String(m_navStack[m_navIndex].c_str()), false);
}

// 实体返回键:层级优先关浮层 → 浏览器后退 → 否则交系统(e->Handled 保持 false → 最小化/退出)。
void MainPage::OnHardwareBack(Platform::Object^, Windows::UI::Core::BackRequestedEventArgs^ e)
{
    using V = Windows::UI::Xaml::Visibility;
    if (OobePanel && OobePanel->Visibility == V::Visible) { e->Handled = true; return; }   // 选语言前拦住,别退出
    if (SuggestPanel && SuggestPanel->Visibility == V::Visible) { HideSuggestions(); e->Handled = true; return; }
    if (FindBar && FindBar->Visibility == V::Visible) { OnFindClose(nullptr, nullptr); e->Handled = true; return; }
    if (ActionMenu && ActionMenu->Visibility == V::Visible) { HideActionMenu(); e->Handled = true; return; }
    if (TabSwitcher && TabSwitcher->Visibility == V::Visible) { HideTabSwitcher(); e->Handled = true; return; }
    if (Drawer && Drawer->Visibility == V::Visible) { HideDrawer(); e->Handled = true; return; }
    if (SettingsPage && SettingsPage->Visibility == V::Visible) { HideSettings(); e->Handled = true; return; }
    if (m_navIndex > 0 && !m_loading) { OnBack(nullptr, nullptr); e->Handled = true; return; }
}
void MainPage::OnMenu(Platform::Object^, RoutedEventArgs^) { ShowActionMenu(); }

// UA 切换:手机/桌面。切引擎 UA 后重载当前页生效。(抽屉头部按钮)
void MainPage::OnToggleUA(Platform::Object^, RoutedEventArgs^)
{
    HideDrawer();
    DoToggleUA();
}

// GPU 合成开关(M2):一次性开启(引擎侧 g_gpuActive 无 teardown,重启回软件)。开启 = 引擎线程
// WebCoreGpuInit(nullptr=离屏)成功 → g_gpuActive=true → 重载当前页 → buildSession 开合成 → 经
// TextureMapper 合成到离屏纹理、readback 出像素(仍走 WriteableBitmap 显示)。结果写 gpuinit.txt 供真机回报。
// GPU 朝向标签:🖥 GPU·<HV/H/V/->(供真机循环时看当前组合并回报)。
static Platform::String^ GpuOrientLabel(int orient)
{
    const wchar_t* tag = (orient == 0) ? L"-" : (orient == 1) ? L"H" : (orient == 2) ? L"V" : L"HV";
    return ref new Platform::String((std::wstring(L"\U0001F5A5 GPU·") + tag).c_str());   // 🖥 GPU·HV
}

// GPU 合成开关(M2):首点 = 引擎线程 WebCoreGpuInit(离屏)→ 成功后重载当前页(buildSession 开合成→
//   经 TextureMapper 合成 readback 出像素)。已开后每点一次 = 循环 4 种 readback 朝向(none/H/V/HV)并重绘
//   当前帧——真机朝向经验未定,点到画面正常那个,把标签(H/V/HV/-)告诉我即可定死。重启回软件。
void MainPage::OnToggleGpu(Platform::Object^, RoutedEventArgs^)
{
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);

    if (!m_gpuOn) { HideDrawer(); EnableGpu(); return; }   // 首次开启 → 统一走 EnableGpu(含崩溃环路保护)

    // 已开(朝向已定 none):再点 = 抓合成图层树诊断 → 写 LocalState\layertree.txt + 标题显示关键标量
    //   (scrollPos/contents/view/docBg/usesCompositing),供定位"背景丢失 / 不能滚动"。
    HideDrawer();
    WebEngine::instance().post([disp, self]() {
        auto buf = std::make_shared<std::vector<char>>(65536, 0);
        try { WebCoreGpuLayerInfo(buf->data(), (int)buf->size()); } catch (...) {}
        std::string info(buf->data());
        try {
            std::wstring d = LocalStateDir();
            if (!d.empty()) {
                std::ofstream f(WideToUtf8(d) + "\\layertree.txt", std::ios::binary | std::ios::trunc);
                if (f) f.write(info.data(), info.size());
            }
        } catch (...) {}
        std::string head = info.substr(0, info.find('\n'));
        std::wstring headW = Utf8ToWide(head);
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, headW]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    s->TitleText->Text = ref new Platform::String(headW.c_str());   // 标题临时显示诊断首行
                }));
        } catch (...) {}
    });
}

// ---- 抽屉 ----
void MainPage::OnDrawerClose(Platform::Object^, RoutedEventArgs^) { HideDrawer(); }
void MainPage::OnTabFav(Platform::Object^, RoutedEventArgs^)  { ShowDrawer(DrawerTab::Favorites); }
void MainPage::OnTabHist(Platform::Object^, RoutedEventArgs^) { ShowDrawer(DrawerTab::History); }
void MainPage::OnTabDl(Platform::Object^, RoutedEventArgs^)   { ShowDrawer(DrawerTab::Downloads); }

void MainPage::OnPrimaryAction(Platform::Object^, RoutedEventArgs^)
{
    if (m_tab == DrawerTab::Favorites) {
        ToggleBookmark();   // 收藏/取消(内部含保存 + 抽屉可见时刷新列表)
        ActionBtn->Content = (!m_currentUrl.empty() && IsBookmarked(m_currentUrl)) ? L8(L"★ 取消收藏", L"★ Remove bookmark") : L8(L"★ 收藏此页", L"★ Bookmark this");
    } else if (m_tab == DrawerTab::History) {
        m_historyList.clear(); SaveHistory(); RebuildDrawerList();
    } else {
        // 下载:下载当前页地址
        if (!m_currentUrl.empty() && m_currentUrl != L"about:home")
            StartDownload(ref new String(m_currentUrl.c_str()));
    }
}

void MainPage::ShowDrawer(DrawerTab tab)
{
    m_tab = tab;
    HideSuggestions();
    Drawer->Visibility = Windows::UI::Xaml::Visibility::Visible;
    // 主操作按钮文案随标签变化
    if (tab == DrawerTab::Favorites)
        ActionBtn->Content = (!m_currentUrl.empty() && IsBookmarked(m_currentUrl)) ? L8(L"★ 取消收藏", L"★ Remove bookmark") : L8(L"★ 收藏此页", L"★ Bookmark this");
    else if (tab == DrawerTab::History)
        ActionBtn->Content = L8(L"\U0001F5D1 清空", L"\U0001F5D1 Clear");
    else
        ActionBtn->Content = L8(L"↓ 下载此页", L"↓ Download page");
    RebuildDrawerList();
    StopLiveMode();   // 抽屉盖住网页,暂停实时渲染省电
}
void MainPage::HideDrawer()
{
    Drawer->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    StartLiveMode();   // 抽屉关闭,恢复实时渲染
}

// 构建抽屉列表项(标题 + URL,可点导航;收藏/下载可删)
static Border^ MakeRow(Platform::String^ title, Platform::String^ sub, Color titleColor)
{
    auto sp = ref new StackPanel();
    sp->Margin = Thickness(15, 12, 15, 12);
    auto t = ref new TextBlock();
    t->Text = title; t->FontSize = 20; t->Foreground = ref new SolidColorBrush(titleColor);
    t->TextTrimming = TextTrimming::CharacterEllipsis; t->MaxLines = 1;
    auto u = ref new TextBlock();
    u->Text = sub; u->FontSize = 14; u->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x91, 0xA2, 0xAD));
    u->TextTrimming = TextTrimming::CharacterEllipsis; u->MaxLines = 1; u->Margin = Thickness(0, 2, 0, 0);
    sp->Children->Append(t); sp->Children->Append(u);
    auto b = ref new Border();
    b->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x17, 0x21, 0x29));
    b->BorderBrush = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x26, 0x36, 0x40));
    b->BorderThickness = Thickness(1);
    b->CornerRadius = CornerRadius(10);
    b->Margin = Thickness(0, 0, 0, 8);
    b->Child = sp;
    return b;
}

void MainPage::RebuildDrawerList()
{
    DrawerList->Children->Clear();
    const std::vector<Entry>* list = nullptr;
    if (m_tab == DrawerTab::Favorites) list = &m_bookmarks;
    else if (m_tab == DrawerTab::History) list = &m_historyList;
    else list = &m_downloads;

    if (list->empty()) {
        auto empty = ref new TextBlock();
        empty->Text = (m_tab == DrawerTab::Favorites) ? L8(L"暂无收藏", L"No bookmarks yet") : (m_tab == DrawerTab::History ? L8(L"暂无历史记录", L"No history yet") : L8(L"暂无下载", L"No downloads yet"));
        empty->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x91, 0xA2, 0xAD));
        empty->FontSize = 18; empty->Margin = Thickness(14, 20, 0, 0);
        DrawerList->Children->Append(empty);
        return;
    }

    Platform::Agile<MainPage^> self(this);
    bool isDownloads = (m_tab == DrawerTab::Downloads);
    bool isFav = (m_tab == DrawerTab::Favorites);
    for (size_t i = 0; i < list->size(); ++i) {
        const Entry& e = (*list)[i];
        Platform::String^ titleS = ref new String(e.title.empty() ? e.url.c_str() : e.title.c_str());
        Platform::String^ subS = ref new String((isDownloads ? (e.extra + L"  ·  " + e.url) : e.url).c_str());
        auto row = MakeRow(titleS, subS, ColorHelper::FromArgb(255, 0xF4, 0xF7, 0xF8));

        if (isDownloads) {
            DrawerList->Children->Append(row);
            continue;
        }
        // 点击行 → 导航
        std::wstring u = e.url;
        auto btn = ref new Button();
        btn->Background = ref new SolidColorBrush(Colors::Transparent);
        btn->BorderThickness = Thickness(0);
        btn->Padding = Thickness(0);
        btn->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        btn->HorizontalContentAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        btn->Content = row;
        btn->Click += ref new RoutedEventHandler([self, u](Platform::Object^, RoutedEventArgs^) {
            MainPage^ s = self.Get(); if (!s) return;
            s->HideDrawer();
            s->NavigateTo(ref new String(u.c_str()), true);
        });
        DrawerList->Children->Append(btn);

        // 删除按钮(收藏/历史)
        if (isFav) {
            auto del = ref new Button();
            del->Content = L8(L"✕ 删除收藏", L"✕ Remove");
            del->FontSize = 15;
            del->Background = ref new SolidColorBrush(Colors::Transparent);
            del->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0xD9, 0x30, 0x25));
            del->BorderThickness = Thickness(0);
            del->Margin = Thickness(8, -6, 0, 8);
            del->Click += ref new RoutedEventHandler([self, u](Platform::Object^, RoutedEventArgs^) {
                MainPage^ s = self.Get(); if (!s) return;
                s->m_bookmarks.erase(std::remove_if(s->m_bookmarks.begin(), s->m_bookmarks.end(),
                    [&](const Entry& en) { return en.url == u; }), s->m_bookmarks.end());
                s->SaveBookmarks(); s->RebuildDrawerList();
            });
            DrawerList->Children->Append(del);
        }
    }
}

// ---- 下载 ----
void MainPage::StartDownload(Platform::String^ url)
{
    std::wstring wurl = url->Data();
    // 文件名:URL 最后一段(去 query),空则 index.html
    std::wstring fn = wurl;
    size_t q = fn.find(L'?'); if (q != std::wstring::npos) fn = fn.substr(0, q);
    size_t sl = fn.find_last_of(L'/');
    fn = (sl == std::wstring::npos) ? fn : fn.substr(sl + 1);
    if (fn.empty() || fn.find(L'.') == std::wstring::npos) fn = L"index.html";

    std::wstring dlDir = LocalStateDir() + L"\\Downloads";
    CreateDirectoryW(dlDir.c_str(), nullptr);
    // 文件名去重:同名文件已存在则追加 (1)(2)…,避免覆盖之前下载的文件。
    std::wstring outPath = dlDir + L"\\" + fn;
    if (GetFileAttributesW(outPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring stem = fn, ext;
        size_t dot = fn.find_last_of(L'.');
        if (dot != std::wstring::npos) { stem = fn.substr(0, dot); ext = fn.substr(dot); }
        for (int i = 1; i < 1000; ++i) {
            std::wstring cand = stem + L"(" + std::to_wstring(i) + L")" + ext;
            std::wstring candPath = dlDir + L"\\" + cand;
            if (GetFileAttributesW(candPath.c_str()) == INVALID_FILE_ATTRIBUTES) { fn = cand; outPath = candPath; break; }
        }
    }

    TitleText->Text = ref new String(((g_lang == L"en" ? L"Downloading  " : L"下载中  ") + fn).c_str());
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    std::string u8url = ToUtf8(url);
    std::string u8out = WideToUtf8(outPath);
    std::wstring fnCopy = fn;
    std::wstring urlCopy = wurl;

    // 下载放专用线程,不占用单引擎线程(否则慢下载会阻塞导航最多 120s)。WebCoreDownload 用独立
    // curl_easy 句柄,不设 CURLOPT_SHARE、不碰 WebKit 主线程调度,与渲染并发安全;CA(g_caBytes)
    // 已由引擎线程的 SetupRuntimeEnv 备好(用户触发下载时主页早已加载)。
    std::thread([disp, self, u8url, u8out, fnCopy, urlCopy]() {
        int code = WebCoreDownload(u8url.c_str(), u8out.c_str());
        long long sz = 0;
        try { std::ifstream f(u8out, std::ios::binary | std::ios::ate); if (f) sz = (long long)f.tellg(); } catch (...) {}
        std::wstring status = (code >= 200 && code < 400)
            ? (W8(L"已完成  ", L"Done  ") + std::to_wstring(sz / 1024) + L" KB")
            : (W8(L"失败(", L"Failed (") + std::to_wstring(code) + L")");
        auto st = std::make_shared<std::wstring>(status);
        auto fnC = std::make_shared<std::wstring>(fnCopy);
        auto urlC = std::make_shared<std::wstring>(urlCopy);
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, st, fnC, urlC]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    Entry e; e.url = *urlC; e.title = *fnC; e.extra = *st;
                    s->m_downloads.insert(s->m_downloads.begin(), e);
                    if (s->m_downloads.size() > 100) s->m_downloads.resize(100);
                    s->SaveDownloads();
                    s->TitleText->Text = ref new String((*fnC + L"  " + *st).c_str());
                    if (s->Drawer->Visibility == Windows::UI::Xaml::Visibility::Visible && s->m_tab == DrawerTab::Downloads)
                        s->RebuildDrawerList();
                }));
        } catch (...) {}
    }).detach();
}

// ============================================================================
// 增量1:地址栏(上下文键 Go/刷新/停止 + 安全锁标 + 历史/书签建议下拉)
// 纯 UI 层,不碰 ContentArea 坐标映射 / 引擎交互路径。
// ============================================================================

void MainPage::Reload()
{
    if (m_currentUrl.empty() || m_currentUrl == L"about:home")
        NavigateTo(ref new String(L"about:home"), false);
    else
        NavigateTo(ref new String(m_currentUrl.c_str()), false);
}

// 上下文键:加载中=停止;有未提交输入=Go;否则=刷新当前页。
void MainPage::OnUrlAction(Platform::Object^, RoutedEventArgs^)
{
    if (m_loading) {
        // 停止:作废在途回调(opSeq++),停看门狗,排队关会话取消网络(单引擎线程串行,加载 job 跑完后才执行)。
        ++m_opSeq;
        m_interacting = false;
        if (m_loadWatchdog) m_loadWatchdog->Stop();
        WebEngine::instance().post([]() { try { WebCoreCloseSession(); } catch (...) {} });
        m_sessionActive = false;
        ScrollFab->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        SetLoading(false);
        TitleText->Text = L8(L"已停止", L"Stopped");
        return;
    }
    std::wstring boxText = UrlBox->Text ? std::wstring(UrlBox->Text->Data()) : L"";
    bool pendingEdit = (m_currentUrl == L"about:home") ? !boxText.empty() : (boxText != m_currentUrl);
    HideSuggestions();
    if (pendingEdit) NavigateTo(NormalizeUrl(UrlBox->Text), true);
    else Reload();
}

void MainPage::UpdateUrlActionGlyph()
{
    if (!UrlActionBtn || !UrlActionGlyph) return;
    if (m_loading) { UrlActionGlyph->Text = ref new String(L"\x2715"); return; }   // ✕ 停止
    std::wstring boxText = UrlBox->Text ? std::wstring(UrlBox->Text->Data()) : L"";
    bool pendingEdit = (m_currentUrl == L"about:home") ? !boxText.empty() : (boxText != m_currentUrl);
    UrlActionGlyph->Text = ref new String(pendingEdit ? L"\x2192" : L"\x21BB");     // → Go / ⟳ 刷新
}

// Segoe MDL2 Assets:Lock=E72E,Warning=E7BA。本地/主页留空。
void MainPage::UpdateLockIcon()
{
    if (!LockIcon) return;
    const std::wstring& u = m_currentUrl;
    if (u.empty() || u == L"about:home" || u.rfind(L"about:", 0) == 0) {
        LockIcon->Text = ref new String(L"");
    } else if (u.rfind(L"https://", 0) == 0) {
        LockIcon->Text = ref new String(L"\xE72E");
        LockIcon->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x5C, 0xB8, 0x5C));
    } else if (u.rfind(L"http://", 0) == 0) {
        LockIcon->Text = ref new String(L"\xE7BA");
        LockIcon->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0xE0, 0xA0, 0x30));
    } else {
        LockIcon->Text = ref new String(L"");
    }
}

void MainPage::OnUrlChanged(Platform::Object^, Windows::UI::Xaml::Controls::TextChangedEventArgs^)
{
    UpdateUrlActionGlyph();
    if (m_urlSyncing) { HideSuggestions(); return; }   // 程序化同步地址栏(导航/回调):绝不弹建议
    if (!m_urlFocused) { HideSuggestions(); return; }   // 没在编辑地址栏:绝不弹(杜绝"莫名其妙弹出")
    std::wstring q = UrlBox->Text ? std::wstring(UrlBox->Text->Data()) : L"";
    if (q.empty()) { HideSuggestions(); return; }
    ShowSuggestions(q);
}

// 编辑地址时把胶囊右侧的刷新/停止键换成白色 ✕(清除),输入框因此拿到整条胶囊的宽度。
void MainPage::SetUrlEditingChrome(bool editing)
{
    using Vis = Windows::UI::Xaml::Visibility;
    if (UrlActionBtn) UrlActionBtn->Visibility = editing ? Vis::Collapsed : Vis::Visible;
    if (UrlClearBtn)  UrlClearBtn->Visibility  = editing ? Vis::Visible   : Vis::Collapsed;
}

// UrlBox 用胶囊右侧那个 ✕,模板自带的清除键就多余了(两个 ✕ 很怪,且占掉输入宽度)。
// 只收 UrlBox 这一个实例(查找条 FindBox 仍保留模板自带的清除键):视觉状态只动 Visibility,
// 所以改 Width/Opacity/命中测试能一直生效。
void MainPage::HideUrlBoxDeleteButton()
{
    if (m_urlDeleteBtnHidden || !UrlBox) return;
    std::function<FrameworkElement^(DependencyObject^)> find = [&](DependencyObject^ node) -> FrameworkElement^ {
        int n = VisualTreeHelper::GetChildrenCount(node);
        for (int i = 0; i < n; ++i) {
            auto child = VisualTreeHelper::GetChild(node, i);
            auto fe = dynamic_cast<FrameworkElement^>(child);
            if (fe && fe->Name == L"DeleteButton") return fe;
            if (auto hit = find(child)) return hit;
        }
        return nullptr;
    };
    auto btn = find(UrlBox);
    if (!btn) return;
    btn->MinWidth = 0; btn->Width = 0; btn->Opacity = 0; btn->IsHitTestVisible = false;
    m_urlDeleteBtnHidden = true;
}

void MainPage::OnUrlClear(Platform::Object^, RoutedEventArgs^)
{
    if (!UrlBox) return;
    UrlBox->Text = ref new String(L"");
    UrlBox->Focus(Windows::UI::Xaml::FocusState::Programmatic);   // 保持编辑态 + 软键盘
}

void MainPage::OnUrlGotFocus(Platform::Object^, RoutedEventArgs^)
{
    m_urlFocused = true;
    HideUrlBoxDeleteButton();
    SetUrlEditingChrome(true);
}
// 不在 LostFocus 里收建议:点建议项会先夺焦再触发其 Click,提前收会取消点击。改由点页面(OnPageTapped)/导航收。
//
// Apotheosis: leaving the field WITHOUT committing (tapped the page, hardware Back, keyboard
//   dismissed) has to undo the edit. Otherwise the box keeps half-typed text that no longer
//   describes what is on screen, and — because the context button is chosen by comparing the
//   box against m_currentUrl — the button stays a "→" (go), so the only way back to reload/stop
//   is to retype the URL. Restore m_currentUrl (empty box on about:home, matching NavigateTo)
//   and let UpdateUrlActionGlyph put ⟳/✕ back. A committed navigation already wrote
//   m_currentUrl and the box together, so nothing is reverted in that case.
void MainPage::OnUrlLostFocus(Platform::Object^, RoutedEventArgs^)
{
    m_urlFocused = false;
    if (UrlBox) {
        std::wstring boxText = UrlBox->Text ? std::wstring(UrlBox->Text->Data()) : L"";
        std::wstring want = (m_currentUrl == L"about:home") ? std::wstring() : m_currentUrl;
        if (boxText != want) {
            m_urlSyncing = true;                       // programmatic write: no suggestion popup
            UrlBox->Text = ref new String(want.c_str());
            m_urlSyncing = false;
        }
    }
    SetUrlEditingChrome(false);
    UpdateUrlActionGlyph();
}

// 历史 + 书签子串匹配(url/title,忽略大小写),去重,最多 8 条。点项即导航。
void MainPage::ShowSuggestions(const std::wstring& query)
{
    if (!SuggestPanel || !SuggestList) return;
    SuggestList->Children->Clear();
    std::wstring ql = query;
    std::transform(ql.begin(), ql.end(), ql.begin(), [](wchar_t c) { return (wchar_t)::towlower(c); });

    std::vector<Entry> matches;
    std::vector<std::wstring> seen;
    auto consider = [&](const std::vector<Entry>& src) {
        for (const auto& e : src) {
            if (matches.size() >= 8) break;
            std::wstring ul = e.url, tl = e.title;
            std::transform(ul.begin(), ul.end(), ul.begin(), [](wchar_t c) { return (wchar_t)::towlower(c); });
            std::transform(tl.begin(), tl.end(), tl.begin(), [](wchar_t c) { return (wchar_t)::towlower(c); });
            if (ul.find(ql) == std::wstring::npos && tl.find(ql) == std::wstring::npos) continue;
            if (std::find(seen.begin(), seen.end(), e.url) != seen.end()) continue;
            seen.push_back(e.url);
            matches.push_back(e);
        }
    };
    consider(m_bookmarks);
    consider(m_historyList);
    if (matches.empty()) { HideSuggestions(); return; }

    Platform::Agile<MainPage^> self(this);
    for (const auto& e : matches) {
        std::wstring u = e.url;
        auto row = MakeRow(ref new String(e.title.empty() ? e.url.c_str() : e.title.c_str()),
                           ref new String(e.url.c_str()),
                           ColorHelper::FromArgb(255, 0xF0, 0xF0, 0xF0));
        auto btn = ref new Button();
        btn->Background = ref new SolidColorBrush(Colors::Transparent);
        btn->BorderThickness = Thickness(0);
        btn->Padding = Thickness(0);
        btn->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        btn->HorizontalContentAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        btn->Content = row;
        btn->Click += ref new RoutedEventHandler([self, u](Platform::Object^, RoutedEventArgs^) {
            MainPage^ s = self.Get(); if (!s) return;
            s->HideSuggestions();
            s->m_urlSyncing = true; s->UrlBox->Text = ref new String(u.c_str()); s->m_urlSyncing = false;
            s->NavigateTo(ref new String(u.c_str()), true);
        });
        SuggestList->Children->Append(btn);
    }
    SuggestPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
}

void MainPage::HideSuggestions()
{
    if (SuggestPanel) SuggestPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
}

// ============================================================================
// 增量2:动作面板(菜单键弹出的 action sheet)+ 分享/复制链接/收藏/UA
// ============================================================================

void MainPage::ShowActionMenu()
{
    HideSuggestions();
    // 收起网页/地址栏输入法并撤销地址栏的“编辑中”状态、真正挪走焦点。硬件 Back 只会关闭当前
    // sheet，不应因此把此前保留焦点的键盘重新唤起。
    DismissKeyboardForOverlay();
    if (ActFavLabel)
        ActFavLabel->Text = (!m_currentUrl.empty() && IsBookmarked(m_currentUrl)) ? L8(L"已收藏", L"Saved") : L8(L"收藏", L"Bookmark");
    if (ActUaLabel)
        ActUaLabel->Text = m_uaMobile ? L8(L"桌面版网站", L"Desktop site") : L8(L"移动版网站", L"Mobile site");
    ActionMenu->Visibility = Windows::UI::Xaml::Visibility::Visible;
    StopLiveMode();   // 面板盖住网页,暂停实时渲染省电
}

void MainPage::HideActionMenu()
{
    ActionMenu->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    StartLiveMode();
}

void MainPage::OnActionScrimTap(Platform::Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^)
{
    HideActionMenu();   // 点遮罩空白处关闭
}

void MainPage::OnSheetTap(Platform::Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    e->Handled = true;  // 点面板本体不冒泡到遮罩(否则点空白区会误关)
}

// 动作分发:读 Button.Tag。先关面板再执行(避免动作触发的 UI 变化被面板挡住)。
void MainPage::OnAction(Platform::Object^ sender, RoutedEventArgs^)
{
    std::wstring t;
    auto btn = dynamic_cast<Button^>(sender);
    if (btn) { auto tag = dynamic_cast<Platform::String^>(btn->Tag); if (tag) t = std::wstring(tag->Data()); }
    HideActionMenu();
    if (t == L"reload") Reload();
    else if (t == L"share") DoShare();
    else if (t == L"copylink") DoCopyLink();
    else if (t == L"bookmark") ToggleBookmark();
    else if (t == L"newtab") NewTab();
    else if (t == L"home") NavigateTo(ref new String(g_homeUrl.c_str()), true);
    else if (t == L"ua") DoToggleUA();
    else if (t == L"find") ShowFindBar();
    else if (t == L"download") { if (!m_currentUrl.empty() && m_currentUrl != L"about:home") StartDownload(ref new String(m_currentUrl.c_str())); }
    else if (t == L"bookmarks") ShowDrawer(DrawerTab::Favorites);
    else if (t == L"history") ShowDrawer(DrawerTab::History);
    else if (t == L"downloads") ShowDrawer(DrawerTab::Downloads);
    else if (t == L"settings") ShowSettings();
}

void MainPage::ToggleBookmark()
{
    if (m_currentUrl.empty() || m_currentUrl == L"about:home") return;
    if (IsBookmarked(m_currentUrl)) {
        m_bookmarks.erase(std::remove_if(m_bookmarks.begin(), m_bookmarks.end(),
            [&](const Entry& e) { return e.url == m_currentUrl; }), m_bookmarks.end());
        TitleText->Text = L8(L"已取消收藏", L"Bookmark removed");
    } else {
        Entry e; e.url = m_currentUrl; e.title = m_currentTitle.empty() ? m_currentUrl : m_currentTitle;
        m_bookmarks.insert(m_bookmarks.begin(), e);
        TitleText->Text = L8(L"已收藏", L"Bookmarked");
    }
    SaveBookmarks();
    if (Drawer->Visibility == Windows::UI::Xaml::Visibility::Visible && m_tab == DrawerTab::Favorites)
        RebuildDrawerList();
}

void MainPage::DoShare()
{
    if (m_currentUrl.empty() || m_currentUrl == L"about:home") { TitleText->Text = L8(L"无可分享内容", L"Nothing to share"); return; }
    try { Windows::ApplicationModel::DataTransfer::DataTransferManager::ShowShareUI(); } catch (...) {}
}

void MainPage::DoCopyLink()
{
    if (m_currentUrl.empty() || m_currentUrl == L"about:home") return;
    try {
        auto dp = ref new Windows::ApplicationModel::DataTransfer::DataPackage();
        dp->SetText(ref new String(m_currentUrl.c_str()));
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
        TitleText->Text = L8(L"已复制链接", L"Link copied");
    } catch (...) {}
}

void MainPage::DoToggleUA()
{
    m_uaMobile = !m_uaMobile;
    if (UaBtn) UaBtn->Content = L8(m_uaMobile ? L"\U0001F4F1 手机UA" : L"\U0001F5A5 桌面UA", m_uaMobile ? L"\U0001F4F1 Mobile UA" : L"\U0001F5A5 Desktop UA");
    int mobile = m_uaMobile ? 1 : 0;
    WebEngine::instance().post([mobile]() { try { WebCoreSetUserAgentMobile(mobile); } catch (...) {} });
    if (!m_currentUrl.empty() && m_currentUrl != L"about:home")
        NavigateTo(ref new String(m_currentUrl.c_str()), false);   // 重载使新 UA 生效
}

// ============================================================================
// 增量3:设置页(搜索引擎/主页/默认UA/缩放/标签模式)+ 清除数据 + 调试导出
// ============================================================================

void MainPage::ApplySettings()
{
    g_searchPrefix = SearchPrefixFor(m_setSearch);
    if (m_defaultZoom < 50) m_defaultZoom = 50;
    if (m_defaultZoom > 200) m_defaultZoom = 200;
    m_uaMobile = !m_setUaDesktop;
    int mobile = m_uaMobile ? 1 : 0;
    std::string ua = WideToUtf8(m_uaCustom);
    WebEngine::instance().post([mobile, ua]() {
        try { WebCoreSetUserAgentMobile(mobile); } catch (...) {}
        try { WebCoreSetUserAgentString(ua.empty() ? nullptr : ua.c_str()); } catch (...) {}   // 自定义 UA(空=清除回退开关)
    });
    UpdateScrollFab();
    ApplyPrefetchSetting();
    ApplyThreadedRasterSetting();            // Apotheosis: DEVELOPER toggle, engine-thread call
    ApplyHideNavBarSetting();                // Apotheosis: DISPLAY toggle, UI thread only
    if (!m_instantPan) InstantPanReset();    // Apotheosis: switching it off must clear a live preview
    if (UaBtn) {
        bool en = (g_lang == L"en");
        UaBtn->Content = ref new String(m_uaMobile ? (en ? L"\U0001F4F1 Mobile UA" : L"\U0001F4F1 手机UA")
                                                   : (en ? L"\U0001F5A5 Desktop UA" : L"\U0001F5A5 桌面UA"));
    }
}

void MainPage::LoadSettings()
{
    std::wstring d = LocalStateDir();
    if (d.empty()) { ApplySettings(); return; }
    std::ifstream f(WideToUtf8(d) + "\\settings.ini", std::ios::binary);
    if (f) {
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "search") m_setSearch = atoi(v.c_str());
            else if (k == "home") g_homeUrl = v.empty() ? L"about:home" : Utf8ToWide(v);
            else if (k == "ua") m_setUaDesktop = (atoi(v.c_str()) != 0);
            else if (k == "zoom") m_defaultZoom = atoi(v.c_str());
            else if (k == "tabmode") m_tabMode = atoi(v.c_str());
            else if (k == "gpudefault") m_gpuDefault = (atoi(v.c_str()) != 0);
            else if (k == "ua_custom") m_uaCustom = Utf8ToWide(v);
            else if (k == "updatecheck") m_updateAuto = (atoi(v.c_str()) != 0);
            else if (k == "prefetch") m_prefetch = atoi(v.c_str());
            else if (k == "scrollfab") m_showScrollFab = (atoi(v.c_str()) != 0);
            else if (k == "instantpan") m_instantPan = (atoi(v.c_str()) != 0);
            else if (k == "threadraster") m_threadedRaster = (atoi(v.c_str()) != 0);
            else if (k == "dragpointer") m_dragPointer = (atoi(v.c_str()) != 0);
            else if (k == "hidenavbar") m_hideNavBar = (atoi(v.c_str()) != 0);
            else if (k == "lang") { g_lang = Utf8ToWide(v); m_langSet = true; }
        }
    }
    if (m_setSearch < 0 || m_setSearch > 4) m_setSearch = 4;   // Apotheosis: unknown index -> Qwant
    if (g_lang != L"en" && g_lang != L"zh") g_lang = L"zh";
    ApplySettings();
}

void MainPage::SaveSettings()
{
    std::wstring d = LocalStateDir();
    if (d.empty()) return;
    std::string s;
    s += "search=" + std::to_string(m_setSearch) + "\n";
    s += "home=" + (g_homeUrl == L"about:home" ? std::string() : WideToUtf8(g_homeUrl)) + "\n";
    s += "ua=" + std::to_string(m_setUaDesktop ? 1 : 0) + "\n";
    s += "zoom=" + std::to_string(m_defaultZoom) + "\n";
    s += "tabmode=" + std::to_string(m_tabMode) + "\n";
    s += "gpudefault=" + std::to_string(m_gpuDefault ? 1 : 0) + "\n";
    s += "ua_custom=" + WideToUtf8(m_uaCustom) + "\n";
    s += "updatecheck=" + std::to_string(m_updateAuto ? 1 : 0) + "\n";
    s += "prefetch=" + std::to_string(m_prefetch) + "\n";
    s += "scrollfab=" + std::to_string(m_showScrollFab ? 1 : 0) + "\n";
    s += "instantpan=" + std::to_string(m_instantPan ? 1 : 0) + "\n";
    s += "threadraster=" + std::to_string(m_threadedRaster ? 1 : 0) + "\n";
    s += "dragpointer=" + std::to_string(m_dragPointer ? 1 : 0) + "\n";
    s += "hidenavbar=" + std::to_string(m_hideNavBar ? 1 : 0) + "\n";
    s += "lang=" + WideToUtf8(g_lang) + "\n";
    std::ofstream f(WideToUtf8(d) + "\\settings.ini", std::ios::binary | std::ios::trunc);
    if (f) f.write(s.data(), s.size());
}

void MainPage::ShowSettings()
{
    HideActionMenu();
    DismissKeyboardForOverlay();   // Apotheosis: the settings page covers the address bar too.
    if (SetLangCombo) SetLangCombo->SelectedIndex = (g_lang == L"en") ? 1 : 0;
    if (SetSearchCombo) SetSearchCombo->SelectedIndex = m_setSearch;
    if (SetHomeBox) SetHomeBox->Text = ref new String(g_homeUrl == L"about:home" ? L"" : g_homeUrl.c_str());
    if (SetUaSwitch) SetUaSwitch->IsOn = m_setUaDesktop;
    if (SetTabModeSwitch) SetTabModeSwitch->IsOn = (m_tabMode == 1);
    if (SetZoomSlider) SetZoomSlider->Value = m_defaultZoom;
    if (SetZoomLabel) SetZoomLabel->Text = ref new String((std::to_wstring(m_defaultZoom) + L"%").c_str());
    if (SetGpuSwitch) SetGpuSwitch->IsOn = m_gpuDefault;
    if (SetUpdateSwitch) SetUpdateSwitch->IsOn = m_updateAuto;
    if (SetPrefetchCombo) SetPrefetchCombo->SelectedIndex = m_prefetch;
    if (SetScrollFabSwitch) SetScrollFabSwitch->IsOn = m_showScrollFab;
    if (SetInstantPanSwitch) SetInstantPanSwitch->IsOn = m_instantPan;
    if (SetThreadedRasterSwitch) SetThreadedRasterSwitch->IsOn = m_threadedRaster;
    if (SetDragPointerSwitch) SetDragPointerSwitch->IsOn = m_dragPointer;
    if (SetHideNavBarSwitch) SetHideNavBarSwitch->IsOn = m_hideNavBar;
    if (SetUaCustomBox) SetUaCustomBox->Text = ref new String(m_uaCustom.c_str());
    // Apotheosis: app version comes from the package manifest, so it can never drift from what
    //   was actually deployed. The engine has no version export (WebCoreDriver.h) — the WebCore
    //   version in the footer is the one the port is pinned to.
    auto pv = Windows::ApplicationModel::Package::Current->Id->Version;
    std::wstring ver = std::to_wstring(pv.Major) + L"." + std::to_wstring(pv.Minor)
                     + L"." + std::to_wstring(pv.Build) + L"." + std::to_wstring(pv.Revision);
    if (VersionText) VersionText->Text = ref new String((W8(L"版本 ", L"Version ") + ver).c_str());
    if (AboutFooterText)
        AboutFooterText->Text = ref new String((L"EdgeHTML Reborn / Apotheosis — App " + ver
                                                + L" — WebKit (WebCore) 2.52.4 — ARM32 UWP").c_str());
    SettingsPage->Visibility = Windows::UI::Xaml::Visibility::Visible;
    StopLiveMode();
}

void MainPage::HideSettings()
{
    // 语言先切:后面的 ApplySettings 才会用新语言刷运行期标签。
    if (SetLangCombo && SetLangCombo->SelectedIndex >= 0)
        SetLanguage(SetLangCombo->SelectedIndex == 1 ? L"en" : L"zh");
    if (SetSearchCombo && SetSearchCombo->SelectedIndex >= 0) m_setSearch = SetSearchCombo->SelectedIndex;
    if (SetHomeBox) {
        std::wstring h = SetHomeBox->Text ? std::wstring(SetHomeBox->Text->Data()) : L"";
        while (!h.empty() && (h.front() == L' ' || h.front() == L'\t')) h.erase(h.begin());
        while (!h.empty() && (h.back() == L' ' || h.back() == L'\t')) h.pop_back();
        if (h.empty() || h == L"about:home") g_homeUrl = L"about:home";
        else { if (h.rfind(L"http", 0) != 0 && h.rfind(L"about:", 0) != 0) h = L"https://" + h; g_homeUrl = h; }
    }
    if (SetUaSwitch) m_setUaDesktop = SetUaSwitch->IsOn;
    if (SetTabModeSwitch) m_tabMode = SetTabModeSwitch->IsOn ? 1 : 0;
    if (SetZoomSlider) m_defaultZoom = (int)(SetZoomSlider->Value + 0.5);
    if (SetGpuSwitch) m_gpuDefault = SetGpuSwitch->IsOn;
    if (SetUpdateSwitch) m_updateAuto = SetUpdateSwitch->IsOn;
    if (SetPrefetchCombo && SetPrefetchCombo->SelectedIndex >= 0) m_prefetch = SetPrefetchCombo->SelectedIndex;
    if (SetScrollFabSwitch) m_showScrollFab = SetScrollFabSwitch->IsOn;
    if (SetInstantPanSwitch) m_instantPan = SetInstantPanSwitch->IsOn;
    if (SetThreadedRasterSwitch) m_threadedRaster = SetThreadedRasterSwitch->IsOn;
    if (SetDragPointerSwitch) m_dragPointer = SetDragPointerSwitch->IsOn;
    if (SetHideNavBarSwitch) m_hideNavBar = SetHideNavBarSwitch->IsOn;
    if (SetUaCustomBox) {
        std::wstring u = SetUaCustomBox->Text ? std::wstring(SetUaCustomBox->Text->Data()) : L"";
        while (!u.empty() && (u.front() == L' ' || u.front() == L'\t')) u.erase(u.begin());
        while (!u.empty() && (u.back() == L' ' || u.back() == L'\t' || u.back() == L'\r' || u.back() == L'\n')) u.pop_back();
        m_uaCustom = u;
    }
    ApplySettings();
    SaveSettings();
    SettingsPage->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    StartLiveMode();
}

void MainPage::OnSettingsBack(Platform::Object^, RoutedEventArgs^) { HideSettings(); }

// ============================== OOBE / 多语言 ==============================
// 中→英串表。选 English 时遍历已加载 XAML 树就地替换(键含 emoji/glyph 前缀的须全字匹配)。
static const wchar_t* const kI18n[][2] = {
    { L"页内查找", L"Find in page" }, { L"搜索或输入网址", L"Search or enter URL" },
    { L"后退", L"Back" }, { L"前进", L"Forward" }, { L"刷新", L"Reload" }, { L"收藏", L"Bookmark" },
    { L"新标签页", L"New tab" }, { L"主页", L"Home" }, { L"桌面版网站", L"Desktop site" },
    { L"分享", L"Share" }, { L"复制链接", L"Copy link" }, { L"下载此页", L"Download page" },
    { L"书签", L"Bookmarks" }, { L"历史记录", L"History" }, { L"下载内容", L"Downloads" }, { L"设置", L"Settings" },
    { L"菜单", L"Menu" },
    { L"\U0001F4F1 手机UA", L"\U0001F4F1 Mobile UA" }, { L"\U0001F5A5 桌面UA", L"\U0001F5A5 Desktop UA" },
    { L"★ 收藏", L"★ Favorites" }, { L"\U0001F551 历史", L"\U0001F551 History" },
    { L"↓ 下载", L"↓ Downloads" }, { L"★ 收藏此页", L"★ Bookmark this" },
    { L"当前页面", L"Current page" }, { L"浏览资料库", L"Library" }, { L"浏览器设置", L"Browser settings" },
    { L"界面语言", L"Language" },
    { L"默认搜索引擎", L"Default search engine" }, { L"百度", L"Baidu" },
    { L"主页(URL,留空用内置主页)", L"Home (URL; blank = built-in)" },
    { L"自定义 User-Agent(留空=用上面的开关;改后刷新网页生效)", L"Custom User-Agent (blank = use the switch above; reload to apply)" },
    { L"默认缩放", L"Default zoom" },
    { L"启动请求桌面版网站", L"Request desktop site on launch" },
    { L"并发多引擎标签(暂搁置,后续实现)", L"Concurrent multi-engine tabs (planned)" },
    { L"默认启用 GPU 渲染(加载首个网页后自动开)", L"Enable GPU rendering by default (auto after first page)" },
    { L"立即开启 GPU 合成(重启回软件)", L"Enable GPU compositing now (restart reverts)" },
    { L"清除数据", L"Clear data" }, { L"清除历史记录", L"Clear history" },
    { L"清除全部收藏", L"Clear all bookmarks" }, { L"清除下载记录", L"Clear downloads" },
    { L"清除 Cookie(退出全部登录)", L"Clear cookies (sign out everywhere)" },
    { L"诊断", L"Diagnostics" }, { L"导出调试日志 / 崩溃 dump", L"Export debug log / crash dump" },
    { L"开发者选项", L"Developer settings" }, { L"显示翻页按钮", L"Show scroll buttons" },
    { L"即时跟手滚动(实验)", L"Instant pan (experimental)" },
    { L"多线程栅格化(实验)", L"Threaded raster (experimental)" },
    { L"隐藏系统导航栏", L"Hide navigation bar" },
    { L"从屏幕底部向上轻扫可临时唤回",
      L"Swipe up from the bottom edge to bring it back temporarily" },
    { L"关于 / 更新", L"About / Update" }, { L"版本 —", L"Version —" },
    { L"自动检查更新", L"Check for updates automatically" },
    { L"开启后每次启动会连接 api.github.com 一次",
      L"When on, the app contacts api.github.com once per start" },
    { L"立即检查更新", L"Check now" },
    { L"预取网站建议的页面", L"Prefetch pages suggested by sites" },
    { L"关闭预取", L"Off" }, { L"仅 Wi-Fi", L"Wi-Fi only" }, { L"始终", L"Always" },
    { L"网站可提前加载你还没点击的链接",
      L"Sites may load links you have not clicked yet" },
    { L"标签", L"Tabs" }, { L"完成", L"Done" }, { L"新建标签页", L"New tab" },
};
static Platform::String^ I18n(Platform::String^ s, bool toEn) {
    if (s == nullptr) return s;
    std::wstring w(s->Data());
    for (auto& m : kI18n) {
        if (toEn) { if (w == m[0]) return ref new Platform::String(m[1]); }
        else      { if (w == m[1]) return ref new Platform::String(m[0]); }
    }
    return s;
}

void MainPage::TranslateNode(Platform::Object^ node, bool toEn) {
    using namespace Windows::UI::Xaml;
    using namespace Windows::UI::Xaml::Controls;
    if (node == nullptr) return;
    if (auto tb = dynamic_cast<TextBlock^>(node)) { tb->Text = I18n(tb->Text, toEn); return; }
    if (auto tx = dynamic_cast<TextBox^>(node)) { tx->PlaceholderText = I18n(tx->PlaceholderText, toEn); return; }
    if (auto sw = dynamic_cast<ToggleSwitch^>(node)) { if (auto h = dynamic_cast<Platform::String^>(sw->Header)) sw->Header = I18n(h, toEn); return; }
    if (auto cbx = dynamic_cast<ComboBox^>(node)) { for (unsigned i = 0; i < cbx->Items->Size; ++i) { if (auto ci = dynamic_cast<ComboBoxItem^>(cbx->Items->GetAt(i))) if (auto s = dynamic_cast<Platform::String^>(ci->Content)) ci->Content = I18n(s, toEn); } return; }
    if (auto p = dynamic_cast<Panel^>(node)) { for (auto c : p->Children) TranslateNode(c, toEn); return; }
    if (auto bd = dynamic_cast<Border^>(node)) { TranslateNode(bd->Child, toEn); return; }
    if (auto sv = dynamic_cast<ScrollViewer^>(node)) { TranslateNode(sv->Content, toEn); return; }
    if (auto cc = dynamic_cast<ContentControl^>(node)) {   // Button 等
        if (auto s = dynamic_cast<Platform::String^>(cc->Content)) cc->Content = I18n(s, toEn);
        else TranslateNode(cc->Content, toEn);
        return;
    }
}

void MainPage::ApplyLanguage() {
    if (g_lang != L"en") return;   // 默认中文,XAML 原文即中文,无需翻译
    TranslateNode(this->Content, true);
}

// Apotheosis: switch the interface language while the app runs (Settings → LANGUAGE, applied when
//   the settings page closes; the OOBE choice is just the initial value).
//   The loaded XAML tree is translated in place, in whichever direction we are going — kI18n is
//   walked backwards for en → zh, so every English string in the table has to stay unique.
//   Labels the code assigns at runtime do not live in the tree's original text, so they are
//   re-stamped from L8 afterwards; panels that rebuild their contents on open (drawer, tab
//   switcher, action sheet, settings footer) pick the new language up by themselves.
void MainPage::SetLanguage(const std::wstring& lang) {
    std::wstring want = (lang == L"en") ? L"en" : L"zh";
    if (want == g_lang) return;
    TranslateNode(this->Content, want == L"en");
    g_lang = want;
    m_langSet = true;
    ApplySettings();          // UaBtn 等运行期标签按新语言重刷
    // Apotheosis: TabSwitcherTitle is built at runtime as "标签 (N)" / "Tabs (N)" (RebuildTabSwitcher) —
    //   once a count is appended it no longer exact-matches either kI18n key, so TranslateNode above
    //   silently leaves it in the old language until something else rebuilds the switcher. Do it here
    //   unconditionally (cheap, safe whether or not the switcher is currently open).
    RebuildTabSwitcher();
}

void MainPage::OnOobeLang(Platform::Object^ sender, RoutedEventArgs^) {
    std::wstring tag = L"zh";
    if (auto b = dynamic_cast<Windows::UI::Xaml::Controls::Button^>(sender))
        if (auto t = dynamic_cast<Platform::String^>(b->Tag)) tag = std::wstring(t->Data());
    SetLanguage(tag);         // 立即把整个界面翻过去(选中文=默认,无操作)
    m_langSet = true;
    SaveSettings();
    if (OobePanel) OobePanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
}

void MainPage::OnZoomChanged(Platform::Object^, Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs^ e)
{
    if (SetZoomLabel) SetZoomLabel->Text = ref new String((std::to_wstring((int)(e->NewValue + 0.5)) + L"%").c_str());
}

void MainPage::OnSettingsBtn(Platform::Object^ sender, RoutedEventArgs^)
{
    std::wstring t;
    auto b = dynamic_cast<Button^>(sender);
    if (b) { auto tag = dynamic_cast<Platform::String^>(b->Tag); if (tag) t = std::wstring(tag->Data()); }
    if (t == L"clearhist") { m_historyList.clear(); SaveHistory(); TitleText->Text = L8(L"历史记录已清除", L"History cleared"); }
    else if (t == L"clearfav") { m_bookmarks.clear(); SaveBookmarks(); TitleText->Text = L8(L"收藏已清除", L"Bookmarks cleared"); }
    else if (t == L"cleardl") { m_downloads.clear(); SaveDownloads(); TitleText->Text = L8(L"下载记录已清除", L"Downloads cleared"); }
    else if (t == L"clearcookies") {
        WebEngine::instance().post([]() { try { WebCoreClearCookies(); } catch (...) {} });   // 引擎线程串行,不与加载互踩
        TitleText->Text = L8(L"Cookie 已清除", L"Cookies cleared");
    }
    else if (t == L"export") ExportDebug();
    else if (t == L"gpu") { HideSettings(); OnToggleGpu(nullptr, nullptr); }
    else if (t == L"checkupdate") CheckForUpdate(true);
}

// ---- 检测更新 ----
// 后台线程(独立 curl,WebCoreDownload 不碰引擎 Page 状态,可离引擎线程跑)拉 GitHub Releases API,
// 比对当前 appx 版本(Package.Current)。有新版弹对话框→可直接在本浏览器里打开发布页下载 appx 手动装。
// manual=true:用户在设置里点的,无更新/失败也提示;false:启动静默自检,仅有新版才提示。
static bool ParseDottedVersion(const std::string& s, int out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    int idx = 0; long cur = 0; bool any = false;
    for (size_t i = 0; i <= s.size() && idx < 4; ++i) {
        if (i < s.size() && s[i] >= '0' && s[i] <= '9') { cur = cur * 10 + (s[i] - '0'); any = true; }
        else if (i == s.size() || s[i] == '.') { out[idx++] = (int)cur; cur = 0; if (i == s.size()) break; }
        else break;   // 非数字非点(如 tag 后缀)→ 停
    }
    return any;
}

void MainPage::CheckForUpdate(bool manual)
{
    if (m_updateChecking) return;
    m_updateChecking = true;
    if (manual) TitleText->Text = L8(L"正在检查更新…", L"Checking for updates…");

    // 当前版本(从 appx 清单读,不写死)
    auto pv = Windows::ApplicationModel::Package::Current->Id->Version;
    int cur[4] = { pv.Major, pv.Minor, pv.Build, pv.Revision };
    std::wstring dir = LocalStateDir();
    std::string jsonPath = dir.empty() ? std::string() : WideToUtf8(dir + L"\\update.json");

    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    std::thread([disp, self, manual, jsonPath, cur]() {
        int rc = -1;
        std::string body;
        if (!jsonPath.empty()) {
            try { rc = WebCoreDownload(
                "https://api.github.com/repos/Jimmyxiao2009/Project-Apotheosis/releases/latest",
                jsonPath.c_str()); } catch (...) {}
            if (rc == 200) {
                std::ifstream f(jsonPath, std::ios::binary);
                if (f) { std::stringstream ss; ss << f.rdbuf(); body = ss.str(); }
            }
        }
        // 极简 JSON 取值(取 key 后第一个带引号字符串)
        auto pick = [&](const char* key) -> std::string {
            std::string pat = std::string("\"") + key + "\"";
            size_t p = body.find(pat); if (p == std::string::npos) return {};
            p = body.find(':', p + pat.size()); if (p == std::string::npos) return {};
            size_t a = body.find('"', p); if (a == std::string::npos) return {};
            size_t b = body.find('"', a + 1); if (b == std::string::npos) return {};
            return body.substr(a + 1, b - a - 1);
        };
        std::string tag = pick("tag_name");      // 形如 v0.1.8.4
        std::string page = pick("html_url");     // 发布页(release 对象第一个 html_url)
        bool ok = (rc == 200 && !tag.empty());
        bool newer = false;
        std::string verStr = tag;
        if (!verStr.empty() && (verStr[0] == 'v' || verStr[0] == 'V')) verStr = verStr.substr(1);
        if (ok) {
            int rel[4]; ParseDottedVersion(verStr, rel);
            for (int i = 0; i < 4; ++i) { if (rel[i] != cur[i]) { newer = rel[i] > cur[i]; break; } }
        }
        std::wstring tagW = Utf8ToWide(tag), pageW = Utf8ToWide(page);
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler(
                [self, manual, ok, newer, tagW, pageW]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    s->m_updateChecking = false;
                    if (!ok) { if (manual) s->TitleText->Text = L8(L"检查更新失败(网络?)", L"Update check failed (network?)"); return; }
                    if (!newer) { if (manual) s->TitleText->Text = ref new String(((g_lang == L"en" ? L"Up to date " : L"已是最新版 ") + tagW).c_str()); return; }
                    // 有新版:提示 + 可直接在本浏览器打开发布页下载
                    s->TitleText->Text = ref new String(((g_lang == L"en" ? L"New version " : L"发现新版本 ") + tagW).c_str());
                    std::wstring target = pageW.empty()
                        ? std::wstring(L"https://github.com/Jimmyxiao2009/Project-Apotheosis/releases/latest")
                        : pageW;
                    try {
                        auto dlg = ref new Windows::UI::Popups::MessageDialog(
                            ref new String((W8(L"发现新版本 ", L"New version ") + tagW
                                            + W8(L"\n是否打开发布页下载 appx?",
                                                 L"\nOpen the release page to download the appx?")).c_str()),
                            L8(L"有可用更新", L"Update available"));
                        auto go = ref new Windows::UI::Popups::UICommand(L8(L"前往下载", L"Download"));
                        auto later = ref new Windows::UI::Popups::UICommand(L8(L"稍后", L"Later"));
                        dlg->Commands->Append(go);
                        dlg->Commands->Append(later);
                        dlg->DefaultCommandIndex = 0;
                        dlg->CancelCommandIndex = 1;
                        Platform::Agile<MainPage^> self2(s);
                        concurrency::create_task(dlg->ShowAsync()).then(
                            [self2, go, target](Windows::UI::Popups::IUICommand^ chosen) {
                                MainPage^ s2 = self2.Get(); if (!s2) return;
                                if (chosen == go) {
                                    if (s2->SettingsPage->Visibility == Windows::UI::Xaml::Visibility::Visible) s2->HideSettings();
                                    s2->NavigateTo(ref new String(target.c_str()), true);
                                }
                            });
                    } catch (...) {}
                }));
        } catch (...) {}
    }).detach();
}

// 调试导出:把 LocalState 下的诊断文本拼成一份报告,FileSavePicker 让用户存到 OneDrive/SD 卡。
void MainPage::ExportDebug()
{
    std::wstring d = LocalStateDir();
    std::string report = std::string("=== Apotheosis ") + U8("调试报告", "debug report") + " ===\n";
    report += "harness / WebCore 2.52.4 / ARM32 UWP\n\n";
    if (!d.empty()) {
        std::string dd = WideToUtf8(d);
        const char* names[] = { "stage.txt", "gpuinit.txt", "gpuresult.txt", "layertree.txt", "autodump.txt", "imedebug.txt", "jitresult.txt", "diag.txt" };
        for (const char* fn : names) {
            std::ifstream f(dd + "\\" + fn, std::ios::binary);
            if (!f) continue;
            std::stringstream ss; ss << f.rdbuf();
            report += std::string("---------- ") + fn + " ----------\n" + ss.str() + "\n\n";
        }
        report += "---------- crash dumps ----------\n";
        report += U8("(崩溃 dump 文件在 LocalState 根目录,可经 Device Portal 拉取)\n",
                     "(crash dumps sit in the LocalState root; pull them with Device Portal)\n");
        try { std::ofstream o(dd + "\\debug-report.txt", std::ios::binary | std::ios::trunc); if (o) o.write(report.data(), report.size()); } catch (...) {}
    }
    Platform::String^ reportW = ref new String(Utf8ToWide(report).c_str());
    try {
        auto picker = ref new Windows::Storage::Pickers::FileSavePicker();
        picker->SuggestedStartLocation = Windows::Storage::Pickers::PickerLocationId::DocumentsLibrary;
        picker->SuggestedFileName = ref new String(L"apotheosis-debug");
        auto exts = ref new Platform::Collections::Vector<Platform::String^>();
        exts->Append(".txt");
        picker->FileTypeChoices->Insert(L8(L"文本文件", L"Text file"), exts);
        concurrency::create_task(picker->PickSaveFileAsync()).then([reportW](Windows::Storage::StorageFile^ file) {
            if (file) concurrency::create_task(Windows::Storage::FileIO::WriteTextAsync(file, reportW));
        });
        TitleText->Text = L8(L"选择保存位置以导出…", L"Pick a location to export…");
    } catch (...) {
        TitleText->Text = L8(L"导出失败", L"Export failed");
    }
}

// ============================================================================
// 增量4:页内查找(查找条 + 引擎 WebCoreFindString/Next/Clear)
// ============================================================================

void MainPage::ShowFindBar()
{
    if (!m_sessionActive) { TitleText->Text = L8(L"当前页不可查找", L"Find not available here"); return; }
    HideActionMenu();
    HideSuggestions();
    FindBar->Visibility = Windows::UI::Xaml::Visibility::Visible;
    FindCount->Text = ref new String(L"");
    FindBox->Text = ref new String(L"");   // 触发一次空查找(清除残留高亮),无害
    FindBox->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void MainPage::OnFindChanged(Platform::Object^, Windows::UI::Xaml::Controls::TextChangedEventArgs^) { DoFind(0); }

void MainPage::OnFindKeyDown(Platform::Object^, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e)
{
    if (e->Key == Windows::System::VirtualKey::Enter) { e->Handled = true; DoFind(1); }
}

void MainPage::OnFindNext(Platform::Object^, RoutedEventArgs^) { DoFind(1); }
void MainPage::OnFindPrev(Platform::Object^, RoutedEventArgs^) { DoFind(2); }

void MainPage::OnFindClose(Platform::Object^, RoutedEventArgs^)
{
    FindBar->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    FindBox->Text = ref new String(L"");   // 触发空查找 → 引擎清除高亮
}

// 查找派发(引擎线程串行)。mode:0=查找(标记全部+选第一个),1=下一个,2=上一个。空串=清除。
void MainPage::DoFind(int mode)
{
    if (!m_sessionActive) { if (FindCount) FindCount->Text = ref new String(L""); return; }
    if (m_loading || m_interacting) return;
    std::wstring query = FindBox->Text ? std::wstring(FindBox->Text->Data()) : L"";
    bool clear = (mode == 0 && query.empty());

    m_interacting = true;
    SetLoading(true);
    if (m_loadWatchdog) m_loadWatchdog->Start();
    std::string q = WideToUtf8(query);
    bool present = m_gpuPresent;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = ++m_opSeq;
    WebEngine::instance().post([disp, self, q, mode, clear, present, mySeq]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -999;
        try {
            if (clear) rc = WebCoreFindClear(rgba->data());
            else if (mode == 0) rc = WebCoreFindString(q.c_str(), /*matchCase*/ 0, /*wrap*/ 1, rgba->data());
            else rc = WebCoreFindNext(mode == 1 ? 1 : 0, rgba->data());
        } catch (...) { rc = -1000; }
        int rcCopy = rc; int modeCopy = mode; bool clearCopy = clear;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, rgba, rcCopy, modeCopy, clearCopy, present, mySeq]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    if (s->m_opSeq != mySeq) return;   // 被更新操作/看门狗取代
                    s->m_interacting = false;
                    if (s->m_loadWatchdog) s->m_loadWatchdog->Stop();
                    s->SetLoading(false);
                    if (rcCopy < 0) {
                        if (rcCopy == -12 || rcCopy == -14) {   // 会话没了
                            s->m_sessionActive = false;
                            s->ScrollFab->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                        }
                        s->FindCount->Text = ref new String(L"");
                        return;
                    }
                    s->PresentSoftwareFrame(rgba);
                    s->m_lastFrameHash = 0;
                    if (clearCopy) s->FindCount->Text = ref new String(L"");
                    else if (modeCopy == 0) s->FindCount->Text = ref new String(rcCopy > 0 ? (std::to_wstring(rcCopy) + (g_lang == L"en" ? L" found" : L" 处")).c_str() : (g_lang == L"en" ? L"No results" : L"无结果"));
                    else s->FindCount->Text = ref new String(rcCopy ? L"" : (g_lang == L"en" ? L"No more" : L"无更多"));
                }));
        } catch (...) {}
    });
}

// ============================================================================
// 增量5:标签(Mode A 单热会话)。活动标签实时状态=全局成员;切换时与 m_tabs 互拷并重载。
// ============================================================================

void MainPage::UpdateTabCount()
{
    if (TabCountText) TabCountText->Text = ref new String(std::to_wstring(m_tabs.size()).c_str());
}

// ---------------------------------------------------------------------------
// Apotheosis: 标签切换快照(TABS-PLAN.md 方案 a)。
// 切换仍是"拆会话 + 完整重载"(见 TABS-PLAN.md §1),但切过去的一瞬间先把目标标签上次离开时
// 的那帧贴出来,而不是让用户盯着上一个标签的画面等一次完整网络加载。快照是明确的占位图:
// 真实重载在下面照跑,第一帧到位(OnNavDone)就换回真画面。
// ---------------------------------------------------------------------------

static const size_t kMaxTabSnapshots = 3;   // 3 × 720×1080×4 ≈ 9 MB 上限(32 位进程,内存紧,见 MEMORY-PLAN.md)

// 把当前会话的最后一帧读回,存进**当前活动**标签(调用时它马上就要变成非活动的)。
// 线程:读回排在引擎线程队列上,而 RestoreTab→NavigateTo 的 WebCoreSessionLoad 排在其后
//   (WebEngine 是单线程 FIFO),故快照一定在 teardownSession 拆掉图层树之前抓完。
//   UI 线程只 post,不等 —— 线程铁律:UI 绝不同步 wait 引擎。
void MainPage::CaptureActiveTabSnapshot()
{
    if (!m_sessionActive) return;   // 主页/错误页:无会话可读回,而且它们本来就是本地秒开
    const int idx = m_activeTab;
    if (idx < 0 || idx >= (int)m_tabs.size()) return;
    const bool gpu = m_gpuPresent;
    const std::wstring url = m_currentUrl;   // 防串位:回调落地时按 URL 校验这一格还是同一个页面
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    const unsigned long long seq = ++m_snapSeq;
    WebEngine::instance().post([disp, self, idx, gpu, url, seq]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)kW * kH * 4, 0);
        int rc = -1;
        try {
            // ★ 直呈现模式下不能用 WebCoreSessionPaint:它会走 gpuPresent 再 swapBuffers 一次,
            //   且根本不填 rgba(见 port\WebCoreDriver.cpp paintToRGBA)。离屏合成+glReadPixels
            //   的 WebCoreCompositeReadback 才是这里要的。软件模式反过来只有 SessionPaint 能用。
            rc = gpu ? WebCoreCompositeReadback(rgba->data()) : WebCoreSessionPaint(rgba->data());
        } catch (...) { rc = -1; }
        if (rc != 0) return;   // 抓不到就没有快照,退回原来的行为(零回归)
        try {
            disp->RunAsync(CoreDispatcherPriority::Low, ref new DispatchedHandler([self, rgba, idx, url, seq]() {
                MainPage^ s = self.Get();
                if (!s) return;
                if (idx < 0 || idx >= (int)s->m_tabs.size()) return;   // 期间关过标签
                if (idx == s->m_activeTab) return;                     // 又切回来了:活动标签不留快照
                if (s->m_tabs[idx].currentUrl != url) return;          // 索引左移/该格换了页面
                s->m_tabs[idx].snapshot = rgba;
                s->m_tabs[idx].snapSeq = seq;
                s->PruneTabSnapshots();
            }));
        } catch (...) {}
    });
}

// 只留最近 kMaxTabSnapshots 张,其余释放(每张 ~3 MB)。
void MainPage::PruneTabSnapshots()
{
    for (;;) {
        size_t n = 0;
        int oldest = -1;
        unsigned long long oldestSeq = 0;
        for (size_t k = 0; k < m_tabs.size(); ++k) {
            if (!m_tabs[k].snapshot) continue;
            ++n;
            if (oldest < 0 || m_tabs[k].snapSeq < oldestSeq) { oldest = (int)k; oldestSeq = m_tabs[k].snapSeq; }
        }
        if (n <= kMaxTabSnapshots || oldest < 0) return;
        m_tabs[oldest].snapshot.reset();
        m_tabs[oldest].snapSeq = 0;
    }
}

// 切到标签 i:有快照就立刻贴出来当占位图,并释放该标签持有的那份(活动标签不留快照)。
// 直呈现模式下 GpuPanel 盖在 RenderImage 之上,故把它 Opacity=0 让下层透出 —— ★ 绝不 Collapse,
// 面板变 0×0 会让 ANGLE 经面板 dispatcher 重建交换链而 std::terminate(见 NavigateTo 处的注释)。
void MainPage::ShowTabSnapshot(int i)
{
    if (i < 0 || i >= (int)m_tabs.size()) return;
    auto snap = m_tabs[i].snapshot;
    m_tabs[i].snapshot.reset();
    m_tabs[i].snapSeq = 0;
    if (!snap || snap->size() != (size_t)kW * kH * 4) return;
    if (!RenderImage || !GpuPanel) return;
    try {
        if (!m_snapBmp) m_snapBmp = ref new WriteableBitmap(kW, kH);
        BlitToBitmapRaw(m_snapBmp, *snap, kW, kH);   // Raw:直呈现模式下 BlitToBitmap 会空转
        m_snapBmp->Invalidate();
        RenderImage->Source = m_snapBmp;
        RenderImage->Visibility = Windows::UI::Xaml::Visibility::Visible;
        if (m_gpuPresent) GpuPanel->Opacity = 0.0;
        m_snapshotShown = true;
    } catch (...) {}
}

// 新会话第一帧到位(OnNavDone)/加载超时:按当前呈现模式恢复正常显示面并放掉快照位图。
void MainPage::HideTabSnapshot()
{
    if (!m_snapshotShown) return;
    m_snapshotShown = false;
    try {
        // 与 NavigateTo 完成回调里的切面逻辑同一套判据:present=GPU 面,否则软件面。
        if (m_gpuPresent) {
            GpuPanel->Opacity = 1.0;
            RenderImage->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
            RenderImage->Source = nullptr;
        } else {
            GpuPanel->Opacity = 0.0;
            RenderImage->Visibility = Windows::UI::Xaml::Visibility::Visible;
            // 软件面:PresentSoftwareFrame 已把新帧贴进 m_frameBmpA/B 并换掉 Source,这里不动它。
        }
    } catch (...) {}
    m_snapBmp = nullptr;
}

void MainPage::SaveActiveTab()
{
    if (m_activeTab < 0 || m_activeTab >= (int)m_tabs.size()) return;
    Tab& t = m_tabs[m_activeTab];
    t.navStack = m_navStack;
    t.navIndex = m_navIndex;
    t.currentUrl = m_currentUrl.empty() ? L"about:home" : m_currentUrl;
    t.currentTitle = m_currentTitle;
    t.pageScale = m_pageScale;
}

void MainPage::RestoreTab(int i)
{
    if (i < 0 || i >= (int)m_tabs.size()) return;
    m_activeTab = i;
    const Tab& t = m_tabs[i];
    m_navStack = t.navStack;
    m_navIndex = t.navIndex;
    m_currentUrl = t.currentUrl;
    m_currentTitle = t.currentTitle;
    m_pageScale = t.pageScale;
    // 切到该标签:作废在途、清加载锁,重载其 URL 重建单热会话。
    ++m_opSeq;
    m_interacting = false;
    if (m_loadWatchdog) m_loadWatchdog->Stop();
    SetLoading(false);          // 同 NewTab:复位进度条/图标,并清掉 GPU 优先拦截置的加载态
    CancelPendingFirstNav();    // 上一个标签攒下的待发导航不许打进这个标签
    UpdateNavButtons();
    UpdateLockIcon();
    m_urlSyncing = true;
    UrlBox->Text = ref new String(m_currentUrl == L"about:home" ? L"" : m_currentUrl.c_str());
    m_urlSyncing = false;
    ShowTabSnapshot(i);   // Apotheosis: 先贴上次离开这个标签时的画面,重载在下面跑
    NavigateTo(ref new String(m_currentUrl.c_str()), false);
}

void MainPage::NewTab()
{
    SaveActiveTab();
    CaptureActiveTabSnapshot();   // Apotheosis: 离开的标签留一帧,切回来时秒出画面
    Tab t; t.currentUrl = g_homeUrl;
    m_tabs.push_back(t);
    m_activeTab = (int)m_tabs.size() - 1;
    // 清空全局,作废在途,重载主页。
    ++m_opSeq;
    m_interacting = false;
    if (m_loadWatchdog) m_loadWatchdog->Stop();
    SetLoading(false);          // 直接写 m_loading 会漏掉进度条/地址栏图标复位(GPU 优先拦截也置过它)
    CancelPendingFirstNav();    // 上一个标签攒下的待发导航不许打进新标签
    m_navStack.clear(); m_navIndex = -1;
    m_currentUrl.clear(); m_currentTitle.clear();
    m_pageScale = 1.0f;
    UpdateTabCount();
    NavigateTo(ref new String(g_homeUrl.c_str()), true);
}

void MainPage::CloseTab(int i)
{
    if (i < 0 || i >= (int)m_tabs.size()) return;
    bool wasActive = (i == m_activeTab);
    m_tabs.erase(m_tabs.begin() + i);
    if (m_tabs.empty()) {                      // 关到空:留一个主页标签
        Tab t; t.currentUrl = g_homeUrl;
        m_tabs.push_back(t);
        m_activeTab = 0;
        ++m_opSeq; m_interacting = false; if (m_loadWatchdog) m_loadWatchdog->Stop();
        SetLoading(false); CancelPendingFirstNav();   // 同 NewTab/RestoreTab
        m_navStack.clear(); m_navIndex = -1; m_currentUrl.clear(); m_currentTitle.clear(); m_pageScale = 1.0f;
        UpdateTabCount();
        NavigateTo(ref new String(g_homeUrl.c_str()), true);
        return;
    }
    if (m_activeTab >= (int)m_tabs.size()) m_activeTab = (int)m_tabs.size() - 1;
    else if (i < m_activeTab) m_activeTab--;   // 索引左移
    UpdateTabCount();
    if (wasActive) RestoreTab(m_activeTab);    // 关掉的是活动标签 → 载入新活动标签
}

void MainPage::SwitchTab(int i)
{
    if (i == m_activeTab) return;
    SaveActiveTab();
    CaptureActiveTabSnapshot();   // Apotheosis: 必须在 RestoreTab 之前 post —— 引擎线程 FIFO,
                                  //   读回排在 WebCoreSessionLoad(teardownSession)之前
    RestoreTab(i);
}

void MainPage::OnTabs(Platform::Object^, RoutedEventArgs^) { ShowTabSwitcher(); }
void MainPage::OnNewTab(Platform::Object^, RoutedEventArgs^) { HideTabSwitcher(); NewTab(); }
void MainPage::OnTabSwitcherDone(Platform::Object^, RoutedEventArgs^) { HideTabSwitcher(); }

void MainPage::ShowTabSwitcher()
{
    HideActionMenu();
    HideSuggestions();
    DismissKeyboardForOverlay();   // Apotheosis: the tab switcher covers the address bar too.
    SaveActiveTab();
    RebuildTabSwitcher();
    TabSwitcher->Visibility = Windows::UI::Xaml::Visibility::Visible;
    StopLiveMode();
}

void MainPage::HideTabSwitcher()
{
    TabSwitcher->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    StartLiveMode();
}

void MainPage::RebuildTabSwitcher()
{
    UpdateTabCount();
    if (TabSwitcherTitle) TabSwitcherTitle->Text = ref new String(((g_lang == L"en" ? L"Tabs (" : L"标签 (") + std::to_wstring(m_tabs.size()) + L")").c_str());
    TabList->Children->Clear();
    Platform::Agile<MainPage^> self(this);
    Color accent = ColorHelper::FromArgb(255, 0x45, 0xD6, 0xC5);
    Color white = ColorHelper::FromArgb(255, 0xF4, 0xF7, 0xF8);
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        int idx = (int)i;
        const Tab& t = m_tabs[i];
        bool active = (idx == m_activeTab);
        std::wstring title = t.currentTitle.empty()
            ? (t.currentUrl == L"about:home" ? W8(L"主页", L"Home") : t.currentUrl)
            : t.currentTitle;
        std::wstring sub = (t.currentUrl == L"about:home") ? std::wstring(L"about:home") : t.currentUrl;

        auto cell = ref new Grid();
        cell->Margin = Thickness(0, 0, 0, 8);

        auto sw = ref new Button();
        sw->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x10, 0x17, 0x1D));
        sw->BorderThickness = Thickness(1);
        sw->BorderBrush = ref new SolidColorBrush(active ? accent : ColorHelper::FromArgb(255, 0x26, 0x36, 0x40));
        sw->Padding = Thickness(0, 0, 40, 0);   // 右留位给关闭键
        sw->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        sw->HorizontalContentAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        sw->Content = MakeRow(ref new String(title.c_str()), ref new String(sub.c_str()), active ? accent : white);
        sw->Click += ref new RoutedEventHandler([self, idx](Platform::Object^, RoutedEventArgs^) {
            MainPage^ s = self.Get(); if (!s) return;
            s->HideTabSwitcher();
            s->SwitchTab(idx);
        });
        cell->Children->Append(sw);

        auto cb = ref new Button();
        cb->Content = ref new String(L"\x2715");
        cb->Background = ref new SolidColorBrush(Colors::Transparent);
        cb->Foreground = ref new SolidColorBrush(ColorHelper::FromArgb(255, 0x91, 0xA2, 0xAD));
        cb->BorderThickness = Thickness(0);
        cb->Width = 44; cb->Height = 44;
        cb->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Right;
        cb->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        cb->Click += ref new RoutedEventHandler([self, idx](Platform::Object^, RoutedEventArgs^) {
            MainPage^ s = self.Get(); if (!s) return;
            s->CloseTab(idx);
            s->RebuildTabSwitcher();
        });
        cell->Children->Append(cb);

        TabList->Children->Append(cell);
    }
}

// ============================================================================
// 默认 GPU:把 OnToggleGpu 首点路径抽出复用,带崩溃环路保护(GpuInit 硬崩→下次启动自动关)。
// ============================================================================
void MainPage::EnableGpu()
{
    if (m_gpuOn) return;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    GpuPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
    auto props = ref new Windows::Foundation::Collections::PropertySet();
    props->Insert(L"EGLNativeWindowTypeProperty", GpuPanel);
    props->Insert(L"EGLRenderSurfaceSizeProperty",
                  Windows::Foundation::PropertyValue::CreateSize(Windows::Foundation::Size((float)kW, (float)kH)));
    m_gpuProps = props;
    void* win = reinterpret_cast<void*>(reinterpret_cast<IInspectable*>(props));
    // 崩溃环路保护:开 GPU 前落 gpu-crash.flag;回调(成功或优雅失败)删它。GpuInit 硬崩则无回调→标记残留→下次启动检测到→关默认GPU。
    {
        std::wstring fd = LocalStateDir();
        if (!fd.empty()) { try { std::ofstream f(WideToUtf8(fd) + "\\gpu-crash.flag", std::ios::binary | std::ios::trunc); if (f) f << "1"; } catch (...) {} }
    }
    WebEngine::instance().post([disp, self, win]() {
        int rc = -999;
        try { rc = WebCoreGpuInit(win, kW, kH); } catch (...) { rc = -1000; }
        try {
            std::wstring d = LocalStateDir();
            if (!d.empty()) { std::ofstream f(WideToUtf8(d) + "\\gpuinit.txt", std::ios::binary | std::ios::trunc); if (f) { std::string s = "WebCoreGpuInit(window) rc=" + std::to_string(rc) + "\n"; f.write(s.data(), s.size()); } }
        } catch (...) {}
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rcCopy]() {
                MainPage^ s = self.Get(); if (!s) return;
                std::wstring d2 = LocalStateDir();   // 回调到达=没硬崩 → 删崩溃标记
                if (!d2.empty()) { try { DeleteFileW((d2 + L"\\gpu-crash.flag").c_str()); } catch (...) {} }
                if (rcCopy == 0) {
                    s->m_gpuOn = true;
                    s->m_gpuPresent = true;
                    g_directPresent.store(true);
                    s->RenderImage->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
                    s->GpuBtn->Content = GpuOrientLabel(s->m_gpuOrient);
                    s->GpuBtn->Foreground = ref new SolidColorBrush(Windows::UI::Colors::LimeGreen);
                    // Apotheosis (M4): 启动路径把第一次导航推迟到这里 → 首个会话直接带合成,不再"加载两遍"。
                    if (!s->m_pendingFirstNav.empty())
                        s->StartPendingFirstNav();
                    // Apotheosis (review 2026-09-03): only reload for a GPU enable that was NOT the
                    // GPU-first startup. If m_gpuStartupBegun is set, this callback belongs to that
                    // startup's own WebCoreGpuInit and an empty m_pendingFirstNav just means the 6 s
                    // m_startupNavTimer (or the size-wait fallback) already sent the navigation
                    // while GpuInit was still running — reloading it here is the "page loads twice"
                    // path, now much easier to hit since the size gate delays GpuInit by up to 2 s.
                    // The page is loading with software compositing; OnNavDone's zoom/GPU handling
                    // covers it from there.
                    else if (!s->m_gpuStartupBegun && !s->m_currentUrl.empty() && s->m_currentUrl != L"about:home")
                        s->NavigateTo(ref new String(s->m_currentUrl.c_str()), false);   // 重载使合成+直呈现生效
                } else {
                    // 同上:只隐不折叠。GpuInit 可能是"窗口表面已建、TextureMapper 才失败"(rc=-22),
                    // 那时 ANGLE 已绑在面板上,折叠 → 0×0 重建交换链 = 崩。
                    s->GpuPanel->Opacity = 0.0;
                    s->GpuPanel->IsHitTestVisible = false;
                    s->GpuBtn->Content = ref new String(L"\U0001F5A5 GPU\x2717");
                    s->GpuBtn->Foreground = ref new SolidColorBrush(Windows::UI::Colors::OrangeRed);
                    // Apotheosis (M4): GPU 起不来 → 待发的首次导航照常走软件路径(g_gpuActive 仍 false)。
                    if (!s->m_pendingFirstNav.empty())
                        s->StartPendingFirstNav();
                }
            }));
        } catch (...) {}
    });
}

// ============================================================================
// Apotheosis (M4):GPU 优先启动 —— 面板就绪后先起 GPU,再发第一次导航。
// 省掉原来的"软件加载首页一遍 → EnableGpu → 重载同一页一遍"(真机 ~14 s / ~300 MB 白工)。
// 引擎侧合成只在 buildSession 里按 g_gpuActive 打开(port\WebCoreDriver.cpp:1357-1358),
// 所以必须在第一次 WebCoreSessionLoad 之前 WebCoreGpuInit;失败则该次导航自然落回软件路径。
// ============================================================================
// 面板当前尺寸(诊断用;取不到算 0)。
std::string MainPage::GpuPanelSizeStr()
{
    int pw = 0, ph = 0;
    try { if (GpuPanel) { pw = (int)GpuPanel->ActualWidth; ph = (int)GpuPanel->ActualHeight; } } catch (...) {}
    return std::to_string(pw) + "x" + std::to_string(ph);
}

// 兜底定时器:待发导航必须在 6 s 内出去,不管 GPU 那边发生了什么。启动时构造函数先武装一次,
// StartupGpuThenNav 再重新武装(用户输入触发的首次网络导航根本没经过构造函数那条路)。
void MainPage::ArmStartupNavTimer()
{
    if (!m_startupNavTimer) {
        m_startupNavTimer = ref new Windows::UI::Xaml::DispatcherTimer();
        Windows::Foundation::TimeSpan sts; sts.Duration = 60000000LL;   // 6s(100ns 单位)
        m_startupNavTimer->Interval = sts;
        m_startupNavTimer->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(this, &MainPage::OnStartupNavTimer);
    }
    m_startupNavTimer->Stop();
    m_startupNavTimer->Start();
}

void MainPage::StartupGpuThenNav()
{
    if (m_pendingFirstNav.empty()) return;
    if (m_gpuStartupBegun) return;   // 去重:页面 Loaded / 面板 Loaded / 面板 SizeChanged / 首次网络导航
    m_gpuStartupBegun = true;
    ArmStartupNavTimer();
    WriteMemLog("startup gpu-first (panel " + GpuPanelSizeStr() + ")"
                + " pageLoaded=" + (m_pageLoadedSeen ? "1" : "0")
                + " panelLoaded=" + (m_gpuPanelLoadedSeen ? "1" : "0")
                + " url=" + WideToUtf8(m_pendingFirstNav));
    if (m_gpuOn || m_gpuAutoTried) { StartPendingFirstNav(); return; }   // 不该发生;绝不吞掉首次导航
    m_gpuAutoTried = true;   // 占住 OnNavDone 里的自动开 GPU 分支(否则加载完又开一次并重载)
    EnableGpu();             // 成功/失败的 UI 回调都会调 StartPendingFirstNav()
}

// Apotheosis (M4): 取消待发的首次网络导航 + 停兜底定时器。开/切/关标签时必调:否则上一个标签攒下的
// 待发导航会打进新标签(NavigateTo 的 GPU 优先拦截见本文件 ~:797)。GpuInit 本身不取消(幂等、已在飞,
// 成功了对新标签一样有用);m_gpuStartupBegun 保持 true → 拦截是一次性的,不会再拦第二次。
void MainPage::CancelPendingFirstNav()
{
    m_pendingFirstNav.clear();
    if (m_startupNavTimer) m_startupNavTimer->Stop();
}

// 发出被推迟的第一次导航(此刻 GpuInit 已有结论:成功=合成+直呈现,失败=软件路径)。
void MainPage::StartPendingFirstNav()
{
    if (m_startupNavTimer) m_startupNavTimer->Stop();
    if (m_pendingFirstNav.empty()) return;
    std::wstring u = m_pendingFirstNav;
    bool push = m_pendingFirstNavPush;
    m_pendingFirstNav.clear();
    SetLoading(false);   // 拦截时置的"等 GpuInit"状态;不清 NavigateTo 会在 m_loading 处早退
    NavigateTo(ref new String(u.c_str()), push);
}

// 兜底:6 s 到点导航还没出去 → 无论如何发出去(GpuInit 卡住/触发源全没来)。
// 迟到的 EnableGpu 回调此时看到 m_pendingFirstNav 为空,只会走它原来的"重载当前页"分支,而那条
// 分支在加载中(m_loading)会自行早退 → 不会变成两次加载。
void MainPage::OnStartupNavTimer(Platform::Object^, Platform::Object^)
{
    if (m_startupNavTimer) m_startupNavTimer->Stop();
    if (m_pendingFirstNav.empty()) return;
    WriteMemLog(std::string("startup fallback (timer) reason=")
                + (m_gpuStartupBegun ? "gpu-init-slow" : "no-trigger")
                + " pageLoaded=" + (m_pageLoadedSeen ? "1" : "0")
                + " panelLoaded=" + (m_gpuPanelLoadedSeen ? "1" : "0")
                + " panel=" + GpuPanelSizeStr()
                + " url=" + WideToUtf8(m_pendingFirstNav));
    StartPendingFirstNav();
}
