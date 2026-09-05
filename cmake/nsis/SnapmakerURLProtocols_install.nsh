; Machine-wide URL protocol registration (mirrors macOS CFBundleURLSchemes).
; Included by CPack-generated NSIS installer via CPACK_NSIS_INSTALL_SCRIPT.
; On 64-bit Windows, 32-bit NSIS must use the 64-bit registry view so the 64-bit app sees these keys.
SetRegView 64

; Register ONLY our own scheme, so a side-by-side official Snapmaker Orca keeps its
; snapmaker-orca:// / Snapmaker_Orca:// handlers and our uninstall never removes them.
; This is the same scheme the running app registers in HKCU (GUI_App::associate_url);
; the two used to disagree, which was the collision this comment claimed to avoid.
WriteRegStr HKLM "Software\Classes\ultraone" "" "URL:UltraOne"
WriteRegStr HKLM "Software\Classes\ultraone" "URL Protocol" ""
WriteRegStr HKLM "Software\Classes\ultraone\shell\open\command" "" '"$INSTDIR\snapmaker-orca.exe" "%1"'

SetRegView 32
