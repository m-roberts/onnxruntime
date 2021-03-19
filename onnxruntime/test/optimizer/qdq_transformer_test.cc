// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/graph/model.h"
#include "core/graph/onnx_protobuf.h"
#include "core/mlas/inc/mlas.h"
#include "core/session/environment.h"
#include "core/session/inference_session.h"
#include "test/compare_ortvalue.h"
#include "test/test_environment.h"
#include "test/framework/test_utils.h"
#include "test/util/include/inference_session_wrapper.h"

#include "gtest/gtest.h"
#include "graph_transform_test_builder.h"

namespace onnxruntime {
namespace test {

template <typename T>
typename std::enable_if<IsTypeQuantLinearCompatible<T>::value, NodeArg*>::type
AddQDQNodePair(ModelTestBuilder& builder, NodeArg* q_input, float scale, T zp) {
  auto* q_output = builder.MakeIntermediate();
  auto* dq_output = builder.MakeIntermediate();
  builder.AddQuantizeLinearNode<T>(q_input, scale, zp, q_output);
  builder.AddDequantizeLinearNode<T>(q_output, scale, zp, dq_output);
  return dq_output;
}

#ifndef DISABLE_CONTRIB_OPS

TEST(QDQTransformerTests, Conv) {
  auto test_case = [&](const std::vector<int64_t>& input_shape, const std::vector<int64_t>& weights_shape) {
    auto build_test_case = [&](ModelTestBuilder& builder) {
      auto* input_arg = builder.MakeInput<float>(input_shape, -1.f, 1.f);
      auto* output_arg = builder.MakeOutput();

      auto* conv_output = builder.MakeIntermediate();
      auto* weight = builder.MakeInitializer<uint8_t>(weights_shape, 0, 255);

      auto* dq_w_output = builder.MakeIntermediate();
      auto* dq_output = AddQDQNodePair<uint8_t>(builder, input_arg, .004f, 129);
      builder.AddDequantizeLinearNode<uint8_t>(weight, .003f, 118, dq_w_output);
      builder.AddConvNode(dq_output, dq_w_output, conv_output);
      builder.AddQuantizeLinearNode<uint8_t>(conv_output, .0039f, 135, output_arg);
    };

    auto check_conv_graph = [&](InferenceSessionWrapper& session) {
      auto op_to_count = CountOpsInGraph(session.GetGraph());
      EXPECT_EQ(op_to_count["QLinearConv"], 1);
      EXPECT_EQ(op_to_count["QuantizeLinear"], 1);
      EXPECT_EQ(op_to_count["DequantizeLinear"], 0);
    };

    TransformerTester(build_test_case, check_conv_graph, TransformerLevel::Level1, TransformerLevel::Level2);
  };

  // Test the basic case of a single 1D/2D/3D convolution.
  test_case({1, 12, 37}, {32, 12, 5});
  test_case({1, 23, 13, 13}, {30, 23, 3, 3});
  test_case({1, 22, 11, 13, 15}, {30, 22, 5, 3, 3});
}

TEST(QDQTransformerTests, ConvMaxPoolReshape) {
  auto test_case = [&](const std::vector<int64_t>& input_shape, const std::vector<int64_t>& weights_shape) {
    auto build_test_case = [&](ModelTestBuilder& builder) {
      auto* input_arg = builder.MakeInput<float>(input_shape, -1.f, 1.f);
      auto* output_arg = builder.MakeOutput();
      auto* weight = builder.MakeInitializer<uint8_t>(weights_shape, 0, 255);

      // add QDQ + Conv
      auto* dq_w_output = builder.MakeIntermediate();
      auto* conv_output = builder.MakeIntermediate();
      auto* dq_conv_output = AddQDQNodePair<uint8_t>(builder, input_arg, .004f, 129);
      builder.AddDequantizeLinearNode<uint8_t>(weight, .003f, 118, dq_w_output);
      builder.AddConvNode(dq_conv_output, dq_w_output, conv_output);

      // add QDQ + MaxPool
      auto* dq_maxpool_output = AddQDQNodePair<uint8_t>(builder, conv_output, .0039f, 135);
      auto* maxpool_output = builder.MakeIntermediate();
      Node& pool_node = builder.AddNode("MaxPool", {dq_maxpool_output}, {maxpool_output});
      std::vector<int64_t> pads((weights_shape.size() - 2) * 2, 1);
      pool_node.AddAttribute("pads", pads);
      std::vector<int64_t> kernel_shape(weights_shape.size() - 2, 3);
      pool_node.AddAttribute("kernel_shape", kernel_shape);

      // add QDQ + Reshape
      auto* dq_reshape_output = AddQDQNodePair<uint8_t>(builder, maxpool_output, .0039f, 135);
      auto* reshape_shape = builder.Make1DInitializer<int64_t>({-1});
      auto* reshape_output = builder.MakeIntermediate();
      builder.AddNode("Reshape", {dq_reshape_output, reshape_shape}, {reshape_output});

      // add Q
      builder.AddQuantizeLinearNode<uint8_t>(reshape_output, .0039f, 135, output_arg);
    };

    auto check_mp_reshape_graph = [&](InferenceSessionWrapper& session) {
      auto op_to_count = CountOpsInGraph(session.GetGraph());
      EXPECT_EQ(op_to_count["QLinearConv"], 1);
      EXPECT_EQ(op_to_count["MaxPool"], 1);
      EXPECT_EQ(op_to_count["Reshape"], 1);
      EXPECT_EQ(op_to_count["QuantizeLinear"], 1);
      EXPECT_EQ(op_to_count["DequantizeLinear"], 0);
    };

    TransformerTester(build_test_case, check_mp_reshape_graph, TransformerLevel::Level1, TransformerLevel::Level2);
  };

  // Test the basic case of a single 1D/2D/3D convolution.
  test_case({1, 12, 37}, {32, 12, 5});
  test_case({1, 23, 13, 13}, {30, 23, 3, 3});
  test_case({1, 22, 11, 13, 15}, {30, 22, 5, 3, 3});
}

TEST(QDQTransformerTests, Add) {
  auto test_case = [&](const std::vector<int64_t>& input_shape) {
    auto build_test_case = [&](ModelTestBuilder& builder) {
      auto* input1_arg = builder.MakeInput<float>(input_shape, -1.f, 1.f);
      auto* input2_arg = builder.MakeInput<float>(input_shape, -1.f, 1.f);
      auto* output_arg = builder.MakeOutput();

      // add QDQ + Add
      auto* add_output = builder.MakeIntermediate();
      auto* dq_add_output1 = AddQDQNodePair<uint8_t>(builder, input1_arg, .004f, 129);
      auto* dq_add_output2 = AddQDQNodePair<uint8_t>(builder, input2_arg, .004f, 129);
      builder.AddNode("Add", {dq_add_output1, dq_add_output2}, {add_output});

      // add Q
      builder.AddQuantizeLinearNode<uint8_t>(add_output, .0039f, 135, output_arg);
    };

    auto check_add_graph = [&](InferenceSessionWrapper& session) {
      auto op_to_count = CountOpsInGraph(session.GetGraph());
      EXPECT_EQ(op_to_count["com.microsoft.QLinearAdd"], 1);
      EXPECT_EQ(op_to_count["QuantizeLinear"], 2);
      EXPECT_EQ(op_to_count["DequantizeLinear"], 0);
    };

    TransformerTester(build_test_case, check_add_graph, TransformerLevel::Level1, TransformerLevel::Level2);
  };

  // Test the basic case of a single 1D/2D/3D convolution.
  test_case({1, 12, 37});
  test_case({1, 23, 13, 13});
  test_case({1, 22, 11, 13, 15});
}

TEST(QDQTransformerTests, Mul) {
  auto test_case = [&](const std::vector<int64_t>& input_shape) {
    auto build_test_case = [&](ModelTestBuilder& builder) {
      auto* input1_arg = builder.MakeInput<float>(input_shape, -1.f, 1.f);
      auto* input2_arg = builder.MakeInput<float>(input_shape, -1.f, 1.f);
      auto* output_arg = builder.MakeOutput();

      // add QDQ + Mul
      auto* mul_output = builder.MakeIntermediate();
      auto* dq_mul_output1 = AddQDQNodePair<uint8_t>(builder, input1_arg, .004f, 129);
      auto* dq_mul_output2 = AddQDQNodePair<uint8_t>(builder, input2_arg, .004f, 129);
      builder.AddNode("Mul", {dq_mul_output1, dq_mul_output2}, {mul_output});

      // add Q
      builder.AddQuantizeLinearNode<uint8_t>(mul_output, .0039f, 135, output_arg);
    };

    auto check_mul_graph = [&](InferenceSessionWrapper& session) {
      auto op_to_count = CountOpsInGraph(session.GetGraph());
      EXPECT_EQ(op_to_count["com.microsoft.QLinearMul"], 1);
      EXPECT_EQ(op_to_count["QuantizeLinear"], 2);
      EXPECT_EQ(op_to_count["DequantizeLinear"], 0);
    };

    TransformerTester(build_test_case, check_mul_graph, TransformerLevel::Level1, TransformerLevel::Level2);
  };

  // Test the basic case of a single 1D/2D/3D convolution.
  test_case({1, 12, 37});
  test_case({1, 23, 13, 13});
  test_case({1, 22, 11, 13, 15});
}

TEST(QDQTransformerTests, MatMul) {
  auto test_case = [&](const std::vector<int64_t>& input1_shape, const std::vector<int64_t>& input2_shape) {
    auto build_test_case = [&](ModelTestBuilder& builder) {
      auto* input1_arg = builder.MakeInput<float>(input1_shape, -1.f, 1.f);
      auto* input2_arg = builder.MakeInput<float>(input2_shape, -1.f, 1.f);
      auto* output_arg = builder.MakeOutput();

      // add QDQ + MatMul
      auto* matmul_output = builder.MakeIntermediate();
      auto* dq_matmul_output1 = AddQDQNodePair<uint8_t>(builder, input1_arg, .004f, 129);
      auto* dq_matmul_output2 = AddQDQNodePair<uint8_t>(builder, input2_arg, .004f, 129);
      builder.AddNode("MatMul", {dq_matmul_output1, dq_matmul_output2}, {matmul_output});

      // add Q
      builder.AddQuantizeLinearNode<uint8_t>(matmul_output, .0039f, 135, output_arg);
    };

    auto check_matmul_graph = [&](InferenceSessionWrapper& session) {
      auto op_to_count = CountOpsInGraph(session.GetGraph());
      EXPECT_EQ(op_to_count["QLinearMatMul"], 1);
      EXPECT_EQ(op_to_count["QuantizeLinear"], 2);
      EXPECT_EQ(op_to_count["DequantizeLinear"], 0);
    };

    TransformerTester(build_test_case, check_matmul_graph, TransformerLevel::Level1, TransformerLevel::Level2);
  };

  // Test the basic case of a single 1D/2D/3D convolution.
  test_case({12, 37}, {37, 12});
  test_case({23, 13, 13}, {13, 13});
  test_case({22, 11, 13, 15}, {15, 13});
}

#endif  // DISABLE_CONTRIB_OPS

}  // namespace test
}  // namespace onnxruntime