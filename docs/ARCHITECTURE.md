# Technical Architecture & Caveats

This document serves as the master technical reference for the Multi-Layer Interception architecture driving the Stalker 2 DualSense InputFix. It outlines the specific boundaries, engine behaviors, and OS-level quirks we discovered during development.

## 1. The Multi-Layer Interception Architecture
The proxy operates on four parallel vectors simultaneously:

1. **The Loader Bypass (version.dll):**
   Unreal Engine 5 explicitly hard-blocks sideloaded DLLs like `xinput1_4.dll` using `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)`. Furthermore, Windows 11 strictly blocks the sideloading of system drivers like `hid.dll`. The only robust, OS-compliant injection vector is masquerading as the legacy system library `version.dll` and forwarding the 17 version APIs directly to the Windows folder via MASM naked assembly (`jmp qword ptr`).

2. **XInput & GameInput Blackhole (COM Interception):**
   When the Steam Overlay opens, we cannot simply sever the controller connection by returning `ERROR_DEVICE_NOT_CONNECTED`. Doing so triggers the game to drop the handle, swap its UI to Mouse/Keyboard, and suffer a multi-second initialization delay when the overlay closes. Instead, we intercept `XInputGetState` and explicitly zero out the state structs via `memset`. For GameInput, we intercept the COM VTables and zero the controller arrays.

3. **Message Pump Nullification (Desktop Configuration):**
   Even when XInput/RawInput are blocked, Steam's "Desktop Configuration" generates synthesized global Windows messages (like `WM_KEYDOWN` mapping to the D-Pad). To prevent the user from navigating the game's menu while in the overlay, we intercept `PeekMessage(W/A)` and nullify `WM_KEYDOWN` and `WM_MOUSEMOVE` into `WM_NULL`.

4. **Raw HID Kernel I/O Thread Profiling (The DualSense Native Fix):**
   Stalker 2 natively reads DualSense packets over raw USB using `GSCDualsensePlugin`. We intercept `ReadFile`, `DeviceIoControl`, and `GetOverlappedResult(Ex)`.

## 2. Critical OS Caveats

### The Steam Thread Impersonation Trap
Steam's overlay architecture injects hooks (`GameOverlayRenderer64.dll`) into the game's input APIs. If you attempt to whitelist Steam using call-stack tracing like `_ReturnAddress()`, the game's native API calls will bounce off Steam's hooks, meaning Steam will falsely appear on the call stack, defeating your whitelist and allowing inputs to bleed.
**Solution:** We completely abandoned whitelists. Steam Big Picture UI inputs are handled by `steam.exe` entirely out-of-process via IPC. It is physically impossible to break the overlay UI by blinding the game process.

### The I/O Completion Port (IOCP) Bypass
When blocking Raw USB File I/O, we cannot return an error. We must fake the read data. However, modern Unreal Engine 5 games do not use `GetOverlappedResult` to await asynchronous kernel reads. Instead, they bind the file handle to an I/O Completion Port (IOCP). This allows the kernel to bypass our DLL hooks and dump hardware packets directly into the engine's memory.
**Solution:** We completely disable asynchronous I/O when the game is blinded. We intercept `ReadFile` and immediately return `TRUE` without passing the request to the OS, returning a spoofed packet in the original buffer instead. Because the request never hits the kernel, the IOCP is never triggered.

### The Perfect DualSense Spoofed Packet
When synthesizing a spoofed packet to blind the game, you cannot simply `memset(0)` the entire packet.
1. **Joysticks:** The joystick axes rest at `128` (`0x80`). A `0` will cause the axes to snap Top-Left, spinning the camera.
2. **Enhanced Buttons:** In the 64+ byte DualSense payload, the triggers occupy bytes 5/6, and the buttons occupy byte 8. A `0` in byte 8 forces the D-Pad to the "UP" position (`0x00` = Up, `0x08` = Neutral). 
3. **Gyroscope Gravity:** The accelerometer natively reports 1G of gravity (`8192`) on the Z-Axis (`buf[27] = 0x20`). Setting this to `0` puts the controller in "0G Freefall", which causes Unreal Engine's rotation math to divide by zero and produce `NaN`, instantly snapping the player's virtual mouse cursor to the top-left pixel of the monitor.
4. **Touchpad Override:** Byte 33 Bit 7 must be explicitly set (`0x80`) to communicate "Not Touching". Furthermore, the X/Y coordinates must be mathematically centered to `960x540` to protect against buggy game engine parsers that ignore the Touch Flag.

### The Background Focus Polling
Unreal Engine continues to process background inputs even when Alt-Tabbed. Rather than intercepting `WM_ACTIVATE` which can be unreliable due to hidden windows, we spawn a 20Hz `BackgroundWorker` thread that polls `GetForegroundWindow()`. If the foreground window's Process ID does not match the game's Process ID, we flip `g_bIsGameInFocus` to `false`, combining it with the Steam Overlay state to universally drop all hardware inputs when the game is not actively being played.

### DX12 Flip Model and Steam Overlay IPC Leakage
True Exclusive Fullscreen is largely deprecated in DirectX 12. Unreal Engine 5 uses DXGI Flip Model to present HDR frames, which behaves fundamentally like a Borderless Window. When the user Alt-Tabs out, the game loses focus but does not minimize. Because the swapchain remains alive in the background, steam.exe continues to route physical controller inputs directly into the steamwebhelper.exe CEF process via an external IPC pipe. Consequently, the Steam Overlay will continue to navigate in the background even when the game is tabbed out. Because this input routing happens entirely outside the game process, our DLL cannot intercept it. Attempting to hook RegisterRawInputDevices to manipulate OS structs will cause swapchain corruption.
