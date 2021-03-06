/**
 * Copyright 2020 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/runtime/kernel/npu/resize_npu.h"
#include <memory>
#include "include/graph/op/all_ops.h"
#include "src/kernel_registry.h"
#include "src/runtime/agent/npu/npu_converter_utils.h"

using mindspore::kernel::KERNEL_ARCH::kNPU;
using mindspore::lite::KernelRegistrar;
using mindspore::schema::PrimitiveType_Resize;

namespace mindspore::kernel {
int ResizeNPUKernel::IsSupport(const std::vector<lite::Tensor *> &inputs, const std::vector<lite::Tensor *> &outputs,
                               OpParameter *opParameter) {
  if (method_ != schema::ResizeMethod_LINEAR || method_ == schema::ResizeMethod_NEAREST) {
    MS_LOG(WARNING) << "Unsupported resize method type:" << method_;
    return RET_ERROR;
  }
  return RET_OK;
}

int ResizeNPUKernel::SetNPUInputs(const std::vector<lite::Tensor *> &inputs, const std::vector<lite::Tensor *> &outputs,
                                  const std::vector<ge::Operator *> &npu_inputs) {
  auto ret = SetPreTranspose(npu_inputs[0]);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "New pre transpose npu operator (NHWC -> NCHW) for op " << name_ << " failed.";
    return RET_ERROR;
  }

  ge::TensorDesc sizeTensorDesc(ge::Shape({2}), ge::FORMAT_NCHW, ge::DT_INT32);
  ge::TensorPtr sizeTensor = std::make_shared<hiai::Tensor>(sizeTensorDesc);
  vector<int32_t> dataValue = {static_cast<int32_t>(new_height_), static_cast<int32_t>(new_width_)};
  sizeTensor->SetData(reinterpret_cast<uint8_t *>(dataValue.data()), 2 * sizeof(int32_t));
  auto out_size = new (std::nothrow) hiai::op::Const(name_ + "_size");
  out_size->set_attr_value(sizeTensor);
  if (method_ == schema::ResizeMethod_LINEAR) {
    auto op = new (std::nothrow) hiai::op::ResizeBilinearV2(name_);
    if (op == nullptr) {
      MS_LOG(ERROR) << " op is nullptr.";
      return RET_ERROR;
    }
    op->set_attr_align_corners(align_corners_);
    op->set_input_x(*pre_trans_);
    op->set_input_size(*out_size);
    op->set_attr_half_pixel_centers(preserve_aspect_ratio_);
    op_ = op;
  } else {
    auto op = new (std::nothrow) hiai::op::ResizeNearestNeighborV2(name_);
    if (op == nullptr) {
      MS_LOG(ERROR) << " op is nullptr.";
      return RET_ERROR;
    }
    op->set_attr_align_corners(align_corners_);
    op->set_input_x(*pre_trans_);
    op->set_input_size(*out_size);
    op_ = op;
  }

  ret = SetPostTranspose(op_);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "New post transpose npu operator (NCHW -> NHWC) for op " << name_ << " failed.";
    return RET_ERROR;
  }

  return RET_OK;
}

ge::Operator *mindspore::kernel::ResizeNPUKernel::GetNPUOp() { return this->post_trans_; }

ResizeNPUKernel::~ResizeNPUKernel() {
  if (op_ != nullptr) {
    delete op_;
    op_ = nullptr;
  }
}
REG_KERNEL(kNPU, kNumberTypeFloat32, PrimitiveType_Resize, NPUKernelCreator<ResizeNPUKernel>)
}  // namespace mindspore::kernel
