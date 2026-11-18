# PROJECT:     Tevera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v1/Invoke-NtfsRawProbe.ps1
# PURPOSE:     Boot Windows 10 22H2, run the NTFS raw probe, dump captures
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

# Raw-format probe session. Writes output/ntfs/probe-v1/ captures for
# tavera/specs/ntfs/ (cite tavera/probes/ntfs/probe-v1 for each capture).
# Does not edit drivers/filesystems/ntfs.

param(
    [int]$TimeoutSec = 1200
)

$ErrorActionPreference = 'Stop'
$Probe = $PSScriptRoot
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
$Buildenv = (Resolve-Path (Join-Path $SourceRoot '..\buildenv')).Path
. (Join-Path $SourceRoot 'tools\pipeline\Resolve-VerosPaths.ps1')
$Buildenv = Resolve-VerosBuildenvRoot -SourceRoot $SourceRoot
$Qemu = Resolve-VerosQemuExe -BuildenvRoot $Buildenv
if (-not $Qemu) { throw 'QEMU was not found under the buildenv tools\qemu folder.' }
$Clang = Join-Path $Buildenv 'tools\clang\bin\clang.exe'
$Out = Join-Path $SourceRoot 'output\ntfs\probe-v1'
New-Item -ItemType Directory -Force -Path $Out | Out-Null

function Build-Guest {
    $kitRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    $ver = Get-ChildItem (Join-Path $kitRoot 'Include') | Sort-Object Name -Descending | Select-Object -First 1
    if (-not $ver) { throw 'Windows SDK was not found' }
    $exe = Join-Path $Out 'ntfs_raw_probe.exe'
    $src = Join-Path $Probe 'guest\ntfs_raw_probe.c'
    $args = @(
        '-target', 'x86_64-pc-windows-msvc', '-std=c11', '-O1', '-Wall',
        '-Wno-unused-function', '-Wno-unused-variable', '-fno-builtin',
        '-fno-stack-protector', '-mno-stack-arg-probe', '-nostdlib', '-fuse-ld=lld',
        '-Wl,-entry:MainCRTStartup', '-Wl,-subsystem:console',
        '-isystem', (Join-Path $kitRoot "Include\$($ver.Name)\ucrt"),
        '-isystem', (Join-Path $kitRoot "Include\$($ver.Name)\shared"),
        '-isystem', (Join-Path $kitRoot "Include\$($ver.Name)\um"),
        '-L', (Join-Path $kitRoot "Lib\$($ver.Name)\um\x64"),
        '-lkernel32', '-ladvapi32', $src, '-o', $exe
    )
    & $Clang @args
    if ($LASTEXITCODE -ne 0) { throw 'ntfs_raw_probe failed to build' }
    return $exe
}

function Build-Dump {
    $exe = Join-Path $Out 'ntfs_raw_dump.exe'
    & $Clang -std=c11 -O1 -Wall (Join-Path $Probe 'host\ntfs_raw_dump.c') -o $exe
    if ($LASTEXITCODE -ne 0) { throw 'ntfs_raw_dump failed to build' }
    return $exe
}

function Start-WindowsRawProbe {
    param([string]$Disk, [string]$Share, [string]$Agent)

    if (Test-Path $Share) { Remove-Item -Recurse -Force $Share }
    New-Item -ItemType Directory -Force -Path $Share | Out-Null
    Set-Content -Path (Join-Path $Share 'NTFS-RAW.txt') -Value 'ntfs raw probe' -Encoding ascii
    Copy-Item $Agent (Join-Path $Share 'ntfs_raw_probe.exe') -Force

    $img = Join-Path (Split-Path $Qemu -Parent) 'qemu-img.exe'
    if (Test-Path $Disk) { Remove-Item -Force $Disk }
    & $img create -f raw $Disk 256M | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'qemu-img create failed' }

    $sys = Join-Path $Out 'windows-system.vhdx'
    $qcow = Join-Path $Buildenv 'virtual-machines\win10-22h2\disk.qcow2'
    if (-not (Test-Path $sys)) {
        Write-Host 'Converting the Windows disk so RunOnce can be registered offline'
        & $img convert -O vhdx $qcow $sys
        if ($LASTEXITCODE -ne 0) { throw 'qemu-img convert failed' }
        & fsutil sparse setflag $sys 0 | Out-Null
        & compact /u $sys | Out-Null
    }

    $regLog = Join-Path $Out 'register-probe.log'
    if (Test-Path $regLog) { Remove-Item -Force $regLog }
    Write-Host 'Approve the administrator prompt to register the logon probe.'
    $reg = Start-Process -FilePath powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Probe 'Register-WindowsRawProbe.ps1')
    )
    if (-not (Test-Path $regLog) -or (Get-Content $regLog -Raw).Trim() -ne 'ok') {
        $why = 'the elevated registration did not finish'
        if (Test-Path $regLog) { $why = (Get-Content $regLog -Raw).Trim() }
        throw $why
    }
    if ($reg.ExitCode -ne 0) { throw "registration exited $($reg.ExitCode)" }

    $code = Join-Path $Buildenv 'tools\qemu\share\edk2-x86_64-code.fd'
    $vars = Join-Path $Out 'win-ovmf-vars.fd'
    Copy-Item (Join-Path $Buildenv 'virtual-machines\win10-22h2\ovmf-vars.fd') $vars -Force
    $shareFwd = ($Share -replace '\\', '/')
    $diskFwd = ($Disk -replace '\\', '/')
    $sysFwd = ($sys -replace '\\', '/')
    $codeFwd = ($code -replace '\\', '/')
    $varsFwd = ($vars -replace '\\', '/')
    $err = Join-Path $Out 'windows-qemu.err'
    # Capture boots use TCG per windows-capture.mdc (not WHPX).
    $argList = @(
        '-accel', 'tcg',
        '-machine', 'q35',
        '-cpu', 'qemu64',
        '-m', '8192',
        '-smp', '4',
        '-display', 'gtk',
        '-vga', 'qxl',
        '-no-reboot',
        '-device', 'qemu-xhci',
        '-device', 'usb-tablet',
        '-drive', "if=pflash,format=raw,readonly=on,file=$codeFwd",
        '-drive', "if=pflash,format=raw,file=$varsFwd",
        '-drive', "file=$sysFwd,format=vhdx,if=none,id=hd,snapshot=on",
        '-device', 'virtio-blk-pci,drive=hd,bootindex=1',
        '-drive', "file=$diskFwd,format=raw,if=none,id=blank",
        '-device', 'virtio-blk-pci,drive=blank',
        '-drive', "file=fat:rw:${shareFwd},format=raw,if=none,id=share",
        '-device', 'virtio-blk-pci,drive=share'
    )
    Write-Host 'Booting Windows 10 22H2 (TCG). Logon runs C:\temp\ntfs_raw_probe.exe.'
    Write-Host 'Window: QEMU. Ready when the guest reaches the desktop / RunOnce probe.'
    if (Test-Path $err) { Remove-Item $err -Force }
    $proc = Start-Process -FilePath $Qemu -ArgumentList $argList -PassThru -RedirectStandardError $err
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $result = Join-Path $Share 'result.log'
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path $result) -and (Select-String -Path $result -Pattern 'NTFS-RAW-PROBE-DONE' -Quiet)) { break }
        if ($proc.HasExited -and (Test-Path $result)) { break }
        Start-Sleep -Seconds 2
    }
    if (-not $proc.HasExited) {
        Start-Sleep -Seconds 5
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
    if (-not (Test-Path $result)) { throw 'Windows did not write result.log' }
    if (-not (Select-String -Path $result -Pattern 'NTFS-RAW-PROBE-DONE\tok' -Quiet)) {
        throw "probe did not finish ok; see $result"
    }
}

$dump = Build-Dump
$agent = Build-Guest
$disk = Join-Path $Out 'windows-raw.img'
$share = Join-Path $Out 'share-windows'
Start-WindowsRawProbe -Disk $disk -Share $share -Agent $agent
$cap = Join-Path $Out 'captures'
if (Test-Path $cap) { Remove-Item -Recurse -Force $cap }
New-Item -ItemType Directory -Force -Path $cap | Out-Null
& $dump $disk $cap
if ($LASTEXITCODE -ne 0) { throw 'ntfs_raw_dump failed' }
Write-Host "Captures in $cap — extend tavera/specs/ntfs/*.md citing manifest.txt and the BLOB lines."
Write-Host 'Do not edit drivers/filesystems/ntfs in this probe session.'
