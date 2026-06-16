// WebCoreDriverProbe.cpp — 地基验证:能否用 WebCore 的 clang-cl flags 独立编一个 include WebCore 内部头的 .cpp。
// 不求渲染,只求「WebCore 公共头从外部可用 + 能编过」。成功=Phase 1b 驱动方案可行。
#include "config.h"            // WebCore 每个 TU 的首个 include(在 -IE:\...\WebKit\Source\WebCore 上)

#include <WebCore/Page.h>
#include <WebCore/Frame.h>
#include <WebCore/Document.h>

#include <cstdint>

// 导出 C 接口(外层 C++/CX app 通过它调用;ABI 中立)。
extern "C" int WebCoreProbe()
{
    // 只取静态大小,确保这些类型完整可见、头能编。不实例化(避免链接期符号)。
    return static_cast<int>(sizeof(WebCore::Page) + sizeof(WebCore::Document));
}
