# CMake's Qt deployment targets supply DLLs/plugins, Sys, translations, and licenses.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$output = Join-Path $root 'build/Binaries'

# Deploy the app-local release CRT rather than requiring a separate installer.
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -version '[17.0,18.0)' -products '*' `
  -requires Microsoft.Component.MSBuild -property installationPath
if ($LASTEXITCODE -ne 0 -or !$vs) { throw 'Visual Studio installation not found' }
$redist = Get-ChildItem "$vs/VC/Redist/MSVC" -Directory |
  Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
  Sort-Object { [version]$_.Name } -Descending |
  Select-Object -First 1
if (!$redist) { throw 'MSVC redistributable directory not found' }
$crt = Join-Path $redist.FullName 'x64/Microsoft.VC143.CRT'
Copy-Item "$crt/*.dll" $output

foreach ($path in @(
  'Dolphin.exe', 'DolphinNoGUI.exe', 'Sys', 'Languages', 'COPYING', 'LICENSES',
  'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Svg.dll', 'qt.conf',
  'QtPlugins/platforms/qwindows.dll', 'vcruntime140.dll', 'msvcp140.dll'
)) {
  if (!(Test-Path (Join-Path $output $path))) { throw "Missing release file: $path" }
}

& "$output/DolphinNoGUI.exe" --version
if ($LASTEXITCODE -ne 0) { throw 'DolphinNoGUI startup check failed' }

# Stage only release files: Debug DLLs/executables may exist in this directory locally.
$stage = Join-Path $root 'release/windows'
if (Test-Path $stage) { throw "Release staging directory already exists: $stage" }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item "$output/Dolphin.exe", "$output/DolphinNoGUI.exe", "$output/qt.conf", `
  "$output/COPYING" $stage
foreach ($directory in @('Sys', 'Languages', 'LICENSES', 'QtPlugins')) {
  Copy-Item "$output/$directory" $stage -Recurse
}
Copy-Item "$output/Qt6Core.dll", "$output/Qt6Gui.dll", "$output/Qt6Widgets.dll", `
  "$output/Qt6Svg.dll", "$crt/*.dll" $stage

$archive = Join-Path $root 'release/dolphin-dap-windows-x64.zip'
Compress-Archive -Path "$stage/*" -DestinationPath $archive -CompressionLevel Optimal -Force
$hash = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  dolphin-dap-windows-x64.zip" |
  Set-Content "$archive.sha256" -Encoding ascii
