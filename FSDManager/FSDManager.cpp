// FSDManager.cpp : Defines the entry point for the console application.
//
#include "CFSDPortConnector.h"
#include "FSDCommonInclude.h"
#include "FSDCommonDefs.h"
#include "stdio.h"
#include "AutoPtr.h"
#include "FSDThreadUtils.h"
#include "Shlwapi.h"
#include "CFSDDynamicByteBuffer.h"

HRESULT HrMain();

#define MAX_COMMAND_LENGTH 10
#define MAX_PARAMETER_LENGTH 256

#define FSD_INPUT_THREADS_COUNT 8

int main()
{
    HRESULT hr = HrMain();
    if (FAILED(hr))
    {
        printf("Main failed with status 0x%x\n", hr);
        return 1;
    }

    return 0;
}

HRESULT OnChangeDirectoryCmd(CFSDPortConnector* pConnector)
{
    HRESULT hr = S_OK;

    CAutoStringW wszParameter = new WCHAR[MAX_PARAMETER_LENGTH];
    RETURN_IF_FAILED_ALLOC(wszParameter);

    wscanf_s(L"%ls[/]", wszParameter.LetPtr(), MAX_FILE_NAME_LENGTH);

    if (!PathFileExistsW(wszParameter.LetPtr()))
    {
        printf("Directory: %ls is not valid\n", wszParameter.LetPtr());
        return S_OK;
    }

    CAutoStringW wszVolumePath = new WCHAR[50];
    hr = GetVolumePathNameW(wszParameter.LetPtr(), wszVolumePath.LetPtr(), 50);
    RETURN_IF_FAILED(hr);

    size_t cVolumePath = wcslen(wszVolumePath.LetPtr());

    FSD_MESSAGE_FORMAT aMessage;
    aMessage.aType = MESSAGE_TYPE_SET_SCAN_DIRECTORY;
    wcscpy_s(aMessage.wszFileName, MAX_FILE_NAME_LENGTH, wszParameter.LetPtr() + cVolumePath);

    printf("Changing directory to: %ls\n", wszParameter.LetPtr());

    hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), NULL, NULL);
    RETURN_IF_FAILED(hr);

    return S_OK;
}

HRESULT OnSendMessageCmd(CFSDPortConnector* pConnector)
{
    HRESULT hr = S_OK;

    CAutoStringW wszParameter = new WCHAR[MAX_PARAMETER_LENGTH];
    RETURN_IF_FAILED_ALLOC(wszParameter);

    wscanf_s(L"%ls", wszParameter.LetPtr(), MAX_FILE_NAME_LENGTH);

    FSD_MESSAGE_FORMAT aMessage;
    aMessage.aType = MESSAGE_TYPE_PRINT_STRING;
    wcscpy_s(aMessage.wszFileName, MAX_FILE_NAME_LENGTH, wszParameter.LetPtr());

    printf("Sending message: %ls\n", wszParameter.LetPtr());

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

struct THREAD_CONTEXT
{
    bool               fExit;
    CFSDPortConnector* pConnector;
};

HRESULT FSDIrpSniffer(PVOID pvContext)
{
    HRESULT hr = S_OK;

    THREAD_CONTEXT* pContext = static_cast<THREAD_CONTEXT*>(pvContext);
    RETURN_IF_FAILED_ALLOC(pContext);

    CFSDPortConnector* pConnector = pContext->pConnector;
    ASSERT(pConnector != NULL);

    CFSDDynamicByteBuffer pBuffer;
    hr = pBuffer.Initialize(1024);
    RETURN_IF_FAILED(hr);

    size_t cTotalIrpsRecieved = 0;
    while (!pContext->fExit)
    {
        FSD_MESSAGE_FORMAT aMessage;
        aMessage.aType = MESSAGE_TYPE_QUERY_NEW_OPS;
       
        //printf("Sending MESSAGE_TYPE_QUERY_NEW_OPS with reply buffer size %Iu\n", pBuffer.ReservedSize());

        BYTE* pResponse = pBuffer.Get();
        DWORD dwReplySize = numeric_cast<DWORD>(pBuffer.ReservedSize());
        hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), pBuffer.Get(), &dwReplySize);
        RETURN_IF_FAILED(hr);

        if (dwReplySize == 0)
        {
            //printf("Recieved empty response\n");

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

            printf("PID: %u MJ: %u MI: %u\n", pOpDescription->uPid, pOpDescription->uMajorType, pOpDescription->uMinorType);
            ASSERT(pOpDescription->cbData == 0);

            cbData += pOpDescription->PureSize();
            cCurrentIrpsRecieved++;
            pOpDescription = pOpDescription->GetNext();
        }

        cTotalIrpsRecieved += cCurrentIrpsRecieved;

        printf("Total IRPs: %Iu Current recieve: %Iu Buffer size: %Iu\n", cTotalIrpsRecieved, cCurrentIrpsRecieved, pBuffer.ReservedSize());

        if (cbData >= pBuffer.ReservedSize() / 2)
        {
            pBuffer.Grow();
        }
    }

    return S_OK;
}

/*
HRESULT FSDInputParser(PVOID pvContext)
{
    HRESULT hr = S_OK;

    THREAD_CONTEXT* pContext = static_cast<THREAD_CONTEXT*>(pvContext);
    RETURN_IF_FAILED_ALLOC(pContext);
    
    CFSDPortConnector* pConnector = pContext->pConnector;
    ASSERT(pConnector != NULL);

    while (!pContext->fExit)
    {
        DWORD dwMessageSize;
        ULONG64 uCompletionKey;
        LPOVERLAPPED pOverlapped;

        bool res = GetQueuedCompletionStatus(pContext->hCompletionPort, &dwMessageSize, &uCompletionKey, &pOverlapped, INFINITE);
        if (!res)
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            RETURN_IF_FAILED(hr);
        }

        CFSDPortConnectorMessage* pConnectorMessage = CFSDPortConnectorMessage::CastFrom(pOverlapped);
        
        FSD_MESSAGE_FORMAT* pMessage = (FSD_MESSAGE_FORMAT*)pConnectorMessage->pBuffer;
        if (pMessage->aType != MESSAGE_TYPE_SNIFFER_NEW_IRP)
        {
            printf("pMessage->aType == %d\n", pMessage->aType);
        }

        memset(&pConnectorMessage->aOverlapped, 0, sizeof(pConnectorMessage->aOverlapped));

        hr = pConnector->RecieveMessage(pConnectorMessage);
        HR_IGNORE_ERROR(STATUS_IO_PENDING);
        if (FAILED(hr))
        {
            printf("Recieve message failed with status 0x%x\n", hr);
            continue;
        }        
        
        /*
        switch (pMessage->aType)
        {
            case MESSAGE_TYPE_SNIFFER_NEW_IRP:
            {
                //printf("[Sniffer] %ls\n", pMessage->wszFileName);
                break;
            }
            default:
            {
                printf("Unknown message type recieved %d", pMessage->aType);
                ASSERT(false);
            }
        }
    }

    return S_OK;
}
*/

HRESULT UserInputParser(PVOID pvContext)
{
    HRESULT hr = S_OK;

    THREAD_CONTEXT* pContext = static_cast<THREAD_CONTEXT*>(pvContext);
    RETURN_IF_FAILED_ALLOC(pContext);

    CFSDPortConnector* pConnector = pContext->pConnector;
    ASSERT(pConnector != NULL);

    CAutoStringW wszCommand = new WCHAR[MAX_COMMAND_LENGTH];
    RETURN_IF_FAILED_ALLOC(wszCommand);

    while (!pContext->fExit)
    {
        printf("Input a command: ");
        wscanf_s(L"%ls", wszCommand.LetPtr(), MAX_COMMAND_LENGTH);
        if (wcscmp(wszCommand.LetPtr(), L"chdir") == 0)
        {
            hr = OnChangeDirectoryCmd(pConnector);
            RETURN_IF_FAILED(hr);
        } 
        else
        if (wcscmp(wszCommand.LetPtr(), L"message") == 0)
        {
            hr = OnSendMessageCmd(pConnector);
            RETURN_IF_FAILED(hr);
        }
        else
        if (wcscmp(wszCommand.LetPtr(), L"exit") == 0)
        {
            pContext->fExit = true;
            printf("Exiting FSDManager\n");
        }
        else
        {
            printf("Invalid command: %ls\n", wszCommand.LetPtr());
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

    THREAD_CONTEXT aContext = {};
    aContext.fExit           = false;
    aContext.pConnector      = pConnector.LetPtr();

    CAutoHandle hFSDIrpSnifferThread;
    hr = UtilCreateThreadSimple(&hFSDIrpSnifferThread, (LPTHREAD_START_ROUTINE)FSDIrpSniffer, (PVOID)&aContext);
    RETURN_IF_FAILED(hr);
    
    CAutoHandle hUserInputParserThread;
    hr = UtilCreateThreadSimple(&hUserInputParserThread, (LPTHREAD_START_ROUTINE)UserInputParser, (PVOID)&aContext);
    RETURN_IF_FAILED(hr);

    hr = WaitForSingleObject(hFSDIrpSnifferThread.LetPtr(), INFINITE);
    RETURN_IF_FAILED(hr);

    hr = WaitForSingleObject(hUserInputParserThread.LetPtr(), INFINITE);
    RETURN_IF_FAILED(hr);

    return S_OK;
}

