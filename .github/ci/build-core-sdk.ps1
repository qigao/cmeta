param([string]$Preset)
$ErrorActionPreference = 'Stop'
if ($env:SALTS_CORE_REF -cnotmatch '^[0-9a-f]{40}$') { throw 'SALTS_CORE_REF must select an immutable commit' }
$root = [IO.Path]::GetFullPath('build/upstream/core')
$actual = git -C $root rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $actual -cne $env:SALTS_CORE_REF) { throw 'Unexpected Core SDK source' }
$prefix = "$root/build/$Preset/stage"
$env:LLVM_MINGW_TARGET = 'x86_64-w64-mingw32'
$env:MSVC_TARGET_TRIPLET = $env:VCPKG_TARGET_TRIPLET
$env:LLVM_NATIVE_TARGET_TRIPLET = $env:VCPKG_TARGET_TRIPLET
$env:GCC_TARGET_TRIPLET = $env:VCPKG_TARGET_TRIPLET
$env:GCC_C_COMPILER = 'gcc'
$env:GCC_CXX_COMPILER = 'g++'
Push-Location $root
try {
  if ($IsWindows) {
    & ./cmake/WithVs.cmd cmake --preset $Preset -DBUILD_TESTS=OFF "-DCMAKE_INSTALL_PREFIX=$prefix"
  } else {
    cmake --preset $Preset -DBUILD_TESTS=OFF "-DCMAKE_INSTALL_PREFIX=$prefix"
  }
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  if ($IsWindows) { & ./cmake/WithVs.cmd cmake --build --preset $Preset }
  else { cmake --build --preset $Preset }
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  cmake --install "build/$Preset"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally { Pop-Location }
"SALTSCORE_ROOT=$($prefix.Replace('\', '/'))" >> $env:GITHUB_ENV
