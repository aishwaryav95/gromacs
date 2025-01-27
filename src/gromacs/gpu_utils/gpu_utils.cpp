/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2014,2015,2017,2018,2019, by the GROMACS development team.
 * Copyright (c) 2020,2021, by the GROMACS development team, led by
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
 *  \brief Function definitions for non-GPU builds
 *
 *  \author Mark Abraham <mark.j.abraham@gmail.com>
 */
#include "gmxpre.h"

#include "gpu_utils.h"

#include "config.h"

#include "gromacs/utility/arrayref.h"
#include "gromacs/utility/enumerationhelpers.h"
#include "gromacs/utility/stringutil.h"

#ifdef _MSC_VER
#    pragma warning(disable : 6237)
#endif

const char* enumValueToString(GpuApiCallBehavior enumValue)
{
    static constexpr gmx::EnumerationArray<GpuApiCallBehavior, const char*> s_gpuApiCallBehaviorNames = {
        "Synchronous", "Asynchronous"
    };
    return s_gpuApiCallBehaviorNames[enumValue];
}

/*! \brief Help build a descriptive message in \c error if there are
 * \c errorReasons why nonbondeds on a GPU are not supported.
 *
 * \returns Whether the lack of errorReasons indicate there is support. */
static bool addMessageIfNotSupported(gmx::ArrayRef<const std::string> errorReasons, std::string* error)
{
    bool isSupported = errorReasons.empty();
    if (!isSupported && error)
    {
        *error = "Nonbonded interactions cannot run on GPUs: ";
        *error += joinStrings(errorReasons, "; ") + ".";
    }
    return isSupported;
}

bool buildSupportsNonbondedOnGpu(std::string* error)
{
    std::vector<std::string> errorReasons;
    if (GMX_DOUBLE)
    {
        errorReasons.emplace_back("double precision");
    }
    if (!GMX_GPU)
    {
        errorReasons.emplace_back("non-GPU build of GROMACS");
    }
    return addMessageIfNotSupported(errorReasons, error);
}
