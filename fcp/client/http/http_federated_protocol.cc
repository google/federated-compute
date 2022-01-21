/*
 * Copyright 2022 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "fcp/client/http/http_federated_protocol.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/cord.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "absl/time/time.h"
#include "fcp/base/monitoring.h"
#include "fcp/client/diag_codes.pb.h"
#include "fcp/client/engine/engine.pb.h"
#include "fcp/client/federated_protocol.h"
#include "fcp/client/federated_protocol_util.h"
#include "fcp/client/fl_runner.pb.h"
#include "fcp/client/flags.h"
#include "fcp/client/http/http_client.h"
#include "fcp/client/http/http_client_util.h"
#include "fcp/client/http/in_memory_request_response.h"
#include "fcp/client/interruptible_runner.h"
#include "fcp/client/log_manager.h"
#include "fcp/client/task_environment.h"
#include "fcp/protos/federated_api.pb.h"
#include "fcp/protos/federatedcompute/common.pb.h"
#include "fcp/protos/federatedcompute/eligibility_eval_tasks.pb.h"
#include "fcp/protos/plan.pb.h"

namespace fcp {
namespace client {
namespace http {

using ::fcp::client::GenerateRetryWindowFromRetryTime;
using ::fcp::client::GenerateRetryWindowFromTargetDelay;
using ::fcp::client::PickRetryTimeFromRange;
using ::google::internal::federated::plan::ClientOnlyPlan;
using ::google::internal::federatedcompute::v1::EligibilityEvalTask;
using ::google::internal::federatedcompute::v1::EligibilityEvalTaskRequest;
using ::google::internal::federatedcompute::v1::EligibilityEvalTaskResponse;
using ::google::internal::federatedcompute::v1::Resource;
using ::google::internal::federatedml::v2::TaskEligibilityInfo;

// A note on error handling:
//
// The implementation here makes a distinction between what we call 'transient'
// and 'permanent' errors. While the exact categorization of transient vs.
// permanent errors is defined by a flag, the intent is that transient errors
// are those types of errors that may occur in the regular course of business,
// e.g. due to an interrupted network connection, a load balancer temporarily
// rejecting our request etc. Generally, these are expected to be resolvable by
// merely retrying the request at a slightly later time. Permanent errors are
// intended to be those that are not expected to be resolvable as quickly or by
// merely retrying the request. E.g. if a client checks in to the server with a
// population name that doesn't exist, then the server may return NOT_FOUND, and
// until the server-side configuration is changed, it will continue returning
// such an error. Hence, such errors can warrant a longer retry period (to waste
// less of both the client's and server's resources).
//
// The errors also differ in how they interact with the server-specified retry
// windows that are returned via the EligbilityEvalTaskResponse message.
// - If a permanent error occurs, then we will always return a retry window
//   based on the target 'permanent errors retry period' flag, regardless of
//   whether we received an EligbilityEvalTaskResponse from the server at an
//   earlier time.
// - If a transient error occurs, then we will only return a retry window
//   based on the target 'transient errors retry period' flag if the server
//   didn't already return an EligibilityEvalTaskResponse. If it did return such
//   a response, then one of the retry windows in that message will be used
//   instead.
//
// Finally, note that for simplicity's sake we generally check whether a
// permanent error was received at the level of this class's public methods,
// rather than deeper down in each of our helper methods that actually call
// directly into the HTTP stack. This keeps our state-managing code simpler, but
// does mean that if any of our helper methods like
// PerformEligibilityEvalTaskRequest produce a permanent error code locally
// (i.e. without it being sent by the server), it will be treated as if the
// server sent it and the permanent error retry period will be used. We consider
// this a reasonable tradeoff.

namespace {

// The URI suffix for a RequestEligibilityEvalTask protocol request.
//
// Arguments (which must be encoded using `EncodeUriSinglePathSegment`):
//   $0: the `EligibilityEvalTaskRequest.population_name` request field.
constexpr absl::string_view kRequestEligibilityEvalTaskUriSuffix =
    "/v1/eligibilityevaltasks/$0:request";

// Convert a Resource proto into a UriOrInlineData object. Returns an
// `INVALID_ARGUMENT` error if the given `Resource` has the `uri` field set to
// an empty value, or an `UNIMPLEMENTED` error if the `Resource` has an unknown
// field set.
absl::StatusOr<UriOrInlineData> ConvertResourceToUriOrInlineData(
    Resource resource) {
  switch (resource.resource_case()) {
    case Resource::ResourceCase::kUri:
      if (resource.uri().empty()) {
        return absl::InvalidArgumentError(
            "Resource.uri must be non-empty when set");
      }
      return UriOrInlineData::CreateUri(resource.uri());
    case Resource::ResourceCase::kData:
      return UriOrInlineData::CreateInlineData(absl::Cord(resource.data()));
    case Resource::ResourceCase::RESOURCE_NOT_SET:
      // If neither field is set at all, we'll just act as if we got an empty
      // inline data field.
      return UriOrInlineData::CreateInlineData(absl::Cord());
    default:
      return absl::UnimplementedError("Unknown Resource type");
  }
}

}  // namespace

HttpFederatedProtocol::HttpFederatedProtocol(
    LogManager* log_manager, const Flags* flags, HttpClient* http_client,
    absl::string_view entry_point_uri, absl::string_view api_key,
    absl::string_view population_name, absl::string_view retry_token,
    absl::string_view client_version, absl::string_view attestation_measurement,
    std::function<bool()> should_abort, absl::BitGen bit_gen,
    const InterruptibleRunner::TimingConfig& timing_config)
    : object_state_(ObjectState::kInitialized),
      flags_(flags),
      http_client_(http_client),
      next_request_base_uri_(entry_point_uri),
      api_key_(api_key),
      population_name_(population_name),
      retry_token_(retry_token),
      client_version_(client_version),
      attestation_measurement_(attestation_measurement),
      bit_gen_(std::move(bit_gen)) {
  interruptible_runner_ = std::make_unique<InterruptibleRunner>(
      log_manager, should_abort, timing_config,
      InterruptibleRunner::DiagnosticsConfig{
          .interrupted = ProdDiagCode::BACKGROUND_TRAINING_INTERRUPT_HTTP,
          .interrupt_timeout =
              ProdDiagCode::BACKGROUND_TRAINING_INTERRUPT_HTTP_TIMED_OUT,
          .interrupted_extended = ProdDiagCode::
              BACKGROUND_TRAINING_INTERRUPT_HTTP_EXTENDED_COMPLETED,
          .interrupt_timeout_extended = ProdDiagCode::
              BACKGROUND_TRAINING_INTERRUPT_HTTP_EXTENDED_TIMED_OUT});
  // Note that we could cast the provided error codes to absl::StatusCode
  // values here. However, that means we'd have to handle the case when
  // invalid integers that don't map to a StatusCode enum are provided in the
  // flag here. Instead, we cast absl::StatusCodes to int32_t each time we
  // compare them with the flag-provided list of codes, which means we never
  // have to worry about invalid flag values (besides the fact that invalid
  // values will be silently ignored, which could make it harder to realize when
  // a flag is misconfigured).
  const std::vector<int32_t>& error_codes =
      flags->federated_training_permanent_error_codes();
  federated_training_permanent_error_codes_ =
      absl::flat_hash_set<int32_t>(error_codes.begin(), error_codes.end());
  // TODO(team): Validate initial URI has https:// scheme, and a trailing
  // slash, either here or in fl_runner.cc.
}

absl::StatusOr<InMemoryHttpResponse>
HttpFederatedProtocol::PerformProtocolRequest(absl::string_view uri_suffix,
                                              std::string request_body) {
  absl::StatusOr<std::string> uri =
      JoinBaseUriWithSuffix(next_request_base_uri_, uri_suffix);
  FCP_CHECK_STATUS(uri.status());

  FCP_ASSIGN_OR_RETURN(std::unique_ptr<http::HttpRequest> request,
                       InMemoryHttpRequest::Create(
                           *uri, HttpRequest::Method::kPost,
                           next_request_headers_, std::move(request_body)));

  // Check whether issuing the request failed as a whole (generally indicating
  // a programming error).
  FCP_ASSIGN_OR_RETURN(
      InMemoryHttpResponse result,
      PerformRequestInMemory(*http_client_, *interruptible_runner_,
                             std::move(request), &bytes_downloaded_,
                             &bytes_uploaded_));
  if (!result.content_encoding.empty()) {
    // Note that the `HttpClient` API contract ensures that if we don't specify
    // an Accept-Encoding request header, then the response should be delivered
    // to us without any Content-Encoding applied to it. Hence, if we somehow do
    // still see a Content-Encoding response header then the `HttpClient`
    // implementation isn't adhering to its part of the API contract.
    return absl::UnavailableError(
        "HTTP response unexpectedly has a Content-Encoding");
  }
  return result;
}

absl::StatusOr<FederatedProtocol::EligibilityEvalCheckinResult>
HttpFederatedProtocol::EligibilityEvalCheckin() {
  FCP_CHECK(object_state_ == ObjectState::kInitialized)
      << "Invalid call sequence";
  object_state_ = ObjectState::kEligibilityEvalCheckinFailed;

  // Send the request and parse the response.
  auto response =
      HandleEligibilityEvalTaskResponse(PerformEligibilityEvalTaskRequest());
  // Update the object state to ensure we return the correct retry delay.
  UpdateObjectStateIfPermanentError(
      response.status(),
      ObjectState::kEligibilityEvalCheckinFailedPermanentError);
  return response;
}

absl::StatusOr<InMemoryHttpResponse>
HttpFederatedProtocol::PerformEligibilityEvalTaskRequest() {
  // Create and serialize the request body. Note that the `population_name`
  // field is set in the URI instead of in this request proto message.
  EligibilityEvalTaskRequest request;
  request.mutable_client_version()->set_version_code(client_version_);
  // TODO(team): Populate an attestation_measurement value here.

  FCP_ASSIGN_OR_RETURN(std::string encoded_population_name,
                       EncodeUriSinglePathSegment(population_name_));
  // Construct the URI suffix.
  std::string uri_suffix = absl::Substitute(
      kRequestEligibilityEvalTaskUriSuffix, encoded_population_name);

  // Issue the request.
  return PerformProtocolRequest(uri_suffix, request.SerializeAsString());
}

absl::StatusOr<FederatedProtocol::EligibilityEvalCheckinResult>
HttpFederatedProtocol::HandleEligibilityEvalTaskResponse(
    absl::StatusOr<InMemoryHttpResponse> http_response) {
  if (!http_response.ok()) {
    // If the protocol request failed then forward the error, but add a prefix
    // to the error message to ensure we can easily distinguish an HTTP error
    // occurring in response to the protocol request from HTTP errors occurring
    // during checkpoint/plan resource fetch requests later on.
    return absl::Status(http_response.status().code(),
                        absl::StrCat("protocol request failed: ",
                                     http_response.status().ToString()));
  }

  EligibilityEvalTaskResponse response_proto;
    if (!response_proto.ParseFromString(std::string(http_response->body))) {
    return absl::InvalidArgumentError("Could not parse response_proto");
  }

  // Upon receiving the server's RetryWindows we immediately choose a concrete
  // target timestamp to retry at. This ensures that a) clients of this class
  // don't have to implement the logic to select a timestamp from a min/max
  // range themselves, b) we tell clients of this class to come back at exactly
  // a point in time the server intended us to come at (i.e. "now +
  // server_specified_retry_period", and not a point in time that is partly
  // determined by how long the remaining protocol interactions (e.g. training
  // and results upload) will take (i.e. "now +
  // duration_of_remaining_protocol_interactions +
  // server_specified_retry_period").
  retry_times_ = RetryTimes{
      .retry_time_if_rejected = PickRetryTimeFromRange(
          response_proto.retry_window_if_rejected().delay_min(),
          response_proto.retry_window_if_rejected().delay_max(), bit_gen_),
      .retry_time_if_accepted = PickRetryTimeFromRange(
          response_proto.retry_window_if_accepted().delay_min(),
          response_proto.retry_window_if_accepted().delay_max(), bit_gen_)};

  // If the request was rejected then the protocol session has ended and there's
  // no more work for us to do.
  if (response_proto.has_rejection_info()) {
    object_state_ = ObjectState::kEligibilityEvalCheckinRejected;
    return Rejection{};
  }

  session_id_ = response_proto.session_id();

  // Extract the base URI and headers to use for the subsequent request.
  if (response_proto.forwarding_info().target_uri_prefix().empty()) {
    return absl::UnimplementedError(
        "Missing `ForwardingInfo.target_uri_prefix`");
  }
  next_request_base_uri_ = response_proto.forwarding_info().target_uri_prefix();
  auto new_headers = response_proto.forwarding_info().extra_request_headers();
  next_request_headers_ = HeaderList(new_headers.begin(), new_headers.end());

  switch (response_proto.result_case()) {
    case EligibilityEvalTaskResponse::kEligibilityEvalTask: {
      const auto& task = response_proto.eligibility_eval_task();

      // Fetch the task resources, returning any errors that may be encountered
      // in the process.
      FCP_ASSIGN_OR_RETURN(
          auto result,
          FetchTaskResources(
              {.plan = task.plan(), .checkpoint = task.init_checkpoint()}));

      object_state_ = ObjectState::kEligibilityEvalEnabled;
      return EligibilityEvalTask{
          .payloads = {.plan = std::move(result.plan),
                       .checkpoint = std::move(result.checkpoint)},
          .execution_id = task.execution_id()};
    }
    case EligibilityEvalTaskResponse::kNoEligibilityEvalConfigured: {
      // Nothing to do...
      object_state_ = ObjectState::kEligibilityEvalDisabled;
      return EligibilityEvalDisabled{};
    }
    default:
      return absl::UnimplementedError(
          "Unrecognized EligibilityEvalCheckinResponse");
  }
}

absl::StatusOr<HttpFederatedProtocol::PlanAndCheckpointPayloads>
HttpFederatedProtocol::FetchTaskResources(
    HttpFederatedProtocol::TaskResources task_resources) {
  FCP_ASSIGN_OR_RETURN(UriOrInlineData plan_uri_or_data,
                       ConvertResourceToUriOrInlineData(task_resources.plan));
  FCP_ASSIGN_OR_RETURN(
      UriOrInlineData checkpoint_uri_or_data,
      ConvertResourceToUriOrInlineData(task_resources.checkpoint));

  // Fetch the plan and init checkpoint resources if they need to be fetched
  // (using the inline data instead if available).
  absl::StatusOr<std::vector<absl::StatusOr<InMemoryHttpResponse>>>
      resource_responses =
          FetchResourcesInMemory(*http_client_, *interruptible_runner_,
                                 {plan_uri_or_data, checkpoint_uri_or_data},
                                 &bytes_downloaded_, &bytes_uploaded_);
  FCP_RETURN_IF_ERROR(resource_responses);
  auto& plan_data_response = (*resource_responses)[0];
  auto& checkpoint_data_response = (*resource_responses)[1];

  // Note: we forward any error during the fetching of the plan/checkpoint
  // resources resources to the caller, which means that these error codes will
  // be checked against the set of 'permanent' error codes, just like the errors
  // in response to the protocol request are.
  if (!plan_data_response.ok()) {
    return absl::Status(plan_data_response.status().code(),
                        absl::StrCat("plan fetch failed: ",
                                     plan_data_response.status().ToString()));
  }
  if (!checkpoint_data_response.ok()) {
    return absl::Status(
        checkpoint_data_response.status().code(),
        absl::StrCat("checkpoint fetch failed: ",
                     checkpoint_data_response.status().ToString()));
  }

  // TODO(team): This copies the plan & checkpoint data, which could be
  // large. Since that data is already an absl::Cord, consider changing this
  // method's return type to use absl::Cord to avoid the copy.
  return PlanAndCheckpointPayloads{std::string(plan_data_response->body),
                                   std::string(checkpoint_data_response->body)};
}

absl::StatusOr<FederatedProtocol::CheckinResult> HttpFederatedProtocol::Checkin(
    const std::optional<TaskEligibilityInfo>& task_eligibility_info) {
  // Checkin(...) must follow an earlier call to EligibilityEvalCheckin() that
  // resulted in a CheckinResultPayload or an EligibilityEvalDisabled result.
  FCP_CHECK(object_state_ == ObjectState::kEligibilityEvalDisabled ||
            object_state_ == ObjectState::kEligibilityEvalEnabled)
      << "Checkin(...) called despite failed/rejected earlier "
         "EligibilityEvalCheckin";
  if (object_state_ == ObjectState::kEligibilityEvalEnabled) {
    FCP_CHECK(task_eligibility_info.has_value())
        << "Missing TaskEligibilityInfo despite receiving prior "
           "EligibilityEvalCheckin payload";
  } else {
    FCP_CHECK(!task_eligibility_info.has_value())
        << "Received TaskEligibilityInfo despite not receiving a prior "
           "EligibilityEvalCheckin payload";
  }
  object_state_ = ObjectState::kCheckinFailed;

  return absl::UnimplementedError("Checkin() not implemented yet!");
}

absl::Status HttpFederatedProtocol::ReportCompleted(
    ComputationResults results,
    const std::vector<std::pair<std::string, double>>& stats,
    absl::Duration plan_duration) {
  FCP_LOG(INFO) << "Reporting outcome: " << static_cast<int>(engine::COMPLETED);
  FCP_CHECK(object_state_ == ObjectState::kCheckinAccepted)
      << "Invalid call sequence";
  object_state_ = ObjectState::kReportCalled;
  return absl::UnimplementedError("ReportCompleted() not implemented yet!");
}

absl::Status HttpFederatedProtocol::ReportNotCompleted(
    engine::PhaseOutcome phase_outcome, absl::Duration plan_duration) {
  FCP_LOG(WARNING) << "Reporting outcome: " << static_cast<int>(phase_outcome);
  FCP_CHECK(object_state_ == ObjectState::kCheckinAccepted)
      << "Invalid call sequence";
  object_state_ = ObjectState::kReportCalled;
  return absl::UnimplementedError("ReportNotCompleted() not implemented yet!");
}

::google::internal::federatedml::v2::RetryWindow
HttpFederatedProtocol::GetLatestRetryWindow() {
  // We explicitly enumerate all possible states here rather than using
  // "default", to ensure that when new states are added later on, the author
  // is forced to update this method and consider which is the correct
  // RetryWindow to return.
  switch (object_state_) {
    case ObjectState::kCheckinAccepted:
    case ObjectState::kReportCalled:
      // If a client makes it past the 'checkin acceptance' stage, we use the
      // 'accepted' RetryWindow unconditionally (unless a permanent error is
      // encountered). This includes cases where the checkin is accepted, but
      // the report request results in a (transient) error.
      FCP_CHECK(retry_times_.has_value());
      return GenerateRetryWindowFromRetryTime(
          retry_times_->retry_time_if_accepted);
    case ObjectState::kEligibilityEvalCheckinRejected:
    case ObjectState::kEligibilityEvalDisabled:
    case ObjectState::kEligibilityEvalEnabled:
    case ObjectState::kCheckinRejected:
      FCP_CHECK(retry_times_.has_value());
      return GenerateRetryWindowFromRetryTime(
          retry_times_->retry_time_if_rejected);
    case ObjectState::kInitialized:
    case ObjectState::kEligibilityEvalCheckinFailed:
    case ObjectState::kCheckinFailed:
      if (retry_times_.has_value()) {
        // If we already received a server-provided retry window, then use it.
        return GenerateRetryWindowFromRetryTime(
            retry_times_->retry_time_if_rejected);
      }
      // Otherwise, we generate a retry window using the flag-provided transient
      // error retry period.
      return GenerateRetryWindowFromTargetDelay(
          absl::Seconds(
              flags_->federated_training_transient_errors_retry_delay_secs()),
          // NOLINTBEGIN(whitespace/line_length)
          flags_
              ->federated_training_transient_errors_retry_delay_jitter_percent(),
          // NOLINTEND(whitespace/line_length)
          bit_gen_);
    case ObjectState::kEligibilityEvalCheckinFailedPermanentError:
    case ObjectState::kCheckinFailedPermanentError:
    case ObjectState::kReportFailedPermanentError:
      // If we encountered a permanent error during the eligibility eval or
      // regular checkins, then we use the Flags-configured 'permanent error'
      // retry period. Note that we do so regardless of whether the server had,
      // by the time the permanent error was received, already returned a
      // CheckinRequestAck containing a set of retry windows. See note on error
      // handling at the top of this file.
      return GenerateRetryWindowFromTargetDelay(
          absl::Seconds(
              flags_->federated_training_permanent_errors_retry_delay_secs()),
          // NOLINTBEGIN(whitespace/line_length)
          flags_
              ->federated_training_permanent_errors_retry_delay_jitter_percent(),
          // NOLINTEND(whitespace/line_length)
          bit_gen_);
  }
}

void HttpFederatedProtocol::UpdateObjectStateIfPermanentError(
    absl::Status status,
    HttpFederatedProtocol::ObjectState permanent_error_object_state) {
  if (federated_training_permanent_error_codes_.contains(
          static_cast<int32_t>(status.code()))) {
    object_state_ = permanent_error_object_state;
  }
}

int64_t HttpFederatedProtocol::chunking_layer_bytes_sent() {
  // Note: we don't distinguish between 'chunking' and 'non-chunking' layers
  // like the legacy protocol, as there is no concept of 'chunking' with the
  // HTTP protocol like there was with the gRPC protocol. Instead we simply
  // report our best estimate of the over-the-wire network usage.
  return bytes_uploaded_;
}

int64_t HttpFederatedProtocol::chunking_layer_bytes_received() {
  // See note about 'chunking' vs. 'non-chunking' layer in
  // `chunking_layer_bytes_sent`.
  return bytes_downloaded_;
}

int64_t HttpFederatedProtocol::bytes_downloaded() { return bytes_downloaded_; }

int64_t HttpFederatedProtocol::bytes_uploaded() { return bytes_uploaded_; }

int64_t HttpFederatedProtocol::report_request_size_bytes() {
  return report_request_size_bytes_;
}

}  // namespace http
}  // namespace client
}  // namespace fcp