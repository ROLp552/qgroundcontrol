# Builds a signed arm64 QGroundControl APK for the RadioMaster AX12 on Windows.
# Usage (from the repo root):  powershell -ExecutionPolicy Bypass -File .\build-ax12.ps1
# Adjust the paths below if you installed things in other places.

$ErrorActionPreference = 'Stop'

$QtRoot      = 'C:\Qt\6.8.3'
$AndroidSdk  = 'C:\Android\Sdk'
$AndroidNdk  = "$AndroidSdk\ndk\26.1.10909125"
$JavaHome    = (Get-ChildItem 'C:\Program Files\Eclipse Adoptium' -Directory -Filter 'jdk-17*' | Select-Object -First 1).FullName
$Keystore    = 'C:\qgc-keys\ax12.keystore'
$KeyAlias    = 'ax12'
$BuildDir    = 'C:\qgc-build'   # Outside OneDrive on purpose: the build has tens of thousands of files
$SourceDir   = $PSScriptRoot
# GStreamer Android SDK extracted by hand (the automatic ~440 MB download tends to get cut off)
$GstDir      = 'C:\qgc-deps\gstreamer-1.22.12'

foreach ($p in "$QtRoot\android_arm64_v8a\bin\qt-cmake.bat", "$QtRoot\msvc2022_64", $AndroidSdk, $AndroidNdk, $JavaHome, $Keystore) {
    if (-not $p -or -not (Test-Path $p)) { throw "Not found: $p  (check the install steps)" }
}

$env:JAVA_HOME        = $JavaHome
$env:ANDROID_SDK_ROOT = $AndroidSdk
$env:ANDROID_NDK_ROOT = $AndroidNdk
# Reload PATH from the registry so freshly installed CMake/Ninja are found
$env:Path             = "$JavaHome\bin;" + [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
foreach ($tool in 'cmake','ninja') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not found in PATH" }
}

# Ask for the keystore password and check it right away (signing is the very last build step)
while ($true) {
    $pass = Read-Host 'Keystore password' -AsSecureString
    $plain = [Runtime.InteropServices.Marshal]::PtrToStringAuto([Runtime.InteropServices.Marshal]::SecureStringToBSTR($pass))
    & "$JavaHome\bin\keytool.exe" -list -keystore $Keystore -storepass $plain -alias $KeyAlias *> $null
    if ($LASTEXITCODE -eq 0) { break }
    Write-Host 'Wrong password for the keystore, try again.' -ForegroundColor Yellow
}
$env:QT_ANDROID_KEYSTORE_PATH       = $Keystore
$env:QT_ANDROID_KEYSTORE_ALIAS      = $KeyAlias
$env:QT_ANDROID_KEYSTORE_STORE_PASS = $plain
$env:QT_ANDROID_KEYSTORE_KEY_PASS   = $plain

git -C $SourceDir submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw 'git submodule update failed' }

$extraArgs = @()
if (Test-Path "$GstDir\arm64\lib") { $extraArgs += "-DCPM_gstreamer_SOURCE=$($GstDir -replace '\\','/')" }  # CMake needs forward slashes

& "$QtRoot\android_arm64_v8a\bin\qt-cmake.bat" -S $SourceDir -B $BuildDir -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_WARN_DEPRECATED=FALSE `
    -DQT_ANDROID_ABIS=arm64-v8a `
    -DQT_ANDROID_BUILD_ALL_ABIS=OFF `
    "-DQT_HOST_PATH=$QtRoot\msvc2022_64" `
    "-DANDROID_SDK_ROOT=$AndroidSdk" `
    "-DANDROID_NDK_ROOT=$AndroidNdk" `
    -DQT_ANDROID_SIGN_APK=ON `
    @extraArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }

cmake --build $BuildDir --target all --config Release
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

Write-Host "`nAPK(s) ready:" -ForegroundColor Green
Get-ChildItem "$BuildDir\android-build" -Recurse -Filter *.apk | ForEach-Object { $_.FullName }
