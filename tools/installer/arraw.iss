; arraw Windows installer (Inno Setup 6). Per-user, no UAC.
; Driven by tools/package_windows.py --installer, which passes /D defines:
;   AppVersion - version from CMakeLists project()
;   StageDir   - absolute path to the staged app (exe + DLLs + plugin dirs + CRT)
;   OutputDir  - absolute path to write the setup.exe into
; Decision of record: docs/adr/0016-windows-installer-inno-setup.md

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\..\build-release\_package\arraw-" + AppVersion + "-windows-x64"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

#define AppName "arraw"
#define AppExe "arraw.exe"

[Setup]
AppId=io.github.janpipek.arraw
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Jan Pipek
AppPublisherURL=https://github.com/janpipek/arraw
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
SetupIconFile=..\..\resources\arraw.ico
UninstallDisplayIcon={app}\{#AppExe}
OutputDir={#OutputDir}
OutputBaseFilename=arraw-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesAssociations=yes

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked
Name: "assocraw"; Description: "Associate RAW files (.cr2, .nef, .arw, .dng, ...) with arraw"; GroupDescription: "File associations:"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{userdesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "Launch arraw"; Flags: nowait postinstall skipifsilent

[Registry]
; Per-user ProgID (HKCU only, no admin). Whole key removed on uninstall.
Root: HKCU; Subkey: "Software\Classes\arraw.RawImage"; ValueType: string; ValueName: ""; ValueData: "arraw RAW image"; Flags: uninsdeletekey; Tasks: assocraw
Root: HKCU; Subkey: "Software\Classes\arraw.RawImage\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: assocraw
Root: HKCU; Subkey: "Software\Classes\arraw.RawImage\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: assocraw
; Map each RAW extension to the ProgID. uninsdeletevalue removes our value on uninstall.
Root: HKCU; Subkey: "Software\Classes\.cr2"; ValueType: string; ValueName: ""; ValueData: "arraw.RawImage"; Flags: uninsdeletevalue; Tasks: assocraw
Root: HKCU; Subkey: "Software\Classes\.cr3"; ValueType: string; ValueName: ""; ValueData: "arraw.RawImage"; Flags: uninsdeletevalue; Tasks: assocraw
Root: HKCU; Subkey: "Software\Classes\.nef"; ValueType: string; ValueName: ""; ValueData: "arraw.RawImage"; Flags: uninsdeletevalue; Tasks: assocraw
Root: HKCU; Subkey: "Software\Classes\.arw"; ValueType: string; ValueName: ""; ValueData: "arraw.RawImage"; Flags: uninsdeletevalue; Tasks: assocraw
Root: HKCU; Subkey: "Software\Classes\.dng"; ValueType: string; ValueName: ""; ValueData: "arraw.RawImage"; Flags: uninsdeletevalue; Tasks: assocraw
Root: HKCU; Subkey: "Software\Classes\.raf"; ValueType: string; ValueName: ""; ValueData: "arraw.RawImage"; Flags: uninsdeletevalue; Tasks: assocraw
Root: HKCU; Subkey: "Software\Classes\.orf"; ValueType: string; ValueName: ""; ValueData: "arraw.RawImage"; Flags: uninsdeletevalue; Tasks: assocraw
Root: HKCU; Subkey: "Software\Classes\.rw2"; ValueType: string; ValueName: ""; ValueData: "arraw.RawImage"; Flags: uninsdeletevalue; Tasks: assocraw
Root: HKCU; Subkey: "Software\Classes\.pef"; ValueType: string; ValueName: ""; ValueData: "arraw.RawImage"; Flags: uninsdeletevalue; Tasks: assocraw
Root: HKCU; Subkey: "Software\Classes\.srw"; ValueType: string; ValueName: ""; ValueData: "arraw.RawImage"; Flags: uninsdeletevalue; Tasks: assocraw
