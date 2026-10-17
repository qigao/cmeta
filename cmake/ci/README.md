# CI 辅助脚本

`select-ci-scope.ps1` 为 `ci.yml` 选择构建与测试；`verify-installed-consumer.cmake`
仍由 CMeta、Component、CFlow 和 Concurrency 的正式 CTest 调用。

已完成的 Component 候选包、CFlow #1105 消费者复现、CNet #1150 候选包，以及
固定 `2.3.0-rc.15` 的 CFlow/CSTL DSO 实验 workflow 和专用脚本已移除，历史实现
保留在 Git 中。静态 owner 的旧分支专用步骤也已移除；当前 native CI 继续执行
`salts_static_owner_exports_test`、`cflow_executor_dso_test` 及其他正式测试。
旧 Android runtime workflow 遗留的两个无调用脚本已移除，设备测试入口仍为
[`tools/android-test.ps1`](../../tools/android-test.ps1)。

ACE 长期 PR 的候选 SDK、当前源码的 CNet TSan、DataBind ASan、RFC8998 互操作，
以及 NuGet feed 维护仍有独立用途，保留对应 workflow。

## Benchmark 报告

`native-io-benchmarks.yml` 通过 CTest 执行 benchmark 和既有 verifier。
详细 CSV、Markdown、日志保留在 `io-benchmark-*` artifact，保留 30 天；
成功 PR 的原始逐次采样及 syscall trace 仍按既有策略裁剪。Summary 不再追加数据表。

`render-benchmark-verdicts.ps1` 同时输出 `benchmark-verdicts.csv` 和
`benchmark-metrics.json`，图表与文字判定复用同一次计算。配对指标显示中位数与 MAD；
诊断项不作为性能门禁，缺失数据不算通过。

CI 仅上传 benchmark 数据 artifact；图表按需在本地生成，不自动上传图片或发布 PR 图片评论。

本地使用下载的真实 CI evidence 生成预览：

```powershell
gh run download RUN_ID --repo qigao/salts --pattern 'io-benchmark-*' --dir benchmark-inputs
python cmake/ci/render-benchmark-report.py --input benchmark-inputs --output benchmark-report --head HEAD_SHA --run RUN_ID --conclusion success
python -m unittest discover -s cmake/ci/tests
```

将 `RUN_ID`、`HEAD_SHA`、`--conclusion` 替换为实际运行信息；本地 Python 需要 Matplotlib。
`benchmark-metrics.json` 必须来自采用新报告流程的运行，旧 artifact 缺少此文件时标记为 missing。
