#define _GNU_SOURCE
#include "fwopen.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define IPT "/usr/local/bin/iptables"
static const struct { const char *name, *verb; } CHAINS[] = {
    {"FIREWALL_IN_EULAON_DYNAMIC", "-A"},   /* обычные прошивки: динамическая цепочка, дописываем в конец */
    {"FIREWALL_IN_EULAON", "-I"},           /* 9.61C: динамической нет — в начало основной, до запретов вендора */
};

/* правило без глагола: CHAIN -i IF -s NET -d IP/32 -p tcp --dport P -m comment --comment ptv-pair-XXXX -j ACCEPT */
static struct { int on; char chain[40], iface[IFNAMSIZ], net[24], dst[24], port[8], comment[40]; } R;

/* iptables с таймаутом 5 с; код выхода или -1. Без оболочки, stdout/stderr — в /dev/null. */
static int ipt(const char *verb, int with_rule) {
    const char *argv[24]; int n = 0;
    argv[n++] = IPT; argv[n++] = verb; argv[n++] = R.chain;
    if (with_rule) {
        const char *rest[] = {"-i", R.iface, "-s", R.net, "-d", R.dst, "-p", "tcp", "--dport", R.port,
                              "-m", "comment", "--comment", R.comment, "-j", "ACCEPT"};
        for (size_t i = 0; i < sizeof rest / sizeof *rest; i++) argv[n++] = rest[i];
    }
    argv[n] = NULL;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execv(IPT, (char *const *)argv);
        _exit(127);
    }
    for (int i = 0; i < 100; i++) {
        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (r < 0) return -1;
        struct timespec w = {0, 50000000};
        nanosleep(&w, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

/* Сторож: sleep N; iptables -D <правило>. Отдельная сессия — переживает падение приложения. */
static int guard(int seconds) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        if (fork() != 0) _exit(0);           /* внук: не зомби у приложения */
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        char s[16]; snprintf(s, sizeof s, "%d", seconds);
        execl("/bin/sh", "sh", "-c", "sleep \"$1\"; shift; exec " IPT " -D \"$@\"", "ptv-pair-guard", s,
              R.chain, "-i", R.iface, "-s", R.net, "-d", R.dst, "-p", "tcp", "--dport", R.port,
              "-m", "comment", "--comment", R.comment, "-j", "ACCEPT", (char *)NULL);
        _exit(127);
    }
    int st;
    return waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* интерфейс и подсеть, где у ТВ адрес ip; только частные сети /16…/30 */
static int find_iface(const char *ip) {
    struct in_addr a;
    if (inet_pton(AF_INET, ip, &a) != 1) return -1;
    uint32_t h = ntohl(a.s_addr);
    int priv = (h >> 24) == 10 || (h >> 20) == (172u << 4 | 1) || (h >> 16) == (192u << 8 | 168);
    if (!priv) return -1;
    struct ifaddrs *ifs = NULL, *i;
    if (getifaddrs(&ifs)) return -1;
    int found = 0;
    for (i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
        if (((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr != a.s_addr) continue;
        uint32_t m = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
        int bits = __builtin_popcount(m);
        if (bits < 16 || bits > 30 || !i->ifa_name || strlen(i->ifa_name) >= IFNAMSIZ) continue;
        struct in_addr net = {htonl(h & m)};
        char n[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &net, n, sizeof n);
        snprintf(R.iface, sizeof R.iface, "%s", i->ifa_name);
        snprintf(R.net, sizeof R.net, "%s/%d", n, bits);
        found++;
    }
    freeifaddrs(ifs);
    return found == 1 ? 0 : -1;
}

int fw_open(const char *ip, int port, int guard_seconds) {
    fw_close();
    const char *why = NULL;
    if (access(IPT, X_OK)) why = "no_iptables";
    else if (port < 1024 || port > 65535) why = "port";
    else if (find_iface(ip)) why = "iface";
    if (!why) {
        snprintf(R.dst, sizeof R.dst, "%s/32", ip);
        snprintf(R.port, sizeof R.port, "%d", port);
        unsigned char r[8] = {0};
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { if (read(fd, r, sizeof r) != (ssize_t)sizeof r) memset(r, 0, sizeof r); close(fd); }
        int k = snprintf(R.comment, sizeof R.comment, "ptv-pair-");
        for (int i = 0; i < 8; i++) k += snprintf(R.comment + k, sizeof R.comment - (size_t)k, "%02x", r[i]);
        const char *verb = NULL;
        for (size_t c = 0; c < sizeof CHAINS / sizeof *CHAINS && !verb; c++) {
            snprintf(R.chain, sizeof R.chain, "%s", CHAINS[c].name);
            if (ipt("-S", 0) == 0) verb = CHAINS[c].verb;    /* только чтение: есть ли цепочка */
        }
        if (!verb) why = "chain";
        else if (guard(guard_seconds)) why = "guard";
        else if (ipt(verb, 1)) { ipt("-D", 1); why = "insert"; }
        else if (ipt("-C", 1)) { ipt("-D", 1); why = "verify"; }
        else R.on = 1;
    }
    if (why) { fprintf(stderr, "fw_open_failed %s\n", why); return -1; }
    fprintf(stderr, "fw_open chain=%s port=%s\n", R.chain, R.port);
    return 0;
}

void fw_close(void) {
    if (!R.on) return;
    ipt("-D", 1);
    R.on = 0;
}
