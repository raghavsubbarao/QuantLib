#include <ql/math/solvers1d/brent.hpp>
#include <ql/experimental/fxslv/fxsmilesection.hpp>
#include <string>

namespace QuantLib {

    FxSmileSection::FxSmileSection(const Date& exerciseDate,
                                   const Handle<Quote>& spot,
                                   const Handle<Quote>& atm,
                                   const std::vector<Handle<Quote>>& rrs,
                                   const std::vector<Handle<Quote>>& bfs,
                                   const std::vector<Real>& deltas,
                                   const Handle<YieldTermStructure>& foreignDiscount,
                                   const Handle<YieldTermStructure>& domesticDiscount,
                                   DeltaVolQuote::DeltaType deltaType,
                                   DeltaVolQuote::AtmType atmType,
                                   FlyType flyType,
                                   const DayCounter& dayCounter,
                                   const FxSettlementConvention& settlement,
                                   const Date& referenceDate)
    : SmileSection(exerciseDate, dayCounter, referenceDate, ShiftedLognormal, 0.0),
      spot_(spot), rrs_(rrs), bfs_(bfs), deltas_(deltas),
      foreignDiscount_(foreignDiscount), domesticDiscount_(domesticDiscount),
      settleConvention_(settlement),
      deltaType_(deltaType), atmType_(atmType), flyType_(flyType), isDeltaVolQuote_(false),
      atmInput_(atm), quotesInput_(),
      atm_(), quotes_(), maxStrike_(QL_MAX_REAL), minStrike_(QL_EPSILON)
    {
        QL_REQUIRE(rrs.size() == deltas.size(),
                   "risk reversal quotes must be the same size as deltas");
        QL_REQUIRE(bfs.size() == deltas.size(), 
                   "butterfly quotes must be the same size as deltas");
        registerWithMarketData();
    }

    FxSmileSection::FxSmileSection(Time exerciseTime,
                                   const Handle<Quote>& spot,
                                   const Handle<Quote>& atm,
                                   const std::vector<Handle<Quote>>& rrs,
                                   const std::vector<Handle<Quote>>& bfs,
                                   const std::vector<Real>& deltas,
                                   const Handle<YieldTermStructure>& foreignDiscount,
                                   const Handle<YieldTermStructure>& domesticDiscount,
                                   DeltaVolQuote::DeltaType deltaType,
                                   DeltaVolQuote::AtmType atmType,
                                   FlyType flyType,
                                   const DayCounter& dayCounter)
    : SmileSection(exerciseTime, dayCounter, ShiftedLognormal, 0.0),
      spot_(spot), rrs_(rrs), bfs_(bfs), deltas_(deltas),
      foreignDiscount_(foreignDiscount), domesticDiscount_(domesticDiscount),
      deltaType_(deltaType), atmType_(atmType), flyType_(flyType), isDeltaVolQuote_(false),
      atmInput_(atm), quotesInput_(),
      atm_(), quotes_(), maxStrike_(QL_MAX_REAL), minStrike_(QL_EPSILON)
    {
        QL_REQUIRE(rrs.size() == deltas.size(),
                   "risk reversal quotes must be the same size as deltas");
        QL_REQUIRE(bfs.size() == deltas.size(), 
                   "butterfly quotes must be the same size as deltas");
        registerWithMarketData();
    }

    FxSmileSection::FxSmileSection(const Date& exerciseDate,
                                   const Handle<Quote>& spot,
                                   const std::vector<Handle<DeltaVolQuote>>& quotes,
                                   const Handle<YieldTermStructure>& foreignDiscount,
                                   const Handle<YieldTermStructure>& domesticDiscount,
                                   DeltaVolQuote::DeltaType deltaType,
                                   DeltaVolQuote::AtmType atmType,
                                   FlyType flyType,
                                   const DayCounter& dayCounter,
                                   const FxSettlementConvention& settlement,
                                   const Date& referenceDate)
    : SmileSection(exerciseDate, dayCounter, referenceDate, ShiftedLognormal, 0.0), 
      spot_(spot), rrs_(), bfs_(), deltas_(),
      foreignDiscount_(foreignDiscount), domesticDiscount_(domesticDiscount),
      settleConvention_(settlement),
      deltaType_(deltaType), atmType_(atmType), flyType_(flyType), isDeltaVolQuote_(true),
      atmInput_(), quotesInput_(quotes),
      atm_(), quotes_(), maxStrike_(QL_MAX_REAL), minStrike_(QL_EPSILON)
    {
        registerWithMarketData();
    }

    FxSmileSection::FxSmileSection(Time exerciseTime,
                                   const Handle<Quote>& spot,
                                   const std::vector<Handle<DeltaVolQuote>>& quotes,
                                   const Handle<YieldTermStructure>& foreignDiscount,
                                   const Handle<YieldTermStructure>& domesticDiscount,
                                   DeltaVolQuote::DeltaType deltaType,
                                   DeltaVolQuote::AtmType atmType,
                                   FlyType flyType,
                                   const DayCounter& dayCounter)
    : SmileSection(exerciseTime, dayCounter, ShiftedLognormal, 0.0), 
      spot_(spot), rrs_(), bfs_(), deltas_(),
      foreignDiscount_(foreignDiscount), domesticDiscount_(domesticDiscount),
      deltaType_(deltaType), atmType_(atmType), flyType_(flyType), isDeltaVolQuote_(true),
      atmInput_(), quotesInput_(quotes),
      atm_(), quotes_(), maxStrike_(QL_MAX_REAL), minStrike_(QL_EPSILON)
    {
        registerWithMarketData();
    }

    void FxSmileSection::registerWithMarketData() 
    {
        registerWith(spot_);
        registerWith(foreignDiscount_);
        registerWith(domesticDiscount_);
        
        if (isDeltaVolQuote()) {
            for (auto& q : quotesInput_) registerWith(q);
        }
        else {
            registerWith(atmInput_);
            for (auto& r : rrs_) registerWith(r);
            for (auto& b : bfs_) registerWith(b);
        }
    }

    namespace {

        void checkCurveCovers(const Handle<YieldTermStructure>& curve,
                              const std::string& name,
                              const Date& spotDate,
                              const Date& deliveryDate) {
            QL_REQUIRE(!curve.empty(), name << " discount curve is empty");
            QL_REQUIRE(curve->referenceDate() <= spotDate,
                       name << " discount curve reference date (" << curve->referenceDate()
                            << ") is after the spot date (" << spotDate << ")");
            QL_REQUIRE(curve->allowsExtrapolation() || deliveryDate <= curve->maxDate(),
                       name << " discount curve ends (" << curve->maxDate()
                            << ") before the delivery date (" << deliveryDate << ")");
        }

    }

    Date FxSmileSection::spotDate() const {
        QL_REQUIRE(settleConvention_, "spot date is only defined for sections built from dates");
        calculate();
        return spotDate_;
    }

    Date FxSmileSection::deliveryDate() const {
        QL_REQUIRE(settleConvention_, "delivery date is only defined for sections built from dates");
        calculate();
        return deliveryDate_;
    }

    void FxSmileSection::calculateForward() const {
        if (settleConvention_) {
            // Date mode: rates run from the spot date of the trade date to
            // the delivery date of the expiry.  Taking ratios of discount
            // factors by date makes the result independent of the curves'
            // own reference dates and day counters.
            spotDate_ = settleConvention_->spotDate(referenceDate());
            deliveryDate_ = settleConvention_->deliveryDate(exerciseDate());
            checkCurveCovers(foreignDiscount_, "foreign", spotDate_, deliveryDate_);
            checkCurveCovers(domesticDiscount_, "domestic", spotDate_, deliveryDate_);
            dfor_ = foreignDiscount_->discount(deliveryDate_) / foreignDiscount_->discount(spotDate_);
            ddom_ = domesticDiscount_->discount(deliveryDate_) / domesticDiscount_->discount(spotDate_);
        } 
        else {
            // Time mode: the expiry time is read on the curves' time axis,
            // which is only well defined if both curves share it.
            QL_REQUIRE(!foreignDiscount_.empty() && !domesticDiscount_.empty(), "empty discount curve");
            QL_REQUIRE(foreignDiscount_->referenceDate() == domesticDiscount_->referenceDate(),
                       "time-based FX smile section requires both discount curves to have the "
                       "same reference date: foreign "
                           << foreignDiscount_->referenceDate() << ", domestic "
                           << domesticDiscount_->referenceDate());
            QL_REQUIRE(foreignDiscount_->dayCounter() == domesticDiscount_->dayCounter(),
                       "time-based FX smile section requires both discount curves to have the "
                       "same day counter: foreign "
                           << foreignDiscount_->dayCounter() << ", domestic "
                           << domesticDiscount_->dayCounter());
            dfor_ = foreignDiscount_->discount(exerciseTime());
            ddom_ = domesticDiscount_->discount(exerciseTime());
        }
        fwd_ = spot_->value() * dfor_ / ddom_;
    }

    void FxSmileSection::calculateAtm() const {
        calculate(); // should not be necc but force calibration!

        const Real spot = spot_->value();
        const Real fwd = fwd_;

        auto atmStrkikeError = [&](Real strike) {
            Volatility v = volByStrike(strike);
            Real k_atm = BlackDeltaCalculator(Option::Call, deltaType(), spot, ddom_, dfor_,
                                              v * sqrt(exerciseTime()))
                             .atmStrike(atmType());
            return strike - k_atm;
        };

        Brent solver;
        solver.setMaxEvaluations(10000);
        Rate k = solver.solve([&](Real strike) { return atmStrkikeError(strike); }, 1e-12, fwd, fwd / 10, 10 * fwd);

        atm_ = makeQuoteHandle(volByStrike(k));
    }

    void FxSmileSection::stripDeltaVolQuotes() const {

        if (isDeltaVolQuote()) 
        {
            // Copy the immutable input quotes into the mutable workspace so that
            // calibrate() always reads from quotes_ regardless of which path we are on.
            quotes_ = quotesInput_;

            // initialParams() in subclasses (e.g. fxSabrSmileSection) reads atm_->value()
            // to seed the optimisation.  atm_ is normally set by calculateAtm() *after*
            // calibrate(), so it is still empty here.  Provide a rough initial guess —
            // the average of all input quote vols — so that the handle is never empty
            // when calibrate() runs.  calculateAtm() below will overwrite atm_ with the
            // proper value derived from the fitted smile.
            if (atm_.empty()) {
                Real sumVol = 0.0;
                for (const auto& q : quotesInput_)
                    sumVol += q->value();
                atm_ = makeQuoteHandle(quotesInput_.empty() ? 0.1 : sumVol / static_cast<Real>(quotesInput_.size()));
            }

            calibrate();

            // When calibrating from delta-vol quotes the atm is not known a priori,
            // so derive it from the fitted smile.
            calculateAtm();
        }
        else if (flyType() == FlyType::MarketStrangle) 
        {
            // atm_ is the market input for this path.
            atm_ = atmInput_;

            // Create a strangle helper for each delta level.
            std::vector<FxStrangleHelper<FxSmileSection>> helpers;
            helpers.reserve(deltas_.size());
            for (Size i = 0; i < deltas_.size(); ++i) {
                helpers.emplace_back(bfs_[i], std::fabs(deltas_[i]));
                helpers.back().setSmileSection(const_cast<FxSmileSection*>(this));
                helpers.back().initialize();
            }

            // Initial guess: smile strangles = broker flies
            std::vector<Real> smileStrangles(deltas_.size());
            for (Size i = 0; i < deltas_.size(); ++i) {
                smileStrangles[i] = bfs_[i]->value();
            }

            // Iteratively solve for each smile strangle.  In the
            // three-point case (one delta) this converges in a single
            // pass; with two or more deltas we iterate until the
            // strangle errors are all within tolerance.
            const Size maxOuterIter = 20;
            const Real tol = 1.0e-10;

            for (Size iter = 0; iter < maxOuterIter; ++iter) 
            {
                Real maxErr = 0.0;

                for (Size i = 0; i < deltas_.size(); ++i) 
                {
                    // Objective: find smileStrangles[i] such that
                    // the smile reproduces the market strangle price.
                    auto error = [&](Real ss) -> Real {
                        smileStrangles[i] = ss;

                        // Rebuild delta-vol quotes from current smile strangles
                        quotes_.clear();
                        quotes_.push_back(Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(atm(), deltaType(),
                                                                                                 exerciseTime(), atmType())));

                        for (Size j = 0; j < deltas_.size(); ++j)
                        {
                            Real d = std::fabs(deltas_[j]);
                            Real rr = rrs_[j]->value();
                            Real bf = smileStrangles[j];

                            Volatility cVol = atm_->value() + bf + rr / 2.;
                            Volatility pVol = atm_->value() + bf - rr / 2.;

                            quotes_.push_back(Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(d, makeQuoteHandle(cVol),
                                                                                                     exerciseTime(), deltaType_)));
                            quotes_.push_back(Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(-d, makeQuoteHandle(pVol),
                                                                                                     exerciseTime(), deltaType_)));
                        }

                        calibrate();
                        return helpers[i].flyError();
                    };

                    Brent solver;
                    solver.setMaxEvaluations(1000);
                    Real guess = smileStrangles[i];
                    smileStrangles[i] = solver.solve(error, 1.0e-12, guess, guess * 0.1, guess * 5.0);

                    maxErr = std::max(maxErr, std::fabs(helpers[i].flyError()));
                }

                if (maxErr < tol)
                    break;
            }

            // Final calibration with converged smile strangles
            quotes_.clear();
            quotes_.push_back(Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(atm(), deltaType(),
                                                                                     exerciseTime(), atmType())));

            for (Size i = 0; i < deltas_.size(); ++i) {
                Real d = std::fabs(deltas_[i]);
                Real rr = rrs_[i]->value();
                Real bf = smileStrangles[i];

                Volatility cVol = atm_->value() + bf + rr / 2.;
                Volatility pVol = atm_->value() + bf - rr / 2.;

                quotes_.push_back(Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(d, makeQuoteHandle(cVol),
                                                                                         exerciseTime(), deltaType_)));
                quotes_.push_back(Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(-d, makeQuoteHandle(pVol),
                                                                                         exerciseTime(), deltaType_)));
            }

            calibrate();

            // the calibrated atm might differ from the input
            // so get the atm from the calibrated smile section
            calculateAtm();
        }
        else {
            // Calibrate from RRs and flies, where the flies are smile strangles.
            // This is easily handled algebraically: convert to delta-vol quotes
            // (stored in the mutable workspace quotes_) then call calibrate().
            // atm_ is the market input for this path.
            atm_ = atmInput_;
            quotes_.clear();

            // handle the atm
            quotes_.push_back(Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(atm(), deltaType(),
                                                                                     exerciseTime(), atmType())));

            for (Size i = 0; i < deltas_.size(); ++i)
            {
                Real d = std::fabs(deltas_[i]);
                Real rr = rrs_[i]->value();
                Real bf = bfs_[i]->value();

                Volatility cVol = atm_->value() + bf + rr / 2.;
                Volatility pVol = atm_->value() + bf - rr / 2.;

                quotes_.push_back(Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(d, makeQuoteHandle(cVol),
                                                                                         exerciseTime(), deltaType_)));
                quotes_.push_back(Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(-d, makeQuoteHandle(pVol),
                                                                                         exerciseTime(), deltaType_)));
            }

            calibrate();

            // the calibrated atm might differ from the input
            // so get the atm from the calibrated smile section
            calculateAtm();
        }
        
    }

    void FxSmileSection::adjustStrikes() const {
        if (premiumAdjust()) {
            calculate();  // should not be necc but force calibration!

            CumulativeNormalDistribution f;

            auto ddelta_dk = [&](Real strike) {
                Volatility w = volByStrike(strike) * std::sqrt(exerciseTime());
                Real d = std::log(fwd_ / strike) / w - w / 2.;
                return f(d) - f.derivative(d) / w;
            };

            QL_ASSERT((ddelta_dk(fwd_) < 0), "call delta should be well defined at the fwd");

            Real k_min = fwd_ * std::exp(-atm()->value() * exerciseTime());
            while (ddelta_dk(k_min) < 0) {
                k_min = 0.95 * k_min;
            }

            Brent solver;
            Rate k = solver.solve([&](Real strike) { return ddelta_dk(strike); }, 
                                  1e-12, (k_min + fwd_) / 2., k_min, fwd_);

            minStrike_ = k;
        }

        // assumes the atm vol is known: either via market input or calibration!
        atmStrike_ = BlackDeltaCalculator(Option::Call, deltaType(), spot()->value(), ddom_, dfor_,
                                          atm()->value() * sqrt(exerciseTime()))
                         .atmStrike(atmType());

    }

    void FxSmileSection::performCalculations() const {
        calculateForward();
        stripDeltaVolQuotes();
        adjustStrikes();
    }

    Real FxSmileSection::normedCallPrice(Rate strike) const {
        calculate();

        Real w = volByStrike(strike) * std::sqrt(exerciseTime());
        BlackCalculator bc = BlackCalculator(Option::Call, strike, fwd_, w);
        return bc.value() / fwd_;
    }

    Real FxSmileSection::normedProbability(Rate strike, Real eps) const {
        QL_REQUIRE((eps > 0) && (eps < 1.), "eps should be between 0 and 1");

        calculate();
        
        Real ncp_dn = normedCallPrice(strike - fwd_ * eps);
        Real ncp_up = normedCallPrice(strike + fwd_ * eps);
        return (ncp_dn - ncp_up) / (2. * eps);
    }

    Rate FxSmileSection::strikeFromNormProb(Real q) const {
        QL_REQUIRE((q > 0.) && (q < 1.), "q should be between 0 and 1.");

        calculate();

        auto normProbError = [&](Rate strike) { 
            return 100 * (normedProbability(strike) - q);
        };

        Bisection solver;
        solver.setMaxEvaluations(10000);
        /*Real cd = normedProbability(fwd_ / 10);
        Real c0 = normedProbability(fwd_);
        Real cu = normedProbability(fwd_ * 10);*/
        return solver.solve([&](Rate strike) { return normProbError(strike); }, 
                                1e-12, fwd_, fwd_ / 10., fwd_ * 10.);
    }

}