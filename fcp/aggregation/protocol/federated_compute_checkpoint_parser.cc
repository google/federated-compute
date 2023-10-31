#include "fcp/aggregation/protocol/federated_compute_checkpoint_parser.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/cord.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "fcp/aggregation/core/tensor.h"
#include "fcp/aggregation/core/tensor.pb.h"
#include "fcp/aggregation/protocol/checkpoint_header.h"
#include "fcp/aggregation/protocol/checkpoint_parser.h"
#include "fcp/base/monitoring.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"

namespace fcp::aggregation {

namespace {
// A CheckpointParser implementation that reads Federated Compute wire format
// checkpoint.
class FederatedComputeCheckpointParser final : public CheckpointParser {
 public:
  explicit FederatedComputeCheckpointParser(
      absl::flat_hash_map<std::string, Tensor> tensors)
      : tensors_(std::move(tensors)) {}

  // Disallow copy and move constructors.
  FederatedComputeCheckpointParser(const FederatedComputeCheckpointParser&) =
      delete;
  FederatedComputeCheckpointParser& operator=(
      const FederatedComputeCheckpointParser&) = delete;

  absl::StatusOr<Tensor> GetTensor(const std::string& name) override {
    auto result = tensors_.find(name);
    if (result == tensors_.end()) {
      return absl::NotFoundError(
          absl::StrFormat("No aggregation tensor found for name %s", name));
    }
    return std::move(result->second);
  }

 private:
  absl::flat_hash_map<std::string, Tensor> tensors_;
};

}  // namespace

absl::StatusOr<std::unique_ptr<CheckpointParser>>
FederatedComputeCheckpointParserFactory::Create(
    const absl::Cord& serialized_checkpoint) const {
  absl::Cord flattenable_checkpoint(serialized_checkpoint);
  absl::string_view str = flattenable_checkpoint.Flatten();
  google::protobuf::io::ArrayInputStream input(str.data(), static_cast<int>(str.size()));

  google::protobuf::io::CodedInputStream stream(&input);

  std::string header;
  if (!stream.ReadString(&header, 4)) {
    return absl::InternalError(
        "Unable to read header from federated compute wire format checkpoint.");
  }
  if (header != kFederatedComputeCheckpointHeader) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Unsupported checkpoint format: %s", header));
  }

  absl::flat_hash_map<std::string, Tensor> tensors;
  while (!stream.ExpectAtEnd()) {
    uint32_t name_size;
    if (!stream.ReadVarint32(&name_size)) {
      return absl::InternalError(
          "Unable to read next tensor name size from federated compute wire "
          "format checkpoint.");
    }

    if (name_size == 0) {
      // Reached end of checkpoint.
      break;
    }

    std::string name;
    if (!stream.ReadString(&name, name_size)) {
      return absl::InternalError(
          "Unable to read next tensor name from federated compute wire "
          "format checkpoint.");
    }

    uint32_t tensor_size;
    if (!stream.ReadVarint32(&tensor_size)) {
      return absl::InternalError(
          absl::StrFormat("Unable to read tensor size for %s", name));
    }

    TensorProto tensor_proto;
    google::protobuf::io::CodedInputStream::Limit limit = stream.PushLimit(tensor_size);
    if (!tensor_proto.ParseFromCodedStream(&stream)) {
      return absl::InternalError(
          absl::StrFormat("Unable to parse tensor proto for %s", name));
    }
    stream.PopLimit(limit);

    FCP_ASSIGN_OR_RETURN(Tensor aggregation_tensor,
                         Tensor::FromProto(std::move(tensor_proto)));
    tensors.emplace(name, std::move(aggregation_tensor));
  }
  return std::make_unique<FederatedComputeCheckpointParser>(std::move(tensors));
}

}  // namespace fcp::aggregation
