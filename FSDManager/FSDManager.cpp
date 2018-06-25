// FSDManager.cpp : Defines the entry point for the console application.
//
#include "CFSDPortConnector.h"
#include "FSDCommonInclude.h"
#include "FSDCommonDefs.h"
#include "stdio.h"
#include "AutoPtr.h"
#include "FSDThreadUtils.h"
#include "Shlwapi.h"
#include <math.h>
#include <fstream>
#include <vector>
#include "CFSDDynamicByteBuffer.h"
#include <unordered_map>
#include "FSDUmFileUtils.h"
#include <iostream>
#include "Psapi.h"
#include "FSDThreadUtils.h"
#include "FSDProcess.h"
#include "FSDFileInformation.h"
#include "FSDFileExtension.h"

#define TIME_AFTER_KILL_BEFORE_REMOVE 20*1000

using namespace std;

HRESULT HrMain();

unordered_map<wstring, CFileInformation> gFiles;
unordered_map<ULONG, CProcess>           gProcesses;
bool                                     g_fKillMode = false;
bool                                     g_fClearHistory = false;
vector<ULONG>                            gKilledPids;
LARGE_INTEGER                            gLastKill;
LARGE_INTEGER                            gFrequency;

struct THREAD_CONTEXT
{
    bool               fExit;
    CFSDPortConnector* pConnector;
    CAutoStringW       wszScanDir;
};

LPCWSTR MajorTypeToString(ULONG uMajorType)
{
    switch (uMajorType)
    {
    case IRP_CREATE:
        return L"IRP_CREATE";
    case IRP_CLOSE:
        return L"IRP_CLOSE";
    case IRP_READ:
        return L"IRP_READ";
    case IRP_WRITE:
        return L"IRP_WRITE";
    case IRP_QUERY_INFORMATION:
        return L"IRP_QUERY_INFORMATION";
    case IRP_SET_INFORMATION:
        return L"IRP_SET_INFORMATION";
    case IRP_CLEANUP:
        return L"IRP_CLEANUP";
    }

    return L"IRP_UNKNOWN";
}

int main(int argc, char **argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    HRESULT hr = HrMain();
    if (FAILED(hr))
    {
        printf("Main failed with status 0x%x\n", hr);
        return 1;
    }

    return 0;
}

HRESULT ChangeDirectory(CFSDPortConnector* pConnector, THREAD_CONTEXT* pContext, LPCWSTR wszDirectory)
{
    HRESULT hr = S_OK;

    if (!PathFileExistsW(wszDirectory))
    {
        printf("Directory: %ls is not valid\n", wszDirectory);
        return S_OK;
    }

    CAutoStringW wszVolumePath = new WCHAR[50];
    hr = GetVolumePathNameW(wszDirectory, wszVolumePath.Get(), 50);
    RETURN_IF_FAILED(hr);

    size_t cVolumePath = wcslen(wszVolumePath.Get());

    FSD_MESSAGE_FORMAT aMessage;
    aMessage.aType = MESSAGE_TYPE_SET_SCAN_DIRECTORY;
    wcscpy_s(aMessage.wszFileName, MAX_FILE_NAME_LENGTH, wszDirectory + cVolumePath);

    printf("Changing directory to: %ls\n", wszDirectory);

    CAutoStringW wszScanDir;
    hr = NewCopyStringW(&wszScanDir, aMessage.wszFileName, MAX_FILE_NAME_LENGTH);
    RETURN_IF_FAILED(hr);

    wszScanDir.Detach(&pContext->wszScanDir);

    hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), NULL, NULL);
    RETURN_IF_FAILED(hr);

    return S_OK;
}

HRESULT OnChangeDirectoryCmd(CFSDPortConnector* pConnector, THREAD_CONTEXT* pContext)
{
    HRESULT hr = S_OK;

    CAutoStringW wszParameter = new WCHAR[MAX_PARAMETER_LENGTH];
    RETURN_IF_FAILED_ALLOC(wszParameter);

    wscanf_s(L"%ls[/]", wszParameter.Get(), MAX_FILE_NAME_LENGTH);

    hr = ChangeDirectory(pConnector, pContext, wszParameter.Get());
    RETURN_IF_FAILED(hr);

    return S_OK;
}

static const char* szKillProcessLogo =
" -------------------------------------------------- \n"
"                                                    \n"
"                                                    \n"
"                 Process %u KILLED                  \n"
"                                                    \n"
"                                                    \n"
" -------------------------------------------------- \n";

HRESULT KillProcess(ULONG uPid)
{
    CAutoHandle hProcess = OpenProcess(PROCESS_TERMINATE, false, uPid);
    if (!hProcess)
    {
        return E_FAIL;
    }

    bool fSuccess = TerminateProcess(hProcess, 0);
    if (!fSuccess)
    {
        return E_FAIL;
    }

    return S_OK;
}

HRESULT OnSendMessageCmd(CFSDPortConnector* pConnector)
{
    HRESULT hr = S_OK;

    CAutoStringW wszParameter = new WCHAR[MAX_PARAMETER_LENGTH];
    RETURN_IF_FAILED_ALLOC(wszParameter);

    wscanf_s(L"%ls", wszParameter.Get(), MAX_FILE_NAME_LENGTH);

    FSD_MESSAGE_FORMAT aMessage;
    aMessage.aType = MESSAGE_TYPE_PRINT_STRING;
    wcscpy_s(aMessage.wszFileName, MAX_FILE_NAME_LENGTH, wszParameter.Get());

    printf("Sending message: %ls\n", wszParameter.Get());

    BYTE pReply[MAX_STRING_LENGTH];
    DWORD dwReplySize = sizeof(pReply);
    hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), pReply, &dwReplySize);
    RETURN_IF_FAILED(hr);

    if (dwReplySize > 0)
    {
        printf("Recieved response: %ls\n", (WCHAR*)pReply);
    }

    return S_OK;
}

void ManagerKillProcess(CProcess* pProcess)
{
    if (g_fKillMode && !pProcess->IsKilled())
    {
        HRESULT hr = KillProcess(pProcess->GetPid());
        if (FAILED(hr))
        {
            printf(
                "------------------------------------------\n"
                "       Failed to kill process %u          \n"
                "       Reason: 0x%x                       \n"
                "------------------------------------------\n"
                , pProcess->GetPid(), hr);
            return;
        }

        printf(szKillProcessLogo, pProcess->GetPid());
        pProcess->PrintInfo(true);
        
        pProcess->Kill();

        QueryPerformanceCounter(&gLastKill);
        gKilledPids.push_back(pProcess->GetPid());
    }
}

void ProcessIrp(FSD_OPERATION_DESCRIPTION* pOperation, THREAD_CONTEXT* pContext)
{
    //printf("PID: %u MJ: %ls MI: %u\n", pOperation->uPid, MajorTypeToString(pOperation->uMajorType), pOperation->uMinorType);
    
    // Find process in global hash or add if does not exist
    auto process = gProcesses.insert({ pOperation->uPid , CProcess(pOperation->uPid) });
    CProcess* pProcess = &process.first->second;
    
    if (pProcess->IsKilled())
    {
        return;
    }
 
    if (pOperation->uMajorType == IRP_SET_INFORMATION && !pOperation->fCheckForDelete)
    {
        // Rename or move operation
        pProcess->SetFileInfo(pOperation, pContext->wszScanDir.Get());
    }
    else
    {
        // Delete operation and other operations
        auto file = gFiles.insert({ pOperation->GetFileName(), CFileInformation(pOperation->GetFileName()) });
        file.first->second.RegisterAccess(pOperation, pProcess);
    }

    if (pProcess->IsMalicious())
    {
        ManagerKillProcess(pProcess);
    }
}

HRESULT FSDIrpSniffer(PVOID pvContext)
{
    HRESULT hr = S_OK;

    THREAD_CONTEXT* pContext = static_cast<THREAD_CONTEXT*>(pvContext);
    RETURN_IF_FAILED_ALLOC(pContext);

    CFSDPortConnector* pConnector = pContext->pConnector;
    ASSERT(pConnector != NULL);

    CFSDDynamicByteBuffer pBuffer;
    hr = pBuffer.Initialize(1024*8);
    RETURN_IF_FAILED(hr);

    size_t cTotalIrpsRecieved = 0;
    while (!pContext->fExit)
    {
        FSD_MESSAGE_FORMAT aMessage;
        aMessage.aType = MESSAGE_TYPE_QUERY_NEW_OPS;

        BYTE* pResponse = pBuffer.Get();
        DWORD dwReplySize = numeric_cast<DWORD>(pBuffer.ReservedSize());
        hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), pBuffer.Get(), &dwReplySize);
        RETURN_IF_FAILED(hr);

        if (dwReplySize == 0)
        {
            continue;
        }

        FSD_OPERATION_DESCRIPTION* pOpDescription = ((FSD_QUERY_NEW_OPS_RESPONSE_FORMAT*)(PVOID)pResponse)->GetFirst();
        size_t cbData = 0;
        size_t cCurrentIrpsRecieved = 0;
        for (;;)
        {
            if (cbData >= dwReplySize)
            {
                ASSERT(cbData == dwReplySize);
                break;
            }

            try
            {
                ProcessIrp(pOpDescription, pContext);
            }
            catch (...)
            {
                printf("Exception in ProcessIrp!!!\n");
                return S_OK;
            }

            cbData += pOpDescription->PureSize();
            cCurrentIrpsRecieved++;
            pOpDescription = pOpDescription->GetNext();
        }

        cTotalIrpsRecieved += cCurrentIrpsRecieved;

        //printf("Total IRPs: %Iu Current Irps: %Iu Recieve size: %Iu Buffer size: %Iu Buffer utilization: %.2lf%%\n", 
        //    cTotalIrpsRecieved, cCurrentIrpsRecieved, cbData, pBuffer.ReservedSize(), ((double)cbData / pBuffer.ReservedSize() ) * 100);

        if (pBuffer.ReservedSize() < MAX_BUFFER_SIZE && cbData >= pBuffer.ReservedSize()*2/3)
        {
            pBuffer.Grow();
        }

        if (cbData < pBuffer.ReservedSize()/2)
        {
            Sleep(1000);
        }

        LARGE_INTEGER aCurrent;
        QueryPerformanceCounter(&aCurrent);

        LONGLONG llTimeDiff = gLastKill.QuadPart - aCurrent.QuadPart;
        double dftDuration = (double)llTimeDiff * 1000.0 / (double)gFrequency.QuadPart;

        if (dftDuration > TIME_AFTER_KILL_BEFORE_REMOVE)
        {
            for (ULONG uPid : gKilledPids)
            {
                gProcesses.erase(uPid);
            }
        }

        if (g_fClearHistory)
        {
            gProcesses.clear();
            gFiles.clear();

            g_fClearHistory = false;
        }
    }

    return S_OK;
}

HRESULT UserInputParser(PVOID pvContext)
{
    HRESULT hr = S_OK;

    THREAD_CONTEXT* pContext = static_cast<THREAD_CONTEXT*>(pvContext);
    RETURN_IF_FAILED_ALLOC(pContext);

    CFSDPortConnector* pConnector = pContext->pConnector;
    ASSERT(pConnector != NULL);
    
    hr = ChangeDirectory(pConnector, pContext, L"C:\\Users\\User\\");
    RETURN_IF_FAILED(hr);

    CAutoStringW wszCommand = new WCHAR[MAX_COMMAND_LENGTH];
    RETURN_IF_FAILED_ALLOC(wszCommand);

    while (!pContext->fExit)
    {
        printf("Input a command: ");
        wscanf_s(L"%ls", wszCommand.Get(), MAX_COMMAND_LENGTH);
        if (wcscmp(wszCommand.Get(), L"chdir") == 0)
        {
            hr = OnChangeDirectoryCmd(pConnector, pContext);
            RETURN_IF_FAILED(hr);
        } 
        else
        if (wcscmp(wszCommand.Get(), L"message") == 0)
        {
            hr = OnSendMessageCmd(pConnector);
            RETURN_IF_FAILED(hr);
        }
        else
        if (wcscmp(wszCommand.Get(), L"exit") == 0)
        {
            pContext->fExit = true;
            printf("Exiting FSDManager\n");
        }
        else
        if (wcscmp(wszCommand.Get(), L"kill") == 0)
        {
            ULONG uPid;
            if (wscanf_s(L"%u", &uPid))
            {
                printf("Killing process %u FSDManager\n", uPid);
                hr = KillProcess(uPid);
                RETURN_IF_FAILED_EX(hr);
            }
            else
            {
                printf("Failed to read PID\n");
            }
        }
        else
        if (wcscmp(wszCommand.Get(), L"killmode") == 0)
        {
            ULONG uKillMode;
            if (wscanf_s(L"%u", &uKillMode))
            {
                g_fKillMode = uKillMode > 0;
            }
            else
            {
                printf("Failed to read killmode flag\n");
            }
        }
        else
        if (wcscmp(wszCommand.Get(), L"clear") == 0)
        {
            g_fClearHistory = true;
        }
        else
        {
            printf("Invalid command: %ls\n", wszCommand.Get());
        }
    }

    return S_OK;
}

HRESULT HrMain()
{
    HRESULT hr = S_OK;

    CAutoPtr<CFSDPortConnector> pConnector;
    hr = NewInstanceOf<CFSDPortConnector>(&pConnector, g_wszFSDPortName);
    if (hr == E_FILE_NOT_FOUND)
    {
        printf("Failed to connect to FSDefender Kernel module. Try to load it.\n");
    }
    RETURN_IF_FAILED(hr);

    FSD_MESSAGE_FORMAT aMessage;
    aMessage.aType = MESSAGE_TYPE_SET_MANAGER_PID;
    aMessage.uPid = GetCurrentProcessId();

    hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), NULL, NULL);
    RETURN_IF_FAILED(hr);

    THREAD_CONTEXT aContext = {};
    aContext.fExit           = false;
    aContext.pConnector      = pConnector.Get();

    CAutoHandle hFSDIrpSnifferThread;
    hr = UtilCreateThreadSimple(&hFSDIrpSnifferThread, (LPTHREAD_START_ROUTINE)FSDIrpSniffer, (PVOID)&aContext);
    RETURN_IF_FAILED(hr);
    
    CAutoHandle hUserInputParserThread;
    hr = UtilCreateThreadSimple(&hUserInputParserThread, (LPTHREAD_START_ROUTINE)UserInputParser, (PVOID)&aContext);
    RETURN_IF_FAILED(hr);

    hr = WaitForSingleObject(hFSDIrpSnifferThread.Get(), INFINITE);
    RETURN_IF_FAILED(hr);

    hr = WaitForSingleObject(hUserInputParserThread.Get(), INFINITE);
    RETURN_IF_FAILED(hr);

    return S_OK;
}