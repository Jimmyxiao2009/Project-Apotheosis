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
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdio>
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

// Apotheosis (M4 load timeline): stage.txt used to be truncated on every write ("where are we
// now"). The driver now appends one "timeline url=... firstbyte=... commit=... " line per
// navigation to the same file (WebCoreDriver.cpp perfWriteStageTimeline), which a truncating
// writer would wipe on the very next after-load line. So: truncate ONCE per process, append
// afterwards. The file stays small - two harness lines plus one driver line per navigation -
// and the device scripts that tail it keep working, now with history instead of one line.
static void WriteStage(const char* stage)
{
    try {
        static bool truncated = false;
        std::wstring d = LocalStateDir();
        if (d.empty()) return;
        auto mode = truncated ? std::ios::app : std::ios::trunc;
        truncated = true;
        std::ofstream f(WideToUtf8(d) + "\\stage.txt", std::ios::binary | mode);
        if (f) f << stage << "\n";
    } catch (...) {}
}

// Apotheosis (M4): UWP enforces a per-app memory cap; on a Lumia 950 the app is terminated with no
// crash dump once it is exceeded (a code-hosting site ≈620 MB → a news site). One compact snapshot of the OS view
// of our own working set, appended to the stage lines so WDP can pull the numbers back.
// 这三个 API 在 15254 上都有;取不到就返回 "mem=n/a",绝不影响调用点。
// Apotheosis (review 2026-09-04 item 1): AppMemoryUsage/Limit only track the UWP App Container's
// assigned memory budget, never the flat 32-bit address space every allocation still has to fit
// in. The map-site abort of 2026-09-04 happened at 57% of that
// budget (pressure level still 0) with GlobalMemoryStatusEx().ullAvailVirtual down to 271 MB — a
// single large contiguous allocation (BitmapTexturePool::Entry vector) failed long before the
// budget percentage said anything was wrong. The driver already reads this same field; sampled
// here too so SampleMemoryPressure() below can react to it and mem.txt can show it.
static unsigned long long AvailVirtMB()
{
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return ~0ULL;   // unavailable -> "plenty", never fabricate pressure
    return ms.ullAvailVirtual / (1024ULL * 1024ULL);
}

static std::string MemSnapshot()
{
    std::string s;
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
        s = "mem=" + std::to_string(used / kMB) + "/" + std::to_string(limit / kMB)
          + "MB(" + std::to_string(pct) + "%) lvl=" + std::string(lvlName);
    } catch (...) { s = "mem=n/a"; }
    unsigned long long availVirt = AvailVirtMB();
    s += " avail_virt=" + (availVirt == ~0ULL ? std::string("n/a") : std::to_string(availVirt) + "MB");
    return s;
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

// Apotheosis: the engine's own view, appended to the OS view above.
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

// Apotheosis: the memory-pressure level we last pushed into
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

// ENGINE THREAD ONLY. Cheap enough for every live tick; the engine is only touched on an actual
// transition. Two independent signals, each with its own hysteresis (up at 65%/80% budget or
// <400/<250 MB free address space, down at 60%/75% or >450/>300 MB, never straight from 2 back to
// 0) — whichever wants more pressure wins, since either running out is a real abort risk (review
// 2026-09-04 item 1: the map-site abort happened with the budget signal still at level 0).
static void SampleMemoryPressure()
{
    unsigned long long used = 0, limit = 0;
    bool haveBudget = true;
    try {
        used = Windows::System::MemoryManager::AppMemoryUsage;
        limit = Windows::System::MemoryManager::AppMemoryUsageLimit;
    } catch (...) { haveBudget = false; }
    unsigned long long availVirt = AvailVirtMB();
    bool haveVirt = (availVirt != ~0ULL);
    if ((!haveBudget || !limit) && !haveVirt) return;

    const int cur = g_engMemPressure;
    int pct = 0;
    // Apotheosis (review 2026-09-04 item 4): start both wishes at 0, not at cur. The two signals are
    //   combined with max(), so a signal that is unavailable this tick (MemoryManager threw, or
    //   VirtualQuery failed) used to keep voting for the current level and veto every de-escalation
    //   -- the engine would then stay at pressure 2 for the rest of the session. Only an available
    //   signal raises the level now; each still de-escalates through its own hysteresis below.
    int wantPct = 0, wantVirt = 0;
    if (haveBudget && limit) {
        pct = (int)((used * 100ULL) / limit);
        if (cur <= 0) {
            if (pct >= 80) wantPct = 2;
            else if (pct >= 65) wantPct = 1;
        } else if (cur == 1) {
            if (pct >= 80) wantPct = 2;
            else if (pct >= 60) wantPct = 1;   // inside the hysteresis band: hold this signal's vote
        } else {
            wantPct = (pct >= 75) ? 2 : 1;     // never straight from 2 back to 0
        }
    }
    if (haveVirt) {
        if (cur <= 0) {
            if (availVirt < 250) wantVirt = 2;
            else if (availVirt < 400) wantVirt = 1;
        } else if (cur == 1) {
            if (availVirt < 250) wantVirt = 2;
            else if (availVirt <= 450) wantVirt = 1;   // inside the band: hold
        } else {
            wantVirt = (availVirt <= 300) ? 2 : 1;
        }
    }
    const int want = (wantPct > wantVirt) ? wantPct : wantVirt;
    if (want != cur) {
        std::string why = "pct=" + std::to_string(pct)
                         + " avail_virt=" + (haveVirt ? std::to_string(availVirt) + "MB" : std::string("n/a"));
        ApplyMemoryPressure(want, "sample " + why);
    }
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
// Apotheosis (privacy review): index 4 = Qwant, the default for a fresh
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
    // Apotheosis (landscape, 0.1.9.43): auto-fill instead of a hard 2 columns, so the tiles use
    //   the width they are given - two across in portrait (360 CSS px), more in landscape - and
    //   the start page reflows on a rotation like any other page. Nothing here is fixed-width.
    h += ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:14px}";
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
    // Apotheosis (review 2026-09-04 item 3): suspend is the last edge that can swallow a
    //   manipulation whole. VisibilityChanged normally fires first, but nothing guarantees it.
    EndGesture(GestureEnd::Suspend);
    // Apotheosis (review 2026-09-04 item 4): the deferral is completed HERE, on the UI thread,
    //   either when the engine's flush comes back or when a 2 s guard timer fires - whichever is
    //   first. It used to be completed from inside the engine job, so an engine that was mid-load
    //   (a navigation or a scroll job ahead of us in the FIFO queue) held the deferral until PLM's
    //   few seconds ran out and the app was killed with no crash.txt - which is exactly what the
    //   "kill without a dump" reports look like. Nothing here waits on the engine: the flush is
    //   still posted and still runs, it just no longer decides when we answer the shell. If the
    //   timer wins, the process is frozen mid-flush - the same outcome as today, minus the kill.
    m_suspendDeferral = deferral;
    if (!m_suspendTimer) {
        m_suspendTimer = ref new Windows::UI::Xaml::DispatcherTimer();
        m_suspendTimer->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(
            [this](Platform::Object^, Platform::Object^) { CompleteSuspendDeferral(); });
    }
    Windows::Foundation::TimeSpan iv; iv.Duration = 2LL * 10000000LL;   // 2 s, in 100 ns units
    m_suspendTimer->Stop();
    m_suspendTimer->Interval = iv;
    m_suspendTimer->Start();

    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    WebEngine::instance().post([disp, self]() {
        try { WebCoreFlushCookiesToDisk(); } catch (...) {}
        // Apotheosis (M4): 同理落盘性能日志 —— 环形缓冲平时只在导航完成时写盘,挂起后进程可能被
        // 系统直接终止,未落盘的行就丢了。关闭时为 no-op。
        try { WebCorePerfFlush(); } catch (...) {}
        try {
            disp->RunAsync(CoreDispatcherPriority::High, ref new DispatchedHandler([self]() {
                MainPage^ s = self.Get(); if (!s) return;
                s->CompleteSuspendDeferral();
            }));
        } catch (...) {}
    });
}

// Apotheosis (review 2026-09-04 item 4): idempotent, UI thread only. Whoever gets here first -
//   the engine flush's UI hop or the 2 s guard timer - answers the shell; the other one finds the
//   deferral gone and does nothing.
void MainPage::CompleteSuspendDeferral()
{
    if (m_suspendTimer) { try { m_suspendTimer->Stop(); } catch (...) {} }
    if (m_suspendDeferral == nullptr) return;
    auto d = m_suspendDeferral;
    m_suspendDeferral = nullptr;
    try { d->Complete(); } catch (...) {}
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

// Apotheosis (landscape/rotation, 0.1.9.41): the engine viewport, in engine pixels. NOT a constant
// any more. It used to be, and that was the whole landscape bug: the ANGLE window surface was
// created once at 720x1080 with EGLRenderSurfaceSizeProperty (a FIXED surface, which ANGLE then
// scales to whatever the panel's rectangle is), and the engine laid out and composited at the same
// two numbers for ever. Rotate the phone and the panel becomes landscape while the surface stays
// portrait, so the portrait picture is stretched across it - exactly what it looked like.
//
// Now: 720 engine px on the SHORT side of the presenting panel, the long side in proportion, so
// the engine viewport always has the panel's aspect ratio (no distortion in either orientation)
// and a rotation lays the page out at the new width the way a phone browser does. The portrait
// width is still 720, so nothing about the way pages look at rest has changed. See
// MainPage::ComputeEngineViewport / UpdateEngineViewport, and EnableGpu() for the ANGLE side.
//
// THREADING: written on the UI thread only, read on both (every engine call sizes its buffer from
// them). An aligned int is never torn on ARM32, and a stale read can only mean a buffer sized for
// the other orientation - which is why every engine buffer is allocated at EngineBufferBytes(),
// the high-water mark over every viewport this session has had, and never at kW*kH*4 directly.
static int kW = 720, kH = 1080;
// The reference short side. Portrait width stays exactly what it has always been.
static const int kEngineShortSidePx = 720;
// Ceiling on the engine viewport, in pixels. The sizes this device actually asks for are around
// 0.9 MPixel in either orientation; this is roughly twice that, and exists only as a stop on the
// surface-mismatch path in UpdateEngineViewport - rastering several times the intended number of
// pixels on an ARM32 phone is worse than the stretch it would be fixing.
static const int kMaxEngineViewportPixels = 1800000;

// Apotheosis (landscape/rotation): the size every engine output buffer is allocated at - the
// largest w*h this session has ever asked the engine for, times 4. The driver writes exactly
// w*h*4 bytes for whatever viewport IT currently has, and the UI thread updates kW/kH before the
// WebCoreResize that moves the engine, so during the hand-over one side is always ahead of the
// other. Sizing every buffer at the high-water mark makes that gap harmless in both directions;
// the two orientations differ by a few percent of the pixel count, so it costs nothing real.
static std::atomic<size_t> g_engineBufferBytes { (size_t)720 * 1080 * 4 };
static void NoteEngineViewport(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    const size_t need = (size_t)w * (size_t)h * 4;
    size_t cur = g_engineBufferBytes.load(std::memory_order_relaxed);
    while (need > cur && !g_engineBufferBytes.compare_exchange_weak(cur, need, std::memory_order_relaxed)) { }
}
static size_t EngineBufferBytes() { return g_engineBufferBytes.load(std::memory_order_relaxed); }

// Apotheosis (M4): 页面缩放边界不能超出引擎侧 —— port\WebCoreDriver.cpp 的 WebCoreSetPageScale
//   自己把 scale 钳到 [0.5, 6.0]；harness 若用更宽的上下界，超界的捏合会被引擎悄悄改成别的值，
//   harness 记的 m_pageScale 就和 Page::pageScaleFactor() 对不上（下次捏合基准错）。子区间是安全的。
// Apotheosis: the committed minimum is the engine's own minimum, 0.5 — pinching out past 1:1
//   and letting go now really zooms out, so a long page can be surveyed at a glance.
//   NOTE what "below 1:1" does and does not do: layout still happens at the engine viewport
//   width (kW), so zooming out does NOT reflow a wider desktop layout into the extra room —
//   the same fit-to-width page is simply drawn smaller from the top-left and the right side
//   stays blank. A real wide-viewport "desktop layout" mode (lay out at, say, 980 px and let
//   the page scale be the fit-to-width ratio) is a separate feature, not done here.
static const float kMinPageScale = 0.5f;
static const float kMaxPageScale = 6.0f;
// The live preview never needs to go below what we can commit any more.
static const float kMinLiveScale = 0.5f;
// Apotheosis (pinch on map widgets, 2026-09-06): how much pinch scale is worth one wheel notch when
//   the gesture is routed to the page. A map zooms by a factor of 2 per notch, so a notch must not
//   be cheap; 1.15 makes a full-screen-width pinch (roughly 2x) about five notches, which reads as
//   a continuous zoom without overshooting on the small movements a two-finger gesture always has.
static const float kPinchNotchScale = 1.15f;
// Spring-back animation on the preview transform. Short enough to feel like a release, long
//   enough to read as a movement rather than a jump; runs on the composition thread.
static const int kZoomSpringMs = 180;
// Apotheosis (2837ce0 review item 1): the title/toast row shows while a page loads and for this
//   long after the load finishes (or after any other TitleText write — the code uses it as a toast
//   bar), then slides down behind the URL bar and gives its strip back to the page. Long enough to
//   read a page title or a "Bookmarked" toast, short enough that the row is gone by the time the
//   user starts reading.
static const int kTitleRowIdleMs = 2000;
static const int kTitleRowSlideMs = 200;   // Apotheosis (title row slide): was kTitleRowFadeMs
// Apotheosis: snap band around 1:1. Generous (±33 %, package 7 feedback widened it from ±20 %) on
//   purpose — 1:1 is the one scale that matters (fit-to-width, crisp text), a pinch is an
//   accumulating product of float deltas, and without a wide band the page ends up parked at 0.94
//   or 1.07 forever, with the error growing on every gesture. Below 0.67 (and above 1.33) the user
//   clearly wants a different scale, so the real value is committed. The 180 ms spring animates the
//   preview across the snap gap so it reads as a release rather than a jump (SpringBackZoom).
static const float kPageScaleSnapTol = 0.33f;
// Apotheosis (double-tap zoom 2026-09-09, route fixed 2026-09-10): how long OnPageTapped holds a
// click waiting for a possible second tap — every tap on a live session, not just one the engine
// calls zoomable, because whether it is zoomable is not known until the engine answers and the
// second tap does not wait for that. It is therefore the latency this feature adds to an ordinary
// tap, and the INTERACTION toggle is what buys it back. Windows has no UWP-surface equivalent of the
// classic
// GetDoubleClickTime() (Windows::UI::ViewManagement::UISettings carries cursor/caret timings, not
// this one) — 300 ms matches the interval mobile Safari/Chrome use for double-tap-to-zoom.
static const int kDoubleTapHoldMs = 300;
// How close (engine px, out of kW=720) a second tap must land to the first to count as the same
// double tap — generous finger-tap tolerance, same order of magnitude as the axis-lock threshold.
static const int kDoubleTapSlopPx = 60;

// 钳到引擎接受的区间，并把接近 1:1 的结果吸附成精确 1.0。
static float SnapAndClampPageScale(float s)
{
    if (!(s > 0.0f)) s = 1.0f;
    if (s < kMinPageScale) s = kMinPageScale;
    if (s > kMaxPageScale) s = kMaxPageScale;
    if (s > 1.0f - kPageScaleSnapTol && s < 1.0f + kPageScaleSnapTol) s = 1.0f;
    return s;
}

// Apotheosis (double-tap zoom, 0.1.9.39): the same clamp WITHOUT the ±33 % snap to 1:1. That snap
// exists so that a pinch, whose scale is whatever the fingers happened to leave behind, comes to
// rest at exactly 1.0 instead of drifting a few percent every time. A double-tap target is not
// that: the driver computed a deliberate number (the width of the column under the finger, or a
// flat 2×) and a target of 1.25-1.32 would be snapped straight back to 1.0, i.e. a double tap that
// visibly does nothing. Only WebCoreSetPageScale's own [0.5, 6.0] range still applies.
static float ClampPageScale(float s)
{
    if (!(s > 0.0f)) s = 1.0f;
    if (s < kMinPageScale) s = kMinPageScale;
    if (s > kMaxPageScale) s = kMaxPageScale;
    return s;
}

// Apotheosis (package 7 feedback: "clamp the live preview to the document edges like
//   Safari/Chrome — content never leaves the viewport at >=1, centred below 1, small rubber-band
//   allowed"): how far the live pinch preview is allowed to drift from a clean anchor-only scale,
//   in DIP, before ClampZoomAxis below starts pulling it back. Eased to 0 by SpringBackZoom on
//   release, exactly like the ±33% page-scale snap band above is eased.
static const double kZoomRubberBandDip = 40.0;

// Apotheosis (map-site pin): how long WebCoreLongPressAt keeps the mouse button down before
//   releasing with the click and the contextmenu event.
//
//   REVERTED to 600 ms on 2026-09-07 after the device A/B this value accidentally became. The
//   0.1.9.18 shortening to 250 ms was an inference - "the contextmenu is the more likely trigger on
//   a mouse-shaped press, the held mousedown is Maps' touch-only pin timer" - and the two sessions
//   disprove it, because the driver logs both halves of the gesture (WebCoreDriver.cpp inputNote):
//     0.1.9.18, pin worked:      long-press ... hold=600 down=0 up=0 click=1 ctx=1/1
//     0.1.9.19, pin never came:  long-press ... hold=250 down=0 up=0 click=1 ctx=1/1  (x4)
//   Same point class (wants=1, i.e. the map canvas), same click, and the contextmenu was sent AND
//   swallowed by the page's own listener (ctx=1/1) in both. So Maps does consume the contextmenu and
//   still does not drop a pin for it: what places the pin is the pointerdown being held long enough
//   for Maps' own press-and-hold timer to fire, and that timer wants roughly half a second. 250 ms is
//   below it, 600 ms is above it - nothing else about the gesture differs.
//
//   Why the hold is not started earlier to win the latency back: WebCoreLongPressAt runs on the
//   engine thread under PumpGuard (g_inPump) and holdPump spins a nested RunLoop for the whole
//   duration, while the harness holds m_interacting for the same span. Beginning the press
//   speculatively on PointerPressed would therefore be uncancellable - a ManipulationDelta arriving
//   150 ms later could not stop it, DragMoveTo/PumpDrag would be dropped by m_interacting, and the
//   drag route's phase-0 press (whose return value is what decides whether the gesture belongs to
//   the page at all, see PumpDrag) would never be sent. Every pan that starts from a stationary
//   finger - the normal way one pans a map - would lose its first ~600 ms and then end in a stray
//   click/contextmenu. Smooth panning outranks half a second of pin latency, so the trigger stays
//   XAML's Holding.
//
//   The finger therefore still feels two stages: XAML's own recognizer needs roughly 500 ms-1 s of
//   stationary touch before it raises Started at all (not shortenable from here), then this hold.
//   If the latency is worth another A/B, 500 is the next value to try - the "long-press ... hold="
//   line in crash.txt always says which one actually ran.
static const int kLongPressEngineHoldMs = 600;

// Apotheosis (bug fix 2026-09-06 evening, device tests 0.1.9.16/0.1.9.18): keyboard avoidance.
//   TWO mechanisms can move the bottom chrome when the on-screen keyboard appears and NEITHER of
//   them knows about the other:
//     (a) the inset path — ApplicationView::VisibleBounds vs CoreWindow::Bounds → RootGrid's bottom
//         Padding (ApplyViewInsets). On W10M this reserves the software navigation bar's strip; on a
//         shell where VisibleBounds ALSO shrinks for the input pane it would reserve the keyboard too.
//     (b) this path — a RenderTransform on the nav bar + the content-row overlays.
//   0.1.9.16 (raw OccludedRect.Height) assumed (a) never sees the keyboard; 0.1.9.17/18
//   (OccludedRect.Height - m_lastInsetBottom) assumed the occluded rect runs all the way down to the
//   window edge, i.e. that it swallows the nav bar strip. Device verdict on 0.1.9.18: the bar sits
//   LOWER than before, by about the nav-bar inset — so the occluded rect covers the keyboard ONLY,
//   sitting on top of the nav-bar strip, and 0.1.9.16 was right by luck.
//   Neither guess is needed. There is one requirement — the nav bar's bottom edge must land exactly
//   on the keyboard's TOP edge — and both quantities are measurable:
//       resting bottom edge (window coords) = CoreWindow::Bounds.Height - <RootGrid bottom padding>
//       keyboard top edge   (window coords) = InputPane::OccludedRect.Y
//       shift               = resting bottom edge - keyboard top edge, clamped to >= 0
//   Using OccludedRect.Y instead of .Height makes the result independent of how far down the rect
//   extends, and folding the CURRENT inset in makes it self-correcting against (a): if VisibleBounds
//   does shrink for the keyboard, the padding alone already puts the bar in the right place and this
//   formula yields 0 — no summing, no double application. ApplyViewInsets() re-runs it whenever the
//   insets change, so whichever mechanism moves first, the end state is the same.
//   The nav bar stays a RenderTransform (not a layout Margin/Padding): Row 1 (nav bar) and Row 0
//   (content, which owns GpuPanel) share RootGrid's row list, so growing Row 1 to make room for the
//   keyboard would shrink Row 0 and resize GpuPanel — the ANGLE swap-chain-rebuild crash class the
//   2026-09-04 review already fixed once (see ApplyViewInsets' GpuPanel comment). A translate leaves
//   every row's size untouched.

// DIP -> nearest whole device pixel, using the same double back. RenderTransform (unlike layout)
//   is never rounded by the XAML layout engine, so an unrounded DIP value can land on a different
//   fractional device pixel depending on which element's transform carries it; snapping once, before
//   handing the same value to both transforms, makes the composited result deterministic. Falls back
//   to 1:1 if DisplayInformation is unavailable (XamlReader::Load fallback path, or an odd Insider
//   build) — shared by RoundToDevicePixel and the stage.txt diagnostic below.
static double CurrentDeviceScale()
{
    double scale = 1.0;
    try {
        auto di = Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
        if (di) scale = di->RawPixelsPerViewPixel;
    } catch (...) {}
    if (!(scale > 0.0)) scale = 1.0;
    return scale;
}

static double RoundToDevicePixel(double dip)
{
    double scale = CurrentDeviceScale();
    return std::round(dip * scale) / scale;
}

// The bottom chrome row's declared Height in MainPage.xaml. Only used as the safety clamp below —
//   the shift may never carry the bar so far up that less than one bar's worth of window is left.
// Apotheosis (bar tightening, 0.1.9.21 feedback): bar shrunk from 62 to 48 DIP (address pill went
//   from 42 to 36 high, its own vertical margin from 9 to 6) — keep this literal in sync with the
//   Grid.Row="1" Height in MainPage.xaml or the keyboard-shift clamp allows too much travel.
static const double kNavBarHeightDip = 48.0;

// Compact DIP formatting for the stage.txt diagnostics — std::to_string(double) writes six decimals
//   per number, which turns one keyboard line into 200 characters of noise. One decimal is finer than
//   any device pixel we can address (RawPixelsPerViewPixel is 2.5 at most here).
static std::string Dip(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return std::string(buf);
}

// Apotheosis (bug fix 0.1.9.44, "rotate with the keyboard up and the whole page jumps"): does
// this InputPane rectangle belong to the window we are in right now? A rotation does not raise
// Showing before the panel resizes, and InputPane::OccludedRect keeps answering with the PREVIOUS
// orientation's rectangle for a moment - so 0.1.9.43's re-query picked up a landscape rect
// (top 186.3, height 173.8), measured it against the portrait window height of 640, and asked
// KeyboardShiftFor for a 454 DIP shift of the whole bottom chrome. The rectangle carries its own
// proof: the on-screen keyboard is docked, so it spans the window's width and its bottom edge IS
// the window's bottom edge (at most the navigation-bar strip short of it, which is what
// insetBottom allows for). A landscape rectangle fails both tests against a portrait window.
// Rejecting is safe: the shift stays where it is until the shell answers properly or raises
// Showing, which it does on the device within a few hundred ms.
static bool KeyboardRectPlausible(double winW, double winH, double insetBottom,
                                  const Windows::Foundation::Rect& occ)
{
    if (!(winH > 0.0) || !(occ.Height > 1.0f))
        return false;
    if (occ.Y < 0.0f || (double)occ.Y > winH)
        return false;
    const double bottomEdge = (double)occ.Y + (double)occ.Height;
    if (bottomEdge < winH - insetBottom - 8.0 || bottomEdge > winH + 8.0)
        return false;
    if (winW > 0.0 && std::abs((double)occ.Width - winW) > 8.0)
        return false;
    return true;
}

// Apotheosis (bug fix 2026-09-06 evening): the one keyboard geometry computation — see the block
//   comment above for the derivation. Everything is in CoreWindow-local DIP: winHeight is
//   CoreWindow::Bounds.Height, insetBottom is what ApplyViewInsets() last put into RootGrid's bottom
//   Padding, and kbTop/kbHeight are InputPane::OccludedRect's Y/Height.
//   `mode` (written to the log) says which branch produced the value:
//     edge   - the normal path, anchored on the occluded rect's TOP edge (layout-independent).
//     height - the rect has a height but a Y we cannot trust (0, or below the window); fall back to
//              the pre-0.1.9.17 behaviour, which device-tested closest to correct.
//     none   - no usable rect at all: do not move.
static double KeyboardShiftFor(double winHeight, double insetBottom,
                               double kbTop, double kbHeight, const char** modeOut)
{
    const double restingBottom = winHeight - insetBottom;   // nav bar's bottom edge at rest
    double shift = 0.0;
    const char* mode = "none";
    if (kbHeight > 0.0 && kbTop > 0.0 && kbTop <= winHeight) {
        shift = restingBottom - kbTop;
        mode = "edge";
    } else if (kbHeight > 0.0) {
        shift = kbHeight;
        mode = "height";
    }
    if (!(shift > 0.0)) shift = 0.0;
    // Never fling the chrome off the top of the window, whatever the shell reports.
    const double maxShift = (winHeight > kNavBarHeightDip) ? (winHeight - kNavBarHeightDip) : 0.0;
    if (shift > maxShift) shift = maxShift;
    *modeOut = mode;
    return RoundToDevicePixel(shift);
}

// One axis of the live-pinch clamp (ApplyLiveZoom). Works entirely in engine px (WebCoreGetScrollState
//   units) — the caller converts the result to DIP.
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
    // Apotheosis (landscape/rotation): both paths size from EngineBufferBytes(), never from
    //   kW*kH directly - see the note at kW. The shared present buffer additionally has to GROW
    //   when a rotation raises the high-water mark; replacing the shared_ptr is safe because every
    //   caller captures its own copy into the engine lambda, so work already in flight keeps the
    //   block it was handed alive until it is done with it.
    if (present) {
        static std::shared_ptr<std::vector<uint8_t>> s_buf;
        if (!s_buf || s_buf->size() < EngineBufferBytes())
            s_buf = std::make_shared<std::vector<uint8_t>>(EngineBufferBytes());
        return s_buf;
    }
    return std::make_shared<std::vector<uint8_t>>(EngineBufferBytes(), 0);
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

    // Apotheosis (landscape/rotation, 0.1.9.41): watch the presenting elements for size changes
    //   from the very first frame, so a rotation re-lays the page out at the new width instead of
    //   stretching the old picture over the new panel. The software path is live before GPU comes
    //   up (and stays the fallback), so ContentArea is wired here; GpuPanel is wired here too and
    //   again when GPU comes up, whichever happens first.
    WirePresentPanelSizeChanged();

    // Apotheosis (suggestion tap, 2026-09-10): the dropdown must not collapse while a finger is down
    //   on it — see m_suggestPressed for why it did, and why that ate the tap. AddHandler with
    //   handledEventsToo is required: the Button inside marks the pointer events handled, and a
    //   plain `SuggestPanel->PointerPressed +=` would never see them.
    try {
        if (SuggestPanel) {
            SuggestPanel->AddHandler(Windows::UI::Xaml::UIElement::PointerPressedEvent,
                ref new Windows::UI::Xaml::Input::PointerEventHandler(this, &MainPage::OnSuggestPointerDown), true);
            SuggestPanel->AddHandler(Windows::UI::Xaml::UIElement::PointerReleasedEvent,
                ref new Windows::UI::Xaml::Input::PointerEventHandler(this, &MainPage::OnSuggestPointerUp), true);
            SuggestPanel->AddHandler(Windows::UI::Xaml::UIElement::PointerCanceledEvent,
                ref new Windows::UI::Xaml::Input::PointerEventHandler(this, &MainPage::OnSuggestPointerUp), true);
            SuggestPanel->AddHandler(Windows::UI::Xaml::UIElement::PointerCaptureLostEvent,
                ref new Windows::UI::Xaml::Input::PointerEventHandler(this, &MainPage::OnSuggestPointerCaptureLost), true);
        }
    } catch (...) {}

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
                // Apotheosis (review 2026-09-04 item 3): a manipulation cannot survive going to the
                //   background, and its ManipulationCompleted may never arrive. Before this, the
                //   handler left m_pinching, m_manipActive, the nested-scroll state and the drag
                //   state set - and with m_pinching stuck, ApplyLiveZoom keeps owning the presenting
                //   layer's transform for the rest of the session.
                EndGesture(GestureEnd::Visibility);
                StopLiveMode();
                // cookie 落盘(JSON Lines 快照):UWP 挂起的应用可能被系统直接终止、不会再回调任何
                // 生命周期事件,切后台这一刻是最后的安全落盘时机。引擎线程异步(线程铁律:UI 线程
                // 绝不同步 wait 引擎),不等它做完就返回——反正马上要挂起,没有下一步依赖它的操作。
                WebEngine::instance().post([]() { try { WebCoreFlushCookiesToDisk(); } catch (...) {} });
            }
        });
    // Apotheosis (privacy review): "Wi-Fi only" prefetch has to follow the connection. The event
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
                    // Apotheosis: Medium is the level we actually
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
    //   菜单键那一行,NavBarShift)上移键盘高度。网页表单输入(ImeBox)不上移——引擎自管把聚焦框滚进视口。
    //   Apotheosis (2837ce0 review item 1): the title row is no longer a chrome row above this one —
    //   it is a bottom-anchored overlay in the content row (TitleRow), so it rides along with the
    //   suggestion dropdown through ShiftSuggestPanel() instead of staying put.
    SetupKeyboardShiftTransforms();
    try {
        auto ip = Windows::UI::ViewManagement::InputPane::GetForCurrentView();
        ip->Showing += ref new Windows::Foundation::TypedEventHandler<
            Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^>(
            [this, ip](Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^ e) {
                // Apotheosis (2026-09-04): the title row must stay visible for as long as the
                //   on-screen keyboard is up - it rides above the address bar through
                //   ShiftSuggestPanel(), so hiding it while typing is what leaves the strip empty.
                m_titleRowPinned = true;
                RevealTitleRow();
                // Apotheosis (bug fix 2026-09-06 evening): record the geometry, then let
                //   ApplyKeyboardShift() decide what (if anything) is left for this path to do —
                //   see the block comment on KeyboardShiftFor(). Recording is UNCONDITIONAL: the
                //   0.1.9.18 device log had no keyboard line at all because the old handler did all
                //   of its work, logging included, inside `if (m_urlFocused && NavBarShift)`, so a
                //   Showing that arrives before UrlBox's GotFocus left no trace and no shift.
                //   OnUrlGotFocus() calls ApplyKeyboardShift() too, which repairs exactly that order.
                Windows::Foundation::Rect occ(0.0f, 0.0f, 0.0f, 0.0f);
                if (e) occ = e->OccludedRect;
                const char* src = "event";
                if (!(occ.Height > 0.0f) && ip) {   // some builds report an empty rect on the first dispatch
                    try { occ = ip->OccludedRect; src = "requery"; } catch (...) {}
                }
                m_kbVisible = occ.Height > 0.0f;
                m_kbTop = m_kbVisible ? (double)occ.Y : 0.0;
                m_kbHeight = m_kbVisible ? (double)occ.Height : 0.0;
                // Apotheosis (0.1.9.44): the shell telling us where the keyboard IS retires any
                //   rejected re-query - this rectangle is current by construction.
                m_kbMetricsStale = false;
                m_kbRecheckTries = 0;
                WriteStage(("keyboard-show occ=" + Dip(occ.X) + "," + Dip(occ.Y)
                            + " " + Dip(occ.Width) + "x" + Dip(occ.Height)
                            + " src=" + src
                            + " urlfocus=" + (m_urlFocused ? "1" : "0")
                            + " inset=" + Dip(m_lastInsetBottom)
                            + " scale=" + Dip(CurrentDeviceScale())).c_str());
                ApplyKeyboardShift("show");
                if (m_kbShiftApplied > 0.0 && e)
                    e->EnsuredFocusedElementInView = true;   // 已自行让位,系统勿再额外滚动
            });
        ip->Hiding += ref new Windows::Foundation::TypedEventHandler<
            Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^>(
            [this](Windows::UI::ViewManagement::InputPane^, Windows::UI::ViewManagement::InputPaneVisibilityEventArgs^ e) {
                // Apotheosis (2026-09-04): keyboard gone - unless the address bar still has focus
                //   (it keeps its own pin, OnUrlLostFocus clears that), hand the row back to the
                //   usual grace period.
                if (!m_urlFocused) { m_titleRowPinned = false; RevealTitleRow(); }
                // Apotheosis (bug fix 2026-09-06 evening): unconditional too, so a device log always
                //   has a matched show/hide pair even when nothing was shifted.
                const double prev = m_kbShiftApplied;
                m_kbVisible = false;
                m_kbTop = 0.0;
                m_kbHeight = 0.0;
                m_kbMetricsStale = false;
                m_kbRecheckTries = 0;
                WriteStage(("keyboard-hide occ=" + Dip(e ? e->OccludedRect.Height : 0.0f)
                            + " inset=" + Dip(m_lastInsetBottom)
                            + " prevshift=" + Dip(prev)).c_str());
                ApplyKeyboardShift("hide");
                if (prev > 0.0 && e)   // 仅当我们上移过才认领(设置页文本框靠系统自身滚动恢复,别干扰)
                    e->EnsuredFocusedElementInView = true;
            });
    } catch (...) {}
    // Apotheosis (review 2026-09-03): DISPLAY toggle "Hide navigation bar". Wired here rather than
    //   with a Toggled="" attribute in the XAML: the hand-rolled code-behind generator
    //   (port\gen-xaml-codebehind.ps1) knows no Toggled event, and an event attribute it cannot
    //   resolve would make XamlReader::Load throw on that fallback path.
    if (SetHideNavBarSwitch)
        SetHideNavBarSwitch->Toggled += ref new Windows::UI::Xaml::RoutedEventHandler(
            this, &MainPage::OnHideNavBarToggled);
    // Apotheosis (review 2026-09-04 item 2b): same reason as above — the hand-rolled generator has
    //   no Toggled event to wire from a XAML attribute.
    if (SetHideStatusBarSwitch)
        SetHideStatusBarSwitch->Toggled += ref new Windows::UI::Xaml::RoutedEventHandler(
            this, &MainPage::OnHideStatusBarToggled);
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
    // Apotheosis (2837ce0 review item 1): the title row auto-hides when the page is idle, but the
    //   code writes TitleText from ~25 places as a toast bar ("Bookmarked", "Link copied", update
    //   check, download progress, …) — every one of those has to bring the row back or the message
    //   would be written into a collapsed element and never seen. Rather than touching all of them
    //   (and every future one), listen on the dependency property itself: one callback, no call
    //   site knows the row can be hidden. RegisterPropertyChangedCallback is 8.1+, so it is there
    //   on 15254; the try/catch is only for the XamlReader::Load fallback path where TitleText
    //   could be null.
    try {
        if (TitleText)
            TitleText->RegisterPropertyChangedCallback(
                Windows::UI::Xaml::Controls::TextBlock::TextProperty,
                ref new Windows::UI::Xaml::DependencyPropertyChangedCallback(
                    [this](Windows::UI::Xaml::DependencyObject^, Windows::UI::Xaml::DependencyProperty^) {
                        RevealTitleRow();
                    }));
    } catch (...) {}
    ApplyViewInsets();
    RevealTitleRow();   // visible at startup, then the same ~2 s grace as everywhere else
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
            auto rgba = std::vector<uint8_t>(EngineBufferBytes(), 0);
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

// Apotheosis (review 2026-09-04 item 2b): DISPLAY toggle "Hide status bar". The constructor already
//   makes the bar translucent and extends content under it (BackgroundOpacity 0, ForegroundColor
//   set so the clock stays readable) — that treatment stays on regardless. This toggle goes further
//   and removes the strip entirely via StatusBar::HideAsync()/ShowAsync(), which is expected to move
//   ApplicationView::VisibleBounds the same way SuppressSystemOverlays does for the bottom strip, so
//   the top inset ApplyViewInsets() computes drops to (near) 0 on its own once VisibleBoundsChanged
//   fires; called here too so the freed strip is used in the same frame.
//   UI THREAD ONLY.
void MainPage::ApplyHideStatusBarSetting()
{
    try {
        if (!Windows::Foundation::Metadata::ApiInformation::IsTypePresent(L"Windows.UI.ViewManagement.StatusBar"))
            return;
        auto sb = Windows::UI::ViewManagement::StatusBar::GetForCurrentView();
        if (sb == nullptr) return;
        if (m_hideStatusBar) sb->HideAsync();
        else sb->ShowAsync();
    } catch (...) { return; }
    ApplyViewInsets();
}

// Takes effect while the settings page is still open, same as OnHideNavBarToggled; HideSettings()
//   re-reads it anyway and persists it.
void MainPage::OnHideStatusBarToggled(Platform::Object^, RoutedEventArgs^)
{
    if (!SetHideStatusBarSwitch) return;
    if (m_hideStatusBar == SetHideStatusBarSwitch->IsOn) return;
    m_hideStatusBar = SetHideStatusBarSwitch->IsOn;
    ApplyHideStatusBarSetting();
}

// Apotheosis (bug fix 2026-09-06 evening): give the two content-row overlays that ride with the nav
//   bar a keyboard transform of their OWN, once, at construction.
//   SuggestPanel had none in the XAML, so the old lazy `el->RenderTransform = new TranslateTransform`
//   worked for it. TitleRow, however, already carries TitleRowShift — the transform RevealTitleRow()/
//   CollapseTitleRow() ANIMATE. Writing the keyboard offset into that same transform could never
//   work: a Storyboard's value wins over a local one while it runs and, with the default
//   FillBehavior=HoldEnd, keeps winning after it completes — and RevealTitleRow() is called from the
//   Showing handler immediately before the shift. So the title row silently stayed at the bottom of
//   the content row while the nav bar and the dropdown moved up, leaving exactly its own 24 DIP of
//   page pixels between the dropdown and the bar: the second of the two reported gaps.
//   A TransformGroup keeps the slide (still targeted by object, so both Storyboards keep working)
//   and adds an independent keyboard translation on top. Built here, before the first RevealTitleRow(),
//   so no animation is ever re-parented mid-flight. UI THREAD ONLY.
void MainPage::SetupKeyboardShiftTransforms()
{
    try {
        if (SuggestPanel && m_suggestKbShift == nullptr) {
            m_suggestKbShift = ref new Windows::UI::Xaml::Media::TranslateTransform();
            SuggestPanel->RenderTransform = m_suggestKbShift;
        }
        if (TitleRow && m_titleKbShift == nullptr) {
            m_titleKbShift = ref new Windows::UI::Xaml::Media::TranslateTransform();
            auto group = ref new Windows::UI::Xaml::Media::TransformGroup();
            if (TitleRowShift) group->Children->Append(TitleRowShift);   // the animated slide
            group->Children->Append(m_titleKbShift);                    // the keyboard offset
            TitleRow->RenderTransform = group;
        }
    } catch (...) {}
}

// Apotheosis (review 2026-09-03): move the URL suggestion dropdown together with the nav bar when
//   the soft keyboard comes up. SuggestPanel lives in the content row (bottom-anchored) while
//   NavBarShift only covers the bottom chrome, so it needs its own translation by the same Y.
//   y == 0 restores the resting position. UI THREAD ONLY.
//   Apotheosis (2837ce0 review item 1): TitleRow is a bottom-anchored overlay in the same subtree
//   since the title row left the bottom chrome, so it needs the identical treatment — otherwise it
//   stays behind the keyboard while the nav bar it belongs above has moved up.
void MainPage::ShiftSuggestPanel(double y)
{
    SetupKeyboardShiftTransforms();   // no-op after the first call; covers a late XamlReader fallback
    if (m_suggestKbShift) m_suggestKbShift->Y = y;
    if (m_titleKbShift) m_titleKbShift->Y = y;
}

// Apotheosis (bug fix 2026-09-06 evening): the single place that decides where the bottom chrome
//   sits while the on-screen keyboard is up — see the block comment on KeyboardShiftFor() for the
//   geometry and for why this is anchored on the occluded rect's top edge and on the CURRENT bottom
//   inset. Idempotent and cheap: called from the InputPane Showing/Hiding handlers, from
//   ApplyViewInsets() whenever the insets actually move (so the inset path and this one can never
//   sum up), and from the UrlBox focus handlers (a Showing that beat GotFocus, or focus leaving
//   while the keyboard stays up). Only logs when the applied value changes, so it cannot spam
//   stage.txt from ApplyViewInsets. UI THREAD ONLY.
void MainPage::ApplyKeyboardShift(const char* why)
{
    // 网页表单输入(ImeBox)不上移——引擎自管把聚焦框滚进视口;只有地址栏编辑态才让位。
    double shiftUp = 0.0;
    const char* mode = "idle";
    double winH = 0.0;
    if (m_kbVisible && m_urlFocused) {
        try {
            auto win = Windows::UI::Core::CoreWindow::GetForCurrentThread();
            if (win) winH = (double)win->Bounds.Height;
        } catch (...) {}
        if (winH > 0.0)
            shiftUp = KeyboardShiftFor(winH, m_lastInsetBottom, m_kbTop, m_kbHeight, &mode);
        else
            mode = "nowindow";
    }
    // Consumed here, whichever way this call ends, so a ticket can never outlive its own call.
    const bool bypass = m_kbShiftBypass;
    m_kbShiftBypass = false;
    // Apotheosis (0.1.9.44): the recorded rectangle is known not to belong to this window (see
    //   KeyboardRectPlausible). Everything computed from it above is meaningless, so leave the
    //   chrome where it is - RefreshKeyboardMetrics has already traced the rejection and armed a
    //   re-query, and the shell's own Showing event repairs it in any case.
    if (m_kbVisible && m_urlFocused && m_kbMetricsStale) return;
    if (shiftUp == m_kbShiftApplied) return;
    // Apotheosis (suggestion tap, 2026-09-10): THE reason a tapped suggestion only closed the list.
    //   Tapping a suggestion presses a Button, a UWP Button takes focus in its OnPointerPressed, and
    //   OnUrlLostFocus runs from there SYNCHRONOUSLY - i.e. with the finger still down - and calls
    //   this. m_urlFocused is already false by then, so the shift drops straight back to 0 and
    //   ShiftSuggestPanel() translates the whole dropdown down by the keyboard height (220 DIP on the
    //   device, stage.txt: "why=show shift=220.0" immediately followed by "why=url-blur shift=0.0").
    //   A RenderTransform moves hit-testing with it, so the button is no longer under the finger when
    //   it lifts: the Button keeps the capture but Click needs the release to be over the button, and
    //   the panel - now behind the still-open keyboard - merely looked closed. The 220 DIP jump also
    //   beat the m_suggestPressed guard, which is set one step later, when the same PointerPressed
    //   finally bubbles up to the panel. So: while the dropdown is up, moving BACK to rest goes through
    //   QueueKeyboardShiftRestore(), which stands down under a finger and is re-armed by the panel's
    //   own pointer handlers. Growing the shift (the keyboard coming up) is never deferred, and
    //   m_kbShiftBypass is that deferred call's one-shot ticket back in here.
    if (shiftUp < m_kbShiftApplied && !bypass
        && SuggestPanel && SuggestPanel->Visibility == Windows::UI::Xaml::Visibility::Visible) {
        WriteStage((std::string("suggest kbshift why=") + why
                    + " from=" + Dip(m_kbShiftApplied) + " to=" + Dip(shiftUp)
                    + " pressed=" + (m_suggestPressed ? "1" : "0")
                    + " act=defer").c_str());
        QueueKeyboardShiftRestore(why);
        return;
    }
    m_kbShiftApplied = shiftUp;
    if (NavBarShift) NavBarShift->Y = -shiftUp;
    ShiftSuggestPanel(-shiftUp);
    WriteStage((std::string("keyboard-shift why=") + why
                + " mode=" + mode
                + " shift=" + Dip(shiftUp)
                + " winh=" + Dip(winH)
                + " kbtop=" + Dip(m_kbTop)
                + " kbh=" + Dip(m_kbHeight)
                + " inset=" + Dip(m_lastInsetBottom)).c_str());
}

// Apotheosis (2837ce0 review item 1): show the title/toast row and, unless a page is loading, arm
//   the grace period after which it slides away again. Called from SetLoading() and — through the
//   TitleText::Text property-changed callback registered in the constructor — from every one of the
//   ~25 places that write a page title or a toast into TitleText, so none of them had to change.
//   UI THREAD ONLY.
void MainPage::RevealTitleRow()
{
    if (!TitleRow) return;
    ++m_titleRowToken;                 // a slide still running (or its Completed) is stale now
    if (m_titleAnim != nullptr) { try { m_titleAnim->Stop(); } catch (...) {} m_titleAnim = nullptr; }
    if (!m_titleRowShown) {
        m_titleRowShown = true;
        TitleRow->Visibility = Windows::UI::Xaml::Visibility::Visible;
        ApplyViewInsets();             // the row takes its strip back from the content area
        // Slide up from behind the URL bar. Row height first (ActualHeight -> declared Height ->
        //   literal, same fallback chain ApplyViewInsets uses for titleH): jump TitleRowShift there
        //   so there is no visible pop, then animate back to rest (Y=0).
        if (TitleRowShift) {
            double rowH = TitleRow->ActualHeight;
            if (!(rowH > 0.0)) {
                const double declared = TitleRow->Height;
                rowH = (declared > 0.0 && declared < 1.0e6) ? declared : 24.0;
            }
            TitleRowShift->Y = rowH;
            try {
                using namespace Windows::UI::Xaml::Media::Animation;
                auto sb = ref new Storyboard();
                auto a = ref new DoubleAnimation();
                a->To = ref new Platform::Box<double>(0.0);
                Windows::Foundation::TimeSpan ts; ts.Duration = (long long)kTitleRowSlideMs * 10000;
                a->Duration = Windows::UI::Xaml::Duration(ts);
                auto ease = ref new QuadraticEase();
                ease->EasingMode = EasingMode::EaseOut;
                a->EasingFunction = ease;
                Storyboard::SetTarget(a, TitleRowShift);
                Storyboard::SetTargetProperty(a, "Y");
                sb->Children->Append(a);
                const unsigned long long token = m_titleRowToken;
                Platform::Agile<MainPage^> self(this);
                sb->Completed += ref new Windows::Foundation::EventHandler<Platform::Object^>(
                    [self, token](Platform::Object^, Platform::Object^) {
                        MainPage^ s = self.Get(); if (!s) return;
                        if (s->m_titleRowToken != token) return;   // overtaken by another Reveal/Collapse
                        s->m_titleAnim = nullptr;
                    });
                m_titleAnim = sb;
                sb->Begin();
            } catch (...) {
                m_titleAnim = nullptr;
                TitleRowShift->Y = 0.0;   // land in the resting position even if the storyboard failed
            }
        }
    } else if (TitleRowShift) {
        // Defensive: Stop() above may have interrupted a slide-down mid-flight. Storyboard::Stop()
        //   is expected to restore the pre-animation local value (Y=0, the row's resting position),
        //   but the row is staying shown either way, so make sure of it rather than trust that.
        TitleRowShift->Y = 0.0;
    }
    if (m_titleHideTimer == nullptr) {
        m_titleHideTimer = ref new Windows::UI::Xaml::DispatcherTimer();
        Windows::Foundation::TimeSpan iv; iv.Duration = (long long)kTitleRowIdleMs * 10000;   // 100 ns
        m_titleHideTimer->Interval = iv;
        m_titleHideTimer->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(
            this, &MainPage::OnTitleRowHideTick);
    }
    try { m_titleHideTimer->Stop(); } catch (...) {}
    // While a page is loading the row stays put (the title is the progress readout). SetLoading(false)
    //   calls back in here and only then does the grace period start.
    // Apotheosis (2026-09-04): so does the keyboard/URL-editing pin - see m_titleRowPinned. The
    //   handlers that clear it call back in here, which is where the grace period then starts.
    // Apotheosis (0.1.9.45): and so does an open link context card - see m_titleRowCtxPinned.
    if (!m_loading && !m_titleRowPinned && !m_titleRowCtxPinned) { try { m_titleHideTimer->Start(); } catch (...) {} }
}

void MainPage::OnTitleRowHideTick(Platform::Object^, Platform::Object^)
{
    if (m_titleHideTimer) { try { m_titleHideTimer->Stop(); } catch (...) {} }   // one-shot
    if (m_loading) return;             // a load started while the grace period ran — keep it up
    if (m_titleRowPinned) return;      // Apotheosis: the keyboard came up while the grace period ran
    if (m_titleRowCtxPinned) return;   // Apotheosis (0.1.9.45): the link card is open - it owns the row
    // Apotheosis (2837ce0 review item 2): collapsing changes the content area's bottom inset, i.e.
    //   ContentArea's size — the one thing a frozen pinch anchor must not have move under it. Wait
    //   the gesture out instead of dropping the collapse, or the row would stay up for good.
    if (m_pinching || m_zoomSpring != nullptr) {
        if (m_titleHideTimer) { try { m_titleHideTimer->Start(); } catch (...) {} }
        return;
    }
    CollapseTitleRow();
}

// Apotheosis (title row slide, review 2026-09-04): slide TitleRowShift's Y from 0 to +rowHeight
//   over kTitleRowSlideMs with an ease-in (starts slow, accelerates away — reads as the row being
//   pulled down out of sight rather than just stopping), then leave the layout entirely
//   (Visibility=Collapsed) so ApplyViewInsets' titleH drops to 0 and the page gets the strip back.
//   TranslateTransform.Y is an independently animatable property, so this runs on the composition
//   thread — no UI-thread work per frame, which matters on this device. Row height comes from the
//   same ActualHeight -> declared Height -> literal fallback chain ApplyViewInsets uses for titleH,
//   so the row always ends up fully behind the URL bar (nav Grid, Grid.Row="1", painted after this
//   content Grid — see the XAML comment) regardless of whether layout has run an arrange pass yet.
//   If the Storyboard cannot start, collapse right away.
void MainPage::CollapseTitleRow()
{
    if (!TitleRow || !m_titleRowShown) return;
    using namespace Windows::UI::Xaml::Media::Animation;
    const unsigned long long token = ++m_titleRowToken;
    Platform::Agile<MainPage^> self(this);
    auto finish = [self, token]() {
        MainPage^ s = self.Get(); if (!s) return;
        if (s->m_titleRowToken != token) return;                 // overtaken by a Reveal()
        s->m_titleAnim = nullptr;
        s->m_titleRowShown = false;
        if (s->TitleRow) {
            s->TitleRow->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
            if (s->TitleRowShift) s->TitleRowShift->Y = 0.0;     // ready for the next Reveal()
        }
        s->ApplyViewInsets();                                    // hand the strip to the content area
    };
    if (!TitleRowShift) { finish(); return; }   // no transform to animate -> collapse right away
    try {
        double rowH = TitleRow->ActualHeight;
        if (!(rowH > 0.0)) {
            const double declared = TitleRow->Height;
            rowH = (declared > 0.0 && declared < 1.0e6) ? declared : 24.0;
        }
        auto sb = ref new Storyboard();
        auto a = ref new DoubleAnimation();
        a->To = ref new Platform::Box<double>(rowH);
        Windows::Foundation::TimeSpan ts; ts.Duration = (long long)kTitleRowSlideMs * 10000;
        a->Duration = Windows::UI::Xaml::Duration(ts);
        auto ease = ref new QuadraticEase();
        ease->EasingMode = EasingMode::EaseIn;
        a->EasingFunction = ease;
        Storyboard::SetTarget(a, TitleRowShift);
        Storyboard::SetTargetProperty(a, "Y");
        sb->Children->Append(a);
        sb->Completed += ref new Windows::Foundation::EventHandler<Platform::Object^>(
            [finish](Platform::Object^, Platform::Object^) { finish(); });
        m_titleAnim = sb;
        sb->Begin();
    } catch (...) {
        m_titleAnim = nullptr;
        finish();
    }
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
    // Apotheosis (2837ce0 review item 2): never move the content edges while a pinch (or its
    //   spring-back) is in flight. Two things would break at once: ContentArea would be resized
    //   under a pinch anchor that SetPinchAnchor froze against the old size (every DIP↔engine-px
    //   factor in ApplyLiveZoom/MapTapToEngine is read off that size), and ApplyPresentTransform()
    //   below rebuilds the transform group — Clear() + re-Append — that the running SpringBackZoom
    //   Storyboard is animating. Since the title row's auto-hide (item 1) is a ~2 s timer, this is
    //   no longer theoretical: a pinch shortly after a load would land right on top of it.
    //   PinchCommit's continuation flushes the deferred call.
    if (m_pinching || m_zoomSpring != nullptr) { m_insetsPending = true; return; }
    double top = 0.0, bottom = 0.0, left = 0.0, right = 0.0;
    // Apotheosis (bug fix 2026-09-06 evening): kept for the stage.txt line at the bottom of this
    //   function — the numbers that say, once and for all, whether this shell moves VisibleBounds for
    //   the on-screen keyboard (the "insets" line's bottom value jumping by the keyboard height when
    //   keyboard-show is logged) or only for the software navigation bar.
    double vbY = 0.0, vbH = 0.0, wbY = 0.0, wbH = 0.0;
    double vbX = 0.0, vbW = 0.0, wbX = 0.0, wbW = 0.0;
    try {
        auto view = Windows::UI::ViewManagement::ApplicationView::GetForCurrentView();
        auto win = Windows::UI::Core::CoreWindow::GetForCurrentThread();
        if (view == nullptr || win == nullptr) return;
        Windows::Foundation::Rect vb = view->VisibleBounds;
        Windows::Foundation::Rect wb = win->Bounds;
        if (!(vb.Height > 0.0f) || !(wb.Height > 0.0f)) return;
        vbY = (double)vb.Y; vbH = (double)vb.Height;
        wbY = (double)wb.Y; wbH = (double)wb.Height;
        vbX = (double)vb.X; vbW = (double)vb.Width;
        wbX = (double)wb.X; wbW = (double)wb.Width;
        top = vbY - wbY;
        bottom = (wbY + wbH) - (vbY + vbH);
        // Apotheosis (landscape, 0.1.9.44): the horizontal pair, computed identically. On this
        //   shell the software navigation bar moves to whichever edge the rotation puts it on -
        //   the bottom in portrait, the RIGHT in landscape - and VisibleBounds shrinks on that
        //   edge either way. Only the vertical pair was ever read, so in landscape every panel in
        //   this file ran the full window width and its right-hand end was covered by the buttons
        //   (device screenshot: the settings menu under the back/home/search column). There is
        //   nothing landscape-specific below: the left inset is applied for the same reason, for
        //   the shells and orientations that put the bar there.
        left = vbX - wbX;
        right = (wbX + wbW) - (vbX + vbW);
    } catch (...) { return; }
    if (!(top > 0.0)) top = 0.0;
    if (!(bottom > 0.0)) bottom = 0.0;
    if (!(left > 0.0)) left = 0.0;
    if (!(right > 0.0)) right = 0.0;
    // Apotheosis (review 2026-09-04 item 2): at least five sources call this (page Loaded, window
    //   SizeChanged, VisibleBoundsChanged, the Hide-status-bar setting, the OOBE/settings overlays),
    //   several of them repeatedly for one user action. Everything below writes layout properties
    //   and rebuilds the presenting element's transform stack, so do none of it while the edges are
    //   where we last left them.
    // Apotheosis (2026-09-04 review, title row reverted to the bottom): TitleBar is gone from up
    //   here — the only thing left at the top edge is Progress, the loading-dots strip, and only
    //   while a page is actually loading. Read its height back from XAML instead of duplicating the
    //   literal (same technique the old titleH used): before the first arrange ActualHeight is still
    //   0, so fall back to the declared Height, then to a literal if that's ever unset.
    double stripH = 0.0;
    if (Progress && Progress->Visibility == Windows::UI::Xaml::Visibility::Visible) {
        stripH = Progress->ActualHeight;
        if (!(stripH > 0.0)) {
            const double declared = Progress->Height;
            if (declared > 0.0 && declared < 1.0e6) stripH = declared;   // false for NaN
        }
        if (!(stripH > 0.0)) stripH = 5.0;
    }
    // Apotheosis (2837ce0 review item 1): the title/toast row is a bottom-anchored OVERLAY in the
    //   content row now (it used to be a layout row of the bottom chrome, permanently costing an
    //   idle page ~24 DIP). Same treatment as the loading strip, mirrored to the bottom edge: while
    //   it is shown its height is the content area's bottom inset, and once RevealTitleRow's timer
    //   has faded it away the page gets that strip back. Read the height from XAML with the same
    //   ActualHeight → declared Height → literal fallback chain as stripH.
    double titleH = 0.0;
    if (TitleRow && TitleRow->Visibility == Windows::UI::Xaml::Visibility::Visible) {
        titleH = TitleRow->ActualHeight;
        if (!(titleH > 0.0)) {
            const double declared = TitleRow->Height;
            if (declared > 0.0 && declared < 1.0e6) titleH = declared;   // false for NaN
        }
        if (!(titleH > 0.0)) titleH = 24.0;
    }
    if (m_insetsValid && top == m_lastInsetTop && bottom == m_lastInsetBottom && stripH == m_lastStripH
        && titleH == m_lastTitleH && left == m_lastInsetLeft && right == m_lastInsetRight)
        return;
    m_insetsValid = true;
    m_lastInsetTop = top;
    m_lastInsetBottom = bottom;
    m_lastInsetLeft = left;
    m_lastInsetRight = right;
    m_lastStripH = stripH;
    m_lastTitleH = titleH;
    Windows::UI::Xaml::Thickness topPad(0, top, 0, 0);
    if (Progress) Progress->Margin = topPad;
    if (FindBar) FindBar->Margin = topPad;
    if (Drawer) Drawer->Padding = topPad;
    if (SettingsPage) SettingsPage->Padding = topPad;
    if (TabSwitcher) TabSwitcher->Padding = topPad;
    if (OobePanel) OobePanel->Padding = topPad;
    // Apotheosis (review 2026-09-04 item 2a/3, updated for the title-row revert): the elements that
    //   actually show engine output — the white content Border in software mode, GpuPanel in
    //   direct-present mode — used to start at y=0 inside their row, so the page ran under the
    //   shell's clock. They get the status-bar inset always, plus the loading strip's height only
    //   while it is visible (stripH is 0 once SetLoading(false) collapses it, called from there so
    //   this re-evaluates on every loading start/stop) — an idle page gets that space back instead
    //   of permanently losing it to a strip nothing is drawing in.
    //   (2837ce0 review item 1) The bottom edge now works the same way for the title/toast row.
    // Apotheosis (bug fix 2026-09-07, first-load loading-strip band): the "+6" breathing gap below
    //   is meant for the IDLE top edge (card look between the status bar and the white content),
    //   but was added unconditionally, so it also landed *below the loading strip itself* — visible
    //   as an extra thin PageBg-coloured band under the dots. That only showed up wherever this
    //   software path is what's on screen while loading — about:home (always software, so every
    //   first-run/new-tab load) and the cold-start window before GPU turns on — because the GPU
    //   panel's translate below (top + stripH, no "+6") has never had that gap, and once GPU takes
    //   over for real navigations the strip already reads flush. Drop the gap while the strip is
    //   visible so both paths match: flush under the dots, breathing room back once idle.
    const double contentTopGap = (stripH > 0.0) ? 0.0 : 6.0;
    if (ContentBorder) ContentBorder->Margin = Windows::UI::Xaml::Thickness(6, top + stripH + contentTopGap, 6, titleH);
    // Apotheosis (2837ce0 review item 1): the suggestion dropdown is anchored to the same bottom
    //   edge as TitleRow and would otherwise cover it while the user types over a loading page.
    //   Lift it by titleH so the two stack (XAML Margin is "8,0" = the left/right 8 stays).
    if (SuggestPanel) SuggestPanel->Margin = Windows::UI::Xaml::Thickness(8, 0, 8, titleH);
    // Apotheosis (review 2026-09-04 item 1): NEVER touch GpuPanel's size here. A margin shrinks the
    //   SwapChainPanel; XAML then reports a new size to ANGLE, which rebuilds the swap chain from
    //   the engine thread's next eglSwapBuffers (the libGLESv2 SEH-AV class of the first-launch
    //   crash) while the driver keeps compositing at a fixed kW x kH — the surviving frames come
    //   out cropped/banded. The panel is TRANSLATED instead: ActualWidth/Height stay exactly what
    //   they were when the surface was created, so the kW/ActualWidth mapping in MapTapToEngine()
    //   and SetPinchAnchor() stays valid, and TransformToVisual(ContentArea -> GpuPanel) folds the
    //   translation in on its own (the target space is the panel's own pre-RenderTransform space).
    //   Cost: the bottom `top + stripH` DIPs of the composited frame fall off the screen edge.
    if (GpuPanel) {
        if (m_gpuInset == nullptr) m_gpuInset = ref new Windows::UI::Xaml::Media::TranslateTransform();
        m_gpuInset->X = 0.0;
        m_gpuInset->Y = top + stripH;
        ApplyPresentTransform();   // re-composes preview transforms + inset onto the right element
    }
    // Apotheosis (landscape, 0.1.9.44): the horizontal insets go on RootGrid together with the
    //   bottom one, because that is the single box every panel in this file lives in - the bottom
    //   chrome row, the content row and all five full-window overlays (Drawer, SettingsPage,
    //   TabSwitcher, LinkMenu, OobePanel) are its children. One padding therefore makes every one
    //   of them span exactly the visible area, and there is no second place that could drift.
    //   The top edge deliberately stays out of it: the page is supposed to run under the status
    //   bar (the elements that must not are given the top inset individually, above).
    //   Consequence to know: the content row shrinks with it, so the presenting panel does too and
    //   the page is laid out for the visible width - which is the point, the right-hand strip of
    //   the page was behind the buttons before. GpuPanel's size change goes through
    //   UpdateEngineViewport like any rotation.
    if (RootGrid) RootGrid->Padding = Windows::UI::Xaml::Thickness(left, 0, right, bottom);
    // Apotheosis (bug fix 2026-09-06 evening): one line per ACTUAL inset change (the early-out above
    //   swallows the many no-op calls), so a device log shows both keyboard-avoidance mechanisms side
    //   by side and the next session can be diagnosed from stage.txt alone.
    WriteStage(("insets l=" + Dip(left) + " t=" + Dip(top)
                + " r=" + Dip(right) + " b=" + Dip(bottom)
                + " strip=" + Dip(stripH) + " title=" + Dip(titleH)
                + " vb=" + Dip(vbX) + "," + Dip(vbY) + " " + Dip(vbW) + "x" + Dip(vbH)
                + " wb=" + Dip(wbX) + "," + Dip(wbY) + " " + Dip(wbW) + "x" + Dip(wbH)
                + " kb=" + (m_kbVisible ? "1" : "0")).c_str());
    // The bottom padding just moved the nav bar's resting position, so whatever the keyboard shift
    //   still owes on top of it has changed with it — recompute from the same single formula rather
    //   than letting the two mechanisms add up. No-op (and silent) when nothing is owed.
    RefreshKeyboardMetrics("insets");
}

// Apotheosis (rotation with the keyboard up, 0.1.9.43): m_kbTop/m_kbHeight are written once, by
// the InputPane Showing handler, and a rotation does not raise Showing again - the keyboard simply
// becomes a different rectangle (it is much shorter in landscape). Everything computed from those
// two numbers was therefore an orientation out of date until the keyboard was dismissed and
// brought back. Re-query first, then let ApplyKeyboardShift decide; it is a no-op when nothing
// moved and it is the only writer of the shift. UI THREAD ONLY.
void MainPage::RefreshKeyboardMetrics(const char* why)
{
    if (!m_kbVisible) {
        m_kbMetricsStale = false;
        m_kbRecheckTries = 0;
        ApplyKeyboardShift(why);
        return;
    }
    double winW = 0.0, winH = 0.0;
    try {
        auto win = Windows::UI::Core::CoreWindow::GetForCurrentThread();
        if (win) { winW = (double)win->Bounds.Width; winH = (double)win->Bounds.Height; }
    } catch (...) {}
    Windows::Foundation::Rect occ(0.0f, 0.0f, 0.0f, 0.0f);
    bool answered = false;
    try {
        auto ip = Windows::UI::ViewManagement::InputPane::GetForCurrentView();
        if (ip) { occ = ip->OccludedRect; answered = true; }
    } catch (...) {}
    // Apotheosis (0.1.9.44): the whole point of this function is that it runs while the window is
    //   changing shape, so what comes back has to be checked against the window it claims to
    //   occlude before a single number of it is kept - see KeyboardRectPlausible.
    if (answered && KeyboardRectPlausible(winW, winH, m_lastInsetBottom, occ)) {
        m_kbTop = occ.Y;
        m_kbHeight = occ.Height;
        m_kbMetricsStale = false;
        m_kbRecheckTries = 0;
    } else {
        m_kbMetricsStale = true;
        WriteStage((std::string("keyboard-stale why=") + (why ? why : "?")
                    + " occ=" + Dip(occ.Y) + "+" + Dip(occ.Height)
                    + " occw=" + Dip(occ.Width)
                    + " win=" + Dip(winW) + "x" + Dip(winH)
                    + " inset=" + Dip(m_lastInsetBottom)
                    + " keep=" + Dip(m_kbShiftApplied)
                    + " try=" + std::to_string(m_kbRecheckTries)).c_str());
        QueueKeyboardMetricsRecheck(why);
    }
    ApplyKeyboardShift(why);
}

// Apotheosis (0.1.9.44): the InputPane answered with a rectangle from the orientation we just
// left. It settles on its own within a frame or two, so ask again on a LOW priority dispatcher hop
// - behind the layout and render work the rotation itself has queued, which is exactly what has to
// finish before the answer can be right. Bounded, and every path that gets a good rectangle (a
// plausible re-query, a Showing event, the keyboard going away) resets the count. UI THREAD ONLY.
void MainPage::QueueKeyboardMetricsRecheck(const char* why)
{
    (void)why;
    if (m_kbRecheckTries >= 4) return;
    ++m_kbRecheckTries;
    Platform::Agile<MainPage^> self(this);
    try {
        this->Dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Low,
            ref new Windows::UI::Core::DispatchedHandler([self]() {
                MainPage^ s = self.Get();
                if (!s) return;
                s->RefreshKeyboardMetrics("kb-recheck");
            }));
    } catch (...) {}
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
    // Apotheosis (2026-09-04 review item 2): the loading strip's visibility is now part of the
    //   content/GpuPanel top-inset math (ApplyViewInsets' stripH) — re-run it here so the inset
    //   actually follows the strip appearing/collapsing instead of only the next unrelated call.
    ApplyViewInsets();
    // Apotheosis (2837ce0 review item 1): the title row is "what am I looking at / what is going
    //   on", so it belongs on screen exactly while something is going on. Loading start pins it
    //   (RevealTitleRow stops the hide timer while m_loading), loading end re-arms the ~2 s grace
    //   so the final page title is readable before the row gets out of the way.
    RevealTitleRow();
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
    // Apotheosis (review 2026-09-04 items 3 + 6b): one exit. This used to clear the pinch flag and
    //   the preview offset by hand and forget the rest - in particular the ZOOM SPRING, whose
    //   Completed handler then ran PinchCommit() on the NEW document (the old page's zoom applied
    //   to the new page), and the nested-scroll/drag state.
    EndGesture(GestureEnd::Navigate);
    m_pageScale = 1.0f;
    m_zoomTransform = nullptr;
    if (m_panTranslate != nullptr) { m_panTranslate->X = 0.0; m_panTranslate->Y = 0.0; }
    m_scrollStateValid = false;
    ++m_scrollStateGen;
    // Apotheosis (review 2026-09-04 item 1): go through ApplyPresentTransform() rather than nulling
    //   the RenderTransforms by hand — the view inset must survive, only the preview is dropped.
    ApplyPresentTransform();

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
    // Apotheosis (landscape, 0.1.9.43): the source of whatever STATIC page ends up on screen -
    //   the start page here, the error page from the engine thread below - kept so a later
    //   viewport change can re-render it. A static page has no session (NavigateTo closes it),
    //   so WebCoreResize has no LocalFrameView to lay out and re-rendering the same HTML at the
    //   new size IS the relayout. Empty for a successful network load, i.e. a live session.
    auto staticHtml = std::make_shared<std::string>(homeHtml);

    WebEngine::instance().post([disp, self, surl, isHome, homeHtml, staticHtml, mySeq]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>(EngineBufferBytes(), 0);
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
                    *staticHtml = eh;   // Apotheosis: re-renderable on a rotation, like the start page
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
                ref new DispatchedHandler([self, rgba, titleCopy, ok, loadOk, sessionActive, comp, links, staticHtml, mySeq]() {
                    MainPage^ s = self.Get();
                    if (!s) return;
                    if (s->m_opSeq != mySeq) return;   // 已被更新操作/看门狗取代,丢弃此迟到回调
                    // Apotheosis (landscape, 0.1.9.43): what a viewport change has to re-render for
                    //   this page, if anything. Empty = a live session, which WebCoreResize handles.
                    s->m_staticHtml = *staticHtml;
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

// Apotheosis (double-tap route, 2026-09-10): every tap writes exactly one "dtap" line before it
// does anything, so a device log tells the whole story even when the route decides to do nothing.
// The 0.1.9.36 log had ZERO dtap lines for a session full of double taps, and that could not be
// told apart from "Tapped never fired", "the toggle was off", "the engine never answered" or "the
// pair was never recognised" — the only two WriteStage calls sat behind the two branches that
// never ran. Volume is one line per tap plus one per policy answer/timer, i.e. nothing next to the
// tile-grid lines in the same file.
void MainPage::DtapTrace(const char* what, const std::string& detail)
{
    WriteStage((std::string("dtap ") + what + " " + detail).c_str());
}

void MainPage::DtapReset()
{
    if (m_dtapHoldTimer) { try { m_dtapHoldTimer->Stop(); } catch (...) {} }
    m_dtapPending = false;
    m_dtapPolicyReady = false;
    m_dtapZoomable = false;
    m_dtapSecondSeen = false;
    m_dtapAtMs = 0;
    ++m_dtapGen;   // a WebCoreTapPolicyAt answer still in flight belongs to nobody now
}

void MainPage::DtapCompleteSecond()
{
    // Apotheosis (0.1.9.39): the driver's "zoomable" is about the POINT; whether the page actually
    // moves is decided here, after the harness' own clamp, because only the harness knows the live
    // page scale and its [0.5, 6.0] range. A target that lands within 5 % of where we already are
    // is not a zoom - it is a dead double tap, which is exactly what the 0.1.9.38 log recorded as
    // "scale=1.0->1.0 act=zoom" three times on a mobile article. Such a tap gets the clickCount=2
    // click instead, so the page's own dblclick handling still works, and the trace says so.
    const float cur = (m_pageScale > 0.0f) ? m_pageScale : 1.0f;
    const float target = ClampPageScale(m_dtapTargetScale);
    const bool moves = std::fabs(target - cur) > 0.05f * cur;
    const bool zoomable = m_dtapPolicyReady && m_dtapZoomable && moves;
    // Apotheosis (0.1.9.40): the zoom anchor is the driver's outAnchorX/Y (WebCoreTapPolicyAt),
    // already in engine viewport px — the tap point itself, unless the driver picked the centre of
    // a narrower column instead (Safari zoom-to-column). See RunDoubleTapZoom's comment.
    const int anchorPx = m_dtapAnchorPx, anchorPy = m_dtapAnchorPy;
    const int px = m_dtapPx, py = m_dtapPy;
    const bool blocked = (!m_sessionActive || m_loading || m_interacting);
    DtapTrace("second", std::string("at=") + std::to_string(px) + "," + std::to_string(py)
        + " zoomable=" + (zoomable ? "1" : "0")
        + " policy=" + ((m_dtapPolicyReady && m_dtapZoomable) ? "1" : "0")
        + " moves=" + (moves ? "1" : "0")
        + " ready=" + (m_dtapPolicyReady ? "1" : "0")
        + " scale=" + Dip(cur) + "->" + Dip(zoomable ? target : cur)
        + " anchor=" + std::to_string(anchorPx) + "," + std::to_string(anchorPy)
        + " act=" + (blocked ? "drop:busy" : (zoomable ? "zoom" : "dblclick")));
    DtapReset();
    if (blocked)
        return;
    if (zoomable)
        RunDoubleTapZoom(anchorPx, anchorPy, target);
    else
        ForwardClickToEngine(px, py, /*longPress*/ false, /*clickCount*/ 2);
}

void MainPage::OnPageTapped(Platform::Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    HideSuggestions();   // 点页面即收起地址栏建议下拉(否则只能靠导航/清空关 → "关不掉")
    // 取相对 ContentArea(承接手势/点击的层,始终参与布局)的坐标。★ 不能用 RenderImage:直呈现模式下它被
    //   Collapsed(让位给 GpuPanel),对已塌缩元素 GetPosition 坐标无效 → 点击错位(滚动后点底部却命中顶部)。
    //   ContentArea 左上角 = 渲染视口原点,故二者在软件模式下等价,直呈现模式下正确。
    const unsigned long long nowMs = GetTickCount64();
    auto pt = e->GetPosition(ContentArea);
    int px = -1, py = -1; MapTapToEngine(pt.X, pt.Y, px, py);
    const bool inBounds = (px >= 0 && py >= 0 && px < kW && py < kH);
    // Apotheosis (map-site pin, 2026-09-06): the tail of a long press is not a tap. XAML normally
    // raises RightTapped rather than Tapped once a hold has been recognised, but the engine has been
    // given the whole press/hold/release either way - a click on top of it would toggle the map view
    // the long press was meant to avoid.
    const bool holdTail = (m_holdAtMs && nowMs - m_holdAtMs < 1000);
    const char* drop = nullptr;
    if (m_loading) drop = "loading";
    else if (m_interacting) drop = "interacting";
    else if (holdTail) drop = "holdtail";
    else if (!inBounds) drop = "oob";
    // Apotheosis (double-tap route, 2026-09-10): the pair is recognised HERE, on the UI thread, from
    // time and distance alone — never from an engine answer that may still be in flight.
    const bool pairing = !drop && m_sessionActive && m_dtapZoomEnabled && m_dtapPending
        && nowMs - m_dtapAtMs <= (unsigned long long)kDoubleTapHoldMs
        && std::abs(px - m_dtapPx) <= kDoubleTapSlopPx
        && std::abs(py - m_dtapPy) <= kDoubleTapSlopPx;
    std::string act = drop ? (std::string("drop:") + drop)
        : (!m_sessionActive ? std::string("nosession")
        : (!m_dtapZoomEnabled ? std::string("click-off")
        : (pairing ? std::string("second") : std::string("hold"))));
    DtapTrace("tap", std::string("at=") + std::to_string(px) + "," + std::to_string(py)
        + " act=" + act
        + " on=" + (m_dtapZoomEnabled ? "1" : "0")
        + " session=" + (m_sessionActive ? "1" : "0")
        + " pending=" + (m_dtapPending ? "1" : "0")
        + " ready=" + (m_dtapPolicyReady ? "1" : "0")
        + " since=" + std::to_string(m_dtapAtMs ? nowMs - m_dtapAtMs : 0ULL));
    if (drop)
        return;

    // 有会话:一律转发引擎真实点击。引擎命中测试是权威的——正确处理弹窗/遮罩层(z-order)、按钮、表单、
    // 以及链接(锚点默认动作=导航)。链接表感知不到模态层覆盖,故不再"链接表优先"(否则点模态关闭按钮
    // 会被误判成点中被它盖住的下层链接 → 弹窗关不掉)。ForwardClickToEngine 内含链接表兜底。
    if (m_sessionActive) {
        if (!m_dtapZoomEnabled) {
            ForwardClickToEngine(px, py);   // 双击缩放关:一切照旧
            return;
        }
        if (pairing) {
            if (m_dtapPolicyReady) {
                DtapCompleteSecond();
                return;
            }
            // The engine has not answered for the first tap yet (its queue is behind live ticks and
            // composites). Park the decision and restart the timer as the deadline: whichever comes
            // first — the answer or the tick — completes the pair, so a slow or wedged engine costs
            // the zoom, never the tap itself.
            m_dtapSecondSeen = true;
            if (m_dtapHoldTimer) { try { m_dtapHoldTimer->Stop(); m_dtapHoldTimer->Start(); } catch (...) {} }
            return;
        }
        // A fresh, independent tap: hold it for the double-tap interval and ask the engine in
        // parallel whether this point is zoomable. Holding is what makes the pair recognisable at
        // all (see the state-machine comment in MainPage.xaml.h) and costs a tap kDoubleTapHoldMs of
        // latency; the INTERACTION toggle turns the whole route, and that cost, off.
        // WebCoreWantsDragAt is the SAME drag-widget probe the pinch/drag routing already uses
        // (dragWidgetAtPoint): a map/canvas is never zoomed by the harness, it gets the dblclick and
        // zooms itself, exactly like a real touch browser leaves map gestures to the map.
        DtapReset();
        m_dtapPending = true;
        m_dtapAtMs = nowMs;
        m_dtapPx = px; m_dtapPy = py;
        if (m_dtapHoldTimer == nullptr) {
            m_dtapHoldTimer = ref new Windows::UI::Xaml::DispatcherTimer();
            m_dtapHoldTimer->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(
                this, &MainPage::OnDtapHoldTimer);
        }
        Windows::Foundation::TimeSpan iv; iv.Duration = (long long)kDoubleTapHoldMs * 10000;
        m_dtapHoldTimer->Interval = iv;
        try { m_dtapHoldTimer->Start(); } catch (...) {}

        const unsigned long long gen = m_dtapGen;
        const unsigned long long askedAt = nowMs;
        CoreDispatcher^ disp = this->Dispatcher;
        Platform::Agile<MainPage^> self(this);
        WebEngine::instance().post([disp, self, px, py, gen, askedAt]() {
            int drag = 0;
            try { drag = WebCoreWantsDragAt(px, py); } catch (...) { drag = 0; }
            int zoomable = 0; float target = 1.0f; int rc = 0; int reason = -1;
            int anchorX = px, anchorY = py;
            // Apotheosis (0.1.9.40): outAnchorX/Y ARE needed now — the driver may answer with the
            // centre of a narrower column (Safari's zoom-to-column behaviour) instead of the raw tap
            // point, and RunDoubleTapZoom must anchor on THAT. Before this, the harness ignored the
            // driver's anchor and always zoomed on the raw tap point instead, which on a column not
            // centred under the finger put most of the column off-screen — read on the device as
            // "jumps to the middle of the page".
            // rc is carried into the trace: the driver answers kErrBusy/kErrNoSession with
            // zoomable=0, which looks exactly like "the page opted out" and must be tellable apart.
            // reason (0.1.9.40, WebCoreDriver.h WebCoreTapPolicyAt): which rule decided — see the
            // header comment for the 0-6 table (1=already zoomed, 2=no element, 3=viewport disables
            // zoom, 4=mobile-optimised viewport, 5=touch-action opt-out, 6=target within 5%).
            if (!drag) {
                try { rc = WebCoreTapPolicyAt(px, py, &zoomable, &target, &anchorX, &anchorY, &reason); }
                catch (...) { zoomable = 0; rc = -1000; anchorX = px; anchorY = py; }
            }
            try {
                disp->RunAsync(CoreDispatcherPriority::Normal,
                    ref new DispatchedHandler([self, zoomable, target, drag, rc, reason, anchorX, anchorY, gen, askedAt]() {
                        MainPage^ s = self.Get(); if (!s) return;
                        const unsigned long long late = GetTickCount64() - askedAt;
                        if (s->m_dtapGen != gen) {   // superseded by a later tap, or the tap is over
                            s->DtapTrace("policy", std::string("zoomable=") + std::to_string(zoomable)
                                + " drag=" + std::to_string(drag) + " rc=" + std::to_string(rc)
                                + " why=" + std::to_string(reason)
                                + " late=" + std::to_string(late) + " act=drop:stale");
                            return;
                        }
                        s->m_dtapPolicyReady = true;
                        s->m_dtapZoomable = (zoomable != 0);
                        s->m_dtapTargetScale = target;
                        s->m_dtapAnchorPx = anchorX; s->m_dtapAnchorPy = anchorY;
                        s->DtapTrace("policy", std::string("zoomable=") + std::to_string(zoomable)
                            + " drag=" + std::to_string(drag) + " rc=" + std::to_string(rc)
                            + " why=" + std::to_string(reason)
                            + " target=" + Dip(target) + " late=" + std::to_string(late)
                            + " act=" + (s->m_dtapSecondSeen ? "second" : "wait"));
                        if (s->m_dtapSecondSeen)
                            s->DtapCompleteSecond();   // the pair was waiting on exactly this answer
                    }));
            } catch (...) {}
        });
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

// Apotheosis (map-site pin, 2026-09-06): press and hold on a map.
//
// Why this is a gesture of its own: a tap already reaches the page as a clean click - hover move,
// mousedown (buttons 1), mouseup at the identical point, DOM click (WebCoreClickAt) - and on the
// mobile Maps page a click is "toggle the full-screen map view", which is what the site does with
// it. The pin sits behind a LONG press, i.e. behind TIME with the button down, and nothing in the
// harness could ask for that: the tap path presses and releases inside one engine call, and the
// drag route only holds a press while the finger keeps moving.
//
// XAML raises Holding for touch/pen while the finger is still down (HoldingState::Started), and
// again as Completed when it leaves or Canceled when the gesture turns into a manipulation. Only
// Started is acted on. Note that this does NOT mean a hold that later becomes a pan is never
// delivered: Started arrives while the finger is still stationary and is acted on at once, so a
// finger that rests and then pans gets the long press anyway and only afterwards the Canceled the
// gesture really deserved. The new stage.txt line below makes that sequence visible (state=0 then
// state=2 for the same gesture) - if it turns out to cost real pans, the fix is to defer the engine
// call by a beat and drop it on Canceled, not to widen these gates. Whether the point deserves a
// long press at all is decided in the engine (WEBCORE_LONGPRESS_DRAG_WIDGET_ONLY): a
// hold over ordinary article text must keep doing nothing, or press-hold-release over a link would
// open it. The route is deliberately ForwardClickToEngine's - it already owns the busy flag, the
// watchdog, the navigation/title/link resync and the keyboard handling for an engine-side gesture.
void MainPage::OnPageHolding(Platform::Object^, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e)
{
    // Apotheosis (diagnostics 2026-09-07): one stage.txt line per Holding event, written
    //   UNCONDITIONALLY here, before any gate, with every gate's answer on it. Until now the only
    //   trace of a hold was the driver's "long-press" line, and that is emitted at the END of
    //   WebCoreLongPressAt - so any of the five early returns below produced complete silence, and a
    //   report of "the pin does nothing" could not be told apart from "XAML never raised Holding",
    //   "the gesture was swallowed as Canceled", or "m_interacting was still set from the previous
    //   gesture". state is HoldingState (0 Started, 1 Completed, 2 Canceled; -1 = null args), and
    //   the volume is low because XAML only raises Completed/Canceled after it has raised Started.
    //   sincehold is the age of the last delivered hold, i.e. the input to the tap suppression in
    //   OnPageTapped. Read together with crash.txt: a "holding ... state=0 ... gates all clear" line
    //   with no "long-press" line after it means the engine call itself never landed.
    const int holdState = (e == nullptr) ? -1 : static_cast<int>(e->HoldingState);
    const bool started = (holdState == static_cast<int>(Windows::UI::Input::HoldingState::Started));
    int px = -1, py = -1;
    if (started) {
        auto pt = e->GetPosition(ContentArea);
        MapTapToEngine(pt.X, pt.Y, px, py);
    }
    const bool inBounds = (px >= 0 && py >= 0 && px < kW && py < kH);
    const unsigned long long sinceHold = m_holdAtMs ? (GetTickCount64() - m_holdAtMs) : 0ULL;
    WriteStage((std::string("holding state=") + std::to_string(holdState)
        + " at=" + std::to_string(px) + "," + std::to_string(py)
        + " session=" + (m_sessionActive ? "1" : "0")
        + " loading=" + (m_loading ? "1" : "0")
        + " interacting=" + (m_interacting ? "1" : "0")
        + " inbounds=" + (inBounds ? "1" : "0")
        + " sincehold=" + std::to_string(sinceHold)
        + " hold=" + std::to_string(kLongPressEngineHoldMs)).c_str());

    if (!started) {
        // Apotheosis (link context menu, 0.1.9.42): Canceled means the hold became a manipulation,
        // i.e. the finger is panning or pinching. The engine's link answer may still be in flight;
        // dropping it here is what keeps a menu from popping up on top of a pan. Completed (finger
        // lifted) is deliberately NOT a cancel - that is the normal way a long press ends, and the
        // menu is supposed to be there afterwards.
        if (holdState == static_cast<int>(Windows::UI::Input::HoldingState::Canceled))
            CancelPendingLinkMenu("canceled");
        return;
    }
    if (!m_sessionActive || m_loading || m_interacting) return;
    if (!inBounds) return;
    if (LinkMenu && LinkMenu->Visibility == Windows::UI::Xaml::Visibility::Visible) return;   // menu is up: it owns the screen
    HideSuggestions();
    m_holdAtMs = GetTickCount64();
    e->Handled = true;
    // Apotheosis (link context menu, 0.1.9.42): where to put the card if this hold turns out to be
    // over a link. Taken HERE, in RootGrid DIP, because the engine's answer arrives on a later turn
    // of the UI thread with no position on it, and because the menu is a XAML element - engine px
    // would be the wrong space and the wrong scale. (px,py) on the trace line is the engine point
    // the hit test runs at, so a report of "the menu was in the wrong place" can be told apart from
    // "the menu was about the wrong link".
    m_ctxPending = true;
    try {
        auto rp = e->GetPosition(RootGrid);
        m_ctxDipX = rp.X;
        m_ctxDipY = rp.Y;
    } catch (...) {}
    WriteStage((std::string("ctx hold at=") + std::to_string(px) + "," + std::to_string(py)
        + " dip=" + std::to_string((int)m_ctxDipX) + "," + std::to_string((int)m_ctxDipY)
        + " scale=" + std::to_string((int)(m_pageScale * 100 + 0.5f))).c_str());
    ForwardClickToEngine(px, py, /*longPress*/ true);
}

// 把 (px,py) 点击转发给引擎活会话。引擎派发真实鼠标事件并处理默认动作;若触发了会话内导航
// (URL 变化),回 UI 后同步地址栏/前进后退栈/历史。引擎点击失败但命中了链接表 → 退回经典导航。
// Apotheosis (double-tap zoom, 2026-09-09): clickCount>=2 forwards to WebCoreClickAtCount instead of
// WebCoreClickAt, so the engine dispatches a real DOM 'dblclick' (see OnPageTapped's
// m_dtapLastClickMs follow-up). longPress and clickCount>=2 never happen together — a long press is
// its own gesture, checked first.
void MainPage::ForwardClickToEngine(int px, int py, bool longPress, int clickCount)
{
    if (m_interacting) return;
    m_interacting = true;
    SetLoading(true);
    if (m_loadWatchdog) m_loadWatchdog->Start();   // 兜底:若引擎/回调卡死,40s 强制复位
    TitleText->Text = L8(L"处理中…", L"Working…");

    // 链接表命中(供引擎点击失败时退回经典导航)
    // Apotheosis (long press, 2026-09-06): never for a long press. This fallback exists for a click
    // the engine could not turn into a navigation; a long press is not supposed to navigate at all,
    // and it answers kOK for its "not a drag widget" skip - with a link hit in hand that skip would
    // open whatever link happens to sit under the finger.
    auto linkHit = std::make_shared<std::wstring>();
    if (!longPress) {
        for (auto it = m_pageLinks.rbegin(); it != m_pageLinks.rend(); ++it) {
            const PageLink& l = *it;
            if (px >= l.x && px < l.x + l.w && py >= l.y && py < l.y + l.h) { *linkHit = l.url; break; }
        }
    }

    std::wstring prevUrl = m_currentUrl;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = ++m_opSeq;
    // Apotheosis (link context menu, 0.1.9.42): filled by the engine when a long press turns out to
    // be over a link. Non-empty means the page was told NOTHING and the UI callback opens the menu.
    auto ctxUrl = std::make_shared<std::wstring>();

    WebEngine::instance().post([disp, self, px, py, linkHit, prevUrl, mySeq, longPress, clickCount, ctxUrl]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>(EngineBufferBytes(), 0);
        int rc = -999;
        unsigned hashBefore = WebCoreGetFrameHash();
        // Apotheosis (map-site pin, 2026-09-06; latency cut 2026-09-06, kLongPressEngineHoldMs):
        // the long press holds the button down in the engine for kLongPressEngineHoldMs, then
        // releases with the click and sends a contextmenu at the same point - the two things a map
        // can turn into a dropped pin. DRAG_WIDGET_ONLY makes it a no-op (kOK, current frame painted)
        // anywhere else on the page.
        // Apotheosis (double-tap zoom, 2026-09-09): clickCount>=2 (the second tap of a double tap
        // WebCoreTapPolicyAt said is NOT zoomable) goes through WebCoreClickAtCount so the page gets
        // a real 'dblclick'; an ordinary tap (clickCount=1, the overwhelming common case) still calls
        // plain WebCoreClickAt, unchanged.
        // Apotheosis (link context menu, 0.1.9.42): a long press asks the engine what is under the
        // finger BEFORE it presses anything. WebCoreLinkAt is a read-only hit test on this same
        // engine thread, so the probe costs a hit test and not a second round trip - and a hold over
        // a link now answers in milliseconds instead of after kLongPressEngineHoldMs, because the
        // hold pump is never started at all. That is also the whole suppression story: the page is
        // sent no press, no release and no contextmenu, so there is nothing to take back on the UI
        // side (OnPageTapped's existing holdTail rule already drops the tap the release raises).
        // A point that is not an http(s) link - plain text, a canvas / touch-action:none widget, an
        // image, a button - reports 0 and falls straight through to the unchanged long press.
        int ctxRc = 0;
        try {
            if (longPress) {
                // Apotheosis (0.1.9.45): 4096, not the link table's 1200. WebCoreLinkAt reports
                //   kErrBadArgs rather than truncating when the href does not fit, i.e. a hold on a
                //   very long link used to open no menu at all - and the header shows up to 2048
                //   characters now, so the URL has to arrive whole.
                char lu[4096] = "";
                ctxRc = WebCoreLinkAt(px, py, lu, sizeof lu);
                if (ctxRc == 1 && lu[0]) *ctxUrl = Utf8ToWide(lu);
            }
        } catch (...) { ctxRc = -1000; }
        try {
            if (!ctxUrl->empty())
                rc = 0;                      // menu case: nothing dispatched, no frame produced
            else rc = longPress
                ? WebCoreLongPressAt(px, py, kLongPressEngineHoldMs,
                                     WEBCORE_LONGPRESS_CONTEXTMENU | WEBCORE_LONGPRESS_DRAG_WIDGET_ONLY,
                                     rgba->data())
                : (clickCount >= 2 ? WebCoreClickAtCount(px, py, clickCount, rgba->data())
                                   : WebCoreClickAt(px, py, rgba->data()));
        } catch (...) { rc = -1000; }
        unsigned hashAfter = (rc == 0) ? WebCoreGetFrameHash() : hashBefore;
        bool changed = (hashAfter != hashBefore);   // 引擎点击是否改变了画面(区分模态关闭/按钮 vs 死链接)
        int editable = 0;
        try { if (rc == 0) editable = WebCoreFocusedEditable(); } catch (...) {}   // 点中的是不是输入框

        std::wstring navUrl, title;
        auto links = std::make_shared<std::vector<Harness::PageLink>>();
        if (rc == 0 && ctxUrl->empty()) {
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
        int ctxRcCopy = ctxRc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal,
                ref new DispatchedHandler([self, rgba, titleW, navW, links, rcCopy, changedCopy, editableCopy, linkHit, mySeq, ctxUrl, ctxRcCopy, longPress]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    if (s->m_opSeq != mySeq) return;   // 已被取代/看门狗复位,丢弃迟到回调
                    s->m_interacting = false;
                    if (s->m_loadWatchdog) s->m_loadWatchdog->Stop();
                    // Apotheosis (link context menu, 0.1.9.42): the hold was over a link, so the
                    // page was never touched - there is no frame to apply, no navigation to sync and
                    // no link table to refresh. Open the menu and stop here, unless the gesture
                    // became a pan while the probe was in flight (m_ctxPending already cleared).
                    if (!ctxUrl->empty()) {
                        s->SetLoading(false);
                        s->TitleText->Text = ref new String(s->m_currentTitle.empty() ? L"EdgeHTML Reborn" : s->m_currentTitle.c_str());
                        if (s->m_ctxPending) s->ShowLinkMenu(*ctxUrl);
                        s->m_ctxPending = false;
                        return;
                    }
                    if (longPress) {
                        // No link here: the page got the long press it always got. ctxRc is the
                        // probe's answer (0 = no link, negative = no session / busy / no document),
                        // which tells "the hold was over plain text" from "the probe never ran".
                        WriteStage((std::string("ctx none rc=") + std::to_string(ctxRcCopy)).c_str());
                        s->m_ctxPending = false;
                    }
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
        auto rgba = std::make_shared<std::vector<uint8_t>>(EngineBufferBytes(), 0);
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
// Apotheosis (privacy review): is the connection we are on unmetered?
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
    // m_scrollAccum* IS the ledger of what the engine has not been told about yet: every delta that
    // arrives while a WebCoreScrollBy is in flight is added here and goes out with the next one, so
    // nothing can be sent twice or lost.
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
        // Apotheosis: read back where the engine ended up, in the same engine-thread hop that just
        // scrolled and presented. The engine clamps at the document edges, so what it actually
        // applied is generally NOT the delta we sent — only the position tells the truth, and the
        // cache it feeds (m_scroll*/m_content*/m_view*) is what MapTapToEngine, ScrollStateUsable
        // and the pinch clamp read. Cheap: WebCoreGetScrollState does no layout and no paint.
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
                if (s->m_opSeq != mySeq) { s->m_scrollBusy = false; s->m_scrollAccum = 0; s->m_scrollAccumX = 0; return; }   // 被导航/点击取代,丢弃迟到帧+全部残留位移
                if (rcCopy == 0) {
                    if (!present) s->PresentSoftwareFrame(rgba);
                    s->m_lastFrameHash = 0;
                    if (haveState) { s->m_contentW = cw; s->m_contentH = ch; s->m_viewW = vw; s->m_viewH = vh; }
                    s->EngineScrollFrameApplied(sx, sy, haveState, dx, dy);
                }
                s->m_scrollBusy = false;
                if (s->m_scrollAccum != 0 || s->m_scrollAccumX != 0)
                    s->PumpScroll();   // 拖拽期间又攒了位移(含纯横向),继续冲刷
                else {
                    s->SyncLinksAfterScroll(); s->StartLiveMode();   // 滚动停了 → 补链接表 + 重启实时(新视口懒加载/动画)
                }
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
// Map widgets - Leaflet, MapLibre, canvas apps - pan by handling
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
                        // This completion is asynchronous and can land AFTER OnImageManipCompleted;
                        //   the buffered movement goes to the engine as a plain scroll either way.
                        if (fdx != 0 || fdy != 0)
                            s->FreeScrollBy(fdx, fdy);
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

// ===========================================================================
// Apotheosis (pinch on map widgets, 2026-09-06): the WebCoreZoomWheelAt route.
//
// A pinch over a map used to scale the rendered page: the map's own tiles stay at the zoom level
// they were fetched for, so the labels grow blurry and the map shows no more detail than before -
// a picture of a map being zoomed. A map zooms itself on a wheel, so a pinch that starts on one is
// converted into wheel notches at the pinch centre (ctrl held, which is what a page reads as "zoom
// me"), and the map fetches the tiles for its next zoom level.
//
// Same one-post-in-flight shape as PumpDrag/PumpNestedScroll. Notches ACCUMULATE while a post is
// out, so a fast pinch becomes one wheel event with a bigger tick count instead of a queue of
// events the engine works through after the fingers have gone.
// ===========================================================================
void MainPage::ZoomWheelBy(int px, int py, int notches)
{
    if (!m_sessionActive || !notches) return;
    m_pinchPagePx = px; m_pinchPagePy = py;   // the centre can drift with the fingers; latest wins
    m_zoomWheelNotches += notches;
    PumpZoomWheel();
}

void MainPage::PumpZoomWheel()
{
    if (m_zoomWheelBusy || !m_sessionActive) return;
    const int notches = m_zoomWheelNotches;
    if (!notches) return;
    m_zoomWheelNotches = 0;
    m_zoomWheelBusy = true;
    const int px = m_pinchPagePx, py = m_pinchPagePy;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = m_opSeq;   // as PumpDrag: not a token of its own, but superseded by one
    bool present = m_gpuPresent;
    WebEngine::instance().post([disp, self, px, py, notches, mySeq, present]() {
        auto rgba = AcquireEngineBuffer(present);
        int rc = -999;
        try { rc = WebCoreZoomWheelAt(px, py, notches, rgba->data()); } catch (...) { rc = -1000; }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rgba, rcCopy, mySeq, present]() {
                MainPage^ s = self.Get(); if (!s) return;
                s->m_zoomWheelBusy = false;
                if (s->m_opSeq != mySeq) { s->m_zoomWheelNotches = 0; return; }   // navigation/click took over
                if (rcCopy == 1) {
                    // WebCoreZoomWheelAt already composited/presented this frame in present mode,
                    // same contract as WebCoreWheelAt - only the software path needs the blit.
                    if (!present) s->PresentSoftwareFrame(rgba);
                    s->m_lastFrameHash = 0;
                }
                if (s->m_zoomWheelNotches) s->PumpZoomWheel();
                else if (!s->m_pinchPage) { s->SyncLinksAfterScroll(); s->StartLiveMode(); }
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
    m_manipActive = true;   // a finger is on the glass until ManipulationCompleted (incl. inertia)
    m_pendingPanX = 0; m_pendingPanY = 0;
    // Apotheosis (axis lock / rail scrolling): a new gesture decides its own axis from scratch.
    m_axisLockState = AxisLock::Deciding;
    m_axisAccumX = 0.0; m_axisAccumY = 0.0;
    // Apotheosis (touch-lag diagnostic): a new gesture starts a fresh lag window, based from
    //   wherever the engine's last known position was (m_scrollX/Y).
    // Apotheosis (2026-09-07, device bug fix — "1781 px lag at ps=5.699"): m_scrollX/Y is engine px
    //   (CSS px * page scale), so it is only a valid base if it was stamped at THIS gesture's page
    //   scale (m_scrollStateScale == m_pageScale) — grabbing it unconditionally, as before, meant a
    //   gesture that started right after a pinch commit (which leaves the m_scrollX/Y NUMBERS at
    //   their pre-pinch scale until the first post-pinch engine round trip) measured against a base
    //   off by roughly the pinch's own scale ratio, not a frame or two. If the cache is not at the
    //   current scale, defer: m_scrollLagBaseValid stays false and NoteScrollLagPresented() takes the
    //   first same-scale sample it sees as the base instead (same lazy-base idiom as
    //   ScrollStateUsable()) — RequestScrollState() a few lines
    //   down guarantees one arrives shortly. m_scrollLagScale records which scale this gesture is
    //   being measured at (fixed for the gesture's lifetime — pan and pinch cannot overlap).
    m_scrollLagActive = g_perfLogEnabled;
    m_scrollLagStarted = m_scrollLagActive;   // Apotheosis (2026-09-07, clamp/fling fix): gesture-scoped
        // "write the line" gate — m_scrollLagActive itself now freezes early on the first inertial
        // delta (OnImageManipDelta), so OnImageManipCompleted must not gate the write on it too.
    if (m_scrollLagActive) {
        m_scrollLagScale = m_pageScale;
        m_scrollLagBaseValid = (m_scrollStateScale == m_pageScale);
        if (m_scrollLagBaseValid) { m_scrollLagBaseX = m_scrollX; m_scrollLagBaseY = m_scrollY; }
        m_scrollLagFingerX = 0.0; m_scrollLagFingerY = 0.0;
        m_scrollLagMoves = 0;
        m_scrollLagMax = m_scrollLagSum = m_scrollLagLast = 0.0;
        m_scrollLagPendingSinceMs = 0; m_scrollLagMsMax = 0;
        m_scrollLagClamped = 0; m_scrollLagFlingMoves = 0;   // Apotheosis (2026-09-07, clamp/fling fix)
    }
    // Apotheosis (drag as pointer events): a new gesture — drop whatever the previous one left
    // behind before the probe below can answer Drag for this one.
    ++m_dragGen;
    DragReset();
    // Apotheosis: a new gesture starts from the frame that is on screen — ask the engine where it
    // is, so the pinch clamp and the tap mapping work against this gesture's own document bounds
    // instead of whatever the previous one left in the cache.
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
    WebEngine::instance().post([disp, self, px, py, gen]() {
        int drag = 0;
        try { drag = WebCoreWantsDragAt(px, py); } catch (...) { drag = 0; }
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
    else if (m_nestedScrollState == NestedScrollState::Yes) {
        ApplyAxisLock(dx, dy);   // axis lock covers nested scrollables too — never the drag route
        NestedScrollBy(m_pendingPanPx, m_pendingPanPy, dx, dy);
    } else {
        ApplyAxisLock(dx, dy);
        FreeScrollBy(dx, dy);
    }
}

// Apotheosis (axis lock / rail scrolling, developer setting "Axis lock", default ON): Chrome/
// Safari-style one-finger rail scrolling. Decides the axis once from the accumulated raw
// (pre-scale, DIP) translation since gesture start, then holds it for the rest of the gesture
// including inertia — nothing here is reconsidered once Deciding leaves that state, so a curve
// that starts vertical and drifts diagonal stays locked vertical (matches Chrome/Safari, and
// keeps this a one-shot per-gesture decision instead of a per-frame wobble).
static const double kAxisLockThresholdDip = 9.0;   // accumulated distance before deciding (~8-10 DIP)
static const double kAxisLockRatio = 0.35;         // minor/major ratio <= this -> lock (~20 deg off-axis)
void MainPage::UpdateAxisLock(double rawDx, double rawDy)
{
    if (m_axisLockState != AxisLock::Deciding) return;
    m_axisAccumX += rawDx;
    m_axisAccumY += rawDy;
    double ax = std::abs(m_axisAccumX), ay = std::abs(m_axisAccumY);
    if (ax < kAxisLockThresholdDip && ay < kAxisLockThresholdDip) return;   // not moved enough yet
    double major = ax > ay ? ax : ay, minor = ax > ay ? ay : ax;
    if (major > 0.0 && minor / major <= kAxisLockRatio)
        m_axisLockState = (ax >= ay) ? AxisLock::X : AxisLock::Y;
    else
        m_axisLockState = AxisLock::Free;   // too diagonal — free pan for the rest of the gesture
}

void MainPage::ApplyAxisLock(int& dx, int& dy)
{
    if (!m_axisLockEnabled) return;
    if (m_axisLockState == AxisLock::X) dy = 0;        // locked horizontal: zero the vertical drift
    else if (m_axisLockState == AxisLock::Y) dx = 0;   // locked vertical: zero the horizontal drift
}

// ===========================================================================
// Apotheosis: the scroll/bounds cache.
//
// A touch pan posts a coalesced WebCoreScrollBy to the engine thread (FreeScrollBy/PumpScroll); the
// engine scrolls, composites and swaps, and the screen moves with that swap. The UI thread shows no
// preview of its own — it only remembers what the engine reported:
//
//   m_scroll*/m_content*/m_view*  last scroll position and document bounds the engine reported
//                (WebCoreGetScrollState, read in the same engine hop as the scroll itself and
//                seeded at gesture start by RequestScrollState). Read by MapTapToEngine (tap
//                mapping), ScrollStateUsable()/ApplyLiveZoom (the pinch clamp) and the touch-lag
//                diagnostic below. The engine clamps at the document edges, so what it actually
//                applied is generally NOT the delta we sent — only the position tells the truth.
// ===========================================================================

// An engine frame landed. newScrollX/Y is where the engine now is (WebCoreGetScrollState, read on
// the engine thread in the same hop as the WebCoreScrollBy). haveScrollState=false means the call
// failed, and the cache is dropped rather than guessed at.
// fallbackDx/fallbackDy is the coalesced delta that round trip was asked for (PumpScroll's dx/dy);
// only the touch-lag diagnostic uses it, to tell a real lag apart from a document-edge clamp.
void MainPage::EngineScrollFrameApplied(int newScrollX, int newScrollY, bool haveScrollState,
                                        int fallbackDx, int fallbackDy)
{
    if (!haveScrollState) {
        m_scrollStateValid = false;
        return;
    }
    // Apotheosis (2026-09-07, clamp fix): capture what the engine actually moved THIS round trip
    //   (new minus the still-old m_scrollX/Y) before overwriting them, so the lag diagnostic can
    //   tell a real lag apart from a document-edge clamp — see NoteScrollLagPresented().
    const int appliedDx = newScrollX - m_scrollX, appliedDy = newScrollY - m_scrollY;
    m_scrollX = newScrollX;
    m_scrollY = newScrollY;
    m_scrollStateValid = true;
    m_scrollStateScale = m_pageScale;   // Apotheosis (2837ce0 review item 2): stamp the scale it is px in
    NoteScrollLagPresented(appliedDx, appliedDy, fallbackDx, fallbackDy);   // Apotheosis (touch-lag diagnostic)
}

// Apotheosis (touch-lag diagnostic): m_scrollX/m_scrollY were just refreshed from a real engine
// present a few lines above (EngineScrollFrameApplied) — fold the newly-applied delta into the
// gesture's running stats and close out the "finger moved, engine hasn't caught up yet" timing
// window opened in OnImageManipDelta. A no-op outside an active, perf-logging-gated free-scroll
// gesture (m_scrollLagActive), so the call site above costs one branch when the diagnostic is off.
// Apotheosis (2026-09-07, clamp fix): appliedDx/Dy is what m_scrollX/Y actually moved THIS round
//   trip (the caller's new minus its still-old m_scrollX/Y, captured right before it overwrites
//   them); requestedDx/Dy is the same round trip's coalesced ask (EngineScrollFrameApplied's own
//   fallbackDx/fallbackDy parameter — PumpScroll's dx/dy). Both are 0/0-safe no-ops on the
//   lazy-base-adopt path below.
void MainPage::NoteScrollLagPresented(int appliedDx, int appliedDy, int requestedDx, int requestedDy)
{
    if (!m_scrollLagActive) return;
    // Apotheosis (2026-09-07, device bug fix): m_scrollX/Y just refreshed above is only comparable
    //   to this gesture's base (or fit to BECOME the base) if it was stamped at the gesture's own
    //   scale — engine px is CSS px * page scale, so a sample from a different scale epoch is a
    //   different unit, not a stale-by-a-frame number. Should not happen mid free-scroll-gesture
    //   (pinch cannot overlap it) but costs one compare to rule out for certain.
    if (m_scrollStateScale != m_scrollLagScale) return;
    if (!m_scrollLagBaseValid) {
        // First same-scale sample since gesture start — this becomes the base (0 lag on the hop
        // that supplies it, exactly like a gesture that already had a valid cache to start from).
        m_scrollLagBaseX = m_scrollX; m_scrollLagBaseY = m_scrollY;
        m_scrollLagBaseValid = true;
        m_scrollLagPendingSinceMs = 0;   // this span's "waiting for the base" is not a real gap
        return;
    }
    // Apotheosis (2026-09-07, device bug fix — "n=28 max=2045 ps=1.0" / "n=187 max=2709 ps=1.0",
    //   both alongside a low, sane ms_max): WebCoreScrollBy clamps at the document's own edges (this
    //   is the main-frame free-scroll path only — a nested scroller routes through NestedScrollBy
    //   instead and is not tracked here), so appliedDx/Dy can legitimately fall short of requestedDx/Dy —
    //   the finger sum below was built from the SAME requested amount, so a sustained swipe held past
    //   the end of the page keeps adding to m_scrollLagFingerX/Y with nothing on the applied side to
    //   match, growing "lag" without bound even though the engine answered every round trip promptly.
    //   That shortfall is not lag, it is pixels that do not exist — shift the base by it so appliedX/Y
    //   below read as if the request had landed in full, leaving m_scrollX/Y (the real position) and
    //   m_scrollLagFingerX/Y (the real finger total) untouched: base -= shortfall makes
    //   (m_scrollX - base) grow by shortfall, cancelling exactly the finger pixels that could never
    //   have been applied.
    const int shortfallX = requestedDx - appliedDx;
    const int shortfallY = requestedDy - appliedDy;
    if (shortfallX != 0 || shortfallY != 0) {
        m_scrollLagBaseX -= shortfallX;
        m_scrollLagBaseY -= shortfallY;
        ++m_scrollLagClamped;
    }
    const double appliedX = (double)(m_scrollX - m_scrollLagBaseX);
    const double appliedY = (double)(m_scrollY - m_scrollLagBaseY);
    const double lagX = m_scrollLagFingerX - appliedX;
    const double lagY = m_scrollLagFingerY - appliedY;
    const double lag = std::sqrt(lagX * lagX + lagY * lagY);
    m_scrollLagLast = lag;
    if (lag > m_scrollLagMax) m_scrollLagMax = lag;
    m_scrollLagSum += lag;
    if (m_scrollLagPendingSinceMs != 0) {
        const unsigned long long gap = GetTickCount64() - m_scrollLagPendingSinceMs;
        if (gap > m_scrollLagMsMax) m_scrollLagMsMax = gap;
        m_scrollLagPendingSinceMs = 0;   // this span is closed; the next move opens a new one
    }
}

// Apotheosis (2837ce0 review item 2): the last line of defence for the post-pinch jump. The cache
//   (m_scrollX/Y, m_contentW/H, m_viewW/H) is engine px, and engine px per CSS px IS m_pageScale —
//   so the instant a pinch commits, all six fields describe a document that no longer exists.
//   PinchCommit invalidates them explicitly now, but every future path that forgets to would again
//   feed pre-zoom bounds to the live-pinch clamp (ApplyLiveZoom/ClampZoomAxis), and a clamp is
//   precisely a function that turns a zero offset into a non-zero one. Comparing the stamp against
//   the committed scale makes that structurally impossible, and leaves one line in mem.txt whenever
//   it catches something.
bool MainPage::ScrollStateUsable()
{
    if (!m_scrollStateValid) return false;
    if (m_scrollStateScale == m_pageScale) return true;
    m_scrollStateValid = false;
    WriteMemLog("post-pinch correction: dropped scroll cache from scale "
                + std::to_string(m_scrollStateScale) + " (page is now " + std::to_string(m_pageScale)
                + ") before clamping");
    return false;
}

// Seed the scroll/bounds cache at gesture start, so the pinch clamp and the touch-lag diagnostic
// have this gesture's own document bounds from its first delta on.
// Fire-and-forget on the engine thread, exactly like the WebCoreIsScrollableAt hit test — the UI
// thread never waits on the engine. A late answer for a gesture that has been superseded is
// dropped via m_scrollStateGen, and an answer that arrives after a real engine frame must not
// overwrite the position that frame reported, so only the bounds are refreshed in that case.
void MainPage::RequestScrollState()
{
    if (!m_sessionActive) return;
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
                    s->m_scrollStateScale = s->m_pageScale;   // Apotheosis (2837ce0 review item 2)
                }
            }));
        } catch (...) {}
    });
}

// Apotheosis: one place that decides what sits on the presenting element. The pinch preview scale
// and the clamp translation ApplyLiveZoom writes with it are composed in a TransformGroup with the
// scale FIRST, so the translation stays in final screen DIPs instead of being scaled by the preview.
// Both elements are cleared and the group emptied before anything is (re)attached: a XAML Transform
// may only have one parent, and re-appending one that still has its old parent throws.
//
// Apotheosis (review 2026-09-04 item 1): the view inset (m_gpuInset, set by ApplyViewInsets) is a
// third, permanent member of the stack and belongs to GpuPanel ONLY — the software path is inset by
// layout, on ContentBorder. It goes LAST: preview scale and translation are expressed in the
// panel's own space and must be shifted by the inset, not the other way round. It also stays on the
// panel while the software path presents, so the panel never jumps when the two swap over.
void MainPage::ApplyPresentTransform()
{
    // ApplySettings() can reach this before the XAML tree exists (settings.ini is read in the
    // constructor), so both elements are checked like NavigateTo does.
    if (!GpuPanel || !RenderImage)
        return;
    const bool haveZoom = (m_zoomTransform != nullptr);
    const bool haveTranslate = (m_panTranslate != nullptr) && (m_panTranslate->X != 0.0 || m_panTranslate->Y != 0.0);
    const bool haveInset = (m_gpuInset != nullptr) && (m_gpuInset->Y != 0.0);
    GpuPanel->RenderTransform = nullptr;
    RenderImage->RenderTransform = nullptr;
    if (m_presentGroup != nullptr) m_presentGroup->Children->Clear();

    Windows::UI::Xaml::Media::Transform^ parts[3];
    int n = 0;
    if (haveZoom) parts[n++] = m_zoomTransform;
    if (haveTranslate) parts[n++] = m_panTranslate;
    if (m_gpuPresent && haveInset) parts[n++] = m_gpuInset;
    // Software present: the preview goes on RenderImage, the panel keeps the bare inset.
    if (!m_gpuPresent && haveInset) GpuPanel->RenderTransform = m_gpuInset;
    if (n == 0)
        return;
    Windows::UI::Xaml::Media::Transform^ t;
    if (n == 1)
        t = parts[0];
    else {
        if (m_presentGroup == nullptr) m_presentGroup = ref new Windows::UI::Xaml::Media::TransformGroup();
        for (int i = 0; i < n; ++i) m_presentGroup->Children->Append(parts[i]);
        t = m_presentGroup;
    }
    if (m_gpuPresent) GpuPanel->RenderTransform = t;
    else              RenderImage->RenderTransform = t;
}

// Apotheosis: event-driven present.
//   The engine calls PresentWakeThunk whenever something wants to be presented. It may run on the
//   ENGINE thread (every WebCore invalidation) or on a RASTER WORKER (a tile replay landing), so
//   all it is allowed to do is post: CoreDispatcher is agile, RunAsync is fire-and-forget, and the
//   whole live-loop state (m_liveBusy, the timers, the rate limit) lives on the UI thread where
//   OnPresentWake then runs. No engine call, no wait — 线程铁律 intact in both directions.
//   Apotheosis (review 2026-09-04 item 5): both globals are written EXACTLY ONCE, on the UI
//   thread, before the callback is first registered on the engine one - see the one-shot guard in
//   ApplyEventPresentSetting(). They used to be reassigned on every Settings-page close, i.e.
//   while PresentWakeThunk was reading them from the engine thread and from raster workers;
//   Platform::Agile assignment is a refcount swap, not an atomic store, so that was a genuine
//   data race on a pointer a worker thread was about to dereference.
static Platform::Agile<Windows::UI::Core::CoreDispatcher^> g_wakeDispatcher;
static Platform::Agile<Harness::MainPage^> g_wakePage;

static void PresentWakeThunk(void*)
{
    Windows::UI::Core::CoreDispatcher^ disp = g_wakeDispatcher.Get();
    if (!disp) return;
    Platform::Agile<Harness::MainPage^> self = g_wakePage;
    try {
        disp->RunAsync(CoreDispatcherPriority::Low, ref new DispatchedHandler([self]() {
            Harness::MainPage^ s = self.Get();
            if (s) s->OnPresentWake();
        }));
    } catch (...) {}
}

// Register the driver's present wake-up: the engine tells us when something wants to be presented
//   and the live loop runs on those wakes plus a fallback tick, instead of a fixed 200 ms timer.
//   UI thread; the registration itself is posted to the engine thread as the ABI demands.
void MainPage::ApplyEventPresentSetting()
{
    // Apotheosis (review 2026-09-04 item 5): bind the wake context once and never touch it again.
    //   Both values are constant for the life of the page (the window's CoreDispatcher and `this`),
    //   so there is nothing to update on a later call - and the first write still happens-before
    //   the registration below, which is posted to the engine thread. Toggling the setting off and
    //   on again therefore only ever (un)registers the callback; the readers keep a stable target.
    static bool s_wakeBound = false;   // UI thread only
    if (!s_wakeBound) {
        g_wakeDispatcher = this->Dispatcher;
        g_wakePage = this;
        s_wakeBound = true;
    }
    WebEngine::instance().post([]() {
        try { WebCoreSetPresentRequestCallback(&PresentWakeThunk, nullptr); } catch (...) {}
    });
    // Re-arm the loop; if no session is live it is a no-op and the next StartLiveMode does it.
    if (m_sessionActive) { StopLiveMode(); StartLiveMode(); }
}

// 自由滚动:内容区 ManipulationDelta(去掉 ScrollViewer 后,触摸不再被吞)。单指拖拽的累计 ΔY → 引擎滚动。
// TranslateInertia 让松手后继续惯性滚(ManipulationDelta 在惯性期持续触发)。点击经 Tapped 走(手势识别器
// 区分点按 vs 拖拽,小位移=Tapped、越阈值=Manipulation,不会冲突)。
void MainPage::OnImageManipDelta(Platform::Object^, Windows::UI::Xaml::Input::ManipulationDeltaRoutedEventArgs^ e)
{
    // Apotheosis (link context menu, 0.1.9.42): the finger is moving, so whatever this gesture
    // started as, it is a pan or a pinch now - a link menu must not appear on top of it. XAML's own
    // HoldingState::Canceled says the same thing (OnPageHolding), but it is not guaranteed to have
    // been raised by the time the first delta arrives, and this is the cheaper of the two gates.
    if (m_ctxPending) CancelPendingLinkMenu("manip");
    if (!m_sessionActive) return;
    // M4 捏合缩放:本次增量 Scale≠1(或已进入捏合)→ 捏合模式:只对显示层做实时 ScaleTransform(零引擎调用,
    //   丝滑),不走引擎滚动;松手(OnImageManipCompleted)再把累计缩放提交给引擎按新尺度重栅格。
    float ds = e->Delta.Scale;
    // Apotheosis (pinch on map widgets, 2026-09-06): m_pinchPage keeps this gesture on the pinch
    // branch even though m_pinching is never set for it - the per-delta scale of a slow pinch drops
    // back inside the dead band all the time, and falling through to the pan path there would scroll
    // the document under a two-finger gesture.
    if (m_pinching || m_pinchPage || (ds > 0.0f && (ds > 1.002f || ds < 0.998f))) {
        // Apotheosis: the anchor is taken ONCE, at the first pinch delta, and then frozen for the
        //   whole gesture. It used to follow e->Position on every delta, and the centroid of a
        //   two-finger manipulation becomes the position of the remaining finger the moment the
        //   first one leaves the glass — re-centring the transform there shifts the content by
        //   (Fnew − Fold)·(1 − live), which is exactly the "it snaps to the other finger" jump on
        //   release (worst when zooming in, where 1 − live is largest).
        if (!m_pinching && !m_pinchPage) {
            // Apotheosis (pinch on map widgets, 2026-09-06): read the gesture-start probe BEFORE the
            // reset below overwrites it. NestedScrollState::Drag means WebCoreWantsDragAt said the
            // touch-down point belongs to something that drags itself (canvas / touch-action:none),
            // i.e. the same test the one-finger drag route uses.
            const bool overDragWidget = (m_nestedScrollState == NestedScrollState::Drag);
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
            // Apotheosis (2837ce0 review item 2): this gesture began as a pan, so
            //   OnImageManipStarted posted a RequestScrollState() for it. Its answer is engine px
            //   at the CURRENT scale and will land while (or after) we zoom — bump the generation
            //   so it is dropped instead of re-validating the cache with pre-zoom bounds, which is
            //   what the live-pinch clamp would then translate the view by.
            ++m_scrollStateGen;
            m_scrollStateValid = false;
            // Apotheosis (axis lock / rail scrolling): a second contact turned this into a pinch —
            // drop any rail lock so a pan that resumes after the pinch (same gesture, one finger
            // lifted) is not still constrained to whatever axis the pre-pinch pan picked.
            m_axisLockState = AxisLock::Free;
            // Apotheosis (pinch on map widgets, 2026-09-06): route the gesture ONCE. To the page
            // only when it started on a drag widget AND the page is at 1:1 - a page the user has
            // already zoomed must keep pinching as page zoom, or a pinch-out on a zoomed map would
            // silently change what the zoom gesture means halfway through a session.
            if (overDragWidget && m_pageScale <= 1.001f) {
                int cx, cy; MapTapToEngine(e->Position.X, e->Position.Y, cx, cy);
                if (cx < 0) cx = 0; else if (cx >= kW) cx = kW - 1;
                if (cy < 0) cy = 0; else if (cy >= kH) cy = kH - 1;
                m_pinchPage = true;
                m_pinchPageAccum = 1.0f;
                m_pinchPagePx = cx; m_pinchPagePy = cy;
            } else {
                m_pinching = true;
                SetPinchAnchor(e->Position.X, e->Position.Y);
            }
        }
        // Apotheosis (pinch on map widgets, 2026-09-06): the page owns this pinch - turn the scale
        // change into wheel notches and leave every zoom transform alone. The accumulator keeps the
        // remainder, so a slow pinch still reaches the map one notch at a time.
        if (m_pinchPage) {
            if (ds > 0.0f) m_pinchPageAccum *= ds;
            int notches = 0;
            while (m_pinchPageAccum >= kPinchNotchScale && notches < 8) {
                m_pinchPageAccum /= kPinchNotchScale; ++notches;
            }
            while (m_pinchPageAccum <= 1.0f / kPinchNotchScale && notches > -8) {
                m_pinchPageAccum *= kPinchNotchScale; --notches;
            }
            if (notches) {
                int cx, cy; MapTapToEngine(e->Position.X, e->Position.Y, cx, cy);
                if (cx < 0) cx = 0; else if (cx >= kW) cx = kW - 1;
                if (cy < 0) cy = 0; else if (cy >= kH) cy = kH - 1;
                ZoomWheelBy(cx, cy, notches);
            }
            return;
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
    // Apotheosis (axis lock / rail scrolling): feed the raw (pre-scale) DIP translation, not idx/idy
    // — the threshold/ratio are meant in screen space, not stretched by the kW/ActualWidth factors.
    UpdateAxisLock(e->Delta.Translation.X, e->Delta.Translation.Y);
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
    // Deliberately no WebCoreScrollBy on this path: the document must not move.
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
        ApplyAxisLock(idx, idy);   // rail lock covers nested scrollables (same WebCoreWheelAt delta)
        NestedScrollBy(px, py, idx, idy);
        return;
    }
    // No:现有主帧快路径不变(coalesced WebCoreScrollBy,引擎自己合成+交换)。
    ApplyAxisLock(idx, idy);   // Apotheosis (axis lock / rail scrolling)
    // Apotheosis (touch-lag diagnostic): the same idx/idy FreeScrollBy is about to see,
    //   summed raw (pre-axis-lock would double count the locked-out axis as "finger asked, engine
    //   never told" — using the post-lock values keeps this the same "what did we actually ask the
    //   engine for" question FreeScrollBy is answering). No allocation, two adds and a compare.
    // Apotheosis (2026-09-07, device bug fix — same "n=28 max=2045"/"n=187 max=2709" report): the
    //   comment in OnImageManipCompleted already notes it — "OnImageManipDelta keeps firing translation
    //   deltas during TranslateInertia" — a synthetic deceleration curve the platform plays out after
    //   the finger has already left the glass, not the finger itself. Summing those into the finger
    //   total answered a different question than "does the page keep up with the finger": once inertia
    //   starts, freeze the comparison here (m_scrollLagActive off) rather than let the engine's own
    //   continued catch-up — now with nothing on the finger side to match it — read as ever-growing
    //   lag in the other direction. m_scrollLagStarted (set once in OnImageManipStarted) is untouched,
    //   so OnImageManipCompleted still writes the line for the tracked part of the gesture; fling=
    //   counts what followed instead of folding it into n=/max=/avg=.
    if (e->IsInertial) {
        if (m_scrollLagStarted) ++m_scrollLagFlingMoves;
        m_scrollLagActive = false;
    } else if (m_scrollLagActive) {
        m_scrollLagFingerX += idx; m_scrollLagFingerY += idy;
        ++m_scrollLagMoves;
        if (m_scrollLagPendingSinceMs == 0) m_scrollLagPendingSinceMs = GetTickCount64();
    }
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
    // TransformToVisual would fold in a PREVIEW transform still sitting on the layer, so drop it
    //   first. A fresh transform per gesture also keeps a finished animation from holding its
    //   value on the old one (FillBehavior=HoldEnd would swallow later writes to ScaleX).
    //   Apotheosis (review 2026-09-04 item 1): the view inset (m_gpuInset) deliberately stays —
    //   it is a genuine visual offset of the panel, and TransformToVisual folding it in is exactly
    //   right: the target space is GpuPanel's own pre-RenderTransform space, the same space the
    //   ScaleTransform centre below lives in. ApplyPresentTransform() re-attaches just the inset.
    m_zoomTransform = nullptr;
    if (m_panTranslate != nullptr) { m_panTranslate->X = 0.0; m_panTranslate->Y = 0.0; }
    ApplyPresentTransform();
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

// Apotheosis (0.1.9.40): same job as SetPinchAnchor above (stop any running spring, drop the
// preview transform, fill m_focalX/Y + m_focalPx/Py), but starting from an anchor already known in
// ENGINE VIEWPORT PX — WebCoreTapPolicyAt's outAnchorX/Y — instead of a ContentArea DIP position.
// Only RunDoubleTapZoom uses this: SetPinchAnchor's ContentArea->layer TransformToVisual step is
// for turning a DIP gesture position (from XAML's e->Position) into layer space; an engine-px
// anchor is already IN that derived space (see SetPinchAnchor's own derivation comment above), one
// division away — going back out to a DIP and letting SetPinchAnchor convert it a second time would
// add nothing but a rounding mismatch between the two paths.
void MainPage::SetPinchAnchorEnginePx(int anchorPx, int anchorPy)
{
    auto layer = PresentLayer();
    if (m_zoomSpring != nullptr) {
        try { m_zoomSpring->Stop(); } catch (...) {}   // Stop() does not raise Completed
        m_zoomSpring = nullptr;
        m_liveScale = m_springTargetLive;
    }
    m_zoomTransform = nullptr;
    if (m_panTranslate != nullptr) { m_panTranslate->X = 0.0; m_panTranslate->Y = 0.0; }
    ApplyPresentTransform();
    double lw = (layer != nullptr) ? layer->ActualWidth : 0.0;
    double lh = (layer != nullptr) ? layer->ActualHeight : 0.0;
    if (!(lw > 1.0)) lw = ContentArea->ActualWidth;
    if (!(lh > 1.0)) lh = ContentArea->ActualHeight;
    // Inverse of SetPinchAnchor's px = lx*kW/lw: lx = px*lw/kW — the same presenting-layer DIP
    // space the ScaleTransform centre (m_focalX/Y) lives in.
    m_focalX = (kW > 0) ? (double)anchorPx * lw / (double)kW : (double)anchorPx;
    m_focalY = (kH > 0) ? (double)anchorPy * lh / (double)kH : (double)anchorPy;
    int px = anchorPx, py = anchorPy;
    if (px < 0) px = 0; else if (px > kW) px = kW;
    if (py < 0) py = 0; else if (py > kH) py = kH;
    m_focalPx = px;
    m_focalPy = py;
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
    //   m_panTranslate is the presenting layer's translation: ApplyPresentTransform composes it
    //   after the scale ("145b29f", scale first so the translation stays screen-space), and
    //   PinchCommit()/SpringBackZoom zero it back out on release.
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

// Apotheosis (review 2026-09-04 item 3): THE ONE EXIT FROM A GESTURE.
//   Five paths used to end a gesture and each cleared a different subset of the state. The one
//   that mattered most - m_pinching - was cleared only by OnImageManipCompleted and NavigateTo, so
//   a manipulation ended by a visibility change, a tab switch or a session teardown left it set,
//   and ApplyLiveZoom kept owning the presenting layer's transform for the rest of the session.
//   Everything that says "a finger owns the page" is reset here instead.
//
//   NOT hooked, deliberately: PointerCaptureLost / PointerCanceled. They fire when the finger
//   leaves the glass, which is exactly where INERTIA begins - ending the gesture there would kill
//   inertia scrolling. ManipulationCompleted is the event that arrives once inertia has run out
//   (ManipulationMode carries TranslateInertia, see MainPage.xaml), and it is the normal exit.
//   XAML raises no ManipulationCanceled for this element, so the abort reasons below are the
//   lifecycle edges that can swallow a manipulation: background, navigation, tab switch, suspend.
//
//   The zoom spring is stopped for every reason EXCEPT Completed: on Completed the caller
//   (OnImageManipCompleted) still owns the pinch commit and is about to start the spring itself.
void MainPage::EndGesture(GestureEnd reason)
{
    const bool abort = (reason != GestureEnd::Completed);
    // Apotheosis (double-tap route, 2026-09-10): "did a finger actually manipulate the page", read
    //   BEFORE anything below clears it — the double-tap hold is cancelled on Completed only for a
    //   real gesture, see the block further down. m_manipActive is set in OnImageManipStarted, which
    //   a stationary tap never reaches (a gesture that never moves never starts a manipulation).
    const bool hadRealGesture = m_manipActive || m_pinching || m_pinchPage
        || m_dragActive || m_dragPressPending || m_dragBusy;
    // --- pinch. Cleared for every reason; this is the flag whose absence froze scrolling.
    m_pinching = false;
    m_liveScale = 1.0f;
    // Apotheosis (pinch on map widgets, 2026-09-06): the ctrl+wheel route ends here too, for every
    // reason including Completed - there is nothing to commit, the page zoomed itself notch by
    // notch while the fingers moved. A post still in flight is harmless: its completion clears
    // m_zoomWheelBusy and, seeing m_pinchPage gone, settles the page instead of pumping more.
    // Apotheosis (review 2026-09-07 M5): the settle for THIS route lives entirely in PumpZoomWheel's
    // completion (`else if (!s->m_pinchPage) { SyncLinksAfterScroll(); StartLiveMode(); }`) —
    // OnImageManipCompleted's `if (!wasPinching) return;` skips it whenever the gesture never became
    // a real pinch. If the last PumpZoomWheel completion lands while the fingers are still down and
    // produces no further notch, EndGesture clears m_pinchPage here with nothing left in flight to
    // run that settle — link table goes stale and, on a pinch held past the idle cut-off, the live
    // loop retires with nothing left to restart it. Capture the flag before clearing it and, once a
    // post is no longer in flight, run the same settle here for every non-abort exit (Completed is
    // the only reason this route ends outside of an abort).
    const bool wasPinchPage = m_pinchPage;
    m_pinchPage = false;
    m_pinchPageAccum = 1.0f;
    m_zoomWheelNotches = 0;
    if (abort && m_zoomSpring != nullptr) {
        try { m_zoomSpring->Stop(); } catch (...) {}   // Stop() does not raise Completed
        m_zoomSpring = nullptr;
        m_zoomTransform = nullptr;
    }
    // --- nested scroll: bump the generation so a hit-test answer still in flight is dropped, and
    //     drop the deltas buffered while it was Unknown. On Completed the caller has already
    //     flushed those through ReplayPendingPan; an abort has nothing left to send them to.
    if (abort) { m_pendingPanX = 0; m_pendingPanY = 0; }
    ++m_nestedScrollGen;
    m_nestedScrollState = NestedScrollState::Unknown;
    // --- drag as pointer events. NOT on Completed: the mouseup for a drag that the page owns is
    //     posted asynchronously (m_dragReleasePending + PumpDrag) and DragReset() would swallow it,
    //     while bumping m_dragGen would make the in-flight answer be dropped and leave m_dragBusy
    //     set for ever. The next ManipulationStarted resets it, as it always did. An abort has no
    //     such completion to wait for.
    if (abort) {
        ++m_dragGen;
        DragReset();
    }
    // --- double-tap zoom (Apotheosis, 2026-09-09): a click OnPageTapped is holding, waiting for a
    //     possible second tap, must not fire into whatever this reason is turning the page into
    //     (navigation, session teardown, tab switch, background) — cancel the hold and bump the
    //     generation so a WebCoreTapPolicyAt answer still in flight is dropped too.
    //     Apotheosis (double-tap route, 2026-09-10): NOT unconditional any more. A tap on an element
    //     with ManipulationMode set raises ManipulationCompleted as well (a zero-delta manipulation),
    //     and if XAML delivers that before Tapped — the order is not contractual, and this route must
    //     not depend on it — then an unconditional cancel here would wipe the tap the FIRST tap is
    //     holding right as the SECOND one arrives, and no pair could ever be recognised. Cancel for
    //     every abort reason, and on Completed only when a real gesture (pan/pinch/drag) is ending:
    //     that is the case the original reasoning was about, and a bare tap is not it.
    if (abort || hadRealGesture) {
        if (m_dtapHoldTimer) { try { m_dtapHoldTimer->Stop(); } catch (...) {} }
        if (m_dtapPending || m_dtapSecondSeen)
            DtapTrace("cancel", std::string("reason=") + std::to_string(static_cast<int>(reason)));
        DtapReset();
    }
    // --- and the master flag: no finger owns the page any more.
    m_manipActive = false;
    if (abort)
        ApplyPresentTransform();   // whatever preview transform is left goes with the gesture
    // Apotheosis (review 2026-09-07 M5): the pinch-to-map route's own settle (see the comment above)
    // never runs when this exit is the one that ends the gesture. A post still in flight will run it
    // itself on completion (and will find m_pinchPage already false, so it will not double it).
    if (wasPinchPage && !abort && !m_zoomWheelBusy) {
        SyncLinksAfterScroll();
        StartLiveMode();
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
    // Apotheosis (touch-lag diagnostic): one line per free-scroll gesture, "did the page keep up
    //   with the finger". n=0 means the gesture never took the free-scroll branch at all (drag/
    //   nested-scroll/pinch, or moved less than half a device px) — expected, not a bug. A
    //   WebCoreScrollBy still in flight completes asynchronously, after this line is written, so it
    //   is not counted here (see the commit message).
    // Apotheosis (2026-09-07, clamp/fling fix): gated on m_scrollLagStarted, not m_scrollLagActive —
    //   the latter now freezes as soon as inertia begins (OnImageManipDelta), which is true of nearly
    //   every swipe, and gating the write on it too would have silently dropped the line for all of
    //   them. m_scrollLagActive itself may already be false here; still clear it (a gesture that ends
    //   without ever going inertial leaves it set) so a stale true cannot survive into the next.
    if (m_scrollLagStarted) {
        m_scrollLagActive = false;
        m_scrollLagStarted = false;
        if (m_scrollLagMoves > 0) {
            const double avg = m_scrollLagSum / m_scrollLagMoves;
            // Apotheosis (2026-09-07): ps= is the page scale this gesture was measured at (always
            //   present, not just when zoomed — a reader should never have to assume 1:1) so a
            //   zoomed gesture's numbers are recognisable at a glance instead of looking like a
            //   regression against the "0-60 px at 1:1" sanity baseline.
            // Apotheosis (2026-09-07, clamp/fling fix): clamped= is how many round trips this gesture
            //   hit a document-edge/nested-scroll clamp (folded out of max/avg/last, not left in them
            //   — see NoteScrollLagPresented); fling= is how many ManipulationDelta events arrived
            //   during TranslateInertia after tracking froze (not folded into n=/max=/avg=/last=
            //   either — see OnImageManipDelta). A sane 1:1 gesture should read clamped=0.
            WriteStage(("scrolllag n=" + std::to_string(m_scrollLagMoves)
                + " max=" + Dip(m_scrollLagMax) + " avg=" + Dip(avg) + " last=" + Dip(m_scrollLagLast)
                + " ms_max=" + std::to_string(m_scrollLagMsMax)
                + " ps=" + Dip(m_scrollLagScale)
                + " clamped=" + std::to_string(m_scrollLagClamped)
                + " fling=" + std::to_string(m_scrollLagFlingMoves)).c_str());
        }
    }
    // Apotheosis (review 2026-09-04 item 3): one exit, shared with every abort path. Capture what
    //   the pinch commit below needs BEFORE the reset - EndGesture() clears m_pinching/m_liveScale
    //   like every other caller, and leaves the zoom spring alone because this path owns it.
    const bool wasPinching = m_pinching;
    const float live = m_liveScale;
    EndGesture(GestureEnd::Completed);
    if (!wasPinching) return;
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
        auto rgba = std::make_shared<std::vector<uint8_t>>(EngineBufferBytes(), 0);
        int rc = -999;
        try { rc = WebCoreSetPageScale(newScale, focalX, focalY, rgba->data()); } catch (...) { rc = -1000; }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rgba, rcCopy, newScale, mySeq, present]() {
                MainPage^ s = self.Get(); if (!s) return;
                // Apotheosis (2837ce0 review item 2) — "the view jumps somewhere else a few seconds
                //   after a pinch". Everything below used to sit BEHIND the m_opSeq guard, i.e. a
                //   commit whose frame was superseded left all of it standing. But the engine has
                //   applied the new scale by the time we get here regardless of who owns the frame,
                //   and from that moment every number expressed in pre-zoom engine px is a lie:
                //     · the scroll/bounds cache (m_scrollX/Y, m_contentW/H, m_viewW/H) is engine px
                //       and content size scales with m_pageScale, so the next live-pinch clamp
                //       (ApplyLiveZoom/ClampZoomAxis) clamps to a document that no longer exists and
                //       manufactures a translation out of a zero offset;
                //     · a RequestScrollState() answer still in flight would re-validate that cache
                //       (m_scrollStateGen was never bumped here — only RequestScrollState bumps it).
                //   So the reset runs unconditionally now, and the generation is bumped with it.
                const bool hadCache = s->m_scrollStateValid;
                // The rubber-band offset ClampZoomAxis left on the presenting layer: the committed
                //   frame lands with no translate at all.
                if (s->m_panTranslate != nullptr) { s->m_panTranslate->X = 0.0; s->m_panTranslate->Y = 0.0; }
                s->m_scrollStateValid = false;
                ++s->m_scrollStateGen;
                // Apotheosis (review 2026-09-04 item 6): record the committed scale HERE, with the
                //   rest of the unconditional reset, not behind the m_opSeq guard below. The engine
                //   has applied the new scale by the time we get here regardless of who owns the
                //   frame - the same argument the block above makes for the scroll cache. Behind
                //   the guard, a superseded commit left m_pageScale describing
                //   the pre-pinch document, and every consumer of it worked in the wrong units:
                //   ScrollStateUsable() compared the wrong stamp (and so trusted a stale cache),
                //   MapTapToEngine mapped taps at the wrong scale, and the next pinch took its base
                //   from it. Only the frame itself is dropped when superseded.
                if (rcCopy == 0)
                    s->m_pageScale = newScale;
                // (m_scrollStateScale is stamped where the cache is VALIDATED, not here — the next
                //  RequestScrollState answer/engine frame re-seeds it against the committed scale.)
                // The insets deferred while the gesture ran (ApplyViewInsets' pinch guard) — e.g.
                //   the title row's auto-hide landing mid-pinch — are safe to apply now.
                if (s->m_insetsPending) { s->m_insetsPending = false; s->ApplyViewInsets(); }
                if (hadCache)
                    WriteMemLog("pinch-commit scale=" + std::to_string(newScale)
                                + " rc=" + std::to_string(rcCopy)
                                + " current=" + std::to_string(s->m_opSeq == mySeq ? 1 : 0)
                                + " dropped pre-zoom scroll cache");
                if (s->m_opSeq != mySeq) return;            // 被新操作取代,丢弃迟到帧
                if (rcCopy == 0)
                    s->PresentSoftwareFrame(rgba);   // present 模式:引擎已 swapBuffers 到 GpuPanel,内部自跳过
                // 复位实时变换(新帧已是按新尺度渲染的清晰图;变换归一,避免叠加二次缩放)。
                s->m_zoomTransform = nullptr;
                // Apotheosis (review 2026-09-04 item 1): drop the preview through the one place
                //   that knows about the view inset, so the panel keeps its translation.
                s->ApplyPresentTransform();
                s->StartLiveMode();
            }));
        } catch (...) {}
    });
}

// Apotheosis (double-tap zoom, 2026-09-09): commit a double-tap zoom through the SAME anchor/
// animate/commit path a pinch release uses — there is only one zoom path in this harness, pinch and
// double-tap both end up on SetPinchAnchor(Px) -> ApplyLiveZoom (seeds the animation's start frame)
// -> SpringBackZoom (eases to the target, then calls PinchCommit). targetScale is the absolute page
// scale WebCoreTapPolicyAt asked for (already clamped to [1.0, 3.0], a subrange of
// SnapAndClampPageScale's own [0.5, 6.0]).
// Apotheosis (0.1.9.40): anchorPx/anchorPy are ENGINE VIEWPORT PX — WebCoreTapPolicyAt's own
// outAnchorX/Y, the same convention WebCoreSetPageScale's focalX/focalY use — not a ContentArea DIP
// position any more. A double tap on a narrow column previously always anchored on the raw tap
// point (a ContentArea DIP converted through SetPinchAnchor), which on a column not centred under
// the finger put most of the column off-screen after the zoom — read on the device as "jumps to
// the middle of the page". The driver now centres the anchor on the column when it zoomed to one
// (see WebCoreTapPolicyAt's column loop); SetPinchAnchorEnginePx consumes that directly instead of
// re-deriving a DIP position from it only to have SetPinchAnchor convert it straight back.
void MainPage::RunDoubleTapZoom(int anchorPx, int anchorPy, float targetScale)
{
    if (!m_sessionActive || m_loading || m_interacting) return;
    SetPinchAnchorEnginePx(anchorPx, anchorPy);
    m_liveScale = 1.0f;
    ApplyLiveZoom();   // seeds the preview at scale 1 around this anchor before the spring animates it
    float target = ClampPageScale(targetScale);   // no ±33 % 1:1 snap here, see ClampPageScale
    float targetLive = (m_pageScale > 0.0f) ? target / m_pageScale : 1.0f;
    if (!(targetLive > 0.0f)) targetLive = 1.0f;
    SpringBackZoom(targetLive, target);   // eases the preview to targetLive, then PinchCommit(target)
}

// Apotheosis (double-tap zoom, 2026-09-09): m_dtapHoldTimer's one-shot Tick — no second tap arrived
// within the hold interval, so the tap OnPageTapped held really was just one tap. WebCoreTapPolicyAt
// having said this point is zoomable does not mean a SINGLE tap here should zoom or be suppressed —
// a real mobile browser dispatches the click either way and only decides late whether a second tap
// also arrived to zoom.
// Apotheosis (double-tap route, 2026-09-10): the tick is also the deadline for a pair whose policy
// answer never arrived (m_dtapSecondSeen) — that tap gets its click too, just without the zoom.
void MainPage::OnDtapHoldTimer(Platform::Object^, Platform::Object^)
{
    if (m_dtapHoldTimer) { try { m_dtapHoldTimer->Stop(); } catch (...) {} }   // one-shot
    const bool second = m_dtapSecondSeen;
    if (!m_dtapPending && !second) {
        DtapTrace("timer", "act=drop:idle");
        return;
    }
    const int px = m_dtapPx, py = m_dtapPy;
    const bool blocked = (!m_sessionActive || m_loading || m_interacting);   // torn down/busy while held
    DtapTrace("timer", std::string("at=") + std::to_string(px) + "," + std::to_string(py)
        + " second=" + (second ? "1" : "0")
        + " ready=" + (m_dtapPolicyReady ? "1" : "0")
        + " act=" + (blocked ? "drop:busy" : (second ? "dblclick" : "click")));
    DtapReset();
    if (blocked)
        return;
    ForwardClickToEngine(px, py, /*longPress*/ false, /*clickCount*/ second ? 2 : 1);
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
    ApplyKeyboardShift("overlay");   // 同理:LostFocus 不触发 → 这里得自己把底栏放回原位
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

// ---- 实时渲染循环:唤醒驱动推进动画/SPA 渐进挂载 ----
// Apotheosis (event-driven present): the loop used to be a fixed 200 ms DispatcherTimer
//   that composited whether or not anything had changed. It runs on engine wake-ups
//   (WebCoreSetPresentRequestCallback -> PresentWakeThunk -> OnPresentWake), rate-limited to one
//   present per ~16 ms, with a fallback tick as the safety net for anything not signalled.
void MainPage::StartLiveMode()
{
    if (!m_sessionActive || !m_appForeground) return;
    if (Drawer->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    if (ActionMenu->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    if (SettingsPage->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    if (TabSwitcher->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    m_liveBusy = false;        // 重置(若上次 RunAsync 抛/后台早退卡住,这里恢复)
    m_liveBusyAge = 0;
    m_liveTotalTicks = 0;
    // 事件驱动:没有固定帧率。兜底 tick 负责自愈 + 内存采样 + 补没被信号覆盖的变化,
    // 刚有活动时按 200ms(给 setTimeout 动画/在途图片留出 RunLoop::cycle),
    // 画面静下来后 1s,再静下来 5s(见 OnFallbackTick)。
    m_fallbackStaticTicks = 0;
    if (!m_fallbackTimer) {
        m_fallbackTimer = ref new Windows::UI::Xaml::DispatcherTimer();
        m_fallbackTimer->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(this, &MainPage::OnFallbackTick);
    }
    Windows::Foundation::TimeSpan fb; fb.Duration = 2000000LL;   // 200ms
    m_fallbackTimer->Interval = fb;
    m_fallbackTimer->Start();
    // 交互/导航之后立刻出一帧,限流仍然生效。
    m_wakePending = true;
    ScheduleWakeComposite();
}
void MainPage::StopLiveMode()
{
    if (m_fallbackTimer) m_fallbackTimer->Stop();
    if (m_wakeTimer) m_wakeTimer->Stop();
    m_wakePending = false;
}

// 引擎说"有东西要呈现"(UI 线程,PresentWakeThunk 转投而来)。只做限流 + 排帧,绝不碰引擎。
void MainPage::OnPresentWake()
{
    m_fallbackStaticTicks = 0;   // 有活动 → 兜底 tick 回到快节奏(OnFallbackTick 里定档)
    m_wakePending = true;
    ScheduleWakeComposite();
}

// 排一帧:满足最小间隔就立刻投引擎,否则用一次性定时器补齐剩下的时间。UI 线程。
void MainPage::ScheduleWakeComposite()
{
    if (!m_wakePending) return;
    if (m_liveBusy) return;                 // 上一帧还没回;它回来时会重新排
    if (!m_sessionActive || !m_appForeground || m_loading || m_interacting) return;
    if (Drawer->Visibility == Windows::UI::Xaml::Visibility::Visible
        || ActionMenu->Visibility == Windows::UI::Xaml::Visibility::Visible
        || SettingsPage->Visibility == Windows::UI::Xaml::Visibility::Visible
        || TabSwitcher->Visibility == Windows::UI::Xaml::Visibility::Visible) return;
    if (m_wakeTimer && m_wakeTimer->IsEnabled) return;   // 已经排好队了
    // 最小间隔 16ms(≈60Hz)。上一帧比这贵 → 按上一帧的耗时来(占空比 ≤50%,别把这台机器打满:
    // 代码托管站那种"每帧都请求渲染更新"的页面否则会从 5fps 直接变成背靠背合成)。上限 200ms = 旧 tick。
    // 连续动画 150 帧(≈30s)无交互 → 至少 1s 一帧,搬的是旧 tick 的防永久动画降速。
    unsigned minGap = 16;
    if (m_lastPresentDurMs > minGap) minGap = (m_lastPresentDurMs > 200) ? 200 : m_lastPresentDurMs;
    if (m_liveTotalTicks >= 150) minGap = 1000;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG since = (now >= m_lastPresentMs) ? (now - m_lastPresentMs) : 0;
    if (since >= minGap) {
        m_wakePending = false;
        DispatchLiveFrame();
        return;
    }
    if (!m_wakeTimer) {
        m_wakeTimer = ref new Windows::UI::Xaml::DispatcherTimer();
        m_wakeTimer->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(this, &MainPage::OnWakeTimer);
    }
    Windows::Foundation::TimeSpan ts; ts.Duration = (long long)(minGap - since) * 10000LL;
    m_wakeTimer->Interval = ts;
    m_wakeTimer->Start();
}

void MainPage::OnWakeTimer(Platform::Object^, Platform::Object^)
{
    if (m_wakeTimer) m_wakeTimer->Stop();   // 一次性
    ScheduleWakeComposite();
}

// 1s 兜底:没被信号覆盖的变化、丢掉的 RunAsync(自愈)、内存采样都靠它。静止久了降到 5s。
void MainPage::OnFallbackTick(Platform::Object^, Platform::Object^)
{
    if (!m_sessionActive || !m_appForeground) return;
    if (Drawer->Visibility == Windows::UI::Xaml::Visibility::Visible
        || ActionMenu->Visibility == Windows::UI::Xaml::Visibility::Visible
        || SettingsPage->Visibility == Windows::UI::Xaml::Visibility::Visible
        || TabSwitcher->Visibility == Windows::UI::Xaml::Visibility::Visible) { StopLiveMode(); return; }
    if (m_loading || m_interacting) return;
    if (m_liveBusy) {
        // 30 × 1s = 旧 tick 的 30s 自愈(150 × 200ms):真丢了的 RunAsync,不是慢帧。
        if (++m_liveBusyAge < 30) return;
        m_liveBusy = false;
    }
    m_liveBusyAge = 0;
    // 节奏阶梯。唤醒覆盖不到的东西只有这条路:WTF 定时器(setTimeout 动画)、在途图片/子资源的
    // 完成回调 —— 它们都要靠 WebCoreLiveTick 里的 RunLoop::cycle 才被取走。所以刚有变化时按
    // 200ms(和旧 tick 一样),连续 5 帧没变化降到 1s,再 10 帧没变化降到 5s(近乎全静默)。
    const long long want = (m_fallbackStaticTicks < 5) ? 2000000LL
                         : ((m_fallbackStaticTicks < 15) ? 10000000LL : 50000000LL);
    if (m_fallbackTimer && m_fallbackTimer->Interval.Duration != want) {
        Windows::Foundation::TimeSpan iv; iv.Duration = want;
        m_fallbackTimer->Interval = iv;   // 重设会重启计时,正是想要的
    }
    m_wakePending = true;
    ScheduleWakeComposite();
}

// 一帧:引擎线程 WebCoreLiveTick + UI 线程呈现。调用方负责守卫与限流。
void MainPage::DispatchLiveFrame()
{
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
        const ULONGLONG t0 = GetTickCount64();
        try { rc = WebCoreLiveTick(rgba->data()); if (rc == 0) { hash = WebCoreGetFrameHash(); pending = WebCoreGetPendingResourceCount(); } } catch (...) { rc = -1000; }
        const unsigned durMs = (unsigned)(GetTickCount64() - t0);   // 事件驱动的限流按它走
        // Apotheosis: sample here, on the engine thread, not in the UI
        // continuation below — WebCoreGetMemoryStats()/WebCoreSetMemoryPressure() must never be
        // called from the UI thread, and the UI thread must never wait on the engine.
        SampleMemoryPressure();
        if (g_perfLogEnabled && (++g_memTickCount % 50) == 0)
            WriteMemLog("mem-tick n=" + std::to_string(g_memTickCount) + " " + MemSnapshot() + EngineMemStats());
        int rcCopy = rc; unsigned hashCopy = hash; int pendingCopy = pending;
        try {
            disp->RunAsync(CoreDispatcherPriority::Low,
                ref new DispatchedHandler([self, rgba, rcCopy, hashCopy, pendingCopy, mySeq, present, durMs]() {
                    MainPage^ s = self.Get(); if (!s) return;
                    s->m_liveBusy = false;
                    s->m_lastPresentMs = GetTickCount64();
                    s->m_lastPresentDurMs = durMs;
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
                    if (hashCopy == s->m_lastFrameHash) {        // 画面没变:静止不停循环——唤醒随时会来,
                        // 只让兜底 tick 逐步降速。还有图片/子资源在途 → 保持快节奏,它们的完成回调
                        // 要 RunLoop::cycle 取。
                        if (pendingCopy > 0) s->m_fallbackStaticTicks = 0;
                        else ++s->m_fallbackStaticTicks;
                    } else {
                        s->m_fallbackStaticTicks = 0;
                        s->m_lastFrameHash = hashCopy;
                        s->PresentSoftwareFrame(rgba);
                        // 永久动画防失控:连续动画超 ~150 帧无交互 → 降到 ~1fps(不硬停,免得动画卡死):
                        // ScheduleWakeComposite 的 minGap 读 m_liveTotalTicks 实现这条规则。
                        // 任何交互/导航/滚动都经 StartLiveMode 重置计数并恢复快帧率。
                        ++s->m_liveTotalTicks;
                    }
                    // 这一帧跑的时候又来了唤醒(动画页每帧都会) → 按限流排下一帧。
                    if (s->m_wakePending) s->ScheduleWakeComposite();
                }));
        } catch (...) {}
    });
}

// ---- 工具栏事件 ----
// Apotheosis (suggestion tap, 2026-09-10): ONE address-bar commit path — Enter, the go button and a
//   tapped suggestion all end up here, so they cannot drift apart again.
//   Apotheosis (2026-09-07): confirming must also put the on-screen keyboard away — TryHide() alone
//   is not enough while UrlBox still holds focus (same reasoning as DismissKeyboardForOverlay(): any
//   control regaining focus re-evaluates the input pane and can bring it right back). Move focus onto
//   the page's own focus sink first; that runs OnUrlLostFocus synchronously, which already restores
//   the address-bar chrome/blur state and — since NavigateTo() above wrote m_currentUrl/UrlBox->Text
//   together before returning — finds nothing to revert. CloseKeyboard()'s TryHide() then fires the
//   InputPane's Hiding handler, which calls ApplyKeyboardShift("hide") and slides the chrome back.
void MainPage::CommitUrlNavigation(Platform::String^ url)
{
    WriteStage((std::string("suggest commit len=")
                + std::to_string(url ? url->Length() : 0)).c_str());
    NavigateTo(NormalizeUrl(url), true);
    try { this->Focus(Windows::UI::Xaml::FocusState::Programmatic); } catch (...) {}
    CloseKeyboard();
}
void MainPage::OnGo(Platform::Object^, RoutedEventArgs^) { CommitUrlNavigation(UrlBox->Text); }
void MainPage::OnUrlKeyDown(Platform::Object^, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e)
{
    if (e->Key == Windows::System::VirtualKey::Enter)
        CommitUrlNavigation(UrlBox->Text);
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
    // Apotheosis (link context menu, 0.1.9.42): topmost light-dismiss surface, so it goes first.
    if (LinkMenu && LinkMenu->Visibility == V::Visible) { HideLinkMenu("back"); e->Handled = true; return; }
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
        // Apotheosis (2026-09-07): 512 KB, not 64. The driver now writes the texmap per-store
        // diagnostics first and the layer tree into what is left (WebCoreGpuLayerInfo), and a real
        // page's tree alone is well over 64 KB - at the old size layertree.txt was exactly 65535
        // bytes of tree and nothing else. Allocated per dump, on the engine thread's task.
        auto buf = std::make_shared<std::vector<char>>(512 * 1024, 0);
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
    // Apotheosis (suggestion tap, 2026-09-10): the "→" glyph commits typed text, so it takes the
    //   same path as Enter (keyboard away, focus off the field) instead of only navigating.
    if (pendingEdit) CommitUrlNavigation(UrlBox->Text);
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
    ++m_suggestHideToken;   // cancel any deferred collapse still queued from a previous LostFocus
    // Apotheosis (2026-09-04): keep the page title above the address bar while it is being edited -
    //   RevealTitleRow() brings the row back if the grace period already took it away, and the pin
    //   stops the timer from taking it away again while the field has focus.
    m_titleRowPinned = true;
    RevealTitleRow();
    // Apotheosis (bug fix 2026-09-06 evening): the InputPane's Showing can arrive BEFORE this handler
    //   (tapping the field raises the pane and moves focus, and the order is not contractual). The
    //   old code shifted the chrome only from inside Showing, guarded on m_urlFocused, so in that
    //   order nothing moved at all and — because the diagnostic sat inside the same guard — nothing
    //   was logged either, which is why the 0.1.9.18 stage.txt had no keyboard line. The shift is a
    //   pure function of (keyboard geometry, current inset, editing state) now, so simply re-running
    //   it whenever any of the three changes covers every ordering.
    ApplyKeyboardShift("url-focus");
    // Apotheosis (2026-09-07): tapping into the bar should select the whole URL so typing replaces
    //   it outright. A SelectAll() called right here is undone a moment later: the same tap that
    //   raised focus also delivers its PointerReleased to the TextBox at Normal priority, which
    //   places the caret where the finger landed — and that runs AFTER GotFocus, clobbering any
    //   selection made here. Defer to a Low-priority dispatch (same trick as the suggestion-collapse
    //   below) so it runs once the tap has finished moving the caret. The token guards against a
    //   LostFocus/GotFocus pair completing before the deferred item runs; GotFocus only fires once
    //   per focus session in XAML, so a second tap on an already-focused box never re-enters here and
    //   the caret it places is left alone. Programmatic focus (OnUrlClear, the keyboard-restore path)
    //   goes through this same handler and gets identical treatment.
    ++m_urlSelectToken;
    unsigned long long selectToken = m_urlSelectToken;
    Platform::Agile<MainPage^> selectSelf(this);
    this->Dispatcher->RunAsync(CoreDispatcherPriority::Low, ref new DispatchedHandler([selectSelf, selectToken]() {
        MainPage^ s = selectSelf.Get(); if (!s) return;
        if (s->m_urlSelectToken != selectToken || !s->m_urlFocused || !s->UrlBox) return;
        try { s->UrlBox->SelectAll(); } catch (...) {}
    }));
}
// Apotheosis (review 2026-09-04 item 4): the collapse used to happen only from explicit callers
//   (tap the page / open the menu-settings-etc — still true, see the other HideSuggestions() call
//   sites) because doing it synchronously here would fire before a suggestion's own Click: tapping
//   a suggestion button unfocuses UrlBox first (this handler), Click follows after: collapsing the
//   panel here would make that button hit-test-invisible before its own tap lands. Deferring the
//   collapse to a Low-priority dispatch fixes that — Click runs at Normal priority, i.e. as part of
//   the same input sequence, strictly before a queued Low item gets its turn — while still catching
//   every other way the field loses focus (tap outside, hardware Back, an overlay opening). The
//   token lets a focus that comes back before the deferred item runs (OnUrlGotFocus above) cancel
//   it instead of hiding a panel the user is still looking at.
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
    WriteStage((std::string("suggest blur panel=")
                + ((SuggestPanel && SuggestPanel->Visibility == Windows::UI::Xaml::Visibility::Visible) ? "1" : "0")
                + " pressed=" + (m_suggestPressed ? "1" : "0")
                + " shift=" + Dip(m_kbShiftApplied)).c_str());
    // Apotheosis (2026-09-04): editing is over - let the row go back to its usual ~2 s grace period.
    //   The InputPane Hiding handler does the same for a keyboard dismissed without losing focus;
    //   whichever runs last re-arms the timer, and RevealTitleRow() is idempotent.
    m_titleRowPinned = false;
    RevealTitleRow();
    // Apotheosis (bug fix 2026-09-06 evening): the chrome only gives way for the ADDRESS BAR's
    //   keyboard (a page's own form field is scrolled into view by the engine). Focus leaving while
    //   the pane is still up therefore has to put it back — Hiding may never come.
    ApplyKeyboardShift("url-blur");
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
    QueueSuggestionHide();
}

// Apotheosis (suggestion tap, 2026-09-10): the deferred collapse, shared by the blur handler and the
//   pointer-up handler. The Low-priority defer is the old fix for "collapsing synchronously kills
//   the suggestion's own Click"; it was not enough, because with TOUCH the button's Click only comes
//   on release while the blur already happened on press, and a Low-priority item runs during the idle
//   moment in between. m_suggestPressed closes that window: while a finger is on the panel this hide
//   stands down entirely, and the pointer-up handler queues it again.
void MainPage::QueueSuggestionHide()
{
    ++m_suggestHideToken;
    unsigned long long token = m_suggestHideToken;
    Platform::Agile<MainPage^> self(this);
    this->Dispatcher->RunAsync(CoreDispatcherPriority::Low, ref new DispatchedHandler([self, token]() {
        MainPage^ s = self.Get(); if (!s) return;
        if (s->m_suggestHideToken != token) return;   // focus came back / a newer hide supersedes this
        if (s->m_suggestPressed) return;              // a finger is on the dropdown: it stays
        WriteStage("suggest hide act=collapse");
        s->HideSuggestions();
    }));
}

// Apotheosis (suggestion tap, 2026-09-10): the deferred half of ApplyKeyboardShift — see the block
//   there for why moving the dropdown back to rest cannot happen while a finger is on it. Same shape
//   as QueueSuggestionHide: a token so a newer request wins, a stand-down while pressed, and the
//   pointer handlers below as the re-arm. m_kbShiftBypass is the one-shot ticket that lets this call
//   through the deferral test it was born from, so the two can never ping-pong.
void MainPage::QueueKeyboardShiftRestore(const char* why)
{
    ++m_kbShiftToken;
    unsigned long long token = m_kbShiftToken;
    std::string reason = std::string(why ? why : "?") + "-deferred";
    Platform::Agile<MainPage^> self(this);
    this->Dispatcher->RunAsync(CoreDispatcherPriority::Low, ref new DispatchedHandler([self, token, reason]() {
        MainPage^ s = self.Get(); if (!s) return;
        if (s->m_kbShiftToken != token) return;
        if (s->m_suggestPressed) return;   // finger on the dropdown: the pointer handlers re-arm this
        s->m_kbShiftBypass = true;
        s->ApplyKeyboardShift(reason.c_str());
        s->m_kbShiftBypass = false;
    }));
}

void MainPage::OnSuggestPointerDown(Platform::Object^, Windows::UI::Xaml::Input::PointerRoutedEventArgs^)
{
    // NB this runs AFTER Button::OnPointerPressed (class handlers go first, and ours is an instance
    // handler on the panel above it, reached by bubbling), i.e. after the button took focus and after
    // OnUrlLostFocus already ran. It can therefore only stop what that blur QUEUED, never what it did
    // synchronously — which is exactly why the keyboard shift had to become deferrable too.
    m_suggestPressed = true;
    ++m_suggestHideToken;   // whatever the blur queued a moment ago must not fire under this finger
    WriteStage("suggest ptr act=down");
}

void MainPage::OnSuggestPointerUp(Platform::Object^, Windows::UI::Xaml::Input::PointerRoutedEventArgs^)
{
    // The Button raised its Click before this routed event got here (Button::OnPointerReleased is a
    // class handler, ours is an instance handler on the panel above it), so by now the navigation
    // has already happened and hidden the panel itself. Re-arming only matters for a release that hit
    // no button at all — but the keyboard shift has to be re-armed either way, because a deferred
    // restore that stood down under this finger has nobody else left to run it.
    m_suggestPressed = false;
    WriteStage((std::string("suggest ptr act=up urlfocus=") + (m_urlFocused ? "1" : "0")).c_str());
    if (!m_urlFocused)
        QueueSuggestionHide();
    QueueKeyboardShiftRestore("suggest-up");
}

void MainPage::OnSuggestPointerCaptureLost(Platform::Object^, Windows::UI::Xaml::Input::PointerRoutedEventArgs^)
{
    // Capture goes to the dropdown's ScrollViewer when the finger starts panning the list. The
    // finger is still down and no PointerReleased will reach us, so the panel itself stays up until
    // something else closes it (page tap, navigation, hardware Back) — but the chrome underneath must
    // not stay parked at the keyboard offset for that whole time, so the shift is re-armed here too.
    m_suggestPressed = false;
    WriteStage("suggest ptr act=capturelost");
    QueueKeyboardShiftRestore("suggest-capturelost");
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
        // Apotheosis (suggestion tap, 2026-09-10): a tapped suggestion commits exactly like Enter —
        //   same normalisation, same focus move, same keyboard dismissal (CommitUrlNavigation).
        //   It used to call NavigateTo() directly, which left the on-screen keyboard up over the
        //   page it had just loaded and the address bar in its editing chrome.
        btn->Click += ref new RoutedEventHandler([self, u](Platform::Object^, RoutedEventArgs^) {
            MainPage^ s = self.Get(); if (!s) return;
            WriteStage("suggest click act=commit");
            s->m_suggestPressed = false;
            s->HideSuggestions();
            s->m_urlSyncing = true; s->UrlBox->Text = ref new String(u.c_str()); s->m_urlSyncing = false;
            s->CommitUrlNavigation(ref new String(u.c_str()));
        });
        SuggestList->Children->Append(btn);
    }
    // Apotheosis (suggestion tap, 2026-09-10): the query itself is never logged — only how many
    //   matches it produced and how long it was. stage.txt is pulled off the device wholesale.
    WriteStage((std::string("suggest show n=") + std::to_string(matches.size())
                + " qlen=" + std::to_string(query.size())
                + " shift=" + Dip(m_kbShiftApplied)).c_str());
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

// ============================================================================
// Apotheosis (link context menu, 0.1.9.42): long press on a link.
//
// One action for now - "open in new tab". Text selection, copy link and save image are
// deliberately NOT here: each needs engine work this port does not have yet (a selection model
// driven by touch, a clipboard round trip, a decoded image handed back to the harness), and a
// menu that offers them greyed out is worse than a menu that does one thing well.
// ============================================================================

// 菜单头部显示的目标:去掉协议和 www.,再按长度截断。仅供辨认,不可点。
static std::wstring LinkMenuTargetText(const std::wstring& url)
{
    std::wstring s = url;
    size_t p = s.find(L"://");
    if (p != std::wstring::npos) s = s.substr(p + 3);
    if (s.size() > 4 && s.compare(0, 4, L"www.") == 0) s = s.substr(4);
    // A trailing "/" on a bare host is noise; anything deeper keeps its path.
    if (s.size() > 1 && s.back() == L'/' && s.find(L'/') == s.size() - 1) s.pop_back();
    // Apotheosis (0.1.9.44): no more eliding at 76 characters. The header is a ScrollViewer now,
    //   so a target longer than the card is pannable and the whole URL can be read; cutting it
    //   here would be cutting it everywhere. The remaining bound is a layout guard, not a design:
    //   one TextBlock line measured at its full length is what the ScrollViewer's extent costs.
    // Apotheosis (0.1.9.45): 512 was still short of what a real deep link can be (tracking query
    //   strings run into four figures), and a cut header is a header that cannot be checked before
    //   opening it. 2048 characters at FontSize 12 is one NoWrap line about 12 000 px wide inside a
    //   268 px ScrollViewer - a single glyph run, measured once per open, no wrapping and no layout
    //   pass that depends on it (the card's width is declared in the XAML).
    const size_t kMaxChars = 2048;
    if (s.size() > kMaxChars) s = s.substr(0, kMaxChars - 1) + std::wstring(1, L'\x2026');
    return s;
}

void MainPage::ShowLinkMenu(const std::wstring& url)
{
    if (!LinkMenu || !LinkMenuCard || url.empty()) return;
    m_ctxUrl = url;
    if (LinkMenuTarget) LinkMenuTarget->Text = ref new String(LinkMenuTargetText(url).c_str());
    // Written on every open rather than translated in place: the menu is filled from code, so it is
    // not part of the XAML tree kI18n/TranslateTree walk over (see the language switch).
    if (LinkMenuOpenLabel) LinkMenuOpenLabel->Text = L8(L"在新标签页中打开", L"Open in new tab");
    LinkMenu->Visibility = Windows::UI::Xaml::Visibility::Visible;

    // Apotheosis (0.1.9.45): the title row belongs to the card while the card is open. Until now the
    //   hold wrote the page title into TitleText (the caller, one step before this), which revealed
    //   the row through the property-changed callback and armed the ordinary ~2 s auto-hide - so the
    //   row slid away under a card that was still up, which reads as a glitch rather than a label.
    //   Pin it here, drop the pin in HideLinkMenu(), and reveal explicitly: the caller's write only
    //   raises the row when the text actually CHANGED, and re-opening the card on the same page
    //   writes the same title.
    m_titleRowCtxPinned = true;
    RevealTitleRow();

    // Placement: ABOVE the finger by default (that is where the hand is not), flipped below only
    // when the card would not fit up there, then clamped into the window.
    //
    // Apotheosis (0.1.9.43): the card was measured with its PREVIOUS placement still on it, and
    // FrameworkElement::DesiredSize INCLUDES the element's Margin - which is exactly what this
    // code uses to position the card. So every open after the first measured "card size + last
    // open's left/top" (device log: at=69,395 -> the next open reported card=337x483 for a card
    // that is 268x88), the flip-if-it-does-not-fit test then fired on a height of 483 in a 640
    // DIP window whatever the finger did, and the card landed on the clamp at the top of the
    // screen. Reset the margin first and the measurement is the card again.
    // Apotheosis (landscape, 0.1.9.44): RootGrid's ActualWidth/Height are the whole window; the
    //   card lives INSIDE RootGrid's padding, which is where the navigation bar's strip has been
    //   subtracted since this version. So the placement box is the padded box, and the finger
    //   position - taken in RootGrid coordinates when the hold started - has to move into it.
    double availW = RootGrid ? RootGrid->ActualWidth - m_lastInsetLeft - m_lastInsetRight : 0.0;
    double availH = RootGrid ? RootGrid->ActualHeight - m_lastInsetBottom : 0.0;
    if (!(availW > 0.0)) availW = 400.0;
    if (!(availH > 0.0)) availH = 640.0;
    const double fingerX = m_ctxDipX - m_lastInsetLeft;
    const double fingerY = m_ctxDipY;   // RootGrid's top padding is always 0 - see ApplyViewInsets
    double cw = 0.0, ch = 0.0;
    try {
        LinkMenuCard->Margin = Windows::UI::Xaml::Thickness(0, 0, 0, 0);
        LinkMenuCard->Measure(Windows::Foundation::Size((float)availW, (float)availH));
        Windows::Foundation::Size d = LinkMenuCard->DesiredSize;
        cw = d.Width; ch = d.Height;
    } catch (...) {}
    // Fallback only for the case where XAML refuses to measure outside a layout pass. `card=` on
    // the trace line is what actually ran, so a wrong placement can be told apart from a wrong
    // measurement without guessing.
    if (!(cw > 0.0)) cw = 268.0;
    if (!(ch > 0.0)) ch = 96.0;
    // Apotheosis (0.1.9.44): "above the finger" meant "its bottom edge a gap above the fingertip",
    //   which on the device still reads as AT the finger - the hand covers the action row. Lift it
    //   by half that row, so what the eye lands on is the row and not the fingertip. Read the row
    //   back from the measurement that just ran (the button is part of the card), same
    //   ActualHeight-free approach the rest of this function uses, with a literal fallback for the
    //   case where XAML refused to measure.
    double rowH = 0.0;
    try { if (LinkMenuOpenBtn) rowH = LinkMenuOpenBtn->DesiredSize.Height; } catch (...) {}
    if (!(rowH > 0.0)) rowH = 46.0;
    const double kGap = 14.0;    // clearance from the fingertip, so the card is not under it
    const double kEdge = 8.0;
    double left = fingerX - cw / 2.0;
    const char* place = "above";
    double top = fingerY - kGap - ch - rowH / 2.0;
    if (top < kEdge) {                        // no room above the finger - go below it
        place = "below";
        top = fingerY + kGap;
    }
    if (left > availW - cw - kEdge) left = availW - cw - kEdge;
    if (left < kEdge) left = kEdge;
    if (top > availH - ch - kEdge) { top = availH - ch - kEdge; place = "clamp"; }
    if (top < kEdge) { top = kEdge; place = "clamp"; }
    LinkMenuCard->Margin = Windows::UI::Xaml::Thickness(left, top, 0, 0);

    // Diagnostics carry lengths and positions only - never the URL, never page text.
    WriteStage((std::string("ctx open len=") + std::to_string(url.size())
        + " dip=" + std::to_string((int)m_ctxDipX) + "," + std::to_string((int)m_ctxDipY)
        + " at=" + std::to_string((int)left) + "," + std::to_string((int)top)
        + " card=" + std::to_string((int)cw) + "x" + std::to_string((int)ch)
        + " row=" + std::to_string((int)rowH)
        + " place=" + place
        + " avail=" + std::to_string((int)availW) + "x" + std::to_string((int)availH)
        + " inset=" + std::to_string((int)m_lastInsetLeft) + "," + std::to_string((int)m_lastInsetRight)).c_str());
}

void MainPage::HideLinkMenu(const char* why)
{
    if (!LinkMenu) return;
    const bool wasOpen = (LinkMenu->Visibility == Windows::UI::Xaml::Visibility::Visible);
    LinkMenu->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    m_ctxUrl.clear();
    // Apotheosis (0.1.9.45): the row came up with the card, so it goes down with it - on a tap
    //   outside, on Back and on the action alike. Unless something else is holding it up: a load in
    //   flight uses it as the progress readout and address-bar editing pins it, and in both cases
    //   RevealTitleRow() simply hands it back to the ordinary rules (which re-arm the timer only
    //   when neither pin is set). An action that writes a toast into TitleText right after this
    //   raises the row again on its own - Reveal takes the collapse's animation token with it.
    if (m_titleRowCtxPinned) {
        m_titleRowCtxPinned = false;
        if (!m_loading && !m_titleRowPinned) CollapseTitleRow();
        else RevealTitleRow();
    }
    if (wasOpen) WriteStage((std::string("ctx dismiss why=") + (why ? why : "?")).c_str());
}

void MainPage::CancelPendingLinkMenu(const char* why)
{
    if (!m_ctxPending) return;
    m_ctxPending = false;
    WriteStage((std::string("ctx dismiss why=") + (why ? why : "?") + " pending=1").c_str());
}

// Apotheosis (link context menu, 0.1.9.42): "open in new tab" here means a new tab that is NOT
// loaded yet, and that is the tab model working as designed rather than a shortcut.
//
// Tabs on this port are Mode A: exactly ONE tab is a live engine session (the active one); every
// other tab is a record - URL, title, nav stack, page scale, and at most a snapshot of the frame
// it last showed (see the Tab struct and RestoreTab). There is no second Page to load into, and
// deliberately so: one document tree in a 32-bit app container is already the memory wall this
// project keeps hitting, and a second live session would double the worst case. So a background
// tab is a QUEUED tab - the URL is parked, the tab counter goes up, the page in front keeps its
// session, its scroll position and its pixels untouched, and the load happens on the first switch
// to it (RestoreTab -> NavigateTo), exactly as it does for every other non-resident tab here.
// The user-visible promise ("the current page stays in front") is kept; the network fetch simply
// does not start until the tab is looked at.
void MainPage::OpenUrlInBackgroundTab(const std::wstring& url)
{
    if (url.empty()) return;
    Tab t;
    t.currentUrl = url;
    t.navStack.push_back(url);   // 新标签的历史起点:该标签内后退无处可去
    t.navIndex = 0;
    m_tabs.push_back(t);
    UpdateTabCount();
    WriteStage((std::string("ctx action=newtab tabs=") + std::to_string(m_tabs.size())
        + " len=" + std::to_string(url.size())).c_str());
    // The only visible feedback a background tab can give: the tab counter moved, and the status
    // row says why. Writing TitleText reveals that row on its own (property-changed callback).
    TitleText->Text = L8(L"已在新标签页中打开", L"Opened in a new tab");
}

void MainPage::OnLinkMenuScrimTap(Platform::Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    if (e) e->Handled = true;   // 点空白处只关菜单,别落到页面上变成一次点击
    HideLinkMenu("outside");
}

void MainPage::OnLinkMenuCardTap(Platform::Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    if (e) e->Handled = true;   // 点卡片本体不冒泡到遮罩(否则点标题行会误关)
}

void MainPage::OnLinkMenuOpenNewTab(Platform::Object^, RoutedEventArgs^)
{
    std::wstring url = m_ctxUrl;   // HideLinkMenu clears it
    HideLinkMenu("action");
    OpenUrlInBackgroundTab(url);
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
    ApplyEventPresentSetting();              // Apotheosis: registers the engine present wake-up
    ApplyHideNavBarSetting();                // Apotheosis: DISPLAY toggle, UI thread only
    ApplyHideStatusBarSetting();             // Apotheosis: DISPLAY toggle, UI thread only
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
            else if (k == "gpudefault") m_gpuDefault = (atoi(v.c_str()) != 0);
            else if (k == "ua_custom") m_uaCustom = Utf8ToWide(v);
            else if (k == "updatecheck") m_updateAuto = (atoi(v.c_str()) != 0);
            else if (k == "prefetch") m_prefetch = atoi(v.c_str());
            else if (k == "scrollfab") m_showScrollFab = (atoi(v.c_str()) != 0);
            else if (k == "hidenavbar") m_hideNavBar = (atoi(v.c_str()) != 0);
            else if (k == "hidestatusbar") m_hideStatusBar = (atoi(v.c_str()) != 0);
            else if (k == "axislock") m_axisLockEnabled = (atoi(v.c_str()) != 0);
            else if (k == "dtapzoom") m_dtapZoomEnabled = (atoi(v.c_str()) != 0);
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
    s += "gpudefault=" + std::to_string(m_gpuDefault ? 1 : 0) + "\n";
    s += "ua_custom=" + WideToUtf8(m_uaCustom) + "\n";
    s += "updatecheck=" + std::to_string(m_updateAuto ? 1 : 0) + "\n";
    s += "prefetch=" + std::to_string(m_prefetch) + "\n";
    s += "scrollfab=" + std::to_string(m_showScrollFab ? 1 : 0) + "\n";
    s += "hidenavbar=" + std::to_string(m_hideNavBar ? 1 : 0) + "\n";
    s += "hidestatusbar=" + std::to_string(m_hideStatusBar ? 1 : 0) + "\n";
    s += "axislock=" + std::to_string(m_axisLockEnabled ? 1 : 0) + "\n";
    s += "dtapzoom=" + std::to_string(m_dtapZoomEnabled ? 1 : 0) + "\n";
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
    if (SetZoomSlider) SetZoomSlider->Value = m_defaultZoom;
    if (SetZoomLabel) SetZoomLabel->Text = ref new String((std::to_wstring(m_defaultZoom) + L"%").c_str());
    if (SetGpuSwitch) SetGpuSwitch->IsOn = m_gpuDefault;
    if (SetUpdateSwitch) SetUpdateSwitch->IsOn = m_updateAuto;
    if (SetPrefetchCombo) SetPrefetchCombo->SelectedIndex = m_prefetch;
    if (SetScrollFabSwitch) SetScrollFabSwitch->IsOn = m_showScrollFab;
    if (SetHideNavBarSwitch) SetHideNavBarSwitch->IsOn = m_hideNavBar;
    if (SetHideStatusBarSwitch) SetHideStatusBarSwitch->IsOn = m_hideStatusBar;
    if (SetAxisLockSwitch) SetAxisLockSwitch->IsOn = m_axisLockEnabled;
    if (SetDtapZoomSwitch) SetDtapZoomSwitch->IsOn = m_dtapZoomEnabled;
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
    if (SetZoomSlider) m_defaultZoom = (int)(SetZoomSlider->Value + 0.5);
    if (SetGpuSwitch) m_gpuDefault = SetGpuSwitch->IsOn;
    if (SetUpdateSwitch) m_updateAuto = SetUpdateSwitch->IsOn;
    if (SetPrefetchCombo && SetPrefetchCombo->SelectedIndex >= 0) m_prefetch = SetPrefetchCombo->SelectedIndex;
    if (SetScrollFabSwitch) m_showScrollFab = SetScrollFabSwitch->IsOn;
    if (SetHideNavBarSwitch) m_hideNavBar = SetHideNavBarSwitch->IsOn;
    if (SetHideStatusBarSwitch) m_hideStatusBar = SetHideStatusBarSwitch->IsOn;
    if (SetAxisLockSwitch) m_axisLockEnabled = SetAxisLockSwitch->IsOn;
    if (SetDtapZoomSwitch) m_dtapZoomEnabled = SetDtapZoomSwitch->IsOn;
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
    { L"默认启用 GPU 渲染(加载首个网页后自动开)", L"Enable GPU rendering by default (auto after first page)" },
    { L"立即开启 GPU 合成(重启回软件)", L"Enable GPU compositing now (restart reverts)" },
    { L"清除数据", L"Clear data" }, { L"清除历史记录", L"Clear history" },
    { L"清除全部收藏", L"Clear all bookmarks" }, { L"清除下载记录", L"Clear downloads" },
    { L"清除 Cookie(退出全部登录)", L"Clear cookies (sign out everywhere)" },
    { L"诊断", L"Diagnostics" }, { L"导出调试日志 / 崩溃 dump", L"Export debug log / crash dump" },
    { L"开发者选项", L"Developer settings" }, { L"显示翻页按钮", L"Show scroll buttons" },
    { L"交互", L"Interaction" },
    { L"轴锁定(单指滚动吸附方向)", L"Axis lock" },
    { L"双击缩放", L"Double-tap to zoom" },
    { L"隐藏系统导航栏", L"Hide navigation bar" },
    { L"从屏幕底部向上轻扫可临时唤回",
      L"Swipe up from the bottom edge to bring it back temporarily" },
    { L"隐藏状态栏", L"Hide status bar" },
    { L"隐藏顶部状态栏(时钟/信号),内容区随之上移",
      L"Hide the top status bar (clock/signal); the content area moves up" },
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
        auto rgba = std::make_shared<std::vector<uint8_t>>(EngineBufferBytes(), 0);
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
// Apotheosis: 标签切换快照。
// 切换仍是"拆会话 + 完整重载",但切过去的一瞬间先把目标标签上次离开时
// 的那帧贴出来,而不是让用户盯着上一个标签的画面等一次完整网络加载。快照是明确的占位图:
// 真实重载在下面照跑,第一帧到位(OnNavDone)就换回真画面。
// ---------------------------------------------------------------------------

static const size_t kMaxTabSnapshots = 3;   // 3 × 720×1080×4 ≈ 9 MB 上限(32 位进程,内存紧)

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
        auto rgba = std::make_shared<std::vector<uint8_t>>(EngineBufferBytes(), 0);
        // Apotheosis (landscape/rotation): the viewport the engine is about to paint at, read on
        //   the engine thread so it is the one the pixels really have (kW/kH move on the UI thread
        //   ahead of the WebCoreResize that follows this call in the same FIFO queue).
        const int snapW = kW, snapH = kH;
        int rc = -1;
        try {
            // ★ 直呈现模式下不能用 WebCoreSessionPaint:它会走 gpuPresent 再 swapBuffers 一次,
            //   且根本不填 rgba(见 port\WebCoreDriver.cpp paintToRGBA)。离屏合成+glReadPixels
            //   的 WebCoreCompositeReadback 才是这里要的。软件模式反过来只有 SessionPaint 能用。
            rc = gpu ? WebCoreCompositeReadback(rgba->data()) : WebCoreSessionPaint(rgba->data());
        } catch (...) { rc = -1; }
        if (rc != 0) return;   // 抓不到就没有快照,退回原来的行为(零回归)
        try {
            disp->RunAsync(CoreDispatcherPriority::Low, ref new DispatchedHandler([self, rgba, idx, url, seq, snapW, snapH]() {
                MainPage^ s = self.Get();
                if (!s) return;
                if (idx < 0 || idx >= (int)s->m_tabs.size()) return;   // 期间关过标签
                if (idx == s->m_activeTab) return;                     // 又切回来了:活动标签不留快照
                if (s->m_tabs[idx].currentUrl != url) return;          // 索引左移/该格换了页面
                s->m_tabs[idx].snapshot = rgba;
                s->m_tabs[idx].snapSeq = seq;
                s->m_tabs[idx].snapW = snapW;
                s->m_tabs[idx].snapH = snapH;
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
    const int snapW = m_tabs[i].snapW, snapH = m_tabs[i].snapH;
    m_tabs[i].snapshot.reset();
    m_tabs[i].snapSeq = 0;
    m_tabs[i].snapW = 0;
    m_tabs[i].snapH = 0;
    // Apotheosis (landscape/rotation): a snapshot is only a placeholder, so one taken in the other
    //   orientation is dropped rather than stretched - the real reload underneath is on its way
    //   anyway. The buffer is >= the frame (session high-water mark), hence the size check is >=.
    if (!snap || snapW != kW || snapH != kH || snap->size() < (size_t)kW * kH * 4) return;
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
    // Apotheosis (review 2026-09-04 item 3): the gesture that opened the switcher belongs to the
    //   tab we are leaving. NavigateTo() below ends it too, but not before ShowTabSnapshot()/the
    //   scale restore have run against gesture state from another page.
    EndGesture(GestureEnd::TabSwitch);
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
    EndGesture(GestureEnd::TabSwitch);   // Apotheosis (review 2026-09-04 item 3): see RestoreTab
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
// ============================================================================
// Apotheosis (landscape/rotation, 0.1.9.41): the engine viewport follows the panel.
//
// ROOT CAUSE of "landscape just stretches the picture". Two numbers decided everything the engine
// drew, and both were written once and never again:
//   * the ANGLE window surface, created in EnableGpu() with EGLRenderSurfaceSizeProperty. That
//     property means "fixed size, scale it to the panel's rectangle" - which is a stretch by
//     definition. In portrait the fixed 720x1080 happened to be near enough the panel's aspect
//     that it read as right; rotate, and a portrait picture is scaled across a landscape panel.
//   * kW/kH, the size the driver lays out and composites at (LocalFrameView + glViewport), passed
//     to WebCoreGpuInit and WebCoreSessionLoad. Nothing ever told the engine the panel had
//     changed, so no relayout at the new width could even happen.
//
// FIX, both halves. ANGLE gets EGLRenderResolutionScaleProperty instead: the surface is then the
// panel's own size times that scale, and ANGLE resizes it when the panel resizes or rotates
// (angle\include\angle_windowsstore.h documents exactly that). The scale is pinned once, from the
// panel we bind to, so that the SHORT side of the viewport is kEngineShortSidePx - portrait width
// stays the 720 it has always been - and the long side follows the panel in proportion, i.e. the
// engine viewport always has the panel's aspect ratio. And UpdateEngineViewport() tells the driver
// (WebCoreResize) whenever that size changes, which is what makes a rotation a real relayout at
// the new width instead of a rescale of the old picture.
//
// Side effect worth knowing: the fixed 720x1080 was never the panel's aspect ratio even in
// portrait (the panel is taller than 3:2 once the nav bar is subtracted), so the frame was already
// being stretched vertically by roughly a tenth. Pages are laid out at their true proportions now,
// in both orientations.
// ============================================================================

// The engine viewport for the panel that is presenting, in engine px. false = nothing usable to
// measure yet (no arrange), in which case the caller leaves the current viewport alone.
bool MainPage::ComputeEngineViewport(bool useGpuPanel, int& outW, int& outH)
{
    double w = 0.0, h = 0.0;
    try {
        // The GPU panel IS the ANGLE surface, so in present mode its rectangle is the one the swap
        //   chain has to match. In software mode the frame is an Image stretched over ContentArea.
        // m_engineFollowsGpuPanel is set the moment EnableGpu() binds ANGLE to the panel, which is
        //   several XAML layout passes before m_gpuPresent goes true - without it the panel's own
        //   first SizeChanged would measure ContentArea and hand the driver a size the surface
        //   does not have.
        if ((useGpuPanel || m_engineFollowsGpuPanel) && GpuPanel
            && GpuPanel->ActualWidth > 1.0 && GpuPanel->ActualHeight > 1.0) {
            w = GpuPanel->ActualWidth;
            h = GpuPanel->ActualHeight;
        } else if (ContentArea && ContentArea->ActualWidth > 1.0 && ContentArea->ActualHeight > 1.0) {
            w = ContentArea->ActualWidth;
            h = ContentArea->ActualHeight;
        }
    } catch (...) { return false; }
    if (!(w > 1.0) || !(h > 1.0)) return false;

    // Pin the DIP -> engine-px scale the first time we can measure. It must never move afterwards:
    //   ANGLE was handed this same number as the resolution scale when the surface was created, and
    //   the two sides only stay in step while they use the same factor. EnableGpu() deliberately
    //   clears it first, so it is pinned to whichever element actually ends up presenting.
    if (!(m_engineScale > 0.0)) {
        m_engineScale = (double)kEngineShortSidePx / (w < h ? w : h);
        if (m_engineScale < 0.25) m_engineScale = 0.25;
        if (m_engineScale > 8.0) m_engineScale = 8.0;
    }

    int ew = (int)(w * m_engineScale + 0.5);
    int eh = (int)(h * m_engineScale + 0.5);
    // Guard rails, not policy: the driver rejects sizes outside its own surface limits with
    //   kErrBadArgs, and a viewport this far from the sane range means the measurement was junk.
    if (ew < 240) ew = 240;
    if (eh < 240) eh = 240;
    if (ew > 2560) ew = 2560;
    if (eh > 2560) eh = 2560;
    outW = ew;
    outH = eh;
    return true;
}

// Apotheosis (0.1.9.45): how much of the presenting panel the on-screen keyboard covers, in ENGINE
//   px. The keyboard is an OS overlay over the window: it does not resize the panel, and while a
//   PAGE field has focus ApplyKeyboardShift deliberately moves nothing out of its way (only
//   address-bar editing shifts the chrome). So the engine's viewport keeps including a strip the
//   user cannot see, and a "scroll the focused field into the viewport" lands the field behind the
//   keyboard - in landscape that strip is more than half the panel. WebCoreSetBottomOcclusion()
//   hands this number over so the engine can leave that much room.
//   Same element choice as ComputeEngineViewport (the panel that actually presents), and the same
//   pinned DIP -> engine px factor. 0 = keyboard down, or it does not reach into the panel.
//   UI THREAD ONLY.
int MainPage::BottomOcclusionEnginePx()
{
    if (!m_kbVisible || !(m_kbHeight > 0.0) || !(m_engineScale > 0.0))
        return 0;
    double panelTop = 0.0, panelH = 0.0;
    try {
        Windows::UI::Xaml::UIElement^ el = nullptr;
        if (m_engineFollowsGpuPanel && GpuPanel && GpuPanel->ActualHeight > 1.0) {
            el = GpuPanel; panelH = GpuPanel->ActualHeight;
        } else if (ContentArea && ContentArea->ActualHeight > 1.0) {
            el = ContentArea; panelH = ContentArea->ActualHeight;
        }
        if (!el) return 0;
        // Window coordinates, which is the space InputPane::OccludedRect answers in.
        auto t = el->TransformToVisual(nullptr);
        panelTop = (double)t->TransformPoint(Windows::Foundation::Point(0.0f, 0.0f)).Y;
    } catch (...) { return 0; }
    if (!(panelH > 0.0)) return 0;
    double occ = (panelTop + panelH) - m_kbTop;   // how far the keyboard reaches up into the panel
    if (occ <= 0.0) return 0;
    if (occ > panelH) occ = panelH;
    return (int)(occ * m_engineScale + 0.5);
}

// UI THREAD ONLY. Re-measure and, if the viewport moved, hand the new size to the engine.
// `force` re-sends the current size even when it has not changed (used right after WebCoreGpuInit,
// where the driver's GL viewport and a session that may already exist have to be brought together).
void MainPage::UpdateEngineViewport(const char* why, bool force, int forceW, int forceH)
{
    // A surface pinned to a fixed size (the no-arranged-panel fallback in EnableGpu) cannot follow
    //   the panel, and telling the driver otherwise would only desync the two. Rotation keeps
    //   stretching in that case, exactly as it did before 0.1.9.41 - and the gpu-surface stage line
    //   says which mode this run is in.
    if (m_gpuOn && !m_engineFollowsGpuPanel)
        return;
    int ew = 0, eh = 0;
    if (forceW > 0 && forceH > 0) {
        // Adopt a measured surface verbatim (surface-mismatch below): the GL viewport has to be
        //   the framebuffer's own size, not a recomputed panel x scale that can round elsewhere.
        ew = forceW;
        eh = forceH;
    } else if (!ComputeEngineViewport(m_gpuPresent, ew, eh)) {
        return;
    }
    const int oldW = kW, oldH = kH;
    if (!force && ew == oldW && eh == oldH)
        return;

    // ORDER MATTERS. Raise the buffer high-water mark, then kW/kH, then post - so that every
    //   engine call queued from here on allocates for the new viewport while the engine itself is
    //   still on the old one, and every call queued before this runs against the old engine with
    //   an old-sized buffer. Both are covered because every buffer is EngineBufferBytes() big.
    NoteEngineViewport(ew, eh);
    kW = ew;
    kH = eh;

    // Anything the harness itself keeps at the old size has to go: the software double buffer and
    //   the snapshot bitmap are recreated on demand, and the cached scroll/content bounds are about
    //   to be replaced by the relayout's.
    m_frameBmpA = nullptr;
    m_frameBmpB = nullptr;
    m_snapBmp = nullptr;
    m_scrollStateValid = false;

    double panelW = 0.0, panelH = 0.0;
    try { if (GpuPanel) { panelW = GpuPanel->ActualWidth; panelH = GpuPanel->ActualHeight; } } catch (...) {}
    // Measured HERE, on the UI thread, and carried to the engine thread below: the engine reveals
    //   the focused field after the relayout and has no other way to know what the keyboard covers.
    const int occ = BottomOcclusionEnginePx();
    WriteStage((std::string("resize why=") + (why ? why : "?")
                + " eng=" + std::to_string(oldW) + "x" + std::to_string(oldH)
                + "->" + std::to_string(ew) + "x" + std::to_string(eh)
                + " panel=" + Dip(panelW) + "x" + Dip(panelH)
                + " scale=" + Dip(m_engineScale)
                + " gpu=" + (m_gpuPresent ? "1" : "0")
                + " occ=" + std::to_string(occ)).c_str());

    // Apotheosis (landscape, 0.1.9.43): a static page (start page / error page) is a ONE-SHOT
    //   WebCoreRenderHtml with no session behind it, so WebCoreResize finds no LocalFrameView and
    //   the picture stayed at the width it was first laid out for - the start page did not reflow
    //   on a rotation while an ordinary site did. Re-rendering the same HTML at the new viewport
    //   is the relayout. Not while a load is running: that load is about to replace the page.
    const bool staticPage = (!m_staticHtml.empty() && !m_loading);

    if (!m_sessionActive && !m_gpuOn) {
        // Nothing live to resize; the next WebCoreSessionLoad carries kW/kH itself - but a static
        // page is on screen right now and nothing else will ever repaint it.
        if (staticPage) RenderStaticPage("resize");
        return;
    }

    bool present = m_gpuPresent;
    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = m_opSeq;   // as PumpScroll: not a token of its own, but superseded by one
    WebEngine::instance().post([disp, self, ew, eh, present, staticPage, mySeq, occ]() {
        auto rgba = AcquireEngineBuffer(present);
        int rc = -999, surfW = 0, surfH = 0;
        try { WebCoreSetBottomOcclusion(occ); } catch (...) {}
        try { rc = WebCoreResize(ew, eh, &surfW, &surfH, rgba->data()); } catch (...) { rc = -1000; }
        int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rgba, rcCopy, mySeq, present, staticPage, ew, eh, surfW, surfH]() {
                MainPage^ s = self.Get(); if (!s) return;
                // What ANGLE really made of it (traced on the UI thread - WriteStage has one
                //   truncate-once static and is not written for two writers). If this ever stops
                //   matching the requested size, the panel-to-surface scale is wrong and the
                //   picture is being scaled again: the one number the next device round needs,
                //   and the reason WebCoreResize reports it at all.
                WriteStage((std::string("resized rc=") + std::to_string(rcCopy)
                            + " eng=" + std::to_string(ew) + "x" + std::to_string(eh)
                            + " surface=" + std::to_string(surfW) + "x" + std::to_string(surfH)).c_str());
                // SAFETY NET. ANGLE derives the surface from the panel and the resolution scale
                //   it was given; the harness derives the viewport from the same panel and the
                //   same scale, so the two should be the same number. If they are not, the
                //   assumption behind the scale is wrong somewhere and the engine would be
                //   rendering at a size the surface does not have - a garbage frame. Adopt what
                //   the surface actually is instead, once (WebCoreResize cannot change the
                //   surface, so the next answer agrees and this settles immediately), unless
                //   doing so would cost more pixels than this device can raster - in that case
                //   keep the requested size and take the stretch, which is what 0.1.9.40 did
                //   everywhere. Either way the `resized` line above has already recorded it.
                const bool surfaceKnown = (surfW > 0 && surfH > 0 && ew > 0 && eh > 0);
                const bool surfaceDiffers = surfaceKnown
                    && (std::abs(surfW - ew) > 2 || std::abs(surfH - eh) > 2);
                // Apotheosis (bug fix 0.1.9.44, half a page header missing at the top edge): TWO
                //   guards this readback needs before a single number of it may be believed, both
                //   of them missing in 0.1.9.41-43 - and the device log shows what that cost. A
                //   rotation queues one WebCoreResize per panel size, each answering on a later
                //   turn of the UI thread, and eglSwapBuffers only picks the panel's new size up
                //   at the swap. So an answer can arrive (a) for a viewport a newer resize has
                //   already superseded, and (b) carrying the surface of the PREVIOUS orientation.
                //   Both happened at once: the readback for a portrait 831x1255 reported the
                //   landscape 1477x720 still on the swap chain, this net multiplied m_engineScale
                //   by 1477/831, and the second correction only walked it back to a value ANGLE
                //   never had. From then on every viewport was ~2.5 % larger than the framebuffer
                //   it was rendered into (852x1401 into 831x1366) - and since glViewport's origin
                //   is the framebuffer's BOTTOM-left while TextureMapper puts the page's top edge
                //   at the viewport's top, the excess is clipped off the TOP: 35 engine px of page
                //   gone, i.e. half of a viewport-fixed header, in both orientations, for the rest
                //   of the session.
                //   (a) is caught by requiring the answer to be for the viewport that is current,
                //   (b) by requiring the surface to have the same orientation as the request. A
                //   clean readback re-arms the correction, so a scale that really is wrong still
                //   gets fixed at the next rotation.
                const bool currentViewport = (ew == kW && eh == kH);
                const bool sameOrientation = surfaceKnown && ((ew >= eh) == (surfW >= surfH));
                if (surfaceKnown && currentViewport && sameOrientation) {
                    if (!surfaceDiffers) {
                        s->m_engineCalibrations = 0;   // the two agree: allow a future correction
                    } else if (s->m_engineFollowsGpuPanel && s->m_engineCalibrations < 3) {
                        ++s->m_engineCalibrations;
                        const long long px = (long long)surfW * (long long)surfH;
                        if (px <= (long long)kMaxEngineViewportPixels) {
                            // Correct OUR factor by the ratio ANGLE applied, so that
                            //   ComputeEngineViewport() produces the surface from now on - in this
                            //   orientation and in the other one, since the same ratio applies to
                            //   both. Yes, this moves m_engineScale after the surface was created:
                            //   ANGLE keeps the resolution scale it was handed at
                            //   eglCreateWindowSurface and nothing can change it afterwards, so it
                            //   is our formula that has to end up where ANGLE already is. The
                            //   viewport itself is then the surface VERBATIM - rendering into a
                            //   framebuffer even one pixel smaller is the bug described above.
                            const double rx = (double)surfW / (double)ew;
                            const double ry = (double)surfH / (double)eh;
                            s->m_engineScale *= (rx + ry) * 0.5;
                            if (s->m_engineScale < 0.05) s->m_engineScale = 0.05;
                            if (s->m_engineScale > 16.0) s->m_engineScale = 16.0;
                            s->UpdateEngineViewport("surface-mismatch", /*force*/ true, surfW, surfH);
                        } else {
                            s->m_engineFollowsGpuPanel = false;   // stop chasing it; stretch as before
                            WriteStage(("resize-giveup surface=" + std::to_string(surfW) + "x" + std::to_string(surfH)
                                        + " max=" + std::to_string(kMaxEngineViewportPixels)).c_str());
                        }
                        return;
                    }
                } else if (surfaceDiffers) {
                    // Not acted on, but the one line that says a mismatch was seen and why it was
                    //   not believed - without it the next round cannot tell "settled" from "the
                    //   guards ate a real correction".
                    WriteStage((std::string("resize-ignored why=")
                                + (!currentViewport ? "stale" : "orientation")
                                + " eng=" + std::to_string(ew) + "x" + std::to_string(eh)
                                + " cur=" + std::to_string(kW) + "x" + std::to_string(kH)
                                + " surface=" + std::to_string(surfW) + "x" + std::to_string(surfH)).c_str());
                }
                if (s->m_opSeq != mySeq) return;     // a navigation/click has taken over since
                // A static page's buffer was never filled here (WebCoreResize returns early with
                //   no session), and RenderStaticPage's own frame is on its way - presenting this
                //   one would be a black flash between the two.
                if (rcCopy == 0 && !present && !staticPage) s->PresentSoftwareFrame(rgba);
                s->m_lastFrameHash = 0;              // force the next live frame to be re-shown
                if (!staticPage) {
                    s->SyncLinksAfterScroll();       // the hit table was built for the old layout
                    s->StartLiveMode();
                }
            }));
        } catch (...) {}
    });

    // Queued AFTER the resize job on the same engine queue, so the GL viewport is already the new
    //   one when the page is re-rendered into it.
    if (staticPage) RenderStaticPage("resize");
}

// Apotheosis (landscape, 0.1.9.43): re-render the static page that is on screen (start page or
// error page) at the current engine viewport. Both are plain engine HTML rendered ONCE by
// WebCoreRenderHtml with no session behind them, so there is no LocalFrameView for WebCoreResize
// to lay out and nothing that would ever repaint them - which is why the start page kept its
// portrait layout after a rotation while an ordinary site reflowed. Rendering the same source at
// the new size is the relayout; the tiles' <a> hit table is re-read with it, because the tile
// rectangles have moved. UI THREAD ONLY.
void MainPage::RenderStaticPage(const char* why)
{
    if (m_staticHtml.empty()) return;
    const std::string html = m_staticHtml;
    const int w = kW, h = kH;
    WriteStage((std::string("static-render why=") + (why ? why : "?")
                + " eng=" + std::to_string(w) + "x" + std::to_string(h)
                + " len=" + std::to_string(html.size())).c_str());

    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    unsigned long long mySeq = m_opSeq;   // superseded by any navigation/click that starts meanwhile
    WebEngine::instance().post([disp, self, html, w, h, mySeq]() {
        auto rgba = std::make_shared<std::vector<uint8_t>>(EngineBufferBytes(), 0);
        int rc = -999;
        try { rc = WebCoreRenderHtml(html.c_str(), w, h, rgba->data()); } catch (...) { rc = -1000; }
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
        const int rcCopy = rc;
        try {
            disp->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([self, rgba, links, rcCopy, mySeq]() {
                MainPage^ s = self.Get(); if (!s) return;
                if (s->m_opSeq != mySeq) return;
                if (rcCopy != 0) return;
                s->PresentSoftwareFrame(rgba);
                s->m_pageLinks = *links;
                s->m_lastFrameHash = 0;
            }));
        } catch (...) {}
    });
}

// Subscribe the presenting elements' SizeChanged, once each. Idempotent: called from the
// constructor (software path is live from the first frame) and again when GPU comes up.
void MainPage::WirePresentPanelSizeChanged()
{
    auto handler = ref new Windows::UI::Xaml::SizeChangedEventHandler(this, &MainPage::OnPresentPanelSizeChanged);
    try {
        if (ContentArea && !m_contentSizeHandlerWired) {
            m_contentSizeHandlerWired = true;
            ContentArea->SizeChanged += handler;
        }
    } catch (...) {}
    try {
        if (GpuPanel && !m_presentSizeHandlerWired) {
            m_presentSizeHandlerWired = true;
            GpuPanel->SizeChanged += handler;
        }
    } catch (...) {}
}

// The panel changed size - a device rotation, or any other relayout that moves it. Both presenting
// elements are wired: GpuPanel is Collapsed (and therefore silent) until GPU comes up, ContentArea
// carries the software path and stays measurable throughout.
void MainPage::OnPresentPanelSizeChanged(Platform::Object^, Windows::UI::Xaml::SizeChangedEventArgs^ e)
{
    if (!e || e->NewSize.Width <= 1.0f || e->NewSize.Height <= 1.0f) return;
    UpdateEngineViewport("panel", false);
    // The keyboard rectangle changes with the orientation without raising Showing again; the
    //   VisibleBoundsChanged path calls this too, whichever of the two arrives with the final
    //   numbers wins and the other one is a no-op.
    RefreshKeyboardMetrics("panel");
}

void MainPage::EnableGpu()
{
    if (m_gpuOn) return;

    // Apotheosis (landscape/rotation, 0.1.9.41): the resolution scale below has to be measured off
    //   an ARRANGED panel, and GpuPanel is Collapsed - therefore 0x0 - until this call makes it
    //   visible. The startup path goes through HookGpuPanelForStartup and arrives with a real
    //   size; the other two callers (the GPU toggle, OnNavDone's default-GPU branch) do not, and
    //   without this they would silently fall back to a fixed surface and rotate by stretching.
    //   So: show the panel, let XAML arrange it, come back once. One retry only - if the size is
    //   still not there the fixed-surface fallback below takes over, exactly as before. This also
    //   keeps the older size gate's promise on those paths: ANGLE is never bound to a panel that
    //   has never been arranged (the 2026-09-03 libGLESv2 AV, see HookGpuPanelForStartup).
    if (!m_gpuEnableRetried && GpuPanel && !(GpuPanel->ActualWidth > 1.0 && GpuPanel->ActualHeight > 1.0)) {
        m_gpuEnableRetried = true;
        GpuPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
        Platform::Agile<MainPage^> retrySelf(this);
        try {
            this->Dispatcher->RunAsync(CoreDispatcherPriority::Low, ref new DispatchedHandler([retrySelf]() {
                MainPage^ s = retrySelf.Get(); if (!s) return;
                s->EnableGpu();
            }));
            return;
        } catch (...) { /* could not defer: fall through and take the old behaviour */ }
    }

    CoreDispatcher^ disp = this->Dispatcher;
    Platform::Agile<MainPage^> self(this);
    GpuPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;

    // Apotheosis (landscape/rotation, 0.1.9.41): pin the panel -> engine scale to the panel we are
    //   about to bind ANGLE to, and give ANGLE that same scale rather than a fixed surface size.
    //   See the block above ComputeEngineViewport for why the fixed size was the landscape bug.
    m_engineScale = 0.0;   // re-pin: this panel, not whatever the software path measured earlier
    int engW = kW, engH = kH;
    const bool sized = ComputeEngineViewport(/*useGpuPanel*/ true, engW, engH);
    m_engineFollowsGpuPanel = sized;

    auto props = ref new Windows::Foundation::Collections::PropertySet();
    props->Insert(L"EGLNativeWindowTypeProperty", GpuPanel);
    if (sized) {
        // "If the window resizes or rotates then the surface will resize accordingly"
        //   (angle\include\angle_windowsstore.h). Single, not Size - the two properties are
        //   mutually exclusive, so EGLRenderSurfaceSizeProperty must NOT be set alongside it.
        props->Insert(L"EGLRenderResolutionScaleProperty",
                      Windows::Foundation::PropertyValue::CreateSingle((float)m_engineScale));
    } else {
        // No arranged panel to calibrate against (the 2 s startup fallback can get here). Keep the
        //   pre-0.1.9.41 fixed-surface behaviour rather than guessing a scale: rotation will still
        //   stretch, but startup is not made any more fragile than it was.
        m_engineScale = 0.0;
        props->Insert(L"EGLRenderSurfaceSizeProperty",
                      Windows::Foundation::PropertyValue::CreateSize(Windows::Foundation::Size((float)kW, (float)kH)));
    }
    m_gpuProps = props;
    void* win = reinterpret_cast<void*>(reinterpret_cast<IInspectable*>(props));
    WriteStage((std::string("gpu-surface mode=") + (sized ? "scaled" : "fixed")
                + " panel=" + GpuPanelSizeStr()
                + " scale=" + Dip(m_engineScale)
                + " eng=" + std::to_string(engW) + "x" + std::to_string(engH)).c_str());
    // The surface is created at engW x engH, so that is what the driver must composite at. kW/kH
    //   move here, on the UI thread, before anything can be queued for the new size - the same
    //   ordering rule UpdateEngineViewport() documents.
    NoteEngineViewport(engW, engH);
    kW = engW;
    kH = engH;
    // 崩溃环路保护:开 GPU 前落 gpu-crash.flag;回调(成功或优雅失败)删它。GpuInit 硬崩则无回调→标记残留→下次启动检测到→关默认GPU。
    {
        std::wstring fd = LocalStateDir();
        if (!fd.empty()) { try { std::ofstream f(WideToUtf8(fd) + "\\gpu-crash.flag", std::ios::binary | std::ios::trunc); if (f) f << "1"; } catch (...) {} }
    }
    WebEngine::instance().post([disp, self, win, engW, engH]() {
        int rc = -999;
        try { rc = WebCoreGpuInit(win, engW, engH); } catch (...) { rc = -1000; }
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
                    // Apotheosis (landscape/rotation, 0.1.9.41): from here the GPU panel is the
                    //   presenting element, and it may have been arranged (or the device rotated)
                    //   while GpuInit was in flight. Force one pass so the driver's GL viewport,
                    //   a session that already exists, and kW/kH are all the same size, and wire
                    //   the panel's SizeChanged for every rotation from now on.
                    s->WirePresentPanelSizeChanged();
                    s->UpdateEngineViewport("gpu-init", /*force*/ true);
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
                    // There is no surface to follow: the viewport must be measured off
                    //   ContentArea (the software frame) from here on, not off the hidden panel.
                    s->m_engineFollowsGpuPanel = false;
                    s->UpdateEngineViewport("gpu-failed", /*force*/ false);
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
