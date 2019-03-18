/**
  Copyright © 2019 Odzhan. All Rights Reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  1. Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

  3. The name of the author may not be used to endorse or promote products
  derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY AUTHORS "AS IS" AND ANY EXPRESS OR
  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
  INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
  STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE. */
  
#define UNICODE

#include <windows.h>
#include <stdio.h>
#include <TlHelp32.h>
#include <psapi.h>
#include <shlwapi.h>
#include <wincrypt.h>
#include <dbghelp.h>

#include "ntddk.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "dbghelp.lib")

// allocate memory
LPVOID xmalloc (SIZE_T dwSize) {
    return HeapAlloc (GetProcessHeap(), HEAP_ZERO_MEMORY, dwSize);
}

// re-allocate memory
LPVOID xrealloc (LPVOID lpMem, SIZE_T dwSize) { 
    return HeapReAlloc (GetProcessHeap(), HEAP_ZERO_MEMORY, lpMem, dwSize);
}

// free memory
void xfree (LPVOID lpMem) {
    HeapFree (GetProcessHeap(), 0, lpMem);
}

BOOL SetPrivilege(wchar_t szPrivilege[], BOOL bEnable) {
    HANDLE           hToken;
    BOOL             bResult;
    LUID             luid;
    TOKEN_PRIVILEGES tp;
    
    bResult = OpenProcessToken(GetCurrentProcess(), 
      TOKEN_ADJUST_PRIVILEGES, &hToken);
    
    if (bResult) {    
      bResult = LookupPrivilegeValue(NULL, szPrivilege, &luid);
      if (bResult) {
        tp.PrivilegeCount           = 1;
        tp.Privileges[0].Luid       = luid;
        tp.Privileges[0].Attributes = (bEnable) ? SE_PRIVILEGE_ENABLED : 0;

        bResult = AdjustTokenPrivileges(hToken, FALSE, &tp, 0, NULL, NULL);
      }
      CloseHandle(hToken);
    }
    return bResult;
}

PWCHAR pid2name(DWORD pid) {
    HANDLE         hSnap;
    BOOL           bResult;
    PROCESSENTRY32 pe32;
    PWCHAR         name=L"N/A";
    
    hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    
    if (hSnap != INVALID_HANDLE_VALUE) {
      pe32.dwSize = sizeof(PROCESSENTRY32);
      
      bResult = Process32First(hSnap, &pe32);
      while (bResult) {
        if (pe32.th32ProcessID == pid) {
          name = pe32.szExeFile;
          break;
        }
        bResult = Process32Next(hSnap, &pe32);
      }
      CloseHandle(hSnap);
    }
    return name;
}

DWORD name2pid(LPWSTR ImageName) {
    HANDLE         hSnap;
    PROCESSENTRY32 pe32;
    DWORD          dwPid=0;
    
    // create snapshot of system
    hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(hSnap == INVALID_HANDLE_VALUE) return 0;
    
    pe32.dwSize = sizeof(PROCESSENTRY32);

    // get first process
    if(Process32First(hSnap, &pe32)){
      do {
        if (lstrcmpi(ImageName, pe32.szExeFile)==0) {
          dwPid = pe32.th32ProcessID;
          break;
        }
      } while(Process32Next(hSnap, &pe32));
    }
    CloseHandle(hSnap);
    return dwPid;
}
  
// the allocation is performed by TppAllocThreadData
#define THREAD_POOL_DATA_SIZE 136

typedef struct _THREAD_POOL_DATA {
    ULONG_PTR data[THREAD_POOL_DATA_SIZE/sizeof(ULONG_PTR)];
} THREAD_POOL_DATA;

// list thread pools for a process
VOID GetProcessThreadPools(DWORD pid, BOOL symbol) {
    HANDLE                   hSnap, hProcess, hThread;
    THREADENTRY32            te32;
    DWORD                    i;
    THREAD_BASIC_INFORMATION tbi;
    NTSTATUS                 status;
    TEB                      teb;
    SIZE_T                   rd;
    THREAD_POOL_DATA         tpd;
    PBYTE                    addr=NULL;
    BYTE                     buffer[sizeof(SYMBOL_INFO)+MAX_SYM_NAME*sizeof(WCHAR)];
    PSYMBOL_INFO             pSymbol=(PSYMBOL_INFO)buffer;
    WCHAR                    filename[MAX_PATH], perms[32];
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T                   res;
    
    // try open the process
    hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if(hProcess==NULL) {
      wprintf(L"Unable to open %s:%lu\n", pid2name(pid), GetLastError());
      return;
    }
    // if symbol is TRUE, try initialize 
    if(symbol && !SymInitialize(hProcess, NULL, TRUE)) {
      wprintf(L"Unable to initialze symbols for %s\n", pid2name(pid));
      return;
    }
    // create snapshot of system
    hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, pid);
    if(hSnap == INVALID_HANDLE_VALUE) return;
    
    te32.dwSize = sizeof(THREADENTRY32);

    // get the first thread
    if(Thread32First(hSnap, &te32)) {
      do {
        // does it match our process?
        if(te32.th32OwnerProcessID == pid) {
          // open the thread
          hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, te32.th32ThreadID);
          if(hThread != NULL) {
            // query the address of TEB
            status = NtQueryInformationThread(hThread, 
              ThreadBasicInformation, &tbi, sizeof(tbi), NULL);
            if(NT_SUCCESS(status)) {
              // try reading the TEB into local memory
              if(ReadProcessMemory(hProcess, tbi.TebBaseAddress, &teb, sizeof(teb), &rd)) {
                // does thread have a thread pool?
                if(teb.ThreadPoolData != NULL) {
                  wprintf(L"\nProcess        : %s:%lu\n", pid2name(pid), pid);
                  wprintf(L"Thread ID      : %lu (0x%lx)\n", te32.th32ThreadID, te32.th32ThreadID);
                  wprintf(L"TEB            : %p\n", tbi.TebBaseAddress);
                  wprintf(L"ThreadPoolData : %p\n\n", teb.ThreadPoolData);
                  // read thread pool
                  if(ReadProcessMemory(hProcess, teb.ThreadPoolData, &tpd, sizeof(tpd), &rd)) {
                    addr = teb.ThreadPoolData;
                    for(i=0;i<sizeof(tpd)/sizeof(ULONG_PTR);i++) {
                      lstrcpy(perms, L"N/A");
                      // get the permissions of address
                      if(tpd.data[i] != 0) {
                        res=VirtualQueryEx(hProcess, (LPVOID)tpd.data[i], &mbi, sizeof(mbi));
                        if(res == sizeof(mbi)) {
                          if(mbi.Protect & PAGE_READWRITE)         lstrcpy(perms, L"RW");
                          if(mbi.Protect & PAGE_READONLY)          lstrcpy(perms, L"R");
                          if(mbi.Protect & PAGE_EXECUTE_READ)      lstrcpy(perms, L"XR");
                          if(mbi.Protect & PAGE_EXECUTE_READWRITE) lstrcpy(perms, L"XRW");
                        }
                      }
                      ZeroMemory(filename, sizeof(filename));
                      GetMappedFileName(hProcess, (LPVOID)tpd.data[i], filename, MAX_PATH);
                      PathStripPath(filename);
                      
                      wprintf(L"0x%p : %p : %3s : %s ", 
                        addr+i*sizeof(ULONG_PTR),
                        (void*)tpd.data[i], perms, filename);
                        
                      // try dump symbol if a memory query succeeded
                      if(symbol && res == sizeof(mbi)) {
                        pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                        pSymbol->MaxNameLen   = MAX_SYM_NAME;
                        
                        if(SymFromAddr(hProcess, tpd.data[i], NULL, pSymbol)) {
                          printf(": %s", pSymbol->Name);
                        }
                      }
                      putchar('\n');
                    }
                  }
                }
              }
            }
            CloseHandle(hThread);
          }
        }
      } while(Thread32Next(hSnap, &te32));
    }
    CloseHandle(hSnap);
    if(symbol) SymCleanup(hProcess);
    CloseHandle(hProcess);
}

// list thread pools for each process on a system
VOID GetSystemThreadPools(DWORD pid, BOOL symbol) {
    HANDLE         hSnap;
    PROCESSENTRY32 pe32;

    // create snapshot of system
    hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(hSnap == INVALID_HANDLE_VALUE) return;
    
    pe32.dwSize = sizeof(PROCESSENTRY32);

    // get first process
    if(Process32First(hSnap, &pe32)){
      do {
        if(pid!=0 && pe32.th32ProcessID != pid) continue;
        if(pe32.th32ProcessID == GetCurrentProcessId()) continue;
        GetProcessThreadPools(pe32.th32ProcessID, symbol);
      } while(Process32Next(hSnap, &pe32));
    }
    CloseHandle(hSnap);
}

int main(void) {
    DWORD   pid=0;
    PWCHAR  *argv, process=NULL;
    int     argc;
    BOOL    symbol=TRUE;  // should probably add an option to disable this
    
    argv = CommandLineToArgvW(GetCommandLine(), &argc);
    
    if(argc==2) {
      process=argv[1];
    }
    // if the user provides parameter
    // assume it's a string name for process or process id
    if(process!=NULL) {
      pid=name2pid(process);
      if(pid==0) pid=_wtoi(process);
      if(pid==0) { 
        wprintf(L"usage: tpplist <process name | process id>\n");
        return 0;
      }
    }
    SetPrivilege(SE_DEBUG_NAME, TRUE);
    SymSetOptions(SYMOPT_DEFERRED_LOADS);
    GetSystemThreadPools(pid, symbol);
    
    return 0;
}