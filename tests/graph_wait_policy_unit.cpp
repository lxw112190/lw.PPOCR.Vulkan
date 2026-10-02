#include "graph_wait_policy.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    auto require = [](bool value) {
        if (!value)
            throw std::runtime_error("graph fence wait policy mismatch");
    };
    for (const auto value : {"", "30000", "180000", "300000", "invalid", "18446744073709551615"})
        require(lwvk::graph_wait_timeout_ns(false, value) == UINT64_C(30000000000));
    require(lwvk::graph_wait_timeout_ns(true, "") == UINT64_C(30000000000));
    require(lwvk::graph_wait_timeout_ns(true, "30000") == UINT64_C(30000000000));
    require(lwvk::graph_wait_timeout_ns(true, "180000") == UINT64_C(180000000000));
    require(lwvk::graph_wait_timeout_ns(true, "300000") == UINT64_C(300000000000));
    for (const auto value : {"0", "29999", "300001", "-1", "+30000", " 30000", "30000 ", "30s", "1e5",
                             "18446744073709551615", "999999999999999999999999999999"}) {
        bool rejected = false;
        try {
            lwvk::graph_wait_timeout_ns(true, value);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected);
    }
    std::cout << "PASS: hardware unchanged / software finite bounds / invalid values and overflow\n";
}
