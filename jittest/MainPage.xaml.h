#pragma once
#include "MainPage.g.h"
#include <string>

namespace JITTest {
    public ref class MainPage sealed {
    public:
        MainPage();
    private:
        void RunTests();
        std::wstring m_log;
    };
}
