#ifndef REST_UTILS_HPP
#define REST_UTILS_HPP

#include <string>
#include "slic3r/Utils/bambu_networking.hpp"

namespace Slic3r {
namespace Utils {

const char* net_code_to_str(int code);
std::string dump_print_params(const Slic3r::PrintParams& p);

} // namespace Utils
} // namespace Slic3r

#endif // REST_UTILS_HPP
