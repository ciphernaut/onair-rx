// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: OnAir's root raised cosine, defined here to keep OnAir's signal generator out of onair-rx's link.
// Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// OnAir's DVB-S receiver (dvbs_dsp.cpp, its matched filter) calls dect2::dvbs::rrcAt, which OnAir defines in its test signal
// generator, core/src/dvbs/dvbs_gen.cpp:75-84 (at a63c3a8). That object also holds demoTsSource, the test programme, whose
// video encoder needs FFmpeg: linking it would make every mode of onair-rx need FFmpeg's libraries and start about 20 MB
// larger. With the function defined here the linker never takes dvbs_gen.o out of libdect2core.a. The body is OnAir's,
// unchanged. Should a later OnAir move rrcAt next to its caller, the link fails with a duplicate symbol: remove this file
// then. onair-gen links the generator on purpose and does not link this file (build.sh: gen.o, ais.o, json.o).
#include "dect2/common/mathconst.h"
#include <cmath>

namespace dect2 {
namespace dvbs {

// Root raised cosine impulse response at time t (in symbol periods), unit energy per symbol period (dvbs_gen.h)
double rrcAt(double t, double a) {
    using dect2::math::kPi;
    const double eps = 1e-9;
    if (std::fabs(t) < eps) return 1 - a + 4 * a / kPi;
    if (a > 0 && std::fabs(std::fabs(4 * a * t) - 1) < 1e-7) {
        return a / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * a)) + (1 - 2 / kPi) * std::cos(kPi / (4 * a)));
    }
    const double num = std::sin(kPi * t * (1 - a)) + 4 * a * t * std::cos(kPi * t * (1 + a));
    const double den = kPi * t * (1 - (4 * a * t) * (4 * a * t));
    return num / den;
}

} // namespace dvbs
} // namespace dect2
