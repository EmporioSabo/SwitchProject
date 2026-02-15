/*
 * mqtt_raw.c - Raw MQTT 3.1.1 packet crafting over TCP
 *
 * Every MQTT packet follows the same structure:
 *
 *   +------------------+----------------------------+
 *   |   Fixed Header   |  Variable Header + Payload |
 *   +------------------+----------------------------+
 *   | type | rem. len  |  (depends on packet type)  |
 *   | 1 byte | 1-4 B   |                            |
 *   +------------------+----------------------------+
 *
 * The "remaining length" field uses a variable-length encoding:
 * each byte encodes 7 bits of length, with bit 7 as a continuation
 * flag. This lets MQTT encode lengths from 0 to 268,435,455 in
 * 1 to 4 bytes. For our small packets, it's always 1 byte.
 *
 * Reference: MQTT v3.1.1 spec, sections 2 and 3.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "mqtt_raw.h"

/* ----------------------------------------------------------------
 * MQTT Control Packet types (upper 4 bits of byte 0)
 * Spec section 2.2.1, Table 2.1
 * ---------------------------------------------------------------- */
#define MQTT_PKT_CONNECT     0x10   /* Client → Server: connection request      */
#define MQTT_PKT_CONNACK     0x20   /* Server → Client: connection acknowledged */
#define MQTT_PKT_PUBLISH     0x30   /* Either direction: publish message        */
#define MQTT_PKT_DISCONNECT  0xE0   /* Client → Server: clean disconnect        */

/* ----------------------------------------------------------------
 * encode_remaining_length - MQTT variable-length integer encoding
 *
 * MQTT encodes the "remaining length" (everything after the fixed
 * header) using a scheme where:
 *   - Each byte contributes 7 bits of value (bits 6..0)
 *   - Bit 7 is a continuation flag: 1 = more bytes follow
 *
 * Examples:
 *   length=0     → [0x00]           (1 byte)
 *   length=127   → [0x7F]           (1 byte)
 *   length=128   → [0x80, 0x01]     (2 bytes: 0 + 128*1)
 *   length=16383 → [0xFF, 0x7F]     (2 bytes: 127 + 128*127)
 *
 * Returns: number of bytes written to buf (1 to 4).
 * ---------------------------------------------------------------- */
static int encode_remaining_length(uint8_t *buf, uint32_t length)
{
    int count = 0;

    do {
        uint8_t encoded = length % 128;   /* Take lowest 7 bits       */
        length /= 128;                    /* Shift right by 7          */

        if (length > 0)
            encoded |= 0x80;             /* Set continuation bit      */

        buf[count++] = encoded;
    } while (length > 0);

    return count;
}

/* ================================================================
 * mqtt_raw_connect - Open a TCP socket to the MQTT broker
 *
 * This is pure BSD sockets — nothing MQTT-specific yet.
 * The three steps are the same on any POSIX system:
 *   1. socket()   — create an endpoint
 *   2. fill sockaddr_in — specify destination (IP + port)
 *   3. connect()  — establish TCP connection (3-way handshake)
 * ================================================================ */
int mqtt_raw_connect(const char *broker_ip, int port)
{
    /*
     * socket(AF_INET, SOCK_STREAM, 0)
     *   AF_INET     = IPv4 address family
     *   SOCK_STREAM = TCP (reliable, ordered byte stream)
     *   0           = let the kernel pick the protocol (TCP for STREAM)
     *
     * Returns a file descriptor — an integer handle to this socket.
     * Everything in Unix is a file, including network connections.
     */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        printf("[TCP] socket() failed: %s\n", strerror(errno));
        return -1;
    }

    /*
     * sockaddr_in: the "address" structure for IPv4.
     *
     *   sin_family = AF_INET  (must match the socket family)
     *   sin_port   = port number in NETWORK byte order (big-endian)
     *   sin_addr   = IP address in NETWORK byte order
     *
     * Why network byte order? Different CPUs store multi-byte integers
     * differently (little-endian on ARM/x86, big-endian on network).
     * htons() converts Host TO Network Short (16-bit).
     * inet_aton() parses "192.168.1.100" into a 32-bit network-order address.
     */
    struct sockaddr_in broker_addr;
    memset(&broker_addr, 0, sizeof(broker_addr));
    broker_addr.sin_family = AF_INET;
    broker_addr.sin_port   = htons(port);

    if (inet_aton(broker_ip, &broker_addr.sin_addr) == 0) {
        printf("[TCP] Invalid IP address: %s\n", broker_ip);
        close(sockfd);
        return -1;
    }

    /*
     * connect() initiates TCP's 3-way handshake:
     *   Client → SYN       → Server
     *   Client ← SYN+ACK   ← Server
     *   Client → ACK       → Server
     *
     * This blocks until the handshake completes or fails.
     * On success, the socket is ready for send()/recv().
     */
    if (connect(sockfd, (struct sockaddr *)&broker_addr, sizeof(broker_addr)) < 0) {
        printf("[TCP] connect() failed: %s\n", strerror(errno));
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/* ================================================================
 * mqtt_raw_send_connect - Build and send MQTT CONNECT packet
 *
 * MQTT CONNECT is the first packet the client sends after TCP
 * connects. It tells the broker:
 *   - "I speak MQTT 3.1.1"
 *   - "My name is <client_id>"
 *   - "Start a clean session"
 *   - "Ping me if I'm silent for 60 seconds"
 *
 * Packet layout (spec section 3.1):
 *
 *   Fixed Header
 *   +---------+-----------------+
 *   | 0x10    | remaining_len   |   type=CONNECT, flags=0
 *   +---------+-----------------+
 *
 *   Variable Header (10 bytes, always the same for our case)
 *   +------+------+---+---+---+---+------+-------+------+------+
 *   | 0x00 | 0x04 | M | Q | T | T | 0x04 | 0x02  | 0x00 | 0x3C |
 *   +------+------+---+---+---+---+------+-------+------+------+
 *   |  protocol name (len+str)    | level | flags | keep-alive  |
 *   +-----------------------------+-------+-------+-------------+
 *
 *   Payload
 *   +------+------+---...---+
 *   | MSB  | LSB  | client  |   client ID as length-prefixed UTF-8
 *   +------+------+---...---+
 * ================================================================ */
int mqtt_raw_send_connect(int sockfd, const char *client_id)
{
    /*
     * Strategy: build the body (variable header + payload) first,
     * then prepend the fixed header. This way we know the exact
     * remaining length before we need to encode it.
     */
    uint8_t body[256];
    int pos = 0;

    /* --- Variable Header --- */

    /* Protocol Name: MQTT uses length-prefixed strings.
     * 2 bytes of length (big-endian) followed by the string bytes.
     * "MQTT" = 4 bytes, so length = 0x0004. */
    body[pos++] = 0x00;   /* Length MSB */
    body[pos++] = 0x04;   /* Length LSB = 4 */
    body[pos++] = 'M';
    body[pos++] = 'Q';
    body[pos++] = 'T';
    body[pos++] = 'T';

    /* Protocol Level: 4 = MQTT 3.1.1 (5 would be MQTT 5.0) */
    body[pos++] = 0x04;

    /* Connect Flags (1 byte, each bit has a meaning):
     *   Bit 7: Username Flag    = 0 (no username)
     *   Bit 6: Password Flag    = 0 (no password)
     *   Bit 5: Will Retain      = 0
     *   Bit 4-3: Will QoS       = 00
     *   Bit 2: Will Flag        = 0 (no last will message)
     *   Bit 1: Clean Session    = 1 (don't resume old session)
     *   Bit 0: Reserved         = 0
     *                           --------
     *                           0x02
     */
    body[pos++] = 0x02;

    /* Keep Alive: 60 seconds, as a 16-bit big-endian integer.
     * If the client doesn't send anything for 60s, the broker
     * expects a PINGREQ. We won't implement ping in this step,
     * but 60s is long enough for our test. */
    body[pos++] = 0x00;   /* MSB: 0   */
    body[pos++] = 0x3C;   /* LSB: 60  */

    /* --- Payload --- */

    /* Client Identifier: another length-prefixed UTF-8 string.
     * This uniquely identifies our client to the broker. */
    uint16_t id_len = strlen(client_id);
    body[pos++] = (id_len >> 8) & 0xFF;   /* Length MSB */
    body[pos++] = id_len & 0xFF;           /* Length LSB */
    memcpy(&body[pos], client_id, id_len);
    pos += id_len;

    /* --- Fixed Header --- */

    /* Now we know remaining_length = pos (size of body).
     * Build the complete packet: fixed header + body. */
    uint8_t packet[264];
    int pkt_pos = 0;

    packet[pkt_pos++] = MQTT_PKT_CONNECT;  /* 0x10 = CONNECT */

    /* Encode remaining length */
    pkt_pos += encode_remaining_length(&packet[pkt_pos], pos);

    /* Append body */
    memcpy(&packet[pkt_pos], body, pos);
    pkt_pos += pos;

    /* --- Send --- */

    ssize_t sent = send(sockfd, packet, pkt_pos, 0);
    if (sent < 0) {
        printf("[MQTT] send CONNECT failed: %s\n", strerror(errno));
        return -1;
    }

    printf("[MQTT] CONNECT sent (%d bytes)\n", (int)sent);
    return 0;
}

/* ================================================================
 * mqtt_raw_recv_connack - Receive and parse MQTT CONNACK
 *
 * CONNACK is the broker's response to CONNECT. It's always
 * exactly 4 bytes — the simplest packet to parse:
 *
 *   +------+------+-------------------+-------------+
 *   | 0x20 | 0x02 | Acknowledge Flags | Return Code |
 *   +------+------+-------------------+-------------+
 *   byte 0  byte 1      byte 2            byte 3
 *
 *   Acknowledge Flags: bit 0 = Session Present (was there an
 *     existing session for this client ID?)
 *
 *   Return Code:
 *     0 = Connection Accepted
 *     1 = Unacceptable protocol version
 *     2 = Client identifier rejected
 *     3 = Server unavailable
 *     4 = Bad username or password
 *     5 = Not authorized
 * ================================================================ */
int mqtt_raw_recv_connack(int sockfd)
{
    uint8_t buf[4];

    /*
     * recv() reads bytes from the TCP stream.
     * We expect exactly 4 bytes for CONNACK.
     *
     * Note: TCP is a STREAM protocol — there are no message
     * boundaries. recv() might return fewer bytes than requested
     * if they haven't arrived yet. For 4 bytes on a LAN this
     * effectively never happens, but production code would loop
     * until all bytes are received. We'll handle that in Step 7.
     */
    ssize_t received = recv(sockfd, buf, sizeof(buf), 0);
    if (received <= 0) {
        if (received == 0)
            printf("[MQTT] Connection closed by broker\n");
        else
            printf("[MQTT] recv CONNACK failed: %s\n", strerror(errno));
        return -1;
    }

    if (received < 4) {
        printf("[MQTT] Incomplete CONNACK (%zd bytes)\n", received);
        return -1;
    }

    /* Verify packet type */
    if (buf[0] != MQTT_PKT_CONNACK) {
        printf("[MQTT] Expected CONNACK (0x20), got 0x%02X\n", buf[0]);
        return -1;
    }

    /* Verify remaining length */
    if (buf[1] != 0x02) {
        printf("[MQTT] Bad CONNACK remaining length: %d\n", buf[1]);
        return -1;
    }

    /* Parse fields */
    uint8_t session_present = buf[2] & 0x01;
    uint8_t return_code     = buf[3];

    /* Human-readable return codes (spec section 3.2.2.3) */
    static const char *rc_strings[] = {
        "Connection Accepted",
        "Unacceptable protocol version",
        "Client identifier rejected",
        "Server unavailable",
        "Bad username or password",
        "Not authorized",
    };

    if (return_code != 0) {
        const char *msg = (return_code < 6) ? rc_strings[return_code] : "Unknown";
        printf("[MQTT] Connection refused: %s (code=%d)\n", msg, return_code);
        return -1;
    }

    printf("[MQTT] CONNACK OK (session_present=%d)\n", session_present);
    return 0;
}

/* ================================================================
 * mqtt_raw_send_publish - Build and send MQTT PUBLISH (QoS 0)
 *
 * PUBLISH carries an application message to the broker.
 * At QoS 0 ("fire and forget"), no acknowledgment is expected.
 *
 * Packet layout (spec section 3.3):
 *
 *   Fixed Header
 *   +---------+-----------------+
 *   | 0x30    | remaining_len   |
 *   +---------+-----------------+
 *     Bits 3-0 of byte 0:
 *       bit 3 = DUP    (0 = first attempt)
 *       bit 2-1 = QoS  (00 = QoS 0)
 *       bit 0 = RETAIN (0 = don't retain)
 *
 *   Variable Header
 *   +------+------+---...---+
 *   | MSB  | LSB  | topic   |   topic as length-prefixed UTF-8
 *   +------+------+---...---+
 *   (No Packet Identifier for QoS 0)
 *
 *   Payload
 *   +---...---+
 *   | message |   raw bytes — length is implied by remaining_length
 *   +---...---+
 * ================================================================ */
int mqtt_raw_send_publish(int sockfd, const char *topic, const char *payload)
{
    uint8_t body[512];
    int pos = 0;

    /* --- Variable Header --- */

    /* Topic name: length-prefixed UTF-8 string */
    uint16_t topic_len = strlen(topic);
    body[pos++] = (topic_len >> 8) & 0xFF;  /* Length MSB */
    body[pos++] = topic_len & 0xFF;          /* Length LSB */
    memcpy(&body[pos], topic, topic_len);
    pos += topic_len;

    /* No Packet Identifier — QoS 0 doesn't need one.
     * At QoS 1/2, a 16-bit packet ID would go here so the broker
     * can acknowledge which specific message it received. */

    /* --- Payload --- */

    /* The message content. Unlike topic and client_id, the payload
     * is NOT length-prefixed — its length is inferred from
     * remaining_length minus the variable header size. */
    uint16_t payload_len = strlen(payload);
    memcpy(&body[pos], payload, payload_len);
    pos += payload_len;

    /* --- Fixed Header --- */

    uint8_t packet[520];
    int pkt_pos = 0;

    /* 0x30 = PUBLISH with DUP=0, QoS=0, RETAIN=0 */
    packet[pkt_pos++] = MQTT_PKT_PUBLISH;

    pkt_pos += encode_remaining_length(&packet[pkt_pos], pos);

    memcpy(&packet[pkt_pos], body, pos);
    pkt_pos += pos;

    /* --- Send --- */

    ssize_t sent = send(sockfd, packet, pkt_pos, 0);
    if (sent < 0) {
        printf("[MQTT] send PUBLISH failed: %s\n", strerror(errno));
        return -1;
    }

    printf("[MQTT] Published to '%s': '%s' (%d bytes)\n",
           topic, payload, (int)sent);
    return 0;
}

/* ================================================================
 * mqtt_raw_send_disconnect - Send MQTT DISCONNECT packet
 *
 * The simplest MQTT packet: just 2 bytes.
 * Tells the broker "I'm leaving cleanly — don't publish my
 * Last Will and Testament (if I had one)."
 *
 *   +------+------+
 *   | 0xE0 | 0x00 |
 *   +------+------+
 *   type    remaining_length=0
 *
 * After sending this, the client should close the TCP connection.
 * The broker will also close its side.
 * ================================================================ */
int mqtt_raw_send_disconnect(int sockfd)
{
    uint8_t packet[2] = {
        MQTT_PKT_DISCONNECT,  /* 0xE0 */
        0x00                  /* remaining length = 0 */
    };

    ssize_t sent = send(sockfd, packet, sizeof(packet), 0);
    if (sent < 0) {
        printf("[MQTT] send DISCONNECT failed: %s\n", strerror(errno));
        return -1;
    }

    printf("[MQTT] DISCONNECT sent\n");
    return 0;
}

/* ================================================================
 * mqtt_raw_close - Close the TCP socket
 *
 * close() triggers TCP's 4-way teardown:
 *   Client → FIN     → Server
 *   Client ← ACK     ← Server
 *   Client ← FIN     ← Server
 *   Client → ACK     → Server
 *
 * After this, the file descriptor is invalid.
 * ================================================================ */
void mqtt_raw_close(int sockfd)
{
    close(sockfd);
    printf("[TCP] Connection closed\n");
}
