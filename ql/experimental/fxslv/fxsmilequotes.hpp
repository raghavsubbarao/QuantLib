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

/*! \file fxsmilequotes.hpp
    \brief Market quotes for FX smile section calibration
*/

#ifndef quantlib_fx_smile_quotes_hpp
#define quantlib_fx_smile_quotes_hpp

#include <ql/handle.hpp>
#include <ql/patterns/observable.hpp>
#include <ql/quote.hpp>
#include <ql/quotes/deltavolquote.hpp>
#include <functional>
#include <vector>

namespace QuantLib {

    class FxSmileSection;

    //! Market quotes for FX smile section calibration
    /*! Separates how the market quotes a smile from how a section
        parameterises it.  A section only knows how to fit its smile 
        to a set of delta-vol quotes; an FxSmileQuotes object can store
        market quotes as RRs/BFs or a set of DeltaVolQuotes. 
        Market conventions on Broker and Market strangles are also 
        handled here providing a uniform interface to FxSmileSection.

        Instances observe their quotes and forward notifications, so the
        smile section only needs to register with this object.
    */
    class FxSmileQuotes : public Observer, public Observable {
      public:
        //! Fits the section's smile to the given delta-vol quotes.
        typedef std::function<void(std::vector<Handle<DeltaVolQuote>>)> Fitter;

        ~FxSmileQuotes() override = default;

        //! \name Observer interface
        //@{
        void update() override { notifyObservers(); }
        //@}

        //! Atm vol used to seed the calibration.
        /*! It depends only on the quotes, never on a previous calibration, 
            so that the same quotes always give the same smile.
        */
        virtual Volatility referenceVol() const = 0;

        //! Calibrates the section to the quotes.
        /*! Calls \c fit one or more times; on return the section's
            smile must be fitted to the final set of delta-vol quotes.
            The section's forward, discount factors and conventions are
            available while this runs.
        */
        virtual void calibrate(const FxSmileSection& section, const Fitter& fit) const = 0;
    };


    //! Market quote of ATM vol and a risk reversal and butterfly per delta
    /*! For each delta \f$ \Delta \f$ 
        the risk reversal is \f$ RR = \sigma_{call} - \sigma_{put} \f$  
        The butterfly is either a 
            smile strangle: \f$ BF = (\sigma_{call} + \sigma_{put})/2 - \sigma_{ATM} \f$
            or broker (market) strangle: 
                  the vol \f$ \sigma_{ATM} + BF \f$ that prices 
                  the \f$ \Delta \f$ strangle, struck at that vol, 
                  to the same premium as the smile does
    */
    class FxRrBfQuotes : public FxSmileQuotes {
      public:
        enum FlyType {
            SmileStrangle, //!< butterflies are smile strangles
            MarketStrangle //!< butterflies are broker (market) strangles
        };

        FxRrBfQuotes(Handle<Quote> atm,
                     std::vector<Handle<Quote>> riskReversals,
                     std::vector<Handle<Quote>> butterflies,
                     std::vector<Real> deltas,
                     FlyType flyType);

        //! \name FxSmileQuotes interface
        //@{
        Volatility referenceVol() const override { return atm_->value(); }
        void calibrate(const FxSmileSection& section, const Fitter& fit) const override;
        //@}

        //! \name Inspectors
        //@{
        const Handle<Quote>& atm() const { return atm_; }
        const std::vector<Handle<Quote>>& riskReversals() const { return riskReversals_; }
        const std::vector<Handle<Quote>>& butterflies() const { return butterflies_; }
        const std::vector<Real>& deltas() const { return deltas_; }
        FlyType flyType() const { return flyType_; }
        //@}

      private:
        //! Delta-vol quotes implied by the ATM, the risk reversals and the given smile strangles.
        std::vector<Handle<DeltaVolQuote>> deltaVolQuotes(const FxSmileSection& section, 
                                                          const std::vector<Real>& smileStrangles) const;
        void calibrateToMarketStrangles(const FxSmileSection& section, const Fitter& fit) const;

        Handle<Quote> atm_;
        std::vector<Handle<Quote>> riskReversals_;
        std::vector<Handle<Quote>> butterflies_;
        std::vector<Real> deltas_;
        FlyType flyType_;
    };


    //! Generic set of delta-vol quotes
    /*! The quotes are fitted as given; the ATM quote, if any, is fitted
        like the others.
    */
    class FxDeltaVolQuotes : public FxSmileQuotes {
      public:
        explicit FxDeltaVolQuotes(std::vector<Handle<DeltaVolQuote>> quotes);

        //! \name FxSmileQuotes interface
        //@{
        //! Average of the quoted vols.
        Volatility referenceVol() const override;
        void calibrate(const FxSmileSection& section, const Fitter& fit) const override;
        //@}

        const std::vector<Handle<DeltaVolQuote>>& quotes() const { return quotes_; }

      private:
        std::vector<Handle<DeltaVolQuote>> quotes_;
    };

}

#endif
