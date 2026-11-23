#include "common/error.hpp"
#include "qoder/errors.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>

using qoderz::error_kind;
using qoderz::qoder::from_response;
using qoderz::qoder::from_status;
using qoderz::qoder::qoder_error;
using qoderz::qoder::to_error;

namespace {

qoder_error classify(int status, std::string_view body) {
  return from_response(status, body);
}

} // namespace

TEST_CASE("a status outside 400..599 is an upstream 502 before the body is read",
          "[qoder][errors]") {
  auto const ok_body = classify(200, R"({"code":"105","message":"unauthorized"})");
  CHECK(ok_body.m_kind == error_kind::upstream);
  CHECK(ok_body.m_status == 502);

  auto const redirect = classify(302, "");
  CHECK(redirect.m_kind == error_kind::upstream);
  CHECK(redirect.m_status == 502);

  auto const zero = classify(0, R"({"code":"10605"})");
  CHECK(zero.m_kind == error_kind::upstream);
  CHECK(zero.m_status == 502);
}

TEST_CASE("10605 and the queue flags are busy with a retry hint", "[qoder][errors]") {
  auto const code_as_string = classify(503, R"({"code":"10605","message":"busy"})");
  CHECK(code_as_string.m_kind == error_kind::busy);
  CHECK(code_as_string.m_status == 503);
  CHECK(code_as_string.m_retry_after == 30);

  auto const code_as_number = classify(503, R"({"code":10605,"message":"queued"})");
  CHECK(code_as_number.m_kind == error_kind::busy);
  CHECK(code_as_number.m_retry_after == 30);

  auto const queued = classify(400, R"({"queue":{"isQueued":true,"retryAfterSeconds":1}})");
  CHECK(queued.m_kind == error_kind::busy);
  CHECK(queued.m_retry_after == 1);

  auto const unavailable = classify(400, R"({"queue":{"serviceAvailable":false}})");
  CHECK(unavailable.m_kind == error_kind::busy);
  CHECK(unavailable.m_retry_after == 30);

  auto const snake = classify(400, R"({"queue":{"is_queued":true,"retry_after_seconds":7}})");
  CHECK(snake.m_kind == error_kind::busy);
  CHECK(snake.m_retry_after == 7);

  auto const available = classify(400, R"({"queue":{"serviceAvailable":true}})");
  CHECK(available.m_kind == error_kind::upstream);
}

TEST_CASE("105 is an auth failure whatever the status says", "[qoder][errors]") {
  auto const string_code = classify(500, R"({"code":"105","message":"token expired"})");
  CHECK(string_code.m_kind == error_kind::auth);
  CHECK(string_code.m_status == 401);
  CHECK_FALSE(string_code.m_retry_after.has_value());

  auto const numeric_code = classify(400, R"({"code":105})");
  CHECK(numeric_code.m_kind == error_kind::auth);
}

TEST_CASE("the quota code set classifies as quota", "[qoder][errors]") {
  for (int const code : {110, 112, 113, 114, 115, 116, 117, 118, 119, 122}) {
    auto const classified =
        classify(400, std::string{"{\"code\":"} + std::to_string(code) + ",\"message\":\"no\"}");
    INFO("code " << code);
    CHECK(classified.m_kind == error_kind::quota);
    CHECK(classified.m_status == 429);
  }
  for (int const code : {111, 120, 121, 123}) {
    auto const classified =
        classify(400, std::string{"{\"code\":"} + std::to_string(code) + ",\"message\":\"no\"}");
    INFO("code " << code);
    CHECK(classified.m_kind == error_kind::upstream);
  }
}

TEST_CASE("the request code set classifies as a bad request", "[qoder][errors]") {
  for (int const code : {406, 416, 430, 80411, 80412, 100400, 100401, 100402, 100403}) {
    auto const classified = classify(500, std::string{"{\"code\":"} + std::to_string(code) + "}");
    INFO("code " << code);
    CHECK(classified.m_kind == error_kind::bad_request);
    CHECK(classified.m_status == 400);
  }
  for (int const code : {405, 407, 100399, 100404}) {
    auto const classified = classify(400, std::string{"{\"code\":"} + std::to_string(code) + "}");
    INFO("code " << code);
    CHECK(classified.m_kind == error_kind::upstream);
  }
}

TEST_CASE("the upstream marker and 429-like messages classify by message", "[qoder][errors]") {
  auto const marker = classify(400, R"({"message":"Error in upstream response: boom"})");
  CHECK(marker.m_kind == error_kind::upstream);
  CHECK(marker.m_status == 502);

  auto const quota_word = classify(400, R"({"message":"your Quota is exhausted"})");
  CHECK(quota_word.m_kind == error_kind::quota);
  CHECK(quota_word.m_status == 429);

  auto const http_429 = classify(429, R"({"message":"slow down"})");
  CHECK(http_429.m_kind == error_kind::quota);
}

TEST_CASE("401 and 403 without a code are auth failures", "[qoder][errors]") {
  for (int const status : {401, 403}) {
    auto const classified = classify(status, R"({"message":"nope"})");
    INFO("status " << status);
    CHECK(classified.m_kind == error_kind::auth);
    CHECK(classified.m_status == 401);
  }
  auto const empty = classify(401, "");
  CHECK(empty.m_kind == error_kind::auth);
  CHECK(empty.m_message == "Qoder returned HTTP 401.");
}

TEST_CASE("103 and duplicate-request messages are busy, never auth", "[qoder][errors]") {
  auto const duplicate = classify(403, R"({"code":"103","message":"Duplicate request"})");
  CHECK(duplicate.m_kind == error_kind::busy);
  CHECK(duplicate.m_status == 503);
  CHECK(duplicate.m_code == "103");
  CHECK(duplicate.m_retry_after == 30);
  CHECK(qoderz::qoder::is_duplicate_error(duplicate));

  auto const message_only = classify(403, R"({"message":"Duplicate request"})");
  CHECK(message_only.m_kind == error_kind::busy);
  CHECK(message_only.m_status == 503);

  auto const upper_case = classify(403, R"({"message":"DUPLICATE REQUEST"})");
  CHECK(upper_case.m_kind == error_kind::busy);

  auto const auth_status = classify(401, R"({"code":"103"})");
  CHECK(auth_status.m_kind == error_kind::busy);
  CHECK(auth_status.m_status == 503);

  auto const bare = from_status(503, "Duplicate request");
  CHECK(bare.m_kind == error_kind::busy);

  CHECK(qoderz::qoder::is_duplicate_code("103"));
  CHECK_FALSE(qoderz::qoder::is_duplicate_code("104"));
  CHECK_FALSE(qoderz::qoder::is_duplicate_error(classify(403, R"({"message":"nope"})")));
}

TEST_CASE("a 10605 nested in a 401 message is still busy", "[qoder][errors]") {
  auto const nested =
      classify(401, R"({"code":"401","message":"{\"code\":\"10605\",\"message\":\"queued\",)"
                    R"(\"queue\":{\"isQueued\":true,\"retryAfterSeconds\":15}}"})");
  CHECK(nested.m_kind == error_kind::busy);
  CHECK(nested.m_status == 503);
  CHECK(nested.m_retry_after == 15);
  CHECK(nested.m_code == "10605");

  auto const nested_number = classify(
      403, R"({"code":"403","message":"{\"code\":10605,\"queue\":{\"serviceAvailable\":false}}"})");
  CHECK(nested_number.m_kind == error_kind::busy);

  auto const not_nested = classify(401, R"({"code":"401","message":"plain text"})");
  CHECK(not_nested.m_kind == error_kind::auth);
}

TEST_CASE("a BOM and an unparsable body are tolerated", "[qoder][errors]") {
  auto const bom = classify(503, "\xEF\xBB\xBF{\"code\":\"10605\",\"message\":\"busy\"}");
  CHECK(bom.m_kind == error_kind::busy);

  auto const garbage = classify(500, "not json at all");
  CHECK(garbage.m_kind == error_kind::upstream);
  CHECK(garbage.m_status == 500);
  CHECK(garbage.m_message == "Qoder returned HTTP 500.");

  auto const array_body = classify(400, R"([1,2,3])");
  CHECK(array_body.m_kind == error_kind::bad_request);
  CHECK(array_body.m_message == "Qoder returned HTTP 400.");
}

TEST_CASE("a details document contributes its error message", "[qoder][errors]") {
  auto const as_string =
      classify(400, R"({"message":"outer","details":"{\"error\":{\"message\":\"inner\"}}"})");
  CHECK(as_string.m_message == "outer: inner");

  auto const as_object =
      classify(400, R"({"message":"outer","details":{"error":{"message":"deep"}}})");
  CHECK(as_object.m_message == "outer: deep");

  auto const only_details = classify(400, R"({"details":{"message":"just details"}})");
  CHECK(only_details.m_message == "just details");
}

TEST_CASE("messages are trimmed and truncated", "[qoder][errors]") {
  auto const padded = classify(400, R"({"message":"   spaced out   "})");
  CHECK(padded.m_message == "spaced out");

  std::string const long_message(5000, 'a');
  auto const truncated = classify(400, "{\"message\":\"" + long_message + "\"}");
  CHECK(truncated.m_message.size() == qoderz::qoder::max_error_message_length);

  std::string const multibyte = "\"\u00e9\u00e9\u00e9\"";
  auto const utf8 = classify(400, "{\"message\":" + multibyte + "}");
  CHECK(utf8.m_message == "\u00e9\u00e9\u00e9");
}

TEST_CASE("from_status classifies a bare status", "[qoder][errors]") {
  CHECK(from_status(400, "bad").m_kind == error_kind::bad_request);
  CHECK(from_status(400, "").m_message == "Qoder returned HTTP 400.");
  CHECK(from_status(401, "no").m_kind == error_kind::auth);
  CHECK(from_status(403, "no").m_kind == error_kind::auth);
  CHECK(from_status(429, "no").m_kind == error_kind::quota);
  CHECK(from_status(500, "no").m_kind == error_kind::upstream);
  CHECK(from_status(500, "no").m_status == 500);
  CHECK(from_status(599, "no").m_status == 599);

  auto const busy = from_status(503, "busy", 12);
  CHECK(busy.m_kind == error_kind::busy);
  CHECK(busy.m_status == 503);
  CHECK(busy.m_retry_after == 12);

  auto const default_busy = from_status(503, "");
  CHECK(default_busy.m_retry_after == 30);

  auto const out_of_range = from_status(200, "");
  CHECK(out_of_range.m_kind == error_kind::upstream);
  CHECK(out_of_range.m_status == 502);

  auto const zero = from_status(0, "canceled");
  CHECK(zero.m_kind == error_kind::upstream);
  CHECK(zero.m_status == 502);
}

TEST_CASE("to_error keeps the classified status, code and retry hint", "[qoder][errors]") {
  auto const classified = classify(400, R"({"code":110,"message":"out of credits"})");
  auto const converted = to_error(classified);
  CHECK(converted.m_kind == error_kind::quota);
  CHECK(converted.m_http_status == 429);
  CHECK(converted.m_code == "110");
  CHECK(converted.m_message == "out of credits");

  auto const busy = to_error(classify(503, R"({"code":"10605"})"));
  CHECK(busy.m_kind == error_kind::busy);
  REQUIRE(busy.m_retry_after.has_value());
  CHECK(*busy.m_retry_after == 30);
}

TEST_CASE("the code helpers agree with the table", "[qoder][errors]") {
  CHECK(qoderz::qoder::is_busy_code("10605"));
  CHECK_FALSE(qoderz::qoder::is_busy_code("10606"));
  CHECK(qoderz::qoder::is_auth_code("105"));
  CHECK(qoderz::qoder::is_quota_code(110));
  CHECK(qoderz::qoder::is_quota_code(119));
  CHECK_FALSE(qoderz::qoder::is_quota_code(120));
  CHECK(qoderz::qoder::is_request_code(100403));
  CHECK_FALSE(qoderz::qoder::is_request_code(100404));
}
