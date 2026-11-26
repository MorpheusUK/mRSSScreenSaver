#ifndef BuildRoot
  #define BuildRoot "..\build"
#endif
[Setup]
AppId={{052F838E-2A1B-4BC8-B186-8CB0FC9F38F2}
AppName=mRSS Screen Saver
AppVersion=1.0.0
AppPublisher=MorpheusUK
DefaultDirName={autopf}\mRSS Screen Saver
DefaultGroupName=mRSS Screen Saver
DisableProgramGroupPage=no
AllowNoIcons=yes
SetupIconFile=..\assets\mrss.ico
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22000
PrivilegesRequired=admin
WizardStyle=modern dynamic
OutputDir={#BuildRoot}\dist
OutputBaseFilename=mRSSScreenSaver-1.0.0-x64-setup
Compression=lzma2
SolidCompression=yes
UninstallDisplayName=mRSS Screen Saver
UninstallDisplayIcon={sys}\mRSSScreenSaver.scr
CloseApplications=yes
RestartApplications=no
SetupLogging=no

[Tasks]
Name: current; Description: "Make this my current screen saver"; Flags: unchecked

[Files]
Source: "{#BuildRoot}\bin\Release\mRSSScreenSaver.scr"; DestDir: "{sys}"; Flags: ignoreversion 64bit
Source: "{#BuildRoot}\package\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
; Administrative install: {group} uses the common Start menu, visible to every user.
Name: "{group}\Screen Saver Settings"; Filename: "{sys}\mRSSScreenSaver.scr"; Parameters: "/c"
Name: "{group}\Testing guide"; Filename: "{app}\TESTING.md"
Name: "{group}\License"; Filename: "{app}\LICENSE.txt"
Name: "{group}\Uninstall mRSS Screen Saver"; Filename: "{uninstallexe}"

[Run]
Filename: "{sys}\mRSSScreenSaver.scr"; Parameters: "/select"; Tasks: current; Flags: runasoriginaluser waituntilterminated skipifsilent
Filename: "{sys}\mRSSScreenSaver.scr"; Parameters: "/c"; Description: "Configure mRSS Screen Saver"; Flags: postinstall unchecked runasoriginaluser nowait skipifsilent
Filename: "{sys}\rundll32.exe"; Parameters: "shell32.dll,Control_RunDLL desk.cpl,,1"; Description: "Open Windows Screen Saver Settings"; Flags: postinstall unchecked runasoriginaluser nowait skipifsilent

[UninstallRun]
Filename: "{sys}\mRSSScreenSaver.scr"; Parameters: "/unselect"; Flags: runhidden waituntilterminated; RunOnceId: "RestoreCurrentScreenSaver"

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  { User caches and feed URLs are kept. They are per-user and may belong to a
    different original account than the elevated installer account. }
end;
