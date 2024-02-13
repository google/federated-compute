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
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "fcp/aggregation/core/agg_vector.h"
#include "fcp/aggregation/core/datatype.h"
#include "fcp/aggregation/core/intrinsic.h"
#include "fcp/aggregation/core/tensor.h"
#include "fcp/aggregation/core/tensor_aggregator_registry.h"
#include "fcp/aggregation/core/tensor_shape.h"
#include "fcp/aggregation/core/tensor_spec.h"
#include "fcp/aggregation/testing/test_data.h"
#include "fcp/aggregation/testing/testing.h"
#include "fcp/base/monitoring.h"
#include "fcp/testing/testing.h"

namespace fcp {
namespace aggregation {

// The following is an explicit specialisation of TensorMatcherImpl for doubles.
// It tolerates low-order errors that come from finite-precision representation.
template <>
class TensorMatcherImpl<double>
    : public ::testing::MatcherInterface<const Tensor&> {
 public:
  TensorMatcherImpl(DataType expected_dtype, TensorShape expected_shape,
                    std::vector<double> expected_values)
      : expected_dtype_(expected_dtype),
        expected_shape_(expected_shape),
        expected_values_(expected_values) {}

  void DescribeTo(std::ostream* os) const override {
    DescribeTensor<double>(os, expected_dtype_, expected_shape_,
                           expected_values_);
  }

  bool MatchAndExplain(
      const Tensor& arg,
      ::testing::MatchResultListener* listener) const override {
    bool match_metadata =
        arg.dtype() == expected_dtype_ && arg.shape() == expected_shape_;
    if (match_metadata) {
      auto real_data = TensorValuesToVector<double>(arg);
      for (int i = 0; i < expected_values_.size(); ++i) {
        if (!(real_data[i] >= expected_values_[i] - 1e-7 &&
              real_data[i] <= expected_values_[i] + 1e-7)) {
          return false;
        }
      }
      return true;
    }
    return false;
  }

 private:
  DataType expected_dtype_;
  TensorShape expected_shape_;
  std::vector<double> expected_values_;
};

namespace {
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::IsTrue;

TensorSpec CreateTensorSpec(std::string name, DataType dtype) {
  return TensorSpec(name, dtype, {-1});
}

template <typename InputType>
std::vector<Tensor> CreateDPGFSParameters(InputType linfinity_bound,
                                          double l1_bound, double l2_bound) {
  std::vector<Tensor> parameters;

  parameters.push_back(
      Tensor::Create(internal::TypeTraits<InputType>::kDataType, {},
                     CreateTestData<InputType>({linfinity_bound}))
          .value());

  parameters.push_back(
      Tensor::Create(DT_DOUBLE, {}, CreateTestData<double>({l1_bound}))
          .value());

  parameters.push_back(
      Tensor::Create(DT_DOUBLE, {}, CreateTestData<double>({l2_bound}))
          .value());

  return parameters;
}

Intrinsic CreateDefaultIntrinsic() {
  return Intrinsic{"GoogleSQL:dp_sum",
                   {CreateTensorSpec("value", DT_INT32)},
                   {CreateTensorSpec("value", DT_INT64)},
                   {CreateDPGFSParameters<int32_t>(1000, -1, -1)},
                   {}};
}

// Below is some shorthand for testing. Each user contributes ordinals (first
// item) & values (second item).
using UserData = std::pair<const Tensor*, const Tensor*>;

// This function compares the outcome of aggregation with an expected outcome.
template <typename InputType, typename OutputType>
inline void MatchSum(InputType linfinity_bound, double l1_bound,
                     double l2_bound, const std::vector<UserData>& data,
                     const std::initializer_list<OutputType>& expected_sum) {
  DataType input_type = internal::TypeTraits<InputType>::kDataType;
  DataType output_type = internal::TypeTraits<OutputType>::kDataType;

  Intrinsic intrinsic{
      "GoogleSQL:dp_sum",
      {CreateTensorSpec("value", input_type)},
      {CreateTensorSpec("value", output_type)},
      {CreateDPGFSParameters<InputType>(linfinity_bound, l1_bound, l2_bound)},
      {}};

  auto aggregator_status = CreateTensorAggregator(intrinsic);
  EXPECT_THAT(aggregator_status, IsOk());

  auto aggregator = std::move(aggregator_status.value());

  for (auto d : data) {
    EXPECT_THAT(aggregator->Accumulate({d.first, d.second}), IsOk());
  }

  EXPECT_THAT(aggregator->CanReport(), IsTrue());
  EXPECT_THAT(aggregator->GetNumInputs(), Eq(data.size()));

  auto result = std::move(*aggregator).Report();
  EXPECT_THAT(result, IsOk());
  EXPECT_THAT(result->size(), Eq(1));
  // Verify the resulting tensor.
  int64_t expected_sum_length = static_cast<int64_t>(expected_sum.size());
  EXPECT_THAT(
      result.value()[0],
      IsTensor<OutputType>(TensorShape{expected_sum_length}, expected_sum));
  // Also ensure that the resulting tensor is dense.
  EXPECT_TRUE(result.value()[0].is_dense());
}

// Test if divide by 0 occurs when the input is 0 and a rescaling factor is
// computed by the aggregator.
TEST(DPGroupingFederatedSumTest, ZeroVectorsCanBeAccumulated) {
  Tensor ordinals =
      Tensor::Create(DT_INT64, {4}, CreateTestData<int64_t>({0, 1, 2, 1}))
          .value();

  Tensor zero_vector_64 =
      Tensor::Create(DT_INT64, {4}, CreateTestData<int64_t>({0, 0, 0, 0}))
          .value();
  MatchSum<int64_t, int64_t>(1000, 3, -1, {{&ordinals, &zero_vector_64}},
                             {0, 0, 0});

  Tensor zero_vector_32 =
      Tensor::Create(DT_INT32, {4}, CreateTestData<int32_t>({0, 0, 0, 0}))
          .value();
  MatchSum<int32_t, int64_t>(1000, 3, -1, {{&ordinals, &zero_vector_32}},
                             {0, 0, 0});

  Tensor zero_vector_f =
      Tensor::Create(DT_FLOAT, {4}, CreateTestData<float>({0, 0, 0, 0}))
          .value();
  MatchSum<float, double>(1000, 3, -1, {{&ordinals, &zero_vector_f}},
                          {0, 0, 0});
  Tensor zero_vector_d =
      Tensor::Create(DT_DOUBLE, {4}, CreateTestData<double>({0, 0, 0, 0}))
          .value();
  MatchSum<double, double>(1000, 3, -1, {{&ordinals, &zero_vector_d}},
                           {0, 0, 0});
}

// The fixture below contains data of varying types that we will re-use.
class ContributionBoundingTester : public testing::Test {
 protected:
  // Alice's local histogram is (3, 5, 4, 0) w/ L1 norm 12 and L2 norm sqrt(50)
  Tensor alice_ordinals_ =
      Tensor::Create(DT_INT64, {4}, CreateTestData<int64_t>({0, 1, 2, 1}))
          .value();
  Tensor alice_32_ =
      Tensor::Create(DT_INT32, {4}, CreateTestData<int32_t>({3, 7, 4, -2}))
          .value();
  Tensor alice_64_ =
      Tensor::Create(DT_INT64, {4}, CreateTestData<int64_t>({3, 7, 4, -2}))
          .value();
  // float ver.: (.3, .5, .4, 0) with L2 norm sqrt(0.5)
  Tensor alice_f_ =
      Tensor::Create(DT_FLOAT, {4}, CreateTestData<float>({.3, .7, .4, -.2}))
          .value();
  Tensor alice_d_ =
      Tensor::Create(DT_DOUBLE, {4}, CreateTestData<double>({.3, .7, .4, -.2}))
          .value();

  // Bob's local histogram is (0, -10, 9, 0) w/ L1 norm 19 and L2 norm sqrt(181)
  Tensor bob_ordinals_ =
      Tensor::Create(DT_INT64, {3}, CreateTestData<int64_t>({2, 1, 1})).value();
  Tensor bob_32_ =
      Tensor::Create(DT_INT32, {3}, CreateTestData<int32_t>({9, -12, 2}))
          .value();
  Tensor bob_64_ =
      Tensor::Create(DT_INT64, {3}, CreateTestData<int64_t>({9, -12, 2}))
          .value();
  // float ver.: (0, -1.0, 0.9, 0) with L2 norm sqrt(1.81)
  Tensor bob_f_ =
      Tensor::Create(DT_FLOAT, {3}, CreateTestData<float>({.9, -1.2, .2}))
          .value();
  Tensor bob_d_ =
      Tensor::Create(DT_DOUBLE, {3}, CreateTestData<double>({.9, -1.2, .2}))
          .value();

  // Cindy's local histogram is (5, -5, 0, 11) w/ L1 norm 21 & L2 norm sqrt(171)
  Tensor cindy_ordinals_ =
      Tensor::Create(DT_INT64, {3}, CreateTestData<int64_t>({3, 1, 0})).value();
  Tensor cindy_32_ =
      Tensor::Create(DT_INT32, {3}, CreateTestData<int32_t>({11, -5, 5}))
          .value();
  Tensor cindy_64_ =
      Tensor::Create(DT_INT64, {3}, CreateTestData<int64_t>({11, -5, 5}))
          .value();
  // float ver.: (.5, -.5, 0, 1.1) with L2 norm sqrt(1.71)
  Tensor cindy_f_ =
      Tensor::Create(DT_FLOAT, {3}, CreateTestData<float>({1.1, -.5, .5}))
          .value();
  Tensor cindy_d_ =
      Tensor::Create(DT_DOUBLE, {3}, CreateTestData<double>({1.1, -.5, .5}))
          .value();
};

// If we give DPGFS loose bounds, data should be unchanged. The output of
// the aggregator should be the raw sum of local histograms.
TEST_F(ContributionBoundingTester, LooseBoundsDoNothing) {
  MatchSum<int32_t, int64_t>(1000, 1000, 1000,
                             {{&alice_ordinals_, &alice_32_},
                              {&bob_ordinals_, &bob_32_},
                              {&cindy_ordinals_, &cindy_32_}},
                             {8, -10, 13, 11});
  MatchSum<int64_t, int64_t>(1000, 1000, 1000,
                             {{&alice_ordinals_, &alice_64_},
                              {&bob_ordinals_, &bob_64_},
                              {&cindy_ordinals_, &cindy_64_}},
                             {8, -10, 13, 11});
  MatchSum<float, double>(1000, 1000, 1000,
                          {{&alice_ordinals_, &alice_f_},
                           {&bob_ordinals_, &bob_f_},
                           {&cindy_ordinals_, &cindy_f_}},
                          {.8, -1.0, 1.3, 1.1});
  MatchSum<double, double>(1000, 1000, 1000,
                           {{&alice_ordinals_, &alice_d_},
                            {&bob_ordinals_, &bob_d_},
                            {&cindy_ordinals_, &cindy_d_}},
                           {.8, -1.0, 1.3, 1.1});
}

// If we give DPGFS nontrivial linfinity bounds, data will be clamped.
TEST_F(ContributionBoundingTester, LinfinityBoundingSucceeds) {
  // If we clamp to 9, then
  // (3, 5, 4, 0) is unchanged
  // (0, -10, 9, 0) becomes (0, -9, 9, 0)
  // (5, -5, 0, 11) becomes (5, -5, 0, 9)
  // for a new sum of 8, -9, 13, 9

  MatchSum<int32_t, int64_t>(9, -1, -1,
                             {{&alice_ordinals_, &alice_32_},
                              {&bob_ordinals_, &bob_32_},
                              {&cindy_ordinals_, &cindy_32_}},
                             {8, -9, 13, 9});
  MatchSum<int64_t, int64_t>(9, -1, -1,
                             {{&alice_ordinals_, &alice_64_},
                              {&bob_ordinals_, &bob_64_},
                              {&cindy_ordinals_, &cindy_64_}},
                             {8, -9, 13, 9});
  MatchSum<float, double>(.9, -1, -1,
                          {{&alice_ordinals_, &alice_f_},
                           {&bob_ordinals_, &bob_f_},
                           {&cindy_ordinals_, &cindy_f_}},
                          {.8, -.9, 1.3, .9});
  MatchSum<double, double>(.9, -1, -1,
                           {{&alice_ordinals_, &alice_d_},
                            {&bob_ordinals_, &bob_d_},
                            {&cindy_ordinals_, &cindy_d_}},
                           {.8, -.9, 1.3, .9});
}

TEST_F(ContributionBoundingTester, L1BoundingSucceeds) {
  // If we force the L1 norms to be <= 20,
  // (3, 5, 4, 0) with L1 norm 12 is unchanged
  MatchSum<int32_t, int64_t>(100, 20, -1, {{&alice_ordinals_, &alice_32_}},
                             {3, 5, 4});
  MatchSum<int64_t, int64_t>(100, 20, -1, {{&alice_ordinals_, &alice_64_}},
                             {3, 5, 4});
  // (0, -10, 9, 0) with L1 norm 19 is unchanged
  MatchSum<int32_t, int64_t>(100, 20, -1, {{&bob_ordinals_, &bob_32_}},
                             {0, -10, 9});
  MatchSum<int64_t, int64_t>(100, 20, -1, {{&bob_ordinals_, &bob_64_}},
                             {0, -10, 9});
  // (5, -5, 0, 11) with L1 norm 21 becomes (5 * 20/21, -5*20/21, 0, 11 * 20/21)
  auto cindy_expected_int = {static_cast<int64_t>(5 * 20.0 / 21.0),
                             static_cast<int64_t>(-5 * 20.0 / 21.0),
                             static_cast<int64_t>(0 * 20.0 / 21.0),
                             static_cast<int64_t>(11 * 20.0 / 21.0)};
  MatchSum<int32_t, int64_t>(100, 20, -1, {{&cindy_ordinals_, &cindy_32_}},
                             cindy_expected_int);
  MatchSum<int64_t, int64_t>(100, 20, -1, {{&cindy_ordinals_, &cindy_64_}},
                             cindy_expected_int);

  // Repeat work for the floating point inputs
  MatchSum<float, double>(100, 2, -1, {{&alice_ordinals_, &alice_f_}},
                          {.3, .5, .4});
  MatchSum<double, double>(100, 2, -1, {{&alice_ordinals_, &alice_d_}},
                           {.3, .5, .4});
  MatchSum<float, double>(100, 2, -1, {{&bob_ordinals_, &bob_f_}}, {0, -1, .9});
  MatchSum<double, double>(100, 2, -1, {{&bob_ordinals_, &bob_d_}},
                           {0, -1, .9});
  auto cindy_expected_double = {.5 * 2.0 / 2.1, -.5 * 2.0 / 2.1, 0.0,
                                1.1 * 2 / 2.1};
  MatchSum<float, double>(100, 2, -1, {{&cindy_ordinals_, &cindy_f_}},
                          cindy_expected_double);
  MatchSum<double, double>(100, 2, -1, {{&cindy_ordinals_, &cindy_d_}},
                           cindy_expected_double);
}

TEST_F(ContributionBoundingTester, L2BoundingSucceeds) {
  // If we force the L2 norms to be <= 12
  // (3, 5, 4, 0) with L2 norm sqrt(50) is unchanged
  MatchSum<int32_t, int64_t>(100, -1, 12, {{&alice_ordinals_, &alice_32_}},
                             {3, 5, 4});
  MatchSum<int64_t, int64_t>(100, -1, 12, {{&alice_ordinals_, &alice_64_}},
                             {3, 5, 4});
  // (0, -10, 9, 0) with L2 norm sqrt(181)
  //  becomes (0, -10*12/sqrt(181), 9 * 12/sqrt(181), 0)
  int64_t zero_int = 0;
  auto bob_expected_int = {zero_int, static_cast<int64_t>(-10 * 12 / sqrt(181)),
                           static_cast<int64_t>(9 * 12 / sqrt(181))};
  MatchSum<int32_t, int64_t>(100, -1, 12, {{&bob_ordinals_, &bob_32_}},
                             bob_expected_int);
  MatchSum<int64_t, int64_t>(100, -1, 12, {{&bob_ordinals_, &bob_64_}},
                             bob_expected_int);
  // (5, -5, 0, 11) with L2 norm sqrt(171)
  //  becomes (5 * 12/sqrt(171), -5 * 12/sqrt(171), 0, 11 * 12/sqrt(171))
  auto cindy_expected_int = {static_cast<int64_t>(5 * 12 / sqrt(171)),
                             static_cast<int64_t>(-5 * 12 / sqrt(171)),
                             zero_int,
                             static_cast<int64_t>(11 * 12 / sqrt(171))};
  MatchSum<int32_t, int64_t>(100, -1, 12, {{&cindy_ordinals_, &cindy_32_}},
                             cindy_expected_int);
  MatchSum<int64_t, int64_t>(100, -1, 12, {{&cindy_ordinals_, &cindy_64_}},
                             cindy_expected_int);

  // Repeat work for the floating point inputs
  double zero_double = 0.0;
  MatchSum<float, double>(100, -1, 1.2, {{&alice_ordinals_, &alice_f_}},
                          {.3, .5, .4});
  MatchSum<double, double>(100, -1, 1.2, {{&alice_ordinals_, &alice_d_}},
                           {.3, .5, .4});

  double bob_l2 = sqrt((-1.0 * -1.0) + (0.9 * 0.9));
  auto bob_expected_double = {zero_double,
                              static_cast<double>(-1.0 * 1.2 / bob_l2),
                              static_cast<double>(.9 * 1.2 / bob_l2)};
  MatchSum<float, double>(100, -1, 1.2, {{&bob_ordinals_, &bob_f_}},
                          bob_expected_double);
  MatchSum<double, double>(100, -1, 1.2, {{&bob_ordinals_, &bob_d_}},
                           bob_expected_double);

  double cindy_l2 = sqrt((.5 * .5) + (-.5 * -.5) + (1.1 * 1.1));
  auto cindy_expected_double = {static_cast<double>(.5 * 1.2 / cindy_l2),
                                static_cast<double>(-.5 * 1.2 / cindy_l2),
                                zero_double,
                                static_cast<double>(1.1 * 1.2 / cindy_l2)};
  MatchSum<float, double>(100, -1, 1.2, {{&cindy_ordinals_, &cindy_f_}},
                          cindy_expected_double);
  MatchSum<double, double>(100, -1, 1.2, {{&cindy_ordinals_, &cindy_d_}},
                           cindy_expected_double);
}

TEST_F(ContributionBoundingTester, AllBoundingSucceeds) {
  // If we clamp to 10 and also force L1 and L2 norms to be <= 20 and <= 12,
  // (3, 5, 4, 0) with L1 norm 12 and L2 norm sqrt(50) is unchanged
  MatchSum<int32_t, int64_t>(10, 20, 12, {{&alice_ordinals_, &alice_32_}},
                             {3, 5, 4});
  MatchSum<int64_t, int64_t>(10, 20, 12, {{&alice_ordinals_, &alice_64_}},
                             {3, 5, 4});
  // (0, -10, 9, 0) with L1 & L2 norms 19 & sqrt(181) is scaled by 12/sqrt(181)
  double scale = 12.0 / sqrt(181);
  int64_t zero_int = 0;
  auto bob_expected_int = {zero_int, static_cast<int64_t>(-10 * scale),
                           static_cast<int64_t>(9 * scale)};
  MatchSum<int32_t, int64_t>(10, 20, 12, {{&bob_ordinals_, &bob_32_}},
                             bob_expected_int);
  MatchSum<int64_t, int64_t>(10, 20, 12, {{&bob_ordinals_, &bob_64_}},
                             bob_expected_int);
  // (5, -5, 0, 11) becomes (5, -5, 0, 10) with L1 & L2 norms 21 & sqrt(150)
  // which gets scaled by min(20/21, 12/sqrt(150))
  scale = std::min(20.0 / 21.0, 12.0 / sqrt(150));
  auto cindy_expected_int = {static_cast<int64_t>(5 * scale),
                             static_cast<int64_t>(-5 * scale), zero_int,
                             static_cast<int64_t>(10 * scale)};
  MatchSum<int32_t, int64_t>(10, 20, 12, {{&cindy_ordinals_, &cindy_32_}},
                             cindy_expected_int);
  MatchSum<int64_t, int64_t>(10, 20, 12, {{&cindy_ordinals_, &cindy_64_}},
                             cindy_expected_int);

  // Repeat work for the floating point inputs
  MatchSum<float, double>(1.0, 2.0, 1.2, {{&alice_ordinals_, &alice_f_}},
                          {.3, .5, .4});
  MatchSum<double, double>(1.0, 2.0, 1.2, {{&alice_ordinals_, &alice_d_}},
                           {.3, .5, .4});
  double zero_double = 0.0;
  double bob_l2 = sqrt((-1.0 * -1.0) + (0.9 * 0.9));
  scale = 1.2 / bob_l2;
  auto bob_expected_double = {zero_double, static_cast<double>(-1.0 * scale),
                              static_cast<double>(.9 * scale)};
  MatchSum<float, double>(1.0, 2.0, 1.2, {{&bob_ordinals_, &bob_f_}},
                          bob_expected_double);
  MatchSum<double, double>(1.0, 2.0, 1.2, {{&bob_ordinals_, &bob_d_}},
                           bob_expected_double);
  double cindy_l1 = .5 - .5 + 1.0;
  double cindy_l2 = sqrt((.5 * .5) + (-.5 * -.5) + (1.0 * 1.0));
  scale = std::min(2.0 / cindy_l1, 1.2 / cindy_l2);
  auto cindy_expected_double = {static_cast<double>(.5 * scale),
                                static_cast<double>(-.5 * scale), zero_double,
                                static_cast<double>(1.0 * scale)};
  MatchSum<float, double>(1.0, 2.0, 1.2, {{&cindy_ordinals_, &cindy_f_}},
                          cindy_expected_double);
  MatchSum<double, double>(1.0, 2.0, 1.2, {{&cindy_ordinals_, &cindy_d_}},
                           cindy_expected_double);
}

// Test merge w/ scalar input (duplicated from grouping_federated_sum_test.cc).
TEST(DPGroupingFederatedSumTest, ScalarMergeSucceeds) {
  auto aggregator1 = CreateTensorAggregator(CreateDefaultIntrinsic()).value();
  auto aggregator2 = CreateTensorAggregator(CreateDefaultIntrinsic()).value();
  Tensor ordinal =
      Tensor::Create(DT_INT64, {}, CreateTestData<int64_t>({0})).value();
  Tensor t1 = Tensor::Create(DT_INT32, {}, CreateTestData({1})).value();
  Tensor t2 = Tensor::Create(DT_INT32, {}, CreateTestData({2})).value();
  Tensor t3 = Tensor::Create(DT_INT32, {}, CreateTestData({3})).value();
  EXPECT_THAT(aggregator1->Accumulate({&ordinal, &t1}), IsOk());
  EXPECT_THAT(aggregator2->Accumulate({&ordinal, &t2}), IsOk());
  EXPECT_THAT(aggregator2->Accumulate({&ordinal, &t3}), IsOk());

  EXPECT_THAT(aggregator1->MergeWith(std::move(*aggregator2)), IsOk());
  EXPECT_THAT(aggregator1->CanReport(), IsTrue());
  EXPECT_THAT(aggregator1->GetNumInputs(), Eq(3));

  auto result = std::move(*aggregator1).Report();
  EXPECT_THAT(result, IsOk());
  EXPECT_THAT(result.value().size(), Eq(1));
  int64_t expected_sum = 6;
  EXPECT_THAT(result.value()[0], IsTensor({1}, {expected_sum}));
}

// Test merge w/ vector input
TEST(DPGroupingFederatedSumTest, VectorMergeSucceeds) {
  auto aggregator1 = CreateTensorAggregator(CreateDefaultIntrinsic()).value();
  Tensor alice_ordinal =
      Tensor::Create(DT_INT64, {4}, CreateTestData<int64_t>({0, 1, 2, 1}))
          .value();
  Tensor alice_values =
      Tensor::Create(DT_INT32, {4}, CreateTestData<int32_t>({3, 7, 4, -2}))
          .value();
  Tensor bob_ordinal =
      Tensor::Create(DT_INT64, {3}, CreateTestData<int64_t>({2, 1, 1})).value();
  Tensor bob_values =
      Tensor::Create(DT_INT32, {3}, CreateTestData<int32_t>({9, -12, 2}))
          .value();
  EXPECT_THAT(aggregator1->Accumulate({&alice_ordinal, &alice_values}), IsOk());
  EXPECT_THAT(aggregator1->Accumulate({&bob_ordinal, &bob_values}), IsOk());

  auto aggregator2 = CreateTensorAggregator(CreateDefaultIntrinsic()).value();
  Tensor cindy_ordinal =
      Tensor::Create(DT_INT64, {3}, CreateTestData<int64_t>({3, 1, 0})).value();
  Tensor cindy_values =
      Tensor::Create(DT_INT32, {3}, CreateTestData<int32_t>({11, -5, 5}))
          .value();
  EXPECT_THAT(aggregator2->Accumulate({&cindy_ordinal, &cindy_values}), IsOk());

  EXPECT_THAT(aggregator1->MergeWith(std::move(*aggregator2)), IsOk());
  EXPECT_THAT(aggregator1->CanReport(), IsTrue());
  EXPECT_THAT(aggregator1->GetNumInputs(), Eq(3));

  auto result = std::move(*aggregator1).Report();
  EXPECT_THAT(result, IsOk());
  EXPECT_THAT(result.value().size(), Eq(1));
  std::initializer_list<int64_t> expected_sum = {8, -10, 13, 11};
  EXPECT_THAT(result.value()[0], IsTensor({4}, expected_sum));
}

TEST(DPGroupingFederatedSumTest, CatchUnsupportedTypes) {
  Intrinsic intrinsic{"GoogleSQL:dp_sum",
                      {CreateTensorSpec("value", DT_UINT64)},
                      {CreateTensorSpec("value", DT_UINT64)},
                      {CreateDPGFSParameters<uint64_t>(1000, -1, -1)},
                      {}};
  Status s = CreateTensorAggregator(intrinsic).status();
  EXPECT_THAT(s, IsCode(INVALID_ARGUMENT));
  EXPECT_THAT(s.message(), HasSubstr("Unsupported input type"));
}

}  // namespace
}  // namespace aggregation
}  // namespace fcp
