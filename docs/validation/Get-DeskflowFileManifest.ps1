# SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
# SPDX-License-Identifier: MIT

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Root,

    [string]$ExpectedManifest
)

$ErrorActionPreference = 'Stop'
$rootItem = Get-Item -LiteralPath $Root -Force
if (!$rootItem.PSIsContainer -or ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'Root must be a directory without a reparse point.'
}

$entries = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
$folders = [Collections.Generic.Stack[IO.DirectoryInfo]]::new()
$folders.Push($rootItem)
while ($folders.Count) {
    $folder = $folders.Pop()
    foreach ($item in Get-ChildItem -LiteralPath $folder.FullName -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Cannot verify a reparse point: $($item.FullName)"
        }
        $relative = [IO.Path]::GetRelativePath($rootItem.FullName, $item.FullName).Replace('\', '/')
        if ($item.PSIsContainer) {
            $entries.Add($relative, [pscustomobject]@{ path = $relative; type = 'directory'; bytes = 0; sha256 = '' })
            $folders.Push($item)
        } else {
            $hash = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            $entries.Add($relative, [pscustomobject]@{ path = $relative; type = 'file'; bytes = $item.Length; sha256 = $hash })
        }
    }
}

$paths = [string[]]@($entries.Keys)
[Array]::Sort($paths, [StringComparer]::Ordinal)

if ($ExpectedManifest) {
    $expected = Get-Content -LiteralPath $ExpectedManifest -Raw -Encoding utf8 | ConvertFrom-Json
    if ($expected.version -ne 1 -or $null -eq $expected.PSObject.Properties['items']) {
        throw 'Expected an acceptance manifest in format 1.'
    }
    $expectedByPath = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
    foreach ($entry in $expected.items) {
        $expectedByPath.Add($entry.path, $entry)
    }
    foreach ($relative in $paths) {
        if (!$expectedByPath.ContainsKey($relative)) {
            throw "Unexpected item: $relative"
        }
        $wanted = $expectedByPath[$relative]
        $actual = $entries[$relative]
        if ($actual.type -cne $wanted.type -or $actual.bytes -ne $wanted.bytes -or $actual.sha256 -cne $wanted.sha256) {
            throw "Item differs: $relative"
        }
        $null = $expectedByPath.Remove($relative)
    }
    if ($expectedByPath.Count) {
        throw "Missing item: $($expectedByPath.Keys | Select-Object -First 1)"
    }
    "Verified $($entries.Count) entries."
} else {
    [ordered]@{
        version = 1
        items = @($paths | ForEach-Object { $entries[$_] })
    } | ConvertTo-Json -Depth 4
}
