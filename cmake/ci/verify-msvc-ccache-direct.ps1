# Standalone Windows CI proof: the MSVC wrapper must return a direct cache hit,
# reproduce the object exactly, then invalidate it when an included header changes.
$ErrorActionPreference = 'Stop'
$ccache = (Get-Command ccache -ErrorAction Stop).Source
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source
$direct = (& $ccache --get-config direct_mode).Trim()
if ($LASTEXITCODE -ne 0 -or $direct -ne 'true') {
    throw "MSVC ccache direct mode is not enabled: $direct"
}

$work = Join-Path $env:RUNNER_TEMP 'salts-msvc-ccache-direct-probe'
New-Item -ItemType Directory -Path $work -Force | Out-Null
$source = Join-Path $work 'probe.c'
$header = Join-Path $work 'probe.h'
$object = Join-Path $work 'probe.obj'
$utf8 = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText($source,
    "#include `"probe.h`"`nint salts_cache_probe(void) { return SALTS_CACHE_VALUE; }`n", $utf8)
[System.IO.File]::WriteAllText($header, "#define SALTS_CACHE_VALUE 17`n", $utf8)

function Get-CacheCounter([string] $key) {
    $lines = @(& $ccache --print-stats)
    if ($LASTEXITCODE -ne 0) { throw 'ccache --print-stats failed' }
    foreach ($line in $lines) {
        if ($line -match '^([a-z_]+)\s+(\d+)\s*$' -and $Matches[1] -eq $key) {
            return [long]$Matches[2]
        }
    }
    throw "ccache counter '$key' was not found"
}

function Invoke-CacheCompiler {
    & $ccache $compiler /nologo /c /TC /Od /Z7 "/I$work" "/Fo$object" $source
    if ($LASTEXITCODE -ne 0) { throw "MSVC cache probe compilation failed: $LASTEXITCODE" }
    if (-not (Test-Path -LiteralPath $object)) { throw 'MSVC did not produce probe.obj' }
}

# ccache skips newly written include files in direct mode to prevent races.
Start-Sleep -Seconds 2
Invoke-CacheCompiler
$firstHash = (Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash

$beforeDirect = Get-CacheCounter 'cache_hit_direct'
Remove-Item -LiteralPath $object -Force
$watch = [System.Diagnostics.Stopwatch]::StartNew()
Invoke-CacheCompiler
$watch.Stop()
$afterDirect = Get-CacheCounter 'cache_hit_direct'
$replayedHash = (Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash
if ($afterDirect -le $beforeDirect) {
    throw 'Second identical MSVC compilation did not hit direct cache'
}
if ($replayedHash -ne $firstHash) {
    throw 'Direct-cache replay did not reproduce byte-identical object'
}

# A changed header must never reuse the old object, even in direct mode.
[System.IO.File]::WriteAllText($header, "#define SALTS_CACHE_VALUE 29`n", $utf8)
Start-Sleep -Seconds 2
Remove-Item -LiteralPath $object -Force
Invoke-CacheCompiler
$changedHash = (Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash
if ($changedHash -eq $firstHash) {
    throw 'Header mutation did not invalidate the cached object'
}

Write-Host "MSVC ccache: direct hit proven (+$($afterDirect - $beforeDirect)); byte-identical object verified; header invalidation verified; replay time = $($watch.ElapsedMilliseconds) ms"
