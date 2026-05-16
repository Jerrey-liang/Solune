#include "Solune.h"

#include <windows.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <winrt/Windows.Foundation.h>

/** 初始化 WinRT 环境并启动应用入口。 */
int main()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    const int stdoutModeResult = _setmode(_fileno(stdout), _O_U16TEXT);
    const int stderrModeResult = _setmode(_fileno(stderr), _O_U16TEXT);

    try
    {
        winrt::init_apartment();
    }
    catch (...)
    {
        return -1;
    }

    sts::App app;
    return app.run();
}
