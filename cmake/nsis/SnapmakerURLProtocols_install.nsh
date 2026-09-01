; Machine-wide URL protocol registration (mirrors macOS CFBundleURLSchemes).
; Included by CPack-generated NSIS installer via CPACK_NSIS_INSTALL_SCRIPT.
; On 64-bit Windows, 32-bit NSIS must use the 64-bit registry view so the 64-bit app sees these keys.
SetRegView 64

; Ultra: register ONLY an Ultra-specific scheme so a side-by-side official Snapmaker Orca keeps
; its own snapmaker-orca:// / Snapmaker_Orca:// handlers (and our uninstall never removes them).
WriteRegStr HKLM "Software\Classes\snapmaker-ultra" "" "URL:Snapmaker-Ultra"
WriteRegStr HKLM "Software\Classes\snapmaker-ultra" "URL Protocol" ""
WriteRegStr HKLM "Software\Classes\snapmaker-ultra\shell\open\command" "" '"$INSTDIR\snapmaker-orca.exe" "%1"'

SetRegView 32
