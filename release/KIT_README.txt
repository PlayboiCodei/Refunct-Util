REFUNCT UTILITY 1.0.0 - INSTALLER CREATOR (DEVELOPER TOOL)

This is not the player installer. It creates it from the newly compiled ASI.
1. Extract the separate release source ZIP, run build_win32.bat and confirm success.
   Alternatively run make_release.bat directly in the source to build and package in one step.
2. Extract this kit and double-click Create_Player_Package.bat.
3. Select the newly compiled build-win32\Release\RefunctUtility.asi from the source.
4. Choose whether to bundle your working x86 ASI loader. If yes, select the loader
   and its license/redistribution notices. Ensure any required source is provided.
   If no, the generated package requires an existing loader.
5. The player ZIP and its SHA-256 checksum appear in dist/ beside the script.
6. Test on a clean installation before distributing it.

The creator validates the x86 DLL format and the compiled release fingerprint;
it does not independently prove binary-to-source reproducibility. Use the clean
build workflow. The installer is unsigned, checks the exact supported game
executable, verifies file hashes and preserves existing player data.
