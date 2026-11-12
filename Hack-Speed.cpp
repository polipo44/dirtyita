#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"

#define D3D9_RESET_INDEX 16
#define D3D9_ENDSCENE_INDEX 42

#define GWORLD_OFFSET           0x22361A0
#define EXCEPTION_INFO_PTR      0x222DE28
#define APAWN_WEAPON            0x940
#define ASGPLAYERCONTROLLER_SGPAWN   0x17a8

#define ASGWEAPON_PROJECTILESPERSSHOT  0x920
#define ASGWEAPON_NORECOIL             0x13d8
#define ASGWEAPON_NOSPREAD_A           0x13dc
#define ASGWEAPON_NOSPREAD_B           0x13e0

// Speed Hack offsets - APawn UE3
#define APAWN_VELOCITY          0x1A0
#define APAWN_GROUNDSPEED       0x2B0

#define OFF_PC_SGUIHM            0x18bc
#define OFF_SGUIHM_PLAYERHUD     0xb44
#define OFF_PLAYERHUD_IFFHUD     0x113c
#define OFF_IFFMGR_ICONS         0x1b0
#define OFF_IFFOBJ_FLAGS         0x204
#define MASK_IFFOBJ_SAME_TEAM       (1u << 8)
#define MASK_IFFOBJ_USE_LOS_TRACES  (1u << 10)

uintptr_t g_Base = 0;
uintptr_t g_PC = 0;
bool bInit = false;
bool bShowMenu = false;
HWND hWindow = nullptr;
WNDPROC oWndProc = nullptr;

bool  g_NoRecoil = false;
bool  g_NoSpread = false;
float g_CustomSpreadValue = 0.0f;
bool  g_ProjectilesPerShotEnabled = false;
int   g_ProjectilesPerShotValue = 99;

bool  g_SpeedHack = false;
float g_SpeedMultiplier = 2.0f;
bool  g_SpeedHack_AlternateTick = false;

bool  g_SpecESP            = false;
bool  g_SpecESP_ForceTeam  = true;

int   g_SpecESP_MarkerCount = 0;
int   g_SpecESP_Patched     = 0;
uintptr_t g_SpecESP_LastHM  = 0;
uintptr_t g_SpecESP_LastHUD = 0;
uintptr_t g_SpecESP_LastIFF = 0;

IDirect3DDevice9* g_pDevice = nullptr;
bool ValidPtr(uintptr_t p) { return p >= 0x10000 && p < 0x00007FFFFFFFFFFF; }

struct LogLine { std::string text; float color[4]; };
std::vector<LogLine> g_Log;
void PushLog(float r, float g, float b, const char* fmt, ...) {
    char buf[1024]; va_list args; va_start(args, fmt); vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    LogLine l; l.text = buf; l.color[0]=r; l.color[1]=g; l.color[2]=b; l.color[3]=1.0f;
    g_Log.push_back(l); if (g_Log.size() > 1000) g_Log.erase(g_Log.begin());
}
#define Log(...)    PushLog(0.9f, 0.9f, 0.9f, __VA_ARGS__)
#define LogOk(...)  PushLog(0.3f, 1.0f, 0.3f, __VA_ARGS__)
#define LogErr(...) PushLog(1.0f, 0.3f, 0.3f, __VA_ARGS__)

std::string BuildLogDump() {
    std::string s; char tmp[256];
    s += "=== Dirty Bomb Tweaks Log Dump ===\n";
    snprintf(tmp, sizeof(tmp), "Base: 0x%llx\n", (unsigned long long)g_Base); s += tmp;
    snprintf(tmp, sizeof(tmp), "PC:   0x%llx\n", (unsigned long long)g_PC);   s += tmp;
    s += "\nToggles:\n";
    s += std::string("  NoRecoil=") + (g_NoRecoil ? "1" : "0") + "\n";
    s += std::string("  NoSpread=") + (g_NoSpread ? "1" : "0") + " val=" + std::to_string(g_CustomSpreadValue) + "\n";
    s += std::string("  ProjectilesPerShot=") + (g_ProjectilesPerShotEnabled ? "1" : "0") + " value=" + std::to_string(g_ProjectilesPerShotValue) + "\n";
    s += std::string("  SpeedHack=") + (g_SpeedHack ? "1" : "0") + " mult=" + std::to_string(g_SpeedMultiplier) + "\n";
    s += std::string("  SpecESP=") + (g_SpecESP ? "1" : "0") + "\n";
    s += std::string("  SpecESP_ForceTeam=") + (g_SpecESP_ForceTeam ? "1" : "0") + "\n";
    s += "\nIFF chain:\n";
    snprintf(tmp, sizeof(tmp), "  SGUIHM=0x%llx  PlayerHUD=0x%llx  IFF=0x%llx\n", (unsigned long long)g_SpecESP_LastHM, (unsigned long long)g_SpecESP_LastHUD, (unsigned long long)g_SpecESP_LastIFF); s += tmp;
    snprintf(tmp, sizeof(tmp), "  Markers=%d  Patched=%d\n", g_SpecESP_MarkerCount, g_SpecESP_Patched); s += tmp;
    s += "\nLog:\n"; for (auto& l : g_Log) { s += l.text; s += "\n"; } return s;
}
void CopyLogToClipboard() {
    std::string dump = BuildLogDump();
    if (!OpenClipboard(nullptr)) { LogErr("[!] OpenClipboard failed"); return; }
    EmptyClipboard(); HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, dump.size() + 1);
    if (h) { char* p = (char*)GlobalLock(h); memcpy(p, dump.c_str(), dump.size() + 1); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); }
    CloseClipboard(); LogOk("[+] Log copied to clipboard (%zu bytes)", dump.size());
}
void SaveConfig() {
    FILE* f; if (fopen_s(&f, "config.ini", "w") == 0) {
        fprintf(f, "NoRecoil=%d\n", g_NoRecoil); fprintf(f, "NoSpread=%d\n", g_NoSpread);
        fprintf(f, "CustomSpreadValue=%.4f\n", g_CustomSpreadValue);
        fprintf(f, "ProjectilesPerShot=%d\n", g_ProjectilesPerShotEnabled);
        fprintf(f, "ProjectilesPerShotValue=%d\n", g_ProjectilesPerShotValue);
        fprintf(f, "SpeedHack=%d\n", g_SpeedHack); fprintf(f, "SpeedMultiplier=%.2f\n", g_SpeedMultiplier);
        fprintf(f, "SpecESP=%d\n", g_SpecESP); fprintf(f, "SpecESP_ForceTeam=%d\n", g_SpecESP_ForceTeam); fclose(f);
    } LogOk("[+] Config saved");
}
void LoadConfig() {
    FILE* f; if (fopen_s(&f, "config.ini", "r") == 0) { char line[256];
        while (fgets(line, sizeof(line), f)) {
            if      (strncmp(line, "NoRecoil=", 9) == 0)                 g_NoRecoil = atoi(line + 9);
            else if (strncmp(line, "NoSpread=", 9) == 0)                 g_NoSpread = atoi(line + 9);
            else if (strncmp(line, "CustomSpreadValue=", 18) == 0)       g_CustomSpreadValue = (float)atof(line + 18);
            else if (strncmp(line, "ProjectilesPerShot=", 19) == 0)      g_ProjectilesPerShotEnabled = atoi(line + 19);
            else if (strncmp(line, "ProjectilesPerShotValue=", 24) == 0) g_ProjectilesPerShotValue = atoi(line + 24);
            else if (strncmp(line, "SpeedHack=", 10) == 0)               g_SpeedHack = atoi(line + 10);
            else if (strncmp(line, "SpeedMultiplier=", 16) == 0)         g_SpeedMultiplier = (float)atof(line + 16);
            else if (strncmp(line, "SpecESP=", 8) == 0)                  g_SpecESP = atoi(line + 8);
            else if (strncmp(line, "SpecESP_ForceTeam=", 18) == 0)       g_SpecESP_ForceTeam = atoi(line + 18);
        } fclose(f); Log("[+] Config loaded");
    }
}
uintptr_t GetLocalPC() {
    if (!g_Base) return 0; uintptr_t root = *(uintptr_t*)(g_Base + EXCEPTION_INFO_PTR);
    if (!ValidPtr(root)) return 0; uintptr_t lpArray = *(uintptr_t*)(root + 0x750);
    if (!ValidPtr(lpArray)) return 0; uintptr_t lp = *(uintptr_t*)lpArray;
    if (!ValidPtr(lp)) return 0; uintptr_t pc = *(uintptr_t*)(lp + 0x68); return ValidPtr(pc) ? pc : 0;
}
bool WriteTArrayInt32First(uintptr_t arrayBase, int32_t value) {
    if (!ValidPtr(arrayBase)) return false; uintptr_t data = *(uintptr_t*)(arrayBase + 0x0);
    int32_t count = *(int32_t*)(arrayBase + 0x8); int32_t maxc = *(int32_t*)(arrayBase + 0xC);
    if (!ValidPtr(data)) return false; if (count <= 0 || count > 8) return false; if (maxc < count || maxc > 32) return false;
    int32_t cur = *(int32_t*)data; if (cur < 0 || cur > 256) return false; *(int32_t*)data = value; return true;
}
void ApplyWeaponTweaks(uintptr_t weapon) {
    if (!ValidPtr(weapon)) return;
    if (g_NoRecoil) *(float*)(weapon + ASGWEAPON_NORECOIL) = 0.0f;
    if (g_NoSpread) { *(float*)(weapon + ASGWEAPON_NOSPREAD_A) = g_CustomSpreadValue; *(float*)(weapon + ASGWEAPON_NOSPREAD_B) = g_CustomSpreadValue; }
    if (g_ProjectilesPerShotEnabled) {
        static uintptr_t lastLoggedWeapon = 0;
        bool ok = WriteTArrayInt32First(weapon + ASGWEAPON_PROJECTILESPERSSHOT, g_ProjectilesPerShotValue);
        if (weapon != lastLoggedWeapon) { lastLoggedWeapon = weapon;
            if (ok) LogOk("[pps] weapon=0x%llx -> %d written", (unsigned long long)weapon, g_ProjectilesPerShotValue);
            else LogErr("[pps] weapon=0x%llx -> TArray sanity check failed, skipped", (unsigned long long)weapon);
        }
    }
}
void ApplySpeedHack(uintptr_t pawn) {
    if (!ValidPtr(pawn)) return; if (!g_SpeedHack) return;
    float* vel = (float*)(pawn + APAWN_VELOCITY);
    float speedSq = vel[0]*vel[0] + vel[1]*vel[1] + vel[2]*vel[2];
    if (speedSq < 1.0f) return;
    if (g_SpeedHack_AlternateTick) { static int tick = 0; tick++; if (tick % 2 != 0) return; }
    vel[0] *= g_SpeedMultiplier; vel[1] *= g_SpeedMultiplier;
}
struct TArrayRaw { void** Data; int32_t Count; int32_t Max; };
static void ApplyIFFSpectatorView(bool enable) {
    g_SpecESP_MarkerCount = 0; g_SpecESP_Patched = 0; g_SpecESP_LastHM = 0; g_SpecESP_LastHUD = 0; g_SpecESP_LastIFF = 0;
    if (!ValidPtr(g_PC)) return;
    uintptr_t hm = *reinterpret_cast<uintptr_t*>(g_PC + OFF_PC_SGUIHM); g_SpecESP_LastHM = hm; if (!ValidPtr(hm)) return;
    uintptr_t hud = *reinterpret_cast<uintptr_t*>(hm + OFF_SGUIHM_PLAYERHUD); g_SpecESP_LastHUD = hud; if (!ValidPtr(hud)) return;
    uintptr_t iff = *reinterpret_cast<uintptr_t*>(hud + OFF_PLAYERHUD_IFFHUD); g_SpecESP_LastIFF = iff; if (!ValidPtr(iff)) return;
    TArrayRaw* icons = reinterpret_cast<TArrayRaw*>(iff + OFF_IFFMGR_ICONS);
    if (!icons->Data || icons->Count <= 0 || icons->Count > 256) return;
    g_SpecESP_MarkerCount = icons->Count;
    for (int i = 0; i < icons->Count; ++i) {
        uintptr_t obj = reinterpret_cast<uintptr_t>(icons->Data[i]); if (!ValidPtr(obj)) continue;
        uint32_t* f = reinterpret_cast<uint32_t*>(obj + OFF_IFFOBJ_FLAGS);
        if (enable) { *f &= ~MASK_IFFOBJ_USE_LOS_TRACES; if (g_SpecESP_ForceTeam) *f |= MASK_IFFOBJ_SAME_TEAM; }
        else { *f |= MASK_IFFOBJ_USE_LOS_TRACES; *f &= ~MASK_IFFOBJ_SAME_TEAM; }
        g_SpecESP_Patched++;
    }
}
void RunGameLogic() {
    if (!g_Base) return; g_PC = GetLocalPC(); if (!ValidPtr(g_PC)) return;
    ApplyIFFSpectatorView(g_SpecESP);
    uintptr_t localPawn = *(uintptr_t*)(g_PC + ASGPLAYERCONTROLLER_SGPAWN); if (!ValidPtr(localPawn)) return;
    ApplySpeedHack(localPawn);
    uintptr_t weapon = *(uintptr_t*)(localPawn + APAWN_WEAPON); if (!ValidPtr(weapon)) return;
    ApplyWeaponTweaks(weapon);
}
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN && wParam == VK_INSERT) { bShowMenu = !bShowMenu; if (bShowMenu) { ClipCursor(NULL); ShowCursor(TRUE); } return 0; }
    if (bShowMenu) { ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
        switch (uMsg) { case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_INPUT: return 1; default: break; } }
    return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}
void DrawGUI() {
    ImGui::NewFrame(); ImGuiIO& io = ImGui::GetIO(); io.MouseDrawCursor = bShowMenu;
    if (bShowMenu) {
        if (hWindow) { POINT p; if (GetCursorPos(&p) && ScreenToClient(hWindow, &p)) io.MousePos = ImVec2((float)p.x, (float)p.y);
            io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0; io.MouseDown[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0; }
        ImGui::SetNextWindowSize(ImVec2(540, 700), ImGuiCond_FirstUseEver);
        ImGui::Begin("Dirty Bomb - Tweaks", &bShowMenu, ImGuiWindowFlags_NoCollapse);
        ImGui::Text("Weapon:"); ImGui::Separator();
        ImGui::Checkbox("No Recoil", &g_NoRecoil); ImGui::Checkbox("No Spread", &g_NoSpread);
        if (g_NoSpread) { ImGui::Indent(); ImGui::SliderFloat("Spread Value", &g_CustomSpreadValue, 0.0f, 2.0f, "%.3f"); ImGui::Unindent(); }
        ImGui::Spacing(); ImGui::Text("One Hit Kill:"); ImGui::Separator();
        ImGui::Checkbox("Projectiles Per Shot", &g_ProjectilesPerShotEnabled);
        if (g_ProjectilesPerShotEnabled) { ImGui::Indent(); ImGui::SliderInt("Value", &g_ProjectilesPerShotValue, 1, 200); ImGui::TextDisabled("Test with 99. Watch hit markers."); ImGui::Unindent(); }
        ImGui::Spacing(); ImGui::Text("Movement:"); ImGui::Separator();
        ImGui::Checkbox("Speed Hack", &g_SpeedHack);
        if (g_SpeedHack) { ImGui::Indent(); ImGui::SliderFloat("Speed Multiplier", &g_SpeedMultiplier, 1.0f, 5.0f, "x%.2f"); ImGui::Checkbox("Modalita leggera (1 frame si / 1 no)", &g_SpeedHack_AlternateTick); ImGui::TextDisabled("1.5x = leggero, 2.0x = consigliato, 3.0x+ = molto visibile"); ImGui::Unindent(); }
        ImGui::Spacing(); ImGui::Text("Spectator ESP (IFF wallhack):"); ImGui::Separator();
        ImGui::Checkbox("Enable Spec ESP", &g_SpecESP);
        if (g_SpecESP) { ImGui::Indent(); ImGui::Checkbox("Force friendly render", &g_SpecESP_ForceTeam); ImGui::TextDisabled("Clears m_UseLineOfSightTraces on every marker."); ImGui::TextDisabled("Force friendly: also sets m_bSameTeam for names.");
            if (g_SpecESP_LastIFF == 0) { if (g_SpecESP_LastHM == 0) ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "Chain: PC->SGUIHM failed"); else if (g_SpecESP_LastHUD == 0) ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "Chain: SGUIHM->PlayerHUD failed"); else ImGui::TextColored(ImVec4(1,0.6f,0.2f,1), "Chain: PlayerHUD->IFF failed"); }
            else if (g_SpecESP_MarkerCount == 0) ImGui::TextColored(ImVec4(1,0.8f,0.2f,1), "No markers (lobby / no players?)");
            else ImGui::TextColored(ImVec4(0.3f,1,0.3f,1), "Markers: %d  patched: %d", g_SpecESP_MarkerCount, g_SpecESP_Patched); ImGui::Unindent(); }
        else ImGui::TextDisabled("Enable to see players through walls");
        ImGui::Spacing(); ImGui::Separator();
        if (ImGui::Button("Save Config", ImVec2(160, 30))) SaveConfig(); ImGui::SameLine();
        if (ImGui::Button("Load Config", ImVec2(160, 30))) LoadConfig(); ImGui::SameLine();
        if (ImGui::Button("Copy Log", ImVec2(160, 30))) CopyLogToClipboard();
        ImGui::Spacing(); ImGui::Separator(); ImGui::Text("Log:");
        ImGui::BeginChild("##log", ImVec2(0, 200), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (auto& l : g_Log) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(l.color[0], l.color[1], l.color[2], l.color[3])); ImGui::TextUnformatted(l.text.c_str()); ImGui::PopStyleColor(); }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f); ImGui::EndChild(); ImGui::End();
    } ImGui::Render();
}
typedef HRESULT(WINAPI* EndScene_t)(IDirect3DDevice9*);
typedef HRESULT(WINAPI* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
EndScene_t oEndScene = nullptr; Reset_t oReset = nullptr;
HRESULT WINAPI hkEndScene(IDirect3DDevice9* d) {
    if (!bInit) { g_pDevice = d; hWindow = FindWindowA("LaunchUnrealUWindowsClient", NULL); if (!hWindow) hWindow = FindWindowA(NULL, "Dirty Bomb");
        oWndProc = (WNDPROC)SetWindowLongPtr(hWindow, GWLP_WNDPROC, (LONG_PTR)WndProc);
        ImGui::CreateContext(); ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui_ImplWin32_Init(hWindow); ImGui_ImplDX9_Init(d); bInit = true; Log("[+] Overlay initialized. Window=%p", hWindow); Log("[*] Base=0x%llx", (unsigned long long)g_Base); }
    static bool isPressed = false;
    if (GetAsyncKeyState(VK_INSERT) & 0x8000) { if (!isPressed) { bShowMenu = !bShowMenu; isPressed = true; } } else { isPressed = false; }
    RunGameLogic();
    IDirect3DStateBlock9* stateBlock = nullptr;
    if (d->CreateStateBlock(D3DSBT_ALL, &stateBlock) == S_OK) { ImGui_ImplDX9_NewFrame(); ImGui_ImplWin32_NewFrame(); DrawGUI(); ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData()); stateBlock->Apply(); stateBlock->Release(); }
    else { ImGui_ImplDX9_NewFrame(); ImGui_ImplWin32_NewFrame(); DrawGUI(); ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData()); }
    return oEndScene(d);
}
HRESULT WINAPI hkReset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) { if (bInit) ImGui_ImplDX9_InvalidateDeviceObjects(); HRESULT hr = oReset(d, pp); if (SUCCEEDED(hr) && bInit) ImGui_ImplDX9_CreateDeviceObjects(); return hr; }
DWORD WINAPI MainThread(LPVOID) {
    g_Base = (uintptr_t)GetModuleHandleA(NULL); LoadConfig();
    HWND hw = NULL; while (!hw) { hw = FindWindowA("LaunchUnrealUWindowsClient", NULL); if (!hw) hw = FindWindowA(NULL, "Dirty Bomb"); Sleep(500); }
    IDirect3D9* d9 = Direct3DCreate9(D3D_SDK_VERSION); if (!d9) return 0;
    D3DPRESENT_PARAMETERS dp = {0}; dp.Windowed = TRUE; dp.SwapEffect = D3DSWAPEFFECT_DISCARD; dp.hDeviceWindow = hw;
    IDirect3DDevice9* dev = NULL;
    if (SUCCEEDED(d9->CreateDevice(0, D3DDEVTYPE_HAL, hw, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &dp, &dev))) {
        void** vt = *(void***)dev; oEndScene = (EndScene_t)vt[D3D9_ENDSCENE_INDEX]; oReset = (Reset_t)vt[D3D9_RESET_INDEX];
        DWORD old; VirtualProtect(&vt[D3D9_ENDSCENE_INDEX], sizeof(void*), PAGE_EXECUTE_READWRITE, &old); vt[D3D9_ENDSCENE_INDEX] = (void*)hkEndScene; VirtualProtect(&vt[D3D9_ENDSCENE_INDEX], sizeof(void*), old, &old);
        VirtualProtect(&vt[D3D9_RESET_INDEX], sizeof(void*), PAGE_EXECUTE_READWRITE, &old); vt[D3D9_RESET_INDEX] = (void*)hkReset; VirtualProtect(&vt[D3D9_RESET_INDEX], sizeof(void*), old, &old); dev->Release();
    } d9->Release(); return 0;
}
extern "C" __declspec(dllexport) BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID l) { if (r == 1) CreateThread(0, 0, MainThread, 0, 0, 0); return 1; }
extern "C" {
    __declspec(dllexport) BOOL WINAPI GetFileVersionInfoA(LPCSTR l, DWORD d, DWORD dl, LPVOID lp) { return FALSE; }
    __declspec(dllexport) BOOL WINAPI GetFileVersionInfoW(LPCWSTR l, DWORD d, DWORD dl, LPVOID lp) { return FALSE; }
    __declspec(dllexport) DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR l, LPDWORD lp) { return 0; }
    __declspec(dllexport) DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR l, LPDWORD lp) { return 0; }
    __declspec(dllexport) BOOL WINAPI VerQueryValueA(LPCVOID p, LPCSTR lp, LPVOID* l, PUINT pu) { return FALSE; }
    __declspec(dllexport) BOOL WINAPI VerQueryValueW(LPCVOID p, LPCWSTR lp, LPVOID* l, PUINT pu) { return FALSE; }
}
