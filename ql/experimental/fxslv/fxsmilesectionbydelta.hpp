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

/*! \file fxsmilesectionbydelta.hpp
    \brief FX smile sections parameterised in delta
*/

#ifndef quantlib_fx_smile_section_delta_hpp
#define quantlib_fx_smile_section_delta_hpp

#include <ql/math/array.hpp>
#include <ql/experimental/fxslv/fxsmilesection.hpp>

namespace QuantLib {

    class FxSmileSectionByDelta : public FxSmileSection {
      public:
        //! Date mode (see FxSmileSection).
        FxSmileSectionByDelta(const Date& exerciseDate,
                              const Handle<Quote>& spot,
                              const ext::shared_ptr<FxSmileQuotes>& quotes,
                              const Handle<YieldTermStructure>& foreignDiscount,
                              const Handle<YieldTermStructure>& domesticDiscount,
                              DeltaVolQuote::DeltaType deltaType,
                              DeltaVolQuote::AtmType atmType,
                              const DayCounter& dayCounter,
                              const FxSettlementConvention& settlement,
                              const Date& referenceDate = Date());

        //! Time mode (see FxSmileSection).
        FxSmileSectionByDelta(Time exerciseTime,
                              const Handle<Quote>& spot,
                              const ext::shared_ptr<FxSmileQuotes>& quotes,
                              const Handle<YieldTermStructure>& foreignDiscount,
                              const Handle<YieldTermStructure>& domesticDiscount,
                              DeltaVolQuote::DeltaType deltaType,
                              DeltaVolQuote::AtmType atmType,
                             const DayCounter& dayCounter = DayCounter());

        //! \name fxSmileSection interface
        //@{
        Volatility volByStrike(Rate strike) const;
        Volatility volByDelta(Real delta, Option::Type parity) const;
        Real deltaByStrike(Rate strike, Option::Type parity) const;
        Rate strikeByDelta(Real delta, Option::Type parity) const;
        //! Residual at the point's put delta, the natural coordinate here.
        Real volResidual(Rate strike, Volatility vol) const override;
        //@}

      private:
        virtual Volatility _volByDelta(Real delta,
                                       Real fwd,
                                       Time tau,
                                       const std::vector<Real>& params) const = 0;


      protected:
        //! \name FxSmileSection interface
        //@{
        void setParams(const Array& params) const override {
            params_.assign(params.begin(), params.end());
        }
        //@}

        /*! Strike of the put with the given put delta (in this section's
            delta convention) and standard deviation.  Unlike
            BlackDeltaCalculator::strikeFromDelta, this also handles
            premium-adjusted put deltas below -dfor (resp. -1), which
            correspond to in-the-money puts and are reached when converting
            low call deltas to put deltas.
        */
        Rate putStrikeFromDelta(Real putDelta, Real stdDev) const;

        mutable std::vector<Real> params_;
    };

    //typedef ext::shared_ptr<fxSmileSectionByDelta> fxSmileSectionByDeltaPtr;


    //! Quadratic smile section parameterized by put delta.
    /*! Implied volatility is a quadratic function of put delta:
        \f$ \sigma(\Delta) = a \Delta^2 + b \Delta + c \f$
    */
    class QuadraticSmileSection : public FxSmileSectionByDelta {
      public:
        //! Date mode (see FxSmileSection).
        QuadraticSmileSection(const Date& exerciseDate,
                              const Handle<Quote>& spot,
                              const ext::shared_ptr<FxSmileQuotes>& quotes,
                              const Handle<YieldTermStructure>& foreignDiscount,
                              const Handle<YieldTermStructure>& domesticDiscount,
                              DeltaVolQuote::DeltaType deltaType,
                              DeltaVolQuote::AtmType atmType,
                              const DayCounter& dayCounter,
                              const FxSettlementConvention& settlement,
                              const Date& referenceDate = Date());

        //! Time mode (see FxSmileSection).
        QuadraticSmileSection(Time exerciseTime,
                              const Handle<Quote>& spot,
                              const ext::shared_ptr<FxSmileQuotes>& quotes,
                              const Handle<YieldTermStructure>& foreignDiscount,
                              const Handle<YieldTermStructure>& domesticDiscount,
                              DeltaVolQuote::DeltaType deltaType,
                              DeltaVolQuote::AtmType atmType,
                              const DayCounter& dayCounter = DayCounter());

        // Introspection
        Real a() const { return params_[0]; };
        Real b() const { return params_[1]; };
        Real c() const { return params_[2]; };

      private:
        //! \name fxSmileSectionByDelta interface
        //@{
        Volatility _volByDelta(Real delta,
                               Real fwd,
                               Time tau,
                               const std::vector<Real>& params) const override;
        //@}

      protected:
        Array initialParams() const override;
    };

} // namespace QuantLib

#endif
