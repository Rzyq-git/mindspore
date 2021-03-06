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

#include "src/net_runner.h"
#include <math.h>
#include <getopt.h>
#include <iostream>
#include <fstream>
#include "include/context.h"

unsigned int NetRunner::seed_ = time(NULL);
// Definition of callback function after forwarding operator.
bool after_callback(const std::vector<mindspore::tensor::MSTensor *> &after_inputs,
                    const std::vector<mindspore::tensor::MSTensor *> &after_outputs,
                    const mindspore::CallBackParam &call_param) {
  printf("%s\n", call_param.node_name.c_str());
  for (size_t i = 0; i < after_inputs.size(); i++) {
    int num2p = (after_inputs.at(i)->ElementsNum());
    printf("in%zu(%d): ", i, num2p);
    if (num2p > 10) num2p = 10;
    if (after_inputs.at(i)->data_type() == mindspore::kNumberTypeInt32) {
      auto d = reinterpret_cast<int *>(after_inputs.at(i)->MutableData());
      for (int j = 0; j < num2p; j++) printf("%d, ", d[j]);
    } else {
      auto d = reinterpret_cast<float *>(after_inputs.at(i)->MutableData());
      for (int j = 0; j < num2p; j++) printf("%f, ", d[j]);
    }
    printf("\n");
  }
  for (size_t i = 0; i < after_outputs.size(); i++) {
    auto d = reinterpret_cast<float *>(after_outputs.at(i)->MutableData());
    int num2p = (after_outputs.at(i)->ElementsNum());
    printf("ou%zu(%d): ", i, num2p);
    if (num2p > 10) num2p = 10;
    for (int j = 0; j < num2p; j++) printf("%f, ", d[j]);
    printf("\n");
  }
  return true;
}

NetRunner::~NetRunner() {
  if (session_ != nullptr) delete session_;
}

void NetRunner::InitAndFigureInputs() {
  mindspore::lite::Context context;
  context.device_list_[0].device_info_.cpu_device_info_.cpu_bind_mode_ = mindspore::lite::NO_BIND;
  context.thread_num_ = 1;

  session_ = mindspore::session::TrainSession::CreateSession(ms_file_, &context);
  assert(nullptr != session_);

  auto inputs = session_->GetInputs();
  assert(inputs.size() > 1);
  data_index_ = 0;
  label_index_ = 1;
  batch_size_ = inputs[data_index_]->shape()[0];
  data_size_ = inputs[data_index_]->Size() / batch_size_;  // in bytes
  if (verbose_) {
    std::cout << "data size: " << data_size_ << std::endl << "batch size: " << batch_size_ << std::endl;
  }
}

mindspore::tensor::MSTensor *NetRunner::SearchOutputsForSize(size_t size) const {
  auto outputs = session_->GetOutputs();
  for (auto it = outputs.begin(); it != outputs.end(); ++it) {
    if (it->second->ElementsNum() == size) return it->second;
  }
  std::cout << "Model does not have an output tensor with size " << size << std::endl;
  return nullptr;
}

std::vector<int> NetRunner::FillInputData(const std::vector<DataLabelTuple> &dataset, bool serially) const {
  std::vector<int> labels_vec;
  static unsigned int idx = 1;
  int total_size = dataset.size();

  auto inputs = session_->GetInputs();
  char *input_data = reinterpret_cast<char *>(inputs.at(data_index_)->MutableData());
  auto labels = reinterpret_cast<float *>(inputs.at(label_index_)->MutableData());
  assert(total_size > 0);
  assert(input_data != nullptr);
  std::fill(labels, labels + inputs.at(label_index_)->ElementsNum(), 0.f);
  for (int i = 0; i < batch_size_; i++) {
    if (serially) {
      idx = ++idx % total_size;
    } else {
      idx = rand_r(&seed_) % total_size;
    }
    int label = 0;
    char *data = nullptr;
    std::tie(data, label) = dataset[idx];
    memcpy(input_data + i * data_size_, data, data_size_);
    labels[i * num_of_classes_ + label] = 1.0;  // Model expects labels in onehot representation
    labels_vec.push_back(label);
  }

  return labels_vec;
}

float NetRunner::CalculateAccuracy(int max_tests) const {
  float accuracy = 0.0;
  const std::vector<DataLabelTuple> test_set = ds_.test_data();
  int tests = test_set.size() / batch_size_;
  if (max_tests != -1 && tests < max_tests) tests = max_tests;

  session_->Eval();
  for (int i = 0; i < tests; i++) {
    auto labels = FillInputData(test_set, (max_tests == -1));
    session_->RunGraph();
    auto outputsv = SearchOutputsForSize(batch_size_ * num_of_classes_);
    assert(outputsv != nullptr);
    auto scores = reinterpret_cast<float *>(outputsv->MutableData());
    for (int b = 0; b < batch_size_; b++) {
      int max_idx = 0;
      float max_score = scores[num_of_classes_ * b];
      for (int c = 0; c < num_of_classes_; c++) {
        if (scores[num_of_classes_ * b + c] > max_score) {
          max_score = scores[num_of_classes_ * b + c];
          max_idx = c;
        }
      }
      if (labels[b] == max_idx) accuracy += 1.0;
    }
  }
  session_->Train();
  accuracy /= static_cast<float>(batch_size_ * tests);
  return accuracy;
}

int NetRunner::InitDB() {
  if (data_size_ != 0) ds_.set_expected_data_size(data_size_);
  int ret = ds_.Init(data_dir_, DS_MNIST_BINARY);
  num_of_classes_ = ds_.num_of_classes();
  if (ds_.test_data().size() == 0) {
    std::cout << "No relevant data was found in " << data_dir_ << std::endl;
    assert(ds_.test_data().size() != 0);
  }

  return ret;
}

float NetRunner::GetLoss() const {
  auto outputsv = SearchOutputsForSize(1);  // Search for Loss which is a single value tensor
  assert(outputsv != nullptr);
  auto loss = reinterpret_cast<float *>(outputsv->MutableData());
  return loss[0];
}

int NetRunner::TrainLoop() {
  session_->Train();
  float min_loss = 1000.;
  float max_acc = 0.;
  for (int i = 0; i < cycles_; i++) {
    FillInputData(ds_.train_data());
    session_->RunGraph(nullptr, verbose_ ? after_callback : nullptr);
    float loss = GetLoss();
    if (min_loss > loss) min_loss = loss;

    if (save_checkpoint_ != 0 && (i + 1) % save_checkpoint_ == 0) {
      auto cpkt_fn = ms_file_.substr(0, ms_file_.find_last_of('.')) + "_trained_" + std::to_string(i + 1) + ".ms";
      session_->SaveToFile(cpkt_fn);
    }

    if ((i + 1) % 100 == 0) {
      float acc = CalculateAccuracy(10);
      if (max_acc < acc) max_acc = acc;
      std::cout << i + 1 << ":\tLoss is " << std::setw(7) << loss << " [min=" << min_loss << "] "
                << " max_acc=" << max_acc << std::endl;
    }
  }
  return 0;
}

int NetRunner::Main() {
  InitAndFigureInputs();

  InitDB();

  TrainLoop();

  float acc = CalculateAccuracy();
  std::cout << "accuracy = " << acc << std::endl;

  if (cycles_ > 0) {
    auto trained_fn = ms_file_.substr(0, ms_file_.find_last_of('.')) + "_trained_" + std::to_string(cycles_) + ".ms";
    session_->SaveToFile(trained_fn);
  }
  return 0;
}

void NetRunner::Usage() {
  std::cout << "Usage: net_runner -f <.ms model file> -d <data_dir> [-c <num of training cycles>] "
            << "[-v (verbose mode)] [-s <save checkpoint every X iterations>]" << std::endl;
}

bool NetRunner::ReadArgs(int argc, char *argv[]) {
  int opt;
  while ((opt = getopt(argc, argv, "f:e:d:s:ihc:v")) != -1) {
    switch (opt) {
      case 'f':
        ms_file_ = std::string(optarg);
        break;
      case 'e':
        cycles_ = atoi(optarg);
        break;
      case 'd':
        data_dir_ = std::string(optarg);
        break;
      case 'v':
        verbose_ = true;
        break;
      case 's':
        save_checkpoint_ = atoi(optarg);
        break;
      case 'h':
      default:
        Usage();
        return false;
    }
  }
  return true;
}

int main(int argc, char **argv) {
  NetRunner nr;

  if (nr.ReadArgs(argc, argv)) {
    nr.Main();
  } else {
    return -1;
  }
  return 0;
}
