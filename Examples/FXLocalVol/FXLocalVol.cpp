/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*!
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

/*! \file FXLocalVol.cpp
    \brief FX local vol risk engine example.

    Demonstrates the full workflow for risk-managing an FX vanilla options book
    using a local volatility surface derived from a calibrated FX variance surface:

    1.  Market data setup: EURUSD-like pillar quotes (ATM, 25d and 10d RR/BF)
        for five tenors (1W, 1M, 3M, 6M, 1Y).

    2.  Calibrate an fxVarianceSurfaceNCP<quadraticSmileSection>.

    3.  Wrap the variance surface in a GeneralizedBlackScholesProcess.

    4.  Price a 3-month EURUSD EUR call at the ATM forward strike using:
        - Black-Scholes FD  (localVol=false)
        - Local-vol Dupire FD  (localVol=true)

    5.  Compute all standard FX option risk metrics via FxVanillaBumpRisk.
        Reported units:
        - spotDelta / fwdDelta : USD per 1% spot / fwd move
        - spotGamma / fwdGamma : USD (delta change per 1% spot / fwd move)
        - theta                : USD per calendar day
        - vega                 : USD per 1% (100bp) ATM vol move
        - rega                 : USD per 0.1% 1M-equivalent RR move
        - sega                 : USD per 0.01% 1M-equivalent BF move
        - vanna / navva        : USD (delta/vega change per 1% vol/spot move)
        - volga                : USD (vega change per 1% vol move)

    6.  Compare sticky-delta vs sticky-strike Greeks.

    7.  Build a FixedLocalVolSurface and compare with Dupire at grid points.
*/

#include <ql/qldefines.hpp>
#if !defined(BOOST_ALL_NO_LIB) && defined(BOOST_MSVC)
#    include <ql/auto_link.hpp>
#endif

// FX vol surface
#include <ql/termstructures/volatility/equityfx/fxvariancesurface.hpp>
#include <ql/termstructures/volatility/fxsmilesectionbydelta.hpp>
#include <ql/termstructures/tradingtimetermstructure.hpp>

// Process and pricing
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/pricingengines/vanilla/fxvanillagreeks.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>

// Instruments
#include <ql/instruments/vanillaoption.hpp>
#include <ql/exercise.hpp>
#include <ql/instruments/payoffs.hpp>

// Market data primitives
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/experimental/fx/deltavolquote.hpp>

// Time
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/time/calendars/weekendsonly.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/settings.hpp>

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace QuantLib;

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

static void printSeparator(int width = 72) {
    std::cout << std::string(width, '=') << "\n";
}

static void printGreeks(const std::string& label, const FxVanillaGreeks& g) {
    const int w0 = 40;
    const int w1 = 14;
    std::cout << "\n  " << label << "\n"
              << "  " << std::string(w0 + w1, '-') << "\n"
              << std::fixed;
    auto row = [&](const std::string& name, Real val, int prec = 2) {
        std::cout << "  " << std::setw(w0) << std::left << name
                  << std::setw(w1) << std::right << std::setprecision(prec) << val
                  << "\n";
    };
    row("NPV (USD)",                              g.npv,       2);
    row("Spot delta  (USD per 1% spot)",          g.spotDelta, 2);
    row("Fwd delta   (USD per 1% fwd)",           g.fwdDelta,  2);
    row("Spot gamma  (USD per 1% spot move)",     g.spotGamma, 2);
    row("Fwd gamma   (USD per 1% fwd move)",      g.fwdGamma,  2);
    row("Theta       (USD/day)",                  g.theta,     2);
    row("Vega        (USD per 1% vol)",           g.vega,      2);
    row("Rega        (USD per 0.1% 1M RR)",       g.rega,      2);
    row("Sega        (USD per 0.01% 1M BF)",      g.sega,      2);
    row("Vanna       (USD per 1%S x 1%vol)",      g.vanna,     2);
    row("Navva       (USD per 1%vol x 1%S) =Vanna",g.navva,   2);
    row("Volga       (USD per 1%vol x 1%vol)",    g.volga,     2);
    std::cout << "  " << std::string(w0 + w1, '-') << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────────────────────────────────────

int main(int, char*[]) {

    try {

        printSeparator();
        std::cout << "  FX Local Vol Risk Engine — EURUSD Example\n";
        printSeparator();

        // ──────────────────────────────────────────────────────────────────────
        //  1. Market data
        // ──────────────────────────────────────────────────────────────────────

        Date today(2, January, 2024);
        Settings::instance().evaluationDate() = today;
        DayCounter dc = Actual365Fixed();

        auto spotSQ = ext::make_shared<SimpleQuote>(1.0850);
        Handle<Quote> spot(spotSQ);

        Handle<YieldTermStructure> eurTs(
            ext::make_shared<FlatForward>(0, NullCalendar(), 0.0400, dc));
        Handle<YieldTermStructure> usdTs(
            ext::make_shared<FlatForward>(0, NullCalendar(), 0.0525, dc));

        Handle<tradingTimeTermStructure> timeTs(
            ext::make_shared<tradingTimeTermStructure>(today, WeekendsOnly(), 0.0));

        DeltaVolQuote::DeltaType  deltaType = DeltaVolQuote::Fwd;
        DeltaVolQuote::AtmType    atmType   = DeltaVolQuote::AtmFwd;
        fxSmileSection::FlyType   flyType   = fxSmileSection::SmileStrangle;

        // Pillar dates: 1W, 1M, 3M, 6M, 1Y
        std::vector<Date> pillars = {
            Date(9,  January,  2024), // 1W
            Date(2,  February, 2024), // 1M
            Date(2,  April,    2024), // 3M
            Date(2,  July,     2024), // 6M
            Date(2,  January,  2025), // 1Y
        };

        struct PillarVols { Real atm, rr25, bf25, rr10, bf10; };
        std::vector<PillarVols> mkt = {
            { 0.0750, -0.008, 0.002, -0.015, 0.005 }, // 1W
            { 0.0800, -0.010, 0.003, -0.020, 0.008 }, // 1M
            { 0.0850, -0.012, 0.004, -0.025, 0.010 }, // 3M
            { 0.0900, -0.015, 0.005, -0.030, 0.012 }, // 6M
            { 0.0950, -0.020, 0.007, -0.040, 0.015 }, // 1Y
        };

        std::vector<Real> deltas = { 0.25, 0.10 };

        // Build all quote handles, keeping SimpleQuote* for bumping.
        std::vector<Handle<Quote>> atms;
        std::vector<std::vector<Handle<Quote>>> rrs(pillars.size()),
                                                 bfs(pillars.size());
        std::vector<ext::shared_ptr<SimpleQuote>> atmSQs;

        // Per-pillar RR and BF SimpleQuotes for rega/sega.
        std::vector<std::vector<ext::shared_ptr<SimpleQuote>>> rrPillarSQs(pillars.size());
        std::vector<std::vector<ext::shared_ptr<SimpleQuote>>> bfPillarSQs(pillars.size());
        std::vector<Time> rrPillarTimes(pillars.size()), bfPillarTimes(pillars.size());

        for (Size i = 0; i < pillars.size(); ++i) {
            auto atmSQ  = ext::make_shared<SimpleQuote>(mkt[i].atm);
            auto rr25SQ = ext::make_shared<SimpleQuote>(mkt[i].rr25);
            auto rr10SQ = ext::make_shared<SimpleQuote>(mkt[i].rr10);
            auto bf25SQ = ext::make_shared<SimpleQuote>(mkt[i].bf25);
            auto bf10SQ = ext::make_shared<SimpleQuote>(mkt[i].bf10);

            atmSQs.push_back(atmSQ);
            rrPillarSQs[i] = { rr25SQ, rr10SQ };
            bfPillarSQs[i] = { bf25SQ, bf10SQ };

            const Time T = dc.yearFraction(today, pillars[i]);
            rrPillarTimes[i] = bfPillarTimes[i] = T;

            atms.push_back(Handle<Quote>(atmSQ));
            rrs[i] = { Handle<Quote>(rr25SQ), Handle<Quote>(rr10SQ) };
            bfs[i] = { Handle<Quote>(bf25SQ), Handle<Quote>(bf10SQ) };
        }

        // ──────────────────────────────────────────────────────────────────────
        //  2. Calibrate the FX variance surface
        // ──────────────────────────────────────────────────────────────────────

        auto fxVolSurface = ext::make_shared<fxVarianceSurfaceNCP<quadraticSmileSection>>(
            today, spot, pillars, atms, rrs, bfs, deltas,
            eurTs, usdTs, timeTs,
            deltaType, atmType, flyType,
            WeekendsOnly(), Following, true);

        fxVolSurface->enableExtrapolation();

        // ──────────────────────────────────────────────────────────────────────
        //  3. Build the GeneralizedBlackScholesProcess
        // ──────────────────────────────────────────────────────────────────────

        auto process = ext::make_shared<GeneralizedBlackScholesProcess>(
            spot,
            eurTs,  // dividendYield = foreign (EUR) rate
            usdTs,  // riskFreeRate  = domestic (USD) rate
            Handle<BlackVolTermStructure>(fxVolSurface));

        // ──────────────────────────────────────────────────────────────────────
        //  4. Define the option: 3M EUR call at ATM forward
        // ──────────────────────────────────────────────────────────────────────

        Date expiryDate = pillars[2]; // 3M: 2024-04-02
        Time T          = dc.yearFraction(today, expiryDate);
        Real Bd         = usdTs->discount(T);
        Real Bf         = eurTs->discount(T);
        Real fwd        = spot->value() * Bf / Bd;
        Real strike     = fwd; // ATM forward

        auto payoff   = ext::make_shared<PlainVanillaPayoff>(Option::Call, strike);
        auto exercise = ext::make_shared<EuropeanExercise>(expiryDate);
        auto option   = ext::make_shared<VanillaOption>(payoff, exercise);

        // ──────────────────────────────────────────────────────────────────────
        //  Print market data summary
        // ──────────────────────────────────────────────────────────────────────

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "\n  Market data:\n"
                  << "    Reference date  : " << today << "\n"
                  << "    Spot (EURUSD)   : " << spot->value() << "\n"
                  << "    EUR rate (for)  : 4.00%\n"
                  << "    USD rate (dom)  : 5.25%\n"
                  << "    Delta type      : Forward (Fwd)\n"
                  << "    ATM type        : Delta-neutral forward (AtmFwd)\n"
                  << "    Fly type        : Smile strangle\n\n";

        const int wp = 7;
        std::cout << "  Pillar quotes:\n";
        std::cout << "  " << std::setw(8) << "Tenor"
                  << std::setw(wp) << "ATM%"
                  << std::setw(wp) << "25RR%"
                  << std::setw(wp) << "25BF%"
                  << std::setw(wp) << "10RR%"
                  << std::setw(wp) << "10BF%"
                  << "\n"
                  << "  " << std::string(8 + 5 * wp, '-') << "\n";

        std::vector<std::string> tenorLabels = {"1W","1M","3M","6M","1Y"};
        for (Size i = 0; i < pillars.size(); ++i) {
            std::cout << "  " << std::setw(8) << tenorLabels[i]
                      << std::setw(wp) << std::setprecision(2) << mkt[i].atm  * 100
                      << std::setw(wp) << mkt[i].rr25 * 100
                      << std::setw(wp) << mkt[i].bf25 * 100
                      << std::setw(wp) << mkt[i].rr10 * 100
                      << std::setw(wp) << mkt[i].bf10 * 100
                      << "\n";
        }

        const Real notional = 1.0e6; // EUR 1,000,000
        std::cout << "\n  Option: 3M EURUSD EUR call / USD put\n"
                  << "    Expiry    : " << expiryDate << "\n"
                  << "    Strike    : " << std::setprecision(4) << strike
                  << " (3M ATM forward)\n"
                  << "    3M fwd    : " << fwd << "\n"
                  << "    Notional  : EUR "
                  << std::fixed << std::setprecision(0) << notional << "\n";

        // ──────────────────────────────────────────────────────────────────────
        //  5. Compute Greeks
        // ──────────────────────────────────────────────────────────────────────

        FxVanillaBumpRisk riskCalc(
            option, process, spotSQ, atmSQs,
            rrPillarSQs, rrPillarTimes,
            bfPillarSQs, bfPillarTimes,
            notional,
            /*spotBump=*/0.001,
            /*volBump=*/0.001,
            /*rrBump=*/0.001,
            /*bfBump=*/0.001,
            /*tGrid=*/100,
            /*xGrid=*/100);

        FxVanillaGreeks bsGreeks = riskCalc.calculate(/*localVol=*/false);
        FxVanillaGreeks lvGreeks = riskCalc.calculate(/*localVol=*/true);

        // ──────────────────────────────────────────────────────────────────────
        //  Print pricing comparison
        // ──────────────────────────────────────────────────────────────────────

        printSeparator();
        std::cout << "  Pricing comparison  (notional = EUR 1,000,000)\n";
        printSeparator();

        const int wa = 30;
        const int wb = 14;
        std::cout << "\n  " << std::setw(wa) << std::left << "Engine"
                  << std::setw(wb) << std::right << "NPV (USD)"
                  << "\n"
                  << "  " << std::string(wa + wb, '-') << "\n"
                  << "  " << std::setw(wa) << std::left << "Black-Scholes FD"
                  << std::setw(wb) << std::right << std::fixed
                  << std::setprecision(2) << bsGreeks.npv
                  << "\n"
                  << "  " << std::setw(wa) << std::left << "Local vol Dupire FD"
                  << std::setw(wb) << std::right << lvGreeks.npv
                  << "\n"
                  << "  " << std::setw(wa) << std::left << "Difference"
                  << std::setw(wb) << std::right << (lvGreeks.npv - bsGreeks.npv)
                  << "\n";

        // ──────────────────────────────────────────────────────────────────────
        //  Print Greeks
        // ──────────────────────────────────────────────────────────────────────

        printSeparator();
        std::cout << "  Greeks  (notional = EUR 1,000,000)\n";
        printSeparator();

        printGreeks("Black-Scholes FD (sticky-delta)", bsGreeks);
        printGreeks("Local vol Dupire FD (sticky-delta)", lvGreeks);

        std::cout << "\n  Notes on rega/sega time-scaling:\n"
                  << "    Rega: 1M tenor bumped by rrBump=0.1%; other tenors by\n"
                  << "          rrBump * sqrt(T_1M / T_pillar). E.g.:\n"
                  << "          3M: 0.1% * sqrt(1/3) = 0.058%\n"
                  << "          1Y: 0.1% * sqrt(1/12) = 0.029%\n"
                  << "    Sega: same scaling with bfBump=0.1%, reported for 0.01%.\n\n";

        // ──────────────────────────────────────────────────────────────────────
        //  Sticky-delta vs sticky-strike comparison
        // ──────────────────────────────────────────────────────────────────────

        printSeparator();
        std::cout << "  Sticky-delta vs Sticky-strike (Local vol Dupire FD)\n";
        printSeparator();

        FxVanillaGreeks ssGreeks = riskCalc.calculate(/*localVol=*/true,
                                                       FxVanillaBumpRisk::StickyType::Strike);

        printGreeks("Local vol Dupire FD (sticky-delta)", lvGreeks);
        printGreeks("Local vol Dupire FD (sticky-strike)", ssGreeks);

        const int wd = 44, wdv = 12;
        std::cout << "\n  Difference (sticky-delta minus sticky-strike):\n"
                  << "  " << std::string(wd + wdv, '-') << "\n"
                  << std::fixed;
        auto diffRow = [&](const std::string& name, Real a, Real b, int prec = 2) {
            std::cout << "  " << std::setw(wd) << std::left << name
                      << std::setw(wdv) << std::right << std::setprecision(prec)
                      << (a - b) << "\n";
        };
        diffRow("Spot delta (USD per 1% spot)",  lvGreeks.spotDelta, ssGreeks.spotDelta);
        diffRow("Fwd delta  (USD per 1% fwd)",   lvGreeks.fwdDelta,  ssGreeks.fwdDelta);
        diffRow("Spot gamma (USD per 1% spot)",  lvGreeks.spotGamma, ssGreeks.spotGamma);
        diffRow("Vega       (USD per 1% vol)",   lvGreeks.vega,      ssGreeks.vega);
        diffRow("Vanna      (USD per 1%S x 1%vol)", lvGreeks.vanna,  ssGreeks.vanna);
        diffRow("Volga      (USD per 1%vol x 1%vol)", lvGreeks.volga, ssGreeks.volga);
        std::cout << "  " << std::string(wd + wdv, '-') << "\n\n"
                  << "  Non-zero spot delta difference = skew adjustment to delta.\n"
                  << "  With EUR put skew (negative RR), sticky-delta delta is lower\n"
                  << "  because the vol surface falls as spot rises.\n\n";

        // ──────────────────────────────────────────────────────────────────────
        //  6. Local vol surface: Dupire vs FixedLocalVolSurface comparison
        // ──────────────────────────────────────────────────────────────────────

        printSeparator();
        std::cout << "  Local vol surface comparison\n";
        printSeparator();

        std::vector<Time> lvTimes = { 1.0/12, 3.0/12, 6.0/12, 12.0/12 };

        std::vector<Real> lvStrikes;
        for (int i = -3; i <= 3; ++i)
            lvStrikes.push_back(fwd * std::exp(0.05 * i));

        riskCalc.printLocalVolComparison(lvTimes, lvStrikes);

        std::cout << "  D = on-the-fly Dupire local vol\n"
                  << "  F = pre-sampled FixedLocalVolSurface\n"
                  << "  Values in percent (%)\n\n";

        auto fixedLV = riskCalc.buildFixedLocalVolSurface(lvTimes, lvStrikes);
        std::cout << "  FixedLocalVolSurface built with "
                  << lvTimes.size() << " time steps x "
                  << lvStrikes.size() << " strikes.\n"
                  << "  Sample local vol at (T=0.25, K=" << std::fixed
                  << std::setprecision(4) << strike << "): "
                  << std::setprecision(2)
                  << fixedLV->localVol(0.25, strike, true) * 100.0 << "%\n\n";

        printSeparator();
        std::cout << "  Done.\n";
        printSeparator();

        return 0;

    } catch (std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "\nUnknown error\n";
        return 1;
    }
}
