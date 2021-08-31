// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lite/core/subgraph/subgraph_bridge_registry.h"
#include "lite/kernels/nnadapter/bridges/converter.h"
#include "lite/kernels/nnadapter/bridges/utility.h"
#include "lite/operators/conv_op.h"

namespace paddle {
namespace lite {
namespace subgraph {
namespace nnadapter {

int ConvTransposeConverter(void* ctx, OpLite* op, KernelBase* kernel) {
  CHECK(ctx != nullptr);
  CHECK(op != nullptr);
  auto converter = static_cast<Converter*>(ctx);
  auto op_info = op->op_info();
  auto op_type = op_info->Type();
  auto scope = op->scope();
  VLOG(3) << "Converting " << op_type << " ...";

  // Get input and output vars and op attributes
  auto input_name = op_info->Input("Input").front();
  auto input_scale_name = "Input0_scale";
  auto has_input_scale = op_info->HasInputScale(input_scale_name, true);
  auto input_scale =
      has_input_scale ? op_info->GetInputScale(input_scale_name, true)[0] : 0.f;
  auto input = scope->FindMutableTensor(input_name);
  auto input_dims = input->dims();
  auto filter_name = op_info->Input("Filter").front();
  auto filter_scale_name = "Filter0_scale";
  auto has_filter_scale = op_info->HasInputScale(filter_scale_name, true);
  auto filter_scale = has_filter_scale
                          ? op_info->GetInputScale(filter_scale_name, true)
                          : std::vector<float>({});
  auto filter = scope->FindMutableTensor(filter_name);
  auto filter_dims = filter->dims();
  auto output_name = op_info->Output("Output").front();
  auto output_scale_name = "Output0_scale";
  auto has_output_scale = op_info->HasOutputScale(output_scale_name, true);
  auto output_scale = has_output_scale
                          ? op_info->GetOutputScale(output_scale_name, true)[0]
                          : 0.f;
  auto output = scope->FindMutableTensor(output_name);
  auto output_dims = output->dims();
  auto batch_size = input_dims[0];
  auto input_channel_size = input_dims[1];
  auto output_channel_size = filter_dims[1];
  CHECK_EQ(input_dims.size(), 4L);
  CHECK_EQ(output_dims.size(), 4L);
  CHECK_EQ(filter_dims.size(), 4L);
  CHECK_EQ(output_dims[0], batch_size);
  CHECK_EQ(filter_dims[0], input_channel_size);
  std::vector<int> strides = op_info->GetAttr<std::vector<int>>("strides");
  std::vector<int> paddings = op_info->GetAttr<std::vector<int>>("paddings");
  auto groups = op_info->GetAttr<int>("groups");
  std::vector<int> dilations = op_info->GetAttr<std::vector<int>>("dilations");
  CHECK_EQ(dilations.size(), 2L);
  bool with_act =
      op_info->HasAttr("with_act") && op_info->GetAttr<bool>("with_act");
  std::string act_type =
      with_act ? op_info->GetAttr<std::string>("act_type") : "";
  std::vector<int> output_size;
  if (op_info->HasAttr("output_size")) {
    output_size = op_info->GetAttr<std::vector<int>>("output_size");
  }
  std::vector<int> output_padding;
  if (op_info->HasAttr("output_padding")) {
    output_padding = op_info->GetAttr<std::vector<int>>("output_padding");
  }
  // Calculate paddings and strides
  CHECK_EQ(strides.size(), 2L);
  if (paddings.size() == 2L) {
    for (size_t i = 0; i < strides.size(); i++) {
      int copy_pad = *(paddings.begin() + 2 * i);
      paddings.insert(paddings.begin() + 2 * i + 1, copy_pad);
    }
  }
  CHECK_EQ(paddings.size(), 4L)
      << "Paddings size should be the same or twice as the input size.";
  std::string padding_algorithm("");
  if (op_info->HasAttr("padding_algorithm")) {
    padding_algorithm = op_info->GetAttr<std::string>("padding_algorithm");
  }
  operators::UpdatePaddingAndDilation(&paddings,
                                      &dilations,
                                      strides,
                                      padding_algorithm,
                                      input_dims,
                                      filter_dims);
  auto fuse_relu =
      op_info->HasAttr("fuse_relu") && op_info->GetAttr<bool>("fuse_relu");
  if (fuse_relu) {
    CHECK(!with_act || (with_act && act_type == "relu"))
        << "There is a conflict between the attribute 'fuse_relu' and "
           "'with_act'.";
    with_act = true;
    act_type = "relu";
  }
  // Caculate output_padding according to output_size, kernel_size, dilations,
  // strides and paddings
  if (!output_size.empty()) {
    CHECK_EQ(output_size.size(), 2);
    output_padding.resize(2);
    for (size_t i = 0; i < 2; i++) {
      int kernel_ext = dilations[i] * (filter_dims[i + 2] - 1) + 1;
      int out_size = (input_dims[i + 2] - 1) * strides[i] + kernel_ext -
                     paddings[i * 2] - paddings[i * 2 + 1];
      CHECK_GE(output_size[i], out_size);
      output_padding[i] = output_size[i] - out_size;
    }
  }

  // Input operand
  NNAdapterOperand* input_operand = nullptr;
  if (converter->HasOperand(input_name)) {
    input_operand = converter->GetOperand(input_name);
  } else {
    if (has_input_scale) {
      input_operand = converter->AddQuant8VariableOperand(
          input_dims, input_scale, input_name);
    } else {
      input_operand =
          converter->AddFloat32VariableOperand(input_dims, input_name);
    }
  }

  // Filter operand
  NNAdapterOperand* filter_operand = nullptr;
  bool is_per_channel = false;
  if (has_filter_scale) {
    is_per_channel = IsPerChannelScales(filter_scale);
    VLOG(5) << "is_per_channel: " << is_per_channel;
    auto filter_data = filter->mutable_data<int8_t>();
    if (is_per_channel) {
      filter_operand = converter->AddQuant8ConstantOperand(filter_data,
                                                           filter_dims,
                                                           &filter_scale[0],
                                                           filter_scale.size(),
                                                           0,
                                                           false);
    } else {
      filter_operand = converter->AddQuant8ConstantOperand(
          filter_data, filter_dims, filter_scale[0], false);
    }
  } else {
    auto filter_data = filter->mutable_data<float>();
    filter_operand =
        converter->AddFloat32ConstantOperand(filter_data, filter_dims, false);
  }

  // Bias
  std::string bias_name = output_name + "_dummy_bias";
  float* bias_data = nullptr;
  if (HasInput(op_info, scope, "Bias")) {
    bias_name = op_info->Input("Bias").front();
    auto bias = scope->FindMutableTensor(bias_name);
    auto bias_dims = bias->dims();
    CHECK((bias_dims.size() == 1 && bias_dims[0] == output_channel_size) ||
          (bias_dims.size() == 2 && bias_dims[0] == 1 &&
           bias_dims[1] == output_channel_size))
        << "The dimensions of bias only supports [C_out], [1, C_out]";
    bias_data = bias->mutable_data<float>();
  }
  DDim bias_dims({output_channel_size});
  NNAdapterOperand* bias_operand = nullptr;
  if (has_input_scale && has_filter_scale) {
    std::vector<float> bias_scale(filter_scale.size());
    for (size_t i = 0; i < filter_scale.size(); i++) {
      bias_scale[i] = input_scale * filter_scale[i];
    }
    std::vector<int32_t> quant_bias_data(output_channel_size, 0);
    if (bias_data) {
      Quantize(bias_data, output_channel_size, bias_scale, &quant_bias_data[0]);
    }
    if (is_per_channel) {
      bias_operand = converter->AddQuant32ConstantOperand(
          &quant_bias_data[0], bias_dims, &bias_scale[0], bias_scale.size());
    } else {
      bias_operand = converter->AddQuant32ConstantOperand(
          &quant_bias_data[0], bias_dims, bias_scale[0]);
    }
  } else {
    if (bias_data) {
      bias_operand =
          converter->AddFloat32ConstantOperand(bias_data, bias_dims, false);
    } else {
      // Dummy bias
      std::vector<float> dummy_bias_data(output_channel_size, 0);
      bias_operand =
          converter->AddFloat32ConstantOperand(&dummy_bias_data[0], bias_dims);
    }
  }

  // Auto_pad operand
  auto auto_pad_operand = converter->AddInt32ConstantOperand(
      static_cast<int32_t>(PaddingAlgorithm2PadCode(padding_algorithm)));

  // Pads operand(optional)
  auto pads_operand = converter->AddInt32ConstantOperand(
      paddings.data(), DDim({static_cast<int64_t>(paddings.size())}));

  // Strides operand
  auto strides_operand = converter->AddInt32ConstantOperand(
      strides.data(), DDim({static_cast<int64_t>(strides.size())}));

  // Group operand
  auto group_operand = converter->AddInt32ConstantOperand(groups);

  // Dilations operand
  auto dilations_operand = converter->AddInt32ConstantOperand(
      dilations.data(), DDim({static_cast<int64_t>(dilations.size())}));

  // Output_padding operand
  NNAdapterOperand* output_padding_operand = nullptr;
  if (!output_padding.empty()) {
    output_padding_operand = converter->AddInt32ConstantOperand(
        output_padding.data(),
        DDim({static_cast<int64_t>(output_padding.size())}));
  }

  // Output_shape operand
  NNAdapterOperand* output_shape_operand = nullptr;
  if (!output_size.empty()) {
    output_shape_operand = converter->AddInt32ConstantOperand(
        output_size.data(), DDim({static_cast<int64_t>(output_size.size())}));
  }

  // Fuse code operand
  int32_t fuse_code_value = NNADAPTER_FUSED_NONE;
  if (act_type == "relu") {
    fuse_code_value = NNADAPTER_FUSED_RELU;
  } else if (act_type == "relu1") {
    fuse_code_value = NNADAPTER_FUSED_RELU1;
  } else if (act_type == "relu6") {
    fuse_code_value = NNADAPTER_FUSED_RELU6;
  } else if (!act_type.empty()) {
    LOG(WARNING) << "Unsupported activation type: " << act_type;
    return FAILED;
  }
  auto fuse_code_operand = converter->AddInt32ConstantOperand(fuse_code_value);

  // Output operand
  NNAdapterOperand* output_operand = nullptr;
  if (has_output_scale) {
    output_operand = converter->AddQuant8VariableOperand(
        output_dims, output_scale, output_name);
  } else {
    output_operand =
        converter->AddFloat32VariableOperand(output_dims, output_name);
  }

  // Conv2D transpose operation
  std::vector<NNAdapterOperand*> input_operands = {input_operand,
                                                   filter_operand,
                                                   bias_operand,
                                                   auto_pad_operand,
                                                   pads_operand,
                                                   strides_operand,
                                                   group_operand,
                                                   dilations_operand,
                                                   output_padding_operand,
                                                   output_shape_operand,
                                                   fuse_code_operand};
  std::vector<NNAdapterOperand*> output_operands = {output_operand};
  converter->AddOperation(
      NNADAPTER_CONV_2D_TRANSPOSE, &input_operands, &output_operands);
  return REBUILD_WHEN_SHAPE_CHANGED;
}

}  // namespace nnadapter
}  // namespace subgraph
}  // namespace lite
}  // namespace paddle

REGISTER_SUBGRAPH_BRIDGE(
    conv2d_transpose,
    kNNAdapter,
    paddle::lite::subgraph::nnadapter::ConvTransposeConverter);
