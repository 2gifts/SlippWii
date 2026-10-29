# SlippWii: copies your Slippi account (user.json, from Slippi Launcher on
# this PC) to the SD card, for SlippWii to log in with. Started by
# "Copy my Slippi account to SD.bat". Only that one file is copied, and only
# to the card; nothing is sent anywhere.
param([string]$Here = '')
$ErrorActionPreference = 'Stop'

function Stop-Copy($Message) {
    Write-Host ''
    Write-Host $Message -ForegroundColor Yellow
    exit 1
}

function Read-Account($Path) {
    try {
        $a = Get-Content -Raw -Encoding UTF8 $Path | ConvertFrom-Json
    } catch { return $null }
    if (-not $a.uid -or -not $a.playKey -or -not $a.connectCode) { return $null }
    return $a
}

function Describe($a) {
    $name = if ($a.displayName) { $a.displayName } else { '(no name)' }
    return "$name ($($a.connectCode))"
}

Write-Host 'SlippWii: copy your Slippi account to the SD card'
Write-Host '(SlippWii is unofficial: not made by Project Slippi. https://github.com/2gifts/SlippWii)'
Write-Host ''

# 1. The account: the one Slippi Launcher on this PC is logged in to.
$candidates = @()
foreach ($sub in @('netplay', 'netplay-beta')) {
    $p = Join-Path $env:APPDATA "Slippi Launcher\$sub\User\Slippi\user.json"
    if (Test-Path $p) { $candidates += $p }
}
$accounts = @()
foreach ($p in $candidates) {
    $a = Read-Account $p
    if ($a -and -not ($accounts | Where-Object { $_.Account.uid -eq $a.uid })) {
        $accounts += [pscustomobject]@{ Path = $p; Account = $a }
    }
}
if ($accounts.Count -eq 0) {
    Stop-Copy ("No Slippi account was found on this PC.`n" +
        "  1. Install Slippi Launcher (https://slippi.gg) and log in (or create an account).`n" +
        "  2. Click Play once, so it sets up Slippi Dolphin.`n" +
        "  3. Run this again.")
}
$pick = $accounts[0]
if ($accounts.Count -gt 1) {
    Write-Host 'Slippi accounts on this PC:'
    for ($i = 0; $i -lt $accounts.Count; $i++) { Write-Host "  $($i + 1). $(Describe $accounts[$i].Account)" }
    $n = Read-Host 'Type the number of the account for the Wii and press Enter'
    if ($n -match '^\d+$' -and [int]$n -ge 1 -and [int]$n -le $accounts.Count) { $pick = $accounts[[int]$n - 1] }
}

# 2. The SD card: where this script is (the zip extracted to the card), or a
#    drive that has SlippWii on it.
$cards = @()
if ($Here -and (Test-Path (Join-Path $Here 'slippwii\netplay.gct'))) {
    $cards += (Resolve-Path $Here).Path
} else {
    foreach ($d in [IO.DriveInfo]::GetDrives()) {
        if ($d.IsReady -and (Test-Path (Join-Path $d.RootDirectory.FullName 'slippwii\netplay.gct'))) {
            $cards += $d.RootDirectory.FullName
        }
    }
}
if ($cards.Count -eq 0) {
    Stop-Copy ("SlippWii was not found on any SD card.`n" +
        "Put the Wii's SD card in this PC, extract the SlippWii zip onto it (so it has`n" +
        "the apps and slippwii folders), then run this again from the SD card.")
}
$card = $cards[0]
if ($cards.Count -gt 1) {
    Write-Host 'Drives with SlippWii:'
    for ($i = 0; $i -lt $cards.Count; $i++) { Write-Host "  $($i + 1). $($cards[$i])" }
    $n = Read-Host 'Type the number of the Wii''s SD card and press Enter'
    if ($n -match '^\d+$' -and [int]$n -ge 1 -and [int]$n -le $cards.Count) { $card = $cards[[int]$n - 1] }
}

# 3. Copy (the previous account, if a different one, is kept beside it).
$dest = Join-Path $card 'slippwii\user.json'
if (Test-Path $dest) {
    $old = Read-Account $dest
    if ((Get-FileHash $dest).Hash -eq (Get-FileHash $pick.Path).Hash) {
        Write-Host "Your account $(Describe $pick.Account) is already on the SD card ($card)."
        Write-Host 'Nothing to do: put the card in your Wii and start SlippWii.'
        exit 0
    }
    $backup = "$dest.old-$(Get-Date -Format yyyyMMdd-HHmmss)"
    Move-Item $dest $backup
    if ($old) { Write-Host "The SD card had the account $(Describe $old): kept as $(Split-Path -Leaf $backup)." }
}
Copy-Item $pick.Path $dest
try { Write-VolumeCache -DriveLetter $card[0] -ErrorAction SilentlyContinue } catch {}
Write-Host ''
Write-Host "Done: your Slippi account $(Describe $pick.Account) is on the SD card ($card)." -ForegroundColor Green
Write-Host ''
Write-Host 'Next: safely eject the SD card, put it in your Wii, and start SlippWii'
Write-Host 'from the Homebrew Channel. (Keep user.json private: it is your Slippi login.)'
exit 0
