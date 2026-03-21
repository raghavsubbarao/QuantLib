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

    2.  Calibrate an fxVarianceSurfaceNCP<quadraticSmileSection> — a variance
        term structure that interpolates between pillar smiles in probability space.

    3.  Wrap the variance surface in a GeneralizedBlackScholesProcess, which makes
        it usable by all QuantLib pricing engines.

    4.  Price a 3-month EURUSD EUR call at the ATM forward strike using:
        - Black-Scholes FD  (constant implied vol, localVol=false)
        - Local-vol Dupire FD  (Dupire formula at each grid cell, localVol=true)

    5.  Compute all standard FX option risk metrics via FxVanillaBumpRisk:
        - NPV (domestic currency, notional-scaled)
        - Spot delta  and  forward delta
        - Spot gamma  and  forward gamma
        - Theta  (per calendar day)
        - Vanna  (cross derivative: spot × ATM vol)
        - Volga  (second derivative: ATM vol²)

    6.  Compare sticky-delta vs sticky-strike Greeks:
        - Sticky-delta (StickyType::Delta, default): market convention.  The FX
          variance surface is delta-parameterised; when spot is bumped the smile
          re-anchors in delta space.  The resulting delta blends direct spot
          sensitivity with the vol surface moving with spot (total delta).
        - Sticky-strike (StickyType::Strike): model-consistent.  The Dupire local
          vol is pre-sampled on a (t,K) grid and frozen before bumping spot, so
          local vols at fixed strikes do not change.  Differences from sticky-delta
          reveal the vol-of-vol adjustment implicit in the skew.

    7.  Build a FixedLocalVolSurface by sampling the Dupire local vol on a
        (time, strike) grid and print a side-by-side comparison showing that
        the two surfaces agree at grid points.
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
    const int w0 = 28;
    const int w1 = 16;
    std::cout << "\n  " << label << "\n"
              << "  " << std::string(w0 + w1, '-') << "\n"
              << std::fixed;
    auto row = [&](const std::string& name, Real val, int prec = 2) {
        std::cout << "  " << std::setw(w0) << std::left << name
                  << std::setw(w1) << std::right << std::setprecision(prec) << val
                  << "\n";
    };
    row("NPV (USD)",            g.npv,       2);
    row("Spot delta (USD/pip)", g.spotDelta * 0.0001,  4); // scaled to 1 pip
    row("Fwd delta  (USD/pip)", g.fwdDelta  * 0.0001,  4);
    row("Spot gamma (USD/pip²)",g.spotGamma * 1e-8,    4); // per pip²
    row("Fwd gamma  (USD/pip²)",g.fwdGamma  * 1e-8,    4);
    row("Theta (USD/day)",      g.theta,     2);
    row("Vanna  (USD/vol-pt)",  g.vanna * 0.01,  2); // vol-pt = 1 percent
    row("Volga  (USD/vol-pt²)", g.volga * 1e-4,  2); // per (1 percent)²
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
        //
        //  Currency pair : EUR/USD  (spot = USD per EUR)
        //  Spot          : 1.0850
        //  EUR rate      : 4.00% flat   (foreign, "dividend" yield in GBS)
        //  USD rate      : 5.25% flat   (domestic, risk-free in GBS)
        //  Delta type    : forward delta (standard for G10 non-USD/JPY)
        //  ATM type      : delta-neutral forward
        //  Fly type      : smile strangle
        // ──────────────────────────────────────────────────────────────────────

        Date today(2, January, 2024);
        Settings::instance().evaluationDate() = today;

        auto spotSQ = ext::make_shared<SimpleQuote>(1.0850);
        Handle<Quote> spot(spotSQ);

        // Use settlement-days=0 so that referenceDate() tracks evaluationDate.
        // This is required for theta to compute correctly when the evaluation
        // date is shifted by one calendar day.
        Handle<YieldTermStructure> eurTs(
            ext::make_shared<FlatForward>(0, NullCalendar(), 0.0400, Actual365Fixed()));
        Handle<YieldTermStructure> usdTs(
            ext::make_shared<FlatForward>(0, NullCalendar(), 0.0525, Actual365Fixed()));

        // Trading time term structure: weekends carry zero vol-time weight
        Handle<tradingTimeTermStructure> timeTs(
            ext::make_shared<tradingTimeTermStructure>(today, WeekendsOnly(), 0.0));

        DeltaVolQuote::DeltaType  deltaType = DeltaVolQuote::Fwd;
        DeltaVolQuote::AtmType    atmType   = DeltaVolQuote::AtmFwd;
        fxSmileSection::FlyType   flyType   = fxSmileSection::SmileStrangle;
        DayCounter                dc        = Actual365Fixed();

        // Pillar dates: 1W, 1M, 3M, 6M, 1Y
        std::vector<Date> pillars = {
            Date(9,  January,  2024), // 1W
            Date(2,  February, 2024), // 1M
            Date(2,  April,    2024), // 3M
            Date(2,  July,     2024), // 6M
            Date(2,  January,  2025), // 1Y
        };

        // EURUSD-like vol quotes (smile-strangle convention, negative RR = put skew)
        //  Tenor  ATM    25RR   25BF   10RR   10BF
        struct PillarVols { Real atm, rr25, bf25, rr10, bf10; };
        std::vector<PillarVols> mkt = {
            { 0.0750, -0.008, 0.002, -0.015, 0.005 }, // 1W
            { 0.0800, -0.010, 0.003, -0.020, 0.008 }, // 1M
            { 0.0850, -0.012, 0.004, -0.025, 0.010 }, // 3M
            { 0.0900, -0.015, 0.005, -0.030, 0.012 }, // 6M
            { 0.0950, -0.020, 0.007, -0.040, 0.015 }, // 1Y
        };

        // Build quote handles and collect ATM SimpleQuotes for bumping
        std::vector<Handle<Quote>> atms;
        std::vector<std::vector<Handle<Quote>>> rrs(pillars.size()),
                                                 bfs(pillars.size());
        std::vector<ext::shared_ptr<SimpleQuote>> atmSQs; // for FxVanillaBumpRisk

        std::vector<Real> deltas = { 0.25, 0.10 };

        for (Size i = 0; i < pillars.size(); ++i) {
            auto atmSQ = ext::make_shared<SimpleQuote>(mkt[i].atm);
            atmSQs.push_back(atmSQ);
            atms.push_back(Handle<Quote>(atmSQ));
            rrs[i] = { makeQuoteHandle(mkt[i].rr25), makeQuoteHandle(mkt[i].rr10) };
            bfs[i] = { makeQuoteHandle(mkt[i].bf25), makeQuoteHandle(mkt[i].bf10) };
        }

        // ──────────────────────────────────────────────────────────────────────
        //  2. Calibrate the FX variance surface
        //
        //  fxVarianceSurfaceNCP<quadraticSmileSection> interpolates between
        //  pillar smiles in probability space (NCP = normed call price), giving
        //  an arbitrage-free surface across tenors.
        // ──────────────────────────────────────────────────────────────────────

        auto fxVolSurface = ext::make_shared<fxVarianceSurfaceNCP<quadraticSmileSection>>(
            today, spot, pillars, atms, rrs, bfs, deltas,
            eurTs, usdTs, timeTs,
            deltaType, atmType, flyType,
            WeekendsOnly(), Following, true);

        fxVolSurface->enableExtrapolation();

        // ──────────────────────────────────────────────────────────────────────
        //  3. Build the GeneralizedBlackScholesProcess
        //
        //  For FX: dividendTS = foreign (EUR) rate,  riskFreeTS = domestic (USD).
        // ──────────────────────────────────────────────────────────────────────

        auto process = ext::make_shared<GeneralizedBlackScholesProcess>(
            spot,
            eurTs,  // dividendYield = foreign rate
            usdTs,  // riskFreeRate  = domestic rate
            Handle<BlackVolTermStructure>(fxVolSurface));

        // ──────────────────────────────────────────────────────────────────────
        //  4. Define the option: 3M EUR call at ATM forward
        //
        //  ATM forward strike = S * B_EUR(3M) / B_USD(3M)
        // ──────────────────────────────────────────────────────────────────────

        Date expiryDate = pillars[2]; // 3M: 2024-04-02
        Time T = dc.yearFraction(today, expiryDate);
        Real Bd = usdTs->discount(T);
        Real Bf = eurTs->discount(T);
        Real fwd = spot->value() * Bf / Bd;

        // Use the ATM forward as the strike (at-the-money forward call)
        Real strike = fwd;

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
        //  5. Pricing comparison: Black-Scholes FD vs Local-vol Dupire FD
        // ──────────────────────────────────────────────────────────────────────

        FxVanillaBumpRisk riskCalc(
            option, process, spotSQ, atmSQs,
            notional,
            /*spotBump=*/0.001,  // 0.1% of spot
            /*volBump=*/0.001,   // 10 bp
            /*tGrid=*/100,
            /*xGrid=*/100);

        FxVanillaGreeks bsGreeks  = riskCalc.calculate(/*localVol=*/false);
        FxVanillaGreeks lvGreeks  = riskCalc.calculate(/*localVol=*/true);

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

        printGreeks("Black-Scholes FD", bsGreeks);
        printGreeks("Local vol Dupire FD (sticky-delta)", lvGreeks);

        // Notes on units:
        std::cout << "\n  Notes:\n"
                  << "    Spot/Fwd delta : USD PnL per 1-pip (0.0001) move in spot\n"
                  << "    Spot/Fwd gamma : USD PnL per 1-pip² move (second order)\n"
                  << "    Vanna          : USD PnL per 1-pip × 1-vol-point (1%) move\n"
                  << "    Volga          : USD PnL per (1-vol-point)² move\n\n";

        // ──────────────────────────────────────────────────────────────────────
        //  Sticky-delta vs sticky-strike comparison
        //
        //  For EURUSD with negative RR (put skew), the sticky-delta and
        //  sticky-strike deltas differ because:
        //
        //  Sticky-delta: as spot rises, the surface re-anchors.  With negative
        //  skew the call at fixed K becomes slightly OTM, moving to lower vol.
        //  This vol decrease offsets the delta, so sticky-delta < sticky-strike.
        //
        //  Sticky-strike: local vols at fixed K are frozen.  The delta only
        //  reflects direct spot sensitivity (no vol adjustment).
        //
        //  The difference between the two is the "skew adjustment" to delta,
        //  sometimes called the "smile delta" correction.
        // ──────────────────────────────────────────────────────────────────────

        printSeparator();
        std::cout << "  Sticky-delta vs Sticky-strike (Local vol Dupire FD)\n";
        printSeparator();

        // sticky-delta already computed above as lvGreeks
        FxVanillaGreeks ssGreeks = riskCalc.calculate(/*localVol=*/true,
                                                       FxVanillaBumpRisk::StickyType::Strike);

        printGreeks("Local vol Dupire FD (sticky-delta)", lvGreeks);
        printGreeks("Local vol Dupire FD (sticky-strike)", ssGreeks);

        const int wd = 36, wdv = 12;
        std::cout << "\n  Difference (sticky-delta minus sticky-strike):\n"
                  << "  " << std::string(wd + wdv, '-') << "\n"
                  << std::fixed;
        auto diffRow = [&](const std::string& name, Real a, Real b, Real scale, int prec = 4) {
            std::cout << "  " << std::setw(wd) << std::left << name
                      << std::setw(wdv) << std::right << std::setprecision(prec)
                      << (a - b) * scale << "\n";
        };
        diffRow("Spot delta (USD/pip)",  lvGreeks.spotDelta, ssGreeks.spotDelta, 0.0001);
        diffRow("Fwd delta  (USD/pip)",  lvGreeks.fwdDelta,  ssGreeks.fwdDelta,  0.0001);
        diffRow("Spot gamma (USD/pip²)", lvGreeks.spotGamma, ssGreeks.spotGamma, 1e-8);
        diffRow("Vanna  (USD/vol-pt)",   lvGreeks.vanna,     ssGreeks.vanna,     0.01, 2);
        diffRow("Volga  (USD/vol-pt²)",  lvGreeks.volga,     ssGreeks.volga,     1e-4, 2);
        std::cout << "  " << std::string(wd + wdv, '-') << "\n\n"
                  << "  A non-zero spot delta difference reflects the vol-of-vol\n"
                  << "  adjustment from the EUR put skew (negative 25d RR):\n"
                  << "  sticky-delta is lower because the vol surface falls as\n"
                  << "  spot rises (the call moves to lower delta / lower vol).\n\n";

        // ──────────────────────────────────────────────────────────────────────
        //  6. Local vol surface: Dupire vs FixedLocalVolSurface comparison
        // ──────────────────────────────────────────────────────────────────────

        printSeparator();
        std::cout << "  Local vol surface comparison\n";
        printSeparator();

        // Time grid: 1M, 3M, 6M, 1Y (as year fractions)
        std::vector<Time> lvTimes = { 1.0/12, 3.0/12, 6.0/12, 12.0/12 };

        // Strike grid: ±15% around ATM forward
        std::vector<Real> lvStrikes;
        for (int i = -3; i <= 3; ++i)
            lvStrikes.push_back(fwd * std::exp(0.05 * i));

        riskCalc.printLocalVolComparison(lvTimes, lvStrikes);

        std::cout << "  D = on-the-fly Dupire local vol\n"
                  << "  F = pre-sampled FixedLocalVolSurface\n"
                  << "  Values in percent (%)\n\n";

        // ──────────────────────────────────────────────────────────────────────
        //  Show how the FixedLocalVolSurface is built
        // ──────────────────────────────────────────────────────────────────────

        auto fixedLV = riskCalc.buildFixedLocalVolSurface(lvTimes, lvStrikes);
        std::cout << "  FixedLocalVolSurface built with "
                  << lvTimes.size() << " time steps × "
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
