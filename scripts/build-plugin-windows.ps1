[CmdletBinding()]
param(
    [string[]]$Versions = @(
        "13.2",
        "14.0",
        "14.1",
        "14.2",
        "15.0",
        "15.1",
        "15.2",
        "16.0",
        "16.1",
        "17.0"
    ),
    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$BuildRoot = "",
    [string]$ArtifactsRoot = "",
    [string]$Generator = "",
    [string]$Toolset = "",
    [string]$Platform = "x64",
    [switch]$ListOnly,
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Section {
    param([string]$Message)
    Write-Host ""
    Write-Host "== $Message ==" -ForegroundColor Cyan
}

function Normalize-VersionList {
    param([string[]]$RawVersions)

    $normalized = [System.Collections.Generic.List[string]]::new()

    foreach ($entry in $RawVersions) {
        if ([string]::IsNullOrWhiteSpace($entry)) {
            continue
        }

        foreach ($part in ($entry -split ',')) {
            $value = $part.Trim()
            if (-not [string]::IsNullOrWhiteSpace($value)) {
                if ($value -match '^\d+$') {
                    $value = "$value.0"
                }
                $normalized.Add($value)
            }
        }
    }

    return $normalized | Select-Object -Unique
}

function Get-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function Get-DefaultPath {
    param(
        [string]$Configured,
        [string]$FallbackRelative
    )

    if ([string]::IsNullOrWhiteSpace($Configured)) {
        return (Join-Path (Get-RepoRoot) $FallbackRelative)
    }

    $resolved = Resolve-Path -LiteralPath $Configured -ErrorAction SilentlyContinue
    if ($resolved) {
        return $resolved.Path
    }

    return $Configured
}

function Get-NukeInstallSearchRoots {
    $roots = [System.Collections.Generic.List[string]]::new()

    if ($env:NUKE_INSTALL_ROOTS) {
        foreach ($root in ($env:NUKE_INSTALL_ROOTS -split ';')) {
            if (-not [string]::IsNullOrWhiteSpace($root)) {
                $roots.Add($root.Trim())
            }
        }
    }

    if ($env:ProgramFiles) {
        $roots.Add($env:ProgramFiles)
    }

    $programFilesX86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
    if ($programFilesX86) {
        $roots.Add($programFilesX86)
    }

    return $roots | Select-Object -Unique
}

function Parse-NukeInstallName {
    param([string]$Name)

    $match = [regex]::Match($Name, '^Nuke(?<minor>\d+\.\d+)v(?<patch>\d+)$')
    if (-not $match.Success) {
        return $null
    }

    [pscustomobject]@{
        MinorVersion = $match.Groups['minor'].Value
        Patch        = [int]$match.Groups['patch'].Value
        FullVersion  = ("Nuke{0}v{1}" -f $match.Groups['minor'].Value, $match.Groups['patch'].Value)
    }
}

function Get-NukeInstallsFromFilesystem {
    $results = [System.Collections.Generic.List[object]]::new()

    foreach ($root in (Get-NukeInstallSearchRoots)) {
        if (-not (Test-Path -LiteralPath $root)) {
            continue
        }

        Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like 'Nuke*' } |
            ForEach-Object {
                $parsed = Parse-NukeInstallName -Name $_.Name
                if ($null -ne $parsed) {
                    $results.Add([pscustomobject]@{
                            MinorVersion = $parsed.MinorVersion
                            Patch        = $parsed.Patch
                            FullVersion  = $parsed.FullVersion
                            InstallDir   = $_.FullName
                            Source       = "filesystem"
                        })
                }
            }
    }

    return $results
}

function Get-NukeInstallsFromRegistry {
    $results = [System.Collections.Generic.List[object]]::new()
    $registryPaths = @(
        "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*"
    )

    foreach ($path in $registryPaths) {
        Get-ItemProperty -Path $path -ErrorAction SilentlyContinue |
            Where-Object {
                $hasDisplayName = $_.PSObject.Properties.Match('DisplayName').Count -gt 0
                $hasInstallLocation = $_.PSObject.Properties.Match('InstallLocation').Count -gt 0
                $hasDisplayName -and $hasInstallLocation -and $_.DisplayName -like 'Nuke *' -and $_.InstallLocation
            } |
            ForEach-Object {
                $displayMatch = [regex]::Match($_.DisplayName, '^Nuke\s+(?<minor>\d+\.\d+)v(?<patch>\d+)$')
                if ($displayMatch.Success) {
                    $installDir = $_.InstallLocation.TrimEnd('\')
                    if (Test-Path -LiteralPath $installDir) {
                        $results.Add([pscustomobject]@{
                                MinorVersion = $displayMatch.Groups['minor'].Value
                                Patch        = [int]$displayMatch.Groups['patch'].Value
                                FullVersion  = ("Nuke{0}v{1}" -f $displayMatch.Groups['minor'].Value, $displayMatch.Groups['patch'].Value)
                                InstallDir   = $installDir
                                Source       = "registry"
                            })
                    }
                }
            }
    }

    return $results
}

function Get-InstalledNukes {
    $all = @()
    $all += Get-NukeInstallsFromFilesystem
    $all += Get-NukeInstallsFromRegistry

    $deduped = $all |
        Group-Object InstallDir |
        ForEach-Object { $_.Group | Select-Object -First 1 }

    $results = @(
        $deduped |
            Group-Object MinorVersion |
            ForEach-Object {
                $_.Group |
                    Sort-Object Patch -Descending |
                    Select-Object -First 1
            } |
            Sort-Object MinorVersion
    )

    return $results
}

function Get-BuildPresetForMinorVersion {
    param([string]$MinorVersion)

    $major = [int]($MinorVersion.Split('.')[0])

    switch ($major) {
        12 {
            return [pscustomobject]@{
                Generator = "Visual Studio 14 2015 Win64"
                Toolset   = "v140"
            }
        }
        13 {
            return [pscustomobject]@{
                Generator = "Visual Studio 15 2017"
                Toolset   = "v141"
            }
        }
        14 {
            return [pscustomobject]@{
                Generator = "Visual Studio 16 2019"
                Toolset   = "v142"
            }
        }
        15 {
            return [pscustomobject]@{
                Generator = "Visual Studio 16 2019"
                Toolset   = "v142"
            }
        }
        16 {
            return [pscustomobject]@{
                Generator = "Visual Studio 16 2019"
                Toolset   = "v142"
            }
        }
        17 {
            return [pscustomobject]@{
                Generator = "Visual Studio 17 2022"
                Toolset   = "v143"
            }
        }
        default {
            throw "No build preset is defined for Nuke $MinorVersion."
        }
    }
}

function Get-VsWherePath {
    $programFilesX86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
    $candidates = @(
        (Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
    )

    return $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
}

function Get-InstalledVisualStudios {
    $vswhere = Get-VsWherePath
    if (-not $vswhere) {
        return @()
    }

    $json = & $vswhere -products * -requires Microsoft.Component.MSBuild -format json 2>$null
    if (-not $json) {
        return @()
    }

    $parsedInstances = $json | ConvertFrom-Json
    if ($parsedInstances -is [System.Array]) {
        $instances = $parsedInstances
    } else {
        $instances = @($parsedInstances)
    }
    $results = [System.Collections.Generic.List[object]]::new()

    foreach ($instance in $instances) {
        if (-not $instance) {
            continue
        }

        $major = 0
        if ($instance.installationVersion) {
            $major = [int]($instance.installationVersion.Split('.')[0])
        } elseif ($instance.catalog -and $instance.catalog.productLineVersion) {
            $productLineVersion = [string]$instance.catalog.productLineVersion
            if ($productLineVersion -match '^\d+$') {
                $major = [int]$productLineVersion
            }
        }

        $generator = switch ($major) {
            18 { "Visual Studio 18 2026" }
            17 { "Visual Studio 17 2022" }
            16 { "Visual Studio 16 2019" }
            15 { "Visual Studio 15 2017" }
            14 { "Visual Studio 14 2015" }
            default { $null }
        }

        if ($generator) {
            $toolsets = [System.Collections.Generic.List[string]]::new()
            $auxiliaryBuildDir = Join-Path $instance.installationPath "VC\Auxiliary\Build"
            if (Test-Path -LiteralPath $auxiliaryBuildDir) {
                Get-ChildItem -LiteralPath $auxiliaryBuildDir -Filter "Microsoft.VCToolsVersion.v*.default.txt" -ErrorAction SilentlyContinue |
                    ForEach-Object {
                        $match = [regex]::Match($_.Name, 'Microsoft\.VCToolsVersion\.(v\d+)\.default\.txt')
                        if ($match.Success) {
                            $toolsets.Add($match.Groups[1].Value)
                        }
                    }
            }

            $msvcToolsDir = Join-Path $instance.installationPath "VC\Tools\MSVC"
            if (Test-Path -LiteralPath $msvcToolsDir) {
                switch ($major) {
                    16 { $toolsets.Add("v142") }
                    17 { $toolsets.Add("v143") }
                }
            }

            $results.Add([pscustomobject]@{
                MajorVersion     = $major
                Generator        = $generator
                InstallationPath = $instance.installationPath
                DisplayName      = $instance.displayName
                Toolsets         = @($toolsets | Select-Object -Unique)
            })
        }
    }

    return @($results | Sort-Object MajorVersion -Descending)
}

function Resolve-GeneratorChoice {
    param(
        [string]$RequestedGenerator,
        [pscustomobject]$Preset,
        [object[]]$InstalledVisualStudios,
        [string[]]$SupportedCMakeGenerators
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedGenerator)) {
        return $RequestedGenerator
    }

    $validInstalledVisualStudios = @(
        $InstalledVisualStudios |
            Where-Object {
                $_ -and
                $_.PSObject -and
                $_.PSObject.Properties.Match('Generator').Count -gt 0 -and
                -not [string]::IsNullOrWhiteSpace([string]$_.Generator)
            }
    )

    $supportedGeneratorSet = @($SupportedCMakeGenerators)
    if ($supportedGeneratorSet.Count -gt 0) {
        $validInstalledVisualStudios = @(
            $validInstalledVisualStudios |
                Where-Object { $supportedGeneratorSet -contains $_.Generator }
        )
    }

    $exactMatch = $validInstalledVisualStudios | Where-Object { $_.Generator -eq $Preset.Generator } | Select-Object -First 1
    if ($exactMatch) {
        return $exactMatch.Generator
    }

    $fallback = $validInstalledVisualStudios | Select-Object -First 1
    if ($fallback) {
        Write-Warning "Preferred generator '$($Preset.Generator)' is not installed. Falling back to '$($fallback.Generator)'."
        return $fallback.Generator
    }

    return $Preset.Generator
}

function Get-CMakeSupportedGenerators {
    $capabilitiesJson = cmake -E capabilities 2>$null
    if (-not $capabilitiesJson) {
        return @()
    }

    $capabilities = $capabilitiesJson | ConvertFrom-Json
    if (-not $capabilities -or -not $capabilities.generators) {
        return @()
    }

    return @(
        $capabilities.generators |
            Where-Object { $_.name } |
            ForEach-Object { [string]$_.name }
    )
}

function Get-VisualStudioInstanceForGenerator {
    param(
        [string]$Generator,
        [object[]]$InstalledVisualStudios
    )

    return $InstalledVisualStudios |
        Where-Object {
            $_ -and
            $_.PSObject -and
            $_.PSObject.Properties.Match('Generator').Count -gt 0 -and
            $_.Generator -eq $Generator
        } |
        Select-Object -First 1
}

function Test-ToolsetAvailable {
    param(
        [pscustomobject]$VisualStudioInstance,
        [string]$RequestedToolset
    )

    if ([string]::IsNullOrWhiteSpace($RequestedToolset)) {
        return $true
    }

    if (-not $VisualStudioInstance) {
        return $false
    }

    if ($VisualStudioInstance.PSObject.Properties.Match('Toolsets').Count -eq 0) {
        return $false
    }

    return @($VisualStudioInstance.Toolsets) -contains $RequestedToolset
}

function Get-ToolsetInstallHint {
    param([string]$Toolset)

    switch ($Toolset) {
        "v141" { return "Install the optional 'MSVC v141 - VS 2017 C++ x64/x86 build tools' component." }
        "v142" { return "Install the optional 'MSVC v142 - VS 2019 C++ x64/x86 build tools' component or Visual Studio 2019 Build Tools." }
        "v143" { return "Install the 'MSVC v143 - VS 2022 C++ x64/x86 build tools' component or Visual Studio 2022 Build Tools." }
        default { return "Install the matching MSVC toolset for $Toolset." }
    }
}

function Invoke-External {
    param(
        [string[]]$Command,
        [string]$WorkingDirectory
    )

    $commandText = $Command -join ' '
    Write-Host $commandText -ForegroundColor DarkGray
    Push-Location $WorkingDirectory
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & $Command[0] $Command[1..($Command.Length - 1)] 2>&1 | Write-Host
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
        Pop-Location
    }

    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}:`n$commandText"
    }
}

function Show-NukeTargets {
    param([object[]]$Targets)

    foreach ($target in @($Targets)) {
        Write-Host ("{0}`t{1}`t{2}`t{3}" -f $target.MinorVersion, $target.FullVersion, $target.InstallDir, $target.Source)
    }
}

function Copy-BuildArtifact {
    param(
        [string]$BuildDir,
        [string]$ConfigurationName,
        [string]$MinorVersion,
        [string]$ArtifactsDir
    )

    $candidatePaths = @(
        (Join-Path (Join-Path $BuildDir $ConfigurationName) "gifWriter.dll"),
        (Join-Path $BuildDir "gifWriter.dll")
    )

    $artifact = $candidatePaths | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $artifact) {
        Write-Warning "No built gifWriter artifact was found for $MinorVersion."
        return
    }

    $targetDir = Join-Path $ArtifactsDir $MinorVersion
    New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
    Copy-Item -LiteralPath $artifact -Destination (Join-Path $targetDir "gifWriter.dll") -Force
}

$repoRoot = Get-RepoRoot
$resolvedBuildRoot = Get-DefaultPath -Configured $BuildRoot -FallbackRelative "build"
$resolvedArtifactsRoot = Get-DefaultPath -Configured $ArtifactsRoot -FallbackRelative "nuke\Windows"
$requestedVersions = Normalize-VersionList -RawVersions $Versions
$installedVisualStudios = Get-InstalledVisualStudios
$supportedCMakeGenerators = Get-CMakeSupportedGenerators

Write-Section "Scanning for Nuke installs"
$installedNukes = Get-InstalledNukes

if (-not $installedNukes) {
    throw "No installed Nuke versions were found. Set NUKE_INSTALL_ROOTS if your installs are in a custom location."
}

$targets = @(
    foreach ($version in $requestedVersions) {
        $match = $installedNukes | Where-Object { $_.MinorVersion -eq $version } | Select-Object -First 1
        if ($match) {
            $match
        }
    }
)

if (-not $targets) {
    $available = ($installedNukes | Select-Object -ExpandProperty MinorVersion) -join ', '
    throw "None of the requested Nuke versions were found. Requested: $($requestedVersions -join ', '). Available: $available"
}

Write-Host "MinorVersion`tFullVersion`tInstallDir`tSource" -ForegroundColor DarkGray
Show-NukeTargets -Targets $targets

if ($ListOnly) {
    return
}

New-Item -ItemType Directory -Force -Path $resolvedBuildRoot | Out-Null
New-Item -ItemType Directory -Force -Path $resolvedArtifactsRoot | Out-Null

foreach ($target in $targets) {
    $preset = Get-BuildPresetForMinorVersion -MinorVersion $target.MinorVersion
    $buildDir = Join-Path $resolvedBuildRoot $target.FullVersion

    Write-Section ("Building against {0}" -f $target.FullVersion)

    if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
        Remove-Item -LiteralPath $buildDir -Recurse -Force
    }

    $configureArgs = @(
        "cmake",
        "-S", $repoRoot,
        "-B", $buildDir,
        "-DNUKE_ROOT=$($target.InstallDir)",
        "-DCMAKE_PREFIX_PATH=$($target.InstallDir)"
    )

    $resolvedGenerator = Resolve-GeneratorChoice -RequestedGenerator $Generator -Preset $preset -InstalledVisualStudios $installedVisualStudios -SupportedCMakeGenerators $supportedCMakeGenerators
    $resolvedToolset = if ([string]::IsNullOrWhiteSpace($Toolset)) { $preset.Toolset } else { $Toolset }
    $generatorInstance = Get-VisualStudioInstanceForGenerator -Generator $resolvedGenerator -InstalledVisualStudios $installedVisualStudios

    Write-Host ("Using generator: {0}" -f $resolvedGenerator) -ForegroundColor Yellow
    if (-not [string]::IsNullOrWhiteSpace($resolvedToolset)) {
        Write-Host ("Using toolset:   {0}" -f $resolvedToolset) -ForegroundColor Yellow
    }

    if (-not $generatorInstance) {
        throw "Visual Studio generator '$resolvedGenerator' is not installed. Install the matching Visual Studio Build Tools first."
    }

    if (-not (Test-ToolsetAvailable -VisualStudioInstance $generatorInstance -RequestedToolset $resolvedToolset)) {
        $hint = Get-ToolsetInstallHint -Toolset $resolvedToolset
        throw "Visual Studio '$($generatorInstance.DisplayName)' does not have toolset '$resolvedToolset'. $hint"
    }

    if (-not [string]::IsNullOrWhiteSpace($resolvedGenerator)) {
        $configureArgs += @("-G", $resolvedGenerator)
    }

    if (-not [string]::IsNullOrWhiteSpace($Platform) -and $resolvedGenerator -notlike "*Win64") {
        $configureArgs += @("-A", $Platform)
    }

    if (-not [string]::IsNullOrWhiteSpace($resolvedToolset)) {
        $configureArgs += @("-T", $resolvedToolset)
    }

    Invoke-External -Command $configureArgs -WorkingDirectory $repoRoot

    $buildArgs = @(
        "cmake",
        "--build", $buildDir,
        "--config", $Configuration
    )

    Invoke-External -Command $buildArgs -WorkingDirectory $repoRoot

    Copy-BuildArtifact `
        -BuildDir $buildDir `
        -ConfigurationName $Configuration `
        -MinorVersion $target.MinorVersion `
        -ArtifactsDir $resolvedArtifactsRoot
}

Write-Section "Done"
Write-Host "Artifacts: $resolvedArtifactsRoot" -ForegroundColor Green
