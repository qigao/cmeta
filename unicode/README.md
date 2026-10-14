<!-- Migrated from qigao/salts-utils @ c259f4c6fabc8c5d4c9663c44c5cb738519f5c30 (Unicode 17.0.0). -->
# Salts Unicode

`Salts::Unicode` 是一个独立的 C11 库：用 re2c 生成的 UTF-8 DFA 顺序读取 Unicode scalar，
并返回固定 Unicode 17.0.0 版本的 `XID_Start`、`XID_Continue` 与 `White_Space` 属性。
它不包含 Jinja 或 Mustache 语义，也不替代 Salts 的 `tstr` / `vstr` 字符串能力。

`salts_unicode_decimal_value(uint32_t scalar, uint32_t *out_value)` 查询Unicode 17的
十进制数字（Nd），成功返回`SALTS_UNICODE_OK`并写入0–9；非Nd返回`SALTS_UNICODE_NO_MATCH`。
NULL输出、surrogate或超出U+10FFFF返回`SALTS_UNICODE_ERR_INVALID_ARGUMENT`，所有非成功路径
均保持输出不变。接口不分配内存、不访问共享可变状态，可并发调用；不改变已有scalar布局。
例如U+0662得到2，而上标²与汉字〇不是Nd。

实现复用 re2c 4.6 的 Unicode 17 分类表，无另一份数字值映射。
在连续Nd区间中按偏移模10取值，支持相邻多组数字；时间O(L)、空间O(1)，固定数据的L最多50。
官方UnicodeData.txt全码点对照验证770个Nd数字、非数字和非法scalar状态。
数据语义参见[Unicode 17数字章节](https://www.unicode.org/versions/Unicode17.0.0/core-spec/chapter-22/)。

## General_Category P*/S*（Salts #1164）

`salts_unicode_is_punctuation_or_symbol(uint32_t scalar, int *out_result)`
提供 Unicode 17.0.0 的标点与符号分类，覆盖 P*、S*，例如 `<!-- Migrated from qigao/salts-utils @ c259f4c6fabc8c5d4c9663c44c5cb738519f5c30 (Unicode 17.0.0). -->
# Salts Unicode

`Salts::Unicode` 是一个独立的 C11 库：用 re2c 生成的 UTF-8 DFA 顺序读取 Unicode scalar，
并返回固定 Unicode 17.0.0 版本的 `XID_Start`、`XID_Continue` 与 `White_Space` 属性。
它不包含 Jinja 或 Mustache 语义，也不替代 Salts 的 `tstr` / `vstr` 字符串能力。

`salts_unicode_decimal_value(uint32_t scalar, uint32_t *out_value)` 查询Unicode 17的
十进制数字（Nd），成功返回`SALTS_UNICODE_OK`并写入0–9；非Nd返回`SALTS_UNICODE_NO_MATCH`。
NULL输出、surrogate或超出U+10FFFF返回`SALTS_UNICODE_ERR_INVALID_ARGUMENT`，所有非成功路径
均保持输出不变。接口不分配内存、不访问共享可变状态，可并发调用；不改变已有scalar布局。
例如U+0662得到2，而上标²与汉字〇不是Nd。

实现复用 re2c 4.6 的 Unicode 17 分类表，无另一份数字值映射。
在连续Nd区间中按偏移模10取值，支持相邻多组数字；时间O(L)、空间O(1)，固定数据的L最多50。
官方UnicodeData.txt全码点对照验证770个Nd数字、非数字和非法scalar状态。
数据语义参见[Unicode 17数字章节](https://www.unicode.org/versions/Unicode17.0.0/core-spec/chapter-22/)。

、
`£`、`€` 和 Unicode 符号，不将 NBSP/字母误分类。成功返回 0/1，
非法 scalar 或 NULL 输出返回错误，且不改写现有输出。零分配、可重入，
复用仓库已经校验 SHA-256 的 re2c Unicode 17 `unicode_categories.re`，
不引入第二套类别数据、locale 或 ICU。

新增完整 scalar 官方 `UnicodeData.txt` conformance 校验以及正式安装 SDK
C11/C++17 消费测试。**这是新增公开 API 的开发候选**：发布前仍需 ABI
审查、跨平台 SDK 验收与明确版本溯源；不会自动更新已有发布包，也不
授权 Stun 使用未发布 API 或添加本地 Unicode 回退。

## Unicode 17 Default Full Case Folding（Salts #1192）

`salts_unicode_case_fold(input, output, capacity, &bytes)` exposes Unicode 17
Default Full C/F Case Folding distinct from LOWER, UPPER, TITLE, NFC and
Turkic T rules (ẞ → ss, ς → σ, İ → i + combining dot).

The checked-in UnicodeData source is CaseFolding.txt from
`unicode-org/unicodetools/final-17.0-20250910`; generated tables are
immutable and have no runtime fallback. Measure/write uses caller-owned
counted UTF-8, validates first and preserves output on failure.

**This is a joint ABI CI candidate, not a published Salts SDK.** Neither
upstream Draft is merged, and Stun must not consume unshipped API exports.

## 使用

```c
#include <salts_unicode.h>

#include <stdio.h>

int main(void) {
  const char text[] = "用户";
  vstr input = vstr_from_buf(text, sizeof(text) - 1u);
  salts_unicode_scalar scalar;
  size_t cursor = 0u;

  while (salts_unicode_utf8_next(input, &cursor, &scalar) == SALTS_UNICODE_OK) {
    printf("U+%04X at byte %zu, XID_Start=%u\n", (unsigned)scalar.value,
           scalar.byte_offset,
           (scalar.properties & SALTS_UNICODE_PROPERTY_XID_START) != 0u);
  }
  return 0;
}
```

常用的零分配字符语义操作直接接受 `vstr`：

```c
vstr trimmed;
size_t identifier_end;
int is_identifier;

salts_unicode_trim_whitespace(input, &trimmed);
salts_unicode_xid_span(trimmed, 0u, &identifier_end);
salts_unicode_is_xid(trimmed, &is_identifier);
```

`salts_unicode_trim_whitespace` 返回借用的子 view，并验证完整输入；
`salts_unicode_xid_span` 只实现 Unicode XID 规则，不把 `_` 等语言扩展算作起始字符；
`salts_unicode_is_xid` 即使已经发现非 XID 字符，也会继续验证余下 UTF-8。空串不是 XID。
`SALTS_UNICODE_NO_MATCH`、`SALTS_UNICODE_END` 与负错误码可明确区分。

```cmake
find_package(Salts CONFIG REQUIRED)
target_link_libraries(app PRIVATE Salts::Unicode)
```

输入是 caller-owned borrowed `vstr`；返回的子 view 使用期间，源存储不得修改或释放。函数不保留 view、无堆分配、
无全局可变状态；不同线程可并发扫描不同或相同的 immutable input。每次调用最多复制 4 bytes
到固定栈 buffer，以隔离 re2c DFA 的最长 lookahead。单 scalar 查询时间与额外空间均为 O(1)；
trim/XID 整体操作按输入 bytes 为 O(n) 时间、O(1) 额外空间。

成功时 cursor 前进一个 scalar；到达末尾返回 `SALTS_UNICODE_END`。参数错误或非法 UTF-8
返回负错误码，cursor 与 output 均保持不变。支持 embedded NUL；拒绝 overlong、surrogate、
超出 U+10FFFF、非法 continuation 与截断序列。

## 构建依赖与数据版本

Unicode 17 的 build-time re2c 属性事实源固定为上游 re2c 4.6 的
`unicode_properties.re` / `unicode_categories.re`。构建配置使用规范化 LF
后的 SHA-256 验证两个数据文件（分别为 `56b5f16e…2aa21` 和
`3d0a3198…5b0689`）。re2c 工具继续来自最新版构建工具包，不固定软件包
版本；如果未来工具携带 Unicode 18 等不同分类表，将 **fail fast**，
必须显式升级 Unicode 数据、ABI 版本和官方 conformance 测试，绝不静默漂移。


构建需要 re2c 4.6 或更新版本，以及它安装的 `unicode_properties.re`。使用 re2c 4.6
基于 Unicode 17.0.0 生成的数据；不接受随日期变化的 `latest`：

```powershell
cmd /d /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --preset win-release-user'
```

`CMakeUserPresets.json` 的 Windows profile 通过 `RE2C_ROOT` 统一指定 re2c 4.6 安装根，
并从同一根目录查找可执行文件与 Unicode 数据。清理缓存后也能重现配置。其他机器需在 user preset
配置实际工具根，并先进入本机 VS 环境。

数据来源：

- [Unicode 17.0.0](https://www.unicode.org/versions/Unicode17.0.0/)
- [DerivedCoreProperties.txt](https://www.unicode.org/Public/17.0.0/ucd/DerivedCoreProperties.txt)
- [PropList.txt](https://www.unicode.org/Public/17.0.0/ucd/PropList.txt)
- [Unicode Terms of Use](https://www.unicode.org/copyright.html)
- [re2c 4.6 unicode_properties.re](https://github.com/skvadrik/re2c/blob/4.6/include/unicode_properties.re)

升级 Unicode 数据必须显式修改公开 version macros、预期 property-file hash、边界测试和兼容
说明；只替换本地 re2c 数据文件会 fail fast。

## #1088 迁移与发布边界

本节是 [#1088](https://github.com/qigao/salts/issues/1088) 的迁移约束。
M1 的源码迁入不等于已发布 SDK，也不提供 NFC 或 IDNA；这些后续能力见
[#1091](https://github.com/qigao/salts/issues/1091)。各阶段均不是 Salts 2.3 RC 的前置条件。

上游事实源为 SaltsUtils `c259f4c6fabc8c5d4c9663c44c5cb738519f5c30` 的 `unicode/`，
原树为 `6e040ea59aa60e2b02f984ffa6a9399daac28cf9`，包含 56 个文件。
迁移保留公开符号、结构体布局、借用生命周期、原有算法与数据；构建和安装归属改为
`SaltsTargets`。SaltsUtils/Jinja 切换后只消费安装的 `Salts::Unicode`，删除旧实现和导出，
不建立长期 shim 或反向依赖。

| Salts SDK | SaltsUtils SDK | 配对结果 |
| --- | --- | --- |
| 迁移前 | 迁移前 | Unicode 由 SaltsUtils 导出 |
| 迁移后 | 迁移后 | Unicode 仅由 Salts 导出 |
| 迁移后 | 迁移前 | 两个 export set 都定义 `Salts::Unicode`，不能作为支持的组合 |
| 迁移前 | 迁移后 | 缺少必须的 Unicode target，配置失败 |

**HIGH / 推论**：顺序发布两个浮动 latest 包会暴露上述混合组合；缩短发布间隔不能消除
这个窗口。稳定晋级前须确定包分发能否让消费者选择完整的一对版本。若不能，应先安排
受控消费方的维护窗口：暂停新 restore，等待在途 restore 完成，发布完整 SDK 对，刷新
latest 解析并完成跨平台安装测试后再恢复。无法协调的外部消费者仍存在兼容风险，不能
将其描述成原子升级。维护窗口及真正的发布操作需要单独授权，本迁移不改变分发系统。

发布顺序为：两个 PR 的待发布提交完成审查和平台验证；保留不可变候选 SDK 对及其来源；
让 Jinja、安装的 C11/C++ 消费者和 stun/flexUI 验证这一对制品；执行已确定的协调晋级；
最后在全新安装前缀中重新解析 latest 并运行正式消费测试。候选记录应包含两库和外部
消费者的提交、run/attempt、RID、配置、工具链、制品身份及实际测试范围。源码分支
集成 CI 的成功只能证明其记录的构建，不能代替全平台候选制品验收或授权稳定发布。

回滚以 SDK 对为单位。尚未晋级时保留旧发布对；晋级后暂停 restore，按分发系统允许的
方式恢复已验收的完整配对，不能只降级 Salts。使用新安装前缀，避免旧 SaltsUtils 的
Unicode header/library 残留。不得覆写不可变包、添加 EXACT pin、静默 fallback，或以
目标存在检查绕开双所有者冲突。

### 安装消费测试

原有 `test_salts_unicode_installed_sdk` 保留独立 C11/C++17 编译和运行用例。父构建
生成只用于本 build tree 的配置输入，传递实际编译器、配置、运行库、平台参数和已解析
的编译/链接选项；vcpkg 继续使用父工程的 manifest 与工具链。消费工程使用版本化的
`installed-sdk` configure/build/test preset，只从测试指定的 `SALTS_ROOT` 查找 SDK。
它不重新选择 Release、不把第一方包放入 `CMAKE_PREFIX_PATH`，也不通过读取导出文本
证明链接正确。

测试将当前构建按所选配置安装到 build tree 内的专属前缀，避免改写共享安装目录。
Debug/Release 分别使用独立前缀和缓存；CTest 仍执行真正的 C/C++ 用例。CI 的原有
Unicode/完整测试步骤已包含此测试，安装打包步骤不重复创建另一个消费 build。
Android/iOS 仅在构建阶段验证，不将宿主执行结果当作移动端运行证据。

## Unicode 17 NFC 与 IDNA（#1091）

`<salts_unicode_normalize.h>` 提供显式 `salts_unicode_nfc`，版本参数必须为
`SALTS_UNICODE_NFC_17_0_0`。调用者提供 uint32_t workspace 和 UTF-8 output，
无隐藏堆分配；所有区域互不重叠，任意失败保持 output 和成功长度不变。
NFC 支持合法 NUL scalar，实施规范分解、CCC 稳定重排、受阻组合、composition
exclusions 和 Hangul；不会隐式改变现有 scanner/case/property API 行为。

```c
#include <salts_unicode_normalize.h>
uint32_t work[32];
char output[32];
size_t size = 0;
salts_unicode_nfc_status rc = salts_unicode_nfc(
    vstr_from_cstr("e\xcc\x81"), SALTS_UNICODE_NFC_17_0_0,
    work, 32, output, sizeof output, &size);
/* rc == SALTS_UNICODE_NFC_OK: output contains UTF-8 U+00E9, size == 2. */
```

workspace_capacity 按 uint32_t 元素计，output_capacity 按字节计且需要包含末尾 NUL。
每个输入字节预留四个 workspace 元素是充分上界；分配前由调用方检查乘法和预算，
较小 workspace 在实际分解可容纳时同样可用。错误区分参数、版本、UTF-8、workspace、
output capacity、overflow。重排最坏复杂度为最长组合序列长度的平方；应用需限定输入预算。
`salts_unicode_normalization_properties_of` 返回 Unicode 17 CCC 与 General_Category=Mark
判定，包含 CCC=0 的 Mark，不执行规范化。

独立的 [`Salts::IDNA`](../idna/README.md) 复用 NFC、scanner 和 Bidi_Class，提供
固定版本的 UTS #46 nontransitional ToASCII，完整说明 profile、ContextJ/O、A-label、
容量、错误和连接身份约束。它只依赖 Unicode/Core，不向基础 Unicode 引入 DNS、CNet、
TLS、ICU 或 SaltsUtils。公开头文件只包含已实现的接口，不包含占位 API。

正式 `test_salts_unicode_normalize` 运行 Unicode 17 `NormalizationTest.txt` 全部
20,034 条五列 NFC 不变量及 Part 1 未列出合法 scalar 的恒等性；现有安装测试同时
覆盖 C11/C++17 的 NFC/IDNA 导出。数据和生成方法见 [data/README.md](data/README.md)。
平台与 sanitizer 验证以实际运行记录为准，不能将交叉编译当作设备运行结果。

原 SaltsUtils Unicode 所有权迁移的配对发布要求保持不变。当前只是提供可显式调用的
转换库和本地 DNS 集成测试；不自动启用消费应用的转换路径，不修改 CNet 的 ASCII、
尾点或 numeric-IP 接纳语义。DNS、TLS hostname 与 SNI 必须使用同一成功 ASCII 结果。
