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

#include <ql/math/distributions/normaldistribution.hpp>
#include <ql/math/optimization/constraint.hpp>
#include <ql/math/optimization/endcriteria.hpp>
#include <ql/math/optimization/levenbergmarquardt.hpp>
#include <ql/math/solvers1d/brent.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/sabr.hpp>
#include <ql/experimental/fxslv/fxsmilesectionbystrike.hpp>
#include <ql/pricingengines/blackdeltacalculator.hpp>

namespace QuantLib {

    FxSmileSectionByStrike::FxSmileSectionByStrike(const Date& exerciseDate,
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

    FxSmileSectionByStrike::FxSmileSectionByStrike(Time exerciseTime,
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

    Volatility FxSmileSectionByStrike::volByStrike(Rate strike) const 
    {
        calculate();
        return _volByStrike(strike, fwd_, exerciseTime(), params_);
    }

    Volatility FxSmileSectionByStrike::volByDelta(Real delta, Option::Type parity) const 
    {
        calculate();
        return volByStrike(strikeByDelta(delta, parity));
    }

    Real FxSmileSectionByStrike::deltaByStrike(Rate strike, Option::Type parity) const 
    {
        calculate();

        Volatility v = volByStrike(strike);
        return BlackDeltaCalculator(parity, deltaType(), spot()->value(), ddom_, dfor_,
                                    v * sqrt(exerciseTime()))
            .deltaFromStrike(strike);
    }

    Rate FxSmileSectionByStrike::strikeByDelta(Real delta, Option::Type parity) const 
    {
        calculate();

        if (parity == Option::Call && premiumAdjust()) {
            Real maxCallDelta = deltaByStrike(minStrike(), Option::Call);
            QL_REQUIRE(delta <= maxCallDelta + QL_EPSILON, "Call delta out of range");
            if (std::fabs(delta - maxCallDelta) <= QL_EPSILON) {
                return minStrike();
            }
        }

        Rate k0 = BlackDeltaCalculator(parity, deltaType(), spot()->value(), ddom_, dfor_,
                                       referenceVol() * sqrt(exerciseTime()))
                      .strikeFromDelta(delta);
        Rate kmin = (premiumAdjust() && parity==Option::Call) ? minStrike() : QL_EPSILON;
        Rate kmax = k0 * 10;

        auto deltaError = [&](Real strike) {
            Volatility v = volByStrike(strike);
            Real d = BlackDeltaCalculator(parity, deltaType(), spot()->value(), ddom_, dfor_,
                                          v * sqrt(exerciseTime()))
                         .deltaFromStrike(strike);
            return d - delta;
        };

        Brent solver;
        solver.setMaxEvaluations(10000);
        Rate k = solver.solve([&](Real strike) { return deltaError(strike); }, 
                              1e-12, k0, kmin, kmax);
        return k;
    }



    //! \name Polynomial smile section
    //@{
    PolynomialSmileSection::PolynomialSmileSection(const Date& exerciseDate,
                                                   const Handle<Quote>& spot,
                                                   const ext::shared_ptr<FxSmileQuotes>& quotes,
                                                   const Handle<YieldTermStructure>& foreignDiscount,
                                                   const Handle<YieldTermStructure>& domesticDiscount,
                                                   DeltaVolQuote::DeltaType deltaType,
                                                   DeltaVolQuote::AtmType atmType,
                                                   const DayCounter& dayCounter,
                                                   const FxSettlementConvention& settlement,
                                                   const Date& referenceDate)
    : FxSmileSectionByStrike(exerciseDate, spot, quotes,
                             foreignDiscount, domesticDiscount,
                             deltaType, atmType, dayCounter, settlement, referenceDate)
    {
        params_.reserve(3);
    }

    PolynomialSmileSection::PolynomialSmileSection(Time exerciseTime,
                                                   const Handle<Quote>& spot,
                                                   const ext::shared_ptr<FxSmileQuotes>& quotes,
                                                   const Handle<YieldTermStructure>& foreignDiscount,
                                                   const Handle<YieldTermStructure>& domesticDiscount,
                                                   DeltaVolQuote::DeltaType deltaType,
                                                   DeltaVolQuote::AtmType atmType,
                                                   const DayCounter& dayCounter)
    : FxSmileSectionByStrike(exerciseTime, spot, quotes,
                             foreignDiscount, domesticDiscount,
                             deltaType, atmType, dayCounter)
    {
        params_.reserve(3);
    }

    Array PolynomialSmileSection::initialParams() const
    {
        // vol = exp(a*x^2 + b*x + c), at ATM x = Phi(0) = 0.5
        // with a=b=0, c = log(atm_vol)
        Array guess(3);
        guess[0] = 0.0;
        guess[1] = 0.0;
        guess[2] = std::log(referenceVol());
        return guess;
    }

    Volatility PolynomialSmileSection::_volByStrike(Rate strike,
                                                    Real fwd,
                                                    Time tau,
                                                    const std::vector<Real>& params) const
    {
        CumulativeNormalDistribution f;
        Real atmfVol = std::exp(params[0] / 4. + params[1] / 2. + params[2]);
        Real x = f(std::log(fwd / strike) / (atmfVol * std::sqrt(tau)));
        return std::exp(params[0] * x * x + params[1] * x + params[2]);
    }
    //@}


    //! \name SABR smile section
    //@{
    FxSabrSmileSection::FxSabrSmileSection(const Date& exerciseDate,
                                           const Handle<Quote>& spot,
                                           const ext::shared_ptr<FxSmileQuotes>& quotes,
                                           const Handle<YieldTermStructure>& foreignDiscount,
                                           const Handle<YieldTermStructure>& domesticDiscount,
                                           DeltaVolQuote::DeltaType deltaType,
                                           DeltaVolQuote::AtmType atmType,
                                           const DayCounter& dayCounter,
                                           const FxSettlementConvention& settlement,
                                           const Date& referenceDate)
    : FxSmileSectionByStrike(exerciseDate, spot, quotes,
                             foreignDiscount, domesticDiscount,
                             deltaType, atmType, dayCounter, settlement, referenceDate) 
    {
        params_.reserve(3);
    }

    FxSabrSmileSection::FxSabrSmileSection(Time exerciseTime,
                                           const Handle<Quote>& spot,
                                           const ext::shared_ptr<FxSmileQuotes>& quotes,
                                           const Handle<YieldTermStructure>& foreignDiscount,
                                           const Handle<YieldTermStructure>& domesticDiscount,
                                           DeltaVolQuote::DeltaType deltaType,
                                           DeltaVolQuote::AtmType atmType,
                                           const DayCounter& dayCounter)
    : FxSmileSectionByStrike(exerciseTime, spot, quotes,
                             foreignDiscount, domesticDiscount,
                             deltaType, atmType, dayCounter) 
    {
        params_.reserve(3);
    }

    Array FxSabrSmileSection::initialParams() const
    {
        // SABR params: alpha, nu, rho (beta fixed at 1)
        Array guess(3);
        guess[0] = referenceVol();  // alpha ~ atm vol for beta=1
        guess[1] = 0.5;            // nu
        guess[2] = 0.0;            // rho
        return guess;
    }

    Volatility FxSabrSmileSection::_volByStrike(Rate strike,
                                                Real fwd,
                                                Time tau,
                                                const std::vector<Real>& params) const
    {
        return unsafeShiftedSabrVolatility(strike, fwd, tau,
                                           params[0], 1.0, params[1], params[2],
                                           0.0, volatilityType());
    }
    //@}


    //! \name SVI smile section
    //@{
    FxSviSmileSection::FxSviSmileSection(const Date& exerciseDate,
                                         const Handle<Quote>& spot,
                                         const ext::shared_ptr<FxSmileQuotes>& quotes,
                                         const Handle<YieldTermStructure>& foreignDiscount,
                                         const Handle<YieldTermStructure>& domesticDiscount,
                                         DeltaVolQuote::DeltaType deltaType,
                                         DeltaVolQuote::AtmType atmType,
                                         const DayCounter& dayCounter,
                                         const FxSettlementConvention& settlement,
                                         const Date& referenceDate)
    : FxSmileSectionByStrike(exerciseDate, spot, quotes,
                             foreignDiscount, domesticDiscount,
                             deltaType, atmType, dayCounter, settlement, referenceDate)
    {
        params_.reserve(5);
    }

    FxSviSmileSection::FxSviSmileSection(Time exerciseTime,
                                         const Handle<Quote>& spot,
                                         const ext::shared_ptr<FxSmileQuotes>& quotes,
                                         const Handle<YieldTermStructure>& foreignDiscount,
                                         const Handle<YieldTermStructure>& domesticDiscount,
                                         DeltaVolQuote::DeltaType deltaType,
                                         DeltaVolQuote::AtmType atmType,
                                         const DayCounter& dayCounter)
    : FxSmileSectionByStrike(exerciseTime, spot, quotes,
                             foreignDiscount, domesticDiscount,
                             deltaType, atmType, dayCounter)
    {
        params_.reserve(5);
    }

    Array FxSviSmileSection::initialParams() const
    {
        // SVI raw: w(k) = a + b*(rho*(k-m) + sqrt((k-m)^2 + sigma^2))
        // At ATM k=0: w(0) = a + b*(rho*(-m) + sqrt(m^2 + sigma^2))
        // Start with m=0, rho=0: w(0) = a + b*sigma = atm_vol^2 * tau
        Real atmVar = referenceVol() * referenceVol();
        Array guess(5);
        guess[0] = atmVar;  // a: base variance level
        guess[1] = 0.1;     // b: slope
        guess[2] = 0.0;     // rho: skew
        guess[3] = 0.0;     // m: translation
        guess[4] = 0.1;     // sigma: curvature
        return guess;
    }

    Volatility FxSviSmileSection::_volByStrike(Rate strike,
                                                Real fwd,
                                                Time tau,
                                                const std::vector<Real>& params) const
    {
        Real k = std::log(strike / fwd);
        Real km = k - params[3];
        Real totalVar = params[0] + params[1] * (params[2] * km +
                        std::sqrt(km * km + params[4] * params[4]));
        return std::sqrt(std::max(totalVar, 0.0) / tau);
    }
    //@}

}
