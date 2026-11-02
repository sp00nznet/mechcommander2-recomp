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

static void shim_GetProcAddress(void) {
    HMODULE m = (HMODULE)(uintptr_t)ARG(0);
    const char* name = (const char*)(uintptr_t)ARG(1);
    FARPROC p = GetProcAddress(m, name);
    if (g_ddtest && (uintptr_t)name >= 0x10000 && !strcmp(name, "DirectDrawCreateEx")) ddtest("in the game");
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

/* GameOS reports its errors through OutputDebugStringA: into the log. */
static void shim_OutputDebugStringA(void) {
    fprintf(stderr, "[debug] %s", (const char*)(uintptr_t)ARG(0));
    RET(0, 1);
}

static native32_shim_t g_shims[] = {
    { "OutputDebugStringA", shim_OutputDebugStringA },
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
    printf("  entering 0x%08X\n\n", mc2_entry_va);
    fflush(stdout);
    native32_call_guest(mc2_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    return (int)g_eax;
}
