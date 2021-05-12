﻿#include "NFEventHandler.hpp"

#include "UDPProxy.h"

EventHandler::EventHandler(logfunc func)
{
    doLog = func;
    // m_tcpProxy = new TcpProxy::TCPProxy;
    m_udpProxy = new UdpProxy::UDPProxy;
}
EventHandler::~EventHandler()
{
    delete m_udpProxy;
}

void EventHandler::tcpCanReceive(nfapi::ENDPOINT_ID){};
void EventHandler::tcpCanSend(nfapi::ENDPOINT_ID){};
void EventHandler::udpConnectRequest(nfapi::ENDPOINT_ID, nfapi::PNF_UDP_CONN_REQUEST){};
void EventHandler::udpCanReceive(nfapi::ENDPOINT_ID){};
void EventHandler::udpCanSend(nfapi::ENDPOINT_ID){};

bool EventHandler::init(const unsigned char *g_proxyAddress)
{
    const int size = (((sockaddr *) g_proxyAddress)->sa_family == AF_INET6) ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    if (!m_udpProxy->init(this, (char *) g_proxyAddress, size, username, password))
    {
        printf("Unable to start UDP proxy");
        free();
        return false;
    }
    return true;
}

void EventHandler::free()
{
    m_udpProxy->free();
    AutoLock lock(m_cs);
    for (auto &&[k, v] : m_udpCtxMap)
        delete v;
    m_filteredUdpIds.clear();
}

void EventHandler::onUdpReceiveComplete(unsigned long long id, char *buf, int len, char *remoteAddress, int remoteAddressLen)
{
    (void) remoteAddressLen;
    AutoLock lock(m_cs);

    auto it = m_udpCtxMap.find(id);
    if (it == m_udpCtxMap.end())
        return;

    nf_udpPostReceive(id, (const unsigned char *) remoteAddress, buf, len, it->second->m_options);
}

void EventHandler::tcpConnectRequest(nfapi::ENDPOINT_ID id, nfapi::PNF_TCP_CONN_INFO pConnInfo)
{
    sockaddr_in addrV4;
    memset(&addrV4, 0, sizeof(addrV4));
    addrV4.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addrV4.sin_addr.S_un.S_addr);
    addrV4.sin_port = htons(1089);

    ORIGINAL_CONN_INFO oci;
    memcpy(oci.remoteAddress, pConnInfo->remoteAddress, sizeof(oci.remoteAddress));

    // Save the original remote address
    m_connInfoMap[id] = oci;

    sockaddr *pAddr = (sockaddr *) pConnInfo->remoteAddress;
    int addrLen = (pAddr->sa_family == AF_INET6) ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);

    // Redirect the connection if it is not already redirected
    if (memcmp(pAddr, &addrV4, addrLen) != 0)
    {
        // Change the remote address
        memcpy(pConnInfo->remoteAddress, &addrV4, sizeof(pConnInfo->remoteAddress));
        pConnInfo->filteringFlag |= nfapi::NF_FILTER;
    }
}

void EventHandler::tcpConnected(nfapi::ENDPOINT_ID id, nfapi::PNF_TCP_CONN_INFO pConnInfo)
{
    LogTCP(true, id, pConnInfo);
    fflush(stdout);
}

void EventHandler::tcpClosed(nfapi::ENDPOINT_ID id, nfapi::PNF_TCP_CONN_INFO pConnInfo)
{
    LogTCP(false, id, pConnInfo);
    m_connInfoMap.erase(id);
    fflush(stdout);
}

void EventHandler::tcpReceive(nfapi::ENDPOINT_ID id, const char *buf, int len)
{
    auto it = m_connInfoMap.find(id);
    if (it != m_connInfoMap.end())
    {
        if (!it->second.pendedSends.empty())
        {
            nfapi::nf_tcpPostSend(id, &it->second.pendedSends[0], (int) it->second.pendedSends.size());
        }

        m_connInfoMap.erase(id);
        return;
    }

    // Send the packet to application
    nfapi::nf_tcpPostReceive(id, buf, len);

    // Don't filter the subsequent packets (optimization)
    nfapi::nf_tcpDisableFiltering(id);
}

void EventHandler::tcpSend(nfapi::ENDPOINT_ID id, const char *buf, int len)
{
    auto it = m_connInfoMap.find(id);

    if (it != m_connInfoMap.end())
    {
        SOCKS4_REQUEST request;
        sockaddr_in *pAddr;
        auto it = m_connInfoMap.find(id);
        if (it == m_connInfoMap.end())
            return;

        pAddr = (sockaddr_in *) &it->second.remoteAddress;
        if (pAddr->sin_family == AF_INET6)
        {
            return;
        }

        request.version = SOCKS_4;
        request.command = S4C_CONNECT;
        request.ip = pAddr->sin_addr.S_un.S_addr;
        request.port = pAddr->sin_port;
        request.userid[0] = '\0';

        // Send the request first
        nfapi::nf_tcpPostSend(id, (char *) &request, (int) sizeof(request));

        it->second.pendedSends.insert(it->second.pendedSends.end(), buf, buf + len);

        return;
    }

    // Send the packet to server
    nfapi::nf_tcpPostSend(id, buf, len);
}

void EventHandler::udpCreated(nfapi::ENDPOINT_ID id, nfapi::PNF_UDP_CONN_INFO pConnInfo)
{
    LogUDP(true, id, pConnInfo);

    AutoLock lock(m_cs);
    m_filteredUdpIds.insert(id);
}

void EventHandler::udpClosed(nfapi::ENDPOINT_ID id, nfapi::PNF_UDP_CONN_INFO pConnInfo)
{
    LogUDP(false, id, pConnInfo);

    m_udpProxy->deleteProxyConnection(id);

    AutoLock lock(m_cs);

    auto it = m_udpCtxMap.find(id);
    if (it != m_udpCtxMap.end())
    {
        delete it->second;
        m_udpCtxMap.erase(it);
    }

    m_filteredUdpIds.erase(id);
}

void EventHandler::udpReceive(nfapi::ENDPOINT_ID id, const unsigned char *remoteAddress, const char *buf, int len, nfapi::PNF_UDP_OPTIONS options)
{
    // Send the packet to application
    nfapi::nf_udpPostReceive(id, remoteAddress, buf, len, options);
}

void EventHandler::udpSend(nfapi::ENDPOINT_ID id, const unsigned char *remoteAddress, const char *buf, int len, nfapi::PNF_UDP_OPTIONS options)
{
    {
        AutoLock lock(m_cs);

        auto itid = m_filteredUdpIds.find(id);
        if (itid == m_filteredUdpIds.end())
        {
            nf_udpPostSend(id, remoteAddress, buf, len, options);
            return;
        }

        auto it = m_udpCtxMap.find(id);
        if (it == m_udpCtxMap.end())
        {
            if (!m_udpProxy->createProxyConnection(id))
                return;

            m_udpCtxMap[id] = new UDPContext(options);
        }
    }

    {
        int addrLen = (((sockaddr *) remoteAddress)->sa_family == AF_INET) ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
        if (!m_udpProxy->udpSend(id, (char *) buf, len, (char *) remoteAddress, addrLen))
        {
            nf_udpPostSend(id, remoteAddress, buf, len, options);
        }
    }
}

void EventHandler::LogTCP(bool connected, nfapi::ENDPOINT_ID id, nfapi::PNF_TCP_CONN_INFO pConnInfo)
{
    TCHAR localAddr[MAX_PATH] = L"";
    TCHAR remoteAddr[MAX_PATH] = L"";
    DWORD dwLen;
    sockaddr *pAddr;
    TCHAR processName[MAX_PATH] = L"";

    pAddr = (sockaddr *) pConnInfo->localAddress;
    dwLen = sizeof(localAddr);

    WSAAddressToString((LPSOCKADDR) pAddr, (pAddr->sa_family == AF_INET6) ? sizeof(sockaddr_in6) : sizeof(sockaddr_in), NULL, localAddr, &dwLen);

    pAddr = (sockaddr *) pConnInfo->remoteAddress;
    dwLen = sizeof(remoteAddr);

    WSAAddressToString((LPSOCKADDR) pAddr, (pAddr->sa_family == AF_INET6) ? sizeof(sockaddr_in6) : sizeof(sockaddr_in), NULL, remoteAddr, &dwLen);

    if (!nfapi::nf_getProcessNameFromKernel(pConnInfo->processId, processName, sizeof(processName) / sizeof(processName[0])))
        processName[0] = '\0';

    doLog(id, PROTOCOL::TCP, connected, localAddr, remoteAddr, pConnInfo->processId, processName);
}

void EventHandler::LogUDP(bool created, nfapi::ENDPOINT_ID id, nfapi::PNF_UDP_CONN_INFO pConnInfo)
{
    TCHAR localAddr[MAX_PATH] = L"";
    sockaddr *pAddr;
    DWORD dwLen;
    TCHAR processName[MAX_PATH] = L"";

    pAddr = (sockaddr *) pConnInfo->localAddress;
    dwLen = sizeof(localAddr);

    WSAAddressToString((LPSOCKADDR) pAddr, (pAddr->sa_family == AF_INET6) ? sizeof(sockaddr_in6) : sizeof(sockaddr_in), NULL, localAddr, &dwLen);

    if (nfapi::nf_getProcessNameFromKernel(pConnInfo->processId, processName, sizeof(processName) / sizeof(processName[0])))
        processName[0] = '\0';
    doLog(id, PROTOCOL::UDP, created, localAddr, L"", pConnInfo->processId, processName);
}
