#include "mqtt.h"
#include "pack.h"
#include <stdlib.h>
#include <string.h>

/**
 * MQTT v3.1.1 standard. Remaining length field for fixed header
 * is at max 4 bytes
 */
static const int MAX_LEN_BYTES = 4;

static size_t unpack_mqtt_connect(const unsigned char *, union mqtt_header *,
                                  union mqtt_packet *);
static size_t unpack_mqtt_publish(const unsigned char *, union mqtt_header *,
                                  union mqtt_packet *);
static size_t unpack_mqtt_subscribe(const unsigned char *, union mqtt_header *,
                                    union mqtt_packet *);
static size_t unpack_mqtt_unsubscribe(const unsigned char *,
                                      union mqtt_header *, union mqtt_packet *);
static size_t unpack_mqtt_ack(const unsigned char *, union mqtt_header *,
                              union mqtt_packet *);
static unsigned char *pack_mqtt_header(const union mqtt_header *);
static unsigned char *pack_mqtt_ack(const union mqtt_packet *);
static unsigned char *pack_mqtt_connack(const union mqtt_packet *);
static unsigned char *pack_mqtt_suback(const union mqtt_packet *);
static unsigned char *pack_mqtt_publish(const union mqtt_packet *);

/**
 * Encode remaining length on a MQTT packet header, comprised of variable
 * header and payload if present. It does not take into account the bytes
 * required to store itself.
 */
int mqtt_encode_length(unsigned char *buf, size_t len) {
  int bytes = 0;
  do {
    if (bytes + 1 > MAX_LEN_BYTES)
      return bytes;
    short d = len % 128;
    len /= 128;
    // if there are more digits to encode, set the top bit of this digit
    if (len > 0)
      d |= 128;
    buf[bytes++] = d;
  } while (len > 0);
  return bytes;
}

/**
 * Decode the remaining length comprised of the variable header and payload
 * if present. It does not take into account the bytes for storing length
 *
 * TODO: Handle case where multiplier > 128 * 128 * 128
 */
unsigned long long mqtt_decode_length(const unsigned char **buf) {
  char c;
  int multiplier = 1;
  unsigned long long value = 0LL;
  do {
    c = **buf;
    value += (c & 127) * multiplier;
    multiplier *= 128;
    (*buf)++;
  } while ((c & 128) != 0);
  return value;
}

/**
 * MQTT unpacking functions
 */
static size_t unpack_mqtt_connect(const unsigned char *buf,
                                  union mqtt_header *hdr,
                                  union mqtt_packet *pkt) {

  struct mqtt_connect connect = {.header = *hdr};
  pkt->connect = connect;
  const unsigned char *init = buf;
  /**
   * Second byte of the fixed header, containts the length of the
   * remaining bytes for the connect packet
   */
  size_t len = mqtt_decode_length(&buf);
  // ignore checks on protocol name and reserved bits. skip to 8th byte
  buf = init + 8;
  // read variable header byte flags
  pkt->connect.byte = unpack_u8((const uint8_t **)&buf);
  // read keepalive MSB and LSB (2 byte words)
  pkt->connect.payload.keepalive = unpack_u16((const uint8_t **)&buf);
  // Read CID length (2 byte words)
  uint16_t cid_len = unpack_u16((const uint8_t **)&buf);
  // read the client id
  if (cid_len > 0) {
    pkt->connect.payload.client_id = malloc(cid_len + 1);
    unpack_bytes((const uint8_t **)&buf, cid_len,
                 pkt->connect.payload.client_id);
  }
  //   read the will topic and message if will is set on flags
  if (pkt->connect.bits.will == 1) {
    unpack_string16(&buf, &pkt->connect.payload.will_topic);
    unpack_string16(&buf, &pkt->connect.payload.will_message);
  }
  //   read the username if username flag is set
  if (pkt->connect.bits.username == 1)
    unpack_string16(&buf, &pkt->connect.payload.username);

  // read the password if the password flag is set
  if (pkt->connect.bits.password == 1)
    unpack_string16(&buf, &pkt->connect.payload.password);
  return len;
}

static size_t unpack_mqtt_publish(const unsigned char *buf,
                                  union mqtt_header *hdr,
                                  union mqtt_packet *pkt) {
  struct mqtt_publish publish = {.header = *hdr};
  pkt->publish = publish;
  /**
   * Second byte of the fixed header. Contains the length of the
   * remaining bytes of the publish packet
   */
  size_t len = mqtt_decode_length(&buf);
  //   Read topic length and topic of the soon-to-be-published message
  pkt->publish.topiclen = unpack_string16(&buf, &pkt->publish.topic);
  uint16_t message_len = len;
  if (publish.header.bits.qos > AT_MOST_ONCE) {
    pkt->publish.pkt_id = unpack_u16(((const uint8_t **)&buf));
    message_len -= sizeof(uint8_t);
  }
  /**
   * Message len is calculated by subtracting the length of the variable
   * header from the remaining length field that is in the fixed header
   */
  message_len -= (sizeof(uint16_t) + pkt->publish.topiclen);
  pkt->publish.payloadlen = message_len;
  pkt->publish.payload = malloc(message_len + 1);
  unpack_bytes((const uint8_t **)&buf, message_len, pkt->publish.payload);
  return len;
}

static size_t unpack_mqtt_subscribe(const unsigned char *buf,
                                    union mqtt_header *hdr,
                                    union mqtt_packet *pkt) {
  struct mqtt_subscribe subscribe = {.header = *hdr};
  /**
   * Second byte of the fixed header. Contains the length of the
   * remaining bytes of the subscribe packet
   */
  size_t len = mqtt_decode_length(&buf);
  size_t remaining_bytes = len;
  //   read packet id
  subscribe.pkt_id = unpack_u16((const uint8_t **)&buf);
  remaining_bytes -= sizeof(uint16_t);
  /**
   * read in a loop all the remaining bytes that is specified by
   * the length found in the fixed header.
   * From now on, the payload consists of 3-tuples formed by
   * - topic length
   * - topic filter (string)
   * - qos
   */
  int i = 0;
  while (remaining_bytes > 0) {
    // read length bytes of the first topic filter
    remaining_bytes -= sizeof(uint16_t);
    // we have to make room for additional tuples
    subscribe.tuples =
        realloc(subscribe.tuples, (i + 1) * sizeof(*subscribe.tuples));
    subscribe.tuples[i].topic_len =
        unpack_string16(&buf, &subscribe.tuples[i].topic);
    remaining_bytes -= subscribe.tuples[i].topic_len;
    subscribe.tuples[i].qos = unpack_u8((const uint8_t **)&buf);
    len -= sizeof(uint8_t);
    i++;
  }
  subscribe.tuples_len = i;
  pkt->subscribe = subscribe;
  return len;
}

static size_t unpack_mqtt_unsubscribe(const unsigned char *buf,
                                      union mqtt_header *hdr,
                                      union mqtt_packet *pkt) {
  struct mqtt_unsubscribe unsubscribe = {.header = *hdr};
  /**
   * Second byte of the fixed header. Contains the length of the
   * remaining bytes of the unsubscribe packet
   */
  size_t len = mqtt_decode_length(&buf);
  size_t remaining_bytes = len;
  // read packet id
  unsubscribe.pkt_id = unpack_u16((const uint8_t **)&buf);
  remaining_bytes -= sizeof(uint16_t);

  /**
   * read in a loop all the remaining bytes that is specified by
   * the length found in the fixed header.
   * From now on, the payload consists of 3-tuples formed by
   * - topic length
   * - topic filter (string)
   */
  int i = 0;
  while (remaining_bytes > 0) {
    // read length bytes of the first topic filter
    remaining_bytes -= sizeof(uint16_t);
    // we have to make room for additional tuples
    unsubscribe.tuples =
        realloc(unsubscribe.tuples, (i + 1) * sizeof(*unsubscribe.tuples));
    unsubscribe.tuples[i].topic_len =
        unpack_string16(&buf, &unsubscribe.tuples[i].topic);
    remaining_bytes -= unsubscribe.tuples[i].topic_len;
    i++;
  }
  unsubscribe.tuples_len = i;
  pkt->unsubscribe = unsubscribe;
  return len;
}

static size_t unpack_mqtt_ack(const unsigned char *buf, union mqtt_header *hdr,
                              union mqtt_packet *pkt) {
  struct mqtt_ack ack = {.header = *hdr};
  /**
   * Second byte of the fixed header. Contains the length of the
   * remaining bytes of the ack packet
   */
  size_t len = mqtt_decode_length(&buf);
  ack.pkt_id = unpack_u16((const uint8_t **)&buf);
  pkt->ack = ack;
  return len;
}

typedef size_t mqtt_unpack_handler(const unsigned char *, union mqtt_header *,
                                   union mqtt_packet *);
/**
 * Unpack functions mapping unpacking_handlers positioned in the
 * array based on packet type (enum in mqtt.h)
 */
static mqtt_unpack_handler *unpack_handlers[11] = {NULL,
                                                   unpack_mqtt_connect,
                                                   NULL,
                                                   unpack_mqtt_publish,
                                                   unpack_mqtt_ack,
                                                   unpack_mqtt_ack,
                                                   unpack_mqtt_ack,
                                                   unpack_mqtt_ack,
                                                   unpack_mqtt_subscribe,
                                                   NULL,
                                                   unpack_mqtt_unsubscribe};

int unpack_mqtt_packet(const unsigned char *buf, union mqtt_packet *pkt) {
  int rc = 0;
  // read the first byte of the fixed header
  unsigned char type = *buf;
  union mqtt_header header = {.byte = type};
  if (header.bits.type == DISCONNECT || header.bits.type == PINGREQ ||
      header.bits.type == PINGRESP)
    pkt->header = header;
  else
    rc = unpack_handlers[header.bits.type](++buf, &header, pkt);
  return rc;
}
