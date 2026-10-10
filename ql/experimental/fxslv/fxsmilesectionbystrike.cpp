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
#include <ql/experimental/fxslv/fxrootbracketing.hpp>
#include <ql/experimental/fxslv/fxsmilesectionbystrike.hpp>

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
        return volByStrikeImpl(strike, fwd_, exerciseTime(), params_);
    }

    Volatility FxSmileSectionByStrike::volByDelta(Real delta, Option::Type parity) const 
    {
        calculate();
        return volByStrike(strikeByDelta(delta, parity));
    }

    Real FxSmileSectionByStrike::deltaByStrike(Rate strike, Option::Type parity) const 
    {
        calculate();

        return deltaConverter().delta(parity, strike,
                                       volByStrike(strike) * std::sqrt(exerciseTime()));
    }

    Rate FxSmileSectionByStrike::strikeByDelta(Real delta, Option::Type parity) const 
    {
        calculate();

        // premium-adjusted call deltas are attained twice below the peak;
        // the strike above it is the one quoted
        const bool paCall = parity == Option::Call && premiumAdjust();
        const Rate kPeak = paCall ? peakCallDeltaStrike() : Null<Rate>();
        if (paCall) {
            Real maxCallDelta = deltaByStrike(kPeak, Option::Call);
            QL_REQUIRE(delta <= maxCallDelta + QL_EPSILON, "Call delta out of range");
            if (std::fabs(delta - maxCallDelta) <= QL_EPSILON) {
                return kPeak;
            }
        }

        // The delta along the smile falls as the strike grows, for puts
        // and for calls above the peak, so walk from a start point towards
        // the root: only strikes between the two are visited.  Start at the
        // peak for premium-adjusted calls, where the error is not negative,
        // otherwise at the strike for the reference vol.
        const FxDeltaConverter conv = deltaConverter();
        const Real sqrtT = std::sqrt(exerciseTime());
        const Real stdDev = referenceVol() * sqrtT;
        const Rate start = paCall ? kPeak : conv.strike(parity, delta, stdDev);

        auto deltaError = [&](Real x) {
            const Rate strike = std::exp(x);
            return conv.delta(parity, strike, volByStrike(strike) * sqrtT) - delta;
        };
        return std::exp(detail::findRoot(deltaError, std::log(start), stdDev,
                                         detail::Slope::Decreasing, 1e-12));
    }



    //! \name Polynomial smile section
    //@{
    FxPolynomialSmileSection::FxPolynomialSmileSection(const Date& exerciseDate,
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

    FxPolynomialSmileSection::FxPolynomialSmileSection(Time exerciseTime,
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

    Array FxPolynomialSmileSection::initialParams() const
    {
        // vol = exp(a*x^2 + b*x + c), at ATM x = Phi(0) = 0.5
        // with a=b=0, c = log(atm_vol)
        Array guess(3);
        guess[0] = 0.0;
        guess[1] = 0.0;
        guess[2] = std::log(referenceVol());
        return guess;
    }

    Volatility FxPolynomialSmileSection::volByStrikeImpl(Rate strike,
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

    Volatility FxSabrSmileSection::volByStrikeImpl(Rate strike,
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

    Volatility FxSviSmileSection::volByStrikeImpl(Rate strike,
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
