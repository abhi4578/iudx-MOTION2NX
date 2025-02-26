/*
./bin/Weights_Share_Receiver --my-id 0 --file-names $model_config --current-path $build_path


./bin/Weights_Share_Receiver --my-id 1--file-names $model_config --current-path $build_path

./bin/weights_provider --compute-server0-port 1234 --compute-server1-port 1235 --dp-id 0
--fractional-bits $fractional_bits --filepath $build_path_model

*/
/* This code generates shares files for w1,b1,w2,b2.
This function needs a config file as input sample file_config_weights_1.txt is as follows

W1
B1
W2
B2

*/

// MIT License
//
// Copyright (c) 2021 Lennart Braun
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <dirent.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <regex>
#include <stdexcept>
#include <thread>

#include <boost/algorithm/string.hpp>
#include <boost/json/serialize.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>
#include <boost/program_options.hpp>

#include "algorithm/circuit_loader.h"
#include "base/gate_factory.h"
#include "base/two_party_backend.h"
#include "communication/communication_layer.h"
#include "communication/tcp_transport.h"
#include "compute_server/compute_server.h"
#include "statistics/analysis.h"
#include "utility/logger.h"

#include "base/two_party_tensor_backend.h"
#include "protocols/beavy/tensor.h"
#include "tensor/tensor.h"
#include "tensor/tensor_op.h"
#include "tensor/tensor_op_factory.h"
#include "utility/fixed_point.h"

namespace po = boost::program_options;

static std::vector<uint64_t> generate_inputs(const MOTION::tensor::TensorDimensions dims) {
  return MOTION::Helpers::RandomVector<uint64_t>(dims.get_data_size());
}

struct Matrix {
  std::vector<uint64_t> Delta;
  std::vector<uint64_t> delta;
  int row;
  int col;
};

struct Options {
  std::size_t threads;
  bool json;
  std::size_t num_simd;
  bool sync_between_setup_and_online;
  Matrix weights[2];
  Matrix biases[2];
  std::size_t my_id;
  // MOTION::Communication::tcp_parties_config tcp_config;
  bool no_run = false;
  std::string filenames;
  std::vector<std::string> data;
  int actual_answer;
  std::vector<std::string> filepaths;
  std::string currentpath;
};

void read_filenames(Options* options) {
  // read file_config.txt
  // auto p = std::filesystem::current_path();

  // model
  auto p = options->filenames;
  std::cout << p << std::endl;

  // model
  std::ifstream indata;
  indata.open(p);
  if (std::ifstream(p)) {
    std::cout << "File found\n";
  } else {
    std::cout << "File not found\n";
  }
  assert(indata);

  // model
  std::string line;
  while (std::getline(indata, line)) {
    std::stringstream lineStream(line);
    std::string cell;

    while (std::getline(lineStream, cell, ' ')) {
      options->data.push_back(cell);
    }
  }
  std::cout << "\n";
}

void generate_filepaths(Options* options) {
  // creation of directory eg. server0
  std::error_code err;
  std::string dirname = "server" + std::to_string(options->my_id);
  std::string dirname1 = options->currentpath + "/" + dirname;
  std::filesystem::create_directories(dirname1, err);

  //.../server0/
  // std::string temp = std::filesystem::current_path();
  // std::cout << "g:" << temp << "\n";
  std::string filename = options->currentpath + "/" + dirname + "/";
  std::ofstream file;
  std::string fullfilename;

  // creation of file_config_0.txt and file_config_1.txt
  std::string model_later =
      options->currentpath + "/file_config_model" + std::to_string(options->my_id);
  std::ofstream file_later1, file_later2;
  file_later2.open(model_later, std::ios_base::out);

  //../server0/filename
  for (int i = 0; i < options->data.size(); ++i) {
    std::string n = options->data[i];
    fullfilename = filename + n;
    file_later2 << fullfilename << "\n";
    options->filepaths.push_back(fullfilename);
  }
}

void retrieve_shares(int port_number, Options* options) {
  std::ofstream file;

  ////////////////////////////////////////////////////////////

  for (auto i = 0; i < 4; i++) {
    std::cout << "Reading shares from weights provider \n";
    auto pair2 = COMPUTE_SERVER::get_provider_mat_mul_data(port_number);
    // auto [q1,q2,q3] = COMPUTE_SERVER::get_provider_mat_mul_data_new(port_number);
    std::vector<COMPUTE_SERVER::Shares> input_values_dp1 = pair2.second.first;
    // std::vector<COMPUTE_SERVER::Shares> input_values_dp1 = q3.first;
    auto temp = options->filepaths[i];
    std::cout << temp << "\n";
    if ((i + 2) % 2 == 0) {
      std::cout << "Weights \n";
      file.open(temp, std::ios_base::out);

      options->weights[i / 2].row = pair2.second.second[0];
      options->weights[i / 2].col = pair2.second.second[1];
      // options->weights[i / 2].row = q3.second[0];
      // options->weights[i / 2].col = q3.second[1];
      file << options->weights[i / 2].row << " " << options->weights[i / 2].col << "\n";

      for (int j = 0; j < input_values_dp1.size(); j++) {
        options->weights[i / 2].Delta.push_back(input_values_dp1[j].Delta);
        options->weights[(i) / 2].delta.push_back(input_values_dp1[j].delta);
        file << input_values_dp1[j].Delta << " " << input_values_dp1[j].delta << "\n";
      }
      file.close();
    } else {
      std::cout << "Bias \n";
      file.open(temp, std::ios_base::out);

      options->biases[(i - 1) / 2].row = pair2.second.second[0];
      options->biases[(i - 1) / 2].col = pair2.second.second[1];
      // options->biases[i / 2].row = q3.second[0];
      // options->biases[i / 2].col = q3.second[1];
      file << options->biases[(i - 1) / 2].row << " " << options->biases[(i - 1) / 2].col << "\n";
      std::cout << "Size:" << input_values_dp1.size() << "rows:" << options->biases[(i - 1) / 2].row
                << "columns:" << options->biases[(i - 2) / 2].col << "\n";
      for (int j = 0; j < input_values_dp1.size(); j++) {
        options->biases[(i - 1) / 2].Delta.push_back(input_values_dp1[j].Delta);
        options->biases[(i - 1) / 2].delta.push_back(input_values_dp1[j].delta);
        file << input_values_dp1[j].Delta << " " << input_values_dp1[j].delta << "\n";
      }
      file.close();
    }
  }
}

std::optional<Options> parse_program_options(int argc, char* argv[]) {
  Options options;
  boost::program_options::options_description desc("Allowed options");
  // clang-format off
  desc.add_options()
    ("help,h", po::bool_switch()->default_value(false),"produce help message")
    ("config-file", po::value<std::string>(), "config file containing options")
    ("my-id", po::value<std::size_t>()->required(), "my party id")
    ("threads", po::value<std::size_t>()->default_value(0), "number of threads to use for gate evaluation")
    ("json", po::bool_switch()->default_value(false), "output data in JSON format")
    ("num-simd", po::value<std::size_t>()->default_value(1), "number of SIMD values")
    ("file-names",po::value<std::string>()->required(), "filename")
    ("current-path",po::value<std::string>()->required(), "current path build_debwithrelinfo")
    ("sync-between-setup-and-online", po::bool_switch()->default_value(false),
     "run a synchronization protocol before the online phase starts")
    ("no-run", po::bool_switch()->default_value(false), "just build the circuit, but not execute it")
    ;
  // clang-format on

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  bool help = vm["help"].as<bool>();
  if (help) {
    std::cerr << desc << "\n";
    return std::nullopt;
  }
  if (vm.count("config-file")) {
    std::ifstream ifs(vm["config-file"].as<std::string>().c_str());
    po::store(po::parse_config_file(ifs, desc), vm);
  }
  try {
    po::notify(vm);
  } catch (std::exception& e) {
    std::cerr << "error:" << e.what() << "\n\n";
    std::cerr << desc << "\n";
    return std::nullopt;
  }

  options.my_id = vm["my-id"].as<std::size_t>();
  options.threads = vm["threads"].as<std::size_t>();
  options.json = vm["json"].as<bool>();
  options.num_simd = vm["num-simd"].as<std::size_t>();
  options.sync_between_setup_and_online = vm["sync-between-setup-and-online"].as<bool>();
  options.no_run = vm["no-run"].as<bool>();
  options.filenames = vm["file-names"].as<std::string>();
  options.currentpath = vm["current-path"].as<std::string>();
  if (options.my_id > 1) {
    std::cerr << "my-id must be one of 0 and 1\n";
    return std::nullopt;
  }

  read_filenames(&options);

  if (options.my_id == 0) {
    generate_filepaths(&options);
  } else if (options.my_id == 1) {
    generate_filepaths(&options);
  }

  if (options.my_id == 0) {
    retrieve_shares(1234, &options);
  } else {
    retrieve_shares(1235, &options);
  }

  return options;
}

int main(int argc, char* argv[]) {
  auto options = parse_program_options(argc, argv);
  if (!options.has_value()) {
    return EXIT_FAILURE;
  }
  try {
    auto logger = std::make_shared<MOTION::Logger>(options->my_id,
                                                   boost::log::trivial::severity_level::trace);

  } catch (std::runtime_error& e) {
    std::cerr << "ERROR OCCURRED: " << e.what() << "\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}