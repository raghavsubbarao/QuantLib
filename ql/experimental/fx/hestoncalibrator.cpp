/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2026 Quantitative Finance Team

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

#include <ql/experimental/fx/hestoncalibrator.hpp>
#include <ql/math/optimization/levenbergmarquardt.hpp>
#include <ql/processes/hestonprocess.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/equityfx/hestonblackvolsurface.hpp>
#include <utility>

namespace QuantLib {

    HestonCalibrator::HestonCalibrator(
        HestonParams initialParams,
        EndCriteria endCriteria,
        BlackCalibrationHelper::CalibrationErrorType errorType,
        std::vector<bool> fixParameters)
    : initialParams_(initialParams), endCriteria_(endCriteria),
      errorType_(errorType), fixParameters_(std::move(fixParameters)) {}

    void HestonCalibrator::calibrate(const Handle<Quote>& spot,
                                     const Handle<YieldTermStructure>& domesticTS,
                                     const Handle<YieldTermStructure>& foreignTS,
                                     const Handle<BlackVolTermStructure>& volSurface,
                                     const Calendar& calendar,
                                     const std::vector<Pillar>& pillars) {
        QL_REQUIRE(!pillars.empty(), "HestonCalibrator: at least one calibration pillar required");

        // Build the Heston process and model with initial parameters.
        const Date refDate = domesticTS->referenceDate();

        auto process = ext::make_shared<HestonProcess>(
            domesticTS, foreignTS, spot,
            initialParams_.v0, initialParams_.kappa,
            initialParams_.theta, initialParams_.sigma, initialParams_.rho);

        hestonModel_ = ext::make_shared<HestonModel>(process);

        // Attach an analytic engine to the model for fast calibration.
        auto engine = ext::make_shared<AnalyticHestonEngine>(hestonModel_);

        // Build one HestonModelHelper per pillar.
        helpers_.clear();
        helpers_.reserve(pillars.size());

        for (const auto& p : pillars) {
            // Query the market vol surface for the calibration vol at this pillar.
            const Date expiryDate = calendar.advance(refDate, p.expiry);
            const Volatility marketVol =
                volSurface->blackVol(expiryDate, p.strike, true);

            auto volQuote = ext::make_shared<SimpleQuote>(marketVol);

            auto helper = ext::make_shared<HestonModelHelper>(
                p.expiry, calendar,
                spot, p.strike,
                Handle<Quote>(volQuote),
                domesticTS, foreignTS,
                errorType_);

            helper->setPricingEngine(engine);
            helpers_.push_back(helper);
        }

        // Calibrate using Levenberg-Marquardt with the Feller constraint.
        LevenbergMarquardt optimiser;
        hestonModel_->calibrate(
            std::vector<ext::shared_ptr<CalibrationHelper>>(helpers_.begin(),
                                                            helpers_.end()),
            optimiser,
            endCriteria_,
            HestonModel::FellerConstraint(),
            {},
            fixParameters_);

        // Collect per-pillar implied-vol errors.
        calibrationErrors_.clear();
        calibrationErrors_.reserve(helpers_.size());
        for (const auto& h : helpers_) {
            // Signed error: model implied vol minus market implied vol.
            const Real modelVol = h->impliedVolatility(h->modelValue(),
                                                       1.0e-4, 1000, 0.001, 10.0);
            const Real mktVol   = h->volatility()->value();
            calibrationErrors_.push_back(modelVol - mktVol);
        }

        // Build the implied vol surface backed by the calibrated model.
        impliedVolSurface_ =
            ext::make_shared<HestonBlackVolSurface>(Handle<HestonModel>(hestonModel_));
    }

    ext::shared_ptr<CalibratedModel> HestonCalibrator::calibratedModel() const {
        QL_REQUIRE(hestonModel_, "HestonCalibrator: calibrate() has not been called");
        return hestonModel_;
    }

    std::vector<Real> HestonCalibrator::calibrationErrors() const {
        return calibrationErrors_;
    }

    ext::shared_ptr<BlackVolTermStructure> HestonCalibrator::impliedVolSurface() const {
        QL_REQUIRE(impliedVolSurface_,
                   "HestonCalibrator: calibrate() has not been called");
        return impliedVolSurface_;
    }

    ext::shared_ptr<HestonModel> HestonCalibrator::calibratedHestonModel() const {
        QL_REQUIRE(hestonModel_, "HestonCalibrator: calibrate() has not been called");
        return hestonModel_;
    }

    const std::vector<ext::shared_ptr<BlackCalibrationHelper>>&
    HestonCalibrator::helpers() const {
        return helpers_;
    }

    Real StochVolCalibrator::rmse() const {
        const std::vector<Real> errs = calibrationErrors();
        QL_REQUIRE(!errs.empty(),
                   "StochVolCalibrator: no calibration errors available");
        Real sumSq = 0.0;
        for (Real e : errs)
            sumSq += e * e;
        return std::sqrt(sumSq / static_cast<Real>(errs.size()));
    }

}
