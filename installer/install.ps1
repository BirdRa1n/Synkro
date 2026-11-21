<#
.SYNOPSIS
    Instala ou atualiza o Synkro nesta máquina. Execute como administrador,
    de dentro da pasta do pacote.

.DESCRIPTION
    - confere o SHA-256 de cada arquivo do pacote (SHA256SUMS.txt);
    - espera um backup em andamento terminar (nunca interrompe o worker);
    - copia os programas para Program Files;
    - cria C:\ProgramData\Synkro com as permissões de docs/operations.md;
    - registra a origem "Synkro" no Visualizador de Eventos;
    - liga o início automático do desktop na bandeja, como administrador
      (tarefa \Synkro\Desktop do Agendador, ao fazer logon);
    - registra em "Programas e Recursos" e no menu Iniciar;
    - se já houver configuração, aplica (tarefas no Agendador).

    Os cofres (discos de backup) nunca são tocados.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\install.ps1
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\install.ps1 -WhatIf
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'Synkro'),
    [string]$DataDir = (Join-Path $env:ProgramData 'Synkro'),
    [switch]$NoAutostart,
    [switch]$NoApply,
    [int]$WaitForBackupMinutes = 30
)

$ErrorActionPreference = 'Stop'
$script:cmdlet = $PSCmdlet
$source = $PSScriptRoot

function Invoke-Step([string]$description, [scriptblock]$action) {
    if ($script:cmdlet.ShouldProcess($description)) {
        & $action
        Write-Host "  ok  $description"
    }
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-Sha256([string]$path) {
    # .NET direto: o Get-FileHash herda o -WhatIf e não calcula nada em simulação.
    $stream = [System.IO.File]::OpenRead($path)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        -join ($sha.ComputeHash($stream) | ForEach-Object { $_.ToString('x2') })
    } finally { $stream.Dispose() }
}

function Invoke-Icacls([string[]]$arguments) {
    & icacls.exe @arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "icacls $($arguments -join ' ') falhou ($LASTEXITCODE)" }
}

if (-not $WhatIfPreference -and -not (Test-Admin)) {
    throw 'Execute como administrador (clique com o botão direito no PowerShell > Executar como administrador).'
}
# Proteção contra um -InstallDir errado: o espelhamento apaga o que não é do pacote.
if ((Split-Path -Leaf $InstallDir) -ne 'Synkro') {
    throw "InstallDir precisa terminar em \Synkro (recebido: $InstallDir)"
}
if (-not (Test-Path (Join-Path $source 'synkro-worker.exe'))) {
    throw "synkro-worker.exe não encontrado em $source; execute o install.ps1 de dentro da pasta do pacote"
}

$version = (Get-Content (Join-Path $source 'VERSION.txt') -ErrorAction SilentlyContinue | Select-Object -First 1)
Write-Host "Instalando $version em $InstallDir"

# ---- 1. Integridade do pacote ---------------------------------------------------
$sumsFile = Join-Path $source 'SHA256SUMS.txt'
if (Test-Path $sumsFile) {
    $bad = @()
    foreach ($line in Get-Content $sumsFile) {
        if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { continue }
        $file = Join-Path $source $Matches[2]
        if (-not (Test-Path $file) -or (Get-Sha256 $file) -ne $Matches[1]) {
            $bad += $Matches[2]
        }
    }
    if ($bad.Count -gt 0) { throw "pacote corrompido ou alterado: $($bad -join ', ')" }
    Write-Host '  ok  SHA-256 de todos os arquivos do pacote conferido'
} else {
    Write-Warning 'SHA256SUMS.txt ausente: a integridade do pacote não foi conferida'
}
$signature = Get-AuthenticodeSignature (Join-Path $source 'synkro-worker.exe')
if ($signature.Status -ne 'Valid') {
    Write-Warning "executáveis sem assinatura digital válida ($($signature.Status))"
}

# ---- 2. Nunca interromper um backup ------------------------------------------------
$deadline = (Get-Date).AddMinutes($WaitForBackupMinutes)
while (Get-Process -Name 'synkro-worker' -ErrorAction SilentlyContinue) {
    if ($WhatIfPreference) { Write-Host '  (há um backup em andamento; a instalação esperaria ele terminar)'; break }
    if ((Get-Date) -gt $deadline) {
        throw "um backup está em andamento há mais de $WaitForBackupMinutes min; tente de novo depois"
    }
    Write-Host '  aguardando o backup em andamento terminar...'
    Start-Sleep -Seconds 15
}
Invoke-Step 'fechar o Synkro Desktop (só a interface; volta no próximo logon)' {
    Get-Process -Name 'synkro-desktop' -ErrorAction SilentlyContinue | Stop-Process -Force
}

# ---- 3. Programas --------------------------------------------------------------------
Invoke-Step "copiar os programas para $InstallDir" {
    New-Item -ItemType Directory -Force $InstallDir | Out-Null
    & robocopy.exe $source $InstallDir /MIR /R:2 /W:2 /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "falha ao copiar os arquivos (robocopy $LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
    # Program Files: somente leitura para Usuários (herdado), nada a ajustar.
}

# ---- 4. Dados e permissões ---------------------------------------------------------
# SYSTEM e Administradores: controle total. Usuários: leitura (o desktop lê a
# configuração) e gravação só em state\ (histórico/alertas, que são cache).
# staging\ e secrets.dat: só SYSTEM e Administradores.
Invoke-Step "preparar $DataDir e as permissões" {
    foreach ($d in '', 'config', 'config\jobs', 'state', 'staging', 'logs') {
        New-Item -ItemType Directory -Force (Join-Path $DataDir $d) | Out-Null
    }
    Invoke-Icacls @($DataDir, '/inheritance:r', '/grant:r', '*S-1-5-18:(OI)(CI)F', '*S-1-5-32-544:(OI)(CI)F',
                    '*S-1-5-32-545:(OI)(CI)RX')
    Invoke-Icacls @((Join-Path $DataDir 'state'), '/grant', '*S-1-5-32-545:(OI)(CI)M')
    Invoke-Icacls @((Join-Path $DataDir 'staging'), '/inheritance:r', '/grant:r', '*S-1-5-18:(OI)(CI)F',
                    '*S-1-5-32-544:(OI)(CI)F')
    $secrets = Join-Path $DataDir 'secrets.dat'
    if (Test-Path $secrets) {
        Invoke-Icacls @($secrets, '/inheritance:r', '/grant:r', '*S-1-5-18:F', '*S-1-5-32-544:F')
    }
    # Versões anteriores guardavam o estado na raiz.
    $legacy = Join-Path $DataDir 'synkro.db'
    $state = Join-Path $DataDir 'state\synkro.db'
    if ((Test-Path $legacy) -and -not (Test-Path $state)) {
        Move-Item $legacy $state
        foreach ($s in '-wal', '-shm') { if (Test-Path "$legacy$s") { Move-Item "$legacy$s" "$state$s" } }
    }
}

# ---- 5. Visualizador de Eventos ------------------------------------------------------
Invoke-Step 'registrar a origem "Synkro" no Visualizador de Eventos (log Aplicativo)' {
    if (-not [System.Diagnostics.EventLog]::SourceExists('Synkro')) {
        New-EventLog -LogName Application -Source 'Synkro'
    }
}

# ---- 6. Início automático e atalhos ------------------------------------------------
$desktopExe = Join-Path $InstallDir 'synkro-desktop.exe'
$workerExe = Join-Path $InstallDir 'synkro-worker.exe'
# O desktop exige administrador, e o Windows não inicia programas assim pela
# chave Run: o início automático é a tarefa \Synkro\Desktop (logon, elevada).
Invoke-Step 'remover o início automático antigo (chave Run)' {
    foreach ($key in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run', 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run') {
        Remove-ItemProperty -Path $key -Name 'Synkro' -ErrorAction SilentlyContinue
    }
}
if ($NoAutostart) {
    Invoke-Step 'desligar o início automático na bandeja' {
        & $workerExe autostart off | Out-Null
        $global:LASTEXITCODE = 0
    }
} else {
    Invoke-Step 'ligar o início automático na bandeja, como administrador (tarefa \Synkro\Desktop)' {
        & $workerExe autostart on | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "não foi possível criar a tarefa de início automático ($LASTEXITCODE)" }
    }
}
Invoke-Step 'criar o atalho no menu Iniciar' {
    $programs = [Environment]::GetFolderPath('CommonPrograms')
    $shell = New-Object -ComObject WScript.Shell
    $lnk = $shell.CreateShortcut((Join-Path $programs 'Synkro.lnk'))
    $lnk.TargetPath = $desktopExe
    $lnk.WorkingDirectory = $InstallDir
    $lnk.Description = 'Synkro — backup em cofres'
    $lnk.Save()
}
Invoke-Step 'acrescentar a pasta do Synkro ao PATH do sistema (synkro-worker no prompt)' {
    $path = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    if (($path -split ';') -notcontains $InstallDir) {
        [Environment]::SetEnvironmentVariable('Path', ($path.TrimEnd(';') + ';' + $InstallDir), 'Machine')
    }
}

# ---- 7. Programas e Recursos -------------------------------------------------------
Invoke-Step 'registrar em "Programas e Recursos"' {
    $key = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Synkro'
    New-Item -Path $key -Force | Out-Null
    $size = [int]((Get-ChildItem -Recurse -File $InstallDir | Measure-Object -Sum Length).Sum / 1KB)
    $ver = ($version -replace '^Synkro\s+', '')
    $values = @{
        DisplayName     = 'Synkro'
        DisplayVersion  = $ver
        Publisher       = 'Synkro'
        InstallLocation = $InstallDir
        DisplayIcon     = $desktopExe
        UninstallString = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $InstallDir 'uninstall.ps1')`""
    }
    foreach ($k in $values.Keys) { Set-ItemProperty -Path $key -Name $k -Value $values[$k] }
    Set-ItemProperty -Path $key -Name 'NoModify' -Value 1 -Type DWord
    Set-ItemProperty -Path $key -Name 'NoRepair' -Value 1 -Type DWord
    Set-ItemProperty -Path $key -Name 'EstimatedSize' -Value $size -Type DWord
}

# ---- 8. Configuração ---------------------------------------------------------------
$worker = Join-Path $InstallDir 'synkro-worker.exe'
$configured = Test-Path (Join-Path $DataDir 'config\synkro.yml')
if ($configured -and -not $NoApply) {
    Invoke-Step 'aplicar a configuração (tarefas no Agendador do Windows)' {
        & $worker config apply
        if ($LASTEXITCODE -ne 0) { Write-Warning "config apply terminou com código $LASTEXITCODE; confira com: synkro-worker config validate" }
        $global:LASTEXITCODE = 0
    }
}

Write-Host ''
if ($WhatIfPreference) { Write-Host 'Simulação concluída: nada foi alterado.'; return }
Write-Host 'Synkro instalado.'
if (-not $configured) {
    Write-Host 'Próximo passo: abra o Synkro (menu Iniciar), cadastre os discos de backup e crie as tarefas.'
    Write-Host 'Ou importe a configuração de outro PDV: synkro-worker config import --from pacote.yml --bind <cofre>=<disco>'
}
