; *** Inno Setup version 6.5.0+ Swahili messages ***
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
LanguageName=Kiswahili
LanguageID=$0441
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
SetupAppTitle=Usanidi
SetupWindowTitle=Usanidi - %1
UninstallAppTitle=Kuondoa
UninstallAppFullTitle=Kuondoa %1

; *** Misc. common
InformationTitle=Taarifa
ConfirmTitle=Thibitisha
ErrorTitle=Hitilafu

; *** SetupLdr messages
SetupLdrStartupMessage=Hii itasakinisha %1. Je, ungependa kuendelea?
LdrCannotCreateTemp=Imeshindwa kuunda faili la muda. Usanidi umesitishwa
LdrCannotExecTemp=Imeshindwa kuendesha faili katika saraka ya muda. Usanidi umesitishwa
HelpTextNote=

; *** Startup error messages
LastErrorMessage=%1.%n%nHitilafu %2: %3
SetupFileMissing=Faili %1 halipo katika saraka ya usakinishaji. Tafadhali rekebisha tatizo au pata nakala mpya ya programu.
SetupFileCorrupt=Faili za usanidi zimeharibika. Tafadhali pata nakala mpya ya programu.
SetupFileCorruptOrWrongVer=Faili za usanidi zimeharibika, au haziendani na toleo hili la Usanidi. Tafadhali rekebisha tatizo au pata nakala mpya ya programu.
InvalidParameter=Kigezo batili kilipitishwa kwenye mstari wa amri:%n%n%1
SetupAlreadyRunning=Usanidi tayari unaendeshwa.
WindowsVersionNotSupported=Programu hii haiauni toleo la Windows linaloendeshwa na kompyuta yako.
WindowsServicePackRequired=Programu hii inahitaji %1 Service Pack %2 au jipya zaidi.
NotOnThisPlatform=Programu hii haitaendeshwa kwenye %1.
OnlyOnThisPlatform=Programu hii lazima iendeshwe kwenye %1.
OnlyOnTheseArchitectures=Programu hii inaweza kusakinishwa tu kwenye matoleo ya Windows yaliyoundwa kwa usanifu ufuatao wa kichakataji:%n%n%1
WinVersionTooLowError=Programu hii inahitaji %1 toleo %2 au jipya zaidi.
WinVersionTooHighError=Programu hii haiwezi kusakinishwa kwenye %1 toleo %2 au jipya zaidi.
AdminPrivilegesRequired=Lazima uwe umeingia kama msimamizi unaposakinisha programu hii.
PowerUserPrivilegesRequired=Lazima uwe umeingia kama msimamizi au mwanachama wa kikundi cha Power Users unaposakinisha programu hii.
SetupAppRunningError=Usanidi umegundua kuwa %1 inaendeshwa kwa sasa.%n%nTafadhali funga madirisha yake yote sasa, kisha bofya Sawa ili kuendelea, au Ghairi ili kuondoka.
UninstallAppRunningError=Kiondoaji kimegundua kuwa %1 inaendeshwa kwa sasa.%n%nTafadhali funga madirisha yake yote sasa, kisha bofya Sawa ili kuendelea, au Ghairi ili kuondoka.

; *** Startup questions
PrivilegesRequiredOverrideTitle=Chagua Hali ya Usakinishaji
PrivilegesRequiredOverrideInstruction=Chagua hali ya usakinishaji
PrivilegesRequiredOverrideText1=%1 inaweza kusakinishwa kwa watumiaji wote (inahitaji haki za msimamizi), au kwa ajili yako tu.
PrivilegesRequiredOverrideText2=%1 inaweza kusakinishwa kwa ajili yako tu, au kwa watumiaji wote (inahitaji haki za msimamizi).
PrivilegesRequiredOverrideAllUsers=Sakinisha kwa &watumiaji wote
PrivilegesRequiredOverrideAllUsersRecommended=Sakinisha kwa &watumiaji wote (inapendekezwa)
PrivilegesRequiredOverrideCurrentUser=Sakinisha kwa ajili yangu &tu
PrivilegesRequiredOverrideCurrentUserRecommended=Sakinisha kwa ajili yangu &tu (inapendekezwa)

; *** Misc. errors
ErrorCreatingDir=Usanidi umeshindwa kuunda saraka "%1"
ErrorTooManyFilesInDir=Imeshindwa kuunda faili katika saraka "%1" kwa sababu ina faili nyingi mno

; *** Setup common messages
ExitSetupTitle=Ondoka kwenye Usanidi
ExitSetupMessage=Usanidi haujakamilika. Ukiondoka sasa, programu haitasakinishwa.%n%nUnaweza kuendesha Usanidi tena wakati mwingine ili kukamilisha usakinishaji.%n%nUondoke kwenye Usanidi?
AboutSetupMenuItem=&Kuhusu Usanidi...
AboutSetupTitle=Kuhusu Usanidi
AboutSetupMessage=%1 toleo %2%n%3%n%nUkurasa wa nyumbani wa %1:%n%4
AboutSetupNote=
TranslatorNote=

; *** Buttons
ButtonBack=< &Nyuma
ButtonNext=&Mbele >
ButtonInstall=&Sakinisha
ButtonOK=Sawa
ButtonCancel=Ghairi
ButtonYes=&Ndiyo
ButtonYesToAll=Ndiyo kwa &Zote
ButtonNo=&Hapana
ButtonNoToAll=H&apana kwa Zote
ButtonFinish=&Maliza
ButtonBrowse=&Vinjari...
ButtonWizardBrowse=V&injari...
ButtonNewFolder=&Unda Folda Mpya

; *** "Select Language" dialog messages
SelectLanguageTitle=Chagua Lugha ya Usanidi
SelectLanguageLabel=Chagua lugha ya kutumia wakati wa usakinishaji.

; *** Common wizard text
ClickNext=Bofya Mbele ili kuendelea, au Ghairi ili kuondoka kwenye Usanidi.
BeveledLabel=
BrowseDialogTitle=Vinjari Folda
BrowseDialogLabel=Chagua folda katika orodha iliyo hapa chini, kisha bofya Sawa.
NewFolderName=Folda Mpya

; *** "Welcome" wizard page
WelcomeLabel1=Karibu kwenye Usanidi wa [name]
WelcomeLabel2=Hii itasakinisha [name/ver] kwenye kompyuta yako.%n%nInapendekezwa ufunge programu nyingine zote kabla ya kuendelea.

; *** "Password" wizard page
WizardPassword=Nenosiri
PasswordLabel1=Usakinishaji huu umelindwa kwa nenosiri.
PasswordLabel3=Tafadhali weka nenosiri, kisha bofya Mbele ili kuendelea. Nenosiri hutofautisha herufi kubwa na ndogo.
PasswordEditLabel=&Nenosiri:
IncorrectPassword=Nenosiri uliloweka si sahihi. Tafadhali jaribu tena.

; *** "License Agreement" wizard page
WizardLicense=Makubaliano ya Leseni
LicenseLabel=Tafadhali soma taarifa muhimu ifuatayo kabla ya kuendelea.
LicenseLabel3=Tafadhali soma Makubaliano ya Leseni yafuatayo. Lazima ukubali masharti ya makubaliano haya kabla ya kuendelea na usakinishaji.
LicenseAccepted=&Ninakubali makubaliano
LicenseNotAccepted=Si&kubali makubaliano

; *** "Information" wizard pages
WizardInfoBefore=Taarifa
InfoBeforeLabel=Tafadhali soma taarifa muhimu ifuatayo kabla ya kuendelea.
InfoBeforeClickLabel=Ukiwa tayari kuendelea na Usanidi, bofya Mbele.
WizardInfoAfter=Taarifa
InfoAfterLabel=Tafadhali soma taarifa muhimu ifuatayo kabla ya kuendelea.
InfoAfterClickLabel=Ukiwa tayari kuendelea na Usanidi, bofya Mbele.

; *** "User Information" wizard page
WizardUserInfo=Taarifa za Mtumiaji
UserInfoDesc=Tafadhali weka taarifa zako.
UserInfoName=&Jina la Mtumiaji:
UserInfoOrg=&Shirika:
UserInfoSerial=Nambari ya &Mfululizo:
UserInfoNameRequired=Lazima uweke jina.

; *** "Select Destination Location" wizard page
WizardSelectDir=Chagua Mahali pa Kusakinisha
SelectDirDesc=[name] isakinishwe wapi?
SelectDirLabel3=Usanidi utasakinisha [name] kwenye folda ifuatayo.
SelectDirBrowseLabel=Ili kuendelea, bofya Mbele. Ikiwa ungependa kuchagua folda tofauti, bofya Vinjari.
DiskSpaceGBLabel=Angalau GB [gb] za nafasi tupu ya diski zinahitajika.
DiskSpaceMBLabel=Angalau MB [mb] za nafasi tupu ya diski zinahitajika.
CannotInstallToNetworkDrive=Usanidi hauwezi kusakinisha kwenye hifadhi ya mtandao.
CannotInstallToUNCPath=Usanidi hauwezi kusakinisha kwenye njia ya UNC.
InvalidPath=Lazima uweke njia kamili yenye herufi ya hifadhi; kwa mfano:%n%nC:\APP%n%nau njia ya UNC katika muundo:%n%n\\server\share
InvalidDrive=Hifadhi au folda ya UNC uliyochagua haipo au haifikiki. Tafadhali chagua nyingine.
DiskSpaceWarningTitle=Nafasi ya Diski Haitoshi
DiskSpaceWarning=Usanidi unahitaji angalau KB %1 za nafasi tupu ili kusakinisha, lakini hifadhi iliyochaguliwa ina KB %2 tu zinazopatikana.%n%nJe, unataka kuendelea hata hivyo?
DirNameTooLong=Jina la folda au njia ni refu mno.
InvalidDirName=Jina la folda si sahihi.
BadDirName32=Majina ya folda hayawezi kuwa na herufi yoyote kati ya zifuatazo:%n%n%1
DirExistsTitle=Folda Ipo
DirExists=Folda:%n%n%1%n%ntayari ipo. Je, ungependa kusakinisha kwenye folda hiyo hata hivyo?
DirDoesntExistTitle=Folda Haipo
DirDoesntExist=Folda:%n%n%1%n%nhaipo. Je, ungependa folda iundwe?

; *** "Select Components" wizard page
WizardSelectComponents=Chagua Vipengele
SelectComponentsDesc=Ni vipengele vipi vinapaswa kusakinishwa?
SelectComponentsLabel2=Chagua vipengele unavyotaka kusakinisha; ondoa alama kwenye vipengele usivyotaka kusakinisha. Bofya Mbele ukiwa tayari kuendelea.
FullInstallation=Usakinishaji kamili
; if possible don't translate 'Compact' as 'Minimal' (I mean 'Minimal' in your language)
CompactInstallation=Usakinishaji mfupi
CustomInstallation=Usakinishaji maalum
NoUninstallWarningTitle=Vipengele Vipo
NoUninstallWarning=Usanidi umegundua kuwa vipengele vifuatavyo tayari vimesakinishwa kwenye kompyuta yako:%n%n%1%n%nKuondoa uteuzi wa vipengele hivi hakutaviondoa kwenye kompyuta.%n%nJe, ungependa kuendelea hata hivyo?
ComponentSize1=%1 KB
ComponentSize2=%1 MB
ComponentsDiskSpaceGBLabel=Uteuzi wa sasa unahitaji angalau GB [gb] za nafasi ya diski.
ComponentsDiskSpaceMBLabel=Uteuzi wa sasa unahitaji angalau MB [mb] za nafasi ya diski.

; *** "Select Additional Tasks" wizard page
WizardSelectTasks=Chagua Kazi za Ziada
SelectTasksDesc=Ni kazi zipi za ziada zinapaswa kutekelezwa?
SelectTasksLabel2=Chagua kazi za ziada ambazo ungependa Usanidi utekeleze wakati wa kusakinisha [name], kisha bofya Mbele.

; *** "Select Start Menu Folder" wizard page
WizardSelectProgramGroup=Chagua Folda ya Menyu ya Anza
SelectStartMenuFolderDesc=Usanidi uweke wapi njia za mkato za programu?
SelectStartMenuFolderLabel3=Usanidi utaunda njia za mkato za programu katika folda ifuatayo ya Menyu ya Anza.
SelectStartMenuFolderBrowseLabel=Ili kuendelea, bofya Mbele. Ikiwa ungependa kuchagua folda tofauti, bofya Vinjari.
MustEnterGroupName=Lazima uweke jina la folda.
GroupNameTooLong=Jina la folda au njia ni refu mno.
InvalidGroupName=Jina la folda si sahihi.
BadGroupName=Jina la folda haliwezi kuwa na herufi yoyote kati ya zifuatazo:%n%n%1
NoProgramGroupCheck2=&Usiunde folda ya Menyu ya Anza

; *** "Ready to Install" wizard page
WizardReady=Tayari Kusakinisha
ReadyLabel1=Usanidi sasa uko tayari kuanza kusakinisha [name] kwenye kompyuta yako.
ReadyLabel2a=Bofya Sakinisha ili kuendelea na usakinishaji, au bofya Nyuma ikiwa unataka kukagua au kubadilisha mipangilio yoyote.
ReadyLabel2b=Bofya Sakinisha ili kuendelea na usakinishaji.
ReadyMemoUserInfo=Taarifa za mtumiaji:
ReadyMemoDir=Mahali pa kusakinisha:
ReadyMemoType=Aina ya usanidi:
ReadyMemoComponents=Vipengele vilivyochaguliwa:
ReadyMemoGroup=Folda ya Menyu ya Anza:
ReadyMemoTasks=Kazi za ziada:

; *** TDownloadWizardPage wizard page and DownloadTemporaryFile
DownloadingLabel2=Inapakua faili...
ButtonStopDownload=&Simamisha upakuaji
StopDownload=Una uhakika unataka kusimamisha upakuaji?
ErrorDownloadAborted=Upakuaji umesitishwa
ErrorDownloadFailed=Upakuaji umeshindwa: %1 %2
ErrorDownloadSizeFailed=Kupata ukubwa kumeshindwa: %1 %2
ErrorProgress=Maendeleo batili: %1 kati ya %2
ErrorFileSize=Ukubwa wa faili si sahihi: ulitarajiwa %1, umepatikana %2

; *** TExtractionWizardPage wizard page and ExtractArchive
ExtractingLabel=Inatoa faili...
ButtonStopExtraction=&Simamisha utoaji
StopExtraction=Una uhakika unataka kusimamisha utoaji?
ErrorExtractionAborted=Utoaji umesitishwa
ErrorExtractionFailed=Utoaji umeshindwa: %1

; *** Archive extraction failure details
ArchiveIncorrectPassword=Nenosiri si sahihi
ArchiveIsCorrupted=Faili la kumbukumbu limeharibika
ArchiveUnsupportedFormat=Muundo wa faili la kumbukumbu hauauniwi

; *** "Preparing to Install" wizard page
WizardPreparing=Inajiandaa Kusakinisha
PreparingDesc=Usanidi unajiandaa kusakinisha [name] kwenye kompyuta yako.
PreviousInstallNotCompleted=Usakinishaji/uondoaji wa programu iliyotangulia haukukamilika. Utahitaji kuwasha upya kompyuta yako ili kukamilisha usakinishaji huo.%n%nBaada ya kuwasha upya kompyuta yako, endesha Usanidi tena ili kukamilisha usakinishaji wa [name].
CannotContinue=Usanidi hauwezi kuendelea. Tafadhali bofya Ghairi ili kuondoka.
ApplicationsFound=Programu zifuatazo zinatumia faili ambazo Usanidi unahitaji kusasisha. Inapendekezwa uruhusu Usanidi kufunga programu hizi kiotomatiki.
ApplicationsFound2=Programu zifuatazo zinatumia faili ambazo Usanidi unahitaji kusasisha. Inapendekezwa uruhusu Usanidi kufunga programu hizi kiotomatiki. Baada ya usakinishaji kukamilika, Usanidi utajaribu kuwasha upya programu hizo.
CloseApplications=&Funga programu kiotomatiki
DontCloseApplications=&Usifunge programu
ErrorCloseApplications=Usanidi umeshindwa kufunga programu zote kiotomatiki. Inapendekezwa ufunge programu zote zinazotumia faili ambazo Usanidi unahitaji kusasisha kabla ya kuendelea.
PrepareToInstallNeedsRestart=Usanidi lazima uwashe upya kompyuta yako. Baada ya kuwasha upya kompyuta yako, endesha Usanidi tena ili kukamilisha usakinishaji wa [name].%n%nJe, ungependa kuwasha upya sasa?

; *** "Installing" wizard page
WizardInstalling=Inasakinisha
InstallingLabel=Tafadhali subiri Usanidi unaposakinisha [name] kwenye kompyuta yako.

; *** "Setup Completed" wizard page
FinishedHeadingLabel=Kukamilisha Usanidi wa [name]
FinishedLabelNoIcons=Usanidi umemaliza kusakinisha [name] kwenye kompyuta yako.
FinishedLabel=Usanidi umemaliza kusakinisha [name] kwenye kompyuta yako. Programu inaweza kuzinduliwa kwa kuchagua njia za mkato zilizosakinishwa.
ClickFinish=Bofya Maliza ili kuondoka kwenye Usanidi.
FinishedRestartLabel=Ili kukamilisha usakinishaji wa [name], Usanidi lazima uwashe upya kompyuta yako. Je, ungependa kuwasha upya sasa?
FinishedRestartMessage=Ili kukamilisha usakinishaji wa [name], Usanidi lazima uwashe upya kompyuta yako.%n%nJe, ungependa kuwasha upya sasa?
ShowReadmeCheck=Ndiyo, ningependa kuona faili la README
YesRadio=&Ndiyo, washa upya kompyuta sasa
NoRadio=&Hapana, nitawasha upya kompyuta baadaye
; used for example as 'Run MyProg.exe'
RunEntryExec=Endesha %1
; used for example as 'View Readme.txt'
RunEntryShellExec=Tazama %1

; *** "Setup Needs the Next Disk" stuff
ChangeDiskTitle=Usanidi Unahitaji Diski Inayofuata
SelectDiskLabel2=Tafadhali ingiza Diski %1 kisha bofya Sawa.%n%nIkiwa faili zilizo kwenye diski hii zinapatikana katika folda nyingine isipokuwa iliyoonyeshwa hapa chini, weka njia sahihi au bofya Vinjari.
PathLabel=&Njia:
FileNotInDir2=Faili "%1" halikupatikana katika "%2". Tafadhali ingiza diski sahihi au chagua folda nyingine.
SelectDirectoryLabel=Tafadhali bainisha mahali pa diski inayofuata.

; *** Installation phase messages
SetupAborted=Usanidi haukukamilika.%n%nTafadhali rekebisha tatizo kisha uendeshe Usanidi tena.
AbortRetryIgnoreSelectAction=Chagua kitendo
AbortRetryIgnoreRetry=&Jaribu tena
AbortRetryIgnoreIgnore=&Puuza hitilafu na uendelee
AbortRetryIgnoreCancel=Ghairi usakinishaji
RetryCancelSelectAction=Chagua kitendo
RetryCancelRetry=&Jaribu tena
RetryCancelCancel=Ghairi

; *** Installation status messages
StatusClosingApplications=Inafunga programu...
StatusCreateDirs=Inaunda saraka...
StatusExtractFiles=Inatoa faili...
StatusDownloadFiles=Inapakua faili...
StatusCreateIcons=Inaunda njia za mkato...
StatusCreateIniEntries=Inaunda maingizo ya INI...
StatusCreateRegistryEntries=Inaunda maingizo ya sajili...
StatusRegisterFiles=Inasajili faili...
StatusSavingUninstall=Inahifadhi taarifa za kuondoa...
StatusRunProgram=Inakamilisha usakinishaji...
StatusRestartingApplications=Inawasha upya programu...
StatusRollback=Inarejesha mabadiliko...

; *** Misc. errors
ErrorInternal2=Hitilafu ya ndani: %1
ErrorFunctionFailedNoCode=%1 imeshindwa
ErrorFunctionFailed=%1 imeshindwa; msimbo %2
ErrorFunctionFailedWithMessage=%1 imeshindwa; msimbo %2.%n%3
ErrorExecutingProgram=Imeshindwa kuendesha faili:%n%1

; *** Registry errors
ErrorRegOpenKey=Hitilafu wakati wa kufungua ufunguo wa sajili:%n%1\%2
ErrorRegCreateKey=Hitilafu wakati wa kuunda ufunguo wa sajili:%n%1\%2
ErrorRegWriteKey=Hitilafu wakati wa kuandika kwenye ufunguo wa sajili:%n%1\%2

; *** INI errors
ErrorIniEntry=Hitilafu wakati wa kuunda ingizo la INI katika faili "%1".

; *** File copying errors
FileAbortRetryIgnoreSkipNotRecommended=&Ruka faili hili (haipendekezwi)
FileAbortRetryIgnoreIgnoreNotRecommended=&Puuza hitilafu na uendelee (haipendekezwi)
SourceIsCorrupted=Faili chanzo limeharibika
SourceDoesntExist=Faili chanzo "%1" halipo
SourceVerificationFailed=Uthibitishaji wa faili chanzo umeshindwa: %1
VerificationSignatureDoesntExist=Faili la sahihi "%1" halipo
VerificationSignatureInvalid=Faili la sahihi "%1" si halali
VerificationKeyNotFound=Faili la sahihi "%1" linatumia ufunguo usiojulikana
VerificationFileNameIncorrect=Jina la faili si sahihi
VerificationFileTagIncorrect=Lebo ya faili si sahihi
VerificationFileSizeIncorrect=Ukubwa wa faili si sahihi
VerificationFileHashIncorrect=Hashi ya faili si sahihi
ExistingFileReadOnly2=Faili lililopo halikuweza kubadilishwa kwa sababu limewekwa kuwa la kusoma tu.
ExistingFileReadOnlyRetry=&Ondoa sifa ya kusoma tu kisha ujaribu tena
ExistingFileReadOnlyKeepExisting=&Baki na faili lililopo
ErrorReadingExistingDest=Hitilafu ilitokea wakati wa kujaribu kusoma faili lililopo:
FileExistsSelectAction=Chagua kitendo
FileExists2=Faili tayari lipo.
FileExistsOverwriteExisting=&Andika juu ya faili lililopo
FileExistsKeepExisting=&Baki na faili lililopo
FileExistsOverwriteOrKeepAll=&Fanya hivi kwa migongano inayofuata
ExistingFileNewerSelectAction=Chagua kitendo
ExistingFileNewer2=Faili lililopo ni jipya kuliko lile ambalo Usanidi unajaribu kusakinisha.
ExistingFileNewerOverwriteExisting=&Andika juu ya faili lililopo
ExistingFileNewerKeepExisting=&Baki na faili lililopo (inapendekezwa)
ExistingFileNewerOverwriteOrKeepAll=&Fanya hivi kwa migongano inayofuata
ErrorChangingAttr=Hitilafu ilitokea wakati wa kujaribu kubadilisha sifa za faili lililopo:
ErrorCreatingTemp=Hitilafu ilitokea wakati wa kujaribu kuunda faili katika saraka lengwa:
ErrorReadingSource=Hitilafu ilitokea wakati wa kujaribu kusoma faili chanzo:
ErrorCopying=Hitilafu ilitokea wakati wa kujaribu kunakili faili:
ErrorDownloading=Hitilafu ilitokea wakati wa kujaribu kupakua faili:
ErrorExtracting=Hitilafu ilitokea wakati wa kujaribu kutoa faili la kumbukumbu:
ErrorReplacingExistingFile=Hitilafu ilitokea wakati wa kujaribu kubadilisha faili lililopo:
ErrorRestartReplace=RestartReplace imeshindwa:
ErrorRenamingTemp=Hitilafu ilitokea wakati wa kujaribu kubadilisha jina la faili katika saraka lengwa:
ErrorRegisterServer=Imeshindwa kusajili DLL/OCX: %1
ErrorRegSvr32Failed=RegSvr32 imeshindwa na msimbo wa kutoka %1
ErrorRegisterTypeLib=Imeshindwa kusajili maktaba ya aina: %1

; *** Uninstall display name markings
; used for example as 'My Program (32-bit)'
UninstallDisplayNameMark=%1 (%2)
; used for example as 'My Program (32-bit, All users)'
UninstallDisplayNameMarks=%1 (%2, %3)
UninstallDisplayNameMark32Bit=biti 32
UninstallDisplayNameMark64Bit=biti 64
UninstallDisplayNameMarkAllUsers=Watumiaji wote
UninstallDisplayNameMarkCurrentUser=Mtumiaji wa sasa

; *** Post-installation errors
ErrorOpeningReadme=Hitilafu ilitokea wakati wa kujaribu kufungua faili la README.
ErrorRestartingComputer=Usanidi umeshindwa kuwasha upya kompyuta. Tafadhali fanya hivyo wewe mwenyewe.

; *** Uninstaller messages
UninstallNotFound=Faili "%1" halipo. Haiwezekani kuondoa.
UninstallOpenError=Faili "%1" halikuweza kufunguliwa. Haiwezekani kuondoa
UninstallUnsupportedVer=Faili la kumbukumbu ya kuondoa "%1" liko katika muundo usiotambuliwa na toleo hili la kiondoaji. Haiwezekani kuondoa
UninstallUnknownEntry=Ingizo lisilojulikana (%1) limepatikana katika kumbukumbu ya kuondoa
ConfirmUninstall=Una uhakika unataka kuondoa kabisa %1 na vipengele vyake vyote?
UninstallOnlyOnWin64=Usakinishaji huu unaweza kuondolewa tu kwenye Windows ya biti 64.
OnlyAdminCanUninstall=Usakinishaji huu unaweza kuondolewa tu na mtumiaji mwenye haki za msimamizi.
UninstallStatusLabel=Tafadhali subiri %1 inapoondolewa kwenye kompyuta yako.
UninstalledAll=%1 imeondolewa kwa mafanikio kwenye kompyuta yako.
UninstalledMost=Kuondoa %1 kumekamilika.%n%nBaadhi ya vipengele havikuweza kuondolewa. Unaweza kuviondoa wewe mwenyewe.
UninstalledAndNeedsRestart=Ili kukamilisha kuondoa %1, kompyuta yako lazima iwashwe upya.%n%nJe, ungependa kuwasha upya sasa?
UninstallDataCorrupted=Faili "%1" limeharibika. Haiwezekani kuondoa

; *** Uninstallation phase messages
ConfirmDeleteSharedFileTitle=Ondoa Faili Linaloshirikiwa?
ConfirmDeleteSharedFile2=Mfumo unaonyesha kuwa faili lifuatalo linaloshirikiwa halitumiki tena na programu yoyote. Je, ungependa kiondoaji kiondoe faili hili linaloshirikiwa?%n%nIkiwa programu yoyote bado inatumia faili hili na likaondolewa, programu hizo huenda zisifanye kazi ipasavyo. Ikiwa huna uhakika, chagua Hapana. Kuacha faili kwenye mfumo wako hakutasababisha madhara yoyote.
SharedFileNameLabel=Jina la faili:
SharedFileLocationLabel=Mahali:
WizardUninstalling=Hali ya Kuondoa
StatusUninstalling=Inaondoa %1...

; *** Shutdown block reasons
ShutdownBlockReasonInstallingApp=Inasakinisha %1.
ShutdownBlockReasonUninstallingApp=Inaondoa %1.

; The custom messages below aren't used by Setup itself, but if you make
; use of them in your scripts, you'll want to translate them.

[CustomMessages]

NameAndVersion=%1 toleo %2
AdditionalIcons=Njia za mkato za ziada:
CreateDesktopIcon=Unda njia ya mkato kwenye &eneo-kazi
CreateQuickLaunchIcon=Unda njia ya mkato ya &Quick Launch
ProgramOnTheWeb=%1 kwenye Wavuti
UninstallProgram=Ondoa %1
LaunchProgram=Zindua %1
AssocFileExtension=&Husisha %1 na kiendelezi cha faili %2
AssocingFileExtension=Inahusisha %1 na kiendelezi cha faili %2...
AutoStartProgramGroupDescription=Kuanzisha:
AutoStartProgram=Anzisha %1 kiotomatiki
AddonHostProgramNotFound=%1 haikupatikana katika folda uliyochagua.%n%nJe, unataka kuendelea hata hivyo?
