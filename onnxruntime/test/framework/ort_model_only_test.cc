// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "core/framework/data_types.h"
#include "core/framework/tensorprotoutils.h"
#include "core/graph/onnx_protobuf.h"
#include "core/session/inference_session.h"
#include "core/graph/model.h"
#include "test/test_environment.h"
#include "test_utils.h"
#include "test/util/include/asserts.h"

#include "gtest/gtest.h"

using namespace std;
using namespace ONNX_NAMESPACE;
using namespace onnxruntime::logging;

namespace onnxruntime {

// InferenceSession wrapper to expose loaded graph.
class InferenceSessionGetGraphWrapper : public InferenceSession {
 public:
  explicit InferenceSessionGetGraphWrapper(const SessionOptions& session_options,
                                           const Environment& env) : InferenceSession(session_options, env) {
  }

  const Graph& GetGraph() const {
    return model_->MainGraph();
  }

  const SessionState& GetSessionState() const {
    return InferenceSession::GetSessionState();
  }
};

namespace test {

struct OrtModelTestInfo {
  std::basic_string<ORTCHAR_T> model_filename;
  std::string logid;
  NameMLValMap inputs;
  std::vector<std::string> output_names;
  std::function<void(const std::vector<OrtValue>&)> output_verifier;
  std::vector<std::pair<std::string, std::string>> configs;
};

void RunOrtModel(const OrtModelTestInfo& test_info) {
  SessionOptions so;
  so.session_logid = test_info.logid;
  for (const auto& config : test_info.configs)
    so.AddConfigEntry(config.first.c_str(), config.second.c_str());

  InferenceSessionGetGraphWrapper session_object{so, GetEnvironment()};
  ASSERT_STATUS_OK(session_object.Load(test_info.model_filename));  // infer type from filename
  ASSERT_STATUS_OK(session_object.Initialize());

  std::vector<OrtValue> fetches;
  ASSERT_STATUS_OK(session_object.Run(test_info.inputs, test_info.output_names, &fetches));
  test_info.output_verifier(fetches);
}

#if !defined(ORT_MINIMAL_BUILD)
// Same Tensor from ONNX and ORT format will have different binary representation, need to compare value by value
static void CompareTensors(const OrtValue& left_value, const OrtValue& right_value) {
  const Tensor& left = left_value.Get<Tensor>();
  const Tensor& right = right_value.Get<Tensor>();

  ASSERT_EQ(left.Shape().GetDims(), right.Shape().GetDims());
  ASSERT_EQ(left.GetElementType(), right.GetElementType());

  if (left.IsDataTypeString()) {
    auto size = left.Shape().Size();
    const auto* left_strings = left.Data<std::string>();
    const auto* right_strings = right.Data<std::string>();

    for (int i = 0; i < size; ++i) {
      EXPECT_EQ(left_strings[i], right_strings[i]) << "Mismatch index:" << i;
    }
  } else {
    ASSERT_EQ(memcmp(left.DataRaw(), right.DataRaw(), left.SizeInBytes()), 0);
  }
}

static void CompareTypeProtos(const TypeProto& left_type_proto, const TypeProto& right_type_proto) {
  ASSERT_EQ(left_type_proto.denotation(), right_type_proto.denotation());

  ASSERT_EQ(left_type_proto.has_tensor_type(), right_type_proto.has_tensor_type());
  ASSERT_EQ(left_type_proto.has_sequence_type(), right_type_proto.has_sequence_type());
  ASSERT_EQ(left_type_proto.has_map_type(), right_type_proto.has_map_type());

  if (left_type_proto.has_tensor_type()) {
    const auto& left_tensor_type = left_type_proto.tensor_type();
    const auto& right_tensor_type = right_type_proto.tensor_type();

    ASSERT_EQ(left_tensor_type.elem_type(), right_tensor_type.elem_type());

    const auto& left_shape = left_tensor_type.shape();
    const auto& right_shape = right_tensor_type.shape();

    ASSERT_EQ(left_shape.dim_size(), right_shape.dim_size());
    for (int i = 0; i < left_shape.dim_size(); i++) {
      const auto& left_dim = left_shape.dim(i);
      const auto& right_dim = right_shape.dim(i);
      ASSERT_EQ(left_dim.has_dim_value(), right_dim.has_dim_value());
      ASSERT_EQ(left_dim.dim_value(), right_dim.dim_value());
      ASSERT_EQ(left_dim.has_dim_param(), right_dim.has_dim_param());
      ASSERT_EQ(left_dim.dim_param(), right_dim.dim_param());
    }
  } else if (left_type_proto.has_sequence_type()) {
    CompareTypeProtos(left_type_proto.sequence_type().elem_type(), right_type_proto.sequence_type().elem_type());
  } else if (left_type_proto.has_map_type()) {
    const auto& left_map = left_type_proto.map_type();
    const auto& right_map = right_type_proto.map_type();
    ASSERT_EQ(left_map.key_type(), right_map.key_type());
    CompareTypeProtos(left_map.value_type(), right_map.value_type());
  } else {
    FAIL();  // We do not support SparseTensor and Opaque for now
  }
}

static void CompareValueInfos(const ValueInfoProto& left, const ValueInfoProto& right) {
  ASSERT_EQ(left.name(), right.name());
  ASSERT_EQ(left.doc_string(), right.doc_string());

  CompareTypeProtos(left.type(), right.type());
}

void CompareGraphAndSessionState(const InferenceSessionGetGraphWrapper& session_object_1,
                                 const InferenceSessionGetGraphWrapper& session_object_2) {
  const auto& graph_1 = session_object_1.GetGraph();
  const auto& graph_2 = session_object_2.GetGraph();

  const auto& session_state_1 = session_object_1.GetSessionState();
  const auto& session_state_2 = session_object_2.GetSessionState();

  const auto& i1 = session_state_1.GetInitializedTensors();
  const auto& i2 = session_state_2.GetInitializedTensors();
  ASSERT_EQ(i1.size(), i2.size());

  for (const auto& pair : i1) {
    auto iter = i2.find(pair.first);
    ASSERT_NE(iter, i2.cend());

    const OrtValue& left = pair.second;
    const OrtValue& right = iter->second;
    CompareTensors(left, right);
  }

  // check all node args are fine
  for (const auto& input : graph_1.GetInputsIncludingInitializers()) {
    const auto& left = *graph_1.GetNodeArg(input->Name());
    const auto* right = graph_2.GetNodeArg(input->Name());
    ASSERT_TRUE(right != nullptr);

    const auto& left_proto = left.ToProto();
    const auto& right_proto = right->ToProto();
    CompareValueInfos(left_proto, right_proto);
  }

  for (const auto& left : graph_1.Nodes()) {
    const auto* right = graph_2.GetNode(left.Index());
    ASSERT_TRUE(right != nullptr);
    const auto& left_outputs = left.OutputDefs();
    const auto& right_outputs = right->OutputDefs();
    ASSERT_EQ(left_outputs.size(), right_outputs.size());

    for (size_t i = 0, end = left_outputs.size(); i < end; ++i) {
      const auto& left_nodearg = *left_outputs[i];
      const auto& right_nodearg = *right_outputs[i];

      if (left_nodearg.Exists()) {
        EXPECT_EQ(left_nodearg.Name(), right_nodearg.Name());
        CompareValueInfos(left_nodearg.ToProto(), right_nodearg.ToProto());
      } else {
        EXPECT_FALSE(right_nodearg.Exists());
      }
    }
  }
}

void SaveAndCompareModels(const std::string& onnx_file, const std::basic_string<ORTCHAR_T>& ort_file) {
  SessionOptions so;
  so.session_logid = "SerializeToOrtFormat";
  so.optimized_model_filepath = ort_file;
  // not strictly necessary - type should be inferred from the filename
  so.AddConfigEntry(ORT_SESSION_OPTIONS_CONFIG_SAVE_MODEL_FORMAT, "ORT");
  InferenceSessionGetGraphWrapper session_object{so, GetEnvironment()};

  // create .ort file during Initialize due to values in SessionOptions
  ASSERT_STATUS_OK(session_object.Load(onnx_file));
  ASSERT_STATUS_OK(session_object.Initialize());

  SessionOptions so2;
  so2.session_logid = "LoadOrtFormat";
  // not strictly necessary - type should be inferred from the filename, but to be sure we're testing what we
  // think we're testing set it.
  so2.AddConfigEntry(ORT_SESSION_OPTIONS_CONFIG_LOAD_MODEL_FORMAT, "ORT");

  // load serialized version
  InferenceSessionGetGraphWrapper session_object2{so2, GetEnvironment()};
  ASSERT_STATUS_OK(session_object2.Load(ort_file));
  ASSERT_STATUS_OK(session_object2.Initialize());

  CompareGraphAndSessionState(session_object, session_object2);
}

TEST(OrtModelOnlyTests, SerializeToOrtFormat) {
  const std::basic_string<ORTCHAR_T> ort_file = ORT_TSTR("ort_github_issue_4031.onnx.ort");
  SaveAndCompareModels("testdata/ort_github_issue_4031.onnx", ort_file);

  OrtModelTestInfo test_info;
  test_info.model_filename = ort_file;
  test_info.logid = "SerializeToOrtFormat";
  test_info.configs.push_back(std::make_pair(ORT_SESSION_OPTIONS_CONFIG_LOAD_MODEL_FORMAT, "ORT"));

  OrtValue ml_value;
  CreateMLValue<float>(TestCPUExecutionProvider()->GetAllocator(0, OrtMemTypeDefault), {1}, {123.f},
                       &ml_value);
  test_info.inputs.insert(std::make_pair("state_var_in", ml_value));

  // prepare outputs
  test_info.output_names = {"state_var_out"};
  test_info.output_verifier = [](const std::vector<OrtValue>& fetches) {
    const auto& output = fetches[0].Get<Tensor>();
    ASSERT_TRUE(output.Shape().Size() == 1);
    ASSERT_TRUE(output.Data<float>()[0] == 125.f);
  };

  RunOrtModel(test_info);
}

#if !defined(DISABLE_ML_OPS)
TEST(OrtModelOnlyTests, SerializeToOrtFormatMLOps) {
  const std::basic_string<ORTCHAR_T> ort_file = ORT_TSTR("sklearn_bin_voting_classifier_soft_converted.ort");
  SaveAndCompareModels("testdata/sklearn_bin_voting_classifier_soft.onnx", ort_file);

  OrtModelTestInfo test_info;
  test_info.model_filename = ort_file;
  test_info.logid = "SerializeToOrtFormatMLOps";
  test_info.configs.push_back(std::make_pair(ORT_SESSION_OPTIONS_CONFIG_LOAD_MODEL_FORMAT, "ORT"));

  OrtValue ml_value;
  CreateMLValue<float>(TestCPUExecutionProvider()->GetAllocator(0, OrtMemTypeDefault), {3, 2},
                       {0.f, 1.f, 1.f, 1.f, 2.f, 0.f}, &ml_value);
  test_info.inputs.insert(std::make_pair("input", ml_value));

  // prepare outputs
  test_info.output_names = {"output_label", "output_probability"};
  test_info.output_verifier = [](const std::vector<OrtValue>& fetches) {
    const auto& output_0 = fetches[0].Get<Tensor>();
    int64_t tensor_size = 3;
    ASSERT_EQ(tensor_size, output_0.Shape().Size());
    const auto& output_0_data = output_0.Data<std::string>();
    for (int64_t i = 0; i < tensor_size; i++)
      ASSERT_TRUE(output_0_data[i] == "A");

    VectorMapStringToFloat expected_output_1 = {{{"A", 0.572734f}, {"B", 0.427266f}},
                                                {{"A", 0.596016f}, {"B", 0.403984f}},
                                                {{"A", 0.656315f}, {"B", 0.343685f}}};
    const auto& actual_output_1 = fetches[1].Get<VectorMapStringToFloat>();
    ASSERT_EQ(actual_output_1.size(), 3);
    for (size_t i = 0; i < 3; i++) {
      const auto& expected = expected_output_1[i];
      const auto& actual = actual_output_1[i];
      ASSERT_EQ(actual.size(), 2);
      ASSERT_NEAR(expected.at("A"), actual.at("A"), 1e-6);
      ASSERT_NEAR(expected.at("B"), actual.at("B"), 1e-6);
    }
  };

  RunOrtModel(test_info);
}
#endif  // #if !defined(DISABLE_ML_OPS)
#endif  // #if !defined(ORT_MINIMAL_BUILD)

// test that we can deserialize and run a previously saved ORT format model
TEST(OrtModelOnlyTests, LoadOrtFormatModel) {
  OrtModelTestInfo test_info;
  test_info.model_filename = ORT_TSTR("testdata/ort_github_issue_4031.onnx.ort");
  test_info.logid = "LoadOrtFormatModel";

  OrtValue ml_value;
  CreateMLValue<float>(TestCPUExecutionProvider()->GetAllocator(0, OrtMemTypeDefault), {1}, {123.f},
                       &ml_value);
  test_info.inputs.insert(std::make_pair("state_var_in", ml_value));

  // prepare outputs
  test_info.output_names = {"state_var_out"};
  test_info.output_verifier = [](const std::vector<OrtValue>& fetches) {
    const auto& output = fetches[0].Get<Tensor>();
    ASSERT_TRUE(output.Shape().Size() == 1);
    ASSERT_TRUE(output.Data<float>()[0] == 125.f);
  };

  RunOrtModel(test_info);
}

#if !defined(DISABLE_ML_OPS)
// test that we can deserialize and run a previously saved ORT format model
// for a model with sequence and map outputs
TEST(OrtModelOnlyTests, LoadOrtFormatModelMLOps) {
  OrtModelTestInfo test_info;
  test_info.model_filename = ORT_TSTR("testdata/sklearn_bin_voting_classifier_soft.ort");
  test_info.logid = "LoadOrtFormatModelMLOps";

  OrtValue ml_value;
  CreateMLValue<float>(TestCPUExecutionProvider()->GetAllocator(0, OrtMemTypeDefault), {3, 2},
                       {0.f, 1.f, 1.f, 1.f, 2.f, 0.f}, &ml_value);
  test_info.inputs.insert(std::make_pair("input", ml_value));

  // prepare outputs
  test_info.output_names = {"output_label", "output_probability"};
  test_info.output_verifier = [](const std::vector<OrtValue>& fetches) {
    const auto& output_0 = fetches[0].Get<Tensor>();
    int64_t tensor_size = 3;
    ASSERT_EQ(tensor_size, output_0.Shape().Size());
    const auto& output_0_data = output_0.Data<std::string>();
    for (int64_t i = 0; i < tensor_size; i++)
      ASSERT_TRUE(output_0_data[i] == "A");

    VectorMapStringToFloat expected_output_1 = {{{"A", 0.572734f}, {"B", 0.427266f}},
                                                {{"A", 0.596016f}, {"B", 0.403984f}},
                                                {{"A", 0.656315f}, {"B", 0.343685f}}};
    const auto& actual_output_1 = fetches[1].Get<VectorMapStringToFloat>();
    ASSERT_EQ(actual_output_1.size(), 3);
    for (size_t i = 0; i < 3; i++) {
      const auto& expected = expected_output_1[i];
      const auto& actual = actual_output_1[i];
      ASSERT_EQ(actual.size(), 2);
      ASSERT_NEAR(expected.at("A"), actual.at("A"), 1e-6);
      ASSERT_NEAR(expected.at("B"), actual.at("B"), 1e-6);
    }
  };

  RunOrtModel(test_info);
}
#endif

}  // namespace test
}  // namespace onnxruntime