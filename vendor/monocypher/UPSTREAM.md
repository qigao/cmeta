# Vendored Monocypher

- Base and optional Ed25519 sources: Monocypher 4.0.3, tag commit
  `ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f`.
- Source: https://github.com/LoupVaillant/Monocypher
- License: BSD-2-Clause OR CC0-1.0; notices remain embedded in every source.

The base source retains the repository's two existing equivalent Argon2
narrowing-expression spellings. The optional source provides RFC 8032 Ed25519
and calls the matching 4.0.3 base implementation's internal EdDSA primitives.
Both sources therefore include the upstream 4.0.3 signature timing-leak fix.

CNet and Core compile independent, hidden, module-specific namespaces so
generic third-party symbols never cross a DSO.
