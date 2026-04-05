# ExplorerNavHook

Compact navigation pane hook for Windows Explorer.

ExplorerNavHook reduces the left indentation in the File Explorer navigation pane and makes the tree view more compact.

## Before / After

### Default Explorer
![Before](screenshots/before.png)

### With ExplorerNavHook
![After](screenshots/after.png)

## What it does

- reduces left indentation in the navigation pane
- keeps the modern Explorer look
- supports config-based behavior through ExplorerNavHook.ini
- includes helper scripts for install and uninstall

## Files

- `ExplorerNavHook.exe` — loader
- `ExplorerNavHook.dll` — hook library
- `ExplorerNavHook.ini` — config
- `register.cmd` — registration script
- `uninstall.cmd` — uninstall script

## Installation

1. Download the latest release.
2. Extract all files into one folder.
3. Run `register.cmd` if needed.
4. Run `ExplorerNavHook.exe`.
5. Open a new Explorer window.

## Example config

```ini
[Hook]
TargetIndent=30
RemoveHasButtons=1
RemoveHasLines=1
RemoveLinesAtRoot=1
```

## Compatibility

- confirmed working on Windows 11
- confirmed working on Windows 10

## Notes

- Behavior may change after major Windows updates.
- Use at your own risk.

## License

MIT