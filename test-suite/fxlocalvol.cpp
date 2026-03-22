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

/*! \file fxlocalvol.cpp
    \brief Tests for the FX local vol surface and FxVanillaBumpRisk Greek calculator.
*/

#include "toplevelfixture.hpp"
#include "utilities.hpp"

#include <ql/termstructures/volatility/equityfx/fxvariancesurface.hpp>
#include <ql/termstructures/volatility/fxsmilesectionbydelta.hpp>
#include <ql/termstructures/volatility/equityfx/localvolsurface.hpp>
#include <ql/termstructures/tradingtimetermstructure.hpp>
#include <ql/pricingengines/vanilla/fxvanillagreeks.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/exercise.hpp>
#include <ql/instruments/payoffs.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/experimental/fx/deltavolquote.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/time/calendars/weekendsonly.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/settings.hpp>

using namespace QuantLib;
using namespace boost::unit_test_framework;

BOOST_FIXTURE_TEST_SUITE(QuantLibTests, TopLevelFixture)

BOOST_AUTO_TEST_SUITE(FxLocalVolTests)

// ─────────────────────────────────────────────────────────────────────────────
//  Shared market data used by all tests.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

    struct FxMarket {
        Date today;
        Date expiry3M;
        Date expiry6M;

        DayCounter dc;
        Handle<YieldTermStructure> eurTs; // foreign
        Handle<YieldTermStructure> usdTs; // domestic
        Handle<tradingTimeTermStructure> timeTs;

        ext::shared_ptr<SimpleQuote> spotSQ;
        Handle<Quote> spot;

        std::vector<Date> pillars;
        std::vector<Handle<Quote>> atms;
        std::vector<std::vector<Handle<Quote>>> rrs, bfs;
        std::vector<Real> deltas;
        std::vector<ext::shared_ptr<SimpleQuote>> atmSQs;

        // Per-pillar RR and BF SimpleQuotes for rega/sega.
        std::vector<std::vector<ext::shared_ptr<SimpleQuote>>> rrPillarSQs, bfPillarSQs;
        std::vector<Time> rrPillarTimes, bfPillarTimes;

        DeltaVolQuote::DeltaType deltaType;
        DeltaVolQuote::AtmType   atmType;
        fxSmileSection::FlyType  flyType;

        ext::shared_ptr<fxVarianceSurfaceNCP<quadraticSmileSection>> surface;
        ext::shared_ptr<GeneralizedBlackScholesProcess> process;

        FxMarket() {
            today    = Date(2, January, 2024);
            expiry3M = Date(2, April,   2024);
            expiry6M = Date(2, July,    2024);
            Settings::instance().evaluationDate() = today;
            dc = Actual365Fixed();

            eurTs = Handle<YieldTermStructure>(
                ext::make_shared<FlatForward>(0, NullCalendar(), 0.04, dc));
            usdTs = Handle<YieldTermStructure>(
                ext::make_shared<FlatForward>(0, NullCalendar(), 0.0525, dc));
            timeTs = Handle<tradingTimeTermStructure>(
                ext::make_shared<tradingTimeTermStructure>(today, WeekendsOnly(), 0.0));

            spotSQ = ext::make_shared<SimpleQuote>(1.0850);
            spot   = Handle<Quote>(spotSQ);

            deltaType = DeltaVolQuote::Fwd;
            atmType   = DeltaVolQuote::AtmFwd;
            flyType   = fxSmileSection::SmileStrangle;
            deltas    = { 0.25, 0.10 };

            // Two pillars: 3M and 6M
            pillars = { expiry3M, expiry6M };

            struct Vol { Real atm, rr25, bf25, rr10, bf10; };
            std::vector<Vol> mkt = {
                { 0.0850, -0.012, 0.004, -0.025, 0.010 }, // 3M
                { 0.0900, -0.015, 0.005, -0.030, 0.012 }, // 6M
            };

            rrPillarSQs.resize(pillars.size());
            bfPillarSQs.resize(pillars.size());
            rrPillarTimes.resize(pillars.size());
            bfPillarTimes.resize(pillars.size());

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
                rrs.push_back({ Handle<Quote>(rr25SQ), Handle<Quote>(rr10SQ) });
                bfs.push_back({ Handle<Quote>(bf25SQ), Handle<Quote>(bf10SQ) });
            }

            surface = ext::make_shared<fxVarianceSurfaceNCP<quadraticSmileSection>>(
                today, spot, pillars, atms, rrs, bfs, deltas,
                eurTs, usdTs, timeTs,
                deltaType, atmType, flyType,
                WeekendsOnly(), Following, true);
            surface->enableExtrapolation();

            process = ext::make_shared<GeneralizedBlackScholesProcess>(
                spot, eurTs, usdTs,
                Handle<BlackVolTermStructure>(surface));
        }

        // Build a 3M EUR call at ATM forward
        ext::shared_ptr<VanillaOption> makeAtmCall() const {
            Time T   = dc.yearFraction(today, expiry3M);
            Real fwd = spot->value() * eurTs->discount(T) / usdTs->discount(T);
            auto payoff   = ext::make_shared<PlainVanillaPayoff>(Option::Call, fwd);
            auto exercise = ext::make_shared<EuropeanExercise>(expiry3M);
            return ext::make_shared<VanillaOption>(payoff, exercise);
        }

        // Build a 3M EUR put at ATM forward
        ext::shared_ptr<VanillaOption> makeAtmPut() const {
            Time T   = dc.yearFraction(today, expiry3M);
            Real fwd = spot->value() * eurTs->discount(T) / usdTs->discount(T);
            auto payoff   = ext::make_shared<PlainVanillaPayoff>(Option::Put, fwd);
            auto exercise = ext::make_shared<EuropeanExercise>(expiry3M);
            return ext::make_shared<VanillaOption>(payoff, exercise);
        }
    };

} // anonymous namespace


// ─────────────────────────────────────────────────────────────────────────────
//  Test 1: Local vol surface values are non-negative
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(testLocalVolSurfaceNonNegative) {
    BOOST_TEST_MESSAGE("Testing FX local vol surface: values are non-negative...");

    FxMarket mkt;

    LocalVolSurface lv(mkt.process->blackVolatility(),
                        mkt.process->riskFreeRate(),
                        mkt.process->dividendYield(),
                        mkt.process->x0());
    lv.enableExtrapolation();

    std::vector<Time> times  = { 1.0/12, 3.0/12, 6.0/12 };
    std::vector<Real> strikes = { 0.95, 1.00, 1.05, 1.08, 1.10, 1.15 };

    for (Time t : times)
        for (Real K : strikes) {
            Volatility vol = lv.localVol(t, K, true);
            BOOST_CHECK_MESSAGE(
                vol > 0.0,
                "negative local vol " << vol
                << " at T=" << t << ", K=" << K);
        }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test 2: FixedLocalVolSurface agrees with Dupire at grid points
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(testFixedLocalVolConsistency) {
    BOOST_TEST_MESSAGE("Testing FX local vol: FixedLocalVolSurface agrees with "
                       "Dupire at sample points...");

    FxMarket mkt;
    auto option = mkt.makeAtmCall();

    FxVanillaBumpRisk riskCalc(option, mkt.process, mkt.spotSQ, mkt.atmSQs);

    std::vector<Time> times   = { 1.0/12, 3.0/12, 6.0/12 };
    std::vector<Real> strikes = { 0.95, 1.00, 1.05, 1.10 };

    auto fixedLV = riskCalc.buildFixedLocalVolSurface(times, strikes);
    fixedLV->enableExtrapolation();

    LocalVolSurface lvDupire(mkt.process->blackVolatility(),
                              mkt.process->riskFreeRate(),
                              mkt.process->dividendYield(),
                              mkt.process->x0());
    lvDupire.enableExtrapolation();

    const Real tol = 1e-10;
    for (Time t : times)
        for (Real K : strikes) {
            Real lvD = lvDupire.localVol(t, K, true);
            Real lvF = fixedLV->localVol(t, K, true);
            BOOST_CHECK_MESSAGE(
                std::fabs(lvD - lvF) < tol,
                "Dupire=" << lvD << " FixedLV=" << lvF
                << " diff=" << std::fabs(lvD - lvF)
                << " at T=" << t << " K=" << K);
        }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test 3: Greek signs
//    Call:  spotDelta > 0, spotGamma > 0, theta < 0, volga > 0, vega > 0
//    Put:   spotDelta < 0, spotGamma > 0, theta < 0, volga > 0, vega > 0
//    navva == vanna (by construction)
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(testGreekSigns) {
    BOOST_TEST_MESSAGE("Testing FX local vol Greeks: sign conventions...");

    FxMarket mkt;

    // ── Call Greeks ──────────────────────────────────────────────────────────
    {
        auto option = mkt.makeAtmCall();
        FxVanillaBumpRisk riskCalc(option, mkt.process, mkt.spotSQ, mkt.atmSQs,
                                   mkt.rrPillarSQs, mkt.rrPillarTimes,
                                   mkt.bfPillarSQs, mkt.bfPillarTimes,
                                   /*notional=*/1.0e6,
                                   /*spotBump=*/0.001,
                                   /*volBump=*/0.001,
                                   /*rrBump=*/0.001,
                                   /*bfBump=*/0.001,
                                   /*tGrid=*/50, /*xGrid=*/50);

        FxVanillaGreeks g = riskCalc.calculate(/*localVol=*/true);

        BOOST_CHECK_MESSAGE(g.npv       > 0.0, "call NPV must be positive: "    << g.npv);
        BOOST_CHECK_MESSAGE(g.spotDelta > 0.0, "call spot delta must be >0: "   << g.spotDelta);
        BOOST_CHECK_MESSAGE(g.fwdDelta  > 0.0, "call fwd delta must be >0: "    << g.fwdDelta);
        BOOST_CHECK_MESSAGE(g.spotGamma > 0.0, "call spot gamma must be >0: "   << g.spotGamma);
        BOOST_CHECK_MESSAGE(g.fwdGamma  > 0.0, "call fwd gamma must be >0: "    << g.fwdGamma);
        BOOST_CHECK_MESSAGE(g.theta     < 0.0, "call theta must be negative: "  << g.theta);
        BOOST_CHECK_MESSAGE(g.vega      > 0.0, "call vega must be positive: "   << g.vega);
        BOOST_CHECK_MESSAGE(g.volga     > 0.0, "call volga must be positive: "  << g.volga);

        // navva == vanna (identity, not approximate)
        BOOST_CHECK_MESSAGE(g.navva == g.vanna,
            "navva must equal vanna: navva=" << g.navva << " vanna=" << g.vanna);
    }

    // ── Put Greeks ───────────────────────────────────────────────────────────
    {
        auto option = mkt.makeAtmPut();
        FxVanillaBumpRisk riskCalc(option, mkt.process, mkt.spotSQ, mkt.atmSQs,
                                   mkt.rrPillarSQs, mkt.rrPillarTimes,
                                   mkt.bfPillarSQs, mkt.bfPillarTimes,
                                   /*notional=*/1.0e6,
                                   /*spotBump=*/0.001,
                                   /*volBump=*/0.001,
                                   /*rrBump=*/0.001,
                                   /*bfBump=*/0.001,
                                   /*tGrid=*/50, /*xGrid=*/50);

        FxVanillaGreeks g = riskCalc.calculate(/*localVol=*/true);

        BOOST_CHECK_MESSAGE(g.npv       > 0.0, "put NPV must be positive: "    << g.npv);
        BOOST_CHECK_MESSAGE(g.spotDelta < 0.0, "put spot delta must be <0: "   << g.spotDelta);
        BOOST_CHECK_MESSAGE(g.fwdDelta  < 0.0, "put fwd delta must be <0: "    << g.fwdDelta);
        BOOST_CHECK_MESSAGE(g.spotGamma > 0.0, "put spot gamma must be >0: "   << g.spotGamma);
        BOOST_CHECK_MESSAGE(g.fwdGamma  > 0.0, "put fwd gamma must be >0: "    << g.fwdGamma);
        BOOST_CHECK_MESSAGE(g.theta     < 0.0, "put theta must be negative: "  << g.theta);
        BOOST_CHECK_MESSAGE(g.vega      > 0.0, "put vega must be positive: "   << g.vega);
        BOOST_CHECK_MESSAGE(g.volga     > 0.0, "put volga must be positive: "  << g.volga);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test 4: Put-call parity
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(testPutCallParity) {
    BOOST_TEST_MESSAGE("Testing FX local vol: put-call parity...");

    FxMarket mkt;

    Time T   = mkt.dc.yearFraction(mkt.today, mkt.expiry3M);
    Real Bd  = mkt.usdTs->discount(T);
    Real Bf  = mkt.eurTs->discount(T);
    Real fwd = mkt.spot->value() * Bf / Bd;

    const Real notional = 1.0e6;

    auto callOption = mkt.makeAtmCall();
    auto putOption  = mkt.makeAtmPut();

    FxVanillaBumpRisk callCalc(callOption, mkt.process, mkt.spotSQ, mkt.atmSQs,
                                mkt.rrPillarSQs, mkt.rrPillarTimes,
                                mkt.bfPillarSQs, mkt.bfPillarTimes,
                                notional, 0.001, 0.001, 0.001, 0.001, 50, 50);
    FxVanillaBumpRisk putCalc(putOption, mkt.process, mkt.spotSQ, mkt.atmSQs,
                               mkt.rrPillarSQs, mkt.rrPillarTimes,
                               mkt.bfPillarSQs, mkt.bfPillarTimes,
                               notional, 0.001, 0.001, 0.001, 0.001, 50, 50);

    FxVanillaGreeks callG = callCalc.calculate(true);
    FxVanillaGreeks putG  = putCalc.calculate(true);

    // For ATM forward: call NPV - put NPV ≈ 0
    const Real priceTol = 10.0; // $10 on a 1M notional position
    BOOST_CHECK_MESSAGE(
        std::fabs(callG.npv - putG.npv) < priceTol,
        "put-call parity violation: call-put=" << (callG.npv - putG.npv)
        << " (tolerance=" << priceTol << ")");

    // Gamma parity: call gamma == put gamma (both in USD per 1% spot^2)
    const Real gammaTol = 0.01; // $0.01 in scaled units
    BOOST_CHECK_MESSAGE(
        std::fabs(callG.spotGamma - putG.spotGamma) < gammaTol,
        "gamma parity violation: call gamma=" << callG.spotGamma
        << " put gamma=" << putG.spotGamma);

    // Vega parity: call vega == put vega
    const Real vegaTol = 0.10;
    BOOST_CHECK_MESSAGE(
        std::fabs(callG.vega - putG.vega) < vegaTol,
        "vega parity violation: call vega=" << callG.vega
        << " put vega=" << putG.vega);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test 5: Delta and gamma consistency
//
//  Taylor approximation: ΔV ≈ spotDelta × pctMove + 0.5 × spotGamma × pctMove²
//  where pctMove = ds / (0.01 × S)  (spot move expressed in units of 1%)
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(testDeltaGammaConsistency) {
    BOOST_TEST_MESSAGE("Testing FX local vol: delta-gamma consistency...");

    FxMarket mkt;
    auto option = mkt.makeAtmCall();

    const Real notional = 1.0;  // unit notional for this test

    FxVanillaBumpRisk riskCalc(option, mkt.process, mkt.spotSQ, mkt.atmSQs,
                                mkt.rrPillarSQs, mkt.rrPillarTimes,
                                mkt.bfPillarSQs, mkt.bfPillarTimes,
                                notional, 0.001, 0.001, 0.001, 0.001, 50, 50);
    FxVanillaGreeks g = riskCalc.calculate(true);

    Real S0  = mkt.spotSQ->value();
    Real ds  = 0.0005; // 5 pips absolute
    Real V0  = g.npv;

    mkt.spotSQ->setValue(S0 + ds);
    FxVanillaGreeks gShifted = riskCalc.calculate(true);
    Real Vshifted = gShifted.npv;
    mkt.spotSQ->setValue(S0);

    Real actualPnL = Vshifted - V0;

    // spotDelta is in USD per 1% spot; convert ds to units of 1% spot.
    Real pctMove     = ds / (0.01 * S0);
    Real predictedPnL = g.spotDelta * pctMove + 0.5 * g.spotGamma * pctMove * pctMove;

    // Allow 5% relative error in the second-order Taylor approximation
    Real relErr = std::fabs(actualPnL - predictedPnL) / std::fabs(actualPnL);
    BOOST_CHECK_MESSAGE(
        relErr < 0.05,
        "delta-gamma Taylor approx error=" << relErr * 100 << "% "
        << "(actual=" << actualPnL << " predicted=" << predictedPnL << ")");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test 6: BS vs local vol pricing
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(testBsVsLocalVolPricing) {
    BOOST_TEST_MESSAGE("Testing FX local vol: BS vs local vol pricing...");

    FxMarket mkt;
    auto option = mkt.makeAtmCall();
    const Real notional = 1.0e6;

    FxVanillaBumpRisk riskCalc(option, mkt.process, mkt.spotSQ, mkt.atmSQs,
                                mkt.rrPillarSQs, mkt.rrPillarTimes,
                                mkt.bfPillarSQs, mkt.bfPillarTimes,
                                notional, 0.001, 0.001, 0.001, 0.001, 100, 100);

    FxVanillaGreeks bsG = riskCalc.calculate(false);
    FxVanillaGreeks lvG = riskCalc.calculate(true);

    BOOST_CHECK_MESSAGE(bsG.npv > 0.0, "BS price must be positive: " << bsG.npv);
    BOOST_CHECK_MESSAGE(lvG.npv > 0.0, "LV price must be positive: " << lvG.npv);

    Real relDiff = std::fabs(bsG.npv - lvG.npv) / bsG.npv;
    BOOST_CHECK_MESSAGE(
        relDiff < 0.20,
        "BS and local vol prices differ too much: BS=" << bsG.npv
        << " LV=" << lvG.npv << " relDiff=" << relDiff * 100 << "%");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test 7: Sticky-delta vs sticky-strike deltas differ when skew is present
// ─────────────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(testStickyDeltaVsStickyStrike) {
    BOOST_TEST_MESSAGE("Testing FX local vol: sticky-delta vs sticky-strike deltas differ...");

    FxMarket mkt;
    auto option = mkt.makeAtmCall();
    const Real notional = 1.0e6;

    FxVanillaBumpRisk riskCalc(option, mkt.process, mkt.spotSQ, mkt.atmSQs,
                                mkt.rrPillarSQs, mkt.rrPillarTimes,
                                mkt.bfPillarSQs, mkt.bfPillarTimes,
                                notional,
                                /*spotBump=*/0.001,
                                /*volBump=*/0.001,
                                /*rrBump=*/0.001,
                                /*bfBump=*/0.001,
                                /*tGrid=*/50, /*xGrid=*/50,
                                /*lvTimePts=*/15, /*lvStrikePts=*/30);

    FxVanillaGreeks sdG = riskCalc.calculate(true, FxVanillaBumpRisk::StickyType::Delta);
    FxVanillaGreeks ssG = riskCalc.calculate(true, FxVanillaBumpRisk::StickyType::Strike);

    BOOST_CHECK_MESSAGE(sdG.spotDelta > 0.0,
        "sticky-delta call spot delta must be >0: " << sdG.spotDelta);
    BOOST_CHECK_MESSAGE(ssG.spotDelta > 0.0,
        "sticky-strike call spot delta must be >0: " << ssG.spotDelta);

    BOOST_CHECK_MESSAGE(sdG.npv > 0.0, "sticky-delta NPV must be >0: " << sdG.npv);
    BOOST_CHECK_MESSAGE(ssG.npv > 0.0, "sticky-strike NPV must be >0: " << ssG.npv);

    const Real npvTol = 10.0;
    BOOST_CHECK_MESSAGE(
        std::fabs(sdG.npv - ssG.npv) < npvTol,
        "sticky-delta and sticky-strike NPVs should agree: "
        << "sd=" << sdG.npv << " ss=" << ssG.npv
        << " diff=" << std::fabs(sdG.npv - ssG.npv));

    // Deltas must differ due to skew (at least 0.01% relative difference).
    const Real minRelDiff = 1.0e-4;
    const Real relDiff = std::fabs(sdG.spotDelta - ssG.spotDelta)
                         / std::fabs(sdG.spotDelta);
    BOOST_CHECK_MESSAGE(
        relDiff > minRelDiff,
        "sticky-delta and sticky-strike deltas should differ with non-zero skew: "
        << "sd=" << sdG.spotDelta << " ss=" << ssG.spotDelta
        << " relDiff=" << relDiff * 100.0 << "%"
        << " (min expected=" << minRelDiff * 100.0 << "%)");

    // With EUR put skew: sticky-delta < sticky-strike.
    BOOST_CHECK_MESSAGE(
        sdG.spotDelta < ssG.spotDelta,
        "with EUR put skew, sticky-delta should be < sticky-strike: "
        << "sd=" << sdG.spotDelta << " ss=" << ssG.spotDelta);

    // navva == vanna in both modes.
    BOOST_CHECK_MESSAGE(sdG.navva == sdG.vanna,
        "sticky-delta: navva must equal vanna");
    BOOST_CHECK_MESSAGE(ssG.navva == ssG.vanna,
        "sticky-strike: navva must equal vanna");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()
