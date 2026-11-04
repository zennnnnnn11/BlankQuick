# BlankQuick

Windows 桌面代理客户端，使用 C++20、Qt Quick 6.12 和 Mihomo。界面目前仍是空白窗口；订阅、配置和运行后端以独立业务切片组织。

- [架构、业务流程和代码关系](docs/architecture.md)
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
cmake --build out/build/debug --target blank_quick blank_quick_service system_proxy_guard kernel_signal subscription_fetcher profile_activation_test
ctest --test-dir out/build/debug --output-on-failure
```

不要修改生成的 `.inc`、`generated/mihomo/`，也不要把完整上游仓库提交进项目。源码同步和离线重建见 [提取文档](docs/mihomo-sync.md)。
