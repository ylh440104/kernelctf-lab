#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>

static const socklen_t lens[] = {0,1,2,3,4,5,7,8,15,16,17,31,32,33,63,64,65,127,128,129,255,256,257,511,512};
static const int domains[] = {AF_INET,AF_INET,AF_INET6,AF_INET6};
static const int types[] = {SOCK_STREAM,SOCK_DGRAM,SOCK_STREAM,SOCK_DGRAM};
static const int protos[] = {IPPROTO_TCP,IPPROTO_UDP,IPPROTO_TCP,IPPROTO_UDP};
static const int levels[] = {SOL_SOCKET,IPPROTO_IP,IPPROTO_TCP,IPPROTO_IPV6};

static unsigned char data[1024];

static int sock_for(int i)
{
    int fd = socket(domains[i], types[i], protos[i]);
    if (fd < 0 && domains[i] == AF_INET6)
        return -1;
    return fd;
}

int main(void)
{
    printf("SETSOCKOPT_ENUM uid=%d euid=%d\n", getuid(), geteuid());
    unsigned long total = 0;
    unsigned long ok = 0;
    unsigned long einval = 0;
    unsigned long eperm = 0;
    unsigned long other = 0;
    memset(data, 0xa5, sizeof(data));
    for (int si = 0; si < 4; si++) {
        for (int li = 0; li < 4; li++) {
            for (int opt = 0; opt < 256; opt++) {
                int fd = sock_for(si);
                if (fd < 0)
                    continue;
                for (unsigned int bi = 0; bi < sizeof(lens) / sizeof(lens[0]); bi++) {
                    int rc = setsockopt(fd, levels[li], opt, data, lens[bi]);
                    int e = errno;
                    total++;
                    if (rc == 0) {
                        ok++;
                        if (opt < 96)
                            printf("OK domain=%d type=%d level=%d opt=%d len=%u\n", domains[si], types[si], levels[li], opt, lens[bi]);
                    } else if (e == EINVAL || e == ENOPROTOOPT) {
                        einval++;
                    } else if (e == EPERM || e == EACCES) {
                        eperm++;
                    } else {
                        other++;
                        printf("ERR domain=%d type=%d level=%d opt=%d len=%u errno=%d\n", domains[si], types[si], levels[li], opt, lens[bi], e);
                    }
                }
                close(fd);
            }
        }
    }
    printf("COUNTS total=%lu ok=%lu inval=%lu perm=%lu other=%lu\n", total, ok, einval, eperm, other);
    const char *names[] = {"tls","nvme","mptcp","not-a-real-ulp"};
    for (int si = 0; si < 2; si++) {
        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) continue;
        for (unsigned int ni = 0; ni < sizeof(names) / sizeof(names[0]); ni++) {
            for (socklen_t n = 0; n <= strlen(names[ni]) + 2; n++) {
                int rc = setsockopt(fd, IPPROTO_TCP, TCP_ULP, names[ni], n);
                printf("ULP name=%s len=%u rc=%d errno=%d\n", names[ni], n, rc, rc < 0 ? errno : 0);
            }
        }
        close(fd);
        break;
    }
    printf("SETSOCKOPT_ENUM_DONE\n");
    return 0;
}