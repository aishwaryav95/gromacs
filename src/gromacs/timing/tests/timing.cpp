/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2021, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
/*! \internal \file
 * \brief
 * This implements tests for timing function wrappers and decorators
 *
 * \author Sebastian Keller <keller@cscs.ch>
 */

#include "gmxpre.h"

#include "config.h"

#include <chrono>
#include <memory>
#include <thread>

#include "gromacs/timing/cyclecounter.h"
#include "gromacs/timing/wallcycle.h"

#include "testutils/refdata.h"
#include "testutils/testasserts.h"

namespace gmx
{
namespace test
{
namespace
{

constexpr bool useCycleSubcounters = GMX_CYCLE_SUBCOUNTERS;

//! Test function
void sleepForMilliseconds(int msecs)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(msecs));
}

//! Test Fixture for timing tests
class TimingTest : public ::testing::Test
{
public:
    TimingTest() : wcycle(wallcycle_init(nullptr, 0, nullptr)) {}

protected:
    const int                      delayInMilliseconds = 1;
    std::unique_ptr<gmx_wallcycle> wcycle;
};


//! Test whether the we can run the cycle counter.
TEST_F(TimingTest, RunWallCycle)
{
    WallCycleCounter probe = WallCycleCounter::Run, ref = WallCycleCounter::Step;
    int              n1, n2;
    double           c1, c2;

    //! credit cycles from enclosing call to the ref field of wcycle
    wallcycle_start(wcycle.get(), ref);
    //! cycles from the probe call
    wallcycle_start(wcycle.get(), probe);
    sleepForMilliseconds(delayInMilliseconds);
    wallcycle_stop(wcycle.get(), probe);
    wallcycle_stop(wcycle.get(), ref);
    //! extract both
    wallcycle_get(wcycle.get(), probe, &n1, &c1);
    wallcycle_get(wcycle.get(), ref, &n2, &c2);

    EXPECT_EQ(n1, n2);
    EXPECT_DOUBLE_EQ_TOL(c1, c2, relativeToleranceAsFloatingPoint(c1, 5e-3));
    EXPECT_GE(c2, c1);
}

//! Test whether the subcyclecounter runs
TEST_F(TimingTest, RunWallCycleSub)
{
    if (useCycleSubcounters)
    {
        WallCycleSubCounter probe = WallCycleSubCounter::DDRedist;
        WallCycleSubCounter ref   = WallCycleSubCounter::DDGrid;
        int                 n1, n2;
        double              c1, c2;
        wallcycle_sub_start(wcycle.get(), ref);
        wallcycle_sub_start(wcycle.get(), probe);
        sleepForMilliseconds(delayInMilliseconds);
        wallcycle_sub_stop(wcycle.get(), probe);
        wallcycle_sub_stop(wcycle.get(), ref);
        wallcycle_sub_get(wcycle.get(), probe, &n1, &c1);
        wallcycle_sub_get(wcycle.get(), ref, &n2, &c2);

        EXPECT_EQ(n1, n2);
        EXPECT_DOUBLE_EQ_TOL(c1, c2, relativeToleranceAsFloatingPoint(c1, 5e-3));
        EXPECT_GE(c2, c1);
    }
}

} // namespace
} // namespace test
} // namespace gmx
