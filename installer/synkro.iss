; Instalador do Synkro (Inno Setup 6), gerado pelo package.ps1 -Installer.
;
; É um "embrulho" do pacote: extrai a pasta do pacote para um temporário e
; roda o install.ps1 já testado (integridade, espera do backup em andamento,
; Program Files, permissões, Visualizador de Eventos, Agendador, Programas e
; Recursos). Não tem desinstalador próprio: a desinstalação continua sendo a
; do install.ps1 (Programas e Recursos -> Synkro).
;
; Silencioso, usado pelo atualizador do app:
;   Synkro-Setup-<versão>.exe /SILENT /SUPPRESSMSGBOXES /NORESTART /SP-
; No fim reabre o Synkro Desktop como o usuário que pediu a atualização.

#ifndef AppVersion
  #error "Informe /DAppVersion=0.9.0-beta.1"
#endif
#ifndef NumericVersion
  #error "Informe /DNumericVersion=0.9.0"
#endif
#ifndef PackageDir
  #error "Informe /DPackageDir=<pasta do pacote>"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{8B5E2C47-3F1A-4D9E-9A61-5C2B7E0D4F13}
AppName=Synkro
AppVersion={#AppVersion}
AppVerName=Synkro {#AppVersion}
AppPublisher=Synkro
AppComments=Backup incremental, versionado e redundante para PDV
VersionInfoVersion={#NumericVersion}
VersionInfoProductVersion={#NumericVersion}
VersionInfoTextVersion={#AppVersion}
DefaultDirName={autopf}\Synkro
CreateAppDir=no
Uninstallable=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=Synkro-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
DisableReadyPage=no
CloseApplications=no
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "ptbr"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Messages]
ptbr.ReadyLabel1=O Synkro {#AppVersion} será instalado (ou atualizado) nesta máquina.
ptbr.ReadyLabel2b=Um backup em andamento não é interrompido: a instalação espera ele terminar. Os discos de backup nunca são alterados.

[Files]
; O install.ps1 vai por último: ao terminar de extraí-lo, o pacote está completo
; e ele é executado (AfterInstall). Uma falha cancela a instalação.
Source: "{#PackageDir}\*"; DestDir: "{tmp}\pacote"; Excludes: "install.ps1"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PackageDir}\install.ps1"; DestDir: "{tmp}\pacote"; Flags: ignoreversion; AfterInstall: RunInstallScript

[Run]
Filename: "{autopf}\Synkro\synkro-desktop.exe"; Description: "Abrir o Synkro"; Flags: nowait postinstall runasoriginaluser

[Code]
procedure RunInstallScript;
var
  ResultCode: Integer;
  LogDir, LogFile, Params: String;
begin
  WizardForm.StatusLabel.Caption := 'Configurando o Synkro (permissões, Agendador de Tarefas)...';
  LogDir := ExpandConstant('{commonappdata}\Synkro\logs');
  ForceDirectories(LogDir);
  LogFile := LogDir + '\install-{#AppVersion}.log';
  Params := '/c powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{tmp}\pacote\install.ps1')
            + '" > "' + LogFile + '" 2>&1';
  if not Exec(ExpandConstant('{cmd}'), Params, ExpandConstant('{tmp}\pacote'), SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    RaiseException('Não foi possível executar o PowerShell: ' + SysErrorMessage(ResultCode));
  if ResultCode <> 0 then
    RaiseException('A instalação do Synkro falhou (código ' + IntToStr(ResultCode) + '). Detalhes em ' + LogFile);
  Log('install.ps1 concluído; log em ' + LogFile);
end;
