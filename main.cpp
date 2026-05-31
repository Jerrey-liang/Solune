#include "Solune.h"
#include "WeConfigManager.h"
#include "WeScene.h"

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
    const int stdoutModeResult = _setmode(_fileno(stdout), _O_U16TEXT);
    const int stderrModeResult = _setmode(_fileno(stderr), _O_U16TEXT);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argc >= 3 && wcscmp(argv[1], L"--test-pkg") == 0)
    {
        try { winrt::init_apartment(); } catch (...) { return -1; }
        std::wstring pkgPath = argv[2];
        double wPct = (argc >= 4) ? _wtof(argv[3]) : 0.25;
        double hPct = (argc >= 5) ? _wtof(argv[4]) : 0.08;

        std::wcout << L"=== Solune Scene Composite Test ===" << std::endl;
        std::wcout << L"PKG: " << pkgPath << std::endl;
        std::wcout << L"ROI: " << wPct << L" x " << hPct << std::endl;

        double roiAvg = 0, roiDark = 0, globalAvg = 0, globalDark = 0;
        std::wstring decodeSummary;
        if (sts::we::TestCalcSceneComposite(pkgPath, wPct, hPct, roiAvg, roiDark, globalAvg, globalDark, decodeSummary))
        {
            std::wcout << L"\nResults:" << std::endl;
            std::wcout << L"  Decode summary: " << decodeSummary << std::endl;
            std::wcout << L"  ROI avg luminance: " << roiAvg << std::endl;
            std::wcout << L"  ROI dark ratio:    " << roiDark << std::endl;
            std::wcout << L"  Global avg:        " << globalAvg << std::endl;
            std::wcout << L"  Global dark ratio: " << globalDark << std::endl;

            const wchar_t* themeLabel = (roiAvg < 0.35) ? L"Dark" : L"Light";
            std::wcout << L"  Classified as: " << themeLabel << L" (avg " << roiAvg << L" vs 0.35 threshold)" << std::endl;
        }
        else
        {
            std::wcerr << L"ERROR: Failed to composite scene from PKG" << std::endl;
            LocalFree(argv);
            return 1;
        }
        LocalFree(argv);
        return 0;
    }

    if (argc >= 4 && wcscmp(argv[1], L"--render-pkg") == 0)
    {
        try { winrt::init_apartment(); } catch (...) { return -1; }
        std::wstring pkgPath = argv[2];
        std::wstring outPath = argv[3];

        sts::we::WallpaperAlignmentSettings alignment{};
        if (argc >= 7)
        {
            alignment.custom = true;
            alignment.mode = _wtoi(argv[4]);
            alignment.x = _wtof(argv[5]);
            alignment.y = _wtof(argv[6]);
        }
        if (argc >= 8)
        {
            alignment.z = _wtof(argv[7]);
        }

        std::wcout << L"=== Solune Scene Render ===" << std::endl;
        std::wcout << L"PKG:       " << pkgPath << std::endl;
        std::wcout << L"Output:    " << outPath << std::endl;
        if (alignment.custom)
            std::wcout << L"Alignment: mode=" << alignment.mode
                       << L" x=" << alignment.x << L" y=" << alignment.y
                       << L" z=" << alignment.z << std::endl;

        double roiAvg = 0, roiDark = 0, globalAvg = 0, globalDark = 0;
        std::wstring decodeSummary;
        if (sts::we::RenderSceneCompositeToPng(pkgPath, outPath, roiAvg, roiDark, globalAvg, globalDark, decodeSummary, alignment))
        {
            std::wcout << L"\nRendered and saved." << std::endl;
            std::wcout << L"  Decode summary: " << decodeSummary << std::endl;
            std::wcout << L"  Global avg:     " << globalAvg << std::endl;
            std::wcout << L"  Global dark:    " << globalDark << std::endl;
            if (alignment.custom)
            {
                std::wcout << L"  ROI avg:        " << roiAvg << std::endl;
                std::wcout << L"  ROI dark:       " << roiDark << std::endl;
            }
            const wchar_t* themeLabel = (roiAvg < 0.35) ? L"Dark" : L"Light";
            std::wcout << L"  Classified as:  " << themeLabel
                       << L" (avg " << roiAvg << L" vs 0.35 threshold)" << std::endl;
        }
        else
        {
            std::wcerr << L"ERROR: Failed to render scene from PKG" << std::endl;
            LocalFree(argv);
            return 1;
        }
        LocalFree(argv);
        return 0;
    }

    LocalFree(argv);

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
