/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2019,2020,2021, by the GROMACS development team, led by
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
 *
 * \brief Implements update and constraints class using CUDA.
 *
 * The class combines Leap-Frog integrator with LINCS and SETTLE constraints.
 *
 * \todo The computational procedures in members should be integrated to improve
 *       computational performance.
 *
 * \author Artem Zhmurov <zhmurov@gmail.com>
 *
 * \ingroup module_mdlib
 */
#include "gmxpre.h"

#include "update_constrain_gpu_impl.h"

#include <assert.h>
#include <stdio.h>

#include <cmath>

#include <algorithm>

#include "gromacs/gpu_utils/cudautils.cuh"
#include "gromacs/gpu_utils/device_context.h"
#include "gromacs/gpu_utils/device_stream.h"
#include "gromacs/gpu_utils/devicebuffer.h"
#include "gromacs/gpu_utils/gpueventsynchronizer.cuh"
#include "gromacs/gpu_utils/gputraits.cuh"
#include "gromacs/gpu_utils/vectype_ops.cuh"
#include "gromacs/mdlib/leapfrog_gpu.h"
#include "gromacs/mdlib/update_constrain_gpu.h"
#include "gromacs/mdtypes/mdatom.h"
#include "gromacs/timing/wallcycle.h"

namespace gmx
{
/*!\brief Number of CUDA threads in a block
 *
 * \todo Check if using smaller block size will lead to better performance.
 */
constexpr static int c_threadsPerBlock = 256;
//! Maximum number of threads in a block (for __launch_bounds__)
constexpr static int c_maxThreadsPerBlock = c_threadsPerBlock;

__launch_bounds__(c_maxThreadsPerBlock) __global__
        static void scaleCoordinates_kernel(const int numAtoms,
                                            float3* __restrict__ gm_x,
                                            const ScalingMatrix scalingMatrix)
{
    int threadIndex = blockIdx.x * blockDim.x + threadIdx.x;
    if (threadIndex < numAtoms)
    {
        float3 x = gm_x[threadIndex];

        x.x = scalingMatrix.xx * x.x + scalingMatrix.yx * x.y + scalingMatrix.zx * x.z;
        x.y = scalingMatrix.yy * x.y + scalingMatrix.zy * x.z;
        x.z = scalingMatrix.zz * x.z;

        gm_x[threadIndex] = x;
    }
}

void UpdateConstrainGpu::Impl::integrate(GpuEventSynchronizer*             fReadyOnDevice,
                                         const real                        dt,
                                         const bool                        updateVelocities,
                                         const bool                        computeVirial,
                                         tensor                            virial,
                                         const bool                        doTemperatureScaling,
                                         gmx::ArrayRef<const t_grp_tcstat> tcstat,
                                         const bool                        doParrinelloRahman,
                                         const float                       dtPressureCouple,
                                         const matrix                      prVelocityScalingMatrix)
{
    wallcycle_start_nocount(wcycle_, WallCycleCounter::LaunchGpu);
    wallcycle_sub_start(wcycle_, WallCycleSubCounter::LaunchGpuUpdateConstrain);

    // Clearing virial matrix
    // TODO There is no point in having separate virial matrix for constraints
    clear_mat(virial);

    // Make sure that the forces are ready on device before proceeding with the update.
    fReadyOnDevice->enqueueWaitEvent(deviceStream_);

    // The integrate should save a copy of the current coordinates in d_xp_ and write updated
    // once into d_x_. The d_xp_ is only needed by constraints.
    integrator_->integrate(
            d_x_, d_xp_, d_v_, d_f_, dt, doTemperatureScaling, tcstat, doParrinelloRahman, dtPressureCouple, prVelocityScalingMatrix);
    // Constraints need both coordinates before (d_x_) and after (d_xp_) update. However, after constraints
    // are applied, the d_x_ can be discarded. So we intentionally swap the d_x_ and d_xp_ here to avoid the
    // d_xp_ -> d_x_ copy after constraints. Note that the integrate saves them in the wrong order as well.
    lincsGpu_->apply(d_xp_, d_x_, updateVelocities, d_v_, 1.0 / dt, computeVirial, virial, pbcAiuc_);
    settleGpu_->apply(d_xp_, d_x_, updateVelocities, d_v_, 1.0 / dt, computeVirial, virial, pbcAiuc_);

    // scaledVirial -> virial (methods above returns scaled values)
    float scaleFactor = 0.5f / (dt * dt);
    for (int i = 0; i < DIM; i++)
    {
        for (int j = 0; j < DIM; j++)
        {
            virial[i][j] = scaleFactor * virial[i][j];
        }
    }

    coordinatesReady_->markEvent(deviceStream_);

    wallcycle_sub_stop(wcycle_, WallCycleSubCounter::LaunchGpuUpdateConstrain);
    wallcycle_stop(wcycle_, WallCycleCounter::LaunchGpu);

    return;
}

void UpdateConstrainGpu::Impl::scaleCoordinates(const matrix scalingMatrix)
{
    wallcycle_start_nocount(wcycle_, WallCycleCounter::LaunchGpu);
    wallcycle_sub_start(wcycle_, WallCycleSubCounter::LaunchGpuUpdateConstrain);

    ScalingMatrix mu(scalingMatrix);

    const auto kernelArgs = prepareGpuKernelArguments(
            scaleCoordinates_kernel, coordinateScalingKernelLaunchConfig_, &numAtoms_, &d_x_, &mu);
    launchGpuKernel(scaleCoordinates_kernel,
                    coordinateScalingKernelLaunchConfig_,
                    deviceStream_,
                    nullptr,
                    "scaleCoordinates_kernel",
                    kernelArgs);
    // TODO: Although this only happens on the pressure coupling steps, this synchronization
    //       can affect the performance if nstpcouple is small.
    deviceStream_.synchronize();

    wallcycle_sub_stop(wcycle_, WallCycleSubCounter::LaunchGpuUpdateConstrain);
    wallcycle_stop(wcycle_, WallCycleCounter::LaunchGpu);
}

void UpdateConstrainGpu::Impl::scaleVelocities(const matrix scalingMatrix)
{
    wallcycle_start_nocount(wcycle_, WallCycleCounter::LaunchGpu);
    wallcycle_sub_start(wcycle_, WallCycleSubCounter::LaunchGpuUpdateConstrain);

    ScalingMatrix mu(scalingMatrix);

    const auto kernelArgs = prepareGpuKernelArguments(
            scaleCoordinates_kernel, coordinateScalingKernelLaunchConfig_, &numAtoms_, &d_v_, &mu);
    launchGpuKernel(scaleCoordinates_kernel,
                    coordinateScalingKernelLaunchConfig_,
                    deviceStream_,
                    nullptr,
                    "scaleCoordinates_kernel",
                    kernelArgs);
    // TODO: Although this only happens on the pressure coupling steps, this synchronization
    //       can affect the performance if nstpcouple is small.
    deviceStream_.synchronize();

    wallcycle_sub_stop(wcycle_, WallCycleSubCounter::LaunchGpuUpdateConstrain);
    wallcycle_stop(wcycle_, WallCycleCounter::LaunchGpu);
}

UpdateConstrainGpu::Impl::Impl(const t_inputrec&     ir,
                               const gmx_mtop_t&     mtop,
                               const int             numTempScaleValues,
                               const DeviceContext&  deviceContext,
                               const DeviceStream&   deviceStream,
                               GpuEventSynchronizer* xUpdatedOnDevice,
                               gmx_wallcycle*        wcycle) :
    deviceContext_(deviceContext),
    deviceStream_(deviceStream),
    coordinatesReady_(xUpdatedOnDevice),
    wcycle_(wcycle)
{
    GMX_ASSERT(xUpdatedOnDevice != nullptr, "The event synchronizer can not be nullptr.");


    integrator_ = std::make_unique<LeapFrogGpu>(deviceContext_, deviceStream_, numTempScaleValues);
    lincsGpu_ = std::make_unique<LincsGpu>(ir.nLincsIter, ir.nProjOrder, deviceContext_, deviceStream_);
    settleGpu_ = std::make_unique<SettleGpu>(mtop, deviceContext_, deviceStream_);

    coordinateScalingKernelLaunchConfig_.blockSize[0]     = c_threadsPerBlock;
    coordinateScalingKernelLaunchConfig_.blockSize[1]     = 1;
    coordinateScalingKernelLaunchConfig_.blockSize[2]     = 1;
    coordinateScalingKernelLaunchConfig_.sharedMemorySize = 0;
}

UpdateConstrainGpu::Impl::~Impl() {}

void UpdateConstrainGpu::Impl::set(DeviceBuffer<Float3>          d_x,
                                   DeviceBuffer<Float3>          d_v,
                                   const DeviceBuffer<Float3>    d_f,
                                   const InteractionDefinitions& idef,
                                   const t_mdatoms&              md)
{
    // TODO wallcycle
    wallcycle_start_nocount(wcycle_, WallCycleCounter::LaunchGpu);
    wallcycle_sub_start(wcycle_, WallCycleSubCounter::LaunchGpuUpdateConstrain);

    GMX_ASSERT(d_x != nullptr, "Coordinates device buffer should not be null.");
    GMX_ASSERT(d_v != nullptr, "Velocities device buffer should not be null.");
    GMX_ASSERT(d_f != nullptr, "Forces device buffer should not be null.");

    d_x_ = d_x;
    d_v_ = d_v;
    d_f_ = d_f;

    numAtoms_ = md.nr;

    reallocateDeviceBuffer(&d_xp_, numAtoms_, &numXp_, &numXpAlloc_, deviceContext_);

    reallocateDeviceBuffer(
            &d_inverseMasses_, numAtoms_, &numInverseMasses_, &numInverseMassesAlloc_, deviceContext_);

    // Integrator should also update something, but it does not even have a method yet
    integrator_->set(numAtoms_, md.invmass, md.cTC);
    lincsGpu_->set(idef, numAtoms_, md.invmass);
    settleGpu_->set(idef);

    coordinateScalingKernelLaunchConfig_.gridSize[0] =
            (numAtoms_ + c_threadsPerBlock - 1) / c_threadsPerBlock;

    wallcycle_sub_stop(wcycle_, WallCycleSubCounter::LaunchGpuUpdateConstrain);
    wallcycle_stop(wcycle_, WallCycleCounter::LaunchGpu);
}

void UpdateConstrainGpu::Impl::setPbc(const PbcType pbcType, const matrix box)
{
    // TODO wallcycle
    setPbcAiuc(numPbcDimensions(pbcType), box, &pbcAiuc_);
}

GpuEventSynchronizer* UpdateConstrainGpu::Impl::getCoordinatesReadySync()
{
    return coordinatesReady_;
}

UpdateConstrainGpu::UpdateConstrainGpu(const t_inputrec&     ir,
                                       const gmx_mtop_t&     mtop,
                                       const int             numTempScaleValues,
                                       const DeviceContext&  deviceContext,
                                       const DeviceStream&   deviceStream,
                                       GpuEventSynchronizer* xUpdatedOnDevice,
                                       gmx_wallcycle*        wcycle) :
    impl_(new Impl(ir, mtop, numTempScaleValues, deviceContext, deviceStream, xUpdatedOnDevice, wcycle))
{
}

UpdateConstrainGpu::~UpdateConstrainGpu() = default;

void UpdateConstrainGpu::integrate(GpuEventSynchronizer*             fReadyOnDevice,
                                   const real                        dt,
                                   const bool                        updateVelocities,
                                   const bool                        computeVirial,
                                   tensor                            virialScaled,
                                   const bool                        doTemperatureScaling,
                                   gmx::ArrayRef<const t_grp_tcstat> tcstat,
                                   const bool                        doParrinelloRahman,
                                   const float                       dtPressureCouple,
                                   const matrix                      prVelocityScalingMatrix)
{
    impl_->integrate(fReadyOnDevice,
                     dt,
                     updateVelocities,
                     computeVirial,
                     virialScaled,
                     doTemperatureScaling,
                     tcstat,
                     doParrinelloRahman,
                     dtPressureCouple,
                     prVelocityScalingMatrix);
}

void UpdateConstrainGpu::scaleCoordinates(const matrix scalingMatrix)
{
    impl_->scaleCoordinates(scalingMatrix);
}

void UpdateConstrainGpu::scaleVelocities(const matrix scalingMatrix)
{
    impl_->scaleVelocities(scalingMatrix);
}

void UpdateConstrainGpu::set(DeviceBuffer<Float3>          d_x,
                             DeviceBuffer<Float3>          d_v,
                             const DeviceBuffer<Float3>    d_f,
                             const InteractionDefinitions& idef,
                             const t_mdatoms&              md)
{
    impl_->set(d_x, d_v, d_f, idef, md);
}

void UpdateConstrainGpu::setPbc(const PbcType pbcType, const matrix box)
{
    impl_->setPbc(pbcType, box);
}

GpuEventSynchronizer* UpdateConstrainGpu::getCoordinatesReadySync()
{
    return impl_->getCoordinatesReadySync();
}

bool UpdateConstrainGpu::isNumCoupledConstraintsSupported(const gmx_mtop_t& mtop)
{
    return LincsGpu::isNumCoupledConstraintsSupported(mtop);
}

} // namespace gmx
