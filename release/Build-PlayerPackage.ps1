# Build a distribution ZIP from a freshly compiled ASI. No network requests.
param([string]$AsiPath, [string]$OutputDir)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
function PickFile([string]$Title, [string]$Filter) {
    $d = New-Object System.Windows.Forms.OpenFileDialog
    $d.Title = $Title; $d.Filter = $Filter; $d.CheckFileExists = $true
    try { if ($d.ShowDialog() -eq 'OK') { return $d.FileName } } finally { $d.Dispose() }
}
function Sha256([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Assert-X86Dll([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $r = New-Object IO.BinaryReader($stream)
        if ($stream.Length -lt 64 -or $r.ReadUInt16() -ne 0x5A4D) { throw "Not a Windows binary: $Path" }
        $stream.Position = 0x3C; $pe = $r.ReadUInt32()
        if ([long]$pe + 24 -gt $stream.Length) { throw 'Invalid PE header.' }
        $stream.Position = $pe
        if ($r.ReadUInt32() -ne 0x4550 -or $r.ReadUInt16() -ne 0x14C) { throw "Not a Win32 x86 binary: $Path" }
        $stream.Position = [long]$pe + 22
        if (($r.ReadUInt16() -band 0x2000) -eq 0) { throw "Not a DLL/ASI: $Path" }
    } finally { $stream.Dispose() }
}
$stage = $null
try {
    if (-not $AsiPath) { $AsiPath = PickFile 'Select the freshly built RefunctUtility.asi' 'Refunct Utility|RefunctUtility.asi' }
    if (-not $AsiPath) { return }
    $AsiPath = (Resolve-Path -LiteralPath $AsiPath).Path
    if ([IO.Path]::GetFileName($AsiPath) -ine 'RefunctUtility.asi') { throw 'Select RefunctUtility.asi.' }
    Assert-X86Dll $AsiPath
    # Require the release-pinned digest to distinguish this source build from older ASIs.
    $gameHash = 'f9145dc602580414a81c56ad9c23b618e1ee164a2fb9911f53399c67711027de'
    $binaryText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($AsiPath))
    if (-not $binaryText.Contains($gameHash)) { throw 'The ASI lacks the release game fingerprint. Rebuild using the supplied release source first.' }
    $loaderPath = $null; $loaderNotice = $null
    if ([System.Windows.Forms.MessageBox]::Show('Bundle a compatible ASI loader for new players? Choose No to make a package for players who already installed a loader.', 'Create player package', 'YesNo', 'Question') -eq 'Yes') {
        $loaderPath = PickFile 'Select the working x86 ASI loader DLL (not a game/system DLL)' 'ASI loader DLL|*.dll'
        if (-not $loaderPath) { throw 'Loader selection cancelled. No package created.' }
        $loaderName = [IO.Path]::GetFileName($loaderPath).ToLowerInvariant()
        if (@('dsound.dll','version.dll','winmm.dll','dbghelp.dll','xinput1_3.dll') -notcontains $loaderName) { throw 'This loader filename is not in the supported proxy list for the supplied game executable.' }
        Assert-X86Dll $loaderPath
        $loaderNotice = PickFile 'Select loader license and redistribution notices (include source information if required)' 'Text notices|*.txt;*.md'
        if (-not $loaderNotice) { throw 'A redistribution notice is required for a bundled loader.' }
        if ([System.Windows.Forms.MessageBox]::Show('Confirm this is your tested ASI loader, redistribution is allowed, and you have supplied the required notices and any required source distribution separately.', 'Loader redistribution', 'YesNo', 'Warning') -ne 'Yes') { return }
    }
    if (-not $OutputDir) { $OutputDir = Join-Path $PSScriptRoot 'dist' }
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    $OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path
    $stage = Join-Path ([IO.Path]::GetTempPath()) ('RefunctRelease-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path (Join-Path $stage 'payload') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $stage 'licenses') -Force | Out-Null
    foreach ($file in @('Install.bat','Install.ps1','README.txt')) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot "installer\$file") -Destination $stage }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'MinHook-LICENSE.txt') -Destination (Join-Path $stage 'licenses\MinHook-LICENSE.txt')
    Copy-Item -LiteralPath $AsiPath -Destination (Join-Path $stage 'payload\RefunctUtility.asi')
    $entries = @([ordered]@{name='RefunctUtility.asi';sha256=(Sha256 (Join-Path $stage 'payload\RefunctUtility.asi'))})
    if ($loaderPath) {
        Copy-Item -LiteralPath $loaderPath -Destination (Join-Path $stage "payload\$loaderName")
        Copy-Item -LiteralPath $loaderNotice -Destination (Join-Path $stage 'licenses\ASI-loader-notices.txt')
        $entries += [ordered]@{name=$loaderName;sha256=(Sha256 (Join-Path $stage "payload\$loaderName"))}
    }
    $manifest = [ordered]@{version='1.0.0';platform='Win32-x86';gameSha256=$gameHash;files=$entries}
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'manifest.json') -Encoding UTF8
    $zipName = if ($loaderPath) {'RefunctUtility-v1.0.0-Win32.zip'} else {'RefunctUtility-v1.0.0-Win32-ASI-only.zip'}
    $zip = Join-Path $OutputDir $zipName
    if (Test-Path -LiteralPath $zip) { $zip = Join-Path $OutputDir ([IO.Path]::GetFileNameWithoutExtension($zipName) + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8) + '.zip') }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($stage,$zip,[IO.Compression.CompressionLevel]::Optimal,$false)
    (Sha256 $zip) + '  ' + [IO.Path]::GetFileName($zip) | Set-Content -LiteralPath ($zip + '.sha256') -Encoding ASCII
    Write-Host "Created: $zip"
    [void][System.Windows.Forms.MessageBox]::Show("Player package created:`n$zip`n`nTest this on a clean Refunct installation before publishing.", 'Package created', 'OK', 'Information')
} catch {
    [void][System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Package not created', 'OK', 'Error')
    Write-Error $_.Exception.Message -ErrorAction Continue
    exit 1
} finally { if ($stage -and (Test-Path -LiteralPath $stage)) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue } }
