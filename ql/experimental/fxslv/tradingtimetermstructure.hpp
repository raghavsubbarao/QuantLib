/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2026 Raghav Subbarao

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

/*! \file tradingtimetermstructure.hpp
    \brief Trading-time term structure with down-weighted weekends
*/

#ifndef quantlib_trading_time_term_structure_hpp
#define quantlib_trading_time_term_structure_hpp

#include <ql/termstructure.hpp>
#include <ql/quote.hpp>
#include <ql/time/date.hpp>
#include <ql/time/calendars/weekendsonly.hpp>

namespace QuantLib {
    class tradingTimeTermStructure : public TermStructure {

      public:

        tradingTimeTermStructure(DayCounter dc = DayCounter(), 
                                 Real weekendWeight = 0.0);

        tradingTimeTermStructure(const Date& referenceDate,
                                 Calendar calendar = WeekendsOnly(),
                                 Real weekendWeight = 0.0,
                                 std::vector<Handle<Quote>> events = {},
                                 const std::vector<Date>& eventDates = {});

        tradingTimeTermStructure(Natural settlementDays=0,
                                 Calendar calendar = WeekendsOnly(),
                                 Real weekendWeight = 0.0,
                                 std::vector<Handle<Quote>> events = {},
                                 const std::vector<Date>& eventDates = {});

        //! \name Observer interface
        //@{
        void update() override;
        //@}
         
        //! \name TermStructure interface
        //@{
        virtual Date maxDate() const override { return Date::maxDate(); }
        //@}

        Real tradingTime(const Date& d1, const Date& d2) const;
        Real tradingTime(const Date& d) const { return tradingTime(referenceDate(), d); };

        Real tradingTime(Time t) const;

      private:
        // methods
        void setEvents(const Date& referenceDate, Period period = Period(5, Years));

        // data members
        Real weekendWeight_;
        std::vector<Handle<Quote>> events_;
        std::vector<Date> eventDates_;
        std::vector<Time> eventTimes_;
        std::vector<Date> holidayDates_;
        std::vector<Time> holidayTimes_;
        Size nEvents_ = 0;
        Date latestReference_;
    
    };

} // namespace QuantLib

#endif
