#include "../src/sample.h"

#include <catch2/catch.hpp>
#include <ctime>

#include "../src/agent_writer.h"
#include "../src/span.h"
#include "../src/tracer.h"
#include "mocks.h"
using namespace datadog::opentracing;

TEST_CASE("priority sampler unit test") {
  PrioritySampler sampler;
  auto buffer = std::make_shared<MockBuffer>();

  SECTION("default unconfigured priority sampling behaviour is to always sample") {
    auto result = sampler.sample("", "", 0);
    REQUIRE(result.priority_rate == 1.0);
    REQUIRE(*result.sampling_priority == SamplingPriority::SamplerKeep);
    result = sampler.sample("env", "service", 1);
    REQUIRE(result.priority_rate == 1.0);
    REQUIRE(*result.sampling_priority == SamplingPriority::SamplerKeep);
  }

  SECTION("configured") {
    sampler.configure("{ \"service:nginx,env:\": 0.8, \"service:nginx,env:prod\": 0.2 }"_json);

    SECTION("spans that don't match a rule use the default rate") {
      auto result = sampler.sample("different env", "different service", 1);
      REQUIRE(result.priority_rate == 1.0);
      REQUIRE(*result.sampling_priority == SamplingPriority::SamplerKeep);
    }

    SECTION("spans can be sampled") {
      // Case 1, service:nginx,env: => 0.8
      int count_sampled = 0;
      int total = 10000;
      for (int i = 0; i < total; i++) {
        auto result = sampler.sample("", "nginx", getId());
        const auto& p = result.sampling_priority;

        REQUIRE(p != nullptr);
        REQUIRE(((*p == SamplingPriority::SamplerKeep) || (*p == SamplingPriority::SamplerDrop)));
        count_sampled += *p == SamplingPriority::SamplerKeep ? 1 : 0;
      }
      double sample_rate = count_sampled / static_cast<double>(total);
      REQUIRE((sample_rate < 0.85 && sample_rate > 0.75));
      // Case 2, service:nginx,env:prod => 0.2
      count_sampled = 0;
      total = 10000;
      for (int i = 0; i < total; i++) {
        auto result = sampler.sample("", "nginx", getId());
        const auto& p = result.sampling_priority;
        REQUIRE(p != nullptr);
        REQUIRE(((*p == SamplingPriority::SamplerKeep) || (*p == SamplingPriority::SamplerDrop)));
        count_sampled += *p == SamplingPriority::SamplerKeep ? 1 : 0;
      }
      sample_rate = count_sampled / static_cast<double>(total);
      REQUIRE((sample_rate < 0.85 && sample_rate > 0.75));
    }
  }
}

TEST_CASE("rules sampler") {
  // `RulesSampler`'s constructor parameters are used to configure the
  // sampler's `Limiter`. Here we prepare those arguments.
  std::tm start{};
  start.tm_mday = 12;
  start.tm_mon = 2;
  start.tm_year = 107;
  const TimePoint time{std::chrono::system_clock::from_time_t(timegm(&start)),
                       std::chrono::steady_clock::time_point{}};
  const TimeProvider get_time = [&time]() { return time; };  // Mock clock.
  // A `Limiter` configured with these parameters will allow the first, but
  // none afterward.
  const long max_tokens = 1;
  const double refresh_rate = 1.0;
  const long tokens_per_refresh = 1;
  const auto sampler =
      std::make_shared<RulesSampler>(get_time, max_tokens, refresh_rate, tokens_per_refresh);

  const ot::StartSpanOptions span_options;
  const ot::FinishSpanOptions finish_options;

  const auto mwriter = std::make_shared<MockWriter>(sampler);
  const auto writer = std::shared_ptr<Writer>(mwriter);

  SECTION("rule matching applied") {
    TracerOptions tracer_options;
    tracer_options.service = "test.service";
    tracer_options.sampling_rules = R"([
    {"name": "test.trace", "service": "test.service", "sample_rate": 0.1},
    {"name": "name.only.match", "sample_rate": 0.2},
    {"service": "service.only.match", "sample_rate": 0.3},
    {"name": "overridden operation name", "sample_rate": 0.4},
    {"sample_rate": 1.0}
])";
    auto tracer = std::make_shared<Tracer>(tracer_options, writer, sampler);
    struct RulesSamplerTestCase {
      std::string service;
      std::string name;
      bool matched;
      double rate;
    };
    auto test_case = GENERATE(values<RulesSamplerTestCase>({
        {"test.service", "test.trace", true, 0.1},
        {"any.service", "name.only.match", true, 0.2},
        {"service.only.match", "any.name", true, 0.3},
        {"any.service", "any.name", true, 1.0},
    }));
    auto result = sampler->match(test_case.service, test_case.name);
    REQUIRE(test_case.matched == result.matched);
    if (std::isnan(test_case.rate)) {
      REQUIRE(std::isnan(result.rate));
    } else {
      REQUIRE(test_case.rate == result.rate);
    }
  }

  SECTION("falls back to priority sampling when no matching rule") {
    TracerOptions tracer_options;
    tracer_options.service = "test.service";
    tracer_options.sampling_rules = R"([
    {"name": "unmatched.name", "service": "unmatched.service", "sample_rate": 0.1}
])";
    auto tracer = std::make_shared<Tracer>(tracer_options, writer, sampler);

    auto span = tracer->StartSpanWithOptions("operation.name", span_options);
    span->FinishWithOptions(finish_options);

    auto& metrics = mwriter->traces[0][0]->metrics;
    REQUIRE(metrics.find("_dd.rule_psr") == metrics.end());
    REQUIRE(metrics.find("_dd.limit_psr") == metrics.end());
    REQUIRE(metrics.find("_dd.agent_psr") != metrics.end());
  }

  SECTION("rule matching applied to overridden name") {
    TracerOptions tracer_options;
    tracer_options.service = "test.service";
    tracer_options.sampling_rules = R"([
    {"name": "overridden operation name", "sample_rate": 0.4},
    {"sample_rate": 1.0}
])";
    tracer_options.operation_name_override = "overridden operation name";
    auto tracer = std::make_shared<Tracer>(tracer_options, writer, sampler);

    auto span = tracer->StartSpanWithOptions("operation name", span_options);
    span->FinishWithOptions(finish_options);

    auto& metrics = mwriter->traces[0][0]->metrics;
    REQUIRE(metrics.find("_dd.rule_psr") != metrics.end());
    REQUIRE(metrics["_dd.rule_psr"] == 0.4);
  }

  SECTION("applies limiter to sampled spans only") {
    TracerOptions tracer_options;
    tracer_options.service = "test.service";
    tracer_options.sampling_rules = R"([
    {"sample_rate": 0.0}
])";
    auto tracer = std::make_shared<Tracer>(tracer_options, writer, sampler);

    auto span = tracer->StartSpanWithOptions("operation name", span_options);
    span->FinishWithOptions(finish_options);

    auto& metrics = mwriter->traces[0][0]->metrics;
    REQUIRE(metrics.find("_dd.rule_psr") != metrics.end());
    REQUIRE(metrics["_dd.rule_psr"] == 0.0);
    REQUIRE(metrics.find("_dd.limit_psr") == metrics.end());
    REQUIRE(metrics.find("_dd.agent_psr") == metrics.end());
  }

  SECTION("sampling based on rule yields a \"user\" sampling priority") {
    // See the comments in `RulesSampler::sample` for an explanation of this
    // section.

    // There are three cases:
    // 1. Create a rule that matches the trace, and has rate `0.0`. Expect
    //     priority `UserDrop`.
    // 2. Create a rule that matches the trace, and has rate `1.0`. Expect
    //     priority `UserKeep`.
    // 3. Create a rule that matches the trace, and has rate `1.0`, but the
    //     limiter drops it. Expect `UserDrop`.

    SECTION("when the matching rule drops a trace") {
      TracerOptions tracer_options;
      tracer_options.service = "test.service";
      tracer_options.sampling_rules = R"([
    {"sample_rate": 0.0}
])";
      const auto tracer = std::make_shared<Tracer>(tracer_options, writer, sampler);

      const auto span = tracer->StartSpanWithOptions("operation name", span_options);
      span->FinishWithOptions(finish_options);

      REQUIRE(mwriter->traces.size() == 1);
      REQUIRE(mwriter->traces[0].size() == 1);
      const auto& metrics = mwriter->traces[0][0]->metrics;
      REQUIRE(metrics.count("_sampling_priority_v1"));
      REQUIRE(metrics.at("_sampling_priority_v1") ==
              static_cast<double>(SamplingPriority::UserDrop));
    }

    SECTION("when the matching rule keeps a trace") {
      TracerOptions tracer_options;
      tracer_options.service = "test.service";
      tracer_options.sampling_rules = R"([
    {"sample_rate": 1.0}
])";
      const auto tracer = std::make_shared<Tracer>(tracer_options, writer, sampler);

      const auto span = tracer->StartSpanWithOptions("operation name", span_options);
      span->FinishWithOptions(finish_options);

      REQUIRE(mwriter->traces.size() == 1);
      REQUIRE(mwriter->traces[0].size() == 1);
      const auto& metrics = mwriter->traces[0][0]->metrics;
      REQUIRE(metrics.count("_sampling_priority_v1"));
      REQUIRE(metrics.at("_sampling_priority_v1") ==
              static_cast<double>(SamplingPriority::UserKeep));
    }

    SECTION("when the limiter drops a trace") {
      TracerOptions tracer_options;
      tracer_options.service = "test.service";
      tracer_options.sampling_rules = R"([
    {"sample_rate": 1.0}
])";
      const auto tracer = std::make_shared<Tracer>(tracer_options, writer, sampler);

      // The first span will be allowed by the limiter (tested in the previous section).
      auto span = tracer->StartSpanWithOptions("operation name", span_options);
      span->FinishWithOptions(finish_options);

      // The second trace will be dropped by the limiter, and the priority will
      // be `UserDrop`.
      span = tracer->StartSpanWithOptions("operation name", span_options);
      span->FinishWithOptions(finish_options);
      {
        REQUIRE(mwriter->traces.size() == 2);
        REQUIRE(mwriter->traces[1].size() == 1);
        const auto& metrics = mwriter->traces[1][0]->metrics;
        REQUIRE(metrics.count("_sampling_priority_v1"));
        REQUIRE(metrics.at("_sampling_priority_v1") ==
                static_cast<double>(SamplingPriority::UserDrop));
      }
    }
  }
}
