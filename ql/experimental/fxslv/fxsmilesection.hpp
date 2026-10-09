#ifndef quantlib_axl_fx_smile_section_hpp
#define quantlib_axl_fx_smile_section_hpp

#include <ql/experimental/fxslv/fxsettlementconvention.hpp>
#include <ql/experimental/fxslv/fxsmilequotes.hpp>
#include <ql/pricingengines/blackdeltacalculator.hpp>
#include <ql/quotes/deltavolquote.hpp>
#include <ql/patterns/lazyobject.hpp>
#include <ql/pricingengines/blackcalculator.hpp>
#include <ql/quote.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/termstructures/volatility/smilesection.hpp>
#include <ql/option.hpp>
#include <ql/math/solvers1d/brent.hpp>
#include <ql/shared_ptr.hpp>
#include <ql/utilities/null.hpp>
#include <optional>


namespace QuantLib {

    //! Base class for FX smile sections calibrated to delta-quoted vols
    /*! A section can be built in one of two modes.

        <b>Date mode</b> (constructors taking an expiry date and an
        FxSettlementConvention).  The reference date \f$ t_0 \f$ is the
        trade date: by default the evaluation date, in which case the
        section floats with it; if a reference date is passed, it is
        fixed.  The exercise date is the expiry date \f$ T_e \f$.  Two
        clocks are used:
        - vol time \f$ \tau = \mathrm{yf}(t_0, T_e) \f$, measured with
          the given day counter, which is exerciseTime() and is used for
          the Black standard deviation \f$ \sigma\sqrt{\tau} \f$;
        - the rate period from the spot date \f$ s_0 \f$ to the delivery
          date \f$ T_d \f$, both given by the settlement convention.  The
          discount factors \f$ P(s_0, T_d) = P(t, T_d) / P(t, s_0) \f$
          used in the forward, in the delta conventions and in premium
          adjustment are taken from each curve by date, so they do not
          depend on the curves' reference dates or day counters.
        The forward is \f$ F = S \, P_f(s_0, T_d) / P_d(s_0, T_d) \f$.

        <b>Time mode</b> (constructors taking an expiry time).  An
        idealised setup for model work, tests and interpolation at
        arbitrary times: the time to expiry is fixed (the section does
        not float), there is no settlement lag, and the same time is
        used as vol time and as rate time, i.e.
        \f$ F = S \, P_f(\tau) / P_d(\tau) \f$ on the curves' time
        axis.  For this to be meaningful both curves must share a
        reference date and a day counter; this is checked on
        calculation.  The section's own day counter is not used in this
        mode, since no date has to be converted to a time.

        The market quotes are given as an FxSmileQuotes object: either
        FxRrBfQuotes (ATM, risk reversals and smile or broker strangles
        per delta) or FxDeltaVolQuotes (generic delta-vol quotes).  The
        section fits its smile to the delta-vol quotes that object
        produces and then derives its ATM vol from the fitted smile, so
        that atm() always lies on the calibrated curve.
    */
    class FxSmileSection : public SmileSection, public LazyObject {
      public:
        //! Date mode.
        FxSmileSection(const Date& exerciseDate,
                       const Handle<Quote>& spot,
                       const ext::shared_ptr<FxSmileQuotes>& quotes,
                       const Handle<YieldTermStructure>& foreignDiscount,
                       const Handle<YieldTermStructure>& domesticDiscount,
                       DeltaVolQuote::DeltaType deltaType,
                       DeltaVolQuote::AtmType atmType,
                       const DayCounter& dayCounter,
                       const FxSettlementConvention& settlement,
                       const Date& referenceDate = Date());

        //! Time mode.
        FxSmileSection(Time exerciseTime,
                       const Handle<Quote>& spot,
                       const ext::shared_ptr<FxSmileQuotes>& quotes,
                       const Handle<YieldTermStructure>& foreignDiscount,
                       const Handle<YieldTermStructure>& domesticDiscount,
                       DeltaVolQuote::DeltaType deltaType,
                       DeltaVolQuote::AtmType atmType,
                       const DayCounter& dayCounter = DayCounter());

        //! \name Observer interface
        //@{
        void update() override;
        //@}

        //! \name SmileSection interface
        //@{
        virtual Real minStrike() const { calculate(); return minStrike_; };
        virtual Real maxStrike() const { calculate(); return maxStrike_; };
        virtual Real atmLevel() const { calculate(); return atmStrike_; };
        //@}

        // Conventions
        DeltaVolQuote::DeltaType deltaType() const { return deltaType_; };
        DeltaVolQuote::AtmType atmType() const { return atmType_; };
        bool premiumAdjust() const {
            return (deltaType_ == DeltaVolQuote::PaSpot || deltaType_ == DeltaVolQuote::PaFwd);
        };

        // Introspection
        Handle<Quote> spot() const { return spot_; };
        Handle<Quote> atm() const { calculate(); return atm_; };
        Real forward() const {calculate(); return fwd_; };
        Handle<YieldTermStructure> foreignDiscount() const { return foreignDiscount_; };
        Handle<YieldTermStructure> domesticDiscount() const { return domesticDiscount_; };

        //! Settlement convention; empty for sections built in time mode.
        const std::optional<FxSettlementConvention>& settleConvention() const { return settleConvention_; }

        Date spotDate() const;      //!< Spot date of the reference date (date mode only).
        Date deliveryDate() const;  //!< Delivery date of the expiry (date mode only).
        
        //! Domestic discount factor from spot to delivery (time mode: to expiry time).
        DiscountFactor domesticDiscountFactor() const { calculate(); return ddom_; }
        
        //! Foreign discount factor from spot to delivery (time mode: to expiry time).
        DiscountFactor foreignDiscountFactor() const { calculate(); return dfor_; }

        //! Market quotes the section is calibrated to.
        const ext::shared_ptr<FxSmileQuotes>& smileQuotes() const { return smileQuotes_; }

        // Calibration
        virtual Volatility volByStrike(Rate strike) const = 0;
        virtual Volatility volByDelta(Real delta, Option::Type parity) const = 0;
        virtual Real deltaByStrike(Rate strike, Option::Type parity) const = 0;
        virtual Rate strikeByDelta(Real delta, Option::Type parity) const = 0;

        // Interpolation
        //! Derivative of the vol with respect to strike.
        /*! Central finite differences by default; models with a
            closed form should override it.
        */
        virtual Real volDerivative(Rate strike) const;

        //! \name Interpolation in probability space
        /*! Functions of the moneyness \f$ k = K/F \f$, used to
            interpolate smiles across expiries.  The inverse requires a
            smile without butterfly arbitrage, which it checks.
        */
        //@{
        //! Undiscounted call price in units of the forward, \f$ c(k) = E[(S_T/F - k)^+] \f$.
        Real normedCallPrice(Real moneyness) const;
        //! Probability of exercise, \f$ P(S_T > kF) = -c'(k) \f$, computed analytically.
        Probability exerciseProbability(Real moneyness) const;
        //! Moneyness at which the exercise probability equals \f$ p \f$.
        Real moneynessFromProbability(Probability p) const;
        //@}

      private:
        //! \name LazyObject interface
        //@{
        void performCalculations() const override;
        //@}

        void registerWithMarketData();
        void calculateForward() const;
        void calculateAtm() const;
        void stripDeltaVolQuotes() const;

        // FxSmileQuotes::fit() is the only caller of fitTo().
        friend class FxSmileQuotes;
        //! Fits the smile to the given quotes; only valid while the quotes calibrate the section.
        void fitTo(std::vector<Handle<DeltaVolQuote>> quotes) const;
        mutable bool calibrating_ = false;  // the quotes are calibrating this section
        mutable bool fitted_ = false;       // fitTo() ran during the current calibration
        virtual void adjustStrikes() const;
        virtual void calibrate() const = 0;
        
        virtual Volatility volatilityImpl(Rate strike) const { return volByStrike(strike); };

        DeltaVolQuote::DeltaType deltaType_;
        DeltaVolQuote::AtmType atmType_;

        Handle<Quote> spot_;
        ext::shared_ptr<FxSmileQuotes> smileQuotes_;
        Handle<YieldTermStructure> foreignDiscount_;
        Handle<YieldTermStructure> domesticDiscount_;
        std::optional<FxSettlementConvention> settleConvention_;
        mutable Date spotDate_, deliveryDate_;

      protected:
        mutable Real ddom_ = Null<Real>();
        mutable Real dfor_ = Null<Real>();
        mutable Real fwd_ = Null<Real>();

        mutable Real atmStrike_ = Null<Real>();
        mutable Real maxStrike_;
        mutable Real minStrike_;

        // Computed state: rebuilt on every calibration in stripDeltaVolQuotes().
        // atm_ is seeded with the quotes' reference vol before calibrating
        // and set to the fitted smile's ATM vol by calculateAtm() after.
        // quotes_ is always a workspace populated before each call to calibrate().
        mutable Handle<Quote> atm_;
        mutable std::vector<Handle<DeltaVolQuote>> quotes_;

    };

    inline void FxSmileSection::update() {
        SmileSection::update();
        LazyObject::update();
    }

}  // namespace QuantLib

#endif