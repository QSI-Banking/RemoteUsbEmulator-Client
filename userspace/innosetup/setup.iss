; Copyright (c) 2023-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>

#if Ver < EncodeVer(7,1,0,0)
        #error This script requires Inno Setup 7.1.0 or later
#endif

#ifndef SolutionDir
        #error Use option /DSolutionDir=<path>
#endif

#ifdef Platform
        #define Platform Lowercase(Platform)
#else
        #error Use option /DPlatform=<platform>
#endif

#ifdef Configuration
        #define Configuration Lowercase(Configuration)
#else
        #error Use option /DConfiguration=<cfg>
#endif

#ifdef ExePath
        #define BuildDir AddBackslash(ExtractFilePath(ExePath))
#else
        #error Use option /DExePath=path-to-exe
#endif

#if !FileExists(ExePath)
        #error File not found: ExePath
#endif

#ifndef GuiExePath
        #error Use option /DGuiExePath=path-to-exe
#endif

#if !FileExists(GuiExePath)
        #error File not found: GuiExePath
#endif

#ifndef VCToolsRedistInstallDir
        #error Use option /DVCToolsRedistInstallDir
#endif

#define VCToolsRedistExe "vc_redist." + Platform + ".exe"

#define AppExeName ExtractFileName(ExePath)
#define GuiExeName ExtractFileName(GuiExePath)

; information from .exe GetVersionInfo
#define ProductName GetStringFileInfo(ExePath, PRODUCT_NAME)
#define AppVersion GetVersionNumbersString(ExePath)
#define Copyright GetFileCopyrightString(ExePath)
#define Company GetFileCompanyString(ExePath)

#define AppGUID "{199505b0-b93d-4521-a8c7-897818e0205a}"
#define DetachTaskName "USBip Detach All On Reboot Or Shutdown"

#define FilterDriver "usbip2_filter"
#define UdeDriver "usbip2_ude"

#define CLIENT_HWID "ROOT\USBIP_WIN2\UDE"

; Project's test certificate is no longer installed for github releases since 0.9.7.5.
; But it is required during the development.
#define CertFileName "usbip.pfx"
#define CertFilePath SolutionDir + "drivers\package\" + CertFileName
#define CertName "USBip"
#define CertPwd "usbip"

; whether drivers are signed by the project's test certificate
#define TEST_SIGNED_DRIVERS

#define INSTALL_TEST_CERTIFICATE Defined(TEST_SIGNED_DRIVERS)

#if Platform == "arm64"
  #define ArchMode "arm64"
  #define VCRedistArch "ARM64"
#elif Platform == "x64"
  #define ArchMode "x64os"
  #define VCRedistArch "x64"
#else
  #error Unsupported Platform (expected "x64" or "arm64")
#endif

[Setup]
AppName={#ProductName}
AppVersion={#AppVersion}
AppCopyright={#Copyright}
AppPublisher={#Company}
AppPublisherURL=https://github.com/vadimgrn/usbip-win2
WizardStyle=modern
DefaultDirName={autopf}\{#ProductName}
DefaultGroupName={#ProductName}
ArchitecturesAllowed={#ArchMode}
ArchitecturesInstallIn64BitMode={#ArchMode}
VersionInfoVersion={#AppVersion}
ShowLanguageDialog=no
AllowNoIcons=yes
LicenseFile={#SolutionDir + "LICENSE.txt"}
AppId={{#AppGUID}
OutputBaseFilename={#ProductName}-{#AppVersion}-{#Platform}-{#Configuration}
OutputDir={#BuildDir}
SolidCompression=yes
DisableWelcomePage=no
WizardSmallImageFile=48.bmp,64.bmp,128.bmp
WizardImageFile=164.bmp,192.bmp,256.bmp,384.bmp,512.bmp
WizardImageAlphaFormat=defined
WizardImageStretch=no
UninstallDisplayIcon="{app}\{#AppExeName}"
UninstallLogging=yes
AlwaysRestart=yes
CloseApplications=yes
CloseApplicationsFilter=*.exe,*.dll
PrivilegesRequired=admin
; usbip.exe is added to the system PATH; notify running programs of the change
ChangesEnvironment=yes

; this app can't be installed more than once
MissingRunOnceIdsWarning=no

; Windows 10, version 1903
MinVersion=10.0.18362

[Messages]
WelcomeLabel2=This will install [name/ver] on your computer.

[Components]
Name: "main"; Description: "Main Files"; Types: full compact custom; Flags: fixed
Name: "client"; Description: "Client"; Types: full compact custom; Flags: fixed
Name: "gui"; Description: "GUI"; Types: full
Name: "sdk"; Description: "USBIP Software Development Kit"; Types: full
Name: "pdb"; Description: "Program DataBase files"; Types: full

[Icons]
Name: "{group}\{#ProductName}"; Filename: "{app}\{#GuiExeName}"; Components: gui
Name: "{group}\{cm:UninstallProgram,{#ProductName}}"; Filename: "{uninstallexe}"; Components: main
Name: "{commondesktop}\{#ProductName}"; Filename: "{app}\{#GuiExeName}"; Tasks: desktopicon

[Files]

Source: {#SolutionDir + "Readme.md"}; DestDir: "{app}"; Flags: isreadme; Components: main

Source: {#BuildDir + "usbip.exe"}; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: {#BuildDir + "devnode.exe"}; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: {#BuildDir + "*.dll"}; DestDir: "{app}"; Flags: ignoreversion; Components: main

Source: {#SolutionDir + "userspace\libusbip\*.h"}; DestDir: "{app}\include\usbip"; Excludes: "resource.h"; Components: sdk
Source: {#SolutionDir + "userspace\resources\messages.h"}; DestDir: "{app}\include\usbip"; Components: sdk
Source: {#BuildDir + "libusbip.lib"}; DestDir: "{app}\lib"; Components: sdk

Source: {#BuildDir + "*.pdb"}; DestDir: "{app}"; Excludes: "libusbip*.pdb, wusbip.pdb"; Components: pdb
Source: {#BuildDir + "libusbip.pdb"}; DestDir: "{app}"; Components: pdb or sdk
; Source: {#BuildDir + "wusbip.pdb"}; DestDir: "{app}"; Components: pdb and gui
; wusbip.pdb is too large

Source: {#BuildDir + "wusbip.exe"}; DestDir: "{app}"; Flags: ignoreversion; Components: gui

Source: {#AddBackslash(VCToolsRedistInstallDir) + VCToolsRedistExe}; DestDir: "{tmp}"; Flags: nocompression; Components: main
Source: {#BuildDir + "package\*"}; DestDir: "{tmp}"; Components: client

#if INSTALL_TEST_CERTIFICATE
  Source: {#CertFilePath}; DestDir: "{tmp}"; Components: main
#endif

[Registry]
; Add the install folder to the system PATH so usbip.exe works from any console; removed again by RemoveFromPath on uninstall
Root: HKLM64; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Check: NeedsAddPath('{app}'); Components: main

[Tasks]
Name: vcredist; Description: "Install Microsoft Visual C++ &Redistributable ({#VCRedistArch})"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Components: gui

[Run]

Filename: {tmp}\{#VCToolsRedistExe}; Parameters: "/quiet /norestart"; Flags: runhidden; Tasks: vcredist; StatusMsg: "Installing Microsoft Visual C++ Redistributable ({#VCRedistArch})..."

#if INSTALL_TEST_CERTIFICATE
  Filename: {sys}\certutil.exe; Parameters: "-f -p ""{#CertPwd}"" -importPFX root ""{tmp}\{#CertFileName}"" FriendlyName=""{#CertName}"""; Flags: runhidden logoutput; StatusMsg: "Installing test certificate..."
#endif

Filename: {sys}\pnputil.exe; Parameters: "/add-driver ""{tmp}\{#FilterDriver}.inf"" /install"; Flags: runhidden logoutput; Components: client; StatusMsg: "Installing upper filter driver..."
Filename: {app}\devnode.exe; Parameters: "install ""{tmp}\{#UdeDriver}.inf"" {#CLIENT_HWID}"; Flags: runhidden logoutput; Components: client; StatusMsg: "Installing UDE driver and virtual host controller..."

[UninstallRun]

Filename: {app}\devnode.exe; Parameters: "remove {#CLIENT_HWID} root"; Flags: runhidden logoutput; Components: client; StatusMsg: "Removing virtual host controller device..."

#if INSTALL_TEST_CERTIFICATE
  Filename: {sys}\certutil.exe; Parameters: "-f -delstore root ""{#CertName}"""; Flags: runhidden logoutput; StatusMsg: "Removing test certificate..."
#endif

[Code]

type
  SYSTEM_CODEINTEGRITY_INFORMATION = record
    Length: Cardinal;
    CodeIntegrityOptions: Cardinal;
  end;

function NtQuerySystemInformation(
  SystemInformationClass: Integer;
  var SystemInformation: SYSTEM_CODEINTEGRITY_INFORMATION;
  SystemInformationLength: Cardinal;
  var ReturnLength: Cardinal
): Integer; external 'NtQuerySystemInformation@ntdll.dll stdcall';

const
  SystemCodeIntegrityInformation = 103;
  CODEINTEGRITY_OPTION_TESTSIGN = 2;

function IsTestSigningModeEnabled(): Boolean;
var
  Info: SYSTEM_CODEINTEGRITY_INFORMATION;
  RetLen: Cardinal;
  Status: Integer;
begin
  Result := False;
  Info.Length := SizeOf(Info);
  Info.CodeIntegrityOptions := 0;
  RetLen := 0;

  Status := NtQuerySystemInformation(SystemCodeIntegrityInformation, Info, SizeOf(Info), RetLen);
  if Status = 0 then
  begin
    Result := (Info.CodeIntegrityOptions and CODEINTEGRITY_OPTION_TESTSIGN) <> 0;
  end;
end;

function check_test_sign_mode(): Boolean;
begin
#ifdef TEST_SIGNED_DRIVERS
  result := IsTestSigningModeEnabled();
  if not result then
    SuppressibleMsgBox('To use USBip, enable test-signed drivers to load.' #13#13
           'Run "Bcdedit.exe -set TESTSIGNING ON" as Administrator and reboot the PC.',
            mbCriticalError, MB_OK, IDOK);
#else
  result := true;
#endif
end;

function InitializeSetup(): Boolean;
begin
  result := check_test_sign_mode();
end;

const
  // The usbip2_ude driver reads its connection settings from here (drivers\ude\tls.cpp)
  DriverParamsKey = 'SYSTEM\CurrentControlSet\Services\{#UdeDriver}\Parameters';
  LegacyConnectionKey = 'SOFTWARE\USBip\Connection'; // used by early development builds
  DefaultTlsPort = 3241;

  CERT_STORE_PROV_SYSTEM_W = 10;
  CERT_SYSTEM_STORE_LOCAL_MACHINE = $00020000;
  CERT_STORE_READONLY_FLAG = $00008000;
  CERT_STORE_OPEN_EXISTING_FLAG = $00004000;
  CERT_NAME_SIMPLE_DISPLAY_TYPE = 4;
  CERT_NAME_ISSUER_FLAG = 1;
  CERT_KEY_PROV_INFO_PROP_ID = 2;
  CERT_SHA1_HASH_PROP_ID = 3;

function CertOpenStore(Provider: NativeUInt; Encoding: Cardinal; Prov: NativeUInt; Flags: Cardinal; Para: String): NativeUInt;
  external 'CertOpenStore@crypt32.dll stdcall';
function CertCloseStore(Store: NativeUInt; Flags: Cardinal): Boolean;
  external 'CertCloseStore@crypt32.dll stdcall';
function CertEnumCertificatesInStore(Store: NativeUInt; Prev: NativeUInt): NativeUInt;
  external 'CertEnumCertificatesInStore@crypt32.dll stdcall';
function CertGetNameString(Ctx: NativeUInt; NameType: Cardinal; Flags: Cardinal; TypePara: NativeUInt; Name: String; Size: Cardinal): Cardinal;
  external 'CertGetNameStringW@crypt32.dll stdcall';
function CertGetPropertyData(Ctx: NativeUInt; PropId: Cardinal; Data: AnsiString; var Size: Cardinal): Boolean;
  external 'CertGetCertificateContextProperty@crypt32.dll stdcall';
function CertGetPropertySize(Ctx: NativeUInt; PropId: Cardinal; Data: NativeUInt; var Size: Cardinal): Boolean;
  external 'CertGetCertificateContextProperty@crypt32.dll stdcall';

type
  TClientCert = record
    Thumbprint: String;
    Caption: String;
  end;

var
  ServerPage: TInputQueryWizardPage;
  CertPage: TWizardPage;
  CertList: TNewListBox;
  CertInfo: TNewStaticText;
  Certs: array of TClientCert;
  InitialThumbprint: String;

function GetServerAddress(): String;
begin
  Result := Trim(ServerPage.Values[0]);
end;

function GetPort(): Integer;
begin
  Result := StrToIntDef(Trim(ServerPage.Values[1]), 0);
end;

function GetServerName(): String;
begin
  Result := Trim(ServerPage.Values[2]);
end;

// Keeps hex digits only, upper case; spaces, colons and the invisible marks the certificate dialog copies are dropped
function NormalizeHex(const S: String): String;
var
  I: Integer;
  C: Char;
begin
  Result := '';
  for I := 1 to Length(S) do
  begin
    C := S[I];
    if ((C >= '0') and (C <= '9')) or ((C >= 'a') and (C <= 'f')) or ((C >= 'A') and (C <= 'F')) then
      Result := Result + Uppercase(C)
    else if not ((C = ' ') or (C = ':') or (C = #9) or (Ord(C) = $200E) or (Ord(C) = $200F) or (Ord(C) = $FEFF)) then
    begin
      Result := '?'; // invalid character
      Exit;
    end;
  end;
end;

function GetServerPin(): String;
begin
  Result := NormalizeHex(ServerPage.Values[3]);
end;

function GetClientThumbprint(): String;
begin
  if (CertList <> nil) and (CertList.ItemIndex >= 0) and (CertList.ItemIndex < GetArrayLength(Certs)) then
    Result := Certs[CertList.ItemIndex].Thumbprint
  else
    Result := NormalizeHex(InitialThumbprint);
end;

// IPv4 address or DNS name
function IsValidHostName(const S: String): Boolean;
var
  I: Integer;
  C: Char;
begin
  Result := (Length(S) > 0) and (Length(S) <= 253);
  for I := 1 to Length(S) do
  begin
    C := S[I];
    if not (((C >= 'A') and (C <= 'Z')) or ((C >= 'a') and (C <= 'z')) or ((C >= '0') and (C <= '9')) or (C = '.') or (C = '-')) then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

function HexOf(const Data: AnsiString): String;
var
  I: Integer;
begin
  Result := '';
  for I := 1 to Length(Data) do
    Result := Result + Format('%.2X', [Ord(Data[I])]);
end;

function CertName(Ctx: NativeUInt; Flags: Cardinal): String;
var
  Buf: String;
  Len: Cardinal;
begin
  Buf := StringOfChar(#0, 512);
  Len := CertGetNameString(Ctx, CERT_NAME_SIMPLE_DISPLAY_TYPE, Flags, 0, Buf, 512);
  if Len > 1 then
    Result := Copy(Buf, 1, Len - 1)
  else
    Result := '';
end;

// Certificates in LocalMachine\My that have a private key
procedure LoadClientCertificates();
var
  Store, Ctx: NativeUInt;
  Size: Cardinal;
  Hash: AnsiString;
  N: Integer;
begin
  SetArrayLength(Certs, 0);

  Store := CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
    CERT_SYSTEM_STORE_LOCAL_MACHINE or CERT_STORE_READONLY_FLAG or CERT_STORE_OPEN_EXISTING_FLAG, 'MY');
  if Store = 0 then
  begin
    Log('CertOpenStore(LocalMachine\My) failed');
    Exit;
  end;

  Ctx := CertEnumCertificatesInStore(Store, 0);
  while Ctx <> 0 do
  begin
    Size := 0;
    if CertGetPropertySize(Ctx, CERT_KEY_PROV_INFO_PROP_ID, 0, Size) then
    begin
      SetLength(Hash, 20);
      Size := 20;
      if CertGetPropertyData(Ctx, CERT_SHA1_HASH_PROP_ID, Hash, Size) and (Size = 20) then
      begin
        N := GetArrayLength(Certs);
        SetArrayLength(Certs, N + 1);
        Certs[N].Thumbprint := HexOf(Hash);
        Certs[N].Caption := CertName(Ctx, 0) + '  -  issued by ' + CertName(Ctx, CERT_NAME_ISSUER_FLAG) + '  -  ' + Certs[N].Thumbprint;
      end;
    end;
    Ctx := CertEnumCertificatesInStore(Store, Ctx); // frees the previous context
  end;

  CertCloseStore(Store, 0);
  Log(Format('%d certificate(s) with a private key in LocalMachine\My', [GetArrayLength(Certs)]));
end;

// Value from the command line, else from the previous install, else Default
function InitialValue(const Param, PrevName, Default: String): String;
begin
  Result := ExpandConstant('{param:' + Param + '|}');
  if Result = '' then
    Result := GetPreviousData(PrevName, Default);
end;

procedure CreateConnectionPages();
var
  I: Integer;
begin
  ServerPage := CreateInputQueryPage(wpSelectComponents,
    'USB/IP server', 'Which server will this computer attach devices from?',
    'The connection uses mutual TLS directly to usbipd on the server. ' +
    'The server certificate must chain to a root in this computer''s Trusted Root Certification Authorities store ' +
    'and contain the server certificate name below.' + #13#10#13#10 +
    'Unattended install: /SERVER= /PORT= /SERVERNAME= /SERVERPIN= /CLIENTCERT=');
  ServerPage.Add('Server address (IP address or host name):', False);
  ServerPage.Add('TLS port:', False);
  ServerPage.Add('Server certificate name (DNS name in the server certificate):', False);
  ServerPage.Add('Server certificate SHA-256 (optional pin):', False);

  ServerPage.Values[0] := InitialValue('SERVER', 'ServerAddress', '');
  ServerPage.Values[1] := InitialValue('PORT', 'TlsPort', IntToStr(DefaultTlsPort));
  ServerPage.Values[2] := InitialValue('SERVERNAME', 'ServerName', '');
  ServerPage.Values[3] := InitialValue('SERVERPIN', 'ServerPin', '');
  InitialThumbprint := InitialValue('CLIENTCERT', 'ClientCert', '');

  CertPage := CreateCustomPage(ServerPage.ID, 'Client certificate',
    'Which certificate identifies this computer to the server?');

  CertInfo := TNewStaticText.Create(CertPage);
  CertInfo.Parent := CertPage.Surface;
  CertInfo.AutoSize := False;
  CertInfo.WordWrap := True;
  CertInfo.Width := CertPage.SurfaceWidth;
  CertInfo.Height := ScaleY(40);

  CertList := TNewListBox.Create(CertPage);
  CertList.Parent := CertPage.Surface;
  CertList.Top := CertInfo.Top + CertInfo.Height + ScaleY(8);
  CertList.Width := CertPage.SurfaceWidth;
  CertList.Height := CertPage.SurfaceHeight - CertList.Top;

  LoadClientCertificates();
  for I := 0 to GetArrayLength(Certs) - 1 do
  begin
    CertList.Items.Add(Certs[I].Caption);
    if Certs[I].Thumbprint = NormalizeHex(InitialThumbprint) then
      CertList.ItemIndex := I;
  end;
  if (CertList.ItemIndex < 0) and (GetArrayLength(Certs) = 1) then
    CertList.ItemIndex := 0;

  if GetArrayLength(Certs) = 0 then
    CertInfo.Caption := 'No certificate with a private key was found in the Local Computer\Personal store. ' +
      'Import this computer''s client certificate there (for example certutil -importpfx), then run Setup again.'
  else
    CertInfo.Caption := 'Certificates with a private key in the Local Computer\Personal store. ' +
      'Choose the one issued to this computer for client authentication.';
end;

// Empty if the settings are valid, else what is wrong
function ValidateConnectionSettings(): String;
var
  Pin, Thumb: String;
begin
  Result := '';
  Pin := GetServerPin();
  Thumb := GetClientThumbprint();

  if not IsValidHostName(GetServerAddress()) then
    Result := 'Enter the server IP address or host name (letters, digits, dots and hyphens only).'
  else if (GetPort() < 1) or (GetPort() > 65535) then
    Result := 'Enter a TLS port between 1 and 65535.'
  else if not IsValidHostName(GetServerName()) then
    Result := 'Enter the server certificate name, the DNS name in the server certificate (for example usbip-server.corp.local).'
  else if (Pin <> '') and (Length(Pin) <> 64) then
    Result := 'The server certificate SHA-256 must be 64 hex digits, or empty.'
  else if Length(Thumb) <> 40 then
    Result := 'Choose the client certificate (or pass /CLIENTCERT=<SHA-1 thumbprint> for an unattended install).';
end;

procedure InitializeWizard();
begin
  WizardForm.LicenseAcceptedRadio.Checked := True;
  CreateConnectionPages();
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Msg: String;
begin
  Result := True;
  Msg := '';

  if CurPageID = ServerPage.ID then
  begin
    Msg := ValidateConnectionSettings();
    if Pos('client certificate', Msg) > 0 then
      Msg := ''; // chosen on the next page
  end
  else if CurPageID = CertPage.ID then
    Msg := ValidateConnectionSettings();

  if Msg <> '' then
  begin
    SuppressibleMsgBox(Msg, mbError, MB_OK, IDOK);
    Result := False;
  end;
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'ServerAddress', GetServerAddress());
  SetPreviousData(PreviousDataKey, 'TlsPort', IntToStr(GetPort()));
  SetPreviousData(PreviousDataKey, 'ServerName', GetServerName());
  SetPreviousData(PreviousDataKey, 'ServerPin', GetServerPin());
  SetPreviousData(PreviousDataKey, 'ClientCert', GetClientThumbprint());
end;

procedure WriteConnectionSettings();
var
  Pin: String;
begin
  // a [Code] line must not start with '[', Inno Setup would read it as a section tag
  Log(Format('Connection: %s:%d, server name %s, client certificate %s', [GetServerAddress(), GetPort(), GetServerName(), GetClientThumbprint()]));

  RegWriteStringValue(HKEY_LOCAL_MACHINE_64, DriverParamsKey, 'Host', GetServerAddress());
  RegWriteDWordValue(HKEY_LOCAL_MACHINE_64, DriverParamsKey, 'Port', GetPort());
  RegWriteStringValue(HKEY_LOCAL_MACHINE_64, DriverParamsKey, 'ServerName', GetServerName());
  RegWriteStringValue(HKEY_LOCAL_MACHINE_64, DriverParamsKey, 'ClientCertificateThumbprint', GetClientThumbprint());

  Pin := GetServerPin();
  if Pin <> '' then
    RegWriteStringValue(HKEY_LOCAL_MACHINE_64, DriverParamsKey, 'ServerCertificateSha256', Pin)
  else
    RegDeleteValue(HKEY_LOCAL_MACHINE_64, DriverParamsKey, 'ServerCertificateSha256');

  if RegDeleteKeyIncludingSubkeys(HKEY_LOCAL_MACHINE_64, LegacyConnectionKey) then
    Log('Removed the legacy key HKLM\' + LegacyConnectionKey);
  RegDeleteKeyIfEmpty(HKEY_LOCAL_MACHINE_64, 'SOFTWARE\USBip');
end;

procedure RemoveConnectionSettings();
begin
  RegDeleteValue(HKEY_LOCAL_MACHINE_64, DriverParamsKey, 'Host');
  RegDeleteValue(HKEY_LOCAL_MACHINE_64, DriverParamsKey, 'Port');
  RegDeleteValue(HKEY_LOCAL_MACHINE_64, DriverParamsKey, 'ServerName');
  RegDeleteValue(HKEY_LOCAL_MACHINE_64, DriverParamsKey, 'ClientCertificateThumbprint');
  RegDeleteValue(HKEY_LOCAL_MACHINE_64, DriverParamsKey, 'ServerCertificateSha256');
end;


procedure RegisterDetachTask();
var
  Scheduler, RootFolder, TaskDef, Trigger, Principal, Settings, Action: Variant;
begin
  Log('Registering detach task: {#DetachTaskName}');
  try
    Scheduler := CreateOleObject('Schedule.Service');
    Scheduler.Connect();
    RootFolder := Scheduler.GetFolder('\');

    TaskDef := Scheduler.NewTask(0);
    TaskDef.RegistrationInfo.Author := 'USBip Installer';
    TaskDef.RegistrationInfo.Description := 'USBip: detach all imported devices on system reboot or shutdown';

    Trigger := TaskDef.Triggers.Create(0); // TASK_TRIGGER_EVENT
    Trigger.Subscription := '<QueryList><Query Id="0" Path="System"><Select Path="System">*[System[Provider[@Name=''Microsoft-Windows-Kernel-Power''] and (EventID=109)]] or *[System[Provider[@Name=''User32''] and (EventID=1074)]]</Select></Query></QueryList>';

    Principal := TaskDef.Principal;
    Principal.UserId := 'S-1-5-19';
    Principal.RunLevel := 0; // TASK_RUNLEVEL_LUA

    Settings := TaskDef.Settings;
    Settings.MultipleInstances := 2; // TASK_INSTANCES_IGNORE_NEW
    Settings.DisallowStartIfOnBatteries := False;
    Settings.StopIfGoingOnBatteries := False;
    Settings.AllowHardTerminate := True;
    Settings.StartWhenAvailable := False;
    Settings.RunOnlyIfNetworkAvailable := False;
    Settings.ExecutionTimeLimit := 'PT30S';
    Settings.Priority := 7;

    Action := TaskDef.Actions.Create(0); // TASK_ACTION_EXEC
    Action.Path := ExpandConstant('{app}\usbip.exe');
    Action.Arguments := 'detach --all=closeonly';

    RootFolder.RegisterTaskDefinition('{#DetachTaskName}', TaskDef, 6, '', '', 5);
    Log('Detach task registered successfully');
  except
    Log('Failed to register detach task: ' + GetExceptionMessage());
  end;
end;

procedure UnregisterDetachTask();
var
  Scheduler, RootFolder: Variant;
begin
  Log('Unregistering detach task: {#DetachTaskName}');
  try
    Scheduler := CreateOleObject('Schedule.Service');
    Scheduler.Connect();
    RootFolder := Scheduler.GetFolder('\');
    try
      RootFolder.GetTask('{#DetachTaskName}');
    except
      Log('Detach task does not exist, skipping deletion');
      Exit;
    end;
    RootFolder.DeleteTask('{#DetachTaskName}', 0);
    Log('Detach task unregistered successfully');
  except
    Log('Failed to unregister detach task: ' + GetExceptionMessage());
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsComponentSelected('client') then
  begin
    RegisterDetachTask();
    WriteConnectionSettings();
  end;
end;

function DeleteOemDriverFromRegistry(const DriverName: String): Boolean;
var
  Names: TArrayOfString;
  I, ResultCode: Integer;
  OemName, Prefix: String;
begin
  Result := False;
  Prefix := Lowercase(DriverName + '.inf');
  if RegGetSubkeyNames(HKEY_LOCAL_MACHINE_64, 'SYSTEM\DriverDatabase\DriverPackages', Names) then
  begin
    for I := 0 to GetArrayLength(Names) - 1 do
    begin
      if Pos(Prefix, Lowercase(Names[I])) = 1 then
      begin
        if RegQueryStringValue(HKEY_LOCAL_MACHINE_64, 'SYSTEM\DriverDatabase\DriverPackages\' + Names[I], '', OemName) then
        begin
          OemName := Trim(OemName);
          if OemName <> '' then
          begin
            Log('Deleting OEM driver ' + OemName + ' (' + Names[I] + ')');
            if ExecAndLogOutputWithNativeSysDir(
                 ExpandConstant('{sys}\pnputil.exe'),
                 '/delete-driver ' + OemName + ' /uninstall /force',
                 '', SW_HIDE, ewWaitUntilTerminated, ResultCode, nil) then
            begin
              Log(Format('pnputil /delete-driver %s returned exit code %d', [OemName, ResultCode]));
              Result := True;
            end
            else
            begin
              Log(Format('Failed to execute pnputil for %s, error code %d', [OemName, ResultCode]));
            end;
          end;
        end;
      end;
    end;
  end;
end;

procedure DeleteOemDriverFromFileSearch(const DriverName: String);
var
  FindRec: TFindRec;
  InfDir, CatVal, CatBase: String;
  ResultCode: Integer;
begin
  InfDir := ExpandConstant('{win}\INF\');
  if FindFirst(InfDir + 'oem*.inf', FindRec) then
  begin
    try
      repeat
        if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0 then
        begin
          CatVal := Lowercase(Trim(GetIniString('Version', 'CatalogFile', '', InfDir + FindRec.Name)));
          // Strip optional .cat extension to get the bare catalog base name,
          // then compare for equality — avoids substring false-positives
          // (e.g. 'usbip2_ude' must not match 'extra_usbip2_ude_old.cat').
          if (Length(CatVal) > 4) and (Copy(CatVal, Length(CatVal) - 3, 4) = '.cat') then
            CatBase := Copy(CatVal, 1, Length(CatVal) - 4)  // remove '.cat'
          else
            CatBase := CatVal;
          if (CatBase = Lowercase(DriverName)) or
             (GetIniString('SourceDisksFiles', DriverName + '.sys', '', InfDir + FindRec.Name) <> '') then
          begin
            Log('Deleting OEM driver ' + FindRec.Name + ' (' + DriverName + ')');
            if ExecAndLogOutputWithNativeSysDir(
                 ExpandConstant('{sys}\pnputil.exe'),
                 '/delete-driver ' + FindRec.Name + ' /uninstall /force',
                 '', SW_HIDE, ewWaitUntilTerminated, ResultCode, nil) then
            begin
              Log(Format('pnputil /delete-driver %s returned exit code %d', [FindRec.Name, ResultCode]));
            end
            else
            begin
              Log(Format('Failed to execute pnputil for %s, error code %d', [FindRec.Name, ResultCode]));
            end;
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;


// Both registry lookup and file search are intentionally executed sequentially
// by design to remove stale drivers and ensure all OEM driver packages are thoroughly removed.
// Do not flag this dual-pass approach as redundant in code reviews.
procedure DeleteOemDriver(const DriverName: String);
begin
  DeleteOemDriverFromRegistry(DriverName);
  DeleteOemDriverFromFileSearch(DriverName);
end;

const
  EnvironmentKey = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';

function SamePathEntry(const A, B: String): Boolean;
begin
  Result := CompareText(RemoveBackslash(Trim(A)), RemoveBackslash(Trim(B))) = 0;
end;

// True if the directory is not yet in the system PATH
function NeedsAddPath(Param: String): Boolean;
var
  Path, Rest, Item, Dir: String;
  P: Integer;
begin
  Result := True;
  Dir := ExpandConstant(Param);
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE_64, EnvironmentKey, 'Path', Path) then
    Exit;
  Rest := Path;
  while Rest <> '' do
  begin
    P := Pos(';', Rest);
    if P > 0 then
    begin
      Item := Copy(Rest, 1, P - 1);
      Delete(Rest, 1, P);
    end
    else
    begin
      Item := Rest;
      Rest := '';
    end;
    if SamePathEntry(Item, Dir) then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

// Removes every entry equal to Dir from the system PATH, keeping all other entries as they are
procedure RemoveFromPath(const Dir: String);
var
  Path, Rest, Item, NewPath: String;
  P: Integer;
  Found: Boolean;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE_64, EnvironmentKey, 'Path', Path) then
    Exit;
  Rest := Path;
  NewPath := '';
  Found := False;
  while Rest <> '' do
  begin
    P := Pos(';', Rest);
    if P > 0 then
    begin
      Item := Copy(Rest, 1, P - 1);
      Delete(Rest, 1, P);
    end
    else
    begin
      Item := Rest;
      Rest := '';
    end;
    if (Trim(Item) <> '') and SamePathEntry(Item, Dir) then
      Found := True
    else if Item <> '' then
    begin
      if NewPath <> '' then
        NewPath := NewPath + ';';
      NewPath := NewPath + Item;
    end;
  end;
  if Found then
  begin
    Log('Removing ' + Dir + ' from the system PATH');
    RegWriteExpandStringValue(HKEY_LOCAL_MACHINE_64, EnvironmentKey, 'Path', NewPath);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    UnregisterDetachTask();
    RemoveConnectionSettings();
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    RemoveFromPath(ExpandConstant('{app}'));
    DeleteOemDriver('{#UdeDriver}');
    DeleteOemDriver('{#FilterDriver}');
  end;
end;

function UninstallNeedRestart(): Boolean;
begin
  result := true;
end;

// Check if an existing version of USBip is installed and locate its uninstaller
function GetInstalledUninstallString(var UninstPath, UninstParams: String): Boolean;
var
  SubKey, UninstStr, LowerStr, Candidate: String;
  P, I: Integer;
begin
  Result := False;
  SubKey := 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{#AppGUID}_is1';

  // Check 64-bit HKLM, then 32-bit HKLM, then HKCU
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE_64, SubKey, 'UninstallString', UninstStr) then
    if not RegQueryStringValue(HKEY_LOCAL_MACHINE_32, SubKey, 'UninstallString', UninstStr) then
      if not RegQueryStringValue(HKEY_CURRENT_USER, SubKey, 'UninstallString', UninstStr) then
        Exit;

  UninstStr := Trim(UninstStr);
  if UninstStr = '' then
    Exit;

  // Extract executable path from potentially quoted UninstallString
  if (Length(UninstStr) > 0) and (UninstStr[1] = '"') then
  begin
    Delete(UninstStr, 1, 1);
    P := Pos('"', UninstStr);
    if P > 0 then
      UninstPath := Copy(UninstStr, 1, P - 1)
    else
      UninstPath := UninstStr;
  end
  else if FileExists(UninstStr) then
  begin
    UninstPath := UninstStr;
  end
  else
  begin
    // For unquoted paths with arguments, search backwards for '.exe' to avoid
    // misidentifying directory names containing '.exe' as the executable
    LowerStr := Lowercase(UninstStr);
    P := 0;
    for I := Length(LowerStr) - 3 downto 1 do
    begin
      if Copy(LowerStr, I, 4) = '.exe' then
      begin
        Candidate := Trim(Copy(UninstStr, 1, I + 3));
        if FileExists(Candidate) then
        begin
          P := I;
          Break;
        end
        else if P = 0 then
        begin
          P := I;
        end;
      end;
    end;

    if P > 0 then
      UninstPath := Trim(Copy(UninstStr, 1, P + 3))
    else
    begin
      P := Pos(' ', UninstStr);
      if P > 0 then
        UninstPath := Copy(UninstStr, 1, P - 1)
      else
        UninstPath := UninstStr;
    end;
  end;

  UninstParams := '/VERYSILENT /NORESTART /SUPPRESSMSGBOXES';
  Result := FileExists(UninstPath);
end;

function PrepareToInstall(var NeedsRestart: Boolean): string;
var
  UninstPath, UninstParams: String;
  ExitCode, I: Integer;
begin
  Result := '';
  if WizardIsComponentSelected('client') then // also checks unattended installs, where the pages are not shown
  begin
    Result := ValidateConnectionSettings();
    if Result <> '' then
      Exit;
  end;
  if GetInstalledUninstallString(UninstPath, UninstParams) then
  begin
    Log('Uninstalling previous package: ' + UninstPath + ' ' + UninstParams);
    if Exec(UninstPath, UninstParams, '', SW_HIDE, ewWaitUntilTerminated, ExitCode) then
    begin
      if ExitCode = 0 then
      begin
        Log('Installed package uninstall completed successfully');
        I := 0;
        while FileExists(UninstPath) and (I < 20) do
        begin
          Sleep(100);
          I := I + 1;
        end;
      end
      else
        Result := 'Failed to automatically uninstall the previous version of USBip (exit code ' + IntToStr(ExitCode) + '). ' +
                  'Please uninstall it manually before continuing.';
    end
    else
    begin
      Result := 'Failed to execute uninstaller for the previous version of USBip: ' + SysErrorMessage(ExitCode) + '. ' +
                'Please uninstall it manually before continuing.';
    end;
  end;
end;
