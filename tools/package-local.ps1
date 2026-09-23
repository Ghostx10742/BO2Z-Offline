param(
    [Parameter(Mandatory = $true)]
    [string] $PublisherDir
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$source = (Resolve-Path -LiteralPath $PublisherDir).Path
$output = Join-Path $project 'dist/local-install'
if (Test-Path -LiteralPath $output) {
    throw "Output already exists: $output. Move or rename it before creating another package."
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

$launcher = Join-Path $project 'build/Release/BO2Z-Offline-Launcher.exe'
$module = Join-Path $project 'build/Release/BO2Z-Offline.dll'
foreach ($path in @($launcher, $module)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Build output missing: $path. Run cmake --build first."
    }
}
foreach ($name in $expected.Keys) {
    $path = Join-Path $source $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Publisher resource missing: $name"
    }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($actual -ne $expected[$name]) {
        throw "Publisher resource hash does not match the tested game build: $name"
    }
}

$drop = Join-Path $output 'Copy into Black Ops II folder'
$moduleRoot = Join-Path $drop 'BO2Z-Offline'
$pub = Join-Path $moduleRoot 'data/pub'
New-Item -ItemType Directory -Path $pub | Out-Null
Copy-Item -LiteralPath $launcher -Destination $drop
Copy-Item -LiteralPath $module -Destination $moduleRoot
foreach ($name in $expected.Keys) {
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination $pub
}
Copy-Item -LiteralPath (Join-Path $project 'README.md') -Destination $output
Copy-Item -LiteralPath (Join-Path $project 'LICENSE') -Destination $output
Copy-Item -LiteralPath (Join-Path $project 'NOTICE') -Destination $output
Write-Host "Private local install layout ready: $output"
Write-Host 'It includes game publisher data. Do not upload or commit it without resolving redistribution rights.'
