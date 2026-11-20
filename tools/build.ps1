<#
.SYNOPSIS
    Configures, builds and optionally tests waku in one step.

.DESCRIPTION
    Finds Visual Studio with vswhere, loads the MSVC environment into this process (no separate
    "activate" step), sets VCPKG_ROOT after it (vcvars points it at the bundled vcpkg, which cannot
    resolve the manifest's baseline) and, when the toolset that cmake\triplets pins is installed,
    selects that toolset and passes the overlay triplet to CMake. Without the pinned toolset it
    builds with the default one and warns when several are installed (the __std_rotate link error).

    The build folders are those of the presets: build\x64-release, build\x64-relwithdebinfo,
    build\x64-debug (arm64 likewise). Each run configures first; with nothing changed that is quick.

.EXAMPLE
    .\tools\build.ps1 rel
.EXAMPLE
    .\tools\build.ps1 relWithDebInfo -Test
.EXAMPLE
    .\tools\build.ps1 rel -Test -Filter 'Tiling|ShellHost'
.EXAMPLE
    .\tools\build.ps1 rel -Target waku -Clean
#>
[CmdletBinding()]
param(
    # rel = Release, relWithDebInfo = Release with a PDB (for crash dumps), debug = Debug.
    [Parameter(Position = 0)]
    [ValidateSet('rel', 'relWithDebInfo', 'debug')]
    [string]$Config = 'rel',

    # Target architecture; defaults to the machine's own.
    [ValidateSet('x64', 'arm64')]
    [string]$Arch = $(if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }),

    # Run ctest after the build.
    [switch]$Test,

    # Only the ctest suites whose name matches this regular expression (ctest -R).
    [string]$Filter,

    # Build one target (waku, wakuc, wakukeys, waku_tests, deploy) instead of all.
    [string]$Target,

    # Delete the build folder first.
    [switch]$Clean,

    # Build with the default toolset and the stock triplet even when the pinned one is installed.
    [switch]$NoOverlay,

    # The vcpkg checkout with a ports tree.
    [string]$VcpkgRoot = $(if ($env:VCPKG_ROOT_OVERRIDE) { $env:VCPKG_ROOT_OVERRIDE } else { 'C:\vcpkg' }),

    # A Visual Studio installation folder, when vswhere should not pick the newest one.
    [string]$VsPath
)

$ErrorActionPreference = 'Stop'

# The repository: the first folder at or above this script, or else the current folder, that holds
# CMakePresets.json. So the script works from the repo root, from tools\, or from anywhere in it.
function Find-Repo([string]$Start) {
    $dir = $Start
    while ($dir) {
        if (Test-Path (Join-Path $dir 'CMakePresets.json')) { return $dir }
        $parent = Split-Path $dir -Parent
        if ($parent -eq $dir) { break }
        $dir = $parent
    }
    return $null
}
$repo = Find-Repo $PSScriptRoot
if (-not $repo) { $repo = Find-Repo (Get-Location).Path }
if (-not $repo) { throw "no CMakePresets.json at or above $PSScriptRoot or $(Get-Location): run it inside the waku repository" }

function Invoke-Step([string]$Exe, [string[]]$Arguments) {
    Write-Host "> $Exe $($Arguments -join ' ')" -ForegroundColor Cyan
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

# --- configuration ------------------------------------------------------------------------------
$buildType = @{ rel = 'Release'; relWithDebInfo = 'RelWithDebInfo'; debug = 'Debug' }[$Config]
$buildDir = Join-Path $repo "build\$Arch-$($buildType.ToLower())"
$preset = "$Arch-release"  # the preset's triplet and Qt folder; the build type and folder are set below

# --- Visual Studio ------------------------------------------------------------------------------
if (-not $VsPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw "vswhere not found at $vswhere; pass -VsPath" }
    $VsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
        Select-Object -First 1
    if (-not $VsPath) { throw 'no Visual Studio with the C++ tools found; pass -VsPath' }
}
$vcvarsall = Join-Path $VsPath 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path $vcvarsall)) { throw "vcvarsall.bat not found under $VsPath" }
$toolsets = @(Get-ChildItem (Join-Path $VsPath 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue)

# --- the pinned toolset and the overlay triplet ------------------------------------------------
$overlayDir = Join-Path $repo 'cmake\triplets'
$tripletFile = Join-Path $overlayDir "$Arch-windows-static-md.cmake"
$pinned = $null
if (-not $NoOverlay -and (Test-Path $tripletFile)) {
    $match = Select-String -Path $tripletFile -Pattern 'VCPKG_PLATFORM_TOOLSET_VERSION\s+([\d.]+)' |
        Select-Object -First 1
    if ($match) {
        $version = $match.Matches[0].Groups[1].Value
        if ($toolsets | Where-Object { $_.Name -like "$version*" }) {
            $pinned = $version
        } else {
            Write-Warning "cmake\triplets pins MSVC $version, which is not installed in $VsPath; building with the default toolset and the stock triplet."
        }
    }
}
if (-not $pinned -and $toolsets.Count -gt 1) {
    Write-Warning "$($toolsets.Count) MSVC toolsets are installed ($($toolsets.Name -join ', ')) and none is pinned: vcpkg may build its packages with another one than waku, and waku_tests then fails to link (__std_rotate)."
}

# --- MSVC environment into this process ---------------------------------------------------------
$hostArm = $env:PROCESSOR_ARCHITECTURE -eq 'ARM64'
$vcArch = switch ("$hostArm-$Arch") {
    'True-x64' { 'arm64_amd64' }
    'True-arm64' { 'arm64' }
    'False-arm64' { 'x64_arm64' }
    default { 'x64' }
}
$vcArgs = $vcArch
if ($pinned) { $vcArgs += " -vcvars_ver=$pinned" }
# A temporary .cmd keeps the quoting of the path away from PowerShell's native argument rules.
$script = Join-Path ([IO.Path]::GetTempPath()) "waku-vcvars-$PID.cmd"
Set-Content -Path $script -Encoding Ascii -Value @(
    "@call `"$vcvarsall`" $vcArgs >nul 2>&1",
    '@if errorlevel 1 exit /b 1',
    '@set'
)
try {
    $lines = & cmd.exe /d /c $script
    if ($LASTEXITCODE -ne 0) { throw "vcvarsall.bat $vcArgs failed" }
} finally {
    Remove-Item $script -ErrorAction SilentlyContinue
}
foreach ($line in $lines) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
# vcvars points VCPKG_ROOT at the bundled vcpkg: set it afterwards.
if (-not (Test-Path (Join-Path $VcpkgRoot 'vcpkg.exe'))) { throw "no vcpkg.exe in $VcpkgRoot; pass -VcpkgRoot" }
$env:VCPKG_ROOT = $VcpkgRoot

$compiler = if ($env:VCToolsVersion) { $env:VCToolsVersion } else { 'unknown' }
Write-Host "waku $Config ($buildType) for $Arch with MSVC $compiler$(if ($pinned) { ' and the overlay triplet' })" -ForegroundColor Green
Write-Host "  build folder: $buildDir"

# --- configure, build, test ---------------------------------------------------------------------
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "> removing $buildDir" -ForegroundColor Cyan
    Remove-Item $buildDir -Recurse -Force
}

Push-Location $repo
try {
    $configure = @('--preset', $preset, '-B', $buildDir, "-DCMAKE_BUILD_TYPE=$buildType")
    # Set or cleared every time, so a value left in the cache by an earlier run cannot linger.
    if ($pinned) { $configure += "-DVCPKG_OVERLAY_TRIPLETS=$overlayDir" } else { $configure += '-DVCPKG_OVERLAY_TRIPLETS=' }
    Invoke-Step 'cmake' $configure

    $build = @('--build', $buildDir)
    if ($Target) { $build += @('--target', $Target) }
    Invoke-Step 'cmake' $build

    if ($Test) {
        $env:QT_QPA_PLATFORM = 'offscreen'  # what the test presets set
        $ctest = @('--test-dir', $buildDir, '--output-on-failure', '-j', $env:NUMBER_OF_PROCESSORS)
        if ($Filter) { $ctest += @('-R', $Filter) }
        Invoke-Step 'ctest' $ctest
    }
} finally {
    Pop-Location
}

Write-Host "done: $(Join-Path $buildDir 'waku.exe')" -ForegroundColor Green
if ($buildType -ne 'Release') { Write-Host "  symbols: $(Join-Path $buildDir 'waku.pdb')" }
