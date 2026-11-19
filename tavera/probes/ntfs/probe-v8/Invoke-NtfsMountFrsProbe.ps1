# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v8/Invoke-NtfsMountFrsProbe.ps1
# PURPOSE:     Dump chkdsk-named FRS from kept disk for NTFS-MOUNT-001
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

# Captures: output/ntfs/probe-v8/  Cite tavera/probes/ntfs/probe-v8.
# Does not edit drivers/filesystems/ntfs.

param(
    [string]$Image = '',
    [string[]]$Frs = @('0x17', '0x7C7', '0x7D3')
)

$ErrorActionPreference = 'Stop'
$Probe = $PSScriptRoot
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
$Buildenv = (Resolve-Path (Join-Path $SourceRoot '..\buildenv')).Path
$Out = Join-Path $SourceRoot 'output\ntfs\probe-v8'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Gcc = Join-Path $Buildenv 'tools\rosbe\amd64\bin\gcc.exe'
if (-not (Test-Path $Gcc)) {
    $Gcc = Join-Path $Buildenv 'tools\rosbe\amd64\bin\x86_64-w64-mingw32-gcc.exe'
}
$QemuImg = Join-Path $Buildenv 'tools\qemu\qemu-img.exe'

if (-not $Image) {
    $Image = Join-Path $Out 'kept-disk.raw'
    $src = Join-Path $Buildenv 'virtual-machines\veros-ntfs\disk.qcow2'
    if (-not (Test-Path $Image) -or (Get-Item $Image).Length -lt 1GB) {
        Write-Host "Converting $src -> $Image"
        & $QemuImg convert -p -O raw $src $Image
        if ($LASTEXITCODE -ne 0) { throw 'qemu-img convert failed' }
    }
}

$dump = Join-Path $Out 'ntfs_mount_frs_dump.exe'
$srcC = (Join-Path $Probe 'host\ntfs_mount_frs_dump.c') -replace '\\', '/'
$outE = $dump -replace '\\', '/'
$p = Start-Process -FilePath $Gcc -WorkingDirectory (Split-Path $Gcc) -ArgumentList @(
    '-std=c11', '-O2', '-Wall', $srcC, '-o', $outE
) -Wait -PassThru -NoNewWindow -RedirectStandardError (Join-Path $Out 'build.err') -RedirectStandardOutput (Join-Path $Out 'build.out')
if ($p.ExitCode -ne 0) { throw "build failed: $(Get-Content (Join-Path $Out 'build.err') -Raw)" }

$cap = Join-Path $Out 'captures'
Remove-Item $cap -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $cap | Out-Null
& $dump $Image $cap @Frs
if ($LASTEXITCODE -ne 0) { throw 'dump failed' }
Copy-Item (Join-Path $cap 'mount-frs-report.txt') (Join-Path $Out 'mount-frs-report.txt') -Force
Write-Host 'probe-v8 mount-frs complete — update tavera/specs/ntfs from mount-frs-report.txt'
Get-Content (Join-Path $Out 'mount-frs-report.txt')
