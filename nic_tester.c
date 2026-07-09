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
#define MAX_IPS_PER_NIC 8
#define SAFE_STRNCPY(dst, src, size) do { \
    strncpy((dst), (src), (size) - 1); \
    (dst)[(size) - 1] = '\0'; \
} while(0)

/* ---------- Windows IPv6 地址宏 (Linux 由 netinet/in.h 提供) ---------- */
#ifdef _WIN32
static int in6_is_addr_v4mapped(const struct in6_addr *a) {
    int i;
    for (i = 0; i < 10; i++) if (a->s6_addr[i] != 0) return 0;
    return (a->s6_addr[10] == 0xFF && a->s6_addr[11] == 0xFF);
}
#define IN6_IS_ADDR_V4MAPPED(a)  in6_is_addr_v4mapped(a)

static int in6_is_addr_multicast(const struct in6_addr *a) {
    return (a->s6_addr[0] == 0xFF);
}
#define IN6_IS_ADDR_MULTICAST(a)  in6_is_addr_multicast(a)

static int in6_is_addr_loopback(const struct in6_addr *a) {
    int i;
    for (i = 0; i < 15; i++) if (a->s6_addr[i] != 0) return 0;
    return (a->s6_addr[15] == 1);
}
#define IN6_IS_ADDR_LOOPBACK(a)   in6_is_addr_loopback(a)

static int in6_is_addr_unspecified(const struct in6_addr *a) {
    int i;
    for (i = 0; i < 16; i++) if (a->s6_addr[i] != 0) return 0;
    return 1;
}
#define IN6_IS_ADDR_UNSPECIFIED(a) in6_is_addr_unspecified(a)

static int in6_is_addr_linklocal(const struct in6_addr *a) {
    return (a->s6_addr[0] == 0xFE && (a->s6_addr[1] & 0xC0) == 0x80);
}
#define IN6_IS_ADDR_LINKLOCAL(a)   in6_is_addr_linklocal(a)
#endif

/* ---------- 网卡数据结构 ---------- */
typedef struct {
    char name[IFNAMSIZ];
    char ipv4[MAX_IPS_PER_NIC][INET6_ADDRSTRLEN];
    int  ipv4_count;
    char ipv6[MAX_IPS_PER_NIC][INET6_ADDRSTRLEN];
    int  ipv6_scope_ids[MAX_IPS_PER_NIC];
    int  ipv6_count;
    int  sock;
    int  success;
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

/* ---------- 目标地址校验 ---------- */
static int validate_target(const char *target_ip, int *family) {
    struct in6_addr in6;
    struct in_addr  in4;
    unsigned char *addr_bytes;
    int i;

    /* 尝试 IPv6 */
    if (inet_pton(AF_INET6, target_ip, &in6) == 1) {
        /* IPv4-mapped 地址: 提取嵌入的 IPv4 地址并对其应用 IPv4 验证规则 */
        if (IN6_IS_ADDR_V4MAPPED(&in6)) {
            /* 从 s6_addr[12..15] 提取 IPv4 字节 */
            addr_bytes = in6.s6_addr + 12;
            /* 检查 unspecified: 0.0.0.0 */
            if (addr_bytes[0] == 0 && addr_bytes[1] == 0 &&
                addr_bytes[2] == 0 && addr_bytes[3] == 0) {
                return 0;
            }
            /* 检查 loopback: 127.0.0.0/8 */
            if (addr_bytes[0] == 127) {
                return 0;
            }
            /* 检查 multicast: 224.0.0.0/4 */
            if ((addr_bytes[0] & 0xF0) == 0xE0) {
                return 0;
            }
            /* 检查 broadcast: 255.255.255.255 */
            if (addr_bytes[0] == 0xFF && addr_bytes[1] == 0xFF &&
                addr_bytes[2] == 0xFF && addr_bytes[3] == 0xFF) {
                return 0;
            }
            *family = AF_INET6;
            return 1;
        }
        /* 原生 IPv6: 检查非 unicast */
        if (IN6_IS_ADDR_MULTICAST(&in6))   return 0;
        if (IN6_IS_ADDR_LOOPBACK(&in6))    return 0;
        if (IN6_IS_ADDR_UNSPECIFIED(&in6)) return 0;
        *family = AF_INET6;
        return 1;
    }

    /* 尝试 IPv4 */
    if (inet_pton(AF_INET, target_ip, &in4) == 1) {
        /* 不使用 inet_ntop 中转，直接基于字节做语义检查 */
        addr_bytes = (unsigned char *)&in4.s_addr;
        /* 检查 unspecified: 0.0.0.0 */
        if (addr_bytes[0] == 0 && addr_bytes[1] == 0 &&
            addr_bytes[2] == 0 && addr_bytes[3] == 0) {
            return 0;
        }
        /* 检查 loopback: 127.0.0.0/8 */
        if (addr_bytes[0] == 127) {
            return 0;
        }
        /* 检查 multicast: 224.0.0.0/4 */
        if ((addr_bytes[0] & 0xF0) == 0xE0) {
            return 0;
        }
        /* 检查 broadcast: 255.255.255.255 */
        if (addr_bytes[0] == 0xFF && addr_bytes[1] == 0xFF &&
            addr_bytes[2] == 0xFF && addr_bytes[3] == 0xFF) {
            return 0;
        }
        *family = AF_INET;
        return 1;
    }

    return 0;
}

/* ---------- 获取网卡列表 (双协议栈) ---------- */
static int find_or_create_nic(nic_info_t *nics, int *count, const char *name) {
    int i;
    for (i = 0; i < *count; i++) {
        if (strcmp(nics[i].name, name) == 0) return i;
    }
    if (*count >= MAX_NICS) return -1;
    i = *count;
    SAFE_STRNCPY(nics[i].name, name, IFNAMSIZ);
    nics[i].ipv4_count = 0;
    nics[i].ipv6_count = 0;
    nics[i].sock = -1;
    nics[i].success = 0;
    (*count)++;
    return i;
}

static int get_nic_list(nic_info_t *nics, int max_count) {
    int count = 0;
#ifdef _WIN32
    ULONG size = 0;
    IP_ADAPTER_ADDRESSES *adapter = NULL;
    IP_ADAPTER_ADDRESSES *p = NULL;
    IP_ADAPTER_UNICAST_ADDRESS *unicast = NULL;
    struct sockaddr_in  *addr4 = NULL;
    struct sockaddr_in6 *addr6 = NULL;
    char ip[INET6_ADDRSTRLEN];
    int idx;

    GetAdaptersAddresses(AF_UNSPEC,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
        NULL, NULL, &size);
    adapter = (IP_ADAPTER_ADDRESSES *)malloc(size);
    if (!adapter) return 0;

    if (GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
            NULL, adapter, &size) == NO_ERROR) {
        for (p = adapter; p && count < max_count; p = p->Next) {
            if (p->OperStatus != IfOperStatusUp) continue;
            if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

            for (unicast = p->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                if (unicast->Address.lpSockaddr->sa_family == AF_INET) {
                    addr4 = (struct sockaddr_in *)unicast->Address.lpSockaddr;
                    inet_ntop(AF_INET, &addr4->sin_addr, ip, sizeof(ip));
                    /* 过滤 IPv4 非 unicast */
                    {
                        unsigned char *b = (unsigned char *)&addr4->sin_addr.s_addr;
                        if (b[0] == 0   && b[1] == 0   && b[2] == 0   && b[3] == 0)   continue; /* 0.0.0.0 */
                        if (b[0] == 127)                                                     continue; /* 127.x */
                        if ((b[0] & 0xF0) == 0xE0)                                          continue; /* 224.x/4 */
                        if (b[0] == 0xFF && b[1] == 0xFF && b[2] == 0xFF && b[3] == 0xFF) continue; /* 255.255.255.255 */
                    }
                    idx = find_or_create_nic(nics, &count, p->AdapterName);
                } else if (unicast->Address.lpSockaddr->sa_family == AF_INET6) {
                    addr6 = (struct sockaddr_in6 *)unicast->Address.lpSockaddr;
                    inet_ntop(AF_INET6, &addr6->sin6_addr, ip, sizeof(ip));
                    /* 过滤 IPv6 非 unicast */
#ifdef _WIN32
                    if (in6_is_addr_multicast(&addr6->sin6_addr))  continue;
                    if (in6_is_addr_loopback(&addr6->sin6_addr))   continue;
                    if (in6_is_addr_unspecified(&addr6->sin6_addr)) continue;
#else
                    if (IN6_IS_ADDR_MULTICAST(&addr6->sin6_addr))  continue;
                    if (IN6_IS_ADDR_LOOPBACK(&addr6->sin6_addr))   continue;
                    if (IN6_IS_ADDR_UNSPECIFIED(&addr6->sin6_addr)) continue;
#endif
                    idx = find_or_create_nic(nics, &count, p->AdapterName);
                } else {
                    continue;
                }

                if (idx < 0) break; /* MAX_NICS reached */

                if (unicast->Address.lpSockaddr->sa_family == AF_INET) {
                    if (nics[idx].ipv4_count < MAX_IPS_PER_NIC) {
                        SAFE_STRNCPY(nics[idx].ipv4[nics[idx].ipv4_count], ip, INET6_ADDRSTRLEN);
                        nics[idx].ipv4_count++;
                    }
                } else {
                    if (nics[idx].ipv6_count < MAX_IPS_PER_NIC) {
                        SAFE_STRNCPY(nics[idx].ipv6[nics[idx].ipv6_count], ip, INET6_ADDRSTRLEN);
                        nics[idx].ipv6_scope_ids[nics[idx].ipv6_count] = (int)addr6->sin6_scope_id;
                        nics[idx].ipv6_count++;
                    }
                }
            }
        }
    }
    free(adapter);

#else /* Linux */
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa = NULL;
    struct sockaddr_in  *addr4 = NULL;
    struct sockaddr_in6 *addr6 = NULL;
    char ip[INET6_ADDRSTRLEN];
    int idx;

    if (getifaddrs(&ifaddr) == -1) return 0;

    for (ifa = ifaddr; ifa && count < max_count; ifa = ifa->ifa_next) {
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (ifa->ifa_addr == NULL) continue;

        if (ifa->ifa_addr->sa_family == AF_INET) {
            addr4 = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &addr4->sin_addr, ip, sizeof(ip));
            /* 过滤 IPv4 非 unicast */
            {
                unsigned char *b = (unsigned char *)&addr4->sin_addr.s_addr;
                if (b[0] == 0   && b[1] == 0   && b[2] == 0   && b[3] == 0)   continue;
                if (b[0] == 127)                                                     continue;
                if ((b[0] & 0xF0) == 0xE0)                                          continue;
                if (b[0] == 0xFF && b[1] == 0xFF && b[2] == 0xFF && b[3] == 0xFF) continue;
            }
            idx = find_or_create_nic(nics, &count, ifa->ifa_name);
        } else if (ifa->ifa_addr->sa_family == AF_INET6) {
            addr6 = (struct sockaddr_in6 *)ifa->ifa_addr;
            inet_ntop(AF_INET6, &addr6->sin6_addr, ip, sizeof(ip));
            /* 过滤 IPv6 非 unicast */
            if (IN6_IS_ADDR_MULTICAST(&addr6->sin6_addr))  continue;
            if (IN6_IS_ADDR_LOOPBACK(&addr6->sin6_addr))   continue;
            if (IN6_IS_ADDR_UNSPECIFIED(&addr6->sin6_addr)) continue;
            idx = find_or_create_nic(nics, &count, ifa->ifa_name);
        } else {
            continue;
        }

        if (idx < 0) break;

        if (ifa->ifa_addr->sa_family == AF_INET) {
            if (nics[idx].ipv4_count < MAX_IPS_PER_NIC) {
                SAFE_STRNCPY(nics[idx].ipv4[nics[idx].ipv4_count], ip, INET6_ADDRSTRLEN);
                nics[idx].ipv4_count++;
            }
        } else {
            if (nics[idx].ipv6_count < MAX_IPS_PER_NIC) {
                SAFE_STRNCPY(nics[idx].ipv6[nics[idx].ipv6_count], ip, INET6_ADDRSTRLEN);
                nics[idx].ipv6_scope_ids[nics[idx].ipv6_count] = (int)addr6->sin6_scope_id;
                nics[idx].ipv6_count++;
            }
        }
    }
    freeifaddrs(ifaddr);
#endif

    return count;
}

/* ---------- 辅助: 构建 IPv4-mapped sockaddr_in6 ---------- */
static void make_v4mapped_addr(struct sockaddr_in6 *addr, const char *ipv4_str, int port) {
    struct in_addr in4;
    memset(addr, 0, sizeof(*addr));
    addr->sin6_family = AF_INET6;
    addr->sin6_port   = htons(port);
    /* IPv4-mapped 前缀: bytes 10-11 为 0xFF, bytes 12-15 为 IPv4 地址 */
    addr->sin6_addr.s6_addr[10] = 0xFF;
    addr->sin6_addr.s6_addr[11] = 0xFF;
    if (inet_pton(AF_INET, ipv4_str, &in4) == 1) {
        memcpy(&addr->sin6_addr.s6_addr[12], &in4.s_addr, 4);
    }
}

/* ---------- 核心检测函数 (双栈) ---------- */
static int test_nics_async(nic_info_t *nics, int nic_count,
                           const char *target_ip, int target_family,
                           int port, double timeout) {
    struct sockaddr_in6 dest_addr6;
    struct sockaddr_in  dest_addr4; /* AF_INET 回退路径 */
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

    /* 构造目标地址 */
    memset(&dest_addr6, 0, sizeof(dest_addr6));
    if (target_family == AF_INET6) {
        dest_addr6.sin6_family = AF_INET6;
        dest_addr6.sin6_port   = htons(port);
        if (inet_pton(AF_INET6, target_ip, &dest_addr6.sin6_addr) != 1) {
            return 0;
        }
    } else {
        make_v4mapped_addr(&dest_addr6, target_ip, port);
        /* 同时准备 AF_INET 回退地址 */
        memset(&dest_addr4, 0, sizeof(dest_addr4));
        dest_addr4.sin_family = AF_INET;
        dest_addr4.sin_port   = htons(port);
        if (inet_pton(AF_INET, target_ip, &dest_addr4.sin_addr) != 1) {
            return 0;
        }
    }

#ifndef _WIN32
    epfd = epoll_create1(0);
    if (epfd < 0) {
        return 0;
    }
#endif

    for (i = 0; i < nic_count; i++) {
        int sock = -1;
        int bound = 0;
        int af_family = AF_INET6;  /* 默认优先 AF_INET6 */
        int j;

        /* ---------- 创建 socket: 优先 AF_INET6 双栈, 按 NIC 回退到 AF_INET ---------- */
        sock = socket(AF_INET6, SOCK_STREAM, 0);
        if (sock >= 0) {
            /* 显式设置双栈模式 */
            int v6only = 0;
            if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
#ifdef _WIN32
                           (const char *)&v6only, sizeof(v6only)) < 0) {
#else
                           &v6only, sizeof(v6only)) < 0) {
#endif
                /* setsockopt 失败: 按 plan 跳过此 NIC */
                close_socket(sock);
                nics[i].sock = -1;
                continue;
            }
        } else {
            /* socket(AF_INET6) 失败 (EAFNOSUPPORT 等) */
            if (target_family == AF_INET) {
                /* IPv4 目标: 回退到 AF_INET */
                sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock < 0) {
                    nics[i].sock = -1;
                    continue;
                }
                af_family = AF_INET;
            } else {
                /* IPv6 目标: 无法回退 */
                nics[i].sock = -1;
                continue;
            }
        }

        /* ---------- 绑定 NIC 地址 ---------- */
        if (af_family == AF_INET) {
            /* AF_INET 回退路径: 绑定 NIC 的第一个 IPv4 地址 */
            if (nics[i].ipv4_count > 0) {
                struct sockaddr_in src_addr;
                memset(&src_addr, 0, sizeof(src_addr));
                src_addr.sin_family = AF_INET;
                src_addr.sin_port   = 0;
                if (inet_pton(AF_INET, nics[i].ipv4[0], &src_addr.sin_addr) == 1) {
                    if (bind(sock, (struct sockaddr *)&src_addr, sizeof(src_addr)) == 0) {
                        bound = 1;
                    }
                }
            }
        } else {
            /* AF_INET6: 绑定 NIC 地址 (优先匹配目标协议族) */
            if (target_family == AF_INET) {
                /* IPv4 目标: 优先绑定 IPv4 地址 (映射) */
                for (j = 0; j < nics[i].ipv4_count && !bound; j++) {
                    struct sockaddr_in6 src_addr;
                    make_v4mapped_addr(&src_addr, nics[i].ipv4[j], 0);
                    if (bind(sock, (struct sockaddr *)&src_addr, sizeof(src_addr)) == 0) {
                        bound = 1;
                    }
                }
                /* 回退: 绑定 IPv6 地址 */
                for (j = 0; j < nics[i].ipv6_count && !bound; j++) {
                    struct sockaddr_in6 src_addr;
                    memset(&src_addr, 0, sizeof(src_addr));
                    src_addr.sin6_family = AF_INET6;
                    src_addr.sin6_port   = 0;
                    if (inet_pton(AF_INET6, nics[i].ipv6[j], &src_addr.sin6_addr) == 1) {
#ifdef _WIN32
                        if (in6_is_addr_linklocal(&src_addr.sin6_addr))
#else
                        if (IN6_IS_ADDR_LINKLOCAL(&src_addr.sin6_addr))
#endif
                        {
                            src_addr.sin6_scope_id = nics[i].ipv6_scope_ids[j];
                        }
                        if (bind(sock, (struct sockaddr *)&src_addr, sizeof(src_addr)) == 0) {
                            bound = 1;
                        }
                    }
                }
            } else {
                /* IPv6 目标: 优先绑定 IPv6 地址 (原生) */
                for (j = 0; j < nics[i].ipv6_count && !bound; j++) {
                    struct sockaddr_in6 src_addr;
                    memset(&src_addr, 0, sizeof(src_addr));
                    src_addr.sin6_family = AF_INET6;
                    src_addr.sin6_port   = 0;
                    if (inet_pton(AF_INET6, nics[i].ipv6[j], &src_addr.sin6_addr) == 1) {
#ifdef _WIN32
                        if (in6_is_addr_linklocal(&src_addr.sin6_addr))
#else
                        if (IN6_IS_ADDR_LINKLOCAL(&src_addr.sin6_addr))
#endif
                        {
                            src_addr.sin6_scope_id = nics[i].ipv6_scope_ids[j];
                        }
                        if (bind(sock, (struct sockaddr *)&src_addr, sizeof(src_addr)) == 0) {
                            bound = 1;
                        }
                    }
                }
                /* 回退: 绑定 IPv4 地址 (映射), 内核决定可达性 */
                for (j = 0; j < nics[i].ipv4_count && !bound; j++) {
                    struct sockaddr_in6 src_addr;
                    make_v4mapped_addr(&src_addr, nics[i].ipv4[j], 0);
                    if (bind(sock, (struct sockaddr *)&src_addr, sizeof(src_addr)) == 0) {
                        bound = 1;
                    }
                }
            }

            if (!bound) {
                close_socket(sock);
                nics[i].sock = -1;
                continue;
            }
        }

        /* ---------- 非阻塞 + connect ---------- */
        if (set_nonblocking(sock) < 0) {
            close_socket(sock);
            nics[i].sock = -1;
            continue;
        }

        if (af_family == AF_INET6) {
            ret = connect(sock, (struct sockaddr *)&dest_addr6, sizeof(dest_addr6));
        } else {
            ret = connect(sock, (struct sockaddr *)&dest_addr4, sizeof(dest_addr4));
        }

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

        nics[i].sock    = sock;
        nics[i].success = 0;

#ifdef _WIN32
        FD_SET(sock, &write_fds);
        if (sock > max_fd) max_fd = sock;
#else
        ev.events   = EPOLLOUT;
        ev.data.fd  = sock;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev) < 0) {
            close_socket(sock);
            nics[i].sock = -1;
            continue;
        }
#endif
    }

    /* ---------- I/O 多路复用等待 ---------- */
    {
        struct timeval tv;
        tv.tv_sec  = (int)timeout;
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
                    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&error, &len) == 0 && error == 0) {
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
            int k;
            for (i = 0; i < nfds; i++) {
                int sock = events[i].data.fd;
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
                    for (k = 0; k < nic_count; k++) {
                        if (nics[k].sock == sock) {
                            nics[k].success = 1;
                            successful++;
                            break;
                        }
                    }
                }
            }
        }
#endif
    }

    /* ---------- 清理 ---------- */
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
    int target_family, nic_count, i, j;
    nic_info_t nics[MAX_NICS];

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|ld",
                              &target_ip, &target_ip_len,
                              &port, &timeout) == FAILURE) {
        RETURN_FALSE;
    }

    /* 校验目标地址 */
    if (!validate_target(target_ip, &target_family)) {
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

    test_nics_async(nics, nic_count, target_ip, target_family, (int)port, timeout);

    /* 构建返回值: [{name, ipv4: [...], ipv6: [...]}, ...] */
    array_init(return_value);
    for (i = 0; i < nic_count; i++) {
        if (nics[i].success) {
            zval *result, *ipv4_arr, *ipv6_arr;

            MAKE_STD_ZVAL(result);
            array_init(result);

            MAKE_STD_ZVAL(ipv4_arr);
            array_init(ipv4_arr);

            MAKE_STD_ZVAL(ipv6_arr);
            array_init(ipv6_arr);

            add_assoc_string(result, "name", nics[i].name, 1);

            for (j = 0; j < nics[i].ipv4_count; j++) {
                add_next_index_string(ipv4_arr, nics[i].ipv4[j], 1);
            }
            add_assoc_zval(result, "ipv4", ipv4_arr);

            for (j = 0; j < nics[i].ipv6_count; j++) {
                add_next_index_string(ipv6_arr, nics[i].ipv6[j], 1);
            }
            add_assoc_zval(result, "ipv6", ipv6_arr);

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
    "2.0.0",
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_NIC_TESTER
    ZEND_GET_MODULE(nic_tester)
#endif
