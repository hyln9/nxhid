#include "server.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }

void server_drop(Server *s) {
    bool release = s->client >= 0;
    if (s->client >= 0) close(s->client);
    s->client = -1;
    s->used = s->output_size = s->output_sent = 0;
    s->peer[0] = 0;
    if (release) s->write_report((const uint8_t[8]){0});
}

void server_close(Server *s) {
    server_drop(s);
    if (s->listener >= 0) close(s->listener);
    s->listener = -1;
}

bool server_init(Server *s, uint16_t port, ReportWriter writer, LedReader reader) {
    *s = (Server){.listener = -1, .client = -1, .write_report = writer, .read_leds = reader};
    s->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listener < 0) return false;
    int yes = 1;
    setsockopt(s->listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY};
    if (!nonblock(s->listener) || bind(s->listener, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(s->listener, 2) < 0) {
        int saved = errno;
        server_close(s);
        errno = saved;
        return false;
    }
    return true;
}

static void reply(Server *s, const char *text) {
    s->output_size = strlen(text);
    memcpy(s->output, text, s->output_size);
    s->output_sent = 0;
}

static void command(Server *s, uint64_t now) {
    s->line[s->used] = 0;
    Command c = protocol_parse(s->line);
    s->used = 0;
    if (c.type == CMD_PING) {
        reply(s, "OK\n");
    } else if (c.type == CMD_LEDS) {
        char response[16];
        snprintf(response, sizeof(response), "LEDS %02x\n", s->read_leds() & 0x1f);
        reply(s, response);
    } else if (c.type == CMD_REPORT && s->write_report(c.report)) {
        s->reports++;
        reply(s, "OK\n");
    } else {
        // Invalid input or failed USB delivery invalidates the whole session.
        server_drop(s);
        return;
    }
    s->last_activity = now;
}

static void tick_client(Server *s, uint64_t now, bool enabled) {
    if (s->client < 0) return;
    if (!enabled || now - s->last_activity >= NXHID_TIMEOUT_MS) { server_drop(s); return; }
    if (s->output_sent < s->output_size) {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        ssize_t n = send(s->client, s->output + s->output_sent, s->output_size - s->output_sent, flags);
        if (n > 0) s->output_sent += (size_t)n;
        else if (n == 0 || !would_block()) server_drop(s);
        return;
    }
    // Read at most one bounded line per tick, preserving TCP stream framing.
    for (size_t budget = 0; budget < NXHID_LINE_SIZE; budget++) {
        char ch;
        ssize_t n = recv(s->client, &ch, 1, 0);
        if (n < 0 && would_block()) return;
        if (n <= 0) { server_drop(s); return; }
        if (ch == '\n') { command(s, now); return; }
        if (ch < 32 || ch > 126 || s->used == sizeof(s->line) - 1) { server_drop(s); return; }
        s->line[s->used++] = ch;
    }
}

void server_tick(Server *s, uint64_t now, bool enabled) {
    // Consume a previous sender's EOF and release its keys before deciding whether
    // a new connection is a second sender. Both can arrive between ticks.
    tick_client(s, now, enabled);
    struct sockaddr_in addr;
    socklen_t size = sizeof(addr);
    int incoming = accept(s->listener, (struct sockaddr *)&addr, &size);
    if (incoming >= 0) {
        if (!enabled || s->client >= 0 || !nonblock(incoming)) close(incoming);
        else {
            int yes = 1;
            setsockopt(incoming, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#ifdef SO_NOSIGPIPE
            setsockopt(incoming, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
            s->client = incoming;
            s->last_activity = now;
            inet_ntop(AF_INET, &addr.sin_addr, s->peer, sizeof(s->peer));
        }
    }
}
