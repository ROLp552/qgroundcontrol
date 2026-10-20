# Creates the keystore used by build-ax12.ps1 to sign the APK. Run once.
$ErrorActionPreference = 'Stop'
$jdk = (Get-ChildItem 'C:\Program Files\Eclipse Adoptium' -Directory -Filter 'jdk-17*' | Select-Object -First 1).FullName
New-Item -ItemType Directory -Force 'C:\qgc-keys' | Out-Null
if (Test-Path 'C:\qgc-keys\ax12.keystore') { throw 'C:\qgc-keys\ax12.keystore already exists' }
& "$jdk\bin\keytool.exe" -genkeypair -v -keystore 'C:\qgc-keys\ax12.keystore' -alias ax12 -keyalg RSA -keysize 2048 -validity 10000 -dname 'CN=AX12 QGC'
