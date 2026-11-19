<#
.SYNOPSIS
    Remove o Synkro desta máquina. Execute como administrador.

.DESCRIPTION
    Remove os programas, as tarefas \Synkro\ do Agendador, o início
    automático, os atalhos e o registro em "Programas e Recursos".

    Por padrão MANTÉM C:\ProgramData\Synkro (configuração, segredos e
    histórico), para uma reinstalação continuar de onde parou. Use
    -RemoveData para apagar também esses dados.

    Os cofres (discos de backup) e os backups dentro deles NUNCA são tocados.
#>
[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'Medium')]
param(
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'Synkro'),
    [string]$DataDir = (Join-Path $env:ProgramData 'Synkro'),
    [switch]$RemoveData,
    [int]$WaitForBackupMinutes = 30
)

$ErrorActionPreference = 'Stop'
$script:cmdlet = $PSCmdlet

function Invoke-Step([string]$description, [scriptblock]$action) {
    if ($script:cmdlet.ShouldProcess($description)) {
        & $action
        Write-Host "  ok  $description"
    }
}

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $WhatIfPreference -and -not ([Security.Principal.WindowsPrincipal]$id).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Execute como administrador.'
}
if ((Split-Path -Leaf $InstallDir) -ne 'Synkro' -or (Split-Path -Leaf $DataDir) -ne 'Synkro') {
    throw 'InstallDir e DataDir precisam terminar em \Synkro'
}
Set-Location $env:SystemRoot # o script pode estar dentro da pasta que será apagada

$deadline = (Get-Date).AddMinutes($WaitForBackupMinutes)
while (Get-Process -Name 'synkro-worker' -ErrorAction SilentlyContinue) {
    if ($WhatIfPreference) { break }
    if ((Get-Date) -gt $deadline) { throw 'um backup está em andamento; tente de novo depois' }
    Write-Host '  aguardando o backup em andamento terminar...'
    Start-Sleep -Seconds 15
}

Invoke-Step 'remover as tarefas \Synkro\ do Agendador' {
    Get-ScheduledTask -TaskPath '\Synkro\' -ErrorAction SilentlyContinue | Unregister-ScheduledTask -Confirm:$false
}
Invoke-Step 'fechar o Synkro Desktop' {
    Get-Process -Name 'synkro-desktop' -ErrorAction SilentlyContinue | Stop-Process -Force
}
Invoke-Step 'remover o início automático e os atalhos' {
    Remove-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run' -Name 'Synkro' -ErrorAction SilentlyContinue
    Remove-ItemProperty -Path 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run' -Name 'Synkro' -ErrorAction SilentlyContinue
    Remove-Item (Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'Synkro.lnk') -ErrorAction SilentlyContinue
}
Invoke-Step 'tirar a pasta do Synkro do PATH do sistema' {
    $path = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $kept = ($path -split ';' | Where-Object { $_ -and $_ -ne $InstallDir }) -join ';'
    [Environment]::SetEnvironmentVariable('Path', $kept, 'Machine')
}
Invoke-Step 'remover o registro em "Programas e Recursos"' {
    Remove-Item 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Synkro' -Recurse -ErrorAction SilentlyContinue
}
Invoke-Step "apagar $InstallDir" {
    if (Test-Path $InstallDir) { Remove-Item -Recurse -Force $InstallDir }
}

if ($RemoveData) {
    if ($script:cmdlet.ShouldContinue(
            "Apagar $DataDir (configuração, segredos e histórico)? Os backups nos cofres NÃO são apagados.",
            'Remover dados do Synkro')) {
        Invoke-Step "apagar $DataDir" { Remove-Item -Recurse -Force $DataDir }
        Invoke-Step 'remover a origem "Synkro" do Visualizador de Eventos' {
            if ([System.Diagnostics.EventLog]::SourceExists('Synkro')) { Remove-EventLog -Source 'Synkro' }
        }
    }
} else {
    Write-Host "Mantido: $DataDir (configuração, segredos e histórico). Use -RemoveData para apagar."
}
if ($WhatIfPreference) { Write-Host 'Simulação concluída: nada foi alterado.'; return }
Write-Host 'Synkro removido. Os backups nos cofres continuam intactos.'
