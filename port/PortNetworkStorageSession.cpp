// PortNetworkStorageSession.cpp — 见 .h。激活自 draft-PortNetworkStorageSession.cpp。
// 拥有一个进程级默认 curl NetworkStorageSession(模仿 WebKitLegacy 的
// NetworkStorageSessionMap::defaultStorageSession()),让 cookie 真正被读写+落盘。
// 落盘路径:harness 经 WebCoreSetCookieJarPath(见 WebCoreDriver.cpp)显式注入 LocalState 路径,
// setPortCookieJarPath 存下,defaultPortStorageSession() 首次建会话时用它直接建 CookieJarDB
// (setCookieDatabase 覆盖构造函数内部按 NetworkStorageSessionCurl.cpp 的 defaultCookieJarPath()
// 建的那份)。不依赖环境变量 CURL_COOKIE_JAR_PATH——clang-cl 引擎与 MSVC v143 harness 是两套独立
// CRT,_putenv_s/getenv 是否共享环境块未经验证;也不落到 defaultCookieJarPath() 的默认路径
// (localUserSpecificStorageDirectory() 在 WK_WINUWP 下返回空串,App Container 不可写)。
#include "config.h"

#include "PortNetworkStorageSession.h"

#include <WebCore/Cookie.h>
#include <WebCore/CookieJarDB.h>
#include <WebCore/NetworkStorageSession.h>
#include <WebCore/StorageSessionProvider.h>
#include <WebCore/FrameNetworkingContext.h>
#include <WebCore/LocalFrame.h>
#include <pal/SessionID.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/MainThread.h>
#include <wtf/UniqueRef.h>
#include <wtf/text/CString.h>
#include <wtf/Vector.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cerrno>
#include <span>
#include <string>

#if PLATFORM(WIN)
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#endif

namespace WebCorePort {

using namespace WebCore;

static String& portCookieJarPathSlot()
{
    static NeverDestroyed<String> path;
    return path.get();
}

void setPortCookieJarPath(const String& path)
{
    portCookieJarPathSlot() = path;
}

// 单一进程级默认 curl session。主线程(WebKit 主线程,驱动在 ensureWebCoreInitialized 里 pin)。
WebCore::NetworkStorageSession& defaultPortStorageSession()
{
    ASSERT(isMainThread());
    static NeverDestroyed<std::unique_ptr<WebCore::NetworkStorageSession>> session;
    if (!session.get()) {
        auto s = makeUnique<WebCore::NetworkStorageSession>(PAL::SessionID::defaultSessionID());
        // ★ 持久会话:显式注入 jar 落盘路径(setPortCookieJarPath,harness 在引擎线程首个任务前调),
        //   立即覆盖构造函数内部按 defaultCookieJarPath() 建的那份(未打开,无 I/O,覆盖无害)。不走
        //   环境变量:两套独立 CRT(clang-cl 引擎 / MSVC v143 harness)是否共享 _putenv_s/getenv 环境块
        //   未经验证;也不走 localUserSpecificStorageDirectory() 的默认路径(WK_WINUWP 下返回空串,
        //   App Container 不可写,曾是 0.1.6.0 访问任何网页闪退的根因)。未设置时退化 ":memory:"
        //   (等同旧版临时会话,不持久但绝不崩;CookieJarDB::open() 的 WK_WINUWP 补丁对此仍是双保险)。
        const String& jarPath = portCookieJarPathSlot();
        s->setCookieDatabase(makeUniqueRef<WebCore::CookieJarDB>(jarPath.isEmpty() ? ":memory:"_s : jarPath));
        session.get() = WTF::move(s);
    }
    return *session.get();
}

// ============================================================================
// cookie 的 JSON Lines 持久化(绕开真实文件 SQLite —— 2026-07-03 真机验证会崩,见 .h 顶部注释)。
// jar 本身固定 ":memory:"(已验证稳定);这里手撸一份极简旁路持久化:每行一个 cookie 的 flat JSON
// 对象,只含 name/value/domain/path/expires/httpOnly/secure 七个已知字段,不引入 JSON 解析库
// (对齐本仓库一贯的"极简取值"风格,见 harness CheckForUpdate 的 pick() 字符串搜索)。逐行存储的
// 好处:单行损坏不影响其它行(截断写入/掉电也只丢最后一条),整体重写也廉价(cookie 数量级顶多
// 几百条,单文件几十 KB)。会话 cookie(无过期时间)不持久,语义等同浏览器"关闭即丢"。
// ============================================================================

static WTF::String& portCookieJsonPathSlot()
{
    static NeverDestroyed<String> path;
    return path.get();
}

void setPortCookieJsonPath(const String& path)
{
    portCookieJsonPathSlot() = path;
}

// Apotheosis: keep cookie side-channel failures observable on the device. The
// browser has no console, and the JSON path itself is already inside LocalState,
// so a sibling `.diag` file is the least invasive diagnostic channel.
static void writeCookieDiag(const char* event, const std::string& detail)
{
    const String& path = portCookieJsonPathSlot();
    if (path.isEmpty())
        return;
    auto u8 = path.utf8();
    std::string diagPath(u8.data(), u8.length());
    diagPath += ".diag";
    FILE* fp = nullptr;
    if (fopen_s(&fp, diagPath.c_str(), "ab") != 0 || !fp)
        return;
    std::fprintf(fp, "%s %s\n", event ? event : "event", detail.c_str());
    std::fclose(fp);
}

static void jsonAppendEscaped(std::string& out, const CString& utf8)
{
    const char* p = utf8.data();
    if (!p)
        return;
    for (size_t i = 0; i < utf8.length(); ++i) {
        unsigned char c = static_cast<unsigned char>(p[i]);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else
                out += static_cast<char>(c);
        }
    }
}

static void writeCookieLine(std::string& out, const Cookie& c)
{
    out += "{\"name\":\"";
    jsonAppendEscaped(out, c.name.utf8());
    out += "\",\"value\":\"";
    jsonAppendEscaped(out, c.value.utf8());
    out += "\",\"domain\":\"";
    jsonAppendEscaped(out, c.domain.utf8());
    out += "\",\"path\":\"";
    jsonAppendEscaped(out, c.path.utf8());
    out += "\",\"expires\":";
    out += std::to_string(c.expires ? static_cast<long long>(*c.expires) : 0LL);
    out += ",\"httpOnly\":";
    out += c.httpOnly ? "true" : "false";
    out += ",\"secure\":";
    out += c.secure ? "true" : "false";
    out += "}\n";
}

// 找 "key":"..." 取带转义的字符串值(只识别自己写的那几种转义,够用)。
static String jsonFieldString(const std::string& line, const char* key)
{
    std::string pat = std::string("\"") + key + "\":\"";
    size_t p = line.find(pat);
    if (p == std::string::npos)
        return String();
    p += pat.size();
    std::string out;
    for (size_t i = p; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"')
            break;
        if (c == '\\' && i + 1 < line.size()) {
            char n = line[++i];
            switch (n) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case 'u':
                if (i + 4 < line.size()) {
                    unsigned v = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = line[i + 1 + k];
                        v <<= 4;
                        if (h >= '0' && h <= '9') v |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') v |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v |= static_cast<unsigned>(h - 'A' + 10);
                    }
                    out += static_cast<char>(v & 0xFF);   // 只我们自己写的 \u00XX 控制字符,单字节够用
                    i += 4;
                }
                break;
            default: out += n;
            }
        } else
            out += c;
    }
    return String::fromUTF8(std::span<const char>(out.data(), out.size()));
}

static bool jsonFieldBool(const std::string& line, const char* key)
{
    return line.find(std::string("\"") + key + "\":true") != std::string::npos;
}

static long long jsonFieldInt(const std::string& line, const char* key)
{
    std::string pat = std::string("\"") + key + "\":";
    size_t p = line.find(pat);
    if (p == std::string::npos)
        return 0;
    return std::strtoll(line.c_str() + p + pat.size(), nullptr, 10);
}

// 首次预热时读回磁盘快照,灌入 ":memory:" jar。文件不存在(首次启动/未配置路径)静默跳过。
static void loadCookiesFromDiskIfConfigured()
{
    const String& path = portCookieJsonPathSlot();
    if (path.isEmpty())
        return;
    auto u8 = path.utf8();
    FILE* fp = nullptr;
    int openResult = fopen_s(&fp, u8.data(), "rb");
    if (openResult != 0 || !fp) {
        writeCookieDiag("load-open-failed", "rc=" + std::to_string(openResult));
        return;
    }
    std::string content;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0)
        content.append(buf, n);
    bool readFailed = std::ferror(fp) != 0;
    std::fclose(fp);
    if (readFailed) {
        writeCookieDiag("load-read-failed", "bytes=" + std::to_string(content.size()));
        return;
    }

    CookieJarDB& jar = defaultPortStorageSession().cookieDatabase();
    const double nowMs = static_cast<double>(std::time(nullptr)) * 1000.0;
    size_t pos = 0, loaded = 0;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        std::string line = content.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? content.size() : nl + 1;
        if (line.empty() || line[0] != '{')
            continue;   // 跳过空行/截断行(掉电写半截),不让一行坏数据拖垮整份快照
        long long expires = jsonFieldInt(line, "expires");
        if (expires && static_cast<double>(expires) <= nowMs)
            continue;   // 已过期,跳过(等同浏览器行为,顺带让快照文件不会只涨不缩)
        Cookie c;
        c.name = jsonFieldString(line, "name");
        c.domain = jsonFieldString(line, "domain");
        if (c.name.isEmpty() && c.domain.isEmpty())
            continue;
        c.value = jsonFieldString(line, "value");
        c.path = jsonFieldString(line, "path");
        if (expires)
            c.expires = static_cast<double>(expires);
        c.httpOnly = jsonFieldBool(line, "httpOnly");
        c.secure = jsonFieldBool(line, "secure");
        c.session = false;
        jar.setCookie(c);
        ++loaded;
    }
    writeCookieDiag("load-ok", "bytes=" + std::to_string(content.size()) + " loaded=" + std::to_string(loaded));
}

// 把当前 jar 里的持久 cookie(有过期时间、非会话)写回磁盘。先写临时文件再改名,防止应用被 UWP
// 挂起/终止在写一半时截断留下坏文件(loadCookiesFromDiskIfConfigured 的逐行跳过也是这层保险的backup)。
void flushCookiesToDisk()
{
    ASSERT(isMainThread());
    const String& path = portCookieJsonPathSlot();
    if (path.isEmpty())
        return;
    Vector<Cookie> cookies = defaultPortStorageSession().cookieDatabase().getAllCookies();
    std::string out;
    for (const auto& c : cookies) {
        if (c.session || !c.expires)
            continue;   // 会话 cookie / 无过期时间:不持久,语义等同浏览器"关闭即丢"
        writeCookieLine(out, c);
    }
    auto u8 = path.utf8();
    std::string dest(u8.data(), u8.length());
    std::string tmp = dest + ".tmp";
    FILE* fp = nullptr;
    int openResult = fopen_s(&fp, tmp.c_str(), "wb");
    if (openResult != 0 || !fp) {
        writeCookieDiag("flush-open-failed", "rc=" + std::to_string(openResult)
            + " cookies=" + std::to_string(cookies.size()));
        return;
    }
    size_t written = std::fwrite(out.data(), 1, out.size(), fp);
    bool writeFailed = written != out.size() || std::ferror(fp) != 0;
    int closeResult = std::fclose(fp);
    if (writeFailed || closeResult != 0) {
        std::remove(tmp.c_str());
        writeCookieDiag("flush-write-failed", "written=" + std::to_string(written)
            + " expected=" + std::to_string(out.size())
            + " close=" + std::to_string(closeResult));
        return;
    }
    int removeResult = std::remove(dest.c_str());
    int renameResult = std::rename(tmp.c_str(), dest.c_str());
    if (renameResult != 0) {
        std::remove(tmp.c_str());
        writeCookieDiag("flush-rename-failed", "remove=" + std::to_string(removeResult)
            + " rename=" + std::to_string(renameResult));
        return;
    }
    writeCookieDiag("flush-ok", "cookies=" + std::to_string(cookies.size())
        + " persisted=" + std::to_string(out.size()));
}

void ensureDefaultPortStorageSession()
{
    auto& s = defaultPortStorageSession();
    // 默认只接受主文档域 cookie(各端口惯例);cookieDatabase() 在此惰性开 jar(固定 ":memory:")。
    s.setCookieAcceptPolicy(CookieAcceptPolicy::OnlyFromMainDocumentDomain);
    loadCookiesFromDiskIfConfigured();   // 把上次落盘的持久 cookie 灌回刚开好的内存 jar
}

// DOM 路:document.cookie 经此 provider 到达真 jar。
class PortStorageSessionProvider final : public WebCore::StorageSessionProvider {
public:
    static Ref<PortStorageSessionProvider> create() { return adoptRef(*new PortStorageSessionProvider); }
    WebCore::NetworkStorageSession* storageSession() const final { return &defaultPortStorageSession(); }
private:
    PortStorageSessionProvider() = default;
};

// HTTP 路:ResourceHandle 的 curl bridge 经 d->m_context->storageSession() 到达真 jar。
class PortFrameNetworkingContext final : public WebCore::FrameNetworkingContext {
public:
    static Ref<PortFrameNetworkingContext> create(WebCore::LocalFrame* frame)
    {
        return adoptRef(*new PortFrameNetworkingContext(frame));
    }
    WebCore::NetworkStorageSession* storageSession() const final { return &defaultPortStorageSession(); }

#if PLATFORM(WIN)
    WebCore::ResourceError blockedError(const WebCore::ResourceRequest&) const final { return { }; }
#endif

private:
    explicit PortFrameNetworkingContext(WebCore::LocalFrame* frame)
        : WebCore::FrameNetworkingContext(frame) { }
};

Ref<WebCore::StorageSessionProvider> makeStorageSessionProvider()
{
    return PortStorageSessionProvider::create();
}

Ref<WebCore::FrameNetworkingContext> makeFrameNetworkingContext(WebCore::LocalFrame* frame)
{
    return PortFrameNetworkingContext::create(frame);
}

} // namespace WebCorePort
