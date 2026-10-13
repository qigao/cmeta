param([string]$Path = "cmake/ci/benchmark-policy.json", [string]$ProjectRoot = ".", [switch]$WriteSummary)
$ErrorActionPreference = "Stop"
$policyPath = Join-Path $ProjectRoot $Path
if (-not (Test-Path -LiteralPath $policyPath -PathType Leaf)) { throw "benchmark policy not found: $policyPath" }
$policy = Get-Content -LiteralPath $policyPath -Raw | ConvertFrom-Json
if ($policy.schema_version -ne 1) { throw "unsupported benchmark policy schema: $($policy.schema_version)" }
$allowedRoles = @("CONTRACT","PERFORMANCE_ANCHOR","DIAGNOSTIC")
$allowedResults = @("PASS","FAIL","UNSTABLE","DIAGNOSTIC")
$allowedPrModes = @("affected","anchor","anchor-candidate","diagnostic","reduced-sweep","master-or-failure","master-or-manual")
$results = @($policy.result_values)
if ($results.Count -ne $allowedResults.Count) { throw "invalid benchmark result vocabulary" }
foreach ($result in $allowedResults) { if ($results -notcontains $result) { throw "missing benchmark result: $result" } }
$families = @($policy.families)
if ($families.Count -eq 0) { throw "benchmark policy contains no families" }
$ids = @{}; $roleCounts = @{}; $performanceAnchors = 0
foreach ($family in $families) {
  $id = [string]$family.id; $role = [string]$family.role; $scope = [string]$family.scope
  $prMode = [string]$family.pr_mode; $verifier = [string]$family.verifier
  $anchor = [string]$family.anchor; $noise = [string]$family.noise; $threshold = [string]$family.threshold
  if ([string]::IsNullOrWhiteSpace($id)) { throw "benchmark family has empty id" }
  if ($ids.ContainsKey($id)) { throw "duplicate benchmark family id: $id" }; $ids[$id] = $true
  if ($allowedRoles -notcontains $role) { throw "invalid benchmark role for $id : $role" }
  if ([string]::IsNullOrWhiteSpace($scope)) { throw "benchmark family has empty scope: $id" }
  if ($allowedPrModes -notcontains $prMode) { throw "invalid PR mode for $id : $prMode" }
  if ([string]::IsNullOrWhiteSpace($anchor)) { throw "benchmark family has no anchor: $id" }
  if (@($family.evidence).Count -eq 0) { throw "benchmark family has no evidence mapping: $id" }
  if (-not [string]::IsNullOrWhiteSpace($verifier)) {
    $verifierPath = Join-Path $ProjectRoot $verifier
    if (-not (Test-Path -LiteralPath $verifierPath -PathType Leaf)) { throw "benchmark verifier missing for $id : $verifier" }
  }
  if ($role -eq "PERFORMANCE_ANCHOR") {
    $performanceAnchors++
    if ($noise -in @("","n/a","none")) { throw "performance anchor missing noise qualification: $id" }
    if ($threshold -in @("","n/a","none","contract only")) { throw "performance anchor missing threshold: $id" }
  } elseif ($role -eq "DIAGNOSTIC") {
    if ($threshold -ne "none") { throw "diagnostic benchmark cannot define performance threshold: $id" }
    if ($prMode -eq "anchor-candidate") {
      if ($null -eq $family.candidate_noise_relative_mad_percent) {
        throw "anchor candidate missing candidate_noise_relative_mad_percent: $id"
      }
      $candidateNoise = [double]$family.candidate_noise_relative_mad_percent
      if (-not [double]::IsFinite($candidateNoise) -or $candidateNoise -le 0.0) {
        throw "anchor candidate has invalid noise limit: $id"
      }
      if (@($family.candidate_payloads).Count -eq 0) {
        throw "anchor candidate has no payload cells: $id"
      }
    }
  } else {
    if ($threshold -ne "contract only") { throw "contract benchmark must use contract-only semantics: $id" }
  }
  if (-not $roleCounts.ContainsKey($role)) { $roleCounts[$role] = 0 }; $roleCounts[$role]++
}
if ($performanceAnchors -eq 0) { throw "policy requires at least one PERFORMANCE_ANCHOR" }
Write-Host ("Benchmark policy verified: families={0} contract={1} anchors={2} diagnostic={3}" -f $families.Count,$roleCounts["CONTRACT"],$roleCounts["PERFORMANCE_ANCHOR"],$roleCounts["DIAGNOSTIC"])
if ($WriteSummary) {
  $summaryPath = $env:GITHUB_STEP_SUMMARY
  if ([string]::IsNullOrWhiteSpace($summaryPath)) { throw "-WriteSummary requires GITHUB_STEP_SUMMARY" }
  Add-Content -LiteralPath $summaryPath -Value ""
  Add-Content -LiteralPath $summaryPath -Value "## Benchmark decision policy"
  Add-Content -LiteralPath $summaryPath -Value ""
  Add-Content -LiteralPath $summaryPath -Value ("**{0} families** — CONTRACT {1} · PERFORMANCE_ANCHOR {2} · DIAGNOSTIC {3}" -f $families.Count,$roleCounts["CONTRACT"],$roleCounts["PERFORMANCE_ANCHOR"],$roleCounts["DIAGNOSTIC"])
  Add-Content -LiteralPath $summaryPath -Value ""
  Add-Content -LiteralPath $summaryPath -Value "> CONTRACT violations block merge. PERFORMANCE_ANCHOR cells may block merge only after noise qualification passes. DIAGNOSTIC relative speed never blocks merge by itself."
  Add-Content -LiteralPath $summaryPath -Value ""
  Add-Content -LiteralPath $summaryPath -Value "| Family | Role | PR mode | Anchor | Noise | Threshold |"
  Add-Content -LiteralPath $summaryPath -Value "|---|---|---|---|---|---|"
  foreach ($family in ($families | Sort-Object scope,id)) {
    $cells = @([string]$family.id,[string]$family.role,[string]$family.pr_mode,[string]$family.anchor,[string]$family.noise,[string]$family.threshold) | ForEach-Object { $_.Replace("|","\|").Replace([char]13," ").Replace([char]10," ") }
    Add-Content -LiteralPath $summaryPath -Value ("| {0} | {1} | {2} | {3} | {4} | {5} |" -f $cells)
  }
  Add-Content -LiteralPath $summaryPath -Value ""
  Add-Content -LiteralPath $summaryPath -Value "Allowed verdicts: **PASS**, **FAIL**, **UNSTABLE**, **DIAGNOSTIC**."
}

# CI structure admission: expensive throughput samples must not gate native
# functional execution. This is checked before the benchmark matrix is run.
$nativeWorkflowPath = Join-Path $ProjectRoot ".github/workflows/native-build.yml"
$benchmarkWorkflowPath = Join-Path $ProjectRoot ".github/workflows/native-io-benchmarks.yml"
$scopeScriptPath = Join-Path $ProjectRoot "cmake/ci/select-ci-scope.ps1"
foreach ($requiredPath in @($nativeWorkflowPath, $benchmarkWorkflowPath, $scopeScriptPath)) {
  if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
    throw "CI benchmark isolation input missing: $requiredPath"
  }
}
$nativeWorkflow = Get-Content -LiteralPath $nativeWorkflowPath -Raw
$benchmarkWorkflow = Get-Content -LiteralPath $benchmarkWorkflowPath -Raw
$scopeScript = Get-Content -LiteralPath $scopeScriptPath -Raw

foreach ($nativeForbidden in @(
    "- name: Run Actor, mailbox and ACE throughput benchmarks",
    "- name: Measure CNet progress strategies",
    "actor_bench_unix", "actor_bench_windows",
    "progress_bench_unix", "progress_bench_windows")) {
  if ($nativeWorkflow.Contains($nativeForbidden)) {
    throw "Throughput step leaked back into native functional jobs: $nativeForbidden"
  }
}
foreach ($functionalGate in @(
    "Run complete Unix integration tests",
    "Run complete Windows ACE integration tests",
    "Run Windows execution and nested CSTL contracts",
    "Run Unix native and plugin contracts")) {
  if (-not $nativeWorkflow.Contains($functionalGate)) {
    throw "Native functional acceptance missing after benchmark split: $functionalGate"
  }
}
foreach ($benchmarkGate in @(
    "Restore compiled benchmark inputs",
    "Sample Actor mailbox / ACE / Leader-Followers throughput",
    "Sample CNet progress strategies",
    "matrix.backend != 'io_uring'",
    'cflow-actor-mailbox-${{ matrix.build_artifact }}',
    'cnet-progress-strategy-${{ matrix.build_artifact }}')) {
  if (-not $benchmarkWorkflow.Contains($benchmarkGate)) {
    throw "Independent benchmark admission missing: $benchmarkGate"
  }
}
foreach ($scopeFlag in @(
    '$checks = [ordered]@{',
    'ace_actor = $aceActor',
    'cnet_progress_bench = $cnetProgressBench',
    '$entry.benchmarks = if ($work -and')) {
  if (-not $scopeScript.Contains($scopeFlag)) {
    throw "Scoped benchmark producer selection missing: $scopeFlag"
  }
}
Write-Host "Native/benchmark CI isolation verified: functional gates kept, performance runs separate"
