REFUNCT UTILITY 1.0.0 | Windows x86

1. Install Refunct through Steam and close the game.
2. Extract ALL files from this ZIP into a temporary folder.
3. Double-click Install.bat. Select Refunct-Win32-Shipping.exe when asked.
   Steam > Refunct > Properties / Manage > Browse local files, then
   Refunct\Binaries\Win32\Refunct-Win32-Shipping.exe.
4. If this is an ASI-only package, install a compatible 32-bit ASI loader
   separately first. If a loader is bundled, follow the prompts. Never install
   multiple ASI loaders together. The installer cannot prove an existing DLL
   is a working loader; it asks for your confirmation instead.
5. Launch Refunct and press F5 to open the utility menu.

Requires the verified Refunct v9.30 Win32 executable and the Microsoft Visual
C++ 2015-2022 x86 runtime (normally already installed by Steam).
Only the verified executable SHA-256 is supported. If the version differs,
installation stops; do not bypass the compatibility check.

Existing ASIs and diagnostic INIs are backed up in a dated backup folder beside Win32 (under Binaries).
Settings, profiles, logs and replay files are left alone. No admin elevation
or system registry changes are performed. If permissions prevent installing,
use a Steam library folder you have permission to write to.

Uninstall: close Refunct and remove RefunctUtility.asi from the Win32 folder.
Keep the ASI loader if other mods use it. To roll back, restore the backed-up
ASI and diagnostic INI as appropriate. Do not delete the RefunctUtility data
folder unless you intentionally want to erase your profiles and records.

MinHook attribution and loader notices (if bundled) are in licenses/.
No Refunct game assets or executables are included. This installer is unsigned.
