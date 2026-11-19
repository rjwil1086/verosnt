# PROJECT:     Tavera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/amd64-logical-processors/probe-v1/Invoke-TopologyProbe.ps1
# PURPOSE:     Boot Windows 10 22H2 three SMP shapes and capture topology probe logs
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

# Spec/probe session only. Writes output/amd64-logical-processors/probe-v1/.
# Does not edit ntoskrnl/ or hal/. Capture boots use TCG per windows-capture.mdc.

param(
    [int]$TimeoutSec = 1800,
    [switch]$SkipConvert
)

$ErrorActionPreference = 'Stop'
$Probe = $PSScriptRoot
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
. (Join-Path $SourceRoot 'tools\pipeline\Resolve-VerosPaths.ps1')
$Buildenv = Resolve-VerosBuildenvRoot -SourceRoot $SourceRoot
$Qemu = Resolve-VerosQemuExe -SourceRoot $SourceRoot
if (-not $Qemu) { throw 'QEMU was not found under the buildenv tools\qemu folder.' }
$Out = Join-Path $SourceRoot 'output\amd64-logical-processors\probe-v1'
New-Item -ItemType Directory -Force -Path $Out | Out-Null

function Build-Probe {
    $gcc = Join-Path $Buildenv 'tools\gcc\bin\x86_64-w64-mingw32-gcc.exe'
    if (-not (Test-Path $gcc)) {
        $rosbe = Resolve-VerosRosbeRoot -SourceRoot $SourceRoot
        if ($rosbe) {
            $gcc = Join-Path $rosbe 'amd64\bin\x86_64-w64-mingw32-gcc.exe'
        }
    }
    if (-not (Test-Path $gcc)) { throw "mingw gcc was not found under $Buildenv\tools\gcc" }
    $env:PATH = "$(Split-Path $gcc -Parent);" + $env:PATH
    $exe = Join-Path $Out 'topology_probe.exe'
    $src = Join-Path $Probe 'guest\topology_probe.c'
    & $gcc -std=c99 -Wall -Wextra -O2 -o $exe $src
    if ($LASTEXITCODE -ne 0) { throw 'topology_probe build failed' }
    return $exe
}

function Ensure-WindowsVhd {
    # Prefer the existing Windows-to-Go VHDX used by syscall-diff (offline
    # RunOnce + snapshot boot). Fall back to converting disk.qcow2 once.
    $wtg = Join-Path $Buildenv 'virtual-machines\win10-22h2\wtg.vhdx'
    if (Test-Path $wtg) {
        Write-Host "Using $wtg"
        return $wtg
    }
    $sys = Join-Path $Out 'windows-system.vhdx'
    $qcow = Join-Path $Buildenv 'virtual-machines\win10-22h2\disk.qcow2'
    if (-not (Test-Path $qcow)) { throw "Windows disk was not found at $qcow" }
    if ($SkipConvert -and (Test-Path $sys)) { return $sys }
    if (Test-Path $sys) {
        Write-Host "Reusing $sys"
        return $sys
    }
    $img = Join-Path (Split-Path $Qemu -Parent) 'qemu-img.exe'
    Write-Host 'Converting the Windows disk for offline RunOnce registration'
    & $img convert -O vhdx $qcow $sys
    if ($LASTEXITCODE -ne 0) { throw 'qemu-img convert failed' }
    & fsutil sparse setflag $sys 0 | Out-Null
    & compact /u $sys | Out-Null
    return $sys
}

function Register-Shape {
    param([string]$Vhd, [string]$Exe, [string]$Shape)
    $regLog = Join-Path $Out "register-$Shape.log"
    if (Test-Path $regLog) { Remove-Item -Force $regLog }
    Write-Host "Approve the administrator prompt to register RunOnce for shape $Shape."
    $reg = Start-Process -FilePath powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', (Join-Path $Probe 'Register-WindowsTopologyProbe.ps1'),
        '-Vhd', $Vhd, '-Exe', $Exe, '-Shape', $Shape, '-Log', $regLog
    )
    if (-not (Test-Path $regLog) -or (Get-Content $regLog -Raw).Trim() -ne 'ok') {
        $why = 'the elevated registration did not finish'
        if (Test-Path $regLog) { $why = (Get-Content $regLog -Raw).Trim() }
        throw $why
    }
    if ($reg.ExitCode -ne 0) { throw "registration exited $($reg.ExitCode)" }
}

function Start-ShapeBoot {
    param(
        [string]$Vhd,
        [string]$Shape,
        [string]$LogName,
        [string[]]$CpuSmpArgs
    )

    $log = Join-Path $Out $LogName
    if (Test-Path $log) { Remove-Item -Force $log }
    $serial = ($log -replace '\\', '/')
    $code = Join-Path $Buildenv 'tools\qemu\share\edk2-x86_64-code.fd'
    $vars = Join-Path $Out "ovmf-vars-$Shape.fd"
    Copy-Item (Join-Path $Buildenv 'tools\qemu\share\edk2-i386-vars.fd') $vars -Force
    $sysFwd = ($Vhd -replace '\\', '/')
    $codeFwd = ($code -replace '\\', '/')
    $varsFwd = ($vars -replace '\\', '/')
    $err = Join-Path $Out "windows-$Shape.err"
    if (Test-Path $err) { Remove-Item -Force $err }

    $argList = @(
        '-accel', 'tcg',
        '-machine', 'q35'
    ) + $CpuSmpArgs + @(
        '-m', '8192',
        '-display', 'gtk',
        '-vga', 'qxl',
        '-device', 'qemu-xhci',
        '-device', 'usb-tablet',
        '-serial', "file:$serial",
        '-drive', "if=pflash,format=raw,readonly=on,file=$codeFwd",
        '-drive', "if=pflash,format=raw,file=$varsFwd",
        '-drive', "file=$sysFwd,format=vhdx,if=none,id=hd,snapshot=on",
        '-device', 'virtio-blk-pci,drive=hd,bootindex=1'
    )

    Write-Host ""
    Write-Host "=== Shape $Shape ==="
    Write-Host "1. Window: QEMU (Windows 10 22H2 topology probe)."
    Write-Host "2. Ready when the guest reaches the desktop and RunOnce starts topology_probe.exe."
    Write-Host "3. Report back when the serial log contains DIFF END OK (script also waits)."
    Write-Host "Log: $log"
    Write-Host "QEMU args: $($CpuSmpArgs -join ' ')"

    $proc = Start-Process -FilePath $Qemu -ArgumentList $argList -PassThru -RedirectStandardError $err
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path $log) -and (Select-String -LiteralPath $log -Pattern 'DIFF END OK' -Quiet)) {
            break
        }
        if ($proc.HasExited) { break }
        Start-Sleep -Seconds 5
    }
    if (-not $proc.HasExited) {
        Start-Sleep -Seconds 3
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
    }
    if (-not (Test-Path $log) -or -not (Select-String -LiteralPath $log -Pattern 'DIFF END OK' -Quiet)) {
        throw "Shape $Shape did not print DIFF END OK; see $log and $err"
    }
    Write-Host "Shape $Shape captured."
    return $log
}

$exe = Build-Probe
$vhd = Ensure-WindowsVhd

$shapes = @(
    @{
        Shape = '4c1t'
        Log = 'windows-4c1t.log'
        CpuSmp = @('-cpu', 'qemu64', '-smp', '4,sockets=1,cores=4,threads=1')
    },
    @{
        Shape = 'qemu64-2c2t'
        Log = 'windows-qemu64-2c2t.log'
        CpuSmp = @('-cpu', 'qemu64', '-smp', '4,sockets=1,cores=2,threads=2')
    },
    @{
        Shape = 'epyc-2c2t'
        Log = 'windows-epyc-2c2t.log'
        CpuSmp = @('-cpu', 'EPYC', '-smp', '4,sockets=1,cores=2,threads=2')
    }
)

foreach ($s in $shapes) {
    Register-Shape -Vhd $vhd -Exe $exe -Shape $s.Shape
    Start-ShapeBoot -Vhd $vhd -Shape $s.Shape -LogName $s.Log -CpuSmpArgs $s.CpuSmp
}

Write-Host ""
Write-Host "All three Windows captures are under $Out"
Write-Host 'Update tavera/specs/amd64-logical-processors/ citing these logs. Do not edit ntoskrnl/hal in this session.'
