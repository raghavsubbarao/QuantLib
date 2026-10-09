#ifndef quantlib_axl_fx_smile_section_delta_hpp
#define quantlib_axl_fx_smile_section_delta_hpp

#include <ql/math/array.hpp>
#include <ql/experimental/fxslv/fxsmilesection.hpp>

namespace QuantLib {

    class fxSmileSectionByDelta : public FxSmileSection {
      public:
        //! Date mode (see FxSmileSection).
        fxSmileSectionByDelta(const Date& exerciseDate,
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
        fxSmileSectionByDelta(Time exerciseTime,
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
        //@}

      private:
        virtual Volatility _volByDelta(Real delta,
                                       Real fwd,
                                       Time tau,
                                       const std::vector<Real>& params) const = 0;

        //! \name fxSmileSection interface
        //@{
        virtual void calibrate() const;
        //@}

      protected:
        //! Initial parameter guess for calibration.
        virtual Array initialParams() const = 0;

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

    typedef ext::shared_ptr<fxSmileSectionByDelta> fxSmileSectionByDeltaPtr;


    //! Quadratic smile section parameterized by put delta.
    /*! Implied volatility is a quadratic function of put delta:
        \f$ \sigma(\Delta) = a \Delta^2 + b \Delta + c \f$
    */
    class quadraticSmileSection : public fxSmileSectionByDelta {
      public:
        //! Date mode (see FxSmileSection).
        quadraticSmileSection(const Date& exerciseDate,
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
        quadraticSmileSection(Time exerciseTime,
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