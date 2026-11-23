#include "common/json.hpp"
#include "common/net/http_client.hpp"
#include "protocol/body_codec.hpp"
#include "qoder/chat/client.hpp"
#include "support/async_test.hpp"
#include "unit/qoder/chat/chat_test_support.hpp"
#include "unit/qoder/stderr_capture.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace qoderz::test::chat;
using qoderz::test::fake_clock;
using qoderz::test::run_awaitable;
using qoderz::test::stderr_capture;

retry_options tight_budget(int attempts) {
  return retry_options{
      .m_total_budget = 30s, .m_max_attempts = attempts, .m_min_delay = 1ms, .m_max_delay = 1ms};
}

} // namespace

TEST_CASE("a queued 503 is retried with the identical signed request",
          "[qoder][chat][client][retry]") {
  asio::io_context io;
  scripted_stub stub;
  stub.m_answers.push_back(text_response(503, std::string{queue_body}));
  stub.m_answers.push_back(text_response(200, success_stream()));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, tight_budget(5)};

  auto request = make_request(server);
  request.m_body_json = qoderz::json{{"prompt", std::string(1024 * 1024, 'x')}}.dump();
  auto const expected_body = qoderz::protocol::body_encode(request.m_body_json);
  auto outcome = run_awaitable(io, open_and_drain(client, std::move(request)), 30s);
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(outcome->value().m_events.back().m_kind == chat_event_kind::stop);

  REQUIRE(stub.m_requests.size() == 2);
  auto const& first = stub.m_requests[0];
  auto const& second = stub.m_requests[1];
  CHECK(first.m_body == expected_body);
  CHECK(first.m_body == second.m_body);
  CHECK(*find_header(first.m_headers, "Authorization") ==
        *find_header(second.m_headers, "Authorization"));
  CHECK(*find_header(first.m_headers, "Cosy-Date") == *find_header(second.m_headers, "Cosy-Date"));
  CHECK(*find_header(first.m_headers, "Cosy-Key") == *find_header(second.m_headers, "Cosy-Key"));
}

TEST_CASE("a busy first event is retried before the stream is handed over",
          "[qoder][chat][client][retry]") {
  asio::io_context io;
  scripted_stub stub;
  stub.m_answers.push_back(text_response(200, "data: " + queue_frame() + "\n"));
  stub.m_answers.push_back(text_response(200, success_stream()));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, tight_budget(5)};

  auto outcome = run_awaitable(io, open_and_drain(client, make_request(server)), 30s);
  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(stub.m_requests.size() == 2);
  auto const& events = outcome->value().m_events;
  CHECK(events.front().m_text == "Hello");
  CHECK(events.back().m_kind == chat_event_kind::stop);
  CHECK(*find_header(stub.m_requests[0].m_headers, "Cosy-Date") ==
        *find_header(stub.m_requests[1].m_headers, "Cosy-Date"));
}

TEST_CASE("an auth failure as the first event is not retried", "[qoder][chat][client][retry]") {
  asio::io_context io;
  scripted_stub stub;
  stub.m_answers.push_back(
      text_response(200, "data: {\"statusCodeValue\":401,\"body\":{\"code\":\"105\","
                         "\"message\":\"token expired\"}}\n"));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, tight_budget(5)};

  auto outcome = run_awaitable(io, client.open(make_request(server)), 30s);
  REQUIRE(outcome.has_value());
  REQUIRE(!outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::auth);
  CHECK(outcome->error().m_http_status == 401);
  CHECK(stub.m_requests.size() == 1);
}

TEST_CASE("a busy first event that never clears ends as busy", "[qoder][chat][client][retry]") {
  asio::io_context io;
  scripted_stub stub;
  stub.m_answers.push_back(text_response(200, "data: " + queue_frame() + "\n"));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, tight_budget(3)};

  auto outcome = run_awaitable(io, client.open(make_request(server)), 30s);
  REQUIRE(outcome.has_value());
  REQUIRE(!outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::busy);
  CHECK(outcome->error().m_http_status == 503);
  CHECK(stub.m_requests.size() == 3);
}

TEST_CASE("the retry budget stops the queue even when attempts remain",
          "[qoder][chat][client][retry]") {
  asio::io_context io;
  scripted_stub stub;
  stub.m_answers.push_back(text_response(503, std::string{queue_body}));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{
      http, wrapper, time,
      retry_options{
          .m_total_budget = 0ms, .m_max_attempts = 11, .m_min_delay = 1ms, .m_max_delay = 1ms}};

  auto outcome = run_awaitable(io, client.open(make_request(server)), 30s);
  REQUIRE(outcome.has_value());
  REQUIRE(!outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::busy);
  CHECK(stub.m_requests.size() == 1);
}

TEST_CASE("the retry logs carry no credential, body or prompt", "[qoder][chat][client][retry]") {
  asio::io_context io;
  scripted_stub stub;
  stub.m_answers.push_back(text_response(503, std::string{queue_body}));
  stub.m_answers.push_back(text_response(200, success_stream()));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, tight_budget(5)};

  stderr_capture captured;
  REQUIRE(captured.available());
  auto outcome = run_awaitable(io, client.open(make_request(server)), 30s);
  auto const logged = captured.release();

  REQUIRE(outcome.has_value());
  REQUIRE(outcome->has_value());
  CHECK(logged.find("test-access-token-not-real") == std::string::npos);
  CHECK(logged.find("Bearer COSY") == std::string::npos);
  CHECK(logged.find("site=qoder") != std::string::npos);
  CHECK(logged.find(std::string(256, 'x')) == std::string::npos);
  CHECK(logged.find("uid=uid-test-not-real") != std::string::npos);
}

TEST_CASE("a duplicate request after a queue retry is busy, not auth",
          "[qoder][chat][client][retry]") {
  asio::io_context io;
  scripted_stub stub;
  stub.m_answers.push_back(text_response(200, "data: " + queue_frame() + "\n"));
  std::string const duplicate =
      R"({"statusCodeValue":403,"body":"{\"code\":\"103\",\"message\":\"Duplicate request\"}"})";
  stub.m_answers.push_back(text_response(200, "data: " + duplicate + "\n"));
  auto server = stub.make(io);
  REQUIRE(server.start().has_value());

  qoderz::net::http_client http{io};
  passthrough_wrapper wrapper;
  fake_clock time;
  chat_client client{http, wrapper, time, tight_budget(5)};

  auto request = make_request(server);
  request.m_body_json =
      qoderz::json{
          {"request_id", std::string{"rid-test-1"}},
          {"request_set_id", std::string{"rset-test-1"}},
          {"session_id", std::string{"session-test-1"}},
          {"business", qoderz::json{{"stage", std::string{"start"}}}},
      }
          .dump();

  auto outcome = run_awaitable(io, client.open(std::move(request)), 30s);
  REQUIRE(outcome.has_value());
  REQUIRE(!outcome->has_value());
  CHECK(outcome->error().m_kind == error_kind::busy);
  CHECK(outcome->error().m_http_status == 503);
  CHECK(stub.m_requests.size() == 2);
  CHECK(stub.m_requests[0].m_body == stub.m_requests[1].m_body);
}
