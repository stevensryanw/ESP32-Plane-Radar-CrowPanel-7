#pragma once

#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace services {

/** The parts of an HTTP/1.x response head a client needs to read the body. */
struct HttpHead {
  int status = 0;
  /** -1 when the response carries no Content-Length. */
  int content_length = -1;
  bool chunked = false;
  /** False when the server will close the connection after this response. */
  bool keep_alive = true;
};

namespace http_head_detail {

inline bool nameIs(const char* line, const char* colon, const char* name) {
  const size_t length = strlen(name);
  return static_cast<size_t>(colon - line) == length &&
         strncasecmp(line, name, length) == 0;
}

inline bool valueHas(const char* value, const char* end, const char* token) {
  const size_t length = strlen(token);
  for (; value + length <= end; ++value) {
    if (strncasecmp(value, token, length) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace http_head_detail

/** Larger than any body this firmware reads; a response claiming more is refused. */
constexpr long kMostContentLength = 64L * 1024L * 1024L;

/**
 * Parse a complete response head: the status line and headers up to and
 * including the blank line. Returns false if it is not an HTTP/1.x response.
 */
inline bool parseHttpHead(const char* head, HttpHead* out) {
  using namespace http_head_detail;
  *out = HttpHead{};
  if (strncmp(head, "HTTP/1.", 7) != 0) {
    return false;
  }
  const char* status = strchr(head, ' ');
  if (status == nullptr) {
    return false;
  }
  out->status = atoi(status + 1);
  out->keep_alive = head[7] != '0';  // HTTP/1.0 closes unless told otherwise.

  const char* line = strstr(head, "\r\n");
  while (line != nullptr) {
    line += 2;
    const char* end = strstr(line, "\r\n");
    if (end == nullptr || end == line) {
      break;  // Blank line: end of the head.
    }
    const char* colon =
        static_cast<const char*>(memchr(line, ':', static_cast<size_t>(end - line)));
    if (colon != nullptr) {
      const char* value = colon + 1;
      if (nameIs(line, colon, "content-length")) {
        // atoi() on a number too long for an int is undefined. A length that
        // is not a plausible one means this is not a response to trust.
        char* digits_end = nullptr;
        const long length = strtol(value, &digits_end, 10);
        if (digits_end == value || length < 0 || length > kMostContentLength) {
          return false;
        }
        out->content_length = static_cast<int>(length);
      } else if (nameIs(line, colon, "transfer-encoding")) {
        out->chunked = valueHas(value, end, "chunked");
      } else if (nameIs(line, colon, "connection")) {
        if (valueHas(value, end, "close")) {
          out->keep_alive = false;
        } else if (valueHas(value, end, "keep-alive")) {
          out->keep_alive = true;
        }
      }
    }
    line = end;
  }
  return out->status >= 100;
}

}  // namespace services
