<#
.SYNOPSIS
    Gera o pacote de instalação do Synkro: pasta pronta + .zip + SHA256SUMS.

.DESCRIPTION
    1. Compila em Release (preset mingw-release), a menos que -SkipBuild.
    2. Copia synkro-desktop.exe e synkro-worker.exe e roda o windeployqt
       (Qt, plugins e runtime do MinGW ao lado dos executáveis).
    3. Acrescenta install.ps1, uninstall.ps1, a documentação e os exemplos.
    4. Confere que o worker roda a partir do pacote sem o Qt do PATH.
    5. Opcional: assina os executáveis com signtool (-CertificateThumbprint).
    6. Gera SHA256SUMS.txt e o .zip.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File installer\package.ps1
#>
[CmdletBinding()]
param(
    [string]$QtDir = $(if ($env:SYNKRO_QT_DIR) { $env:SYNKRO_QT_DIR } else { 'C:\Qt\6.12.0\mingw_64' }),
    [string]$QtToolsDir = 'C:\Qt\Tools',
    [string]$OutDir,
    [string]$CertificateThumbprint,
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $OutDir) { $OutDir = Join-Path $root 'out\package' }
$QtDir = $QtDir -replace '/', '\'

function Invoke-Checked([string]$what, [scriptblock]$block) {
    & $block
    if ($LASTEXITCODE -ne 0) { throw "$what falhou (código $LASTEXITCODE)" }
}

$env:PATH = "$QtToolsDir\mingw1310_64\bin;$QtToolsDir\Ninja;$QtToolsDir\CMake_64\bin;$QtDir\bin;$env:PATH"
$env:SYNKRO_QT_DIR = $QtDir -replace '\\', '/'
# O FetchContent (yaml-cpp) clona com git: usa o do PATH ou o do GitHub Desktop.
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    $desktopGit = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'GitHubDesktop') -Recurse -Filter git.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\git\\cmd\\git\.exe$' } | Sort-Object FullName -Descending | Select-Object -First 1
    if ($desktopGit) { $env:PATH = "$($desktopGit.DirectoryName);$env:PATH" }
    else { Write-Warning 'git não encontrado: a primeira configuração (FetchContent) vai falhar' }
}

$version = ((Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern '^\s*VERSION\s+([0-9.]+)').Matches[0].Groups[1].Value)
if (-not $version) { throw 'versão não encontrada no CMakeLists.txt' }

Push-Location $root
try {
    if (-not $SkipBuild) {
        Write-Host "Compilando o Synkro $version (Release)..."
        Invoke-Checked 'configuração do CMake' { cmake --preset mingw-release | Out-Host }
        Invoke-Checked 'compilação' { cmake --build --preset mingw-release | Out-Host }
    }
    $build = Join-Path $root 'out\build\mingw-release'
    foreach ($exe in 'synkro-desktop.exe', 'synkro-worker.exe') {
        if (-not (Test-Path (Join-Path $build $exe))) { throw "$exe não encontrado em $build (rode sem -SkipBuild)" }
    }

    $name = "Synkro-$version-win64"
    $stage = Join-Path $OutDir $name
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force $stage | Out-Null

    Copy-Item (Join-Path $build 'synkro-desktop.exe'), (Join-Path $build 'synkro-worker.exe') $stage
    Write-Host 'Copiando o Qt e o runtime (windeployqt)...'
    Invoke-Checked 'windeployqt' {
        & "$QtDir\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw `
            --compiler-runtime --dir $stage (Join-Path $stage 'synkro-desktop.exe') (Join-Path $stage 'synkro-worker.exe') | Out-Null
    }

    Copy-Item (Join-Path $PSScriptRoot 'install.ps1'), (Join-Path $PSScriptRoot 'uninstall.ps1') $stage
    New-Item -ItemType Directory -Force (Join-Path $stage 'docs'), (Join-Path $stage 'examples') | Out-Null
    Copy-Item (Join-Path $root 'README.md') $stage
    Copy-Item (Join-Path $root 'docs\*.md') (Join-Path $stage 'docs')
    Copy-Item -Recurse (Join-Path $root 'config\examples\*') (Join-Path $stage 'examples')

    $commit = ''
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) { $commit = (& git -C $root rev-parse --short HEAD 2>$null) }
    @"
Synkro $version
commit: $commit
gerado em: $((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))
"@ | Set-Content -Encoding utf8 (Join-Path $stage 'VERSION.txt')

    # O pacote precisa funcionar sem o Qt instalado: roda o worker com um PATH mínimo.
    Write-Host 'Conferindo o pacote sem o Qt no PATH...'
    $saved = $env:PATH
    try {
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
        $out = & (Join-Path $stage 'synkro-worker.exe') --version 2>&1
        if ($LASTEXITCODE -ne 0) { throw "synkro-worker não roda a partir do pacote: $out" }
        Write-Host "  $out"
    } finally { $env:PATH = $saved }

    if ($CertificateThumbprint) {
        $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending | Select-Object -First 1
        if (-not $signtool) { throw 'signtool.exe não encontrado (instale o Windows SDK)' }
        Write-Host 'Assinando os executáveis...'
        Invoke-Checked 'assinatura' {
            & $signtool.FullName sign /sha1 $CertificateThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 `
                (Join-Path $stage 'synkro-desktop.exe') (Join-Path $stage 'synkro-worker.exe') | Out-Host
        }
    } else {
        Write-Warning 'pacote NÃO assinado (informe -CertificateThumbprint para assinar)'
    }

    # Somas de verificação de todos os arquivos (conferidas pelo install.ps1).
    $sums = Get-ChildItem -Recurse -File $stage | Where-Object { $_.Name -ne 'SHA256SUMS.txt' } | ForEach-Object {
        $rel = $_.FullName.Substring($stage.Length + 1)
        "{0}  {1}" -f (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant(), $rel
    }
    $sums | Set-Content -Encoding utf8 (Join-Path $stage 'SHA256SUMS.txt')

    $zip = Join-Path $OutDir "$name.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path $stage -DestinationPath $zip
    "{0}  {1}" -f (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant(), (Split-Path -Leaf $zip) |
        Set-Content -Encoding ascii "$zip.sha256"

    Write-Host ''
    Write-Host "Pacote: $stage"
    Write-Host "Zip:    $zip"
} finally {
    Pop-Location
}
