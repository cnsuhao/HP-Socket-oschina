/*
 * Copyright: JessMA Open Source (ldcsaa@gmail.com)
 *
 * Version	: 3.5.3
 * Author	: Bruce Liang
 * Website	: http://www.jessma.org
 * Project	: https://github.com/ldcsaa
 * Blog		: http://www.cnblogs.com/ldcsaa
 * Wiki		: http://www.oschina.net/p/hp-socket
 * QQ Group	: 75375912
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#include "stdafx.h"
#include <atlfile.h>
#include "TcpClient.h"
#include "../../Common/Src/WaitFor.h"

#include <process.h>

BOOL CTcpClient::Start(LPCTSTR lpszRemoteAddress, USHORT usPort, BOOL bAsyncConnect, LPCTSTR lpszBindAddress)
{
	if(!CheckParams() || !CheckStarting())
		return FALSE;

	PrepareStart();
	m_ccContext.Reset();

	BOOL isOK		= FALSE;
	m_bAsyncConnect	= bAsyncConnect;

	if(CreateClientSocket())
	{
		if(BindClientSocket(lpszBindAddress))
		{
			if(FirePrepareConnect(this, m_soClient) != HR_ERROR)
			{
				if(ConnectToServer(lpszRemoteAddress, usPort))
				{
					if(CreateWorkerThread())
						isOK = TRUE;
					else
						SetLastError(SE_WORKER_THREAD_CREATE, __FUNCTION__, ERROR_CREATE_FAILED);
				}
				else
					SetLastError(SE_CONNECT_SERVER, __FUNCTION__, ::WSAGetLastError());
			}
			else
				SetLastError(SE_SOCKET_PREPARE, __FUNCTION__, ERROR_CANCELLED);
		}
		else
			SetLastError(SE_SOCKET_BIND, __FUNCTION__, ::WSAGetLastError());
	}
	else
		SetLastError(SE_SOCKET_CREATE, __FUNCTION__, ::WSAGetLastError());

	if(!isOK)
	{
		m_ccContext.Reset(FALSE);
		Stop();
	}

	return isOK;
}

BOOL CTcpClient::CheckParams()
{
	if((int)m_dwSocketBufferSize > 0)
		if((int)m_dwFreeBufferPoolSize >= 0)
			if((int)m_dwFreeBufferPoolHold >= 0)
				if((int)m_dwKeepAliveTime >= 1000 || m_dwKeepAliveTime == 0)
					if((int)m_dwKeepAliveInterval >= 1000 || m_dwKeepAliveInterval == 0)
						return TRUE;

	SetLastError(SE_INVALID_PARAM, __FUNCTION__, ERROR_INVALID_PARAMETER);
	return FALSE;
}

void CTcpClient::PrepareStart()
{
	m_itPool.SetItemCapacity((int)m_dwSocketBufferSize);
	m_itPool.SetPoolSize((int)m_dwFreeBufferPoolSize);
	m_itPool.SetPoolHold((int)m_dwFreeBufferPoolHold);

	m_itPool.Prepare();
}

BOOL CTcpClient::CheckStarting()
{
	CSpinLock locallock(m_csState);

	if(m_enState == SS_STOPPED)
		m_enState = SS_STARTING;
	else
	{
		SetLastError(SE_ILLEGAL_STATE, __FUNCTION__, ERROR_INVALID_OPERATION);
		return FALSE;
	}

	return TRUE;
}

BOOL CTcpClient::CheckStoping(DWORD dwCurrentThreadID)
{
	if(m_enState == SS_STOPPED)
		return FALSE;

	CSpinLock locallock(m_csState);

	if(HasStarted())
	{
		m_enState = SS_STOPPING;
		return TRUE;
	}
	else if(m_enState == SS_STOPPING)
	{
		if(dwCurrentThreadID != m_dwWorkerID)
		{
			while(m_enState != SS_STOPPED)
				::Sleep(30);
		}

		SetLastError(SE_ILLEGAL_STATE, __FUNCTION__, ERROR_INVALID_OPERATION);
	}

	return FALSE;
}

BOOL CTcpClient::CreateClientSocket()
{
	m_soClient = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if(m_soClient != INVALID_SOCKET)
	{
		BOOL bOnOff	= (m_dwKeepAliveTime > 0 && m_dwKeepAliveInterval > 0);
		VERIFY(::SSO_KeepAliveVals(m_soClient, bOnOff, m_dwKeepAliveTime, m_dwKeepAliveInterval) == NO_ERROR);

		m_evSocket = ::WSACreateEvent();
		ASSERT(m_evSocket != WSA_INVALID_EVENT);

		return TRUE;
	}

	return FALSE;
}

BOOL CTcpClient::BindClientSocket(LPCTSTR lpszBindAddress)
{
	if(lpszBindAddress)
	{
		SOCKADDR_IN bindAddr;
		if(!::sockaddr_A_2_IN(AF_INET, lpszBindAddress, 0, bindAddr))
		{
			::WSASetLastError(WSAEADDRNOTAVAIL);
			return FALSE;
		}

		if(::bind(m_soClient, (struct sockaddr*)&bindAddr, sizeof(SOCKADDR_IN)) == SOCKET_ERROR)
			return FALSE;
	}

	m_dwConnID = ::GenerateConnectionID();

	return TRUE;
}

BOOL CTcpClient::ConnectToServer(LPCTSTR lpszRemoteAddress, USHORT usPort)
{
	TCHAR szAddress[40];
	int iAddressLen = sizeof(szAddress) / sizeof(TCHAR);

	if(!::GetIPAddress(lpszRemoteAddress, szAddress, iAddressLen))
	{
		::WSASetLastError(WSAEADDRNOTAVAIL);
		return FALSE;
	}

	SOCKADDR_IN addr;
	if(!::sockaddr_A_2_IN(AF_INET, szAddress, usPort, addr))
	{
		::WSASetLastError(WSAEADDRNOTAVAIL);
		return FALSE;
	}

	BOOL isOK = FALSE;

	if(m_bAsyncConnect)
	{
		if(::WSAEventSelect(m_soClient, m_evSocket, FD_CONNECT | FD_CLOSE) != SOCKET_ERROR)
		{
			int rc = ::connect(m_soClient, (SOCKADDR*)&addr, sizeof(SOCKADDR_IN));
			isOK = (rc == NO_ERROR || (rc == SOCKET_ERROR && ::WSAGetLastError() == WSAEWOULDBLOCK));
		}
	}
	else
	{
		if(::connect(m_soClient, (SOCKADDR*)&addr, sizeof(SOCKADDR_IN)) != SOCKET_ERROR)
		{
			if(::WSAEventSelect(m_soClient, m_evSocket, FD_READ | FD_WRITE | FD_CLOSE) != SOCKET_ERROR)
			{
				if(FireConnect(this) != HR_ERROR)
				{
					m_enState	= SS_STARTED;
					isOK		= TRUE;
				}
			}
		}
	}

	return isOK;
}

BOOL CTcpClient::CreateWorkerThread()
{
	m_hWorker = (HANDLE)_beginthreadex(nullptr, 0, WorkerThreadProc, (LPVOID)this, 0, &m_dwWorkerID);

	return m_hWorker != nullptr;
}

UINT WINAPI CTcpClient::WorkerThreadProc(LPVOID pv)
{
	TRACE("---------------> Client Worker Thread 0x%08X started <---------------\n", ::GetCurrentThreadId());

	BOOL bCallStop		= TRUE;
	CTcpClient* pClient	= (CTcpClient*)pv;
	HANDLE hEvents[]	= {pClient->m_evSocket, pClient->m_evBuffer, pClient->m_evWorker};

	pClient->m_rcBuffer.Malloc(pClient->m_dwSocketBufferSize);

	while(pClient->HasStarted())
	{
		DWORD retval = ::WSAWaitForMultipleEvents(3, hEvents, FALSE, WSA_INFINITE, FALSE);

		if(retval == WSA_WAIT_EVENT_0)
		{
			if(!pClient->ProcessNetworkEvent())
				break;
		}
		else if(retval == WSA_WAIT_EVENT_0 + 1)
		{
			if(!pClient->SendData())
				break;
		}
		else if(retval == WSA_WAIT_EVENT_0 + 2)
		{
			bCallStop = FALSE;
			break;
		}
		else
			ASSERT(FALSE);
	}

	pClient->OnWorkerThreadEnd(::GetCurrentThreadId());

	if(bCallStop && pClient->HasStarted())
		pClient->Stop();

	TRACE("---------------> Client Worker Thread 0x%08X stoped <---------------\n", ::GetCurrentThreadId());

	return 0;
}

BOOL CTcpClient::ProcessNetworkEvent()
{
	BOOL bContinue = TRUE;
	WSANETWORKEVENTS events;
	
	int rc = ::WSAEnumNetworkEvents(m_soClient, m_evSocket, &events);

	if(rc == SOCKET_ERROR)
		bContinue = HandleError(events);

	if(m_bAsyncConnect && bContinue && events.lNetworkEvents & FD_CONNECT)
		bContinue = HandleConnect(events);

	if(bContinue && events.lNetworkEvents & FD_READ)
		bContinue = HandleRead(events);

	if(bContinue && events.lNetworkEvents & FD_WRITE)
		bContinue = HandleWrite(events);

	if(bContinue && events.lNetworkEvents & FD_CLOSE)
		bContinue = HandleClose(events);

	return bContinue;
}

BOOL CTcpClient::HandleError(WSANETWORKEVENTS& events)
{
	int iCode						= ::WSAGetLastError();
	EnSocketOperation enOperation	= SO_UNKNOWN;

	if(events.lNetworkEvents & FD_CONNECT)
		enOperation = SO_CONNECT;
	else if(events.lNetworkEvents & FD_CLOSE)
		enOperation = SO_CLOSE;
	else if(events.lNetworkEvents & FD_READ)
		enOperation = SO_RECEIVE;
	else if(events.lNetworkEvents & FD_WRITE)
		enOperation = SO_SEND;

	VERIFY(::WSAResetEvent(m_evSocket));
	m_ccContext.Reset(TRUE, enOperation, iCode);

	return FALSE;
}

BOOL CTcpClient::HandleRead(WSANETWORKEVENTS& events)
{
	BOOL bContinue	= TRUE;
	int iCode		= events.iErrorCode[FD_READ_BIT];

	if(iCode == 0)
		bContinue = ReadData();
	else
	{
		m_ccContext.Reset(TRUE, SO_RECEIVE, iCode);
		bContinue = FALSE;
	}

	return bContinue;
}

BOOL CTcpClient::HandleWrite(WSANETWORKEVENTS& events)
{
	BOOL bContinue	= TRUE;
	int iCode		= events.iErrorCode[FD_WRITE_BIT];

	if(iCode == 0)
		bContinue = SendData();
	else
	{
		m_ccContext.Reset(TRUE, SO_SEND, iCode);
		bContinue = FALSE;
	}

	return bContinue;
}

BOOL CTcpClient::HandleConnect(WSANETWORKEVENTS& events)
{
	BOOL bContinue	= TRUE;
	int iCode		= events.iErrorCode[FD_CONNECT_BIT];

	if(iCode == 0)
	{
		if(::WSAEventSelect(m_soClient, m_evSocket, FD_READ | FD_WRITE | FD_CLOSE) != SOCKET_ERROR)
		{
			if(FireConnect(this) != HR_ERROR)
				m_enState = SS_STARTED;
			else
				iCode = ERROR_CANCELLED;
		}
		else
			iCode = ::WSAGetLastError();
	}

	if(iCode != 0)
	{
		if(iCode != ERROR_CANCELLED)
			m_ccContext.Reset(TRUE, SO_CONNECT, iCode);
		else
			m_ccContext.Reset(FALSE);

		bContinue = FALSE;
	}

	return bContinue;
}

BOOL CTcpClient::HandleClose(WSANETWORKEVENTS& events)
{
	int iCode = events.iErrorCode[FD_CLOSE_BIT];

	if(iCode == 0)
		m_ccContext.Reset(TRUE, SO_CLOSE, SE_OK);
	else
		m_ccContext.Reset(TRUE, SO_CLOSE, iCode);

	return FALSE;
}

BOOL CTcpClient::ReadData()
{
	while(TRUE)
	{
		int rc = recv(m_soClient, (char*)(BYTE*)m_rcBuffer, m_dwSocketBufferSize, 0);

		if(rc > 0)
		{
			if(FireReceive(this, m_rcBuffer, rc) == HR_ERROR)
			{
				TRACE("<C-CNNID: %Iu> OnReceive() event return 'HR_ERROR', connection will be closed !\n", m_dwConnID);

				m_ccContext.Reset(TRUE, SO_RECEIVE, ERROR_CANCELLED);
				return FALSE;
			}
		}
		else if(rc == SOCKET_ERROR)
		{
			int code = ::WSAGetLastError();

			if(code == WSAEWOULDBLOCK)
				break;
			else
			{
				m_ccContext.Reset(TRUE, SO_RECEIVE, code);
				return FALSE;
			}
		}
		else if(rc == 0)
		{
			m_ccContext.Reset(TRUE, SO_CLOSE, SE_OK);
			return FALSE;
		}
		else
			ASSERT(FALSE);
	}

	return TRUE;
}

BOOL CTcpClient::SendData()
{
	BOOL isOK = TRUE;

	while(TRUE)
	{
		TItemPtr itPtr(m_itPool, GetSendBuffer());

		if(itPtr.IsValid())
		{
			ASSERT(!itPtr->IsEmpty());

			isOK = DoSendData(itPtr);

			if(isOK)
			{
				if(!itPtr->IsEmpty())
				{
					CCriSecLock locallock(m_csSend);
					m_lsSend.PushFront(itPtr.Detach());
					
					break;
				}
			}
			else
				break;
		}
		else
			break;
	}

	return isOK;
}

TItem* CTcpClient::GetSendBuffer()
{
	TItem* pItem = nullptr;

	if(m_lsSend.Size() > 0)
	{
		CCriSecLock locallock(m_csSend);

		if(m_lsSend.Size() > 0)
			pItem = m_lsSend.PopFront();
	}

	return pItem;
}

BOOL CTcpClient::DoSendData(TItem* pItem)
{
	while(!pItem->IsEmpty())
	{
		int rc = 0;

		{
			CCriSecLock locallock(m_csSend);

			rc = send(m_soClient, (char*)pItem->Ptr(), min(pItem->Size(), (int)m_dwSocketBufferSize), 0);
			if(rc > 0) m_iPending -= rc;
		}

		if(rc > 0)
		{
			if(FireSend(this, pItem->Ptr(), rc) == HR_ERROR)
			{
				TRACE("<C-CNNID: %Iu> OnSend() event should not return 'HR_ERROR' !!\n", m_dwConnID);
				ASSERT(FALSE);
			}

			pItem->Reduce(rc);
		}
		else if(rc == SOCKET_ERROR)
		{
			int code = ::WSAGetLastError();

			if(code == WSAEWOULDBLOCK)
				break;
			else
			{
				m_ccContext.Reset(TRUE, SO_SEND, code);
				return FALSE;
			}
		}
		else
			ASSERT(FALSE);
	}

	return TRUE;
}

BOOL CTcpClient::Stop()
{
	DWORD dwCurrentThreadID = ::GetCurrentThreadId();

	if(!CheckStoping(dwCurrentThreadID))
		return FALSE;

	WaitForWorkerThreadEnd(dwCurrentThreadID);

	if(m_ccContext.bFireOnClose)
		FireClose(this, m_ccContext.enOperation, m_ccContext.iErrorCode);

	if(m_evSocket != nullptr)
	{
		::WSACloseEvent(m_evSocket);
		m_evSocket	= nullptr;
	}

	if(m_soClient != INVALID_SOCKET)
	{
		shutdown(m_soClient, SD_SEND);
		closesocket(m_soClient);
		m_soClient	= INVALID_SOCKET;
	}

	Reset();

	return TRUE;
}

void CTcpClient::Reset()
{
	m_rcBuffer.Free();
	m_evBuffer.Reset();
	m_evWorker.Reset();
	m_lsSend.Clear();
	m_itPool.Clear();

	m_iPending	= 0;
	m_enState	= SS_STOPPED;
}

void CTcpClient::WaitForWorkerThreadEnd(DWORD dwCurrentThreadID)
{
	if(m_hWorker != nullptr)
	{
		if(dwCurrentThreadID != m_dwWorkerID)
		{
			m_evWorker.Set();
			VERIFY(::WaitForSingleObject(m_hWorker, INFINITE) == WAIT_OBJECT_0);
		}

		::CloseHandle(m_hWorker);

		m_hWorker		= nullptr;
		m_dwWorkerID	= 0;
	}
}

BOOL CTcpClient::Send(const BYTE* pBuffer, int iLength, int iOffset)
{
	ASSERT(pBuffer && iLength > 0);

	if(iOffset != 0) pBuffer += iOffset;

	WSABUF buffer;
	buffer.len = iLength;
	buffer.buf = (char*)pBuffer;

	return SendPackets(&buffer, 1);
}

BOOL CTcpClient::DoSendPackets(const WSABUF pBuffers[], int iCount)
{
	ASSERT(pBuffers && iCount > 0);

	int result = NO_ERROR;

	if(pBuffers && iCount > 0)
	{
		if(HasStarted())
		{
			CCriSecLock locallock(m_csSend);

			if(HasStarted())
				result = SendInternal(pBuffers, iCount);
			else
				result = ERROR_INVALID_STATE;
		}
		else
			result = ERROR_INVALID_STATE;
	}
	else
		result = ERROR_INVALID_PARAMETER;

	if(result != NO_ERROR)
		::SetLastError(result);

	return (result == NO_ERROR);
}

BOOL CTcpClient::SendInternal(const WSABUF pBuffers[], int iCount)
{
	int result = NO_ERROR;

	ASSERT(m_iPending >= 0);

	int iPending	= m_iPending;
	BOOL isPending	= m_iPending > 0;

	for(int i = 0; i < iCount; i++)
	{
		int iBufLen = pBuffers[i].len;

		if(iBufLen > 0)
		{
			BYTE* pBuffer = (BYTE*)pBuffers[i].buf;
			ASSERT(pBuffer);

			m_lsSend.Cat(pBuffer, iBufLen);
			m_iPending += iBufLen;
		}
	}

	if(!isPending && m_iPending > iPending) m_evBuffer.Set();

	return result;
}

BOOL CTcpClient::SendSmallFile(LPCTSTR lpszFileName, const LPWSABUF pHead, const LPWSABUF pTail)
{
	ASSERT(lpszFileName != nullptr);

	CAtlFile file;
	HRESULT hr = file.Create(lpszFileName, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);

	if(SUCCEEDED(hr))
	{
		ULONGLONG ullLen;
		hr = file.GetSize(ullLen);

		if(SUCCEEDED(hr))
		{
			ULONGLONG ullTotal = ullLen + (pHead ? pHead->len : 0) + (pTail ? pTail->len : 0);

			if(ullLen > 0 && ullTotal <= MAX_SMALL_FILE_SIZE)
			{
				CAtlFileMapping<> fmap;
				hr = fmap.MapFile(file);

				if(SUCCEEDED(hr))
				{
					WSABUF bufs[3] = {0};

					bufs[1].len = (ULONG)ullLen;
					bufs[1].buf = fmap;

					if(pHead) memcpy(&bufs[0], pHead, sizeof(WSABUF));
					if(pTail) memcpy(&bufs[2], pTail, sizeof(WSABUF));

					return SendPackets(bufs, 3);
				}
			}
			else if(ullLen == 0)
				hr = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
			else
				hr = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
		}
	}

	::SetLastError(hr & 0x0000FFFF);

	return FALSE;
}

void CTcpClient::SetLastError(EnSocketError code, LPCSTR func, int ec)
{
	TRACE("%s --> Error: %d, EC: %d\n", func, code, ec);

	m_enLastError = code;
	::SetLastError(ec);
}

BOOL CTcpClient::GetLocalAddress(TCHAR lpszAddress[], int& iAddressLen, USHORT& usPort)
{
	ASSERT(lpszAddress != nullptr && iAddressLen > 0);

	return ::GetSocketLocalAddress(m_soClient, lpszAddress, iAddressLen, usPort);
}
