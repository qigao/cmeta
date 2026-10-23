# Verify that ASN.1 source/header/test changes select native and mobile builds.
# Use synthetic commits and a temporary index; never mutate the current branch.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (& git rev-parse --show-toplevel).Trim()
if ($LASTEXITCODE -ne 0 -or -not $root) { throw 'Missing Salts Git worktree' }
$base = (& git rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $base -notmatch '^[0-9a-f]{40}$') {
  throw 'Missing exact checked-out source SHA'
}
$selector = Join-Path $root 'cmake/ci/select-ci-scope.ps1'
$cases = @(
  'asn1/src/asn1.c',
  'asn1/include/salts/asn1.h',
  'asn1/tests/asn1_reader_test.c',
  'asn1/tests/asn1_value_test.c'
)
$hosts = @(
  'linux-release', 'linux-arm64-release', 'linux-clang-release',
  'windows-release', 'macos-release', 'macos-clang-release',
  'android-arm64-v8a-release', 'ios-arm64-release'
)
$previousIndex = $env:GIT_INDEX_FILE

function Assert-Git([string]$operation) {
  if ($LASTEXITCODE -ne 0) { throw "Git $operation failed with exit code $LASTEXITCODE" }
}

try {
  for ($i = 0; $i -lt $cases.Count; ++$i) {
    $path = $cases[$i]
    $temporaryIndex = Join-Path ([System.IO.Path]::GetTempPath()) "salts-asn1-scope-$PID-$i.index"
    Remove-Item -LiteralPath $temporaryIndex -Force -ErrorAction SilentlyContinue
    $env:GIT_INDEX_FILE = $temporaryIndex
    & git read-tree $base
    Assert-Git 'read-tree'
    $blob = ("synthetic ASN.1 selector probe for $path" | & git hash-object -w --stdin).Trim()
    Assert-Git 'hash-object'
    & git update-index --add --cacheinfo "100644,$blob,$path"
    Assert-Git 'update-index'
    $tree = (& git write-tree).Trim()
    Assert-Git 'write-tree'
    $head = ("Synthetic ASN.1 selector probe: $path" |
      & git -c 'user.name=Salts CI Scope Probe' -c 'user.email=scope-probe@example.invalid' commit-tree $tree -p $base).Trim()
    Assert-Git 'commit-tree'
    if ($head -notmatch '^[0-9a-f]{40}$') { throw "Invalid synthetic commit for $path" }

    Remove-Item Env:GIT_INDEX_FILE -ErrorAction SilentlyContinue
    $diff = @(& git diff --name-only --no-renames "$base...$head" --)
    Assert-Git 'diff'
    if ($diff.Count -ne 1 -or $diff[0] -ne $path) {
      throw "Synthetic diff was not exactly $path"
    }

    Push-Location $root
    try {
      $result = @(& $selector -EventName pull_request -BaseRef $base -HeadRef $head -HeadBranch 'test/asn1-selector-probe' -PrepareRelease $false -AceMatrix $false -AceSan $false -UnifiedSan $false)
    } finally {
      Pop-Location
    }
    if ($result.Count -ne 2) { throw "Expected scope JSON and matrix JSON for $path" }
    $checks = $result[0] | ConvertFrom-Json
    $matrix = $result[1] | ConvertFrom-Json
    if ([string]$checks.native -ne 'true' -or [string]$checks.mobile -ne 'true') {
      throw "$path did not select native and mobile build scopes"
    }
    if ([string]$checks.work -ne 'false' -or [string]$checks.lean -ne 'false') {
      throw "$path unnecessarily selected benchmark or formal work"
    }

    $sanitizer = @($matrix.include | Where-Object { $_.id -eq 'linux-ace-asan-ubsan' })
    if ($sanitizer.Count -ne 1 -or [string]$sanitizer[0].sanitizer -ne 'asan-ubsan') {
      throw "$path did not select exactly one Linux ASan/UBSan profile"
    }
    if ([string]$sanitizer[0].package -ne 'false' -or [string]$sanitizer[0].benchmarks -ne 'OFF') {
      throw "$path unexpectedly enabled package or benchmark work in the sanitizer profile"
    }
    if (@($matrix.include | Where-Object { $_.id -eq 'linux-ace-tsan' }).Count -ne 0) {
      throw "$path must not select TSan for ASN.1 parser changes"
    }

    foreach ($id in $hosts) {
      $entry = @($matrix.include | Where-Object { $_.id -eq $id })
      if ($entry.Count -ne 1) { throw "$path did not select exactly one $id profile" }
      if ([string]$entry[0].benchmarks -ne 'OFF' -or [string]$entry[0].package -ne 'false') {
        throw "$path unexpectedly selected $id benchmarks or package publication"
      }
      if ($id -in @('linux-release','linux-clang-release','windows-release','macos-release','macos-clang-release')) {
        if ([string]$entry[0].native -ne 'true') { throw "$path omitted native qualification on $id" }
      }
      if ($id -in @('android-arm64-v8a-release','ios-arm64-release')) {
        if ([string]$entry[0].artifact -ne 'true') { throw "$path omitted mobile SDK build on $id" }
      }
      if ($id -eq 'linux-arm64-release' -and [string]$entry[0].armcontracts -ne 'true') {
        throw "$path omitted portable ARM64 contracts"
      }
    }
    Write-Output "PASS ASN.1 scope $path => native + mobile builds, no benchmarks or package publication"
    Remove-Item -LiteralPath $temporaryIndex -Force -ErrorAction SilentlyContinue
  }
} finally {
  if ([string]::IsNullOrEmpty($previousIndex)) {
    Remove-Item Env:GIT_INDEX_FILE -ErrorAction SilentlyContinue
  } else {
    $env:GIT_INDEX_FILE = $previousIndex
  }
}
