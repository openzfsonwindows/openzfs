; Script generated by the Inno Setup Script Wizard.
; SEE THE DOCUMENTATION FOR DETAILS ON CREATING INNO SETUP SCRIPT FILES!
#define Root SourcePath+"..\..\..\"

#define MyAppName "OpenZFS On Windows"
#define MyAppPublisher "OpenZFS"
#define MyAppURL "http://www.openzfsonwindows.org/"

; Attempt to read in the gitrev version from the header file.
#define VerFile FileOpen(Root + "include\zfs_gitrev.h")
#define VerStr FileRead(VerFile)       ; "// This file is only used on Windows and CMake, other platforms"
#define VerStr FileRead(VerFile)       ; "// see scripts/make-gitrev.sh"
#define VerStr FileRead(VerFile)       ; "#define	ZFS_META_GITREV "zfs-2.0.0-7-g689dbcfbf-dirty""
; #pragma message VerStr + " readline"
#expr FileClose(VerFile)
#undef VerFile
; Parse out 2.0.0-7-g689dbcfbf-dirty and delete the double-quote
#define MyAppVersion StringChange(Copy(VerStr, 33), '"','')

#pragma message MyAppVersion + " is version"

#if MyAppVersion == ""
#error Failed to read version from include/zfs_gitrev.h
#endif

#include "environment.iss"

[Setup]
; NOTE: The value of AppId uniquely identifies this application.
; Do not use the same AppId value in installers for other applications.
; (To generate a new GUID, click Tools | Generate GUID inside the IDE.)
AppId={{61A38ED1-4D3F-4869-A8D5-87A58A301D56}
AppName={#MyAppName}-debug
AppVersion={#MyAppVersion}
AppVerName={#MyAppName}-{#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf64}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
LicenseFile={#SourcePath}\OPENSOLARIS.LICENSE.txt
InfoBeforeFile={#SourcePath}\readme.txt
OutputBaseFilename=OpenZFSOnWindows-debug-{#MyAppVersion}
SetupIconFile={#SourcePath}\openzfs.ico
Compression=lzma
SolidCompression=yes
PrivilegesRequired=admin
OutputDir={#SourcePath}\..
ChangesEnvironment=true
WizardSmallImageFile="{#SourcePath}\openzfs-small.bmp"
WizardImageFile="{#SourcePath}\openzfs-large.bmp"
; Tools/Configure Sign Tools -> Add ->
; "signtoola" = C:\Program Files (x86)\Windows Kits\10\bin\x64\signtool.exe sign /sha1 09A7835D2496C08389CF1CC1031EF73BAE897A08 /n $qWest Coast Enterprises Limited$q /t http://ts.digicert.com /as /fd sha256 /td sha256 /d $qOpenZFS on Windows$q $f
; "signtoolb" = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.17763.0\x64\signtool.exe sign /sha1 ab8e4f6b94cecfa4638847122b511e507e147c50 /as /n $qJoergen Lundman$q /tr http://timestamp.digicert.com /td sha256 /fd sha256 /d $qOpenZFS on Windows$q $f"
; "signtoolc" = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\signtool.exe sign /v /fd sha256 /n $qOpenZFS Test Signing Certificate$q /t http://timestamp.digicert.com $f"
; SignTool=signtoola
; SignTool=signtoolb
SignTool=signtoolc
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Code]
procedure CurStepChanged(CurStep: TSetupStep);
begin
    if (CurStep = ssPostInstall) and WizardIsTaskSelected('envPath')
    then EnvAddPath(ExpandConstant('{app}'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
    if CurUninstallStep = usPostUninstall
    then EnvRemovePath(ExpandConstant('{app}'));
end;

[Tasks]
Name: envPath; Description: "Add OpenZFS to PATH variable"

[Files]
Source: "{#Root}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\CODE_OF_CONDUCT.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\os\windows/kstat/kstat.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\os\windows/zfsinstaller/zfsinstaller.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\zpool\zpool.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\zfs\zfs.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\zdb\zdb.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\zed\zed.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\zstream\zstreamdump.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\raidz_test\raidz_test.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\module\os\windows\driver\OpenZFS.sys"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\module\os\windows\driver\OpenZFS.cat"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\module\os\windows\driver\OpenZFS.inf"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\module\os\windows\driver\OpenZFS.man"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\zed\cmd\zed\os\windows\zed.cat"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\zed\cmd\zed\os\windows\zed.inf"; DestDir: "{app}"; Flags: ignoreversion
;Source: "{#Root}\x64\Debug\ZFSin.cer"; DestDir: "{app}"; Flags: ignoreversion
;Source: "{#Root}\zfs\cmd\arcstat\arcstat.pl"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\module\os\windows\driver\*.pdb"; DestDir: "{app}\symbols"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\zstream\*.pdb"; DestDir: "{app}\symbols"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\zpool\*.pdb"; DestDir: "{app}\symbols"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\zfs\*.pdb"; DestDir: "{app}\symbols"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\zdb\*.pdb"; DestDir: "{app}\symbols"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\zed\*.pdb"; DestDir: "{app}\symbols"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\raidz_test\*.pdb"; DestDir: "{app}\symbols"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\os\windows\zfsinstaller\*.pdb"; DestDir: "{app}\symbols"; Flags: ignoreversion
Source: "{#Root}\out\build\x64-Debug\cmd\os\windows\kstat\*.pdb"; DestDir: "{app}\symbols"; Flags: ignoreversion
Source: "{#SourcePath}\HowToDebug.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\contrib\windows\parsedump\*.*"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\scripts\zfs_prepare_disk"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\cmd\zpool\compatibility.d\*"; DestDir: "{app}\compatibility.d"; Flags: ignoreversion
Source: "{#Root}\cmd\zpool\zpool.d\*"; DestDir: "{app}\zpool.d"; Flags: ignoreversion
Source: "{#Root}\cmd\zed\os\windows\zed.d\*"; DestDir: "{app}\zed.d"; Flags: ignoreversion

; NOTE: Don't use "Flags: ignoreversion" on any shared system files

[Icons]
Name: "{group}\{cm:ProgramOnTheWeb,{#MyAppName}}"; Filename: "{#MyAppURL}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"

[Run]
; Filename: "{sys}\sc.exe"; Parameters: "stop OpenZFS_zed"; Flags: runascurrentuser; StatusMsg: "Stopping ZED Service..."
; Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM zed.exe"; Flags: runhidden; StatusMsg: "Ensuring zed is stopped...";
Filename: "{app}\ZFSInstaller.exe"; Parameters: "install .\OpenZFS.inf"; StatusMsg: "Installing Driver..."; Flags: runascurrentuser;
Filename: "{sys}\rundll32.exe"; Parameters: "setupapi.dll,InstallHinfSection DefaultInstall 132 {app}\zed.inf"; Flags: runhidden; StatusMsg: "Installing zed driver..."
Filename: "{sys}\sc.exe"; Parameters: "start OpenZFS_zed"; Flags: runhidden; StatusMsg: "Starting zed Service..."

[UninstallRun]
Filename: "{sys}\sc.exe"; Parameters: "stop OpenZFS_zed"; RunOnceId: "zed-stop"; Flags: runhidden; StatusMsg: "Stopping ZED Service..."
Filename: "{sys}\sc.exe"; Parameters: "delete OpenZFS_zed"; RunOnceId: "zed-delete"; Flags: runhidden; StatusMsg: "Removing ZED Service..."
Filename: "{sys}\rundll32.exe"; Parameters: "setupapi.dll,InstallHinfSection DefaultUninstall 132 {app}\zed.inf"; RunOnceId: "zed-uninstalled"; Flags: runhidden; StatusMsg: "Uninstalling zed driver..."
Filename: "{app}\ZFSInstaller.exe"; Parameters: "uninstall .\OpenZFS.inf"; RunOnceId: "driver"; Flags: runascurrentuser;

[Registry]
Root: HKLM; Subkey: "Software\{#MyAppPublisher}\{#MyAppName}"; Flags: uninsdeletekeyifempty
Root: HKLM; Subkey: "Software\{#MyAppPublisher}\{#MyAppName}"; ValueType: string; ValueName: "InstallLocation"; ValueData: "{app}"
