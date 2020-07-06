// MIT License
//
// Copyright (c) 2020 Lennart Braun
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

#include <array>
#include <iterator>
#include <memory>

#include <gtest/gtest.h>

#include "algorithm/circuit_loader.h"
#include "base/gate_register.h"
#include "communication/communication_layer.h"
#include "crypto/arithmetic_provider.h"
#include "crypto/base_ots/base_ot_provider.h"
#include "crypto/motion_base_provider.h"
#include "crypto/multiplication_triple/linalg_triple_provider.h"
#include "crypto/multiplication_triple/mt_provider.h"
#include "crypto/multiplication_triple/sb_provider.h"
#include "crypto/multiplication_triple/sp_provider.h"
#include "crypto/oblivious_transfer/ot_provider.h"
#include "gate/new_gate.h"
#include "protocols/gmw/gmw_provider.h"
#include "protocols/gmw/tensor_op.h"
#include "protocols/yao/tensor.h"
#include "protocols/yao/yao_provider.h"
#include "statistics/run_time_stats.h"
#include "tensor/tensor.h"
#include "utility/helpers.h"
#include "utility/linear_algebra.h"
#include "utility/logger.h"

using namespace MOTION::proto::gmw;
using namespace MOTION::proto::yao;

class YaoGMWTensorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    comm_layers_ = MOTION::Communication::make_dummy_communication_layers(2);
    for (std::size_t i = 0; i < 2; ++i) {
      loggers_[i] = std::make_shared<MOTION::Logger>(i, boost::log::trivial::severity_level::trace);
      comm_layers_[i]->set_logger(loggers_[i]);
      base_ot_providers_[i] =
          std::make_unique<MOTION::BaseOTProvider>(*comm_layers_[i], nullptr, nullptr);
      motion_base_providers_[i] =
          std::make_unique<MOTION::Crypto::MotionBaseProvider>(*comm_layers_[i], nullptr);
      ot_provider_managers_[i] = std::make_unique<ENCRYPTO::ObliviousTransfer::OTProviderManager>(
          *comm_layers_[i], *base_ot_providers_[i], *motion_base_providers_[i], nullptr, nullptr);
      arithmetic_provider_managers_[i] = std::make_unique<MOTION::ArithmeticProviderManager>(
          *comm_layers_[i], *ot_provider_managers_[i], nullptr);
      linalg_triple_providers_[i] = std::make_shared<MOTION::LinAlgTriplesFromAP>(
          arithmetic_provider_managers_[i]->get_provider(1 - i), nullptr);
      mt_providers_[i] = std::make_unique<MOTION::MTProviderFromOTs>(
          i, 2, *arithmetic_provider_managers_[i], *ot_provider_managers_[i], stats_[i], nullptr);
      sp_providers_[i] = std::make_unique<MOTION::SPProviderFromOTs>(
          ot_provider_managers_[i]->get_providers(), i, stats_[i], nullptr);
      sb_providers_[i] = std::make_unique<MOTION::TwoPartySBProvider>(
          *comm_layers_[i], ot_provider_managers_[i]->get_provider(1 - i), stats_[i], nullptr);
      gate_registers_[i] = std::make_unique<MOTION::GateRegister>();
      yao_providers_[i] = std::make_unique<YaoProvider>(
          *comm_layers_[i], *gate_registers_[i], circuit_loader_, *motion_base_providers_[i],
          ot_provider_managers_[i]->get_provider(1 - i), loggers_[i]);
      gmw_providers_[i] = std::make_unique<GMWProvider>(
          *comm_layers_[i], *gate_registers_[i], *motion_base_providers_[i], *mt_providers_[i],
          *sp_providers_[i], *sb_providers_[i], loggers_[i]);
      gmw_providers_[i]->set_linalg_triple_provider(linalg_triple_providers_[i]);
    }
  }

  void TearDown() override {
    std::vector<std::future<void>> futs;
    for (std::size_t i = 0; i < 2; ++i) {
      futs.emplace_back(std::async(std::launch::async, [this, i] { comm_layers_[i]->shutdown(); }));
    }
    std::for_each(std::begin(futs), std::end(futs), [](auto& f) { f.get(); });
  }

  const std::size_t garbler_i_ = 0;
  const std::size_t evaluator_i_ = 1;
  ENCRYPTO::ObliviousTransfer::OTProvider& get_garbler_ot_provider() {
    return ot_provider_managers_[garbler_i_]->get_provider(evaluator_i_);
  }
  ENCRYPTO::ObliviousTransfer::OTProvider& get_evaluator_ot_provider() {
    return ot_provider_managers_[evaluator_i_]->get_provider(garbler_i_);
  }

  void run_setup() {
    std::vector<std::future<void>> futs;
    for (std::size_t i = 0; i < 2; ++i) {
      futs.emplace_back(std::async(std::launch::async, [this, i] {
        comm_layers_[i]->start();
        motion_base_providers_[i]->setup();
        base_ot_providers_[i]->ComputeBaseOTs();
        mt_providers_[i]->PreSetup();
        sp_providers_[i]->PreSetup();
        sb_providers_[i]->PreSetup();
        auto f = std::async(std::launch::async, [this, i] {
          ot_provider_managers_[i]->get_provider(1 - i).SendSetup();
        });
        ot_provider_managers_[i]->get_provider(1 - i).ReceiveSetup();
        f.get();
        linalg_triple_providers_[i]->setup();
        mt_providers_[i]->Setup();
        sp_providers_[i]->Setup();
        sb_providers_[i]->Setup();
        gmw_providers_[i]->setup();
        yao_providers_[i]->setup();
      }));
    }
    std::for_each(std::begin(futs), std::end(futs), [](auto& f) { f.get(); });
  }

  void run_gates_setup() {
    auto& gates_g = gate_registers_[garbler_i_]->get_gates();
    auto& gates_e = gate_registers_[evaluator_i_]->get_gates();
    auto fut_g = std::async(std::launch::async, [&gates_g] {
      for (auto& gate : gates_g) {
        if (gate->need_setup()) {
          gate->evaluate_setup();
        }
      }
    });
    auto fut_e = std::async(std::launch::async, [&gates_e] {
      for (auto& gate : gates_e) {
        if (gate->need_setup()) {
          gate->evaluate_setup();
        }
      }
    });
    fut_g.get();
    fut_e.get();
  }

  void run_gates_online() {
    auto& gates_g = gate_registers_[garbler_i_]->get_gates();
    auto& gates_e = gate_registers_[evaluator_i_]->get_gates();
    auto fut_g = std::async(std::launch::async, [&gates_g] {
      for (auto& gate : gates_g) {
        if (gate->need_online()) {
          gate->evaluate_online();
        }
      }
    });
    auto fut_e = std::async(std::launch::async, [&gates_e] {
      for (auto& gate : gates_e) {
        if (gate->need_online()) {
          gate->evaluate_online();
        }
      }
    });
    fut_g.get();
    fut_e.get();
  }

  MOTION::CircuitLoader circuit_loader_;
  std::vector<std::unique_ptr<MOTION::Communication::CommunicationLayer>> comm_layers_;
  std::array<std::unique_ptr<MOTION::BaseOTProvider>, 2> base_ot_providers_;
  std::array<std::unique_ptr<MOTION::Crypto::MotionBaseProvider>, 2> motion_base_providers_;
  std::array<std::unique_ptr<ENCRYPTO::ObliviousTransfer::OTProviderManager>, 2>
      ot_provider_managers_;
  std::array<std::unique_ptr<MOTION::ArithmeticProviderManager>, 2> arithmetic_provider_managers_;
  std::array<std::shared_ptr<MOTION::LinAlgTripleProvider>, 2> linalg_triple_providers_;
  std::array<std::unique_ptr<MOTION::MTProvider>, 2> mt_providers_;
  std::array<std::unique_ptr<MOTION::SPProvider>, 2> sp_providers_;
  std::array<std::unique_ptr<MOTION::SBProvider>, 2> sb_providers_;
  std::array<std::unique_ptr<MOTION::GateRegister>, 2> gate_registers_;
  std::array<std::unique_ptr<GMWProvider>, 2> gmw_providers_;
  std::array<std::unique_ptr<YaoProvider>, 2> yao_providers_;
  std::array<std::shared_ptr<MOTION::Logger>, 2> loggers_;
  std::array<MOTION::Statistics::RunTimeStats, 2> stats_;
};

template <typename T>
class YaoArithmeticGMWTensorTest : public YaoGMWTensorTest {
 public:
  static std::vector<T> generate_inputs(const MOTION::tensor::TensorDimensions dims) {
    return MOTION::Helpers::RandomVector<T>(dims.get_data_size());
  }
  std::pair<ENCRYPTO::ReusableFiberPromise<MOTION::IntegerValues<T>>, MOTION::tensor::TensorCP>
  make_arithmetic_T_tensor_input_my(std::size_t party_id,
                                    const MOTION::tensor::TensorDimensions& dims) {
    auto& gp = *gmw_providers_.at(party_id);
    static_assert(ENCRYPTO::bit_size_v<T> == 64);
    return gp.make_arithmetic_64_tensor_input_my(dims);
  }
  MOTION::tensor::TensorCP make_arithmetic_T_tensor_input_other(
      std::size_t party_id, const MOTION::tensor::TensorDimensions& dims) {
    auto& gp = *gmw_providers_.at(party_id);
    static_assert(ENCRYPTO::bit_size_v<T> == 64);
    return gp.make_arithmetic_64_tensor_input_other(dims);
  }
  ENCRYPTO::ReusableFiberFuture<MOTION::IntegerValues<T>> make_arithmetic_T_tensor_output_my(
      std::size_t party_id, const MOTION::tensor::TensorCP& in) {
    auto& gp = *gmw_providers_.at(party_id);
    static_assert(ENCRYPTO::bit_size_v<T> == 64);
    return gp.make_arithmetic_64_tensor_output_my(in);
  }
};

using integer_types = ::testing::Types<std::uint64_t>;
TYPED_TEST_SUITE(YaoArithmeticGMWTensorTest, integer_types);

TYPED_TEST(YaoArithmeticGMWTensorTest, ConversionToYao) {
  MOTION::tensor::TensorDimensions dims = {
      .batch_size_ = 1, .num_channels_ = 1, .height_ = 28, .width_ = 28};
  const auto input = this->generate_inputs(dims);

  auto [input_promise, tensor_in_0] = this->make_arithmetic_T_tensor_input_my(0, dims);
  auto tensor_in_1 = this->make_arithmetic_T_tensor_input_other(1, dims);

  auto tensor_0 = this->yao_providers_[0]->make_convert_from_arithmetic_gmw_tensor(tensor_in_0);
  auto tensor_1 = this->yao_providers_[1]->make_convert_from_arithmetic_gmw_tensor(tensor_in_1);

  this->run_setup();
  this->run_gates_setup();
  input_promise.set_value(input);
  this->run_gates_online();

  const auto yao_tensor_0 = std::dynamic_pointer_cast<const YaoTensor>(tensor_0);
  const auto yao_tensor_1 = std::dynamic_pointer_cast<const YaoTensor>(tensor_1);
  ASSERT_NE(yao_tensor_0, nullptr);
  ASSERT_NE(yao_tensor_1, nullptr);
  yao_tensor_0->wait_setup();
  yao_tensor_1->wait_online();

  const auto& R = this->yao_providers_[0]->get_global_offset();
  const auto& zero_keys = yao_tensor_0->get_keys();
  const auto& evaluator_keys = yao_tensor_1->get_keys();
  constexpr auto bit_size = ENCRYPTO::bit_size_v<TypeParam>;
  const auto data_size = input.size();
  ASSERT_EQ(zero_keys.size(), data_size * bit_size);
  ASSERT_EQ(evaluator_keys.size(), data_size * bit_size);
  for (std::size_t int_i = 0; int_i < data_size; ++int_i) {
    const auto value = input.at(int_i);
    for (std::size_t bit_j = 0; bit_j < ENCRYPTO::bit_size_v<TypeParam>; ++bit_j) {
      auto idx = bit_j * data_size + int_i;
      if (value & (TypeParam(1) << bit_j)) {
        EXPECT_EQ(evaluator_keys.at(idx), zero_keys.at(idx) ^ R);
      } else {
        EXPECT_EQ(evaluator_keys.at(idx), zero_keys.at(idx));
      }
    }
    if (int_i == 1) break;
  }
}

TYPED_TEST(YaoArithmeticGMWTensorTest, ConversionBoth) {
  MOTION::tensor::TensorDimensions dims = {
      .batch_size_ = 1, .num_channels_ = 1, .height_ = 28, .width_ = 28};
  const auto input = this->generate_inputs(dims);

  auto [input_promise, tensor_in_0] = this->make_arithmetic_T_tensor_input_my(0, dims);
  auto tensor_in_1 = this->make_arithmetic_T_tensor_input_other(1, dims);

  auto yao_tensor_0 = this->yao_providers_[0]->make_convert_from_arithmetic_gmw_tensor(tensor_in_0);
  auto yao_tensor_1 = this->yao_providers_[1]->make_convert_from_arithmetic_gmw_tensor(tensor_in_1);

  auto gmw_tensor_0 = this->yao_providers_[0]->make_convert_to_arithmetic_gmw_tensor(yao_tensor_0);
  auto gmw_tensor_1 = this->yao_providers_[1]->make_convert_to_arithmetic_gmw_tensor(yao_tensor_1);

  this->gmw_providers_[0]->make_arithmetic_tensor_output_other(gmw_tensor_0);
  auto output_future = this->make_arithmetic_T_tensor_output_my(1, gmw_tensor_1);

  this->run_setup();
  this->run_gates_setup();
  input_promise.set_value(input);
  this->run_gates_online();

  auto output = output_future.get();

  ASSERT_EQ(output.size(), dims.get_data_size());
  ASSERT_EQ(input, output);
}