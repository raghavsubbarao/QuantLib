/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2025 Raghav Subbarao

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

/*! \file fxsmilesections.cpp
    \brief Tests for FX smile section calibration (strike- and delta-parameterized).
*/

#include "toplevelfixture.hpp"
#include "utilities.hpp"
#include <ql/experimental/fxslv/fxsettlementconvention.hpp>
#include <ql/experimental/fxslv/fxsmilequotes.hpp>
#include <ql/experimental/fxslv/fxsmilesection.hpp>
#include <ql/experimental/fxslv/fxsmilesectionbystrike.hpp>
#include <ql/experimental/fxslv/fxsmilesectionbydelta.hpp>
#include <ql/experimental/fxslv/fxcostsmilesection.hpp>
#include <ql/math/distributions/normaldistribution.hpp>
#include <ql/pricingengines/blackcalculator.hpp>
#include <ql/pricingengines/blackdeltacalculator.hpp>
#include <ql/quotes/deltavolquote.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/weekendsonly.hpp>
#include <ql/time/daycounters/actual360.hpp>
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
        FxRrBfQuotes::FlyType flyType;
        FxSettlementConvention settlement{WeekendsOnly(), 2};

        ext::shared_ptr<FxSmileQuotes> rrBfQuotes() const {
            return ext::make_shared<FxRrBfQuotes>(
                v_atm, std::vector<Handle<Quote>>{v_25rr, v_10rr},
                std::vector<Handle<Quote>>{v_25bf, v_10bf}, deltas, flyType);
        }

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
            flyType   = FxRrBfQuotes::SmileStrangle;

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
    void checkSmileSection(FxSmileSection& ss,
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
    void checkStrikeDeltaConsistency(FxSmileSection& ss,
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

    PolynomialSmileSection ss(md.expiryDate, md.spot, md.rrBfQuotes(), md.forDiscount, md.domDiscount,
                              md.deltaType, md.atmType,
                              Actual365Fixed(), md.settlement);

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

    FxSabrSmileSection ss(md.expiryDate, md.spot, md.rrBfQuotes(), md.forDiscount, md.domDiscount,
                          md.deltaType, md.atmType,
                          Actual365Fixed(), md.settlement);

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

    FxSviSmileSection ss(md.expiryDate, md.spot, md.rrBfQuotes(), md.forDiscount, md.domDiscount,
                         md.deltaType, md.atmType,
                         Actual365Fixed(), md.settlement);

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

    QuadraticSmileSection ss(md.expiryDate, md.spot, md.rrBfQuotes(), md.forDiscount, md.domDiscount,
                             md.deltaType, md.atmType,
                             Actual365Fixed(), md.settlement);

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

    FxCostSmileSectionFlatDynamics ss(md.expiryDate, md.spot, md.rrBfQuotes(), md.forDiscount, md.domDiscount,
                                      md.deltaType, md.atmType,
                                      Actual365Fixed(), md.settlement, Date(), true);

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

    FxCostSmileSectionScaledDynamics ss(md.expiryDate, md.spot, md.rrBfQuotes(), md.forDiscount, md.domDiscount,
                                        md.deltaType, md.atmType,
                                        Actual365Fixed(), md.settlement, Date(), true);

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
        PolynomialSmileSection ss(md.expiryDate, md.spot, ext::make_shared<FxDeltaVolQuotes>(quotes),
                                  md.forDiscount, md.domDiscount,
                                  md.deltaType, md.atmType,
                                  Actual365Fixed(), md.settlement);
        Real atm_computed = ss.volByStrike(ss.atmLevel());
        BOOST_CHECK_MESSAGE(std::fabs(atm_computed - md.v_atm->value()) < 5.0e-3,
            "Polynomial (DeltaVolQuote path) ATM vol error too large");
    }

    // --- SABR ---
    {
        FxSabrSmileSection ss(md.expiryDate, md.spot, ext::make_shared<FxDeltaVolQuotes>(quotes),
                              md.forDiscount, md.domDiscount,
                              md.deltaType, md.atmType,
                              Actual365Fixed(), md.settlement);
        Real atm_computed = ss.volByStrike(ss.atmLevel());
        BOOST_CHECK_MESSAGE(std::fabs(atm_computed - md.v_atm->value()) < 5.0e-3,
            "SABR (DeltaVolQuote path) ATM vol error too large");
    }

    // --- SVI ---
    {
        FxSviSmileSection ss(md.expiryDate, md.spot, ext::make_shared<FxDeltaVolQuotes>(quotes),
                             md.forDiscount, md.domDiscount,
                             md.deltaType, md.atmType,
                             Actual365Fixed(), md.settlement);
        Real atm_computed = ss.volByStrike(ss.atmLevel());
        BOOST_CHECK_MESSAGE(std::fabs(atm_computed - md.v_atm->value()) < 1.0e-4,
            "SVI (DeltaVolQuote path) ATM vol error too large");
    }

    // --- quadratic ---
    {
        QuadraticSmileSection ss(md.expiryDate, md.spot, ext::make_shared<FxDeltaVolQuotes>(quotes),
                                 md.forDiscount, md.domDiscount,
                                 md.deltaType, md.atmType,
                                 Actual365Fixed(), md.settlement);
        Real atm_computed = ss.volByStrike(ss.atmLevel());
        BOOST_CHECK_MESSAGE(std::fabs(atm_computed - md.v_atm->value()) < 5.0e-3,
            "Quadratic (DeltaVolQuote path) ATM vol error too large");
    }

    // --- cost flat dynamics ---
    {
        FxCostSmileSectionFlatDynamics ss(md.expiryDate, md.spot, ext::make_shared<FxDeltaVolQuotes>(quotes),
                                          md.forDiscount, md.domDiscount,
                                          md.deltaType, md.atmType,
                                          Actual365Fixed(), md.settlement);
        Real atm_computed = ss.volByStrike(ss.atmLevel());
        BOOST_CHECK_MESSAGE(std::fabs(atm_computed - md.v_atm->value()) < 1.0e-4,
            "CostFlatDynamics (DeltaVolQuote path) ATM vol error too large");
    }

    // --- cost scaled dynamics ---
    {
        FxCostSmileSectionScaledDynamics ss(md.expiryDate, md.spot, ext::make_shared<FxDeltaVolQuotes>(quotes),
                                            md.forDiscount, md.domDiscount,
                                            md.deltaType, md.atmType,
                                            Actual365Fixed(), md.settlement);
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

    PolynomialSmileSection ss(
        md.expiryDate,
        Handle<Quote>(spotQuote),
        ext::make_shared<FxRrBfQuotes>(
            Handle<Quote>(atmQuote),
            std::vector<Handle<Quote>>{Handle<Quote>(rr25Quote), Handle<Quote>(rr10Quote)},
            std::vector<Handle<Quote>>{Handle<Quote>(bf25Quote), Handle<Quote>(bf10Quote)},
            md.deltas, md.flyType),
        md.forDiscount, md.domDiscount,
        md.deltaType, md.atmType, Actual365Fixed(), md.settlement);

    Real atm_before = ss.atmLevel();

    // Shift the ATM vol up and verify that the smile section reacts.
    atmQuote->setValue(md.v_atm->value() + 0.01);
    Real atm_after = ss.atmLevel();

    BOOST_CHECK_MESSAGE(std::fabs(atm_after - atm_before) > 1.0e-6,
        "Smile section did not react to ATM vol change");
}

namespace {

    // Section used by the settlement tests: the quadratic is the cheapest
    // model to calibrate, and the forward does not depend on the model.
    ext::shared_ptr<FxSmileSection>
    dateModeSection(const MarketData& md,
                    const Handle<YieldTermStructure>& forDiscount,
                    const Handle<YieldTermStructure>& domDiscount,
                    const Date& referenceDate = Date()) {
        return ext::make_shared<QuadraticSmileSection>(
            md.expiryDate, md.spot, md.rrBfQuotes(),
            forDiscount, domDiscount, md.deltaType, md.atmType,
            Actual365Fixed(), md.settlement, referenceDate);
    }

    Real expectedForward(Real spot,
                         const Handle<YieldTermStructure>& forDiscount,
                         const Handle<YieldTermStructure>& domDiscount,
                         const Date& spotDate,
                         const Date& deliveryDate) {
        return spot * (forDiscount->discount(deliveryDate) / forDiscount->discount(spotDate)) /
               (domDiscount->discount(deliveryDate) / domDiscount->discount(spotDate));
    }

}

BOOST_AUTO_TEST_CASE(testSettlementConventionDates) {
    BOOST_TEST_MESSAGE("Testing FX spot and delivery dates...");

    FxSettlementConvention conv(WeekendsOnly(), 2);
    // Tuesday -> Thursday
    BOOST_CHECK_EQUAL(conv.spotDate(Date(2, January, 2024)), Date(4, January, 2024));
    // Thursday -> Monday: the lag skips the weekend
    BOOST_CHECK_EQUAL(conv.deliveryDate(Date(2, January, 2025)), Date(6, January, 2025));
    // T+1 pairs
    BOOST_CHECK_EQUAL(FxSettlementConvention(WeekendsOnly(), 1).spotDate(Date(5, January, 2024)),
                      Date(8, January, 2024));

    BOOST_CHECK_THROW(FxSettlementConvention(Calendar(), 2), Error);
}

BOOST_AUTO_TEST_CASE(testDateModeForwardUsesSpotToDelivery) {
    BOOST_TEST_MESSAGE("Testing that date-based FX smile sections discount from spot to delivery...");

    MarketData md;
    auto ss = dateModeSection(md, md.forDiscount, md.domDiscount);

    const Date spotDate(4, January, 2024), deliveryDate(6, January, 2025);
    BOOST_CHECK_EQUAL(ss->spotDate(), spotDate);
    BOOST_CHECK_EQUAL(ss->deliveryDate(), deliveryDate);

    const Real expected =
        expectedForward(md.spot->value(), md.forDiscount, md.domDiscount, spotDate, deliveryDate);
    BOOST_CHECK_CLOSE(ss->forward(), expected, 1.0e-12);

    // vol time still runs from the trade date to the expiry date
    BOOST_CHECK_CLOSE(ss->exerciseTime(),
                      Actual365Fixed().yearFraction(md.todaysDate, md.expiryDate), 1.0e-12);

    // the spot-to-delivery forward differs from the naive one to the expiry time
    const Real naive = md.spot->value() * md.forDiscount->discount(ss->exerciseTime()) /
                       md.domDiscount->discount(ss->exerciseTime());
    BOOST_CHECK(std::fabs(ss->forward() - naive) > 1.0e-8);
}

BOOST_AUTO_TEST_CASE(testDateModeIgnoresCurveReferenceDate) {
    BOOST_TEST_MESSAGE("Testing that date-based FX smile sections do not depend on the curves' "
                       "reference dates...");

    MarketData md;
    const Date spotDate = md.settlement.spotDate(md.todaysDate);
    // same continuously-compounded rates, but curves starting on the spot date
    Handle<YieldTermStructure> forFromSpot(
        ext::make_shared<FlatForward>(spotDate, 0.05, Actual365Fixed()));
    Handle<YieldTermStructure> domFromSpot(
        ext::make_shared<FlatForward>(spotDate, 0.03, Actual365Fixed()));

    auto fromToday = dateModeSection(md, md.forDiscount, md.domDiscount);
    auto fromSpot = dateModeSection(md, forFromSpot, domFromSpot);
    BOOST_CHECK_CLOSE(fromToday->forward(), fromSpot->forward(), 1.0e-12);
    BOOST_CHECK_CLOSE(fromToday->foreignDiscountFactor(), fromSpot->foreignDiscountFactor(), 1.0e-12);
    BOOST_CHECK_CLOSE(fromToday->domesticDiscountFactor(), fromSpot->domesticDiscountFactor(), 1.0e-12);

    // a curve starting after the spot date cannot give P(spot, delivery)
    Handle<YieldTermStructure> late(
        ext::make_shared<FlatForward>(spotDate + 1, 0.05, Actual365Fixed()));
    BOOST_CHECK_THROW(dateModeSection(md, late, md.domDiscount)->forward(), Error);
}

BOOST_AUTO_TEST_CASE(testDateModeFloatsWithEvaluationDate) {
    BOOST_TEST_MESSAGE("Testing that date-based FX smile sections float with the evaluation date...");

    MarketData md;
    auto floating = dateModeSection(md, md.forDiscount, md.domDiscount);
    auto fixed = dateModeSection(md, md.forDiscount, md.domDiscount, md.todaysDate);
    const Real fixedForward = fixed->forward();
    BOOST_CHECK_EQUAL(floating->spotDate(), Date(4, January, 2024));

    // move from Tuesday to Wednesday: the new spot date is Friday
    Settings::instance().evaluationDate() = Date(3, January, 2024);
    BOOST_CHECK_EQUAL(floating->spotDate(), Date(5, January, 2024));
    BOOST_CHECK_EQUAL(floating->deliveryDate(), Date(6, January, 2025));
    BOOST_CHECK_CLOSE(floating->forward(),
                      expectedForward(md.spot->value(), md.forDiscount, md.domDiscount,
                                      Date(5, January, 2024), Date(6, January, 2025)),
                      1.0e-12);

    // a section with an explicit reference date stays where it was
    BOOST_CHECK_EQUAL(fixed->spotDate(), Date(4, January, 2024));
    BOOST_CHECK_CLOSE(fixed->forward(), fixedForward, 1.0e-12);
}

BOOST_AUTO_TEST_CASE(testTimeModeForwardAndConsistencyChecks) {
    BOOST_TEST_MESSAGE("Testing time-based FX smile sections...");

    MarketData md;
    const Time tau = 1.0;
    auto section = [&](const Handle<YieldTermStructure>& forDiscount,
                       const Handle<YieldTermStructure>& domDiscount,
                       const DayCounter& dc) {
        return ext::make_shared<QuadraticSmileSection>(
            tau, md.spot, md.rrBfQuotes(), forDiscount,
            domDiscount, md.deltaType, md.atmType, dc);
    };

    auto ss = section(md.forDiscount, md.domDiscount, Actual365Fixed());
    BOOST_CHECK_CLOSE(ss->forward(),
                      md.spot->value() * md.forDiscount->discount(tau) /
                          md.domDiscount->discount(tau),
                      1.0e-12);
    BOOST_CHECK(!ss->settleConvention());
    BOOST_CHECK_THROW(ss->spotDate(), Error);

    // an empty day counter is fine in time mode
    BOOST_CHECK_NO_THROW(section(md.forDiscount, md.domDiscount, DayCounter())->forward());

    // the curves must share a time axis...
    Handle<YieldTermStructure> act360(
        ext::make_shared<FlatForward>(md.todaysDate, 0.03, Actual360()));
    BOOST_CHECK_THROW(section(md.forDiscount, act360, Actual365Fixed())->forward(), Error);
    Handle<YieldTermStructure> shifted(
        ext::make_shared<FlatForward>(md.todaysDate + 1, 0.03, Actual365Fixed()));
    BOOST_CHECK_THROW(section(md.forDiscount, shifted, Actual365Fixed())->forward(), Error);
    // ...but the section's own day counter plays no part in time mode
    BOOST_CHECK_NO_THROW(section(md.forDiscount, md.domDiscount, Actual360())->forward());
}

BOOST_AUTO_TEST_CASE(testSmileQuotesValidationAndNotification) {
    BOOST_TEST_MESSAGE("Testing FX smile quotes validation and change notification...");

    MarketData md;

    // risk reversals, butterflies and deltas must line up
    BOOST_CHECK_THROW(FxRrBfQuotes(md.v_atm, {md.v_25rr}, {md.v_25bf, md.v_10bf}, md.deltas,
                                   FxRrBfQuotes::SmileStrangle),
                      Error);
    BOOST_CHECK_THROW(FxRrBfQuotes(md.v_atm, {md.v_25rr, md.v_10rr}, {md.v_25bf}, md.deltas,
                                   FxRrBfQuotes::SmileStrangle),
                      Error);

    // the section is notified through the quotes object, which may be
    // shared by several sections
    auto rr25 = ext::make_shared<SimpleQuote>(md.v_25rr->value());
    auto quotes = ext::make_shared<FxRrBfQuotes>(
        md.v_atm, std::vector<Handle<Quote>>{Handle<Quote>(rr25), md.v_10rr},
        std::vector<Handle<Quote>>{md.v_25bf, md.v_10bf}, md.deltas, md.flyType);
    BOOST_CHECK_CLOSE(quotes->referenceVol(), md.v_atm->value(), 1.0e-12);

    QuadraticSmileSection first(md.expiryDate, md.spot, quotes, md.forDiscount, md.domDiscount,
                                md.deltaType, md.atmType, Actual365Fixed(), md.settlement);
    QuadraticSmileSection second(1.0, md.spot, quotes, md.forDiscount, md.domDiscount,
                                 md.deltaType, md.atmType, Actual365Fixed());
    const Real K = 1.05 * first.forward();
    const Real v1 = first.volByStrike(K), v2 = second.volByStrike(K);

    rr25->setValue(rr25->value() + 0.01);
    BOOST_CHECK(std::fabs(first.volByStrike(K) - v1) > 1.0e-6);
    BOOST_CHECK(std::fabs(second.volByStrike(K) - v2) > 1.0e-6);

    BOOST_CHECK_THROW(QuadraticSmileSection(md.expiryDate, md.spot,
                                            ext::shared_ptr<FxSmileQuotes>(), md.forDiscount,
                                            md.domDiscount, md.deltaType, md.atmType,
                                            Actual365Fixed(), md.settlement),
                      Error);
}

BOOST_AUTO_TEST_CASE(testCalibrationDoesNotDependOnHistory) {
    BOOST_TEST_MESSAGE("Testing that the same FX quotes always give the same smile...");

    MarketData md;
    auto spot = ext::make_shared<SimpleQuote>(md.spot->value());
    std::vector<Handle<DeltaVolQuote>> q = {
        Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(md.v_atm, md.deltaType, 1.0, md.atmType)),
        Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(0.25, makeQuoteHandle(md.v_25c), 1.0, md.deltaType)),
        Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(-0.25, makeQuoteHandle(md.v_25p), 1.0, md.deltaType)),
        Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(0.10, makeQuoteHandle(md.v_10c), 1.0, md.deltaType)),
        Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(-0.10, makeQuoteHandle(md.v_10p), 1.0, md.deltaType))};

    // the polynomial seeds and scales with the starting ATM vol, so it
    // used to drift when that seed was the previous calibration's ATM
    PolynomialSmileSection ss(md.expiryDate, Handle<Quote>(spot),
                              ext::make_shared<FxDeltaVolQuotes>(q), md.forDiscount,
                              md.domDiscount, md.deltaType, md.atmType, Actual365Fixed(),
                              md.settlement);
    const Real K1 = 1.60, K2 = 1.90;
    const Real first1 = ss.volByStrike(K1), first2 = ss.volByStrike(K2);

    for (int i = 0; i < 3; ++i) {
        spot->setValue(1.80);
        ss.volByStrike(K1); // recalibrate elsewhere...
        spot->setValue(md.spot->value());
        // ...and back to the original quotes
        BOOST_CHECK_CLOSE(ss.volByStrike(K1), first1, 1.0e-10);
        BOOST_CHECK_CLOSE(ss.volByStrike(K2), first2, 1.0e-10);
    }
}

namespace {

    // Quote objects that break the calibration contract.
    class QuotesThatNeverFit : public FxSmileQuotes {
      public:
        Volatility referenceVol() const override { return 0.1; }
      private:
        void calibrate(const FxSmileSection&) const override {}
    };

    class QuotesThatFitLater : public FxSmileQuotes {
      public:
        explicit QuotesThatFitLater(std::vector<Handle<DeltaVolQuote>> quotes)
        : quotes_(std::move(quotes)) {}
        Volatility referenceVol() const override { return 0.1; }
        void fitAgain() const { fit(*section_, targets(*section_)); }
      private:
        void calibrate(const FxSmileSection& section) const override {
            section_ = &section;
            fit(section, targets(section));
        }
        FxSmileTargets targets(const FxSmileSection& section) const {
            FxSmileTargets t;
            for (const auto& q : quotes_)
                t.push_back(ext::make_shared<FxDeltaVolTarget>(q));
            return t;
        }
        std::vector<Handle<DeltaVolQuote>> quotes_;
        mutable const FxSmileSection* section_ = nullptr;
    };

}

BOOST_AUTO_TEST_CASE(testSmileQuotesMustFitDuringCalibration) {
    BOOST_TEST_MESSAGE("Testing that FX smile quotes can only fit a section while calibrating it...");

    MarketData md;

    QuadraticSmileSection unfitted(md.expiryDate, md.spot,
                                   ext::make_shared<QuotesThatNeverFit>(), md.forDiscount,
                                   md.domDiscount, md.deltaType, md.atmType, Actual365Fixed(),
                                   md.settlement);
    BOOST_CHECK_THROW(unfitted.volByStrike(md.spot->value()), Error);

    std::vector<Handle<DeltaVolQuote>> q = {
        Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(md.v_atm, md.deltaType, 1.0, md.atmType)),
        Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(0.25, makeQuoteHandle(md.v_25c), 1.0, md.deltaType)),
        Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(-0.25, makeQuoteHandle(md.v_25p), 1.0, md.deltaType))};
    auto later = ext::make_shared<QuotesThatFitLater>(q);
    QuadraticSmileSection ss(md.expiryDate, md.spot, later, md.forDiscount, md.domDiscount,
                             md.deltaType, md.atmType, Actual365Fixed(), md.settlement);
    const Real K = 1.05 * ss.forward(), v = ss.volByStrike(K);
    BOOST_CHECK_THROW(later->fitAgain(), Error);   // outside calibration: rejected...
    BOOST_CHECK_EQUAL(ss.volByStrike(K), v);       // ...and the smile is untouched
}

BOOST_AUTO_TEST_CASE(testMarketStrangleCalibration) {
    BOOST_TEST_MESSAGE("Testing joint FX smile calibration to broker (market) strangles...");

    MarketData md;
    const DeltaVolQuote::DeltaType dt = DeltaVolQuote::Spot;
    const DeltaVolQuote::AtmType at = DeltaVolQuote::AtmDeltaNeutral;
    const Real delta = 0.25;

    // One delta level: ATM, risk reversal and broker strangle are three
    // targets for the quadratic's three parameters, so the joint fit is
    // exact and each condition can be checked on its own.
    for (Real flyScale : {1.0, 0.0}) {   // quoted fly, and a zero (flat-wing) fly
        const Volatility atm = md.v_atm->value(), rr = md.v_25rr->value();
        const Volatility bf = flyScale * md.v_25bf->value();
        QuadraticSmileSection ss(md.expiryDate, md.spot,
                                 ext::make_shared<FxRrBfQuotes>(
                                     md.v_atm, std::vector<Handle<Quote>>{md.v_25rr},
                                     std::vector<Handle<Quote>>{makeQuoteHandle(bf)},
                                     std::vector<Real>{delta}, FxRrBfQuotes::MarketStrangle),
                                 md.forDiscount, md.domDiscount, dt, at, Actual365Fixed(),
                                 md.settlement);

        const Time tau = ss.exerciseTime();
        const Real F = ss.forward(), S = md.spot->value();
        const Real ddom = ss.domesticDiscountFactor(), dfor = ss.foreignDiscountFactor();

        // the broker strangle, struck and priced at atm + bf, is repriced by the smile
        const Real w = (atm + bf) * std::sqrt(tau);
        const Real Kc = BlackDeltaCalculator(Option::Call, dt, S, ddom, dfor, w).strikeFromDelta(delta);
        const Real Kp = BlackDeltaCalculator(Option::Put, dt, S, ddom, dfor, w).strikeFromDelta(-delta);
        const BlackCalculator mc(Option::Call, Kc, F, w), mp(Option::Put, Kp, F, w);
        const Real smile =
            BlackCalculator(Option::Call, Kc, F, ss.volByStrike(Kc) * std::sqrt(tau)).value() +
            BlackCalculator(Option::Put, Kp, F, ss.volByStrike(Kp) * std::sqrt(tau)).value();
        BOOST_CHECK_SMALL((smile - mc.value() - mp.value()) / (mc.vega(tau) + mp.vega(tau)), 1.0e-8);

        // the risk reversal holds at the smile's own deltas
        BOOST_CHECK_SMALL(ss.volByDelta(delta, Option::Call) - ss.volByDelta(-delta, Option::Put) - rr,
                          1.0e-8);

        // the ATM quote holds, and atmVol() lies on the smile
        BOOST_CHECK_SMALL(ss.atmVol() - atm, 1.0e-8);
        BOOST_CHECK_SMALL(ss.atmVol() - ss.volByStrike(ss.atmLevel()), 1.0e-12);
    }

    // Two delta levels: five targets for three parameters, fitted jointly
    // in the least-squares sense; the ATM convention still holds.
    QuadraticSmileSection ss(md.expiryDate, md.spot,
                             ext::make_shared<FxRrBfQuotes>(
                                 md.v_atm, std::vector<Handle<Quote>>{md.v_25rr, md.v_10rr},
                                 std::vector<Handle<Quote>>{md.v_25bf, md.v_10bf}, md.deltas,
                                 FxRrBfQuotes::MarketStrangle),
                             md.forDiscount, md.domDiscount, dt, at, Actual365Fixed(),
                             md.settlement);
    BOOST_CHECK_SMALL(ss.atmVol() - ss.volByStrike(ss.atmLevel()), 1.0e-12);

    // cost models have a closed form for points on the smile only
    FxCostSmileSectionFlatDynamics cost(
        md.expiryDate, md.spot,
        ext::make_shared<FxRrBfQuotes>(md.v_atm, std::vector<Handle<Quote>>{md.v_25rr},
                                       std::vector<Handle<Quote>>{md.v_25bf},
                                       std::vector<Real>{delta}, FxRrBfQuotes::MarketStrangle),
        md.forDiscount, md.domDiscount, dt, at, Actual365Fixed(), md.settlement);
    BOOST_CHECK_THROW(cost.volByStrike(md.spot->value()), Error);
}

BOOST_AUTO_TEST_CASE(testProbabilitySpaceFunctions) {
    BOOST_TEST_MESSAGE("Testing FX smile normed call prices and exercise probabilities...");

    MarketData md;
    const DeltaVolQuote::DeltaType dt = DeltaVolQuote::Spot;
    const DeltaVolQuote::AtmType at = DeltaVolQuote::AtmDeltaNeutral;

    // flat smile: closed forms are Black's
    std::vector<Handle<Quote>> zero = {makeQuoteHandle(0.0), makeQuoteHandle(0.0)};
    QuadraticSmileSection flat(md.expiryDate, md.spot,
                               ext::make_shared<FxRrBfQuotes>(md.v_atm, zero, zero, md.deltas,
                                                              FxRrBfQuotes::SmileStrangle),
                               md.forDiscount, md.domDiscount, dt, at, Actual365Fixed(),
                               md.settlement);
    const Real w = md.v_atm->value() * std::sqrt(flat.exerciseTime());
    CumulativeNormalDistribution N;
    for (Real k = 0.7; k <= 1.4001; k += 0.05) {
        const Real d2 = (-std::log(k) - 0.5 * w * w) / w;
        BOOST_CHECK_SMALL(flat.exerciseProbability(k) - N(d2), 1.0e-10);
        BOOST_CHECK_SMALL(flat.normedCallPrice(k) -
                              BlackCalculator(Option::Call, k, 1.0, w).value(),
                          1.0e-12);
    }

    // a skewed smile: the inverse recovers the moneyness
    QuadraticSmileSection ss(md.expiryDate, md.spot, md.rrBfQuotes(), md.forDiscount,
                             md.domDiscount, dt, at, Actual365Fixed(), md.settlement);
    for (Real p : {0.02, 0.1, 0.25, 0.5, 0.75, 0.9, 0.98}) {
        const Real k = ss.moneynessFromProbability(p);
        BOOST_CHECK_SMALL(ss.exerciseProbability(k) - p, 1.0e-10);
    }

    BOOST_CHECK_THROW(ss.moneynessFromProbability(0.0), Error);
    BOOST_CHECK_THROW(ss.moneynessFromProbability(1.0), Error);
    BOOST_CHECK_THROW(ss.exerciseProbability(0.0), Error);
    BOOST_CHECK_THROW(ss.normedCallPrice(-1.0), Error);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()
