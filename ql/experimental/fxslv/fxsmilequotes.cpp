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
#include <algorithm>
#include <cmath>
#include <utility>

namespace QuantLib {

    namespace {

        //! Broker-strangle residual for one delta level, in price.
        /*! The market strangle has both legs struck, and priced, at the
            broker vol \f$ \sigma_{ATM} + BF \f$; it is priced once.  The
            residual is the premium of the same strangle priced on the
            section's current smile minus the market premium, divided by
            the market strangle's vega so that it reads as a vol and a
            tolerance means the same at any expiry.  Dividing by a constant
            does not move the root.
        */
        class StrangleHelper {
          public:
            StrangleHelper(const FxSmileSection& section,
                           Volatility marketAtm,
                           Real brokerFly,
                           Real delta)
            : section_(section) {
                const Time tau = section_.exerciseTime();
                const Real w = (marketAtm + brokerFly) * std::sqrt(tau);

                const Real spot = section_.spot()->value();
                const Real ddom = section_.domesticDiscountFactor();
                const Real dfor = section_.foreignDiscountFactor();
                const Real fwd = section_.forward();
                const DeltaVolQuote::DeltaType dt = section_.deltaType();

                callStrike_ = BlackDeltaCalculator(Option::Call, dt, spot, ddom, dfor, w)
                                  .strikeFromDelta(delta);
                putStrike_ = BlackDeltaCalculator(Option::Put, dt, spot, ddom, dfor, w)
                                 .strikeFromDelta(-delta);

                const BlackCalculator call(Option::Call, callStrike_, fwd, w);
                const BlackCalculator put(Option::Put, putStrike_, fwd, w);
                marketPrice_ = call.value() + put.value();
                marketVega_ = call.vega(tau) + put.vega(tau);
                QL_REQUIRE(marketVega_ > 0.0,
                           "market strangle has no vega for delta " << delta);
            }

            //! Smile strangle premium minus market premium, over the market vega.
            Real residual() const {
                const Real htau = std::sqrt(section_.exerciseTime());
                const Real fwd = section_.forward();
                const Real vc = section_.volByStrike(callStrike_);
                const Real vp = section_.volByStrike(putStrike_);
                const Real smilePrice =
                    BlackCalculator(Option::Call, callStrike_, fwd, vc * htau).value() +
                    BlackCalculator(Option::Put, putStrike_, fwd, vp * htau).value();
                return (smilePrice - marketPrice_) / marketVega_;
            }

          private:
            const FxSmileSection& section_;
            Real callStrike_, putStrike_;
            Real marketPrice_, marketVega_;
        };

    }


    void FxSmileQuotes::fit(const FxSmileSection& section,
                            std::vector<Handle<DeltaVolQuote>> quotes) {
        section.fitTo(std::move(quotes));
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

    std::vector<Handle<DeltaVolQuote>>
    FxRrBfQuotes::deltaVolQuotes(const FxSmileSection& section,
                                 const std::vector<Real>& smileStrangles) const {
        const Time tau = section.exerciseTime();
        const DeltaVolQuote::DeltaType deltaType = section.deltaType();

        std::vector<Handle<DeltaVolQuote>> quotes;
        quotes.reserve(1 + 2 * deltas_.size());
        quotes.emplace_back(ext::make_shared<DeltaVolQuote>(atm_, deltaType, tau, section.atmType()));

        for (Size j = 0; j < deltas_.size(); ++j) {
            const Real d = std::fabs(deltas_[j]);
            const Real rr = riskReversals_[j]->value();
            const Real bf = smileStrangles[j];

            const Volatility cVol = atm_->value() + bf + rr / 2.;
            const Volatility pVol = atm_->value() + bf - rr / 2.;

            quotes.emplace_back(ext::make_shared<DeltaVolQuote>(d, makeQuoteHandle(cVol), tau, deltaType));
            quotes.emplace_back(ext::make_shared<DeltaVolQuote>(-d, makeQuoteHandle(pVol), tau, deltaType));
        }
        return quotes;
    }

    void FxRrBfQuotes::calibrate(const FxSmileSection& section) const {
        if (flyType_ == MarketStrangle) {
            calibrateToMarketStrangles(section);
            return;
        }

        // Smile strangles convert algebraically to delta-vol quotes.
        std::vector<Real> smileStrangles(deltas_.size());
        for (Size i = 0; i < deltas_.size(); ++i)
            smileStrangles[i] = butterflies_[i]->value();
        fit(section, deltaVolQuotes(section, smileStrangles));
    }

    void FxRrBfQuotes::calibrateToMarketStrangles(const FxSmileSection& section) const {
        // one premium residual per delta level, priced at the market ATM
        std::vector<StrangleHelper> helpers;
        helpers.reserve(deltas_.size());
        for (Size i = 0; i < deltas_.size(); ++i)
            helpers.emplace_back(section, atm_->value(), butterflies_[i]->value(),
                                 std::fabs(deltas_[i]));

        // initial guess: smile strangles = broker flies
        std::vector<Real> smileStrangles(deltas_.size());
        for (Size i = 0; i < deltas_.size(); ++i)
            smileStrangles[i] = butterflies_[i]->value();

        // Solve for each smile strangle in turn, the others fixed, and
        // sweep until all strangles reprice.  With one delta a single
        // sweep suffices.  The residuals are measured after each sweep on
        // the smile fitted to the swept strangles, which is also the fit
        // the section keeps.
        const Size maxSweeps = 20;
        const Real tolerance = 1.0e-10; // vol-equivalent premium error
        const Real accuracy = 1.0e-12;
        const Real step = 1.0e-3;       // initial bracket step, in vol

        Real maxError = QL_MAX_REAL;
        for (Size sweep = 0; sweep < maxSweeps && maxError >= tolerance; ++sweep) {
            for (Size i = 0; i < deltas_.size(); ++i) {
                auto error = [&](Real ss) -> Real {
                    smileStrangles[i] = ss;
                    fit(section, deltaVolQuotes(section, smileStrangles));
                    return helpers[i].residual();
                };
                // No sign is assumed, so zero and negative strangles work;
                // the search only keeps both wing vols positive and the
                // strangle below the ATM vol (wings at twice the ATM).
                const Volatility atm = atm_->value();
                Brent solver;
                solver.setMaxEvaluations(1000);
                solver.setLowerBound(std::fabs(riskReversals_[i]->value()) / 2.0 - atm + accuracy);
                solver.setUpperBound(atm);
                smileStrangles[i] = solver.solve(error, accuracy, smileStrangles[i], step);
            }

            fit(section, deltaVolQuotes(section, smileStrangles));
            maxError = 0.0;
            for (const auto& h : helpers)
                maxError = std::max(maxError, std::fabs(h.residual()));
        }

        QL_ENSURE(maxError < tolerance,
                  "broker strangles not repriced after " << maxSweeps
                      << " sweeps: largest vol-equivalent premium error " << maxError);
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
        fit(section, quotes_);
    }

}
