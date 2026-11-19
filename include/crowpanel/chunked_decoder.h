#pragma once

#include <cstddef>
#include <cstdint>

namespace services {

/**
 * Incremental decoder for an HTTP "Transfer-Encoding: chunked" body, for code
 * that reads the response stream itself instead of through HTTPClient.
 */
class ChunkedDecoder {
 public:
  /**
   * Decode `length` raw bytes in place and return how many body bytes now
   * start at `data`. Call repeatedly as bytes arrive. The zero-length chunk and
   * the trailer after it are consumed too, so a connection that is kept open
   * has nothing of this response left on it.
   */
  size_t decode(uint8_t* data, size_t length) {
    size_t out = 0;
    for (size_t i = 0; i < length && state_ != State::Done; ++i) {
      const uint8_t c = data[i];
      switch (state_) {
        case State::Size: {
          const int digit = hexValue(c);
          if (digit >= 0) {
            remaining_ = remaining_ * 16 + static_cast<size_t>(digit);
            size_seen_ = true;
          } else if (c == ';') {
            state_ = State::Extension;
          } else if (c == '\n' && size_seen_) {
            startChunk();
          }
          // Otherwise this is the CR of a size line or the CRLF that closes
          // the previous chunk's data.
          break;
        }
        case State::Extension:
          if (c == '\n') {
            startChunk();
          }
          break;
        case State::Data:
          data[out++] = c;
          if (--remaining_ == 0) {
            state_ = State::Size;
          }
          break;
        case State::Trailer:
          // Optional trailer fields, then an empty line ends the response.
          if (c == '\n') {
            if (trailer_line_empty_) {
              state_ = State::Done;
            }
            trailer_line_empty_ = true;
          } else if (c != '\r') {
            trailer_line_empty_ = false;
          }
          break;
        case State::Done:
          break;
      }
    }
    return out;
  }

  /** True once the whole response, trailer included, has been read. */
  bool done() const { return state_ == State::Done; }
  /** True once every body byte has been returned, even if the trailer has not. */
  bool bodyComplete() const {
    return state_ == State::Trailer || state_ == State::Done;
  }

 private:
  enum class State { Size, Extension, Data, Trailer, Done };

  static int hexValue(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  void startChunk() {
    size_seen_ = false;
    trailer_line_empty_ = true;
    state_ = remaining_ == 0 ? State::Trailer : State::Data;
  }

  State state_ = State::Size;
  size_t remaining_ = 0;
  bool size_seen_ = false;
  bool trailer_line_empty_ = true;
};

}  // namespace services
