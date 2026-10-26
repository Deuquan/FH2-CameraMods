# Shows a folder picker until the chosen folder contains azimuth.exe. Prints the path, or nothing if cancelled.
Add-Type -AssemblyName System.Windows.Forms
$owner = New-Object System.Windows.Forms.Form
$owner.TopMost = $true
while ($true) {
  $d = New-Object System.Windows.Forms.FolderBrowserDialog
  $d.Description = 'Select your Azimuth folder (the folder that contains azimuth.exe)'
  $d.ShowNewFolderButton = $false
  if ($d.ShowDialog($owner) -ne [System.Windows.Forms.DialogResult]::OK) { exit 0 }
  $p = $d.SelectedPath
  if (Test-Path -LiteralPath (Join-Path $p 'azimuth.exe')) { Write-Output $p; exit 0 }
  [void][System.Windows.Forms.MessageBox]::Show($owner, "azimuth.exe was not found in:`n$p`n`nPlease pick the folder that contains azimuth.exe.", 'Camera Mods', 'OK', 'Warning')
}
