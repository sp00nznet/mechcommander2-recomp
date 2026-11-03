/*
 * MechCommander 2 - static recompilation host.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks
 * and the machine lock; see its header). The lifted Mc2Rel.exe calls the real
 * MSVCRT, binkw32 and (loaded at run time by GameOS) DirectDraw/Direct3D 7,
 * DirectSound and DirectInput through the bridge;
 * what is here is only what is specific to this game: the guest image, its
 * folder, its command line and the fault report. docs/architecture.md has
 * the reasoning.
 *
 * Linked at /BASE:0x60000000 (CMakeLists.txt) so 0x00400000 (Mc2Rel.exe) is
 * free when main() maps the image.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "recomp_trace.h"

extern const uint32_t mc2_entry_va;      /* recomp_dispatch.c */

#define MC2_BASE 0x00400000u

static DWORD g_watchdog_s;
static char  g_game[MAX_PATH];           /* the game folder, with a trailing '\' */
static char  g_guest_exe[MAX_PATH], g_guest_cmdline[MAX_PATH + 3];

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define RET(v, nargs) do { g_eax = (uint32_t)(v); g_esp += 4 + 4 * (nargs); } while (0)

/* ------------------------------------------------------------- the guest */

/* The guest is Mc2Rel.exe in the game folder, not this host: its hInstance
 * (icons, dialogs, strings) is GetModuleHandleA(NULL); the CRT startup passes
 * it on to WinMain. */
static void shim_GetModuleHandleA(void) {
    uint32_t name = ARG(0);
    RET(name ? (uint32_t)(uintptr_t)GetModuleHandleA((const char*)(uintptr_t)name) : MC2_BASE, 1);
}

/* GetModuleFileNameA for the guest's handle (or NULL): Mc2Rel.exe, not this
 * host. -1 if h is some other module. */
static int guest_file_name(uint32_t h, char* out, uint32_t size) {
    if (h != 0 && h != MC2_BASE) return -1;
    uint32_t n = (uint32_t)strlen(g_guest_exe);
    if (size) {
        if (n >= size) n = size - 1;
        memcpy(out, g_guest_exe, n);
        out[n] = 0;
    }
    return (int)n;
}

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0), size = ARG(2);
    char* out = (char*)(uintptr_t)ARG(1);
    int n = guest_file_name(h, out, size);
    RET(n >= 0 ? (uint32_t)n : GetModuleFileNameA((HMODULE)(uintptr_t)h, out, size), 3);
}

static void shim_GetCommandLineA(void) { RET((uintptr_t)g_guest_cmdline, 0); }

static void shim_ExitProcess(void) {
    fprintf(stderr, "[exit] ExitProcess(%u)\n", ARG(0));
    fflush(stderr);
    ExitProcess(ARG(0));
}

/* IsBadReadPtr/IsBadWritePtr probe the memory and catch their own fault with
 * SEH, but the crash handler (a vectored handler) sees the fault first and
 * ends the run. GameOS's ErrorHandler calls them while dumping memory.
 * Answer from VirtualQuery instead, which never faults.
 * ponytail: host-only for now; belongs in native32 as a built-in shim. */
static uint32_t bad_ptr(uint32_t p, uint32_t n, DWORD ok) {
    if (!n) return 0;
    if (!p) return 1;
    uint64_t a = p, end = (uint64_t)p + n;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void*)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
            (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mi.Protect & ok))
            return 1;
        a = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
    return 0;
}

#define READABLE (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | \
                  PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)
#define WRITABLE (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)
static void shim_IsBadReadPtr(void)  { RET(bad_ptr(ARG(0), ARG(1), READABLE), 2); }
static void shim_IsBadWritePtr(void) { RET(bad_ptr(ARG(0), ARG(1), WRITABLE), 2); }

/* GameOS loads dinput.dll itself and calls DirectInputCreateEx with its
 * hInstance, 0x00400000. That image is mapped, not loaded, so DirectInput
 * rejects it (E_INVALIDARG, "STOP : FAILED ... DirectInputCreateEx"). Give
 * it this host's handle instead. */
typedef HRESULT (WINAPI *dicreate_t)(HINSTANCE, DWORD, const void*, void**, void*);
static dicreate_t g_dicreate;

/* No joysticks. GameOS opens every game controller it finds
 * (CMCreateJoystick7, from IDirectInput7::EnumDevices) and that path faults
 * in ntdll with some devices (a USB microphone with HID buttons is one).
 * MechCommander 2 is played with the mouse and keyboard, so the enumeration
 * of joysticks finds none. ponytail: hides the fault rather than fixing it;
 * look at CMCreateJoystick7 if a pad is ever wanted. */
#define DIDEVTYPE_JOYSTICK 4
#define MC2_CMCREATEJOYSTICK7 0x005A0E70u   /* the PDB names it */
typedef HRESULT (WINAPI *dienum_t)(void*, DWORD, void*, void*, DWORD);
static dienum_t g_dienum;

static HRESULT WINAPI host_EnumDevices(void* di, DWORD type, void* cb, void* ref, DWORD flags) {
    /* GameOS enumerates every device (type 0) into CMCreateJoystick7, which
     * skips mice and keyboards itself: that callback only opens joysticks. */
    if ((type & 0xFF) == DIDEVTYPE_JOYSTICK || (uint32_t)(uintptr_t)cb == MC2_CMCREATEJOYSTICK7) return 0;
    return g_dienum(di, type, cb, ref, flags);
}

static HRESULT WINAPI host_DirectInputCreateEx(HINSTANCE h, DWORD ver, const void* iid, void** out, void* outer) {
    if ((uint32_t)(uintptr_t)h == MC2_BASE) h = GetModuleHandleA(NULL);
    HRESULT r = g_dicreate(h, ver, iid, out, outer);
    if (r == 0 && out && *out) {
        /* EnumDevices is IDirectInput7A's fifth method. Patched in dinput's
         * own vtable: DirectInput checks an object's vtable pointer is one of
         * its own (a copied table gets E_INVALIDARG from every call). */
        void** vt = *(void***)*out;
        if (vt[4] != (void*)host_EnumDevices) {
            DWORD old;
            g_dienum = (dienum_t)vt[4];
            VirtualProtect(&vt[4], sizeof *vt, PAGE_READWRITE, &old);
            vt[4] = (void*)host_EnumDevices;
            VirtualProtect(&vt[4], sizeof *vt, old, &old);
        }
    }
    return r;
}

static int g_ddtest;
static void ddtest(const char* stage);
static int g_have_display_guid;
typedef HRESULT (WINAPI *ddcreate_t)(GUID*, void**, const void*, void*);
static ddcreate_t g_ddcreate;
static HRESULT WINAPI host_DirectDrawCreateEx(GUID* g, void** out, const void* iid, void* outer);

static void shim_GetProcAddress(void) {
    HMODULE m = (HMODULE)(uintptr_t)ARG(0);
    const char* name = (const char*)(uintptr_t)ARG(1);
    FARPROC p = GetProcAddress(m, name);
    if (g_ddtest && (uintptr_t)name >= 0x10000 && !strcmp(name, "DirectDrawCreateEx")) ddtest("in the game");
    /* GameOS takes DirectDrawCreateEx from blade.dll, which passes it on to
     * ddraw.dll; either way --display picks the device. */
    if (p && g_have_display_guid && (uintptr_t)name >= 0x10000 && !strcmp(name, "DirectDrawCreateEx")) {
        g_ddcreate = (ddcreate_t)p;
        p = (FARPROC)host_DirectDrawCreateEx;
    }
    if (p && (uintptr_t)name >= 0x10000 && !strcmp(name, "DirectInputCreateEx")) {
        g_dicreate = (dicreate_t)p;
        p = (FARPROC)host_DirectInputCreateEx;
    }
    if (native32_trace_native) {
        char mod[MAX_PATH] = "?";
        GetModuleFileNameA(m, mod, sizeof mod);
        fprintf(stderr, "[getproc] %s!%s -> %p\n", mod, (uintptr_t)name >= 0x10000 ? name : "#", (void*)p);
    }
    RET((uintptr_t)p, 2);
}

/* --input FILE: scripted mouse, so a run can click through the menus without
 * the real cursor. Each line is "MS X Y B": from MS milliseconds after the
 * game starts, the mouse is at client X,Y of the game window with the left
 * button down (B=1) or up (B=0). The game reads the mouse with GetCursorPos
 * and GetAsyncKeyState/GetKeyState, and its window procedure sees button
 * messages; all three follow the script. Before the first line, and with no
 * script, the real mouse is used. */
#define MAX_INPUT 512
static struct { DWORD ms; int x, y, b; } g_in[MAX_INPUT];
static int g_nin, g_inb;
static DWORD g_in_t0;

static void load_input(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot read %s\n", path); exit(1); }
    while (g_nin < MAX_INPUT && fscanf(f, "%lu %d %d %d", &g_in[g_nin].ms, &g_in[g_nin].x,
                                       &g_in[g_nin].y, &g_in[g_nin].b) == 4)
        g_nin++;
    fclose(f);
}

static HWND game_window(void) { return FindWindowA(NULL, "MechCommander2\\1.0"); }

/* The script line in force now, or -1. Posts the button messages when the
 * button changes, so the window procedure sees the click too. */
static int input_now(POINT* screen) {
    if (!g_nin) return -1;
    DWORD t = GetTickCount() - g_in_t0;
    int i = -1;
    while (i + 1 < g_nin && g_in[i + 1].ms <= t) i++;
    if (i < 0) return -1;
    HWND w = game_window();
    /* The game's cursor sits where the script says when the point is taken
     * from the window's outer corner: measured from the client area it
     * landed a title bar's height (37 px) low. */
    RECT r = {0};
    if (w) GetWindowRect(w, &r);
    screen->x = r.left + g_in[i].x;
    screen->y = r.top + g_in[i].y;
    if (g_in[i].b != g_inb && w) {
        g_inb = g_in[i].b;
        PostMessageA(w, g_inb ? WM_LBUTTONDOWN : WM_LBUTTONUP, g_inb ? MK_LBUTTON : 0,
                     MAKELPARAM(g_in[i].x, g_in[i].y));
    }
    return i;
}

static void shim_GetCursorPos(void) {
    POINT* out = (POINT*)(uintptr_t)ARG(0);
    POINT p;
    if (input_now(&p) >= 0) { *out = p; RET(1, 1); return; }
    RET(GetCursorPos(out), 1);
}

static void shim_GetAsyncKeyState(void) {
    POINT p;
    int vk = (int)ARG(0);
    if (vk == VK_LBUTTON && input_now(&p) >= 0) { RET(g_inb ? 0x8000u : 0, 1); return; }
    RET((uint16_t)GetAsyncKeyState(vk), 1);
}

static void shim_GetKeyState(void) {
    POINT p;
    int vk = (int)ARG(0);
    if (vk == VK_LBUTTON && input_now(&p) >= 0) { RET(g_inb ? 0xFF80u : 0, 1); return; }
    RET((uint16_t)GetKeyState(vk), 1);
}

/* GameOS reports its errors through OutputDebugStringA: into the log. */
static void shim_OutputDebugStringA(void) {
    fprintf(stderr, "[debug] %s", (const char*)(uintptr_t)ARG(0));
    RET(0, 1);
}

static native32_shim_t g_shims[] = {
    { "OutputDebugStringA", shim_OutputDebugStringA },
    { "GetCursorPos", shim_GetCursorPos },
    { "GetAsyncKeyState", shim_GetAsyncKeyState },
    { "GetKeyState", shim_GetKeyState },
    { "GetProcAddress", shim_GetProcAddress },
    { "IsBadReadPtr", shim_IsBadReadPtr },
    { "IsBadWritePtr", shim_IsBadWritePtr },
    { "GetModuleHandleA", shim_GetModuleHandleA },
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetCommandLineA", shim_GetCommandLineA },
    { "ExitProcess", shim_ExitProcess },
};

/* --ddtest: does DirectDraw find the hardware from this process? Creates a
 * DirectDraw 7 object and a primary surface (no window), and says whether
 * the display driver's user-mode DLL got loaded. Run at each stage of
 * startup to find the one that loses the hardware. */
/* --display \\.\DISPLAYn: run the game's DirectDraw on that display. GameOS
 * sets display modes while it starts (its hardware probe, then 800x600x16),
 * even when it ends up windowed, and DirectDrawCreateEx(NULL) means the
 * primary display: the main monitor changes resolution wherever the window
 * is. With --display, a NULL device becomes that display's GUID, so the mode
 * changes land there (an offstage virtual monitor, in test runs). */
static char g_display[64];
static GUID g_display_guid;

static BOOL WINAPI find_display(GUID* g, char* desc, char* name, void* ctx, HMONITOR m) {
    (void)desc; (void)ctx; (void)m;
    /* Compared from the last backslash, so DISPLAY2, \\.\DISPLAY2 and a
     * shell-mangled \.\DISPLAY2 all match. */
    const char* want = strrchr(g_display, '\\') ? strrchr(g_display, '\\') + 1 : g_display;
    const char* have = name && strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    if (g && have && !_stricmp(have, want)) { g_display_guid = *g; g_have_display_guid = 1; return FALSE; }
    return TRUE;
}

static void resolve_display(void) {
    typedef HRESULT (WINAPI *enum_t)(void*, void*, DWORD);
    enum_t en = (enum_t)GetProcAddress(LoadLibraryA("ddraw.dll"), "DirectDrawEnumerateExA");
    if (en) en((void*)find_display, NULL, 1 /* DDENUM_ATTACHEDSECONDARYDEVICES */);
    if (!g_have_display_guid) { fprintf(stderr, "no DirectDraw device for %s\n", g_display); exit(1); }
    printf("  DirectDraw on %s\n", g_display);
}

static HRESULT WINAPI host_DirectDrawCreateEx(GUID* g, void** out, const void* iid, void* outer) {
    return g_ddcreate(g ? g : &g_display_guid, out, iid, outer);
}

static void ddtest(const char* stage) {
    static const GUID iid_dd7 = {0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};
    typedef HRESULT (WINAPI *create_t)(GUID*, void**, const GUID*, void*);
    create_t create = (create_t)GetProcAddress(LoadLibraryA("ddraw.dll"), "DirectDrawCreateEx");
    void** dd = NULL;
    HRESULT h = create(NULL, (void**)&dd, &iid_dd7, NULL), hs = -1;
    if (h == 0) {
        void** vt = *(void***)dd;
        uint32_t desc[124 / 4] = {124, 1 /* DDSD_CAPS */};
        void* surf = NULL;
        ((HRESULT (WINAPI*)(void*, HWND, DWORD))vt[20])(dd, NULL, 8 /* DDSCL_NORMAL */);
        desc[26] = 0x200;                                      /* ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE */
        hs = ((HRESULT (WINAPI*)(void*, void*, void**, void*))vt[6])(dd, desc, &surf, NULL);
        if (surf) ((ULONG (WINAPI*)(void*))(*(void***)surf)[2])(surf);
        ((ULONG (WINAPI*)(void*))vt[2])(dd);
    }
    if (stage) fprintf(stderr, "[ddtest] %-14s create %08lX primary %08lX driver %s\n", stage, h, hs,
           GetModuleHandleA("nvldumd.dll") || GetModuleHandleA("nvd3dum.dll") ? "loaded" : "NOT loaded");
}

/* ------------------------------------------------------------ reports */

recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

/* Added after native32's own handler, so callbacks are resolved first and
 * only real faults get here. */
static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) {
        fprintf(stderr, "[exception] 0x%08lX at 0x%p, in sub_%08X, last native call %s\n",
                r->ExceptionCode, r->ExceptionAddress, g_cur_func, g_cur_import);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    HMODULE in = NULL;
    char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)r->ExceptionAddress, &in))
        GetModuleFileNameA(in, mod, sizeof mod);
    fprintf(stderr, "\n=== fault 0x%08lX at 0x%p (%s+0x%X) ===\n", r->ExceptionCode, r->ExceptionAddress,
            mod, (unsigned)((uintptr_t)r->ExceptionAddress - (uintptr_t)in));
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        fprintf(stderr, "  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    recomp_trace_flush();
    fprintf(stderr, "  in lifted sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    native32_dump_icalls(12);
    recomp_dump_trace("fault");
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* --watch PTR OFF,OFF,...: every 500 ms, print the floats at those offsets
 * of the object a guest pointer variable points to (the camera is
 * "--peek 0x6D5DD4 0x3C,0x40,0x44,0x250"). Read without the machine lock:
 * a torn value now and then is fine for a log. */
static uint32_t g_watch_ptr, g_watch_off[256];
static int g_nwatch;

static DWORD WINAPI watcher(LPVOID unused) {
    (void)unused;
    for (DWORD t = 0;; t += 500) {
        Sleep(500);
        uint32_t obj = MEM32(g_watch_ptr);
        if (!obj) continue;
        fprintf(stderr, "[peek] %6lu ms", t);
        for (int i = 0; i < g_nwatch; i++)
            if (!bad_ptr(obj + g_watch_off[i], 4, READABLE))
                fprintf(stderr, "  +%X=%g", g_watch_off[i], *(float*)(uintptr_t)(obj + g_watch_off[i]));
        fprintf(stderr, "\n");
    }
}

static void parse_watch(const char* ptr, const char* offs) {
    g_watch_ptr = strtoul(ptr, NULL, 0);
    if (!strcmp(offs, "*")) { for (uint32_t o = 0; o < 0x300; o += 4) g_watch_off[g_nwatch++] = o; return; }  /* the first 0x300 bytes */
    for (const char* s = offs; *s && g_nwatch < 256; s += strspn(s, ","))
        g_watch_off[g_nwatch++] = strtoul(s, (char**)&s, 0);
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count);
    native32_dump_icalls(8);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

/* The game sets 800x600 in 16-bit colour. Windows 10/11 refuses 16-bit
 * display modes unless the process runs with the DWM8And16BitMitigation
 * compatibility layer (GameOS then stops with "Another application is
 * preventing use of full screen mode"). A layer is read from __COMPAT_LAYER
 * when a process starts, so set it and run this host again, once. No
 * registry change. Returns -1 when this process already has it. */
#define MC2_LAYER "DWM8And16BitMitigation"
static int relaunch_with_layer(void) {
    char cur[512];
    DWORD n = GetEnvironmentVariableA("__COMPAT_LAYER", cur, sizeof cur);
    if (n && n < sizeof cur && strstr(cur, MC2_LAYER)) return -1;
    char layer[600];
    _snprintf(layer, sizeof layer - 1, "%s%s" MC2_LAYER, n && n < sizeof cur ? cur : "", n ? " " : "");
    layer[sizeof layer - 1] = 0;
    SetEnvironmentVariableA("__COMPAT_LAYER", layer);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, GetCommandLineA(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}

int main(int argc, char** argv) {
    int relaunched = relaunch_with_layer();
    if (relaunched >= 0) return relaunched;
    const char* game = "game";
    int run = 0, dd = 0;  /* dd: --ddtest */
    /* Redirected stderr is block-buffered, and the guest can end the process
     * natively (TerminateProcess, MSVCRT's exit): keep every line. */
    setvbuf(stderr, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else if (!strcmp(argv[i], "--ddtest")) dd = g_ddtest = 1;
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) load_input(argv[++i]);
        else if (!strcmp(argv[i], "--display") && i + 1 < argc) strncpy(g_display, argv[++i], sizeof g_display - 1);
        else if (!strcmp(argv[i], "--peek") && i + 2 < argc) { parse_watch(argv[i + 1], argv[i + 2]); i += 2; }
        else {
            printf("usage: mc2 [--run] [--game DIR] [--watchdog S] [--native-trace] [--callbacks]\n"
                   "  without --run: map and bind Mc2Rel.exe, then stop (no window)\n");
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    GetFullPathNameA(game, MAX_PATH - 1, g_game, NULL);
    if (g_game[strlen(g_game) - 1] != '\\') strcat(g_game, "\\");
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%sMc2Rel.exe", g_game);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"", g_guest_exe);

    /* binkw32.dll and MC2RES.DLL come from the game folder, as they did
     * from the install folder. */
    if (dd) ddtest("start");
    SetDllDirectoryA(g_game);
    native32_init();
    if (dd) ddtest("native32_init");
    AddVectoredExceptionHandler(0, crash);
    printf("MechCommander 2 recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);
    uint32_t span = native32_map(g_guest_exe, MC2_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", g_guest_exe, MC2_BASE); return 1; }
    printf("  mapped Mc2Rel.exe: 0x%08X-0x%08X\n", MC2_BASE, MC2_BASE + span);
    int unresolved = native32_bind(MC2_BASE, g_shims, (int)(sizeof g_shims / sizeof g_shims[0]));
    printf("  imports bound, %d unresolved\n", unresolved);
    if (unresolved) return 1;
    if (dd) ddtest("mapped+bound");
    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", mc2_entry_va);
        return 0;
    }
    /* The CRT startup takes WinMain's lpCmdLine from MSVCRT's _acmdln, not
     * from GetCommandLineA: give it the guest's, not this host's flags. */
    char** acmdln = (char**)GetProcAddress(GetModuleHandleA("MSVCRT.dll"), "_acmdln");
    if (acmdln) *acmdln = g_guest_cmdline;
    /* The game opens its .fst archives and data\ relative to its own folder. */
    if (!SetCurrentDirectoryA(g_game)) { fprintf(stderr, "cannot enter %s\n", g_game); return 1; }
    /* Bring DirectDraw up once, natively, before the game starts. When the
     * game's own first DirectDrawCreateEx is what loads the display driver's
     * user-mode DLL (nvldumd/nvd3dum), the load fails and DirectDraw reports
     * no hardware: no 3D device, so GameOS falls back to its software
     * rasterizer and stops. Loaded here first, the game finds the hardware.
     * ponytail: works around the failed in-game load; the cause (what the
     * game's thread does to the driver's DllMain) is not found yet. */
    ddtest(dd ? "before entry" : NULL);
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    if (g_display[0]) resolve_display();
    g_in_t0 = GetTickCount();
    if (g_nwatch) CloseHandle(CreateThread(NULL, 0, watcher, NULL, 0, NULL));
    printf("  entering 0x%08X\n\n", mc2_entry_va);
    fflush(stdout);
    native32_call_guest(mc2_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    return (int)g_eax;
}
