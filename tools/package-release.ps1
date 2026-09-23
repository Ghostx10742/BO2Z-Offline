param(
    [string] $OutputZip
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$version = '1.0.0'
$publisher = Join-Path $project 'resources/publisher'
$staging = Join-Path $project "dist/release-stage-$version"
$zip = if ($OutputZip) { $OutputZip }
       else { Join-Path $project "dist/BO2Z-Offline-$version-local-build.zip" }
if ((Test-Path -LiteralPath $staging) -or (Test-Path -LiteralPath $zip)) {
    throw 'Release output already exists. Move or rename it before packaging again.'
}

$expected = [ordered]@{
    'ffotd_tu17_mp_147.ff.00' = 'AA870C574560754851005F2BC92E3A5DE71DB3F2F7C19F5DB47D5241229E54D8'
    'ffotd_tu17_zm_147.ff.00' = '72897E6C440170C25155FCAADF23EBBA9EF0FBAD1677682702C2327A38D0B469'
    'ffotd_tu17_zm_147.ff.01' = '68115B2E7446EB0B7F88458C00350B116B6142B4291E27ECA96E4D599849FFFB'
    'largeheatmap.raw' = 'EF2A60EDCBD2061168CABE022FA258439232D02D5619857FA47326E5A015A8FE'
    'lsssk0' = 'B1F11ABA643BCACAF6E59E9E4309CEAB7315543EAD3FA3A3AD0555C0FDD73DF6'
    'lsssk1' = '95EAF05ADDF877327991996F041D8086E158AD20043F29B00664CC35BDBE6789'
    'lsssk2' = '33E9BA894A16E4B3B68E8DE75FEB469F77CC51F23891392EC3BB1B96271E749A'
    'online_tu17_mp.wad' = 'A009122C6937572EF3D6A7B7FE93FAB07CE7F516CE86F8B11A3E6A941E1DFE16'
    'online_tu17_zm.wad' = 'DCD949E3871E87E37758D2910F3AF5E7B59A9D3E7AF0C418B9F5BEA4138CE105'
}
foreach ($name in $expected.Keys) {
    $path = Join-Path $publisher $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Publisher file missing: $name" }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $expected[$name]) {
        throw "Publisher file hash mismatch: $name"
    }
}

$launcher = Join-Path $project 'build/Release/BO2Z-Offline-Launcher.exe'
$backend = Join-Path $project 'build/Release/BO2Z-Offline.dll'
foreach ($path in @($launcher, $backend)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Build output missing: $path" }
}

$drop = Join-Path $staging 'Copy into Black Ops II folder'
$moduleRoot = Join-Path $drop 'BO2Z-Offline'
$pub = Join-Path $moduleRoot 'data/pub'
New-Item -ItemType Directory -Path $pub -Force | Out-Null
Copy-Item -LiteralPath $launcher -Destination $drop
Copy-Item -LiteralPath $backend -Destination (Join-Path $moduleRoot 'BO2Z-Offline.dll')
foreach ($name in $expected.Keys) { Copy-Item -LiteralPath (Join-Path $publisher $name) -Destination $pub }
foreach ($name in @('README.md','LICENSE','NOTICE','THIRD-PARTY-NOTICES.md')) {
    Copy-Item -LiteralPath (Join-Path $project $name) -Destination $staging
}

$files = @(Get-ChildItem -LiteralPath $staging -Recurse -File | ForEach-Object {
    @{ path = $_.FullName.Substring($staging.Length + 1).Replace('\','/');
       sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash;
       bytes = $_.Length }
})
if ($files.Count -ne 15) { throw "Unexpected release file count: $($files.Count)" }
$manifest = @{
    name = 'BO2Z-Offline'; version = $version;
    backend = 'fresh 1.0.0 source build';
    launcher = 'fresh 1.0.0 source build';
    personalProfilesIncluded = $false; logsIncluded = $false; files = $files
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $staging 'MANIFEST.json') -Encoding utf8
New-Item -ItemType Directory -Path (Split-Path -Parent $zip) -Force | Out-Null
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $zip
Write-Host "Release archive ready: $zip"
Write-Host 'No personal profiles or logs were included.'
