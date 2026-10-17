; ============================================================================
;  scourge.iss - Scourge Windows 安装包（Inno Setup 6）
;
;  用法（在 packaging/ 目录下）：
;    ISCC scourge.iss                       -> x64/x86 安装包
;    ISCC /DARCH=arm64 scourge.iss          -> arm64 安装包
;
;  输入：staging\（git archive 干净树 + build-rel\ 三件套，打包前组装）
;  输出：..\release\installer\<名>-setup.exe
;  行为：可选加入系统 PATH（安装/卸载对称，ChangesEnvironment 广播刷新）
; ============================================================================

#define Version "0.1.0"
#define Staging "staging"
#define OutDir "..\release\installer"

; ARCH 默认 x64-x86；arm64 包用 ISCC /DARCH=arm64 传入
#ifndef ARCH
#define ARCH "x64-x86"
#endif

#if ARCH == "arm64"
#define OutName "Scourge-" + Version + "-win-arm64-setup"
#else
#define OutName "Scourge-" + Version + "-win-x64-x86-setup"
#endif

[Setup]
AppId={{7C1E5B0A-2F6B-4A0D-9C3E-SCOURGE00010}
AppName=Scourge
AppVersion={#Version}
AppPublisher=Scourge Project
AppPublisherURL=https://afdian.com/a/YuanBaoSMadLab
DefaultDirName={autopf}\Scourge
DefaultGroupName=Scourge
LicenseFile={#Staging}\LICENSE.txt
SetupIconFile=sg.ico
PrivilegesRequired=admin
ChangesEnvironment=yes
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
OutputDir={#OutDir}
OutputBaseFilename={#OutName}
DisableProgramGroupPage=yes

[Tasks]
Name: "addpath"; Description: "将 Scourge 添加到系统 PATH（sg / sget / sgmcp 全局可用）| Add Scourge to the system PATH"; \
    GroupDescription: "附加任务 | Additional tasks:"

[Files]
Source: "{#Staging}\bin\*"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#Staging}\std\*"; DestDir: "{app}\std"; Flags: recursesubdirs createallsubdirs
Source: "{#Staging}\examples\*"; DestDir: "{app}\examples"; Flags: recursesubdirs createallsubdirs
Source: "{#Staging}\demos\*"; DestDir: "{app}\demos"; Flags: recursesubdirs createallsubdirs
Source: "{#Staging}\docs\*"; DestDir: "{app}\docs"; Flags: recursesubdirs createallsubdirs
Source: "{#Staging}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Staging}\README_EN.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Staging}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "sg.ico"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#Staging}\scourge.png"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Scourge README"; Filename: "{app}\README.md"; IconFilename: "{app}\bin\sg.ico"
Name: "{group}\Scourge 语言手册"; Filename: "{app}\docs"; IconFilename: "{app}\bin\sg.ico"
Name: "{group}\Uninstall Scourge"; Filename: "{uninstallexe}"

[Registry]
; .sg 源文件关联：资源管理器中显示 Scourge 图标（卸载对称清理）
Root: HKCR; Subkey: ".sg"; ValueType: string; ValueName: ""; ValueData: "Scourge.Source"; Flags: uninsdeletekey
Root: HKCR; Subkey: "Scourge.Source"; ValueType: string; ValueName: ""; ValueData: "Scourge 源文件 | Scourge Source"; Flags: uninsdeletekey
Root: HKCR; Subkey: "Scourge.Source\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\sg.ico,0"; Flags: uninsdeletekey

[Run]
Filename: "{app}\README.md"; Description: "查看 README / View README"; \
    Flags: postinstall skipifsilent shellexec

[Code]
const
    EnvironmentKey = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';
    AddPathFlagKey = 'Software\Scourge';

// 向系统 PATH 追加一项（已存在则跳过，重复分号折叠）
procedure EnvAddPath(Path: string);
var
    Paths: string;
begin
    if RegQueryStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths) then
    begin
        if Pos(';' + Uppercase(Path) + ';', ';' + Uppercase(Paths) + ';') > 0 then
            exit;
    end
    else
        Paths := '';
    Paths := Paths + ';' + Path;
    while Pos(';;', Paths) > 0 do
        StringChange(Paths, ';;', ';');
    RegWriteStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths);
end;

// 从系统 PATH 移除一项（大小写不敏感定位，摘除后折叠空分隔）
procedure EnvRemovePath(Path: string);
var
    Paths, Probe: string;
    I, Start: Integer;
begin
    if not RegQueryStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths) then
        exit;
    Probe := ';' + Uppercase(Paths) + ';';
    I := Pos(';' + Uppercase(Path) + ';', Probe);
    if I = 0 then
        exit;
    if I = 1 then
        Start := 1
    else
        Start := I - 1;
    Delete(Paths, Start, Length(Path) + 1);
    while Pos(';;', Paths) > 0 do
        StringChange(Paths, ';;', ';');
    RegWriteStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
    if CurStep = ssPostInstall then
        if WizardIsTaskSelected('addpath') then
        begin
            EnvAddPath(ExpandConstant('{app}\bin'));
            RegWriteDWordValue(HKEY_LOCAL_MACHINE, AddPathFlagKey, 'AddPathApplied', 1);
        end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
    Flag: Cardinal;
begin
    if CurUninstallStep = usUninstall then
    begin
        if RegQueryDWordValue(HKEY_LOCAL_MACHINE, AddPathFlagKey, 'AddPathApplied', Flag) and
           (Flag = 1) then
            EnvRemovePath(ExpandConstant('{app}\bin'));
        RegDeleteKeyIncludingSubkeys(HKEY_LOCAL_MACHINE, AddPathFlagKey);
    end;
end;
