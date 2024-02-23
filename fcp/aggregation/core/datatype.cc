/*
 * Copyright 2024 Google LLC
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

#include "fcp/aggregation/core/datatype.h"

namespace fcp {
namespace aggregation {

namespace internal {
TypeKind GetTypeKind(DataType dtype) {
  TypeKind type_kind = TypeKind::kUnknown;
  DTYPE_CASES(dtype, T, type_kind = internal::TypeTraits<T>::type_kind);
  return type_kind;
}

}  // namespace internal
}  // namespace aggregation
}  // namespace fcp
