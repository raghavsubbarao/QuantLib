#include <ql/math/solvers1d/brent.hpp>
#include <ql/experimental/fxslv/fxsmilesection.hpp>
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