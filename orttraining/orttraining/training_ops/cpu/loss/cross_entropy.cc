// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "cross_entropy.h"
#include "core/util/math.h"
#include "core/util/math_cpuonly.h"
#include "core/providers/common.h"
#include <unsupported/Eigen/SpecialFunctions>
#include "core/util/math.h"
#include "core/providers/cpu/math/matmul_helper.h"
#include "core/providers/cpu/tensor/transpose.h"
#include "core/providers/cpu/controlflow/scan_utils.h"
#include "gsl/gsl"

namespace onnxruntime {
namespace contrib {

template <typename T>
void ComputeShareSoftmaxCrossEntropyCPU(const int n,
                                        const int d,
                                        const int nd,
                                        const T* logit_data,
                                        T* shifted_logit,
                                        T* log_prob_data) {
  // Find the max in each batch, resulting in a tensor of shape [batch]
  // logit_max = max(logit_data)
  std::vector<T> logit_max(n);
  math::RowwiseMax<T, CPUMathUtil>(n, d, logit_data, logit_max.data(), nullptr);

  // Subtract the max in batch b from every element in batch b.
  // Broadcasts along the batch dimension.
  // shifted_logit = logit_data - logit_max
  gsl::copy(gsl::make_span(logit_data, nd), gsl::make_span(shifted_logit, nd));
  math::SubToCol<T, CPUMathUtil>(n, d, logit_max.data(), shifted_logit, nullptr);

  // exp_shifted_logit = exp(shifted_logit)
  math::Exp<T, CPUMathUtil>(nd, shifted_logit, log_prob_data, nullptr);

  // sum_exp = sum_{class} (exp_shifted_logit)
  float* sum_exp = logit_max.data();
  math::RowwiseSum<T, CPUMathUtil>(n, d, log_prob_data, sum_exp, nullptr);

  // log_sum_exp = log(sum_exp)
  float* log_sum_exp = sum_exp;
  math::Log<T, CPUMathUtil>(n, sum_exp, log_sum_exp, nullptr);

  // log_prob = shifted_logit - log(sum_exp)
  // the subtraction broadcasts along the batch dimension
  gsl::copy(gsl::make_span(shifted_logit, nd), gsl::make_span(log_prob_data, nd));
  math::SubToCol<T, CPUMathUtil>(n, d, log_sum_exp, log_prob_data, nullptr);
}

ONNX_OPERATOR_KERNEL_EX(
    SoftmaxCrossEntropy,
    kMSDomain,
    1,
    kCpuExecutionProvider,
    KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<float>()),
    SoftmaxCrossEntropy<float>);

template <typename T>
Status SoftmaxCrossEntropy<T>::Compute(OpKernelContext* context) const {
  const Tensor& logit = *context->Input<Tensor>(0);
  const Tensor& label = *context->Input<Tensor>(1);

  const TensorShape logit_shape{logit.Shape()};
  const TensorShape label_shape{label.Shape()};

  ORT_ENFORCE(label_shape == logit_shape, "The shape of logit and label is not identical");

  int64_t N = logit_shape.SizeToDimension(logit_shape.NumDimensions() - 1);
  int64_t D = logit_shape[logit_shape.NumDimensions() - 1];
  const int n = gsl::narrow_cast<int>(N);
  const int d = gsl::narrow_cast<int>(D);
  const int nd = gsl::narrow_cast<int>(N * D);

  Tensor* loss = context->Output(0, TensorShape({}));
  Tensor* log_prob = context->Output(1, logit_shape);

  const float* logit_data = logit.template Data<float>();
  const float* label_data = label.template Data<float>();
  float* loss_data = loss->template MutableData<float>();
  float* log_prob_data = log_prob->template MutableData<float>();

  // computation begins here
  std::vector<float> shifted_logit(nd);
  // probability = exp(shifted_logit) / sum(exp(shifted_logit))
  // where shifted_logit = logit - max_logit
  // along classes
  ComputeShareSoftmaxCrossEntropyCPU(n, d, nd, logit_data,
                                     shifted_logit.data(),
                                     log_prob_data);

  // loss = sum(label * log_prob)
  auto& mul = shifted_logit;
  math::Mul<float, CPUMathUtil>(nd, label_data, log_prob_data, mul.data(), nullptr);

  // Sum over batches and classes
  math::Sum<float, CPUMathUtil>(nd, mul.data(), loss_data, nullptr);

  if (reduction_ == ReductionType::MEAN) {
    *loss_data /= -n;
  } else if (reduction_ == ReductionType::SUM) {
    *loss_data *= -1;
  }

  return Status::OK();
}

ONNX_OPERATOR_KERNEL_EX(
    SoftmaxCrossEntropyGrad,
    kMSDomain,
    1,
    kCpuExecutionProvider,
    KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<float>()),
    SoftmaxCrossEntropyGrad<float>);

template <typename T>
Status SoftmaxCrossEntropyGrad<T>::Compute(OpKernelContext* context) const {
  const Tensor& dY = *context->Input<Tensor>(0);
  const Tensor& log_prob = *context->Input<Tensor>(1);
  const Tensor& label = *context->Input<Tensor>(2);

  const TensorShape probability_shape{log_prob.Shape()};
  const TensorShape label_shape{label.Shape()};

  ORT_ENFORCE(label_shape == probability_shape, "The shape of probability and label is not identical");

  int64_t N = probability_shape.SizeToDimension(probability_shape.NumDimensions() - 1);
  const int n = gsl::narrow_cast<int>(N);
  const int nd = gsl::narrow_cast<int>(probability_shape.Size());

  Tensor* d_logit = context->Output(0, probability_shape);

  const float* dY_data = dY.template Data<float>();
  const float* log_prob_data = log_prob.template Data<float>();
  const float* label_data = label.template Data<float>();
  float* d_logit_data = d_logit->template MutableData<float>();

  // computation begins here
  float* probability_data = d_logit_data;
  math::Exp<float, CPUMathUtil>(nd, log_prob_data, probability_data, nullptr);

  // backprop: prob - label
  math::Sub<float, CPUMathUtil>(nd, probability_data, label_data, d_logit_data, nullptr);

  float dY_scaled;
  if (reduction_ == ReductionType::MEAN) {
    dY_scaled = *dY_data / n;
  } else if (reduction_ == ReductionType::SUM) {
    dY_scaled = *dY_data;
  }

  // d_logit = dY * backprop, dY is a scalar
  math::Scale<float, CPUMathUtil>(nd, &dY_scaled, d_logit_data, d_logit_data, nullptr);

  return Status::OK();
}
#define REGISTER_KERNEL_TYPED(OpName, Domain, VER, T1, T2)          \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                    \
      OpName,                                                       \
      Domain,                                                       \
      VER,                                                          \
      T1##_##T2,                                                    \
      kCpuExecutionProvider,                                        \
      KernelDefBuilder()                                            \
          .TypeConstraint("T", DataTypeImpl::GetTensorType<T1>())   \
          .TypeConstraint("T1", DataTypeImpl::GetTensorType<T2>()), \
      OpName<T1, T2>);

REGISTER_KERNEL_TYPED(SoftmaxCrossEntropyLoss, kOnnxDomain, 12, float, int32_t)
REGISTER_KERNEL_TYPED(SoftmaxCrossEntropyLoss, kOnnxDomain, 12, float, int64_t)

template <typename T1, typename T2>
Status SoftmaxCrossEntropyLoss<T1, T2>::Compute(OpKernelContext* context) const {
  const Tensor& logit = *context->Input<Tensor>(0);
  const Tensor& label = *context->Input<Tensor>(1);

  const TensorShape logit_shape{logit.Shape()};
  const TensorShape label_shape{label.Shape()};
  const size_t label_dims = label_shape.NumDimensions();
  ORT_ENFORCE(logit_shape.NumDimensions() == label_dims + 1,
              "logit_shape must be (1 + label_shape)");

  ORT_ENFORCE(label_shape[0] == logit_shape[0], "The shape of logit and label does not match");

  if (label_dims >= 2) {
    for (size_t i = 0; i < label_shape.NumDimensions() - 1; i++) {
      ORT_ENFORCE(label_shape[i + 1] == logit_shape[i + 2], "The shape of logit and label does not match");
    }
  }

  int64_t N = label_shape.Size();
  int64_t D = logit_shape[label_dims];
  const T1* logit_data = logit.template Data<T1>();
  OrtValue transpose_output;
  Tensor transpose_tensor;
  std::vector<int64_t> new_shape;
  AllocatorPtr alloc;
  if (logit_shape.NumDimensions() > 2) {
    ORT_RETURN_IF_ERROR(context->GetTempSpaceAllocator(&alloc));

    new_shape = {logit_shape[0], logit_shape[2], logit_shape[1]};
    transpose_output = scan::detail::AllocateTensorInMLValue(logit.DataType(), new_shape, alloc);

    ORT_RETURN_IF_ERROR(TransposeBase::DoTranspose({0, 2, 1}, logit, *transpose_output.GetMutable<Tensor>()));

    logit_data = (*transpose_output.GetMutable<Tensor>()).template Data<T1>();
    N = logit_shape[0] * logit_shape[2];
    D = logit_shape[1];
  }

  const int n = gsl::narrow_cast<int>(N);
  const int d = gsl::narrow_cast<int>(D);
  const int nd = gsl::narrow_cast<int>(N * D);
  Tensor* loss = context->Output(0, reduction_ == ReductionType::NONE ? TensorShape({label_shape[0]}) : TensorShape({}));

  T1* log_prob_data;
  std::vector<T1> log_prob_data_buffer(0);
  if (context->OutputCount() > 1) {
    log_prob_data = context->Output(1, logit_shape)->template MutableData<T1>();
  } else {
    log_prob_data_buffer.resize(logit_shape.Size());
    log_prob_data = log_prob_data_buffer.data();
  }

  const T2* label_data = label.template Data<T2>();
  T1* loss_data = loss->template MutableData<T1>();

  // computation begins here
  std::vector<T1> shifted_logit(nd);
  ComputeShareSoftmaxCrossEntropyCPU(n, d, nd, logit_data, shifted_logit.data(), log_prob_data);
  std::vector<T1> loss_sample_buffer(0);
  T1* loss_sample;
  if (reduction_ == ReductionType::NONE) {
    loss_sample = loss_data;
  } else {
    loss_sample_buffer.resize(n);
    loss_sample = loss_sample_buffer.data();
  }

  if (OpKernel::Node().InputDefs().size() == 3) {
    const Tensor& weight = *context->Input<Tensor>(2);
    const TensorShape weight_shape{weight.Shape()};
    ORT_ENFORCE(1 == weight_shape.NumDimensions(), "Weights tensor is not 1-D.");
    const T1* weight_data = weight.template Data<T1>();
    T1 sum_weight = (T1)0;

    if (reduction_ == ReductionType::MEAN) {
      for (int i = 0; i < n; i++) {
        loss_sample[i] = -log_prob_data[i * d + label_data[i]] * weight_data[label_data[i]];
        sum_weight += weight_data[label_data[i]];
      }

    } else {
      for (int i = 0; i < n; i++) {
        loss_sample[i] = -log_prob_data[i * d + label_data[i]] * weight_data[label_data[i]];
      }
    }

    if (reduction_ == ReductionType::NONE) {
      return Status::OK();
    } else {
      // Sum loss over n samples
      math::Sum<float, CPUMathUtil>(n, loss_sample, loss_data, nullptr);

      // Average sum_loss over sum_weights
      if (reduction_ == ReductionType::MEAN) {
        *loss_data /= sum_weight;
      }
    }
  } else {
    for (int i = 0; i < n; i++) {
      loss_sample[i] = -log_prob_data[i * d + label_data[i]];
    }

    if (reduction_ == ReductionType::NONE) {
      return Status::OK();
    } else {
      // Sum loss over n samples
      math::Sum<T1, CPUMathUtil>(n, loss_sample, loss_data, nullptr);

      if (reduction_ == ReductionType::MEAN) {
        *loss_data /= n;
      }
    }
  }
  return Status::OK();
}

REGISTER_KERNEL_TYPED(SoftmaxCrossEntropyLossGrad, kMSDomain, 1, float, int32_t)
REGISTER_KERNEL_TYPED(SoftmaxCrossEntropyLossGrad, kMSDomain, 1, float, int64_t)

template <typename T1, typename T2>
Status SoftmaxCrossEntropyLossGrad<T1, T2>::Compute(OpKernelContext* context) const {
  const Tensor& dY = *context->Input<Tensor>(0);
  const Tensor& log_prob = *context->Input<Tensor>(1);
  const Tensor& label = *context->Input<Tensor>(2);

  const TensorShape probability_shape{log_prob.Shape()};
  const TensorShape label_shape{label.Shape()};
  ORT_ENFORCE(probability_shape.NumDimensions() == label_shape.NumDimensions() + 1,
              "probability_shape must be (1 + label_shape)");
  for (size_t i = 0; i < label_shape.NumDimensions(); i++) {
    ORT_ENFORCE(label_shape[i] == probability_shape[i], "The shape of probability and label does not match");
  }

  int64_t N = label_shape.Size();
  int64_t D = probability_shape[probability_shape.NumDimensions() - 1];
  const int n = gsl::narrow_cast<int>(N);
  const int d = gsl::narrow_cast<int>(D);

  Tensor* d_logit = context->Output(0, probability_shape);

  const T1* dY_data = dY.template Data<T1>();
  const T1* log_prob_data = log_prob.template Data<T1>();
  const T2* label_data = label.template Data<T2>();
  T1* d_logit_data = d_logit->template MutableData<T1>();

  // computation begins here
  if (OpKernel::Node().InputDefs().size() == 4) {
    const Tensor& weight = *context->Input<Tensor>(3);
    const TensorShape weight_shape{weight.Shape()};
    ORT_ENFORCE(weight_shape == label_shape, "The shape of weight and label is different");
    const T1* weight_data = weight.template Data<T1>();

    T1 dY_scaled = *dY_data;
    if (reduction_ == ReductionType::MEAN) {
      float sum_weight;
      math::Sum<float, CPUMathUtil>(n, weight_data, &sum_weight, nullptr);
      dY_scaled = *dY_data / sum_weight;
    }

    for (int i = 0; i < n; i++) {
      T2 label_sample = label_data[i];
      T1 weight_smaple = weight_data[i] * dY_scaled;
      for (int j = 0; j < d; j++) {
        int index = i * d + j;
        d_logit_data[index] = (exp(log_prob_data[index]) - (label_sample == j)) * weight_smaple;
      }
    }
  } else {
    T1 dY_scaled = *dY_data;
    if (reduction_ == ReductionType::MEAN) {
      dY_scaled = *dY_data / n;
    }

    for (int i = 0; i < n; i++) {
      T2 label_sample = label_data[i];
      for (int j = 0; j < d; j++) {
        int index = i * d + j;
        d_logit_data[index] = (exp(log_prob_data[index]) - (label_sample == j)) * dY_scaled;
      }
    }
  }

  return Status::OK();
}

}  // namespace contrib
}  // namespace onnxruntime
