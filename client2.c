// client2.c
// Like client1 but uses 20ms windows and sends control messages to server
// to adjust output1 frequency/amplitude based on output3 value.

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <poll.h>
#include <math.h>

#define MAX_PORT 3
#define BUF_SIZE 2048
#define TOKEN_MAX 512
#define CONTROL_PORT 4000

// Debug macro - only prints if DEBUG is defined at compile time
#ifdef DEBUG_ENABLED
#define DEBUG(fmt, ...) fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG(fmt, ...) /* disabled */
#endif

// Control protocol fields (16-bit unsigned big-endian)
#define OP_READ 1
#define OP_WRITE 2
#define OBJ_OUT1 1
#define PROP_FREQ 255      // Property ID 255 = Frequency
#define PROP_AMP 170       // Property ID 170 = Amplitude

struct conn {
	int port;
	int fd;
	char inbuf[BUF_SIZE];
	int inlen;
	char latest[TOKEN_MAX];
	int have;
	time_t last_connect_try;
};

static long long epoch_ms_now(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000LL + (tv.tv_usec / 1000LL);
}

static int set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1) return -1;
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;
	return 0;
}

static int connect_to_port(int port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	if (set_nonblocking(fd) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void trim(char *s) {
	int i = 0, j = strlen(s) - 1;
	while (i <= j && (s[i] == '\n' || s[i] == '\r' || s[i] == ' ' || s[i] == '\t')) i++;
	while (j >= i && (s[j] == '\n' || s[j] == '\r' || s[j] == ' ' || s[j] == '\t')) j--;
	if (i == 0 && j == (int)strlen(s) - 1) return;
	if (i > j) { s[0] = '\0'; return; }
	memmove(s, s + i, j - i + 1);
	s[j - i + 1] = '\0';
}

static int create_control_socket(void) {
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return -1;
	return fd;
}

static void send_read_command(int fd, struct sockaddr_in *addr, uint16_t obj, uint16_t prop) {
	uint16_t msg[3];
	msg[0] = htons(OP_READ);
	msg[1] = htons(obj);
	msg[2] = htons(prop);
	sendto(fd, msg, sizeof(msg), MSG_CONFIRM, (const struct sockaddr *)addr, sizeof(*addr));
}

int main(void) {
    struct conn conns[MAX_PORT];
    int ports[MAX_PORT] = {4001, 4002, 4003};
    for (int i = 0; i < MAX_PORT; ++i) {
        conns[i].port = ports[i];
        conns[i].fd = -1;
        conns[i].inlen = 0;
        conns[i].latest[0] = '\0';
        conns[i].have = 0;
        conns[i].last_connect_try = 0;
    }

    // Create UDP control socket
    int ctrl_fd = create_control_socket();
    struct sockaddr_in ctrl_addr;
    memset(&ctrl_addr, 0, sizeof(ctrl_addr));
    ctrl_addr.sin_family = AF_INET;
    ctrl_addr.sin_port = htons(CONTROL_PORT);
    inet_pton(AF_INET, "127.0.0.1", &ctrl_addr.sin_addr);

    long long now_ms = epoch_ms_now();
    long long next_tick = now_ms + (20 - (now_ms % 20));

    int last_state = -1; // -1 unknown, 0 <3.0, 1 >=3.0

    while (1) {
        long long now_try = epoch_ms_now();
        for (int i = 0; i < MAX_PORT; ++i) {
            if (conns[i].fd < 0) {
                if (now_try - (long long)conns[i].last_connect_try >= 1000) {
                    conns[i].last_connect_try = now_try;
                    int fd = connect_to_port(conns[i].port);
                    if (fd >= 0) {
                        conns[i].fd = fd;
                        conns[i].inlen = 0;
                        conns[i].have = 0;
                        conns[i].latest[0] = '\0';
                    }
                }
            }
        }

        int nfds = 0;
        struct pollfd pfds[MAX_PORT];
        for (int i = 0; i < MAX_PORT; ++i) {
            if (conns[i].fd >= 0) {
                pfds[nfds].fd = conns[i].fd;
                pfds[nfds].events = POLLIN | POLLHUP | POLLERR;
                pfds[nfds].revents = 0;
                nfds++;
            }
        }

        long long now = epoch_ms_now();
        long long timeout = next_tick - now;
        if (timeout < 0) timeout = 0;
        if (timeout > 2000) timeout = 2000;

        int poll_res = 0;
        if (nfds > 0) poll_res = poll(pfds, nfds, (int)timeout);
        else {
            struct timespec ts;
            ts.tv_sec = timeout / 1000;
            ts.tv_nsec = (timeout % 1000) * 1000000;
            nanosleep(&ts, NULL);
        }

        if (poll_res > 0) {
            int idx = 0;
            for (int i = 0; i < MAX_PORT; ++i) {
                if (conns[i].fd < 0) continue;
                struct pollfd *p = &pfds[idx++];
                if (p->revents & POLLIN) {
                    while (1) {
                        ssize_t r = recv(conns[i].fd, conns[i].inbuf + conns[i].inlen, sizeof(conns[i].inbuf) - conns[i].inlen - 1, 0);
                        if (r > 0) {
                            conns[i].inlen += r;
                            conns[i].inbuf[conns[i].inlen] = '\0';
                            char *start = conns[i].inbuf;
                            char *pnl;
                            while ((pnl = strpbrk(start, "\r\n")) != NULL) {
                                size_t len = pnl - start;
                                char token[TOKEN_MAX];
                                if (len >= sizeof(token)) len = sizeof(token) - 1;
                                memcpy(token, start, len);
                                token[len] = '\0';
                                trim(token);
                                if (token[0] != '\0') {
                                    size_t clen = strlen(token);
                                    if (clen > sizeof(conns[i].latest) - 1) clen = sizeof(conns[i].latest) - 1;
                                    memcpy(conns[i].latest, token, clen);
                                    conns[i].latest[clen] = '\0';
                                    conns[i].have = 1;
                                }
                                start = pnl + 1;
                            }
                            int remain = conns[i].inlen - (start - conns[i].inbuf);
                            if (remain > 0 && start != conns[i].inbuf) memmove(conns[i].inbuf, start, remain);
                            conns[i].inlen = remain;
                            conns[i].inbuf[conns[i].inlen] = '\0';
                            continue;
                        } else if (r == 0) {
                            close(conns[i].fd);
                            conns[i].fd = -1;
                            conns[i].inlen = 0;
                            break;
                        } else {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            close(conns[i].fd);
                            conns[i].fd = -1;
                            conns[i].inlen = 0;
                            break;
                        }
                    }
                } else if (p->revents & (POLLHUP | POLLERR)) {
                    close(conns[i].fd);
                    conns[i].fd = -1;
                    conns[i].inlen = 0;
                }
            }
        }

        now = epoch_ms_now();
        if (now >= next_tick) {
            long long ts = next_tick;
            const char *o1 = conns[0].have ? conns[0].latest : "--";
            const char *o2 = conns[1].have ? conns[1].latest : "--";
            const char *o3 = conns[2].have ? conns[2].latest : "--";

            // control logic based on out3
            double v3 = 0.0/0.0; // NaN
            if (conns[2].have) {
                char *endptr = NULL;
                v3 = strtod(conns[2].latest, &endptr);
                if (endptr == conns[2].latest) v3 = 0.0/0.0;
            }
            int state = -1;
            if (!isnan(v3)) {
                state = (v3 >= 3.0) ? 1 : 0;
            }

            if (state != -1 && state != last_state) {
                // send settings to output1 over UDP control port
                DEBUG("State change detected: %d -> %d, out3=%.1f", last_state, state, v3);
                if (ctrl_fd >= 0) {
                    if (state == 1) {
                        // >=3.0 -> freq 1kHz (1000), amp 8000
                        DEBUG("Sending: freq=1000 (1kHz), amp=8000 (threshold reached)");
                        sendto(ctrl_fd, &(uint16_t[4]){htons(OP_WRITE), htons(OBJ_OUT1), htons(PROP_FREQ), htons(1000)}, 8, MSG_CONFIRM, (const struct sockaddr *)&ctrl_addr, sizeof(ctrl_addr));
                        sendto(ctrl_fd, &(uint16_t[4]){htons(OP_WRITE), htons(OBJ_OUT1), htons(PROP_AMP), htons(8000)}, 8, MSG_CONFIRM, (const struct sockaddr *)&ctrl_addr, sizeof(ctrl_addr));
                        // Verify by reading back
                        DEBUG("Verifying frequency and amplitude...");
                        send_read_command(ctrl_fd, &ctrl_addr, OBJ_OUT1, PROP_FREQ);
                        send_read_command(ctrl_fd, &ctrl_addr, OBJ_OUT1, PROP_AMP);
                    } else {
                        // <3.0 -> freq 2kHz (2000), amp 4000
                        DEBUG("Sending: freq=2000 (2kHz), amp=4000 (threshold not reached)");
                        sendto(ctrl_fd, &(uint16_t[4]){htons(OP_WRITE), htons(OBJ_OUT1), htons(PROP_FREQ), htons(2000)}, 8, MSG_CONFIRM, (const struct sockaddr *)&ctrl_addr, sizeof(ctrl_addr));
                        sendto(ctrl_fd, &(uint16_t[4]){htons(OP_WRITE), htons(OBJ_OUT1), htons(PROP_AMP), htons(4000)}, 8, MSG_CONFIRM, (const struct sockaddr *)&ctrl_addr, sizeof(ctrl_addr));
                        // Verify by reading back
                        DEBUG("Verifying frequency and amplitude...");
                        send_read_command(ctrl_fd, &ctrl_addr, OBJ_OUT1, PROP_FREQ);
                        send_read_command(ctrl_fd, &ctrl_addr, OBJ_OUT1, PROP_AMP);
                    }
                } else {
                    DEBUG("ERROR: Control socket not available (fd=%d)", ctrl_fd);
                }
                last_state = state;
            }

            // print JSON line
            printf("{\"timestamp\": %lld, \"out1\": \"%s\", \"out2\": \"%s\", \"out3\": \"%s\"}\n",
                   ts, o1, o2, o3);
            fflush(stdout);

            for (int i = 0; i < MAX_PORT; ++i) conns[i].have = 0;

            do { next_tick += 20; } while (next_tick <= now);
        }
    }

    return 0;
}
