/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2018,2019,2020,2021, by the GROMACS development team, led by
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
 * \brief Subroutines to run LINCS on GPU
 *
 * Copies data to GPU, runs LINCS and copies the results back.
 *
 * \author Artem Zhmurov <zhmurov@gmail.com>
 * \ingroup module_mdlib
 */
#include "gmxpre.h"

#include "constrtestrunners.h"

#include <assert.h>

#include <cmath>

#include <algorithm>
#include <vector>

#include "gromacs/gpu_utils/devicebuffer.cuh"
#include "gromacs/gpu_utils/gputraits.h"
#include "gromacs/hardware/device_information.h"
#include "gromacs/mdlib/lincs_gpu.h"
#include "gromacs/pbcutil/pbc.h"
#include "gromacs/utility/unique_cptr.h"

namespace gmx
{
namespace test
{

void LincsDeviceConstraintsRunner::applyConstraints(ConstraintsTestData* testData, t_pbc pbc)
{
    const DeviceContext& deviceContext = testDevice_.deviceContext();
    const DeviceStream&  deviceStream  = testDevice_.deviceStream();
    setActiveDevice(testDevice_.deviceInfo());

    auto lincsGpu = std::make_unique<LincsGpu>(
            testData->ir_.nLincsIter, testData->ir_.nProjOrder, deviceContext, deviceStream);

    bool updateVelocities = true;
    int  numAtoms         = testData->numAtoms_;

    Float3* h_x  = gmx::asGenericFloat3Pointer(testData->x_);
    Float3* h_xp = gmx::asGenericFloat3Pointer(testData->xPrime_);
    Float3* h_v  = gmx::asGenericFloat3Pointer(testData->v_);

    DeviceBuffer<Float3> d_x, d_xp, d_v;

    lincsGpu->set(*testData->idef_, testData->numAtoms_, testData->invmass_.data());
    PbcAiuc pbcAiuc;
    setPbcAiuc(pbc.ndim_ePBC, pbc.box, &pbcAiuc);

    allocateDeviceBuffer(&d_x, numAtoms, deviceContext);
    allocateDeviceBuffer(&d_xp, numAtoms, deviceContext);
    allocateDeviceBuffer(&d_v, numAtoms, deviceContext);

    copyToDeviceBuffer(&d_x, h_x, 0, numAtoms, deviceStream, GpuApiCallBehavior::Sync, nullptr);
    copyToDeviceBuffer(&d_xp, h_xp, 0, numAtoms, deviceStream, GpuApiCallBehavior::Sync, nullptr);
    if (updateVelocities)
    {
        copyToDeviceBuffer(&d_v, h_v, 0, numAtoms, deviceStream, GpuApiCallBehavior::Sync, nullptr);
    }
    lincsGpu->apply(
            d_x, d_xp, updateVelocities, d_v, testData->invdt_, testData->computeVirial_, testData->virialScaled_, pbcAiuc);

    copyFromDeviceBuffer(h_xp, &d_xp, 0, numAtoms, deviceStream, GpuApiCallBehavior::Sync, nullptr);
    if (updateVelocities)
    {
        copyFromDeviceBuffer(h_v, &d_v, 0, numAtoms, deviceStream, GpuApiCallBehavior::Sync, nullptr);
    }

    freeDeviceBuffer(&d_x);
    freeDeviceBuffer(&d_xp);
    freeDeviceBuffer(&d_v);
}

} // namespace test
} // namespace gmx
