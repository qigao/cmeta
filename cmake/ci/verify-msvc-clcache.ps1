# Prove a real MSVC cache hit and header dependency invalidation.
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$cache = $env:CLCACHE_EXE
if (!(Test-Path -LiteralPath $cache)) { throw 'CLCACHE_EXE unavailable' }
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source
$dir = Join-Path $env:RUNNER_TEMP ('salts-msvc-clcache-smoke-' + $env:GITHUB_RUN_ID)
New-Item -ItemType Directory -Path $dir -Force | Out-Null
$source = Join-Path $dir 'probe.c'
$header = Join-Path $dir 'probe.h'
$obj = Join-Path $dir 'probe.obj'
$utf8 = [System.Text.UTF8Encoding]::new($false)
$nl = [Environment]::NewLine
[System.IO.File]::WriteAllText($source, ('#include "probe.h"' + $nl + 'int clcache_probe(void) {return VALUE;}' + $nl), $utf8)
[System.IO.File]::WriteAllText($header, ('#define VALUE 17' + $nl), $utf8)
$env:CLCACHE_TRACE = '1'
# CMake/Ninja emits /Fd<directory>\ even in /Z7 builds. This trailing
# backslash must not swallow the closing quote or the source filename.
$fdArg = '/Fd' + $dir + [IO.Path]::DirectorySeparatorChar

function Compile([bool] $expectSuccess) {
    $logLines = @(& $cache $compiler /nologo /c /TC /Z7 "/I$dir" "/Fo$obj" $fdArg /FS /showIncludes $source 2>&1)
    $code = $LASTEXITCODE
    $output = ($logLines | Out-String)
    if ($expectSuccess -and $code -ne 0) { throw ('MSVC probe failed: exit ' + $code + ' ' + $output) }
    if (!$expectSuccess -and $code -eq 0) { throw ('MSVC probe unexpectedly succeeded: ' + $output) }
    return $output
}
function Assert-Log([string] $log, [string] $needle, [string] $label) {
    if (!$log.Contains($needle)) { throw ($label + ': missing ' + $needle + ' in ' + $log) }
}

$initial = Compile $true
Assert-Log $initial 'clcache: MISS' 'Cold'
Assert-Log $initial 'including file:' 'Ninja dependency lines (cold)'
$hash1 = (Get-FileHash -LiteralPath $obj -Algorithm SHA256).Hash

Remove-Item -LiteralPath $obj -Force
$watch = [System.Diagnostics.Stopwatch]::StartNew()
$hot = Compile $true
$watch.Stop()
Assert-Log $hot 'clcache: HIT' 'Replayed'
Assert-Log $hot 'including file:' 'Ninja dependency lines (hit)'
$hash2 = (Get-FileHash -LiteralPath $obj -Algorithm SHA256).Hash
if ($hash1 -ne $hash2) { throw 'Cached object differs from original object' }

[System.IO.File]::WriteAllText($header, ('#define VALUE 29' + $nl), $utf8)
Remove-Item -LiteralPath $obj -Force
$changed = Compile $true
Assert-Log $changed 'clcache: MISS' 'Changed header invalidation'
$hash3 = (Get-FileHash -LiteralPath $obj -Algorithm SHA256).Hash
if ($hash1 -eq $hash3) { throw 'Changed header incorrectly reused prior object' }

Remove-Item -LiteralPath $header, $obj -Force
$removed = Compile $false
Assert-Log $removed 'clcache: MISS' 'Deleted header invalidation'
Write-Host ('MSVC native clcache smoke passed: cold MISS, hot HIT, object SHA256 parity, changed/missing header rejection, Ninja /showIncludes preserved; hot replay ' + $watch.ElapsedMilliseconds + 'ms')
