# Exercise the real selector's dedicated canonical ServiceHost audit profile.
# This checks routing only: no branch mutation, SDK build, package publication or
# expensive build is performed in this scope job.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (& git rev-parse --show-toplevel).Trim()
if ($LASTEXITCODE -ne 0 -or -not $root) {
  throw 'Missing Salts Git worktree'
}
$head = (& git rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $head -notmatch '^[0-9a-f]{40}$') {
  throw 'Missing exact checked-out source SHA'
}

$selector = Join-Path $root 'cmake/ci/select-ci-scope.ps1'
Push-Location $root
try {
  $result = @(& $selector -EventName pull_request -BaseRef $head -HeadRef $head -HeadBranch 'review/servicehost-canonical-sdk-audit' -PrepareRelease $false -AceMatrix $false -AceSan $false -UnifiedSan $false)
} finally {
  Pop-Location
}
if ($result.Count -ne 2) {
  throw 'Expected scope JSON and build matrix JSON from the real selector'
}

$checks = $result[0] | ConvertFrom-Json
$matrix = $result[1] | ConvertFrom-Json
if ([string]$checks.work -ne 'false') {
  throw 'ServiceHost SDK qualification must not select benchmark work'
}

$expectedRids = @(
  'linux-release',
  'linux-arm64-release',
  'windows-release',
  'macos-release',
  'android-arm64-v8a-release',
  'ios-arm64-release'
)
foreach ($id in $expectedRids) {
  $entry = @($matrix.include | Where-Object { $_.id -eq $id })
  if ($entry.Count -ne 1) {
    throw "ServiceHost SDK qualification must select exactly one $id profile"
  }
  if ([string]$entry[0].package -ne 'true') {
    throw "ServiceHost SDK qualification omitted package=true for $id"
  }
}

foreach ($id in @('linux-ace-asan-ubsan', 'linux-ace-tsan')) {
  $entry = @($matrix.include | Where-Object { $_.id -eq $id })
  if ($entry.Count -ne 1) {
    throw "ServiceHost qualification must include exactly one sanitizer profile: $id"
  }
  if ([string]$entry[0].sanitizer -eq '') {
    throw "Sanitizer profile $id has no sanitizer selected"
  }
  if ([string]$entry[0].package -ne 'false') {
    throw "Sanitizer profile $id must not publish an SDK package"
  }
}

if (@($matrix.include).Count -ne 8) {
  throw "Expected six SDK profiles plus two sanitizer profiles, got $(@($matrix.include).Count)"
}

Write-Output 'PASS canonical ServiceHost selector => six SDK/package RIDs, ASan/UBSan and TSan, no benchmark work'
