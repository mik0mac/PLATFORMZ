# packaging/windows/pack.ps1 - the Windows handout zip (F3, #94).
#
# The Windows counterpart of the Makefile's app -> sign -> dist-pack chain, and
# built on the same rule: VERIFY what landed rather than trusting that a step
# succeeded, and never let a build that would warn on a player's machine reach a
# zip named like the real handout.
#
#   cmake --preset client
#   cmake --build --preset client
#   pwsh packaging/windows/pack.ps1
#
# Output, in dist/:
#   PLATFORMZ-windows-x64.zip            signed, signature verified
#   PLATFORMZ-UNSIGNED-windows-x64.zip   no signing configured - local/testing only
#
# SIGNING is not set up yet (no certificate; see #94). The hook is one variable:
# set PLATFORMZ_SIGNTOOL_ARGS to the arguments for `signtool sign`, everything
# except the file. Both routes a certificate can come by are signtool:
#   a certificate on a USB token (in the Windows cert store):
#     /fd sha256 /tr http://timestamp.digicert.com /td sha256 /a
#   Azure Trusted Signing:
#     /fd sha256 /tr http://timestamp.acs.microsoft.com /td sha256
#     /dlib <path>\Azure.CodeSigning.Dlib.dll /dmdf <path>\metadata.json
# A timestamp (/tr) is not optional: without one the signature dies with the
# certificate, and the exe starts warning again the day it expires. The check
# below refuses a signature that has none.

[CmdletBinding()]
param(
    # Where CMake put platformz.exe (and assets\ beside it).
    [string]$BuildDir = "build/cmake-client/Release",
    [string]$OutDir   = "dist"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Fail([string]$msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

# Tools that ship with Visual Studio / the Windows SDK but are not on PATH
# outside a developer prompt.
function Find-VsTool([string]$pattern) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    $hit = & $vswhere -latest -products * -find $pattern | Select-Object -First 1
    return $hit
}
function Find-SignTool {
    $onPath = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    return Get-ChildItem $kits -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -match "\\x64\\" } |
           Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}

#MARK: Stage
$exe    = Join-Path $BuildDir "platformz.exe"
$assets = Join-Path $BuildDir "assets"
if (-not (Test-Path $exe))    { Fail "$exe not found - build first: cmake --build --preset client" }
if (-not (Test-Path $assets)) { Fail "$assets not found - the CMake post-build step copies it beside the exe" }

$stage = Join-Path $OutDir "stage-windows"
$root  = Join-Path $stage "PLATFORMZ"          # the folder the player unzips
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $root | Out-Null
Copy-Item $exe $root
Copy-Item $assets (Join-Path $root "assets") -Recurse
# Same exclusion as the web build and `make app`: macOS litter, never runtime.
Get-ChildItem $root -Recurse -Force -Filter ".DS_Store" | Remove-Item -Force
$staged = Join-Path $root "platformz.exe"

#MARK: Check the version resource landed
$ver = (Get-Item $staged).VersionInfo
if ([string]::IsNullOrWhiteSpace($ver.ProductVersion) -or $ver.ProductName -ne "PLATFORMZ") {
    Fail "platformz.exe has no version resource (ProductName='$($ver.ProductName)') - packaging/windows/platformz.rc.in did not compile in"
}
Write-Host "==> platformz.exe $($ver.ProductVersion)"

#MARK: Check it needs nothing the player lacks
# The Mac chain's equivalent is "otool -L shows only Apple libraries". Every DLL
# the exe imports must be part of Windows itself. Two ways to fail: a DLL that is
# not in System32 at all (a dependency that would have to ship beside the exe),
# or the Visual C++ runtime, which IS often in System32 - on this build machine
# certainly - but is a separate install on a player's, and exactly the missing
# DLL this check exists to catch. CMakeLists.txt links it statically (/MT).
$dumpbin = Find-VsTool "VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe"
if (-not $dumpbin) { Fail "dumpbin not found (needs Visual Studio's C++ tools) - cannot check the exe's DLLs" }
$deps = & $dumpbin /nologo /dependents $staged |
        ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^\S+\.dll$' }
if (-not $deps) { Fail "dumpbin listed no DLLs for platformz.exe - the check cannot be trusted" }
$system32 = Join-Path $env:SystemRoot "System32"
$bad = @()
foreach ($d in $deps) {
    $isCrt = $d -match '^(vcruntime|msvcp|ucrtbase|concrt|api-ms-win-crt-)'
    if ($isCrt -or -not (Test-Path (Join-Path $system32 $d))) { $bad += $d }
}
if ($bad.Count -gt 0) {
    Fail ("platformz.exe needs DLLs a player may not have: " + ($bad -join ", ") +
          ". The C++ runtime should be static (CMAKE_MSVC_RUNTIME_LIBRARY in CMakeLists.txt).")
}
Write-Host "==> imports only Windows system DLLs: $($deps -join ', ')"

#MARK: Sign (when configured)
$signing = -not [string]::IsNullOrWhiteSpace($env:PLATFORMZ_SIGNTOOL_ARGS)
if ($signing) {
    $signtool = Find-SignTool
    if (-not $signtool) { Fail "PLATFORMZ_SIGNTOOL_ARGS is set but signtool.exe was not found (Windows SDK)" }
    # The variable holds several arguments; split it the way a command line would.
    $signArgs = [System.Management.Automation.PSParser]::Tokenize($env:PLATFORMZ_SIGNTOOL_ARGS, [ref]$null) |
                ForEach-Object { $_.Content }
    & $signtool sign @signArgs $staged
    if ($LASTEXITCODE -ne 0) { Fail "signtool sign failed (exit $LASTEXITCODE)" }
}

#MARK: Verify the signature, BEFORE zipping
# The equivalent of `spctl -a` in `make dist-pack`: ask Windows what it now
# thinks of the file. A zip carrying the real handout name exists only if the
# answer is a valid, timestamped signature.
$sig = Get-AuthenticodeSignature $staged
if ($signing) {
    if ($sig.Status -ne "Valid") {
        Fail "signature did not verify: $($sig.Status) - $($sig.StatusMessage)"
    }
    if (-not $sig.TimeStamperCertificate) {
        Fail "signed but NOT timestamped - the signature would lapse when the certificate does. Add /tr to PLATFORMZ_SIGNTOOL_ARGS."
    }
    Write-Host "==> signed by $($sig.SignerCertificate.Subject), timestamped"
    $zipName = "PLATFORMZ-windows-x64.zip"
} else {
    if ($sig.Status -ne "NotSigned") {
        Fail "no signing configured, yet the exe reports '$($sig.Status)' - refusing to guess what it is"
    }
    $zipName = "PLATFORMZ-UNSIGNED-windows-x64.zip"
}

#MARK: Zip
# .NET's ZipFile rather than Compress-Archive: Windows PowerShell 5.1's
# Compress-Archive writes backslashes into entry names, which other unzippers
# (and Steam's uploader) do not treat as folders.
Add-Type -AssemblyName System.IO.Compression.FileSystem
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$zip = Join-Path (Resolve-Path $OutDir) $zipName
if (Test-Path $zip) { Remove-Item $zip -Force }
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    (Resolve-Path $root), $zip, [System.IO.Compression.CompressionLevel]::Optimal, $true)
Remove-Item $stage -Recurse -Force

if ($signing) {
    Write-Host "==> $zip  (signed, verified, timestamped)"
} else {
    Write-Host "==> $zip  (UNSIGNED - SmartScreen will warn; local test only)"
}
