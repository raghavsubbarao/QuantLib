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

#include <complex>
#include <ql/math/distributions/normaldistribution.hpp>
#include <ql/math/matrix.hpp>
#include <ql/math/matrixutilities/svd.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/fxcostsmilesection.hpp>

namespace QuantLib {

    Integer cubic::roots(std::vector<Real>& zeros)
    {
        Real pi = 3.14159265358979323846;
        const std::vector<Real> c = coefficients();
        if (c[3] == 0.0 || std::abs(c[3] / c[2]) < 1.0e-6)
        {
            quadratic q(c[2], c[1], c[0]);
            Real r1, r2;
            if (q.roots(r1, r2))
            {
                    zeros.clear();
                    zeros.push_back(r1);
                    zeros.push_back(r2);
                    return 2;
            }
        }

        Real B = c[2] / c[3], C = c[1] / c[3], D = c[0] / c[3];
        Real Q = (B * B - C * 3.0) / 9.0, QQQ = Q * Q * Q;
        Real R = (2.0 * B * B * B - 9.0 * B * C + 27.0 * D) / 54.0, RR = R * R;

        if (std::abs(RR - QQQ) < 1.0e-16)
        {
            // single repeated root
            zeros.clear();
            zeros.push_back(-std::pow(D, 1.0 / 3.0));
            return 3;
        }
        else if (RR <= QQQ)
        {
            // 3 real roots
            // sqrt and division is safe as RR >= 0 and QQQ > RR => QQQ > 0
            // The acos is also safe, since RR / QQQ < 1 => R / sqrt(QQQ) < 1
            Real theta = std::acos(R / std::sqrt(QQQ));

            // This is safe, since QQQ >= 0 => Q >= 0
            Real r1, r2, r3;
            r1 = r2 = r3 = -2.0 * std::sqrt(Q);
            r1 *= cos(theta / 3.0);
            r2 *= cos((theta + 2 * pi) / 3.0);
            r3 *= cos((theta - 2 * pi) / 3.0);

            r1 -= B / 3.0;
            r2 -= B / 3.0;
            r3 -= B / 3.0;

            zeros.clear();
            zeros.push_back(r1);
            zeros.push_back(r2);
            zeros.push_back(r3);
            return 3;
        }
        else
        {
            // 1 real root
            Real root;
            Real d = std::sqrt(RR - QQQ) - R;
            Real s = (d > 0) ? std::pow(d, 1.0 / 3.0) : -std::pow(-d, 1.0 / 3.0);
            if (std::fabs(s) < 1.0e-10)
            {
                d = -std::sqrt(RR - QQQ) - R;
                root = (d > 0) ? std::pow(d, 1.0 / 3.0) : -std::pow(-d, 1.0 / 3.0);
            }
            else
            {

                root = s + Q / s;
            }
            root -= B / 3.0;

            zeros.clear();
            zeros.push_back(root);
            return 1;
        }

        zeros.clear();
        return 0;
    }


    Integer quartic::roots(std::vector<Real>& zeros)
    {
        const std::vector<Real> c = coefficients();

        if (c[4] == 0.0) {
            return cubic(std::vector<Real>(c.begin() + 1, c.end())).roots(zeros);
        }

        Real B = c[3] / c[4], C = c[2] / c[4], D = c[1] / c[4], E = c[0] / c[4];
        Real BB = B * B;
        Real I = C - 0.375 * BB;
        Real J = D - B * C * 0.5 + BB * B * 0.125;
        Real K = E - B * D * 0.25 + C * BB * 0.0625 - BB * BB * 0.01171875;
        // the depressed quartic is u^4 + I * u^2 + J * u + K

        if (std::abs(J) < 1.0e-10) {
            // TODO: depressed equation is a biquadratic!
            quadratic quad(1.0, I, K);

            zeros.clear();
            if (quad.discriminant() < 0) {
                // no roots;
                return 0;
            }

            Real z1, z2;
            quad.roots(z1, z2);

            if (z1 > 0) {
                zeros.push_back(std::sqrt(z1) - 0.25 * B);
                zeros.push_back(-std::sqrt(z1) - 0.25 * B);
            }

            if (z2 > 0) {
                zeros.push_back(std::sqrt(z2) - 0.25 * B);
                zeros.push_back(-std::sqrt(z2) - 0.25 * B);
            }

            return zeros.size();
        }

        // create and solve cubic - this cubic is -J*J<0 at 0 and
        // tends to infinity as x-> infinity -> it has a positive root!
        cubic p3(std::vector<Real>{-J * J, I * I - 4.0 * K, 2.0 * I, 1.0});

        std::vector<Real> z3;
        int n = p3.roots(z3);

        QL_ASSERT(n > 0, "a cubic should have at least one real root!");

        Real z = z3[0];
        if (z < 0) z = z3[1];
        if (z < 0) z = z3[2];

        Real p = std::sqrt(z);

        if (std::abs(p) < 1.0e-10) {
            zeros.clear();
            zeros.push_back(-0.25 * B);
            return 1;

        }

        Real r = -p;
        Real q = (I + z - J / p) / 2.0;
        Real s = (I + z + J / p) / 2.0;

        quadratic quad1(1.0, p, q), quad2(1.0, r, s);

        Real z1, z2;
        if (quad1.discriminant() >= 0.0)
        {
            quad1.roots(z1, z2);
            zeros.push_back(z1 - 0.25 * B);
            zeros.push_back(z2 - 0.25 * B);
        }

        if (quad2.discriminant() >= 0.0) {
            quad2.roots(z1, z2);
            zeros.push_back(z1 - 0.25 * B);
            zeros.push_back(z2 - 0.25 * B);
        }

        return zeros.size();
    }


    // -----------------------------------------------------------------------
    //  fxCostSmileSection — constructors
    // -----------------------------------------------------------------------

	fxCostSmileSection::fxCostSmileSection(const Date& exerciseDate,
                                           const Handle<Quote>& spot,
                                           const Handle<Quote>& atm,
                                           const std::vector<Handle<Quote>>& rrs,
                                           const std::vector<Handle<Quote>>& bfs,
                                           const std::vector<Real>& deltas,
                                           const Handle<YieldTermStructure>& foreignDiscount,
                                           const Handle<YieldTermStructure>& domesticDiscount,
                                           DeltaVolQuote::DeltaType deltaType,
                                           DeltaVolQuote::AtmType atmType,
                                           fxSmileSection::FlyType flyType,
                                           Real alpha,
                                           const DayCounter& dayCounter,
                                           const Date& referenceDate,
                                           bool weightedCalibrationFlag)
    : fxSmileSectionByStrike(exerciseDate, spot, atm, rrs, bfs, deltas,
                             foreignDiscount, domesticDiscount,
                             deltaType, atmType, flyType, dayCounter, referenceDate),
        weightedCalibrationFlag_(weightedCalibrationFlag), alpha_(alpha)
    {
        QL_REQUIRE(alpha >= 0.0 && alpha <= 1.0, "alpha must be in [0,1], got " << alpha);
        params_.reserve(4);
    }

    fxCostSmileSection::fxCostSmileSection(Time exerciseTime,
                                           const Handle<Quote>& spot,
                                           const Handle<Quote>& atm,
                                           const std::vector<Handle<Quote>>& rrs,
                                           const std::vector<Handle<Quote>>& bfs,
                                           const std::vector<Real>& deltas,
                                           const Handle<YieldTermStructure>& foreignDiscount,
                                           const Handle<YieldTermStructure>& domesticDiscount,
                                           DeltaVolQuote::DeltaType deltaType,
                                           DeltaVolQuote::AtmType atmType,
                                           fxSmileSection::FlyType flyType,
                                           Real alpha,
                                           const DayCounter& dayCounter,
                                           bool weightedCalibrationFlag)
    : fxSmileSectionByStrike(exerciseTime, spot, atm, rrs, bfs, deltas,
                             foreignDiscount, domesticDiscount,
                             deltaType, atmType, flyType, dayCounter),
        weightedCalibrationFlag_(weightedCalibrationFlag), alpha_(alpha)
    {
        QL_REQUIRE(alpha >= 0.0 && alpha <= 1.0, "alpha must be in [0,1], got " << alpha);
        params_.reserve(4);
    }

    fxCostSmileSection::fxCostSmileSection(const Date& exerciseDate,
                                           const Handle<Quote>& spot,
                                           const std::vector<Handle<DeltaVolQuote>>& quotes,
                                           const Handle<YieldTermStructure>& foreignDiscount,
                                           const Handle<YieldTermStructure>& domesticDiscount,
                                           DeltaVolQuote::DeltaType deltaType,
                                           DeltaVolQuote::AtmType atmType,
                                           fxSmileSection::FlyType flyType,
                                           Real alpha,
                                           const DayCounter& dayCounter,
                                           const Date& referenceDate,
                                           bool weightedCalibrationFlag)
    : fxSmileSectionByStrike(exerciseDate, spot, quotes,
                             foreignDiscount, domesticDiscount,
                             deltaType, atmType, flyType, dayCounter, referenceDate),
        weightedCalibrationFlag_(weightedCalibrationFlag), alpha_(alpha)
    {
        QL_REQUIRE(alpha >= 0.0 && alpha <= 1.0, "alpha must be in [0,1], got " << alpha);
        params_.reserve(4);
    }

    fxCostSmileSection::fxCostSmileSection(Time exerciseTime,
                                           const Handle<Quote>& spot,
                                           const std::vector<Handle<DeltaVolQuote>>& quotes,
                                           const Handle<YieldTermStructure>& foreignDiscount,
                                           const Handle<YieldTermStructure>& domesticDiscount,
                                           DeltaVolQuote::DeltaType deltaType,
                                           DeltaVolQuote::AtmType atmType,
                                           fxSmileSection::FlyType flyType,
                                           Real alpha,
                                           const DayCounter& dayCounter,
                                           bool weightedCalibrationFlag)
    : fxSmileSectionByStrike(exerciseTime, spot, quotes,
                             foreignDiscount, domesticDiscount,
                             deltaType, atmType, flyType, dayCounter),
        weightedCalibrationFlag_(weightedCalibrationFlag), alpha_(alpha)
    {
        QL_REQUIRE(alpha >= 0.0 && alpha <= 1.0, "alpha must be in [0,1], got " << alpha);
        params_.reserve(4);
    }


    // -----------------------------------------------------------------------
    //  fxCostSmileSection — calibration (linear SVD, general alpha)
    // -----------------------------------------------------------------------

    Array fxCostSmileSection::initialParams() const
    {
        // Not used — calibration is algebraic (SVD).
        Array guess(4, 0.0);
        guess[0] = 1.0;
        return guess;
    }

    void fxCostSmileSection::calibrate() const {
        // Build the regressor matrix for the general-alpha cost equation.
        //
        // The cost equation at a quoted point with total vol w = sigma * sqrt(T) is:
        //
        //   Cgamma + 2*Ctheta*w^(1+alpha) - 2*Ceta*d+(w)*w^alpha + Comega*d+(w)*d-(w)*w^(2*alpha) = 0
        //
        // Columns: [Cgamma, Ctheta, Ceta, Comega]
        // Regressors: [1, 2*w^(1+a), -2*d+*w^a, d+*d-*w^(2a)]   (times optional weight)
        //
        // Note: d+ is used for the vanna regressor in both the standard and premium-adjusted
        // cases — this is consistent with the existing per-class behaviour preserved here.

        auto dp = [&](Real k, Real iw) { return std::log(fwd_ / k) / iw + iw / 2.; };
        auto dm = [&](Real k, Real iw) { return std::log(fwd_ / k) / iw - iw / 2.; };

        Real htau = std::sqrt(exerciseTime());

        std::vector<Real> ts;
        Real w, k, wt;
        Option::Type ot;
        CumulativeNormalDistribution f;

        for (Size i = 0; i < quotes_.size(); i++) {
            w = quotes_[i]->value() * htau;
            if (quotes_[i]->atmType() == DeltaVolQuote::AtmNull) {
                ot = (quotes_[i]->delta() < 0) ? Option::Type::Put : Option::Type::Call;
                k = BlackDeltaCalculator(ot, deltaType(), spot()->value(), ddom_, dfor_, w)
                        .strikeFromDelta(quotes_[i]->delta());
            } else {
                k = BlackDeltaCalculator(Option::Type::Call, deltaType(), spot()->value(), ddom_,
                                         dfor_, w)
                        .atmStrike(quotes_[i]->atmType());
            }

            wt = weightedCalibration() ? f.derivative(dp(k, w)) : 1.0;

            ts.push_back(1.0 * wt);
            ts.push_back(2.0 * std::pow(w, 1.0 + alpha_) * wt);
            ts.push_back(-2.0 * dp(k, w) * std::pow(w, alpha_) * wt);
            ts.push_back(dp(k, w) * dm(k, w) * std::pow(w, 2.0 * alpha_) * wt);
        }

        Matrix A(quotes_.size(), 4, ts.begin(), ts.end());
        SVD svd(A);

        const Matrix V = svd.V();

        params_.clear();
        params_.push_back(1.0);
        params_.push_back(V(1, 3) / V(0, 3));
        params_.push_back(V(2, 3) / V(0, 3));
        params_.push_back(V(3, 3) / V(0, 3));
    }


    // -----------------------------------------------------------------------
    //  fxCostSmileSection — Newton-Raphson vol by strike (general alpha)
    // -----------------------------------------------------------------------

    Volatility fxCostSmileSection::_volByStrike(Real strike,
                                                 Real fwd,
                                                 Time tau,
                                                 const std::vector<Real>& params) const
    {
        // Solve f(rho) = 0 for total vol rho = sigma*sqrt(tau) at log-moneyness x = ln(F/K),
        // where the cost equation with d(rho) = c * rho^alpha dynamics is:
        //
        //   p0 + (2*p1 - s*p2)*rho^(1+a) - 2*p2*x*rho^(a-1)
        //      + p3*x^2*rho^(2a-2) - (p3/4)*rho^(2+2a)  =  0
        //
        // where  s = -1 (standard, d- in vanna term) or +1 (premium-adjusted, d+ in vanna term).
        // p = [Cgamma, Ctheta, Ceta, Comega] normalised so that p0 = 1.
        //
        // Since 0 <= a <= 1, the most negative exponent above is (2a-2). Multiplying
        // throughout by rho^(2-2a) clears every negative power, giving the equivalent
        // (same positive roots) but numerically safer equation g(rho) = 0:
        //
        //   g(rho) = p0*rho^(2-2a) + (2*p1 - s*p2)*rho^(3-a)
        //          - 2*p2*x*rho^(1-a) + p3*x^2 - (p3/4)*rho^4

        Real x = std::log(fwd / strike);
        Real a = alpha_;
        Real s = premiumAdjust() ? 1.0 : -1.0;

        auto f = [&](Real rho) -> Real {
            return params[0] * std::pow(rho, 2.0 - 2.0 * a)
                 + (2.0 * params[1] - s * params[2]) * std::pow(rho, 3.0 - a)
                 - 2.0 * params[2] * x * std::pow(rho, 1.0 - a)
                 + params[3] * x * x
                 - (params[3] / 4.0) * std::pow(rho, 4.0);
        };

        auto df = [&](Real rho) -> Real {
            return params[0] * (2.0 - 2.0 * a) * std::pow(rho, 1.0 - 2.0 * a)
                 + (2.0 * params[1] - s * params[2]) * (3.0 - a) * std::pow(rho, 2.0 - a)
                 - 2.0 * params[2] * x * (1.0 - a) * std::pow(rho, -a)
                 - params[3] * std::pow(rho, 3.0);
        };

        // Start from ATM total vol.
        Real rho = atm_->value() * std::sqrt(tau);

        static const Size maxIter = 100;
        static const Real tol = 1.0e-12;

        for (Size i = 0; i < maxIter; ++i) {
            Real fval = f(rho);
            if (std::fabs(fval) < tol)
                break;
            Real dfval = df(rho);
            QL_REQUIRE(std::fabs(dfval) > 1.0e-20,
                       "near-zero derivative in Newton-Raphson for fxCostSmileSection");
            Real step = -fval / dfval;
            // Halve the step until rho stays strictly positive.
            while (rho + step <= 0.0)
                step *= 0.5;
            rho += step;
        }

        return rho / std::sqrt(tau);
    }


    // -----------------------------------------------------------------------
    //  fxCostSmileSectionFlatDynamics — constructors (forward alpha=0)
    // -----------------------------------------------------------------------

    fxCostSmileSectionFlatDynamics::fxCostSmileSectionFlatDynamics(const Date& exerciseDate,
                                                                   const Handle<Quote>& spot,
                                                                   const Handle<Quote>& atm,
                                                                   const std::vector<Handle<Quote>>& rrs,
                                                                   const std::vector<Handle<Quote>>& bfs,
                                                                   const std::vector<Real>& deltas,
                                                                   const Handle<YieldTermStructure>& foreignDiscount,
                                                                   const Handle<YieldTermStructure>& domesticDiscount,
                                                                   DeltaVolQuote::DeltaType deltaType,
                                                                   DeltaVolQuote::AtmType atmType,
                                                                   fxSmileSection::FlyType flyType,
                                                                   const DayCounter& dayCounter,
                                                                   const Date& referenceDate,
                                                                   bool weightedCalibrationFlag)
    : fxCostSmileSection(exerciseDate, spot, atm, rrs, bfs, deltas,
                         foreignDiscount, domesticDiscount,
                         deltaType, atmType, flyType, 0.0,
                         dayCounter, referenceDate, weightedCalibrationFlag) {}

    fxCostSmileSectionFlatDynamics::fxCostSmileSectionFlatDynamics(Time exerciseTime,
                                                                   const Handle<Quote>& spot,
                                                                   const Handle<Quote>& atm,
                                                                   const std::vector<Handle<Quote>>& rrs,
                                                                   const std::vector<Handle<Quote>>& bfs,
                                                                   const std::vector<Real>& deltas,
                                                                   const Handle<YieldTermStructure>& foreignDiscount,
                                                                   const Handle<YieldTermStructure>& domesticDiscount,
                                                                   DeltaVolQuote::DeltaType deltaType,
                                                                   DeltaVolQuote::AtmType atmType,
                                                                   fxSmileSection::FlyType flyType,
                                                                   const DayCounter& dayCounter,
                                                                   bool weightedCalibrationFlag)
    : fxCostSmileSection(exerciseTime, spot, atm, rrs, bfs, deltas,
                         foreignDiscount, domesticDiscount,
                         deltaType, atmType, flyType, 0.0,
                         dayCounter, weightedCalibrationFlag) {}

    fxCostSmileSectionFlatDynamics::fxCostSmileSectionFlatDynamics(const Date& exerciseDate,
                                                                   const Handle<Quote>& spot,
                                                                   const std::vector<Handle<DeltaVolQuote>>& quotes,
                                                                   const Handle<YieldTermStructure>& foreignDiscount,
                                                                   const Handle<YieldTermStructure>& domesticDiscount,
                                                                   DeltaVolQuote::DeltaType deltaType,
                                                                   DeltaVolQuote::AtmType atmType,
                                                                   fxSmileSection::FlyType flyType,
                                                                   const DayCounter& dayCounter,
                                                                   const Date& referenceDate,
                                                                   bool weightedCalibrationFlag)
    : fxCostSmileSection(exerciseDate, spot, quotes,
                         foreignDiscount, domesticDiscount,
                         deltaType, atmType, flyType, 0.0,
                         dayCounter, referenceDate, weightedCalibrationFlag) {}

    fxCostSmileSectionFlatDynamics::fxCostSmileSectionFlatDynamics(Time exerciseTime,
                                                                   const Handle<Quote>& spot,
                                                                   const std::vector<Handle<DeltaVolQuote>>& quotes,
                                                                   const Handle<YieldTermStructure>& foreignDiscount,
                                                                   const Handle<YieldTermStructure>& domesticDiscount,
                                                                   DeltaVolQuote::DeltaType deltaType,
                                                                   DeltaVolQuote::AtmType atmType,
                                                                   fxSmileSection::FlyType flyType,
                                                                   const DayCounter& dayCounter,
                                                                   bool weightedCalibrationFlag)
    : fxCostSmileSection(exerciseTime, spot, quotes,
                         foreignDiscount, domesticDiscount,
                         deltaType, atmType, flyType, 0.0,
                         dayCounter, weightedCalibrationFlag) {}


    // -----------------------------------------------------------------------
    //  fxCostSmileSectionFlatDynamics — quartic vol by strike (alpha = 0)
    // -----------------------------------------------------------------------

    Volatility fxCostSmileSectionFlatDynamics::_volByStrike(Real strike,
                                                            Real fwd,
                                                            Time tau,
                                                            const std::vector<Real>& params) const
    {
        Real x = std::log(fwd / strike);
        Integer s = premiumAdjust() ? 1 : -1;

        quartic q(std::vector<Real>{-x * x * params[3], 2 * x * params[2], -params[0],
                                    s * params[2] - 2 * params[1], params[3] / 4.});
        std::vector<Real> omega;
        Integer n = q.roots(omega);

        QL_REQUIRE(n > 0, "calibrated smile should have a real root");

        Real w = omega[0];
        if (w < 0) w = omega[1];
        if (w < 0) w = omega[2];
        if (w < 0) w = omega[3];

        // improve root if necc
        Real err = q(w);
        if (std::abs(err) > 1.0e-16) {
            Brent solver;
            w = solver.solve([&](Real x) { return q(x); }, 1e-16, w, w - 0.001 * std::sqrt(tau), w + 0.001 * std::sqrt(tau));
        }

        return w / std::sqrt(tau);
    }


    // -----------------------------------------------------------------------
    //  fxCostSmileSectionScaledDynamics — constructors (forward alpha=1)
    // -----------------------------------------------------------------------

    fxCostSmileSectionScaledDynamics::fxCostSmileSectionScaledDynamics(const Date& exerciseDate,
                                                                       const Handle<Quote>& spot,
                                                                       const Handle<Quote>& atm,
                                                                       const std::vector<Handle<Quote>>& rrs,
                                                                       const std::vector<Handle<Quote>>& bfs,
                                                                       const std::vector<Real>& deltas,
                                                                       const Handle<YieldTermStructure>& foreignDiscount,
                                                                       const Handle<YieldTermStructure>& domesticDiscount,
                                                                       DeltaVolQuote::DeltaType deltaType,
                                                                       DeltaVolQuote::AtmType atmType,
                                                                       fxSmileSection::FlyType flyType,
                                                                       const DayCounter& dayCounter,
                                                                       const Date& referenceDate,
                                                                       bool weightedCalibrationFlag)
    : fxCostSmileSection(exerciseDate, spot, atm, rrs, bfs, deltas,
                         foreignDiscount, domesticDiscount,
                         deltaType, atmType, flyType, 1.0,
                         dayCounter, referenceDate, weightedCalibrationFlag) {}

    fxCostSmileSectionScaledDynamics::fxCostSmileSectionScaledDynamics(Time exerciseTime,
                                                                       const Handle<Quote>& spot,
                                                                       const Handle<Quote>& atm,
                                                                       const std::vector<Handle<Quote>>& rrs,
                                                                       const std::vector<Handle<Quote>>& bfs,
                                                                       const std::vector<Real>& deltas,
                                                                       const Handle<YieldTermStructure>& foreignDiscount,
                                                                       const Handle<YieldTermStructure>& domesticDiscount,
                                                                       DeltaVolQuote::DeltaType deltaType,
                                                                       DeltaVolQuote::AtmType atmType,
                                                                       fxSmileSection::FlyType flyType,
                                                                       const DayCounter& dayCounter,
                                                                       bool weightedCalibrationFlag)
    : fxCostSmileSection(exerciseTime, spot, atm, rrs, bfs, deltas,
                         foreignDiscount, domesticDiscount,
                         deltaType, atmType, flyType, 1.0,
                         dayCounter, weightedCalibrationFlag) {}

    fxCostSmileSectionScaledDynamics::fxCostSmileSectionScaledDynamics(const Date& exerciseDate,
                                                                       const Handle<Quote>& spot,
                                                                       const std::vector<Handle<DeltaVolQuote>>& quotes,
                                                                       const Handle<YieldTermStructure>& foreignDiscount,
                                                                       const Handle<YieldTermStructure>& domesticDiscount,
                                                                       DeltaVolQuote::DeltaType deltaType,
                                                                       DeltaVolQuote::AtmType atmType,
                                                                       fxSmileSection::FlyType flyType,
                                                                       const DayCounter& dayCounter,
                                                                       const Date& referenceDate,
                                                                       bool weightedCalibrationFlag)
    : fxCostSmileSection(exerciseDate, spot, quotes,
                         foreignDiscount, domesticDiscount,
                         deltaType, atmType, flyType, 1.0,
                         dayCounter, referenceDate, weightedCalibrationFlag) {}

    fxCostSmileSectionScaledDynamics::fxCostSmileSectionScaledDynamics(Time exerciseTime,
                                                                       const Handle<Quote>& spot,
                                                                       const std::vector<Handle<DeltaVolQuote>>& quotes,
                                                                       const Handle<YieldTermStructure>& foreignDiscount,
                                                                       const Handle<YieldTermStructure>& domesticDiscount,
                                                                       DeltaVolQuote::DeltaType deltaType,
                                                                       DeltaVolQuote::AtmType atmType,
                                                                       fxSmileSection::FlyType flyType,
                                                                       const DayCounter& dayCounter,
                                                                       bool weightedCalibrationFlag)
    : fxCostSmileSection(exerciseTime, spot, quotes,
                         foreignDiscount, domesticDiscount,
                         deltaType, atmType, flyType, 1.0,
                         dayCounter, weightedCalibrationFlag) {}


    // -----------------------------------------------------------------------
    //  fxCostSmileSectionScaledDynamics — quadratic vol by strike (alpha = 1)
    // -----------------------------------------------------------------------

    Volatility fxCostSmileSectionScaledDynamics::_volByStrike(Real strike,
                                                              Real fwd,
                                                              Time tau,
                                                              const std::vector<Real>& params) const
    {
        Real x = std::log(fwd / strike);
        Integer s = premiumAdjust() ? 1 : -1;

        quadratic q(params[3] / 4., s * params[2] - 2 * params[1],
                    2 * x * params[2] - params[0] - x * x * params[3]);
        Real y, z;
        bool hasroots = q.roots(y, z);

        QL_REQUIRE(hasroots, "calibrated smile should have a real root");

        Real w = y;
        if (w < 0)
            w = z;

        return std::sqrt(w / tau);
    }

}
