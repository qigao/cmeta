# Canonical contract fingerprint（#926）

`<cmeta/fingerprint.h>` 的 `cmeta_contract_fingerprint_*()` 直接消费 canonical type、Struct、enum domain、FunctionAbi、interface 和 Component descriptor，链接 `CMeta::CMeta`。每个查询返回 `cmeta_status`，只在成功时写入 `uint64_t *out`。这是控制面投影，没有 runtime registry、动态分配、对象 callback 或模块 retain/release。

原 `CMETA_ABI_FINGERPRINT_VERSION = 1` builder 与 `cmeta_abi_fingerprint_type()` 的输出保留，包括旧 type query 的名字参与方式。新 projection 使用独立 domain 和 `CMETA_CONTRACT_FINGERPRINT_VERSION = 1`，不隐式升级既有 fingerprint。

## v1 字节契约

算法仍为 FNV-1a 64：初值 `14695981039346656037`，逐字节执行 `(hash XOR byte) * 1099511628211 mod 2^64`。所有数值、count、布尔 presence 和 enum tag 都编码为 **8 字节 unsigned little-endian**。字符串是同格式的字节长度，再跟原字节；不含 NUL，不做 locale、Unicode 或大小写归一化。UTF-8 可用于跨工具链的非 ASCII 标识。

顶层先编码字符串 `cmeta.contract`、算法版本 `1`、projection 版本 `1`、domain。domain 固定为 type=1、struct=2、enum=3、function=4、interface=5、component=6。然后按下表编码；子 row 不重复顶层 envelope。

| Row | 顺序 |
|---|---|
| Identity ATOM | form=0、stable atom ID |
| Identity POINTER / CONST | form=1 / 2、base identity |
| Identity APPLY | form=3、constructor row、arity、每个 argument identity |
| Constructor | stable ID、min arity、max arity、category |
| Type | identity row、native size、alignment、kind、trait flags（NULL 为 0）、pointee presence、存在时递归 Type row |
| Struct | native size、alignment、field count；逐 field 编码 name、offset、size、alignment、Type row、declared-type presence、存在时 declared row |
| Declared generic | constructor row、storage Type row、arity、逐 argument Type row、construction capability presence |
| Enum domain | signedness、width bits、kind、declared mask、item count；逐 item 编码 canonical uint64 bits、symbol |
| FunctionAbi | return carrier、return Type row、effects、properties、result flags、parameter count；逐 parameter 编码 flags、carrier、Type row |
| Interface | method count；逐 method 编码 name、dispatch arity、flags、FunctionAbi row |
| Config Data contract | stable ID、Data kind、storage Type presence、存在时 Type row |
| Component declaration | declaration format version、config presence、存在时 Config Data contract、capability count；逐 capability 编码 role、Interface row |

所有 row 保留 canonical 数组顺序；共享节点按每条引用边重复编码，不 hash 地址或建立节点地址编号。Enum 使用 `cmeta_enum_domain` 的 unsigned bits：signed8 的 -1 编码为 255，unsigned64 最高位不经过 `int64_t`。Aliases 和空 domain 保留 canonical validator 的既有语义。旧 `EnumDesc` 没有 signedness/width，不能猜测为完整 enum ABI。

**排除项：**descriptor/function/callback 地址、padding、源路径、时间戳、build directory，以及 type/struct/interface 的显示名、field `type_name`、constructor display name、enum text、function/parameter name。Field、enum symbol 和 interface method name 是可查询的语义 row，参与指纹。Component stable_id 单独校验，不参与 shape digest；config Data contract 的 stable ID 参与指纹。Traits 消费 capability flags，不描述 callback 的实现算法；construction 只投影已验证的 capability presence，不执行 provider。

完整指纹要求 stable canonical type identity、explicit ABI carrier 和完整反射的 interface methods。缺失 identity、未指定 carrier、legacy ABI-only interface rows、动态 struct offset 返回 `CMETA_TYPE_MISMATCH`。Field 必须具有 canonical type 且 native size/alignment 一致；无效 layout、domain mask、function ownership/effect/property 契约返回 `CMETA_INVALID_ARGUMENT`。没有按显示字符串查找类型或降级猜测 signature。

## 预算、状态与生命周期

调用方显式提供非零 depth、node、累计 row 和累计 string-byte 上限。Depth 不超过 64；默认建议分别为 32、4096、1024、65536。Type、identity、declared generic、Struct、enum domain、FunctionAbi、interface 每次访问消耗一个 node；constructor 是其所属 row 的内联部分。泛型 argument、field、enum item、parameter、method 共用累计 row 预算。参与校验的显示名也消耗 string budget，包含 NUL；扫描在预算耗尽前停止。Cycle、过深图、过多 row 或过长字符串返回 `CMETA_CAPACITY_EXCEEDED`，保持原输出。

Descriptor 是唯一事实源；临时 builder 只拥有本次查询的派生摘要，成功后一次性发布。输入必须是 live、可读的 ABI-compatible descriptor 与数组；预算不能证明任意指针有效。跨 DSO 须先协商 `CMETA_REFLECTION_ABI_VERSION`，调用方保持 Plugin lease；本接口不解析未经验证的文件或网络字节。

兼容性（MED）：新 projection 描述 native layout/signature/trait **形状**，须与单独约定的 stable contract ID、算法版本和 projection 版本一起比较。Struct/interface 显示名不是新的身份源；相同形状可得到相同摘要。64 位 FNV 可能碰撞，也不覆盖 descriptor 尚未表达的 calling convention 或 provider 实现语义；摘要用于诊断与快速拒绝，不能替代 canonical compatibility 校验或安全认证。跨平台 size/alignment 不同时，指纹应不同。Reflection ABI、manifest v1 布局与旧 builder 输出未改变；enum domain kind 只追加值 9，不改变既有 kind 编号。

投影只跟随 descriptor 已表达的边。`TypeDesc` 不包含关联的 Struct fields 或 enum domain 指针，FunctionAbi/field 的 Type row 因而只投影 stable identity、native layout 与 traits；不会搜索额外 registry 来补充内部结构。需要验证关联 record/domain 的完整 schema 时，consumer 必须显式查询对应的 canonical Struct/enum fingerprint，并在其契约中组合这些结果。Plugin 的 contract ID/version 和 lease 仍是原 owner 的事实。

```c
#include <cmeta/fingerprint.h>
cmeta_struct(FingerprintExample, cmeta_field(int, code));
int main(void) {
    const cmeta_fingerprint_limits limits = {CMETA_FINGERPRINT_DEFAULT_DEPTH,
        CMETA_FINGERPRINT_DEFAULT_NODES, CMETA_FINGERPRINT_DEFAULT_ROWS,
        CMETA_FINGERPRINT_DEFAULT_STRING_BYTES};
    uint64_t fingerprint;
    return cmeta_contract_fingerprint_struct(StructMeta(FingerprintExample),
        &limits, &fingerprint) == CMETA_OK ? 0 : 1;
}
```

## 固定向量与验证

`cmeta_fingerprint_test`、`cmeta_fingerprint_cpp_test` 与 `cmeta_fingerprint_dso_test` 使用同一 immutable canonical fixture。Word 是 stable ID `test.word32`、size/alignment=4、INTEGER、无 traits/pointee；Record 只有 offset=0 的 `value` Word 字段。Flags 是 unsigned64 的 `FingerprintLow=1`、`FingerprintHigh=0x8000000000000000`。Function 是 Word→Word，参数 IN、SCALAR carrier、value 契约（effects=0、properties=11、result=0）。Interface 有一个 `read` 方法，返回 Word、无显式参数、同一 value 契约；self dispatch 的既有 `void*` 形状由 interface 协议规定。

| Domain | v1 digest |
|---|---|
| Type | `da23bb506d89ad46` |
| Struct | `61362ebbad0aa96e` |
| Enum | `fdcfb9ac20892503` |
| Function | `6b5430608eacabce` |
| Interface | `c34c77442ac68534` |

这些向量从上述字节语法独立计算，测试不以另一次同函数调用充当期望值。CI 在 GCC、GCC ASan、MSVC、ClangCL、AppleClang 的独立 build 中断言同一向量；C/C++ TU 和链接的 DLL/DSO 具有不同 descriptor 地址。DSO fixture 的加载期覆盖整个测试进程，并在返回 metadata 前协商 Reflection epoch，不声明 unload-race 验证。安装包 C11/C++17 测试实际链接 fingerprint symbol，验证 core 仍独立消费。

Component declaration 与 domain 6 指纹组合见 [COMPONENT_MANIFESTS.md](COMPONENT_MANIFESTS.md)。所有权继续归 Plugin lease；不新增第二个 schema 或 registry。

Salts 3.0.0 的 domain 6 替代 2.x static Plugin declaration；numeric domain 相同不代表旧指纹兼容。迁移及缓存重建见 [Component 迁移指南](COMPONENT_MANIFESTS.md#migration-from-salts-2x)。
