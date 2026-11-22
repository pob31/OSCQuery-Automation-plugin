; This file is part of the OSCQuery Automation Plugin —
; https://github.com/pob31/OSCQuery-Automation-plugin
;
; Copyright (C) 2026 Pierre-Olivier Boulant, Julien Bayle
;
; The OSCQuery Automation Plugin is free software: you can redistribute it
; and/or modify it under the terms of the GNU General Public License as
; published by the Free Software Foundation, either version 3 of the License,
; or (at your option) any later version. It is distributed in the hope that it
; will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
; of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
; Public License (LICENSE, at the repository root) for more details.
;
; SPDX-License-Identifier: GPL-3.0-or-later
;
; The Windows installer, Inno Setup 6 (2026-11-22). Shape from WFS-DIY's
; Plugin/Installer/WFS-DIY-Plugins.iss. Compiled by scripts/build-release.ps1, which passes
; the four defines below; it reads only the staged tree, never the build tree.
;
; Where things go:
;   VST3 bundles   {commoncf64}\VST3\OSCQuery Automation\
;   CLAP module    {commoncf64}\CLAP\
;   core library   {commoncf64}\Pix et Bel\OSCQuery Automation\   the registry loader's fixed
;                  path (src/core/registry/RegistryLoader.cpp), one copy for every format, so
;                  a VST3 and a CLAP in one host share one registry; the licence and notices
;                  beside it.
;
; AppId IS FIXED FOR GOOD (2026-11-22): Windows finds the previous install, to upgrade or
; uninstall it, by this GUID. Never change it, never reuse it for another product.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef RepoRoot
  #define RepoRoot "..\.."
#endif
#ifndef StageDir
  #define StageDir RepoRoot + "\build\stage\windows"
#endif
#ifndef OutputDir
  #define OutputDir RepoRoot + "\build\installer"
#endif

#define AppName      "OSCQuery Automation"
#define AppPublisher "Pix et Bel"
#define AppURL       "https://github.com/pob31/OSCQuery-Automation-plugin"

[Setup]
AppId={{1BFA5C2D-430A-4D8D-9CB2-624D78B3B4A2}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases
DefaultDirName={commoncf64}\Pix et Bel\OSCQuery Automation
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=OSCQuery-Automation-{#AppVersion}-Windows-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayName={#AppName} {#AppVersion}
; GPLv3: the licence is shown, and installed with the binaries.
LicenseFile={#RepoRoot}\LICENSE
; A host holding the plugin open would leave the old files in place.
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french";  MessagesFile: "compiler:Languages\French.isl"

[Types]
Name: "full";   Description: "VST3 and CLAP"
Name: "custom"; Description: "Choose the formats"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plugins (Reaper, Live, Bitwig, Nuendo, Samplitude...)"; Types: full custom
Name: "clap"; Description: "CLAP plugin (Bitwig, Reaper...)";                           Types: full custom

[Files]
; The core library and the documents, whatever the formats chosen.
Source: "{#StageDir}\Core\*"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\Docs\*"; DestDir: "{app}"; Flags: ignoreversion

Source: "{#StageDir}\VST3\*"; DestDir: "{commoncf64}\VST3\OSCQuery Automation"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\CLAP\*"; DestDir: "{commoncf64}\CLAP";                     Components: clap; Flags: ignoreversion

[InstallDelete]
; A format unticked on an upgrade is removed, not left behind at the old version.
Type: filesandordirs; Name: "{commoncf64}\VST3\OSCQuery Automation"; Components: not vst3
Type: files;          Name: "{commoncf64}\CLAP\OSCQuery Automation.clap"; Components: not clap

[UninstallDelete]
Type: dirifempty; Name: "{commoncf64}\VST3\OSCQuery Automation"
Type: dirifempty; Name: "{commoncf64}\Pix et Bel"
