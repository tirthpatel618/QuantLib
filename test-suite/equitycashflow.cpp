/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 Copyright (C) 2023 Marcin Rybacki

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

#include "equitycashflow.hpp"
#include "utilities.hpp"
#include <ql/cashflows/equitycashflow.hpp>
#include <ql/indexes/equityindex.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/quotes/simplequote.hpp>

using namespace QuantLib;
using namespace boost::unit_test_framework;

namespace equitycashflow_test {

    // Used to check that the exception message contains the expected message string, expMsg.
    struct ExpErrorPred {

        explicit ExpErrorPred(std::string msg) : expMsg(std::move(msg)) {}

        bool operator()(const Error& ex) const {
            std::string errMsg(ex.what());
            if (errMsg.find(expMsg) == std::string::npos) {
                BOOST_TEST_MESSAGE("Error expected to contain: '" << expMsg << "'.");
                BOOST_TEST_MESSAGE("Actual error is: '" << errMsg << "'.");
                return false;
            } else {
                return true;
            }
        }

        std::string expMsg;
    };

    struct CommonVars {

        Date today;
        Calendar calendar;
        DayCounter dayCount;

        Real notional;

        ext::shared_ptr<EquityIndex> equityIndex;
        
        RelinkableHandle<YieldTermStructure> localCcyInterestHandle;
        RelinkableHandle<YieldTermStructure> dividendHandle;
        RelinkableHandle<YieldTermStructure> quantoCcyInterestHandle;

        RelinkableHandle<BlackVolTermStructure> equityVolHandle;
        RelinkableHandle<BlackVolTermStructure> fxVolHandle;
        
        RelinkableHandle<Quote> spotHandle;
        RelinkableHandle<Quote> correlationHandle;

        // cleanup
        SavedSettings backup;
        // utilities

        CommonVars() {
            calendar = TARGET();
            dayCount = Actual365Fixed();
            notional = 1.0e7;

            today = calendar.adjust(Date(27, January, 2023));
            Settings::instance().evaluationDate() = today;

            equityIndex = ext::make_shared<EquityIndex>("eqIndex", calendar, localCcyInterestHandle,
                                                        dividendHandle, spotHandle);
            IndexManager::instance().clearHistory(equityIndex->name());
            equityIndex->addFixing(Date(5, January, 2023), 9010.0);
            equityIndex->addFixing(today, 8690.0);

            localCcyInterestHandle.linkTo(flatRate(0.0375, dayCount));
            dividendHandle.linkTo(flatRate(0.005, dayCount));
            quantoCcyInterestHandle.linkTo(flatRate(0.001, dayCount));

            equityVolHandle.linkTo(flatVol(0.4, dayCount));
            fxVolHandle.linkTo(flatVol(0.2, dayCount));

            spotHandle.linkTo(ext::make_shared<SimpleQuote>(8700.0));
            correlationHandle.linkTo(ext::make_shared<SimpleQuote>(0.4));
        }

        ext::shared_ptr<EquityCashFlow> createEquityQuantoCashFlow(
            const ext::shared_ptr<EquityIndex>& index, const Date& start, const Date& end) {
            return ext::make_shared<EquityCashFlow>(notional, index, start, end, end);
        }

        ext::shared_ptr<EquityCashFlowPricer> createEquityQuantoPricer() {
            return ext::make_shared<EquityQuantoCashFlowPricer>(
                quantoCcyInterestHandle, equityVolHandle, fxVolHandle, correlationHandle);
        }

        ext::shared_ptr<EquityCashFlowPricer> createEquityQuantoPricerWithMissingHandles() {
            Handle<BlackVolTermStructure> vol;
            return ext::make_shared<EquityQuantoCashFlowPricer>(quantoCcyInterestHandle, vol, vol,
                                                                correlationHandle);
        }
    };

    void bumpMarketData(CommonVars& vars) {
        
        vars.localCcyInterestHandle.linkTo(flatRate(0.04, vars.dayCount));
        vars.dividendHandle.linkTo(flatRate(0.01, vars.dayCount));
        vars.quantoCcyInterestHandle.linkTo(flatRate(0.03, vars.dayCount));

        vars.equityVolHandle.linkTo(flatVol(0.45, vars.dayCount));
        vars.fxVolHandle.linkTo(flatVol(0.25, vars.dayCount));

        vars.spotHandle.linkTo(ext::make_shared<SimpleQuote>(8710.0));
    }

    void checkQuantoCorrection(const Date& start,
                               const Date& end,
                               bool includeDividend,
                               bool bumpData = false) {
        const Real tolerance = 1.0e-6;

        CommonVars vars;

        ext::shared_ptr<EquityIndex> equityIndex =
            includeDividend ?
                vars.equityIndex :
                vars.equityIndex->clone(vars.localCcyInterestHandle, Handle<YieldTermStructure>(),
                                        vars.spotHandle);

        auto cf = vars.createEquityQuantoCashFlow(equityIndex, start, end);
        auto pricer = vars.createEquityQuantoPricer();
        cf->setPricer(pricer);

        if (bumpData)
            bumpMarketData(vars);

        Real strike = vars.equityIndex->fixing(end);
        Real indexStart = vars.equityIndex->fixing(start);

        Real time = vars.localCcyInterestHandle->timeFromReference(end);
        Real rf = vars.localCcyInterestHandle->zeroRate(time, Continuous);
        Real q = includeDividend ? vars.dividendHandle->zeroRate(time, Continuous) : 0.0;
        Real eqVol = vars.equityVolHandle->blackVol(end, strike);
        Real fxVol = vars.fxVolHandle->blackVol(end, 1.0);
        Real rho = vars.correlationHandle->value();
        Real spot = vars.spotHandle->value();

        Real quantoForward = spot * std::exp((rf - q - rho * eqVol * fxVol) * time);
        Real expectedAmount = (quantoForward / indexStart - 1.0) * vars.notional;

        Real actualAmount = cf->amount();

        if ((std::fabs(actualAmount - expectedAmount) > tolerance))
            BOOST_ERROR("could not replicate equity quanto correction\n"
                        << "    actual amount:    " << actualAmount << "\n"
                        << "    expected amount:    " << expectedAmount << "\n"
                        << "    index start:    " << indexStart << "\n"
                        << "    index end:    " << quantoForward << "\n"
                        << "    local rate:    " << rf << "\n"
                        << "    equity volatility:    " << eqVol << "\n"
                        << "    FX volatility:    " << fxVol << "\n"
                        << "    correlation:    " << rho << "\n"
                        << "    spot:    " << spot << "\n");
    }
}

void EquityCashFlowTest::testSimpleEquityCashFlow() {
    BOOST_TEST_MESSAGE("Testing simple equity cash flow...");

    using namespace equitycashflow_test;

    const Real tolerance = 1.0e-6;

    CommonVars vars;

    Date startDate(5, January, 2023);
    Date endDate(5, April, 2023);

    auto cf = vars.createEquityQuantoCashFlow(vars.equityIndex, startDate, endDate);

    Real indexStart = vars.equityIndex->fixing(startDate);
    Real indexEnd = vars.equityIndex->fixing(endDate);

    Real expectedAmount = (indexEnd / indexStart - 1.0) * vars.notional;

    Real actualAmount = cf->amount();

    if ((std::fabs(actualAmount - expectedAmount) > tolerance))
        BOOST_ERROR("could not replicate simple equity quanto cash flow\n"
                    << "    actual amount:    " << actualAmount << "\n"
                    << "    expected amount:    " << expectedAmount << "\n"
                    << "    index start:    " << indexStart << "\n"
                    << "    index end:    " << indexEnd << "\n");
}

void EquityCashFlowTest::testQuantoCorrection() {
    BOOST_TEST_MESSAGE("Testing quanto correction...");

    using namespace equitycashflow_test;

    Date startDate(5, January, 2023);
    Date endDate(5, April, 2023);

    checkQuantoCorrection(startDate, endDate, true);
    checkQuantoCorrection(startDate, endDate, false);

    // Checks whether observers are being notified
    // about changes in market data handles.
    checkQuantoCorrection(startDate, endDate, false, true);
}

void EquityCashFlowTest::testErrorWhenBaseDateAfterFixingDate() {
    BOOST_TEST_MESSAGE("Testing error when base date after fixing date...");

    using namespace equitycashflow_test;

    CommonVars vars;

    Date endDate(5, January, 2023);
    Date startDate(5, April, 2023);

    auto cf = vars.createEquityQuantoCashFlow(vars.equityIndex, startDate, endDate);
    auto pricer = vars.createEquityQuantoPricer();
    cf->setPricer(pricer);

    BOOST_CHECK_EXCEPTION(
        cf->amount(), Error,
        equitycashflow_test::ExpErrorPred("Fixing date cannot fall before base date."));
}

void EquityCashFlowTest::testErrorWhenHandleInPricerIsEmpty() {
    BOOST_TEST_MESSAGE("Testing error when market data handle in pricer is empty...");

    using namespace equitycashflow_test;

    CommonVars vars;

    Date startDate(5, January, 2023);
    Date endDate(5, April, 2023);

    auto cf = vars.createEquityQuantoCashFlow(vars.equityIndex, startDate, endDate);
    auto pricer = vars.createEquityQuantoPricerWithMissingHandles();
    cf->setPricer(pricer);

    BOOST_CHECK_EXCEPTION(
        cf->amount(), Error,
        equitycashflow_test::ExpErrorPred(
            "Quanto currency, equity and FX volatility term structure handles cannot be empty."));
}

void EquityCashFlowTest::testErrorWhenInconsistentMarketDataReferenceDate() {
    BOOST_TEST_MESSAGE("Testing error when market data reference dates are inconsistent...");

    using namespace equitycashflow_test;

    CommonVars vars;

    Date startDate(5, January, 2023);
    Date endDate(5, April, 2023);

    auto cf = vars.createEquityQuantoCashFlow(vars.equityIndex, startDate, endDate);
    auto pricer = vars.createEquityQuantoPricer();
    cf->setPricer(pricer);

    vars.quantoCcyInterestHandle.linkTo(flatRate(Date(26, January, 2023), 0.02, vars.dayCount));

    BOOST_CHECK_EXCEPTION(
        cf->amount(), Error,
        equitycashflow_test::ExpErrorPred(
            "Quanto currency term structure, equity and FX volatility need to have the same "
            "reference date."));
}

test_suite* EquityCashFlowTest::suite() {
    auto* suite = BOOST_TEST_SUITE("Equity quanto cash flow tests");

    suite->add(QUANTLIB_TEST_CASE(&EquityCashFlowTest::testSimpleEquityCashFlow));
    suite->add(QUANTLIB_TEST_CASE(&EquityCashFlowTest::testQuantoCorrection));
    suite->add(QUANTLIB_TEST_CASE(&EquityCashFlowTest::testErrorWhenBaseDateAfterFixingDate));
    suite->add(QUANTLIB_TEST_CASE(&EquityCashFlowTest::testErrorWhenHandleInPricerIsEmpty));
    suite->add(
        QUANTLIB_TEST_CASE(&EquityCashFlowTest::testErrorWhenInconsistentMarketDataReferenceDate));

    return suite;
}