; PS3EyeNativeSetup.iss
;
; Packages the PS3 Eye native camera into a single Setup.exe:
;   - PS3EyeCaptureService + libusb (owns the camera over WinUSB)
;   - PS3EyeVCamSource.dll (Media Foundation source, COM server)
;   - PS3EyeVCamRegistrar.exe (creates the virtual camera)
;   - PS3 Eye Universal driver MSI (WinUSB driver + DirectShow filter), if present
;
; All the actual work is done by Install-PS3EyeNative.ps1, which this package
; copies into the install folder and runs (and on uninstall, runs with
; -Uninstall). Build it with Inno Setup 6:
;
;   iscc /DPayloadDir="C:\path\to\bin\x64\Release" PS3EyeNativeSetup.iss
;
; without /DPayloadDir it looks in ..\..\bin\x64\Release relative to this file.

#define AppName "PS3 Eye Native Camera"
#define AppVersion "0.1.0"
#define AppPublisher "PS3 Eye DirectShow contributors"
#define ScriptName "Install-PS3EyeNative.ps1"

#ifndef PayloadDir
  #define PayloadDir AddBackslash(SourcePath) + "..\bin\x64\Release"
#endif

#ifndef DriverMsiDir
  #define DriverMsiDir AddBackslash(SourcePath) + "..\..\.."
#endif

[Setup]
AppId={{9C4A1F62-7B3E-4C58-8E21-4F0B7D2A6C93}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\PS3 Eye
DefaultGroupName=PS3 Eye
DisableProgramGroupPage=yes
OutputDir=Output
OutputBaseFilename=PS3EyeNativeSetup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22000
UninstallDisplayName={#AppName}
CloseApplications=yes
RestartApplications=no
AllowNoIcons=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; --- required binaries -------------------------------------------------------
Source: "{#PayloadDir}\PS3EyeCaptureService.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\PS3EyeVCamSource.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\PS3EyeVCamRegistrar.exe"; DestDir: "{app}"; Flags: ignoreversion

; The installer itself
Source: "{#ScriptName}"; DestDir: "{app}"; Flags: ignoreversion

; --- optional binaries -------------------------------------------------------
#if FileExists(AddBackslash(ExpandConstant('{#PayloadDir}')) + "libusb-1.0.dll")
Source: "{#PayloadDir}\libusb-1.0.dll"; DestDir: "{app}"; Flags: ignoreversion
#endif

#if FileExists(AddBackslash(ExpandConstant('{#PayloadDir}')) + "PS3EyeSourceFilter64.dll")
Source: "{#PayloadDir}\PS3EyeSourceFilter64.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
#endif

#if FileExists(AddBackslash(ExpandConstant('{#PayloadDir}')) + "PS3EyeSourceFilter.dll")
Source: "{#PayloadDir}\PS3EyeSourceFilter.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
#endif

; WinUSB driver + DirectShow filter package (PS3 Eye Universal Driver MSI).
; Optional: the native camera only needs the camera to be on WinUSB already.
Source: "{#DriverMsiDir}\PS3EyeInstallerBeta2.msi"; DestDir: "{tmp}"; Flags: deleteafterinstall skipifsourcedoesntexist

[Run]
; Everything (driver MSI, service, COM registration, virtual camera) happens here.
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\{#ScriptName}"" -InPlace -Quiet -InstallDir ""{app}"" -DriverMsi ""{tmp}\PS3EyeInstallerBeta2.msi"""; \
  StatusMsg: "Installing the PS3 Eye camera and capture service..."; \
  Flags: runhidden waituntilterminated

Filename: "{app}\PS3 Eye.txt"; Description: "Open the camera test page"; \
  Flags: shellexec postinstall skipifsilent unchecked

[UninstallRun]
; Reverse order: the camera entry has to go before the DLL is removed.
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\{#ScriptName}"" -Uninstall -InPlace -Quiet -InstallDir ""{app}"""; \
  RunOnceId: "RemovePS3EyeCamera"; \
  Flags: runhidden waituntilterminated

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
Type: files; Name: "{commonappdata}\PS3 Eye\install.log"

[Code]
function InitializeSetup(): Boolean;
var
  Build: Cardinal;
begin
  Result := True;
  Build := GetWindowsVersion and $FFFF;
  if Build < 22000 then
  begin
    Result :=
      MsgBox('The native camera API (MFCreateVirtualCamera) only exists on Windows 11 ' +
             'build 22000 or newer.' + #13#10#13#10 +
             'This machine reports build ' + IntToStr(Build) + ', so the camera will not ' +
             'appear in apps.' + #13#10#13#10 +
             'Install anyway?', mbConfirmation, MB_YESNO) = IDYES;
  end;
end;
