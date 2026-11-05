# Mihomo 契约的来源与生成

项目不包含完整 Mihomo 仓库。`tools/mihomo/upstream.lock.json` 固定源码版本与校验身份，`src/services/kernel/dependencies.lock.json` 固定发布二进制和 Wintun 的版本及哈希。源码字段提取和运行时加载是两条不同的链，不能只更新其中一条就宣称内核升级完成。

## 哪些文件可以编辑

- `src/domain/nodes/include/blank/nodes/unified_node.hpp`：手写的强类型统一模型。
- `tools/mihomo/model_mapping.json`：经过审查的上游字段与模型字段映射，包括未支持字段的原因。
- `tools/mihomo/*.py`：提取、覆盖统计、输入绑定、缓存和导出的生成器。
- `generated/mihomo/*.json`、`generated/mihomo/model_contract.cpp` 和各模块生成的 `.inc`：生成结果，不能手动补改。

生成文件所在位置：

| 结果 | 位置 |
| --- | --- |
| 内核版本、协议名称 | `src/domain/nodes/include/blank/nodes/*.inc` |
| 输入字段绑定 | `src/domain/nodes/node_input_bindings.inc` |
| 缓存字段反射 | `src/domain/profiles/profile_codec_fields.inc` |
| Mihomo 导出和配置模式 | `src/domain/runtime_config/mihomo_export_fields.inc`、`runtime_schema.inc` |
| 字段清单、默认值、来源、覆盖统计和编译契约 | `generated/mihomo` |

另外一些 `.inc` 是人工维护的私有实现拆分；是否生成以文件开头的声明及生成器输出清单为准。

## 离线验证

```powershell
python tools/sync_mihomo.py check --offline
```

该路径只读取项目中固定的字段快照和映射，验证第三方依赖哈希并在内存中重新生成结果进行比较。它不访问网络、不检查上游工作目录，也不调用 Git。默认质量检查和 CTest 使用这条路径。

需要按已提交的字段快照重新生成时：

```powershell
python tools/sync_mihomo.py generate
```

生成器先在内存中完成所有结果，再发布文件，避免映射错误留下半套结果。内容未变化时不改写文件，避免无意义的重编译。缓存格式及上游字段由 `model_contract.cpp` 一并进行编译验证。

## 主动同步上游

以下是维护流程说明，日常重构不需要执行。涉及 Git 的同步必须符合当前任务的限制；禁止查看 Git 信息的任务只使用离线验证。

`sync` 会校验指定的上游工作目录；`sync --latest` 还会查询最新稳定发布并下载所需源码。它们会使用网络和 Git，不能用来替代离线检查。同步后应审查新增字段、修改映射、重新生成，再运行构建和回归。完整上游源码应位于 `out/upstream` 或其他外部目录，不应混入 `src`。

第三方实现的来源和许可证位于 `third_party` 内；服务发布依赖的说明、许可证要求和来源记录位于 `src/services/kernel`。发布打包使用 `tools/package_backend.ps1` 校验固定的二进制哈希，并将许可证一起放入结果。
