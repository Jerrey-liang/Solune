#pragma once

#include <string>
#include <windows.h>

namespace sts
{
    enum class Theme
    {
        Light,
        Dark
    };

    class App
    {
    public:
        int run();
        Theme getSystemTheme();
        std::wstring detectWallpaperEnginePath();

    private:
        bool acquireSingleInstance();
        void releaseSingleInstance();
        void ensureAutoRun();

        void applyTheme(Theme targetTheme, bool switchWallpaper = true);
        void loop();

    private:
        HANDLE mutex_ = nullptr;
        std::wstring weExePath_;
        std::wstring configPathW_;
        FILETIME lastConfigWriteTime_ = { 0 };
    };
}
