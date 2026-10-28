# Started by "Install SlippWii.bat". Fetches a private copy of Python (pinned
# and hash-checked, kept in %LOCALAPPDATA%\SlippWii) and runs install.py with
# it. Nothing is installed on the PC itself.
param([string]$Iso = '')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch {}

$PythonUrl = 'https://api.nuget.org/v3-flatcontainer/python/3.14.7/python.3.14.7.nupkg'
$PythonSha256 = '46A4DA5529A92D18FF894911F6E6033A8253198D705B8161BF28C9123C87D46B'
$Work = Join-Path $env:LOCALAPPDATA 'SlippWii'
$Python = Join-Path $Work 'python\python.exe'

function Stop-Setup($Message) {
    Write-Host ''
    Write-Host $Message -ForegroundColor Yellow
    exit 1
}

function Get-Python($Package) {
    # Three tries, then Windows' own curl.exe (same pinned file either way).
    for ($try = 1; $try -le 3; $try++) {
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $PythonUrl -OutFile $Package -TimeoutSec 120
            return
        } catch {
            $last = $_.Exception.Message
            Start-Sleep -Seconds (5 * $try)
        }
    }
    $curl = Join-Path $env:SystemRoot 'System32\curl.exe'
    if (Test-Path $curl) {
        try {
            & $curl -fsSL --retry 3 -o $Package $PythonUrl 2>$null
            if ($LASTEXITCODE -eq 0) { return }
        } catch {}
    }
    Stop-Setup ("Could not download Python ($last).`n" +
        "Check your internet connection and that your PC's date and time are correct,`n" +
        "then run the installer again.")
}

function Install-Python {
    $package = Join-Path $Work 'downloads\python.3.14.7.zip'
    New-Item -ItemType Directory -Force -Path (Split-Path $package) | Out-Null
    if (-not (Test-Path $package) -or (Get-FileHash $package -Algorithm SHA256).Hash -ne $PythonSha256) {
        Write-Host 'Getting a private copy of Python for the installer (15 MB, once)...'
        Get-Python $package
        if ((Get-FileHash $package -Algorithm SHA256).Hash -ne $PythonSha256) {
            Remove-Item $package
            Stop-Setup 'The Python download was damaged. Run the installer again.'
        }
    }
    $unpack = Join-Path $Work 'python-unpack'
    $dest = Join-Path $Work 'python'
    foreach ($dir in @($unpack, $dest)) { if (Test-Path $dir) { Remove-Item -Recurse -Force $dir } }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($package, $unpack)
    Move-Item (Join-Path $unpack 'tools') $dest
    Remove-Item -Recurse -Force $unpack
}

function Test-Python {
    if (-not (Test-Path $Python)) { return $false }
    try {
        & $Python -c 'import ssl, ctypes, json' 2>$null
        return $LASTEXITCODE -eq 0
    } catch { return $false }
}

New-Item -ItemType Directory -Force -Path $Work | Out-Null
if (-not (Test-Python)) {
    Install-Python
    if (-not (Test-Python)) { Stop-Setup 'The private copy of Python does not start on this PC.' }
}

$installer = Join-Path $PSScriptRoot 'install.py'
$env:PYTHONIOENCODING = 'utf-8'
if ($Iso) { & $Python -I $installer $Iso } else { & $Python -I $installer }
exit $LASTEXITCODE
