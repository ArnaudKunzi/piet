; Inno Setup script for the PIET Windows installer — NATIVE 2.0 (WP-W6).
;
; REPLACES the STALE 1.x pathway: the old version of this file installed a
; PyInstaller onedir (dist\PIET\ = PIET.exe + _internal\ with a frozen Python
; interpreter, piet_win.spec). piet/native/'s C++ core (2.0.x) has no Python
; at runtime (Doctrine ③) — this script installs the native bundle instead:
; PIET.exe (piet_native.exe), frontend/ (verbatim SPA), cobra.exe, the EULA,
; and the generated THIRD_PARTY_NOTICES.txt. piet_win.spec is SUPERSEDED by
; this + stage_native_windows.sh (see that script's header comment) — kept
; only as a historical reference, same treatment piet.spec (the macOS
; PyInstaller spec) got after M5 GA. Do not build the .exe from it again.
;
; Owner: Porting Op (operators/porting-op.md lists "installer (Inno)" as an
; explicit Phase D Windows deliverable). Coordinate with the Conductor before
; folding this into any shared release-checklist automation the Conductor
; owns.
;
; Prerequisites (staging): run piet/packaging/stage_native_windows.sh FIRST
; — it assembles the flat bundle this script compresses (see that script's
; header for exactly what it copies and from where). Then compile:
;     iscc /DDistDir="<path to the staged dir>" piet\packaging\piet_setup.iss
; DistDir defaults to ..\..\dist\windows-amd64-piet\PIET-windows (relative to
; this file) — stage_native_windows.sh's own default output — so a plain
; `iscc piet\packaging\piet_setup.iss` works right after staging, with no
; overrides at all.
;
; AppVersion is NOT hardcoded here (the single-version-source doctrine —
; piet/native/CMakeLists.txt's PIET_VERSION_VALUE is the one and only
; definition site, see that file's header comment). It is read at COMPILE
; time out of the ProductVersion string resource of the exact staged
; PIET.exe being packaged — the same binary the user will run. That string
; is embedded by piet/native/resources/windows/piet.rc.in, itself
; substituted from PIET_VERSION_VALUE, so the chain
;   CMakeLists.txt -> VERSIONINFO -> installer AppVersion
; has no hand-synced link in it. This mirrors package_native_app.sh reading
; `piet_native --version` back out of the binary being bundled (the 2.0.1
; contract_a Info.plist drift bug class).
;
; If the staged exe carries no ProductVersion (an old build, or a dummy
; fixture), the compile FAILS with #error rather than shipping a wrong
; number. Override for fixture/syntax-only builds:
;     iscc /DAppVersion=0.0.0-SYNTAXCHECK ... piet_setup.iss
;
; SYNTAX-CHECKED with real iscc (Inno Setup 6 under Wine, docker image
; amake/innosetup, 2026-07-08): compiles clean end-to-end against a dummy
; staged dir — "Successful compile", [Code] section included. CAVEAT: that
; recipe passes /DAppVersion=, which short-circuits the entire
; FileExists/GetStringFileInfo block below — so it does NOT exercise the
; version-resource read. To syntax-check THAT path you must compile without
; /DAppVersion against a staged dir holding a real, resource-carrying
; PIET.exe. Re-check recipe from the repo root:
;   SCRATCH=$(mktemp -d); mkdir -p "$SCRATCH/stage/PIET-windows"
;   cp piet/packaging/piet_setup.iss "$SCRATCH/"
;   echo dummy > "$SCRATCH/stage/PIET-windows/PIET.exe"
;   echo eula  > "$SCRATCH/stage/PIET-windows/LICENSE.txt"
;   docker run --rm --platform linux/amd64 -v "$SCRATCH":/work \
;     amake/innosetup '/DDistDir=stage\PIET-windows' \
;     '/DAppVersion=0.0.0-SYNTAXCHECK' piet_setup.iss
; (A compile against the REAL staged bundle + an install/uninstall smoke on
; actual Windows hardware is still a first-release gate item — syntax-clean
; is not installed-and-runs.)

#define AppName        "PIET"
; Matches the CompanyName string compiled into PIET.exe's VERSIONINFO
; (piet/native/resources/windows/piet.rc.in) — Add/Remove Programs and the
; PE's Details tab must not disagree about the publisher.
#define AppPublisher   "PIET"
#define AppExeName     "PIET.exe"
; The staged native bundle (stage_native_windows.sh's output), relative to
; this .iss file. Override: iscc /DDistDir="C:\path\to\staged" piet_setup.iss
#ifndef DistDir
  #define DistDir      "..\..\dist\windows-amd64-piet\PIET-windows"
#endif

; ── Minimum Inno Setup version ────────────────────────────────────────────
; Binding constraint is [Setup] ArchitecturesAllowed=x64compatible below:
; that identifier arrived in Inno Setup 6.3 (the release that also renamed
; the old `x64` identifier to `x64os`) —
; https://jrsoftware.org/ishelp/topic_archidentifiers.htm
; The ISPP constructs used here need far less (SourcePath-relative ISPP file
; functions landed in 5.4.1), so 6.3 governs. Guard pattern is the one from
; the #error docs themselves (https://jrsoftware.org/ishelp/topic_error.htm).
#if VER < EncodeVer(6,3,0)
  #error Inno Setup 6.3 or newer is required to compile this script (ArchitecturesAllowed=x64compatible)
#endif

; ── AppVersion, read out of the staged binary (see header) ────────────────
; DistDir may be relative to this .iss (the default) or absolute (a /DDistDir
; override). BOTH resolve with a single lookup: since Inno Setup 5.4.1, "all
; ISPP functions that take filenames have been changed to interpret the
; filenames as relative to SourcePath instead of the current directory"
; (https://jrsoftware.org/files/is5.5-whatsnew.htm), and SourcePath is the
; directory holding this .iss. An earlier revision of this file tried the
; path twice (as-given, then re-anchored at SourcePath) on the mistaken
; belief that ISPP file functions ignore SourcePath; that second probe could
; never fire usefully and has been dropped.
;
; GetStringFileInfo(FileName, StringName [, LangCodePage]) is a genuine ISPP
; function — https://jrsoftware.org/ishelp/topic_getstringfileinfo.htm and the
; ISPP function index https://jrsoftware.org/ishelp/topic_funcs.htm.
; GetFileProductVersionString(FileName) is equally genuine but is merely a
; convenience macro declared in ISPPBuiltins.iss as
;     #define GetFileProductVersionString(str FileName) \
;               GetStringFileInfo(FileName, PRODUCT_VERSION)
; (PRODUCT_VERSION is itself #defined there as "ProductVersion"). Since the
; wrapper is implemented in terms of the function, GetStringFileInfo is the
; strictly-wider-support spelling of the two — hence the choice here.
;
; It returns null (void), NOT an empty string, when the requested string is
; absent from the resource. So the "no ProductVersion" case is detected with
; TypeOf() against TYPE_NULL rather than by comparing against "" — both
; TypeOf and the TYPE_* constants come from ISPPBuiltins.iss, see
; https://jrsoftware.org/ishelp/topic_typeof.htm
#define StagedExe  AddBackslash(DistDir) + AppExeName
#ifndef AppVersion
  #if FileExists(StagedExe)
    #define AppVersion GetStringFileInfo(StagedExe, "ProductVersion")
  #else
    #error Staged PIET.exe not found under DistDir - run piet/packaging/stage_native_windows.sh first (or pass /DDistDir=... and, for a fixture build, /DAppVersion=...)
  #endif
#endif
; Defensive: reachable only if #error above did not abort preprocessing.
; Keeps the TypeOf() below from being handed an undefined identifier.
#ifndef AppVersion
  #error AppVersion could not be determined from the staged PIET.exe - pass /DAppVersion=... to override
#endif
#if TypeOf(AppVersion) == TYPE_NULL
  #error Staged PIET.exe carries no ProductVersion resource - it predates piet/native/resources/windows/piet.rc.in; rebuild with docker/Dockerfile.piet-windows
#endif
; Nested rather than && so the string comparison is only ever reached for a
; value already known to be a string (a /DAppVersion=123 override would make
; AppVersion an integer, which must stay legal).
#if TypeOf(AppVersion) == TYPE_STRING
  #if AppVersion == ""
    #error Staged PIET.exe's ProductVersion resource is present but empty - check piet/native/resources/windows/piet.rc.in substitution of PIET_VERSION_VALUE
  #endif
#endif

; The WebView2 Evergreen bootstrapper (download once from
; https://developer.microsoft.com/microsoft-edge/webview2/ -> "Evergreen
; Bootstrapper") and place it next to this script as
; MicrosoftEdgeWebview2Setup.exe. Same mechanism as the 1.x installer; the
; native build's WEBVIEW_EDGE backend (piet/native/vendor/webview/webview.h,
; WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL=1) needs the WebView2 Runtime present on
; the target machine exactly as much as the old pywebview EdgeChromium
; backend did — this installer step is UNCHANGED by the 1.x -> native
; rewrite.
#define WV2Bootstrapper "MicrosoftEdgeWebview2Setup.exe"

[Setup]
AppId={{B7E5B3A2-9C4D-4E1A-9F2B-PIET00000001}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\{#AppExeName}
; Icon for setup.exe / the uninstaller entry itself. Guarded because
; branding\PIET.ico is a GENERATED artifact (branding/make_windows_ico.py) —
; a checkout without it must still compile an installer, it just falls back
; to Inno's default setup icon. The app's own Explorer/taskbar icon is the
; one embedded in PIET.exe by piet/native/resources/windows/piet.rc.in and is
; unaffected by this line.
#define BrandingIcon AddBackslash(SourcePath) + "..\..\branding\PIET.ico"
#if FileExists(BrandingIcon)
SetupIconFile={#BrandingIcon}
#endif
; End-user license: the installer shows this and requires acceptance before
; Next is enabled (Inno's standard LicenseFile behavior) — same EULA the
; bundle itself carries at {app}\LICENSE.txt post-install (LEGAL-DISTRIBUTION
; §6: an installed copy without the accepted EULA visible is not
; distributable).
LicenseFile={#DistDir}\LICENSE.txt
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; Per-machine install (Program Files) needs admin; use lowest for per-user instead.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=PIET-{#AppVersion}-setup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; The entire staged native bundle: PIET.exe + LICENSE.txt +
; THIRD_PARTY_NOTICES.txt at the root, and resources\{frontend\, cobra.exe,
; THIRD_PARTY_NOTICES.txt, sample\} — the runtime layer bundle_paths.cpp's
; bundleResourcesDir() (WP-W5) requires beside the exe. See
; stage_native_windows.sh for exactly what populates {#DistDir}.
Source: "{#DistDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion
; A SECOND, WRITABLE copy of the demo kit at {userdocs}\PIET — which is ALSO
; the app's default working directory (main.cpp defaultUserCwd() prefers
; Documents\PIET when it exists), so a fresh install opens with
; `use "fatalities.csv"` already resolvable: no cd, no copying. {app} lives
; under Program Files, which is read-only for a normal user — the pangram
; writes scratch files (pangram_*.dta, a session log) into its working
; directory, so running it from {app}\demo fails on the first save.
; onlyifdoesntexist so a rerun of the installer never clobbers a professor's
; edited copy; uninsneveruninstall because user documents are the user's.
Source: "{#DistDir}\demo\*"; DestDir: "{userdocs}\PIET"; Flags: recursesubdirs createallsubdirs onlyifdoesntexist uninsneveruninstall
; Ship the WebView2 bootstrapper into a temp location; [Run] invokes it below.
; EMBEDDED into the setup (was `external`): an installer carried to a fresh machine must
; be self-contained — with `external` the bootstrapper had to travel BESIDE setup.exe, and
; when it didn't, install "succeeded" but the app hard-crashed on the missing runtime
; (verified on Windows Server 2022: 0xC0000005 at first launch). The #ifexist guard keeps
; the old "compiles even if you forgot to drop the file" property (but then WebView2 must
; already be present on the target, as before) — and it now guards the matching [Run]
; entry too, since a [Run] line pointing at a {tmp} file that was never installed is an
; install-time error box, not a no-op.
#ifexist WV2Bootstrapper
Source: "{#WV2Bootstrapper}"; DestDir: "{tmp}"; Flags: deleteafterinstall
#endif

[Icons]
Name: "{group}\{#AppName}";            Filename: "{app}\{#AppExeName}"
Name: "{group}\Third-Party Notices";   Filename: "{app}\THIRD_PARTY_NOTICES.txt"
; Points at the WRITABLE Documents copy, not {app}\demo — see the [Files] note.
Name: "{group}\Fatalities Demo Kit";    Filename: "{userdocs}\PIET"
Name: "{group}\Uninstall {#AppName}";  Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";      Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
; Install the WebView2 Evergreen runtime when the installed one is missing or
; OLDER THAN THE API FLOOR THIS APP NEEDS. Silent install.
;
; History: the original probe was a mere presence check (any non-empty
; EdgeUpdate 'pv'), which wrongly passed machines carrying an ancient
; Evergreen runtime. That was then "fixed" by deleting the check and running
; the bootstrapper unconditionally. Both are wrong in opposite directions:
; presence-only under-installs, unconditional over-installs (it forces a
; network round-trip to Microsoft's servers on every install, including on
; machines that are already fine and on sites that are offline — and Inno
; ignores a [Run] entry's exit code, so a bootstrapper that fails for lack of
; network fails SILENTLY either way).
;
; Replaced with a VERSION FLOOR, which is what the requirement actually is.
; The Windows drag-and-drop path goes through WebView2's AdditionalObjects
; API (ICoreWebView2WebMessageReceivedEventArgs2 / ICoreWebView2File — see
; piet/native/vendor/VENDOR.md's webview.h patch registry). Microsoft's
; reference documents both interfaces as "Applies to: WebView2 Win32,
; Introduced 1.0.1774.30":
;   https://learn.microsoft.com/microsoft-edge/webview2/reference/win32/icorewebview2webmessagereceivedeventargs2
;   https://learn.microsoft.com/microsoft-edge/webview2/reference/win32/icorewebview2file
; and the forward-compatibility rule is that "a WebView2 API works when using
; a WebView2 Runtime that has an equal or higher build number as the SDK build
; number in which the API was introduced. The build number is the third part
; of the four-part version number":
;   https://learn.microsoft.com/microsoft-edge/webview2/concepts/versioning
; => the floor is runtime BUILD >= 1774 (shipped as Edge/WebView2 113.x).
; This subsumes the 86.0.616.0 WebView2 GA floor from the same page.
;
; NeedsWebView2 (see [Code]) reads the documented EdgeUpdate 'pv' regkeys and
; fails SAFE: unreadable, unparseable, absent, or "0.0.0.0" all bootstrap.
;
; Per the project's legal-distribution notes: this installer step runs Microsoft's
; OWN bootstrapper under Microsoft's own EULA/terms (accepted by the
; bootstrapper itself at install time, not by this script) — PIET does not
; redistribute the WebView2 Runtime binary, only triggers Microsoft's
; installer for it. The exact WebView2 distribution terms accepted must be
; retained alongside the release (see the [Code] comment at the bottom of
; this file) — NOT bundled inside this installer, since that is Microsoft's
; document, not ours to ship.
#ifexist WV2Bootstrapper
Filename: "{tmp}\{#WV2Bootstrapper}"; Parameters: "/silent /install"; \
    StatusMsg: "Installing Microsoft Edge WebView2 Runtime under Microsoft's terms..."; \
    Flags: waituntilterminated; Check: NeedsWebView2
#endif
; Offer to launch PIET at the end.
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; \
    Flags: nowait postinstall skipifsilent

[Code]
{ WebView2 Evergreen runtime version floor — see the [Run] section comment for
  the sourcing of the 1774 build number and of these registry locations.

  NOTE: everything below is Pascal Script, evaluated at INSTALL time on the
  target machine. It is a different namespace from the ISPP preprocessor
  directives at the top of this file, which run at COMPILE time on the build
  machine. In particular Pascal's FileExists() (runtime, target machine) and
  ISPP's FileExists() (compile time, build machine) are unrelated functions
  that merely share a name; neither is used here, but do not conflate them
  when editing. }

const
  { Per https://learn.microsoft.com/microsoft-edge/webview2/concepts/distribution
    ("Detect if a WebView2 Runtime is already installed"): inspect pv (REG_SZ)
    under this Clients GUID in HKLM (per-machine) and HKCU (per-user). The doc
    gives SOFTWARE\WOW6432Node\... on 64-bit Windows and SOFTWARE\... on
    32-bit; HKLM32 is exactly that distinction, so one constant covers both. }
  WV2ClientKey = 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';
  { Build (third) component of the WebView2 SDK version that introduced
    ICoreWebView2WebMessageReceivedEventArgs2.AdditionalObjects and
    ICoreWebView2File, i.e. the postMessageWithAdditionalObjects file-drop
    path: SDK 1.0.1774.30 -> runtime build >= 1774. }
  WV2MinBuild = 1774;

{ Third component of a four-part "138.0.3351.83" version string, or -1 if the
  string is absent/malformed. -1 deliberately compares as "too old". }
function WV2BuildComponent(const Ver: String): Integer;
var
  S: String;
  P, I: Integer;
begin
  Result := -1;
  S := Ver;
  { Drop the first two components. }
  for I := 1 to 2 do
  begin
    P := Pos('.', S);
    if P = 0 then
      Exit;
    S := Copy(S, P + 1, Length(S) - P);
  end;
  P := Pos('.', S);
  if P > 0 then
    S := Copy(S, 1, P - 1);
  if S = '' then
    Exit;
  Result := StrToIntDef(S, -1);
end;

function WV2InstalledBuild(RootKey: Integer): Integer;
var
  Pv: String;
begin
  Result := -1;
  if RegQueryStringValue(RootKey, WV2ClientKey, 'pv', Pv) then
    Result := WV2BuildComponent(Pv);
end;

{ True => run Microsoft's Evergreen bootstrapper. Fails safe: any runtime we
  cannot read or cannot parse, and the documented "not installed" sentinels
  (missing key, empty pv, 0.0.0.0), all yield a build below the floor. }
function NeedsWebView2: Boolean;
var
  Best, B: Integer;
begin
  Best := WV2InstalledBuild(HKLM32);
  B := WV2InstalledBuild(HKCU);
  if B > Best then
    Best := B;
  Result := Best < WV2MinBuild;
end;

{ ────────────────────────────────────────────────────────────────────────
  WebView2 distribution-terms retention (project legal-distribution notes):
  PIET installs the WebView2 Evergreen Runtime via Microsoft's own
  bootstrapper (MicrosoftEdgeWebview2Setup.exe, downloaded separately — not
  vendored in this repo, see the #define WV2Bootstrapper comment above). The
  exact distribution terms in effect for that bootstrapper at the time it
  was staged for a release must be retained alongside the release (a saved
  copy of the "Microsoft Edge WebView2 Runtime and SDKs" distribution terms
  page, e.g. webview2-distribution-terms-<date>.pdf or
  .txt) — this is a release-time step for whoever stages
  MicrosoftEdgeWebview2Setup.exe next to this script, not something this
  .iss can do at compile time. Flagged here so it is not missed; NOT YET
  DONE (no Windows release has shipped from this recipe).
  ──────────────────────────────────────────────────────────────────────── }
