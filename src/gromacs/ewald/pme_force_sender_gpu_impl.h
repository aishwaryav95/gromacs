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
 * \brief Declaration of class which sends PME Force from GPU memory to PP task
 *
 * \author Alan Gray <alang@nvidia.com>
 *
 * \ingroup module_ewald
 */
#ifndef GMX_PMEFORCESENDERGPU_IMPL_H
#define GMX_PMEFORCESENDERGPU_IMPL_H

#include "gromacs/ewald/pme_force_sender_gpu.h"
#include "gromacs/gpu_utils/devicebuffer_datatype.h"
#include "gromacs/gpu_utils/gputraits.h"
#include "gromacs/utility/arrayref.h"

class GpuEventSynchronizer;

namespace gmx
{

/*! \internal \brief Class with interfaces and data for CUDA version of PME Force sending functionality*/

class PmeForceSenderGpu::Impl
{

public:
    /*! \brief Creates PME GPU Force sender object
     * \param[in] pmeForcesReady  Event synchronizer marked when PME forces are ready on the GPU
     * \param[in] comm            Communicator used for simulation
     * \param[in] ppRanks         List of PP ranks
     */
    Impl(GpuEventSynchronizer* pmeForcesReady, MPI_Comm comm, gmx::ArrayRef<PpRanks> ppRanks);
    ~Impl();

    /*! \brief
     * sends force buffer address to PP rank
     * \param[in] d_f   force buffer in GPU memory
     */
    void sendForceBufferAddressToPpRanks(DeviceBuffer<Float3> d_f);

    /*! \brief
     * Send force synchronizer to PP rank (used with Thread-MPI)
     * \param[in] ppRank           PP rank to receive data
     */
    void sendFSynchronizerToPpCudaDirect(int ppRank);

    /*! \brief
     * Send force to PP rank (used with Lib-MPI)
     * \param[in] sendbuf  force buffer in GPU memory
     * \param[in] offset   starting element in buffer
     * \param[in] numBytes number of bytes to transfer
     * \param[in] ppRank   PP rank to receive data
     * \param[in] request  MPI request to track asynchronous MPI call status
     */
    void sendFToPpCudaMpi(DeviceBuffer<RVec> sendbuf, int offset, int numBytes, int ppRank, MPI_Request* request);

private:
    //! Event indicating when PME forces are ready on the GPU in order for PP stream to sync with the PME stream
    GpuEventSynchronizer* pmeForcesReady_;
    //! communicator for simulation
    MPI_Comm comm_;
    //! list of PP ranks
    gmx::ArrayRef<PpRanks> ppRanks_;
};

} // namespace gmx

#endif
