#include <ql/math/distributions/normaldistribution.hpp>
#include <ql/math/solvers1d/brent.hpp>
#include <ql/experimental/fxslv/fxsmilesection.hpp>
#include <cmath>
#include <string>
#include <utility>

namespace QuantLib {

    FxSmileSection::FxSmileSection(const Date& exerciseDate,
                                   const Handle<Quote>& spot,
                                   const ext::shared_ptr<FxSmileQuotes>& quotes,
                                   const Handle<YieldTermStructure>& foreignDiscount,
                                   const Handle<YieldTermStructure>& domesticDiscount,
                                   DeltaVolQuote::DeltaType deltaType,
                                   DeltaVolQuote::AtmType atmType,
                                   const DayCounter& dayCounter,
                                   const FxSettlementConvention& settlement,
                                   const Date& referenceDate)
    : SmileSection(exerciseDate, dayCounter, referenceDate, ShiftedLognormal, 0.0),
      deltaType_(deltaType), atmType_(atmType), spot_(spot), smileQuotes_(quotes),
      foreignDiscount_(foreignDiscount), domesticDiscount_(domesticDiscount),
      settleConvention_(settlement), maxStrike_(QL_MAX_REAL), minStrike_(QL_EPSILON) {
        QL_REQUIRE(smileQuotes_, "no smile quotes given");
        registerWithMarketData();
    }

    FxSmileSection::FxSmileSection(Time exerciseTime,
                                   const Handle<Quote>& spot,
                                   const ext::shared_ptr<FxSmileQuotes>& quotes,
                                   const Handle<YieldTermStructure>& foreignDiscount,
                                   const Handle<YieldTermStructure>& domesticDiscount,
                                   DeltaVolQuote::DeltaType deltaType,
                                   DeltaVolQuote::AtmType atmType,
                                   const DayCounter& dayCounter)
    : SmileSection(exerciseTime, dayCounter, ShiftedLognormal, 0.0),
      deltaType_(deltaType), atmType_(atmType), spot_(spot), smileQuotes_(quotes),
      foreignDiscount_(foreignDiscount), domesticDiscount_(domesticDiscount),
      maxStrike_(QL_MAX_REAL), minStrike_(QL_EPSILON) {
        QL_REQUIRE(smileQuotes_, "no smile quotes given");
        registerWithMarketData();
    }

    void FxSmileSection::registerWithMarketData() {
        registerWith(spot_);
        registerWith(foreignDiscount_);
        registerWith(domesticDiscount_);
        registerWith(smileQuotes_);
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
        } else {
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
        // Called from performCalculations() after the fit.  The ATM strike
        // is the fixed point K = K_atm(vol(K)); search in log-strike from
        // the forward, in steps of one ATM standard deviation, widening as
        // needed, so that far-away strikes where a smile may be undefined
        // are only visited if the root is really out there.
        const Real spot = spot_->value();
        const Real stdDev = atm_->value() * std::sqrt(exerciseTime()); // reference vol

        auto atmStrikeError = [&](Real logStrike) {
            const Real strike = std::exp(logStrike);
            const Volatility v = volByStrike(strike);
            const Real kAtm = BlackDeltaCalculator(Option::Call, deltaType(), spot, ddom_, dfor_,
                                                   v * std::sqrt(exerciseTime()))
                                  .atmStrike(atmType());
            return logStrike - std::log(kAtm);
        };

        Brent solver;
        solver.setMaxEvaluations(1000);
        const Rate k = std::exp(solver.solve(atmStrikeError, 1.0e-12, std::log(fwd_), stdDev));

        atm_ = makeQuoteHandle(volByStrike(k));
    }

    void FxSmileSection::stripDeltaVolQuotes() const {
        // Seed with a vol that depends on the quotes only, so that the
        // same quotes always give the same smile; subclasses read atm_ in
        // initialParams().
        atm_ = makeQuoteHandle(smileQuotes_->referenceVol());

        // The quotes drive the fit; check they actually fitted the section,
        // otherwise it would silently keep a previous calibration.
        calibrating_ = true;
        fitted_ = false;
        try {
            smileQuotes_->calibrate(*this);
        } catch (...) {
            calibrating_ = false;
            throw;
        }
        calibrating_ = false;
        QL_ENSURE(fitted_, "smile quotes did not fit the smile section");

        // the calibrated atm might differ from the quoted one, so take it
        // from the calibrated smile
        calculateAtm();
    }

    void FxSmileSection::fitTo(std::vector<Handle<DeltaVolQuote>> quotes) const {
        QL_REQUIRE(calibrating_, "smile section can only be fitted while its quotes calibrate it");
        quotes_ = std::move(quotes);
        calibrate();
        fitted_ = true;
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

    Real FxSmileSection::volDerivative(Rate strike) const {
        QL_REQUIRE(strike > 0.0, "positive strike required: " << strike);
        // central difference; vols are far smoother than prices, so a
        // relative step of 1e-4 balances truncation and rounding errors
        const Real h = 1.0e-4 * strike;
        return (volByStrike(strike + h) - volByStrike(strike - h)) / (2.0 * h);
    }

    Real FxSmileSection::normedCallPrice(Real moneyness) const {
        QL_REQUIRE(moneyness > 0.0, "positive moneyness required: " << moneyness);
        calculate();

        const Rate strike = moneyness * fwd_;
        const Real w = volByStrike(strike) * std::sqrt(exerciseTime());
        return BlackCalculator(Option::Call, strike, fwd_, w).value() / fwd_;
    }

    Probability FxSmileSection::exerciseProbability(Real moneyness) const {
        QL_REQUIRE(moneyness > 0.0, "positive moneyness required: " << moneyness);
        calculate();

        // P(S_T > K) = -dC/dK for the undiscounted call C(K) = Black(F, K, vol(K)):
        //            = N(d2) - F n(d1) sqrt(T) vol'(K)
        const Rate strike = moneyness * fwd_;
        const Time tau = exerciseTime();
        const Real w = volByStrike(strike) * std::sqrt(tau);
        QL_REQUIRE(w > 0.0, "positive standard deviation required at strike " << strike);
        const Real d1 = (std::log(fwd_ / strike) + 0.5 * w * w) / w;
        const Real d2 = d1 - w;

        CumulativeNormalDistribution N;
        NormalDistribution n;
        return N(d2) - fwd_ * n(d1) * std::sqrt(tau) * volDerivative(strike);
    }

    Real FxSmileSection::moneynessFromProbability(Probability p) const {
        QL_REQUIRE(p > 0.0 && p < 1.0, "probability must be in (0, 1): " << p);
        calculate();

        // Solve in log-moneyness, starting from the flat-smile answer
        // N(d2) = p at the reference vol and widening the bracket as needed.
        const Real stdDev = atm_->value() * std::sqrt(exerciseTime());
        const Real guess = -stdDev * InverseCumulativeNormal()(p) - 0.5 * stdDev * stdDev;
        auto error = [&](Real x) { return exerciseProbability(std::exp(x)) - p; };

        Brent solver;
        solver.setMaxEvaluations(1000);
        const Real k = std::exp(solver.solve(error, 1.0e-12, guess, stdDev));

        // The inverse is only unique where the density is non-negative,
        // i.e. the exercise probability decreases with moneyness.
        const Real h = 1.0e-4 * k;
        QL_ENSURE(exerciseProbability(k + h) <= exerciseProbability(k - h) + 1.0e-12,
                  "negative density at moneyness " << k
                      << ": the smile has butterfly arbitrage there");
        return k;
    }

}