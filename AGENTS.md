# Agent Notes

## Build Environment

This ESP-IDF project needs the Espressif PowerShell environment loaded before
running build commands.

Use this from the repository root:

```powershell
& 'C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1'
idf.py build
```

For one-shot commands, run:

```powershell
& 'C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1'; idf.py build
```

If PowerShell execution policy blocks the profile script, use a process-scoped
bypass:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& 'C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1'; idf.py build"
```
