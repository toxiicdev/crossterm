; *** Inno Setup version 6.5.0+ Javanese messages ***
;
; Translation of Default.isl for Inno Setup 6.7.3, made for CrossTerm.
;
; Javanese has no Windows language identifier, so LanguageID is $0C00
; (custom locale) and Setup cannot detect it from the system language:
; users pick it themselves in the "Select Setup Language" dialog.
;
; To download user-contributed translations of this file, go to:
;   https://jrsoftware.org/files/istrans/
;
; Note: When translating this text, do not add periods (.) to the end of
; messages that didn't have them already, because on those messages Inno
; Setup adds the periods automatically (appending a period would result in
; two periods being displayed).

[LangOptions]
; The following three entries are very important. Be sure to read and
; understand the '[LangOptions] section' topic in the help file.
LanguageName=Basa Jawa
LanguageID=$0C00
; LanguageCodePage should always be set if possible, even if this file is Unicode
; Set to zero: this file is Unicode
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
SetupAppTitle=Pamasangan
SetupWindowTitle=Pamasangan - %1
UninstallAppTitle=Pambusakan
UninstallAppFullTitle=Pambusakan %1

; *** Misc. common
InformationTitle=Informasi
ConfirmTitle=Konfirmasi
ErrorTitle=Kasalahan

; *** SetupLdr messages
SetupLdrStartupMessage=Iki bakal masang %1. Apa panjenengan arep nerusake?
LdrCannotCreateTemp=Ora bisa nggawe berkas sementara. Pamasangan dibatalake
LdrCannotExecTemp=Ora bisa nglakokake berkas ing direktori sementara. Pamasangan dibatalake
HelpTextNote=

; *** Startup error messages
LastErrorMessage=%1.%n%nKasalahan %2: %3
SetupFileMissing=Berkas %1 ora ana ing direktori instalasi. Mangga dandani masalahe utawa golek salinan program sing anyar.
SetupFileCorrupt=Berkas pamasangan rusak. Mangga golek salinan program sing anyar.
SetupFileCorruptOrWrongVer=Berkas pamasangan rusak, utawa ora cocog karo versi Pamasangan iki. Mangga dandani masalahe utawa golek salinan program sing anyar.
InvalidParameter=Ana parameter sing ora sah ing baris prentah:%n%n%1
SetupAlreadyRunning=Pamasangan wis lagi mlaku.
WindowsVersionNotSupported=Program iki ora ndhukung versi Windows ing komputer panjenengan.
WindowsServicePackRequired=Program iki mbutuhake %1 Service Pack %2 utawa sing luwih anyar.
NotOnThisPlatform=Program iki ora bakal mlaku ing %1.
OnlyOnThisPlatform=Program iki kudu dilakokake ing %1.
OnlyOnTheseArchitectures=Program iki mung bisa dipasang ing versi Windows sing dirancang kanggo arsitektur prosesor iki:%n%n%1
WinVersionTooLowError=Program iki mbutuhake %1 versi %2 utawa sing luwih anyar.
WinVersionTooHighError=Program iki ora bisa dipasang ing %1 versi %2 utawa sing luwih anyar.
AdminPrivilegesRequired=Panjenengan kudu mlebu minangka administrator kanggo masang program iki.
PowerUserPrivilegesRequired=Panjenengan kudu mlebu minangka administrator utawa anggota grup Power Users kanggo masang program iki.
SetupAppRunningError=Pamasangan nemokake yen %1 saiki lagi mlaku.%n%nMangga tutup kabeh jendhelane saiki, banjur klik OK kanggo nerusake, utawa Batal kanggo metu.
UninstallAppRunningError=Pambusakan nemokake yen %1 saiki lagi mlaku.%n%nMangga tutup kabeh jendhelane saiki, banjur klik OK kanggo nerusake, utawa Batal kanggo metu.

; *** Startup questions
PrivilegesRequiredOverrideTitle=Pilih Mode Pamasangan
PrivilegesRequiredOverrideInstruction=Pilih mode pamasangan
PrivilegesRequiredOverrideText1=%1 bisa dipasang kanggo kabeh pangguna (mbutuhake hak administrator), utawa mung kanggo panjenengan.
PrivilegesRequiredOverrideText2=%1 bisa dipasang mung kanggo panjenengan, utawa kanggo kabeh pangguna (mbutuhake hak administrator).
PrivilegesRequiredOverrideAllUsers=Pasang kanggo &kabeh pangguna
PrivilegesRequiredOverrideAllUsersRecommended=Pasang kanggo &kabeh pangguna (disaranake)
PrivilegesRequiredOverrideCurrentUser=Pasang mung kanggo &aku
PrivilegesRequiredOverrideCurrentUserRecommended=Pasang mung kanggo &aku (disaranake)

; *** Misc. errors
ErrorCreatingDir=Pamasangan ora bisa nggawe direktori "%1"
ErrorTooManyFilesInDir=Ora bisa nggawe berkas ing direktori "%1" amarga isine kakehan berkas

; *** Setup common messages
ExitSetupTitle=Metu saka Pamasangan
ExitSetupMessage=Pamasangan durung rampung. Yen panjenengan metu saiki, program ora bakal kapasang.%n%nPanjenengan bisa nglakokake Pamasangan maneh ing wektu liya kanggo ngrampungake instalasi.%n%nMetu saka Pamasangan?
AboutSetupMenuItem=&Babagan Pamasangan...
AboutSetupTitle=Babagan Pamasangan
AboutSetupMessage=%1 versi %2%n%3%n%nKaca ngarep %1:%n%4
AboutSetupNote=
TranslatorNote=

; *** Buttons
ButtonBack=< &Bali
ButtonNext=&Sabanjure >
ButtonInstall=&Pasang
ButtonOK=OK
ButtonCancel=Batal
ButtonYes=&Iya
ButtonYesToAll=Iya &kabeh
ButtonNo=&Ora
ButtonNoToAll=Ora k&abeh
ButtonFinish=&Rampung
ButtonBrowse=&Telusuri...
ButtonWizardBrowse=T&elusuri...
ButtonNewFolder=&Gawe folder anyar

; *** "Select Language" dialog messages
SelectLanguageTitle=Pilih Basa Pamasangan
SelectLanguageLabel=Pilih basa sing arep dienggo nalika instalasi.

; *** Common wizard text
ClickNext=Klik Sabanjure kanggo nerusake, utawa Batal kanggo metu saka Pamasangan.
BeveledLabel=
BrowseDialogTitle=Telusuri Folder
BrowseDialogLabel=Pilih folder ing dhaptar ngisor iki, banjur klik OK.
NewFolderName=Folder Anyar

; *** "Welcome" wizard page
WelcomeLabel1=Sugeng rawuh ing Wisaya Pamasangan [name]
WelcomeLabel2=Iki bakal masang [name/ver] ing komputer panjenengan.%n%nDisaranake nutup kabeh aplikasi liyane sadurunge nerusake.

; *** "Password" wizard page
WizardPassword=Tembung Sandhi
PasswordLabel1=Instalasi iki direksa nganggo tembung sandhi.
PasswordLabel3=Mangga lebokake tembung sandhi, banjur klik Sabanjure kanggo nerusake. Tembung sandhi mbedakake aksara gedhe lan cilik.
PasswordEditLabel=&Tembung sandhi:
IncorrectPassword=Tembung sandhi sing panjenengan lebokake ora bener. Mangga jajal maneh.

; *** "License Agreement" wizard page
WizardLicense=Prajanjian Lisensi
LicenseLabel=Mangga waca informasi penting iki sadurunge nerusake.
LicenseLabel3=Mangga waca Prajanjian Lisensi ing ngisor iki. Panjenengan kudu nampa syarat-syarat prajanjian iki sadurunge nerusake instalasi.
LicenseAccepted=Aku &nampa prajanjian iki
LicenseNotAccepted=Aku &ora nampa prajanjian iki

; *** "Information" wizard pages
WizardInfoBefore=Informasi
InfoBeforeLabel=Mangga waca informasi penting iki sadurunge nerusake.
InfoBeforeClickLabel=Yen panjenengan wis siyap nerusake Pamasangan, klik Sabanjure.
WizardInfoAfter=Informasi
InfoAfterLabel=Mangga waca informasi penting iki sadurunge nerusake.
InfoAfterClickLabel=Yen panjenengan wis siyap nerusake Pamasangan, klik Sabanjure.

; *** "User Information" wizard page
WizardUserInfo=Informasi Pangguna
UserInfoDesc=Mangga lebokake informasi panjenengan.
UserInfoName=&Jeneng pangguna:
UserInfoOrg=&Organisasi:
UserInfoSerial=&Nomer seri:
UserInfoNameRequired=Panjenengan kudu nglebokake jeneng.

; *** "Select Destination Location" wizard page
WizardSelectDir=Pilih Lokasi Tujuan
SelectDirDesc=Ing ngendi [name] arep dipasang?
SelectDirLabel3=Pamasangan bakal masang [name] ing folder ing ngisor iki.
SelectDirBrowseLabel=Kanggo nerusake, klik Sabanjure. Yen panjenengan pengin milih folder liya, klik Telusuri.
DiskSpaceGBLabel=Butuh paling ora [gb] GB papan disk sing kosong.
DiskSpaceMBLabel=Butuh paling ora [mb] MB papan disk sing kosong.
CannotInstallToNetworkDrive=Pamasangan ora bisa masang ing drive jaringan.
CannotInstallToUNCPath=Pamasangan ora bisa masang ing path UNC.
InvalidPath=Panjenengan kudu nglebokake path lengkap nganggo aksara drive; contone:%n%nC:\APP%n%nutawa path UNC kanthi wujud:%n%n\\server\share
InvalidDrive=Drive utawa share UNC sing panjenengan pilih ora ana utawa ora bisa diakses. Mangga pilih liyane.
DiskSpaceWarningTitle=Papan Disk Ora Cukup
DiskSpaceWarning=Pamasangan butuh paling ora %1 KB papan kosong kanggo masang, nanging drive sing dipilih mung duwe %2 KB.%n%nApa panjenengan tetep arep nerusake?
DirNameTooLong=Jeneng folder utawa path kedawan.
InvalidDirName=Jeneng folder ora sah.
BadDirName32=Jeneng folder ora kena ngemot aksara-aksara iki:%n%n%1
DirExistsTitle=Folder Wis Ana
DirExists=Folder:%n%n%1%n%nwis ana. Apa panjenengan tetep arep masang ing folder kuwi?
DirDoesntExistTitle=Folder Ora Ana
DirDoesntExist=Folder:%n%n%1%n%nora ana. Apa folder kuwi arep digawe?

; *** "Select Components" wizard page
WizardSelectComponents=Pilih Komponen
SelectComponentsDesc=Komponen apa wae sing arep dipasang?
SelectComponentsLabel2=Pilih komponen sing arep dipasang; busak centhang komponen sing ora arep dipasang. Klik Sabanjure yen wis siyap nerusake.
FullInstallation=Instalasi lengkap
; if possible don't translate 'Compact' as 'Minimal' (I mean 'Minimal' in your language)
CompactInstallation=Instalasi ringkes
CustomInstallation=Instalasi kustom
NoUninstallWarningTitle=Komponen Wis Ana
NoUninstallWarning=Pamasangan nemokake yen komponen-komponen iki wis kapasang ing komputer panjenengan:%n%n%1%n%nNgilangi pilihan komponen iki ora bakal mbusak komponen kasebut.%n%nApa panjenengan tetep arep nerusake?
ComponentSize1=%1 KB
ComponentSize2=%1 MB
ComponentsDiskSpaceGBLabel=Pilihan saiki butuh paling ora [gb] GB papan disk.
ComponentsDiskSpaceMBLabel=Pilihan saiki butuh paling ora [mb] MB papan disk.

; *** "Select Additional Tasks" wizard page
WizardSelectTasks=Pilih Tugas Tambahan
SelectTasksDesc=Tugas tambahan apa wae sing arep ditindakake?
SelectTasksLabel2=Pilih tugas tambahan sing panjenengan karepake ditindakake Pamasangan nalika masang [name], banjur klik Sabanjure.

; *** "Select Start Menu Folder" wizard page
WizardSelectProgramGroup=Pilih Folder Menu Start
SelectStartMenuFolderDesc=Ing ngendi Pamasangan kudu nyelehake trabasan program?
SelectStartMenuFolderLabel3=Pamasangan bakal nggawe trabasan program ing folder Menu Start ing ngisor iki.
SelectStartMenuFolderBrowseLabel=Kanggo nerusake, klik Sabanjure. Yen panjenengan pengin milih folder liya, klik Telusuri.
MustEnterGroupName=Panjenengan kudu nglebokake jeneng folder.
GroupNameTooLong=Jeneng folder utawa path kedawan.
InvalidGroupName=Jeneng folder ora sah.
BadGroupName=Jeneng folder ora kena ngemot aksara-aksara iki:%n%n%1
NoProgramGroupCheck2=&Aja nggawe folder Menu Start

; *** "Ready to Install" wizard page
WizardReady=Siyap Masang
ReadyLabel1=Pamasangan saiki wis siyap miwiti masang [name] ing komputer panjenengan.
ReadyLabel2a=Klik Pasang kanggo nerusake instalasi, utawa klik Bali yen panjenengan pengin mriksa utawa ngganti setelan.
ReadyLabel2b=Klik Pasang kanggo nerusake instalasi.
ReadyMemoUserInfo=Informasi pangguna:
ReadyMemoDir=Lokasi tujuan:
ReadyMemoType=Jinis instalasi:
ReadyMemoComponents=Komponen sing dipilih:
ReadyMemoGroup=Folder Menu Start:
ReadyMemoTasks=Tugas tambahan:

; *** TDownloadWizardPage wizard page and DownloadTemporaryFile
DownloadingLabel2=Ngundhuh berkas...
ButtonStopDownload=&Mandhegake undhuhan
StopDownload=Apa panjenengan yakin arep mandhegake undhuhan?
ErrorDownloadAborted=Undhuhan dibatalake
ErrorDownloadFailed=Undhuhan gagal: %1 %2
ErrorDownloadSizeFailed=Gagal njupuk ukuran: %1 %2
ErrorProgress=Kemajuan ora sah: %1 saka %2
ErrorFileSize=Ukuran berkas ora sah: samesthine %1, ditemokake %2

; *** TExtractionWizardPage wizard page and ExtractArchive
ExtractingLabel=Ngekstrak berkas...
ButtonStopExtraction=&Mandhegake ekstraksi
StopExtraction=Apa panjenengan yakin arep mandhegake ekstraksi?
ErrorExtractionAborted=Ekstraksi dibatalake
ErrorExtractionFailed=Ekstraksi gagal: %1

; *** Archive extraction failure details
ArchiveIncorrectPassword=Tembung sandhi ora bener
ArchiveIsCorrupted=Arsip rusak
ArchiveUnsupportedFormat=Format arsip ora didhukung

; *** "Preparing to Install" wizard page
WizardPreparing=Nyiapake Instalasi
PreparingDesc=Pamasangan lagi nyiapake masang [name] ing komputer panjenengan.
PreviousInstallNotCompleted=Instalasi/pambusakan program sadurunge durung rampung. Panjenengan kudu miwiti ulang komputer kanggo ngrampungake instalasi kasebut.%n%nSawise miwiti ulang komputer, lakokake Pamasangan maneh kanggo ngrampungake instalasi [name].
CannotContinue=Pamasangan ora bisa nerusake. Mangga klik Batal kanggo metu.
ApplicationsFound=Aplikasi-aplikasi iki nggunakake berkas sing kudu dianyari dening Pamasangan. Disaranake panjenengan ngidini Pamasangan nutup aplikasi-aplikasi iki kanthi otomatis.
ApplicationsFound2=Aplikasi-aplikasi iki nggunakake berkas sing kudu dianyari dening Pamasangan. Disaranake panjenengan ngidini Pamasangan nutup aplikasi-aplikasi iki kanthi otomatis. Sawise instalasi rampung, Pamasangan bakal nyoba miwiti ulang aplikasi-aplikasi kasebut.
CloseApplications=&Tutup aplikasi kanthi otomatis
DontCloseApplications=&Aja nutup aplikasi
ErrorCloseApplications=Pamasangan ora bisa nutup kabeh aplikasi kanthi otomatis. Disaranake panjenengan nutup kabeh aplikasi sing nggunakake berkas sing kudu dianyari dening Pamasangan sadurunge nerusake.
PrepareToInstallNeedsRestart=Pamasangan kudu miwiti ulang komputer panjenengan. Sawise miwiti ulang komputer, lakokake Pamasangan maneh kanggo ngrampungake instalasi [name].%n%nApa arep miwiti ulang saiki?

; *** "Installing" wizard page
WizardInstalling=Lagi Masang
InstallingLabel=Mangga ngenteni, Pamasangan lagi masang [name] ing komputer panjenengan.

; *** "Setup Completed" wizard page
FinishedHeadingLabel=Ngrampungake Wisaya Pamasangan [name]
FinishedLabelNoIcons=Pamasangan wis rampung masang [name] ing komputer panjenengan.
FinishedLabel=Pamasangan wis rampung masang [name] ing komputer panjenengan. Aplikasi bisa dibukak kanthi milih trabasan sing wis kapasang.
ClickFinish=Klik Rampung kanggo metu saka Pamasangan.
FinishedRestartLabel=Kanggo ngrampungake instalasi [name], Pamasangan kudu miwiti ulang komputer panjenengan. Apa arep miwiti ulang saiki?
FinishedRestartMessage=Kanggo ngrampungake instalasi [name], Pamasangan kudu miwiti ulang komputer panjenengan.%n%nApa arep miwiti ulang saiki?
ShowReadmeCheck=Iya, aku pengin ndeleng berkas README
YesRadio=&Iya, miwiti ulang komputer saiki
NoRadio=&Ora, aku bakal miwiti ulang komputer mengko
; used for example as 'Run MyProg.exe'
RunEntryExec=Lakokake %1
; used for example as 'View Readme.txt'
RunEntryShellExec=Deleng %1

; *** "Setup Needs the Next Disk" stuff
ChangeDiskTitle=Pamasangan Butuh Disk Sabanjure
SelectDiskLabel2=Mangga lebokake Disk %1 banjur klik OK.%n%nYen berkas ing disk iki bisa ditemokake ing folder liya saliyane sing katampilake ing ngisor iki, lebokake path sing bener utawa klik Telusuri.
PathLabel=&Path:
FileNotInDir2=Berkas "%1" ora bisa ditemokake ing "%2". Mangga lebokake disk sing bener utawa pilih folder liya.
SelectDirectoryLabel=Mangga temtokake lokasi disk sabanjure.

; *** Installation phase messages
SetupAborted=Pamasangan durung rampung.%n%nMangga dandani masalahe lan lakokake Pamasangan maneh.
AbortRetryIgnoreSelectAction=Pilih tumindak
AbortRetryIgnoreRetry=&Jajal maneh
AbortRetryIgnoreIgnore=&Lirwakake kasalahan lan terusake
AbortRetryIgnoreCancel=Batalake instalasi
RetryCancelSelectAction=Pilih tumindak
RetryCancelRetry=&Jajal maneh
RetryCancelCancel=Batal

; *** Installation status messages
StatusClosingApplications=Nutup aplikasi...
StatusCreateDirs=Nggawe direktori...
StatusExtractFiles=Ngekstrak berkas...
StatusDownloadFiles=Ngundhuh berkas...
StatusCreateIcons=Nggawe trabasan...
StatusCreateIniEntries=Nggawe entri INI...
StatusCreateRegistryEntries=Nggawe entri registri...
StatusRegisterFiles=Ndhaptar berkas...
StatusSavingUninstall=Nyimpen informasi pambusakan...
StatusRunProgram=Ngrampungake instalasi...
StatusRestartingApplications=Miwiti ulang aplikasi...
StatusRollback=Mbalekake owah-owahan...

; *** Misc. errors
ErrorInternal2=Kasalahan internal: %1
ErrorFunctionFailedNoCode=%1 gagal
ErrorFunctionFailed=%1 gagal; kode %2
ErrorFunctionFailedWithMessage=%1 gagal; kode %2.%n%3
ErrorExecutingProgram=Ora bisa nglakokake berkas:%n%1

; *** Registry errors
ErrorRegOpenKey=Kasalahan nalika mbukak kunci registri:%n%1\%2
ErrorRegCreateKey=Kasalahan nalika nggawe kunci registri:%n%1\%2
ErrorRegWriteKey=Kasalahan nalika nulis menyang kunci registri:%n%1\%2

; *** INI errors
ErrorIniEntry=Kasalahan nalika nggawe entri INI ing berkas "%1".

; *** File copying errors
FileAbortRetryIgnoreSkipNotRecommended=&Liwati berkas iki (ora disaranake)
FileAbortRetryIgnoreIgnoreNotRecommended=&Lirwakake kasalahan lan terusake (ora disaranake)
SourceIsCorrupted=Berkas sumber rusak
SourceDoesntExist=Berkas sumber "%1" ora ana
SourceVerificationFailed=Verifikasi berkas sumber gagal: %1
VerificationSignatureDoesntExist=Berkas tandha tangan "%1" ora ana
VerificationSignatureInvalid=Berkas tandha tangan "%1" ora sah
VerificationKeyNotFound=Berkas tandha tangan "%1" nggunakake kunci sing ora dikenal
VerificationFileNameIncorrect=Jeneng berkas ora bener
VerificationFileTagIncorrect=Tag berkas ora bener
VerificationFileSizeIncorrect=Ukuran berkas ora bener
VerificationFileHashIncorrect=Hash berkas ora bener
ExistingFileReadOnly2=Berkas sing wis ana ora bisa diganti amarga ditandhani mung-waca.
ExistingFileReadOnlyRetry=&Busak atribut mung-waca lan jajal maneh
ExistingFileReadOnlyKeepExisting=&Tetepake berkas sing wis ana
ErrorReadingExistingDest=Ana kasalahan nalika nyoba maca berkas sing wis ana:
FileExistsSelectAction=Pilih tumindak
FileExists2=Berkas wis ana.
FileExistsOverwriteExisting=&Timpa berkas sing wis ana
FileExistsKeepExisting=T&etepake berkas sing wis ana
FileExistsOverwriteOrKeepAll=&Lakoni iki kanggo konflik sabanjure
ExistingFileNewerSelectAction=Pilih tumindak
ExistingFileNewer2=Berkas sing wis ana luwih anyar tinimbang sing arep dipasang dening Pamasangan.
ExistingFileNewerOverwriteExisting=&Timpa berkas sing wis ana
ExistingFileNewerKeepExisting=T&etepake berkas sing wis ana (disaranake)
ExistingFileNewerOverwriteOrKeepAll=&Lakoni iki kanggo konflik sabanjure
ErrorChangingAttr=Ana kasalahan nalika nyoba ngganti atribut berkas sing wis ana:
ErrorCreatingTemp=Ana kasalahan nalika nyoba nggawe berkas ing direktori tujuan:
ErrorReadingSource=Ana kasalahan nalika nyoba maca berkas sumber:
ErrorCopying=Ana kasalahan nalika nyoba nyalin berkas:
ErrorDownloading=Ana kasalahan nalika nyoba ngundhuh berkas:
ErrorExtracting=Ana kasalahan nalika nyoba ngekstrak arsip:
ErrorReplacingExistingFile=Ana kasalahan nalika nyoba ngganti berkas sing wis ana:
ErrorRestartReplace=RestartReplace gagal:
ErrorRenamingTemp=Ana kasalahan nalika nyoba ngganti jeneng berkas ing direktori tujuan:
ErrorRegisterServer=Ora bisa ndhaptar DLL/OCX: %1
ErrorRegSvr32Failed=RegSvr32 gagal kanthi kode metu %1
ErrorRegisterTypeLib=Ora bisa ndhaptar type library: %1

; *** Uninstall display name markings
; used for example as 'My Program (32-bit)'
UninstallDisplayNameMark=%1 (%2)
; used for example as 'My Program (32-bit, All users)'
UninstallDisplayNameMarks=%1 (%2, %3)
UninstallDisplayNameMark32Bit=32-bit
UninstallDisplayNameMark64Bit=64-bit
UninstallDisplayNameMarkAllUsers=Kabeh pangguna
UninstallDisplayNameMarkCurrentUser=Pangguna saiki

; *** Post-installation errors
ErrorOpeningReadme=Ana kasalahan nalika nyoba mbukak berkas README.
ErrorRestartingComputer=Pamasangan ora bisa miwiti ulang komputer. Mangga tindakake kanthi manual.

; *** Uninstaller messages
UninstallNotFound=Berkas "%1" ora ana. Ora bisa mbusak instalasi.
UninstallOpenError=Berkas "%1" ora bisa dibukak. Ora bisa mbusak instalasi
UninstallUnsupportedVer=Berkas log pambusakan "%1" nganggo format sing ora dikenali dening versi pambusak iki. Ora bisa mbusak instalasi
UninstallUnknownEntry=Ditemokake entri sing ora dikenal (%1) ing log pambusakan
ConfirmUninstall=Apa panjenengan yakin arep mbusak %1 lan kabeh komponene nganti resik?
UninstallOnlyOnWin64=Instalasi iki mung bisa dibusak ing Windows 64-bit.
OnlyAdminCanUninstall=Instalasi iki mung bisa dibusak dening pangguna sing duwe hak administrator.
UninstallStatusLabel=Mangga ngenteni, %1 lagi dibusak saka komputer panjenengan.
UninstalledAll=%1 wis kasil dibusak saka komputer panjenengan.
UninstalledMost=Pambusakan %1 rampung.%n%nSawetara elemen ora bisa dibusak. Elemen kasebut bisa dibusak kanthi manual.
UninstalledAndNeedsRestart=Kanggo ngrampungake pambusakan %1, komputer panjenengan kudu diwiwiti ulang.%n%nApa arep miwiti ulang saiki?
UninstallDataCorrupted=Berkas "%1" rusak. Ora bisa mbusak instalasi

; *** Uninstallation phase messages
ConfirmDeleteSharedFileTitle=Busak Berkas Bebarengan?
ConfirmDeleteSharedFile2=Sistem nuduhake yen berkas bebarengan iki wis ora dienggo program apa wae. Apa panjenengan pengin Pambusakan mbusak berkas bebarengan iki?%n%nYen isih ana program sing nggunakake berkas iki lan berkase dibusak, program-program kasebut bisa ora mlaku kanthi bener. Yen ora yakin, pilih Ora. Ninggalake berkas iki ing sistem ora bakal nyebabake masalah.
SharedFileNameLabel=Jeneng berkas:
SharedFileLocationLabel=Lokasi:
WizardUninstalling=Status Pambusakan
StatusUninstalling=Mbusak %1...

; *** Shutdown block reasons
ShutdownBlockReasonInstallingApp=Masang %1.
ShutdownBlockReasonUninstallingApp=Mbusak %1.

; The custom messages below aren't used by Setup itself, but if you make
; use of them in your scripts, you'll want to translate them.

[CustomMessages]

NameAndVersion=%1 versi %2
AdditionalIcons=Trabasan tambahan:
CreateDesktopIcon=Gawe trabasan ing &desktop
CreateQuickLaunchIcon=Gawe trabasan &Quick Launch
ProgramOnTheWeb=%1 ing Web
UninstallProgram=Busak %1
LaunchProgram=Bukak %1
AssocFileExtension=&Gandhengake %1 karo ekstensi berkas %2
AssocingFileExtension=Nggandhengake %1 karo ekstensi berkas %2...
AutoStartProgramGroupDescription=Wiwitan:
AutoStartProgram=Wiwiti %1 kanthi otomatis
AddonHostProgramNotFound=%1 ora bisa ditemokake ing folder sing panjenengan pilih.%n%nApa panjenengan tetep arep nerusake?
