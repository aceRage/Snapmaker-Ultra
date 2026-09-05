; Remove URL protocol keys installed by SnapmakerURLProtocols_install.nsh
SetRegView 64
; Remove only our own schemes -- never the official app's snapmaker-orca:// /
; Snapmaker_Orca:// keys. snapmaker-ultra was ours under the old name, so it goes too.
DeleteRegKey HKLM "Software\Classes\ultraone"
DeleteRegKey HKLM "Software\Classes\snapmaker-ultra"
SetRegView 32
