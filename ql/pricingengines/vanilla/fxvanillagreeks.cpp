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

#include <ql/pricingengines/vanilla/fxvanillagreeks.hpp>
#include <ql/pricingengines/vanilla/fdblackscholesvanillaengine.hpp>
#include <ql/termstructures/volatility/equityfx/localvolsurface.hpp>
#include <ql/methods/finitedifferences/solvers/fdmbackwardsolver.hpp>
#include <ql/settings.hpp>
#include <cmath>
#include <iomanip>
#include <utility>

namespace QuantLib {

    FxVanillaBumpRisk::FxVanillaBumpRisk(ext::shared_ptr<VanillaOption> option,
                                         ext::shared_ptr<GeneralizedBlackScholesProcess> process,
                                         ext::shared_ptr<SimpleQuote> spotQuote,
                                         std::vector<ext::shared_ptr<SimpleQuote>> atmVolQuotes,
                                         Real notional,
                                         Real spotBump,
                                         Real volBump,
                                         Size tGrid,
                                         Size xGrid,
                                         Size lvTimePts,
                                         Size lvStrikePts,
                                         Real lvStrikeSpread)
    : option_(std::move(option)), process_(std::move(process)),
      spotQuote_(std::move(spotQuote)), atmVolQuotes_(std::move(atmVolQuotes)),
      notional_(notional), spotBump_(spotBump), volBump_(volBump),
      tGrid_(tGrid), xGrid_(xGrid),
      lvTimePts_(lvTimePts), lvStrikePts_(lvStrikePts), lvStrikeSpread_(lvStrikeSpread) {
        QL_REQUIRE(spotQuote_ != nullptr, "null spot quote");
        QL_REQUIRE(!atmVolQuotes_.empty(), "at least one ATM vol quote required");
        QL_REQUIRE(lvTimePts_ >= 2, "lvTimePts must be at least 2");
        QL_REQUIRE(lvStrikePts_ >= 2, "lvStrikePts must be at least 2");
        QL_REQUIRE(lvStrikeSpread_ > 0.0, "lvStrikeSpread must be positive");
    }

    ext::shared_ptr<PricingEngine>
    FxVanillaBumpRisk::makeEngineFor(
        const ext::shared_ptr<GeneralizedBlackScholesProcess>& proc,
        bool localVol) const {
        return ext::make_shared<FdBlackScholesVanillaEngine>(
            proc, tGrid_, xGrid_,
            /*dampingSteps=*/0,
            FdmSchemeDesc::Douglas(),
            localVol,
            localVol ? 0.20 : -Null<Real>());
    }

    ext::shared_ptr<PricingEngine> FxVanillaBumpRisk::makeEngine(bool localVol) const {
        return makeEngineFor(process_, localVol);
    }

    std::pair<std::vector<Time>, std::vector<Real>> FxVanillaBumpRisk::buildLvGrid() const {
        const Date expiry = option_->exercise()->lastDate();
        const Time T = process_->time(expiry);
        QL_REQUIRE(T > 0.0, "option has expired; cannot build local vol grid");

        // Average ATM vol across all pillar quotes for grid sizing.
        Real avgAtm = 0.0;
        for (const auto& q : atmVolQuotes_)
            avgAtm += q->value();
        avgAtm /= static_cast<Real>(atmVolQuotes_.size());

        // Time grid: lvTimePts_ equally-spaced points from T/lvTimePts_ to T.
        std::vector<Time> times(lvTimePts_);
        for (Size i = 0; i < lvTimePts_; ++i)
            times[i] = T * (i + 1.0) / static_cast<Real>(lvTimePts_);

        // Strike grid: lvStrikePts_ log-spaced points spanning
        //   ± lvStrikeSpread_ * avgAtm * sqrt(T)  around current spot.
        const Real S = spotQuote_->value();
        const Real halfWidth = lvStrikeSpread_ * avgAtm * std::sqrt(T);
        std::vector<Real> strikes(lvStrikePts_);
        for (Size j = 0; j < lvStrikePts_; ++j) {
            const Real u = 2.0 * j / static_cast<Real>(lvStrikePts_ - 1) - 1.0;
            strikes[j] = S * std::exp(halfWidth * u);
        }

        return {times, strikes};
    }

    ext::shared_ptr<GeneralizedBlackScholesProcess>
    FxVanillaBumpRisk::makeStickyProcess(const std::vector<Time>& times,
                                          const std::vector<Real>& strikes) const {
        auto fixedLV = buildFixedLocalVolSurface(times, strikes);
        fixedLV->enableExtrapolation();

        // Construct a GBS process using the external local vol constructor.
        // When hasExternalLocalVol_=true, process->localVolatility() returns
        // fixedLV directly, bypassing the Dupire formula.  Since FixedLocalVolSurface
        // does not observe the spot quote, local vols are frozen at fixed (t,K) points
        // even when spotQuote_ is bumped — implementing sticky-strike dynamics.
        return ext::make_shared<GeneralizedBlackScholesProcess>(
            Handle<Quote>(spotQuote_),
            process_->dividendYield(),
            process_->riskFreeRate(),
            process_->blackVolatility(),
            Handle<LocalVolTermStructure>(fixedLV));
    }

    FxVanillaGreeks FxVanillaBumpRisk::calculate(bool localVol, StickyType sticky) const {
        const Real S = spotQuote_->value();
        const Real ds = S * spotBump_;
        const Real dv = volBump_;

        // Capture ATM vol base values for later restoration.
        std::vector<Real> sigmas;
        sigmas.reserve(atmVolQuotes_.size());
        for (const auto& q : atmVolQuotes_)
            sigmas.push_back(q->value());

        auto shiftAtms = [&](Real delta) {
            for (Size i = 0; i < atmVolQuotes_.size(); ++i)
                atmVolQuotes_[i]->setValue(sigmas[i] + delta);
        };
        auto restoreAtms = [&]() {
            for (Size i = 0; i < atmVolQuotes_.size(); ++i)
                atmVolQuotes_[i]->setValue(sigmas[i]);
        };

        // Forward discount ratio: F = S * Bf/Bd  =>  dF/dS = Bf/Bd
        const Date expiry = option_->exercise()->lastDate();
        const Time T_exp = process_->time(expiry);
        const Real Bd = process_->riskFreeRate()->discount(T_exp);
        const Real Bf = process_->dividendYield()->discount(T_exp);
        const Real dFdS = Bf / Bd;

        if (!localVol || sticky == StickyType::Delta) {
            // ── Sticky-delta path (or BS mode) ────────────────────────────────
            //
            // The FX variance surface is delta-parameterised: when spot is bumped,
            // the smile moves with it in delta space.  This is the market convention.
            option_->setPricingEngine(makeEngine(localVol));

            // Base NPV
            const Real V0 = option_->NPV() * notional_;

            // Spot delta and gamma
            spotQuote_->setValue(S + ds);
            const Real Vup = option_->NPV() * notional_;

            spotQuote_->setValue(S - ds);
            const Real Vdn = option_->NPV() * notional_;

            spotQuote_->setValue(S);

            const Real spotDelta = (Vup - Vdn) / (2.0 * ds);
            const Real spotGamma = (Vup - 2.0 * V0 + Vdn) / (ds * ds);

            // Forward delta/gamma
            const Real fwdDelta = spotDelta / dFdS;
            const Real fwdGamma = spotGamma / (dFdS * dFdS);

            // Theta: shift evaluation date forward by one calendar day
            const Date today = Settings::instance().evaluationDate();
            Settings::instance().evaluationDate() = today + 1;
            const Real Vnext = option_->NPV() * notional_;
            Settings::instance().evaluationDate() = today;
            const Real theta = Vnext - V0;

            // Volga: second derivative w.r.t. parallel ATM vol shift
            shiftAtms(+dv);
            const Real Vvp = option_->NPV() * notional_;

            shiftAtms(-2.0 * dv);
            const Real Vvm = option_->NPV() * notional_;

            restoreAtms();
            const Real volga = (Vvp - 2.0 * V0 + Vvm) / (dv * dv);

            // Vanna: cross second derivative — (V++ - V+- - V-+ + V--) / (4 ds dv)
            spotQuote_->setValue(S + ds);
            shiftAtms(+dv);
            const Real Vpp = option_->NPV() * notional_;

            shiftAtms(-2.0 * dv);
            const Real Vpm = option_->NPV() * notional_;
            restoreAtms();

            spotQuote_->setValue(S - ds);
            shiftAtms(+dv);
            const Real Vmp = option_->NPV() * notional_;

            shiftAtms(-2.0 * dv);
            const Real Vmm = option_->NPV() * notional_;
            restoreAtms();

            spotQuote_->setValue(S);

            const Real vanna = (Vpp - Vpm - Vmp + Vmm) / (4.0 * ds * dv);

            FxVanillaGreeks result;
            result.npv = V0;
            result.spotDelta = spotDelta;
            result.fwdDelta = fwdDelta;
            result.spotGamma = spotGamma;
            result.fwdGamma = fwdGamma;
            result.theta = theta;
            result.vanna = vanna;
            result.volga = volga;
            return result;

        } else {
            // ── Sticky-strike path (localVol=true only) ───────────────────────
            //
            // Pre-sample the Dupire local vol on a fixed (time, strike) grid and
            // freeze it in a FixedLocalVolSurface before bumping.  Because
            // FixedLocalVolSurface does not observe the spot quote, local vols at
            // fixed (t,K) points are unchanged when spot is bumped — giving the
            // model-consistent (sticky-strike) delta.
            //
            // Three frozen surfaces are needed:
            //   stickyBase : sampled at base vols  (for NPV, delta, gamma, theta)
            //   stickyVp   : sampled at vols + dv  (for vanna and volga)
            //   stickyVm   : sampled at vols - dv  (for vanna and volga)

            auto [times, strikes] = buildLvGrid();

            auto stickyBase = makeStickyProcess(times, strikes);

            shiftAtms(+dv);
            auto stickyVp = makeStickyProcess(times, strikes);

            shiftAtms(-2.0 * dv);
            auto stickyVm = makeStickyProcess(times, strikes);

            restoreAtms();

            // Build engines once — reused across multiple spot bumps.
            auto engBase = makeEngineFor(stickyBase, true);
            auto engVp   = makeEngineFor(stickyVp,   true);
            auto engVm   = makeEngineFor(stickyVm,   true);

            // ── Base NPV ─────────────────────────────────────────────────────
            option_->setPricingEngine(engBase);
            const Real V0 = option_->NPV() * notional_;

            // ── Spot delta and gamma (frozen local vol, spot bumped) ──────────
            spotQuote_->setValue(S + ds);
            const Real Vup = option_->NPV() * notional_;

            spotQuote_->setValue(S - ds);
            const Real Vdn = option_->NPV() * notional_;

            spotQuote_->setValue(S);

            const Real spotDelta = (Vup - Vdn) / (2.0 * ds);
            const Real spotGamma = (Vup - 2.0 * V0 + Vdn) / (ds * ds);

            // Forward delta/gamma
            const Real fwdDelta = spotDelta / dFdS;
            const Real fwdGamma = spotGamma / (dFdS * dFdS);

            // ── Theta (frozen local vol, evaluation date shifted) ─────────────
            const Date today = Settings::instance().evaluationDate();
            Settings::instance().evaluationDate() = today + 1;
            const Real Vnext = option_->NPV() * notional_;
            Settings::instance().evaluationDate() = today;
            const Real theta = Vnext - V0;

            // ── Volga: second derivative w.r.t. parallel ATM vol shift ────────
            // Price using surfaces frozen at vol±dv, then apply 3-point formula.
            option_->setPricingEngine(engVp);
            const Real Vvp = option_->NPV() * notional_;

            option_->setPricingEngine(engVm);
            const Real Vvm = option_->NPV() * notional_;

            const Real volga = (Vvp - 2.0 * V0 + Vvm) / (dv * dv);

            // ── Vanna: cross second derivative — (V++ - V+- - V-+ + V--) / (4 ds dv)
            // Each scenario uses the surface frozen at the corresponding vol level.
            spotQuote_->setValue(S + ds);
            option_->setPricingEngine(engVp);
            const Real Vpp = option_->NPV() * notional_;

            option_->setPricingEngine(engVm);
            const Real Vpm = option_->NPV() * notional_;

            spotQuote_->setValue(S - ds);
            option_->setPricingEngine(engVp);
            const Real Vmp = option_->NPV() * notional_;

            option_->setPricingEngine(engVm);
            const Real Vmm = option_->NPV() * notional_;

            spotQuote_->setValue(S);

            const Real vanna = (Vpp - Vpm - Vmp + Vmm) / (4.0 * ds * dv);

            // Restore base engine so the option is left in a consistent state.
            option_->setPricingEngine(engBase);

            FxVanillaGreeks result;
            result.npv = V0;
            result.spotDelta = spotDelta;
            result.fwdDelta = fwdDelta;
            result.spotGamma = spotGamma;
            result.fwdGamma = fwdGamma;
            result.theta = theta;
            result.vanna = vanna;
            result.volga = volga;
            return result;
        }
    }

    ext::shared_ptr<FixedLocalVolSurface>
    FxVanillaBumpRisk::buildFixedLocalVolSurface(const std::vector<Time>& times,
                                                  const std::vector<Real>& strikes) const {
        QL_REQUIRE(!times.empty(), "time grid is empty");
        QL_REQUIRE(!strikes.empty(), "strike grid is empty");

        // Build the Dupire local vol surface wrapping the process's implied vol surface.
        // Mapping for FX:  riskFreeTS = domestic,  dividendTS = foreign.
        LocalVolSurface lvDupire(process_->blackVolatility(),
                                  process_->riskFreeRate(),
                                  process_->dividendYield(),
                                  process_->x0());
        lvDupire.enableExtrapolation();

        // FixedLocalVolSurface stores local vols in a matrix with
        // rows = strikes, columns = times.
        auto lvMatrix = ext::make_shared<Matrix>(strikes.size(), times.size());
        for (Size ti = 0; ti < times.size(); ++ti)
            for (Size si = 0; si < strikes.size(); ++si)
                (*lvMatrix)[si][ti] = lvDupire.localVol(times[ti], strikes[si], true);

        return ext::make_shared<FixedLocalVolSurface>(
            process_->blackVolatility()->referenceDate(),
            times,
            strikes,
            lvMatrix,
            process_->blackVolatility()->dayCounter());
    }

    void FxVanillaBumpRisk::printLocalVolComparison(const std::vector<Time>& times,
                                                     const std::vector<Real>& strikes,
                                                     std::ostream& out) const {
        LocalVolSurface lvDupire(process_->blackVolatility(),
                                  process_->riskFreeRate(),
                                  process_->dividendYield(),
                                  process_->x0());
        lvDupire.enableExtrapolation();

        auto fixedLV = buildFixedLocalVolSurface(times, strikes);
        fixedLV->enableExtrapolation();

        const int wt = 10; // column width for time headers
        const int wv = 9;  // column width for vol values
        const int wk = 8;  // column width for strike labels

        out << "\n--- Local vol surface: Dupire (D) vs FixedLocalVolSurface (F) ---\n";
        out << std::setw(wk) << "K\\t";
        for (Time t : times)
            out << std::setw(wt) << std::fixed << std::setprecision(2) << t;
        out << "\n" << std::string(wk + wt * times.size(), '-') << "\n";

        for (Real K : strikes) {
            // Dupire row
            out << std::setw(wk - 1) << std::fixed << std::setprecision(4) << K << "D";
            for (Time t : times)
                out << std::setw(wv) << std::fixed << std::setprecision(2)
                    << lvDupire.localVol(t, K, true) * 100.0;
            out << "\n";

            // FixedLocalVolSurface row
            out << std::setw(wk - 1) << std::fixed << std::setprecision(4) << K << "F";
            for (Time t : times)
                out << std::setw(wv) << std::fixed << std::setprecision(2)
                    << fixedLV->localVol(t, K, true) * 100.0;
            out << "\n\n";
        }
    }

}
