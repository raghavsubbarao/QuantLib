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

#include <ql/math/optimization/constraint.hpp>
#include <ql/math/optimization/endcriteria.hpp>
#include <ql/math/optimization/levenbergmarquardt.hpp>
#include <ql/math/solvers1d/brent.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/experimental/fxslv/fxsmilesectionbydelta.hpp>
#include <ql/pricingengines/blackdeltacalculator.hpp>
#include <algorithm>
#include <cmath>

namespace QuantLib {

    FxSmileSectionByDelta::FxSmileSectionByDelta(const Date& exerciseDate,
                                                 const Handle<Quote>& spot,
                                                 const ext::shared_ptr<FxSmileQuotes>& quotes,
                                                 const Handle<YieldTermStructure>& foreignDiscount,
                                                 const Handle<YieldTermStructure>& domesticDiscount,
                                                 DeltaVolQuote::DeltaType deltaType,
                                                 DeltaVolQuote::AtmType atmType,
                                                 const DayCounter& dayCounter,
                                                 const FxSettlementConvention& settlement,
                                                 const Date& referenceDate)
    : FxSmileSection(exerciseDate, spot, quotes,                     
                     foreignDiscount, domesticDiscount,
                     deltaType, atmType, dayCounter, settlement, referenceDate),
      params_() {}

    FxSmileSectionByDelta::FxSmileSectionByDelta(Time exerciseTime,
                                                 const Handle<Quote>& spot,
                                                 const ext::shared_ptr<FxSmileQuotes>& quotes,
                                                 const Handle<YieldTermStructure>& foreignDiscount,
                                                 const Handle<YieldTermStructure>& domesticDiscount,
                                                 DeltaVolQuote::DeltaType deltaType,
                                                 DeltaVolQuote::AtmType atmType,
                                                 const DayCounter& dayCounter)
    : FxSmileSection(exerciseTime, spot, quotes,
                     foreignDiscount, domesticDiscount,
                     deltaType, atmType, dayCounter),
      params_() {}

    Volatility FxSmileSectionByDelta::volByStrike(Rate strike) const 
    {
        calculate();

        Real delta = deltaByStrike(strike, Option::Type::Put);
        return volByDelta(delta, Option::Type::Put);
    }

    Volatility FxSmileSectionByDelta::volByDelta(Real delta, Option::Type parity) const 
    {
        calculate();

        if (parity == Option::Call) {
            // call delta needs to be converted to put delta
            switch (deltaType()) { 
            
            case DeltaVolQuote::Spot:
                QL_REQUIRE(std::fabs(delta) <= dfor_, "Spot delta out of range.");
                delta -= dfor_;
                break;

            case DeltaVolQuote::Fwd:
                QL_REQUIRE(std::fabs(delta) <= 1.0, "Forward delta out of range.");
                delta -= 1;
                break;

            case DeltaVolQuote::PaSpot:
            case DeltaVolQuote::PaFwd:
                const Rate kPeak = peakCallDeltaStrike();
                Volatility v = volByStrike(kPeak);
                Real maxCallDelta = BlackDeltaCalculator(Option::Call, deltaType(), spot()->value(),
                                                         ddom_, dfor_, v * sqrt(exerciseTime()))
                                        .deltaFromStrike(kPeak);
                QL_REQUIRE(delta <= maxCallDelta + QL_EPSILON, "Call delta out of range");
                if (std::fabs(delta - maxCallDelta) <= QL_EPSILON) {
                    return v;
                }

                // Otherwise find the put delta d whose strike has the given
                // call delta, by put-call parity
                //   call delta - put delta = dfor K/F (PaSpot) or K/F (PaFwd).
                // Below the peak each call delta is attained at two strikes;
                // the quoted one is above the peak, i.e. at put deltas below
                // the peak's, where the call delta falls from its maximum to 0.
                auto deltaError = [&](Real d) {
                    Volatility v = volByDelta(d, Option::Type::Put);
                    Real k = putStrikeFromDelta(d, v * sqrt(exerciseTime()));
                    if (deltaType() == DeltaVolQuote::PaSpot) {
                        return delta - d - dfor_ * k / fwd_;

                    } else {
                        return delta - d - k / fwd_;
                    }
                };

                // the error is negative at the peak and positive far above it
                const Real dPeak = BlackDeltaCalculator(Option::Put, deltaType(), spot()->value(),
                                                        ddom_, dfor_, v * sqrt(exerciseTime()))
                                       .deltaFromStrike(kPeak);
                Real step = std::max(std::fabs(dPeak), 0.01), dFar = dPeak - step;
                Size steps = 0;
                while (deltaError(dFar) <= 0.0) {
                    QL_REQUIRE(++steps < 100, "cannot convert call delta " << delta
                                                  << " to a put delta");
                    step *= 2.0;
                    dFar = dPeak - step;
                }

                Brent solver;
                solver.setMaxEvaluations(1000);
                delta = solver.solve(deltaError, 1e-12, 0.5 * (dFar + dPeak), dFar, dPeak);
            }
        }

        // got vol as a function of delta
        return _volByDelta(delta, fwd_, exerciseTime(), params_);
    }

    Rate FxSmileSectionByDelta::strikeByDelta(Real delta, Option::Type parity) const 
    {
        calculate();

        Volatility v = volByDelta(delta, parity);
        if (parity == Option::Put)
            return putStrikeFromDelta(delta, v * sqrt(exerciseTime()));
        BlackDeltaCalculator bdc(parity, deltaType(), spot()->value(), ddom_, dfor_,
                                 v * sqrt(exerciseTime()));
        return bdc.strikeFromDelta(delta);
    }

    Rate FxSmileSectionByDelta::putStrikeFromDelta(Real putDelta, Real stdDev) const
    {
        QL_REQUIRE(putDelta < 0.0, "put delta must be negative: " << putDelta);

        BlackDeltaCalculator bdc(Option::Put, deltaType(), spot()->value(), ddom_, dfor_, stdDev);
        if (deltaType() == DeltaVolQuote::Spot || deltaType() == DeltaVolQuote::Fwd)
            return bdc.strikeFromDelta(putDelta);

        // Premium-adjusted put delta -dfor*(K/F)*N(-d2) (or -(K/F)*N(-d2))
        // decreases monotonically from 0 to -infinity as K grows, so the
        // strike is bracketed by expanding upwards from the forward.
        auto f = [&](Real k) { return bdc.deltaFromStrike(k) - putDelta; };
        Brent solver;
        solver.setMaxEvaluations(1000);
        solver.setLowerBound(QL_EPSILON * fwd_);
        return solver.solve(f, 1.0e-12, fwd_, 0.1 * fwd_);
    }

    Real FxSmileSectionByDelta::deltaByStrike(Rate strike, Option::Type parity) const 
    {
        calculate();

        // Since the slice is parameterized by put deltas, ignore parity and get
        // the put delta at the specified strike! This requires a root finding
        // procedure as we know the strike but not the vol!
        Rate d0 = BlackDeltaCalculator(Option::Type::Put, deltaType(), spot()->value(), ddom_,
                                       dfor_, referenceVol() * sqrt(exerciseTime()))
                      .deltaFromStrike(strike);

        // Solve the fixed point d = putDelta(strike, vol(d)). This only needs
        // deltaFromStrike, so it avoids inverting delta -> strike at every
        // step (which, for premium-adjusted deltas, throws for put deltas
        // below -dfor). The put delta at a fixed strike is bounded below
        // whatever the vol:
        //   Spot:   -dfor              Fwd:   -1
        //   PaSpot: -dfor * K / F      PaFwd: -K / F
        // since N(-d1), N(-d2) <= 1.
        Real dmin = 0.0;
        switch (deltaType()) {
            case DeltaVolQuote::Spot:
                dmin = -dfor_;
                break;
            case DeltaVolQuote::Fwd:
                dmin = -1.0;
                break;
            case DeltaVolQuote::PaSpot:
                dmin = -dfor_ * strike / fwd_;
                break;
            case DeltaVolQuote::PaFwd:
                dmin = -strike / fwd_;
                break;
            default:
                QL_FAIL("unknown delta type");
        }
        // The upper end is 0 rather than -epsilon: a deep out-of-the-money
        // put can have a delta smaller in magnitude than epsilon, and the
        // fixed-point form never needs to invert delta -> strike at 0.
        const Real dmax = 0.0;
        if (!(d0 > dmin && d0 < dmax))
            d0 = 0.5 * (dmin + dmax);

        auto deltaError = [&](Real delta) {
            Volatility v = volByDelta(delta, Option::Type::Put);
            return BlackDeltaCalculator(Option::Type::Put, deltaType(), spot()->value(), ddom_,
                                        dfor_, v * sqrt(exerciseTime()))
                       .deltaFromStrike(strike) -
                   delta;
        };

        Brent solver;
        Real d = solver.solve(deltaError, 1e-12, d0, dmin, dmax);

        if (parity == Option::Type::Call) {
            Volatility v = volByDelta(d, Option::Type::Put);
            d = BlackDeltaCalculator(Option::Type::Call, deltaType(), spot()->value(), ddom_, dfor_,
                                     v * sqrt(exerciseTime()))
                    .deltaFromStrike(strike);
        }

        return d;
    }

    Real FxSmileSectionByDelta::volResidual(Rate strike, Volatility vol) const
    {
        // the model vol at the point's put delta, computed with the point's own vol
        const Real stdDev = vol * std::sqrt(exerciseTime());
        const Real putDelta = BlackDeltaCalculator(Option::Put, deltaType(), spot()->value(),
                                                   ddom_, dfor_, stdDev)
                                  .deltaFromStrike(strike);
        return volByDelta(putDelta, Option::Put) - vol;
    }


    //! \name Quadratic smile section (delta-parameterized)
    //@{
    QuadraticSmileSection::QuadraticSmileSection(const Date& exerciseDate,
                                                 const Handle<Quote>& spot,
                                                 const ext::shared_ptr<FxSmileQuotes>& quotes,
                                                 const Handle<YieldTermStructure>& foreignDiscount,
                                                 const Handle<YieldTermStructure>& domesticDiscount,
                                                 DeltaVolQuote::DeltaType deltaType,
                                                 DeltaVolQuote::AtmType atmType,
                                                 const DayCounter& dayCounter,
                                                 const FxSettlementConvention& settlement,
                                                 const Date& referenceDate)
    : FxSmileSectionByDelta(exerciseDate, spot, quotes,
                            foreignDiscount, domesticDiscount,
                            deltaType, atmType, dayCounter, settlement, referenceDate)
    {
        params_.reserve(3);
    }

    QuadraticSmileSection::QuadraticSmileSection(Time exerciseTime,
                                                 const Handle<Quote>& spot,
                                                 const ext::shared_ptr<FxSmileQuotes>& quotes,
                                                 const Handle<YieldTermStructure>& foreignDiscount,
                                                 const Handle<YieldTermStructure>& domesticDiscount,
                                                 DeltaVolQuote::DeltaType deltaType,
                                                 DeltaVolQuote::AtmType atmType,
                                                 const DayCounter& dayCounter)
    : FxSmileSectionByDelta(exerciseTime, spot, quotes,
                            foreignDiscount, domesticDiscount,
                            deltaType, atmType, dayCounter)
    {
        params_.reserve(3);
    }

    Array QuadraticSmileSection::initialParams() const
    {
        // vol = a*delta^2 + b*delta + c
        // ATM put delta is conventionally -0.5, so at ATM:
        //   atm_vol = a*0.25 + b*(-0.5) + c
        // Start with a=0, b=0, c=atm_vol
        Array guess(3);
        guess[0] = 0.0;
        guess[1] = 0.0;
        guess[2] = referenceVol();
        return guess;
    }

    Volatility QuadraticSmileSection::_volByDelta(Real delta,
                                                   Real fwd,
                                                   Time tau,
                                                   const std::vector<Real>& params) const
    {
        return params[0] * delta * delta + params[1] * delta + params[2];
    }
    //@}

}
