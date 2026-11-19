# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/test-suites/ntfs/Invoke-NtfsSuite.ps1
# PURPOSE:     Run the NTFS compliance guest
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
#              Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@verosnt.com>
#              Ryan Wilson <ryan@taveraos.com>

param(
    [ValidateSet('all', 'static', 'compliance', 'stress', 'fault', 'fuzz')]
    [string]$Phase = 'all',
    [int]$Count = 25,
    [int]$TimeoutSec = 1200,
    [string]$Iso = '',
    [string]$Qemu = '',
    [string]$Clang = ''
)

$ErrorActionPreference = 'Stop'
$SuiteRoot = $PSScriptRoot

function Find-ResolveVerosPaths {
    param([string]$Hint)
    $cursor = $Hint
    for ($i = 0; $i -lt 8; $i++) {
        $candidate = Join-Path $cursor 'buildenv\Resolve-VerosPaths.ps1'
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
        $atRoot = Join-Path $cursor 'Resolve-VerosPaths.ps1'
        if ((Test-Path -LiteralPath $atRoot) -and (Test-Path -LiteralPath (Join-Path $cursor 'env.cmd'))) {
            return (Resolve-Path -LiteralPath $atRoot).Path
        }
        $parent = Split-Path -Parent $cursor
        if (-not $parent -or $parent -eq $cursor) { break }
        $cursor = $parent
    }
    throw "Resolve-VerosPaths.ps1 was not found from '$Hint'."
}

. (Find-ResolveVerosPaths -Hint $SuiteRoot)
$BuildenvRoot = Resolve-VerosBuildenvRoot -Hint $SuiteRoot
$SourceRoot = Resolve-VerosSourceRoot -BuildenvRoot $BuildenvRoot
$Out = Join-Path $SuiteRoot 'out'
New-Item -ItemType Directory -Force -Path $Out | Out-Null

if (-not $Iso) { $Iso = Join-Path $SourceRoot 'build-amd64\bootcd.iso' }
if (-not $Clang) {
    $clangBin = Resolve-VerosClangBin -BuildenvRoot $BuildenvRoot
    if ($clangBin) { $Clang = Join-Path $clangBin 'clang.exe' }
    if (-not $Clang -or -not (Test-Path $Clang)) { $Clang = 'clang' }
}
if (-not $Qemu) {
    $Qemu = Resolve-VerosQemuExe -BuildenvRoot $BuildenvRoot
}
if (-not $Qemu -or -not (Test-Path $Qemu)) {
    throw 'QEMU was not found under the buildenv tools\qemu folder.'
}

. (Join-Path $SuiteRoot 'features.ps1')
$Catalog = @(Get-NtfsFeatureCatalog)
$Status = @{}
$Detail = @{}
foreach ($row in $Catalog) {
    $Status[$row.Name] = 'untested'
    $Detail[$row.Name] = ''
}

function Set-Row {
    param([string]$Name, [string]$State, [string]$Why)
    if (-not $Status.ContainsKey($Name)) { return }
    if ($Status[$Name] -eq 'red' -and $State -ne 'red') { return }
    $Status[$Name] = $State
    if ($Why) { $Detail[$Name] = $Why }
}

function Read-ResultLog {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return }
    foreach ($line in Get-Content $Path) {
        $parts = $line -split "`t", 3
        if ($parts.Count -lt 2) { continue }
        $name = $parts[0].Trim()
        $state = $parts[1].Trim()
        $why = ''
        if ($parts.Count -ge 3) { $why = $parts[2].Trim() }
        if ($state -eq 'green' -or $state -eq 'red') {
            Set-Row $name $state $why
        }
    }
}

function Write-Snapshot {
    param([string]$RanPhase)
    $previous = $null
    $snapPath = Join-Path $Out 'ntfs31.json'
    if (Test-Path $snapPath) {
        $previous = Get-Content $snapPath -Raw | ConvertFrom-Json
    }
    $green = @($Catalog | Where-Object { $Status[$_.Name] -eq 'green' }).Count
    $red = @($Catalog | Where-Object { $Status[$_.Name] -eq 'red' }).Count
    $untested = @($Catalog | Where-Object { $Status[$_.Name] -eq 'untested' }).Count
    $scored = $green + $red
    $percent = $null
    if ($scored -gt 0) { $percent = [math]::Round(100.0 * $green / $scored, 1) }
    $older = @{}
    if ($previous -and $previous.features) {
        foreach ($row in @($previous.features)) { $older[$row.name] = [string]$row.status }
    }
    $improvements = @()
    $regressions = @()
    $features = @()
    foreach ($row in $Catalog) {
        $features += [ordered]@{
            name   = $row.Name
            group  = $row.Group
            status = $Status[$row.Name]
            detail = $Detail[$row.Name]
        }
        if ($older.ContainsKey($row.Name)) {
            $was = $older[$row.Name]
            $now = $Status[$row.Name]
            if ($now -eq 'green' -and $was -ne 'green') {
                $improvements += $row.Name
            } elseif ($was -eq 'green' -and $now -eq 'red') {
                $regressions += $row.Name
            }
        }
    }
    $snap = [ordered]@{
        spdx          = 'GPL-2.0-or-later'
        capturedAt    = (Get-Date).ToUniversalTime().ToString('o')
        phase         = $RanPhase
        percent       = $percent
        green         = $green
        red           = $red
        untested      = $untested
        features      = $features
        improvements  = $improvements
        regressions   = $regressions
    }
    ($snap | ConvertTo-Json -Depth 6) | Set-Content -Path $snapPath -Encoding utf8
    $lines = @()
    $score = 'untested'
    if ($null -ne $percent) { $score = "$green of $scored  $percent%" }
    $lines += "NTFS 3.1  $score"
    $lines += ''
    $group = ''
    foreach ($row in $Catalog) {
        if ($row.Group -ne $group) {
            $group = $row.Group
            $lines += $group
        }
        $mark = $Status[$row.Name]
        $extra = $Detail[$row.Name]
        if ($extra) { $lines += ("  {0}  {1}  {2}" -f $row.Name, $mark, $extra) }
        else { $lines += ("  {0}  {1}" -f $row.Name, $mark) }
    }
    if ($improvements.Count -gt 0) {
        $lines += ''
        $lines += 'Turned green:'
        foreach ($name in $improvements) { $lines += "  $name" }
    }
    if ($regressions.Count -gt 0) {
        $lines += ''
        $lines += 'Turned red:'
        foreach ($name in $regressions) { $lines += "  $name" }
    }
    $text = $lines -join [Environment]::NewLine
    Set-Content -Path (Join-Path $Out 'ntfs31.txt') -Value $text -Encoding utf8
    Write-Host $text
}

function Get-NinjaFlags {
    param([string]$Fragment)
    $ninja = Join-Path $SourceRoot 'build-amd64\build.ninja'
    if (-not (Test-Path $ninja)) { return $null }
    $lines = Get-Content $ninja
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -notlike "*$Fragment*") { continue }
        $flags = @{ Defines = ''; Includes = ''; Flags = '' }
        for ($j = 1; $j -le 12 -and ($i + $j) -lt $lines.Count; $j++) {
            $line = $lines[$i + $j]
            if ($line -match '^\s+DEFINES = (.*)$') { $flags.Defines = $Matches[1] }
            elseif ($line -match '^\s+INCLUDES = (.*)$') { $flags.Includes = $Matches[1] }
            elseif ($line -match '^\s+FLAGS = (.*)$') { $flags.Flags = $Matches[1] }
            elseif ($line -match '^build ') { break }
        }
        if ($flags.Flags) { return $flags }
    }
    return $null
}

function Invoke-Static {
    $clang = $Clang
    $tidy = Join-Path (Split-Path $clang -Parent) 'clang-tidy.exe'
    $build = Join-Path $SourceRoot 'build-amd64'
    $groups = @(
        @{ Fragment = 'drivers/filesystems/ntfs/ntfs.c'; Dir = (Join-Path $SourceRoot 'drivers\filesystems\ntfs') }
        @{ Fragment = 'sdk/lib/fslib/ntfslib/ntfslib.c'; Dir = (Join-Path $SourceRoot 'sdk\lib\fslib\ntfslib') }
    )
    $parsed = 0
    $defects = New-Object System.Collections.Generic.List[string]
    $unparsed = New-Object System.Collections.Generic.List[string]
    $logDir = Join-Path $Out 'static'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    foreach ($group in $groups) {
        $flags = Get-NinjaFlags $group.Fragment
        if (-not $flags) {
            $unparsed.Add($group.Fragment)
            continue
        }
        $sources = Get-ChildItem $group.Dir -Filter *.c
        foreach ($src in $sources) {
            $outLog = Join-Path $logDir ($src.Name + '.log')
            $arg = "--analyze $($flags.Defines) $($flags.Includes) $($flags.Flags) -c `"$($src.FullName)`" > `"$outLog`" 2>&1"
            $psi = New-Object System.Diagnostics.ProcessStartInfo
            $psi.FileName = 'cmd.exe'
            $psi.Arguments = "/c `"`"$clang`" $arg`""
            $psi.WorkingDirectory = $build
            $psi.UseShellExecute = $false
            $proc = [System.Diagnostics.Process]::Start($psi)
            $proc.WaitForExit()
            $text = ''
            if (Test-Path $outLog) { $text = [string](Get-Content $outLog -Raw -ErrorAction SilentlyContinue) }
            $warnings = ([regex]::Matches($text, 'warning:')).Count
            $errors = ([regex]::Matches($text, 'error:')).Count
            if ($proc.ExitCode -ne 0 -and $warnings -eq 0) {
                $unparsed.Add($src.Name)
            } else {
                $parsed++
                if ($warnings -gt 0) { $defects.Add("$($src.Name) analyze $warnings") }
            }
            if (Test-Path $tidy) {
                $tidyLog = Join-Path $logDir ($src.Name + '.tidy')
                $tidyArg = "-checks=-*,clang-analyzer-*,bugprone-*,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling `"$($src.FullName)`" -- $($flags.Defines) $($flags.Includes) $($flags.Flags) > `"$tidyLog`" 2>&1"
                $tpsi = New-Object System.Diagnostics.ProcessStartInfo
                $tpsi.FileName = 'cmd.exe'
                $tpsi.Arguments = "/c `"`"$tidy`" $tidyArg`""
                $tpsi.WorkingDirectory = $build
                $tpsi.UseShellExecute = $false
                $tproc = [System.Diagnostics.Process]::Start($tpsi)
                $tproc.WaitForExit()
                $tidyText = ''
                if (Test-Path $tidyLog) { $tidyText = [string](Get-Content $tidyLog -Raw -ErrorAction SilentlyContinue) }
                if ($tidyText -match 'warning:') { $defects.Add("$($src.Name) tidy") }
            }
        }
    }
    if ($parsed -eq 0) {
        Set-Row 'StaticAnalysis' 'untested' 'sources did not parse'
    } elseif ($defects.Count -gt 0) {
        Set-Row 'StaticAnalysis' 'red' ($defects -join '; ')
    } else {
        Set-Row 'StaticAnalysis' 'green' ''
    }
}

function Build-HostTools {
    $hostDir = Join-Path $SuiteRoot 'host'
    $check = Join-Path $Out 'ntfs_check.exe'
    $fuzz = Join-Path $Out 'ntfs_fuzz.exe'
    $hostArgs = @('-fsanitize=address,undefined', '-g', '-O1', '-std=c11', '-Wall', '-D_CRT_SECURE_NO_WARNINGS')
    & $Clang @hostArgs (Join-Path $hostDir 'ntfs_check.c') -o $check
    if ($LASTEXITCODE -ne 0) { throw "ntfs_check failed to build" }
    & $Clang @hostArgs (Join-Path $hostDir 'ntfs_fuzz.c') -o $fuzz
    if ($LASTEXITCODE -ne 0) { throw "ntfs_fuzz failed to build" }
    & $check --self-test
    if ($LASTEXITCODE -ne 0) { Set-Row 'HostSanitizers' 'red' 'checker self-test failed'; return }
    & $fuzz --self-test
    if ($LASTEXITCODE -ne 0) { Set-Row 'HostSanitizers' 'red' 'fuzzer self-test failed'; return }
    Set-Row 'HostSanitizers' 'green' ''
}

function Find-MingwGcc {
    $candidates = @(
        (Join-Path $BuildenvRoot 'tools\rosbe\amd64\bin\x86_64-w64-mingw32-gcc.exe'),
        (Join-Path $BuildenvRoot 'tools\rosbe\i386\bin\gcc.exe')
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { return (Resolve-Path -LiteralPath $c).Path }
    }
    $winget = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    if (Test-Path $winget) {
        $found = Get-ChildItem -Path $winget -Filter 'x86_64-w64-mingw32-gcc.exe' -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName
        if ($found) { return $found }
    }
    $pathGcc = Get-Command x86_64-w64-mingw32-gcc -ErrorAction SilentlyContinue
    if ($pathGcc) { return $pathGcc.Source }
    return $null
}

function Build-Agent {
    # Freestanding MainCRTStartup agent — MinGW (Windows Kits optional / often absent).
    $gcc = Find-MingwGcc
    if (-not $gcc) { throw 'MinGW gcc was not found (x86_64-w64-mingw32-gcc)' }
    $exe = Join-Path $Out 'ntfs_agent.exe'
    $src = (Join-Path $SuiteRoot 'agent\ntfs_agent.c') -replace '\\', '/'
    $outFwd = $exe -replace '\\', '/'
    $wd = Split-Path -Parent $gcc
    $args = @(
        '-std=c11', '-O1', '-Wall', '-mconsole',
        '-fno-stack-check', '-fno-stack-protector', '-mno-stack-arg-probe',
        '-nostdlib', '-Wl,-e,MainCRTStartup',
        $src, '-o', $outFwd, '-lkernel32', '-ladvapi32'
    )
    $proc = Start-Process -FilePath $gcc -WorkingDirectory $wd -ArgumentList $args -Wait -PassThru -NoNewWindow `
        -RedirectStandardError (Join-Path $Out 'agent-build.err') `
        -RedirectStandardOutput (Join-Path $Out 'agent-build.out')
    if ($proc.ExitCode -ne 0 -or -not (Test-Path $exe)) {
        $why = ''
        if (Test-Path (Join-Path $Out 'agent-build.err')) { $why = Get-Content (Join-Path $Out 'agent-build.err') -Raw }
        throw "ntfs_agent failed to build: $why"
    }
    return $exe
}

function Build-SuiteIso {
    param([string]$AgentExe)
    $seven = Join-Path $BuildenvRoot 'tools\7zip\7z.exe'
    if (-not (Test-Path $seven)) { $seven = Join-Path $env:ProgramFiles '7-Zip\7z.exe' }
    $mkisofs = Join-Path $SourceRoot 'build-amd64\host-tools\bin\mkisofs.exe'
    $isohybrid = Join-Path $SourceRoot 'build-amd64\host-tools\bin\isohybrid.exe'
    $isombr = Join-Path $SourceRoot 'build-amd64\boot\freeldr\bootsect\isombr.bin'
    if (-not (Test-Path $Iso)) { throw "ISO was not found at $Iso" }
    if (-not (Test-Path $seven) -or -not (Test-Path $mkisofs)) { throw '7-Zip or mkisofs is missing' }
    $tree = Join-Path $Out 'iso-tree'
    $stamp = Join-Path $tree '.source-stamp'
    $ticks = (Get-Item $Iso).LastWriteTimeUtc.Ticks.ToString()
    if (-not (Test-Path $stamp) -or (Get-Content $stamp -Raw).Trim() -ne $ticks) {
        if (Test-Path $tree) { Remove-Item -Recurse -Force $tree }
        New-Item -ItemType Directory -Force -Path $tree | Out-Null
        & $seven x $Iso "-o$tree" -y | Out-Null
        Set-Content -Path $stamp -Value $ticks
    }
    $target = Join-Path $tree 'VerOS\system32\verreport.exe'
    if (-not (Test-Path $target)) { throw 'VerOS\system32\verreport.exe is not on the boot CD' }
    Copy-Item $AgentExe $target -Force
    $suiteIso = Join-Path $Out 'ntfs-suite.iso'
    & $mkisofs -quiet -o $suiteIso -iso-level 4 -publisher 'ReactOS Project' -preparer 'ReactOS Project' -volid ReactOS -volset ReactOS `
        -eltorito-platform x86 -eltorito-boot loader/isoboot.bin -no-emul-boot -boot-load-size 4 `
        -eltorito-alt-boot -eltorito-platform efi -eltorito-boot loader/efisys.bin -no-emul-boot `
        -no-cache-inodes -duplicates-once $tree
    if ($LASTEXITCODE -ne 0) { throw 'mkisofs failed' }
    if (Test-Path $isohybrid) { & $isohybrid -b $isombr -t 0x96 $suiteIso | Out-Null }
    return $suiteIso
}

function New-Share {
    param([string]$Name, [string]$PhaseName, [string]$AgentExe)
    $share = Join-Path $Out $Name
    if (Test-Path $share) { Remove-Item -Recurse -Force $share }
    New-Item -ItemType Directory -Force -Path $share | Out-Null
    Set-Content -Path (Join-Path $share 'NTFS-SUITE.txt') -Value 'ntfs 3.1' -Encoding ascii
    Set-Content -Path (Join-Path $share 'phase.txt') -Value $PhaseName -Encoding ascii
    Copy-Item $AgentExe (Join-Path $share 'ntfs_agent.exe') -Force
    return $share
}

function New-Disk {
    param([string]$Name)
    $qemuImg = Join-Path (Split-Path $Qemu -Parent) 'qemu-img.exe'
    $disk = Join-Path $Out $Name
    if (Test-Path $disk) { Remove-Item -Force $disk }
    & $qemuImg create -f raw $disk 64M | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'qemu-img create failed' }
    return $disk
}

function Test-Bugcheck {
    param([string]$Serial)
    if (-not (Test-Path $Serial)) { return $false }
    $text = Get-Content $Serial -Raw -ErrorAction SilentlyContinue
    return ($text -match 'BugCheck' -or $text -match 'Fatal System Error')
}

function Invoke-Guest {
    param(
        [string]$PhaseName,
        [string]$Disk,
        [string]$Share,
        [string]$SuiteIso,
        [string]$Blkdebug,
        [int]$KillAfterWriteSec = -1,
        [int]$BootTimeout = 0
    )
    if ($BootTimeout -le 0) { $BootTimeout = $TimeoutSec }
    $serial = Join-Path $Out ("serial-{0}-{1}.log" -f $PhaseName, (Get-Date -Format 'HHmmss'))
    $bios = Join-Path (Split-Path $Qemu -Parent) 'share'
    $diskArg = $Disk
    if ($Blkdebug) {
        $diskArg = "blkdebug:../fault/$(Split-Path $Blkdebug -Leaf):$(Split-Path $Disk -Leaf)"
    }
    $shareFwd = ($Share -replace '\\', '/')
    # VerOS stays on TCG until a live CD boot under WHPX reaches the desktop.
    $argLine = "-accel tcg -L `"$bios`" -m 8192 -smp 4 -boot d -no-reboot -display gtk -name `"VerOS NT`" -serial `"file:$serial`" -cdrom `"$SuiteIso`" -drive file=$diskArg,format=raw,if=ide,index=0,media=disk -drive file=fat:rw:${shareFwd},format=raw,if=ide,index=1,media=disk"
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Qemu
    $psi.Arguments = $argLine
    $psi.WorkingDirectory = $Out
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $started = Get-Date
    $hookNoted = $false
    $killed = $false
    while (-not $proc.HasExited) {
        $elapsed = ((Get-Date) - $started).TotalSeconds
        $result = Join-Path $Share 'result.log'
        $doneOk = Test-Path (Join-Path $Share 'DONE.ok')
        $resultDone = (Test-Path $result) -and (Select-String -Path $result -Pattern 'NTFS-AGENT-DONE' -Quiet)
        $serialDone = (Test-Path $serial) -and (
            (Select-String -Path $serial -Pattern 'NTFS-AGENT-DONE' -Quiet) -or
            (Select-String -Path $serial -Pattern 'Entering sleep state S5' -Quiet)
        )
        if ($resultDone -or $doneOk -or $serialDone) {
            if ((Test-Path $result) -and -not $resultDone) {
                Add-Content -Path $result -Value 'NTFS-AGENT-DONE' -Encoding ascii
            }
            $grace = 20
            while ($grace -gt 0 -and -not $proc.HasExited) {
                Start-Sleep -Seconds 1
                $grace--
            }
            if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
            break
        }
        if (-not $hookNoted -and $elapsed -gt 180) {
            $seen = $false
            if (Test-Path $result) {
                $seen = Select-String -Path $result -Pattern 'NTFS-AGENT' -Quiet
            }
            if (-not $seen) {
                Write-Host 'The live CD has not started the NTFS agent.'
                Write-Host '1. In the VerOS NT window, open a command prompt.'
                Write-Host '2. Switch to the drive that contains NTFS-SUITE.txt.'
                Write-Host '3. Run ntfs_agent.exe from that drive.'
                Write-Host '4. Leave the window open until the guest powers off or the log says NTFS-AGENT-DONE.'
                $hookNoted = $true
                $script:HookFailed = $true
            }
        }
        if ($KillAfterWriteSec -ge 0 -and (Test-Path (Join-Path $Share 'WRITING.txt')) -and -not $killed) {
            Start-Sleep -Seconds $KillAfterWriteSec
            if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
            $killed = $true
        }
        if ($elapsed -gt $BootTimeout -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
            break
        }
        Start-Sleep -Seconds 2
    }
    if (-not $proc.HasExited) { $proc.WaitForExit() }
    return @{ Serial = $serial; Bugcheck = (Test-Bugcheck $serial) }
}

$script:HookFailed = $false
try {
$wantStatic = $Phase -eq 'all' -or $Phase -eq 'static'
$wantCompliance = $Phase -eq 'all' -or $Phase -eq 'compliance'
$wantStress = $Phase -eq 'all' -or $Phase -eq 'stress'
$wantFault = $Phase -eq 'all' -or $Phase -eq 'fault'
$wantFuzz = $Phase -eq 'all' -or $Phase -eq 'fuzz'

if ($wantStatic) { Invoke-Static }

# Host ASAN tools need a MSVC-compatible clang environment. Compliance does not.
$hostWanted = $wantStress -or $wantFault -or $wantFuzz -or $Phase -eq 'all'
if ($hostWanted) {
    Build-HostTools
} else {
    Set-Row 'HostSanitizers' 'untested' 'skipped for this phase'
}

$agent = $null
$suiteIso = $null
$guestWanted = $wantCompliance -or $wantStress -or $wantFault -or $wantFuzz
if ($guestWanted) {
    $agent = Build-Agent
    $suiteIso = Build-SuiteIso $agent
}

if ($wantCompliance) {
    $disk = New-Disk 'compliance.img'
    $share = New-Share 'share-compliance' 'compliance' $agent
    Invoke-Guest -PhaseName 'compliance' -Disk $disk -Share $share -SuiteIso $suiteIso | Out-Null
    Read-ResultLog (Join-Path $share 'result.log')
    Copy-Item $disk (Join-Path $Out 'seed.img') -Force
}

if ($script:HookFailed -and $Phase -eq 'all') {
    Write-Host 'Stopping after this boot. The agent did not start on its own, so fault and fuzz boots were not started.'
    $wantStress = $false
    $wantFault = $false
    $wantFuzz = $false
}

if ($wantStress -and $agent) {
    $disk = New-Disk 'stress.img'
    $share = New-Share 'share-stress' 'stress' $agent
    $boot = Invoke-Guest -PhaseName 'stress' -Disk $disk -Share $share -SuiteIso $suiteIso
    Read-ResultLog (Join-Path $share 'result.log')
    if ($boot.Bugcheck) { Set-Row 'ConcurrentWriters' 'red' 'bugcheck' }
}

if ($wantFault -and $agent) {
    $disk = New-Disk 'fault.img'
    $share = New-Share 'share-fault' 'fault' $agent
    $cfg = Join-Path $SuiteRoot 'fault\write-error.cfg'
    $boot = Invoke-Guest -PhaseName 'fault' -Disk $disk -Share $share -SuiteIso $suiteIso -Blkdebug $cfg
    Read-ResultLog (Join-Path $share 'result.log')
    if ($boot.Bugcheck) { Set-Row 'IoErrorVisible' 'green' 'bugcheck' }
    $powerRed = $false
    foreach ($delay in @(1, 2, 4)) {
        $pdisk = New-Disk ("power-{0}.img" -f $delay)
        $pshare = New-Share ("share-power-{0}" -f $delay) 'powerwrite' $agent
        Invoke-Guest -PhaseName 'powerwrite' -Disk $pdisk -Share $pshare -SuiteIso $suiteIso -KillAfterWriteSec $delay | Out-Null
        $rshare = New-Share ("share-recover-{0}" -f $delay) 'recover' $agent
        $recover = Invoke-Guest -PhaseName 'recover' -Disk $pdisk -Share $rshare -SuiteIso $suiteIso
        $check = Join-Path $Out 'ntfs_check.exe'
        & $check $pdisk | Out-Null
        $corrupt = $LASTEXITCODE -ne 0
        if ($recover.Bugcheck -or $corrupt) { $powerRed = $true }
        Read-ResultLog (Join-Path $rshare 'result.log')
    }
    if ($powerRed) { Set-Row 'PowerLoss' 'red' 'bugcheck or corrupt image after power loss' }
}

if ($wantFuzz -and $agent) {
    $seed = Join-Path $Out 'seed.img'
    if (-not (Test-Path $seed)) {
        Set-Row 'Fuzz' 'untested' 'no seed image'
    } else {
        $fuzzExe = Join-Path $Out 'ntfs_fuzz.exe'
        $check = Join-Path $Out 'ntfs_check.exe'
        $bad = $false
        $why = ''
        for ($i = 0; $i -lt $Count; $i++) {
            $mutant = Join-Path $Out ("fuzz-{0}.img" -f $i)
            & $fuzzExe $seed $mutant $i
            if ($LASTEXITCODE -ne 0) { $bad = $true; $why = 'mutator failed'; break }
            $share = New-Share ("share-fuzz-{0}" -f $i) 'fuzz' $agent
            $boot = Invoke-Guest -PhaseName 'fuzz' -Disk $mutant -Share $share -SuiteIso $suiteIso -BootTimeout 480
            if ($boot.Bugcheck) { $bad = $true; $why = "bugcheck on image $i"; break }
            $log = Join-Path $share 'result.log'
            $rejected = $false
            $mounted = $false
            if (Test-Path $log) {
                $rejected = Select-String -Path $log -Pattern 'FuzzImage\trejected' -Quiet
                $mounted = Select-String -Path $log -Pattern 'FuzzImage\tmounted' -Quiet
            }
            if ($mounted) {
                & $check $mutant | Out-Null
                if ($LASTEXITCODE -ne 0) { $bad = $true; $why = "mounted image $i is corrupt"; break }
            } elseif (-not $rejected) {
                $bad = $true
                $why = "image $i timed out"
                break
            }
        }
        if ($bad) { Set-Row 'Fuzz' 'red' $why } else { Set-Row 'Fuzz' 'green' '' }
    }
}

} finally {
    Write-Snapshot $Phase
}
