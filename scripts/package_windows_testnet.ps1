param(
    [string]$BuildDir = "$(Join-Path $PSScriptRoot '..\build\windows-gpu')",
    [string]$OutputDir = "$(Join-Path $PSScriptRoot '..\dist\amarian-testnet-windows-x64')",
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildDir = (Resolve-Path $BuildDir).Path
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
$binDir = Join-Path $OutputDir 'bin'

if ($Clean -and (Test-Path -LiteralPath $OutputDir)) {
    Remove-Item -LiteralPath $OutputDir -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if (-not (Test-Path -LiteralPath $cmake)) { throw "CMake was not found" }
& $cmake --install $BuildDir --config Release --prefix $OutputDir

$required = @(
    'amariand.exe', 'amarian-cli.exe', 'amarian-wallet.exe',
    'amarian-gui.exe', 'amarian-wallet-gui.exe', 'amarian-miner.exe',
    'amarian-genesis.exe', 'amarian-retarget-sim.exe'
)
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $binDir $name))) {
        throw "CMake install did not produce $name"
    }
}

function Find-File([string]$Name, [string[]]$Roots) {
    foreach ($root in $Roots) {
        if ([string]::IsNullOrWhiteSpace($root) -or -not (Test-Path -LiteralPath $root)) { continue }
        $candidate = Join-Path $root $Name
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path $candidate).Path }
    }
    return $null
}

$vcpkgBin = if ($env:AMARIAN_VCPKG_BIN) { $env:AMARIAN_VCPKG_BIN } else { Join-Path $repoRoot '..\vcpkg\installed\x64-windows\bin' }
$cudaBin = if ($env:CUDA_PATH) { Join-Path $env:CUDA_PATH 'bin' } else { $null }
$crtRoots = @(
    $env:VCToolsRedistDir,
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC'),
    (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC')
)

$runtime = @{
    'libcrypto-3-x64.dll' = @($vcpkgBin)
    'z.dll'               = @($vcpkgBin)
    'cudart64_12.dll'     = @($cudaBin)
}
foreach ($entry in $runtime.GetEnumerator()) {
    $source = Find-File $entry.Key $entry.Value
    if ($null -eq $source) { throw "Required runtime dependency not found: $($entry.Key)" }
    Copy-Item -LiteralPath $source -Destination $binDir -Force
}

foreach ($name in @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    $source = $null
    foreach ($root in $crtRoots) {
        if ($null -eq $root -or -not (Test-Path -LiteralPath $root)) { continue }
        $source = Get-ChildItem -LiteralPath $root -Filter $name -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\Microsoft\.VC143\.CRT\\' } |
            Select-Object -First 1 -ExpandProperty FullName
        if ($source) { break }
    }
    if ($source) { Copy-Item -LiteralPath $source -Destination $binDir -Force }
}

$infoPath = Join-Path $OutputDir 'BUILD-INFO.txt'
& (Join-Path $binDir 'amariand.exe') --build-info 2>&1 | Set-Content -LiteralPath $infoPath -Encoding utf8

$manifest = Join-Path $OutputDir 'SHA256SUMS.txt'
Get-ChildItem -LiteralPath $OutputDir -File -Recurse |
    Where-Object { $_.FullName -ne $manifest } |
    Sort-Object FullName |
    Get-FileHash -Algorithm SHA256 |
    ForEach-Object {
        $relative = $_.Path.Substring($OutputDir.Length).TrimStart('\', '/').Replace('\', '/')
        '{0}  {1}' -f $_.Hash.ToLowerInvariant(), $relative
    } | Set-Content -LiteralPath $manifest -Encoding ascii

$zipPath = "$OutputDir.zip"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path (Join-Path $OutputDir '*') -DestinationPath $zipPath -CompressionLevel Optimal

Write-Output "Created $zipPath"
Write-Output "Checksums: $manifest"
