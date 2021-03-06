#include <climits>
/*
 * Copyright (c) 2020.Huawei Technologies Co., Ltd. All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ACLMANAGER_H
#define ACLMANAGER_H

#include <string>
#include <string.h>
#include <map>
#include <iostream>
#include <memory>
#include "acl/acl.h"
#include "CommonDataType.h"
#include "mindspore/core/utils/log_adapter.h"
#include "ErrorCode.h"
#include "DvppCommon.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

mode_t SetFileDefaultUmask();

class AclProcess {
 public:
  AclProcess(uint32_t resizeWidth, uint32_t resizeHeight, uint32_t cropWidth, uint32_t cropHeight, aclrtContext context,
             aclrtStream stream = nullptr, std::shared_ptr<DvppCommon> dvppCommon = nullptr);

  ~AclProcess(){};

  // Release all the resource
  APP_ERROR Release();
  // Create resource for this sample
  APP_ERROR InitResource();
  // Process the result
  APP_ERROR Process(RawData &ImageInfo);
  // API for access memory
  std::shared_ptr<void> Get_Memory_Data();
  // API for access device memory
  std::shared_ptr<DvppDataInfo> Get_Device_Memory_Data();
  // change output method
  void set_mode(bool flag);
  // Get the mode of Acl process
  bool get_mode();
  // Save the result
  APP_ERROR WriteResult(uint32_t fileSize, std::shared_ptr<void> outBuf, std::string filename);
  // Color space reform
  void YUV420TOYUV444(unsigned char *inputBuffer, unsigned char *outputBuffer, int w, int h);
  // Crop definition
  void CropConfigFilter(CropRoiConfig &cfg, DvppCropInputInfo &cropinfo);
  // D-chip memory release
  void device_memory_release();

 private:
  // Initialize the modules used by this sample
  APP_ERROR InitModule();
  // Preprocess the input image
  APP_ERROR Preprocess(RawData &ImageInfo);
  // Filename process
  APP_ERROR RenameFile(std::string &filename);

  aclrtContext context_;
  aclrtStream stream_;
  std::shared_ptr<DvppCommon> dvppCommon_;  // dvpp object
  std::shared_ptr<void> processedInfo_;     // processed data
  uint32_t resizeWidth_;                    // dvpp resize width
  uint32_t resizeHeight_;                   // dvpp resize height
  uint32_t cropWidth_;                      // dvpp crop width
  uint32_t cropHeight_;                     // dvpp crop height
  bool repeat_;                             // Repeatly process image or not
};

#endif
