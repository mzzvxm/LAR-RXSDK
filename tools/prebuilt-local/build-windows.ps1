<#
.SYNOPSIS
    Builds the prebuilt Windows SDK of one commit on this PC: the "windows" job of
    .github/workflows/prebuilt-sdk.yaml as a script (Windows PowerShell 5.1, no extra modules).

.DESCRIPTION
    The workflow is the source of truth. This script repeats its steps with the same pins, the same edits, the same
    configure line, the same checks and the same file names, so that the .zip made here is the one the workflow
    would have made. It exists because the workflow could not run (the account's GitHub Actions were locked); the
    result is uploaded by hand with upload.ps1. If the workflow changes, change this file with it (the pins and the
    strings that end up in the bundle are checked against the workflow file at the start).

    What it does, in order (each is the workflow step of the same meaning):
      1. checks its own pins against ../../.github/workflows/prebuilt-sdk.yaml
      2. the toolchain: llvm-mingw 20250709 (UCRT), CMake 4.4.4, Ninja 1.13.2. The copies in -Tools are used when
         their folder name and `--version` match the pins; otherwise the pinned file is downloaded and its SHA-256
         checked, as the workflow does
      3. a clean checkout of the commit (a blobless clone of the repository, detached at the commit, with all
         submodules filled recursively, like actions/checkout with fetch-depth 0 and submodules: recursive)
      4. the two edits of the launcher's SDK step: FFmpeg's config.h and libmspack's symlink stand-ins
      5. configure (cmake --preset win-amd64 ...), build, install (the registry write turned into an echo),
         libc++.dll and libunwind.dll beside the binaries
      6. lar-sdk-bundle.json, the file check, the check for DLLs that are not shipped, and rexglue.exe --version
      7. lar-rxsdk-prebuilt-<12>-windows-x86_64-llvm-mingw-20250709.zip, its .sha256 and fragment-windows.json

    Everything is written under -Out: work\ (source, build, install; delete it when done, it is several GB),
    tools-dl\ (only when something had to be downloaded), logs\, and the result files in -Out itself.

.PARAMETER Commit
    The commit to build: the 40-digit hash, or a name the clone knows (a branch as origin/<name> or <name>).
    The release will be sdk-<its first 12 digits>.

.PARAMETER Out
    The folder that receives the result. Keep the path SHORT (for example C:\mcv\pb\<12 digits>): FFmpeg's tree
    has paths near the 260 character limit.

.PARAMETER Tools
    The folder that holds llvm-mingw-20250709-ucrt-x86_64, cmake-4.4.4-windows-x86_64 and ninja-1.13.2.

.PARAMETER Source
    Where the repository is cloned from. The default is the public repository, like the workflow. Do not point it
    at a partial clone on this PC (a blobless clone cannot serve its missing files).

.PARAMETER SubmoduleDonor
    A checkout of the SDK that has its submodules (git folders under .git\modules). Its objects are borrowed
    (alternates) so that the submodules are not downloaded again; what it lacks is fetched from the network as
    usual. Pass '' to turn this off.

.PARAMETER Jobs
    Compiler jobs (CMAKE_BUILD_PARALLEL_LEVEL). 0 (default): Ninja's own choice, as on the runner.

.PARAMETER Clean
    Delete -Out\work first and start from nothing. Without it a finished checkout of the same commit is reused,
    and the configure, build and install steps run again on top of it (Ninja only rebuilds what is out of date).

.PARAMETER DownloadTools
    Do not look in -Tools; download the pinned toolchain into -Out\tools-dl.

.PARAMETER SkipPinCheck
    Do not compare the pins with the workflow file.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File build-windows.ps1 -Commit f0f34904c579e4b81537cef2fce46afb716388e0 -Out C:\mcv\pb\f0f34904c579
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Commit,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Tools = 'C:\mcv\base\tools',
    [string]$Source = 'https://github.com/mzzvxm/LAR-RXSDK.git',
    [string]$SubmoduleDonor = 'C:\mcv\base\sdk-src',
    [int]$Jobs = 0,
    [switch]$Clean,
    [switch]$DownloadTools,
    [switch]$SkipPinCheck
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# PowerShell variable names are not case sensitive: $Out, $Tools and $Commit are parameters, so no variable below
# may be called out, tools or commit (the workflow's own names for them).

# ---- The workflow's pins (the "windows" job's env block). Keep in step with prebuilt-sdk.yaml. ----
$Pins = [ordered]@{
    LLVM_MINGW_VERSION = '20250709'
    LLVM_MINGW_URL     = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20250709/llvm-mingw-20250709-ucrt-x86_64.zip'
    LLVM_MINGW_SHA256  = '82babcd6aae4dc3606e8e0471d816989c384b4bf86a139f184a8b4a1b2c2758d'
    CMAKE_VERSION      = '4.4.4'
    CMAKE_URL          = 'https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.zip'
    CMAKE_SHA256       = 'bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536'
    NINJA_URL          = 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip'
    NINJA_SHA256       = '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'
}
$LlvmMingwVersion = $Pins['LLVM_MINGW_VERSION']
$CmakeVersion = $Pins['CMAKE_VERSION']
$NinjaVersion = [regex]::Match($Pins['NINJA_URL'], '/v([0-9][0-9.]*)/').Groups[1].Value

# The repository that owns the release, and the strings the workflow writes into the bundle and the asset name.
$SdkRepo = 'mzzvxm/LAR-RXSDK'
$BundleWhat = 'LAR-RXSDK install built with llvm-mingw'
$BundleOptions = 'REXGLUE_USE_VULKAN=ON REXGLUE_ENABLE_TRACY=OFF (FidelityFX off, its default), Release'
$AssetTemplate = 'lar-rxsdk-prebuilt-$env:SHORT-windows-x86_64-$toolchain.zip'   # as written in the workflow (literal)

$script:PhaseLines = New-Object 'System.Collections.Generic.List[string]'
$script:PhaseName = ''
$script:PhaseWatch = $null

# ---------------------------------------------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------------------------------------------
function Start-Phase([string]$Name) {
    Stop-Phase
    Write-Host ''
    Write-Host ('=== {0}  ({1})' -f $Name, (Get-Date -Format 'HH:mm:ss'))
    $script:PhaseName = $Name
    $script:PhaseWatch = [Diagnostics.Stopwatch]::StartNew()
}

function Stop-Phase {
    if ($null -ne $script:PhaseWatch) {
        $script:PhaseLines.Add(('{0,-52} {1,7:N0} s' -f $script:PhaseName, $script:PhaseWatch.Elapsed.TotalSeconds))
        $script:PhaseWatch = $null
    }
}

function Resolve-Absolute([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path (Get-Location).ProviderPath $Path))
}

function Fwd([string]$Path) { return $Path.Replace('\', '/') }

# Runs a program, shows its output, and stops the script when it does not end with 0. The error preference is
# relaxed for the call only: Windows PowerShell 5.1 turns a native program's stderr lines into errors otherwise.
function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$What
    )
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Exe @Arguments | Out-Host } finally { $ErrorActionPreference = $savedPreference }
    if ($LASTEXITCODE -ne 0) { throw ('{0} failed (exit code {1}).' -f $What, $LASTEXITCODE) }
}

# Runs a program and returns its output lines (stdout only) and exit code; nothing is shown.
function Get-NativeText {
    param([Parameter(Mandatory = $true)][string]$Exe, [string[]]$Arguments = @())
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $lines = @(& $Exe @Arguments 2>$null) } finally { $ErrorActionPreference = $savedPreference }
    return [pscustomobject]@{ Output = $lines; ExitCode = $LASTEXITCODE }
}

function Get-VersionLine([string]$Exe, [string]$Argument) {
    try {
        $reply = Get-NativeText -Exe $Exe -Arguments @($Argument)
        if ($reply.ExitCode -ne 0 -or $reply.Output.Count -eq 0) { return '' }
        return ([string]$reply.Output[0]).Trim()
    } catch { return '' }
}

# Deletes a folder under -Out (cmd's rmdir copes with long paths and read-only git files; Remove-Item in 5.1 does not).
function Remove-Tree([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if (-not $full.StartsWith($OutRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to delete ${full}: it is not inside $OutRoot." }
    & "$env:SystemRoot\System32\cmd.exe" /d /c ('rmdir /s /q "' + $full + '"') | Out-Null
    if (Test-Path -LiteralPath $full) { throw "Could not remove $full." }
}

function Test-AllFiles([string]$Folder, [string[]]$Relative) {
    foreach ($name in $Relative) {
        if (-not (Test-Path -LiteralPath (Join-Path $Folder $name))) { return $false }
    }
    return $true
}

# The workflow's Get-Verified: download with curl, check the SHA-256.
function Get-Verified([string]$Url, [string]$Sha256, [string]$DownloadFolder) {
    New-Item -ItemType Directory -Force $DownloadFolder | Out-Null
    $file = Join-Path $DownloadFolder ([IO.Path]::GetFileName(([Uri]$Url).AbsolutePath))
    Invoke-Native -Exe "$env:SystemRoot\System32\curl.exe" -What "Downloading $Url" -Arguments @(
        '--fail', '--silent', '--show-error', '--location', '--retry', '5', '--retry-delay', '5', '--output', $file, $Url)
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256.ToLowerInvariant()) { throw "$file has the SHA-256 $actual, expected $Sha256." }
    return $file
}

function Expand-Zip([string]$Zip, [string]$Destination) {
    New-Item -ItemType Directory -Force $Destination | Out-Null
    [IO.Compression.ZipFile]::ExtractToDirectory($Zip, $Destination)
}

# The install folder's content at the root of the zip, Deflate at its best, forward slashes in the entry names
# (the workflow's CreateFromDirectory call, done by hand so that the names are the same on any .NET).
function New-SdkZip([string]$Folder, [string]$ZipPath) {
    if (Test-Path -LiteralPath $ZipPath) { Remove-Item -LiteralPath $ZipPath -Force }
    $rootPrefix = [IO.Path]::GetFullPath($Folder).TrimEnd('\') + '\'
    $stream = [IO.File]::Open($ZipPath, [IO.FileMode]::CreateNew)
    try {
        $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $false)
        try {
            foreach ($item in @(Get-ChildItem -LiteralPath $Folder -Recurse -Force | Sort-Object FullName)) {
                $entryName = $item.FullName.Substring($rootPrefix.Length).Replace('\', '/')
                if ($item.PSIsContainer) {
                    if (@(Get-ChildItem -LiteralPath $item.FullName -Force).Count -eq 0) { [void]$archive.CreateEntry($entryName + '/') }
                    continue
                }
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $item.FullName, $entryName, [IO.Compression.CompressionLevel]::Optimal)
            }
        } finally { $archive.Dispose() }
    } finally { $stream.Dispose() }
}

# The pins and strings above must be the ones in the workflow file of the same repository folder.
function Assert-WorkflowAgrees {
    $workflowPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\.github\workflows\prebuilt-sdk.yaml'))
    if (-not (Test-Path -LiteralPath $workflowPath)) {
        Write-Warning "The workflow file is not at ${workflowPath}: the pins were not compared with it."
        return
    }
    $workflowText = [IO.File]::ReadAllText($workflowPath)
    $problems = @()
    foreach ($key in $Pins.Keys) {
        $pattern = '(?m)^\s+' + [regex]::Escape($key) + ':\s*"?' + [regex]::Escape($Pins[$key]) + '"?\s*$'
        if (-not [regex]::IsMatch($workflowText, $pattern)) { $problems += ('{0}: the workflow has no line "{0}: {1}"' -f $key, $Pins[$key]) }
    }
    foreach ($text in @($BundleWhat, $BundleOptions, $AssetTemplate)) {
        if (-not $workflowText.Contains($text)) { $problems += "the workflow does not contain the text: $text" }
    }
    if ($problems.Count -gt 0) {
        throw ("This script is out of step with $workflowPath (change them together, or pass -SkipPinCheck):`n  " + ($problems -join "`n  "))
    }
    Write-Host 'The pins and the bundle texts agree with the workflow file.'
}

# ---------------------------------------------------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------------------------------------------------
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$OutRoot = (Resolve-Absolute $Out).TrimEnd('\')
$ToolsRoot = (Resolve-Absolute $Tools).TrimEnd('\')
$WorkRoot = Join-Path $OutRoot 'work'
$SrcDir = Join-Path $WorkRoot 'src'
$BuildDir = Join-Path $WorkRoot 'build'
$InstallDir = Join-Path $WorkRoot 'install'
$StateDir = Join-Path $WorkRoot 'state'
$LogDir = Join-Path $OutRoot 'logs'
New-Item -ItemType Directory -Force $OutRoot | Out-Null
New-Item -ItemType Directory -Force $LogDir | Out-Null

$transcriptStarted = $false
try {
    Start-Transcript -Path (Join-Path $LogDir ('build-windows-{0}.log' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))) | Out-Null
    $transcriptStarted = $true
} catch { Write-Warning "No transcript: $_" }

$overallWatch = [Diagnostics.Stopwatch]::StartNew()
$savedPath = $env:PATH
$exitCode = 0

try {
    Write-Host "build-windows.ps1: commit '$Commit', out $OutRoot"

    if ($OutRoot.Length -gt 60) { Write-Warning "The -Out path is long ($($OutRoot.Length) characters); FFmpeg's tree needs the room. Something like C:\mcv\pb\<12 digits> is better." }

    # -------------------------------------------------------------------------------------------------------
    Start-Phase '1/8 pins agree with the workflow'
    if ($SkipPinCheck) { Write-Host 'Skipped (-SkipPinCheck).' } else { Assert-WorkflowAgrees }

    # -------------------------------------------------------------------------------------------------------
    # The workflow's "Get the toolchain" step. The PC's own copies are used when they are the pinned versions.
    Start-Phase '2/8 toolchain (llvm-mingw, CMake, Ninja)'
    $mingwNeeded = @('bin\clang.exe', 'bin\clang++.exe', 'bin\x86_64-w64-mingw32-windres.exe', 'bin\llvm-objdump.exe',
                     'x86_64-w64-mingw32\bin\libc++.dll', 'x86_64-w64-mingw32\bin\libunwind.dll')
    $mingwName = "llvm-mingw-$LlvmMingwVersion-ucrt-x86_64"
    $cmakeName = "cmake-$CmakeVersion-windows-x86_64"
    $mingwRoot = ''
    $cmakeRoot = ''
    $ninjaRoot = ''

    if (-not $DownloadTools) {
        $candidate = Join-Path $ToolsRoot $mingwName
        # CHECK: llvm-mingw 20250709 reports "clang version 20.x" (LLVM 20.1.8); the folder name is the pin, the version line a sanity check.
        if ((Test-AllFiles $candidate $mingwNeeded) -and ((Get-VersionLine "$candidate\bin\clang++.exe" '--version') -match '^clang version 20\.')) { $mingwRoot = $candidate }
        $candidate = Join-Path $ToolsRoot $cmakeName
        if ((Test-AllFiles $candidate @('bin\cmake.exe')) -and ((Get-VersionLine "$candidate\bin\cmake.exe" '--version') -eq "cmake version $CmakeVersion")) { $cmakeRoot = $candidate }
        foreach ($ninjaFolderName in @("ninja-$NinjaVersion", 'ninja')) {
            $candidate = Join-Path $ToolsRoot $ninjaFolderName
            if ((Test-AllFiles $candidate @('ninja.exe')) -and ((Get-VersionLine "$candidate\ninja.exe" '--version') -eq $NinjaVersion)) { $ninjaRoot = $candidate; break }
        }
    }

    if ($mingwRoot -eq '' -or $cmakeRoot -eq '' -or $ninjaRoot -eq '') {
        $downloadRoot = Join-Path $OutRoot 'tools-dl'
        $downloadFiles = Join-Path $downloadRoot 'downloads'
        if ($mingwRoot -eq '') {
            $target = Join-Path $downloadRoot $mingwName
            if (-not (Test-AllFiles $target $mingwNeeded)) {
                Write-Host "Downloading llvm-mingw $LlvmMingwVersion (SHA-256 checked)."
                Remove-Tree $target
                Expand-Zip (Get-Verified $Pins['LLVM_MINGW_URL'] $Pins['LLVM_MINGW_SHA256'] $downloadFiles) $downloadRoot
            }
            $mingwRoot = $target
        }
        if ($cmakeRoot -eq '') {
            $target = Join-Path $downloadRoot $cmakeName
            if (-not (Test-AllFiles $target @('bin\cmake.exe'))) {
                Write-Host "Downloading CMake $CmakeVersion (SHA-256 checked)."
                Remove-Tree $target
                Expand-Zip (Get-Verified $Pins['CMAKE_URL'] $Pins['CMAKE_SHA256'] $downloadFiles) $downloadRoot
            }
            $cmakeRoot = $target
        }
        if ($ninjaRoot -eq '') {
            $target = Join-Path $downloadRoot 'ninja'
            if (-not (Test-AllFiles $target @('ninja.exe'))) {
                Write-Host "Downloading Ninja $NinjaVersion (SHA-256 checked)."
                Remove-Tree $target
                Expand-Zip (Get-Verified $Pins['NINJA_URL'] $Pins['NINJA_SHA256'] $downloadFiles) $target
            }
            $ninjaRoot = $target
        }
    }

    foreach ($needed in @(
            "$mingwRoot\bin\clang.exe", "$mingwRoot\bin\clang++.exe", "$mingwRoot\bin\x86_64-w64-mingw32-windres.exe", "$mingwRoot\bin\llvm-objdump.exe",
            "$mingwRoot\x86_64-w64-mingw32\bin\libc++.dll", "$mingwRoot\x86_64-w64-mingw32\bin\libunwind.dll",
            "$cmakeRoot\bin\cmake.exe", "$ninjaRoot\ninja.exe")) {
        if (-not (Test-Path -LiteralPath $needed)) { throw "The toolchain is not as expected: $needed is missing." }
    }
    Write-Host "llvm-mingw: $mingwRoot"
    Write-Host "CMake:      $cmakeRoot"
    Write-Host "Ninja:      $ninjaRoot"
    Write-Host ('  ' + (Get-VersionLine "$mingwRoot\bin\clang++.exe" '--version'))
    Write-Host ('  ' + (Get-VersionLine "$cmakeRoot\bin\cmake.exe" '--version'))
    Write-Host ('  ninja ' + (Get-VersionLine "$ninjaRoot\ninja.exe" '--version'))

    # git is needed to check the sources out, and CMake runs it for the version number (git describe).
    $gitCommand = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($null -ne $gitCommand) { $git = $gitCommand.Source } else {
        $git = ''
        foreach ($miniGit in @(Get-ChildItem -LiteralPath $ToolsRoot -Directory -Filter 'mingit-*' -ErrorAction SilentlyContinue)) {
            if (Test-Path -LiteralPath "$($miniGit.FullName)\cmd\git.exe") { $git = "$($miniGit.FullName)\cmd\git.exe" }
        }
        if ($git -eq '') { throw "No git.exe on PATH and no mingit-* in $ToolsRoot." }
        $env:PATH = (Split-Path -Parent $git) + ';' + $env:PATH
    }
    Write-Host ('git: ' + $git + '  ' + (Get-VersionLine $git '--version'))

    # -------------------------------------------------------------------------------------------------------
    # The workflow's "Allow long paths" + actions/checkout (fetch-depth 0, fetch-tags, submodules: recursive).
    # Process-local git settings only (nothing in the user's git configuration changes): long paths on, symlinks
    # off (so libmspack's links become the small text files that the edit below expects, as on the runner).
    Start-Phase '3/8 checkout of the commit and its submodules'
    $env:GIT_CONFIG_COUNT = '2'
    $env:GIT_CONFIG_KEY_0 = 'core.longpaths'
    $env:GIT_CONFIG_VALUE_0 = 'true'
    $env:GIT_CONFIG_KEY_1 = 'core.symlinks'
    $env:GIT_CONFIG_VALUE_1 = 'false'
    $env:GIT_TERMINAL_PROMPT = '0'

    if ($Clean) { Remove-Tree $WorkRoot }
    $markerFile = Join-Path $StateDir 'checkout.ok'

    function Resolve-CommitIn([string]$Repo, [string]$Name) {
        foreach ($candidate in @($Name, ('origin/' + $Name))) {
            $reply = Get-NativeText -Exe $git -Arguments @('-C', $Repo, 'rev-parse', '--verify', '--quiet', ($candidate + '^{commit}'))
            if ($reply.ExitCode -eq 0 -and $reply.Output.Count -eq 1 -and ([string]$reply.Output[0]) -match '^[0-9a-f]{40}$') { return [string]$reply.Output[0] }
        }
        return ''
    }

    if (-not (Test-Path -LiteralPath $markerFile)) {
        # A half-made checkout from an earlier try: start over.
        Remove-Tree $WorkRoot
        New-Item -ItemType Directory -Force $WorkRoot | Out-Null
        New-Item -ItemType Directory -Force $StateDir | Out-Null

        # Blobless: the whole history and every tag (the SDK's version number is made from them), file contents only
        # for what is checked out. The result is the same tree as a full clone.
        Invoke-Native -Exe $git -What 'git clone' -Arguments @('clone', '--filter=blob:none', '--no-checkout', '--quiet', $Source, $SrcDir)

        $sha = Resolve-CommitIn $SrcDir $Commit
        if ($sha -eq '' -and $Commit -match '^[0-9a-f]{40}$') {
            # A commit that no branch or tag holds: GitHub serves it by its hash.
            $fetchReply = Get-NativeText -Exe $git -Arguments @('-C', $SrcDir, 'fetch', '--quiet', 'origin', $Commit)
            if ($fetchReply.ExitCode -eq 0) { $sha = Resolve-CommitIn $SrcDir $Commit }
        }
        if ($sha -eq '') { throw "The commit '$Commit' is not in $Source (is it pushed? a local-only commit cannot be built from here)." }

        Invoke-Native -Exe $git -What 'git checkout' -Arguments @('-C', $SrcDir, '-c', 'advice.detachedHead=false', 'checkout', '--quiet', '--detach', $sha)

        # Borrow the objects of a checkout that has the submodules already (nothing is downloaded twice; what it does
        # not have is fetched from the network by git as usual). submodule.alternateLocation=superproject makes git
        # look for <donor>\.git\modules\<submodule>\objects.
        if ($SubmoduleDonor -ne '') {
            $donorRoot = Resolve-Absolute $SubmoduleDonor
            $donorObjects = Join-Path $donorRoot '.git\objects'
            if ((Test-Path -LiteralPath $donorObjects) -and (Test-Path -LiteralPath (Join-Path $donorRoot '.git\modules'))) {
                [IO.File]::WriteAllText((Join-Path $SrcDir '.git\objects\info\alternates'), ((Fwd $donorObjects) + "`n"), (New-Object Text.UTF8Encoding($false)))
                Invoke-Native -Exe $git -What 'git config' -Arguments @('-C', $SrcDir, 'config', 'submodule.alternateLocation', 'superproject')
                Invoke-Native -Exe $git -What 'git config' -Arguments @('-C', $SrcDir, 'config', 'submodule.alternateErrorStrategy', 'info')
                Write-Host "Submodule objects are borrowed from $donorRoot."
            } else {
                Write-Host "No usable submodule donor at ${donorRoot}: the submodules are downloaded."
            }
        }

        Invoke-Native -Exe $git -What 'git submodule sync' -Arguments @('-C', $SrcDir, 'submodule', 'sync', '--recursive', '--quiet')
        Invoke-Native -Exe $git -What 'git submodule update' -Arguments @('-C', $SrcDir, 'submodule', 'update', '--init', '--force', '--recursive', '--jobs', '4', '--quiet')

        # Every submodule must be at the commit the SDK pins ('-' = not checked out, '+' = another commit, 'U' = conflict).
        $statusReply = Get-NativeText -Exe $git -Arguments @('-C', $SrcDir, 'submodule', 'status', '--recursive')
        if ($statusReply.ExitCode -ne 0) { throw 'git submodule status failed.' }
        $badSubmodules = @($statusReply.Output | Where-Object { $_ -match '^[-+U]' })
        if ($badSubmodules.Count -gt 0) { throw ("Submodules are not at their pinned commits:`n  " + ($badSubmodules -join "`n  ")) }

        [IO.File]::WriteAllText($markerFile, ($sha + "`n"), (New-Object Text.UTF8Encoding($false)))
    } else {
        $sha = (Get-Content -LiteralPath $markerFile -TotalCount 1).Trim()
        $asked = Resolve-CommitIn $SrcDir $Commit
        if ($asked -ne '' -and $asked -ne $sha) { throw "$WorkRoot holds $sha, not $asked. Use -Clean (or another -Out)." }
        if ($asked -eq '' -and $Commit -match '^[0-9a-f]{40}$' -and $Commit -ne $sha) { throw "$WorkRoot holds $sha, not $Commit. Use -Clean (or another -Out)." }
        Write-Host "The checkout of $sha from an earlier run is reused."
    }

    # The workflow's "Check out the commit that was asked for".
    $head = (Get-NativeText -Exe $git -Arguments @('-C', $SrcDir, 'rev-parse', 'HEAD')).Output[0].Trim()
    if ($head -ne $sha) { throw "HEAD is $head, expected $sha." }
    Invoke-Native -Exe $git -What 'git log' -Arguments @('-C', $SrcDir, 'log', '-1', '--format=%H %s')
    $short = $sha.Substring(0, 12)
    $releaseTag = "sdk-$short"
    $toolchain = "llvm-mingw-$LlvmMingwVersion"
    $assetName = "lar-rxsdk-prebuilt-$short-windows-x86_64-$toolchain.zip"
    Write-Host "Release tag $releaseTag, asset $assetName"

    # -------------------------------------------------------------------------------------------------------
    # The workflow's "Prepare the sources for llvm-mingw" (python there, the same logic here). Each edit is
    # idempotent, so a reused checkout is fine.
    Start-Phase '4/8 prepare the sources for llvm-mingw'
    $latin1 = [Text.Encoding]::GetEncoding(28591)   # byte for byte, whatever the line endings are

    $configPath = Join-Path $SrcDir 'thirdparty\FFmpeg\config.h'
    if (-not (Test-Path -LiteralPath $configPath)) { throw "$configPath is missing (the FFmpeg submodule is not checked out)." }
    $configText = [IO.File]::ReadAllText($configPath, $latin1)
    $newline = "`n"
    if ($configText.Contains("`r`n")) { $newline = "`r`n" }
    $oldPart = (@('#include "config_windows_x86_64.h"', '#endif', '#elif defined(__APPLE__) && defined(__MACH__)') -join $newline)
    $newPart = (@(
            '#include "config_windows_x86_64.h"',
            '#endif',
            '#if defined(__MINGW32__)',
            "/* The Windows configuration is MSVC's (--toolchain=msvc). mingw-w64 with the UCRT has no crtversion.h and takes the C99 printf specifiers. */",
            '#undef HAVE_LIBC_MSVCRT',
            '#define HAVE_LIBC_MSVCRT 0',
            '#endif',
            '#elif defined(__APPLE__) && defined(__MACH__)') -join $newline)
    $oldCount = [regex]::Matches($configText, [regex]::Escape($oldPart)).Count
    if ($configText.Contains($newPart)) {
        Write-Host 'FFmpeg config.h: already edited'
    } elseif ($oldCount -eq 1) {
        [IO.File]::WriteAllText($configPath, $configText.Replace($oldPart, $newPart), $latin1)
        Write-Host 'FFmpeg config.h: edited'
    } else {
        throw "FFmpeg config.h is not as expected: the text to change is there $oldCount times."
    }

    # Git on Windows without symlinks checks libmspack's links out as small text files holding the target path.
    $libmspackRoot = [IO.Path]::GetFullPath((Join-Path $SrcDir 'thirdparty\libmspack')).TrimEnd('\')
    $mspackFolder = Join-Path $libmspackRoot 'cabextract\mspack'
    if (-not (Test-Path -LiteralPath $mspackFolder)) { throw "$mspackFolder is missing (the libmspack submodule is not checked out)." }
    foreach ($stub in @(Get-ChildItem -LiteralPath $mspackFolder -File | Sort-Object Name)) {
        if (($stub.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $stub.Length -ge 200) { continue }
        $stubText = ([IO.File]::ReadAllText($stub.FullName, [Text.Encoding]::UTF8)).Trim()
        if ($stubText -eq '' -or $stubText.Contains("`n") -or $stubText.IndexOfAny([char[]]'<>"|?*') -ge 0) { continue }
        try { $stubTarget = [IO.Path]::GetFullPath([IO.Path]::Combine($mspackFolder, $stubText)) } catch { continue }
        if (-not $stubTarget.StartsWith($libmspackRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path -LiteralPath $stubTarget -PathType Leaf)) { continue }
        Copy-Item -LiteralPath $stubTarget -Destination $stub.FullName -Force
        Write-Host ('symlink stand-in replaced: ' + $stub.Name)
    }

    # -------------------------------------------------------------------------------------------------------
    # The launcher's configure line (SdkStep.BuildAsync), step for step, as the workflow has it. No Visual Studio
    # or Windows SDK variable may leak in; the CMake package registry is neither read nor written. The extra
    # variables removed here are ones a developer PC may have and a runner does not.
    Start-Phase '5/8 configure, build, install'
    $env:PATH = (@("$mingwRoot\bin", "$mingwRoot\x86_64-w64-mingw32\bin", "$cmakeRoot\bin", $ninjaRoot) -join ';') + ';' + $env:PATH
    foreach ($variable in 'VCToolsInstallDir', 'INCLUDE', 'LIB', 'LIBPATH', 'CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'RCFLAGS',
                          'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH', 'CMAKE_GENERATOR', 'CMAKE_TOOLCHAIN_FILE',
                          'CMAKE_PREFIX_PATH', 'CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER', 'VSINSTALLDIR', 'VCINSTALLDIR', 'WindowsSdkDir') {
        Remove-Item "Env:\$variable" -ErrorAction SilentlyContinue
    }
    if ($Jobs -gt 0) { $env:CMAKE_BUILD_PARALLEL_LEVEL = "$Jobs" } else { Remove-Item 'Env:\CMAKE_BUILD_PARALLEL_LEVEL' -ErrorAction SilentlyContinue }

    $cmake = "$cmakeRoot\bin\cmake.exe"
    $cc = Fwd "$mingwRoot\bin\clang.exe"
    $cxx = Fwd "$mingwRoot\bin\clang++.exe"
    $rc = Fwd "$mingwRoot\bin\x86_64-w64-mingw32-windres.exe"
    $prefix = Fwd $InstallDir

    Push-Location $SrcDir
    try {
        Write-Host '--- Configure'
        Invoke-Native -Exe $cmake -What 'cmake configure' -Arguments @(
            '--preset', 'win-amd64',
            '-B', $BuildDir,
            "-DCMAKE_INSTALL_PREFIX=$prefix",
            '-DREXGLUE_USE_VULKAN=ON',
            '-DREXGLUE_ENABLE_TRACY=OFF',
            "-DCMAKE_C_COMPILER=$cc",
            "-DCMAKE_CXX_COMPILER=$cxx",
            "-DCMAKE_RC_COMPILER=$rc",
            '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
            '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',
            '-DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON')

        Write-Host '--- Build'
        Invoke-Native -Exe $cmake -What 'cmake build' -Arguments @('--build', $BuildDir, '--config', 'Release')

        # The install writes a value into the CMake package registry with `reg add` (cmake/rexglue_install.cmake).
        # The launcher turns that command into an echo, and so does this: the same install, nothing outside it.
        Write-Host '--- Install'
        $installScript = Join-Path $BuildDir 'cmake_install.cmake'
        $installText = [IO.File]::ReadAllText($installScript)
        if ($installText.Contains('COMMAND reg add')) {
            $installText = $installText.Replace('COMMAND reg add', 'COMMAND ${CMAKE_COMMAND} -E echo registry-write-skipped reg add')
            [IO.File]::WriteAllText($installScript, $installText, (New-Object Text.UTF8Encoding($false)))
        }
        if ([IO.File]::ReadAllText($installScript).Contains('COMMAND reg add')) { throw 'The install would write into the registry, and that could not be turned off.' }
        Remove-Tree $InstallDir   # no file of an earlier try may end up in the archive
        Invoke-Native -Exe $cmake -What 'cmake install' -Arguments @('--install', $BuildDir, '--config', 'Release')

        # The runtime of a program built with llvm-mingw, beside rexglue.exe and the DLLs.
        foreach ($dll in 'libc++.dll', 'libunwind.dll') {
            Copy-Item -LiteralPath "$mingwRoot\x86_64-w64-mingw32\bin\$dll" -Destination "$InstallDir\bin\$dll" -Force
        }
    } finally { Pop-Location }

    # -------------------------------------------------------------------------------------------------------
    # The workflow's "Write the bundle info and check the install": what the launcher reads (SdkBundleInfo).
    Start-Phase '6/8 bundle info and checks'
    $info = [ordered]@{
        what      = $BundleWhat
        sdkCommit = $sha
        toolchain = $toolchain
        builtAt   = [DateTime]::UtcNow.ToString('o')
        options   = $BundleOptions
    }
    [IO.File]::WriteAllText("$InstallDir\lar-sdk-bundle.json", ($info | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))

    # The files the launcher's SDK step requires (SdkStep.WindowsFiles).
    foreach ($needed in @(
            'bin\rexglue.exe', 'bin\rexruntime.dll', 'bin\rexgpu-xenos.dll', 'bin\libc++.dll', 'bin\libunwind.dll',
            'lib\rexruntime.dll.a', 'lib\cmake\rexglue\rexglueConfig.cmake', 'lar-sdk-bundle.json')) {
        if (-not (Test-Path -LiteralPath "$InstallDir\$needed")) { throw "The install has no $needed." }
    }

    # A DLL of the gcc runtime would be missing on the player's PC (the launcher ships libc++ and libunwind only).
    $objdump = "$mingwRoot\bin\llvm-objdump.exe"
    $badImports = @()
    foreach ($file in Get-ChildItem -LiteralPath "$InstallDir\bin" -File | Where-Object { $_.Extension -in '.dll', '.exe' }) {
        foreach ($line in (Get-NativeText -Exe $objdump -Arguments @('-p', $file.FullName)).Output | Select-String 'DLL Name:') {
            $importedDll = ($line.Line -split ':', 2)[1].Trim()
            if ($importedDll -match '^(libwinpthread|libgcc_s|libstdc\+\+|libssp)') { $badImports += "$($file.Name) imports $importedDll" }
        }
    }
    if ($badImports.Count -gt 0) { throw "The binaries need a DLL that is not shipped: $($badImports -join '; ')" }

    $installFiles = @(Get-ChildItem -LiteralPath $InstallDir -Recurse -File)
    $installBytes = ($installFiles | Measure-Object -Property Length -Sum).Sum
    Write-Host ('install: {0} files, {1:N1} MB' -f $installFiles.Count, ($installBytes / 1MB))

    # CHECK: rexglue.exe --version exits 0 with only Windows' own folders on PATH (so the DLLs beside it are what is found).
    Start-Phase '7/8 rexglue.exe starts'
    $pathBeforeCheck = $env:PATH
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    try {
        Invoke-Native -Exe "$InstallDir\bin\rexglue.exe" -What 'rexglue.exe --version' -Arguments @('--version')
    } finally { $env:PATH = $pathBeforeCheck }

    # -------------------------------------------------------------------------------------------------------
    # The workflow's "Pack and upload" without the upload (upload.ps1 does that, after manifest.py).
    Start-Phase '8/8 zip, sha256, fragment'
    $zipPath = Join-Path $OutRoot $assetName
    New-SdkZip $InstallDir $zipPath

    # No entry name may hold a backslash (the launcher unpacks into sdk\ and would not find the folders).
    $zipRead = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $zipEntries = @($zipRead.Entries)
        foreach ($entry in $zipEntries) { if ($entry.FullName.Contains('\')) { throw "The zip has an entry name with a backslash: $($entry.FullName)" } }
        $hasBundle = @($zipEntries | Where-Object { $_.FullName -eq 'lar-sdk-bundle.json' }).Count
        if ($hasBundle -ne 1) { throw 'lar-sdk-bundle.json is not at the root of the zip.' }
    } finally { $zipRead.Dispose() }

    $zipSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash.ToLowerInvariant()
    $zipSize = (Get-Item -LiteralPath $zipPath).Length
    [IO.File]::WriteAllText("$zipPath.sha256", "$zipSha  $assetName`n", (New-Object Text.UTF8Encoding($false)))
    Write-Host ('{0}: {1:N1} MB, sha256 {2}' -f $assetName, ($zipSize / 1MB), $zipSha)

    $releaseUrl = "https://github.com/$SdkRepo/releases/download/$releaseTag/$assetName"
    $fragment = [ordered]@{
        sdkPrebuilt = [ordered]@{
            asset     = $assetName
            url       = $releaseUrl
            sha256    = $zipSha
            size      = $zipSize
            sdkCommit = $sha
            toolchain = $toolchain
        }
    }
    [IO.File]::WriteAllText((Join-Path $OutRoot 'fragment-windows.json'), ($fragment | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))

    Stop-Phase
    Write-Host ''
    Write-Host '=== done'
    foreach ($line in $script:PhaseLines) { Write-Host ('  ' + $line) }
    Write-Host ('  total {0:N1} min' -f $overallWatch.Elapsed.TotalMinutes)
    Write-Host ''
    Write-Host "Commit:  $sha"
    Write-Host "Files:   $zipPath"
    Write-Host "         $zipPath.sha256"
    Write-Host ("         " + (Join-Path $OutRoot 'fragment-windows.json'))
    Write-Host 'Next: build the Linux SDK (build-linux.sh) if wanted, then manifest.py, then upload.ps1 (see README.md).'
} catch {
    Write-Host ''
    Write-Host ('FAILED: ' + $_.Exception.Message)
    Write-Host $_.ScriptStackTrace
    $exitCode = 1
} finally {
    $env:PATH = $savedPath
    if ($transcriptStarted) { try { Stop-Transcript | Out-Null } catch { } }
}
exit $exitCode
