# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v9/Invoke-NtfsPagefileProbe.ps1
# PURPOSE:     Boot Windows 10 22H2; run pagefile API probe; capture serial
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

# Captures: output/ntfs/probe-v9/guest-serial.log  Cite tavera/probes/ntfs/probe-v9.
# Marker: NTFS-PAGE-PROBE-DONE. Does not edit drivers/filesystems/ntfs.
# Boot: -accel tcg -m 8192 -smp 4; VHDX snapshot=on (windows-capture.mdc).

param([int]$TimeoutSec = 1200)

$ErrorActionPreference = 'Stop'
$Probe = $PSScriptRoot
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
$Buildenv = (Resolve-Path (Join-Path $SourceRoot '..\buildenv')).Path
$Out = Join-Path $SourceRoot 'output\ntfs\probe-v9'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Qemu = Join-Path $Buildenv 'tools\qemu\qemu-system-x86_64.exe'
$Img = Join-Path (Split-Path $Qemu -Parent) 'qemu-img.exe'
$Gcc = Join-Path $Buildenv 'tools\rosbe\amd64\bin\x86_64-w64-mingw32-gcc.exe'
if (-not (Test-Path $Gcc)) { throw 'MinGW gcc missing' }

$wd = Split-Path $Gcc -Parent
$guestSrc = (Join-Path $Probe 'guest\ntfs_pagefile_probe.c') -replace '\\', '/'
$guestOut = (Join-Path $Out 'ntfs_pagefile_probe.exe') -replace '\\', '/'
$p = Start-Process -FilePath $Gcc -WorkingDirectory $wd -ArgumentList @(
    '-std=c11', '-O1', '-Wall', '-mconsole', $guestSrc, '-o', $guestOut, '-lkernel32', '-ladvapi32'
) -Wait -PassThru -NoNewWindow -RedirectStandardError (Join-Path $Out 'guest-build.err') -RedirectStandardOutput (Join-Path $Out 'guest-build.out')
if ($p.ExitCode -ne 0) { throw "guest build failed: $(Get-Content (Join-Path $Out 'guest-build.err') -Raw)" }

$sys = Join-Path $SourceRoot 'output\ntfs\windows-system.vhdx'
if (-not (Test-Path $sys)) { throw "missing $sys — convert win10-22h2 disk.qcow2 first" }

$disk = Join-Path $Out 'windows-raw.img'
$serial = Join-Path $Out 'guest-serial.log'
$err = Join-Path $Out 'windows-qemu.err'
Remove-Item $disk, $serial, $err -Force -ErrorAction SilentlyContinue
& $Img create -f raw $disk 64M | Out-Null

$regLog = Join-Path $Out 'register-probe.log'
Remove-Item $regLog -Force -ErrorAction SilentlyContinue
Write-Host 'Approve UAC to register the pagefile probe RunOnce.'
$reg = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Probe 'Register-WindowsPagefileProbe.ps1')
)
if ($reg.ExitCode -ne 0 -or -not (Test-Path $regLog) -or (Get-Content $regLog -Raw).Trim() -ne 'ok') {
    throw "register failed: $(Get-Content $regLog -Raw -ErrorAction SilentlyContinue)"
}

function Read-SerialShared([string]$Path) {
    try {
        $fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object IO.StreamReader($fs)
        $t = $sr.ReadToEnd(); $sr.Close(); $fs.Close(); return $t
    } catch { return '' }
}

$vars = Join-Path $Out 'win-ovmf-vars.fd'
Copy-Item (Join-Path $Buildenv 'virtual-machines\win10-22h2\ovmf-vars.fd') $vars -Force
$code = (Join-Path $Buildenv 'tools\qemu\share\edk2-x86_64-code.fd') -replace '\\', '/'
$argList = @(
    '-accel', 'tcg', '-cpu', 'qemu64',
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
Write-Host 'Booting Windows 10 22H2 (tcg). Window: QEMU.'
Write-Host '1. Wait for the desktop (or auto-logon).'
Write-Host '2. RunOnce should start C:\temp\ntfs_pagefile_probe.exe.'
Write-Host '3. If it does not, run that path from an elevated cmd.'
Write-Host '4. Leave the window open until guest-serial.log shows NTFS-PAGE-PROBE-DONE.'
$proc = Start-Process -FilePath $Qemu -ArgumentList $argList -PassThru -RedirectStandardError $err
Start-Sleep -Seconds 5
if ($proc.HasExited) { throw "QEMU exited early; see $err" }

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    if ((Read-SerialShared $serial) -match 'NTFS-PAGE-PROBE-DONE') { break }
    if ($proc.HasExited) { break }
    Start-Sleep -Seconds 10
}
$exitDeadline = (Get-Date).AddSeconds(180)
while ((Get-Date) -lt $exitDeadline -and -not $proc.HasExited) { Start-Sleep -Seconds 5 }
if (-not $proc.HasExited) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
}
$serialText = Read-SerialShared $serial
Set-Content -Path (Join-Path $Out 'pagefile-report.txt') -Value $serialText -Encoding ascii
if ($serialText -notmatch 'NTFS-PAGE-PROBE-DONE\tok') {
    throw "pagefile probe did not finish ok; see $serial"
}
Write-Host 'probe-v9 pagefile complete — update tavera/specs/ntfs from pagefile-report.txt'
Write-Host 'Do not edit drivers/filesystems/ntfs in this probe session.'
