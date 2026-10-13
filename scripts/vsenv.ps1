# Copyright (c) 2026 Scourge Project. All rights reserved.
# ============================================================================
#  vsenv.ps1 - Inject VS2026 (MSVC) + Windows SDK build environment into
#  the current PowerShell session.
#  Usage:  . d:\Scourge\scripts\vsenv.ps1; <command...>
#  NOTE: ASCII only. Windows PowerShell 5.1 reads BOM-less UTF-8 as ANSI,
#        and multi-byte comment text can swallow the following newline.
# ============================================================================
$ErrorActionPreference = "Stop"

$vsRoot   = "C:\Program Files\Microsoft Visual Studio\18\Community"
$msvcRoot = Join-Path $vsRoot "VC\Tools\MSVC"
$sdkRoot  = "C:\Program Files (x86)\Windows Kits\10"

if (-not (Test-Path $msvcRoot)) { throw "MSVC not found: $msvcRoot" }
if (-not (Test-Path $sdkRoot))  { throw "Windows SDK not found: $sdkRoot" }

# Pick the highest installed version.
$msvcVer = (Get-ChildItem $msvcRoot -Directory |
    Sort-Object Name -Descending | Select-Object -First 1).Name
$sdkVer = (Get-ChildItem (Join-Path $sdkRoot "Include") -Directory |
    Sort-Object Name -Descending | Select-Object -First 1).Name

$msvc = Join-Path $msvcRoot $msvcVer

$env:INCLUDE = @(
    "$msvc\include",
    "$sdkRoot\Include\$sdkVer\ucrt",
    "$sdkRoot\Include\$sdkVer\um",
    "$sdkRoot\Include\$sdkVer\shared",
    "$sdkRoot\Include\$sdkVer\winrt",
    "$sdkRoot\Include\$sdkVer\cppwinrt"
) -join ";"

$env:LIB = @(
    "$msvc\lib\x64",
    "$sdkRoot\Lib\$sdkVer\ucrt\x64",
    "$sdkRoot\Lib\$sdkVer\um\x64"
) -join ";"

# MSVC link.exe for clang (MSVC ABI): put it first on PATH.
$env:PATH = "$msvc\bin\Hostx64\x64;$sdkRoot\bin\$sdkVer\x64;$env:PATH"

Write-Host "[vsenv] MSVC $msvcVer / SDK $sdkVer"
