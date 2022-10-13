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

#ifndef FCP_AGGREGATION_TENSORFLOW_CHECKPOINT_WRITER_H_
#define FCP_AGGREGATION_TENSORFLOW_CHECKPOINT_WRITER_H_

#include <string>

#include "absl/status/status.h"
#include "fcp/aggregation/core/tensor.h"
#include "tensorflow/core/util/tensor_slice_writer.h"

namespace fcp::aggregation::tensorflow {

// This class wraps TensorSliceWriter and provides a similar
// functionality but accepts Aggregation Core tensors instead.
// This class is designed to write only dense tensors that consist of a
// single slice.
class CheckpointWriter final {
 public:
  // CheckpointReader is neither copyable nor moveable
  CheckpointWriter(const CheckpointWriter&) = delete;
  CheckpointWriter& operator=(const CheckpointWriter&) = delete;

  // Constructs CheckpointWriter for the given filename.
  explicit CheckpointWriter(const std::string& filename);

  // Adds a tensor to the checkpoint.
  absl::Status Add(const std::string& tensor_name, const Tensor& tensor);

  // Writes the checkpoint to the file.
  absl::Status Finish();

 private:
  ::tensorflow::checkpoint::TensorSliceWriter tensorflow_writer_;
};

}  // namespace fcp::aggregation::tensorflow

#endif  // FCP_AGGREGATION_TENSORFLOW_CHECKPOINT_WRITER_H_
