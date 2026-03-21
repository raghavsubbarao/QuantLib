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

/*! \file fxsmilesections.cpp
    \brief Tests for FX smile section calibration (strike- and delta-parameterized).
*/

#include "toplevelfixture.hpp"
#include "utilities.hpp"
#include <ql/termstructures/volatility/fxsmilesection.hpp>
#include <ql/termstructures/volatility/fxsmilesectionbystrike.hpp>
#include <ql/termstructures/volatility/fxsmilesectionbydelta.hpp>
#include <ql/termstructures/volatility/fxcostsmilesection.hpp>
#include <ql/experimental/fx/deltavolquote.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/time/date.hpp>
#include <ql/settings.hpp>

using namespace QuantLib;
using namespace boost::unit_test_framework;

BOOST_FIXTURE_TEST_SUITE(QuantLibTests, TopLevelFixture)

BOOST_AUTO_TEST_SUITE(FxSmileSectionTests)

// ---------------------------------------------------------------------------
//  Shared market data used by all tests.
// ---------------------------------------------------------------------------

namespace {

    struct MarketData {
        Date todaysDate;
        Date expiryDate;
        Handle<YieldTermStructure> forDiscount;
        Handle<YieldTermStructure> domDiscount;
        Handle<Quote> spot;
        Handle<Quote> v_atm;
        Handle<Quote> v_25rr;
        Handle<Quote> v_10rr;
        Handle<Quote> v_25bf;
        Handle<Quote> v_10bf;
        std::vector<Real> deltas;
        DeltaVolQuote::DeltaType deltaType;
        DeltaVolQuote::AtmType atmType;
        fxSmileSection::FlyType flyType;

        // Derived market vols (smile-strangle convention)
        // 25D: call = atm + rr/2 + bf,  put = atm - rr/2 + bf
        // 10D: call = atm + rr/2 + bf,  put = atm - rr/2 + bf
        Real v_25c, v_25p, v_10c, v_10p;

        MarketData() {
            todaysDate = Date(2, January, 2024);
            expiryDate = Date(2, January, 2025);
            Settings::instance().evaluationDate() = todaysDate;

            forDiscount = Handle<YieldTermStructure>(
                ext::make_shared<FlatForward>(todaysDate, 0.05, Actual365Fixed()));
            domDiscount = Handle<YieldTermStructure>(
                ext::make_shared<FlatForward>(todaysDate, 0.03, Actual365Fixed()));

            spot  = makeQuoteHandle(1.7554);
            v_atm = makeQuoteHandle(0.14483);
            v_25rr = makeQuoteHandle(0.05770);
            v_10rr = makeQuoteHandle(0.101575);
            v_25bf = makeQuoteHandle(0.007425);
            v_10bf = makeQuoteHandle(0.016125);

            deltas    = {0.25, 0.1};
            deltaType = DeltaVolQuote::PaSpot;
            atmType   = DeltaVolQuote::AtmFwd;
            flyType   = fxSmileSection::SmileStrangle;

            // In smile-strangle convention the market quotes are:
            //   bf  = (call_vol + put_vol)/2 - atm
            //   rr  = call_vol - put_vol
            // => call_vol = atm + rr/2 + bf
            //    put_vol  = atm - rr/2 + bf
            Real atm = v_atm->value();
            v_25c = atm + 0.5 * v_25rr->value() + v_25bf->value();
            v_25p = atm - 0.5 * v_25rr->value() + v_25bf->value();
            v_10c = atm + 0.5 * v_10rr->value() + v_10bf->value();
            v_10p = atm - 0.5 * v_10rr->value() + v_10bf->value();
        }
    };

    // Check that a smile section reproduces the input vols to within tolerance.
    // rr_tol / bf_tol are tolerances on the RR and BF residuals respectively.
    void checkSmileSection(fxSmileSection& ss,
                           const MarketData& md,
                           Real rr25_tol,
                           Real bf25_tol,
                           Real rr10_tol,
                           Real bf10_tol) {
        // ATM: model vol at ATM strike should match input ATM vol
        Real atm_computed = ss.volByStrike(ss.atmLevel());
        Real atm_error = std::fabs(atm_computed - md.v_atm->value());
        BOOST_CHECK_MESSAGE(atm_error < 1.0e-4,
            "ATM vol mismatch: model=" << atm_computed
            << " market=" << md.v_atm->value()
            << " error=" << atm_error);

        // 25-delta risk reversal residual
        Real model_v25c = ss.volByDelta(0.25, Option::Call);
        Real model_v25p = ss.volByDelta(-0.25, Option::Put);
        Real model_rr25 = model_v25c - model_v25p;
        Real market_rr25 = md.v_25rr->value();
        Real rr25_error = std::fabs(model_rr25 - market_rr25);
        BOOST_CHECK_MESSAGE(rr25_error < rr25_tol,
            "25D risk-reversal mismatch: model=" << model_rr25
            << " market=" << market_rr25
            << " error=" << rr25_error
            << " tol=" << rr25_tol);

        // 25-delta butterfly residual
        Real model_bf25 = 0.5 * (model_v25c + model_v25p) - md.v_atm->value();
        Real market_bf25 = md.v_25bf->value();
        Real bf25_error = std::fabs(model_bf25 - market_bf25);
        BOOST_CHECK_MESSAGE(bf25_error < bf25_tol,
            "25D butterfly mismatch: model=" << model_bf25
            << " market=" << market_bf25
            << " error=" << bf25_error
            << " tol=" << bf25_tol);

        // 10-delta risk reversal residual
        Real model_v10c = ss.volByDelta(0.10, Option::Call);
        Real model_v10p = ss.volByDelta(-0.10, Option::Put);
        Real model_rr10 = model_v10c - model_v10p;
        Real market_rr10 = md.v_10rr->value();
        Real rr10_error = std::fabs(model_rr10 - market_rr10);
        BOOST_CHECK_MESSAGE(rr10_error < rr10_tol,
            "10D risk-reversal mismatch: model=" << model_rr10
            << " market=" << market_rr10
            << " error=" << rr10_error
            << " tol=" << rr10_tol);

        // 10-delta butterfly residual
        Real model_bf10 = 0.5 * (model_v10c + model_v10p) - md.v_atm->value();
        Real market_bf10 = md.v_10bf->value();
        Real bf10_error = std::fabs(model_bf10 - market_bf10);
        BOOST_CHECK_MESSAGE(bf10_error < bf10_tol,
            "10D butterfly mismatch: model=" << model_bf10
            << " market=" << market_bf10
            << " error=" << bf10_error
            << " tol=" << bf10_tol);
    }

    // Check that volByStrike and volByDelta are consistent via the
    // strikeByDelta round-trip.
    void checkStrikeDeltaConsistency(fxSmileSection& ss,
                                     const MarketData& /*md*/,
                                     Real tol = 1.0e-6) {
        std::vector<Real> testDeltas = {-0.10, -0.25, -0.40};
        for (Real delta : testDeltas) {
            Rate k = ss.strikeByDelta(delta, Option::Put);
            Volatility v_delta = ss.volByDelta(delta, Option::Put);
            Volatility v_strike = ss.volByStrike(k);
            Real error = std::fabs(v_delta - v_strike);
            BOOST_CHECK_MESSAGE(error < tol,
                "volByDelta / volByStrike inconsistency at delta=" << delta
                << ": v_delta=" << v_delta
                << " v_strike=" << v_strike
                << " error=" << error);
        }
    }

} // anonymous namespace


// ---------------------------------------------------------------------------
//  1. polynomialSmileSection (strike-parameterized, 3 params)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(testPolynomialSmileSection) {
    BOOST_TEST_MESSAGE("Testing polynomial smile section calibration...");

    MarketData md;

    polynomialSmileSection ss(md.expiryDate, md.spot, md.v_atm,
                              {md.v_25rr, md.v_10rr}, {md.v_25bf, md.v_10bf},
                              md.deltas, md.forDiscount, md.domDiscount,
                              md.deltaType, md.atmType, md.flyType,
                              Actual365Fixed());

    // 3 parameters for 5 constraints => over-determined; expect best-fit
    // Use wider tolerances than the cost models.
    checkSmileSection(ss, md, 5.0e-3, 5.0e-3, 5.0e-3, 5.0e-3);
    checkStrikeDeltaConsistency(ss, md);
}


// ---------------------------------------------------------------------------
//  2. fxSabrSmileSection (strike-parameterized, 3 params: alpha, nu, rho)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(testSabrSmileSection) {
    BOOST_TEST_MESSAGE("Testing FX SABR smile section calibration...");

    MarketData md;

    fxSabrSmileSection ss(md.expiryDate, md.spot, md.v_atm,
                          {md.v_25rr, md.v_10rr}, {md.v_25bf, md.v_10bf},
                          md.deltas, md.forDiscount, md.domDiscount,
                          md.deltaType, md.atmType, md.flyType,
                          Actual365Fixed());

    // SABR has 3 free params (alpha, nu, rho) for 5 constraints.
    checkSmileSection(ss, md, 5.0e-3, 5.0e-3, 5.0e-3, 5.0e-3);
    checkStrikeDeltaConsistency(ss, md);

    // Sanity-check parameter bounds
    BOOST_CHECK_MESSAGE(ss.alpha() > 0.0, "SABR alpha should be positive");
    BOOST_CHECK_MESSAGE(ss.nu() > 0.0, "SABR nu should be positive");
    BOOST_CHECK_MESSAGE(ss.rho() > -1.0 && ss.rho() < 1.0,
                        "SABR rho should be in (-1,1)");
}


// ---------------------------------------------------------------------------
//  3. fxSviSmileSection (strike-parameterized, 5 params: a,b,rho,m,sigma)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(testSviSmileSection) {
    BOOST_TEST_MESSAGE("Testing FX SVI smile section calibration...");

    MarketData md;

    fxSviSmileSection ss(md.expiryDate, md.spot, md.v_atm,
                         {md.v_25rr, md.v_10rr}, {md.v_25bf, md.v_10bf},
                         md.deltas, md.forDiscount, md.domDiscount,
                         md.deltaType, md.atmType, md.flyType,
                         Actual365Fixed());

    // SVI has 5 params matching 5 constraints exactly in principle.
    checkSmileSection(ss, md, 1.0e-4, 1.0e-4, 1.0e-4, 1.0e-4);
    checkStrikeDeltaConsistency(ss, md);
}


// ---------------------------------------------------------------------------
//  4. quadraticSmileSection (delta-parameterized, 3 params: a, b, c)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(testQuadraticSmileSection) {
    BOOST_TEST_MESSAGE("Testing quadratic (delta-parameterized) smile section calibration...");

    MarketData md;

    quadraticSmileSection ss(md.expiryDate, md.spot, md.v_atm,
                             {md.v_25rr, md.v_10rr}, {md.v_25bf, md.v_10bf},
                             md.deltas, md.forDiscount, md.domDiscount,
                             md.deltaType, md.atmType, md.flyType,
                             Actual365Fixed());

    // 3 params for 5 constraints => over-determined, best-fit.
    checkSmileSection(ss, md, 5.0e-3, 5.0e-3, 5.0e-3, 5.0e-3);
    checkStrikeDeltaConsistency(ss, md, 1.0e-5);
}


// ---------------------------------------------------------------------------
//  5. fxCostSmileSectionFlatDynamics
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(testCostSmileSectionFlatDynamics) {
    BOOST_TEST_MESSAGE("Testing FX cost smile section (flat dynamics) calibration...");

    MarketData md;

    fxCostSmileSectionFlatDynamics ss(md.expiryDate, md.spot, md.v_atm,
                                      {md.v_25rr, md.v_10rr}, {md.v_25bf, md.v_10bf},
                                      md.deltas, md.forDiscount, md.domDiscount,
                                      md.deltaType, md.atmType, md.flyType,
                                      Actual365Fixed(), Date(), true);

    // Cost-based models calibrate exactly; use tight tolerances.
    checkSmileSection(ss, md, 1.0e-6, 1.0e-6, 1.0e-6, 1.0e-6);
    checkStrikeDeltaConsistency(ss, md, 1.0e-5);
}


// ---------------------------------------------------------------------------
//  6. fxCostSmileSectionScaledDynamics
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(testCostSmileSectionScaledDynamics) {
    BOOST_TEST_MESSAGE("Testing FX cost smile section (scaled dynamics) calibration...");

    MarketData md;

    fxCostSmileSectionScaledDynamics ss(md.expiryDate, md.spot, md.v_atm,
                                        {md.v_25rr, md.v_10rr}, {md.v_25bf, md.v_10bf},
                                        md.deltas, md.forDiscount, md.domDiscount,
                                        md.deltaType, md.atmType, md.flyType,
                                        Actual365Fixed(), Date(), true);

    // Cost-based models calibrate exactly; use tight tolerances.
    checkSmileSection(ss, md, 1.0e-6, 1.0e-6, 1.0e-6, 1.0e-6);
    checkStrikeDeltaConsistency(ss, md, 1.0e-5);
}


// ---------------------------------------------------------------------------
//  7. DeltaVolQuote constructor path (all models)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(testDeltaVolQuoteConstructorPath) {
    BOOST_TEST_MESSAGE("Testing FX smile sections via DeltaVolQuote constructor...");

    MarketData md;

    // Build DeltaVolQuote handles: ATM + 4 wing quotes (25D and 10D)
    std::vector<Handle<DeltaVolQuote>> quotes;
    quotes.push_back(Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(
        md.v_atm, DeltaVolQuote::Fwd, 1.0, DeltaVolQuote::AtmFwd)));

    // 25D put / 25D call
    quotes.push_back(Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(
        -0.25, makeQuoteHandle(md.v_25p), 1.0, DeltaVolQuote::PaSpot)));
    quotes.push_back(Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(
        0.25, makeQuoteHandle(md.v_25c), 1.0, DeltaVolQuote::PaSpot)));

    // 10D put / 10D call
    quotes.push_back(Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(
        -0.10, makeQuoteHandle(md.v_10p), 1.0, DeltaVolQuote::PaSpot)));
    quotes.push_back(Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(
        0.10, makeQuoteHandle(md.v_10c), 1.0, DeltaVolQuote::PaSpot)));

    // --- polynomial ---
    {
        polynomialSmileSection ss(md.expiryDate, md.spot, quotes,
                                  md.forDiscount, md.domDiscount,
                                  md.deltaType, md.atmType, md.flyType,
                                  Actual365Fixed());
        Real atm_computed = ss.volByStrike(ss.atmLevel());
        BOOST_CHECK_MESSAGE(std::fabs(atm_computed - md.v_atm->value()) < 5.0e-3,
            "Polynomial (DeltaVolQuote path) ATM vol error too large");
    }

    // --- SABR ---
    {
        fxSabrSmileSection ss(md.expiryDate, md.spot, quotes,
                              md.forDiscount, md.domDiscount,
                              md.deltaType, md.atmType, md.flyType,
                              Actual365Fixed());
        Real atm_computed = ss.volByStrike(ss.atmLevel());
        BOOST_CHECK_MESSAGE(std::fabs(atm_computed - md.v_atm->value()) < 5.0e-3,
            "SABR (DeltaVolQuote path) ATM vol error too large");
    }

    // --- SVI ---
    {
        fxSviSmileSection ss(md.expiryDate, md.spot, quotes,
                             md.forDiscount, md.domDiscount,
                             md.deltaType, md.atmType, md.flyType,
                             Actual365Fixed());
        Real atm_computed = ss.volByStrike(ss.atmLevel());
        BOOST_CHECK_MESSAGE(std::fabs(atm_computed - md.v_atm->value()) < 1.0e-4,
            "SVI (DeltaVolQuote path) ATM vol error too large");
    }

    // --- quadratic ---
    {
        quadraticSmileSection ss(md.expiryDate, md.spot, quotes,
                                 md.forDiscount, md.domDiscount,
                                 md.deltaType, md.atmType, md.flyType,
                                 Actual365Fixed());
        Real atm_computed = ss.volByStrike(ss.atmLevel());
        BOOST_CHECK_MESSAGE(std::fabs(atm_computed - md.v_atm->value()) < 5.0e-3,
            "Quadratic (DeltaVolQuote path) ATM vol error too large");
    }

    // --- cost flat dynamics ---
    {
        fxCostSmileSectionFlatDynamics ss(md.expiryDate, md.spot, quotes,
                                          md.forDiscount, md.domDiscount,
                                          md.deltaType, md.atmType, md.flyType,
                                          Actual365Fixed());
        Real atm_computed = ss.volByStrike(ss.atmLevel());
        BOOST_CHECK_MESSAGE(std::fabs(atm_computed - md.v_atm->value()) < 1.0e-4,
            "CostFlatDynamics (DeltaVolQuote path) ATM vol error too large");
    }

    // --- cost scaled dynamics ---
    {
        fxCostSmileSectionScaledDynamics ss(md.expiryDate, md.spot, quotes,
                                            md.forDiscount, md.domDiscount,
                                            md.deltaType, md.atmType, md.flyType,
                                            Actual365Fixed());
        Real atm_computed = ss.volByStrike(ss.atmLevel());
        BOOST_CHECK_MESSAGE(std::fabs(atm_computed - md.v_atm->value()) < 1.0e-4,
            "CostScaledDynamics (DeltaVolQuote path) ATM vol error too large");
    }
}


// ---------------------------------------------------------------------------
//  8. Market data reactivity (observer pattern)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(testMarketDataReactivity) {
    BOOST_TEST_MESSAGE("Testing FX smile section reactivity to market data changes...");

    MarketData md;

    auto spotQuote = ext::make_shared<SimpleQuote>(md.spot->value());
    auto atmQuote  = ext::make_shared<SimpleQuote>(md.v_atm->value());
    auto rr25Quote = ext::make_shared<SimpleQuote>(md.v_25rr->value());
    auto rr10Quote = ext::make_shared<SimpleQuote>(md.v_10rr->value());
    auto bf25Quote = ext::make_shared<SimpleQuote>(md.v_25bf->value());
    auto bf10Quote = ext::make_shared<SimpleQuote>(md.v_10bf->value());

    polynomialSmileSection ss(
        md.expiryDate,
        Handle<Quote>(spotQuote),
        Handle<Quote>(atmQuote),
        {Handle<Quote>(rr25Quote), Handle<Quote>(rr10Quote)},
        {Handle<Quote>(bf25Quote), Handle<Quote>(bf10Quote)},
        md.deltas, md.forDiscount, md.domDiscount,
        md.deltaType, md.atmType, md.flyType, Actual365Fixed());

    Real atm_before = ss.atmLevel();

    // Shift the ATM vol up and verify that the smile section reacts.
    atmQuote->setValue(md.v_atm->value() + 0.01);
    Real atm_after = ss.atmLevel();

    BOOST_CHECK_MESSAGE(std::fabs(atm_after - atm_before) > 1.0e-6,
        "Smile section did not react to ATM vol change");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()
