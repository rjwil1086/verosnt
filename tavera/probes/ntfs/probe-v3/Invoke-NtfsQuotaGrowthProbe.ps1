# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v3/Invoke-NtfsQuotaGrowthProbe.ps1
# PURPOSE:     Boot F/T/M/D stages, dump $Q, write growth-report.txt
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

# Stages: F=format, T=+track, M=+modify 4096/8192, D=+FSCTL_SET_QUOTA limit -2
# Captures: output/ntfs/probe-v3/<stage>/  Cite tavera/probes/ntfs/probe-v3.
# Does not edit drivers/filesystems/ntfs.

param(
    [int]$TimeoutSec = 1200,
    [ValidateSet('F', 'T', 'M', 'D', 'All')][string]$Stage = 'All',
    [switch]$PreferWhpx
)

$ErrorActionPreference = 'Stop'
$Probe = $PSScriptRoot
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
$Buildenv = (Resolve-Path (Join-Path $SourceRoot '..\buildenv')).Path
$Out = Join-Path $SourceRoot 'output\ntfs\probe-v3'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Qemu = Join-Path $Buildenv 'tools\qemu\qemu-system-x86_64.exe'
$Img = Join-Path (Split-Path $Qemu -Parent) 'qemu-img.exe'
$Gcc = Join-Path $Buildenv 'tools\rosbe\amd64\bin\x86_64-w64-mingw32-gcc.exe'
if (-not (Test-Path $Gcc)) {
    $Gcc = Get-ChildItem -Path (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages') -Filter 'x86_64-w64-mingw32-gcc.exe' -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Gcc) { throw 'MinGW gcc missing' }
$hostGcc = Join-Path (Split-Path $Gcc) 'gcc.exe'
if (-not (Test-Path $hostGcc)) { $hostGcc = 'gcc' }

function Build-Tools {
    $wd = Split-Path $Gcc -Parent
    $guestSrc = (Join-Path $Probe 'guest\ntfs_quota_growth_probe.c') -replace '\\', '/'
    $guestOut = (Join-Path $Out 'ntfs_quota_growth_probe.exe') -replace '\\', '/'
    $p = Start-Process -FilePath $Gcc -WorkingDirectory $wd -ArgumentList @(
        '-std=c11', '-O1', '-Wall', '-mconsole', $guestSrc, '-o', $guestOut, '-lkernel32', '-ladvapi32'
    ) -Wait -PassThru -NoNewWindow -RedirectStandardError (Join-Path $Out 'guest-build.err') -RedirectStandardOutput (Join-Path $Out 'guest-build.out')
    if ($p.ExitCode -ne 0) { throw "guest build failed: $(Get-Content (Join-Path $Out 'guest-build.err') -Raw)" }
    $dumpOut = Join-Path $Out 'ntfs_raw_dump.exe'
    & $hostGcc -std=c11 -O1 -Wall (Join-Path $Probe 'host\ntfs_raw_dump.c') -o $dumpOut 2>$null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dumpOut)) {
        $fallback = Join-Path $SourceRoot 'output\ntfs\probe-v2\ntfs_raw_dump.exe'
        if (Test-Path $fallback) {
            Copy-Item $fallback $dumpOut -Force
            Write-Host "Reused probe-v2 ntfs_raw_dump.exe (host gcc unavailable)"
        } else {
            throw 'dump build failed and no probe-v2 dump binary'
        }
    }
}

function Read-SerialShared([string]$Path) {
    try {
        $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object IO.StreamReader($fs)
        $t = $sr.ReadToEnd(); $sr.Close(); $fs.Close(); return $t
    } catch { return '' }
}

function Ensure-SystemVhdx {
    $sys = Join-Path $Out 'windows-system.vhdx'
    if (Test-Path $sys) { return $sys }
    $candidates = @(
        (Join-Path $SourceRoot 'output\ntfs\probe-v2\windows-system.vhdx'),
        (Join-Path $SourceRoot 'tavera\test-suites\ntfs\out\windows-system.vhdx')
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) {
            Write-Host "Copying Windows VHDX from $c"
            Copy-Item $c $sys
            return $sys
        }
    }
    $qcow = Join-Path $Buildenv 'virtual-machines\win10-22h2\disk.qcow2'
    Write-Host 'Converting Windows qcow2 to VHDX (slow once)'
    & $Img convert -O vhdx $qcow $sys
    if ($LASTEXITCODE -ne 0) { throw 'qemu-img convert failed' }
    & fsutil sparse setflag $sys 0 | Out-Null
    & compact /u $sys | Out-Null
    return $sys
}

function Invoke-Stage([string]$S) {
    $caseOut = Join-Path $Out $S
    New-Item -ItemType Directory -Force -Path $caseOut | Out-Null
    $disk = Join-Path $caseOut 'windows-raw.img'
    $serial = Join-Path $caseOut 'guest-serial.log'
    $err = Join-Path $caseOut 'windows-qemu.err'
    Remove-Item $disk, $serial, $err -Force -ErrorAction SilentlyContinue
    & $Img create -f raw $disk 64M | Out-Null

    $sys = Ensure-SystemVhdx
    $regLog = Join-Path $Out 'register-probe.log'
    Remove-Item $regLog -Force -ErrorAction SilentlyContinue
    Write-Host "Approve UAC to register stage $S"
    $reg = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Probe 'Register-WindowsQuotaGrowthProbe.ps1'),
        '-Stage', $S
    )
    if ($reg.ExitCode -ne 0 -or (Get-Content $regLog -Raw).Trim() -ne 'ok') {
        throw "register $S failed: $(Get-Content $regLog -Raw -ErrorAction SilentlyContinue)"
    }

    $vars = Join-Path $caseOut 'win-ovmf-vars.fd'
    Copy-Item (Join-Path $Buildenv 'virtual-machines\win10-22h2\ovmf-vars.fd') $vars -Force
    $code = (Join-Path $Buildenv 'tools\qemu\share\edk2-x86_64-code.fd') -replace '\\', '/'
    $common = @(
        '-machine', 'q35', '-m', '8192', '-smp', '4',
        '-display', 'gtk', '-vga', 'std', '-no-reboot',
        '-serial', "file:$(($serial -replace '\\', '/'))",
        '-device', 'qemu-xhci', '-device', 'usb-tablet',
        '-drive', "if=pflash,format=raw,readonly=on,file=$code",
        '-drive', "if=pflash,format=raw,file=$(($vars -replace '\\', '/'))",
        '-drive', "file=$(($sys -replace '\\', '/')),format=vhdx,if=none,id=hd,snapshot=on",
        '-device', 'virtio-blk-pci,drive=hd,bootindex=1',
        '-drive', "file=$(($disk -replace '\\', '/')),format=raw,if=none,id=blank",
        '-device', 'virtio-blk-pci,drive=blank'
    )
    $accel = 'tcg'
    $cpu = 'qemu64'
    if ($PreferWhpx) { $accel = 'whpx'; $cpu = 'host' }
    $argList = @('-accel', $accel, '-cpu', $cpu) + $common
    Write-Host "Booting stage $S ($accel)"
    $proc = Start-Process -FilePath $Qemu -ArgumentList $argList -PassThru -RedirectStandardError $err
    Start-Sleep -Seconds 5
    if ($proc.HasExited -and $PreferWhpx -and $accel -eq 'whpx') {
        Write-Host "WHPX failed early; falling back to TCG. $(Get-Content $err -Raw -EA SilentlyContinue)"
        Remove-Item $err -Force -EA SilentlyContinue
        $argList = @('-accel', 'tcg', '-cpu', 'qemu64') + $common
        $proc = Start-Process -FilePath $Qemu -ArgumentList $argList -PassThru -RedirectStandardError $err
        Start-Sleep -Seconds 5
    }
    if ($proc.HasExited) { throw "QEMU exited early for $S. $(Get-Content $err -Raw -EA SilentlyContinue)" }

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ((Read-SerialShared $serial) -match 'NTFS-QUOTA-GROWTH-DONE') { break }
        if ($proc.HasExited) { break }
        Start-Sleep -Seconds 10
    }
    $exitDeadline = (Get-Date).AddSeconds(180)
    while ((Get-Date) -lt $exitDeadline -and -not $proc.HasExited) { Start-Sleep -Seconds 5 }
    if (-not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 3
    }
    $text = Read-SerialShared $serial
    if ($text -notmatch 'NTFS-QUOTA-GROWTH-DONE\tok') {
        throw "stage $S did not finish ok; see $serial"
    }
    $cap = Join-Path $caseOut 'captures'
    Remove-Item $cap -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $cap | Out-Null
    & (Join-Path $Out 'ntfs_raw_dump.exe') $disk $cap
    if ($LASTEXITCODE -ne 0) { throw "dump $S failed" }
    $q = Join-Path $cap 'quota_q_index_root.bin'
    if (-not (Test-Path $q)) { throw "stage $S missing quota_q_index_root.bin" }
    Write-Host ("stage ${S}: $q length=$((Get-Item $q).Length)")
}

Build-Tools
$run = @()
if ($Stage -eq 'All') { $run = @('F', 'T', 'M', 'D') } else { $run = @($Stage) }
foreach ($s in $run) { Invoke-Stage $s }
& (Join-Path $Probe 'host\Diff-QuotaGrowth.ps1') -OutRoot $Out
Write-Host 'probe-v3 quota growth complete — update tavera/specs/ntfs from growth-report.txt'
Write-Host 'Do not edit drivers/filesystems/ntfs in this probe session.'
