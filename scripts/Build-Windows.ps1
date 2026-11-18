# SPDX-License-Identifier: GPL-3.0-or-later
#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$GamePath,

    [string]$BuildDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build'),

    [ValidateRange(1, 128)]
    [int]$Jobs = [Math]::Min(10, [Environment]::ProcessorCount)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$discId = 'SOMP01'
$dolSha256 = '2e0368359106a2eef773c70ef083e89aafe1e49603eefc7f16b9ed414cd098fe'
$modernGekkoCommit = 'a2e4e2b026a68d4848f20ebf16f0e0ddeb573957'
$recompCoreCommit = 'cf8ceb81b47d6172164cd0572f57aa7624719f33'
$dolRecompCommit = '02e27aca1c408cfcb2a4079a3a5b5ab3ae39d340'

function Resolve-Tool([string]$Name) {
    (Get-Command -Name $Name -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
}

function Invoke-Tool([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$(Split-Path -Leaf $Program) exited with code $LASTEXITCODE. See the output above."
    }
}

function Initialize-Source([string]$Directory, [string]$Url, [string]$Commit) {
    if (-not (Test-Path -LiteralPath (Join-Path $Directory '.git') -PathType Container)) {
        if (Test-Path -LiteralPath $Directory) {
            throw "Refusing to initialize an existing directory at $Directory. Choose a fresh build directory."
        }
        Invoke-Tool $git @('init', $Directory)
    }

    $head = & $git -C $Directory rev-parse --verify --quiet HEAD
    if ($LASTEXITCODE -ne 0) {
        Invoke-Tool $git @('-C', $Directory, 'fetch', '--no-tags', '--depth', '1', $Url, $Commit)
        Invoke-Tool $git @('-C', $Directory, '-c', 'core.longpaths=true', 'checkout', '--detach', $Commit)
    } elseif ($head -ne $Commit) {
        throw "$Directory must be at $Commit. Choose a fresh build directory."
    }

    # The public submodule wrapper invokes sh.exe. The native helper works with MinGit too.
    Invoke-Tool $git @('-C', $Directory, '-c', 'core.longpaths=true', '-c',
        'url.https://github.com/libarchive/bzip2.git.insteadOf=https://gitlab.com/bzip2/bzip2.git',
        'submodule--helper', 'update', '--init', '--recursive', '--depth', '1', '--jobs', "$Jobs")
}

function Set-SourcePatch([string]$Directory, [string]$PatchPath) {
    & $git -C $Directory diff --quiet --ignore-submodules=all HEAD
    if ($LASTEXITCODE -eq 0) {
        Invoke-Tool $git @('-C', $Directory, 'apply', '--check', $PatchPath)
        Invoke-Tool $git @('-C', $Directory, 'apply', $PatchPath)
    } elseif ($LASTEXITCODE -eq 1) {
        Invoke-Tool $git @('-C', $Directory, 'apply', '--reverse', '--check', $PatchPath)
    } else {
        throw "Could not inspect source changes in $Directory."
    }
}

function Confirm-Game([string]$GameRoot) {
    foreach ($part in @('sys\boot.bin', 'sys\main.dol')) {
        if (-not (Test-Path -LiteralPath (Join-Path $GameRoot $part) -PathType Leaf)) {
            throw "Cannot find $part in the supplied data partition."
        }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $GameRoot 'files') -PathType Container)) {
        throw 'The supplied data partition must contain a files directory.'
    }

    $discHeader = [byte[]]::new(8)
    $stream = [IO.File]::OpenRead((Join-Path $GameRoot 'sys\boot.bin'))
    try {
        $read = $stream.Read($discHeader, 0, $discHeader.Length)
    } finally {
        $stream.Dispose()
    }
    if ($read -ne 8 -or [Text.Encoding]::ASCII.GetString($discHeader, 0, 6) -cne $discId -or $discHeader[7] -ne 0) {
        throw "The data partition must be $discId revision 0."
    }

    $dol = Join-Path $GameRoot 'sys\main.dol'
    if ((Get-FileHash -LiteralPath $dol -Algorithm SHA256).Hash -ne $dolSha256) {
        throw "The PowerPC executable does not match the supported $discId revision 0 dump."
    }
}

if (-not [Environment]::Is64BitProcess -or $env:OS -ne 'Windows_NT') {
    throw 'Run this script from 64-bit PowerShell on Windows.'
}

$inputItem = Get-Item -LiteralPath $GamePath
$root = [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDirectory))
$project = Split-Path -Parent $PSScriptRoot
$defaultBuild = Join-Path $project 'build'
$compare = [StringComparison]::OrdinalIgnoreCase
$comparisonPath = $root.TrimEnd('\')
$withinProject = $comparisonPath.Equals($project, $compare) -or $comparisonPath.StartsWith("$project\", $compare)
if ($withinProject -and -not ($comparisonPath.Equals($defaultBuild, $compare) -or $comparisonPath.StartsWith("$defaultBuild\", $compare))) {
    throw 'Use the ignored build directory or a directory outside the project checkout.'
}
$null = [IO.Directory]::CreateDirectory($root)
$lock = [IO.File]::Open((Join-Path $root '.build.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
$previousResponseSetting = $env:CMAKE_NINJA_FORCE_RESPONSE_FILE

try {
    if ($inputItem.PSIsContainer) {
        $gameRoot = $inputItem.FullName
    } else {
        if ($inputItem.Extension -ne '.wbfs') {
            throw 'Supply a .wbfs file or its extracted data-partition directory.'
        }
        if (Test-Path -LiteralPath ([IO.Path]::ChangeExtension($inputItem.FullName, '.wbf1'))) {
            throw 'For a split WBFS image, supply the already-extracted data partition.'
        }

        $dtk = Resolve-Tool 'dtk'
        $wbfsHash = (Get-FileHash -LiteralPath $inputItem.FullName -Algorithm SHA256).Hash
        $gameRoot = Join-Path $root "wbfs-$wbfsHash"
        if (-not (Test-Path -LiteralPath $gameRoot -PathType Container)) {
            $extraction = Join-Path $root ('extract-' + [Guid]::NewGuid().ToString('N'))
            Invoke-Tool $dtk @('disc', 'extract', '-p', 'data', $inputItem.FullName, $extraction)
            Confirm-Game $extraction
            [IO.Directory]::Move($extraction, $gameRoot)
        }
    }
    Confirm-Game $gameRoot

    $git = Resolve-Tool 'git'
    $cmake = Resolve-Tool 'cmake'
    $ninja = Resolve-Tool 'ninja'
    $clang = Resolve-Tool 'clang'
    $clangXX = Resolve-Tool 'clang++'
    $python = Resolve-Tool 'python'
    $compilerTarget = & $clang -dumpmachine
    if ($LASTEXITCODE -ne 0 -or $compilerTarget -notmatch '^x86_64-.*-(windows-gnu|mingw32)$') {
        throw 'Place the x86-64 UCRT LLVM-MinGW clang and clang++ on PATH first.'
    }
    $compilerBin = Split-Path -Parent $clang

    $mg = Join-Path $root 'mg'
    $dr = Join-Path $root 'dr'
    $vendor = Join-Path $mg 'vendor\dolphin'
    Initialize-Source $mg 'https://github.com/ExpansionPak/ModernGekko.git' $modernGekkoCommit
    Initialize-Source $dr 'https://github.com/ExpansionPak/DolRecomp.git' $dolRecompCommit
    $actualRecompCoreCommit = & $git -C $vendor rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $actualRecompCoreCommit -ne $recompCoreCommit) {
        throw "RecompCore did not resolve to $recompCoreCommit."
    }

    $patches = Join-Path $project 'patches'
    Set-SourcePatch $mg (Join-Path $patches 'ModernGekko.patch')
    Set-SourcePatch $vendor (Join-Path $patches 'RecompCore.patch')

    $config = @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
        "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_C_COMPILER=$clang", '-DCMAKE_OBJECT_PATH_MAX=240')
    $recompilerBuild = Join-Path $root 'db'
    Invoke-Tool $cmake ($config + @('-S', $dr, '-B', $recompilerBuild,
        '-DDOLRECOMP_ENABLE_LLVM=OFF', '-DBUILD_TESTING=OFF'))
    Invoke-Tool $cmake @('--build', $recompilerBuild, '--target', 'dolrecomp', '--parallel', "$Jobs")

    $generated = Join-Path $root "generated\$($discId)_generated"
    $codeStamp = Join-Path $generated '.complete'
    if (-not (Test-Path -LiteralPath $codeStamp -PathType Leaf)) {
        $database = Join-Path $root 'database'
        $null = [IO.Directory]::CreateDirectory($database)
        Set-Content -LiteralPath (Join-Path $database 'titles.txt') -Encoding ASCII -Value "$discId = Beat the Beat: Rhythm Paradise"
        Push-Location -LiteralPath $root
        try {
            Invoke-Tool (Join-Path $recompilerBuild 'dolrecomp.exe') @("-j$Jobs", '--cpu', 'broadway',
                '--backend', 'c', '--semantics', 'exact', (Join-Path $gameRoot 'sys\main.dol'), $discId, 'generated')
        } finally {
            Pop-Location
        }
        Copy-Item -LiteralPath (Join-Path $generated "$discId.h") -Destination (Join-Path $generated 'generated.h')
        Copy-Item -LiteralPath (Join-Path $generated "$($discId)_smc.txt") -Destination (Join-Path $generated 'generated_smc.txt')
        Copy-Item -LiteralPath (Join-Path $gameRoot 'sys\main.dol') -Destination (Join-Path $generated 'main.dol')
        Set-Content -LiteralPath $codeStamp -Encoding ASCII -Value $dolRecompCommit
    }

    $env:CMAKE_NINJA_FORCE_RESPONSE_FILE = '1'
    $moduleBuild = Join-Path $root 'mod'
    Invoke-Tool $cmake ($config + @('-S', (Join-Path $vendor 'module-template'), '-B', $moduleBuild,
        "-DPython3_EXECUTABLE=$python", "-DGAME_ID=$discId", "-DGENERATED_DIR=$generated",
        '-DRECOMPCORE_MODULE_OPT_LEVEL=1', '-DRECOMPCORE_MODULE_ENABLE_IPO=ON'))
    Invoke-Tool $cmake @('--build', $moduleBuild, '--parallel', "$Jobs")

    $hostBuild = Join-Path $root 'mb'
    $windowsApi = '-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000004'
    Invoke-Tool $cmake ($config + @('-S', $mg, '-B', $hostBuild, "-DCMAKE_CXX_COMPILER=$clangXX",
        '-DCMAKE_CXX_STANDARD=23', '-DCMAKE_CXX_STANDARD_REQUIRED=ON', '-DBUILD_TESTING=OFF',
        '-DUSE_SYSTEM_LIBS=OFF', '-DENABLE_VULKAN=ON', '-DDOLRECOMP_ENABLE_LLVM=OFF',
        "-DCMAKE_C_FLAGS=$windowsApi", "-DCMAKE_CXX_FLAGS=$windowsApi",
        "-DMODERNGEKKO_REQUIRED_DISC_ID=$discId", "-DMODERNGEKKO_REQUIRED_DOL_SHA256=$dolSha256",
        '-DMODERNGEKKO_DEFAULT_WINDOW_TITLE=Beat the Beat - Rhythm Paradise'))
    Invoke-Tool $cmake @('--build', $hostBuild, '--target', 'moderngekko-run', '--parallel', "$Jobs")

    $runtime = Join-Path $root 'run'
    $null = [IO.Directory]::CreateDirectory($runtime)
    Copy-Item -LiteralPath (Join-Path $hostBuild 'moderngekko-run.exe'), (Join-Path $moduleBuild "g$($discId)_recomp.dll"),
        (Join-Path $compilerBin 'libc++.dll'), (Join-Path $compilerBin 'libunwind.dll'),
        (Join-Path $PSScriptRoot 'Launch.cmd') -Destination $runtime
    Invoke-Tool $cmake @('-E', 'copy_directory', (Join-Path $hostBuild 'Sys'), (Join-Path $runtime 'Sys'))
    $utf8 = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText((Join-Path $runtime 'game-path.txt'), "$gameRoot`r`n", $utf8)
    Write-Host "Run $(Join-Path $runtime 'Launch.cmd')"
} finally {
    $env:CMAKE_NINJA_FORCE_RESPONSE_FILE = $previousResponseSetting
    $lock.Dispose()
}
