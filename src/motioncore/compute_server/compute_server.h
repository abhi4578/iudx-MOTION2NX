#pragma once
#include <boost/asio.hpp>
#include "compute_server.h"

using namespace boost::asio;
using ip::tcp;
using std::string;

namespace COMPUTE_SERVER {
struct Shares {
  uint64_t Delta, delta;
};
string read_(tcp::socket& socket);
void send_(tcp::socket& socket, const string& message);
std::vector<Shares> read_struct(tcp::socket& socket, int num_elements);
std::vector<std::pair<uint64_t, uint64_t>> get_provider_data(int port_number);
std::pair<std::vector<Shares>, int> get_provider_dot_product_data(int port_number);
std::pair<std::size_t, std::pair<std::vector<Shares>, std::vector<int>>> get_provider_mat_mul_data(
    int port_number);
std::tuple<int, std::size_t, std::pair<std::vector<Shares>, std::vector<int>>>
get_provider_mat_mul_data_new(int port_number);
std::pair<std::size_t, std::pair<std::vector<Shares>, std::vector<int>>>
get_provider_mat_mul_const_data(int port_number);
}  // namespace COMPUTE_SERVER
