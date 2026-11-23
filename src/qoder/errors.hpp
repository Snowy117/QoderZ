#pragma once

#include "common/error.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace qoderz::qoder {

/// One classified Qoder failure, before it becomes a `qoderz::error`.
///
/// `m_code` is the numeric Qoder code as text, exactly as the mapping table compares it; an empty
/// value means the response carried no code. `m_retry_after` is in seconds.
struct qoder_error {
  error_kind m_kind;
  int m_status;
  std::string m_code;
  std::string m_message;
  std::optional<int> m_retry_after;
};

inline constexpr std::size_t max_error_message_length = 1000;

/// Classifies a non-2xx Qoder response by the table in docs/protocol/qoder-auth.md §8.
///
/// The order is fixed and each step can shadow a later one: a status outside 400..599 is a 502
/// before the body is even looked at, then the body's `code`/`queue` wins, then the status and the
/// message. The body may carry a UTF-8 BOM, a numeric or string `code`, a `details` string holding
/// a second JSON document, and a `10605` nested inside a 401/403 `message`. A body that is not
/// JSON is not an error of its own: the status still classifies it.
[[nodiscard]] qoder_error from_response(int http_status, std::string_view body);

/// Classifies a status that arrived without a body, as an SSE error frame does.
///
/// 400 is a bad request, 401/403 auth, 429 quota, 503 busy (carrying `retry_after`), any other
/// 400..599 upstream, and anything else — including 0 and 2xx — upstream as a 502. A blank
/// `message` becomes `Qoder returned HTTP {status}.`.
[[nodiscard]] qoder_error from_status(int status, std::string message, int retry_after = 0);

/// Converts the classification into the layer-independent `error`. The HTTP status kept on the
/// error is the classified one, so 401/403 both surface as 401 and an unclassifiable status as 502.
[[nodiscard]] error to_error(qoder_error const& value);

[[nodiscard]] constexpr bool is_busy_code(std::string_view code) noexcept {
  return code == "10605";
}

/// Qoder code 103 ("Duplicate request") means the same request is already queued or already
/// answered. Repeating it cannot help, and it must never be classified as an auth failure.
[[nodiscard]] constexpr bool is_duplicate_code(std::string_view code) noexcept {
  return code == "103";
}

/// True for both a 103 code and a "Duplicate request" message, because the SSE path keeps only
/// the classified status and message when it hands the event to the chat client.
[[nodiscard]] bool is_duplicate_error(std::string_view code, std::string_view message) noexcept;

/// Convenience overload for a classified Qoder error.
[[nodiscard]] bool is_duplicate_error(qoder_error const& value) noexcept;

/// Numeric codes the table treats as quota exhaustion: 110, 112–119 and 122.
[[nodiscard]] constexpr bool is_quota_code(int code) noexcept {
  return code == 110 || (code >= 112 && code <= 119) || code == 122;
}

/// Numeric codes the table treats as a bad request.
[[nodiscard]] constexpr bool is_request_code(int code) noexcept {
  return code == 406 || code == 416 || code == 430 || code == 80411 || code == 80412 ||
         (code >= 100400 && code <= 100403);
}

[[nodiscard]] constexpr bool is_auth_code(std::string_view code) noexcept {
  return code == "105";
}

} // namespace qoderz::qoder
