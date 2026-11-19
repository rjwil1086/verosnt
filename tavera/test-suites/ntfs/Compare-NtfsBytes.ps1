# PROJECT: Tavera OS
# LICENSE: GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE: tavera/test-suites/ntfs/Compare-NtfsBytes.ps1
# PURPOSE: Compare NTFS file records with Windows 10 22H2
# COPYRIGHT: Copyright 2026 Ryan Wilson <ryan@verosnt.com>
#              Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@verosnt.com>
#              Ryan Wilson <ryan@taveraos.com>

# The same guest probe formats a 64 MB disk on Windows 10 22H2 and on the
# VerOS live CD. ntfs_bytes prints the on-disk fields that should agree.

param(
    [int]$TimeoutSec = 1200,
    [switch]$WindowsOnly,
    # Suite troubleshooting only — try Hyper-V WHPX first, fall back to TCG.
    [switch]$PreferWhpx
)

$ErrorActionPreference = 'Stop'
$Suite = $PSScriptRoot

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

. (Find-ResolveVerosPaths -Hint $Suite)
$Root = Resolve-VerosBuildenvRoot -Hint $Suite
$Out = Join-Path $Suite 'out'
$Source = Resolve-VerosSourceRoot -BuildenvRoot $Root
$Qemu = Resolve-VerosQemuExe -BuildenvRoot $Root
if (-not $Qemu) { throw 'QEMU was not found under the buildenv tools\qemu folder.' }
$Clang = Join-Path $Root 'tools\clang\bin\clang.exe'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
. (Join-Path $Root 'differential-tests\Set-ProbeRunOnce.ps1')

function Find-MingwGcc {
    $candidates = @(
        (Join-Path $Root 'tools\rosbe\amd64\bin\x86_64-w64-mingw32-gcc.exe'),
        (Join-Path $Root 'tools\rosbe\i386\bin\gcc.exe')
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { return (Resolve-Path -LiteralPath $c).Path }
    }
    $pathGcc = Get-Command x86_64-w64-mingw32-gcc -ErrorAction SilentlyContinue
    if ($pathGcc) { return $pathGcc.Source }
    return $null
}

function Build-Agent {
    $gcc = Find-MingwGcc
    if (-not $gcc) { throw 'MinGW gcc was not found (x86_64-w64-mingw32-gcc)' }
    $exe = Join-Path $Out 'ntfs_agent.exe'
    $src = (Join-Path $Suite 'agent\ntfs_agent.c') -replace '\\', '/'
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

function Build-Bytes {
    $gcc = Find-MingwGcc
    if (-not $gcc) { throw 'MinGW gcc was not found (for ntfs_bytes)' }
    $exe = Join-Path $Out 'ntfs_bytes.exe'
    $src = (Join-Path $Suite 'host\ntfs_bytes.c') -replace '\\', '/'
    $outFwd = $exe -replace '\\', '/'
    $wd = Split-Path -Parent $gcc
    $proc = Start-Process -FilePath $gcc -WorkingDirectory $wd -ArgumentList @(
        '-std=c11', '-O1', '-Wall', $src, '-o', $outFwd
    ) -Wait -PassThru -NoNewWindow `
        -RedirectStandardError (Join-Path $Out 'bytes-build.err') `
        -RedirectStandardOutput (Join-Path $Out 'bytes-build.out')
    if ($proc.ExitCode -ne 0 -or -not (Test-Path $exe)) {
        throw 'ntfs_bytes failed to build'
    }
    return $exe
}

function Start-WindowsDisk {
    param([string]$Disk, [string]$Share, [string]$Agent)
    if (Test-Path $Share) { Remove-Item -Recurse -Force $Share }
    New-Item -ItemType Directory -Force -Path $Share | Out-Null
    Set-Content -Path (Join-Path $Share 'NTFS-SUITE.txt') -Value 'ntfs 3.1' -Encoding ascii
    Set-Content -Path (Join-Path $Share 'phase.txt') -Value 'compliance' -Encoding ascii
    Copy-Item $Agent (Join-Path $Share 'ntfs_agent.exe') -Force
    $img = Join-Path (Split-Path $Qemu -Parent) 'qemu-img.exe'
    if (Test-Path $Disk) { Remove-Item -Force $Disk }
    & $img create -f raw $Disk 64M | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'qemu-img create failed' }

    $sys = Join-Path $Out 'windows-system.vhdx'
    $qcow = Join-Path $Root 'virtual-machines\win10-22h2\disk.qcow2'
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
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Suite 'Register-WindowsProbe.ps1')
    )
    if (-not (Test-Path $regLog) -or (Get-Content $regLog -Raw).Trim() -ne 'ok') {
        $why = 'the elevated registration did not finish'
        if (Test-Path $regLog) { $why = (Get-Content $regLog -Raw).Trim() }
        throw $why
    }
    if ($reg.ExitCode -ne 0) { throw "registration exited $($reg.ExitCode)" }

    $code = Join-Path $Root 'tools\qemu\share\edk2-x86_64-code.fd'
    $vars = Join-Path $Out 'win-ovmf-vars.fd'
    Copy-Item (Join-Path $Root 'virtual-machines\win10-22h2\ovmf-vars.fd') $vars -Force
    $shareFwd = ($Share -replace '\\', '/')
    $err = Join-Path $Out 'windows-qemu.err'
    $common = @(
        '-machine', 'q35',
        '-m', '8192',
        '-smp', '4',
        '-display', 'gtk',
        '-vga', 'std',
        '-no-reboot',
        '-device', 'qemu-xhci',
        '-device', 'usb-tablet',
        '-drive', "if=pflash,format=raw,readonly=on,file=$code",
        '-drive', "if=pflash,format=raw,file=$vars",
        # snapshot=on keeps the registered RunOnce VHDX from guest corruption.
        '-drive', "file=$sys,format=vhdx,if=none,id=hd,snapshot=on",
        '-device', 'virtio-blk-pci,drive=hd,bootindex=1',
        # Blank + vvfat share on IDE — same as Invoke-NtfsSuite Tavera guest.
        '-drive', "file=$Disk,format=raw,if=ide,index=0,media=disk",
        '-drive', "file=fat:rw:${shareFwd},format=raw,if=ide,index=1,media=disk"
    )
    function New-WinArgList([string]$Accel, [string]$Cpu) {
        return @('-accel', $Accel, '-cpu', $Cpu) + $common
    }
    if (Test-Path $err) { Remove-Item $err -Force }
    $accelUsed = 'tcg'
    $argList = New-WinArgList 'tcg' 'qemu64'
    if ($PreferWhpx) {
        $argList = New-WinArgList 'whpx' 'host'
        $accelUsed = 'whpx'
    }
    Write-Host "Booting Windows 10 22H2 ($accelUsed). Window: QEMU."
    Write-Host '1. Wait for the desktop (or auto-logon).'
    Write-Host '2. RunOnce should start C:\temp\ntfs_agent.exe.'
    Write-Host '3. If it does not, open the FAT share disk and run ntfs_agent.exe, or run C:\temp\ntfs_agent.exe.'
    Write-Host '4. Leave the window open until result.log shows NTFS-AGENT-DONE.'
    $proc = Start-Process -FilePath $Qemu -ArgumentList $argList -PassThru -RedirectStandardError $err
    Start-Sleep -Seconds 5
    if ($proc.HasExited -and $PreferWhpx -and $accelUsed -eq 'whpx') {
        $early = ''
        if (Test-Path $err) { $early = [System.IO.File]::ReadAllText($err) }
        Write-Host "WHPX boot failed early; falling back to TCG.`n$early"
        if (Test-Path $err) { Remove-Item $err -Force }
        $accelUsed = 'tcg'
        $argList = New-WinArgList 'tcg' 'qemu64'
        Write-Host "Booting Windows 10 22H2 ($accelUsed). Window: QEMU."
        $proc = Start-Process -FilePath $Qemu -ArgumentList $argList -PassThru -RedirectStandardError $err
        Start-Sleep -Seconds 5
    }
    if ($proc.HasExited) {
        $early = ''
        if (Test-Path $err) { $early = [System.IO.File]::ReadAllText($err) }
        throw "QEMU exited early. See $err`n$early"
    }
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $result = Join-Path $Share 'result.log'
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path $result) -and (Select-String -Path $result -Pattern 'NTFS-AGENT-DONE' -Quiet)) { break }
        if ($proc.HasExited -and (Test-Path $result)) { break }
        Start-Sleep -Seconds 2
    }
    if (-not $proc.HasExited) {
        Start-Sleep -Seconds 5
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
    if (-not (Test-Path $result)) { throw 'Windows did not write result.log' }
    if (-not (Select-String -Path $result -Pattern 'NTFS-AGENT-DONE' -Quiet)) {
        Write-Host 'WARNING: result.log incomplete (no NTFS-AGENT-DONE). Using partial marks.'
    }
}

$agent = Build-Agent
$windowsDisk = Join-Path $Out 'windows.img'
$windowsShare = Join-Path $Out 'share-windows'
Start-WindowsDisk -Disk $windowsDisk -Share $windowsShare -Agent $agent
$winResult = Join-Path $windowsShare 'result.log'
if (-not (Test-Path $winResult)) { throw 'Windows did not write result.log' }
Write-Host '--- Windows result.log ---'
Get-Content $winResult | ForEach-Object { Write-Host $_ }
# Tab-separated red rows: Name<TAB>red
$winRedRows = @(Get-Content $winResult | Where-Object { $_ -match "`tred(\t|$)" })
$winGreenRows = @(Get-Content $winResult | Where-Object { $_ -match "`tgreen(\t|$)" })
Write-Host "Windows marks: $($winGreenRows.Count) green, $($winRedRows.Count) red."
if ($winRedRows.Count -gt 0) {
    Write-Host "Windows baseline has $($winRedRows.Count) red row(s). Fix the suite before blaming Tavera."
    foreach ($row in $winRedRows) { Write-Host "  $row" }
}
if ($WindowsOnly) {
    Write-Host 'WindowsOnly: skipping VerOS boot and byte compare.'
    if ($winRedRows.Count -gt 0) { exit 1 }
    exit 0
}
$bytes = Build-Bytes
Write-Host 'Windows probe finished. Booting the VerOS live CD.'
& (Join-Path $Suite 'Invoke-NtfsSuite.ps1') -Phase compliance -TimeoutSec $TimeoutSec -Iso (Join-Path $Source 'build-amd64\bootcd.iso') -Qemu $Qemu -Clang $Clang
if ($LASTEXITCODE -ne 0) { throw "VerOS NTFS suite exited $LASTEXITCODE" }
$verosDisk = Join-Path $Out 'compliance.img'
$winText = & $bytes $windowsDisk
if ($LASTEXITCODE -ne 0) { throw 'Windows image could not be read' }
$verosText = & $bytes $verosDisk
if ($LASTEXITCODE -ne 0) { throw 'VerOS image could not be read' }
$winMap = @{}
$verosMap = @{}
foreach ($line in $winText) {
    if ($line -like 'FILE *') { $winMap[$line.Substring(5).Split('|')[0]] = $line.Substring(5) }
}
foreach ($line in $verosText) {
    if ($line -like 'FILE *') { $verosMap[$line.Substring(5).Split('|')[0]] = $line.Substring(5) }
}
$differ = 0
Write-Host ($winText | Where-Object { $_ -like 'VOL *' })
Write-Host ($verosText | Where-Object { $_ -like 'VOL *' })
$names = @($winMap.Keys + $verosMap.Keys) | Sort-Object -Unique
foreach ($name in $names) {
    $left = $winMap[$name]
    $right = $verosMap[$name]
    if ($left -ne $right) {
        $differ++
        Write-Host "DIFF $name"
        Write-Host "  Windows: $left"
        Write-Host "  VerOS:   $right"
    }
}
Write-Host "Files compared: $($names.Count). Differences: $differ."
if ($differ -ne 0) { exit 1 }
