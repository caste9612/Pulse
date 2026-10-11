# Registra un task pianificato di Pulse da un modello dell'installer (task-logon.xml, task-crash.xml), al posto di schtasks: mette
# nei segnaposto l'exe (PULSE_EXE, PULSE_EXE_NAME), l'utente che installa (PULSE_USER, il suo SID) e il livello dei diritti
# (PULSE_RUNLEVEL). Lo lancia l'installer con Windows PowerShell 5.1; si può provare anche a mano, su un exe qualsiasi.
param(
  [Parameter(Mandatory)] [string] $Template,
  [Parameter(Mandatory)] [string] $Name,
  [Parameter(Mandatory)] [string] $Exe,
  [ValidateSet('HighestAvailable', 'LeastPrivilege')] [string] $RunLevel = 'LeastPrivilege'
)
$ErrorActionPreference = 'Stop'

function Escape([string] $text) { [Security.SecurityElement]::Escape($text) }

$xml = Get-Content -LiteralPath $Template -Raw -Encoding UTF8
# PULSE_EXE_NAME prima di PULSE_EXE, che ne è l'inizio
$xml = $xml.Replace('PULSE_EXE_NAME', (Escape ([IO.Path]::GetFileName($Exe))))
$xml = $xml.Replace('PULSE_EXE', (Escape $Exe))
$xml = $xml.Replace('PULSE_USER', [Security.Principal.WindowsIdentity]::GetCurrent().User.Value)
$xml = $xml.Replace('PULSE_RUNLEVEL', $RunLevel)
Register-ScheduledTask -TaskName $Name -Xml $xml -Force | Out-Null
