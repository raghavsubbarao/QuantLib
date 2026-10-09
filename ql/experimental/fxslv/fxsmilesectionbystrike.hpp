#ifndef quantlib_fx_smile_section_strike_hpp
#define quantlib_fx_smile_section_strike_hpp

#include <ql/math/array.hpp>
#include <ql/experimental/fxslv/fxsmilesection.hpp>

namespace QuantLib {

    class FxSmileSectionByStrike : public FxSmileSection {
      public:
        //! Date mode (see FxSmileSection).
        FxSmileSectionByStrike(const Date& exerciseDate,
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
        FxSmileSectionByStrike(Time exerciseTime,
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
        virtual Volatility _volByStrike(Real strike,
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

        mutable std::vector<Real> params_;

    };

    //typedef ext::shared_ptr<FxSmileSectionByStrike> fxSmileSectionByStrikePtr;


    class PolynomialSmileSection : public FxSmileSectionByStrike {
      public:
        //! Date mode (see FxSmileSection).
        PolynomialSmileSection(const Date& exerciseDate,
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
        PolynomialSmileSection(Time exerciseTime,
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
        //! \name fxSmileSectionByStrike interface
        //@{
        Volatility _volByStrike(Real strike,
                                Real fwd,
                                Time tau,
                                const std::vector<Real>& params) const override;
        //@}

      protected:
        Array initialParams() const override;
    };


    class FxSabrSmileSection : public FxSmileSectionByStrike {
      public:
        //! Date mode (see FxSmileSection).
        FxSabrSmileSection(const Date& exerciseDate,
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
        FxSabrSmileSection(Time exerciseTime,
                           const Handle<Quote>& spot,
                           const ext::shared_ptr<FxSmileQuotes>& quotes,
                           const Handle<YieldTermStructure>& foreignDiscount,
                           const Handle<YieldTermStructure>& domesticDiscount,
                           DeltaVolQuote::DeltaType deltaType,
                           DeltaVolQuote::AtmType atmType,
                           const DayCounter& dayCounter = DayCounter());

        // Introspection
        Real alpha() const { return params_[0]; };
        Real beta() const { return 1.0; };
        Real nu() const { return params_[1]; };
        Real rho() const { return params_[2]; };

      private:
        //! \name fxSmileSectionByStrike interface
        //@{
        Volatility _volByStrike(Real strike,
                                Real fwd,
                                Time tau,
                                const std::vector<Real>& params) const override;
        //@}

      protected:
        Array initialParams() const override;
    };


    //! SVI (Stochastic Volatility Inspired) smile section.
    /*! Total implied variance is given by the SVI raw parameterization:
        \f$ w(k) = a + b \left( \rho (k - m) + \sqrt{(k - m)^2 + \sigma^2} \right) \f$
        where \f$ k = \log(K / F) \f$ is the log-moneyness.
        Implied volatility is \f$ \sigma_{impl} = \sqrt{w(k) / \tau} \f$.

        Parameters: a, b, rho, m, sigma (5 parameters).
    */
    class FxSviSmileSection : public FxSmileSectionByStrike {
      public:
        //! Date mode (see FxSmileSection).
        FxSviSmileSection(const Date& exerciseDate,
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
        FxSviSmileSection(Time exerciseTime,
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
        Real rho() const { return params_[2]; };
        Real m() const { return params_[3]; };
        Real sigma() const { return params_[4]; };

      private:
        //! \name fxSmileSectionByStrike interface
        //@{
        Volatility _volByStrike(Real strike,
                                Real fwd,
                                Time tau,
                                const std::vector<Real>& params) const override;
        //@}

      protected:
        Array initialParams() const override;
    };

} // namespace QuantLib

#endif