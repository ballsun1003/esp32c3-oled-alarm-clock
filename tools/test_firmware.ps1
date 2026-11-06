param(
  [string]$Fqbn = 'esp32:esp32:esp32c3:PartitionScheme=no_ota',
  [string]$ArduinoCli = '',
  [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$firmwareRoot = Split-Path -Parent $PSScriptRoot
if (-not $ArduinoCli) {
  $cliCommand = Get-Command arduino-cli -ErrorAction SilentlyContinue
  if ($cliCommand) { $ArduinoCli = $cliCommand.Source }
  else { $ArduinoCli = Join-Path $env:ProgramFiles 'Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe' }
}
if (-not (Test-Path -LiteralPath $ArduinoCli)) { throw 'Arduino CLI not found. Pass -ArduinoCli with its full path.' }

# Compile exact copies of the six firmware sources. Keep PC tests, historical
# backups and build outputs out of Arduino's recursive sketch discovery.
$firmwareRun = Join-Path $firmwareRoot '_backup/firmware-build'
$firmwareSketch = Join-Path $firmwareRun 'sketch/co2'
$firmwareOutput = Join-Path $firmwareRun 'output'
New-Item -ItemType Directory -Force -Path $firmwareSketch,$firmwareOutput | Out-Null
$firmwareFiles = @('co2.ino','oled_input.h','oled_widgets.h','oled_ui.h','oled_motion.h','oled_path.h')
$sourceRecord = foreach ($firmwareFile in $firmwareFiles) {
  $sourcePath = Join-Path $firmwareRoot $firmwareFile
  $copyPath = Join-Path $firmwareSketch $firmwareFile
  Copy-Item -LiteralPath $sourcePath -Destination $copyPath -Force
  $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourcePath).Hash
  if ((Get-FileHash -Algorithm SHA256 -LiteralPath $copyPath).Hash -ne $sourceHash) {
    throw "Firmware copy differs: $firmwareFile"
  }
  [ordered]@{name=$firmwareFile;sha256=$sourceHash}
}
$compileArgs = @('compile','--fqbn',$Fqbn,'--warnings','all','--jobs','4','--build-path',$firmwareOutput)
if ($Clean) { $compileArgs += '--clean' }
$compileArgs += $firmwareSketch
& $ArduinoCli @compileArgs 2>&1 | Tee-Object -FilePath (Join-Path $firmwareRun 'compile.log')
$compileExit = $LASTEXITCODE
foreach ($source in $sourceRecord) {
  if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $firmwareRoot $source.name)).Hash -ne $source.sha256) {
    throw "Source changed during build: $($source.name). Run again against the final source."
  }
}
[ordered]@{
  completedAt=(Get-Date).ToString('o');fqbn=$Fqbn;exitCode=$compileExit;
  uploaded=$false;cleanBuild=[bool]$Clean;sources=$sourceRecord
} | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $firmwareRun 'result.json')
if ($compileExit -ne 0) { throw "Firmware compile failed (exit $compileExit); see _backup/firmware-build/compile.log." }
