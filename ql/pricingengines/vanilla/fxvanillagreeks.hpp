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

/*! \file fxvanillagreeks.hpp
    \brief FX vanilla option risk metrics via bump-and-reval finite differences.
*/

#ifndef quantlib_fx_vanilla_greeks_hpp
#define quantlib_fx_vanilla_greeks_hpp

#include <ql/instruments/vanillaoption.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/equityfx/fixedlocalvolsurface.hpp>
#include <iostream>
#include <vector>

namespace QuantLib {

    //! Risk metrics for a single FX vanilla option position.
    /*!
        All monetary values are expressed in domestic currency and are scaled
        by the \p notional supplied to \c FxVanillaBumpRisk.

        \li \c npv        — present value
        \li \c spotDelta  — \f$ \partial V / \partial S \f$
        \li \c fwdDelta   — \f$ \partial V / \partial F \f$
        \li \c spotGamma  — \f$ \partial^2 V / \partial S^2 \f$
        \li \c fwdGamma   — \f$ \partial^2 V / \partial F^2 \f$
        \li \c theta      — value change per calendar day (negative for long options)
        \li \c vanna       — \f$ \partial^2 V / (\partial S\,\partial\sigma) \f$
        \li \c volga       — \f$ \partial^2 V / \partial\sigma^2 \f$
    */
    struct FxVanillaGreeks {
        Real npv;
        Real spotDelta;
        Real fwdDelta;
        Real spotGamma;
        Real fwdGamma;
        Real theta;
        Real vanna;
        Real volga;
    };

    //! FX vanilla option risk calculator via bump-and-reval.
    /*!
        Prices a \c VanillaOption driven by a \c GeneralizedBlackScholesProcess
        backed by an FX variance surface (\c fxVarianceSurface) and computes all
        standard FX option risk metrics using symmetric finite differences.

        Two pricing modes are supported (selected at call time):
        - <b>Black-Scholes FD</b> (\c localVol=false): the implied vol surface is
          used directly in the FD PDE, equivalent to pricing with the BS formula
          in each time-step.
        - <b>Local-vol Dupire FD</b> (\c localVol=true): Dupire's formula is applied
          at each FD grid cell, making the model consistent with the full vol smile.

        Vol bumps for vanna and volga apply a parallel shift to <em>all</em>
        supplied ATM vol quotes simultaneously, representing a uniform translation
        of the entire vol surface.

        All reported results are scaled by \p notional.

        Convenience methods are also provided to:
        - build a \c FixedLocalVolSurface by sampling the Dupire local vol on a
          user-specified (time, strike) grid, and
        - print a side-by-side comparison of Dupire vs. FixedLocalVolSurface local
          vol values at those grid points.

        \ingroup vanillaengines
    */
    class FxVanillaBumpRisk {
      public:
        /*! \param option        The vanilla option to price (exercise + payoff).
            \param process       GBS process holding spot, domestic and foreign
                                 discount curves, and the FX variance surface.
            \param spotQuote     The \c SimpleQuote underlying the spot handle inside
                                 \p process — used to bump for delta/gamma/vanna.
            \param atmVolQuotes  All pillar ATM \c SimpleQuote objects inside the
                                 FX variance surface — bumped in parallel for
                                 vanna and volga.
            \param notional      Option notional in foreign currency units.
            \param spotBump      Relative spot bump size (e.g. 0.001 = 0.1 pct).
            \param volBump       Absolute vol bump size (e.g. 0.001 = 10 bp).
            \param tGrid         Number of time steps for the FD solver.
            \param xGrid         Number of spot grid points for the FD solver.
        */
        FxVanillaBumpRisk(ext::shared_ptr<VanillaOption> option,
                          ext::shared_ptr<GeneralizedBlackScholesProcess> process,
                          ext::shared_ptr<SimpleQuote> spotQuote,
                          std::vector<ext::shared_ptr<SimpleQuote>> atmVolQuotes,
                          Real notional = 1.0e6,
                          Real spotBump = 0.001,
                          Real volBump = 0.001,
                          Size tGrid = 100,
                          Size xGrid = 100);

        //! Compute all Greeks via bump-and-reval.
        /*!
            \param localVol  If \c true, uses the Dupire local-vol FD engine;
                             if \c false, uses the Black-Scholes FD engine.
            \return          Fully populated \c FxVanillaGreeks struct.
        */
        FxVanillaGreeks calculate(bool localVol = true) const;

        //! Build a \c FixedLocalVolSurface by sampling the Dupire local vol on a grid.
        /*!
            The Dupire local vol is evaluated at every (\p times[i], \p strikes[j])
            point and stored in a \c FixedLocalVolSurface.  This pre-computed surface
            is useful as input to Monte Carlo engines where repeated evaluation of
            the Dupire formula would be expensive.

            \param times    Vector of year-fraction time points (strictly increasing,
                            first element must be positive).
            \param strikes  Vector of absolute strike levels (sorted ascending).
        */
        ext::shared_ptr<FixedLocalVolSurface>
        buildFixedLocalVolSurface(const std::vector<Time>& times,
                                  const std::vector<Real>& strikes) const;

        //! Print a table comparing Dupire vs. \c FixedLocalVolSurface local vol values.
        /*!
            For each (strike, time) grid point the table shows two rows:
            - <tt>D</tt>: local vol from the Dupire \c LocalVolSurface (on-the-fly)
            - <tt>F</tt>: local vol from the pre-sampled \c FixedLocalVolSurface

            Both values are printed as percentages.
        */
        void printLocalVolComparison(const std::vector<Time>& times,
                                     const std::vector<Real>& strikes,
                                     std::ostream& out = std::cout) const;

      private:
        ext::shared_ptr<PricingEngine> makeEngine(bool localVol) const;

        ext::shared_ptr<VanillaOption> option_;
        ext::shared_ptr<GeneralizedBlackScholesProcess> process_;
        ext::shared_ptr<SimpleQuote> spotQuote_;
        std::vector<ext::shared_ptr<SimpleQuote>> atmVolQuotes_;
        Real notional_;
        Real spotBump_;
        Real volBump_;
        Size tGrid_, xGrid_;
    };

}

#endif
