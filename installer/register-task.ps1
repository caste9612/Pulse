# Registra i task pianificati di Pulse dai modelli dell'installer (task-logon.xml, task-crash.xml), al posto di schtasks: mette
# nei segnaposto l'exe (PULSE_EXE, PULSE_EXE_NAME), l'utente che installa (PULSE_USER, il suo SID) e il livello dei diritti
# (PULSE_RUNLEVEL). Un secondo task (-Template2, -Name2) si registra nella stessa chiamata, allo stesso livello. Lo lancia
# l'installer con Windows PowerShell 5.1; si può provare anche a mano, su un exe qualsiasi.
# Windows nega a un processo non elevato i task con i diritti massimi, e anche la sovrascrittura di un task registrato da
# amministratore: con "Install for me only" l'installer non è elevato. Se arriva quel rifiuto lo script si rilancia elevato, con
# una sola richiesta UAC per i task della chiamata, e passa il SID di chi installa (-User), che resta l'utente dei task anche se
# a confermare la richiesta è un altro account. Se l'utente rifiuta la richiesta i task non ci sono: è la sua scelta.
param(
  [Parameter(Mandatory)] [string] $Template,
  [Parameter(Mandatory)] [string] $Name,
  [string] $Template2,
  [string] $Name2,
  [Parameter(Mandatory)] [string] $Exe,
  [ValidateSet('HighestAvailable', 'LeastPrivilege')] [string] $RunLevel = 'LeastPrivilege',
  [string] $User
)
$ErrorActionPreference = 'Stop'

$me = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $User) { $User = $me.User.Value }
$elevated = ([Security.Principal.WindowsPrincipal]::new($me)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

function Escape([string] $text) { [Security.SecurityElement]::Escape($text) }

function Register([string] $template, [string] $name) {
  $xml = Get-Content -LiteralPath $template -Raw -Encoding UTF8
  # PULSE_EXE_NAME prima di PULSE_EXE, che ne è l'inizio
  $xml = $xml.Replace('PULSE_EXE_NAME', (Escape ([IO.Path]::GetFileName($Exe))))
  $xml = $xml.Replace('PULSE_EXE', (Escape $Exe))
  $xml = $xml.Replace('PULSE_USER', $User)
  $xml = $xml.Replace('PULSE_RUNLEVEL', $RunLevel)
  # -ErrorAction esplicito: i cmdlet del modulo ScheduledTasks non seguono $ErrorActionPreference dello script, e un rifiuto di
  # Windows passava in silenzio
  Register-ScheduledTask -TaskName $name -Xml $xml -Force -ErrorAction Stop | Out-Null
}

# il rifiuto arriva come errore CIM con il codice di accesso negato (E_ACCESSDENIED) nei suoi dati
function Test-Denied($exception) {
  if ($exception -is [UnauthorizedAccessException]) { return $true }
  try { return $exception.ErrorData.CimInstanceProperties['error_Code'].Value -eq 2147942405 } catch { return $false }
}

try {
  Register $Template $Name
  if ($Template2) { Register $Template2 $Name2 }
}
catch {
  if ($elevated -or -not (Test-Denied $_.Exception)) { throw }
  $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Template', "`"$Template`"", '-Name', "`"$Name`"",
    '-Exe', "`"$Exe`"", '-RunLevel', $RunLevel, '-User', $User)
  if ($Template2) { $arguments += @('-Template2', "`"$Template2`"", '-Name2', "`"$Name2`"") }
  $process = Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -Wait -PassThru -ArgumentList $arguments
  exit $process.ExitCode
}
