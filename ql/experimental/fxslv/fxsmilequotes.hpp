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
#include <ql/shared_ptr.hpp>
#include <optional>
#include <utility>
#include <vector>

namespace QuantLib {

    class FxSmileSection;

    //! A condition the calibrated smile should meet
    /*! Each target gives a residual, in vol units, for the section's
        current smile.  During calibration the section holds trial
        parameters, so residual() measures that trial smile.
    */
    class FxSmileTarget {
      public:
        virtual ~FxSmileTarget() = default;

        //! Residual of the section's current smile, in vol units.
        virtual Real residual(const FxSmileSection& section) const = 0;

        //! The point (strike, vol) on the section's smile this target fixes, if any
        /*! For models with closed-form fits to points (e.g. the cost
            models).  The strike depends on the section's forward and
            conventions, so it is worked out for the given section.
        */
        virtual std::optional<std::pair<Rate, Volatility>> point(const FxSmileSection&) const {
            return std::nullopt;
        }
    };

    //! The smile matches a delta-vol quote
    /*! The quote's strike follows from its own delta and vol, or from
        the ATM convention, and the section's forward and conventions; it
        is worked out when the target is evaluated, so a target is not
        tied to one section.  The residual is measured in the section's
        natural coordinate, see FxSmileSection::volResidual().
    */
    class FxDeltaVolTarget : public FxSmileTarget {
      public:
        explicit FxDeltaVolTarget(Handle<DeltaVolQuote> quote) : quote_(std::move(quote)) {}
        Real residual(const FxSmileSection& section) const override;
        std::optional<std::pair<Rate, Volatility>> point(const FxSmileSection& section) const override;

      private:
        Handle<DeltaVolQuote> quote_;
    };

    //! Risk reversal at the smile's own deltas
    /*! \f$ \sigma(\Delta_{call}) - \sigma(-\Delta_{put}) = RR \f$, with
        the strikes for \f$ \pm\Delta \f$ found on the calibrated smile,
        so they move with the smile.
    */
    class FxRiskReversalTarget : public FxSmileTarget {
      public:
        FxRiskReversalTarget(Real delta, Volatility riskReversal)
        : delta_(delta), riskReversal_(riskReversal) {}
        Real residual(const FxSmileSection& section) const override;

      private:
        Real delta_;
        Volatility riskReversal_;
    };

    //! Broker (market) strangle premium
    /*! Both legs are struck, and the market premium priced, at the
        broker vol \f$ \sigma_{ATM} + BF \f$; the residual is the premium
        of the same strangle on the smile minus the market premium,
        divided by the market strangle's vega so that it reads as a vol.
    */
    class FxBrokerStrangleTarget : public FxSmileTarget {
      public:
        FxBrokerStrangleTarget(Volatility atmVol, Volatility brokerFly, Real delta)
        : atmVol_(atmVol), brokerFly_(brokerFly), delta_(delta) {}
        Real residual(const FxSmileSection& section) const override;

      private:
        Volatility atmVol_, brokerFly_;
        Real delta_;
    };

    typedef std::vector<ext::shared_ptr<FxSmileTarget>> FxSmileTargets;


    //! Market quotes for FX smile section calibration
    /*! Separates how the market quotes a smile from how a section
        parameterises it.  An FxSmileQuotes object can store market
        quotes as RRs/BFs or a set of DeltaVolQuotes, and turns them into
        calibration targets; market conventions on smile and broker
        strangles are handled here, providing a uniform interface to
        FxSmileSection, which fits its smile to the targets in one
        least-squares problem.

        Instances observe their quotes and forward notifications, so the
        smile section only needs to register with this object.
    */
    class FxSmileQuotes : public Observer, public Observable {
      public:
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

      protected:
        //! Fits the section's smile to the given targets.
        /*! For use inside calibrate() only; the section rejects fits
            requested at any other time.
        */
        static void fit(const FxSmileSection& section, FxSmileTargets targets);

      private:
        // Only the section can start its own calibration.
        friend class FxSmileSection;

        //! Calibrates the section to the quotes.
        /*! Builds the targets and calls fit(); on return the section's
            smile must be fitted, which the section checks.  The section's
            forward, discount factors and conventions are available while
            this runs.
        */
        virtual void calibrate(const FxSmileSection& section) const = 0;
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

        With smile strangles every quote is a point on the smile.  With
        broker strangles the smile is fitted jointly to the ATM vol, the
        risk reversals at the smile's own deltas and the broker strangle
        premiums.
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
        void calibrate(const FxSmileSection& section) const override;

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
        //@}

        const std::vector<Handle<DeltaVolQuote>>& quotes() const { return quotes_; }

      private:
        void calibrate(const FxSmileSection& section) const override;

        std::vector<Handle<DeltaVolQuote>> quotes_;
    };

}

#endif
