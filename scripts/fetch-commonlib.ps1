# Clones the pinned CommonLibSSE-NG release (alandtse fork) into extern/CommonLibSSE-NG, with its openvr submodule.
# xmake.lua includes that folder, so run this once before the first `xmake f`. Safe to re-run: it does nothing when the tag is already there.
$tag = 'v13.1.0'   # supports 1.7.104; keep in step with the README
$dir = Join-Path (Split-Path $PSScriptRoot -Parent) 'extern\CommonLibSSE-NG'
# git writes progress to stderr, which Windows PowerShell 5.1 turns into errors under 'Stop'; judge by exit code instead
$ErrorActionPreference = 'Continue'

if (Test-Path (Join-Path $dir 'xmake.lua')) {
  $have = (git -C $dir describe --tags --exact-match 2>$null)
  if ($have -eq $tag) { Write-Host "CommonLibSSE-NG $tag already in $dir"; return }
  throw "$dir exists but is not at $tag (found '$have'); delete it and re-run"
}

git clone --depth 1 --branch $tag --recurse-submodules --shallow-submodules https://github.com/alandtse/CommonLibSSE-NG.git $dir 2>&1 | Out-Host
if ($LASTEXITCODE -ne 0) { throw "git clone failed (exit $LASTEXITCODE)" }
