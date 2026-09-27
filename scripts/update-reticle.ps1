<#
.SYNOPSIS
  Updates lib/Reticle to a Reticle tag: builds Reticle.Wpf at that tag from the Reticle repository cloned next to Pulse
  (../Reticle), in a temporary git worktree so the Reticle working copy is left alone, then copies Reticle.Wpf.dll and
  Reticle.Tokens.dll into lib/Reticle and updates the version line of lib/Reticle/README.md.
.EXAMPLE
  ./scripts/update-reticle.ps1 -Tag v0.5.0
  ./scripts/update-reticle.ps1 -Tag v0.6.0 -Reticle D:\src\Reticle
#>
param(
  [Parameter(Mandatory)] [string] $Tag,
  [string] $Reticle = (Join-Path $PSScriptRoot "..\..\Reticle")
)
$ErrorActionPreference = 'Stop'
$pulse = Resolve-Path (Join-Path $PSScriptRoot "..")
$Reticle = Resolve-Path $Reticle
$lib = Join-Path $pulse "lib\Reticle"
$worktree = Join-Path ([IO.Path]::GetTempPath()) "reticle-$Tag-$([guid]::NewGuid().ToString('N').Substring(0, 8))"

git -C $Reticle fetch --tags --quiet
git -C $Reticle worktree add --detach --quiet $worktree $Tag
try {
  dotnet build (Join-Path $worktree "src\Reticle.Wpf\Reticle.Wpf.csproj") -c Release -nologo -v q
  if ($LASTEXITCODE -ne 0) { throw "Reticle build failed" }
  foreach ($name in "Reticle.Wpf", "Reticle.Tokens") {
    Copy-Item (Join-Path $worktree "src\$name\bin\Release\net8.0-windows\$name.dll") (Join-Path $lib "$name.dll") -Force
  }
  $commit = git -C $worktree rev-parse --short HEAD
  $readme = Join-Path $lib "README.md"
  $text = [IO.File]::ReadAllText($readme)
  $text = [regex]::Replace($text, 'Release build of \*\*Reticle v[^*]+\*\* \(commit `[0-9a-f]+`\)', "Release build of **Reticle $Tag** (commit ``$commit``)")
  [IO.File]::WriteAllText($readme, $text)
  Write-Host "lib/Reticle updated to Reticle $Tag ($commit)"
}
finally {
  git -C $Reticle worktree remove --force $worktree
}
