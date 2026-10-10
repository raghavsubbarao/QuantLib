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

/*! \file fxdeltaconvention.hpp
    \brief Strike and delta conversions in an FX delta convention
*/

#ifndef quantlib_fx_delta_convention_hpp
#define quantlib_fx_delta_convention_hpp

#include <ql/option.hpp>
#include <ql/pricingengines/blackdeltacalculator.hpp>
#include <ql/quotes/deltavolquote.hpp>
#include <utility>

namespace QuantLib {

    //! Strike and delta conversions in an FX delta convention
    /*! Holds the market state the conversions need (spot and the
        discount factors from spot to delivery) and the delta type, and
        converts at a given total volatility \f$ \sigma\sqrt{\tau} \f$.
        It knows nothing of a smile: conversions along a smile, where the
        vol depends on the strike, belong to FxSmileSection.

        Two identities hold whatever the vol, with \f$ D_f \f$ the
        foreign discount factor:
        - put-call parity in delta: call delta minus put delta at a
          strike is \f$ D_f \f$ (spot), 1 (forward), \f$ D_f K/F \f$
          (premium-adjusted spot) or \f$ K/F \f$ (premium-adjusted
          forward);
        - the put delta at a strike lies between minus that and 0.

        Premium-adjusted call deltas are not monotonic in strike: they
        peak, so below the peak strike every call delta is attained
        twice and higher deltas not at all.  Strikes are those above the
        peak, as quoted in the market.
    */
    class FxDeltaConvention {
      public:
        FxDeltaConvention(DeltaVolQuote::DeltaType type,
                          Real spot,
                          DiscountFactor domesticDiscount,
                          DiscountFactor foreignDiscount);

        //! \name Inspectors
        //@{
        DeltaVolQuote::DeltaType type() const { return type_; }
        bool premiumAdjusted() const {
            return type_ == DeltaVolQuote::PaSpot || type_ == DeltaVolQuote::PaFwd;
        }
        Real spot() const { return spot_; }
        Real forward() const { return forward_; }
        DiscountFactor domesticDiscount() const { return domesticDiscount_; }
        DiscountFactor foreignDiscount() const { return foreignDiscount_; }
        //@}

        //! The same market in another delta convention.
        FxDeltaConvention withType(DeltaVolQuote::DeltaType type) const {
            return FxDeltaConvention(type, spot_, domesticDiscount_, foreignDiscount_);
        }

        //! \name Conversions at a given total vol
        //@{
        //! Delta of the option at the given strike.
        Real delta(Option::Type type, Rate strike, Real stdDev) const;
        //! Strike of the option with the given delta.
        /*! For premium-adjusted calls, the strike above the peak; fails
            with a clear message if the delta is above the peak delta at
            this vol.  Premium-adjusted put deltas below \f$ -D_f \f$
            (resp. -1), i.e. in-the-money puts, are handled too.
        */
        Rate strike(Option::Type type, Real delta, Real stdDev) const;
        //! ATM strike under the given convention.
        Rate atmStrike(DeltaVolQuote::AtmType type, Real stdDev) const;
        //! Strike at which the premium-adjusted call delta peaks at this vol.
        Rate peakCallStrike(Real stdDev) const;
        //@}

        //! \name Identities at any vol
        //@{
        //! Call delta minus put delta at the given strike.
        Real parity(Rate strike) const;
        //! Range of the put delta at the given strike, whatever the vol.
        std::pair<Real, Real> putDeltaRange(Rate strike) const {
            return {-parity(strike), 0.0};
        }
        //! Supremum of the call delta, attained at zero strike, for unadjusted types.
        /*! For premium-adjusted types it depends on the vol at each
            strike; see FxSmileSection::maxCallDelta().
        */
        Real callDeltaLimit() const;
        //@}

      private:
        BlackDeltaCalculator calculator(Option::Type type, Real stdDev) const;

        DeltaVolQuote::DeltaType type_;
        Real spot_, forward_;
        DiscountFactor domesticDiscount_, foreignDiscount_;
    };

}

#endif
