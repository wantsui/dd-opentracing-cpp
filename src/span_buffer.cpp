#include "span_buffer.h"

#include "sample.h"
#include "span.h"
#include "writer.h"

namespace datadog {
namespace opentracing {

namespace {
const std::string sampling_priority_metric = "_sampling_priority_v1";
const std::string datadog_origin_tag = "_dd.origin";
const std::string datadog_hostname_tag = "_dd.hostname";
const std::string event_sample_rate_metric = "_dd1.sr.eausr";
const std::string rules_sampler_applied_rate = "_dd.rule_psr";
const std::string rules_sampler_limiter_rate = "_dd.limit_psr";
const std::string priority_sampler_applied_rate = "_dd.agent_psr";

// Return whether the specified `span` is without a parent among the specified
// `all_spans_in_trace`.
bool is_root(const SpanData& span, const std::unordered_set<uint64_t>& all_spans_in_trace) {
  return
      // root span
      span.parent_id == 0 ||
      // local root span of a distributed trace
      all_spans_in_trace.find(span.parent_id) == all_spans_in_trace.end();
}

// Alter the specified `span` to prepare it for encoding with the specified
// `trace`.
void finish_span(const PendingTrace& trace, SpanData& span) {
  // Propagate the trace origin in every span, if present.  This allows, for
  // example, sampling to vary with the trace's stated origin.
  if (!trace.origin.empty()) {
    span.meta[datadog_origin_tag] = trace.origin;
  }
}

// Alter the specified root (i.e. having no parent in the local trace) `span`
// to prepare it for encoding with the specified `trace`.
void finish_root_span(const PendingTrace& trace, SpanData& span) {
  // Check for sampling.
  if (trace.sampling_priority != nullptr) {
    span.metrics[sampling_priority_metric] = static_cast<int>(*trace.sampling_priority);
    // The span's datadog origin tag is set in `finish_span`, below.
  }
  if (!trace.hostname.empty()) {
    span.meta[datadog_hostname_tag] = trace.hostname;
  }
  if (!std::isnan(trace.analytics_rate) &&
      span.metrics.find(event_sample_rate_metric) == span.metrics.end()) {
    span.metrics[event_sample_rate_metric] = trace.analytics_rate;
  }
  if (!std::isnan(trace.sample_result.rule_rate)) {
    span.metrics[rules_sampler_applied_rate] = trace.sample_result.rule_rate;
  }
  if (!std::isnan(trace.sample_result.limiter_rate)) {
    span.metrics[rules_sampler_limiter_rate] = trace.sample_result.limiter_rate;
  }
  if (!std::isnan(trace.sample_result.priority_rate)) {
    span.metrics[priority_sampler_applied_rate] = trace.sample_result.priority_rate;
  }
  // Forward to the finisher that applies to all spans (not just root spans).
  finish_span(trace, span);
}

}  // namespace

void PendingTrace::finish() {
  // Apply changes to spans, in particular treating the root / local-root
  // span as special.
  for (const auto& span : *finished_spans) {
    if (is_root(*span, all_spans)) {
      finish_root_span(*this, *span);
    } else {
      finish_span(*this, *span);
    }
  }
}

WritingSpanBuffer::WritingSpanBuffer(std::shared_ptr<const Logger> logger,
                                     std::shared_ptr<Writer> writer,
                                     std::shared_ptr<RulesSampler> sampler,
                                     WritingSpanBufferOptions options)
    : logger_(logger), writer_(writer), sampler_(sampler), options_(options) {}

void WritingSpanBuffer::registerSpan(const SpanContext& context) {
  std::lock_guard<std::mutex> lock_guard{mutex_};
  uint64_t trace_id = context.traceId();
  auto trace = traces_.find(trace_id);
  if (trace == traces_.end() || trace->second.all_spans.empty()) {
    traces_.emplace(std::make_pair(trace_id, PendingTrace{logger_}));
    trace = traces_.find(trace_id);
    OptionalSamplingPriority p = context.getPropagatedSamplingPriority();
    trace->second.sampling_priority_locked = p != nullptr;
    trace->second.sampling_priority = std::move(p);
    if (!context.origin().empty()) {
      trace->second.origin = context.origin();
    }
    trace->second.hostname = options_.hostname;
    trace->second.analytics_rate = options_.analytics_rate;
  }
  trace->second.all_spans.insert(context.id());
}

void WritingSpanBuffer::finishSpan(std::unique_ptr<SpanData> span) {
  std::lock_guard<std::mutex> lock_guard{mutex_};
  auto trace_iter = traces_.find(span->traceId());
  if (trace_iter == traces_.end()) {
    logger_->Log(LogLevel::error, "Missing trace for finished span");
    return;
  }
  auto& trace = trace_iter->second;
  if (trace.all_spans.find(span->spanId()) == trace.all_spans.end()) {
    logger_->Log(LogLevel::error, "A Span that was not registered was submitted to WritingSpanBuffer");
    return;
  }
  uint64_t trace_id = span->traceId();
  trace.finished_spans->push_back(std::move(span));
  if (trace.finished_spans->size() == trace.all_spans.size()) {
    assignSamplingPriorityImpl(trace.finished_spans->back().get());
    trace.finish();
    unbufferAndWriteTrace(trace_id);
  }
}

void WritingSpanBuffer::unbufferAndWriteTrace(uint64_t trace_id) {
  auto trace_iter = traces_.find(trace_id);
  if (trace_iter == traces_.end()) {
    return;
  }
  auto& trace = trace_iter->second;
  if (options_.enabled) {
    writer_->write(std::move(trace.finished_spans));
  }
  traces_.erase(trace_iter);
}

void WritingSpanBuffer::flush(std::chrono::milliseconds timeout) { writer_->flush(timeout); }

OptionalSamplingPriority WritingSpanBuffer::getSamplingPriority(uint64_t trace_id) const {
  std::lock_guard<std::mutex> lock_guard{mutex_};
  return getSamplingPriorityImpl(trace_id);
}
OptionalSamplingPriority WritingSpanBuffer::getSamplingPriorityImpl(uint64_t trace_id) const {
  auto trace = traces_.find(trace_id);
  if (trace == traces_.end()) {
    logger_->Trace(trace_id, "cannot get sampling priority, trace not found");
    return nullptr;
  }
  if (trace->second.sampling_priority == nullptr) {
    return nullptr;
  }
  return std::make_unique<SamplingPriority>(*trace->second.sampling_priority);
}

OptionalSamplingPriority WritingSpanBuffer::setSamplingPriority(
    uint64_t trace_id, OptionalSamplingPriority priority) {
  std::lock_guard<std::mutex> lock_guard{mutex_};
  return setSamplingPriorityImpl(trace_id, std::move(priority));
}

OptionalSamplingPriority WritingSpanBuffer::setSamplingPriorityImpl(
    uint64_t trace_id, OptionalSamplingPriority priority) {
  auto trace_entry = traces_.find(trace_id);
  if (trace_entry == traces_.end()) {
    logger_->Trace(trace_id, "cannot set sampling priority, trace not found");
    return nullptr;
  }
  PendingTrace& trace = trace_entry->second;
  if (trace.sampling_priority_locked) {
    if (priority == nullptr || *priority == SamplingPriority::UserKeep ||
        *priority == SamplingPriority::UserDrop) {
      // Only print an error if a user is taking this action. This case is legitimate (albeit with
      // the same outcome) if the Sampler itself is trying to assignSamplingPriority.
      logger_->Trace(trace_id, "sampling priority already set and cannot be reassigned");
    }
    return getSamplingPriorityImpl(trace_id);
  }
  if (priority == nullptr) {
    trace.sampling_priority.reset(nullptr);
  } else {
    trace.sampling_priority.reset(new SamplingPriority(*priority));
    if (*priority == SamplingPriority::SamplerDrop || *priority == SamplingPriority::SamplerKeep) {
      // This is an automatically-assigned sampling priority.
      trace.sampling_priority_locked = true;
    }
  }
  return getSamplingPriorityImpl(trace_id);
}

OptionalSamplingPriority WritingSpanBuffer::assignSamplingPriority(const SpanData* span) {
  std::lock_guard<std::mutex> lock{mutex_};
  return assignSamplingPriorityImpl(span);
}

OptionalSamplingPriority WritingSpanBuffer::assignSamplingPriorityImpl(const SpanData* span) {
  bool sampling_priority_unset = getSamplingPriorityImpl(span->trace_id) == nullptr;
  if (sampling_priority_unset) {
    auto sampler_result = sampler_->sample(span->env(), span->service, span->name, span->trace_id);
    setSamplingPriorityImpl(span->trace_id, std::move(sampler_result.sampling_priority));
    setSamplerResult(span->trace_id, sampler_result);
  }
  return getSamplingPriorityImpl(span->trace_id);
}

void WritingSpanBuffer::setSamplerResult(uint64_t trace_id, SampleResult& sample_result) {
  auto trace_entry = traces_.find(trace_id);
  if (trace_entry == traces_.end()) {
    logger_->Trace(trace_id, "cannot assign rules sampler result, trace not found");
    return;
  }
  PendingTrace& trace = trace_entry->second;
  trace.sample_result.rule_rate = sample_result.rule_rate;
  trace.sample_result.limiter_rate = sample_result.limiter_rate;
  trace.sample_result.priority_rate = sample_result.priority_rate;
  if (sample_result.sampling_priority != nullptr) {
    trace.sample_result.sampling_priority =
        std::make_unique<SamplingPriority>(*sample_result.sampling_priority);
  }
}

}  // namespace opentracing
}  // namespace datadog
