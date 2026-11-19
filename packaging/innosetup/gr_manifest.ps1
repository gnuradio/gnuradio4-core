<#
    .SYNOPSIS
        Reads and maintains the shared GNU Radio 4 install manifest.

    .DESCRIPTION
        Every GNU Radio 4 installer (gr-core, gr-blocks, ... and the combined
        superbuild installer) shares one manifest per plugin API version.  It
        records where the runtime libraries and the plugins live, plus the
        version of every component and plugin that is currently installed and
        the build configurations each of them was installed from.

        Schema 2 added the per-package 'configurations' list.  A schema 1
        manifest is read without it and gains it on the next register.

        Results are written as 'key=value' lines to -OutFile (UTF-8) so the
        caller does not depend on console encoding or redirection.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('query', 'register', 'unregister')][string] $Action,
    [Parameter(Mandatory = $true)][string] $ManifestPath,
    [string] $OutFile = '',
    [string] $ApiVersion = '',
    [string] $InstallRoot = '',
    [string] $PluginRoot = '',
    # '|'-separated 'name=version' pairs, e.g. 'gr-core=4.0.0|gr-blocks=4.0.0'
    [string] $Components = '',
    [string] $Plugins = '',
    # Build configuration these packages were built in, e.g. 'Release' or
    # 'Debug'.  The release and debug builds of one package share an install
    # root and an uninstall entry, so they are one manifest entry recording
    # both configurations rather than two entries.
    [string] $Configuration = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:schemaVersion = 2
$script:resultLines = New-Object System.Collections.Generic.List[string]

function Add-ResultLine {
    param([string] $Key, [string] $Value)
    $script:resultLines.Add("$Key=$Value")
}

function Write-Result {
    $text = ($script:resultLines -join "`r`n") + "`r`n"
    if ([string]::IsNullOrEmpty($OutFile)) {
        Write-Output $text
        return
    }
    [System.IO.File]::WriteAllText($OutFile, $text, (New-Object System.Text.UTF8Encoding($false)))
}

function ConvertTo-OrderedDictionary {
    param($InputObject)
    if ($null -eq $InputObject) { return $null }
    if ($InputObject -is [System.Management.Automation.PSCustomObject]) {
        $converted = [ordered]@{}
        foreach ($property in $InputObject.PSObject.Properties) {
            $converted[$property.Name] = ConvertTo-OrderedDictionary $property.Value
        }
        return $converted
    }
    if ($InputObject -is [System.Array]) {
        # the leading comma keeps the array whole: a function's output is unrolled, so a one-element list returned bare
        # would come back as its element, and be written back as a scalar
        return , @($InputObject | ForEach-Object { ConvertTo-OrderedDictionary $_ })
    }
    return $InputObject
}

function Read-Manifest {
    # -Repair moves an unparseable manifest aside so a fresh one can be written;
    # reads that only report never mutate what is on disc
    param([switch] $Repair)

    if (-not (Test-Path -LiteralPath $ManifestPath)) { return $null }
    # the manifest is written as UTF-8 without a BOM; without -Encoding, Windows
    # PowerShell decodes it as ANSI and mangles non-ASCII install paths
    $raw = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8
    if ([string]::IsNullOrWhiteSpace($raw)) { return $null }

    try {
        return ConvertTo-OrderedDictionary (ConvertFrom-Json $raw)
    } catch {
        Add-ResultLine 'manifest-unreadable' '1'
        if (-not $Repair) { return $null }
        $corrupt = "$ManifestPath.corrupt"
        Move-Item -LiteralPath $ManifestPath -Destination $corrupt -Force
        Add-ResultLine 'manifest-replaced' $corrupt
        return $null
    }
}

function Write-Manifest {
    param($Manifest)
    $directory = Split-Path -Parent $ManifestPath
    if (-not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    # an earlier version of this script wrote a one-element 'configurations' list as a scalar; it is written as a list
    foreach ($section in @('components', 'plugins')) {
        if (-not $Manifest.Contains($section) -or $Manifest[$section] -isnot [System.Collections.IDictionary]) { continue }
        foreach ($entry in $Manifest[$section].Values) {
            if ($entry -is [System.Collections.IDictionary] -and $entry.Contains('configurations')) {
                $entry['configurations'] = @($entry['configurations'])
            }
        }
    }
    # write to a sibling temporary file first so a crash cannot truncate the manifest
    $temporary = "$ManifestPath.new"
    $json = $Manifest | ConvertTo-Json -Depth 6
    [System.IO.File]::WriteAllText($temporary, $json, (New-Object System.Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $temporary -Destination $ManifestPath -Force
}

function New-Manifest {
    return [ordered]@{
        'schema'       = $script:schemaVersion
        'api-version'  = $ApiVersion
        'install-root' = $InstallRoot
        'plugin-root'  = $PluginRoot
        'components'   = [ordered]@{}
        'plugins'      = [ordered]@{}
    }
}

function Get-Section {
    param($Manifest, [string] $Name)
    if (-not $Manifest.Contains($Name) -or $null -eq $Manifest[$Name]) {
        $Manifest[$Name] = [ordered]@{}
    }
    return $Manifest[$Name]
}

function Split-Entries {
    param([string] $Value)
    if ([string]::IsNullOrWhiteSpace($Value)) { return @() }
    return $Value.Split('|') | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
}

function Invoke-Query {
    $manifest = Read-Manifest
    if ($null -eq $manifest) {
        Add-ResultLine 'manifest-present' '0'
        return
    }
    Add-ResultLine 'manifest-present' '1'
    foreach ($key in @('install-root', 'plugin-root', 'api-version')) {
        if ($manifest.Contains($key)) { Add-ResultLine $key ([string] $manifest[$key]) }
    }
    foreach ($section in @('components', 'plugins')) {
        if (-not $manifest.Contains($section) -or $null -eq $manifest[$section]) { continue }
        $prefix = $section.Substring(0, $section.Length - 1)
        foreach ($name in $manifest[$section].Keys) {
            $entry = $manifest[$section][$name]
            $version = ''
            $configurations = @()
            if ($entry -is [System.Collections.IDictionary]) {
                if ($entry.Contains('version')) { $version = [string] $entry['version'] }
                # absent from a schema 1 manifest, which predates the per-configuration record
                if ($entry.Contains('configurations')) { $configurations = @($entry['configurations']) }
            }
            Add-ResultLine "$prefix.$name" $version
            Add-ResultLine "$prefix-config.$name" ($configurations -join ',')
        }
    }
}

function Invoke-Register {
    $manifest = Read-Manifest -Repair
    if ($null -eq $manifest) { $manifest = New-Manifest }

    $manifest['schema'] = $script:schemaVersion
    foreach ($pair in @{ 'api-version' = $ApiVersion; 'install-root' = $InstallRoot; 'plugin-root' = $PluginRoot }.GetEnumerator()) {
        if (-not [string]::IsNullOrWhiteSpace($pair.Value)) { $manifest[$pair.Key] = $pair.Value }
    }

    foreach ($section in @(@{ Name = 'components'; Entries = $Components }, @{ Name = 'plugins'; Entries = $Plugins })) {
        $target = Get-Section $manifest $section.Name
        foreach ($entry in Split-Entries $section.Entries) {
            $separator = $entry.IndexOf('=')
            if ($separator -lt 1) { throw "malformed entry '$entry', expected 'name=version'" }
            $name = $entry.Substring(0, $separator)
            # Configurations already recorded are kept: installing the debug
            # build of a package must not retract the release build sitting
            # beside it under the same install root.
            $configurations = New-Object System.Collections.Generic.List[string]
            if ($target.Contains($name)) {
                $existing = $target[$name]
                if ($existing -is [System.Collections.IDictionary] -and $existing.Contains('configurations')) {
                    foreach ($value in @($existing['configurations'])) {
                        if (-not [string]::IsNullOrWhiteSpace($value)) { $configurations.Add([string] $value) }
                    }
                }
            }
            # Case-insensitively: CMake accepts any casing of CMAKE_BUILD_TYPE
            # and the Pascal reader compares with CompareText, so a 'debug'
            # build followed by a 'Debug' one must not be recorded twice.
            $alreadyRecorded = $false
            foreach ($value in $configurations) {
                if ([string]::Equals($value, $Configuration, [System.StringComparison]::OrdinalIgnoreCase)) {
                    $alreadyRecorded = $true
                }
            }
            if (-not [string]::IsNullOrWhiteSpace($Configuration) -and -not $alreadyRecorded) {
                $configurations.Add($Configuration)
            }
            $target[$name] = [ordered]@{
                'version'        = $entry.Substring($separator + 1)
                'configurations' = @($configurations)
            }
            Add-ResultLine 'registered' $name
        }
    }

    Write-Manifest $manifest
}

function Invoke-Unregister {
    $manifest = Read-Manifest
    if ($null -eq $manifest) { return }

    foreach ($section in @(@{ Name = 'components'; Entries = $Components }, @{ Name = 'plugins'; Entries = $Plugins })) {
        $target = Get-Section $manifest $section.Name
        foreach ($entry in Split-Entries $section.Entries) {
            $separator = $entry.IndexOf('=')
            $name = $entry
            if ($separator -gt 0) { $name = $entry.Substring(0, $separator) }
            # The whole entry goes, every configuration with it: the release
            # and debug builds of a package share one uninstall entry, so
            # uninstalling always removes both.
            if ($target.Contains($name)) {
                $target.Remove($name)
                Add-ResultLine 'unregistered' $name
            }
        }
    }

    if ((Get-Section $manifest 'components').Count -eq 0 -and (Get-Section $manifest 'plugins').Count -eq 0) {
        Remove-Item -LiteralPath $ManifestPath -Force
        Add-ResultLine 'manifest-removed' '1'
        return
    }
    Write-Manifest $manifest
}

try {
    switch ($Action) {
        'query' { Invoke-Query }
        'register' { Invoke-Register }
        'unregister' { Invoke-Unregister }
    }
    Add-ResultLine 'status' 'ok'
    Write-Result
    exit 0
} catch {
    Add-ResultLine 'status' 'error'
    Add-ResultLine 'message' ($_.Exception.Message -replace '[\r\n]+', ' ')
    Write-Result
    exit 1
}
