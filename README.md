# BlankQuick

Windows 桌面代理客户端，使用 C++20、Qt Quick 6.12 和 Mihomo。它把订阅转换成统一节点和运行配置，再通过受控的内核会话提供代理、TUN、流量和日志能力。

当前界面仍是空白 Mica 窗口，尚未接入后端操作；后端以库和 Windows 服务的形式提供能力。理解项目建议从应用层的 `RuntimeSession` 和配置准备流程开始，再沿调用关系进入数据层与系统适配层。

- [系统作用、阅读顺序、数据流与架构边界](docs/architecture.md)
- [Mihomo 源码提取与生成流程](docs/mihomo-sync.md)

## 构建与验证

需要 Windows x64、MSVC 19.44+、Qt 6.12 MSVC x64、CMake 3.30+、Ninja、PowerShell 7、Python 3.10+ 和 clang-format。`QT_ROOT` 默认是 `C:/Qt/6.12.0/msvc2022_64`。

```powershell
python tools/sync_mihomo.py check --offline
pwsh -NoProfile -File tools/project.ps1 build
pwsh -NoProfile -File tools/project.ps1 test
pwsh -NoProfile -File tools/project.ps1 run
```

`build` 保留完整质量门禁：生成结果、依赖边界、CTest、格式、clang-tidy、Clazy、Cppcheck、qmllint。Windows 原生 Clazy 可通过 `tools/setup-clazy.ps1` 准备，或设置 `CLAZY_EXECUTABLE`。

单独编译、执行回归测试而不运行所有静态分析器：

```powershell
. ./tools/project.ps1 configure
cmake --build out/build/debug --target blank_quick blank_quick_service system_proxy_guard kernel_signal subscription_fetcher test_binaries
ctest --test-dir out/build/debug --output-on-failure
```

构建目录被其他任务占用时，所有入口都可以指定独立目录：

```powershell
pwsh -NoProfile -File tools/project.ps1 build -BuildDirectory out/build/local-debug
pwsh -NoProfile -File tools/project.ps1 test -BuildDirectory out/build/local-debug
```

`test_binaries` 构建三个 C++ 回归程序。CTest 同时验证纯数据生成、缓存激活、脚本与遥测调度、Python 工具、离线生成结果和当前构建依赖图；Python 回归还会故意建立越界依赖，确认 CMake 在配置阶段拒绝它们。

代码入口分为 `src/domain`（计算）、`src/platform`（系统操作）、`src/application`（总调度）、`src/services`（提权执行）和 `src/app` / `qml`（交互）。依赖白名单在 `tools/architecture.json`；检查由 CMake 配置阶段及默认质量目标共同执行。

不要修改生成的 `.inc`、`generated/mihomo/`，也不要把完整上游仓库提交进项目。源码同步和离线重建见 [提取文档](docs/mihomo-sync.md)。

## SAST Tools

[PVS-Studio](https://pvs-studio.com/en/pvs-studio/?utm_source=website&utm_medium=github&utm_campaign=open_source) - static code analyzer for Enterprise (C, C++, C#, Go, and Java) and Web (JS and TS) development.

