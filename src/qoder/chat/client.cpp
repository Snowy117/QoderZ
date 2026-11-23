#include "qoder/chat/client.hpp"

#include "common/json.hpp"
#include "common/log.hpp"
#include "common/net/http_types.hpp"
#include "qoder/chat/request.hpp"
#include "qoder/chat/stream.hpp"
#include "qoder/errors.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace qoderz::qoder::chat {
namespace {

namespace asio = boost::asio;

constexpr std::size_t max_error_body = 1024 * 1024;
constexpr int default_queue_seconds = 30;

std::chrono::seconds to_seconds(std::chrono::milliseconds value) {
  return std::chrono::duration_cast<std::chrono::seconds>(value);
}

constexpr std::size_t max_logged_message = 240;

std::string truncate_utf8(std::string_view text, std::size_t limit) {
  if (text.size() <= limit) {
    return std::string{text};
  }
  std::size_t cut = limit;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) {
    --cut;
  }
  return std::string{text.substr(0, cut)};
}

/// Diagnostics only: the ids the upstream request was built with, never its prompt text.
struct request_identity {
  std::string m_request_id;
  std::string m_request_set_id;
  std::string m_session_id;
  std::string m_stage;
};

std::string read_string(json const& object, std::string_view key) {
  auto const found = object.find(std::string{key});
  if (found == object.end() || !found->is_string()) {
    return {};
  }
  return found->get<std::string>();
}

request_identity read_identity(std::string const& body) {
  request_identity identity;
  json const parsed = json::parse(body, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return identity;
  }
  identity.m_request_id = read_string(parsed, "request_id");
  identity.m_request_set_id = read_string(parsed, "request_set_id");
  identity.m_session_id = read_string(parsed, "session_id");
  auto const business = parsed.find("business");
  if (business != parsed.end() && business->is_object()) {
    identity.m_stage = read_string(*business, "stage");
  }
  return identity;
}

/// Reads a failed response's body, capped, so the classifier can see the queue state inside it.
/// Hitting the cap is not an error: the status still classifies whatever arrived.
boost::asio::awaitable<result<std::string>> read_error_body(net::http_stream& body) {
  std::string text;
  for (;;) {
    if (text.size() >= max_error_body) {
      co_return text;
    }
    auto chunk = co_await body.read_some();
    if (!chunk) {
      co_return std::unexpected(std::move(chunk.error()));
    }
    if (!chunk->has_value()) {
      co_return text;
    }
    auto remaining = max_error_body - text.size();
    auto piece = std::move(**chunk);
    if (piece.size() > remaining) {
      piece.resize(remaining);
      text.append(piece);
      co_return text;
    }
    text.append(piece);
  }
}

error event_error(protocol::chat_event const& event) {
  return to_error(from_status(event.m_status, event.m_error, event.m_retry_after));
}

} // namespace

chat_client::chat_client(net::http_client& http, protocol::rsa_key_wrapper const& wrapper,
                         clock& time, retry_options options)
    : m_http{http}, m_wrapper{wrapper}, m_time{time}, m_options{options} {}

boost::asio::awaitable<result<void>> chat_client::wait(std::chrono::milliseconds delay) {
  asio::steady_timer timer{co_await asio::this_coro::executor, delay};
  auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
  if (ec == asio::error::operation_aborted) {
    co_return std::unexpected(make_error(error_kind::canceled, "The chat request was cancelled."));
  }
  if (ec) {
    co_return std::unexpected(
        make_error(error_kind::io, "Waiting to retry the chat request failed."));
  }
  co_return result<void>{};
}

boost::asio::awaitable<result<std::unique_ptr<chat_stream>>>
chat_client::open(upstream_request request) {
  auto const identity = read_identity(request.m_body_json);
  auto signed_request = build_signed_request(request, m_wrapper, m_time.now_ms() / 1000);
  std::string{}.swap(request.m_body_json);
  if (!signed_request) {
    co_return std::unexpected(std::move(signed_request.error()));
  }

  net::http_request message;
  message.m_method = "POST";
  message.m_url = std::move(signed_request->m_url);
  message.m_headers = std::move(signed_request->m_headers);
  message.m_body = std::move(signed_request->m_wire_body);

  log(log_level::debug, "chat request identity",
      {{"site", request.m_site.m_site_id},
       {"uid", request.m_account.m_uid},
       {"request_id", identity.m_request_id},
       {"request_set_id", identity.m_request_set_id},
       {"session_id", identity.m_session_id},
       {"stage", identity.m_stage}});

  auto const started = std::chrono::steady_clock::now();
  auto const deadline = started + m_options.m_total_budget;
  int attempt = 0;
  std::optional<error> last_busy;

  while (attempt < m_options.m_max_attempts) {
    ++attempt;
    auto response = co_await m_http.open(message);
    if (!response) {
      co_return std::unexpected(std::move(response.error()));
    }

    int const status = (*response)->status();
    if (status < 200 || status > 299) {
      auto body = co_await read_error_body(**response);
      if (!body) {
        co_return std::unexpected(std::move(body.error()));
      }
      auto failed = to_error(from_response(status, *body));
      log(log_level::warn, "chat request failed",
          {{"site", request.m_site.m_site_id},
           {"uid", request.m_account.m_uid},
           {"status", std::to_string(status)},
           {"code", failed.m_code},
           {"message", truncate_utf8(failed.m_message, max_logged_message)},
           {"request_id", identity.m_request_id},
           {"request_set_id", identity.m_request_set_id},
           {"session_id", identity.m_session_id},
           {"stage", identity.m_stage},
           {"attempt", std::to_string(attempt)}});
      if (failed.m_kind != error_kind::busy ||
          is_duplicate_error(failed.m_code, failed.m_message)) {
        co_return std::unexpected(std::move(failed));
      }
      last_busy = failed;
    } else {
      auto stream = std::make_unique<sse_chat_stream>(std::move(*response));
      auto first = co_await stream->next();
      if (!first) {
        co_return std::unexpected(std::move(first.error()));
      }
      // Kept on purpose even though today it cannot be taken: `sse_chat_stream::next()` sets its
      // end-of-stream flag when the body ends and only reports "nothing left" after `finish()` has
      // produced the closing stop or error, so the first event of a live response is never absent.
      // It is the correct answer if that EOF contract ever changes, and without it a stream that
      // ended before its first event would fall through to the error peek below and dereference an
      // empty optional. Returning the drained stream is what the caller already handles: the
      // response is written as an empty completion.
      if (!first->has_value()) {
        co_return std::unique_ptr<chat_stream>{std::move(stream)};
      }

      auto const& event = **first;
      if (event.m_kind != protocol::chat_event_kind::error) {
        stream->requeue(std::move(**first));
        co_return std::unique_ptr<chat_stream>{std::move(stream)};
      }

      auto failed = event_error(event);
      log(log_level::warn, "chat stream failed before its first event",
          {{"site", request.m_site.m_site_id},
           {"uid", request.m_account.m_uid},
           {"status", std::to_string(failed.m_http_status)},
           {"code", failed.m_code},
           {"message", truncate_utf8(failed.m_message, max_logged_message)},
           {"request_id", identity.m_request_id},
           {"request_set_id", identity.m_request_set_id},
           {"session_id", identity.m_session_id},
           {"stage", identity.m_stage},
           {"attempt", std::to_string(attempt)}});
      if (failed.m_kind != error_kind::busy ||
          is_duplicate_error(failed.m_code, failed.m_message)) {
        co_return std::unexpected(std::move(failed));
      }
      last_busy = failed;
    }

    if (attempt >= m_options.m_max_attempts) {
      break;
    }

    auto const queue_seconds = last_busy->m_retry_after.value_or(0) > 0 ? *last_busy->m_retry_after
                                                                        : default_queue_seconds;
    auto delay = std::chrono::milliseconds{std::chrono::seconds{queue_seconds}};
    if (delay < m_options.m_min_delay) {
      delay = m_options.m_min_delay;
    }
    if (delay > m_options.m_max_delay) {
      delay = m_options.m_max_delay;
    }
    auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
      break;
    }
    if (delay > remaining) {
      delay = remaining;
    }
    if (delay <= std::chrono::milliseconds::zero()) {
      break;
    }

    log(log_level::debug, "chat request queued upstream",
        {{"site", request.m_site.m_site_id},
         {"uid", request.m_account.m_uid},
         {"attempt", std::to_string(attempt)},
         {"wait_seconds", std::to_string(to_seconds(delay).count())},
         {"code", last_busy->m_code},
         {"message", truncate_utf8(last_busy->m_message, max_logged_message)},
         {"request_id", identity.m_request_id},
         {"request_set_id", identity.m_request_set_id},
         {"stage", identity.m_stage}});
    auto waited = co_await wait(delay);
    if (!waited) {
      co_return std::unexpected(std::move(waited.error()));
    }
  }

  if (last_busy.has_value()) {
    log(log_level::warn, "chat request gave up on the upstream queue",
        {{"site", request.m_site.m_site_id},
         {"uid", request.m_account.m_uid},
         {"attempt", std::to_string(attempt)},
         {"code", last_busy->m_code},
         {"message", truncate_utf8(last_busy->m_message, max_logged_message)},
         {"request_id", identity.m_request_id},
         {"request_set_id", identity.m_request_set_id},
         {"stage", identity.m_stage}});
    co_return std::unexpected(std::move(*last_busy));
  }
  co_return std::unexpected(make_error(error_kind::busy, "The upstream queue never cleared."));
}

} // namespace qoderz::qoder::chat
