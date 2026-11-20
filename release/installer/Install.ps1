# Refunct Utility installer. Runs as the current user; no network or registry changes.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
function Ask([string]$Message) {
    return [System.Windows.Forms.MessageBox]::Show($Message, 'Refunct Utility 1.0.0', 'YesNo', 'Question') -eq 'Yes'
}
function Sha256([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Test-X86Dll([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $reader = New-Object IO.BinaryReader($stream)
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) { return $false }
        $stream.Position = 0x3C
        $pe = $reader.ReadUInt32()
        if ([long]$pe + 24 -gt $stream.Length) { return $false }
        $stream.Position = $pe
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x14C) { return $false }
        $stream.Position = [long]$pe + 22
        return ($reader.ReadUInt16() -band 0x2000) -ne 0
    } finally { $stream.Dispose() }
}
$backup = $null
$changed = New-Object 'System.Collections.Generic.List[object]'
$tempFiles = New-Object 'System.Collections.Generic.List[string]'
try {
    $manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw | ConvertFrom-Json
    $expectedGame = 'f9145dc602580414a81c56ad9c23b618e1ee164a2fb9911f53399c67711027de'
    if ($manifest.gameSha256 -ne $expectedGame) { throw 'Unexpected game compatibility manifest.' }
    $allowedLoaders = @('dsound.dll','version.dll','winmm.dll','dbghelp.dll','xinput1_3.dll')
    $payload = Join-Path $PSScriptRoot 'payload'
    $files = @($manifest.files)
    if ($files.Count -lt 1 -or $files.Count -gt 2 -or @($files | Where-Object {$_.name -eq 'RefunctUtility.asi'}).Count -ne 1) { throw 'Invalid payload manifest.' }
    if ($files.Count -eq 2 -and @($files | Where-Object {$allowedLoaders -contains $_.name}).Count -ne 1) { throw 'Invalid loader name.' }
    foreach ($entry in $files) {
        $path = Join-Path $payload $entry.name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing payload: $($entry.name)" }
        if ((Sha256 $path) -ne $entry.sha256 -or -not (Test-X86Dll $path)) { throw "Invalid or damaged x86 payload: $($entry.name)" }
    }
    if (Get-Process -Name 'Refunct-Win32-Shipping' -ErrorAction SilentlyContinue) { throw 'Close Refunct before installing, then run Install.bat again.' }
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select Refunct-Win32-Shipping.exe (Steam > Refunct > Browse local files)'
    $dialog.Filter = 'Refunct game executable|Refunct-Win32-Shipping.exe'
    $dialog.FileName = 'Refunct-Win32-Shipping.exe'
    $dialog.CheckFileExists = $true
    try {
        if ($dialog.ShowDialog() -ne 'OK') { return }
        $exe = $dialog.FileName
    } finally { $dialog.Dispose() }
    if ([IO.Path]::GetFileName($exe) -ine 'Refunct-Win32-Shipping.exe' -or (Sha256 $exe) -ne $expectedGame) { throw 'Unsupported game executable. This release only supports the verified Refunct Win32 build; no files were changed.' }
    $destination = Split-Path -Parent $exe
    $duplicates = @(Get-ChildItem -LiteralPath $destination -File -Filter 'RefunctUtility*.asi' | Where-Object {$_.Name -ine 'RefunctUtility.asi'})
    if ($duplicates.Count -gt 0) { throw ('Other Refunct Utility ASIs are present. Move them out of Win32 before installation: ' + ($duplicates.Name -join ', ')) }
    $installFiles = @($files | Where-Object {$_.name -eq 'RefunctUtility.asi'})
    $loader = @($files | Where-Object {$_.name -ne 'RefunctUtility.asi'})
    if ($loader.Count -eq 0) {
        if (-not (Ask 'This package does not include an ASI loader. Have you already installed a compatible 32-bit ASI loader in the selected game folder?')) { return }
    } else {
        $loaderTarget = Join-Path $destination $loader[0].name
        if (Test-Path -LiteralPath $loaderTarget) {
            if ((Sha256 $loaderTarget) -ne $loader[0].sha256) {
                if (-not (Ask "A different $($loader[0].name) already exists. It will NOT be overwritten. Continue ONLY if your existing loader already works with Refunct?")) { return }
            }
        } else {
            if (-not (Ask "Install the packaged ASI loader ($($loader[0].name))? Choose No if you already use a different ASI loader (this cancels installation).")) { return }
            $installFiles += $loader[0]
        }
    }
    $diag = Join-Path $destination 'RefunctUtility.diagnostic.ini'
    $summary = "Install Refunct Utility 1.0.0 in:`n$destination`n`nExisting ASI and diagnostic configuration will be backed up if present. Logs, settings, profiles and replays will not be changed."
    if (-not (Ask $summary)) { return }
    if (Get-Process -Name 'Refunct-Win32-Shipping' -ErrorAction SilentlyContinue) { throw 'Refunct is running. Close it and retry.' }
    $backup = Join-Path (Split-Path -Parent $destination) ('RefunctUtility-install-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
    New-Item -ItemType Directory -Path $backup | Out-Null
    # Back up every existing file before any modification. Stage payload on the same volume.
    foreach ($entry in $installFiles) {
        $target = Join-Path $destination $entry.name
        $existed = Test-Path -LiteralPath $target -PathType Leaf
        if ($existed) { Copy-Item -LiteralPath $target -Destination (Join-Path $backup $entry.name) }
        $temp = Join-Path $destination ('.refunct-install-' + [Guid]::NewGuid().ToString('N') + '.tmp')
        $tempFiles.Add($temp)
        Copy-Item -LiteralPath (Join-Path $payload $entry.name) -Destination $temp
        if ((Sha256 $temp) -ne $entry.sha256) { throw 'Staging hash verification failed.' }
        $entry | Add-Member -NotePropertyName tempPath -NotePropertyValue $temp
        $entry | Add-Member -NotePropertyName existed -NotePropertyValue $existed
    }
    $hadDiag = Test-Path -LiteralPath $diag -PathType Leaf
    if ($hadDiag) { Copy-Item -LiteralPath $diag -Destination (Join-Path $backup 'RefunctUtility.diagnostic.ini') }
    foreach ($entry in $installFiles) {
        $target = Join-Path $destination $entry.name
        # File.Replace is atomic for an existing file, File.Move for a new file.
        if ($entry.existed) { [IO.File]::Replace($entry.tempPath, $target, (Join-Path $backup ($entry.name + '.replace-backup'))) }
        else { [IO.File]::Move($entry.tempPath, $target) }
        $changed.Add([pscustomobject]@{name=$entry.name; existed=$entry.existed})
        if ((Sha256 $target) -ne $entry.sha256) { throw "Installed hash mismatch: $($entry.name)" }
    }
    if ($hadDiag) {
        Remove-Item -LiteralPath $diag
        $changed.Add([pscustomobject]@{name='RefunctUtility.diagnostic.ini'; existed=$true})
    }
    [void][System.Windows.Forms.MessageBox]::Show("Installed successfully. Start Refunct and press F5.`n`nBackups: $backup", 'Refunct Utility 1.0.0', 'OK', 'Information')
} catch {
    $errorMessage = $_.Exception.Message
    $rollbackErrors = @()
    for ($i=$changed.Count-1; $i -ge 0; $i--) {
        $item = $changed[$i]
        $target = Join-Path $destination $item.name
        try {
            if ($item.existed) { Copy-Item -LiteralPath (Join-Path $backup $item.name) -Destination $target -Force }
            else { Remove-Item -LiteralPath $target -Force }
        } catch { $rollbackErrors += $_.Exception.Message }
    }
    if ($rollbackErrors.Count) { $errorMessage += "`nRollback incomplete. Backups: $backup`n" + ($rollbackErrors -join "`n") }
    [void][System.Windows.Forms.MessageBox]::Show($errorMessage, 'Installation not completed', 'OK', 'Error')
    Write-Error $errorMessage -ErrorAction Continue
    exit 1
} finally {
    foreach ($temp in $tempFiles) { if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue } }
}
