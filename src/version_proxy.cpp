// ============================================================================
// STALKER 2: VERSION.DLL GLOBAL PROXY & XINPUT/RAWINPUT FIREWALL
// Architecture: MASM Naked Assembly Forwarder + MinHook Background Thread
// ============================================================================
// This DLL masquerades as the system 'version.dll' to bypass UE5's SetDefaultDllDirectories
// anti-hijacking mechanics. It uses MASM naked jumps to perfectly forward all 17 
// VERSION API calls to the real Windows file, while secretly spawning a background thread 
// to MinHook XInput and RawInput.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mutex>
#include <atomic>
#include <stdio.h>
#include "MinHook.h"
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

#include "version_pointers.h"


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
    void* m_pObj; void (*m_Func)(void*, void*);
    virtual void Run(void* pvParam) override { m_Func(m_pObj, pvParam); }
    virtual void Run(void* pvParam, bool bIOFailure, HSteamPipe hSteamPipe) override { m_Func(m_pObj, pvParam); }
    virtual int GetCallbackSizeBytes() override { return sizeof(GameOverlayActivated_t); }
    struct GameOverlayActivated_t { uint8_t m_bActive; };
};
typedef void (__cdecl *SteamAPI_RegisterCallback_t)(CCallbackBase* pCallback, int iCallback);
typedef void (__cdecl *SteamAPI_UnregisterCallback_t)(CCallbackBase* pCallback);
#pragma pack(pop)

std::atomic<bool> g_bIsSteamOverlayActive(false);
std::atomic<bool> g_bIsGameInFocus(true);

bool ShouldBlockInput() {
    return g_bIsSteamOverlayActive.load(std::memory_order_relaxed) || !g_bIsGameInFocus.load(std::memory_order_relaxed);
}

// ============================================================================
// STEAM OVERLAY LISTENER
// Purpose: Hooks into the Steam API to detect when Shift+Tab is pressed.
// Note: We intentionally leak this object on DLL_PROCESS_DETACH to prevent 
// crash-on-exit race conditions with steam_api64.dll.
// ============================================================================
class CSteamOverlayListener {
public:
    CSteamOverlayListener() {
        m_CallbackImpl.m_pObj = this;
        m_CallbackImpl.m_nCallbackFlags = 0; // CRITICAL: Must be 0 so Steam doesn't ignore it
        m_CallbackImpl.m_iCallback = 331;
        m_CallbackImpl.m_Func = [](void* obj, void* param) {
            static_cast<CSteamOverlayListener*>(obj)->OnGameOverlayActivated(
                static_cast<CCallbackImpl::GameOverlayActivated_t*>(param));
        };
        
        HMODULE hSteam = GetModuleHandleA("steam_api64.dll");
        if (hSteam) {
            auto reg = (SteamAPI_RegisterCallback_t)GetProcAddress(hSteam, "SteamAPI_RegisterCallback");
            if (reg) reg(&m_CallbackImpl, m_CallbackImpl.m_iCallback);
            
            FILE* f;
            if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) {
                fprintf(f, "Registered Steam Callback 331\n");
                fclose(f);
            }
        }
    }
    void OnGameOverlayActivated(CCallbackImpl::GameOverlayActivated_t* pCallback) {
        g_bIsSteamOverlayActive.store(pCallback->m_bActive != 0, std::memory_order_release);
        
        FILE* f;
        if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) {
            fprintf(f, "OVERLAY TRIGGERED: %d\n", pCallback->m_bActive);
            fclose(f);
        }
    }
private:
    CCallbackImpl m_CallbackImpl;
};

CSteamOverlayListener* g_pSteamListener = nullptr;
std::once_flag g_SteamInitOnceFlag;

// ============================================================================
// STEAM CALLER WHITELIST (Trevigintuple Guard)
// Purpose: When the overlay is open, we must block the Game from reading inputs,
// but ALLOW the Steam Overlay itself to read them so you can navigate the UI.
// Mechanism: Uses the CPU _ReturnAddress() to check if the caller lives inside
// a known Steam DLL memory space.
// ============================================================================
void NeutralizeDualSensePacket(PBYTE buf, DWORD size);
// Constructs a perfect, flawless hardware spoof that explicitly sets the Touchpad to "Not Touching",
// neutralizes all sticks and buttons, and bypasses IOCP.
void SynthesizeNeutralDualSensePacket(PBYTE buf, DWORD size) {
    if (!buf || size < 64) return;
    memset(buf, 0, size);
    
    // USB Report ID
    buf[0] = 0x01;
    
    // Joysticks (Centered = 128)
    buf[1] = 0x80; buf[2] = 0x80; buf[3] = 0x80; buf[4] = 0x80;
    
    // Triggers (Unpressed = 0)
    buf[5] = 0x00; buf[6] = 0x00;
    
    // Sequence Counter
    buf[7] = 0x00;
    
    // Buttons (Byte 8 = D-Pad and Shapes. 0x08 = D-Pad Neutral, Shapes unpressed)
    // BUG FIX: I previously set buf[5] to 0x08 because I confused the DualSense layout 
    // with the DualShock 4 layout. The DualSense puts triggers in bytes 5/6 and buttons in byte 8!
    buf[8] = 0x08;
    buf[9] = 0x00;
    buf[10] = 0x00;
    
    // Gyroscope (Centered = 0)
    // buf[16] to buf[21] already 0 from memset
    
    // Accelerometer (X, Y = 0. Z = 8192 for 1G Gravity to prevent NaN math)
    // BUG FIX: Freefall (0G) causes Unreal Engine to compute NaN for orientation!
    if (size >= 28) {
        buf[26] = 0x00;
        buf[27] = 0x20;
    }
    
    // Touchpad 1 (Set to Not Touching, and mathematically centered at 960x540)
    // BUG FIX: The previous pure-zero memset forced "Is Touching = Active" at coordinates 0,0 (Top Left).
    if (size >= 37) {
        buf[33] = 0x80; // Not Touching
        buf[34] = 0xC0; // X Low
        buf[35] = 0xC3; // X High + Y Low
        buf[36] = 0x21; // Y High
    }
    
    // Touchpad 2
    if (size >= 41) {
        buf[37] = 0x80; // Not Touching
        buf[38] = 0xC0; // X Low
        buf[39] = 0xC3; // X High + Y Low
        buf[40] = 0x21; // Y High
    }
}
bool IsCallerSteam(void* callerAddress) {
    HMODULE hCaller = NULL;
    // Returns FALSE if caller is unmapped JIT memory (e.g. Anti-Cheat/Mods)
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)callerAddress, &hCaller);
    if (!hCaller) return false;
    
    static HMODULE hOverlay = NULL;
    static HMODULE hApi = NULL;
    static HMODULE hClient = NULL;
    static bool bInitialized = false;
    
    if (hCaller == hOverlay || hCaller == hApi || (hClient && hCaller == hClient)) return true;
    
    // Cache miss handling: Only run GetModuleHandle once. If steamclient64.dll 
    // isn't injected into the game, we cache the NULL so we don't spam the OS.
    if (!bInitialized) {
        hOverlay = GetModuleHandleA("GameOverlayRenderer64.dll");
        hApi = GetModuleHandleA("steam_api64.dll");
        hClient = GetModuleHandleA("steamclient64.dll");
        bInitialized = true;
    }
    
    return (hCaller == hOverlay || hCaller == hApi || (hClient && hCaller == hClient));
}



// --- Hook Typedefs ---
typedef UINT (WINAPI *GetRawInputData_t)(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);
GetRawInputData_t real_GetRawInputData = nullptr;

typedef DWORD (WINAPI *XInputGetState_t)(DWORD dwUserIndex, void* pState);
XInputGetState_t real_XInputGetState = nullptr;

typedef DWORD (WINAPI *XInputSetState_t)(DWORD dwUserIndex, void* pVibration);
XInputSetState_t real_XInputSetState = nullptr;

typedef DWORD (WINAPI *XInputGetKeystroke_t)(DWORD dwUserIndex, DWORD dwReserved, void* pKeystroke);
XInputGetKeystroke_t real_XInputGetKeystroke = nullptr;

typedef DWORD (WINAPI *XInputGetStateEx_t)(DWORD dwUserIndex, void* pState);
XInputGetStateEx_t real_XInputGetStateEx = nullptr;


// ============================================================================
// HOOK IMPLEMENTATIONS
// Purpose: Completely sever the Game's connection to physical and emulated
// controllers while the Steam Overlay is active.
// ============================================================================


#include <unordered_set>
#include <shared_mutex>
std::unordered_set<HANDLE> g_RawInputSonyHandles;
std::unordered_set<HANDLE> g_RawInputNonSonyHandles;
std::shared_mutex g_RawInputMutex;

bool IsRawInputSony(HANDLE hDevice) {
    if (!hDevice) return false;
    
    {
        std::shared_lock<std::shared_mutex> lock(g_RawInputMutex);
        if (g_RawInputSonyHandles.find(hDevice) != g_RawInputSonyHandles.end()) return true;
        if (g_RawInputNonSonyHandles.find(hDevice) != g_RawInputNonSonyHandles.end()) return false;
    }
    
    UINT size = 0;
    GetRawInputDeviceInfoA(hDevice, RIDI_DEVICENAME, nullptr, &size);
    if (size > 0 && size < 1024) {
        char name[1024] = {0};
        if (GetRawInputDeviceInfoA(hDevice, RIDI_DEVICENAME, name, &size) != (UINT)-1) {
            std::string s(name);
            for (auto& c : s) c = tolower(c);
            
            std::unique_lock<std::shared_mutex> lock(g_RawInputMutex);
            if (s.find("vid_054c") != std::string::npos) {
                g_RawInputSonyHandles.insert(hDevice);
                return true;
            } else {
                g_RawInputNonSonyHandles.insert(hDevice);
                return false;
            }
        }
    }
    return false;
}

UINT WINAPI Hook_GetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader) {
    UINT result = real_GetRawInputData(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
    if (result != (UINT)-1 && pData && uiCommand == RID_INPUT && ShouldBlockInput()) {
        RAWINPUT* raw = (RAWINPUT*)pData;
        if (raw->header.dwType == RIM_TYPEHID) {
            DWORD reportSize = raw->data.hid.dwSizeHid * raw->data.hid.dwCount;
            if ((reportSize == 64 || reportSize == 78) && IsRawInputSony(raw->header.hDevice)) {
                SynthesizeNeutralDualSensePacket((PBYTE)raw->data.hid.bRawData, reportSize);
            }
        }
    }
    return result;
}

// ============================================================================
// KEYBOARD / MOUSE / WM_INPUT MESSAGE FILTER
// Purpose: Block synthesized Desktop Configuration inputs from Steam
// ============================================================================

void NullifyMessage(LPMSG lpMsg) {
    if (lpMsg && ShouldBlockInput()) {
        if ((lpMsg->message >= WM_KEYFIRST && lpMsg->message <= WM_KEYLAST) ||
            (lpMsg->message >= WM_MOUSEFIRST && lpMsg->message <= WM_MOUSELAST) ||
            (lpMsg->message == WM_INPUT)) {
            lpMsg->message = WM_NULL;
        }
    }
}

typedef BOOL (WINAPI *PeekMessageW_t)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
PeekMessageW_t real_PeekMessageW = nullptr;
BOOL WINAPI Hook_PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg) {
    BOOL result = real_PeekMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
    if (result) NullifyMessage(lpMsg);
    return result;
}

typedef BOOL (WINAPI *PeekMessageA_t)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
PeekMessageA_t real_PeekMessageA = nullptr;
BOOL WINAPI Hook_PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg) {
    BOOL result = real_PeekMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
    if (result) NullifyMessage(lpMsg);
    return result;
}

typedef BOOL (WINAPI *GetMessageW_t)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
GetMessageW_t real_GetMessageW = nullptr;
BOOL WINAPI Hook_GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax) {
    BOOL result = real_GetMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax);
    if (result != -1 && result != 0) NullifyMessage(lpMsg);
    return result;
}

typedef BOOL (WINAPI *GetMessageA_t)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
GetMessageA_t real_GetMessageA = nullptr;
BOOL WINAPI Hook_GetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax) {
    BOOL result = real_GetMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax);
    if (result != -1 && result != 0) NullifyMessage(lpMsg);
    return result;
}

typedef LRESULT (WINAPI *DispatchMessageW_t)(const MSG* lpMsg);
DispatchMessageW_t real_DispatchMessageW = nullptr;
LRESULT WINAPI Hook_DispatchMessageW(const MSG* lpMsg) {
    if (lpMsg && ShouldBlockInput()) {
        if ((lpMsg->message >= WM_KEYFIRST && lpMsg->message <= WM_KEYLAST) ||
            (lpMsg->message >= WM_MOUSEFIRST && lpMsg->message <= WM_MOUSELAST) ||
            (lpMsg->message == WM_INPUT)) {
            return 0;
        }
    }
    return real_DispatchMessageW(lpMsg);
}

typedef LRESULT (WINAPI *DispatchMessageA_t)(const MSG* lpMsg);
DispatchMessageA_t real_DispatchMessageA = nullptr;
LRESULT WINAPI Hook_DispatchMessageA(const MSG* lpMsg) {
    if (lpMsg && ShouldBlockInput()) {
        if ((lpMsg->message >= WM_KEYFIRST && lpMsg->message <= WM_KEYLAST) ||
            (lpMsg->message >= WM_MOUSEFIRST && lpMsg->message <= WM_MOUSELAST) ||
            (lpMsg->message == WM_INPUT)) {
            return 0;
        }
    }
    return real_DispatchMessageA(lpMsg);
}
// ============================================================================
// XINPUT INTERCEPTOR
// ============================================================================
DWORD WINAPI Hook_XInputGetState(DWORD dwUserIndex, void* pState) {
    DWORD res = real_XInputGetState(dwUserIndex, pState);
    if (res == ERROR_SUCCESS && pState && ShouldBlockInput()) {
        memset(pState, 0, 16);
    }
    return res;
}

DWORD WINAPI Hook_XInputSetState(DWORD dwUserIndex, void* pVibration) {
    if (ShouldBlockInput()) {
        return 1167;
    }
    return real_XInputSetState(dwUserIndex, pVibration);
}

DWORD WINAPI Hook_XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved, void* pKeystroke) {
    if (ShouldBlockInput()) {
        if (pKeystroke) memset(pKeystroke, 0, 8);
        return 1167; 
    }
    return real_XInputGetKeystroke(dwUserIndex, dwReserved, pKeystroke);
}

DWORD WINAPI Hook_XInputGetStateEx(DWORD dwUserIndex, void* pState) {
    DWORD res = real_XInputGetStateEx(dwUserIndex, pState);
    if (res == ERROR_SUCCESS && pState && ShouldBlockInput()) {
        memset(pState, 0, 16);
    }
    return res;
}


// ============================================================================
// MOUSE AND KEYBOARD STATE INTERCEPTORS (Desktop Configuration Bypass)
// ============================================================================
typedef BOOL (WINAPI *GetCursorPos_t)(LPPOINT lpPoint);
GetCursorPos_t real_GetCursorPos = nullptr;
BOOL WINAPI Hook_GetCursorPos(LPPOINT lpPoint) {
    BOOL result = real_GetCursorPos(lpPoint);
    if (result && ShouldBlockInput()) {
        // Freeze the cursor at 0,0 for the game
        if (lpPoint) { lpPoint->x = 0; lpPoint->y = 0; }
    }
    return result;
}

typedef BOOL (WINAPI *SetCursorPos_t)(int X, int Y);
SetCursorPos_t real_SetCursorPos = nullptr;
BOOL WINAPI Hook_SetCursorPos(int X, int Y) {
    if (ShouldBlockInput()) {
        return TRUE; // Ignore sets
    }
    return real_SetCursorPos(X, Y);
}

typedef SHORT (WINAPI *GetAsyncKeyState_t)(int vKey);
GetAsyncKeyState_t real_GetAsyncKeyState = nullptr;
SHORT WINAPI Hook_GetAsyncKeyState(int vKey) {
    if (ShouldBlockInput()) return 0;
    return real_GetAsyncKeyState(vKey);
}

typedef SHORT (WINAPI *GetKeyState_t)(int vKey);
GetKeyState_t real_GetKeyState = nullptr;
SHORT WINAPI Hook_GetKeyState(int vKey) {
    if (ShouldBlockInput()) return 0;
    return real_GetKeyState(vKey);
}

typedef BOOL (WINAPI *GetKeyboardState_t)(PBYTE lpKeyState);
GetKeyboardState_t real_GetKeyboardState = nullptr;
BOOL WINAPI Hook_GetKeyboardState(PBYTE lpKeyState) {
    BOOL result = real_GetKeyboardState(lpKeyState);
    if (result && ShouldBlockInput() && lpKeyState) {
        memset(lpKeyState, 0, 256);
    }
    return result;
}

// ============================================================================
// MESSAGE QUEUE INTERCEPTORS (User32)
// ============================================================================
// GAMEINPUT INTERCEPTOR (Microsoft GDK)
// ============================================================================
typedef HRESULT (WINAPI *IGameInput_GetCurrentReading_t)(void* pThis, UINT32 inputKind, void* device, void** reading);
IGameInput_GetCurrentReading_t real_IGameInput_GetCurrentReading = nullptr;

typedef HRESULT (WINAPI *IGameInput_GetNextReading_t)(void* pThis, void* refReading, UINT32 inputKind, void* device, void** reading);
IGameInput_GetNextReading_t real_IGameInput_GetNextReading = nullptr;
IGameInput_GetNextReading_t real_IGameInput_GetPreviousReading = nullptr;

HRESULT WINAPI Hook_IGameInput_GetCurrentReading(void* pThis, UINT32 inputKind, void* device, void** reading) {
    if (ShouldBlockInput()) {
        static std::once_flag log_flag;
        std::call_once(log_flag, []() {
            FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fprintf(f, "BLOCKED: GameInput GetCurrentReading intercepted successfully!\n"); fclose(f); }
        });
        if (reading) *reading = nullptr;
        return 0x8007048F; // ERROR_DEVICE_NOT_CONNECTED
    }
    return real_IGameInput_GetCurrentReading(pThis, inputKind, device, reading);
}

HRESULT WINAPI Hook_IGameInput_GetNextReading(void* pThis, void* refReading, UINT32 inputKind, void* device, void** reading) {
    if (ShouldBlockInput()) {
        static std::once_flag log_flag;
        std::call_once(log_flag, []() {
            FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fprintf(f, "BLOCKED: GameInput GetNextReading intercepted successfully!\n"); fclose(f); }
        });
        if (reading) *reading = nullptr;
        return 0x8007048F;
    }
    return real_IGameInput_GetNextReading(pThis, refReading, inputKind, device, reading);
}

HRESULT WINAPI Hook_IGameInput_GetPreviousReading(void* pThis, void* refReading, UINT32 inputKind, void* device, void** reading) {
    if (ShouldBlockInput()) {
        static std::once_flag log_flag;
        std::call_once(log_flag, []() {
            FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fprintf(f, "BLOCKED: GameInput GetPreviousReading intercepted successfully!\n"); fclose(f); }
        });
        if (reading) *reading = nullptr;
        return 0x8007048F;
    }
    return real_IGameInput_GetPreviousReading(pThis, refReading, inputKind, device, reading);
}

typedef HRESULT (WINAPI *GameInputCreate_t)(void** gameInput);
GameInputCreate_t real_GameInputCreate = nullptr;
HRESULT WINAPI Hook_GameInputCreate(void** gameInput) {
    FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fprintf(f, "API DETECT: GameInputCreate Called!\n"); fclose(f); }
    HRESULT hr = real_GameInputCreate(gameInput);
    if (SUCCEEDED(hr) && gameInput && *gameInput) {
        void** vtable = *(void***)(*gameInput);
        if (MH_CreateHook(vtable[4], (LPVOID)&Hook_IGameInput_GetCurrentReading, (reinterpret_cast<LPVOID*>(&real_IGameInput_GetCurrentReading))) == MH_OK) MH_EnableHook(vtable[4]);
        if (MH_CreateHook(vtable[5], (LPVOID)&Hook_IGameInput_GetNextReading, (reinterpret_cast<LPVOID*>(&real_IGameInput_GetNextReading))) == MH_OK) MH_EnableHook(vtable[5]);
        if (MH_CreateHook(vtable[6], (LPVOID)&Hook_IGameInput_GetPreviousReading, (reinterpret_cast<LPVOID*>(&real_IGameInput_GetPreviousReading))) == MH_OK) MH_EnableHook(vtable[6]);
    }
    return hr;
}

// ============================================================================
// WGI INTERCEPTOR (Windows.Gaming.Input)
// ============================================================================
typedef HRESULT (WINAPI* GetCurrentReading_t)(void* pThis, UINT32 bLen, BOOLEAN* bArr, UINT32 sLen, void* sArr, UINT32 aLen, double* aArr, UINT64* ts);
GetCurrentReading_t real_GetCurrentReading = nullptr;
HRESULT WINAPI Hook_GetCurrentReading(void* pThis, UINT32 bLen, BOOLEAN* bArr, UINT32 sLen, void* sArr, UINT32 aLen, double* aArr, UINT64* ts) {
    HRESULT hr = real_GetCurrentReading(pThis, bLen, bArr, sLen, sArr, aLen, aArr, ts);
    if (SUCCEEDED(hr) && ShouldBlockInput()) {
        if (bArr && bLen > 0) memset(bArr, 0, bLen * sizeof(BOOLEAN));
        if (sArr && sLen > 0) memset(sArr, 0, sLen * sizeof(void*));
        if (aArr && aLen > 0) for (UINT32 i = 0; i < aLen; ++i) aArr[i] = 0.5;
    }
    return hr;
}

typedef HRESULT (WINAPI* EventHandler_Invoke_t)(void* pThis, void* sender, void* args);
EventHandler_Invoke_t real_EventHandler_Invoke = nullptr;
HRESULT WINAPI Hook_EventHandler_Invoke(void* pThis, void* sender, void* args) {
    if (args) {
        void** vtable = *(void***)args;
        void* get_reading = vtable[12];
        if (MH_CreateHook(get_reading, (LPVOID)&Hook_GetCurrentReading, (reinterpret_cast<LPVOID*>(&real_GetCurrentReading))) == MH_OK) MH_EnableHook(get_reading);
    }
    return real_EventHandler_Invoke(pThis, sender, args);
}

typedef HRESULT (WINAPI* add_RawGameControllerAdded_t)(void* pThis, void* eventHandler, void* token);
add_RawGameControllerAdded_t real_add_RawGameControllerAdded = nullptr;
HRESULT WINAPI Hook_add_RawGameControllerAdded(void* pThis, void* eventHandler, void* token) {
    if (eventHandler) {
        void** vtable = *(void***)eventHandler;
        void* invoke = vtable[3];
        if (MH_CreateHook(invoke, (LPVOID)&Hook_EventHandler_Invoke, (reinterpret_cast<LPVOID*>(&real_EventHandler_Invoke))) == MH_OK) MH_EnableHook(invoke);
    }
    return real_add_RawGameControllerAdded(pThis, eventHandler, token);
}

typedef HRESULT (WINAPI *RoGetActivationFactory_t)(void* activatableClassId, REFIID iid, void** factory);
RoGetActivationFactory_t real_RoGetActivationFactory = nullptr;
typedef PCWSTR (WINAPI *WindowsGetStringRawBuffer_t)(void* string, UINT32* length);
WindowsGetStringRawBuffer_t ptr_WindowsGetStringRawBuffer = nullptr;

HRESULT WINAPI Hook_RoGetActivationFactory(void* activatableClassId, REFIID iid, void** factory) {
    if (activatableClassId && ptr_WindowsGetStringRawBuffer) {
        PCWSTR className = ptr_WindowsGetStringRawBuffer(activatableClassId, nullptr);
        if (className) {
            FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fwprintf(f, L"RoGetActivationFactory Called: %s\n", className); fclose(f); }
        }
    }
    
    HRESULT hr = real_RoGetActivationFactory(activatableClassId, iid, factory);
    if (SUCCEEDED(hr) && factory && *factory && activatableClassId && ptr_WindowsGetStringRawBuffer) {
        PCWSTR className = ptr_WindowsGetStringRawBuffer(activatableClassId, nullptr);
        if (className && wcsstr(className, L"Windows.Gaming.Input.RawGameController") != nullptr) {
            void** vtable = *(void***)(*factory);
            void* add_event = vtable[6];
            if (MH_CreateHook(add_event, (LPVOID)&Hook_add_RawGameControllerAdded, (reinterpret_cast<LPVOID*>(&real_add_RawGameControllerAdded))) == MH_OK) MH_EnableHook(add_event);
        }
    }
    return hr;
}


// ============================================================================
// BACKGROUND WORKER THREAD
// ============================================================================
DWORD WINAPI BackgroundWorker(LPVOID lpParam) {
    bool xinputHooked = false;
    bool gameinputHooked = false;
    DWORD currentProcessId = GetCurrentProcessId();
    
    while (true) {
        // Track Focus State efficiently
        HWND hForeground = GetForegroundWindow();
        if (hForeground) {
            DWORD foregroundProcessId = 0;
            GetWindowThreadProcessId(hForeground, &foregroundProcessId);
            g_bIsGameInFocus.store((foregroundProcessId == currentProcessId), std::memory_order_relaxed);
        } else {
            g_bIsGameInFocus.store(false, std::memory_order_relaxed);
        }

        if (!g_pSteamListener) {
            if (GetModuleHandleA("steam_api64.dll") != NULL) {
                std::call_once(g_SteamInitOnceFlag, []() {
                    g_pSteamListener = new CSteamOverlayListener();
                });
            }
        }
        
        if (ShouldBlockInput()) {
            if (GetModuleHandleA("GameOverlayRenderer64.dll") == NULL) {
                g_bIsSteamOverlayActive.store(false, std::memory_order_release);
                FILE* f;
                if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) {
                    fprintf(f, "WATCHDOG: Steam crashed! Force-disabled firewall.\n");
                    fclose(f);
                }
            }
        }
        
        if (!xinputHooked) {
            char sysDir[MAX_PATH];
            if (GetSystemDirectoryA(sysDir, MAX_PATH)) {
                strcat_s(sysDir, sizeof(sysDir), "\\xinput1_4.dll");
                HMODULE hXInput = LoadLibraryExA(sysDir, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!hXInput) {
                    GetSystemDirectoryA(sysDir, MAX_PATH);
                    strcat_s(sysDir, sizeof(sysDir), "\\xinput1_3.dll");
                    hXInput = LoadLibraryExA(sysDir, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
                }
                
                if (hXInput) {
                    void* xget = GetProcAddress(hXInput, "XInputGetState");
                    void* xset = GetProcAddress(hXInput, "XInputSetState");
                    void* xkey = GetProcAddress(hXInput, "XInputGetKeystroke");
                    void* xgetex = GetProcAddress(hXInput, (LPCSTR)100);
                    
                    if (xget) { if (MH_CreateHook(xget, (LPVOID)&Hook_XInputGetState, (reinterpret_cast<LPVOID*>(&real_XInputGetState))) == MH_OK) MH_EnableHook(xget); }
                    if (xset) { if (MH_CreateHook(xset, (LPVOID)&Hook_XInputSetState, (reinterpret_cast<LPVOID*>(&real_XInputSetState))) == MH_OK) MH_EnableHook(xset); }
                    if (xkey) { if (MH_CreateHook(xkey, (LPVOID)&Hook_XInputGetKeystroke, (reinterpret_cast<LPVOID*>(&real_XInputGetKeystroke))) == MH_OK) MH_EnableHook(xkey); }
                    if (xgetex) { if (MH_CreateHook(xgetex, (LPVOID)&Hook_XInputGetStateEx, (reinterpret_cast<LPVOID*>(&real_XInputGetStateEx))) == MH_OK) MH_EnableHook(xgetex); }
                    
                    xinputHooked = true;
                    FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fprintf(f, "XInput dynamically hooked by BackgroundWorker!\n"); fclose(f); }
                }
            }
        }
        
        if (!gameinputHooked) {
            char sysDir[MAX_PATH];
            if (GetSystemDirectoryA(sysDir, MAX_PATH)) {
                strcat_s(sysDir, sizeof(sysDir), "\\gameinput.dll");
                HMODULE hGameInput = LoadLibraryExA(sysDir, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (hGameInput) {
                    void* gic = GetProcAddress(hGameInput, "GameInputCreate");
                    if (gic) {
                        if (MH_CreateHook(gic, (LPVOID)&Hook_GameInputCreate, (reinterpret_cast<LPVOID*>(&real_GameInputCreate))) == MH_OK) MH_EnableHook(gic);
                        gameinputHooked = true;
                        FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fprintf(f, "GameInput dynamically hooked by BackgroundWorker!\n"); fclose(f); }
                    }
                }
            }
        }
        
        Sleep(50);
    }
    return 0;
}

// ============================================================================
// FILE IO AND HID CREATION INTERCEPTORS
// ============================================================================
#include <unordered_set>
#include <shared_mutex>

std::unordered_set<HANDLE> g_HidHandles;
std::shared_mutex g_HidMutex;

void RegisterHidHandle(HANDLE h, LPCWSTR name) {
    if (h != INVALID_HANDLE_VALUE && name) {
        std::wstring s(name);
        for (auto& c : s) c = towlower(c);
        if (s.find(L"vid_054c") != std::wstring::npos) {
            std::unique_lock<std::shared_mutex> lock(g_HidMutex);
            g_HidHandles.insert(h);
            FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fwprintf(f, L"Registered HID Handle (W): %s\n", name); fclose(f); }
        }
    }
}

void RegisterHidHandleA(HANDLE h, LPCSTR name) {
    if (h != INVALID_HANDLE_VALUE && name) {
        std::string s(name);
        for (auto& c : s) c = tolower(c);
        if (s.find("vid_054c") != std::string::npos) {
            std::unique_lock<std::shared_mutex> lock(g_HidMutex);
            g_HidHandles.insert(h);
            FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fprintf(f, "Registered HID Handle (A): %s\n", name); fclose(f); }
        }
    }
}

bool IsHidHandle(HANDLE h) {
    std::shared_lock<std::shared_mutex> lock(g_HidMutex);
    return g_HidHandles.find(h) != g_HidHandles.end();
}

typedef HANDLE (WINAPI *CreateFileW_t)(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
CreateFileW_t real_CreateFileW = nullptr;
HANDLE WINAPI Hook_CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    bool isHid = false;
    if (lpFileName) {
        std::wstring s(lpFileName);
        for (auto& c : s) c = towlower(c);
        if (s.find(L"vid_054c") != std::wstring::npos) isHid = true;
    }
    
    HANDLE h = real_CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
    RegisterHidHandle(h, lpFileName);
    return h;
}

typedef HANDLE (WINAPI *CreateFile2_t)(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, DWORD dwCreationDisposition, void* pCreateExParams);
CreateFile2_t real_CreateFile2 = nullptr;
HANDLE WINAPI Hook_CreateFile2(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, DWORD dwCreationDisposition, void* pCreateExParams) {
    bool isHid = false;
    if (lpFileName) {
        std::wstring s(lpFileName);
        for (auto& c : s) c = towlower(c);
        if (s.find(L"vid_054c") != std::wstring::npos) isHid = true;
    }
    
    HANDLE h = real_CreateFile2(lpFileName, dwDesiredAccess, dwShareMode, dwCreationDisposition, pCreateExParams);
    RegisterHidHandle(h, lpFileName);
    return h;
}

typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
CreateFileA_t real_CreateFileA = nullptr;
HANDLE WINAPI Hook_CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    bool isHid = false;
    if (lpFileName) {
        std::string s(lpFileName);
        for (auto& c : s) c = tolower(c);
        if (s.find("vid_054c") != std::string::npos) isHid = true;
    }
    
    HANDLE h = real_CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
    RegisterHidHandleA(h, lpFileName);
    return h;
}


typedef BOOL (WINAPI *DeviceIoControl_t)(HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer, DWORD nInBufferSize, LPVOID lpOutBuffer, DWORD nOutBufferSize, LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped);
DeviceIoControl_t real_DeviceIoControl = nullptr;
BOOL WINAPI Hook_DeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer, DWORD nInBufferSize, LPVOID lpOutBuffer, DWORD nOutBufferSize, LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped) {
    if (ShouldBlockInput() && IsHidHandle(hDevice)) {
        if (lpOutBuffer && nOutBufferSize >= 64) {
            SynthesizeNeutralDualSensePacket((PBYTE)lpOutBuffer, nOutBufferSize);
        }
        if (lpBytesReturned) *lpBytesReturned = nOutBufferSize >= 64 ? 64 : nOutBufferSize;
        if (lpOverlapped) {
            lpOverlapped->Internal = 0;
            lpOverlapped->InternalHigh = nOutBufferSize >= 64 ? 64 : nOutBufferSize;
            if (lpOverlapped->hEvent) SetEvent(lpOverlapped->hEvent);
        }
        return TRUE;
    }
    return real_DeviceIoControl(hDevice, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer, nOutBufferSize, lpBytesReturned, lpOverlapped);
}

typedef BOOL (WINAPI *ReadFile_t)(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped);
ReadFile_t real_ReadFile = nullptr;
BOOL WINAPI Hook_ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped) {
    if (ShouldBlockInput() && IsHidHandle(hFile)) {
        if (lpBuffer && nNumberOfBytesToRead >= 64) {
            SynthesizeNeutralDualSensePacket((PBYTE)lpBuffer, nNumberOfBytesToRead);
        }
        DWORD bytesRead = nNumberOfBytesToRead >= 64 ? 64 : nNumberOfBytesToRead;
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = bytesRead;
        if (lpOverlapped) {
            lpOverlapped->Internal = 0;
            lpOverlapped->InternalHigh = bytesRead;
            if (lpOverlapped->hEvent) SetEvent(lpOverlapped->hEvent);
        }
        return TRUE;
    }
    return real_ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
}

typedef BOOL (WINAPI *GetOverlappedResult_t)(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait);
GetOverlappedResult_t real_GetOverlappedResult = nullptr;
BOOL WINAPI Hook_GetOverlappedResult(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait) {
    if (ShouldBlockInput() && IsHidHandle(hFile)) {
        if (lpNumberOfBytesTransferred) *lpNumberOfBytesTransferred = 64;
        return TRUE;
    }
    return real_GetOverlappedResult(hFile, lpOverlapped, lpNumberOfBytesTransferred, bWait);
}

typedef BOOL (WINAPI *GetOverlappedResultEx_t)(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, DWORD dwMilliseconds, BOOL bAlertable);
GetOverlappedResultEx_t real_GetOverlappedResultEx = nullptr;
BOOL WINAPI Hook_GetOverlappedResultEx(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, DWORD dwMilliseconds, BOOL bAlertable) {
    if (ShouldBlockInput() && IsHidHandle(hFile)) {
        if (lpNumberOfBytesTransferred) *lpNumberOfBytesTransferred = 64;
        return TRUE;
    }
    return real_GetOverlappedResultEx(hFile, lpOverlapped, lpNumberOfBytesTransferred, dwMilliseconds, bAlertable);
}

// ============================================================================
// XINPUT INTERCEPTOR
// ============================================================================
// ============================================================================
// DYNAMIC LOADER INTERCEPTORS (LoadLibrary)
// ============================================================================
void CheckAndHookDynamicLibsW(HMODULE hModule, LPCWSTR lpLibFileName) {
    if (!hModule || !lpLibFileName) return;
    
    if (wcsstr(lpLibFileName, L"gameinput.dll") || wcsstr(lpLibFileName, L"GameInput.dll") || wcsstr(lpLibFileName, L"GAMEINPUT.DLL")) {
        void* gic = GetProcAddress(hModule, "GameInputCreate");
        if (gic) {
            if (MH_CreateHook(gic, (LPVOID)&Hook_GameInputCreate, (reinterpret_cast<LPVOID*>(&real_GameInputCreate))) == MH_OK) {
                MH_EnableHook(gic);
                FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fwprintf(f, L"GameInput hooked via LoadLibraryW interception! (%s)\n", lpLibFileName); fclose(f); }
            }
        }
    }
}

void CheckAndHookDynamicLibsA(HMODULE hModule, LPCSTR lpLibFileName) {
    if (!hModule || !lpLibFileName) return;
    
    if (strstr(lpLibFileName, "gameinput.dll") || strstr(lpLibFileName, "GameInput.dll") || strstr(lpLibFileName, "GAMEINPUT.DLL")) {
        void* gic = GetProcAddress(hModule, "GameInputCreate");
        if (gic) {
            if (MH_CreateHook(gic, (LPVOID)&Hook_GameInputCreate, (reinterpret_cast<LPVOID*>(&real_GameInputCreate))) == MH_OK) {
                MH_EnableHook(gic);
                FILE* f; if (fopen_s(&f, "proxy_loaded.txt", "a") == 0) { fprintf(f, "GameInput hooked via LoadLibraryA interception! (%s)\n", lpLibFileName); fclose(f); }
            }
        }
    }
}

typedef HMODULE (WINAPI *LoadLibraryExW_t)(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
LoadLibraryExW_t real_LoadLibraryExW = nullptr;
HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE hModule = real_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    CheckAndHookDynamicLibsW(hModule, lpLibFileName);
    return hModule;
}

typedef HMODULE (WINAPI *LoadLibraryW_t)(LPCWSTR lpLibFileName);
LoadLibraryW_t real_LoadLibraryW = nullptr;
HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR lpLibFileName) {
    HMODULE hModule = real_LoadLibraryW(lpLibFileName);
    CheckAndHookDynamicLibsW(hModule, lpLibFileName);
    return hModule;
}

typedef HMODULE (WINAPI *LoadLibraryExA_t)(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
LoadLibraryExA_t real_LoadLibraryExA = nullptr;
HMODULE WINAPI Hook_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE hModule = real_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    CheckAndHookDynamicLibsA(hModule, lpLibFileName);
    return hModule;
}

typedef HMODULE (WINAPI *LoadLibraryA_t)(LPCSTR lpLibFileName);
LoadLibraryA_t real_LoadLibraryA = nullptr;
HMODULE WINAPI Hook_LoadLibraryA(LPCSTR lpLibFileName) {
    HMODULE hModule = real_LoadLibraryA(lpLibFileName);
    CheckAndHookDynamicLibsA(hModule, lpLibFileName);
    return hModule;
}

// ============================================================================
// DLL ENTRY POINT
// ============================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        
        FILE* f;
        if (fopen_s(&f, "proxy_loaded.txt", "w") == 0) {
            fprintf(f, "VERSION_PROXY LOADED SUCCESSFULLY! Synchronous hooks initialized.\n");
            fclose(f);
        }
        
        LoadRealVersion();
        
        HMODULE hSelf;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)&DllMain, &hSelf);
        
        MH_Initialize();
        
        // --- SYNCHRONOUS HOOKS (DllMain) ---
        HMODULE hUser32 = GetModuleHandleA("user32.dll");
        if (hUser32) {
            void* getRawInput = GetProcAddress(hUser32, "GetRawInputData");
            void* peekMsgW = GetProcAddress(hUser32, "PeekMessageW");
            void* peekMsgA = GetProcAddress(hUser32, "PeekMessageA");
            void* getMsgW = GetProcAddress(hUser32, "GetMessageW");
            void* getMsgA = GetProcAddress(hUser32, "GetMessageA");
            void* dispMsgW = GetProcAddress(hUser32, "DispatchMessageW");
            void* dispMsgA = GetProcAddress(hUser32, "DispatchMessageA");
            
            void* getAsync = GetProcAddress(hUser32, "GetAsyncKeyState");
            void* getKey = GetProcAddress(hUser32, "GetKeyState");
            void* getKbdState = GetProcAddress(hUser32, "GetKeyboardState");
            void* getCurPos = GetProcAddress(hUser32, "GetCursorPos");
            void* setCurPos = GetProcAddress(hUser32, "SetCursorPos");
            
            if (getRawInput) { if (MH_CreateHook(getRawInput, (LPVOID)&Hook_GetRawInputData, (reinterpret_cast<LPVOID*>(&real_GetRawInputData))) == MH_OK) MH_EnableHook(getRawInput); }
            if (peekMsgW) { if (MH_CreateHook(peekMsgW, (LPVOID)&Hook_PeekMessageW, (reinterpret_cast<LPVOID*>(&real_PeekMessageW))) == MH_OK) MH_EnableHook(peekMsgW); }
            if (peekMsgA) { if (MH_CreateHook(peekMsgA, (LPVOID)&Hook_PeekMessageA, (reinterpret_cast<LPVOID*>(&real_PeekMessageA))) == MH_OK) MH_EnableHook(peekMsgA); }
            if (getMsgW) { if (MH_CreateHook(getMsgW, (LPVOID)&Hook_GetMessageW, (reinterpret_cast<LPVOID*>(&real_GetMessageW))) == MH_OK) MH_EnableHook(getMsgW); }
            if (getMsgA) { if (MH_CreateHook(getMsgA, (LPVOID)&Hook_GetMessageA, (reinterpret_cast<LPVOID*>(&real_GetMessageA))) == MH_OK) MH_EnableHook(getMsgA); }
            if (dispMsgW) { if (MH_CreateHook(dispMsgW, (LPVOID)&Hook_DispatchMessageW, (reinterpret_cast<LPVOID*>(&real_DispatchMessageW))) == MH_OK) MH_EnableHook(dispMsgW); }
            if (dispMsgA) { if (MH_CreateHook(dispMsgA, (LPVOID)&Hook_DispatchMessageA, (reinterpret_cast<LPVOID*>(&real_DispatchMessageA))) == MH_OK) MH_EnableHook(dispMsgA); }
            
            if (getAsync) { if (MH_CreateHook(getAsync, (LPVOID)&Hook_GetAsyncKeyState, (reinterpret_cast<LPVOID*>(&real_GetAsyncKeyState))) == MH_OK) MH_EnableHook(getAsync); }
            if (getKey) { if (MH_CreateHook(getKey, (LPVOID)&Hook_GetKeyState, (reinterpret_cast<LPVOID*>(&real_GetKeyState))) == MH_OK) MH_EnableHook(getKey); }
            if (getKbdState) { if (MH_CreateHook(getKbdState, (LPVOID)&Hook_GetKeyboardState, (reinterpret_cast<LPVOID*>(&real_GetKeyboardState))) == MH_OK) MH_EnableHook(getKbdState); }
            if (getCurPos) { if (MH_CreateHook(getCurPos, (LPVOID)&Hook_GetCursorPos, (reinterpret_cast<LPVOID*>(&real_GetCursorPos))) == MH_OK) MH_EnableHook(getCurPos); }
            if (setCurPos) { if (MH_CreateHook(setCurPos, (LPVOID)&Hook_SetCursorPos, (reinterpret_cast<LPVOID*>(&real_SetCursorPos))) == MH_OK) MH_EnableHook(setCurPos); }
        }
        
        HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
        if (hKernel32) {
            void* rf = GetProcAddress(hKernel32, "ReadFile");
            void* gor = GetProcAddress(hKernel32, "GetOverlappedResult");
            void* llw = GetProcAddress(hKernel32, "LoadLibraryW");
            void* llexw = GetProcAddress(hKernel32, "LoadLibraryExW");
            void* lla = GetProcAddress(hKernel32, "LoadLibraryA");
            void* llexa = GetProcAddress(hKernel32, "LoadLibraryExA");
            void* cfa = GetProcAddress(hKernel32, "CreateFileA");
            void* cfw = GetProcAddress(hKernel32, "CreateFileW");
            void* cf2 = GetProcAddress(hKernel32, "CreateFile2");
            void* dio = GetProcAddress(hKernel32, "DeviceIoControl");
            void* gorex = GetProcAddress(hKernel32, "GetOverlappedResultEx");
            
            if (rf) { if (MH_CreateHook(rf, (LPVOID)&Hook_ReadFile, (reinterpret_cast<LPVOID*>(&real_ReadFile))) == MH_OK) MH_EnableHook(rf); }
            if (gor) { if (MH_CreateHook(gor, (LPVOID)&Hook_GetOverlappedResult, (reinterpret_cast<LPVOID*>(&real_GetOverlappedResult))) == MH_OK) MH_EnableHook(gor); }
            if (llw) { if (MH_CreateHook(llw, (LPVOID)&Hook_LoadLibraryW, (reinterpret_cast<LPVOID*>(&real_LoadLibraryW))) == MH_OK) MH_EnableHook(llw); }
            if (llexw) { if (MH_CreateHook(llexw, (LPVOID)&Hook_LoadLibraryExW, (reinterpret_cast<LPVOID*>(&real_LoadLibraryExW))) == MH_OK) MH_EnableHook(llexw); }
            if (lla) { if (MH_CreateHook(lla, (LPVOID)&Hook_LoadLibraryA, (reinterpret_cast<LPVOID*>(&real_LoadLibraryA))) == MH_OK) MH_EnableHook(lla); }
            if (llexa) { if (MH_CreateHook(llexa, (LPVOID)&Hook_LoadLibraryExA, (reinterpret_cast<LPVOID*>(&real_LoadLibraryExA))) == MH_OK) MH_EnableHook(llexa); }
            if (cfa) { if (MH_CreateHook(cfa, (LPVOID)&Hook_CreateFileA, (reinterpret_cast<LPVOID*>(&real_CreateFileA))) == MH_OK) MH_EnableHook(cfa); }
            if (cfw) { if (MH_CreateHook(cfw, (LPVOID)&Hook_CreateFileW, (reinterpret_cast<LPVOID*>(&real_CreateFileW))) == MH_OK) MH_EnableHook(cfw); }
            if (cf2) { if (MH_CreateHook(cf2, (LPVOID)&Hook_CreateFile2, (reinterpret_cast<LPVOID*>(&real_CreateFile2))) == MH_OK) MH_EnableHook(cf2); }
            if (dio) { if (MH_CreateHook(dio, (LPVOID)&Hook_DeviceIoControl, (reinterpret_cast<LPVOID*>(&real_DeviceIoControl))) == MH_OK) MH_EnableHook(dio); }
            if (gorex) { if (MH_CreateHook(gorex, (LPVOID)&Hook_GetOverlappedResultEx, (reinterpret_cast<LPVOID*>(&real_GetOverlappedResultEx))) == MH_OK) MH_EnableHook(gorex); }
        }
        
        HMODULE hCombase = GetModuleHandleA("combase.dll");
        if (hCombase) {
            void* roGet = GetProcAddress(hCombase, "RoGetActivationFactory");
            ptr_WindowsGetStringRawBuffer = (WindowsGetStringRawBuffer_t)GetProcAddress(hCombase, "WindowsGetStringRawBuffer");
            if (roGet && ptr_WindowsGetStringRawBuffer) {
                if (MH_CreateHook(roGet, (LPVOID)&Hook_RoGetActivationFactory, (reinterpret_cast<LPVOID*>(&real_RoGetActivationFactory))) == MH_OK) MH_EnableHook(roGet);
            }
        }
        
        HANDLE hThread = CreateThread(NULL, 0, BackgroundWorker, NULL, 0, NULL);
        if (hThread) CloseHandle(hThread);
    }
    return TRUE;
}


