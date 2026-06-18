/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2025 Raghav Subbarao

 This file is part of QuantLib, a free-software/open-source library
 for financial quantitative analysts and developers - http://quantlib.org/

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <http://quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.
*/

/*! \file fxcostsmilesection.hpp
    \brief FX cost-model smile sections (flat, scaled, and general-alpha dynamics).
*/

#ifndef quantlib_axl_cost_fx_smile_section_hpp
#define quantlib_axl_cost_fx_smile_section_hpp

#include <ql/math/array.hpp>
#include <ql/math/polynomialmathfunction.hpp>
#include <ql/math/quadratic.hpp>
#include <ql/termstructures/volatility/fxsmilesectionbystrike.hpp>

namespace QuantLib {

    class cubic : public PolynomialFunction {
      public:
          cubic(const std::vector<Real>& coeff): PolynomialFunction(coeff) {
            QL_REQUIRE(coeff.size() == 4,
                       "cubic requires four coefficients to initialize");

        }

        Integer roots(std::vector<Real>& zeros);
    };

    class quartic : public PolynomialFunction {
      public:
        quartic(const std::vector<Real>& coeff) : PolynomialFunction(coeff) {
            QL_REQUIRE(coeff.size() == 5, "quartic requires five coefficients to initialize");
        }

        Integer roots(std::vector<Real>& zeros);
    };

    /*! Base class for FX cost-model smile sections.

        Calibrates the four cost parameters [Cgamma, Ctheta, Ceta, Comega] via
        weighted SVD regression on the quoted market smile, for a general power-law
        dynamics exponent alpha in [0,1]:

            d(rho) = c * rho^alpha

        The default _volByStrike() solves the cost equation by Newton-Raphson.
        Derived classes may override _volByStrike() with closed-form solvers when
        alpha takes a special value (0 → quartic, 1 → biquadratic/quadratic).
    */
    class fxCostSmileSection : public fxSmileSectionByStrike {
      public:
        // ctor from market quotes for specific date
        fxCostSmileSection(const Date& exerciseDate,
                           const Handle<Quote>& spot,
                           const Handle<Quote>& atm,
                           const std::vector<Handle<Quote>>& rrs,
                           const std::vector<Handle<Quote>>& bfs,
                           const std::vector<Real>& deltas,
                           const Handle<YieldTermStructure>& foreignDiscount,
                           const Handle<YieldTermStructure>& domesticDiscount,
                           DeltaVolQuote::DeltaType deltaType,
                           DeltaVolQuote::AtmType atmType,
                           fxSmileSection::FlyType flyType,
                           Real alpha = 0.5,
                           const DayCounter& dayCounter = DayCounter(),
                           const Date& referenceDate = Date(),
                           bool weightedCalibrationFlag = true);

        // ctor from market quotes with expiry time - floats with evaluation date
        fxCostSmileSection(Time exerciseTime,
                           const Handle<Quote>& spot,
                           const Handle<Quote>& atm,
                           const std::vector<Handle<Quote>>& rrs,
                           const std::vector<Handle<Quote>>& bfs,
                           const std::vector<Real>& deltas,
                           const Handle<YieldTermStructure>& foreignDiscount,
                           const Handle<YieldTermStructure>& domesticDiscount,
                           DeltaVolQuote::DeltaType deltaType,
                           DeltaVolQuote::AtmType atmType,
                           fxSmileSection::FlyType flyType,
                           Real alpha = 0.5,
                           const DayCounter& dayCounter = DayCounter(),
                           bool weightedCalibrationFlag = true);

        // ctor from derived quotes for specific date
        fxCostSmileSection(const Date& exerciseDate,
                           const Handle<Quote>& spot,
                           const std::vector<Handle<DeltaVolQuote>>& quotes,
                           const Handle<YieldTermStructure>& foreignDiscount,
                           const Handle<YieldTermStructure>& domesticDiscount,
                           DeltaVolQuote::DeltaType deltaType,
                           DeltaVolQuote::AtmType atmType,
                           FlyType flyType,
                           Real alpha = 0.5,
                           const DayCounter& dayCounter = DayCounter(),
                           const Date& referenceDate = Date(),
                           bool weightedCalibrationFlag = true);

        // ctor from derived quotes for expiry time - floats with evaluation date
        fxCostSmileSection(Time exerciseTime,
                           const Handle<Quote>& spot,
                           const std::vector<Handle<DeltaVolQuote>>& quotes,
                           const Handle<YieldTermStructure>& foreignDiscount,
                           const Handle<YieldTermStructure>& domesticDiscount,
                           DeltaVolQuote::DeltaType deltaType,
                           DeltaVolQuote::AtmType atmType,
                           FlyType flyType,
                           Real alpha = 0.5,
                           const DayCounter& dayCounter = DayCounter(),
                           bool weightedCalibrationFlag = true);

        bool weightedCalibration() const { return weightedCalibrationFlag_; }
        Real alpha() const { return alpha_; }

      private:
        //! \name fxSmileSection interface
        //@{
        void calibrate() const override;
        //@}

        //! \name fxSmileSectionByStrike interface
        //@{
        Volatility _volByStrike(Real strike,
                                Real fwd,
                                Time tau,
                                const std::vector<Real>& params) const override;
        //@}

        bool weightedCalibrationFlag_;
        Real alpha_;

      protected:
        Array initialParams() const override;
    };

    /*! FX cost smile section with alpha = 0 (flat / constant-vol dynamics).

        The cost equation reduces to a quartic in total vol rho, which is solved
        analytically (Ferrari's method).  Calibration is inherited from the base
        class using alpha = 0.
    */
    class fxCostSmileSectionFlatDynamics : public fxCostSmileSection {
      public:
        // ctor from market quotes for specific date
        fxCostSmileSectionFlatDynamics(const Date& exerciseDate,
                                       const Handle<Quote>& spot,
                                       const Handle<Quote>& atm,
                                       const std::vector<Handle<Quote>>& rrs,
                                       const std::vector<Handle<Quote>>& bfs,
                                       const std::vector<Real>& deltas,
                                       const Handle<YieldTermStructure>& foreignDiscount,
                                       const Handle<YieldTermStructure>& domesticDiscount,
                                       DeltaVolQuote::DeltaType deltaType,
                                       DeltaVolQuote::AtmType atmType,
                                       fxSmileSection::FlyType flyType,
                                       const DayCounter& dayCounter = DayCounter(),
                                       const Date& referenceDate = Date(),
                                       bool weightedCalibrationFlag = true);

        // ctor from market quotes with expiry time - floats with evaluation date
        fxCostSmileSectionFlatDynamics(Time exerciseTime,
                                       const Handle<Quote>& spot,
                                       const Handle<Quote>& atm,
                                       const std::vector<Handle<Quote>>& rrs,
                                       const std::vector<Handle<Quote>>& bfs,
                                       const std::vector<Real>& deltas,
                                       const Handle<YieldTermStructure>& foreignDiscount,
                                       const Handle<YieldTermStructure>& domesticDiscount,
                                       DeltaVolQuote::DeltaType deltaType,
                                       DeltaVolQuote::AtmType atmType,
                                       fxSmileSection::FlyType flyType,
                                       const DayCounter& dayCounter = DayCounter(),
                                       bool weightedCalibrationFlag = true);

        // ctor from derived quotes for specific date
        fxCostSmileSectionFlatDynamics(const Date& exerciseDate,
                                       const Handle<Quote>& spot,
                                       const std::vector<Handle<DeltaVolQuote>>& quotes,
                                       const Handle<YieldTermStructure>& foreignDiscount,
                                       const Handle<YieldTermStructure>& domesticDiscount,
                                       DeltaVolQuote::DeltaType deltaType,
                                       DeltaVolQuote::AtmType atmType,
                                       FlyType flyType,
                                       const DayCounter& dayCounter = DayCounter(),
                                       const Date& referenceDate = Date(),
                                       bool weightedCalibrationFlag = true);

        // ctor from derived quotes for expiry time - floats with evaluation date
        fxCostSmileSectionFlatDynamics(Time exerciseTime,
                                       const Handle<Quote>& spot,
                                       const std::vector<Handle<DeltaVolQuote>>& quotes,
                                       const Handle<YieldTermStructure>& foreignDiscount,
                                       const Handle<YieldTermStructure>& domesticDiscount,
                                       DeltaVolQuote::DeltaType deltaType,
                                       DeltaVolQuote::AtmType atmType,
                                       FlyType flyType,
                                       const DayCounter& dayCounter = DayCounter(),
                                       bool weightedCalibrationFlag = true);

      private:
        //! \name fxSmileSectionByStrike interface
        //@{
        Volatility _volByStrike(Real strike,
                                Real fwd,
                                Time tau,
                                const std::vector<Real>& params) const override;
        //@}
    };

    /*! FX cost smile section with alpha = 1 (scaled / log-normal total-vol dynamics).

        The cost equation reduces to a biquadratic (quadratic in rho^2), which is solved
        analytically.  Calibration is inherited from the base class using alpha = 1.
    */
    class fxCostSmileSectionScaledDynamics : public fxCostSmileSection {
      public:
        // ctor from market quotes for specific date
        fxCostSmileSectionScaledDynamics(const Date& exerciseDate,
                                         const Handle<Quote>& spot,
                                         const Handle<Quote>& atm,
                                         const std::vector<Handle<Quote>>& rrs,
                                         const std::vector<Handle<Quote>>& bfs,
                                         const std::vector<Real>& deltas,
                                         const Handle<YieldTermStructure>& foreignDiscount,
                                         const Handle<YieldTermStructure>& domesticDiscount,
                                         DeltaVolQuote::DeltaType deltaType,
                                         DeltaVolQuote::AtmType atmType,
                                         fxSmileSection::FlyType flyType,
                                         const DayCounter& dayCounter = DayCounter(),
                                         const Date& referenceDate = Date(),
                                         bool weightedCalibrationFlag = true);

        // ctor from market quotes with expiry time - floats with evaluation date
        fxCostSmileSectionScaledDynamics(Time exerciseTime,
                                         const Handle<Quote>& spot,
                                         const Handle<Quote>& atm,
                                         const std::vector<Handle<Quote>>& rrs,
                                         const std::vector<Handle<Quote>>& bfs,
                                         const std::vector<Real>& deltas,
                                         const Handle<YieldTermStructure>& foreignDiscount,
                                         const Handle<YieldTermStructure>& domesticDiscount,
                                         DeltaVolQuote::DeltaType deltaType,
                                         DeltaVolQuote::AtmType atmType,
                                         fxSmileSection::FlyType flyType,
                                         const DayCounter& dayCounter = DayCounter(),
                                         bool weightedCalibrationFlag = true);

        // ctor from derived quotes for specific date
        fxCostSmileSectionScaledDynamics(const Date& exerciseDate,
                                         const Handle<Quote>& spot,
                                         const std::vector<Handle<DeltaVolQuote>>& quotes,
                                         const Handle<YieldTermStructure>& foreignDiscount,
                                         const Handle<YieldTermStructure>& domesticDiscount,
                                         DeltaVolQuote::DeltaType deltaType,
                                         DeltaVolQuote::AtmType atmType,
                                         FlyType flyType,
                                         const DayCounter& dayCounter = DayCounter(),
                                         const Date& referenceDate = Date(),
                                         bool weightedCalibrationFlag = true);

        // ctor from derived quotes for expiry time - floats with evaluation date
        fxCostSmileSectionScaledDynamics(Time exerciseTime,
                                         const Handle<Quote>& spot,
                                         const std::vector<Handle<DeltaVolQuote>>& quotes,
                                         const Handle<YieldTermStructure>& foreignDiscount,
                                         const Handle<YieldTermStructure>& domesticDiscount,
                                         DeltaVolQuote::DeltaType deltaType,
                                         DeltaVolQuote::AtmType atmType,
                                         FlyType flyType,
                                         const DayCounter& dayCounter = DayCounter(),
                                         bool weightedCalibrationFlag = true);

      private:
        //! \name fxSmileSectionByStrike interface
        //@{
        Volatility _volByStrike(Real strike,
                                Real fwd,
                                Time tau,
                                const std::vector<Real>& params) const override;
        //@}
    };

} // namespace QuantLib

#endif
