
#if PLATFORM_MSW
#include <Windows.h>
#include <Dbghelp.h>
#include <signal.h>
#include <cstring>
#include <cstdio>

#pragma comment (lib, "dbghelp.lib")

#include "msw_app_console_init.h"

// CALL_FIRST means call this exception handler first;
// CALL_LAST means call this exception handler last
static const int CALL_FIRST		= 1;
static const int CALL_LAST		= 0;

const int MAX_APP_NAME_LEN = 24;
static char s_msw_app_name_for_coredumps[MAX_APP_NAME_LEN+1] = "msw-app";
static bool s_interactive_user_environ = 1;
static bool s_allow_ugly_console = 1;
static bool s_dump_on_abort = 1;


void msw_set_abort_message(bool onoff) {
    // _set_abort_behavior() only does things in debug CRT. We need popups in release builds too, so
    // we have to manage all this ourselves anyway...
    _set_abort_behavior(onoff, _WRITE_ABORT_MSG);
    s_interactive_user_environ = onoff;
}

void msw_set_abort_crashdump(bool onoff) {
    s_dump_on_abort = onoff;
}

void msw_WriteFullDump(EXCEPTION_POINTERS* pep, const char* dumpname)
{
    DWORD Flags = MiniDumpNormal;

    Flags |= MiniDumpWithDataSegs;
    Flags |= MiniDumpWithFullMemoryInfo;
    if (pep) Flags |= MiniDumpWithThreadInfo;

    //Flags |= MiniDumpWithFullMemory;

    MINIDUMP_EXCEPTION_INFORMATION mdei;

    mdei.ThreadId           = GetCurrentThreadId();
    mdei.ExceptionPointers  = pep;
    mdei.ClientPointers     = FALSE;

    MINIDUMP_TYPE mdt       = MiniDumpNormal;

    char dumpfile[200];
    sprintf_s(dumpfile, "%s-crashdump.dmp", dumpname);

    // GENERIC_WRITE, FILE_SHARE_READ used to minimize vectors for failure.
    // I've had multiple issues of this crap call failing for some permission denied reason. --jstine
    if (HANDLE hFile = CreateFileA(dumpfile, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        BOOL Result = MiniDumpWriteDump(
            ::GetCurrentProcess(),
            ::GetCurrentProcessId(),
            hFile,
            (MINIDUMP_TYPE)Flags,
            pep ? &mdei : nullptr,
            nullptr,
            nullptr
        );

        CloseHandle(hFile);

        if (!Result) {
            fprintf(stderr, "Minidump generation failed: %08x\n", ::GetLastError());
        }
        else {
            fprintf(stderr, "Crashdump written to %s\n", dumpfile);
        }
    }
}

static LONG NTAPI msw_PageFaultExceptionFilter( EXCEPTION_POINTERS* eps )
{
    fflush(nullptr);

    if(eps->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    // MS hides the target address of the load/store operation in the second
    // parameter in ExceptionInformation.

    void* access_address = (void*)eps->ExceptionRecord->ExceptionInformation[1];
    bool isWrite         = eps->ExceptionRecord->ExceptionInformation[0];

    fprintf(stderr, "PageFault %s at %p\n", isWrite ? "write" : "read", access_address);
    fflush(nullptr);

    msw_WriteFullDump(eps, s_msw_app_name_for_coredumps);

    if (s_interactive_user_environ && !::IsDebuggerPresent()) {
        // surface it to the user if no debugger attached, if debugger it likely already popped up an exception dialog...
        char fmt_buf[MAX_APP_NAME_LEN + 24];	// avoid heap, it could be corrupt
        sprintf_s(fmt_buf, "SIGSEGV - %s", s_msw_app_name_for_coredumps);
        auto result = ::MessageBoxA(nullptr,
            "ACCESS VIOLATION (SIGSEGV) has occurred and the process has been terminated. "
            "Check the console output for more details.",
            fmt_buf,
            MB_ICONEXCLAMATION | MB_OK
        );
    }

    // pass pagefault to OS, it'll add an entry to the pointlessly over-engineered Windows Event Viewer.
    return EXCEPTION_CONTINUE_SEARCH;
}

void SignalHandler(int signal)
{
    if (signal == SIGABRT) {
        if (!s_dump_on_abort) {
            return;
        }

        msw_WriteFullDump(nullptr, s_msw_app_name_for_coredumps);

        if (s_interactive_user_environ && !::IsDebuggerPresent()) {
            char fmt_buf[MAX_APP_NAME_LEN + 24];	// avoid heap.
            sprintf_s(fmt_buf, "abort() - %s", s_msw_app_name_for_coredumps);

            auto result = ::MessageBoxA(nullptr,
                "An error has occured and the application has aborted.\n"
                "Check the console output for details.",
                fmt_buf,
                MB_ICONEXCLAMATION | MB_OK
            );
        }
    }
}

void msw_AllowUglyConsole(bool enable) {
    s_allow_ugly_console = enable;
}

void msw_InitAppForConsole(const char* app_name) {

    strncpy(s_msw_app_name_for_coredumps, app_name, MAX_APP_NAME_LEN);
    s_msw_app_name_for_coredumps[MAX_APP_NAME_LEN] = 0;	// because strncpy is stupid.

    // Win10 no longer pops up msgs when exceptions occur. We'll need to produce crash dumps ourself...
    AddVectoredExceptionHandler(CALL_FIRST, msw_PageFaultExceptionFilter);

    // let's not have the app ask QA to send reports to microsoft when an abort() occurs.
    _set_abort_behavior(0, _CALL_REPORTFAULT);

#if !defined(_DEBUG)

    typedef void (*SignalHandlerPointer)(int);
    auto previousHandler = signal(SIGABRT, SignalHandler);
#endif

    // abort message popup is typically skipped when debugger is attached. When not attached its purpose is to
    // allow a debugger to attach, or to allow a user to ignore assertions and "hope for the best".
    msw_set_abort_message(1);

    auto* automated_flag    = getenv("AUTOMATED");
    auto* noninteract_flag  = getenv("NONINTERACTIVE");
    auto* jenkins_node_name = getenv("NODE_NAME");
    auto* jenkins_job_name  = getenv("JOB_NAME");

    bool allow_popups = 1;

    // heuristically autodetect jenkins first, and honor explicit AUTOMATED/NONINTERACTIVE afterward.

    if ((jenkins_node_name && jenkins_node_name[0]) &&
        (jenkins_job_name  && jenkins_job_name [0])) {
        allow_popups = 0;
    }

    if (automated_flag  ) allow_popups   = (automated_flag  [0] != '0');
    if (noninteract_flag) allow_popups   = (noninteract_flag[0] != '0');

    msw_set_abort_message(allow_popups);

    // Tell the system not to display the critical-error-handler message box.
    // from msdn: Best practice is that all applications call the process-wide SetErrorMode function
    //     with a parameter of SEM_FAILCRITICALERRORS at startup. This is to prevent error mode dialogs
    //     from hanging the application.
    //
    // Translation: disable this silly ill-advised legacy behavior. --jstine
    // (note in Win10, popups are disabled by default and cannot be re-enabled anyway)
    ::SetErrorMode(SEM_FAILCRITICALERRORS);

#if !defined(_CONSOLE)
    // In order to have a windowed mode application behave in a normal way when run from an existing
    // _Windows 10 console shell_, we have to do this. This is because CMD.exe inside a windows console
    // doesn't set up its own stdout pipes, causing GetStdHandle(STD_OUTPUT_HANDLE) to return nullptr.
    //
    // MINTTY: This problem does not occur on mintty or other conemu or other non-shitty console apps.
    // And we must take care _not_ to override their own pipe redirection bindings. This is why we only
    // call AttachConsole() if the stdio handle is NULL.

    auto previn  = ::GetStdHandle(STD_INPUT_HANDLE );
    auto prevout = ::GetStdHandle(STD_OUTPUT_HANDLE);
    auto preverr = ::GetStdHandle(STD_ERROR_HANDLE );

    if (!prevout || !previn) {
        // this workaround won't capture stdout from DLLs. Nothing we can do about that. The DLL has
        // to do its own freopen() calls on its own. Sorry folks.
        // easy solution: don't use cmd.exe or windows shitty terminal!
        // open outputs as binary to suppress Windows newline corruption (\r mess)
        if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
            if (!previn)  { freopen("CONIN$",  "r" , stdin ); }
            if (!prevout) { freopen("CONOUT$", "wb", stdout); }
            if (!preverr) { freopen("CONOUT$", "wb", stderr); }
            msw_set_abort_message(0);
        }
        else {
            // stdout is dangling, alloc windows built-in console to visualize it.
            // This is kind of an "emergency procedure only" option, since this console is
            // ugly, buggy, and generally limited.

            if (allow_popups && s_allow_ugly_console) {
                msw_AllocUglyConsole();
            }
        }
    }

    // when MSYS BASH shell is present somewhere, assume the user doesn't want or need popups.
    // (can be overridden by CLI switch in the main app, etc)
    if (getenv("SHLVL")) {
        msw_set_abort_message(0);
    }
#endif
}

void msw_AllocUglyConsole() {
    // Creates an old-fashioned console window.
    // This is actually pretty much shit, and we should never use it, because it
    // won't work the way we expect it should whenever DLLs are involved. The DLLs
    // will use a different instance of the CRT, which means different instances of
    // stdin/stdout/stdfile, which means their writes to printf() won't go anywhere.
    //
    // GetStdHandle / SetStdHandle also don't help the situation much.
    //
    // The only good way to redirect input and bind consoles in Windows is to use the
    // command shell or git-bash to redirect things.  It's the only way to ensure DLLs
    // behave correctly.  --jstine

    auto previn  = ::GetStdHandle(STD_INPUT_HANDLE );
    auto prevout = ::GetStdHandle(STD_OUTPUT_HANDLE);
    auto preverr = ::GetStdHandle(STD_ERROR_HANDLE );

    if (!::AllocConsole()) {
        // Console could fail to allocate on a Jenkins environment, for example.
        // And in this specific case, the thing we definitely don't want to do is deadlock the
        // program waiting for input from a non-existant user, so guard it against the interactive_user
        // environment flag.
        if (s_interactive_user_environ) {
            char fmt_buf[256];	// avoid heap.
            sprintf_s(fmt_buf,
                "AllocConsole() failed, error=0x%08x\n"
                "To work-around this problem, please run the application from an existing console, "
                "by opening Git BASH, MinTTY, or Windows Command Prompt and starting the program there.\n",
                ::GetLastError()
            );

            auto result = ::MessageBoxA(nullptr, fmt_buf,
                "AllocConsole Failure",
                MB_ICONEXCLAMATION | MB_OK
            );
        }
    }

    // remap unassigned pipes to a newly-opened console.
    // By affecting only unbound pipes, it allows the program to accept input from stdin or honor
    // tee of stdout or stderr.
    // open outputs as binary to suppress Windows newline corruption (\r mess)

    if (!previn)  { freopen("CONIN$",  "r",  stdin ); }
    if (!prevout) { freopen("CONOUT$", "wb", stdout); }
    if (!preverr) { freopen("CONOUT$", "wb", stderr); }

    // Set console output to UTF8
    ::SetConsoleCP(CP_UTF8);
    ::SetConsoleOutputCP(CP_UTF8);

    // Set console font to MS Gothic, which supports all charsets (CJK, Russian, Euro)
    CONSOLE_FONT_INFOEX cfi = {};
    cfi.cbSize = sizeof(cfi);
    cfi.nFont = 0;
    cfi.dwFontSize.X = 0;
    cfi.dwFontSize.Y = 14;
    cfi.FontFamily = FF_DONTCARE;
    cfi.FontWeight = FW_NORMAL;
    wcscpy_s(cfi.FaceName, L"MS Gothic");
    ::SetCurrentConsoleFontEx(::GetStdHandle(STD_OUTPUT_HANDLE), FALSE, &cfi);
    ::SetCurrentConsoleFontEx(::GetStdHandle(STD_ERROR_HANDLE), FALSE, &cfi);
}
#endif
