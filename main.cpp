#include "Solune.h"

#include <windows.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <winrt/Windows.Foundation.h>

int main()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (...)
    {
        return -1;
    }

    sts::App app;
    return app.run();
}
