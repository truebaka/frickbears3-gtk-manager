## Frickbears 3 Manager is a native Linux GTK4 application for managing Five Nights at Frickbear's 3 installations, mods, and custom guards when running the game through Wine.

It solves the usual pain of modding this game on Linux: instead of manually unzipping archives and copying data.win / audiogroup*.dat / texgroup_* files by hand (and risking overwriting something you needed), the app keeps every game version, mod, and custom guard you import separated on disk, and lets you combine or switch between them instantly.

Core functionality:

- Import clean copies of the game as separate versions, so nothing gets overwritten between updates
- Import mods (texture/audio/skin replacements) as separate packages, independent from any specific game version
- Import custom guards, correctly placed into AppData/Local/Frickbears3/addons/<name>/ per the official custom guard spec, so multiple guards never conflict
- Build any version + any mod (or no mod) into a ready-to-launch build; builds are never deleted automatically, so several combinations can coexist
- Switch the active build instantly via a symlink, with the ability to roll back to a previous build at any time
- Enable or disable individual custom guards without deleting them, or remove them permanently
- Native folder picker dialogs for every import step, no manual path typing required
- Generate a compiled native launcher plus a .desktop entry, so the game appears in the normal application menu
- Clean up old versions, mods, or builds that are no longer needed
- Toggle between Russian and English interface language at runtime

## Building
is just binary file lol
