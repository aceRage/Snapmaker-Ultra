; Remove URL protocol keys installed by SnapmakerURLProtocols_install.nsh
SetRegView 64
; Ultra: remove only our own scheme -- never the official app's snapmaker-orca:// / Snapmaker_Orca:// keys.
DeleteRegKey HKLM "Software\Classes\snapmaker-ultra"
SetRegView 32
