# Probe the REAL selector with unreferenced synthetic commits.
# No checkout, branch mutation, merge, SDK publication or test relaxation.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (& git rev-parse --show-toplevel).Trim()
if ($LASTEXITCODE -ne 0 -or -not $root) { throw 'Missing Salts Git worktree' }
$base = (& git rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $base -notmatch '^[0-9a-f]{40}$') {
  throw 'Missing exact checked-out source SHA'
}
$selector = Join-Path $root 'cmake/ci/select-ci-scope.ps1'
# Selection alone is insufficient: actual host execution must cover all
# CFlow test binaries, not just an unrelated projected subset.
$nativeBuild = Get-Content (Join-Path $root '.github/workflows/native-build.yml') -Raw
$fullCflowFilter = [regex]::Escape('cflow_.*_test$cases = @(
  'cflow/tests/cflow_reactive_test.c',
  'cflow/tests/cflow_actor_test.c'
)
$hosts = @(
  'linux-release',
  'linux-clang-release',
  'windows-release',
  'macos-release',
  'macos-clang-release'
)
$previousIndex = $env:GIT_INDEX_FILE
$previousOutput = $env:GITHUB_OUTPUT
$previousSummary = $env:GITHUB_STEP_SUMMARY

function Assert-Git([string]$operation) {
  if ($LASTEXITCODE -ne 0) {
    throw "Git $operation failed with exit code $LASTEXITCODE"
  }
}

try {
  Remove-Item Env:GITHUB_OUTPUT -ErrorAction SilentlyContinue
  Remove-Item Env:GITHUB_STEP_SUMMARY -ErrorAction SilentlyContinue

  for ($i = 0; $i -lt $cases.Count; ++$i) {
    $path = $cases[$i]
    $temporaryIndex = Join-Path ([System.IO.Path]::GetTempPath()) "salts-cflow-scope-$PID-$i.index"
    if (Test-Path -LiteralPath $temporaryIndex) {
      Remove-Item -LiteralPath $temporaryIndex -Force
    }
    $env:GIT_INDEX_FILE = $temporaryIndex

    & git read-tree $base
    Assert-Git 'read-tree'
    $blob = ("synthetic CFlow test selector probe for $path" | & git hash-object -w --stdin).Trim()
    Assert-Git 'hash-object'
    & git update-index --add --cacheinfo "100644,$blob,$path"
    Assert-Git 'update-index'
    $tree = (& git write-tree).Trim()
    Assert-Git 'write-tree'
    $head = ("Synthetic tests-only selector probe: $path" |
      & git -c 'user.name=Salts CI Scope Probe' -c 'user.email=scope-probe@example.invalid' commit-tree $tree -p $base).Trim()
    Assert-Git 'commit-tree'
    if ($head -notmatch '^[0-9a-f]{40}$') { throw "Invalid synthetic commit for $path" }

    Remove-Item Env:GIT_INDEX_FILE -ErrorAction SilentlyContinue
    $diff = @(& git diff --name-only --no-renames "$base...$head" --)
    Assert-Git 'diff'
    if ($diff.Count -ne 1 -or $diff[0] -ne $path) {
      throw "Synthetic diff was not tests-only: $($diff -join ',')"
    }

    Push-Location $root
    try {
      $result = @(& $selector -EventName pull_request -BaseRef $base -HeadRef $head -HeadBranch 'test/cflow-selector-probe' -PrepareRelease $false -AceMatrix $false -AceSan $false -UnifiedSan $false)
    } finally {
      Pop-Location
    }
    if ($result.Count -ne 2) {
      throw "Expected scope JSON and matrix JSON for $path"
    }
    $checks = $result[0] | ConvertFrom-Json
    $matrix = $result[1] | ConvertFrom-Json
    if ([string]$checks.execution -ne 'true') {
      throw "$path failed to select actual execution CTests"
    }
    if ([string]$checks.work -ne 'false' -or [string]$checks.lean -ne 'false') {
      throw "$path unnecessarily selected benchmark or formal work"
    }
    foreach ($id in $hosts) {
      $entry = @($matrix.include | Where-Object { $_.id -eq $id })
      if ($entry.Count -ne 1) { throw "$path did not select exactly one $id host" }
      if ([string]$entry[0].execution -ne 'true') {
        throw "$path selected $id build without actual execution tests"
      }
      if ([string]$entry[0].package -ne 'false' -or [string]$entry[0].benchmarks -ne 'OFF') {
        throw "$path unexpectedly selected $id SDK packaging/benchmarks"
      }
    }
    Write-Output "PASS tests-only $path => Linux/Windows/macOS execution CTests; no pack/benchmark"
    Remove-Item -LiteralPath $temporaryIndex -Force -ErrorAction SilentlyContinue
  }
} finally {
  if ([string]::IsNullOrEmpty($previousIndex)) {
    Remove-Item Env:GIT_INDEX_FILE -ErrorAction SilentlyContinue
  } else {
    $env:GIT_INDEX_FILE = $previousIndex
  }
  if ([string]::IsNullOrEmpty($previousOutput)) {
    Remove-Item Env:GITHUB_OUTPUT -ErrorAction SilentlyContinue
  } else {
    $env:GITHUB_OUTPUT = $previousOutput
  }
  if ([string]::IsNullOrEmpty($previousSummary)) {
    Remove-Item Env:GITHUB_STEP_SUMMARY -ErrorAction SilentlyContinue
  } else {
    $env:GITHUB_STEP_SUMMARY = $previousSummary
  }
}
)
if ([regex]::Matches($nativeBuild, $fullCflowFilter).Count -ne 2) {
  throw 'Both POSIX and Windows execution CTest filters must cover all CFlow tests'
}
$cases = @(
  'cflow/tests/cflow_reactive_test.c',
  'cflow/tests/cflow_actor_test.c'
)
$hosts = @(
  'linux-release',
  'linux-clang-release',
  'windows-release',
  'macos-release',
  'macos-clang-release'
)
$previousIndex = $env:GIT_INDEX_FILE
$previousOutput = $env:GITHUB_OUTPUT
$previousSummary = $env:GITHUB_STEP_SUMMARY

function Assert-Git([string]$operation) {
  if ($LASTEXITCODE -ne 0) {
    throw "Git $operation failed with exit code $LASTEXITCODE"
  }
}

try {
  Remove-Item Env:GITHUB_OUTPUT -ErrorAction SilentlyContinue
  Remove-Item Env:GITHUB_STEP_SUMMARY -ErrorAction SilentlyContinue

  for ($i = 0; $i -lt $cases.Count; ++$i) {
    $path = $cases[$i]
    $temporaryIndex = Join-Path ([System.IO.Path]::GetTempPath()) "salts-cflow-scope-$PID-$i.index"
    if (Test-Path -LiteralPath $temporaryIndex) {
      Remove-Item -LiteralPath $temporaryIndex -Force
    }
    $env:GIT_INDEX_FILE = $temporaryIndex

    & git read-tree $base
    Assert-Git 'read-tree'
    $blob = ("synthetic CFlow test selector probe for $path" | & git hash-object -w --stdin).Trim()
    Assert-Git 'hash-object'
    & git update-index --add --cacheinfo "100644,$blob,$path"
    Assert-Git 'update-index'
    $tree = (& git write-tree).Trim()
    Assert-Git 'write-tree'
    $head = ("Synthetic tests-only selector probe: $path" |
      & git -c 'user.name=Salts CI Scope Probe' -c 'user.email=scope-probe@example.invalid' commit-tree $tree -p $base).Trim()
    Assert-Git 'commit-tree'
    if ($head -notmatch '^[0-9a-f]{40}$') { throw "Invalid synthetic commit for $path" }

    Remove-Item Env:GIT_INDEX_FILE -ErrorAction SilentlyContinue
    $diff = @(& git diff --name-only --no-renames "$base...$head" --)
    Assert-Git 'diff'
    if ($diff.Count -ne 1 -or $diff[0] -ne $path) {
      throw "Synthetic diff was not tests-only: $($diff -join ',')"
    }

    Push-Location $root
    try {
      $result = @(& $selector -EventName pull_request -BaseRef $base -HeadRef $head -HeadBranch 'test/cflow-selector-probe' -PrepareRelease $false -AceMatrix $false -AceSan $false -UnifiedSan $false)
    } finally {
      Pop-Location
    }
    if ($result.Count -ne 2) {
      throw "Expected scope JSON and matrix JSON for $path"
    }
    $checks = $result[0] | ConvertFrom-Json
    $matrix = $result[1] | ConvertFrom-Json
    if ([string]$checks.execution -ne 'true') {
      throw "$path failed to select actual execution CTests"
    }
    if ([string]$checks.work -ne 'false' -or [string]$checks.lean -ne 'false') {
      throw "$path unnecessarily selected benchmark or formal work"
    }
    foreach ($id in $hosts) {
      $entry = @($matrix.include | Where-Object { $_.id -eq $id })
      if ($entry.Count -ne 1) { throw "$path did not select exactly one $id host" }
      if ([string]$entry[0].execution -ne 'true') {
        throw "$path selected $id build without actual execution tests"
      }
      if ([string]$entry[0].package -ne 'false' -or [string]$entry[0].benchmarks -ne 'OFF') {
        throw "$path unexpectedly selected $id SDK packaging/benchmarks"
      }
    }
    Write-Output "PASS tests-only $path => Linux/Windows/macOS execution CTests; no pack/benchmark"
    Remove-Item -LiteralPath $temporaryIndex -Force -ErrorAction SilentlyContinue
  }
} finally {
  if ([string]::IsNullOrEmpty($previousIndex)) {
    Remove-Item Env:GIT_INDEX_FILE -ErrorAction SilentlyContinue
  } else {
    $env:GIT_INDEX_FILE = $previousIndex
  }
  if ([string]::IsNullOrEmpty($previousOutput)) {
    Remove-Item Env:GITHUB_OUTPUT -ErrorAction SilentlyContinue
  } else {
    $env:GITHUB_OUTPUT = $previousOutput
  }
  if ([string]::IsNullOrEmpty($previousSummary)) {
    Remove-Item Env:GITHUB_STEP_SUMMARY -ErrorAction SilentlyContinue
  } else {
    $env:GITHUB_STEP_SUMMARY = $previousSummary
  }
}
