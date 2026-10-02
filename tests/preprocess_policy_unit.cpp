#include "preprocess_policy.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    using O = lwvk::PreprocessOption;
    auto require = [](bool value) {
        if (!value)
            throw std::runtime_error("preprocessing policy mismatch");
    };
    for (bool fp64 : {false, true})
        for (bool profile : {false, true})
            for (O det : {O::Auto, O::Off, O::On})
                for (O text : {O::Auto, O::Off, O::On})
                    for (O crop : {O::Auto, O::Off, O::On}) {
                        const bool forced = det == O::On || text == O::On || crop == O::On;
                        const bool reject =
                            (crop == O::On && (det == O::Off || text == O::Off)) || (forced && (!fp64 || profile));
                        bool threw = false;
                        lwvk::PreprocessPolicy p;
                        try {
                            p = lwvk::resolve_preprocess_policy(det, text, crop, fp64, profile);
                        } catch (const std::invalid_argument&) {
                            threw = true;
                        } catch (const std::runtime_error&) {
                            if (!reject)
                                throw;
                            threw = true;
                        }
                        require(threw == reject);
                        if (!threw) {
                            require(p.det == (det != O::Off && fp64 && !profile));
                            require(p.text == (text != O::Off && fp64 && !profile));
                            require(p.crop == (crop != O::Off && p.det && p.text));
                        }
                    }
    std::cout << "PASS: 108 capability / profile / explicit option combinations\n";
}
