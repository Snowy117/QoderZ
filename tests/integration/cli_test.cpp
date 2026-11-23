#include "cli/app.hpp"
#include "cli/commands/serve/wiring.hpp"
#include "cli/options.hpp"
#include "common/clock.hpp"
#include "common/json.hpp"
#include "config/credentials.hpp"
#include "support/async_test.hpp"
#include "support/server_harness.hpp"
#include "support/temp_dir.hpp"

#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;

using qoderz::json;
using qoderz::cli::app_context;
using qoderz::cli::serve_options;
using qoderz::cli::serve_plan;
using qoderz::config::account_record;
using qoderz::config::credential_file;
using qoderz::config::credential_store;
using qoderz::test::bearer;
using qoderz::test::build_request;
using qoderz::test::http_result;
using qoderz::test::one_request;
using qoderz::test::run_awaitable;
using qoderz::testing::temp_dir;

constexpr std::int64_t created_ms = 1'760'000'000'000;
constexpr std::int64_t one_hour_ms = 3600 * 1000;
serve_options listen_on(std::string listen) {
  serve_options options;
  options.m_listen = std::move(listen);
  return options;
}

serve_options listen_and_accounts(std::string listen, std::string accounts) {
  serve_options options = listen_on(std::move(listen));
  options.m_accounts = std::move(accounts);
  return options;
}

serve_options listen_and_account(std::string listen, std::string account) {
  serve_options options = listen_on(std::move(listen));
  options.m_account = std::move(account);
  return options;
}

constexpr std::string_view site_id = "qoder";
constexpr std::string_view listing_path = "/algo/api/v2/model/list";

account_record make_account(std::string name, std::string vpc = {}) {
  account_record record;
  record.m_site = std::string{site_id};
  record.m_name = std::move(name);
  record.m_vpc = std::move(vpc);
  record.m_access_token = "test-access-token-not-real";
  record.m_refresh_token = "test-refresh-token-not-real";
  // The token must look fresh against the real wall clock, otherwise `ensure_fresh` would try to
  // refresh it and reach for an endpoint the stub does not serve.
  record.m_expires_at = qoderz::system_clock{}.now_ms() + one_hour_ms;
  record.m_uid = "uid-" + record.m_name;
  record.m_display_name = "Display " + record.m_name;
  record.m_email = record.m_name + "@example.invalid";
  record.m_account_id = record.m_email;
  record.m_machine_id = "machine-not-real";
  record.m_created_at = created_ms;
  return record;
}

std::string listing_body() {
  json const ultimate = json{
      {"key", "ultimate"},
      {"display_name", "Ultimate"},
      {"enable", true},
      {"max_input_tokens", 1000000},
      {"is_vl", true},
      {"thinking_config",
       json{{"enabled", json{{"efforts", json{{"low", json::object()}, {"high", json::object()}}}}},
            {"disabled", json::object()}}}};
  return json{{"app", json::array({ultimate})}, {"chat", json::array()}}.dump();
}
std::string usage_body() {
  return R"({"displayMode":"qoder","userType":"pro_trial","expiresAt":1767225600000,)"
         R"("qoderUsage":{"userQuota":{"total":1000,"used":250,"unit":"credits"},)"
         R"("addOnQuota":{"total":500,"used":100,"unit":"credits"}}})";
}
/// A `serve` fixture: a loopback upstream stub plus a temporary credential store.
struct serve_fixture {
  temp_dir m_dir;
  credential_store m_store{m_dir.file("credentials.json")};
  asio::io_context m_upstream_io;
  qoderz::test::stub_server m_upstream{
      m_upstream_io,
      [this](qoderz::test::stub_request const& request)
          -> asio::awaitable<qoderz::test::stub_response> {
        qoderz::test::stub_response response;
        auto const path = qoderz::server::path_of(request.m_target);
        if (path == listing_path) {
          ++m_listing_calls;
          response.m_body = listing_body();
          co_return response;
        }
        if (path == "/api/v1/jobToken/exchange") {
          response.m_body = json{{"token", "test-access-token-not-real"},
                                 {"refresh_token", "test-refresh-token-not-real"},
                                 {"expires_at", 4102444800000LL}}
                                .dump();
          co_return response;
        }
        if (path == "/api/v1/userinfo") {
          response.m_body =
              json{{"id", "uid-pat"}, {"name", "Pat User"}, {"email", "pat@example.invalid"}}
                  .dump();
          co_return response;
        }
        if (path == "/sash/api/v2/me/usage") {
          response.m_body = usage_body();
          co_return response;
        }
        response.m_status = 404;
        co_return response;
      }};
  std::size_t m_listing_calls = 0;
  /// The stub and the CLI drive different io_contexts, so the stub needs its own worker.
  std::jthread m_upstream_thread;

  serve_fixture() {
    REQUIRE(m_upstream.start().has_value());
    m_upstream_thread = std::jthread{[this] { m_upstream_io.run(); }};
  }

  ~serve_fixture() {
    m_upstream.stop();
    m_upstream_io.stop();
    if (m_upstream_thread.joinable()) {
      m_upstream_thread.join();
    }
  }

  std::map<std::string, std::string> env() const {
    return {{"QODERZ_CREDENTIALS", m_dir.file("credentials.json").string()},
            {"QODERZ_SITE", std::string{site_id}},
            {"QODERZ_UPSTREAM", m_upstream.origin()},
            {"HOME", m_dir.path().string()}};
  }

  void store(std::vector<account_record> accounts) {
    credential_file file;
    for (auto const& account : accounts) {
      file.m_accounts.emplace(account.key(), account);
    }
    if (!accounts.empty()) {
      file.m_defaults[std::string{site_id}] = accounts.front().key();
    }
    REQUIRE(m_store.save(file).has_value());
  }
};

struct cli_run {
  std::istringstream m_in;
  std::ostringstream m_out;
  std::ostringstream m_err;
  app_context m_context;

  cli_run(std::vector<std::string> args, std::map<std::string, std::string> env)
      : m_context{.m_args = std::move(args),
                  .m_env = std::move(env),
                  .m_out = m_out,
                  .m_err = m_err,
                  .m_in = m_in} {}

  int operator()() { return qoderz::cli::run(m_context); }
};

std::optional<http_result>
request_port(std::uint16_t port, std::string method, std::string target,
             std::vector<std::pair<std::string, std::string>> headers = {}, std::string body = {}) {
  asio::io_context io;
  auto wire = build_request(method, target, headers, body, false);
  auto nested = run_awaitable(io, one_request(port, std::move(wire)));
  if (!nested.has_value()) {
    return std::nullopt;
  }
  return std::move(*nested);
}

} // namespace

TEST_CASE("the CLI reports the documented exit codes", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({make_account("work")});

  CHECK(cli_run{{"--version"}, fixture.env()}() == 0);
  CHECK(cli_run{{"--help"}, fixture.env()}() == 0);
  CHECK(cli_run{{}, fixture.env()}() == 0);
  CHECK(cli_run{{"nope"}, fixture.env()}() == 2);
  CHECK(cli_run{{"accounts", "--bogus"}, fixture.env()}() == 2);
  CHECK(cli_run{{"accounts"}, fixture.env()}() == 0);
}

TEST_CASE("accounts reads a temporary store without a token", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({make_account("work"), make_account("home")});

  cli_run run{{"accounts", "--json"}, fixture.env()};
  REQUIRE(run() == 0);
  auto const parsed = json::parse(run.m_out.str(), nullptr, false);
  REQUIRE(parsed.is_array());
  CHECK(parsed.size() == 2);
  CHECK(run.m_out.str().find("test-access-token-not-real") == std::string::npos);
}

TEST_CASE("serve refuses an empty pool", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({});

  cli_run run{{"serve", "--listen", "http://127.0.0.1:0"}, fixture.env()};
  CHECK(run() == 1);
  CHECK(run.m_err.str().find("qoderz login") != std::string::npos);
}

TEST_CASE("serve refuses a pinned account that does not exist", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({make_account("work")});

  cli_run run{{"serve", "--listen", "http://127.0.0.1:0", "--account", "nobody"}, fixture.env()};
  CHECK(run() == 1);
  CHECK(run.m_err.str().find("nobody") != std::string::npos);
}

TEST_CASE("serve refuses --account together with --accounts", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({make_account("work"), make_account("home")});

  cli_run run{
      {"serve", "--listen", "http://127.0.0.1:0", "--account", "work", "--accounts", "work,home"},
      fixture.env()};
  CHECK(run() == 2);
  CHECK(run.m_err.str().find("mutually exclusive") != std::string::npos);
}

TEST_CASE("serve refuses a public listener without an API key", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({make_account("work")});

  cli_run run{{"serve", "--listen", "http://0.0.0.0:0"}, fixture.env()};
  CHECK(run() == 1);
  CHECK(run.m_err.str().find("--allow-public") != std::string::npos);
}

TEST_CASE("serve plans a loopback pool with the upstream override applied", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({make_account("work"), make_account("home")});

  auto env = fixture.env();
  cli_run run{{"serve"}, env};
  qoderz::cli::command_context context{.m_app = run.m_context,
                                       .m_env = qoderz::cli::read_env(env),
                                       .m_cli = qoderz::cli::make_global_options({})};
  auto const plan = qoderz::cli::plan_serve(context, listen_on("http://127.0.0.1:0"));
  REQUIRE(plan.has_value());
  CHECK(plan->m_accounts.size() == 2);
  CHECK(plan->m_config.m_listen_host == "127.0.0.1");
  CHECK(plan->m_config.m_port == 0);
  CHECK(plan->m_config.m_site_id == std::string{site_id});
  CHECK_FALSE(plan->m_pool_options.m_pinned_key.has_value());
  CHECK(plan->m_pool_options.m_only_keys.empty());
  CHECK(plan->m_upstream.has_value());
  for (auto const& account : plan->m_accounts) {
    CHECK(account.m_site.m_api_base == fixture.m_upstream.origin());
  }
}

TEST_CASE("serve rejects a subset that names no account", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({make_account("work")});

  auto env = fixture.env();
  cli_run run{{"serve"}, env};
  qoderz::cli::command_context context{.m_app = run.m_context,
                                       .m_env = qoderz::cli::read_env(env),
                                       .m_cli = qoderz::cli::make_global_options({})};
  auto const plan =
      qoderz::cli::plan_serve(context, listen_and_accounts("http://127.0.0.1:0", "work,missing"));
  CHECK_FALSE(plan.has_value());

  // A subset that parses to nothing must not silently widen to the whole pool.
  auto const empty =
      qoderz::cli::plan_serve(context, listen_and_accounts("http://127.0.0.1:0", ","));
  CHECK_FALSE(empty.has_value());
  CHECK(empty.error().m_kind == qoderz::error_kind::bad_request);
}

TEST_CASE("a pinned pool reports the pinned mode", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({make_account("work"), make_account("home")});

  auto env = fixture.env();
  cli_run run{{"serve"}, env};
  qoderz::cli::command_context context{.m_app = run.m_context,
                                       .m_env = qoderz::cli::read_env(env),
                                       .m_cli = qoderz::cli::make_global_options({})};
  auto plan = qoderz::cli::plan_serve(context, listen_and_account("http://127.0.0.1:0", "work"));
  REQUIRE(plan.has_value());
  REQUIRE(plan->m_accounts.size() == 1);
  CHECK(plan->m_pool_options.m_pinned_key == "qoder/work");

  auto runtime = qoderz::cli::serve_runtime::create(std::move(*plan));
  REQUIRE(runtime.has_value());
  CHECK((*runtime)->mode() == "pinned");
  CHECK((*runtime)->account_count() == 1);
}

TEST_CASE("serve answers health, models and chat against a stub upstream", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({make_account("work")});

  auto env = fixture.env();
  cli_run run{{"serve"}, env};
  auto context = qoderz::cli::command_context{.m_app = run.m_context,
                                              .m_env = qoderz::cli::read_env(env),
                                              .m_cli = qoderz::cli::make_global_options({})};
  auto plan = qoderz::cli::plan_serve(context, listen_on("http://127.0.0.1:0"));
  REQUIRE(plan.has_value());

  auto runtime = qoderz::cli::serve_runtime::create(std::move(*plan));
  REQUIRE(runtime.has_value());
  auto started = (*runtime)->start();
  REQUIRE(started.has_value());
  auto const port = *started;
  REQUIRE(port != 0);

  std::jthread pumping{[&] { (*runtime)->io().run(); }};

  auto health = request_port(port, "GET", "/healthz");
  REQUIRE(health.has_value());
  CHECK(health->m_status == 200);

  auto models = request_port(port, "GET", "/v1/models");
  REQUIRE(models.has_value());
  CHECK(models->m_status == 200);
  auto const listing = json::parse(models->m_body, nullptr, false);
  REQUIRE(listing.is_object());
  CHECK(listing.at("object") == "list");
  CHECK(listing.at("data").at(0).at("id") == "ultimate");

  auto chat = request_port(port, "POST", "/v1/chat/completions", {},
                           R"({"model":"ultimate","messages":[]})");
  REQUIRE(chat.has_value());
  // The upstream stub answers 404 for the chat path, so the gateway maps it to a 502.
  CHECK(chat->m_status == 502);

  (*runtime)->listener().stop();
  (*runtime)->io().stop();
  pumping.join();
}

TEST_CASE("models prints the catalogue from a stub upstream", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({make_account("work")});

  auto env = fixture.env();
  env["QODERZ_SITE"] = std::string{site_id};

  cli_run run{{"models", "--json"}, env};
  auto const code = run();
  CHECK(code == 0);
  auto const parsed = json::parse(run.m_out.str(), nullptr, false);
  REQUIRE(parsed.is_object());
  CHECK(parsed.at("object") == "list");
  CHECK(parsed.at("data").at(0).at("id") == "ultimate");
  CHECK(parsed.at("data").at(0).at("owned_by") == "qoder");
  CHECK(parsed.at("data").at(0).at("context_window") == 1000000);
  CHECK(parsed.at("data").at(0).at("supports_tools") == true);
  CHECK(parsed.at("data").at(0).at("supports_images") == true);
  REQUIRE(parsed.at("data").at(0).at("reasoning").is_array());
  CHECK(parsed.at("data").at(0).at("reasoning").size() == 2);
  CHECK(parsed.at("data").at(0).at("reasoning").at(0) == "low");
  CHECK(parsed.at("data").at(0).at("reasoning").at(1) == "high");
  CHECK(run.m_out.str().find("test-access-token-not-real") == std::string::npos);
}

TEST_CASE("models without an account is an auth error", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({});

  cli_run run{{"models"}, fixture.env()};
  CHECK(run() == 3);
  CHECK(run.m_err.str().find("qoderz login") != std::string::npos);
}

TEST_CASE("a PAT login stores the account and never prints the token", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({});

  cli_run run{{"login", "--pat", "pt-test-not-real", "--name", "work", "--json"}, fixture.env()};
  REQUIRE(run() == 0);

  auto const parsed = json::parse(run.m_out.str(), nullptr, false);
  REQUIRE(parsed.is_object());
  CHECK(parsed.at("site") == std::string{site_id});
  CHECK(parsed.at("name") == "work");
  CHECK(parsed.at("uid") == "uid-pat");
  CHECK(parsed.at("email") == "pat@example.invalid");
  CHECK(parsed.at("default") == true);
  CHECK(run.m_out.str().find("pt-test-not-real") == std::string::npos);
  CHECK(run.m_out.str().find("test-access-token-not-real") == std::string::npos);

  auto const file = fixture.m_store.load();
  REQUIRE(file.has_value());
  REQUIRE(file->m_accounts.size() == 1);
  auto const& record = file->m_accounts.begin()->second;
  CHECK(record.m_name == "work");
  CHECK(record.m_pat_auth);
  CHECK(record.m_pat == "pt-test-not-real");
  CHECK(file->m_defaults.at(std::string{site_id}) == "qoder/work");
}

TEST_CASE("QODERZ_PAT is only a fallback for an explicit --pat", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({});

  auto env = fixture.env();
  env["QODERZ_PAT"] = "pt-from-env-not-real";
  cli_run run{{"login", "--pat", "pt-test-not-real", "--name", "work", "--json"}, env};
  REQUIRE(run() == 0);

  auto const file = fixture.m_store.load();
  REQUIRE(file.has_value());
  REQUIRE(file->m_accounts.size() == 1);
  CHECK(file->m_accounts.begin()->second.m_pat == "pt-test-not-real");
  CHECK(run.m_out.str().find("pt-from-env-not-real") == std::string::npos);
}

TEST_CASE("a PAT login prints the documented summary", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({});

  cli_run run{{"login", "--pat", "pt-test-not-real", "--name", "work"}, fixture.env()};
  REQUIRE(run() == 0);
  CHECK(run.m_out.str() == "Signed in to qoder as pat@example.invalid (account 'work').\n"
                           "work is now the default account for qoder.\n");
}

TEST_CASE("a second login does not steal the default account", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({make_account("work")});

  cli_run run{{"login", "--pat", "pt-test-not-real", "--name", "home", "--json"}, fixture.env()};
  REQUIRE(run() == 0);
  auto const parsed = json::parse(run.m_out.str(), nullptr, false);
  REQUIRE(parsed.is_object());
  CHECK(parsed.at("default") == false);

  auto const file = fixture.m_store.load();
  REQUIRE(file.has_value());
  CHECK(file->m_defaults.at(std::string{site_id}) == "qoder/work");
}

TEST_CASE("usage prints the plan and the windows", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({make_account("work")});

  cli_run text{{"usage"}, fixture.env()};
  REQUIRE(text() == 0);
  CHECK(text.m_out.str().find("Plan: pro_trial") != std::string::npos);
  CHECK(text.m_out.str().find("Credits: 250 / 1000 credits (25%)") != std::string::npos);
  CHECK(text.m_out.str().find("Add-on credits: 100 / 500 credits (20%)") != std::string::npos);

  cli_run json_run{{"usage", "--json"}, fixture.env()};
  REQUIRE(json_run() == 0);
  auto const parsed = json::parse(json_run.m_out.str(), nullptr, false);
  REQUIRE(parsed.is_object());
  CHECK(parsed.at("plan") == "pro_trial");
  REQUIRE(parsed.at("windows").is_array());
  REQUIRE(parsed.at("windows").size() == 2);
  CHECK(parsed.at("windows").at(0).at("name") == "Credits");
  CHECK(parsed.at("windows").at(0).at("display") == "250 / 1000 credits");
  CHECK(json_run.m_out.str().find("test-access-token-not-real") == std::string::npos);
}

TEST_CASE("serve rejects an https listen address and a bad thread count", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({make_account("work")});

  cli_run https{{"serve", "--listen", "https://127.0.0.1:8788"}, fixture.env()};
  CHECK(https() == 1);
  CHECK(https.m_err.str().find("http://host:port") != std::string::npos);

  cli_run threads{{"serve", "--listen", "http://127.0.0.1:0", "--threads", "0"}, fixture.env()};
  CHECK(threads() == 1);

  cli_run bodies{{"serve", "--listen", "http://127.0.0.1:0", "--max-body-bytes", "many"},
                 fixture.env()};
  CHECK(bodies() == 1);
}

TEST_CASE("an explicit API key outranks QODERZ_API_KEY", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({make_account("work")});

  auto env = fixture.env();
  env["QODERZ_API_KEY"] = "env-key-not-real";
  cli_run run{{"serve"}, env};
  qoderz::cli::command_context context{.m_app = run.m_context,
                                       .m_env = qoderz::cli::read_env(env),
                                       .m_cli = qoderz::cli::make_global_options({})};

  // §10 of `docs/design/cli.md`: command line > environment, so a key on the command line wins
  // over QODERZ_API_KEY instead of being reported as a conflicting source.
  auto options = listen_on("http://127.0.0.1:0");
  options.m_api_key = "cli-key-not-real";
  auto const planned = qoderz::cli::plan_serve(context, options);
  REQUIRE(planned.has_value());
  REQUIRE(planned->m_config.m_api_key.has_value());
  CHECK(*planned->m_config.m_api_key == "cli-key-not-real");

  auto conflicting = options;
  conflicting.m_api_key_stdin = true;
  auto const refused = qoderz::cli::plan_serve(context, conflicting);
  CHECK_FALSE(refused.has_value());
  CHECK(refused.error().m_kind == qoderz::error_kind::bad_request);
}

TEST_CASE("the serve startup summary goes to stderr and JSON to stdout", "[cli][integration]") {
  serve_fixture fixture;
  fixture.store({make_account("work")});

  auto env = fixture.env();
  cli_run run{{"serve"}, env};
  qoderz::cli::command_context context{.m_app = run.m_context,
                                       .m_env = qoderz::cli::read_env(env),
                                       .m_cli = qoderz::cli::make_global_options({})};
  auto plan = qoderz::cli::plan_serve(context, listen_on("http://127.0.0.1:0"));
  REQUIRE(plan.has_value());
  plan->m_json = true;
  auto runtime = qoderz::cli::serve_runtime::create(std::move(*plan));
  REQUIRE(runtime.has_value());
  auto const started = (*runtime)->start();
  REQUIRE(started.has_value());
  (*runtime)->print_summary(run.m_context, *started);

  auto const summary = json::parse(run.m_out.str(), nullptr, false);
  REQUIRE(summary.is_object());
  CHECK(summary.at("site") == std::string{site_id});
  CHECK(summary.at("accounts") == 1);
  CHECK(summary.at("mode") == "pool");
  CHECK(summary.at("upstream_loopback") == true);
  CHECK(summary.at("threads") == qoderz::cli::default_threads());
  CHECK(run.m_err.str().find("qoderz 0.1.2") != std::string::npos);
  CHECK(run.m_err.str().find("listen:") != std::string::npos);
  CHECK(run.m_out.str().find("listen:") == std::string::npos);

  (*runtime)->listener().stop();
  (*runtime)->io().stop();
}
