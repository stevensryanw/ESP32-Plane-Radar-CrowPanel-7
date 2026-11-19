#include "crowpanel/phone_location.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecp.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "crowpanel/phone_location_page.h"
#include "services/radar_location.h"
#include "services/route_lookup.h"

namespace crowpanel::phone_location {
namespace {

constexpr unsigned long kValidForMs = 10UL * 60UL * 1000UL;

// --- The certificate -------------------------------------------------------

// Before this the clock has not been set from the network (2026-01-01).
constexpr time_t kClockIsSetAfter = 1767225600;
constexpr time_t kDaySeconds = 86400;
constexpr time_t kCertificateDays = 365;
// Make a new one well before the old one's year is up.
constexpr int64_t kCertificateGoodForUs = 300LL * 86400LL * 1000000LL;

// Kept until the radar restarts, so that a phone which has been told to trust
// it is not asked again the next time. It names the radar's address and
// carries dates, so it is made again if either goes stale.
uint8_t s_certificate[768];
size_t s_certificate_length = 0;
uint8_t s_private_key[192];
size_t s_private_key_length = 0;
uint32_t s_certificate_address = 0;
int64_t s_certificate_made_us = 0;
bool s_certificate_dated_by_clock = false;

int hardwareRandom(void*, unsigned char* output, size_t length) {
  esp_fill_random(output, length);
  return 0;
}

bool clockIsSet() { return time(nullptr) > kClockIsSetAfter; }

void formatTime(time_t when, char* text, size_t length) {
  struct tm parts;
  gmtime_r(&when, &parts);
  strftime(text, length, "%Y%m%d%H%M%S", &parts);
}

// mbedTLS's writers fill their buffer from the end and return how much they
// wrote. Moves that to the start; returns 0, or the writer's error.
int moveToStart(uint8_t* buffer, size_t size, int written, size_t* length) {
  if (written <= 0) {
    return written == 0 ? -1 : written;
  }
  memmove(buffer, buffer + size - written, written);
  *length = static_cast<size_t>(written);
  return 0;
}

bool makeCertificate(const IPAddress& address) {
  const uint32_t started_ms = millis();
  mbedtls_pk_context key;
  mbedtls_x509write_cert certificate;
  mbedtls_mpi serial;
  mbedtls_pk_init(&key);
  mbedtls_x509write_crt_init(&certificate);
  mbedtls_mpi_init(&serial);

  uint8_t serial_bytes[8];
  esp_fill_random(serial_bytes, sizeof(serial_bytes));
  // A serial number is a positive integer; keep all eight bytes of it.
  serial_bytes[0] = (serial_bytes[0] & 0x7F) | 0x40;

  const bool by_clock = clockIsSet();
  const time_t now = by_clock ? time(nullptr) : kClockIsSetAfter;
  char not_before[16];
  char not_after[16];
  // From yesterday, in case the phone's clock is a little behind.
  formatTime(now - kDaySeconds, not_before, sizeof(not_before));
  formatTime(now + kCertificateDays * kDaySeconds, not_after, sizeof(not_after));

  // subjectAltName, written out by hand because this mbedTLS has no call for
  // it: a sequence holding one iPAddress. Browsers match the address against
  // this, not against the name.
  const uint8_t alt_name[] = {0x30, 0x06, 0x87, 0x04, address[0],
                              address[1], address[2], address[3]};
  // extKeyUsage: a sequence holding the identifier for a TLS server.
  const uint8_t server_use[] = {0x30, 0x0A, 0x06, 0x08, 0x2B, 0x06,
                                0x01, 0x05, 0x05, 0x07, 0x03, 0x01};

  int result = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (result == 0) {
    result = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                                 hardwareRandom, nullptr);
  }
  if (result == 0) {
    result = mbedtls_mpi_read_binary(&serial, serial_bytes, sizeof(serial_bytes));
  }
  if (result == 0) {
    mbedtls_x509write_crt_set_version(&certificate, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);
    // Signed with its own key: there is nobody else to sign it.
    mbedtls_x509write_crt_set_subject_key(&certificate, &key);
    mbedtls_x509write_crt_set_issuer_key(&certificate, &key);
    result = mbedtls_x509write_crt_set_serial(&certificate, &serial);
  }
  if (result == 0) {
    result = mbedtls_x509write_crt_set_subject_name(&certificate, "CN=Plane Radar");
  }
  if (result == 0) {
    result = mbedtls_x509write_crt_set_issuer_name(&certificate, "CN=Plane Radar");
  }
  if (result == 0) {
    result = mbedtls_x509write_crt_set_validity(&certificate, not_before, not_after);
  }
  if (result == 0) {
    result = mbedtls_x509write_crt_set_basic_constraints(&certificate, 0, -1);
  }
  if (result == 0) {
    result = mbedtls_x509write_crt_set_key_usage(&certificate,
                                                 MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
  }
  if (result == 0) {
    result = mbedtls_x509write_crt_set_extension(
        &certificate, MBEDTLS_OID_SUBJECT_ALT_NAME,
        MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0, alt_name, sizeof(alt_name));
  }
  if (result == 0) {
    result = mbedtls_x509write_crt_set_extension(
        &certificate, MBEDTLS_OID_EXTENDED_KEY_USAGE,
        MBEDTLS_OID_SIZE(MBEDTLS_OID_EXTENDED_KEY_USAGE), 0, server_use,
        sizeof(server_use));
  }
  if (result == 0) {
    result = moveToStart(s_certificate, sizeof(s_certificate),
                         mbedtls_x509write_crt_der(&certificate, s_certificate,
                                                   sizeof(s_certificate),
                                                   hardwareRandom, nullptr),
                         &s_certificate_length);
  }
  if (result == 0) {
    result = moveToStart(
        s_private_key, sizeof(s_private_key),
        mbedtls_pk_write_key_der(&key, s_private_key, sizeof(s_private_key)),
        &s_private_key_length);
  }
  mbedtls_mpi_free(&serial);
  mbedtls_x509write_crt_free(&certificate);
  mbedtls_pk_free(&key);

  if (result != 0) {
    s_certificate_length = 0;
    Serial.printf("phone_location: no certificate (mbedTLS -0x%04x)\n",
                  static_cast<unsigned>(-result));
    return false;
  }
  s_certificate_address = static_cast<uint32_t>(address);
  s_certificate_made_us = esp_timer_get_time();
  s_certificate_dated_by_clock = by_clock;
  Serial.printf("phone_location: certificate made in %u ms\n",
                static_cast<unsigned>(millis() - started_ms));
  return true;
}

bool haveCertificate() {
  const IPAddress address = WiFi.localIP();
  const bool usable =
      s_certificate_length > 0 &&
      s_certificate_address == static_cast<uint32_t>(address) &&
      esp_timer_get_time() - s_certificate_made_us < kCertificateGoodForUs &&
      // One dated before the clock was set may already look expired.
      (s_certificate_dated_by_clock || !clockIsSet());
  return usable || makeCertificate(address);
}

// --- Handing a position to the main loop -----------------------------------

// The server answers phones on a task of its own, but saving the location
// belongs to the main loop: nothing else in the firmware expects it to change
// under it. So the server offers the position and waits for the loop's word.
enum class Verdict : uint8_t { None, Saved, Failed };

constexpr TickType_t kLoopWait = pdMS_TO_TICKS(8000);

SemaphoreHandle_t s_handoff_lock = nullptr;  // Guards the four below.
SemaphoreHandle_t s_handoff_done = nullptr;  // Given when a verdict is in.
volatile bool s_offered = false;
double s_offered_lat = 0.0;
double s_offered_lon = 0.0;
Verdict s_verdict = Verdict::None;

// On the server's task.
Verdict offerToLoop(double lat, double lon) {
  xSemaphoreTake(s_handoff_lock, portMAX_DELAY);
  xSemaphoreTake(s_handoff_done, 0);  // A signal left by a wait that gave up.
  s_offered_lat = lat;
  s_offered_lon = lon;
  s_verdict = Verdict::None;
  s_offered = true;
  xSemaphoreGive(s_handoff_lock);

  xSemaphoreTake(s_handoff_done, kLoopWait);
  // If the loop is in the middle of saving, this waits for it to finish.
  xSemaphoreTake(s_handoff_lock, portMAX_DELAY);
  const Verdict verdict = s_verdict;
  s_offered = false;  // Withdrawn, if the loop never got to it.
  xSemaphoreGive(s_handoff_lock);
  return verdict;
}

// --- The server ------------------------------------------------------------

// This is a server of its own, small as it is, and not the SDK's
// esp_https_server, which was tried first. That one waits for as long as it
// takes for a caller to finish its TLS handshake, so anything on the network
// that connected and then said nothing stopped it answering, and stopped it
// shutting down: tapping Cancel froze the screen until the caller went away.
// Here every wait has a time limit and gives up at once when told to stop.
//
// It serves one caller at a time, one request per connection. An open TLS
// session takes nearly 60 KB of internal RAM, and next to the aircraft
// feed's own there is room for just one; others wait their turn.
//
// (Keeping TLS's buffers in PSRAM instead looked like the answer to that and
// was tried too. It freed the memory, and 11 frames slipped in 45 connections
// where none do with the buffers in internal RAM.)

constexpr uint16_t kPort = 443;
// How long one caller gets to complete the handshake and send its request,
// and then as long again to take the reply. While it has the server nobody
// else is served, so a caller that dawdles is not given long.
constexpr uint32_t kPatienceMs = 6000;
constexpr uint32_t kStopCheckMs = 100;
// Enough for a TLS handshake's arithmetic, with the certificate made first.
constexpr uint32_t kTaskStackBytes = 12288;
// A request's headers past this are not from a phone's browser.
constexpr size_t kMostRequestBytes = 8192;
constexpr int kGaveUp = -1;

SemaphoreHandle_t s_task_ready = nullptr;  // Given once the task is serving, or has failed to.
SemaphoreHandle_t s_task_gone = nullptr;   // Given as the task ends.
bool s_running = false;                    // The task exists. Main loop only.
volatile bool s_started = false;
volatile bool s_stop = false;
volatile bool s_waiting = false;
unsigned long s_started_ms = 0;
char s_key[17] = {};

int s_listener = -1;
mbedtls_ssl_config s_tls;
mbedtls_x509_crt s_tls_certificate;
mbedtls_pk_context s_tls_key;

bool timeIsUp() { return millis() - s_started_ms >= kValidForMs; }

// Waits until the socket can be read, or written. False if `deadline_ms`
// came first, or the server was told to stop.
bool waitFor(int socket, bool to_write, uint32_t deadline_ms) {
  while (!s_stop) {
    const int32_t left_ms = static_cast<int32_t>(deadline_ms - millis());
    if (left_ms <= 0) {
      return false;
    }
    fd_set sockets;
    FD_ZERO(&sockets);
    FD_SET(socket, &sockets);
    timeval wait = {};
    wait.tv_usec = static_cast<long>(std::min<int32_t>(left_ms, kStopCheckMs)) * 1000;
    const int ready = lwip_select(socket + 1, to_write ? nullptr : &sockets,
                                  to_write ? &sockets : nullptr, nullptr, &wait);
    if (ready != 0) {
      return ready > 0;
    }
  }
  return false;
}

int socketSend(void* context, const unsigned char* data, size_t length) {
  const int sent = lwip_send(*static_cast<int*>(context), data, length, 0);
  if (sent >= 0) {
    return sent;
  }
  return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE
                                                 : MBEDTLS_ERR_NET_SEND_FAILED;
}

int socketReceive(void* context, unsigned char* data, size_t length) {
  const int received = lwip_recv(*static_cast<int*>(context), data, length, 0);
  if (received >= 0) {
    return received;
  }
  return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_READ
                                                 : MBEDTLS_ERR_NET_RECV_FAILED;
}

// Repeats one mbedTLS call until it has done its work or failed, waiting on
// the socket in between. Returns the call's result, or kGaveUp.
template <typename Step>
int untilDone(int socket, uint32_t deadline_ms, Step step) {
  for (;;) {
    const int result = step();
    if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
      return result;
    }
    if (!waitFor(socket, result == MBEDTLS_ERR_SSL_WANT_WRITE, deadline_ms)) {
      return kGaveUp;
    }
  }
}

bool sendAll(mbedtls_ssl_context* session, int socket, uint32_t deadline_ms,
             const char* data, size_t length) {
  while (length > 0) {
    const int sent = untilDone(socket, deadline_ms, [&] {
      return mbedtls_ssl_write(session, reinterpret_cast<const unsigned char*>(data),
                               length);
    });
    if (sent <= 0) {
      return false;
    }
    data += sent;
    length -= static_cast<size_t>(sent);
  }
  return true;
}

void reply(mbedtls_ssl_context* session, int socket, const char* status,
           const char* type, const char* body, size_t length) {
  const uint32_t deadline_ms = millis() + kPatienceMs;
  char head[192];
  const int head_length = snprintf(head, sizeof(head),
                                   "HTTP/1.1 %s\r\n"
                                   "Content-Type: %s; charset=utf-8\r\n"
                                   "Content-Length: %u\r\n"
                                   "Cache-Control: no-store\r\n"
                                   "Connection: close\r\n\r\n",
                                   status, type, static_cast<unsigned>(length));
  if (sendAll(session, socket, deadline_ms, head, static_cast<size_t>(head_length)) &&
      sendAll(session, socket, deadline_ms, body, length)) {
    untilDone(socket, deadline_ms, [&] { return mbedtls_ssl_close_notify(session); });
  }
}

void replyText(mbedtls_ssl_context* session, int socket, const char* status,
               const char* text) {
  reply(session, socket, status, "text/plain", text, strlen(text));
}

// Reads a request's headers to their end and keeps its first line, which is
// all that is used. The rest still has to be read: closing a connection with
// input unread makes the far end throw the reply away.
bool readRequestLine(mbedtls_ssl_context* session, int socket, uint32_t deadline_ms,
                     char* line, size_t size) {
  size_t line_length = 0;
  bool line_complete = false;
  int ending = 0;  // How much of "\r\n\r\n" the input currently ends with.
  size_t total = 0;
  unsigned char chunk[256];
  while (total < kMostRequestBytes) {
    const int received = untilDone(socket, deadline_ms, [&] {
      return mbedtls_ssl_read(session, chunk, sizeof(chunk));
    });
    if (received <= 0) {
      return false;
    }
    total += static_cast<size_t>(received);
    for (int i = 0; i < received; ++i) {
      const char c = static_cast<char>(chunk[i]);
      if (!line_complete) {
        if (c == '\r' || c == '\n') {
          line_complete = true;
        } else if (line_length + 1 < size) {
          line[line_length++] = c;
        }
      }
      const bool expected = (ending % 2 == 0) ? c == '\r' : c == '\n';
      ending = expected ? ending + 1 : (c == '\r' ? 1 : 0);
      if (ending == 4) {
        line[line_length] = '\0';
        return true;
      }
    }
  }
  return false;
}

// Copies the value of `name` in a query string such as "lat=1.5&k=ab".
bool queryValue(const char* query, const char* name, char* value, size_t size) {
  const size_t name_length = strlen(name);
  for (const char* pair = query; pair != nullptr && *pair != '\0';) {
    const char* next = strchr(pair, '&');
    const size_t pair_length = next != nullptr ? static_cast<size_t>(next - pair)
                                               : strlen(pair);
    if (pair_length > name_length && pair[name_length] == '=' &&
        strncmp(pair, name, name_length) == 0) {
      const size_t length = pair_length - name_length - 1;
      if (length >= size) {
        return false;
      }
      memcpy(value, pair + name_length + 1, length);
      value[length] = '\0';
      return true;
    }
    pair = next != nullptr ? next + 1 : nullptr;
  }
  return false;
}

// Takes the same time whichever character differs.
bool keyMatches(const char* offered) {
  if (strlen(offered) != sizeof(s_key) - 1) {
    return false;
  }
  uint8_t difference = 0;
  for (size_t i = 0; i < sizeof(s_key) - 1; ++i) {
    difference |= static_cast<uint8_t>(offered[i] ^ s_key[i]);
  }
  return difference == 0;
}

bool readCoordinate(const char* text, double limit, double* value) {
  if (text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  *value = strtod(text, &end);
  return *end == '\0' && std::isfinite(*value) && *value >= -limit &&
         *value <= limit;
}

void receivePosition(mbedtls_ssl_context* session, int socket, const char* query) {
  char key[24];
  if (!s_waiting || timeIsUp() || !queryValue(query, "k", key, sizeof(key)) ||
      !keyMatches(key)) {
    replyText(session, socket, "403 Forbidden",
              "That code has expired. Tap Find me on the radar again and scan "
              "the new code.");
    return;
  }
  char lat_text[20];
  char lon_text[20];
  double lat = 0.0;
  double lon = 0.0;
  if (!queryValue(query, "lat", lat_text, sizeof(lat_text)) ||
      !queryValue(query, "lon", lon_text, sizeof(lon_text)) ||
      !readCoordinate(lat_text, 90.0, &lat) || !readCoordinate(lon_text, 180.0, &lon)) {
    replyText(session, socket, "400 Bad Request",
              "That position could not be used. Try again.");
    return;
  }
  switch (offerToLoop(lat, lon)) {
    case Verdict::Saved:
      replyText(session, socket, "200 OK", "Saved");
      return;
    case Verdict::Failed:
      replyText(session, socket, "500 Internal Server Error",
                "The radar could not save that position. Try again.");
      return;
    case Verdict::None:
      break;
  }
  replyText(session, socket, "503 Service Unavailable",
            "The radar was busy and did not save the position. Try again.");
}

void answer(mbedtls_ssl_context* session, int socket, uint32_t deadline_ms) {
  char line[160];
  if (!readRequestLine(session, socket, deadline_ms, line, sizeof(line))) {
    return;
  }
  // "GET /?k=0123abcd HTTP/1.1"
  char* target = strchr(line, ' ');
  char* version = target != nullptr ? strchr(target + 1, ' ') : nullptr;
  if (version == nullptr) {
    replyText(session, socket, "400 Bad Request", "Not understood.");
    return;
  }
  *target++ = '\0';
  *version = '\0';
  char* query = strchr(target, '?');
  if (query != nullptr) {
    *query++ = '\0';
  }
  if (strcmp(line, "GET") == 0 && strcmp(target, "/") == 0) {
    reply(session, socket, "200 OK", "text/html", kPage, sizeof(kPage) - 1);
  } else if (strcmp(line, "POST") == 0 && strcmp(target, "/set") == 0) {
    receivePosition(session, socket, query != nullptr ? query : "");
  } else {
    replyText(session, socket, "404 Not Found", "There is nothing here.");
  }
}

// One caller, from handshake to goodbye. Closes the socket.
void serve(int socket) {
  lwip_fcntl(socket, F_SETFL, O_NONBLOCK);
  mbedtls_ssl_context session;
  mbedtls_ssl_init(&session);
  if (mbedtls_ssl_setup(&session, &s_tls) == 0) {
    mbedtls_ssl_set_bio(&session, &socket, socketSend, socketReceive, nullptr);
    const uint32_t deadline_ms = millis() + kPatienceMs;
    const int result = untilDone(socket, deadline_ms,
                                 [&] { return mbedtls_ssl_handshake(&session); });
    // A phone that does not trust the certificate yet breaks off here, and
    // comes back once its owner has said to carry on.
    if (result == 0) {
      answer(&session, socket, deadline_ms);
    }
  }
  mbedtls_ssl_free(&session);
  // Say that nothing more is coming, then give the far end a moment to do the
  // same: closing first can cut off a reply that is still on its way.
  lwip_shutdown(socket, SHUT_WR);
  const uint32_t deadline_ms = millis() + 500;
  char unwanted[64];
  while (waitFor(socket, false, deadline_ms) &&
         lwip_recv(socket, unwanted, sizeof(unwanted), 0) > 0) {
  }
  lwip_close(socket);
}

bool prepareTls() {
  mbedtls_ssl_config_init(&s_tls);
  mbedtls_x509_crt_init(&s_tls_certificate);
  mbedtls_pk_init(&s_tls_key);
  if (mbedtls_x509_crt_parse_der(&s_tls_certificate, s_certificate,
                                 s_certificate_length) != 0 ||
      mbedtls_pk_parse_key(&s_tls_key, s_private_key, s_private_key_length, nullptr,
                           0) != 0 ||
      mbedtls_ssl_config_defaults(&s_tls, MBEDTLS_SSL_IS_SERVER,
                                  MBEDTLS_SSL_TRANSPORT_STREAM,
                                  MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
    return false;
  }
  mbedtls_ssl_conf_rng(&s_tls, hardwareRandom, nullptr);
  // TLS 1.2 and up: nothing older is offered by a phone made this decade.
  mbedtls_ssl_conf_min_version(&s_tls, MBEDTLS_SSL_MAJOR_VERSION_3,
                               MBEDTLS_SSL_MINOR_VERSION_3);
  return mbedtls_ssl_conf_own_cert(&s_tls, &s_tls_certificate, &s_tls_key) == 0;
}

bool openListener() {
  s_listener = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (s_listener < 0) {
    return false;
  }
  // So that "Find me" can be opened again straight after it was closed.
  const int yes = 1;
  lwip_setsockopt(s_listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(kPort);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  return lwip_bind(s_listener, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == 0 &&
         lwip_listen(s_listener, 2) == 0 &&
         lwip_fcntl(s_listener, F_SETFL, O_NONBLOCK) == 0;
}

void serverTask(void*) {
  const uint32_t started_ms = millis();
  const bool ready = haveCertificate() && prepareTls() && openListener();
  s_started = ready;
  if (ready) {
    Serial.printf("phone_location: HTTPS server up in %u ms\n",
                  static_cast<unsigned>(millis() - started_ms));
  } else {
    Serial.println("phone_location: HTTPS server did not start");
  }
  xSemaphoreGive(s_task_ready);
  while (ready && !s_stop) {
    if (!waitFor(s_listener, false, millis() + kStopCheckMs)) {
      continue;
    }
    const int caller = lwip_accept(s_listener, nullptr, nullptr);
    if (caller >= 0) {
      serve(caller);
    }
  }
  if (s_listener >= 0) {
    lwip_close(s_listener);
    s_listener = -1;
  }
  mbedtls_ssl_config_free(&s_tls);
  mbedtls_x509_crt_free(&s_tls_certificate);
  mbedtls_pk_free(&s_tls_key);
  Serial.printf("phone_location: server stopped, %u bytes of its stack unused\n",
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  xSemaphoreGive(s_task_gone);
  vTaskDelete(nullptr);
}

}  // namespace

Start begin(char* url, size_t length) {
  end();
  url[0] = '\0';
  if (WiFi.status() != WL_CONNECTED) {
    return Start::NoWifi;
  }
  if (s_handoff_lock == nullptr) {
    s_handoff_lock = xSemaphoreCreateMutex();
    s_handoff_done = xSemaphoreCreateBinary();
    s_task_ready = xSemaphoreCreateBinary();
    s_task_gone = xSemaphoreCreateBinary();
  }
  if (s_handoff_lock == nullptr || s_handoff_done == nullptr ||
      s_task_ready == nullptr || s_task_gone == nullptr) {
    return Start::Failed;
  }
  // 64 bits from the hardware generator: not something to arrive at by trying.
  snprintf(s_key, sizeof(s_key), "%08lx%08lx", static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
  s_started_ms = millis();
  s_stop = false;
  // A route lookup is a TLS session too, and there is no room for it and a
  // phone's at once. They wait until end().
  services::route::pause(true);
  // The server gets a task of its own, on the Wi-Fi core and below it in
  // priority: a handshake is a few hundred milliseconds of arithmetic, and
  // the other core draws. The main loop's task could not run it anyway, with
  // under 3 KB of stack to spare.
  if (xTaskCreatePinnedToCore(serverTask, "locate", kTaskStackBytes, nullptr, 2,
                              nullptr, 0) != pdPASS) {
    services::route::pause(false);
    return Start::Failed;
  }
  s_running = true;
  // It makes the certificate first if there is none, about a third of a second.
  xSemaphoreTake(s_task_ready, portMAX_DELAY);
  if (!s_started) {
    end();
    return Start::Failed;
  }
  snprintf(url, length, "https://%s/?k=%s", WiFi.localIP().toString().c_str(), s_key);
  s_waiting = true;
  return Start::Ready;
}

void end() {
  s_waiting = false;
  if (!s_running) {
    return;
  }
  s_stop = true;
  // A phone's request may be waiting for the main loop, and the main loop is
  // here, about to wait for the server. Let the request go first.
  xSemaphoreTake(s_handoff_lock, portMAX_DELAY);
  s_offered = false;
  xSemaphoreGive(s_handoff_done);
  xSemaphoreGive(s_handoff_lock);
  // Every wait in the task is short or watches s_stop, so this is brief.
  xSemaphoreTake(s_task_gone, portMAX_DELAY);
  s_running = false;
  services::route::pause(false);
}

bool expired() { return s_waiting && timeIsUp(); }

bool received() {
  if (!s_offered) {
    return false;
  }
  xSemaphoreTake(s_handoff_lock, portMAX_DELAY);
  bool saved = false;
  if (s_offered) {
    s_offered = false;
    saved = services::location::saveUserLocation(s_offered_lat, s_offered_lon);
    if (saved) {
      s_waiting = false;  // The key has done its one job.
    }
    s_verdict = saved ? Verdict::Saved : Verdict::Failed;
    xSemaphoreGive(s_handoff_done);
  }
  xSemaphoreGive(s_handoff_lock);
  return saved;
}

}  // namespace crowpanel::phone_location
