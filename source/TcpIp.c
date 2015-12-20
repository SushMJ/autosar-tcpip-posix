/* Copyright (C) 2015 Joakim Plate
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "TcpIp.h"
#include "TcpIp_Cfg.h"
#include "SoAd_Cbk.h"


#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <errno.h>

#define TCPIP_MODULEID   170u
#define TCPIP_INSTANCEID 0u

#ifndef TCPIP_CFG_TRANSMIT_STACKSIZE
#define TCPIP_CFG_TRANSMIT_STACKSIZE 128
#endif

#ifndef TCPIP_CFG_ENABLE_DEVELOPMENT_ERROR
#define TCPIP_CFG_ENABLE_DEVELOPMENT_ERROR STD_OFF
#endif

#if(TCPIP_CFG_ENABLE_DEVELOPMENT_ERROR == STD_ON)
#include "Det.h"
#define TCPIP_DET_ERROR(api, error) Det_ReportError(TCPIP_MODULEID, TCPIP_INSTANCEID, api, error)
#define TCPIP_DET_CHECK_RET(check, api, error)       \
    do {                                             \
        if (!(check)) {                              \
            (void)Det_ReportError(TCPIP_MODULEID     \
                                , TCPIP_INSTANCEID   \
                                , api                \
                                , error);            \
            return E_NOT_OK;                         \
        }                                            \
    } while(0)

#else
#define TCPIP_DET_ERROR(api, error)
#define TCPIP_DET_CHECK_RET(check, api, error)
#endif

typedef int TcpIp_OsSocketType;

#define INVALID_SOCKET (TcpIp_OsSocketType)-1
#define closesocket(x) close(x)

const TcpIp_ConfigType* TcpIp_Config;


typedef enum {
    TCPIP_SOCKET_STATE_UNUSED,
    TCPIP_SOCKET_STATE_ALLOCATED,
    TCPIP_SOCKET_STATE_BOUND,
    TCPIP_SOCKET_STATE_LISTEN,
    TCPIP_SOCKET_STATE_CONNECTING,
    TCPIP_SOCKET_STATE_CONNECTED,
    TCPIP_SOCKET_STATE_TCPCLOSE,
} TcpIp_SocketStateType;

typedef struct {
    TcpIp_ProtocolType    protocol;
    TcpIp_DomainType      domain;
    TcpIp_SocketStateType state;
    TcpIp_OsSocketType    fd;
} TcpIp_SocketType;

TcpIp_SocketType      TcpIp_Sockets[TCPIP_CFG_MAX_SOCKETS];
struct pollfd         TcpIp_PollFds[TCPIP_CFG_MAX_SOCKETS];

static void TcpIp_SocketState_Enter(TcpIp_SocketIdType index, TcpIp_SocketStateType state);

static void TcpIp_InitSocket(TcpIp_SocketIdType id)
{
    TcpIp_SocketType* s = &TcpIp_Sockets[id];
    memset(s, 0, sizeof(*s));
    s->state = TCPIP_SOCKET_STATE_UNUSED;
    s->fd = INVALID_SOCKET;
}


static sint8 TcpIp_GetBsdTypeFromProtocol(TcpIp_ProtocolType  protocol)
{
    sint8 res;
    switch(protocol) {
        case TCPIP_IPPROTO_TCP:
            res = SOCK_STREAM;
            break;
        case TCPIP_IPPROTO_UDP:
            res = SOCK_DGRAM;
            break;
        default:
            res = 0;
    }
    return res;
}

static sint8 TcpIp_GetBsdDomainFromDomain(TcpIp_DomainType domain)
{
    sint8 res;

    switch(domain) {
        case TCPIP_AF_INET:
            res = AF_INET;
            break;
        case TCPIP_AF_INET6:
            res = AF_INET6;
            break;
        default:
            res = 0;
    }
    return res;
}

static Std_ReturnType TcpIp_GetBsdSockaddrFromSocketAddr(struct sockaddr_storage* trg, const TcpIp_SockAddrType* src)
{
    Std_ReturnType res;
    memset(trg, 0, sizeof(*trg));
    if (src->domain == TCPIP_AF_INET) {
        TcpIp_SockAddrInetType* inet = (TcpIp_SockAddrInetType*)src;
        struct sockaddr_in*     in   = (struct sockaddr_in*)trg;
        in->sin_family      = AF_INET;
        in->sin_port        = inet->port;
        in->sin_len         = sizeof(*in);
        in->sin_addr.s_addr = inet->addr[0];
        res = E_OK;
    } else if (src->domain == TCPIP_AF_INET6) {
        TcpIp_SockAddrInet6Type* inet = (TcpIp_SockAddrInet6Type*)src;
        struct sockaddr_in6*     in6  = (struct sockaddr_in6*)trg;
        in6->sin6_family = AF_INET6;
        in6->sin6_port   = inet->port;
        in6->sin6_addr.__u6_addr.__u6_addr32[0] = inet->addr[0];
        in6->sin6_addr.__u6_addr.__u6_addr32[1] = inet->addr[1];
        in6->sin6_addr.__u6_addr.__u6_addr32[2] = inet->addr[2];
        in6->sin6_addr.__u6_addr.__u6_addr32[3] = inet->addr[3];
        in6->sin6_len    = sizeof(*in6);
        res = E_OK;
    } else {
        res = E_NOT_OK;
    }
    return res;
}


static Std_ReturnType TcpIp_GetSockaddrFromBsdSocketAddr(TcpIp_SockAddrStorageType* trg, const struct sockaddr* src)
{
    Std_ReturnType res;
    memset(trg, 0, sizeof(*trg));
    if (src->sa_family == AF_INET) {
        struct sockaddr_in*     in   = (struct sockaddr_in*)src;
        trg->inet.domain  = TCPIP_AF_INET;
        trg->inet.port    = in->sin_port;
        trg->inet.addr[0] = in->sin_addr.s_addr;
        res = E_OK;
    } else if (src->sa_family == AF_INET6) {
        struct sockaddr_in6*     in6  = (struct sockaddr_in6*)trg;
        trg->inet6.domain  = TCPIP_AF_INET6;
        trg->inet6.port    = in6->sin6_port;
        trg->inet6.addr[0] = in6->sin6_addr.__u6_addr.__u6_addr32[0];
        trg->inet6.addr[1] = in6->sin6_addr.__u6_addr.__u6_addr32[1];
        trg->inet6.addr[2] = in6->sin6_addr.__u6_addr.__u6_addr32[2];
        trg->inet6.addr[3] = in6->sin6_addr.__u6_addr.__u6_addr32[3];
        res = E_OK;
    } else {
        res = E_NOT_OK;
    }
    return res;
}

static Std_ReturnType TcpIp_SetBlockingState(TcpIp_OsSocketType fd, boolean blocking)
{
    Std_ReturnType     res;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        res = E_NOT_OK;
    } else {
        if (blocking) {
            flags &= ~O_NONBLOCK;
        } else {
            flags |= O_NONBLOCK;
        }
        flags = fcntl(fd, F_SETFL, flags);
        if (flags < 0) {
            res = E_NOT_OK;
        } else {
            res = E_OK;
        }
    }
    return res;
}
/**
 * @brief This service initializes the TCP/IP Stack.
 *
 * TcpIp_Init may not block the start-up process for an indefinite amount of time.
 * @note The call of this service is mandatory before using the TcpIp instance for further processing.
 * @param[in] config Pointer to the configuration data of the TcpIp module
 */
void TcpIp_Init(const TcpIp_ConfigType* config)
{
    TcpIp_SocketIdType id;
    TcpIp_Config = config;

    for (id = 0u; id < TCPIP_CFG_MAX_SOCKETS; ++id) {
        TcpIp_InitSocket(id);
    }
}

/**
 * @brief By this API service the TCP/IP stack is requested to close the socket and release all related resources.
 * @param[in] Abort TRUE:  connection will immediately be terminated by sending a
 *                         RST-Segment and releasing all related resources.
 *                  FALSE: connection will be terminated after performing a regular
 *                         connection termination handshake and releasing all related
 *                         resources.
 * @return    E_OK:     The request has been accepted
 *            E_NOT_OK: The request has not been accepted.
 *
 * @req SWS_TCPIP_00017
 * @req SWS_TCPIP_00109
 * @req SWS_TCPIP_00110
 */
Std_ReturnType TcpIp_Close(
        TcpIp_SocketIdType          id,
        boolean                     abort
    )
{
    TcpIp_SocketType* s = &TcpIp_Sockets[id];
    Std_ReturnType   res;

    if (s->protocol == TCPIP_IPPROTO_TCP) {
        if (abort) {
            TcpIp_SocketState_Enter(id, TCPIP_SOCKET_STATE_UNUSED);
            SoAd_TcpIpEvent(id, TCPIP_TCP_RESET);
        } else {
            if (s->state == TCPIP_SOCKET_STATE_CONNECTED) {
                if (shutdown(s->fd, SHUT_RDWR) == 0) {
                    TcpIp_SocketState_Enter(id, TCPIP_SOCKET_STATE_TCPCLOSE);
                    res = E_OK;
                } else {
                    res = E_NOT_OK;
                }
            } else {
                TcpIp_SocketState_Enter(id, TCPIP_SOCKET_STATE_UNUSED);
                SoAd_TcpIpEvent(id, TCPIP_TCP_CLOSED);
            }
        }
    } else if (s->protocol == TCPIP_IPPROTO_UDP) {
        TcpIp_SocketState_Enter(id, TCPIP_SOCKET_STATE_UNUSED);
        SoAd_TcpIpEvent(id, TCPIP_UDP_CLOSED);
        res = E_OK;
    } else {
        res = E_NOT_OK;
    }

    return res;
}

/**
 * @brief By this API service the TCP/IP stack is requested to bind a UDP or TCP socket to a local resource.
 * @param[in] id          Socket identifier of the related local socket resource.
 * @param[in] local_addr  IP address identifier representing the local IP address and EthIf
 *                        controller to bind the socket to.
 *                        Note: to listen to all EthIf controller, TCPIP_LOCALADDRID_ANY
 *                              has to be specified as LocalAddrId.
 *                        Note: to listen on any IP addresss of a EthIf controller, the
 *                              configuration parameter TcpIpStaticIpAddress referenced by
 *                              LocalAddrId must be set to "ANY". The remote IP address of an
 *                              incoming packet has no effect then.
 *                        In case the socket shall be used as client socket, the IP address
 *                              and EthIf controller represented by LocalAddrId is used for
 *                              transmission.
 *                        Note: for an automatic selection of the Local IP address and EthIf
 *                              Controller, TCPIP_LOCALADDRID_ANY has to be specified as
 *                              LocalAddrId
 * @param[in,out] port    Pointer to memory where the local port to which the socket shall
 *                        be bound is specified. In case the parameter is specified as
 *                        TCPIP_PORT_ANY, the TCP/IP stack shall choose the local port
 *                        automatically from the range 49152 to 65535 and shall update the
 *                        parameter to the chosen value.
 *
 * @req SWS_TCPIP_00015
 *
 */
Std_ReturnType TcpIp_Bind(
        TcpIp_SocketIdType          id,
        TcpIp_LocalAddrIdType       local_addr,
        uint16*                     port
    )
{
    TcpIp_SocketType* s = &TcpIp_Sockets[id];
    Std_ReturnType    res;
    union {
        struct sockaddr_in  in;
        struct sockaddr_in6 in6;
    } addr = {};
    socklen_t len;

    if (local_addr != TCPIP_LOCALADDRID_ANY) {
        /** @req SWS_TCPIP_00111-TODO */
        /** @req SWS_TCPIP_00147-TODO */
        res = E_NOT_OK;
        goto done;
    }

    if (s->domain == TCPIP_AF_INET) {
        addr.in.sin_family   = AF_INET;
        addr.in.sin_port     = *port;
        len = sizeof(addr.in);
    } else if (s->domain == TCPIP_AF_INET6) {
        addr.in6.sin6_family = AF_INET6;
        addr.in6.sin6_port   = *port;
        len = sizeof(addr.in6);
    } else {
        res = E_NOT_OK;
        goto done;
    }

    if (bind(s->fd, (const struct sockaddr*)&addr, len) != 0) {
        if (errno == EADDRINUSE) {
            /** @req SWS_TCPIP_00146 */
            TCPIP_DET_ERROR(TCPIP_API_BIND, TCPIP_E_ADDRINUSE);
        }
        res = E_NOT_OK;
        goto done;
    }

    len = sizeof(addr);
    if (getsockname(s->fd, (struct sockaddr*)&addr, &len) != 0) {
        res = E_NOT_OK;
        goto done;
    }
    if (addr.in.sin_family == AF_INET) {
        *port = addr.in.sin_port;
    } else if (addr.in6.sin6_family == AF_INET6) {
        *port = addr.in6.sin6_port;
    } else {
        res = E_NOT_OK;
        goto done;
    }

    res = E_OK;
    TcpIp_SocketState_Enter(id, TCPIP_SOCKET_STATE_BOUND);

done:
    return res;
}

/**
 * @brief By this API service the TCP/IP stack is requested to establish a TCP connection to the configured peer.
 * @warn Reentrant for different SocketIds. Non reentrant for the same SocketId.
 * @info Asynchronous
 * @req  SWS_TCPIP_00022
 *
 * @param[in] id     Socket identifier of the related local socket resource.
 * @param[in] remote IP address and port of the remote host to connect to.
 * @return E_OK:     The request has been accepted
 *         E_NOT_OK: The request has not been accepted, e.g. connection
 *                   is already established or no route to destination specified by
 *                   remoteAddrPtr found.
 */
Std_ReturnType TcpIp_TcpConnect(
        TcpIp_SocketIdType          id,
        const TcpIp_SockAddrType*   remote
    )
{
    TcpIp_SocketType* s = &TcpIp_Sockets[id];
    Std_ReturnType    res;

    struct sockaddr_storage  addr;
    socklen_t len;

    TCPIP_DET_CHECK_RET(remote != NULL_PTR, TCPIP_API_TCPCONNECT, TCPIP_E_PARAM_POINTER);

    if (remote->domain != s->domain) {
        return E_NOT_OK;
    }

    if (TcpIp_GetBsdSockaddrFromSocketAddr(&addr, remote) != E_OK) {
        return E_NOT_OK;
    }

    if (TcpIp_SetBlockingState(s->fd, FALSE) != E_OK) {
        return E_NOT_OK;
    }

    int v = connect(s->fd, (const struct sockaddr*)&addr, addr.ss_len);
    if (v != 0) {
        v = errno;
    }

    if (v == 0) {
        TcpIp_SocketState_Enter(id, TCPIP_SOCKET_STATE_CONNECTED);
    } else if(v == EINPROGRESS) {
        TcpIp_SocketState_Enter(id, TCPIP_SOCKET_STATE_CONNECTING);
    } else {
        return E_NOT_OK;
    }

    return E_OK;
}

/**
 * @brief By this API service the TCP/IP stack is requested to listen on the TCP socket specified by the socket identifier.
 * @warn Reentrant for different SocketIds. Non reentrant for the same SocketId.
 * @info Asynchronous
 * @req  SWS_TCPIP_00023
 *
 * @param[in] id       Socket identifier of the related local socket resource.
 * @param[in) channels Maximum number of new parallel connections established on
 *                     this listen connection.
 *
 * @return E_OK:     The request has been accepted
 *         E_NOT_OK: Maximum number of new parallel connections established on
 *                   this listen connection.
 */
Std_ReturnType TcpIp_TcpListen(
        TcpIp_SocketIdType id,
        uint16             channels
    )
{
    TcpIp_SocketType* s = &TcpIp_Sockets[id];
    Std_ReturnType    res;

    /**
     * @req SWS_TCPIP_00113
     * @req SWS_TCPIP_00114
     */
    if (listen(s->fd, channels) == 0) {
        res = E_OK;
        TcpIp_SocketState_Enter(id, TCPIP_SOCKET_STATE_LISTEN);
    } else {
        res = E_NOT_OK;
    }
    return res;
}

/**
 * @brief This service transmits data via UDP to a remote node. The transmission of the
 *        data is immediately performed with this function call by forwarding it to EthIf.
 * @warn  Reentrant for different SocketIds. Non reentrant for the same SocketId.
 * @info  Synchronous
 *
 * @param[in] id     Socket identifier of the related local socket resource.
 * @param[in] data   Pointer to a linear buffer of TotalLength bytes containing the
 *                   data to be transmitted.
 *                   In case DataPtr is a NULL_PTR, TcpIp shall retrieve data from
 *                   upper layer via callback <Up>_CopyTxData().
 * @param[in] remote IP address and port of the remote host to transmit to.
 * @param[in] len    indicates the payload size of the UDP datagram.
 */
Std_ReturnType TcpIp_UdpTransmit(
        TcpIp_SocketIdType        id,
        const uint8*              data,
        const TcpIp_SockAddrType* remote,
        uint16                    len
    )
{
    TcpIp_SocketType* s = &TcpIp_Sockets[id];
    int v;
    Std_ReturnType res;
    struct sockaddr_storage  addr;

    if (remote->domain != s->domain) {
        TCPIP_DET_ERROR(TCPIP_API_UDPTRANSMIT, TCPIP_E_PROTOCOL);
        return E_NOT_OK;
    }

    if (TcpIp_GetBsdSockaddrFromSocketAddr(&addr, remote) != E_OK) {
        TCPIP_DET_ERROR(TCPIP_API_UDPTRANSMIT, TCPIP_E_INV_ARG);
        return E_NOT_OK;
    }

    if (TcpIp_SetBlockingState(s->fd, TRUE) != E_OK) {
        return E_NOT_OK;
    }

    while (len > 0u) {
        const uint8*  src;
        uint16        src_len;

        if (data) {
            src     = data;
            src_len = len;
        } else {
            uint8 buf[TCPIP_CFG_TRANSMIT_STACKSIZE];
            src     = buf;
            src_len = TCPIP_CFG_TRANSMIT_STACKSIZE;
            if (src_len > len) {
                src_len = len;
            }
            if (SoAd_CopyTxData(s->fd, buf, src_len) != BUFREQ_OK) {
                return E_NOT_OK;
            }
        }

        v = sendto(s->fd, src, src_len, 0, (struct sockaddr *)&addr, addr.ss_len);

        if (v == -1) {
            if (errno == EMSGSIZE) {
                TCPIP_DET_ERROR(TCPIP_API_UDPTRANSMIT, TCPIP_E_MSGSIZE);
            }
            return E_NOT_OK;
        } else if (v != src_len) {
            TCPIP_DET_ERROR(TCPIP_API_UDPTRANSMIT, TCPIP_E_MSGSIZE);
            return E_NOT_OK;
        } else {
            len -= v;
        }
    }

    return E_OK;
}

Std_ReturnType TcpIp_TcpTransmit(
        TcpIp_SocketIdType  id,
        const uint8*        data,
        uint32              aailable,
        boolean             force
    )
{
    return E_NOT_OK;
}

static Std_ReturnType TcpIp_GetFreeSocket(TcpIp_SocketIdType* socketid)
{
    TcpIp_SocketIdType index;
    Std_ReturnType     res;

    for (index = 0u; index < TCPIP_CFG_MAX_SOCKETS; ++index) {
        if (TcpIp_Sockets[index].state == TCPIP_SOCKET_STATE_UNUSED) {
            break;
        }
    }
    if (index < TCPIP_CFG_MAX_SOCKETS) {
        *socketid = index;
        res = E_OK;
    } else {
        res = E_NOT_OK;
    }
    return res;
}

Std_ReturnType TcpIp_SoAdGetSocket(
        TcpIp_DomainType    domain,
        TcpIp_ProtocolType  protocol,
        TcpIp_SocketIdType* socketid
    )
{
    Std_ReturnType     res;

    res = TcpIp_GetFreeSocket(socketid);
    if (res == E_OK) {
        TcpIp_SocketType*  s = &TcpIp_Sockets[*socketid];
        TcpIp_OsSocketType fd;
        fd = socket( TcpIp_GetBsdDomainFromDomain(domain)
                   , TcpIp_GetBsdTypeFromProtocol(protocol)
                   , 0);

        if (fd != INVALID_SOCKET) {
            s->fd       = fd;
            s->state    = TCPIP_SOCKET_STATE_ALLOCATED;
            s->protocol = protocol;
            s->domain   = domain;
        } else {
            res = E_NOT_OK;
        }
    }

    return res;
}

void TcpIp_SocketState_Connecting(TcpIp_SocketIdType index)
{
    TcpIp_SocketType* s = &TcpIp_Sockets[index];
    struct pollfd*    p = &TcpIp_PollFds[index];
    int v;

    if (p->revents & POLLOUT) {
        struct sockaddr_storage addr;
        socklen_t len = sizeof(addr);

        /* check if connect succeeded */
        v = getpeername(s->fd, (struct sockaddr*)&addr, &len);
        if (v == 0) {
            TcpIp_SocketState_Enter(index, TCPIP_SOCKET_STATE_CONNECTED);
            SoAd_TcpConnected(index);
        } else {
            TcpIp_SocketState_Enter(index, TCPIP_SOCKET_STATE_ALLOCATED);
        }
    }
}

void TcpIp_SocketState_Listen_Accept(TcpIp_SocketIdType index)
{
    TcpIp_SocketType*  s   = &TcpIp_Sockets[index];
    TcpIp_SocketIdType id2 = TCPIP_SOCKETID_INVALID;
    int fd                 = INVALID_SOCKET;

    socklen_t len;
    struct sockaddr_storage addr;
    len = sizeof(addr);

    TcpIp_SockAddrStorageType data;

    fd = accept(s->fd, (struct sockaddr*)&addr, &len);
    if (fd == INVALID_SOCKET) {
        goto cleanup;
    }

    if (TcpIp_SoAdGetSocket(s->domain, s->protocol, &id2) != E_OK) {
        goto cleanup;
    }

    if (TcpIp_GetSockaddrFromBsdSocketAddr(&data, (struct sockaddr*)&addr) != E_OK) {
        goto cleanup;
    }

    if (SoAd_TcpAccepted(index, id2, &data.base) != E_OK) {
        goto cleanup;
    }

    TcpIp_SocketState_Enter(id2, TCPIP_SOCKET_STATE_CONNECTED);
    goto done;

cleanup:
    if (id2 != TCPIP_SOCKETID_INVALID) {
        TcpIp_SocketState_Enter(id2, TCPIP_SOCKET_STATE_UNUSED);
    }

    if (fd != INVALID_SOCKET) {
        closesocket(fd);
    }
done:
    return;
}

void TcpIp_SocketState_Listen(TcpIp_SocketIdType index)
{
    TcpIp_SocketType* s = &TcpIp_Sockets[index];
    struct pollfd*    p = &TcpIp_PollFds[index];

    if (p->revents & POLLIN) {
        TcpIp_SocketState_Listen_Accept(index);
    }
}

void TcpIp_SocketState_TcpClose(TcpIp_SocketIdType index)
{
    TcpIp_SocketType* s = &TcpIp_Sockets[index];
    struct pollfd*    p = &TcpIp_PollFds[index];

    /* TODO - how to handle proper FIN */
    TcpIp_SocketState_Enter(index, TCPIP_SOCKET_STATE_UNUSED);
    SoAd_TcpIpEvent(index, TCPIP_TCP_CLOSED);
}

void TcpIp_SocketState_Connected(TcpIp_SocketIdType index)
{
    TcpIp_SocketType* s = &TcpIp_Sockets[index];
    struct pollfd*    p = &TcpIp_PollFds[index];

    if (p->revents & POLLIN) {
    }

    if (p->revents & POLLOUT) {
    }
}

static void TcpIp_SocketState_Enter(TcpIp_SocketIdType index, TcpIp_SocketStateType state)
{
    TcpIp_SocketType* s = &TcpIp_Sockets[index];
    struct pollfd*    p = &TcpIp_PollFds[index];

    s->state = state;

    /* what events are we listening on */
    switch (s->state) {
        case TCPIP_SOCKET_STATE_CONNECTING:
            p->events = POLLOUT;
            break;

        case TCPIP_SOCKET_STATE_LISTEN:
        case TCPIP_SOCKET_STATE_TCPCLOSE:
            p->events = POLLIN;
            break;

        case TCPIP_SOCKET_STATE_UNUSED:
            if (s->fd != INVALID_SOCKET) {
                closesocket(s->fd);
                s->fd = INVALID_SOCKET;
            }
            p->events = 0;
            break;
        default:
            p->events = 0;
            break;
    }
}

static void TcpIp_SocketState_All(TcpIp_SocketIdType index)
{
    TcpIp_SocketType* s = &TcpIp_Sockets[index];
    int res;

    /* handle current state */
    switch (s->state) {
        case TCPIP_SOCKET_STATE_CONNECTING:
            TcpIp_SocketState_Connecting(index);
            break;
        case TCPIP_SOCKET_STATE_CONNECTED:
            TcpIp_SocketState_Connected(index);
            break;
        case TCPIP_SOCKET_STATE_LISTEN:
            TcpIp_SocketState_Listen(index);
            break;
        case TCPIP_SOCKET_STATE_TCPCLOSE:
            TcpIp_SocketState_TcpClose(index);
            break;
        default:
            break;
    }

}

void TcpIp_MainFunction(void)
{
    TcpIp_SocketIdType index;
    int                res;

    for (index = 0u; index < TCPIP_CFG_MAX_SOCKETS; ++index) {
        TcpIp_PollFds[index].fd      = TcpIp_Sockets[index].fd;
        TcpIp_PollFds[index].revents = 0;
    }

    res = poll(TcpIp_PollFds, TCPIP_CFG_MAX_SOCKETS, 0);
    if (res > 0) {
        /* something to do */
    } else if (res < 0) {
        /* error occured */
    } else {
        /* nothing to do */
    }
    for (index = 0u; index < TCPIP_CFG_MAX_SOCKETS; ++index) {
        TcpIp_SocketState_All(index);
    }
}

