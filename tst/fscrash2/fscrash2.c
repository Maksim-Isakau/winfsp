#include <winfsp/winfsp.h>
#include <stdio.h>
#include <stdlib.h>

#include "memfs.h"

#define fail(format, ...)               fprintf(stderr, format "\n", __VA_ARGS__)
#define ASSERT(expr)                    \
    (!(expr) ?                          \
        (fail("ASSERT(%s) failed at %s:%d:%s (LastError=%lu)\n", #expr, __FILE__, __LINE__, __func__, GetLastError()), exit(1)) :\
        (void)0)

#define FSCRASH2_LOAD_THREADS           8
#define FSCRASH2_EXIT_TIMEOUT           10000

ULONG OptIterations = 20;

static int RunFs(PWSTR Id)
{
    MEMFS *Memfs;
    WCHAR Name[1024], Path[MAX_PATH];
    HANDLE Event, File;
    DWORD BytesTransferred;
    NTSTATUS Result;

    Result = MemfsCreate(
        MemfsDisk,
        0,
        1024,
        16 * 1024 * 1024,
        0,
        0,
        &Memfs);

    if (!NT_SUCCESS(Result))
    {
        fail("cannot create MEMFS file system: (Status=%lx)", Result);
        exit(1);
    }

    Result = MemfsStart(Memfs);
    if (!NT_SUCCESS(Result))
    {
        fail("cannot start MEMFS file system: (Status=%lx)", Result);
        exit(1);
    }

    GetTempPathW(MAX_PATH, Path);
    wcscat_s(Path, MAX_PATH, Id);
    File = CreateFileW(Path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    ASSERT(INVALID_HANDLE_VALUE != File);
    
    ASSERT(WriteFile(File,
        MemfsFileSystem(Memfs)->VolumeName,
        (DWORD)(wcslen(MemfsFileSystem(Memfs)->VolumeName) * sizeof(WCHAR)),
        &BytesTransferred,
        0));
    
        ASSERT(CloseHandle(File));

    wsprintfW(Name, L"fscrash2-ready-%s", Id);
    Event = OpenEventW(EVENT_MODIFY_STATE, FALSE, Name);
    ASSERT(0 != Event);
    SetEvent(Event);

    Sleep(INFINITE); /* wait termination */
    return 0;
}

static WCHAR LoadRoot[1024];

static DWORD WINAPI LoadThread(PVOID Param)
{
    WCHAR FileName[1024], StreamName[1024];
    HANDLE DocHandle, StreamHandle;
    DWORD Index = (DWORD)(UINT_PTR)Param, BytesTransferred;

    wsprintfW(FileName, L"%s\\file-%lu", LoadRoot, Index);
    wsprintfW(StreamName, L"%s\\file-%lu:s", LoadRoot, Index);

    for (ULONG Iter = 0;; Iter++)
    {
        DocHandle = CreateFileW(FileName,
            GENERIC_READ | GENERIC_WRITE | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
            CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, 0);
        if (INVALID_HANDLE_VALUE == DocHandle)
            return 0;
        StreamHandle = CreateFileW(StreamName,
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
            CREATE_ALWAYS, 0, 0);
        if (INVALID_HANDLE_VALUE == StreamHandle)
        {
            CloseHandle(DocHandle);
            return 0;
        }
        WriteFile(StreamHandle, "x", 1, &BytesTransferred, 0);
        CloseHandle(DocHandle);
        CloseHandle(StreamHandle);
    }

    return 0;
}

static int RunLoad(PWSTR VolumeName)
{
    WCHAR FileName[1024];
    HANDLE Handle, Threads[FSCRASH2_LOAD_THREADS];
    ULONG Index;

    wsprintfW(LoadRoot, L"\\\\?\\GLOBALROOT%s", VolumeName);

    wsprintfW(FileName, L"%s\\file0", LoadRoot);
    Handle = CreateFileW(FileName,
        GENERIC_WRITE, 0, 0,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    ASSERT(INVALID_HANDLE_VALUE != Handle);
    ASSERT(CloseHandle(Handle));

    for (Index = 0; FSCRASH2_LOAD_THREADS > Index; Index++)
    {
        Threads[Index] = CreateThread(0, 0, LoadThread, (PVOID)(UINT_PTR)Index, 0, 0);
        ASSERT(0 != Threads[Index]);
    }
    WaitForMultipleObjects(FSCRASH2_LOAD_THREADS, Threads, TRUE, INFINITE);

    return 0;
}

static HANDLE Spawn(PWSTR Options)
{
    WCHAR Executable[MAX_PATH], CommandLine[1024];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;

    GetModuleFileNameW(0, Executable, MAX_PATH);
    wsprintfW(CommandLine, L"\"%s\" %s", Executable, Options);

    memset(&StartupInfo, 0, sizeof StartupInfo);
    memset(&ProcessInfo, 0, sizeof ProcessInfo);
    StartupInfo.cb = sizeof StartupInfo;
    ASSERT(CreateProcessW(Executable, CommandLine, 0, 0, FALSE, 0, 0, 0, &StartupInfo, &ProcessInfo));
    CloseHandle(ProcessInfo.hThread);

    return ProcessInfo.hProcess;
}

static int Control(VOID)
{
    WCHAR Id[64], Name[1024], Path[MAX_PATH], VolumeName[1024], Options[1024];
    HANDLE Event, File, FsProcess, LoadProcess;
    DWORD BytesTransferred, WaitResult, FsPid;
    ULONG Iter;

    srand(GetTickCount());

    for (Iter = 1; OptIterations >= Iter; Iter++)
    {
        wsprintfW(Id, L"%lu-%lu", GetCurrentProcessId(), Iter);
        wsprintfW(Name, L"fscrash2-ready-%s", Id);
        Event = CreateEventW(0, TRUE, FALSE, Name);
        ASSERT(0 != Event);

        wsprintfW(Options, L"--run-fs=%s", Id);
        FsProcess = Spawn(Options);
        FsPid = GetProcessId(FsProcess);

        WaitResult = WaitForSingleObject(Event, 10000);
        ASSERT(WAIT_OBJECT_0 == WaitResult);
        CloseHandle(Event);

        GetTempPathW(MAX_PATH, Path);
        wcscat_s(Path, MAX_PATH, Id);
        File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        ASSERT(INVALID_HANDLE_VALUE != File);
        memset(VolumeName, 0, sizeof VolumeName);
        ASSERT(ReadFile(File, VolumeName, sizeof VolumeName - sizeof(WCHAR), &BytesTransferred, 0));
        CloseHandle(File);
        DeleteFileW(Path);

        wsprintfW(Options, L"--run-load=%s", VolumeName);
        LoadProcess = Spawn(Options);

        /* wait for the loader to start working, then kill FS mid-run */
        Sleep(200 + rand() % 500);
        ASSERT(TerminateProcess(FsProcess, 1));

        WaitResult = WaitForSingleObject(FsProcess, FSCRASH2_EXIT_TIMEOUT);
        if (WAIT_OBJECT_0 != WaitResult)
        {
            printf("fscrash2: FAIL at iteration %lu: FS process (pid=%lu) survived\n", Iter, FsPid);
            fflush(stdout);
            TerminateProcess(LoadProcess, 1);
            return 2;
        }
        CloseHandle(FsProcess);

        WaitResult = WaitForSingleObject(LoadProcess, FSCRASH2_EXIT_TIMEOUT);
        if (WAIT_OBJECT_0 != WaitResult)
        {
            TerminateProcess(LoadProcess, 1);
            WaitResult = WaitForSingleObject(LoadProcess, FSCRASH2_EXIT_TIMEOUT);
            if (WAIT_OBJECT_0 != WaitResult)
            {
                printf("fscrash2: FAIL at iteration %lu: load process survived TerminateProcess;\n", Iter);
                fflush(stdout);
                return 2;
            }
        }
        CloseHandle(LoadProcess);

        printf("fscrash2: iteration %lu OK\n", Iter);
        fflush(stdout);
    }

    printf("fscrash2: PASS (%lu iterations)\n", OptIterations);
    fflush(stdout);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    PWSTR OptRunFs = 0, OptRunLoad = 0;

    for (int argi = 1; argc > argi; argi++)
    {
        wchar_t *a = argv[argi];
        if (0 == wcsncmp(L"--run-fs=", a, sizeof "--run-fs=" - 1))
            OptRunFs = a + sizeof "--run-fs=" - 1;
        else if (0 == wcsncmp(L"--run-load=", a, sizeof "--run-load=" - 1))
            OptRunLoad = a + sizeof "--run-load=" - 1;
        else if (0 == wcsncmp(L"--iterations=", a, sizeof "--iterations=" - 1))
            OptIterations = wcstoul(a + sizeof "--iterations=" - 1, 0, 10);
        else
        {
            fail("unknown option %S", a);
            exit(2);
        }
    }

    if (0 != OptRunFs)
        return RunFs(OptRunFs);
    if (0 != OptRunLoad)
        return RunLoad(OptRunLoad);
    return Control();
}
