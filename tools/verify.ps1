<#
.SYNOPSIS
  Replay regression check: builds the tool and checks that a movie replays exactly the same way at every speed,
  with savestates, and with the editor's way of rewinding. Needs the game installed on this PC.

.DESCRIPTION
  Every check runs the real game through tools\sstest.exe and compares the player's values (health, weapon points,
  score, speeds, position, invisibility) at every frame. A check passes only if the runs are identical.

    Two runs at Max            the same movie twice at full speed, no savestates
    Savestates                 continuous vs a savestate every 2000 frames vs loading one midway and running on
    Editor rewind              the editor's Rewind (whole schedule armed at launch) vs stepping
    Title-screen savestate     save at frame 100 (before the save file loads), load and replay 3 times
    Late savestate             save near the end, load and replay twice, then play different inputs after loads
    Real time (-Full only)     Max vs 1x real time (takes as long as the movie lasts)

  The game is killed and relaunched for every run: do not play while it runs. Logs: build\verify\<check>.log.
  Exit code 0 = all passed, 1 = a check failed, 2 = could not run.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\verify.ps1 -Movie ..\test3.bscotm
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\verify.ps1 -Movie ..\test3.bscotm -Full
#>
param(
    [string]$Movie = "..\test3.bscotm",     # relative to the repository folder, or absolute
    [string]$Game,                          # COTM.exe; default: the editor's setting in build\bscotm_tas.ini
    [string]$Baseline,                      # baseline folder; default: the editor's last baseline
    [string]$Prelude,                       # prelude movie, if the movie needs one
    [switch]$Full,                          # also compare with a real-time (1x) run
    [switch]$NoBuild                        # use the binaries already in build\
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build'
function Fail([string]$msg) { Write-Host "verify: $msg" -ForegroundColor Red; exit 2 }

# ---- settings (the editor's ini: [paths] game=, [last] baseline=) -------------------------------------------------
$ini = @{}
$iniPath = Join-Path $build 'bscotm_tas.ini'
if (Test-Path $iniPath) {
    $sec = ''
    foreach ($line in Get-Content $iniPath) {
        if ($line -match '^\[(.+)\]$') { $sec = $Matches[1] }
        elseif ($line -match '^([^=]+)=(.*)$') { $ini["$sec.$($Matches[1])"] = $Matches[2] }
    }
}
if (-not $Game) { $Game = $ini['paths.game'] }
if (-not $Game -or -not (Test-Path $Game)) { Fail "game not found; pass -Game <path to COTM.exe> (or set it once in the editor)" }
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $Game))      # the folder that holds the game folder
if (-not $Baseline -and $ini['last.baseline']) { $Baseline = Join-Path $root "save_backups\baselines\$($ini['last.baseline'])" }
if ($Baseline -and -not (Test-Path $Baseline)) { Fail "baseline folder not found: $Baseline" }
if (-not [IO.Path]::IsPathRooted($Movie)) { $Movie = Join-Path $repo $Movie }
if (-not (Test-Path $Movie)) { Fail "movie not found: $Movie" }
$Movie = (Resolve-Path $Movie).Path
$frames = ((Get-Content -Raw $Movie | ConvertFrom-Json).frames).Count
if ($frames -lt 1200) { Fail "the movie is too short ($frames frames); use one of at least 1200 frames" }
if (Get-Process COTM -ErrorAction SilentlyContinue) { Fail "the game is running; close it first (every check relaunches it)" }

# ---- build ------------------------------------------------------------------------------------------------------
if (-not $NoBuild) {
    Write-Host "building..."
    # cmd needs its own cd (Push-Location does not change it) and the full path (the current folder may not be searched)
    & cmd /c "cd /d `"$repo`" && `"$repo\build.bat`" all tools > build\verify-build.log 2>&1"
    if ($LASTEXITCODE -ne 0) { Fail "build failed, see build\verify-build.log" }
}
$sstest = Join-Path $build 'sstest.exe'
$dll = Join-Path $build 'bscotm_hook.dll'
foreach ($f in $sstest, $dll) { if (-not (Test-Path $f)) { Fail "missing $f (run build.bat all tools)" } }

# ---- checks -----------------------------------------------------------------------------------------------------
$n = $frames - 5                             # compare up to here
$late = [Math]::Max(500, $n - 900)
$checks = @(
    @{ Name = 'Two runs at Max';         Args = @('--cmp', $n, '--variant', 3, '--nostates', 1) },
    @{ Name = 'Savestates';              Args = @('--cmp', $n, '--variant', 0, '--loadat', [int]($n / 2)) },
    @{ Name = 'Editor rewind';           Args = @('--cmp', $n, '--variant', 4) },
    @{ Name = 'Title-screen savestate';  Args = @('--at', 100, '--gap', 300, '--reps', 3) },
    @{ Name = 'Late savestate';          Args = @('--at', $late, '--gap', 400, '--reps', 2, '--altoff', 100, '--alts', 2) }
)
if ($Full) { $checks += @{ Name = 'Real time vs Max'; Args = @('--cmp', $n, '--variant', 2, '--nostates', 1) } }

$common = @($Movie, '--exe', $Game, '--dll', $dll, '--speed', 1000000)
if ($Baseline) { $common += @('--baseline', $Baseline) }
if ($Prelude) { $common += @('--prelude', (Resolve-Path $Prelude).Path) }
$logDir = Join-Path $build 'verify'
New-Item -ItemType Directory -Force $logDir | Out-Null

Write-Host "movie: $Movie ($frames frames)"
$results = @()
$t0 = Get-Date
foreach ($c in $checks) {
    $log = Join-Path $logDir (($c.Name -replace '[^A-Za-z0-9]+', '-').ToLower() + '.log')
    Write-Host ("{0,-24} running..." -f $c.Name) -NoNewline
    $start = Get-Date
    for ($try = 1; $try -le 2; $try++) {            # a launch now and then fails with "The game exited" right after a build: retry once
        $out = & $sstest @common @($c.Args | ForEach-Object { "$_" }) 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
        if (-not ($out -match 'launch failed')) { break }
    }
    $out | Set-Content -Encoding utf8 $log
    $warn = @($out | Select-String 'timed out [1-9]|by itself [1-9]' | ForEach-Object { $_.Line.Trim() })
    $ok = ($code -eq 0) -and ($out -match 'VERDICT: PASS')
    $secs = [int]((Get-Date) - $start).TotalSeconds
    $status = if ($ok) { 'PASS' } else { 'FAIL' }
    Write-Host ("`r{0,-24} {1}  ({2} s){3}" -f $c.Name, $status, $secs, $(if ($warn) { '  (clock fallbacks used, see log)' } else { '' })) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
    if (-not $ok) {
        $out | Select-String 'first difference|differing frames|FAIL|launch failed|exited' | Select-Object -First 4 | ForEach-Object { Write-Host "    $($_.Line.Trim())" }
        Write-Host "    log: $log"
    }
    $results += [pscustomobject]@{ Check = $c.Name; Result = $status; Seconds = $secs }
}

$failed = @($results | Where-Object { $_.Result -ne 'PASS' }).Count
Write-Host ""
Write-Host ("{0} of {1} checks passed in {2:N0} s" -f ($results.Count - $failed), $results.Count, ((Get-Date) - $t0).TotalSeconds) -ForegroundColor $(if ($failed) { 'Red' } else { 'Green' })
exit $(if ($failed) { 1 } else { 0 })
