# Mihomo 源码契约与 Python 生成链

## 版本与输入

2026-11-04 核对 [官方最新稳定发布](https://github.com/MetaCubeX/mihomo/releases/latest)，结果为 [v1.19.33](https://github.com/MetaCubeX/mihomo/releases/tag/v1.19.33)，提交 `3f27ef39f8c96bbfbc8b0b9bb10f5c9ef60149d8`。没有使用 main、alpha 或 prerelease。

`tools/mihomo/upstream.lock.json` 锁定仓库、稳定标签和实际提交。`sync --latest` 通过 GitHub 官方 latest API 检查 draft/prerelease，并解析标签；日常生成和检查使用锁定提交。完整源码位于被忽略的 `out/upstream/`，也可显式提供一个干净的 checkout。不同提交或包含未跟踪/未提交修改的 checkout 会被拒绝。

## 统一入口

```powershell
# 获取锁定版本并从源码提取、重建全部产物
python tools/sync_mihomo.py sync

# 明确更新到官方最新稳定版，提取前先检查现有模型映射
python tools/sync_mihomo.py sync --latest

# 使用已经准备的干净源码，避免重复下载
python tools/sync_mihomo.py sync --source out/upstream/mihomo
python tools/sync_mihomo.py check --source out/upstream/mihomo

# 从已入库的源码事实重建 C++，不访问网络或源码 checkout
python tools/sync_mihomo.py generate

# 构建/CI 默认检查：已入库事实、映射与生成结果的一致性
python tools/sync_mihomo.py check --offline
```

`check --offline` 验证的是已提交快照与代码的一致性；`check --source ...` 还会重新解析锁定源码核实快照。正常构建不会查询 latest 或偷偷升级依赖。

## 提取哪些内容

| Python 模块 | 源码事实 / 产物 |
| --- | --- |
| `go_source.py` | Go 配置声明子集读取：结构体、嵌入、指针、本地类型别名、导入别名、数组/映射、proxy/obfs/yaml 标签、位置与默认值表达式 |
| `extract.py` | `adapter/parser.go` 的出站分派和通用 smux，Shadowsocks 插件分派，具体 Option 字段，以及 `config.RawConfig` 递归配置字段与 `DefaultRawConfig` |
| `bindings.py` | 从审核后的模型映射生成 URI/文档的节点叶字段绑定 |
| `exporter.py` | 生成全部出站协议、插件、嵌套对象/数组导出，以及运行配置输入 schema |
| `cache.py` | 从本项目 C++ 模型生成完整缓存反射，包括所有字段、枚举与 variant 分支；这是客户端持久化协议，不是假称来源于 Go 的数据 |
| `coverage.py` | 为每个 mapped 模型路径生成 C++20 编译检查，验证协议分派名称和凭证变体 |

`generated/mihomo/` 提交可重现的事实快照、覆盖率清单和字段路径编译检查；生成的 `.inc` 存在对应功能切片内。模型中的内核版本、提交和协议名称分派也通过生成文件同步，订阅默认 User-Agent 引用同一版本。

提取器只解析声明，不执行上游 Go 程序、不运行 `go generate`，也不把默认值表达式里的函数调用伪造为 Python 常量。`raw-defaults.json` 原样记录规范化后的 Go 表达式。源码来源清单的哈希对应 UTF-8/LF 规范化内容，使不同 checkout 的换行方式不影响结果。

## 源码事实与产品决策

`tools/mihomo/model_mapping.json` 是显式产品映射策略：Go 字段类型、解码器必填策略、C++ 路径，或排除原因。模型采用标准 C++ 的不可变所有权、optional、variant 和时间单位，结构不与 Go 一对一同构，因此不会在升级时根据相似字段名猜测密码、时间单位、传输变体等语义。

首次迁移通过现有 C++ 导出器恢复字段路径与容器/变体关系，并逐项对照锁定源码，形成已入库的策略。之后运行生成器不再依赖旧 `.inc` 来推断映射，也不依赖相邻项目或没有入库的 docs 文件。仍由客户端手写的统一节点类型是产品模型，不是上游结构体的机械副本。

当前核对范围为 28 种出站协议、7 种 Shadowsocks 插件、4 种 Snell 混淆上下文、1455 个上下文字段和 207 个运行配置字段。`excluded` 项明确记录原因，包括无效的 TCP 控制、虚拟路由适配器、旧字段别名和现有模型未实现的选项；它们不被默认为已支持。

遇到新增/删除协议或字段、类型变化、解码器必填策略变化、插件分派变化、无法确定的构建条件声明或递归配置，流程会失败。先复核新源码，更新 C++ 模型和映射，再运行 sync。不会自动添加含糊的映射来使检查通过。全部事实和生成结果先在内存中准备，解析/映射/渲染失败时不发布任何产物。

## 二进制依赖

`src/services/kernel/dependencies.lock.json` 继续锁定实际打包的 Mihomo/Wintun 二进制摘要；它与源码契约锁分开，下载来源和文件类型不同。此次最新稳定版与原二进制锁一致，因此保留已有摘要。今后升级源版本时，还需核对相应二进制及其摘要后打包。`package_backend.ps1` 读取新路径，测试打包只包含实际存在的 `profile_activation_test`。

第三方 JSON/YAML 源码仍由其各自 `PROVENANCE` 和 LICENSE 记录来源。`src/services/kernel/provenance.json` 保存本地旧后端迁入时的历史哈希，已更新目标路径；它不冒充重构后文件的实时摘要。
