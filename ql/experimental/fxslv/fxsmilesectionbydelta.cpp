#include <ql/math/optimization/constraint.hpp>
#include <ql/math/optimization/endcriteria.hpp>
#include <ql/math/optimization/levenbergmarquardt.hpp>
#include <ql/math/solvers1d/brent.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/experimental/fxslv/fxsmilesectionbydelta.hpp>
#include <cmath>

namespace QuantLib {

    fxSmileSectionByDelta::fxSmileSectionByDelta(const Date& exerciseDate,
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

    fxSmileSectionByDelta::fxSmileSectionByDelta(Time exerciseTime,
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

    Volatility fxSmileSectionByDelta::volByStrike(Rate strike) const 
    {
        calculate();

        Real delta = deltaByStrike(strike, Option::Type::Put);
        return volByDelta(delta, Option::Type::Put);
    }

    Volatility fxSmileSectionByDelta::volByDelta(Real delta, Option::Type parity) const 
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
                Volatility v = volByStrike(minStrike());
                Real maxCallDelta = BlackDeltaCalculator(Option::Call, deltaType(), spot()->value(),
                                                         ddom_, dfor_, v * sqrt(exerciseTime()))
                                        .deltaFromStrike(minStrike());
                QL_REQUIRE(delta <= maxCallDelta + QL_EPSILON, "Call delta out of range");
                if (std::fabs(delta - maxCallDelta) <= QL_EPSILON) {
                    return v;
                }

                // for other strikes we need to adjust the call delta to a put delta
                // however, we do not know the strike at which this conversion takes 
                // place, so estimate that through another root finding procedure
                Real k_atm = BlackDeltaCalculator(Option::Type::Put, deltaType(), spot()->value(),
                                                  ddom_, dfor_, v * sqrt(exerciseTime())).atmStrike(atmType());
                Real pdx = delta - (deltaType() == DeltaVolQuote::PaSpot
                                    ? dfor_ * k_atm / fwd_
                                    : k_atm / fwd_);

                auto deltaError = [&](Real d) {
                    Volatility v = volByDelta(d, Option::Type::Put);
                    Real k = putStrikeFromDelta(d, v * sqrt(exerciseTime()));
                    if (deltaType() == DeltaVolQuote::PaSpot) {
                        return delta - d - dfor_ * k / fwd_;

                    } else {
                        return delta - d - k / fwd_;
                    }
                };

                // The put delta is negative but unbounded below for
                // premium-adjusted conventions, so let Brent expand the
                // bracket from the guess, capped just below zero.
                Brent solver;
                solver.setMaxEvaluations(1000);
                solver.setUpperBound(-QL_EPSILON);
                delta = solver.solve(deltaError, 1e-12, pdx, 0.1 * std::fabs(pdx));
            }
        }

        // got vol as a function of delta
        return _volByDelta(delta, fwd_, exerciseTime(), params_);
    }

    Rate fxSmileSectionByDelta::strikeByDelta(Real delta, Option::Type parity) const 
    {
        calculate();

        Volatility v = volByDelta(delta, parity);
        if (parity == Option::Put)
            return putStrikeFromDelta(delta, v * sqrt(exerciseTime()));
        BlackDeltaCalculator bdc(parity, deltaType(), spot()->value(), ddom_, dfor_,
                                 v * sqrt(exerciseTime()));
        return bdc.strikeFromDelta(delta);
    }

    Rate fxSmileSectionByDelta::putStrikeFromDelta(Real putDelta, Real stdDev) const
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

    Real fxSmileSectionByDelta::deltaByStrike(Rate strike, Option::Type parity) const 
    {
        calculate();

        // Since the slice is parameterized by put deltas, ignore parity and get
        // the put delta at the specified strike! This requires a root finding
        // procedure as we know the strike but not the vol!
        Rate d0 = BlackDeltaCalculator(Option::Type::Put, deltaType(), spot()->value(), ddom_,
                                       dfor_, atm()->value() * sqrt(exerciseTime()))
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

    void fxSmileSectionByDelta::calibrate() const
    {
        QL_REQUIRE(!quotes_.empty(), "no delta-vol quotes to calibrate against");

        const Real fwd = fwd_;
        const Time tau = exerciseTime();

        // Precompute target vols and deltas from delta-vol quotes.
        // For a delta-parameterized smile the natural coordinates are
        // put deltas, so convert call deltas to the put-delta equivalent.
        std::vector<Real> targetVols(quotes_.size());
        std::vector<Real> deltas(quotes_.size());

        // Each quote carries its own vol, so its strike is known exactly:
        // invert (delta, vol) -> strike, or use the ATM convention, and then
        // take the put delta at that strike. This is exact for all four delta
        // conventions; for premium-adjusted deltas the call/put relation
        // depends on the strike and is not a constant shift.
        const Real stdDevScale = std::sqrt(tau);
        for (Size i = 0; i < quotes_.size(); ++i)
        {
            targetVols[i] = quotes_[i]->value();
            const Real stdDev = targetVols[i] * stdDevScale;

            Real strike;
            if (quotes_[i]->atmType() != DeltaVolQuote::AtmNull) {
                strike = BlackDeltaCalculator(Option::Call, deltaType(), spot()->value(), ddom_,
                                              dfor_, stdDev)
                             .atmStrike(quotes_[i]->atmType());
            } else {
                const Real d = quotes_[i]->delta();
                const Option::Type type = d > 0.0 ? Option::Call : Option::Put;
                strike = BlackDeltaCalculator(type, deltaType(), spot()->value(), ddom_, dfor_,
                                              stdDev)
                             .strikeFromDelta(d);
            }

            deltas[i] = BlackDeltaCalculator(Option::Put, deltaType(), spot()->value(), ddom_,
                                             dfor_, stdDev)
                            .deltaFromStrike(strike);
        }

        // Cost function: residual = model_vol(delta_i) - target_vol_i
        auto costValues = [&](const Array& x) -> Array {
            std::vector<Real> p(x.begin(), x.end());
            Array residuals(quotes_.size());
            for (Size i = 0; i < quotes_.size(); ++i) 
            {
                residuals[i] = _volByDelta(deltas[i], fwd, tau, p) - targetVols[i];
            }
            return residuals;
        };

        SimpleCostFunction<decltype(costValues)> costFunction(costValues);
        NoConstraint constraint;
        Array guess = initialParams();

        Problem problem(costFunction, constraint, guess);
        LevenbergMarquardt lm;
        EndCriteria endCriteria(1000, 100, 1.0e-12, 1.0e-12, 1.0e-12);
        lm.minimize(problem, endCriteria);

        const Array& solution = problem.currentValue();
        params_.assign(solution.begin(), solution.end());
    }


    //! \name Quadratic smile section (delta-parameterized)
    //@{
    quadraticSmileSection::quadraticSmileSection(const Date& exerciseDate,
                                                 const Handle<Quote>& spot,
                                                 const ext::shared_ptr<FxSmileQuotes>& quotes,
                                                 const Handle<YieldTermStructure>& foreignDiscount,
                                                 const Handle<YieldTermStructure>& domesticDiscount,
                                                 DeltaVolQuote::DeltaType deltaType,
                                                 DeltaVolQuote::AtmType atmType,
                                                 const DayCounter& dayCounter,
                                                 const FxSettlementConvention& settlement,
                                                 const Date& referenceDate)
    : fxSmileSectionByDelta(exerciseDate, spot, quotes,
                            foreignDiscount, domesticDiscount,
                            deltaType, atmType, dayCounter, settlement, referenceDate)
    {
        params_.reserve(3);
    }

    quadraticSmileSection::quadraticSmileSection(Time exerciseTime,
                                                 const Handle<Quote>& spot,
                                                 const ext::shared_ptr<FxSmileQuotes>& quotes,
                                                 const Handle<YieldTermStructure>& foreignDiscount,
                                                 const Handle<YieldTermStructure>& domesticDiscount,
                                                 DeltaVolQuote::DeltaType deltaType,
                                                 DeltaVolQuote::AtmType atmType,
                                                 const DayCounter& dayCounter)
    : fxSmileSectionByDelta(exerciseTime, spot, quotes,
                            foreignDiscount, domesticDiscount,
                            deltaType, atmType, dayCounter)
    {
        params_.reserve(3);
    }

    Array quadraticSmileSection::initialParams() const
    {
        // vol = a*delta^2 + b*delta + c
        // ATM put delta is conventionally -0.5, so at ATM:
        //   atm_vol = a*0.25 + b*(-0.5) + c
        // Start with a=0, b=0, c=atm_vol
        Array guess(3);
        guess[0] = 0.0;
        guess[1] = 0.0;
        guess[2] = atm_->value();
        return guess;
    }

    Volatility quadraticSmileSection::_volByDelta(Real delta,
                                                   Real fwd,
                                                   Time tau,
                                                   const std::vector<Real>& params) const
    {
        return params[0] * delta * delta + params[1] * delta + params[2];
    }
    //@}

}