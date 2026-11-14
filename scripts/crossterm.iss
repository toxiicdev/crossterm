; Inno Setup script for the Windows installer, compiled by build-windows.ps1:
;   ISCC.exe /DVersion=<x.y> /DSource=<portable folder> /O<output folder> scripts\crossterm.iss
; Installs for all users (administrator) or only for the current user (no administrator), chosen at start.
#ifndef Version
  #define Version "0.0"
#endif
#ifndef Source
  #define Source "..\dist"
#endif

[Setup]
AppId={{6F3B8C2A-4E1D-4B7A-9C55-2D8E1F0A7C31}
AppName=CrossTerm
AppVersion={#Version}
AppPublisher=CrossTerm
AppPublisherURL=https://github.com/toxiicdev/crossterm
DefaultDirName={autopf}\CrossTerm
DefaultGroupName=CrossTerm
DisableProgramGroupPage=yes
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=CrossTerm-{#Version}-Windows-Setup
SetupIconFile=..\assets\crossterm.ico
UninstallDisplayIcon={app}\CrossTerm.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
ShowLanguageDialog=auto

; Every language of the program that Inno Setup has a translation for; the system language is picked automatically.
; Unofficial translations (inno-languages\) come from jrsoftware/issrc at tag is-6_7_3. No Inno Setup translation
; exists for am, fil, gu, ha, jv, kn, pa, sw, te: those systems get English.
[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ar"; MessagesFile: "compiler:Languages\Arabic.isl"
Name: "bn"; MessagesFile: "inno-languages\Bengali.islu"
Name: "de"; MessagesFile: "compiler:Languages\German.isl"
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "fa"; MessagesFile: "inno-languages\Farsi.isl"
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"
Name: "hi"; MessagesFile: "inno-languages\Hindi.islu"
Name: "id"; MessagesFile: "inno-languages\Indonesian.isl"
Name: "it"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "ja"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "ko"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "mr"; MessagesFile: "inno-languages\Marathi.islu"
Name: "pt_BR"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "ta"; MessagesFile: "compiler:Languages\Tamil.isl"
Name: "th"; MessagesFile: "compiler:Languages\Thai.isl"
Name: "tr"; MessagesFile: "compiler:Languages\Turkish.isl"
Name: "ur"; MessagesFile: "inno-languages\Urdu.isl"
Name: "vi"; MessagesFile: "inno-languages\Vietnamese.isl"
Name: "zh_CN"; MessagesFile: "inno-languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; CrossTerm.portable would switch the installed copy to portable mode (data next to the executable).
Source: "{#Source}\*"; DestDir: "{app}"; Excludes: "CrossTerm.portable,CrossTerm-data"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\CrossTerm"; Filename: "{app}\CrossTerm.exe"
Name: "{autodesktop}\CrossTerm"; Filename: "{app}\CrossTerm.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\CrossTerm.exe"; Description: "{cm:LaunchProgram,CrossTerm}"; Flags: nowait postinstall skipifsilent
