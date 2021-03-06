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

#include "src/runtime/kernel/opencl/opencl_kernel.h"
#include "src/runtime/kernel/arm/base/dequant.h"

using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_OK;

namespace mindspore::kernel {

int OpenCLKernel::AlignGlobalLocal(const std::vector<size_t> &global, const std::vector<size_t> &local) {
  std::vector<size_t> internal_global_ws = global;
  for (size_t i = 0; i < local.size(); ++i) {
    internal_global_ws.at(i) = UP_ROUND(global.at(i), local.at(i));
  }

  MS_LOG(DEBUG) << "global size: " << global.size() << ", local size: " << local.size();
  for (size_t i = 0; i < global.size(); i++) {
    MS_LOG(DEBUG) << "global[" << i << "] = " << global.at(i);
  }
  for (size_t i = 0; i < local.size(); i++) {
    MS_LOG(DEBUG) << "local[" << i << "] = " << local.at(i);
  }
  if (local.empty()) {
    local_range_ = cl::NullRange;
  }
  if (global.size() == 1) {
    global_range_ = cl::NDRange(internal_global_ws.at(0));
    if (!local.empty()) {
      local_range_ = cl::NDRange(local.at(0));
    }
  } else if (global.size() == 2) {
    global_range_ = cl::NDRange(internal_global_ws.at(0), internal_global_ws.at(1));
    if (!local.empty()) {
      local_range_ = cl::NDRange(local.at(0), local.at(1));
    }
  } else if (global.size() == 3) {
    global_range_ = cl::NDRange(internal_global_ws.at(0), internal_global_ws.at(1), internal_global_ws.at(2));
    if (!local.empty()) {
      local_range_ = cl::NDRange(local.at(0), local.at(1), local.at(2));
    }
  } else {
    MS_LOG(ERROR) << "Not supported NDRange!";
    return RET_ERROR;
  }
  return RET_OK;
}

int OpenCLKernel::GetImageSize(size_t idx, std::vector<size_t> *img_size) {
  MS_ASSERT(img_size);
  if (idx >= out_tensors_.size()) {
    return RET_ERROR;
  }
  auto img_info = GpuTensorInfo(out_tensors_[idx]);
  size_t img_dtype = ocl_runtime_->GetFp16Enable() ? CL_HALF_FLOAT : CL_FLOAT;
  *img_size = {img_info.width, img_info.height, img_dtype};
  return RET_OK;
}

int OpenCLKernel::PostProcess() {
  for (auto *output : this->out_tensors()) {
    MS_ASSERT(output != nullptr);
    output->ResetRefCount();
  }
  return FreeInWorkTensor();
}

std::vector<BaseTuningParameter> OpenCLKernel::GenerateTuningParam() {
  size_t ndim = global_size_.size();
  std::vector<BaseTuningParameter> tuning_params = {};
  if (ndim == 0) {
    MS_LOG(ERROR) << "Generate tuning param failed, global_size_ is null.";
    return tuning_params;
  }
  BaseTuningParameter default_tuning_param = BaseTuningParameter();
  default_tuning_param.local_size = local_size_;
  tuning_params.push_back(default_tuning_param);
  std::vector<size_t> max_work_items = ocl_runtime_->GetWorkItemSize();
  size_t max_workgroup_size = ocl_runtime_->GetMaxWorkGroupSize(kernel_);
  const size_t MIN_WORKGROUP_SIZE = 8;
  std::set<size_t> candidate_x = GenerateLocalByGlobal(global_size_[0]);
  std::set<size_t> candidate_y = {1};
  std::set<size_t> candidate_z = {1};
  if (ndim > 1) {
    candidate_y = GenerateLocalByGlobal(global_size_[1]);
  }
  if (ndim > 2) {
    candidate_z = GenerateLocalByGlobal(global_size_[2]);
  }
  for (auto x : candidate_x) {
    if (x <= max_work_items[0]) {
      for (auto y : candidate_y) {
        if (y <= max_work_items[1]) {
          for (auto z : candidate_z) {
            auto group_size = x * y * z;
            if (z <= max_work_items[2] && group_size <= max_workgroup_size && group_size >= MIN_WORKGROUP_SIZE) {
              BaseTuningParameter tuning_param = BaseTuningParameter();
              tuning_param.local_size = {x, y, z};
              tuning_params.push_back(tuning_param);
            }
          }
        }
      }
    }
  }
  return tuning_params;
}

int OpenCLKernel::AssignTuningParam(const BaseTuningParameter &param) {
  std::vector<size_t> local_size_tmp = param.local_size;
  if (local_size_tmp.size() > global_size_.size()) {
    local_size_tmp = std::vector<size_t>(local_size_tmp.begin(), local_size_tmp.begin() + global_size_.size());
  }
  AlignGlobalLocal(global_size_, local_size_tmp);
  return RET_OK;
}

int OpenCLKernel::Tune() {
  if (!ocl_runtime_->isProfiling()) {
    MS_LOG(WARNING) << "Tuning mode require opencl runtime profiling.";
    return RET_OK;
  }
  lite::opencl::TuningMode mode = ocl_runtime_->GetTuningMode();
  if (mode == lite::opencl::TuningMode::DEFAULT) {
    return RET_OK;
  }
  static const std::set<int> FAST_MODE_OPS = {schema::PrimitiveType_Conv2D, schema::PrimitiveType_DepthwiseConv2D,
                                              schema::PrimitiveType_DeConv2D};
  if (mode == lite::opencl::TuningMode::FAST && FAST_MODE_OPS.find(op_parameter_->type_) == FAST_MODE_OPS.end()) {
    return RET_OK;
  }
  auto tuning_params = GenerateTuningParam();
  if (tuning_params.empty()) {
    MS_LOG(WARNING) << "Tuning param size is 0.";
    return RET_OK;
  }
  int index = -1;
  double min_time = MAX_PROFILING_TIME_MILLI_SECOND;
  for (int i = 0; i < tuning_params.size(); i++) {
    AssignTuningParam(tuning_params[i]);
    auto ret = Run();
    if (ret != RET_OK) {
      MS_LOG(ERROR) << "Tuning " << name() << " failed for tuning param " << tuning_params[i];
      return ret;
    }
    double current_time = GetProfilingTimeMs();
    MS_LOG(DEBUG) << "Tuning " << name() << " param (" << tuning_params[i] << ") exectime " << current_time << "ms";
    if (current_time < min_time) {
      min_time = current_time;
      index = i;
    }
  }
  if (index != -1) {
    MS_LOG(INFO) << "Tuning " << name() << " result: param (" << tuning_params[index] << ") exectime " << min_time
                 << "ms";
    AssignTuningParam(tuning_params[index]);
  } else {
    MS_LOG(WARNING) << "Cannot find suitable param.";
  }
  return RET_OK;
}

double OpenCLKernel::GetProfilingTimeMs() {
  if (!ocl_runtime_->isProfiling()) {
    return MAX_PROFILING_TIME_MILLI_SECOND;
  }
  cl_ulong time_start;
  cl_ulong time_end;
  event_.getProfilingInfo(CL_PROFILING_COMMAND_START, &time_start);
  event_.getProfilingInfo(CL_PROFILING_COMMAND_END, &time_end);
  cl_ulong time_ns = time_end - time_start;
  return static_cast<double>(time_ns) * 1e-6;
}

std::set<size_t> OpenCLKernel::GenerateLocalByGlobal(size_t global_i) {
  std::set<size_t> local_ = {};
  int index = 1;
  while (index <= global_i) {
    local_.insert(index);
    index *= 2;
  }
  for (size_t i = 1; i <= 16; i++) {
    if (global_i % i == 0) {
      local_.insert(i);
    }
  }
  return local_;
}
int OpenCLKernel::DequantWeight() {
  bool is_fp16 = ocl_runtime_->GetFp16Enable();
  auto *weight_tensor = in_tensors_.at(kWeightIndex);
  auto *restore_data = weight_tensor->data_c();
  dequant_flag_ =
    !weight_tensor->quant_params().empty() && weight_tensor->quant_params().front().inited && restore_data != nullptr;
  if (dequant_flag_) {
    void *dequant_weight{nullptr};
    bool set_flag{true};
    if (is_fp16) {
      if (in_tensors_.at(kWeightIndex)->data_type() == kNumberTypeInt8) {
        dequant_weight = kernel::DequantUtil::DequantData<int8_t, float16_t>(weight_tensor);
      } else if (in_tensors_.at(kWeightIndex)->data_type() == kNumberTypeInt16) {
        dequant_weight = kernel::DequantUtil::DequantData<int16_t, float16_t>(weight_tensor);
      } else {
        set_flag = false;
      }
    } else {
      if (in_tensors_.at(kWeightIndex)->data_type() == kNumberTypeInt8) {
        dequant_weight = kernel::DequantUtil::DequantData<int8_t, float>(weight_tensor);
      } else if (in_tensors_.at(kWeightIndex)->data_type() == kNumberTypeInt16) {
        dequant_weight = kernel::DequantUtil::DequantData<int16_t, float>(weight_tensor);
      } else {
        set_flag = false;
      }
    }
    if (set_flag && dequant_weight == nullptr) {
      MS_LOG(ERROR) << "dequant data failed.";
      return RET_ERROR;
    }
    weight_tensor->set_data(dequant_weight);
  }
  return RET_OK;
}
void OpenCLKernel::FreeDequantedWeight() {
  auto *weight_tensor = in_tensors_.at(kWeightIndex);
  if (dequant_flag_) {
    free(weight_tensor->data_c());
  }
}
}  // namespace mindspore::kernel
