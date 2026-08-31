# Stalker 2 InputFix: Steam Overlay & Focus Integration

## Overview
This repository provides a custom fix for **S.T.A.L.K.E.R. 2: Heart of Chornobyl** to address controller input issues when using the Steam Overlay or tabbing out of the game.

By default, the game continues to collect and respond to controller inputs even when the Steam Overlay is active or the game window loses focus. This causes severe input bleed—meaning navigating the Steam Overlay or tabbing out simultaneously moves your character and navigates menus in the background. 

This fix ensures that the game seamlessly stops collecting input when the Steam Overlay is open or when the game is not in focus. It achieves this by temporarily intercepting the controller data and telling the game that no buttons are currently being pressed. The exact moment you close the overlay or return focus to the game, it instantly regains control.

**For a deep dive into the technical mechanics and OS-level mitigations, please read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).**

### Important Caveat: Tabbed-Out Overlay Inputs
While this fix completely protects the **game** from receiving unintended background inputs, it cannot protect the Steam Overlay itself.

If you leave the Steam Overlay **open** and then Alt-Tab out of the game, the Steam Overlay may continue to receive and respond to your controller inputs in the background. This is a known quirk of how Windows handles modern display presentation. For more details on why this cannot be mitigated, please refer to [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Quickstart
You can quickly install the provided pre-compiled fix by running the deployment script.

1. Ensure the game is closed.
2. Copy `config.ini.example` and name it `config.ini`. Update the `GAME_PATH` inside to match your local Stalker 2 installation directory.
3. Run `scripts\deploy.bat` to automatically copy the compiled `version.dll` directly into your `S.T.A.L.K.E.R. 2\Binaries\Win64` directory.

## Project Structure
* `src/` - Core C++ and MASM source code (`version_proxy.cpp`, `version_asm.asm`).
* `scripts/` - Build and deployment utility batch files.
* `docs/` - Contains technical architecture documentation and the historical bug tracker.
* `archive/` - Contains obsolete R&D attempts (DXGI Hooking, XInput proxies) for reference.
* `build/` - A dedicated directory for compiling build artifacts and storing temporary test files.

## How to Compile (For Developers)
This project is compiled using the MSVC compiler and the MASM assembler.
1. Run `scripts\build.bat`.
2. The compiler will isolate all intermediate files into the `build/` directory and output the final `version.dll` into the repository root.

> [!NOTE]
> For help or questions, please contact [stalker2-input-fix@xt3rm1n8r.com](mailto:stalker2-input-fix@xt3rm1n8r.com)
