/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
***********************************************************************************************************************
* @file  ddcWinPlatform.cpp
* @brief User mode Windows platform layer implementation
***********************************************************************************************************************
*/

#include <ddPlatform.h>
#include <stdio.h>
#include <stdlib.h>

////////////////////////////////
// UMD
// Each define (in this order) sanity checks something:
//      1) We're not compiling as kernel mode by mistake
//      2) Our Windows UM platform is setup correctly
//      3) Our generic "is Um" macro is setup correctly
// This is at worst redundant, but when this is wrong it can save us half an hour of debugging the build system
#if (defined(_KERNEL_MODE) || !defined(DD_PLATFORM_WINDOWS_UM) )
#error "This file must be compiled for user-mode."
#endif

#include <Psapi.h>

namespace DevDriver
{
    namespace Platform
    {
        // Function prototype of SetThreadDescription which is required to set thread names on Windows 10 and above
        // We have to load this function dynamically to avoid compatibility issues on Windows 7.
        typedef HRESULT (WINAPI *PFN_SetThreadDescription)(
            HANDLE hThread,
            PCWSTR lpThreadDescription
        );

        inline Result WaitObject(HANDLE hObject, uint32 millisecTimeout)
        {
            DD_ASSERT(hObject != NULL);
            DWORD status = WaitForSingleObject(hObject, millisecTimeout);
            Result result = Result::Error;
            switch (status)
            {
                case WAIT_OBJECT_0:
                    result = Result::Success;
                    break;
                case WAIT_TIMEOUT:
                    result = Result::NotReady;
                    break;
                // When WaitForSingleObject fails, it reports additional information through GetLastError().
                case WAIT_FAILED:
                {
                    DWORD lastError = GetLastError();
                    if (lastError == ERROR_INVALID_HANDLE)
                    {
                        DD_PRINT(LogLevel::Always, "WaitForSingleObject() failed with ERROR_INVALID_HANDLE");
                    }
                    else
                    {
                        DD_PRINT(LogLevel::Always, "WaitForSingleObject() failed - GLE=%d 0x%x", GetLastError(), GetLastError());
                    }
                    DD_ASSERT_ALWAYS();
                    break;
                }
                default:
                {
                    DD_PRINT(LogLevel::Always, "WaitForSingleObject() returned %d (0x%x)", status, status);
                    break;
                }
            }
            DD_WARN(result != Result::Error);
            return result;
        }

        ////////////////////////
        // Open an event create in user space
        // If hObject is not nullptr, the passed in handle is opened.
        // If it is nullptr, we use the passed in name string.
        inline HANDLE CopyHandleFromProcess(ProcessId processId, HANDLE hObject)
        {
            DD_ASSERT(hObject != NULL);

            HANDLE outputObject = nullptr;

            HANDLE hProcess = ::OpenProcess(PROCESS_ALL_ACCESS, TRUE /*bInheritHandle*/, static_cast<DWORD>(processId));

            if (hProcess != nullptr)
            {
                // Just Duplicate the handle for UMD test
                DuplicateHandle(
                    hProcess,
                    hObject,
                    GetCurrentProcess(),
                    &outputObject,
                    EVENT_ALL_ACCESS,
                    TRUE, /*Inherit Handle*/
                    0 /*options*/);

                CloseHandle(hProcess);
            }

            DD_WARN(outputObject != NULL);
            return outputObject;
        }

        /////////////////////////////////////////////////////
        // Local routines.....
        //
        void DebugPrint(LogLevel lvl, const char* pFormat, ...)
        {
            DD_UNUSED(lvl);

            va_list args;
            va_start(args, pFormat);
            char buffer[1024];
            Platform::Vsnprintf(buffer, ArraySize(buffer), pFormat, args);
            va_end(args);

            // Append a newline
            Platform::Snprintf(buffer, "%s\n", buffer);

            OutputDebugString(buffer);
            printf("[DevDriver] %s", buffer);
        }

        int32 AtomicIncrement(Atomic* pVariable)
        {
            return static_cast<int32>(InterlockedIncrement(pVariable));
        }

        int32 AtomicAdd(Atomic* pVariable, int32 num)
        {
            return static_cast<int32>(InterlockedAdd(pVariable, static_cast<long>(num)));
        }

        int32 AtomicDecrement(Atomic* pVariable)
        {
            return static_cast<int32>(InterlockedDecrement(pVariable));
        }

        int32 AtomicSubtract(Atomic* pVariable, int32 num)
        {
            return static_cast<int32>(InterlockedAdd(pVariable, -static_cast<long>(num)));
        }

        int64 AtomicIncrement(Atomic64* pVariable)
        {
            return static_cast<int64>(InterlockedIncrement64(pVariable));
        }

        int64 AtomicAdd(Atomic64* pVariable, int64 num)
        {
            return static_cast<int64>(InterlockedAdd64(pVariable, static_cast<LONG64>(num)));
        }

        int64 AtomicDecrement(Atomic64* pVariable)
        {
            return static_cast<int64>(InterlockedDecrement64(pVariable));
        }

        int64 AtomicSubtract(Atomic64* pVariable, int64 num)
        {
            return static_cast<int64>(InterlockedAdd64(pVariable, -static_cast<LONG64>(num)));
        }

        /////////////////////////////////////////////////////
        // Thread routines.....
        //

        Result Thread::Start(ThreadFunction pFnThreadFunc, void* pThreadParameter)
        {
            Result result = Result::Error;

            if ((hThread == NULL) && (pFnThreadFunc != nullptr))
            {
                pParameter  = pThreadParameter;
                pFnFunction = pFnThreadFunc;

                hThread = ::CreateThread(
                    nullptr,            // Thread Attributes
                    0,                  // Stack size (use default)
                    Thread::ThreadShim, // New thread's entry point
                    this,               // New thread entry's paramter
                    0,                  // Creation flags - start immediately
                    nullptr);

                if (hThread != NULL)
                {
                    result = Result::Success;
                }
                DD_WARN(result != Result::Error);
            }
            return result;
        };

        Result Thread::SetNameRaw(const char* pThreadName)
        {
            Result result = Result::Unavailable;

            // SetThreadDescription is only available on Windows 10 and above.

            // We load the thread naming function dynamically to avoid issues when the current OS doesn't have
            // support for the function.
            HMODULE hModule = GetModuleHandle("kernel32.dll");
            if (hModule != nullptr)
            {
                // Attempt to load the function for setting thread names
                PFN_SetThreadDescription pfnSetThreadDescription =
                    reinterpret_cast<PFN_SetThreadDescription>(GetProcAddress(hModule, "SetThreadDescription"));

                if (pfnSetThreadDescription != nullptr)
                {
                    wchar_t wThreadName[kThreadNameMaxLength];
                    memset(wThreadName, 0, sizeof(wThreadName));

                    const size_t len = Min(ArraySize(wThreadName), strlen(pThreadName));

                    const size_t converted = mbstowcs(wThreadName, pThreadName, len);

                    HRESULT hResult = E_FAIL;
                    if (converted < ArraySize(wThreadName))
                    {
                        hResult = pfnSetThreadDescription(hThread, wThreadName);
                    }

                    result = (SUCCEEDED(hResult) ? Result::Success : Result::Error);
                }
            }

            return result;
        }

        Result Thread::Join(uint32 timeoutInMs)
        {
            Result result = IsJoinable() ? Result::Success : Result::Error;

            if (result == Result::Success)
            {
                // We only need to wait on our event here if the thread object is still unsignaled/running.
                // If the thread is terminated externally, the thread object will be signaled by the OS but
                // our event won't be. This check prevents us from incorrectly timing out in that situation.
                const bool isThreadAlive = (WaitObject(hThread, 0) == Result::NotReady);
                if (isThreadAlive)
                {
                    result = onExit.Wait(timeoutInMs);
                }
            }

            if (result == Result::Success)
            {
                // Note: This does not stop the thread - WaitObject should have done that already.
                if (CloseHandle(hThread) == 0)
                {
                    DD_WARN_REASON("Closing the thread handle failed!");
                    result = Result::Error;
                }
            }

            if (result == Result::Success)
            {
                // Erase our handle now to avoid double-joining.
                Reset();
            }

            DD_WARN(result != Result::Error);
            return result;
        };

        bool Thread::IsJoinable() const
        {
            return (hThread != NULL);
        };

        /////////////////////////////////////////////////////
        // Library
        /////////////////////////////////////////////////////

        // Loads a DLL with the specified name into this process.  The system will search for the DLL according to the
        // documentation available here:
        Result Library::Load(
            const char* pLibraryName)
        {
            Result result = Result::Success;

            // First, try to access an existing instance of this library, if one has already been loaded (this should be more
            // friendly to UWP applications).
            // Note: GetModuleHandleEx is used instead of GetModuleHandle because that allows us to avoid a race condition, as
            // well as increment the DLL's reference count.  See the documentation here:

            constexpr uint32 Flags = 0;
            if (GetModuleHandleExA(Flags, pLibraryName, &m_hLib) == FALSE)
            {
                m_hLib = LoadLibraryA(pLibraryName);
                if (m_hLib == nullptr)
                {
                    result = Result::FileNotFound;
                }
            }

            return result;
        }

        // Unloads this DLL if it was loaded previously.  Called automatically during the object destructor.
        void Library::Close()
        {
            if (m_hLib != nullptr)
            {
                FreeLibrary(m_hLib);
                m_hLib = nullptr;
            }
        }

        void* Library::GetFunctionHelper(
            const char* pName
        ) const
        {
            DD_ASSERT(m_hLib != nullptr);
            return reinterpret_cast<void*>(GetProcAddress(m_hLib, pName));
        }

        /////////////////////////////////////////////////////
        // Memory Management
        /////////////////////////////////////////////////////

        void* AllocateMemory(size_t size, size_t alignment, bool zero)
        {
            void* pMemory = _aligned_malloc(size, alignment);
            if ((pMemory != nullptr) && zero)
            {
                memset(pMemory, 0, size);
            }

            return pMemory;
        }

        void FreeMemory(void* pMemory)
        {
            _aligned_free(pMemory);
        }

        /////////////////////////////////////////////////////
        // Synchronization primatives...
        //

        void AtomicLock::Lock()
        {
            // TODO - implement timeout
            while (InterlockedCompareExchangeAcquire(&m_lock, 1, 0) == 1)
            {
                while (m_lock != 0)
                {
                    // Spin until the mutex is unlocked again
                }
            }
        }

        void AtomicLock::Unlock()
        {
            if (InterlockedCompareExchangeRelease(&m_lock, 0, 1) == 0)
            {
                DD_ASSERT_REASON("Tried to unlock an already unlocked AtomicLock");
            }
        }

        Mutex::Mutex()
            : m_mutex()
        {
            InitializeCriticalSection(&m_mutex.criticalSection);
        }

        Mutex::~Mutex()
        {
#if !defined(NDEBUG)
            // This mutex was destroyed while locked. Potentially hazardous due to possibility of a pending wait on
            // the lock
            DD_ASSERT(m_mutex.lockCount == 0);
#endif
            DeleteCriticalSection(&m_mutex.criticalSection);
        }

        void Mutex::Lock()
        {
            EnterCriticalSection(&m_mutex.criticalSection);
#if !defined(NDEBUG)
            const int32 count = AtomicIncrement(&m_mutex.lockCount);
            // This lock was successfully locked twice. This indicates recursive lock usage, which is non supported
            // on all platforms
            DD_ASSERT(count == 1);
            DD_UNUSED(count);
#endif
        }

        void Mutex::Unlock()
        {
#if !defined(NDEBUG)
            AtomicDecrement(&m_mutex.lockCount);
#endif
            LeaveCriticalSection(&m_mutex.criticalSection);
        }

        Semaphore::Semaphore(uint32 initialCount, uint32 maxCount)
        {
            m_semaphore = Windows::CreateSharedSemaphore(initialCount, maxCount);
        }

        Semaphore::~Semaphore()
        {
            Windows::CloseSharedSemaphore(m_semaphore);
        }

        Result Semaphore::Signal()
        {
            return Windows::SignalSharedSemaphore(m_semaphore);
        }

        Result Semaphore::Wait(uint32 millisecTimeout)
        {
            return WaitObject(reinterpret_cast<HANDLE>(m_semaphore), millisecTimeout);
        }

        Event::Event(bool signaled)
        {
            m_event = CreateEvent(nullptr, TRUE, static_cast<BOOL>(signaled), nullptr);
        }

        Event::~Event()
        {
            CloseHandle(m_event);
        }

        void Event::Clear()
        {
            ResetEvent(m_event);
        }

        void Event::Signal()
        {
            SetEvent(m_event);
        }

        Result Event::Wait(uint32 timeoutInMs)
        {
            return WaitObject(m_event,timeoutInMs);
        }

        Random::Random()
        {
            LARGE_INTEGER seed;
            QueryPerformanceCounter(&seed);
            m_prevState = seed.QuadPart;
        }

        Result Mkdir(const char* pDir)
        {
            Result result = Result::InvalidParameter;

            if (pDir != nullptr)
            {
                const BOOL ret = CreateDirectory(pDir, NULL);
                if (ret != 0)
                {
                    result = Result::Success;
                }
                else
                {
                    const int err = GetLastError();
                    if (err == ERROR_ALREADY_EXISTS)
                    {
                        // The directory already exists, which is fine.
                        result = Result::Success;
                    }
                    else
                    {
                        result = Result::FileIoError;
                    }
                }
            }

            return result;
        }

        ProcessId GetProcessId()
        {
            return static_cast<ProcessId>(GetCurrentProcessId());
        }

        uint64 GetCurrentTimeInMs()
        {
            return GetTickCount64();
        }

        uint64 QueryTimestampFrequency()
        {
            uint64 frequency = 0;
            LARGE_INTEGER perfFrequency = {};
            const BOOL result = QueryPerformanceFrequency(&perfFrequency);
            if (result)
            {
                frequency = perfFrequency.QuadPart;
            }
            else
            {
                DD_ASSERT_REASON("Failed to query performance counter frequency!");
            }

            return frequency;
        }

        uint64 QueryTimestamp()
        {
            uint64 timestamp = 0;
            LARGE_INTEGER perfTimestamp = {};
            const BOOL result = QueryPerformanceCounter(&perfTimestamp);
            if (result)
            {
                timestamp = perfTimestamp.QuadPart;
            }
            else
            {
                DD_ASSERT_REASON("Failed to query performance counter timestamp!");
            }

            return timestamp;
        }

        void Sleep(uint32 millisecTimeout)
        {
            ::Sleep(millisecTimeout);
        }

        void GetProcessName(char* buffer, size_t bufferSize)
        {
            char path[1024] = {};
            size_t  numChars = 0;

            numChars = GetModuleFileNameExA(GetCurrentProcess(), nullptr, path, 1024);

            buffer[0] = 0;
            if (numChars > 0)
            {
                char fname[256] = {};
                char ext[256] = {};
                _splitpath_s(path, nullptr, 0, nullptr, 0, fname, sizeof(fname), ext, sizeof(ext));
                strcat_s(buffer, bufferSize, fname);
                strcat_s(buffer, bufferSize, ext);
            }
        }

        void Strncpy(char* pDst, const char* pSrc, size_t dstSize)
        {
            DD_ASSERT(pDst != nullptr);
            DD_ASSERT(pSrc != nullptr);
            DD_WARN(strlen(pSrc) < dstSize);

            // Clamp the copy to the size of the dst buffer (1 char reserved for the null terminator).
            strcpy_s(pDst, dstSize, pSrc);
        }

        char* Strtok(char* pDst, const char* pDelimiter, char** ppContext)
        {
            DD_ASSERT(pDelimiter != nullptr);

            return strtok_s(pDst, pDelimiter, ppContext);
        }

        void Strcat(char* pDst, const char* pSrc, size_t dstSize)
        {
            DD_ASSERT(pDst != nullptr);
            DD_ASSERT(pSrc != nullptr);

            strcat_s(pDst, dstSize, pSrc);
        }

        int32 Strcmpi(const char* pSrc1, const char* pSrc2)
        {
            DD_ASSERT(pSrc1 != nullptr);
            DD_ASSERT(pSrc2 != nullptr);

            return _stricmp(pSrc1, pSrc2);
        }

        int32 Vsnprintf(char* pDst, size_t dstSize, const char* format, va_list args)
        {
            int32 ret = vsnprintf(pDst, dstSize, format, args);

            // If the return value looks like a valid length, add one to account for a NULL byte.
            if (ret >= 0)
            {
                ret += 1;
            }
            else
            {
                // A negative value means that some error occurred
                // We don't print anything here because our logging requires Vsnprintf
            }

            return ret;
        }

        Result QueryOsInfo(OsInfo* pInfo)
        {
            DD_ASSERT(pInfo != nullptr);
            memset(pInfo, 0, sizeof(*pInfo));

            Result result = Result::Success;

            HKEY hKey = 0;
            LONG res  = RegOpenKeyExA(
                HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                0,
                KEY_READ,
                &hKey
            );

            /// Populate a name
            {
                DWORD keyType = 0;
                DWORD valueSize = 0;
                char  textBuffer[128];

                if (res == ERROR_SUCCESS)
                {
                    memset(textBuffer, 0, ArraySize(textBuffer));
                    valueSize = ArraySize<DWORD>(textBuffer);
                    res = RegQueryValueExA(
                        hKey,
                        "productName",
                        nullptr,
                        &keyType,
                        (LPBYTE)&textBuffer[0],
                        &valueSize
                    );
                    if (res == ERROR_SUCCESS)
                    {
                        DD_ASSERT(valueSize < ArraySize(textBuffer));
                        DD_ASSERT(keyType == REG_SZ);

                        Strncpy(pInfo->name, textBuffer);
                    }
                    else
                    {
                        result = Result::Error;
                    }
                }
            }

            /// Populate a description
            {
                DWORD keyType = 0;
                DWORD valueSize = 0;
                char  textBuffer[128];

                if (res == ERROR_SUCCESS)
                {
                    memset(textBuffer, 0, ArraySize(textBuffer));
                    valueSize = ArraySize<DWORD>(textBuffer);
                    res = RegQueryValueExA(
                        hKey,
                        "buildLabEx",
                        nullptr,
                        &keyType,
                        (LPBYTE)&textBuffer[0],
                        &valueSize
                    );
                    if (res == ERROR_SUCCESS)
                    {
                        DD_ASSERT(valueSize < ArraySize(textBuffer));
                        DD_ASSERT(keyType == REG_SZ);

                        Strncpy(pInfo->description, textBuffer);
                    }
                    else
                    {
                        result = Result::Error;
                    }
                }
            }

            /// Query the machine's hostname
            {
                DWORD nSize = ArraySize<DWORD>(pInfo->hostname);
                GetComputerNameEx(ComputerNameDnsFullyQualified, pInfo->hostname, &nSize);
                DD_WARN(nSize > 0);
            }

            /// Query information about the current user
            {
                const char* pUser = getenv("USERNAME");
                DD_WARN(pUser != nullptr);
                if (pUser != nullptr)
                {
                    Platform::Strncpy(pInfo->user.name, pUser);
                }

                const char* pHomeDir = getenv("HOMEPATH");
                DD_WARN(pHomeDir != nullptr);
                if (pHomeDir != nullptr)
                {
                    Platform::Strncpy(pInfo->user.homeDir, pHomeDir);
                }
            }

            /// Query available memory
            {
                MEMORYSTATUSEX memoryStatus = {};
                memoryStatus.dwLength = sizeof(memoryStatus);
                DD_UNHANDLED_RESULT(BoolToResult(GlobalMemoryStatusEx(&memoryStatus) != 0));
                pInfo->physMemory = memoryStatus.ullTotalPhys;
                pInfo->swapMemory = memoryStatus.ullTotalPageFile;
            }

            RegCloseKey(hKey);

            return result;
        }

        namespace Windows
        {
            // These two functions are here for back-compat.
            // They are required to link against the existing messagelib files.
            // TODO: Remove these definitions when we cut messagelib.
            Result AcquireFastLock(Atomic *mutex)
            {
                // TODO - implement timeout
                while (InterlockedCompareExchangeAcquire(mutex, 1, 0) == 1)
                {
                    // spin until the mutex is unlocked again
                    while (*mutex != 0)
                    {
                    }
                }
                return Result::Success;
            }

            Result ReleaseFastLock(Atomic *mutex)
            {
                if (InterlockedCompareExchangeRelease(mutex, 0, 1) == 0)
                {
                    // tried to unlock an already unlocked mutex
                    return Result::Error;
                }
                return Result::Success;
            }

            /////////////////////////////////////////////////////
            // Local routines.....
            //

            Handle CreateSharedSemaphore(uint32 initialCount, uint32 maxCount)
            {
                // Create original object in the current process
                return DD_PTR_TO_HANDLE(CreateSemaphore(
                    nullptr,
                    initialCount,
                    maxCount,
                    nullptr /* Not Named*/));
            }

            Handle CopySemaphoreFromProcess(ProcessId processId, Handle hObject)
            {
                return DD_PTR_TO_HANDLE(CopyHandleFromProcess(processId, reinterpret_cast<HANDLE>(hObject)));
            }

            Result SignalSharedSemaphore(Handle pSemaphore)
            {
                DD_ASSERT(pSemaphore != NULL);
                BOOL result = ReleaseSemaphore(reinterpret_cast<HANDLE>(pSemaphore), 1, nullptr);
                return (result != 0) ? Result::Success : Result::Error;
            }

            Result WaitSharedSemaphore(Handle pSemaphore, uint32 millisecTimeout)
            {
                return WaitObject(reinterpret_cast<HANDLE>(pSemaphore), millisecTimeout);
            }

            void CloseSharedSemaphore(Handle pSemaphore)
            {
                if (pSemaphore != NULL)
                {
                    CloseHandle(reinterpret_cast<HANDLE>(pSemaphore));
                }
            }

            Handle CreateSharedBuffer(Size bufferSizeInBytes)
            {
                HANDLE hSharedBuffer =
                    CreateFileMapping(
                        INVALID_HANDLE_VALUE,    // use paging file
                        nullptr,                    // default security
                        PAGE_READWRITE,          // read/write access
                        0,                       // maximum object size (high-order DWORD)
                        bufferSizeInBytes, // maximum object size (low-order DWORD)
                        nullptr); // name of mapping object

                DD_WARN(hSharedBuffer != nullptr);
                return DD_PTR_TO_HANDLE(hSharedBuffer);
            }

            Handle MapSystemBufferView(Handle hBuffer, Size bufferSizeInBytes)
            {
                DD_ASSERT(hBuffer != kNullPtr);
                LPVOID pSharedBufferView = MapViewOfFile(
                    reinterpret_cast<HANDLE>(hBuffer),
                    FILE_MAP_ALL_ACCESS, // read/write permission
                    0, // File offset high dword
                    0, // File offset low dword
                    bufferSizeInBytes);
                DD_WARN(pSharedBufferView != nullptr);
                return DD_PTR_TO_HANDLE(pSharedBufferView);
            }

            void UnmapBufferView(Handle hSharedBuffer, Handle hSharedBufferView)
            {
                // The shared buffer is only used in the kernel implementation
                DD_UNUSED(hSharedBuffer);
                DD_ASSERT(hSharedBufferView != kNullPtr);
                BOOL result = UnmapViewOfFile(reinterpret_cast<HANDLE>(hSharedBufferView));
                DD_UNUSED(result);
                DD_WARN(result == TRUE);
            }

            void CloseSharedBuffer(Handle hSharedBuffer)
            {
                if (hSharedBuffer != kNullPtr)
                {
                    BOOL result = CloseHandle(reinterpret_cast<HANDLE>(hSharedBuffer));
                    DD_WARN(result == TRUE);
                    DD_UNUSED(result);
                }
            }

            Handle MapProcessBufferView(Handle hBuffer, ProcessId processId)
            {
                Handle sharedBuffer = kNullPtr;

                HANDLE hProcess = ::OpenProcess(PROCESS_ALL_ACCESS, TRUE /*bInheritHandle*/, static_cast<DWORD>(processId));

                if (hProcess != nullptr)
                {
                    // Just Duplicate the handle for UMD test
                    DuplicateHandle(
                        GetCurrentProcess(),
                        (HANDLE)hBuffer,
                        hProcess,
                        reinterpret_cast<LPHANDLE>(&sharedBuffer),
                        0,
                        TRUE, /*Inherit Handle*/
                        DUPLICATE_SAME_ACCESS /*options*/);

                    CloseHandle(hProcess);
                }

                DD_WARN(sharedBuffer != kNullPtr);

                return sharedBuffer;
            }
        }
    }
}
