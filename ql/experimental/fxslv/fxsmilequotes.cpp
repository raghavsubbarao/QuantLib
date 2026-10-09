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

        //! Broker-fly residual for one delta level.
        /*! Prices the market strangle (both legs struck at the broker
            vol \f$ \sigma_{ATM} + BF \f$) once, then for each candidate
            smile compares it with the strangle priced on the smile and
            converts the difference back to a broker-fly vol.
        */
        class StrangleHelper {
          public:
            StrangleHelper(const FxSmileSection& section,
                           Volatility marketAtm,
                           Real brokerFly,
                           Real delta)
            : section_(section), marketAtm_(marketAtm), brokerFly_(brokerFly) {
                const Real strdVol = marketAtm_ + brokerFly_;
                const Real htau = std::sqrt(section_.exerciseTime());
                const Real w = strdVol * htau;

                const Real spot = section_.spot()->value();
                const Real ddom = section_.domesticDiscountFactor();
                const Real dfor = section_.foreignDiscountFactor();
                const Real fwd = section_.forward();
                const DeltaVolQuote::DeltaType dt = section_.deltaType();

                callStrike_ = BlackDeltaCalculator(Option::Call, dt, spot, ddom, dfor, w)
                                  .strikeFromDelta(delta);
                putStrike_ = BlackDeltaCalculator(Option::Put, dt, spot, ddom, dfor, w)
                                 .strikeFromDelta(-delta);

                marketStranglePrice_ = BlackCalculator(Option::Call, callStrike_, fwd, w).value() +
                                       BlackCalculator(Option::Put, putStrike_, fwd, w).value();
            }

            //! Broker fly implied by the section's current smile.
            Real impliedQuote() const {
                const Real htau = std::sqrt(section_.exerciseTime());
                const Real fwd = section_.forward();

                // price the strangle on the calibrated smile
                const Real vc = section_.volByStrike(callStrike_);
                const Real vp = section_.volByStrike(putStrike_);
                const Real smileStranglePrice =
                    BlackCalculator(Option::Call, callStrike_, fwd, vc * htau).value() +
                    BlackCalculator(Option::Put, putStrike_, fwd, vp * htau).value();

                // convert back to a broker-fly vol: find bf such that the
                // strangle priced at atm + bf gives the smile price
                auto priceError = [&](Real bf) {
                    const Real w = (marketAtm_ + bf) * htau;
                    return BlackCalculator(Option::Call, callStrike_, fwd, w).value() +
                           BlackCalculator(Option::Put, putStrike_, fwd, w).value() -
                           smileStranglePrice;
                };

                Brent solver;
                solver.setMaxEvaluations(1000);
                const Real guess = brokerFly_;
                return solver.solve(priceError, 1.0e-12, guess, guess * 0.5, guess * 2.0);
            }

            //! Residual: market broker fly minus implied broker fly.
            Real flyError() const { return brokerFly_ - impliedQuote(); }

          private:
            const FxSmileSection& section_;
            Volatility marketAtm_;
            Real brokerFly_;
            Real callStrike_, putStrike_, marketStranglePrice_;
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
        // one broker-fly residual per delta level, priced at the market ATM
        std::vector<StrangleHelper> helpers;
        helpers.reserve(deltas_.size());
        for (Size i = 0; i < deltas_.size(); ++i)
            helpers.emplace_back(section, atm_->value(), butterflies_[i]->value(),
                                 std::fabs(deltas_[i]));

        // initial guess: smile strangles = broker flies
        std::vector<Real> smileStrangles(deltas_.size());
        for (Size i = 0; i < deltas_.size(); ++i)
            smileStrangles[i] = butterflies_[i]->value();

        // Solve for each smile strangle in turn.  With one delta this
        // converges in a single pass; with two or more, iterate until all
        // strangle errors are within tolerance.
        const Size maxOuterIter = 20;
        const Real tol = 1.0e-10;

        for (Size iter = 0; iter < maxOuterIter; ++iter) {
            Real maxErr = 0.0;

            for (Size i = 0; i < deltas_.size(); ++i) {
                // find smileStrangles[i] such that the smile 
                // reproduces the market strangle price
                auto error = [&](Real ss) -> Real {
                    smileStrangles[i] = ss;
                    fit(section, deltaVolQuotes(section, smileStrangles));
                    return helpers[i].flyError();
                };

                Brent solver;
                solver.setMaxEvaluations(1000);
                const Real guess = smileStrangles[i];
                smileStrangles[i] = solver.solve(error, 1.0e-12, guess, guess * 0.1, guess * 5.0);

                maxErr = std::max(maxErr, std::fabs(helpers[i].flyError()));
            }

            if (maxErr < tol)
                break;
        }

        // final fit with the converged smile strangles
        fit(section, deltaVolQuotes(section, smileStrangles));
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
