/*
 * mqtt_switch.c - Paho MQTT platform layer for Nintendo Switch (libnx)
 *
 * This file implements the two platform abstractions that Paho needs:
 *
 *   Timer   — uses ARM system counter (hardware register, no syscall)
 *   Network — uses BSD sockets with poll() for timeouts
 *
 * Compare with the Linux port (MQTTLinux.c) which uses:
 *   Timer   → gettimeofday()     (POSIX wall clock)
 *   Network → setsockopt()       (socket-level timeout)
 *
 * We use poll() instead of setsockopt(SO_RCVTIMEO) because libnx
 * may not support socket timeout options, but poll() is guaranteed.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>

#include "mqtt_switch.h"

/* ================================================================
 * Timer implementation — ARM system counter
 *
 * The Switch's Cortex-A57 has a dedicated counter register
 * (CNTPCT_EL0) that increments at a fixed frequency. libnx
 * exposes it via armGetSystemTick().
 *
 * This is the same kind of hardware timer you'd use on STM32
 * (SysTick or a TIM peripheral), but accessed through a CPU
 * register instead of a memory-mapped peripheral.
 * ================================================================ */

void TimerInit(Timer *timer)
{
    timer->end_tick = 0;
}

char TimerIsExpired(Timer *timer)
{
    return armGetSystemTick() >= timer->end_tick;
}

void TimerCountdownMS(Timer *timer, unsigned int ms)
{
    u64 freq = armGetSystemTickFreq();
    timer->end_tick = armGetSystemTick() + (u64)ms * freq / 1000;
}

void TimerCountdown(Timer *timer, unsigned int seconds)
{
    u64 freq = armGetSystemTickFreq();
    timer->end_tick = armGetSystemTick() + (u64)seconds * freq;
}

int TimerLeftMS(Timer *timer)
{
    u64 now = armGetSystemTick();

    if (now >= timer->end_tick)
        return 0;

    u64 freq = armGetSystemTickFreq();
    return (int)((timer->end_tick - now) * 1000 / freq);
}

/* ================================================================
 * Network implementation — BSD sockets with poll()
 *
 * Paho's read/write functions have a timeout parameter.
 * In Step 2, our mqtt_raw used blocking recv() with no timeout —
 * fine for a one-shot test, but Paho needs timeouts to manage
 * keepalive (sending PINGREQ if idle for too long).
 *
 * poll() blocks until a socket is ready or the timeout expires.
 * It's the standard POSIX way to do multiplexed I/O and is
 * available on libnx, Linux, and most embedded TCP stacks.
 * ================================================================ */

/*
 * switch_read - Read exactly `len` bytes with timeout
 *
 * Paho expects this to either:
 *   - Return `len` (all bytes received)
 *   - Return 0 (connection closed)
 *   - Return -1 (error or timeout)
 *
 * The loop handles partial reads — TCP is a stream protocol,
 * so recv() may return fewer bytes than requested even when
 * more are coming. Same lesson as mqtt_raw.c Step 2, but now
 * handled properly.
 */
static int switch_read(Network *n, unsigned char *buf, int len, int timeout_ms)
{
    int bytes = 0;

    while (bytes < len) {
        /* Wait for data or timeout */
        struct pollfd pfd = { .fd = n->socket, .events = POLLIN };
        int rc = poll(&pfd, 1, timeout_ms);

        if (rc == 0)
            break;              /* Timeout — return what we have */
        if (rc < 0)
            return -1;          /* poll() error */

        rc = recv(n->socket, &buf[bytes], len - bytes, 0);

        if (rc > 0)
            bytes += rc;
        else if (rc == 0)
            return 0;           /* Connection closed by peer */
        else
            return -1;          /* recv() error */
    }

    return bytes;
}

/*
 * switch_write - Write `len` bytes with timeout
 *
 * Similar to read, but for sending. poll() with POLLOUT ensures
 * the socket's send buffer has space before we call send().
 */
static int switch_write(Network *n, unsigned char *buf, int len, int timeout_ms)
{
    struct pollfd pfd = { .fd = n->socket, .events = POLLOUT };
    int rc = poll(&pfd, 1, timeout_ms);

    if (rc <= 0)
        return -1;

    return send(n->socket, buf, len, 0);
}

/* ================================================================
 * NetworkInit / NetworkConnect / NetworkDisconnect
 *
 * Same socket setup as mqtt_raw_connect() from Step 2, but
 * packaged into the struct that Paho expects.
 * ================================================================ */

void NetworkInit(Network *n)
{
    n->socket = -1;
    n->mqttread = switch_read;
    n->mqttwrite = switch_write;
}

int NetworkConnect(Network *n, char *addr, int port)
{
    n->socket = socket(AF_INET, SOCK_STREAM, 0);
    if (n->socket < 0)
        return -1;

    struct sockaddr_in broker;
    memset(&broker, 0, sizeof(broker));
    broker.sin_family = AF_INET;
    broker.sin_port = htons(port);

    if (inet_aton(addr, &broker.sin_addr) == 0) {
        close(n->socket);
        n->socket = -1;
        return -1;
    }

    if (connect(n->socket, (struct sockaddr *)&broker, sizeof(broker)) < 0) {
        close(n->socket);
        n->socket = -1;
        return -1;
    }

    return 0;
}

void NetworkDisconnect(Network *n)
{
    if (n->socket >= 0) {
        close(n->socket);
        n->socket = -1;
    }
}
