#include "qoder/chat/stream.hpp"

#include "common/codec/random.hpp"
#include "common/json.hpp"
#include "common/log.hpp"
#include "common/strings.hpp"
#include "qoder/errors.hpp"

#include <boost/asio/use_awaitable.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qoderz::qoder::chat {
namespace {

constexpr std::string_view data_prefix = "data:";
constexpr std::string_view done_marker = "[DONE]";
constexpr int ok_status = 200;
constexpr int error_status = 502;
constexpr std::string_view unnamed_tool_message =
    "the model's native tool call has no name; it is not sent on as a tool";
/// The reference parser caps nesting at 64 levels; deeper input is rejected before nlohmann sees
/// it, because recursion would overflow the stack.
constexpr std::size_t max_json_depth = 64;

bool exceeds_json_depth(std::string_view text) noexcept {
  std::size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (char const character : text) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        in_string = false;
      }
      continue;
    }
    if (character == '"') {
      in_string = true;
    } else if (character == '{' || character == '[') {
      if (++depth > max_json_depth) {
        return true;
      }
    } else if ((character == '}' || character == ']') && depth > 0) {
      --depth;
    }
  }
  return false;
}

json parsed_json(std::string_view text) {
  json document;
  if (exceeds_json_depth(text)) {
    return json(json::value_t::discarded);
  }
  try {
    document = json::parse(text, nullptr, false);
  } catch (json::exception const&) {
    return json(json::value_t::discarded);
  }
  if (document.is_discarded()) {
    return json(json::value_t::discarded);
  }
  return document;
}

/// The string member `name` of `object`, or an empty string when it is absent or not a string.
std::string read_string(json const& object, std::string_view name) {
  if (!object.is_object()) {
    return {};
  }
  auto const found = object.find(std::string{name});
  if (found == object.end() || !found->is_string()) {
    return {};
  }
  return found->get<std::string>();
}

json const* find_member(json const& object, std::string_view name) {
  if (!object.is_object()) {
    return nullptr;
  }
  auto const found = object.find(std::string{name});
  return found == object.end() ? nullptr : &*found;
}

bool read_int(json const& value, int& out) noexcept {
  if (value.is_number_integer()) {
    out = static_cast<int>(value.get<long long>());
    return true;
  }
  if (value.is_number_unsigned()) {
    out = static_cast<int>(value.get<unsigned long long>());
    return true;
  }
  if (value.is_number_float()) {
    auto const number = value.get<double>();
    if (number == static_cast<double>(static_cast<int>(number))) {
      out = static_cast<int>(number);
      return true;
    }
  }
  return false;
}

/// A strict integer, so `"abc"` is not a status and `"0200"` is still 200 rather than rubbish.
bool parse_integer(std::string_view text, int& out) noexcept {
  auto const trimmed = strings::trim(text);
  if (trimmed.empty() || trimmed.size() > 9) {
    return false;
  }
  int value = 0;
  for (char const digit : trimmed) {
    if (digit < '0' || digit > '9') {
      return false;
    }
    value = value * 10 + (digit - '0');
  }
  out = value;
  return true;
}

int as_int(json const& value, int fallback) noexcept {
  int out = fallback;
  return read_int(value, out) ? out : fallback;
}

/// Diagnostics only: the upstream error frame, capped, so the queue flags inside it are visible.
std::string error_frame_excerpt(std::string_view text) {
  constexpr std::size_t limit = 512;
  if (text.size() <= limit) {
    return std::string{text};
  }
  std::size_t cut = limit;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) {
    --cut;
  }
  return std::string{text.substr(0, cut)};
}

} // namespace

bool stream_parser::append(std::vector<protocol::chat_event>& events,
                           std::vector<protocol::chat_event> produced) {
  for (auto& event : produced) {
    if (event.m_kind == protocol::chat_event_kind::tool_start) {
      event.m_index = m_next_tool_index++;
      m_saw_tool = true;
    }
    if (event.m_kind == protocol::chat_event_kind::error) {
      events.push_back(std::move(event));
      m_done = true;
      return false;
    }
    events.push_back(std::move(event));
  }
  return true;
}

void stream_parser::apply_tool_call(int upstream_index, std::string id, std::string name,
                                    std::string arguments,
                                    std::vector<protocol::chat_event>& events, bool& failed) {
  auto& state = m_tools[upstream_index];
  if (state.m_id.empty()) {
    state.m_id = std::move(id);
  }
  if (state.m_name.empty()) {
    state.m_name = std::move(name);
  }
  if (!arguments.empty()) {
    state.m_arguments.append(arguments);
  }

  if (state.m_started) {
    if (!arguments.empty()) {
      protocol::chat_event event;
      event.m_kind = protocol::chat_event_kind::tool_arguments;
      event.m_index = state.m_output_index;
      event.m_arguments = std::move(arguments);
      events.push_back(std::move(event));
    }
    return;
  }

  if (state.m_name.empty()) {
    return;
  }

  // The name can arrive after the arguments, so the buffered text tools have to be emitted first:
  // their calls come before this one in the model's answer, and they share its index counter.
  if (!append(events, m_splitter.flush())) {
    failed = true;
    return;
  }

  if (state.m_id.empty()) {
    state.m_id = "call_" + codec::random_hex(16);
  }
  state.m_output_index = m_next_tool_index++;
  state.m_started = true;
  m_saw_tool = true;

  protocol::chat_event event;
  event.m_kind = protocol::chat_event_kind::tool_start;
  event.m_index = state.m_output_index;
  event.m_id = state.m_id;
  event.m_name = state.m_name;
  event.m_arguments = state.m_arguments;
  events.push_back(std::move(event));
}

void stream_parser::record_usage(json const& chunk) {
  auto const* usage = find_member(chunk, "usage");
  if (usage == nullptr || !usage->is_object()) {
    return;
  }

  protocol::token_usage recorded;
  int value = 0;
  if (auto const* prompt = find_member(*usage, "prompt_tokens");
      prompt != nullptr && read_int(*prompt, value)) {
    recorded.m_prompt = value;
  }
  if (auto const* completion = find_member(*usage, "completion_tokens");
      completion != nullptr && read_int(*completion, value)) {
    recorded.m_completion = value;
  }
  if (auto const* total = find_member(*usage, "total_tokens");
      total != nullptr && read_int(*total, value)) {
    recorded.m_total = value;
  }
  if (auto const* cached = find_member(*usage, "cached_tokens");
      cached != nullptr && read_int(*cached, value)) {
    recorded.m_cached = value;
  } else if (auto const* details = find_member(*usage, "prompt_tokens_details");
             details != nullptr && details->is_object()) {
    if (auto const* detail_cached = find_member(*details, "cached_tokens");
        detail_cached != nullptr && read_int(*detail_cached, value)) {
      recorded.m_cached = value;
    }
  }
  if (auto const* details = find_member(*usage, "completion_tokens_details");
      details != nullptr && details->is_object()) {
    if (auto const* reasoning = find_member(*details, "reasoning_tokens");
        reasoning != nullptr && read_int(*reasoning, value)) {
      recorded.m_reasoning = value;
    }
  }
  if (recorded.m_total == 0) {
    recorded.m_total = recorded.m_prompt + recorded.m_completion;
  }
  m_usage = recorded;
}

void stream_parser::record_finish_reason(json const& choice) {
  auto const reason = read_string(choice, "finish_reason");
  if (!reason.empty()) {
    m_finish_reason = reason;
  }
}

std::vector<protocol::chat_event> stream_parser::apply_chunk(json const& chunk) {
  std::vector<protocol::chat_event> events;
  auto const* choices = find_member(chunk, "choices");
  if (choices != nullptr && choices->is_array() && !choices->empty()) {
    auto const& choice = choices->front();
    record_finish_reason(choice);
    auto const* delta = find_member(choice, "delta");
    if (delta != nullptr && delta->is_object()) {
      if (auto const reasoning = read_string(*delta, "reasoning_content"); !reasoning.empty()) {
        protocol::chat_event event;
        event.m_kind = protocol::chat_event_kind::reasoning;
        event.m_text = reasoning;
        events.push_back(std::move(event));
      }
      if (auto const content = read_string(*delta, "content"); !content.empty()) {
        if (!append(events, m_splitter.feed(content))) {
          return events;
        }
      }
      if (auto const* calls = find_member(*delta, "tool_calls");
          calls != nullptr && calls->is_array()) {
        bool failed = false;
        for (auto const& call : *calls) {
          if (!call.is_object()) {
            continue;
          }
          auto const* function = find_member(call, "function");
          auto name = function != nullptr ? read_string(*function, "name") : std::string{};
          if (name.empty()) {
            name = read_string(call, "name");
          }
          auto arguments =
              function != nullptr ? read_string(*function, "arguments") : std::string{};
          if (arguments.empty()) {
            arguments = read_string(call, "arguments");
          }
          auto const* index = find_member(call, "index");
          auto const upstream_index = index != nullptr ? as_int(*index, 0) : 0;
          apply_tool_call(upstream_index, read_string(call, "id"), std::move(name),
                          std::move(arguments), events, failed);
          if (failed) {
            return events;
          }
        }
      }
    }
  }

  // A tail frame carries `usage` with an empty `choices` array, so usage is read regardless of what
  // the choices above contributed.
  record_usage(chunk);
  return events;
}

std::vector<protocol::chat_event> stream_parser::apply_body_string(std::string const& text) {
  auto const trimmed = strings::trim(text);
  if (trimmed.empty()) {
    return {};
  }
  if (trimmed == done_marker) {
    return complete();
  }
  auto parsed = parsed_json(trimmed);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return {};
  }
  return apply_chunk(parsed);
}

std::vector<protocol::chat_event> stream_parser::failure_event(int status,
                                                               std::string const& text) {
  log(log_level::debug, "chat upstream error frame",
      {{"status", std::to_string(status)}, {"body", error_frame_excerpt(text)}});
  auto const error = to_error(from_response(status, text));
  protocol::chat_event event;
  event.m_kind = protocol::chat_event_kind::error;
  event.m_status = error.m_http_status;
  event.m_error = error.m_message;
  event.m_retry_after = error.m_retry_after.value_or(0);
  m_done = true;
  return {std::move(event)};
}

std::vector<protocol::chat_event> stream_parser::apply_envelope(std::string_view payload) {
  auto envelope = parsed_json(payload);
  if (envelope.is_discarded() || !envelope.is_object()) {
    return {};
  }

  // `statusCodeValue` has three states, not two: absent is a normal frame that happens to carry
  // no status, present-but-unreadable is a failure with status 0, and present-and-readable fails
  // only when it is not 200 — so 201 and 0 are failures too.
  int status = 0;
  bool failure = false;
  if (auto const* value = find_member(envelope, "statusCodeValue"); value != nullptr) {
    if (value->is_string()) {
      failure = !parse_integer(value->get_ref<std::string const&>(), status);
    } else {
      failure = !read_int(*value, status);
    }
    if (failure) {
      status = 0;
    } else {
      failure = status != ok_status;
    }
  }

  if (failure) {
    std::string text;
    if (auto const* body = find_member(envelope, "body"); body != nullptr) {
      if (body->is_string()) {
        text = body->get<std::string>();
      }
      if (text.empty() && body->is_object()) {
        try {
          text = body->dump(-1, ' ', false, json::error_handler_t::strict);
        } catch (json::exception const&) {
          text.clear();
        }
      }
    }
    if (text.empty()) {
      text = envelope.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    return failure_event(status, text);
  }

  auto const* body = find_member(envelope, "body");
  if (body == nullptr) {
    return {};
  }
  if (body->is_string()) {
    return apply_body_string(body->get_ref<std::string const&>());
  }
  if (body->is_object()) {
    return apply_chunk(*body);
  }
  return {};
}

std::vector<protocol::chat_event> stream_parser::complete() {
  std::vector<protocol::chat_event> events;
  if (m_done) {
    return events;
  }
  if (!append(events, m_splitter.flush())) {
    return events;
  }
  for (auto const& [index, state] : m_tools) {
    (void)index;
    if (!state.m_started && !state.m_arguments.empty()) {
      protocol::chat_event event;
      event.m_kind = protocol::chat_event_kind::error;
      event.m_status = error_status;
      event.m_error = std::string{unnamed_tool_message};
      events.push_back(std::move(event));
      m_done = true;
      return events;
    }
  }

  protocol::chat_event stop;
  stop.m_kind = protocol::chat_event_kind::stop;
  if (m_saw_tool) {
    stop.m_reason = "tool_calls";
  } else if (strings::equals_ignore_case(m_finish_reason, "length")) {
    stop.m_reason = "length";
  } else {
    stop.m_reason = "stop";
  }
  stop.m_usage = m_usage;
  events.push_back(std::move(stop));
  m_done = true;
  return events;
}

std::vector<protocol::chat_event> stream_parser::on_line(std::string_view line) {
  if (m_done) {
    return {};
  }

  auto const trimmed = strings::trim(line);
  if (trimmed.size() <= data_prefix.size() || !strings::starts_with(trimmed, data_prefix)) {
    return {};
  }
  return apply_envelope(strings::trim(trimmed.substr(data_prefix.size())));
}

std::vector<protocol::chat_event> stream_parser::finish() {
  return complete();
}

sse_chat_stream::sse_chat_stream(std::unique_ptr<net::http_stream> body)
    : m_body{std::move(body)} {}

void sse_chat_stream::requeue(protocol::chat_event event) {
  m_pending.push_front(std::move(event));
}

boost::asio::awaitable<result<std::optional<protocol::chat_event>>> sse_chat_stream::next() {
  for (;;) {
    if (!m_pending.empty()) {
      protocol::chat_event event = std::move(m_pending.front());
      m_pending.pop_front();
      co_return std::optional<protocol::chat_event>{std::move(event)};
    }
    if (m_finished) {
      co_return std::optional<protocol::chat_event>{};
    }

    if (m_eof) {
      auto tail = m_reader.finish();
      if (!tail) {
        co_return std::unexpected(std::move(tail.error()));
      }
      for (auto const& line : *tail) {
        auto events = m_parser.on_line(line);
        for (auto& event : events) {
          m_pending.push_back(std::move(event));
        }
      }
      for (auto& event : m_parser.finish()) {
        m_pending.push_back(std::move(event));
      }
      m_finished = true;
      continue;
    }

    auto chunk = co_await m_body->read_some();
    if (!chunk) {
      co_return std::unexpected(std::move(chunk.error()));
    }
    if (!chunk->has_value()) {
      m_eof = true;
      continue;
    }
    auto lines = m_reader.feed(chunk->value());
    if (!lines) {
      co_return std::unexpected(std::move(lines.error()));
    }
    for (auto const& line : *lines) {
      for (auto& event : m_parser.on_line(line)) {
        m_pending.push_back(std::move(event));
      }
    }
    if (m_parser.done()) {
      m_finished = true;
    }
  }
}

} // namespace qoderz::qoder::chat
