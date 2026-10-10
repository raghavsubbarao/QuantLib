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
#include <functional>
#include <limits>
#include <string>

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
            v_25rr = makeQuoteHandle(0.02770);
            v_10rr = makeQuoteHandle(0.048752);
            v_25bf = makeQuoteHandle(0.007425);
            v_10bf = makeQuoteHandle(0.02376);

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

    // Check the fit to the market quotes.  Most models have fewer
    // parameters than the five quotes, so they cannot reproduce them; the
    // bound on the RMS error, in vol, is then a regression limit set a
    // little above the current fit.
    void checkSmileSection(const FxSmileSection& ss, Real maxError) {
        BOOST_CHECK_EQUAL(ss.calibrationResiduals().size(), Size(5));
        BOOST_CHECK_MESSAGE(ss.calibrationError() < maxError,
                            "calibration error too large: " << ss.calibrationError()
                                << " (limit " << maxError << ")");
    }

    typedef std::function<ext::shared_ptr<FxSmileSection>(const ext::shared_ptr<FxSmileQuotes>&)>
        SectionFactory;

    template <class Section>
    SectionFactory sectionFactory(const MarketData& md) {
        return [&md](const ext::shared_ptr<FxSmileQuotes>& quotes) {
            return ext::make_shared<Section>(md.expiryDate, md.spot, quotes, md.forDiscount,
                                             md.domDiscount, md.deltaType, md.atmType,
                                             Actual365Fixed(), md.settlement);
        };
    }

    // Quotes read off a calibrated smile at the market's deltas: the model
    // fits these exactly, with the parameters it already has.
    ext::shared_ptr<FxSmileQuotes> quotesFromSmile(const FxSmileSection& ss) {
        const Time t = ss.exerciseTime();
        const DeltaVolQuote::DeltaType dt = ss.deltaType();
        std::vector<Handle<DeltaVolQuote>> q;
        q.emplace_back(
            ext::make_shared<DeltaVolQuote>(makeQuoteHandle(ss.atmVol()), dt, t, ss.atmType()));
        for (Real d : {0.25, 0.10}) {
            q.emplace_back(ext::make_shared<DeltaVolQuote>(
                d, makeQuoteHandle(ss.volByDelta(d, Option::Call)), t, dt));
            q.emplace_back(ext::make_shared<DeltaVolQuote>(
                -d, makeQuoteHandle(ss.volByDelta(-d, Option::Put)), t, dt));
        }
        return ext::make_shared<FxDeltaVolQuotes>(q);
    }

    // Two sections give the same smile.
    void checkSameSmile(const std::string& name,
                        const FxSmileSection& a,
                        const FxSmileSection& b,
                        Real tolerance) {
        for (Real m : {0.8, 0.9, 1.0, 1.1, 1.2}) {
            const Rate K = m * a.forward();
            BOOST_CHECK_MESSAGE(std::fabs(a.volByStrike(K) - b.volByStrike(K)) < tolerance,
                                name << ": smiles differ at K/F = " << m << ": "
                                     << a.volByStrike(K) << " vs " << b.volByStrike(K));
        }
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

    // 3 parameters for 5 quotes
    checkSmileSection(ss, 15.0e-4);
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

    // 3 free parameters (alpha, nu, rho) for 5 quotes
    checkSmileSection(ss, 25.0e-4);
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

    // 5 parameters for 5 quotes: the fit is exact
    checkSmileSection(ss, 1.0e-6);
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

    // 3 parameters for 5 quotes; residuals in put delta coordinates
    checkSmileSection(ss, 30.0e-4);
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

    // 3 free coefficients for 5 quotes, fitted in closed form
    checkSmileSection(ss, 17.0e-4);
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

    // 3 free coefficients for 5 quotes, fitted in closed form
    checkSmileSection(ss, 35.0e-4);
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

    // These are the market quotes, in the conventions the section reads
    // them in, so every model must give the smile it gets from the RR/BF
    // quotes.
    auto deltaVolQuotes = ext::make_shared<FxDeltaVolQuotes>(quotes);
    auto compare = [&](const std::string& name, const SectionFactory& make) {
        auto fromRrBf = make(md.rrBfQuotes());
        auto fromDeltaVols = make(deltaVolQuotes);
        // the targets come in a different order, and LM stops on a relative
        // change of the objective, so the fits agree to well below 0.1bp
        // rather than to machine precision
        BOOST_CHECK_SMALL(fromRrBf->calibrationError() - fromDeltaVols->calibrationError(), 1.0e-7);
        checkSameSmile(name, *fromRrBf, *fromDeltaVols, 1.0e-5);
    };
    compare("Polynomial", sectionFactory<PolynomialSmileSection>(md));
    compare("SABR", sectionFactory<FxSabrSmileSection>(md));
    compare("SVI", sectionFactory<FxSviSmileSection>(md));
    compare("Quadratic", sectionFactory<QuadraticSmileSection>(md));
    compare("CostFlatDynamics", sectionFactory<FxCostSmileSectionFlatDynamics>(md));
    compare("CostScaledDynamics", sectionFactory<FxCostSmileSectionScaledDynamics>(md));
}


BOOST_AUTO_TEST_CASE(testCalibrationRoundTrip) {
    BOOST_TEST_MESSAGE("Testing that FX smile sections refit quotes generated by their own smile...");

    MarketData md;

    // Each model is calibrated to the market, quotes are read off its
    // smile, and a fresh section must fit them exactly and give back the
    // same smile.
    auto roundTrip = [&](const std::string& name, const SectionFactory& make) {
        auto original = make(md.rrBfQuotes());
        auto refitted = make(quotesFromSmile(*original));
        BOOST_CHECK_MESSAGE(refitted->calibrationError() < 1.0e-8,
                            name << ": quotes from its own smile not refitted: error "
                                 << refitted->calibrationError());
        checkSameSmile(name, *original, *refitted, 1.0e-6);
    };
    roundTrip("Polynomial", sectionFactory<PolynomialSmileSection>(md));
    roundTrip("SABR", sectionFactory<FxSabrSmileSection>(md));
    roundTrip("SVI", sectionFactory<FxSviSmileSection>(md));
    roundTrip("Quadratic", sectionFactory<QuadraticSmileSection>(md));
    roundTrip("CostFlatDynamics", sectionFactory<FxCostSmileSectionFlatDynamics>(md));
    roundTrip("CostScaledDynamics", sectionFactory<FxCostSmileSectionScaledDynamics>(md));
}


// ---------------------------------------------------------------------------
//  8. Market data reactivity (observer pattern)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(testAtmLevelIsTheForward) {
    BOOST_TEST_MESSAGE("Testing that an FX smile section's ATM level is the forward...");

    MarketData md;
    // under delta-neutral ATM the ATM strike is not the forward
    QuadraticSmileSection ss(md.expiryDate, md.spot, md.rrBfQuotes(), md.forDiscount,
                             md.domDiscount, md.deltaType, DeltaVolQuote::AtmDeltaNeutral,
                             Actual365Fixed(), md.settlement);
    const Real F = ss.forward();
    BOOST_CHECK_EQUAL(ss.atmLevel(), F);
    BOOST_CHECK(std::fabs(ss.atmStrike() - F) > 1.0e-4);
    BOOST_CHECK_SMALL(ss.volByStrike(ss.atmStrike()) - ss.atmVol(), 1.0e-12);

    // so SmileSection's pricing uses the right forward
    const Real sqrtT = std::sqrt(ss.exerciseTime());
    for (Real m : {0.9, 1.0, 1.1}) {
        const Rate K = m * F;
        const Option::Type type = K >= F ? Option::Call : Option::Put;
        const Real expected = BlackCalculator(type, K, F, ss.volByStrike(K) * sqrtT).value();
        BOOST_CHECK_CLOSE(ss.optionPrice(K, type), expected, 1.0e-10);
    }
}

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

    // Each input must move the smile.  Under AtmFwd the ATM strike is the
    // forward whatever the vol, so vol changes are checked on vols.
    const Real F = ss.forward();
    const Rate kPut = 0.9 * F, kCall = 1.1 * F;

    auto checkMoves = [&](SimpleQuote& quote, Real bump, const std::string& name,
                          const std::function<Real()>& result) {
        const Real before = result(), base = quote.value();
        quote.setValue(base + bump);
        const Real after = result();
        quote.setValue(base);
        BOOST_CHECK_MESSAGE(std::fabs(after - before) > 1.0e-6,
                            "smile section did not react to a change in the " << name);
        BOOST_CHECK_CLOSE(result(), before, 1.0e-8);   // and comes back
    };

    checkMoves(*atmQuote, 0.01, "ATM vol", [&] { return ss.atmVol(); });
    checkMoves(*rr25Quote, 0.01, "25D risk reversal",
               [&] { return ss.volByStrike(kCall) - ss.volByStrike(kPut); });
    checkMoves(*rr10Quote, 0.01, "10D risk reversal",
               [&] { return ss.volByStrike(kCall) - ss.volByStrike(kPut); });
    checkMoves(*bf25Quote, 0.005, "25D butterfly",
               [&] { return ss.volByStrike(kCall) + ss.volByStrike(kPut); });
    checkMoves(*bf10Quote, 0.005, "10D butterfly",
               [&] { return ss.volByStrike(kCall) + ss.volByStrike(kPut); });
    checkMoves(*spotQuote, 0.01, "spot", [&] { return ss.forward(); });
    checkMoves(*spotQuote, 0.01, "spot (ATM strike)", [&] { return ss.atmStrike(); });

    // an ATM vol bump moves the calibrated ATM vol by about as much
    const Real atmBefore = ss.atmVol();
    atmQuote->setValue(atmQuote->value() + 0.01);
    BOOST_CHECK_SMALL(ss.atmVol() - atmBefore - 0.01, 1.0e-3);
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
    // deltas must be distinct and in (0, 0.5), and every quote given
    for (Real badDelta : {0.0, 0.5, -0.25, 0.6})
        BOOST_CHECK_THROW(FxRrBfQuotes(md.v_atm, {md.v_25rr}, {md.v_25bf}, {badDelta},
                                       FxRrBfQuotes::SmileStrangle),
                          Error);
    BOOST_CHECK_THROW(FxRrBfQuotes(md.v_atm, {md.v_25rr, md.v_10rr}, {md.v_25bf, md.v_10bf},
                                   {0.25, 0.25}, FxRrBfQuotes::SmileStrangle),
                      Error);
    BOOST_CHECK_THROW(FxRrBfQuotes(Handle<Quote>(), {md.v_25rr}, {md.v_25bf}, {0.25},
                                   FxRrBfQuotes::SmileStrangle),
                      Error);
    BOOST_CHECK_THROW(FxRrBfQuotes(md.v_atm, {Handle<Quote>()}, {md.v_25bf}, {0.25},
                                   FxRrBfQuotes::SmileStrangle),
                      Error);
    BOOST_CHECK_THROW(FxRrBfQuotes(md.v_atm, {md.v_25rr}, {Handle<Quote>()}, {0.25},
                                   FxRrBfQuotes::SmileStrangle),
                      Error);

    // delta-vol quotes: at least one, none empty, and each with a delta or
    // an ATM convention
    BOOST_CHECK_THROW(FxDeltaVolQuotes({}), Error);
    BOOST_CHECK_THROW(FxDeltaVolQuotes({Handle<DeltaVolQuote>()}), Error);
    BOOST_CHECK_THROW(FxDeltaVolQuotes({Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(
                          0.0, md.v_atm, 1.0, md.deltaType))}),
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

BOOST_AUTO_TEST_CASE(testDeltaVolQuotesUseTheirOwnConventions) {
    BOOST_TEST_MESSAGE("Testing that FX delta-vol quotes are read in their own conventions...");

    MarketData md;
    // spot-delta quotes on a premium-adjusted section: three quotes for
    // the quadratic's three parameters, so the fit is exact
    const DeltaVolQuote::DeltaType quoteType = DeltaVolQuote::Spot;
    BOOST_REQUIRE(md.deltaType != quoteType);
    std::vector<Handle<DeltaVolQuote>> q = {
        Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(md.v_atm, quoteType, 1.0,
                                                              DeltaVolQuote::AtmDeltaNeutral)),
        Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(0.25, makeQuoteHandle(md.v_25c), 1.0, quoteType)),
        Handle<DeltaVolQuote>(ext::make_shared<DeltaVolQuote>(-0.25, makeQuoteHandle(md.v_25p), 1.0, quoteType))};
    QuadraticSmileSection ss(md.expiryDate, md.spot, ext::make_shared<FxDeltaVolQuotes>(q),
                             md.forDiscount, md.domDiscount, md.deltaType, md.atmType,
                             Actual365Fixed(), md.settlement);

    const Real S = md.spot->value(), sqrtT = std::sqrt(ss.exerciseTime());
    const Real ddom = ss.domesticDiscountFactor(), dfor = ss.foreignDiscountFactor();
    auto strike = [&](Option::Type type, DeltaVolQuote::DeltaType dt, Real delta, Volatility v) {
        return BlackDeltaCalculator(type, dt, S, ddom, dfor, v * sqrtT).strikeFromDelta(delta);
    };
    const Real kAtm = BlackDeltaCalculator(Option::Call, quoteType, S, ddom, dfor,
                                           md.v_atm->value() * sqrtT)
                          .atmStrike(DeltaVolQuote::AtmDeltaNeutral);
    const Real kCall = strike(Option::Call, quoteType, 0.25, md.v_25c);
    const Real kPut = strike(Option::Put, quoteType, -0.25, md.v_25p);

    // the smile passes through each quote at the strike of the quote's own convention...
    BOOST_CHECK_SMALL(ss.volByStrike(kAtm) - md.v_atm->value(), 1.0e-8);
    BOOST_CHECK_SMALL(ss.volByStrike(kCall) - md.v_25c, 1.0e-8);
    BOOST_CHECK_SMALL(ss.volByStrike(kPut) - md.v_25p, 1.0e-8);
    // ...which is not the strike the section's convention would give
    BOOST_CHECK(std::fabs(strike(Option::Call, md.deltaType, 0.25, md.v_25c) - kCall) > 1.0e-3);
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

namespace {

    // A target no smile can meet.
    class UnreachableTarget : public FxSmileTarget {
      public:
        Real residual(const FxSmileSection&) const override {
            return std::numeric_limits<Real>::quiet_NaN();
        }
    };

    class QuotesWithUnreachableTarget : public FxSmileQuotes {
      public:
        Volatility referenceVol() const override { return 0.1; }
      private:
        void calibrate(const FxSmileSection& section) const override {
            fit(section, {ext::make_shared<UnreachableTarget>()});
        }
    };

}

BOOST_AUTO_TEST_CASE(testFailedCalibrationThrows) {
    BOOST_TEST_MESSAGE("Testing that a failed FX smile calibration raises an error...");

    MarketData md;
    QuadraticSmileSection ss(md.expiryDate, md.spot, ext::make_shared<QuotesWithUnreachableTarget>(),
                             md.forDiscount, md.domDiscount, md.deltaType, md.atmType,
                             Actual365Fixed(), md.settlement);
    BOOST_CHECK_THROW(ss.volByStrike(md.spot->value()), Error);
    BOOST_CHECK_THROW(ss.calibrationError(), Error);
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
        BOOST_CHECK_SMALL(ss.atmVol() - ss.volByStrike(ss.atmStrike()), 1.0e-12);
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
    BOOST_CHECK_SMALL(ss.atmVol() - ss.volByStrike(ss.atmStrike()), 1.0e-12);

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
