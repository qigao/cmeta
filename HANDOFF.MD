# CMeta 构建与依赖交接

## 2026-11-14：CMeta 0.1.7 候选升级至 SaltsBase 0.1.8

- **授权与范围**：用户明确要求升级 CMeta 至 Base 0.1.8。工作分支为
  `upgrade/base-0.1.8-20261114`，起点为 `f8aa79ea`；原工作区干净。
  本轮更改 CMeta 自身的源码版本和依赖选择，不改已发布 0.1.6 的安装配置或资产。
- **实现**：根项目及 vcpkg manifest 版本改为 0.1.7；`.env.example` 和忽略的
  本机 `.env` 选择 Base 0.1.8、vcpkg-cache 0.1.3。正式测试使用匹配的已发布
  CNet 0.1.12，必需查找从 `SaltsCore` 改为 `SaltsCNet`，沿用 `SALTSCORE_ROOT`。
  测试只链接共享 `Salts::CNet`，不查找未使用的静态 Crypto/GmSSL。
- **兼容性**：公开头文件、生产源码、ABI epoch/布局、所有权及错误语义未改。
  API 兼容性下限仍为 Base 0.1.4；本次选用的精确 SDK 为 0.1.8。
  生成和安装的 `CMetaConfig.cmake` 均要求 `SaltsBase 0.1.8 EXACT`，
  安装版本为 0.1.7，既有 `SameMinorVersion` 契约保持。
- **实际验证（Windows MSVC x64）**：加载本机 `.env` 的相关构建字段后执行：
  `./cmake/WithVs.cmd cmake --fresh --preset ci-msvc-release-user`，
  `./cmake/WithVs.cmd cmake --build --preset ci-msvc-release-user --parallel 3`，
  `./cmake/WithVs.cmd ctest --preset ci-msvc-release-user --output-on-failure --no-tests=error`，
  `./cmake/WithVs.cmd cmake --build --preset install-ci-msvc-release-user`。
  configure、完整 396 个 build steps、120/120 项正式 CTest 和安装全部通过。
  CTest 实测总时间 14.62 秒，未据此声明性能收益。
  日志及 JUnit 副本在忽略的 `build/base-0.1.8-upgrade/`。
  安装根为 `build/ci-msvc-release-user/stage`。
- **静态检查**：现有 workflow 的 actionlint、变更范围的 `git diff --check`
  通过；24 个 presets、CMakeOptions 和公共 CMake helper 的 SHA-256 不变。
  Base 白盒头文件来自干净的 `salts-base` v0.1.8 checkout，与已恢复 SDK 一致。
- **CI 与发布限制（MED）**：仅验证本机 MSVC x64；Linux/macOS/MinGW/Android、
  sanitizer 和十平台生产 CI 未运行。尚未提交、推送或发布 0.1.7。
  CI 变量需与合入同步设为 Base 0.1.8、CNet 0.1.12、
  `SALTS_CORE_REF=a29cea5465b5423e512cb63a17d5e76b84f2fdad`，cache 仍为 0.1.3；
  本轮未修改远端变量，以免先于源码变更影响其他分支。
- **Toad 边界**：仍遵守只消费已发布 SDK 的要求，未将其 `.env` 指向本地未发布
  0.1.7 候选。CHttp 0.1.0 的生产 CMeta 为 0.1.6；Toad 的入站检查按 CMeta
  已有 SameMinorVersion 允许明确选择同 minor 的新 patch，同时继续强制
  CMeta 发布清单与 Base 精确一致。真正切换及完整验证需等待 0.1.7 发布，
  并修复 Toad 当前冻结 presets 中已记录的无效引用。
