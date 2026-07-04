# Solune

Windows 服务，根据当地日出日落时间自动切换系统亮/暗主题，并联动 Wallpaper Engine 播放列表。

## 工作方式

```
开机自启 (Windows Service)
  ├─ 读取 solune.json（播放列表名 + IP 定位缓存）
  ├─ 天文算法计算当天日出/日落时间
  └─ 每分钟检查 → 日出 → Light + WE 亮色播放列表
                → 日落 → Dark + WE 暗色播放列表
```

## 安装

```powershell
# 1. 先跑一次控制台模式（获取定位 + 缓存）
.\Solune.exe

# 2. 注册为 Windows 服务（需管理员）
.\Solune.exe --install
```

## 命令

| 命令 | 说明 |
|---|---|
| `Solune.exe` | 控制台模式（调试用） |
| `Solune.exe --install` | 注册 Windows 服务并立即启动（需管理员） |
| `Solune.exe --uninstall` | 停止并删除 Windows 服务（需管理员） |

## 配置

exe 同目录下的 `solune.json`，首次运行自动生成：

```json
{
    "light_playlist": "white_auto",
    "dark_playlist": "black_auto",
    "location": { "lat": 31.23, "lng": 121.47 }
}
```

播放列表名称改成你在 Wallpaper Engine 里建好的即可。`location` 由程序自动获取（通过 IP 定位），无需手动填写。

## 定位策略

1. 读 `solune.json` 缓存
2. 无缓存 → HTTP 请求 `ip-api.com`（备用 `api.ip.sb`）
3. 网络不通 → 用系统时间 7:00–19:00 兜底

## 依赖

- Windows 10+
- Visual Studio 2022 (C++17, WinRT)
- 静态链接 CRT，单文件部署，无运行时依赖
