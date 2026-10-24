# CMeta crypto fuzz targets

Configure with Clang and `-DSALTS_BUILD_CRYPTO_FUZZERS=ON`. The two libFuzzer
targets exercise the installed, provider-neutral `Salts::Core` ABI:

- `fuzz_cmeta_sm4_gcm` checks arbitrary key/nonce/AAD/message round trips and
  fail-closed authentication with unchanged caller output.
- `fuzz_cmeta_sm2_verify` combines raw bounded DER partitions with a valid
  GM/T verification seed, then mutates the key, identity, message and canonical
  DER signature.

CTest runs each committed corpus for 20,000 inputs under ASan/UBSan. For an
unbounded local campaign, run either executable with its corpus directory and
`-max_len=4096`; preserve any generated `crash-*`, `leak-*` or `timeout-*`
reproducer before minimizing it with libFuzzer.
