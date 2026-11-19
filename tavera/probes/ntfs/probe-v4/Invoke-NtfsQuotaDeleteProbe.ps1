# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v4/Invoke-NtfsQuotaDeleteProbe.ps1
# PURPOSE:     Boot M/Q stages, dump $Q, write delete-report.txt
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

# Stages: M=track+modify; Q=M+NtSetQuotaInformationFile QuotaLimit=-2
# Captures: output/ntfs/probe-v4/<stage>/  Cite tavera/probes/ntfs/probe-v4.
# Does not edit drivers/filesystems/ntfs.

param(
    [int]$TimeoutSec = 1200,
    [ValidateSet('M', 'Q', 'All')][string]$Stage = 'All',
    [switch]$PreferWhpx
)

$ErrorActionPreference = 'Stop'
$Probe = $PSScriptRoot
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
$Buildenv = (Resolve-Path (Join-Path $SourceRoot '..\buildenv')).Path
$Out = Join-Path $SourceRoot 'output\ntfs\probe-v4'
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
    $guestSrc = (Join-Path $Probe 'guest\ntfs_quota_delete_probe.c') -replace '\\', '/'
    $guestOut = (Join-Path $Out 'ntfs_quota_delete_probe.exe') -replace '\\', '/'
    $p = Start-Process -FilePath $Gcc -WorkingDirectory $wd -ArgumentList @(
        '-std=c11', '-O1', '-Wall', '-mconsole', $guestSrc, '-o', $guestOut, '-lkernel32', '-ladvapi32'
    ) -Wait -PassThru -NoNewWindow -RedirectStandardError (Join-Path $Out 'guest-build.err') -RedirectStandardOutput (Join-Path $Out 'guest-build.out')
    if ($p.ExitCode -ne 0) { throw "guest build failed: $(Get-Content (Join-Path $Out 'guest-build.err') -Raw)" }
    $dumpOut = Join-Path $Out 'ntfs_raw_dump.exe'
    & $hostGcc -std=c11 -O1 -Wall (Join-Path $Probe 'host\ntfs_raw_dump.c') -o $dumpOut 2>$null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dumpOut)) {
        foreach ($fb in @(
            (Join-Path $SourceRoot 'output\ntfs\probe-v3\ntfs_raw_dump.exe'),
            (Join-Path $SourceRoot 'output\ntfs\probe-v2\ntfs_raw_dump.exe')
        )) {
            if (Test-Path $fb) { Copy-Item $fb $dumpOut -Force; Write-Host "Reused $fb"; return }
        }
        throw 'dump build failed'
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
    foreach ($c in @(
        (Join-Path $SourceRoot 'output\ntfs\probe-v3\windows-system.vhdx'),
        (Join-Path $SourceRoot 'output\ntfs\probe-v2\windows-system.vhdx'),
        (Join-Path $SourceRoot 'tavera\test-suites\ntfs\out\windows-system.vhdx')
    )) {
        if (Test-Path $c) {
            # Prefer hardlink/junction-free copy only if missing — use existing path via junction? Copy is slow.
            # Create a directory junction-style by copying once from newest small metadata… use Copy-Item.
            Write-Host "Linking Windows VHDX from $c (Copy-Item)"
            Copy-Item $c $sys
            return $sys
        }
    }
    throw 'windows-system.vhdx not found; run probe-v2/v3 once or convert qcow2'
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
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Probe 'Register-WindowsQuotaDeleteProbe.ps1'),
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
        Write-Host "WHPX failed early; falling back to TCG."
        Remove-Item $err -Force -EA SilentlyContinue
        $argList = @('-accel', 'tcg', '-cpu', 'qemu64') + $common
        $proc = Start-Process -FilePath $Qemu -ArgumentList $argList -PassThru -RedirectStandardError $err
        Start-Sleep -Seconds 5
    }
    if ($proc.HasExited) { throw "QEMU exited early for $S. $(Get-Content $err -Raw -EA SilentlyContinue)" }

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ((Read-SerialShared $serial) -match 'NTFS-QUOTA-DELETE-DONE') { break }
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
    if ($text -notmatch 'NTFS-QUOTA-DELETE-DONE\tok') {
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
if ($Stage -eq 'All') { $run = @('M', 'Q') } else { $run = @($Stage) }
foreach ($s in $run) { Invoke-Stage $s }
& (Join-Path $Probe 'host\Diff-QuotaDelete.ps1') -OutRoot $Out
Write-Host 'probe-v4 quota delete complete — update tavera/specs/ntfs from delete-report.txt'
Write-Host 'Do not edit drivers/filesystems/ntfs in this probe session.'
