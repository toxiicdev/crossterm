; *** Inno Setup version 6.7.3 Filipino messages ***
;
; Translation of Default.isl for Inno Setup 6.7.3, made for CrossTerm.
;
; Note: When translating this text, do not add periods (.) to the end of
; messages that didn't have them already, because on those messages Inno
; Setup adds the periods automatically (appending a period would result in
; two periods being displayed).

[LangOptions]
; The following three entries are very important. Be sure to read and
; understand the '[LangOptions] section' topic in the help file.
LanguageName=Filipino
LanguageID=$0464
LanguageCodePage=0
; If the language you are translating to requires special font faces or
; sizes, uncomment any of the following entries and change them accordingly.
;DialogFontName=
;DialogFontSize=9
;DialogFontBaseScaleWidth=7
;DialogFontBaseScaleHeight=15
;WelcomeFontName=Segoe UI
;WelcomeFontSize=14

[Messages]

; *** Application titles
SetupAppTitle=Setup
SetupWindowTitle=Setup - %1
UninstallAppTitle=Pag-uninstall
UninstallAppFullTitle=Pag-uninstall ng %1

; *** Misc. common
InformationTitle=Impormasyon
ConfirmTitle=Kumpirmahin
ErrorTitle=Error

; *** SetupLdr messages
SetupLdrStartupMessage=Ii-install nito ang %1. Gusto mo bang magpatuloy?
LdrCannotCreateTemp=Hindi makagawa ng pansamantalang file. Itinigil ang Setup
LdrCannotExecTemp=Hindi mapatakbo ang file sa pansamantalang folder. Itinigil ang Setup
HelpTextNote=

; *** Startup error messages
LastErrorMessage=%1.%n%nError %2: %3
SetupFileMissing=Nawawala ang file na %1 sa folder ng installation. Pakiayos ang problema o kumuha ng bagong kopya ng program.
SetupFileCorrupt=Sira ang mga file ng setup. Pakikuha ng bagong kopya ng program.
SetupFileCorruptOrWrongVer=Sira ang mga file ng setup, o hindi tugma sa bersyong ito ng Setup. Pakiayos ang problema o kumuha ng bagong kopya ng program.
InvalidParameter=May di-wastong parameter na ipinasa sa command line:%n%n%1
SetupAlreadyRunning=Tumatakbo na ang Setup.
WindowsVersionNotSupported=Hindi sinusuportahan ng program na ito ang bersyon ng Windows na tumatakbo sa iyong computer.
WindowsServicePackRequired=Kailangan ng program na ito ang %1 Service Pack %2 o mas bago.
NotOnThisPlatform=Hindi tatakbo ang program na ito sa %1.
OnlyOnThisPlatform=Dapat patakbuhin ang program na ito sa %1.
OnlyOnTheseArchitectures=Maaari lamang i-install ang program na ito sa mga bersyon ng Windows na ginawa para sa mga sumusunod na processor architecture:%n%n%1
WinVersionTooLowError=Kailangan ng program na ito ang %1 bersyon %2 o mas bago.
WinVersionTooHighError=Hindi maaaring i-install ang program na ito sa %1 bersyon %2 o mas bago.
AdminPrivilegesRequired=Dapat ay naka-log in ka bilang administrator kapag ini-install ang program na ito.
PowerUserPrivilegesRequired=Dapat ay naka-log in ka bilang administrator o bilang miyembro ng grupong Power Users kapag ini-install ang program na ito.
SetupAppRunningError=Natukoy ng Setup na kasalukuyang tumatakbo ang %1.%n%nPakisara ang lahat ng instance nito ngayon, saka i-click ang OK para magpatuloy, o Cancel para lumabas.
UninstallAppRunningError=Natukoy ng Uninstall na kasalukuyang tumatakbo ang %1.%n%nPakisara ang lahat ng instance nito ngayon, saka i-click ang OK para magpatuloy, o Cancel para lumabas.

; *** Startup questions
PrivilegesRequiredOverrideTitle=Piliin ang Mode ng Pag-install
PrivilegesRequiredOverrideInstruction=Piliin ang mode ng pag-install
PrivilegesRequiredOverrideText1=Maaaring i-install ang %1 para sa lahat ng user (kailangan ng pahintulot ng administrator), o para sa iyo lamang.
PrivilegesRequiredOverrideText2=Maaaring i-install ang %1 para sa iyo lamang, o para sa lahat ng user (kailangan ng pahintulot ng administrator).
PrivilegesRequiredOverrideAllUsers=I-install para sa &lahat ng user
PrivilegesRequiredOverrideAllUsersRecommended=I-install para sa &lahat ng user (inirerekomenda)
PrivilegesRequiredOverrideCurrentUser=I-install para sa &akin lamang
PrivilegesRequiredOverrideCurrentUserRecommended=I-install para sa &akin lamang (inirerekomenda)

; *** Misc. errors
ErrorCreatingDir=Hindi magawa ng Setup ang folder na "%1"
ErrorTooManyFilesInDir=Hindi makagawa ng file sa folder na "%1" dahil masyadong marami na itong file

; *** Setup common messages
ExitSetupTitle=Lumabas sa Setup
ExitSetupMessage=Hindi pa tapos ang Setup. Kung lalabas ka ngayon, hindi mai-install ang program.%n%nMaaari mong patakbuhin muli ang Setup sa ibang pagkakataon para tapusin ang pag-install.%n%nLumabas sa Setup?
AboutSetupMenuItem=&Tungkol sa Setup...
AboutSetupTitle=Tungkol sa Setup
AboutSetupMessage=%1 bersyon %2%n%3%n%nHome page ng %1:%n%4
AboutSetupNote=
TranslatorNote=

; *** Buttons
ButtonBack=< &Bumalik
ButtonNext=&Susunod >
ButtonInstall=&I-install
ButtonOK=OK
ButtonCancel=Kanselahin
ButtonYes=&Oo
ButtonYesToAll=Oo sa &Lahat
ButtonNo=&Hindi
ButtonNoToAll=Hin&di sa Lahat
ButtonFinish=&Tapusin
ButtonBrowse=&Mag-browse...
ButtonWizardBrowse=Mag-b&rowse...
ButtonNewFolder=&Gumawa ng Bagong Folder

; *** "Select Language" dialog messages
SelectLanguageTitle=Piliin ang Wika ng Setup
SelectLanguageLabel=Piliin ang wikang gagamitin habang nag-i-install.

; *** Common wizard text
ClickNext=I-click ang Susunod para magpatuloy, o Kanselahin para lumabas sa Setup.
BeveledLabel=
BrowseDialogTitle=Mag-browse ng Folder
BrowseDialogLabel=Pumili ng folder sa listahan sa ibaba, saka i-click ang OK.
NewFolderName=Bagong Folder

; *** "Welcome" wizard page
WelcomeLabel1=Maligayang pagdating sa Setup Wizard ng [name]
WelcomeLabel2=Ii-install nito ang [name/ver] sa iyong computer.%n%nInirerekomendang isara mo ang lahat ng iba pang application bago magpatuloy.

; *** "Password" wizard page
WizardPassword=Password
PasswordLabel1=Protektado ng password ang installation na ito.
PasswordLabel3=Pakilagay ang password, saka i-click ang Susunod para magpatuloy. Mahalaga ang malaki at maliit na titik sa password.
PasswordEditLabel=&Password:
IncorrectPassword=Mali ang password na inilagay mo. Pakisubukan muli.

; *** "License Agreement" wizard page
WizardLicense=Kasunduan sa Lisensya
LicenseLabel=Pakibasa ang sumusunod na mahalagang impormasyon bago magpatuloy.
LicenseLabel3=Pakibasa ang sumusunod na Kasunduan sa Lisensya. Dapat mong tanggapin ang mga tuntunin ng kasunduang ito bago ituloy ang pag-install.
LicenseAccepted=&Tinatanggap ko ang kasunduan
LicenseNotAccepted=&Hindi ko tinatanggap ang kasunduan

; *** "Information" wizard pages
WizardInfoBefore=Impormasyon
InfoBeforeLabel=Pakibasa ang sumusunod na mahalagang impormasyon bago magpatuloy.
InfoBeforeClickLabel=Kapag handa ka nang ituloy ang Setup, i-click ang Susunod.
WizardInfoAfter=Impormasyon
InfoAfterLabel=Pakibasa ang sumusunod na mahalagang impormasyon bago magpatuloy.
InfoAfterClickLabel=Kapag handa ka nang ituloy ang Setup, i-click ang Susunod.

; *** "User Information" wizard page
WizardUserInfo=Impormasyon ng User
UserInfoDesc=Pakilagay ang iyong impormasyon.
UserInfoName=&Pangalan ng User:
UserInfoOrg=&Organisasyon:
UserInfoSerial=&Serial Number:
UserInfoNameRequired=Dapat kang maglagay ng pangalan.

; *** "Select Destination Location" wizard page
WizardSelectDir=Piliin ang Lokasyong Paglalagyan
SelectDirDesc=Saan dapat i-install ang [name]?
SelectDirLabel3=Ii-install ng Setup ang [name] sa sumusunod na folder.
SelectDirBrowseLabel=Para magpatuloy, i-click ang Susunod. Kung gusto mong pumili ng ibang folder, i-click ang Mag-browse.
DiskSpaceGBLabel=Kailangan ng hindi bababa sa [gb] GB na bakanteng espasyo sa disk.
DiskSpaceMBLabel=Kailangan ng hindi bababa sa [mb] MB na bakanteng espasyo sa disk.
CannotInstallToNetworkDrive=Hindi makapag-install ang Setup sa network drive.
CannotInstallToUNCPath=Hindi makapag-install ang Setup sa UNC path.
InvalidPath=Dapat kang maglagay ng buong path na may drive letter; halimbawa:%n%nC:\APP%n%no UNC path na nasa anyong:%n%n\\server\share
InvalidDrive=Hindi umiiral o hindi ma-access ang drive o UNC share na pinili mo. Pakipili ng iba.
DiskSpaceWarningTitle=Kulang ang Espasyo sa Disk
DiskSpaceWarning=Kailangan ng Setup ng hindi bababa sa %1 KB na bakanteng espasyo para mag-install, pero %2 KB lamang ang bakante sa napiling drive.%n%nGusto mo pa rin bang magpatuloy?
DirNameTooLong=Masyadong mahaba ang pangalan ng folder o ang path.
InvalidDirName=Hindi wasto ang pangalan ng folder.
BadDirName32=Hindi maaaring maglaman ang pangalan ng folder ng alinman sa mga sumusunod na character:%n%n%1
DirExistsTitle=Umiiral na ang Folder
DirExists=Ang folder na:%n%n%1%n%nay umiiral na. Gusto mo pa rin bang mag-install sa folder na iyon?
DirDoesntExistTitle=Hindi Umiiral ang Folder
DirDoesntExist=Ang folder na:%n%n%1%n%nay hindi umiiral. Gusto mo bang gawin ang folder?

; *** "Select Components" wizard page
WizardSelectComponents=Piliin ang mga Component
SelectComponentsDesc=Aling mga component ang dapat i-install?
SelectComponentsLabel2=Piliin ang mga component na gusto mong i-install; alisin ang check sa mga ayaw mong i-install. I-click ang Susunod kapag handa ka nang magpatuloy.
FullInstallation=Buong installation
; if possible don't translate 'Compact' as 'Minimal' (I mean 'Minimal' in your language)
CompactInstallation=Compact na installation
CustomInstallation=Custom na installation
NoUninstallWarningTitle=May mga Component na Umiiral
NoUninstallWarning=Natukoy ng Setup na naka-install na sa iyong computer ang mga sumusunod na component:%n%n%1%n%nHindi mai-uninstall ang mga component na ito kapag inalis mo ang pagkakapili sa kanila.%n%nGusto mo pa rin bang magpatuloy?
ComponentSize1=%1 KB
ComponentSize2=%1 MB
ComponentsDiskSpaceGBLabel=Kailangan ng kasalukuyang pinili ang hindi bababa sa [gb] GB na espasyo sa disk.
ComponentsDiskSpaceMBLabel=Kailangan ng kasalukuyang pinili ang hindi bababa sa [mb] MB na espasyo sa disk.

; *** "Select Additional Tasks" wizard page
WizardSelectTasks=Piliin ang mga Karagdagang Gawain
SelectTasksDesc=Aling mga karagdagang gawain ang dapat isagawa?
SelectTasksLabel2=Piliin ang mga karagdagang gawain na gusto mong isagawa ng Setup habang ini-install ang [name], saka i-click ang Susunod.

; *** "Select Start Menu Folder" wizard page
WizardSelectProgramGroup=Piliin ang Folder sa Start Menu
SelectStartMenuFolderDesc=Saan dapat ilagay ng Setup ang mga shortcut ng program?
SelectStartMenuFolderLabel3=Gagawin ng Setup ang mga shortcut ng program sa sumusunod na folder sa Start Menu.
SelectStartMenuFolderBrowseLabel=Para magpatuloy, i-click ang Susunod. Kung gusto mong pumili ng ibang folder, i-click ang Mag-browse.
MustEnterGroupName=Dapat kang maglagay ng pangalan ng folder.
GroupNameTooLong=Masyadong mahaba ang pangalan ng folder o ang path.
InvalidGroupName=Hindi wasto ang pangalan ng folder.
BadGroupName=Hindi maaaring maglaman ang pangalan ng folder ng alinman sa mga sumusunod na character:%n%n%1
NoProgramGroupCheck2=&Huwag gumawa ng folder sa Start Menu

; *** "Ready to Install" wizard page
WizardReady=Handa nang Mag-install
ReadyLabel1=Handa na ang Setup na simulan ang pag-install ng [name] sa iyong computer.
ReadyLabel2a=I-click ang I-install para ituloy ang pag-install, o i-click ang Bumalik kung gusto mong suriin o baguhin ang anumang setting.
ReadyLabel2b=I-click ang I-install para ituloy ang pag-install.
ReadyMemoUserInfo=Impormasyon ng user:
ReadyMemoDir=Lokasyong paglalagyan:
ReadyMemoType=Uri ng setup:
ReadyMemoComponents=Mga napiling component:
ReadyMemoGroup=Folder sa Start Menu:
ReadyMemoTasks=Mga karagdagang gawain:

; *** TDownloadWizardPage wizard page and DownloadTemporaryFile
DownloadingLabel2=Dina-download ang mga file...
ButtonStopDownload=&Ihinto ang pag-download
StopDownload=Sigurado ka bang gusto mong ihinto ang pag-download?
ErrorDownloadAborted=Itinigil ang pag-download
ErrorDownloadFailed=Nabigo ang pag-download: %1 %2
ErrorDownloadSizeFailed=Nabigong makuha ang laki: %1 %2
ErrorProgress=Di-wastong progreso: %1 sa %2
ErrorFileSize=Di-wastong laki ng file: inaasahan ang %1, nakita ang %2

; *** TExtractionWizardPage wizard page and ExtractArchive
ExtractingLabel=Ine-extract ang mga file...
ButtonStopExtraction=&Ihinto ang pag-extract
StopExtraction=Sigurado ka bang gusto mong ihinto ang pag-extract?
ErrorExtractionAborted=Itinigil ang pag-extract
ErrorExtractionFailed=Nabigo ang pag-extract: %1

; *** Archive extraction failure details
ArchiveIncorrectPassword=Mali ang password
ArchiveIsCorrupted=Sira ang archive
ArchiveUnsupportedFormat=Hindi sinusuportahan ang format ng archive

; *** "Preparing to Install" wizard page
WizardPreparing=Naghahandang Mag-install
PreparingDesc=Naghahanda ang Setup na i-install ang [name] sa iyong computer.
PreviousInstallNotCompleted=Hindi natapos ang pag-install/pag-alis ng isang naunang program. Kailangan mong i-restart ang iyong computer para matapos iyon.%n%nPagkatapos i-restart ang computer, patakbuhin muli ang Setup para tapusin ang pag-install ng [name].
CannotContinue=Hindi makapagpatuloy ang Setup. Paki-click ang Kanselahin para lumabas.
ApplicationsFound=Ginagamit ng mga sumusunod na application ang mga file na kailangang i-update ng Setup. Inirerekomendang payagan mo ang Setup na awtomatikong isara ang mga application na ito.
ApplicationsFound2=Ginagamit ng mga sumusunod na application ang mga file na kailangang i-update ng Setup. Inirerekomendang payagan mo ang Setup na awtomatikong isara ang mga application na ito. Pagkatapos ng pag-install, susubukan ng Setup na i-restart ang mga application.
CloseApplications=&Awtomatikong isara ang mga application
DontCloseApplications=&Huwag isara ang mga application
ErrorCloseApplications=Hindi awtomatikong naisara ng Setup ang lahat ng application. Inirerekomendang isara mo ang lahat ng application na gumagamit ng mga file na kailangang i-update ng Setup bago magpatuloy.
PrepareToInstallNeedsRestart=Kailangang i-restart ng Setup ang iyong computer. Pagkatapos i-restart ang computer, patakbuhin muli ang Setup para tapusin ang pag-install ng [name].%n%nGusto mo bang mag-restart ngayon?

; *** "Installing" wizard page
WizardInstalling=Nag-i-install
InstallingLabel=Pakihintay habang ini-install ng Setup ang [name] sa iyong computer.

; *** "Setup Completed" wizard page
FinishedHeadingLabel=Tinatapos ang Setup Wizard ng [name]
FinishedLabelNoIcons=Tapos nang i-install ng Setup ang [name] sa iyong computer.
FinishedLabel=Tapos nang i-install ng Setup ang [name] sa iyong computer. Maaaring buksan ang application sa pamamagitan ng mga na-install na shortcut.
ClickFinish=I-click ang Tapusin para lumabas sa Setup.
FinishedRestartLabel=Para matapos ang pag-install ng [name], kailangang i-restart ng Setup ang iyong computer. Gusto mo bang mag-restart ngayon?
FinishedRestartMessage=Para matapos ang pag-install ng [name], kailangang i-restart ng Setup ang iyong computer.%n%nGusto mo bang mag-restart ngayon?
ShowReadmeCheck=Oo, gusto kong tingnan ang README file
YesRadio=&Oo, i-restart ang computer ngayon
NoRadio=&Hindi, ire-restart ko ang computer mamaya
; used for example as 'Run MyProg.exe'
RunEntryExec=Patakbuhin ang %1
; used for example as 'View Readme.txt'
RunEntryShellExec=Tingnan ang %1

; *** "Setup Needs the Next Disk" stuff
ChangeDiskTitle=Kailangan ng Setup ang Susunod na Disk
SelectDiskLabel2=Pakipasok ang Disk %1 at i-click ang OK.%n%nKung ang mga file sa disk na ito ay nasa ibang folder bukod sa nakikita sa ibaba, ilagay ang tamang path o i-click ang Mag-browse.
PathLabel=&Path:
FileNotInDir2=Hindi mahanap ang file na "%1" sa "%2". Pakipasok ang tamang disk o pumili ng ibang folder.
SelectDirectoryLabel=Pakitukoy ang lokasyon ng susunod na disk.

; *** Installation phase messages
SetupAborted=Hindi natapos ang Setup.%n%nPakiayos ang problema at patakbuhin muli ang Setup.
AbortRetryIgnoreSelectAction=Pumili ng aksyon
AbortRetryIgnoreRetry=&Subukan muli
AbortRetryIgnoreIgnore=&Balewalain ang error at magpatuloy
AbortRetryIgnoreCancel=Kanselahin ang pag-install
RetryCancelSelectAction=Pumili ng aksyon
RetryCancelRetry=&Subukan muli
RetryCancelCancel=Kanselahin

; *** Installation status messages
StatusClosingApplications=Isinasara ang mga application...
StatusCreateDirs=Ginagawa ang mga folder...
StatusExtractFiles=Ine-extract ang mga file...
StatusDownloadFiles=Dina-download ang mga file...
StatusCreateIcons=Ginagawa ang mga shortcut...
StatusCreateIniEntries=Ginagawa ang mga INI entry...
StatusCreateRegistryEntries=Ginagawa ang mga registry entry...
StatusRegisterFiles=Inirerehistro ang mga file...
StatusSavingUninstall=Sine-save ang impormasyon para sa pag-uninstall...
StatusRunProgram=Tinatapos ang pag-install...
StatusRestartingApplications=Nire-restart ang mga application...
StatusRollback=Ibinabalik ang mga pagbabago...

; *** Misc. errors
ErrorInternal2=Internal na error: %1
ErrorFunctionFailedNoCode=Nabigo ang %1
ErrorFunctionFailed=Nabigo ang %1; code %2
ErrorFunctionFailedWithMessage=Nabigo ang %1; code %2.%n%3
ErrorExecutingProgram=Hindi mapatakbo ang file:%n%1

; *** Registry errors
ErrorRegOpenKey=Error sa pagbukas ng registry key:%n%1\%2
ErrorRegCreateKey=Error sa paggawa ng registry key:%n%1\%2
ErrorRegWriteKey=Error sa pagsulat sa registry key:%n%1\%2

; *** INI errors
ErrorIniEntry=Error sa paggawa ng INI entry sa file na "%1".

; *** File copying errors
FileAbortRetryIgnoreSkipNotRecommended=&Laktawan ang file na ito (hindi inirerekomenda)
FileAbortRetryIgnoreIgnoreNotRecommended=&Balewalain ang error at magpatuloy (hindi inirerekomenda)
SourceIsCorrupted=Sira ang source file
SourceDoesntExist=Hindi umiiral ang source file na "%1"
SourceVerificationFailed=Nabigo ang beripikasyon ng source file: %1
VerificationSignatureDoesntExist=Hindi umiiral ang signature file na "%1"
VerificationSignatureInvalid=Di-wasto ang signature file na "%1"
VerificationKeyNotFound=Gumagamit ang signature file na "%1" ng hindi kilalang key
VerificationFileNameIncorrect=Mali ang pangalan ng file
VerificationFileTagIncorrect=Mali ang tag ng file
VerificationFileSizeIncorrect=Mali ang laki ng file
VerificationFileHashIncorrect=Mali ang hash ng file
ExistingFileReadOnly2=Hindi mapalitan ang umiiral na file dahil naka-mark ito bilang read-only.
ExistingFileReadOnlyRetry=&Alisin ang read-only na attribute at subukan muli
ExistingFileReadOnlyKeepExisting=&Panatilihin ang umiiral na file
ErrorReadingExistingDest=May naganap na error habang binabasa ang umiiral na file:
FileExistsSelectAction=Pumili ng aksyon
FileExists2=Umiiral na ang file.
FileExistsOverwriteExisting=&Patungan ang umiiral na file
FileExistsKeepExisting=Pana&tilihin ang umiiral na file
FileExistsOverwriteOrKeepAll=&Gawin ito sa mga susunod na conflict
ExistingFileNewerSelectAction=Pumili ng aksyon
ExistingFileNewer2=Mas bago ang umiiral na file kaysa sa sinusubukang i-install ng Setup.
ExistingFileNewerOverwriteExisting=&Patungan ang umiiral na file
ExistingFileNewerKeepExisting=Pana&tilihin ang umiiral na file (inirerekomenda)
ExistingFileNewerOverwriteOrKeepAll=&Gawin ito sa mga susunod na conflict
ErrorChangingAttr=May naganap na error habang binabago ang mga attribute ng umiiral na file:
ErrorCreatingTemp=May naganap na error habang gumagawa ng file sa folder na paglalagyan:
ErrorReadingSource=May naganap na error habang binabasa ang source file:
ErrorCopying=May naganap na error habang kinokopya ang isang file:
ErrorDownloading=May naganap na error habang nagda-download ng file:
ErrorExtracting=May naganap na error habang ine-extract ang isang archive:
ErrorReplacingExistingFile=May naganap na error habang pinapalitan ang umiiral na file:
ErrorRestartReplace=Nabigo ang RestartReplace:
ErrorRenamingTemp=May naganap na error habang pinapalitan ang pangalan ng file sa folder na paglalagyan:
ErrorRegisterServer=Hindi mairehistro ang DLL/OCX: %1
ErrorRegSvr32Failed=Nabigo ang RegSvr32 na may exit code %1
ErrorRegisterTypeLib=Hindi mairehistro ang type library: %1

; *** Uninstall display name markings
; used for example as 'My Program (32-bit)'
UninstallDisplayNameMark=%1 (%2)
; used for example as 'My Program (32-bit, All users)'
UninstallDisplayNameMarks=%1 (%2, %3)
UninstallDisplayNameMark32Bit=32-bit
UninstallDisplayNameMark64Bit=64-bit
UninstallDisplayNameMarkAllUsers=Lahat ng user
UninstallDisplayNameMarkCurrentUser=Kasalukuyang user

; *** Post-installation errors
ErrorOpeningReadme=May naganap na error habang binubuksan ang README file.
ErrorRestartingComputer=Hindi ma-restart ng Setup ang computer. Pakigawa ito nang mano-mano.

; *** Uninstaller messages
UninstallNotFound=Hindi umiiral ang file na "%1". Hindi makapag-uninstall.
UninstallOpenError=Hindi mabuksan ang file na "%1". Hindi makapag-uninstall
UninstallUnsupportedVer=Ang uninstall log file na "%1" ay nasa format na hindi kilala ng bersyong ito ng uninstaller. Hindi makapag-uninstall
UninstallUnknownEntry=May nakitang hindi kilalang entry (%1) sa uninstall log
ConfirmUninstall=Sigurado ka bang gusto mong tuluyang alisin ang %1 at ang lahat ng component nito?
UninstallOnlyOnWin64=Maaari lamang i-uninstall ang installation na ito sa 64-bit na Windows.
OnlyAdminCanUninstall=Maaari lamang i-uninstall ang installation na ito ng user na may pahintulot ng administrator.
UninstallStatusLabel=Pakihintay habang inaalis ang %1 sa iyong computer.
UninstalledAll=Matagumpay na naalis ang %1 sa iyong computer.
UninstalledMost=Tapos na ang pag-uninstall ng %1.%n%nMay ilang elementong hindi naalis. Maaari mong alisin ang mga ito nang mano-mano.
UninstalledAndNeedsRestart=Para matapos ang pag-uninstall ng %1, kailangang i-restart ang iyong computer.%n%nGusto mo bang mag-restart ngayon?
UninstallDataCorrupted=Sira ang file na "%1". Hindi makapag-uninstall

; *** Uninstallation phase messages
ConfirmDeleteSharedFileTitle=Alisin ang Shared File?
ConfirmDeleteSharedFile2=Ipinapakita ng system na hindi na ginagamit ng anumang program ang sumusunod na shared file. Gusto mo bang alisin ng Uninstall ang shared file na ito?%n%nKung may program pang gumagamit ng file na ito at inalis ito, maaaring hindi gumana nang maayos ang mga program na iyon. Kung hindi ka sigurado, piliin ang Hindi. Walang masamang mangyayari kung iiwan ang file sa iyong system.
SharedFileNameLabel=Pangalan ng file:
SharedFileLocationLabel=Lokasyon:
WizardUninstalling=Katayuan ng Pag-uninstall
StatusUninstalling=Ina-uninstall ang %1...

; *** Shutdown block reasons
ShutdownBlockReasonInstallingApp=Ini-install ang %1.
ShutdownBlockReasonUninstallingApp=Ina-uninstall ang %1.

; The custom messages below aren't used by Setup itself, but if you make
; use of them in your scripts, you'll want to translate them.

[CustomMessages]

NameAndVersion=%1 bersyon %2
AdditionalIcons=Mga karagdagang shortcut:
CreateDesktopIcon=Gumawa ng shortcut sa &desktop
CreateQuickLaunchIcon=Gumawa ng shortcut sa &Quick Launch
ProgramOnTheWeb=%1 sa Web
UninstallProgram=I-uninstall ang %1
LaunchProgram=Buksan ang %1
AssocFileExtension=&Iugnay ang %1 sa file extension na %2
AssocingFileExtension=Iniuugnay ang %1 sa file extension na %2...
AutoStartProgramGroupDescription=Startup:
AutoStartProgram=Awtomatikong simulan ang %1
AddonHostProgramNotFound=Hindi mahanap ang %1 sa folder na pinili mo.%n%nGusto mo pa rin bang magpatuloy?
