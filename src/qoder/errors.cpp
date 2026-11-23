#include "qoder/errors.hpp"

#include "common/json.hpp"
#include "common/strings.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace qoderz::qoder {
namespace {

constexpr int upstream_status = 502;
constexpr int busy_status = 503;
constexpr int auth_status = 401;
constexpr int quota_http_status = 429;
constexpr int request_status = 400;
constexpr int default_busy_retry_after = 30;
constexpr std::string_view upstream_response_marker = "error in upstream response";
constexpr std::string_view quota_marker = "quota";
constexpr std::string_view bom = "\xEF\xBB\xBF";

std::size_t utf8_length(std::string_view text) noexcept {
  std::size_t count = 0;
  for (auto const value : text) {
    if ((static_cast<unsigned char>(value) & 0xC0U) != 0x80U) {
      ++count;
    }
  }
  return count;
}

/// Truncation counts code points and cuts on a lead byte, so a message can never end in half a
/// character and a caller printing it cannot emit invalid UTF-8.
std::string truncate_utf8(std::string_view text, std::size_t limit) {
  if (utf8_length(text) <= limit) {
    return std::string{text};
  }
  std::size_t count = 0;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if ((static_cast<unsigned char>(text[index]) & 0xC0U) == 0x80U) {
      continue;
    }
    if (count == limit) {
      return std::string{text.substr(0, index)};
    }
    ++count;
  }
  return std::string{text};
}

std::string normalize_message(std::string_view text) {
  return truncate_utf8(strings::trim(text), max_error_message_length);
}

bool truthy(json const& value) noexcept {
  if (value.is_boolean()) {
    return value.get<bool>();
  }
  if (value.is_number()) {
    return value.get<double>() != 0.0;
  }
  if (value.is_string()) {
    auto const text = strings::trim(value.get_ref<std::string const&>());
    return strings::equals_ignore_case(text, "true") || text == "1";
  }
  return false;
}

/// The table compares `code` as text, so an integer becomes its decimal spelling and a fractional
/// or exponent value has no code at all instead of a rounded one that could land in a code set.
std::string read_code(json const& object) {
  auto const value = object.find("code");
  if (value == object.end()) {
    return {};
  }
  if (value->is_string()) {
    return value->get<std::string>();
  }
  if (value->is_number_integer()) {
    return std::to_string(value->get<long long>());
  }
  if (value->is_number_unsigned()) {
    return std::to_string(value->get<unsigned long long>());
  }
  return {};
}

std::optional<json> parse_object(std::string_view text) {
  auto trimmed = strings::trim(text);
  if (strings::starts_with(trimmed, bom)) {
    trimmed = strings::trim(trimmed.substr(bom.size()));
  }
  if (trimmed.empty() || trimmed.front() != '{') {
    return std::nullopt;
  }
  json parsed = json::parse(trimmed, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return std::nullopt;
  }
  return parsed;
}

std::string read_message(json const& object) {
  auto const value = object.find("message");
  if (value == object.end()) {
    return {};
  }
  if (value->is_string()) {
    return value->get<std::string>();
  }
  if (value->is_object()) {
    auto const inner = value->find("message");
    if (inner != value->end() && inner->is_string()) {
      return inner->get<std::string>();
    }
  }
  return {};
}

/// `details` is either an object or a JSON document inside a string, and its text sits under
/// `error.message` or `message`.
std::string read_details_message(json const& object) {
  auto const value = object.find("details");
  if (value == object.end()) {
    return {};
  }
  json detail;
  if (value->is_string()) {
    json parsed = json::parse(value->get_ref<std::string const&>(), nullptr, false);
    if (parsed.is_discarded()) {
      return {};
    }
    detail = std::move(parsed);
  } else if (value->is_object()) {
    detail = *value;
  } else {
    return {};
  }
  if (!detail.is_object()) {
    return {};
  }
  auto const error_object = detail.find("error");
  if (error_object != detail.end() && error_object->is_object()) {
    auto const message = error_object->find("message");
    if (message != error_object->end() && message->is_string()) {
      return message->get<std::string>();
    }
  }
  auto const message = detail.find("message");
  if (message != detail.end() && message->is_string()) {
    return message->get<std::string>();
  }
  return {};
}

struct queue_state {
  bool m_queued = false;
  bool m_unavailable = false;
  int m_retry_after = 0;
};

queue_state read_queue(json const& object) {
  queue_state state;
  auto const value = object.find("queue");
  if (value == object.end() || !value->is_object()) {
    return state;
  }
  for (auto const& [key, entry] : value->items()) {
    if (key == "isQueued" || key == "is_queued") {
      state.m_queued = state.m_queued || truthy(entry);
    } else if (key == "serviceAvailable" || key == "service_available") {
      state.m_unavailable = state.m_unavailable || !truthy(entry);
    } else if (key == "retryAfterSeconds" || key == "retry_after_seconds") {
      if (entry.is_number_integer()) {
        state.m_retry_after = static_cast<int>(entry.get<long long>());
      }
    }
  }
  return state;
}

struct envelope {
  std::string m_code;
  std::string m_message;
  queue_state m_queue;
};

/// A 401/403 whose `message` is itself a JSON document with code 10605 is how a queue-full error
/// arrives behind an auth status; the inner document is the real answer.
envelope unwrap_nested(json const& object, std::string code, std::string message) {
  envelope result{.m_code = std::move(code), .m_message = std::move(message), .m_queue = {}};
  result.m_queue = read_queue(object);
  if (!result.m_code.empty() && result.m_code != "401" && result.m_code != "403") {
    return result;
  }
  auto const inner = parse_object(result.m_message);
  if (!inner.has_value() || read_code(*inner) != "10605") {
    return result;
  }
  result.m_code = "10605";
  result.m_message = read_message(*inner);
  result.m_queue = read_queue(*inner);
  return result;
}

bool contains_ignore_case(std::string_view text, std::string_view needle) noexcept {
  if (needle.empty() || text.size() < needle.size()) {
    return false;
  }
  for (std::size_t start = 0; start + needle.size() <= text.size(); ++start) {
    if (strings::equals_ignore_case(text.substr(start, needle.size()), needle)) {
      return true;
    }
  }
  return false;
}

std::optional<int> numeric_code(std::string_view code) noexcept {
  if (code.empty()) {
    return std::nullopt;
  }
  long long value = 0;
  for (auto const digit : code) {
    if (digit < '0' || digit > '9') {
      return std::nullopt;
    }
    value = value * 10 + (digit - '0');
    if (value > 1'000'000'000LL) {
      return std::nullopt;
    }
  }
  return static_cast<int>(value);
}

qoder_error upstream_failure(int status, std::string code, std::string message) {
  return qoder_error{.m_kind = error_kind::upstream,
                     .m_status = status,
                     .m_code = std::move(code),
                     .m_message = std::move(message),
                     .m_retry_after = std::nullopt};
}

qoder_error classify(int http_status, envelope const& parsed) {
  auto const message = normalize_message(parsed.m_message);
  auto const code = parsed.m_code;

  if (is_busy_code(code) || parsed.m_queue.m_queued || parsed.m_queue.m_unavailable) {
    auto const retry_after =
        parsed.m_queue.m_retry_after > 0 ? parsed.m_queue.m_retry_after : default_busy_retry_after;
    return qoder_error{.m_kind = error_kind::busy,
                       .m_status = busy_status,
                       .m_code = code,
                       .m_message = message,
                       .m_retry_after = retry_after};
  }
  // 103 is Qoder's "Duplicate request": the same request is already queued or already answered.
  // It is busy, but the chat client must stop rather than resend, and it must never cool the
  // account down as if the token were rejected.
  if (is_duplicate_code(code) || contains_ignore_case(message, "duplicate request")) {
    return qoder_error{.m_kind = error_kind::busy,
                       .m_status = busy_status,
                       .m_code = code,
                       .m_message = message,
                       .m_retry_after = default_busy_retry_after};
  }
  if (is_auth_code(code)) {
    return qoder_error{.m_kind = error_kind::auth,
                       .m_status = auth_status,
                       .m_code = code,
                       .m_message = message,
                       .m_retry_after = std::nullopt};
  }
  if (auto const numeric = numeric_code(code); numeric.has_value()) {
    if (is_quota_code(*numeric)) {
      return qoder_error{.m_kind = error_kind::quota,
                         .m_status = quota_http_status,
                         .m_code = code,
                         .m_message = message,
                         .m_retry_after = std::nullopt};
    }
    if (is_request_code(*numeric)) {
      return qoder_error{.m_kind = error_kind::bad_request,
                         .m_status = request_status,
                         .m_code = code,
                         .m_message = message,
                         .m_retry_after = std::nullopt};
    }
  }
  if (contains_ignore_case(message, upstream_response_marker)) {
    return upstream_failure(upstream_status, code, message);
  }
  if (http_status == 401 || http_status == 403) {
    return qoder_error{.m_kind = error_kind::auth,
                       .m_status = auth_status,
                       .m_code = code,
                       .m_message = message,
                       .m_retry_after = std::nullopt};
  }
  if (http_status == quota_http_status || contains_ignore_case(message, quota_marker)) {
    return qoder_error{.m_kind = error_kind::quota,
                       .m_status = quota_http_status,
                       .m_code = code,
                       .m_message = message,
                       .m_retry_after = std::nullopt};
  }
  return upstream_failure(http_status, code, message);
}

std::string status_fallback(int status) {
  return "Qoder returned HTTP " + std::to_string(status) + ".";
}

} // namespace

bool is_duplicate_error(std::string_view code, std::string_view message) noexcept {
  return is_duplicate_code(code) || contains_ignore_case(message, "duplicate request");
}

bool is_duplicate_error(qoder_error const& value) noexcept {
  return is_duplicate_error(value.m_code, value.m_message);
}

qoder_error from_response(int http_status, std::string_view body) {
  if (http_status < 400 || http_status > 599) {
    return upstream_failure(upstream_status, {}, status_fallback(http_status));
  }

  auto const parsed_body = parse_object(body);
  if (!parsed_body.has_value()) {
    return from_status(http_status, {}, 0);
  }

  std::string combined = read_message(*parsed_body);
  auto const details = read_details_message(*parsed_body);
  if (!details.empty()) {
    if (!combined.empty()) {
      combined += ": ";
    }
    combined += details;
  }

  auto const parsed = unwrap_nested(*parsed_body, read_code(*parsed_body), std::move(combined));
  auto classified = classify(http_status, parsed);
  if (classified.m_message.empty()) {
    classified.m_message = status_fallback(http_status);
  }
  return classified;
}

qoder_error from_status(int status, std::string message, int retry_after) {
  auto const text = normalize_message(message);
  auto const fallback = status_fallback(status);
  auto const chosen = text.empty() ? fallback : text;

  if (status == request_status) {
    return qoder_error{.m_kind = error_kind::bad_request,
                       .m_status = request_status,
                       .m_code = {},
                       .m_message = chosen,
                       .m_retry_after = std::nullopt};
  }
  if (status == 401 || status == 403) {
    return qoder_error{.m_kind = error_kind::auth,
                       .m_status = auth_status,
                       .m_code = {},
                       .m_message = chosen,
                       .m_retry_after = std::nullopt};
  }
  if (status == quota_http_status) {
    return qoder_error{.m_kind = error_kind::quota,
                       .m_status = quota_http_status,
                       .m_code = {},
                       .m_message = chosen,
                       .m_retry_after = std::nullopt};
  }
  if (status == busy_status) {
    return qoder_error{.m_kind = error_kind::busy,
                       .m_status = busy_status,
                       .m_code = {},
                       .m_message = chosen,
                       .m_retry_after = retry_after > 0 ? retry_after : default_busy_retry_after};
  }
  if (status >= 400 && status <= 599) {
    return upstream_failure(status, {}, chosen);
  }
  return upstream_failure(upstream_status, {}, fallback);
}

error to_error(qoder_error const& value) {
  error converted = make_error(value.m_kind, value.m_message, value.m_code);
  converted.m_http_status = value.m_status;
  converted.m_retry_after = value.m_retry_after;
  return converted;
}

} // namespace qoderz::qoder
