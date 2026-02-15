/*
 * mqtt_raw.h - Raw MQTT 3.1.1 packet crafting over TCP
 *
 * This module builds MQTT control packets byte-by-byte and sends them
 * over a plain TCP socket. No MQTT library is used — every byte is
 * explicit so you can see exactly what travels on the wire.
 *
 * Reference: MQTT v3.1.1 specification (OASIS Standard, 2014)
 * http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/mqtt-v3.1.1.html
 */

#ifndef MQTT_RAW_H
#define MQTT_RAW_H

/*
 * Open a TCP connection to the MQTT broker.
 * Returns: socket file descriptor on success, -1 on error.
 */
int mqtt_raw_connect(const char *broker_ip, int port);

/*
 * Build and send an MQTT CONNECT packet.
 * Uses MQTT 3.1.1, Clean Session, 60s keep-alive, no auth.
 * Returns: 0 on success, -1 on error.
 */
int mqtt_raw_send_connect(int sockfd, const char *client_id);

/*
 * Receive and parse an MQTT CONNACK packet.
 * Returns: 0 if connection accepted, -1 on error or rejection.
 */
int mqtt_raw_recv_connack(int sockfd);

/*
 * Build and send an MQTT PUBLISH packet (QoS 0, no retain).
 * Returns: 0 on success, -1 on error.
 */
int mqtt_raw_send_publish(int sockfd, const char *topic, const char *payload);

/*
 * Send an MQTT DISCONNECT packet (2 bytes).
 * Returns: 0 on success, -1 on error.
 */
int mqtt_raw_send_disconnect(int sockfd);

/*
 * Close the TCP socket.
 */
void mqtt_raw_close(int sockfd);

#endif /* MQTT_RAW_H */
