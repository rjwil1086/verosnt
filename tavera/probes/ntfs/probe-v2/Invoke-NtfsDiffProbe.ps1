# PROJECT:     Tevera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v2/Invoke-NtfsDiffProbe.ps1
# PURPOSE:     Boot variants A/B, dump captures, write diff-report.txt
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

# Variant A: log create-only; quota 4096/8192; EFS secretA (i*3+1) + secretB (i*5+7)
# Variant B: log create-rename-delete; quota 8192/16384; same EFS patterns
# Does not edit drivers/filesystems/ntfs.

param(
    [int]$TimeoutSec = 1200,
    [ValidateSet('A', 'B', 'Both')][string]$Variant = 'Both'
)

$ErrorActionPreference = 'Stop'
$Probe = $PSScriptRoot
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
$Buildenv = 'C:\tavera\buildenv'
$Out = Join-Path $SourceRoot 'output\ntfs\probe-v2'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Qemu = Join-Path $Buildenv 'tools\qemu\qemu-system-x86_64.exe'
$Img = Join-Path (Split-Path $Qemu -Parent) 'qemu-img.exe'
$Gcc = Get-ChildItem -Path (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages') -Filter 'x86_64-w64-mingw32-gcc.exe' -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
$hostGcc = Join-Path (Split-Path $Gcc) 'gcc.exe'
if (-not $Gcc) { throw 'MinGW gcc missing' }

function Build-Tools {
    & $Gcc -std=c11 -O1 -Wall -mconsole (Join-Path $Probe 'guest\ntfs_diff_probe.c') -o (Join-Path $Out 'ntfs_diff_probe.exe') -lkernel32 -ladvapi32
    if ($LASTEXITCODE -ne 0) { throw 'guest build failed' }
    & $hostGcc -std=c11 -O1 -Wall (Join-Path $Probe 'host\ntfs_raw_dump.c') -o (Join-Path $Out 'ntfs_raw_dump.exe')
    if ($LASTEXITCODE -ne 0) { throw 'dump build failed' }
}

function Read-SerialShared([string]$Path) {
    try {
        $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object IO.StreamReader($fs)
        $t = $sr.ReadToEnd(); $sr.Close(); $fs.Close(); return $t
    } catch { return '' }
}

function Invoke-Variant([string]$V) {
    $caseOut = Join-Path $Out $V
    New-Item -ItemType Directory -Force -Path $caseOut | Out-Null
    $disk = Join-Path $caseOut 'windows-raw.img'
    $serial = Join-Path $caseOut 'guest-serial.log'
    $err = Join-Path $caseOut 'windows-qemu.err'
    Remove-Item $disk, $serial, $err -Force -ErrorAction SilentlyContinue
    & $Img create -f raw $disk 64M | Out-Null

    $regLog = Join-Path $Out 'register-probe.log'
    Remove-Item $regLog -Force -ErrorAction SilentlyContinue
    $reg = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Probe 'Register-WindowsDiffProbe.ps1'),
        '-Variant', $V
    )
    if ($reg.ExitCode -ne 0 -or (Get-Content $regLog -Raw).Trim() -ne 'ok') {
        throw "register $V failed: $(Get-Content $regLog -Raw -ErrorAction SilentlyContinue)"
    }

    $sys = Join-Path $Out 'windows-system.vhdx'
    $vars = Join-Path $caseOut 'win-ovmf-vars.fd'
    Copy-Item (Join-Path $Buildenv 'virtual-machines\win10-22h2\ovmf-vars.fd') $vars -Force
    $code = (Join-Path $Buildenv 'tools\qemu\share\edk2-x86_64-code.fd') -replace '\\', '/'
    $argList = @(
        '-accel', 'tcg', '-machine', 'q35', '-cpu', 'qemu64', '-m', '8192', '-smp', '4',
        '-display', 'gtk', '-vga', 'qxl', '-no-reboot',
        '-serial', "file:$(($serial -replace '\\', '/'))",
        '-device', 'qemu-xhci', '-device', 'usb-tablet',
        '-drive', "if=pflash,format=raw,readonly=on,file=$code",
        '-drive', "if=pflash,format=raw,file=$(($vars -replace '\\', '/'))",
        '-drive', "file=$(($sys -replace '\\', '/')),format=vhdx,if=none,id=hd,snapshot=on",
        '-device', 'virtio-blk-pci,drive=hd,bootindex=1',
        '-drive', "file=$(($disk -replace '\\', '/')),format=raw,if=none,id=blank",
        '-device', 'virtio-blk-pci,drive=blank'
    )
    Write-Host "Booting variant $V"
    $proc = Start-Process -FilePath $Qemu -ArgumentList $argList -PassThru -RedirectStandardError $err
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ((Read-SerialShared $serial) -match 'NTFS-DIFF-PROBE-DONE') { break }
        if ($proc.HasExited) { break }
        Start-Sleep -Seconds 15
    }
    $exitDeadline = (Get-Date).AddSeconds(180)
    while ((Get-Date) -lt $exitDeadline -and -not $proc.HasExited) { Start-Sleep -Seconds 5 }
    if (-not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 3
    }
    $text = Read-SerialShared $serial
    if ($text -notmatch 'NTFS-DIFF-PROBE-DONE\tok') {
        throw "variant $V did not finish ok; see $serial"
    }
    $cap = Join-Path $caseOut 'captures'
    Remove-Item $cap -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $cap | Out-Null
    & (Join-Path $Out 'ntfs_raw_dump.exe') $disk $cap
    if ($LASTEXITCODE -ne 0) { throw "dump $V failed" }
    Write-Host "variant $V captures in $cap"
}

Build-Tools
$run = @()
if ($Variant -eq 'Both') { $run = @('A', 'B') } else { $run = @($Variant) }
foreach ($v in $run) { Invoke-Variant $v }
if ((Test-Path (Join-Path $Out 'A\captures')) -and (Test-Path (Join-Path $Out 'B\captures'))) {
    & (Join-Path $Probe 'host\Diff-NtfsCaptures.ps1') -OutRoot $Out
}
Write-Host 'probe-v2 differential complete — update tavera/specs/ntfs from diff-report.txt'
Write-Host 'Do not edit drivers/filesystems/ntfs in this probe session.'
