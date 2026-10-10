/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2026 Raghav Subbarao

 This file is part of QuantLib, a free-software/open-source library
 for financial quantitative analysts and developers - http://quantlib.org/

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <https://www.quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.
*/

#include <ql/experimental/fxslv/fxdeltaconverter.hpp>
#include <ql/math/distributions/normaldistribution.hpp>
#include <ql/math/solvers1d/brent.hpp>
#include <algorithm>
#include <cmath>

namespace QuantLib {

    FxDeltaConverter::FxDeltaConverter(DeltaVolQuote::DeltaType type,
                                       Real spot,
                                       DiscountFactor domesticDiscount,
                                       DiscountFactor foreignDiscount)
    : type_(type), spot_(spot), forward_(spot * foreignDiscount / domesticDiscount),
      domesticDiscount_(domesticDiscount), foreignDiscount_(foreignDiscount) {
        QL_REQUIRE(spot > 0.0, "positive spot required: " << spot);
        QL_REQUIRE(domesticDiscount > 0.0 && foreignDiscount > 0.0,
                   "positive discount factors required: " << domesticDiscount << ", "
                                                          << foreignDiscount);
    }

    BlackDeltaCalculator FxDeltaConverter::calculator(Option::Type type, Real stdDev) const {
        return BlackDeltaCalculator(type, type_, spot_, domesticDiscount_, foreignDiscount_,
                                    stdDev);
    }

    Real FxDeltaConverter::delta(Option::Type type, Rate strike, Real stdDev) const {
        return calculator(type, stdDev).deltaFromStrike(strike);
    }

    Rate FxDeltaConverter::atmStrike(DeltaVolQuote::AtmType type, Real stdDev) const {
        return calculator(Option::Call, stdDev).atmStrike(type);
    }

    Real FxDeltaConverter::callMinusPutDelta(Rate strike) const {
        switch (type_) {
          case DeltaVolQuote::Spot:
            return foreignDiscount_;
          case DeltaVolQuote::Fwd:
            return 1.0;
          case DeltaVolQuote::PaSpot:
            return foreignDiscount_ * strike / forward_;
          case DeltaVolQuote::PaFwd:
            return strike / forward_;
          default:
            QL_FAIL("unknown delta type");
        }
    }

    Real FxDeltaConverter::callDeltaLimit() const {
        QL_REQUIRE(!premiumAdjusted(),
                   "the largest premium-adjusted call delta depends on the smile");
        return callMinusPutDelta(forward_);
    }

    Rate FxDeltaConverter::peakCallStrike(Real stdDev) const {
        QL_REQUIRE(premiumAdjusted(), "the call delta only peaks for premium-adjusted deltas");
        QL_REQUIRE(stdDev > 0.0, "positive standard deviation required: " << stdDev);
        // The premium-adjusted call delta is proportional to (K/F) N(d2);
        // its derivative in K vanishes where w N(d2) = n(d2), w = stdDev.
        // h(d) = w N(d) - n(d) falls to its minimum at d = -w, where it is
        // negative, and rises to w after it, so it has a single root above -w.
        const Real w = stdDev;
        CumulativeNormalDistribution N;
        NormalDistribution n;
        auto h = [&](Real d) { return w * N(d) - n(d); };
        Real hi = std::max(1.0, -w + 1.0);
        while (h(hi) <= 0.0)
            hi += 1.0;
        Brent solver;
        solver.setMaxEvaluations(1000);
        const Real d2 = solver.solve(h, 1.0e-14, 0.5 * (hi - w), -w, hi);
        // d2 = log(F/K)/w - w/2
        return forward_ * std::exp(-w * d2 - 0.5 * w * w);
    }

    Rate FxDeltaConverter::strike(Option::Type type, Real delta, Real stdDev) const {
        QL_REQUIRE(stdDev > 0.0, "positive standard deviation required: " << stdDev);
        QL_REQUIRE(delta * static_cast<Real>(type) > 0.0,
                   "option type and delta are incoherent: " << type << ", " << delta);

        if (!premiumAdjusted())
            return calculator(type, stdDev).strikeFromDelta(delta);

        // Premium-adjusted deltas are solved for in log-strike.  Puts fall
        // monotonically from 0 to -infinity as the strike grows; calls
        // fall from their peak to 0 above the peak strike.
        auto error = [&](Real x) { return this->delta(type, std::exp(x), stdDev) - delta; };

        Real lo, hi;
        if (type == Option::Put) {
            // the error is positive at low strikes and negative at high ones
            Real step = stdDev;
            lo = hi = std::log(forward_);
            while (error(lo) <= 0.0) {
                lo -= step;
                step *= 2.0;
            }
            step = stdDev;
            while (error(hi) >= 0.0) {
                hi += step;
                step *= 2.0;
            }
        } else {
            const Rate kPeak = peakCallStrike(stdDev);
            const Real maxDelta = this->delta(Option::Call, kPeak, stdDev);
            QL_REQUIRE(delta <= maxDelta,
                       "call delta " << delta << " not attainable: the largest premium-adjusted "
                                     << "call delta at a standard deviation of " << stdDev
                                     << " is " << maxDelta);
            lo = std::log(kPeak);
            if (delta == maxDelta)
                return kPeak;
            // the error is positive at the peak and negative far above it
            Real step = stdDev;
            hi = lo + step;
            while (error(hi) >= 0.0) {
                step *= 2.0;
                hi = lo + step;
            }
        }

        Brent solver;
        solver.setMaxEvaluations(1000);
        return std::exp(solver.solve(error, 1.0e-12, 0.5 * (lo + hi), lo, hi));
    }

}
