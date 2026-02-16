/*
 * mqtt_switch.h - Paho MQTT platform layer for Nintendo Switch (libnx)
 *
 * This is the "porting layer" — the glue between Paho's
 * platform-agnostic MQTT client and the Switch's hardware.
 *
 * Paho needs two abstractions from every platform:
 *
 *   1. Timer  — countdown timers for keepalive and command timeouts
 *   2. Network — read/write with timeout for TCP communication
 *
 * On Linux, timers use gettimeofday() and sockets use setsockopt().
 * On FreeRTOS, timers use TickType_t and sockets go through lwIP.
 * On Switch, we use armGetSystemTick() and BSD sockets with poll().
 *
 * Paho includes this header via:
 *   -DMQTTCLIENT_PLATFORM_HEADER=mqtt_switch.h
 * which makes MQTTClient.h do: #include "mqtt_switch.h"
 */

#ifndef MQTT_SWITCH_H
#define MQTT_SWITCH_H

#include <switch.h>

/*
 * Timer — backed by the ARM system counter (CNTPCT_EL0)
 *
 * armGetSystemTick() reads the hardware counter directly via
 * an MRS instruction — no syscall, nanosecond-class resolution.
 * This is the same counter used by the Switch's OS scheduler.
 *
 * We store the absolute tick value at which the timer expires.
 * Checking expiry is just: current_tick >= end_tick.
 */
typedef struct Timer {
    u64 end_tick;
} Timer;

void TimerInit(Timer *timer);
char TimerIsExpired(Timer *timer);
void TimerCountdownMS(Timer *timer, unsigned int ms);
void TimerCountdown(Timer *timer, unsigned int seconds);
int  TimerLeftMS(Timer *timer);

/*
 * Network — TCP socket with timeout-capable read/write
 *
 * Paho calls mqttread() and mqttwrite() with a timeout parameter.
 * Unlike Step 2's blocking recv(), we use poll() to implement
 * timeouts — this lets Paho manage keepalive correctly.
 *
 * The function pointer signature (from Paho):
 *   int read(Network*, unsigned char* buffer, int len, int timeout_ms);
 *   int write(Network*, unsigned char* buffer, int len, int timeout_ms);
 *
 * Return: number of bytes transferred, or negative on error.
 */
typedef struct Network {
    int socket;
    int (*mqttread)(struct Network *, unsigned char *, int, int);
    int (*mqttwrite)(struct Network *, unsigned char *, int, int);
} Network;

void NetworkInit(Network *n);
int  NetworkConnect(Network *n, char *addr, int port);
void NetworkDisconnect(Network *n);

#endif /* MQTT_SWITCH_H */
