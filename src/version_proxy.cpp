// ============================================================================
// STALKER 2: VERSION.DLL GLOBAL PROXY & XINPUT/RAWINPUT FIREWALL
// Strategy: MASM Naked Assembly Forwarder + MinHook Background Thread
// ============================================================================
// This DLL masquerades as the system 'version.dll' to bypass UE5's SetDefaultDllDirectories
// anti-hijacking mechanics. It uses MASM naked jumps to perfectly forward all 17 
// VERSION API calls to the real Windows file, while secretly spawning a background thread 
// to MinHook XInput and RawInput.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <mutex>
#include <atomic>
#include <stdio.h>
#include "MinHook.h"
#include <intrin.h>
#pragma intrinsic(_ReturnAddress) // Force compiler to use intrinsic version of _ReturnAddress.

#include "version_pointers.h"
enum class ControllerType { Unknown = 0, DualSense = 1, DualShock4 = 2 };

// Hardware Identification Constants
#define SONY_VID_W L"vid_054c"
#define DUALSENSE_PID_1_W L"pid_0ce6"
#define DUALSENSE_EDGE_PID_W L"pid_0df2"
#define DS4_PID_1_W L"pid_05c4"
#define DS4_PID_2_W L"pid_09cc"

#define SONY_VID_A "vid_054c"
#define DUALSENSE_PID_1_A "pid_0ce6"
#define DUALSENSE_EDGE_PID_A "pid_0df2"
#define DS4_PID_1_A "pid_05c4"
#define DS4_PID_2_A "pid_09cc"



// Steamworks SDK Minimal Header
#pragma pack(push, 8)
typedef void* HSteamPipe;
typedef void* HSteamUser;
struct CallbackMsg_t { HSteamUser m_hSteamUser; int m_iCallback; uint8_t* m_pubParam; int m_cubParam; };
class CCallbackBase {
public:
    virtual void Run(void* pvParam) = 0;
    virtual void Run(void* pvParam, bool bIOFailure, HSteamPipe hSteamPipe) = 0;
    virtual int GetCallbackSizeBytes() = 0;
public:
    uint8_t m_nCallbackFlags; int m_iCallback;
};
class CCallbackImpl : public CCallbackBase {
public:
    void* m_pObj; void (*m_Func)(void*, void*); // Pointers to the instance object and the function to handle the callback.
    virtual void Run(void* pvParam) override { m_Func(m_pObj, pvParam); } // Execute the callback by invoking the stored function.
    virtual void Run(void* pvParam, bool bIOFailure, HSteamPipe hSteamPipe) override { m_Func(m_pObj, pvParam); } // Overloaded version of Run; currently ignores bIOFailure and hSteamPipe.
    virtual int GetCallbackSizeBytes() override { return sizeof(GameOverlayActivated_t); } // Return the size of the specifically handled overlay activated structure.
    struct GameOverlayActivated_t { uint8_t m_bActive; }; // Structure storing whether the game overlay is active (boolean-like byte).
};
typedef void (__cdecl *SteamAPI_RegisterCallback_t)(CCallbackBase* pCallback, int iCallback);
typedef void (__cdecl *SteamAPI_UnregisterCallback_t)(CCallbackBase* pCallback);
#pragma pack(pop)

std::atomic<bool> g_bIsSteamOverlayActive(false); // Thread-safe flag indicating if the Steam overlay is open.
std::atomic<bool> g_bIsGameInFocus(true); // Thread-safe flag indicating if the game window is currently focused.

bool ShouldBlockInput() { // Function to determine if inputs should be blocked.
    // Return true if either the overlay is active or the game is out of focus.
    return g_bIsSteamOverlayActive.load(std::memory_order_relaxed) || !g_bIsGameInFocus.load(std::memory_order_relaxed);
}

// ============================================================================
// STEAM OVERLAY LISTENER
// Purpose: Hooks into the Steam API to detect when Shift+Tab is pressed.
// This object is intentionally leaked on DLL_PROCESS_DETACH because cleaning it up
// while steam_api64.dll is unloading causes a race condition and crashes the game on exit.
// ============================================================================
class CSteamOverlayListener { // Class for managing Steam overlay events.
public:
    CSteamOverlayListener() { // Constructor.
        m_CallbackImpl.m_pObj = this; // Set the object pointer to this instance.
        // Initialize flags to 0. If uninitialized garbage memory is here, 
        // the Steam API silently ignores the callback registration.
        m_CallbackImpl.m_nCallbackFlags = 0; // Explicitly set callback flags to 0.
        m_CallbackImpl.m_iCallback = 331; // Set callback ID for GameOverlayActivated.
        m_CallbackImpl.m_Func = [](void* obj, void* param) { // Define a lambda as the callback handler function.
            // Cast the void pointers to appropriate types and invoke OnGameOverlayActivated.
            static_cast<CSteamOverlayListener*>(obj)->OnGameOverlayActivated(
                static_cast<CCallbackImpl::GameOverlayActivated_t*>(param));
        };
        
        HMODULE hSteam = GetModuleHandleA("steam_api64.dll"); // Get a handle to the loaded steam_api64.dll module.
        if (hSteam) { // Check if steam_api64.dll was successfully found.
            // Retrieve the address of the SteamAPI_RegisterCallback export.
            auto reg = (SteamAPI_RegisterCallback_t)GetProcAddress(hSteam, "SteamAPI_RegisterCallback");
            if (reg) reg(&m_CallbackImpl, m_CallbackImpl.m_iCallback); // If found, register the callback with Steam.
            
            FILE* f; // Declare a file pointer.
            if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { // Try to open proxy_loaded.txt in append mode.
                fprintf(f, "Registered Steam Callback 331\n"); // Log that the callback was registered.
                fclose(f); // Close the log file.
            }
        }
    }
    void OnGameOverlayActivated(CCallbackImpl::GameOverlayActivated_t* pCallback) { // Handler for overlay state changes.
        // Update the atomic flag for overlay state using memory_order_release.
        g_bIsSteamOverlayActive.store(pCallback->m_bActive != 0, std::memory_order_release);
        
        FILE* f; // Declare a file pointer.
        if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { // Try to open proxy_loaded.txt in append mode.
            fprintf(f, "OVERLAY TRIGGERED: %d\n", pCallback->m_bActive); // Log the new overlay state.
            fclose(f); // Close the log file.
        }
    }
private: // Private access modifier.
    CCallbackImpl m_CallbackImpl; // The actual callback implementation object.
};

CSteamOverlayListener* g_pSteamListener = nullptr; // Global pointer to the Steam overlay listener instance.
std::once_flag g_SteamInitOnceFlag; // Once-flag to ensure the listener is initialized only a single time.

// ============================================================================
// STEAM CALLER WHITELIST (Trevigintuple Guard)
// Purpose: When the overlay is open, we must block the Game from reading inputs,
// but ALLOW the Steam Overlay itself to read them so you can navigate the UI.
// Mechanism: Uses the CPU _ReturnAddress() to check if the caller lives inside
// a known Steam DLL memory space.
// ============================================================================
void NeutralizeDualSensePacket(PBYTE buf, DWORD size); // Forward declaration of function to neutralize DualSense packets.
// Constructs a flawless hardware spoof that explicitly sets the Touchpad to "Not Touching",
// neutralizes all sticks and buttons, and provides proper IMU data to prevent NaN math.
void SynthesizeNeutralDualShock4Packet(PBYTE buf, DWORD size) { // Function to zero out or neutralize DualShock 4 input.
    if (!buf || size < 64) return; // If buffer is invalid or too small, abort synthesis.
    memset(buf, 0, size); // Clear the entire buffer to zeroes.
    
    // USB Report ID
    buf[0] = 0x01; // Set the report ID to 1.
    
    // Set joysticks to mechanical center (128). 
    buf[1] = 0x80; buf[2] = 0x80; buf[3] = 0x80; buf[4] = 0x80; // Assign 128 (0x80) to both X and Y axes of both sticks.
    
    // DS4 expects triggers at buf[8] and buf[9], and buttons/D-pad at buf[5]. 0x08 is neutral D-pad.
    buf[5] = 0x08; // Set D-Pad to neutral position.
    buf[6] = 0x00; // Clear buttons.
    buf[7] = 0x00; // Clear system/special buttons.
    buf[8] = 0x00; // Left trigger.
    buf[9] = 0x00; // Right trigger.
    
    // IMU (Gravity)
    // 1G gravity on Z axis to prevent camera NaN snapping. DS4 expects this at bytes 23-24.
    if (size >= 25) {
        buf[23] = 0x00; // Accelerometer Z LSB.
        buf[24] = 0x20; // 0x2000 = 8192 in little endian. Accelerometer Z MSB.
    }
    
    // Touchpad 1 data.
    if (size >= 40) { // Check if buffer includes touchpad data.
        // We set the MSB (0x80) to indicate the finger is NOT touching the pad.
        buf[36] = 0x80; 
        
        // As a fallback for lazy parsers that ignore the "Not Touching" bit,
        // we explicitly set the coordinate to the physical center of the pad (960x471)
        // instead of leaving it at 0,0 (Top-Left) which could cause UI glitches.
        buf[37] = 0xC0; // X Low
        buf[38] = 0x73; // X High + Y Low
        buf[39] = 0x1D; // Y High
    }
}

void SynthesizeNeutralDualSensePacket(PBYTE buf, DWORD size) { // Function to zero out or neutralize DualSense input.
    if (!buf || size < 64) return; // If buffer is invalid or too small, abort synthesis.
    memset(buf, 0, size); // Clear the entire buffer to zeroes.
    
    // USB Report ID
    buf[0] = 0x01; // Set the report ID to 1 (standard DualSense USB report).
    
    // Set joysticks to mechanical center (128). 
    // Setting these to 0 would cause the camera/character to violently snap to the top-left.
    buf[1] = 0x80; buf[2] = 0x80; buf[3] = 0x80; buf[4] = 0x80; // Assign 128 (0x80) to both X and Y axes of both sticks.
    
    // Triggers (Unpressed = 0)
    buf[5] = 0x00; buf[6] = 0x00; // Reset both trigger values to 0.
    
    // Sequence Counter
    buf[7] = 0x00; // Set the sequence counter to 0.
    
    // DualSense maps buttons to byte 8 (unlike DualShock 4 which uses byte 5).
    // 0x08 represents a neutral, unpressed D-Pad.
    // If left as 0x00, the game interprets the D-Pad as being permanently held "UP".
    buf[8] = 0x08; // Set D-Pad to neutral position.
    buf[9] = 0x00; // Clear remaining main buttons.
    buf[10] = 0x00; // Clear system/special buttons.
    
    // Gyroscope (Centered = 0)
    // buf[16] to buf[21] already 0 from memset
    
    // Synthesize 1G of downward gravity on the Z-axis (8192 units).
    // If we supply 0G (freefall) across all axes, Unreal Engine's IMU math 
    // divides by zero, resulting in NaNs that violently snap the camera.
    if (size >= 28) { // Make sure buffer is large enough for IMU values.
        buf[26] = 0x00; // Accelerometer Z LSB.
        buf[27] = 0x20; // 0x2000 = 8192 in little endian. Accelerometer Z MSB.
    }
    
    // Touchpad 1 data.
    if (size >= 37) { // Check if buffer includes touchpad data.
        // We set the MSB (0x80) to indicate the finger is NOT touching the pad.
        buf[33] = 0x80; 
        
        // As a fallback for lazy parsers that ignore the "Not Touching" bit,
        // we explicitly set the coordinate to the physical center of the pad (960x540)
        // instead of leaving it at 0,0 (Top-Left) which could cause UI glitches.
        buf[34] = 0xC0; // X Low
        buf[35] = 0xC3; // X High + Y Low
        buf[36] = 0x21; // Y High
    }
    
    // Touchpad 2
    if (size >= 41) { // Check if buffer includes second touchpad data point.
        buf[37] = 0x80; // Not Touching flag for second touchpad point.
        buf[38] = 0xC0; // X Low for point 2.
        buf[39] = 0xC3; // X High + Y Low for point 2.
        buf[40] = 0x21; // Y High for point 2.
    }
}

bool IsCallerSteam(void* callerAddress) { // Function to determine if a calling address belongs to Steam.
    HMODULE hCaller = NULL; // Initialize module handle for the caller to NULL.
    // Returns FALSE if caller is unmapped JIT memory (e.g. Anti-Cheat/Mods)
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)callerAddress, &hCaller); // Try to get the module handle for the given caller address.
    if (!hCaller) return false; // If no module was found (e.g., JIT code or invalid address), it's not Steam.
    
    static HMODULE hOverlay = NULL; // Static cache for the GameOverlayRenderer64 module handle.
    static HMODULE hApi = NULL; // Static cache for the steam_api64 module handle.
    static HMODULE hClient = NULL; // Static cache for the steamclient64 module handle.
    static bool bInitialized = false; // Static flag to track if the handles have been cached.
    
    if (hCaller == hOverlay || hCaller == hApi || (hClient && hCaller == hClient)) return true; // Fast-path check: return true immediately if caller matches cached handles.
    
    // Cache miss handling: Only run GetModuleHandle once. If steamclient64.dll 
    // isn't injected into the game, we cache the NULL so we don't spam the OS.
    if (!bInitialized) { // Check if we haven't initialized the cache yet.
        hOverlay = GetModuleHandleA("GameOverlayRenderer64.dll"); // Look up overlay DLL.
        hApi = GetModuleHandleA("steam_api64.dll"); // Look up steam API DLL.
        hClient = GetModuleHandleA("steamclient64.dll"); // Look up steam client DLL.
        bInitialized = true; // Mark as initialized so we don't look up again.
    }
    
    return (hCaller == hOverlay || hCaller == hApi || (hClient && hCaller == hClient)); // Check one last time with freshly cached values.
}



// --- Hook Typedefs ---
typedef UINT (WINAPI *GetRawInputData_t)(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);
GetRawInputData_t real_GetRawInputData = nullptr; // Function pointer to store the original GetRawInputData.

typedef DWORD (WINAPI *XInputGetState_t)(DWORD dwUserIndex, void* pState);
XInputGetState_t real_XInputGetState = nullptr; // Function pointer to store the original XInputGetState.

typedef DWORD (WINAPI *XInputSetState_t)(DWORD dwUserIndex, void* pVibration);
XInputSetState_t real_XInputSetState = nullptr; // Function pointer to store the original XInputSetState.

typedef DWORD (WINAPI *XInputGetKeystroke_t)(DWORD dwUserIndex, DWORD dwReserved, void* pKeystroke);
XInputGetKeystroke_t real_XInputGetKeystroke = nullptr; // Function pointer to store the original XInputGetKeystroke.

typedef DWORD (WINAPI *XInputGetStateEx_t)(DWORD dwUserIndex, void* pState);
XInputGetStateEx_t real_XInputGetStateEx = nullptr; // Function pointer to store the original XInputGetStateEx.


// ============================================================================
// HOOK IMPLEMENTATIONS
// Purpose: Completely sever the Game's connection to physical and emulated
// controllers while the Steam Overlay is active.
// ============================================================================


#include <unordered_map>
#include <shared_mutex>
std::unordered_map<HANDLE, ControllerType> g_RawInputSonyHandles; // Map tracking handles known to be Sony controllers.
std::unordered_map<HANDLE, bool> g_RawInputNonSonyHandles; // Map tracking handles known NOT to be Sony controllers.
std::shared_mutex g_RawInputMutex; // Mutex protecting the raw input handle sets.

ControllerType GetRawInputSonyType(HANDLE hDevice) { // Function to determine if a raw input device is a Sony controller.
    if (!hDevice) return ControllerType::Unknown; // If the device handle is null, it's not a valid device.
    
    { // Create a scope for the reader lock.
        std::shared_lock<std::shared_mutex> lock(g_RawInputMutex); // Acquire a shared lock for reading.
        auto it = g_RawInputSonyHandles.find(hDevice);
        if (it != g_RawInputSonyHandles.end()) return it->second; // If found in Sony set, return the type.
        if (g_RawInputNonSonyHandles.find(hDevice) != g_RawInputNonSonyHandles.end()) return ControllerType::Unknown; // If found in non-Sony set, return unknown.
    } // Release shared lock.
    
    UINT size = 0; // Variable to hold the size of the device name.
    GetRawInputDeviceInfoA(hDevice, RIDI_DEVICENAME, nullptr, &size); // Query the required size for the device name.
    if (size > 0 && size < 1024) { // Verify the size is sensible before allocating on stack.
        char name[1024] = {0}; // Buffer for the device name.
        if (GetRawInputDeviceInfoA(hDevice, RIDI_DEVICENAME, name, &size) != (UINT)-1) { // Actually retrieve the device name.
            std::string s(name); // Convert to std::string for easier processing.
            for (auto& c : s) c = tolower((unsigned char)c); // Convert the entire string to lowercase (safe cast).
            
            std::unique_lock<std::shared_mutex> lock(g_RawInputMutex); // Acquire an exclusive lock to update caches.
            if (s.find(SONY_VID_A) != std::string::npos) { // Check if the device name contains Sony's Vendor ID.
                ControllerType type = ControllerType::Unknown;
                if (s.find(DS4_PID_1_A) != std::string::npos || s.find(DS4_PID_2_A) != std::string::npos) {
                    type = ControllerType::DualShock4;
                } else if (s.find(DUALSENSE_PID_1_A) != std::string::npos || s.find(DUALSENSE_EDGE_PID_A) != std::string::npos) {
                    type = ControllerType::DualSense;
                }
                
                if (type != ControllerType::Unknown) {
                    g_RawInputSonyHandles[hDevice] = type; // Add to known Sony handles.
                    return type; // Return the type as it's a known Sony device.
                }
            }
            g_RawInputNonSonyHandles[hDevice] = true; // Add to known non-Sony handles.
            return ControllerType::Unknown; // Return unknown.
        }
    }
    return ControllerType::Unknown; // Default to unknown if we couldn't determine the device type.
}

UINT WINAPI Hook_GetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader) { // Intercept GetRawInputData calls.
    UINT result = real_GetRawInputData(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader); // Call the original GetRawInputData first to fill the buffer.
    if (result != (UINT)-1 && pData && uiCommand == RID_INPUT && ShouldBlockInput()) { // Check if the read succeeded, we have data, we're reading input, and we should be blocking.
        RAWINPUT* raw = (RAWINPUT*)pData; // Cast the raw data to a RAWINPUT structure.
        if (raw->header.dwType == RIM_TYPEHID) { // Verify this is a Human Interface Device report.
            DWORD reportSize = raw->data.hid.dwSizeHid * raw->data.hid.dwCount; // Calculate the total byte size of the HID report.
            ControllerType type = GetRawInputSonyType(raw->header.hDevice);
            if ((reportSize == 64 || reportSize == 78) && type != ControllerType::Unknown) { // DualSense reports are usually 64 or 78 bytes. Check size and Sony vendor.
                if (type == ControllerType::DualSense) {
                    SynthesizeNeutralDualSensePacket((PBYTE)raw->data.hid.bRawData, reportSize); // Overwrite the report with a neutral (zero-state) packet.
                } else if (type == ControllerType::DualShock4) {
                    SynthesizeNeutralDualShock4Packet((PBYTE)raw->data.hid.bRawData, reportSize); // Overwrite the report with a neutral DS4 packet.
                }
            }
        }
    }
    return result; // Return the (potentially modified) result to the caller.
}

// ============================================================================
// KEYBOARD / MOUSE / WM_INPUT MESSAGE FILTER
// Purpose: Block synthesized Desktop Configuration inputs from Steam
// ============================================================================

// Listens for PnP (Plug and Play) device removal broadcasts from the Windows kernel.
// Because RawInput device handles are not closed via CloseHandle(), the OS can aggressively 
// recycle internal handle IDs when a user hot-swaps controllers. By purging the handles 
// immediately upon physical disconnect, we prevent the proxy from misidentifying a new Xbox 
// controller as an old disconnected DualShock 4 due to handle ID recycling.
void HandleDeviceChangeMessage(LPMSG lpMsg) {
    if (lpMsg && lpMsg->message == 0x00FE) { // 0x00FE is WM_INPUT_DEVICE_CHANGE, fired by the OS on hot-plug events.
        if (lpMsg->wParam == 2) { // 2 corresponds to GIDC_REMOVAL, meaning the hardware was physically disconnected.
            HANDLE hDevice = (HANDLE)lpMsg->lParam; // The lParam explicitly holds the internal RawInput handle of the dropped device.
            std::unique_lock<std::shared_mutex> lock(g_RawInputMutex); // Acquire exclusive write lock to safely modify our internal tracking maps.
            g_RawInputSonyHandles.erase(hDevice); // Purge the handle from the Sony controller cache so it cannot be falsely reused.
            g_RawInputNonSonyHandles.erase(hDevice); // Purge the handle from the non-Sony blacklist cache.
        }
    }
}

// Prevent Steam's global "Desktop Configuration" from synthesizing fake Windows Keyboard events
// when the overlay is active.
void NullifyMessage(LPMSG lpMsg) { // Function to zero out Windows messages if blocking is active.
 // Function to zero out Windows messages if blocking is active.
    if (lpMsg && ShouldBlockInput()) { // Check if the message pointer is valid and input should be blocked.
        if ((lpMsg->message >= WM_KEYFIRST && lpMsg->message <= WM_KEYLAST) || // Check if it's a keyboard message.
            (lpMsg->message >= WM_MOUSEFIRST && lpMsg->message <= WM_MOUSELAST) || // Check if it's a mouse message.
            (lpMsg->message == WM_INPUT)) { // Check if it's a raw input message.
            lpMsg->message = WM_NULL; // Change the message type to WM_NULL so the game ignores it.
        }
    }
}

typedef BOOL (WINAPI *PeekMessageW_t)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
PeekMessageW_t real_PeekMessageW = nullptr; // Store the original PeekMessageW pointer.
BOOL WINAPI Hook_PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg) { // Intercept PeekMessageW.
    BOOL result = real_PeekMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg); // Call the original PeekMessageW.
    if (result) { HandleDeviceChangeMessage(lpMsg); NullifyMessage(lpMsg); } // If a message was retrieved, try to nullify it if necessary.
    return result; // Return the result of PeekMessageW.
}

typedef BOOL (WINAPI *PeekMessageA_t)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
PeekMessageA_t real_PeekMessageA = nullptr; // Store the original PeekMessageA pointer.
BOOL WINAPI Hook_PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg) { // Intercept PeekMessageA.
    BOOL result = real_PeekMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg); // Call original PeekMessageA.
    if (result) { HandleDeviceChangeMessage(lpMsg); NullifyMessage(lpMsg); } // Check if a message was retrieved, then nullify it if necessary.
    return result; // Return the potentially modified result.
}

typedef BOOL (WINAPI *GetMessageW_t)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
GetMessageW_t real_GetMessageW = nullptr; // Store the original GetMessageW pointer.
BOOL WINAPI Hook_GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax) { // Intercept GetMessageW.
    BOOL result = real_GetMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax); // Call original GetMessageW.
    if (result != -1 && result != 0) { HandleDeviceChangeMessage(lpMsg); NullifyMessage(lpMsg); } // If we successfully got a message (not an error or WM_QUIT), nullify it if necessary.
    return result; // Return the potentially modified result.
}

typedef BOOL (WINAPI *GetMessageA_t)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
GetMessageA_t real_GetMessageA = nullptr; // Store the original GetMessageA pointer.
BOOL WINAPI Hook_GetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax) { // Intercept GetMessageA.
    BOOL result = real_GetMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax); // Call original GetMessageA.
    if (result != -1 && result != 0) { HandleDeviceChangeMessage(lpMsg); NullifyMessage(lpMsg); } // Check if a message was retrieved successfully and nullify it if needed.
    return result; // Return the potentially modified result.
}

typedef LRESULT (WINAPI *DispatchMessageW_t)(const MSG* lpMsg);
DispatchMessageW_t real_DispatchMessageW = nullptr; // Store the original DispatchMessageW pointer.
LRESULT WINAPI Hook_DispatchMessageW(const MSG* lpMsg) { // Intercept DispatchMessageW.
    if (lpMsg && ShouldBlockInput()) { // Check if we have a message and we should be blocking.
        if ((lpMsg->message >= WM_KEYFIRST && lpMsg->message <= WM_KEYLAST) || // Check if message is a keyboard event.
            (lpMsg->message >= WM_MOUSEFIRST && lpMsg->message <= WM_MOUSELAST) || // Check if message is a mouse event.
            (lpMsg->message == WM_INPUT)) { // Check if message is raw input.
            return 0; // Return 0 to indicate the message was "processed" and should not be dispatched.
        }
    }
    return real_DispatchMessageW(lpMsg); // Otherwise, forward to the real DispatchMessageW.
}

typedef LRESULT (WINAPI *DispatchMessageA_t)(const MSG* lpMsg);
DispatchMessageA_t real_DispatchMessageA = nullptr; // Store the original DispatchMessageA pointer.
LRESULT WINAPI Hook_DispatchMessageA(const MSG* lpMsg) { // Intercept DispatchMessageA.
    if (lpMsg && ShouldBlockInput()) { // Check if message pointer is valid and input should be blocked.
        if ((lpMsg->message >= WM_KEYFIRST && lpMsg->message <= WM_KEYLAST) || // Is it a keyboard message?
            (lpMsg->message >= WM_MOUSEFIRST && lpMsg->message <= WM_MOUSELAST) || // Is it a mouse message?
            (lpMsg->message == WM_INPUT)) { // Is it a raw input message?
            return 0; // Drop the message by returning 0.
        }
    }
    return real_DispatchMessageA(lpMsg); // Otherwise, forward to the real DispatchMessageA.
}
// ============================================================================
// ============================================================================
// ============================================================================
// ============================================================================
// XINPUT INTERCEPTOR (Xbox Controller Support)
// ============================================================================

// Strictly wipe the state memory when blocking input. 
// If we just return success without zeroing, Unreal Engine thinks the last pressed button is permanently stuck down.
DWORD WINAPI Hook_XInputGetState(DWORD dwUserIndex, void* pState) { // Intercept XInputGetState.
    DWORD res = real_XInputGetState(dwUserIndex, pState); // Retrieve the real controller state.
    if (res == ERROR_SUCCESS && pState && ShouldBlockInput()) { // If successful and we have state, check if we should block.
        memset(pState, 0, 16); // Wipe the XINPUT_STATE struct (16 bytes) to clear all buttons and axes.
    }
    return res; // Return the success code to fake that a controller is connected but neutral.
}

DWORD WINAPI Hook_XInputSetState(DWORD dwUserIndex, void* pVibration) { // Intercept XInputSetState (Rumble).
    if (ShouldBlockInput()) { // Check if input is blocked.
        return 1167; // ERROR_DEVICE_NOT_CONNECTED - pretend device isn't connected so game stops rumbling.
    }
    return real_XInputSetState(dwUserIndex, pVibration); // Otherwise, set the rumble state normally.
}

DWORD WINAPI Hook_XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved, void* pKeystroke) { // Intercept XInputGetKeystroke.
    if (ShouldBlockInput()) { // Check if we should block.
        if (pKeystroke) memset(pKeystroke, 0, 8); // Clear the XINPUT_KEYSTROKE struct (8 bytes).
        return 1167; // Return ERROR_DEVICE_NOT_CONNECTED.
    }
    return real_XInputGetKeystroke(dwUserIndex, dwReserved, pKeystroke); // Otherwise, get keystroke normally.
}

// We must intercept XInputGetStateEx (ordinal 100) exactly like XInputGetState, because Unreal Engine 
// will silently fall back to it for internal state polling if it finds it.
DWORD WINAPI Hook_XInputGetStateEx(DWORD dwUserIndex, void* pState) {
    DWORD res = real_XInputGetStateEx(dwUserIndex, pState); // Call the real underlying function.
    if (res == ERROR_SUCCESS && pState && ShouldBlockInput()) { // If call succeeded and state is valid, see if input is blocked.
        memset(pState, 0, 16); // Zero out the state to report neutral inputs.
    }
    return res; // Return original response code.
}


// ============================================================================
// MOUSE AND KEYBOARD STATE INTERCEPTORS (Desktop Configuration Bypass)
// ============================================================================
typedef BOOL (WINAPI *GetCursorPos_t)(LPPOINT lpPoint);
GetCursorPos_t real_GetCursorPos = nullptr; // Store the original GetCursorPos pointer.
BOOL WINAPI Hook_GetCursorPos(LPPOINT lpPoint) { // Intercept GetCursorPos.
    BOOL result = real_GetCursorPos(lpPoint); // Get the real cursor position.
    if (result && ShouldBlockInput()) { // Check if the function succeeded and we should block input.
        // Freeze the cursor at 0,0 for the game
        if (lpPoint) { lpPoint->x = 0; lpPoint->y = 0; } // Set coordinates to top-left to prevent camera spinning.
    }
    return result; // Return the success status.
}

typedef BOOL (WINAPI *SetCursorPos_t)(int X, int Y);
SetCursorPos_t real_SetCursorPos = nullptr; // Store the original SetCursorPos pointer.
BOOL WINAPI Hook_SetCursorPos(int X, int Y) { // Intercept SetCursorPos.
    if (ShouldBlockInput()) { // Check if input is blocked.
        return TRUE; // Ignore attempts to set cursor position and fake success.
    }
    return real_SetCursorPos(X, Y); // Otherwise, actually set the cursor position.
}

typedef SHORT (WINAPI *GetAsyncKeyState_t)(int vKey);
GetAsyncKeyState_t real_GetAsyncKeyState = nullptr; // Store the original GetAsyncKeyState pointer.
SHORT WINAPI Hook_GetAsyncKeyState(int vKey) { // Intercept GetAsyncKeyState.
    if (ShouldBlockInput()) return 0; // Return 0 (key not pressed) if input is blocked.
    return real_GetAsyncKeyState(vKey); // Otherwise, get the actual async key state.
}

typedef SHORT (WINAPI *GetKeyState_t)(int vKey);
GetKeyState_t real_GetKeyState = nullptr; // Store the original GetKeyState pointer.
SHORT WINAPI Hook_GetKeyState(int vKey) { // Intercept GetKeyState.
    if (ShouldBlockInput()) return 0; // Return 0 (key not pressed) if input is blocked.
    return real_GetKeyState(vKey); // Otherwise, return actual key state.
}

typedef BOOL (WINAPI *GetKeyboardState_t)(PBYTE lpKeyState);
GetKeyboardState_t real_GetKeyboardState = nullptr; // Store the original GetKeyboardState pointer.
BOOL WINAPI Hook_GetKeyboardState(PBYTE lpKeyState) { // Intercept GetKeyboardState.
    BOOL result = real_GetKeyboardState(lpKeyState); // Retrieve the current keyboard state.
    if (result && ShouldBlockInput() && lpKeyState) { // If successful and blocking is on and pointer is valid.
        memset(lpKeyState, 0, 256); // Zero out the entire 256-byte keyboard state array.
    }
    return result; // Return the function's status.
}

// ============================================================================
// MESSAGE QUEUE INTERCEPTORS (User32)
// ============================================================================
// GAMEINPUT INTERCEPTOR (Microsoft GDK)
// ============================================================================
typedef HRESULT (WINAPI *IGameInput_GetCurrentReading_t)(void* pThis, UINT32 inputKind, void* device, void** reading);
IGameInput_GetCurrentReading_t real_IGameInput_GetCurrentReading = nullptr; // Store the original pointer.

typedef HRESULT (WINAPI *IGameInput_GetNextReading_t)(void* pThis, void* refReading, UINT32 inputKind, void* device, void** reading);
IGameInput_GetNextReading_t real_IGameInput_GetNextReading = nullptr; // Store the original pointer for NextReading.
IGameInput_GetNextReading_t real_IGameInput_GetPreviousReading = nullptr; // Store the original pointer for PreviousReading (same signature).

HRESULT WINAPI Hook_IGameInput_GetCurrentReading(void* pThis, UINT32 inputKind, void* device, void** reading) { // Intercept GetCurrentReading.
    if (ShouldBlockInput()) { // Check if we should block.
        static std::once_flag log_flag; // Flag to log this intercept only once.
        std::call_once(log_flag, []() { // Ensure lambda is executed at most once.
            FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fprintf(f, "BLOCKED: GameInput GetCurrentReading intercepted successfully!\n"); fclose(f); } // Open log, write, close.
        });
        if (reading) *reading = nullptr; // Set the output reading pointer to null.
        return 0x8007048F; // Return ERROR_DEVICE_NOT_CONNECTED to simulate device drop.
    }
    return real_IGameInput_GetCurrentReading(pThis, inputKind, device, reading); // If not blocking, call original GetCurrentReading.
}

HRESULT WINAPI Hook_IGameInput_GetNextReading(void* pThis, void* refReading, UINT32 inputKind, void* device, void** reading) { // Intercept GetNextReading.
    if (ShouldBlockInput()) { // Check if we should block.
        static std::once_flag log_flag; // Flag to log this intercept only once.
        std::call_once(log_flag, []() { // Ensure lambda is executed at most once.
            FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fprintf(f, "BLOCKED: GameInput GetNextReading intercepted successfully!\n"); fclose(f); } // Open log, write, close.
        });
        if (reading) *reading = nullptr; // Set the output reading pointer to null.
        return 0x8007048F; // Return ERROR_DEVICE_NOT_CONNECTED.
    }
    return real_IGameInput_GetNextReading(pThis, refReading, inputKind, device, reading); // Otherwise, fetch next reading normally.
}

HRESULT WINAPI Hook_IGameInput_GetPreviousReading(void* pThis, void* refReading, UINT32 inputKind, void* device, void** reading) { // Intercept GetPreviousReading.
    if (ShouldBlockInput()) { // Check if we should block.
        static std::once_flag log_flag; // Flag to log this intercept only once.
        std::call_once(log_flag, []() { // Ensure lambda is executed at most once.
            FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fprintf(f, "BLOCKED: GameInput GetPreviousReading intercepted successfully!\n"); fclose(f); } // Open log, write, close.
        });
        if (reading) *reading = nullptr; // Set the output reading pointer to null.
        return 0x8007048F; // Return ERROR_DEVICE_NOT_CONNECTED.
    }
    return real_IGameInput_GetPreviousReading(pThis, refReading, inputKind, device, reading); // Otherwise, fetch previous reading normally.
}

typedef HRESULT (WINAPI *GameInputCreate_t)(void** gameInput);
GameInputCreate_t real_GameInputCreate = nullptr; // Store original GameInputCreate function pointer.
HRESULT WINAPI Hook_GameInputCreate(void** gameInput) { // Intercept GameInputCreate to hook COM interfaces dynamically.
    FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fprintf(f, "API DETECT: GameInputCreate Called!\n"); fclose(f); } // Log the creation call.
    HRESULT hr = real_GameInputCreate(gameInput); // Call the original GameInputCreate to get the interface.
    if (SUCCEEDED(hr) && gameInput && *gameInput) { // Check if interface creation succeeded.
        void** vtable = *(void***)(*gameInput); // Dereference the object to get the VTable array.
        // Hook GetCurrentReading (index 4 in IGameInput vtable)
        if (MH_CreateHook(vtable[4], (LPVOID)&Hook_IGameInput_GetCurrentReading, (reinterpret_cast<LPVOID*>(&real_IGameInput_GetCurrentReading))) == MH_OK) MH_EnableHook(vtable[4]);
        // Hook GetNextReading (index 5)
        if (MH_CreateHook(vtable[5], (LPVOID)&Hook_IGameInput_GetNextReading, (reinterpret_cast<LPVOID*>(&real_IGameInput_GetNextReading))) == MH_OK) MH_EnableHook(vtable[5]);
        // Hook GetPreviousReading (index 6)
        if (MH_CreateHook(vtable[6], (LPVOID)&Hook_IGameInput_GetPreviousReading, (reinterpret_cast<LPVOID*>(&real_IGameInput_GetPreviousReading))) == MH_OK) MH_EnableHook(vtable[6]);
    }
    return hr; // Return original result.
}

// ============================================================================
// WGI INTERCEPTOR (Windows.Gaming.Input)
// ============================================================================
typedef HRESULT (WINAPI* GetCurrentReading_t)(void* pThis, UINT32 bLen, BOOLEAN* bArr, UINT32 sLen, void* sArr, UINT32 aLen, double* aArr, UINT64* ts);
GetCurrentReading_t real_GetCurrentReading = nullptr;
HRESULT WINAPI Hook_GetCurrentReading(void* pThis, UINT32 bLen, BOOLEAN* bArr, UINT32 sLen, void* sArr, UINT32 aLen, double* aArr, UINT64* ts) {
    HRESULT hr = real_GetCurrentReading(pThis, bLen, bArr, sLen, sArr, aLen, aArr, ts); // Retrieve the reading.
    if (SUCCEEDED(hr) && ShouldBlockInput()) { // Check if reading succeeded and we need to block input.
        if (bArr && bLen > 0) memset(bArr, 0, bLen * sizeof(BOOLEAN)); // Zero out the buttons array.
        if (sArr && sLen > 0) memset(sArr, 0, sLen * sizeof(void*)); // Zero out the switches/D-Pad array.
        if (aArr && aLen > 0) for (UINT32 i = 0; i < aLen; ++i) aArr[i] = 0.5; // Set all analog axes to center (0.5 for WGI).
    }
    return hr; // Return the original result.
}

typedef HRESULT (WINAPI* EventHandler_Invoke_t)(void* pThis, void* sender, void* args);
EventHandler_Invoke_t real_EventHandler_Invoke = nullptr;
HRESULT WINAPI Hook_EventHandler_Invoke(void* pThis, void* sender, void* args) { // Hook the event handler invocation.
    if (args) { // Check if event arguments exist.
        void** vtable = *(void***)args; // Get the VTable from the args (which is typically the controller object).
        void* get_reading = vtable[12]; // Index 12 is typically GetCurrentReading for RawGameController.
        // Hook the GetCurrentReading method on the specific controller instance.
        if (MH_CreateHook(get_reading, (LPVOID)&Hook_GetCurrentReading, (reinterpret_cast<LPVOID*>(&real_GetCurrentReading))) == MH_OK) MH_EnableHook(get_reading);
    }
    return real_EventHandler_Invoke(pThis, sender, args); // Execute original invoke.
}

typedef HRESULT (WINAPI* add_RawGameControllerAdded_t)(void* pThis, void* eventHandler, void* token);
add_RawGameControllerAdded_t real_add_RawGameControllerAdded = nullptr;
HRESULT WINAPI Hook_add_RawGameControllerAdded(void* pThis, void* eventHandler, void* token) { // Hook when a new controller event listener is added.
    if (eventHandler) { // Check if event handler is valid.
        void** vtable = *(void***)eventHandler; // Get event handler VTable.
        void* invoke = vtable[3]; // Index 3 is typically the Invoke method.
        // Hook the Invoke method of this event handler so we can intercept controller instances when they are connected.
        if (MH_CreateHook(invoke, (LPVOID)&Hook_EventHandler_Invoke, (reinterpret_cast<LPVOID*>(&real_EventHandler_Invoke))) == MH_OK) MH_EnableHook(invoke);
    }
    return real_add_RawGameControllerAdded(pThis, eventHandler, token); // Forward to original function.
}

typedef HRESULT (WINAPI *RoGetActivationFactory_t)(void* activatableClassId, REFIID iid, void** factory);
RoGetActivationFactory_t real_RoGetActivationFactory = nullptr;
typedef PCWSTR (WINAPI *WindowsGetStringRawBuffer_t)(void* string, UINT32* length);
WindowsGetStringRawBuffer_t ptr_WindowsGetStringRawBuffer = nullptr;

HRESULT WINAPI Hook_RoGetActivationFactory(void* activatableClassId, REFIID iid, void** factory) { // Hook the WinRT object factory to intercept WGI instantiation.
    if (activatableClassId && ptr_WindowsGetStringRawBuffer) { // Check if the class ID and string extractor are available.
        PCWSTR className = ptr_WindowsGetStringRawBuffer(activatableClassId, nullptr); // Get the string name of the class being activated.
        if (className) { // Check if name is valid.
            FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fwprintf(f, L"RoGetActivationFactory Called: %s\n", className); fclose(f); } // Log the requested WinRT class.
        }
    }
    
    HRESULT hr = real_RoGetActivationFactory(activatableClassId, iid, factory); // Call real factory getter.
    if (SUCCEEDED(hr) && factory && *factory && activatableClassId && ptr_WindowsGetStringRawBuffer) { // If successful, check if it's the class we care about.
        PCWSTR className = ptr_WindowsGetStringRawBuffer(activatableClassId, nullptr); // Get the class name again.
        if (className && wcsstr(className, L"Windows.Gaming.Input.RawGameController") != nullptr) { // If it is RawGameController factory.
            void** vtable = *(void***)(*factory); // Get the factory VTable.
            void* add_event = vtable[6]; // Index 6 is typically add_RawGameControllerAdded.
            // Hook the add event method.
            if (MH_CreateHook(add_event, (LPVOID)&Hook_add_RawGameControllerAdded, (reinterpret_cast<LPVOID*>(&real_add_RawGameControllerAdded))) == MH_OK) MH_EnableHook(add_event);
        }
    }
    return hr; // Return factory creation result.
}


// ============================================================================
// BACKGROUND WORKER THREAD
// ============================================================================
DWORD WINAPI BackgroundWorker(LPVOID lpParam) { // The main background thread entry point.
    bool xinputHooked = false; // Flag to track if XInput has been hooked.
    bool gameinputHooked = false; // Flag to track if GameInput has been hooked.
    DWORD currentProcessId = GetCurrentProcessId(); // Cache the current process ID.
    
    while (true) { // Enter infinite polling loop.
        // Track Focus State efficiently
        HWND hForeground = GetForegroundWindow(); // Get the currently focused window handle.
        if (hForeground) { // Check if a foreground window exists.
            DWORD foregroundProcessId = 0; // Initialize variable for process ID.
            GetWindowThreadProcessId(hForeground, &foregroundProcessId); // Get the process ID of the focused window.
            g_bIsGameInFocus.store((foregroundProcessId == currentProcessId), std::memory_order_relaxed); // Update focus state: true if game process owns focused window.
        } else { // If no foreground window exists.
            g_bIsGameInFocus.store(false, std::memory_order_relaxed); // Assume game is not in focus.
        }

        if (!g_pSteamListener) { // If the Steam listener isn't initialized yet.
            if (GetModuleHandleA("steam_api64.dll") != NULL) { // Check if steam_api64.dll has been loaded into the process.
                std::call_once(g_SteamInitOnceFlag, []() { // Ensure listener is created only once.
                    g_pSteamListener = new CSteamOverlayListener(); // Instantiate the listener to register the overlay callback.
                });
            }
        }
        
        if (ShouldBlockInput()) { // If inputs are currently being blocked.
            if (GetModuleHandleA("GameOverlayRenderer64.dll") == NULL) { // Check if the overlay renderer DLL is missing (meaning Steam crashed or was forcibly closed).
                g_bIsSteamOverlayActive.store(false, std::memory_order_release); // Force-disable the block so the user doesn't get permanently stuck.
                FILE* f; // Declare file pointer.
                if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { // Open log file.
                    fprintf(f, "WATCHDOG: Steam crashed! Force-disabled firewall.\n"); // Log the watchdog intervention.
                    fclose(f); // Close log file.
                }
            }
        }
        
        if (!xinputHooked) { // If XInput hasn't been hooked yet.
            char sysDir[MAX_PATH]; // Buffer for system directory path.
            if (GetSystemDirectoryA(sysDir, MAX_PATH)) { // Get the Windows System32 directory.
                strcat_s(sysDir, sizeof(sysDir), "\\xinput1_4.dll"); // Append the XInput DLL name.
                HMODULE hXInput = LoadLibraryExA(sysDir, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32); // Try to load it explicitly from System32.
                if (!hXInput) { // If loading failed.
                    GetSystemDirectoryA(sysDir, MAX_PATH); // Refresh system directory path.
                    strcat_s(sysDir, sizeof(sysDir), "\\xinput1_3.dll"); // Try fallback XInput 1.3 DLL.
                    hXInput = LoadLibraryExA(sysDir, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32); // Attempt to load fallback.
                }
                
                if (hXInput) { // If either version of XInput was successfully loaded.
                    void* xget = GetProcAddress(hXInput, "XInputGetState"); // Get original XInputGetState pointer.
                    void* xset = GetProcAddress(hXInput, "XInputSetState"); // Get original XInputSetState pointer.
                    void* xkey = GetProcAddress(hXInput, "XInputGetKeystroke"); // Get original XInputGetKeystroke pointer.
                    void* xgetex = GetProcAddress(hXInput, (LPCSTR)100); // Get original XInputGetStateEx pointer (ordinal 100).
                    
                    if (xget) { if (MH_CreateHook(xget, (LPVOID)&Hook_XInputGetState, (reinterpret_cast<LPVOID*>(&real_XInputGetState))) == MH_OK) MH_EnableHook(xget); } // Hook XInputGetState if available.
                    if (xset) { if (MH_CreateHook(xset, (LPVOID)&Hook_XInputSetState, (reinterpret_cast<LPVOID*>(&real_XInputSetState))) == MH_OK) MH_EnableHook(xset); } // Hook XInputSetState if available.
                    if (xkey) { if (MH_CreateHook(xkey, (LPVOID)&Hook_XInputGetKeystroke, (reinterpret_cast<LPVOID*>(&real_XInputGetKeystroke))) == MH_OK) MH_EnableHook(xkey); } // Hook XInputGetKeystroke if available.
                    if (xgetex) { if (MH_CreateHook(xgetex, (LPVOID)&Hook_XInputGetStateEx, (reinterpret_cast<LPVOID*>(&real_XInputGetStateEx))) == MH_OK) MH_EnableHook(xgetex); } // Hook XInputGetStateEx if available.
                    
                    xinputHooked = true; // Mark XInput as hooked so we don't try again.
                    FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fprintf(f, "XInput dynamically hooked by BackgroundWorker!\n"); fclose(f); } // Log the successful hook.
                }
            }
        }
        
        if (!gameinputHooked) { // If GameInput hasn't been hooked yet.
            char sysDir[MAX_PATH]; // Buffer for system directory.
            if (GetSystemDirectoryA(sysDir, MAX_PATH)) { // Get Windows system directory.
                strcat_s(sysDir, sizeof(sysDir), "\\gameinput.dll"); // Append GameInput DLL name.
                HMODULE hGameInput = LoadLibraryExA(sysDir, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32); // Attempt to load gameinput.dll.
                if (hGameInput) { // If GameInput was successfully loaded.
                    void* gic = GetProcAddress(hGameInput, "GameInputCreate"); // Get address of GameInputCreate.
                    if (gic) { // If function exists in the loaded module.
                        if (MH_CreateHook(gic, (LPVOID)&Hook_GameInputCreate, (reinterpret_cast<LPVOID*>(&real_GameInputCreate))) == MH_OK) MH_EnableHook(gic); // Hook GameInputCreate.
                        gameinputHooked = true; // Mark as hooked.
                        FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fprintf(f, "GameInput dynamically hooked by BackgroundWorker!\n"); fclose(f); } // Log hook success.
                    }
                }
            }
        }
        
        Sleep(50); // Pause the thread for 50 milliseconds to prevent 100% CPU usage.
    }
    return 0; // Return 0 (never reached).
}

// ============================================================================
// FILE IO AND HID CREATION INTERCEPTORS (Sony Controller Support)
// ============================================================================
#include <atomic>

// Array mapping every theoretical Windows user-mode handle index to a tracked ControllerType.
// Because handle indices map directly to array slots, this acts as a lock-free direct-mapped table.
std::atomic<uint8_t> g_HandleTable[16777216];

// Converts a raw Windows handle into a deterministic array index. 
// Windows assigns handle values as sequential multiples of 4. This method filters out invalid
// handles, negative pseudo-handles, and out-of-bounds kernel handles.
inline bool TryGetHandleIndex(HANDLE h, size_t& outIndex) {
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return false;
    size_t index = (size_t)h / 4;
    if (index >= 16777216) return false;
    outIndex = index;
    return true;
}

// Interrogates the hardware path of a newly opened file handle to identify targeted Sony controllers.
// If the path matches specific Sony DualSense or DualShock Product IDs, the handle is flagged
// in the global state array so that future read operations can be intercepted and spoofed.
void RegisterHidHandle(HANDLE h, LPCWSTR name) {
    size_t index; // Declare an index variable to store the calculated array slot.
    if (TryGetHandleIndex(h, index) && name) { // Ensure the handle is valid and the hardware string pointer exists.
        std::wstring s(name); // Copy the wide string into a local wstring object for manipulation.
        for (auto& c : s) c = towlower(c); // Convert every character to lowercase for case-insensitive matching.
        
        if (s.find(SONY_VID_W) != std::wstring::npos) { // Check if the hardware string contains the Sony Vendor ID.
            ControllerType type = ControllerType::Unknown; // Default the type to Unknown (0).
            if (s.find(DS4_PID_1_W) != std::wstring::npos || s.find(DS4_PID_2_W) != std::wstring::npos) { // Check for DualShock 4 PIDs.
                type = ControllerType::DualShock4; // Mark the controller type as a DualShock 4.
            } else if (s.find(DUALSENSE_PID_1_W) != std::wstring::npos || s.find(DUALSENSE_EDGE_PID_W) != std::wstring::npos) { // Check for DualSense PIDs.
                type = ControllerType::DualSense; // Mark the controller type as a DualSense.
            }
            if (type != ControllerType::Unknown) { // If a matching Sony controller was identified.
                g_HandleTable[index].store(static_cast<uint8_t>(type), std::memory_order_release); // Store the controller type into the O(1) tracking array using a thread-safe release fence.
            }
        }
    }
}

// ANSI equivalent of the HID hardware path parser. Required because older game engine modules 
// or legacy APIs might invoke CreateFileA instead of the modern wide-string CreateFileW.
void RegisterHidHandleA(HANDLE h, LPCSTR name) {
    size_t index; // Declare an index variable for the O(1) table lookup.
    if (TryGetHandleIndex(h, index) && name) { // Validate the handle bounds and the name pointer.
        std::string s(name); // Copy the ANSI string into a local string object.
        for (auto& c : s) c = tolower((unsigned char)c); // Convert the entire string to lowercase.
        
        if (s.find(SONY_VID_A) != std::string::npos) { // Check if the path contains the ANSI Sony Vendor ID.
            ControllerType type = ControllerType::Unknown; // Default to Unknown.
            if (s.find(DS4_PID_1_A) != std::string::npos || s.find(DS4_PID_2_A) != std::string::npos) { // Look for DualShock 4 specific IDs.
                type = ControllerType::DualShock4; // Mark as DualShock 4.
            } else if (s.find(DUALSENSE_PID_1_A) != std::string::npos || s.find(DUALSENSE_EDGE_PID_A) != std::string::npos) { // Look for DualSense specific IDs.
                type = ControllerType::DualSense; // Mark as DualSense.
            }
            if (type != ControllerType::Unknown) { // If a Sony controller was correctly identified.
                g_HandleTable[index].store(static_cast<uint8_t>(type), std::memory_order_release); // Safely store the enumeration byte into the direct-mapped handle array.
            }
        }
    }
}

// Queries the global handle table to verify if the file stream currently being accessed 
// belongs to a tracked DualSense or DualShock 4 controller.
ControllerType GetHidHandleType(HANDLE h) {
    size_t index;
    if (TryGetHandleIndex(h, index)) {
        return static_cast<ControllerType>(g_HandleTable[index].load(std::memory_order_relaxed));
    }
    return ControllerType::Unknown;
}


typedef HANDLE (WINAPI *CreateFileW_t)(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
CreateFileW_t real_CreateFileW = nullptr;
// We must hook CreateFile so we can spy on the Windows kernel when Unreal Engine opens a USB device.
// If the filename matches a DualSense PID, we cache the handle so our ReadFile hook knows which handles to intercept.
HANDLE WINAPI Hook_CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    HANDLE h = real_CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile); 
    RegisterHidHandle(h, lpFileName); 
    return h; 
}

typedef HANDLE (WINAPI *CreateFile2_t)(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, DWORD dwCreationDisposition, void* pCreateExParams);
CreateFile2_t real_CreateFile2 = nullptr;
// We must hook CreateFile2 for the exact same reason as CreateFileW (caching DualSense hardware handles).
HANDLE WINAPI Hook_CreateFile2(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, DWORD dwCreationDisposition, void* pCreateExParams) {
    HANDLE h = real_CreateFile2(lpFileName, dwDesiredAccess, dwShareMode, dwCreationDisposition, pCreateExParams); 
    RegisterHidHandle(h, lpFileName); 
    return h; 
}

typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
CreateFileA_t real_CreateFileA = nullptr;
// We must hook CreateFileA for the exact same reason as CreateFileW (caching DualSense hardware handles).
HANDLE WINAPI Hook_CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    HANDLE h = real_CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile); 
    RegisterHidHandleA(h, lpFileName); 
    return h; 
}

typedef NTSTATUS (WINAPI *NtClose_t)(HANDLE Handle);
NtClose_t real_NtClose = nullptr;
// Intercepts handle closures to immediately untrack gamepads disconnected by the engine.
// Hooking at the ntdll layer guarantees interception of closures initiated via any higher-level API.
NTSTATUS WINAPI Hook_NtClose(HANDLE Handle) {
    size_t index;
    if (TryGetHandleIndex(Handle, index)) {
        // Read without synchronization to avoid cross-core cache invalidation for the vast majority of non-HID handles.
        if (g_HandleTable[index].load(std::memory_order_relaxed) != 0) {
            g_HandleTable[index].store(0, std::memory_order_relaxed); // Untrack the controller.
        }
    }
    return real_NtClose(Handle);
}


typedef BOOL (WINAPI *DeviceIoControl_t)(HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer, DWORD nInBufferSize, LPVOID lpOutBuffer, DWORD nOutBufferSize, LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped);
DeviceIoControl_t real_DeviceIoControl = nullptr;
// Returning TRUE without error prevents USB drop and avoids UE5's IOCP completely bypassing GetOverlappedResult.
BOOL WINAPI Hook_DeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer, DWORD nInBufferSize, LPVOID lpOutBuffer, DWORD nOutBufferSize, LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped) {
    ControllerType type = GetHidHandleType(hDevice); // Get the device type.
    if (ShouldBlockInput() && type != ControllerType::Unknown) { // Check if we should override.
        // FAILSAFE WHITELIST: Ensure the handle is actually a raw device (like a USB controller).
        // If the OS recycled a handle ID from a closed controller and assigned it to a disk file 
        // (FILE_TYPE_DISK) or a socket/pipe (FILE_TYPE_PIPE), we dynamically abort the spoofing.
        DWORD fileType = GetFileType(hDevice);
        if (fileType != FILE_TYPE_UNKNOWN && fileType != FILE_TYPE_CHAR) {
            size_t index;
            if (TryGetHandleIndex(hDevice, index)) {
                g_HandleTable[index].store(0, std::memory_order_relaxed); // Dynamically purge the recycled handle
            }
            return real_DeviceIoControl(hDevice, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer, nOutBufferSize, lpBytesReturned, lpOverlapped);
        }

        if (dwIoControlCode == 0x000B00C0) { // IOCTL_HID_GET_INPUT_REPORT
            if (lpOutBuffer && nOutBufferSize > 0) { // Verify output buffer.
                if (nOutBufferSize >= 64) { // Verify size.
                    if (type == ControllerType::DualSense) {
                        SynthesizeNeutralDualSensePacket((PBYTE)lpOutBuffer, nOutBufferSize); // Write a completely neutral DualSense report.
                    } else if (type == ControllerType::DualShock4) {
                        SynthesizeNeutralDualShock4Packet((PBYTE)lpOutBuffer, nOutBufferSize); // Write a completely neutral DS4 report.
                    }
                }
            }
            DWORD bytesReturned = nOutBufferSize >= 64 ? 64 : nOutBufferSize; // Fake the number of bytes returned.
            
            if (lpBytesReturned) *lpBytesReturned = bytesReturned; 
            if (lpOverlapped) { // If an asynchronous OVERLAPPED structure was provided.
                lpOverlapped->Internal = 0; // Set internal status to success (0).
                lpOverlapped->InternalHigh = bytesReturned; // Set bytes transferred.
                if (lpOverlapped->hEvent) SetEvent(lpOverlapped->hEvent); // Manually signal the completion event so the game's IOCP wait wakes up.
            }
            Sleep(4); // Throttle the intercepted polling loop to prevent 100% CPU infinite-loop deadlocks.
            return TRUE; // Return success (prevent error reporting to game).
        }
    }
    return real_DeviceIoControl(hDevice, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer, nOutBufferSize, lpBytesReturned, lpOverlapped); // Call original DeviceIoControl.
}

typedef BOOL (WINAPI *ReadFile_t)(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped);
ReadFile_t real_ReadFile = nullptr;
BOOL WINAPI Hook_ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped) {
    ControllerType type = GetHidHandleType(hFile); // Get the device type for this handle.
    if (ShouldBlockInput() && type != ControllerType::Unknown) { // Check if we should override.
        // FAILSAFE WHITELIST: Ensure the handle is actually a raw device (like a USB controller).
        // If the OS recycled a handle ID from a closed controller and assigned it to a disk file 
        // (FILE_TYPE_DISK) or a socket/pipe (FILE_TYPE_PIPE), we dynamically abort the spoofing.
        DWORD fileType = GetFileType(hFile);
        if (fileType != FILE_TYPE_UNKNOWN && fileType != FILE_TYPE_CHAR) {
            size_t index;
            if (TryGetHandleIndex(hFile, index)) {
                g_HandleTable[index].store(0, std::memory_order_relaxed); // Dynamically purge the recycled handle
            }
            return real_ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
        }
        
        if (lpBuffer && nNumberOfBytesToRead > 0) { // Verify output buffer is valid.
            if (nNumberOfBytesToRead >= 64) { // Ensure buffer is large enough for spoofing.
                if (type == ControllerType::DualSense) {
                    SynthesizeNeutralDualSensePacket((PBYTE)lpBuffer, nNumberOfBytesToRead); // Write neutral DualSense inputs to the buffer directly.
                } else if (type == ControllerType::DualShock4) {
                    SynthesizeNeutralDualShock4Packet((PBYTE)lpBuffer, nNumberOfBytesToRead); // Write neutral DS4 inputs to the buffer directly.
                }
            }
        }
        DWORD bytesRead = nNumberOfBytesToRead >= 64 ? 64 : nNumberOfBytesToRead; // Determine bytes read to report.
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = bytesRead; // Set the synchronous out-parameter.
        if (lpOverlapped) { // Check for async IO structure.
            lpOverlapped->Internal = 0; // Mark operation as successful.
            lpOverlapped->InternalHigh = bytesRead; // Mark bytes transferred.
            if (lpOverlapped->hEvent) SetEvent(lpOverlapped->hEvent); // Signal completion event.
        }
        Sleep(4); // Throttle the intercepted polling loop to prevent 100% CPU infinite-loop deadlocks.
        return TRUE; // Return true to indicate immediate success.
    }
    return real_ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped); // Otherwise, fall back to the real ReadFile.
}

typedef BOOL (WINAPI *GetOverlappedResult_t)(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait);
GetOverlappedResult_t real_GetOverlappedResult = nullptr;
BOOL WINAPI Hook_GetOverlappedResult(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait) {
    if (ShouldBlockInput() && GetHidHandleType(hFile) != ControllerType::Unknown) { // Check if we should override.
        if (lpNumberOfBytesTransferred) *lpNumberOfBytesTransferred = 64; // Pretend we transferred a full packet.
        Sleep(4); // Throttle the intercepted polling loop
        return TRUE; // Say the async op completed successfully.
    }
    return real_GetOverlappedResult(hFile, lpOverlapped, lpNumberOfBytesTransferred, bWait); // Call real function.
}

typedef BOOL (WINAPI *GetOverlappedResultEx_t)(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, DWORD dwMilliseconds, BOOL bAlertable);
GetOverlappedResultEx_t real_GetOverlappedResultEx = nullptr;
BOOL WINAPI Hook_GetOverlappedResultEx(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, DWORD dwMilliseconds, BOOL bAlertable) {
    if (ShouldBlockInput() && GetHidHandleType(hFile) != ControllerType::Unknown) { // Check if we should override.
        if (lpNumberOfBytesTransferred) *lpNumberOfBytesTransferred = 64; // Return faked bytes transferred count.
        Sleep(4); // Throttle the intercepted polling loop
        return TRUE; // Return success immediately.
    }
    return real_GetOverlappedResultEx(hFile, lpOverlapped, lpNumberOfBytesTransferred, dwMilliseconds, bAlertable); // Fall through to real func.
}

// ============================================================================
// XINPUT INTERCEPTOR
// ============================================================================
// ============================================================================
// DYNAMIC LOADER INTERCEPTORS (LoadLibrary)
// ============================================================================
// Background watcher that inspects newly loaded DLLs. If the engine dynamically loads GameInput.dll 
// late into execution, this catches it and injects our COM VTable intercepts on the fly.
void CheckAndHookDynamicLibsW(HMODULE hModule, LPCWSTR lpLibFileName) {
    if (!hModule || !lpLibFileName) return; // Skip if null parameters.
    
    if (wcsstr(lpLibFileName, L"gameinput.dll") || wcsstr(lpLibFileName, L"GameInput.dll") || wcsstr(lpLibFileName, L"GAMEINPUT.DLL")) { // Did they just load GameInput dynamically?
        void* gic = GetProcAddress(hModule, "GameInputCreate"); // Get the exported function pointer.
        if (gic) { // Check if it's there.
            if (MH_CreateHook(gic, (LPVOID)&Hook_GameInputCreate, (reinterpret_cast<LPVOID*>(&real_GameInputCreate))) == MH_OK) { // Hook it.
                MH_EnableHook(gic); // Enable hook immediately.
                FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fwprintf(f, L"GameInput hooked via LoadLibraryW interception! (%s)\n", lpLibFileName); fclose(f); } // Log it.
            }
        }
    }
}

// ANSI equivalent for late-bound library interception, checking for GameInput.dll.
void CheckAndHookDynamicLibsA(HMODULE hModule, LPCSTR lpLibFileName) {
    if (!hModule || !lpLibFileName) return; // Validate parameters.
    
    if (strstr(lpLibFileName, "gameinput.dll") || strstr(lpLibFileName, "GameInput.dll") || strstr(lpLibFileName, "GAMEINPUT.DLL")) { // Check for gameinput.dll load.
        void* gic = GetProcAddress(hModule, "GameInputCreate"); // Find the function pointer.
        if (gic) { // Ensure function was exported.
            if (MH_CreateHook(gic, (LPVOID)&Hook_GameInputCreate, (reinterpret_cast<LPVOID*>(&real_GameInputCreate))) == MH_OK) { // Hook the creation function.
                MH_EnableHook(gic); // Turn it on.
                FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fprintf(f, "GameInput hooked via LoadLibraryA interception! (%s)\n", lpLibFileName); fclose(f); } // Make a log entry.
            }
        }
    }
}

typedef HMODULE (WINAPI *LoadLibraryExW_t)(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
LoadLibraryExW_t real_LoadLibraryExW = nullptr; // Real pointer.
// Intercepts dynamic library loading to catch Unreal Engine instantiating input modules at runtime.
HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE hModule = real_LoadLibraryExW(lpLibFileName, hFile, dwFlags); // Forward the call.
    CheckAndHookDynamicLibsW(hModule, lpLibFileName); // Run check against the new module.
    return hModule; // Return module to caller.
}

typedef HMODULE (WINAPI *LoadLibraryW_t)(LPCWSTR lpLibFileName);
LoadLibraryW_t real_LoadLibraryW = nullptr; // Real pointer.
// Intercepts standard wide-string library loading to catch late-bound input modules.
HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR lpLibFileName) {
    HMODULE hModule = real_LoadLibraryW(lpLibFileName); // Load it normally.
    CheckAndHookDynamicLibsW(hModule, lpLibFileName); // Inspect the freshly loaded lib.
    return hModule; // Return the handle.
}

typedef HMODULE (WINAPI *LoadLibraryExA_t)(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
LoadLibraryExA_t real_LoadLibraryExA = nullptr; // Real pointer.
// Intercepts ANSI dynamic library loading to catch late-bound input modules.
HMODULE WINAPI Hook_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE hModule = real_LoadLibraryExA(lpLibFileName, hFile, dwFlags); // Normal execution.
    CheckAndHookDynamicLibsA(hModule, lpLibFileName); // Check for gameinput DLLs.
    return hModule; // Provide handle back.
}

typedef HMODULE (WINAPI *LoadLibraryA_t)(LPCSTR lpLibFileName);
LoadLibraryA_t real_LoadLibraryA = nullptr; // Real pointer.
// Intercepts standard ANSI library loading to catch late-bound input modules.
HMODULE WINAPI Hook_LoadLibraryA(LPCSTR lpLibFileName) {
    HMODULE hModule = real_LoadLibraryA(lpLibFileName); // Hand off to system.
    CheckAndHookDynamicLibsA(hModule, lpLibFileName); // See if it's our target.
    return hModule; // Output result.
}

// ============================================================================
// DLL ENTRY POINT
// ============================================================================
// Standard Win32 entry point. Because we execute inside the process's Loader Lock, we must initialize 
// all of our MinHook detours completely synchronously here before the engine's WinMain ever begins.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) { // Only execute logic when first attaching to the process.
        DisableThreadLibraryCalls(hModule); // Optimize performance by refusing DLL_THREAD_ATTACH / DETACH calls.
        
        FILE* f; // Debug file pointer.
        if (fopen_s(&f, "proxy_loaded.txt", "w") == 0) { // Wipe/create the proxy debug log.
            fprintf(f, "VERSION_PROXY LOADED SUCCESSFULLY! Synchronous hooks initialized.\n"); // Note startup.
            fclose(f); // Close log.
        }
        
        LoadRealVersion(); // IMPORTANT: Load the actual system version.dll and map all 17 pointers so naked exports don't crash.
        
        HMODULE hSelf; // Handle to this DLL.
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)&DllMain, &hSelf); // Pin this proxy DLL in memory so it cannot be unloaded.
        
        MH_Initialize(); // Fire up MinHook framework.
        
        // --- SYNCHRONOUS HOOKS (DllMain) ---
        HMODULE hUser32 = GetModuleHandleA("user32.dll"); // Look up user32 block for core window functionality.
        if (hUser32) { // Ensure user32 was actually resolved.
            void* getRawInput = GetProcAddress(hUser32, "GetRawInputData"); // Find raw input reader.
            void* peekMsgW = GetProcAddress(hUser32, "PeekMessageW"); // Find PeekMessageW.
            void* peekMsgA = GetProcAddress(hUser32, "PeekMessageA"); // Find PeekMessageA.
            void* getMsgW = GetProcAddress(hUser32, "GetMessageW"); // Find GetMessageW.
            void* getMsgA = GetProcAddress(hUser32, "GetMessageA"); // Find GetMessageA.
            void* dispMsgW = GetProcAddress(hUser32, "DispatchMessageW"); // Find DispatchMessageW.
            void* dispMsgA = GetProcAddress(hUser32, "DispatchMessageA"); // Find DispatchMessageA.
            
            void* getAsync = GetProcAddress(hUser32, "GetAsyncKeyState"); // Find asynchronous key state.
            void* getKey = GetProcAddress(hUser32, "GetKeyState"); // Find synchronous key state.
            void* getKbdState = GetProcAddress(hUser32, "GetKeyboardState"); // Find full keyboard array reader.
            void* getCurPos = GetProcAddress(hUser32, "GetCursorPos"); // Find mouse position getter.
            void* setCurPos = GetProcAddress(hUser32, "SetCursorPos"); // Find mouse position setter.
            
            if (getRawInput) { if (MH_CreateHook(getRawInput, (LPVOID)&Hook_GetRawInputData, (reinterpret_cast<LPVOID*>(&real_GetRawInputData))) == MH_OK) MH_EnableHook(getRawInput); } // Hook RawInput.
            if (peekMsgW) { if (MH_CreateHook(peekMsgW, (LPVOID)&Hook_PeekMessageW, (reinterpret_cast<LPVOID*>(&real_PeekMessageW))) == MH_OK) MH_EnableHook(peekMsgW); } // Hook Peek W.
            if (peekMsgA) { if (MH_CreateHook(peekMsgA, (LPVOID)&Hook_PeekMessageA, (reinterpret_cast<LPVOID*>(&real_PeekMessageA))) == MH_OK) MH_EnableHook(peekMsgA); } // Hook Peek A.
            if (getMsgW) { if (MH_CreateHook(getMsgW, (LPVOID)&Hook_GetMessageW, (reinterpret_cast<LPVOID*>(&real_GetMessageW))) == MH_OK) MH_EnableHook(getMsgW); } // Hook Get W.
            if (getMsgA) { if (MH_CreateHook(getMsgA, (LPVOID)&Hook_GetMessageA, (reinterpret_cast<LPVOID*>(&real_GetMessageA))) == MH_OK) MH_EnableHook(getMsgA); } // Hook Get A.
            if (dispMsgW) { if (MH_CreateHook(dispMsgW, (LPVOID)&Hook_DispatchMessageW, (reinterpret_cast<LPVOID*>(&real_DispatchMessageW))) == MH_OK) MH_EnableHook(dispMsgW); } // Hook Dispatch W.
            if (dispMsgA) { if (MH_CreateHook(dispMsgA, (LPVOID)&Hook_DispatchMessageA, (reinterpret_cast<LPVOID*>(&real_DispatchMessageA))) == MH_OK) MH_EnableHook(dispMsgA); } // Hook Dispatch A.
            
            if (getAsync) { if (MH_CreateHook(getAsync, (LPVOID)&Hook_GetAsyncKeyState, (reinterpret_cast<LPVOID*>(&real_GetAsyncKeyState))) == MH_OK) MH_EnableHook(getAsync); } // Hook async key.
            if (getKey) { if (MH_CreateHook(getKey, (LPVOID)&Hook_GetKeyState, (reinterpret_cast<LPVOID*>(&real_GetKeyState))) == MH_OK) MH_EnableHook(getKey); } // Hook sync key.
            if (getKbdState) { if (MH_CreateHook(getKbdState, (LPVOID)&Hook_GetKeyboardState, (reinterpret_cast<LPVOID*>(&real_GetKeyboardState))) == MH_OK) MH_EnableHook(getKbdState); } // Hook kb block.
            if (getCurPos) { if (MH_CreateHook(getCurPos, (LPVOID)&Hook_GetCursorPos, (reinterpret_cast<LPVOID*>(&real_GetCursorPos))) == MH_OK) MH_EnableHook(getCurPos); } // Hook get pos.
            if (setCurPos) { if (MH_CreateHook(setCurPos, (LPVOID)&Hook_SetCursorPos, (reinterpret_cast<LPVOID*>(&real_SetCursorPos))) == MH_OK) MH_EnableHook(setCurPos); } // Hook set pos.
        }
        
        HMODULE hKernel32 = GetModuleHandleA("kernel32.dll"); // Look up kernel32 for file/loader ops.
        if (hKernel32) { // Ensure kernel32 exists.
            void* rf = GetProcAddress(hKernel32, "ReadFile"); // Find ReadFile.
            void* gor = GetProcAddress(hKernel32, "GetOverlappedResult"); // Find GetOverlappedResult.
            void* llw = GetProcAddress(hKernel32, "LoadLibraryW"); // Find LoadLibraryW.
            void* llexw = GetProcAddress(hKernel32, "LoadLibraryExW"); // Find LoadLibraryExW.
            void* lla = GetProcAddress(hKernel32, "LoadLibraryA"); // Find LoadLibraryA.
            void* llexa = GetProcAddress(hKernel32, "LoadLibraryExA"); // Find LoadLibraryExA.
            void* cfa = GetProcAddress(hKernel32, "CreateFileA"); // Find CreateFileA.
            void* cfw = GetProcAddress(hKernel32, "CreateFileW"); // Find CreateFileW.
            void* cf2 = GetProcAddress(hKernel32, "CreateFile2"); // Find CreateFile2.
            void* dio = GetProcAddress(hKernel32, "DeviceIoControl"); // Find DeviceIoControl.
            void* gorex = GetProcAddress(hKernel32, "GetOverlappedResultEx"); // Find GetOverlappedResultEx.
            
            if (rf) { if (MH_CreateHook(rf, (LPVOID)&Hook_ReadFile, (reinterpret_cast<LPVOID*>(&real_ReadFile))) == MH_OK) MH_EnableHook(rf); } // Hook ReadFile.
            if (gor) { if (MH_CreateHook(gor, (LPVOID)&Hook_GetOverlappedResult, (reinterpret_cast<LPVOID*>(&real_GetOverlappedResult))) == MH_OK) MH_EnableHook(gor); } // Hook GetOverlappedResult.
            if (llw) { if (MH_CreateHook(llw, (LPVOID)&Hook_LoadLibraryW, (reinterpret_cast<LPVOID*>(&real_LoadLibraryW))) == MH_OK) MH_EnableHook(llw); } // Hook LoadLibW.
            if (llexw) { if (MH_CreateHook(llexw, (LPVOID)&Hook_LoadLibraryExW, (reinterpret_cast<LPVOID*>(&real_LoadLibraryExW))) == MH_OK) MH_EnableHook(llexw); } // Hook LoadLibExW.
            if (lla) { if (MH_CreateHook(lla, (LPVOID)&Hook_LoadLibraryA, (reinterpret_cast<LPVOID*>(&real_LoadLibraryA))) == MH_OK) MH_EnableHook(lla); } // Hook LoadLibA.
            if (llexa) { if (MH_CreateHook(llexa, (LPVOID)&Hook_LoadLibraryExA, (reinterpret_cast<LPVOID*>(&real_LoadLibraryExA))) == MH_OK) MH_EnableHook(llexa); } // Hook LoadLibExA.
            if (cfa) { if (MH_CreateHook(cfa, (LPVOID)&Hook_CreateFileA, (reinterpret_cast<LPVOID*>(&real_CreateFileA))) == MH_OK) MH_EnableHook(cfa); } // Hook CreateFileA.
            if (cfw) { if (MH_CreateHook(cfw, (LPVOID)&Hook_CreateFileW, (reinterpret_cast<LPVOID*>(&real_CreateFileW))) == MH_OK) MH_EnableHook(cfw); } // Hook CreateFileW.
            if (cf2) { if (MH_CreateHook(cf2, (LPVOID)&Hook_CreateFile2, (reinterpret_cast<LPVOID*>(&real_CreateFile2))) == MH_OK) MH_EnableHook(cf2); } // Hook CreateFile2.
            if (dio) { if (MH_CreateHook(dio, (LPVOID)&Hook_DeviceIoControl, (reinterpret_cast<LPVOID*>(&real_DeviceIoControl))) == MH_OK) MH_EnableHook(dio); } // Hook DeviceIoControl.
            if (gorex) { if (MH_CreateHook(gorex, (LPVOID)&Hook_GetOverlappedResultEx, (reinterpret_cast<LPVOID*>(&real_GetOverlappedResultEx))) == MH_OK) MH_EnableHook(gorex); } // Hook GetOverlappedResultEx.
        }
        
        HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll"); // Look up ntdll for lowest-level handle closure
        if (hNtDll) {
            void* pNtClose = GetProcAddress(hNtDll, "NtClose");
            if (pNtClose) {
                if (MH_CreateHook(pNtClose, (LPVOID)&Hook_NtClose, (reinterpret_cast<LPVOID*>(&real_NtClose))) == MH_OK) MH_EnableHook(pNtClose);
            }
        }
        
        HMODULE hCombase = GetModuleHandleA("combase.dll"); // Look up combase for WinRT hooking.
        if (hCombase) { // Ensure combase exists.
            void* roGet = GetProcAddress(hCombase, "RoGetActivationFactory"); // Find WinRT factory instantiator.
            ptr_WindowsGetStringRawBuffer = (WindowsGetStringRawBuffer_t)GetProcAddress(hCombase, "WindowsGetStringRawBuffer"); // Find string extractor.
            if (roGet && ptr_WindowsGetStringRawBuffer) { // Validate pointers.
                if (MH_CreateHook(roGet, (LPVOID)&Hook_RoGetActivationFactory, (reinterpret_cast<LPVOID*>(&real_RoGetActivationFactory))) == MH_OK) MH_EnableHook(roGet); // Hook factory.
            }
        }
        
        HANDLE hThread = CreateThread(NULL, 0, BackgroundWorker, NULL, 0, NULL); // Kick off the background watchdog thread.
        if (hThread) CloseHandle(hThread); // Close the handle to the thread as we don't need to join/wait on it.
    }
    return TRUE; // Tell the OS the DLL initialized flawlessly.
}


