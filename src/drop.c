/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Diamond Bond
 * The phone's way in: while Load's drop screen is open a thread serves one small page, and takes one
 * upload at a time into Music/Groovebox/Drops. Anyone on the network could reach it, so the page
 * lives under a code shown only on this screen, and the name, the size and the free space are all
 * the sender's word and are checked. No shell, no path from the wire: the name is reduced to plain
 * characters. Written to .new, fsynced and renamed, as the card can drop out. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>
#include "drop.h"
#include "platform.h"

#define DROP_MAX (64LL * 1024 * 1024) /* an hour's memo */
#define DROP_SPARE (8LL * 1024 * 1024) /* the card stays usable for the project */
#define IDLE_S 10                     /* a sender silent this long is gone */

static struct {
    pthread_t thread;
    volatile int run, ready;
    int fd;
    char code[8], url[64], path[900];
} d;

static const char PAGE[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'><title>Groovebox</title>"
    "<style>body{font:18px sans-serif;margin:2em;background:#101010;color:#e8e8e8}</style><h2>Send to Groovebox</h2>"
    "<input id=f type=file accept='audio/*'><p id=s></p><script>"
    "f.onchange=async()=>{let x=f.files[0];if(!x)return;s.textContent='Sending...';"
    "try{let r=await fetch(location.pathname.replace(/\\/$/,'')+'/up?name='+encodeURIComponent(x.name),{method:'POST',body:x});"
    "s.textContent=r.ok?'Sent. Look at the Q2.':'It would not take that file.'}catch(e){s.textContent='Lost the Q2.'}}</script>";

static void reply(int c, const char *status, const char *type, const char *body) {
    char h[160];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", status, type, strlen(body));
    if (send(c, h, (size_t)n, MSG_NOSIGNAL) == n) (void)!send(c, body, strlen(body), MSG_NOSIGNAL);
}
/* the sender's name, %xx undone, kept to what any card takes: never a path */
static void safe_name(const char *in, char *out, size_t cap) {
    size_t k = 0;
    for (; *in && *in != ' ' && *in != '&' && k + 1 < cap; ++in) {
        int c = (unsigned char)*in;
        if (c == '%' && in[1] && in[2]) {
            char hex[3] = { in[1], in[2], 0 };
            c = (int)strtol(hex, 0, 16), in += 2;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ' || (c == '.' && k)) out[k++] = (char)c;
    }
    out[k] = 0;
    if (!k) snprintf(out, cap, "memo.m4a");
}
static int make_path(const char *name, char *out, size_t n) {
    char base[700];
    snprintf(base, sizeof base, "%s/Music", plat_media()), mkdir(base, 0755);
    snprintf(base, sizeof base, "%s/Music/Groovebox", plat_media()), mkdir(base, 0755);
    snprintf(base, sizeof base, "%s/Music/Groovebox/Drops", plat_media()), mkdir(base, 0755);
    for (int i = 0; i < 1000; ++i) {
        const char *dot = strrchr(name, '.');
        int stem = dot ? (int)(dot - name) : (int)strlen(name);
        if (i) snprintf(out, n, "%s/%.*s-%d%s", base, stem, name, i, dot ? dot : "");
        else snprintf(out, n, "%s/%s", base, name);
        if (access(out, F_OK)) return 1;
    }
    return 0;
}
static int receive(int c, const char *name, long long size, const char *have, int nhave) {
    char path[800], tmp[820];
    static char buf[32768]; /* this thread's */
    struct statvfs fs;
    if (!make_path(name, path, sizeof path)) return 500;
    if (!statvfs(plat_media(), &fs) && (long long)fs.f_bavail * (long long)fs.f_frsize < size + DROP_SPARE) return 507;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int f = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0) return 500;
    long long got = 0;
    int idle = 0, ok = 1;
    if (nhave > size) nhave = (int)size;
    if (nhave && write(f, have, (size_t)nhave) != nhave) ok = 0;
    got = nhave;
    while (ok && got < size && d.run) {
        long long want = size - got > (long long)sizeof buf ? (long long)sizeof buf : size - got;
        ssize_t n = recv(c, buf, (size_t)want, 0);
        if (n > 0) idle = 0, ok = write(f, buf, (size_t)n) == n, got += n;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && ++idle < IDLE_S) continue;
        else ok = 0;
    }
    ok = ok && got == size && fsync(f) == 0;
    ok = close(f) == 0 && ok;
    if (!ok || rename(tmp, path)) return unlink(tmp), 400;
    snprintf(d.path, sizeof d.path, "%s", path);
    d.ready = 1;
    return 200;
}
static void serve(int c) {
    char h[4096], method[8], target[600], name[64];
    int got = 0, idle = 0;
    char *end = 0;
    struct timeval tv = { 1, 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    while (!end && got < (int)sizeof h - 1 && d.run) {
        ssize_t n = recv(c, h + got, sizeof h - 1 - (size_t)got, 0);
        if (n > 0) got += (int)n, h[got] = 0, idle = 0, end = strstr(h, "\r\n\r\n");
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && ++idle < IDLE_S) continue;
        else return;
    }
    if (!end || sscanf(h, "%7s %599s", method, target) != 2) return;
    size_t cl = strlen(d.code);
    const char *rest = target + 1 + cl;
    if (target[0] != '/' || strncmp(target + 1, d.code, cl) || (*rest && *rest != '/')) { reply(c, "404 Not Found", "text/plain", "no\n"); return; }
    if (!strcmp(method, "GET") && (!*rest || !strcmp(rest, "/"))) { reply(c, "200 OK", "text/html", PAGE); return; }
    if (strcmp(method, "POST") || strncmp(rest, "/up?name=", 9)) { reply(c, "404 Not Found", "text/plain", "no\n"); return; }
    const char *len = strcasestr(h, "\r\ncontent-length:");
    long long size = len ? strtoll(len + 17, 0, 10) : -1;
    if (size <= 0 || size > DROP_MAX) { reply(c, "413 Too Large", "text/plain", "no\n"); return; }
    safe_name(rest + 9, name, sizeof name);
    int code = receive(c, name, size, end + 4, got - (int)(end + 4 - h));
    if (code == 200) reply(c, "200 OK", "text/plain", "ok\n");
    else reply(c, code == 507 ? "507 No Space" : code == 500 ? "500 No" : "400 Bad", "text/plain", "no\n");
}
static void *loop(void *arg) {
    (void)arg;
    while (d.run) {
        struct pollfd p = { d.fd, POLLIN, 0 };
        if (poll(&p, 1, 200) <= 0) continue;
        int c = accept(d.fd, 0, 0);
        if (c < 0) continue;
        serve(c);
        close(c);
    }
    return 0;
}
static void local_ip(char *out, size_t n) { /* Wi-Fi if it is up, else any address that isn't this machine */
    struct ifaddrs *a, *i;
    out[0] = 0;
    if (getifaddrs(&a)) return;
    for (i = a; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !strcmp(i->ifa_name, "lo")) continue;
        if (out[0] && strncmp(i->ifa_name, "wlan", 4)) continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, (socklen_t)n);
    }
    freeifaddrs(a);
}
int drop_start(int port) {
    char ip[INET_ADDRSTRLEN];
    unsigned r = 0;
    int on = 1, u = open("/dev/urandom", O_RDONLY);
    if (u < 0 || read(u, &r, sizeof r) != sizeof r) { if (u >= 0) close(u); return 0; }
    close(u);
    snprintf(d.code, sizeof d.code, "%06u", r % 1000000);
    d.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (d.fd < 0) return 0;
    setsockopt(d.fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET, a.sin_port = htons((unsigned short)port), a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(d.fd, (struct sockaddr *)&a, sizeof a) || listen(d.fd, 2)) return close(d.fd), 0;
    d.ready = 0, d.run = 1;
    local_ip(ip, sizeof ip);
    if (ip[0]) snprintf(d.url, sizeof d.url, "http://%s:%d/%s", ip, port, d.code);
    else d.url[0] = 0;
    if (pthread_create(&d.thread, 0, loop, 0)) return d.run = 0, close(d.fd), 0;
    return 1;
}
void drop_stop(void) {
    if (!d.run) return;
    d.run = 0;
    pthread_join(d.thread, 0);
    close(d.fd);
}
const char *drop_url(void) { return d.url; }
int drop_take(char *path, size_t n) {
    if (!d.ready) return 0;
    snprintf(path, n, "%s", d.path);
    d.ready = 0;
    return 1;
}
