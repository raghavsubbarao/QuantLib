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

#include <ql/experimental/fxslv/fxsmilequotes.hpp>
#include <ql/experimental/fxslv/fxsmilesection.hpp>
#include <ql/math/solvers1d/brent.hpp>
#include <ql/pricingengines/blackcalculator.hpp>
#include <ql/pricingengines/blackdeltacalculator.hpp>
#include <ql/quotes/simplequote.hpp>
#include <cmath>
#include <utility>

namespace QuantLib {

    Real FxVolTarget::residual(const FxSmileSection& section) const {
        return section.volResidual(strike_, vol_);
    }

    Real FxRiskReversalTarget::residual(const FxSmileSection& section) const {
        return section.volByDelta(delta_, Option::Call) - section.volByDelta(-delta_, Option::Put) -
               riskReversal_;
    }

    Real FxBrokerStrangleTarget::residual(const FxSmileSection& section) const {
        const Time tau = section.exerciseTime();
        const Real htau = std::sqrt(tau);

        const Real spot = section.spot()->value();
        const Real ddom = section.domesticDiscountFactor();
        const Real dfor = section.foreignDiscountFactor();
        const Real fwd = section.forward();

        const Real w = (atmVol_ + brokerFly_) * htau;
        const DeltaVolQuote::DeltaType dt = section.deltaType();
        
        const Rate callStrike = BlackDeltaCalculator(Option::Call, dt, spot, ddom, dfor, w).strikeFromDelta(delta_);
        const Rate putStrike = BlackDeltaCalculator(Option::Put, dt, spot, ddom, dfor, w).strikeFromDelta(-delta_);
        const BlackCalculator call(Option::Call, callStrike, fwd, w);
        const BlackCalculator put(Option::Put, putStrike, fwd, w);
        Real marketPrice = call.value() + put.value();
        Real marketVega = call.vega(tau) + put.vega(tau);
        QL_REQUIRE(marketVega > 0.0, "market strangle has no vega for delta " << delta);

        const Real vc = section.volByStrike(callStrike);
        const Real vp = section.volByStrike(putStrike);
        const Real smilePrice = BlackCalculator(Option::Call, callStrike, fwd, vc * htau).value() +
                                BlackCalculator(Option::Put, putStrike, fwd, vp * htau).value();
        return (smilePrice - marketPrice) / marketVega;
    }


    void FxSmileQuotes::fit(const FxSmileSection& section, FxSmileTargets targets) {
        section.fitToTargets(std::move(targets));
    }

    ext::shared_ptr<FxSmileTarget> FxSmileQuotes::volTarget(const FxSmileSection& section,
                                                            const DeltaVolQuote& quote) {
        // the quote's own vol fixes its strike
        const Volatility vol = quote.value();
        const Real w = vol * std::sqrt(section.exerciseTime());
        BlackDeltaCalculator calc(quote.atmType() == DeltaVolQuote::AtmNull && quote.delta() < 0 ?
                                      Option::Put :
                                      Option::Call,
                                  section.deltaType(), section.spot()->value(),
                                  section.domesticDiscountFactor(),
                                  section.foreignDiscountFactor(), w);
        const Rate strike = quote.atmType() == DeltaVolQuote::AtmNull ?
                                calc.strikeFromDelta(quote.delta()) :
                                calc.atmStrike(quote.atmType());
        return ext::make_shared<FxVolTarget>(strike, vol);
    }


    FxRrBfQuotes::FxRrBfQuotes(Handle<Quote> atm,
                               std::vector<Handle<Quote>> riskReversals,
                               std::vector<Handle<Quote>> butterflies,
                               std::vector<Real> deltas,
                               FlyType flyType)
    : atm_(std::move(atm)), riskReversals_(std::move(riskReversals)),
      butterflies_(std::move(butterflies)), deltas_(std::move(deltas)), flyType_(flyType) {
        QL_REQUIRE(riskReversals_.size() == deltas_.size(),
                   "risk reversal quotes must be the same size as deltas");
        QL_REQUIRE(butterflies_.size() == deltas_.size(),
                   "butterfly quotes must be the same size as deltas");
        registerWith(atm_);
        for (const auto& rr : riskReversals_)
            registerWith(rr);
        for (const auto& bf : butterflies_)
            registerWith(bf);
    }

    void FxRrBfQuotes::calibrate(const FxSmileSection& section) const {
        const Time tau = section.exerciseTime();
        const DeltaVolQuote::DeltaType deltaType = section.deltaType();

        FxSmileTargets targets;

        // atm vol target
        targets.push_back(volTarget(section, DeltaVolQuote(atm_, deltaType, tau, section.atmType())));

        for (Size i = 0; i < deltas_.size(); ++i) {
            const Real d = std::fabs(deltas_[i]);
            const Real rr = riskReversals_[i]->value();
            const Real bf = butterflies_[i]->value();

            if (flyType_ == SmileStrangle) {
                // smile strangles convert algebraically to points on the smile
                const Volatility cVol = atm_->value() + bf + rr / 2.;
                const Volatility pVol = atm_->value() + bf - rr / 2.;
                targets.push_back(volTarget(section, DeltaVolQuote(d, makeQuoteHandle(cVol), tau, deltaType)));
                targets.push_back(volTarget(section, DeltaVolQuote(-d, makeQuoteHandle(pVol), tau, deltaType)));
            } else {
                // broker strangles: risk reversal at the smile's deltas plus 
                // the strangle premium, fitted jointly with the ATM
                targets.push_back(ext::make_shared<FxRiskReversalTarget>(d, rr));
                targets.push_back(ext::make_shared<FxBrokerStrangleTarget>(atm_->value(), bf, d));
            }
        }

        fit(section, std::move(targets));
    }


    FxDeltaVolQuotes::FxDeltaVolQuotes(std::vector<Handle<DeltaVolQuote>> quotes)
    : quotes_(std::move(quotes)) {
        for (const auto& q : quotes_)
            registerWith(q);
    }

    Volatility FxDeltaVolQuotes::referenceVol() const {
        if (quotes_.empty())
            return 0.1;
        Real sumVol = 0.0;
        for (const auto& q : quotes_)
            sumVol += q->value();
        return sumVol / static_cast<Real>(quotes_.size());
    }

    void FxDeltaVolQuotes::calibrate(const FxSmileSection& section) const {
        FxSmileTargets targets;
        targets.reserve(quotes_.size());
        for (const auto& q : quotes_)
            targets.push_back(volTarget(section, **q));
        fit(section, std::move(targets));
    }

}
