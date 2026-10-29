# Canonical source and runtime architecture

The Salts repository implements **only the CMeta/ACE/CFlow layer**.

```text
Applications / services (Flowie, CHttp, TurboDB, TurboAgent, ...)
                 |
Salts 3: CMeta + Plugin + Component/Configurator + ServiceHost + CFlow
                 |
SaltsCore: Core + Platform + Concurrency + Coroutine + NativeIO
           + CNet/SCTP/URI + Unicode/IDNA + ASN1/CSerde/TinyTest + SIMD
                 |
Crypto: one Salts::Crypto STATIC archive containing pinned GmSSL
```

CMeta/ACE owns type metadata, interfaces, resource scopes, reflection,
interceptors, service construction, component activation and plugin leases.
CFlow owns actor, reactive, DAG and state machine semantics. NativeIO owner,
TCP/UDP/KCP/SCTP transport and system I/O belong to **salts-core**;
cryptographic algorithms and TLS/X.509 belong only to **crypto**.

## Invariants

1. Each exported target is **`Salts::Name`** regardless of owning package.
2. Salts imports native owners from the installed SaltsCore package. Stateful
   Core/Platform/Concurrency/Coroutine/NativeIO owners remain shared. SIMD is
   a static module containing numerical execution and immutable lane shapes;
   Salts does not compile a second native implementation.
3. No package aliases, 2.x fallback build, retained native implementation
   headers or compatibility shims.
4. No raw network TLS parser, platform thread implementation, FS/Process
   syscall owner, native SCTP or GmSSL sources in Salts.
5. No CMeta/Reflection dependency in SaltsCore or Crypto.
6. Core and Crypto feature/ABI additions require their own cross-platform
   source and installed SDK conformance before dependent production cutover.
7. Debug/tracing/metrics/adapters implemented with CMeta belong to Salts;
   underlying native resources and handles are owned by Core.

See public headers in `cmeta/`, `component-runtime/`, `plugin/`, and
`cflow/` for concrete ABI and lifecycle contracts. The canonical installed
SDK CI exercises genuine owner and Provider DSO unload paths.

## SIMD reflection and execution

`Salts::SIMD` is the required installed **static** module from SaltsCore.
`Salts::CMeta` links it publicly so installed C11/C++17 consumers receive the
native header and archive through the same package graph. SIMDe remains a
private implementation dependency of SaltsCore. The old Salts `simd/` source
snapshot is not added to the build graph or exported as another target.

`cmeta_vector_desc` contains a CMeta `type` and a borrowed `native_shape`
pointer to `salts_simd_desc`. CMeta owns the fourteen reflection identities,
names, traits and lookup operations. SaltsCore owns carrier storage, lane
kind/width/count/mask metadata and all numerical execution. Keeping copied
lane fields in CMeta would create two metadata authorities, so callers now
access numerical fields through `native_shape`. There is no second lane enum,
carrier structure, compatibility descriptor or executable provider.

This changes the reflected descriptor layout. Consumers must rebuild and
pass the native shape explicitly; old descriptors must never be cast or
copied into native descriptor storage. The fourteen stable reflection IDs,
display names and storage size/alignment remain unchanged. Type lookup uses
`cmeta_type_equal`, and execution uses shape values rather than descriptor
addresses. Static linkage can place equivalent descriptors in different DSOs.

Both pointers are borrowed. Their defining module must outlive their use;
copying a reflection descriptor does not retain a plugin or extend a type's
lifetime. A consumer retaining numerical metadata beyond an owning module's
lifetime must copy the native shape value while its borrow remains valid.
The SIMD executor allocates nothing and retains no caller pointers.

The installed reflection consumers exercise every shape, semantic type
identity with distinct descriptor addresses, original builtin lookup and
mask/vector classification, invalid inputs and real numeric execution. This
evidence is separate from final cross-platform and TurboWasm admission. The
frozen SaltsCore 2,975-file / 610-function inventory and its 32-function SIMD
supplement are not automatically marked complete by this integration.
