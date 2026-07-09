#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <php.h>
#include <zend_API.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

/* 平台相关网络头文件 */
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <iphlpapi.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "iphlpapi.lib")
    #define close_socket(s) closesocket(s)
    #define socket_errno WSAGetLastError()
    #define EINPROGRESS WSAEINPROGRESS
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <net/if.h>
    #include <ifaddrs.h>
    #include <sys/epoll.h>
    #include <sys/select.h>
    #define close_socket(s) close(s)
    #define socket_errno errno
#endif

#define MAX_NICS 64
#define SAFE_STRNCPY(dst, src, size) do { \
    strncpy((dst), (src), (size) - 1); \
    (dst)[(size) - 1] = '\0'; \
} while(0)

typedef struct {
    int sock;
    char ip[INET_ADDRSTRLEN];
    char name[IFNAMSIZ];
    int success;
} nic_info_t;

/* ---------- 辅助函数 ---------- */
#ifdef _WIN32
static int winsock_initialized = 0;
void nic_tester_init_winsock(void) {
    if (!winsock_initialized) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        winsock_initialized = 1;
    }
}
#endif

static int set_nonblocking(int sock) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

/* ---------- 获取网卡列表 ---------- */
static int get_nic_list(nic_info_t *nics, int max_count) {
    int count = 0;
#ifdef _WIN32
    ULONG size = 0;
    IP_ADAPTER_ADDRESSES *adapter = NULL;
    IP_ADAPTER_ADDRESSES *p = NULL;
    IP_ADAPTER_UNICAST_ADDRESS *unicast = NULL;
    struct sockaddr_in *addr = NULL;
    char ip[INET_ADDRSTRLEN];

    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, NULL, NULL, &size);
    adapter = (IP_ADAPTER_ADDRESSES*)malloc(size);
    if (!adapter) return 0;

    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, NULL, adapter, &size) == NO_ERROR) {
        for (p = adapter; p && count < max_count; p = p->Next) {
            if (p->OperStatus != IfOperStatusUp) continue;
            if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

            for (unicast = p->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
                addr = (struct sockaddr_in*)unicast->Address.lpSockaddr;
                inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
                if (strcmp(ip, "127.0.0.1") == 0) continue;

                SAFE_STRNCPY(nics[count].ip, ip, INET_ADDRSTRLEN);
                SAFE_STRNCPY(nics[count].name, p->AdapterName, IFNAMSIZ);
                nics[count].sock = -1;
                nics[count].success = 0;
                count++;
                break;
            }
        }
    }
    free(adapter);

#else /* Linux */
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa = NULL;
    char ip[INET_ADDRSTRLEN];

    if (getifaddrs(&ifaddr) == -1) return 0;

    for (ifa = ifaddr; ifa && count < max_count; ifa = ifa->ifa_next) {
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) continue;

        inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr, ip, sizeof(ip));
        if (strcmp(ip, "127.0.0.1") == 0) continue;

        SAFE_STRNCPY(nics[count].ip, ip, INET_ADDRSTRLEN);
        SAFE_STRNCPY(nics[count].name, ifa->ifa_name, IFNAMSIZ);
        nics[count].sock = -1;
        nics[count].success = 0;
        count++;
    }
    freeifaddrs(ifaddr);
#endif

    return count;
}

/* ---------- 核心检测函数 ---------- */
static int test_nics_async(nic_info_t *nics, int nic_count, const char *target_ip, int port, double timeout) {
    struct sockaddr_in dest_addr;
    int i, ret, successful = 0;
    int max_fd = 0;
    int epfd = -1;
#ifdef _WIN32
    fd_set write_fds, write_fds_copy;
    FD_ZERO(&write_fds);
#else
    struct epoll_event ev, events[MAX_NICS];
#endif

    if (nic_count == 0) return 0;

    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, target_ip, &dest_addr.sin_addr) != 1) {
        return 0;
    }

#ifndef _WIN32
    epfd = epoll_create1(0);
    if (epfd < 0) {
        return 0;
    }
#endif

    for (i = 0; i < nic_count; i++) {
        int sock;
        struct sockaddr_in src_addr;

        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            nics[i].sock = -1;
            continue;
        }

        src_addr.sin_family = AF_INET;
        src_addr.sin_addr.s_addr = inet_addr(nics[i].ip);
        src_addr.sin_port = 0;
        if (bind(sock, (struct sockaddr*)&src_addr, sizeof(src_addr)) < 0) {
            close_socket(sock);
            nics[i].sock = -1;
            continue;
        }

        if (set_nonblocking(sock) < 0) {
            close_socket(sock);
            nics[i].sock = -1;
            continue;
        }

        ret = connect(sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
        if (ret < 0) {
#ifdef _WIN32
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                close_socket(sock);
                nics[i].sock = -1;
                continue;
            }
#else
            if (errno != EINPROGRESS) {
                close_socket(sock);
                nics[i].sock = -1;
                continue;
            }
#endif
        }

        nics[i].sock = sock;
        nics[i].success = 0;

#ifdef _WIN32
        FD_SET(sock, &write_fds);
        if (sock > max_fd) max_fd = sock;
#else
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLOUT;
        ev.data.fd = sock;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev) < 0) {
            close_socket(sock);
            nics[i].sock = -1;
            continue;
        }
#endif
    }

    {
        struct timeval tv;
        tv.tv_sec = (int)timeout;
        tv.tv_usec = (int)((timeout - tv.tv_sec) * 1e6);

#ifdef _WIN32
        int nfds;
        write_fds_copy = write_fds;
        nfds = select(max_fd + 1, NULL, &write_fds_copy, NULL, &tv);
        if (nfds > 0) {
            for (i = 0; i < nic_count; i++) {
                int sock = nics[i].sock;
                if (sock >= 0 && FD_ISSET(sock, &write_fds_copy)) {
                    int error = 0;
                    socklen_t len = sizeof(error);
                    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&error, &len) == 0 && error == 0) {
                        nics[i].success = 1;
                        successful++;
                    }
                }
            }
        }
#else
        int timeout_ms = (int)(timeout * 1000);
        int nfds = epoll_wait(epfd, events, nic_count, timeout_ms);
        if (nfds > 0) {
            int j;
            for (i = 0; i < nfds; i++) {
                int sock = events[i].data.fd;
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
                    for (j = 0; j < nic_count; j++) {
                        if (nics[j].sock == sock) {
                            nics[j].success = 1;
                            successful++;
                            break;
                        }
                    }
                }
            }
        }
#endif
    }

    for (i = 0; i < nic_count; i++) {
        if (nics[i].sock >= 0) {
            close_socket(nics[i].sock);
            nics[i].sock = -1;
        }
    }
#ifndef _WIN32
    if (epfd >= 0) {
        close(epfd);
    }
#endif

    return successful;
}

/* ---------- PHP 导出函数 ---------- */
PHP_FUNCTION(nic_tester_check)
{
    char *target_ip;
    int target_ip_len;
    long port = 80;
    double timeout = 3.0;
    int nic_count, i;
    nic_info_t nics[MAX_NICS];

    /* 关键修正：添加 TSRMLS_CC */
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|ld",
                              &target_ip, &target_ip_len,
                              &port, &timeout) == FAILURE) {
        RETURN_FALSE;
    }

#ifdef _WIN32
    nic_tester_init_winsock();
#endif

    nic_count = get_nic_list(nics, MAX_NICS);
    if (nic_count == 0) {
        array_init(return_value);
        return;
    }

    test_nics_async(nics, nic_count, target_ip, (int)port, timeout);

    array_init(return_value);
    for (i = 0; i < nic_count; i++) {
        if (nics[i].success) {
            zval *result;
	    MAKE_STD_ZVAL(result);
            array_init(result);
            add_assoc_string(result, "name", nics[i].name, 1);
            add_assoc_string(result, "ip", nics[i].ip, 1);
            add_next_index_zval(return_value, result);
        }
    }
}

/* ---------- 扩展模块定义 ---------- */
static const zend_function_entry nic_tester_functions[] = {
    PHP_FE(nic_tester_check, NULL)
    PHP_FE_END
};

zend_module_entry nic_tester_module_entry = {
    STANDARD_MODULE_HEADER,
    "nic_tester",
    nic_tester_functions,
    NULL, NULL, NULL, NULL, NULL,
    "1.0.0",
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_NIC_TESTER
    ZEND_GET_MODULE(nic_tester)
#endif
