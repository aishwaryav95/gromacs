/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2012,2013,2014,2015,2016 by the GROMACS development team.
 * Copyright (c) 2017,2018,2019,2020,2021, by the GROMACS development team, led by
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

#include "gmxpre.h"

#include "gromacs/gmxlib/nrnb.h"
#include "gromacs/gmxlib/nonbonded/nb_free_energy.h"
#include "gromacs/gmxlib/nonbonded/nonbonded.h"
#include "gromacs/math/vectypes.h"
#include "gromacs/mdlib/enerdata_utils.h"
#include "gromacs/mdlib/force.h"
#include "gromacs/mdlib/gmx_omp_nthreads.h"
#include "gromacs/mdtypes/enerdata.h"
#include "gromacs/mdtypes/forceoutput.h"
#include "gromacs/mdtypes/forcerec.h"
#include "gromacs/mdtypes/inputrec.h"
#include "gromacs/mdtypes/interaction_const.h"
#include "gromacs/mdtypes/md_enums.h"
#include "gromacs/mdtypes/mdatom.h"
#include "gromacs/mdtypes/nblist.h"
#include "gromacs/mdtypes/simulation_workload.h"
#include "gromacs/nbnxm/gpu_data_mgmt.h"
#include "gromacs/nbnxm/nbnxm.h"
#include "gromacs/simd/simd.h"
#include "gromacs/timing/wallcycle.h"
#include "gromacs/utility/enumerationhelpers.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/gmxassert.h"
#include "gromacs/utility/real.h"

#include "kernel_common.h"
#include "nbnxm_gpu.h"
#include "nbnxm_simd.h"
#include "pairlistset.h"
#include "pairlistsets.h"
#include "kernels_reference/kernel_gpu_ref.h"
#define INCLUDE_KERNELFUNCTION_TABLES
#include "kernels_reference/kernel_ref.h"
#ifdef GMX_NBNXN_SIMD_2XNN
#    include "kernels_simd_2xmm/kernels.h"
#endif
#ifdef GMX_NBNXN_SIMD_4XN
#    include "kernels_simd_4xm/kernels.h"
#endif
#undef INCLUDE_FUNCTION_TABLES

/*! \brief Clears the energy group output buffers
 *
 * \param[in,out] out  nbnxn kernel output struct
 */
static void clearGroupEnergies(nbnxn_atomdata_output_t* out)
{
    std::fill(out->Vvdw.begin(), out->Vvdw.end(), 0.0_real);
    std::fill(out->Vc.begin(), out->Vc.end(), 0.0_real);
    std::fill(out->VSvdw.begin(), out->VSvdw.end(), 0.0_real);
    std::fill(out->VSc.begin(), out->VSc.end(), 0.0_real);
}

/*! \brief Reduce the group-pair energy buffers produced by a SIMD kernel
 * to single terms in the output buffers.
 *
 * The SIMD kernels produce a large number of energy buffer in SIMD registers
 * to avoid scattered reads and writes.
 *
 * \tparam        unrollj         The unroll size for j-particles in the SIMD kernel
 * \param[in]     numGroups       The number of energy groups
 * \param[in]     numGroups_2log  Log2 of numGroups, rounded up
 * \param[in,out] out             Struct with energy buffers
 */
template<int unrollj>
static void reduceGroupEnergySimdBuffers(int numGroups, int numGroups_2log, nbnxn_atomdata_output_t* out)
{
    const int unrollj_half = unrollj / 2;
    /* Energies are stored in SIMD registers with size 2^numGroups_2log */
    const int numGroupsStorage = (1 << numGroups_2log);

    const real* gmx_restrict vVdwSimd     = out->VSvdw.data();
    const real* gmx_restrict vCoulombSimd = out->VSc.data();
    real* gmx_restrict vVdw               = out->Vvdw.data();
    real* gmx_restrict vCoulomb           = out->Vc.data();

    /* The size of the SIMD energy group buffer array is:
     * numGroups*numGroups*numGroupsStorage*unrollj_half*simd_width
     */
    for (int i = 0; i < numGroups; i++)
    {
        for (int j1 = 0; j1 < numGroups; j1++)
        {
            for (int j0 = 0; j0 < numGroups; j0++)
            {
                int c = ((i * numGroups + j1) * numGroupsStorage + j0) * unrollj_half * unrollj;
                for (int s = 0; s < unrollj_half; s++)
                {
                    vVdw[i * numGroups + j0] += vVdwSimd[c + 0];
                    vVdw[i * numGroups + j1] += vVdwSimd[c + 1];
                    vCoulomb[i * numGroups + j0] += vCoulombSimd[c + 0];
                    vCoulomb[i * numGroups + j1] += vCoulombSimd[c + 1];
                    c += unrollj + 2;
                }
            }
        }
    }
}

static int getCoulombKernelType(const Nbnxm::KernelSetup& kernelSetup, const interaction_const_t& ic)
{

    if (EEL_RF(ic.eeltype) || ic.eeltype == CoulombInteractionType::Cut)
    {
        return coulktRF;
    }
    else
    {
        if (kernelSetup.ewaldExclusionType == Nbnxm::EwaldExclusionType::Table)
        {
            if (ic.rcoulomb == ic.rvdw)
            {
                return coulktTAB;
            }
            else
            {
                return coulktTAB_TWIN;
            }
        }
        else
        {
            if (ic.rcoulomb == ic.rvdw)
            {
                return coulktEWALD;
            }
            else
            {
                return coulktEWALD_TWIN;
            }
        }
    }
}

static int getVdwKernelType(const Nbnxm::KernelSetup&       kernelSetup,
                            const nbnxn_atomdata_t::Params& nbatParams,
                            const interaction_const_t&      ic)
{
    if (ic.vdwtype == VanDerWaalsType::Cut)
    {
        switch (ic.vdw_modifier)
        {
            case InteractionModifiers::None:
            case InteractionModifiers::PotShift:
                switch (nbatParams.ljCombinationRule)
                {
                    case LJCombinationRule::Geometric: return vdwktLJCUT_COMBGEOM;
                    case LJCombinationRule::LorentzBerthelot: return vdwktLJCUT_COMBLB;
                    case LJCombinationRule::None: return vdwktLJCUT_COMBNONE;
                    default: gmx_incons("Unknown combination rule");
                }
            case InteractionModifiers::ForceSwitch: return vdwktLJFORCESWITCH;
            case InteractionModifiers::PotSwitch: return vdwktLJPOTSWITCH;
            default:
                std::string errorMsg =
                        gmx::formatString("Unsupported VdW interaction modifier %s (%d)",
                                          enumValueToString(ic.vdw_modifier),
                                          static_cast<int>(ic.vdw_modifier));
                gmx_incons(errorMsg);
        }
    }
    else if (ic.vdwtype == VanDerWaalsType::Pme)
    {
        if (ic.ljpme_comb_rule == LongRangeVdW::Geom)
        {
            return vdwktLJEWALDCOMBGEOM;
        }
        else
        {
            /* At setup we (should have) selected the C reference kernel */
            GMX_RELEASE_ASSERT(kernelSetup.kernelType == Nbnxm::KernelType::Cpu4x4_PlainC,
                               "Only the C reference nbnxn SIMD kernel supports LJ-PME with LB "
                               "combination rules");
            return vdwktLJEWALDCOMBLB;
        }
    }
    else
    {
        std::string errorMsg = gmx::formatString("Unsupported VdW interaction type %s (%d)",
                                                 enumValueToString(ic.vdwtype),
                                                 static_cast<int>(ic.vdwtype));
        gmx_incons(errorMsg);
    }
}

/*! \brief Dispatches the non-bonded N versus M atom cluster CPU kernels.
 *
 * OpenMP parallelization is performed within this function.
 * Energy reduction, but not force and shift force reduction, is performed
 * within this function.
 *
 * \param[in]     pairlistSet   Pairlists with local or non-local interactions to compute
 * \param[in]     kernelSetup   The non-bonded kernel setup
 * \param[in,out] nbat          The atomdata for the interactions
 * \param[in]     ic            Non-bonded interaction constants
 * \param[in]     shiftVectors  The PBC shift vectors
 * \param[in]     stepWork      Flags that tell what to compute
 * \param[in]     clearF        Enum that tells if to clear the force output buffer
 * \param[out]    vCoulomb      Output buffer for Coulomb energies
 * \param[out]    vVdw          Output buffer for Van der Waals energies
 * \param[in]     wcycle        Pointer to cycle counting data structure.
 */
static void nbnxn_kernel_cpu(const PairlistSet&             pairlistSet,
                             const Nbnxm::KernelSetup&      kernelSetup,
                             nbnxn_atomdata_t*              nbat,
                             const interaction_const_t&     ic,
                             gmx::ArrayRef<const gmx::RVec> shiftVectors,
                             const gmx::StepWorkload&       stepWork,
                             int                            clearF,
                             real*                          vCoulomb,
                             real*                          vVdw,
                             gmx_wallcycle*                 wcycle)
{

    const nbnxn_atomdata_t::Params& nbatParams = nbat->params();

    const int coulkt = getCoulombKernelType(kernelSetup, ic);
    const int vdwkt  = getVdwKernelType(kernelSetup, nbatParams, ic);

    gmx::ArrayRef<const NbnxnPairlistCpu> pairlists = pairlistSet.cpuLists();

    const auto* shiftVecPointer = as_rvec_array(shiftVectors.data());

    int gmx_unused nthreads = gmx_omp_nthreads_get(ModuleMultiThread::Nonbonded);
    wallcycle_sub_start(wcycle, WallCycleSubCounter::NonbondedClear);
#pragma omp parallel for schedule(static) num_threads(nthreads)
    for (gmx::index nb = 0; nb < pairlists.ssize(); nb++)
    {
        // Presently, the kernels do not call C++ code that can throw,
        // so no need for a try/catch pair in this OpenMP region.
        nbnxn_atomdata_output_t* out = &nbat->out[nb];

        if (clearF == enbvClearFYes)
        {
            clearForceBuffer(nbat, nb);

            clear_fshift(out->fshift.data());
        }

        if (nb == 0)
        {
            wallcycle_sub_stop(wcycle, WallCycleSubCounter::NonbondedClear);
            wallcycle_sub_start(wcycle, WallCycleSubCounter::NonbondedKernel);
        }

        // TODO: Change to reference
        const NbnxnPairlistCpu* pairlist = &pairlists[nb];

        if (!stepWork.computeEnergy)
        {
            /* Don't calculate energies */
            switch (kernelSetup.kernelType)
            {
                case Nbnxm::KernelType::Cpu4x4_PlainC:
                    nbnxn_kernel_noener_ref[coulkt][vdwkt](pairlist, nbat, &ic, shiftVecPointer, out);
                    break;
#ifdef GMX_NBNXN_SIMD_2XNN
                case Nbnxm::KernelType::Cpu4xN_Simd_2xNN:
                    nbnxm_kernel_noener_simd_2xmm[coulkt][vdwkt](pairlist, nbat, &ic, shiftVecPointer, out);
                    break;
#endif
#ifdef GMX_NBNXN_SIMD_4XN
                case Nbnxm::KernelType::Cpu4xN_Simd_4xN:
                    nbnxm_kernel_noener_simd_4xm[coulkt][vdwkt](pairlist, nbat, &ic, shiftVecPointer, out);
                    break;
#endif
                default: GMX_RELEASE_ASSERT(false, "Unsupported kernel architecture");
            }
        }
        else if (out->Vvdw.size() == 1)
        {
            /* A single energy group (pair) */
            out->Vvdw[0] = 0;
            out->Vc[0]   = 0;

            switch (kernelSetup.kernelType)
            {
                case Nbnxm::KernelType::Cpu4x4_PlainC:
                    nbnxn_kernel_ener_ref[coulkt][vdwkt](pairlist, nbat, &ic, shiftVecPointer, out);
                    break;
#ifdef GMX_NBNXN_SIMD_2XNN
                case Nbnxm::KernelType::Cpu4xN_Simd_2xNN:
                    nbnxm_kernel_ener_simd_2xmm[coulkt][vdwkt](pairlist, nbat, &ic, shiftVecPointer, out);
                    break;
#endif
#ifdef GMX_NBNXN_SIMD_4XN
                case Nbnxm::KernelType::Cpu4xN_Simd_4xN:
                    nbnxm_kernel_ener_simd_4xm[coulkt][vdwkt](pairlist, nbat, &ic, shiftVecPointer, out);
                    break;
#endif
                default: GMX_RELEASE_ASSERT(false, "Unsupported kernel architecture");
            }
        }
        else
        {
            /* Calculate energy group contributions */
            clearGroupEnergies(out);

            int unrollj = 0;

            switch (kernelSetup.kernelType)
            {
                case Nbnxm::KernelType::Cpu4x4_PlainC:
                    unrollj = c_nbnxnCpuIClusterSize;
                    nbnxn_kernel_energrp_ref[coulkt][vdwkt](pairlist, nbat, &ic, shiftVecPointer, out);
                    break;
#ifdef GMX_NBNXN_SIMD_2XNN
                case Nbnxm::KernelType::Cpu4xN_Simd_2xNN:
                    unrollj = GMX_SIMD_REAL_WIDTH / 2;
                    nbnxm_kernel_energrp_simd_2xmm[coulkt][vdwkt](
                            pairlist, nbat, &ic, shiftVecPointer, out);
                    break;
#endif
#ifdef GMX_NBNXN_SIMD_4XN
                case Nbnxm::KernelType::Cpu4xN_Simd_4xN:
                    unrollj = GMX_SIMD_REAL_WIDTH;
                    nbnxm_kernel_energrp_simd_4xm[coulkt][vdwkt](pairlist, nbat, &ic, shiftVecPointer, out);
                    break;
#endif
                default: GMX_RELEASE_ASSERT(false, "Unsupported kernel architecture");
            }

            if (kernelSetup.kernelType != Nbnxm::KernelType::Cpu4x4_PlainC)
            {
                switch (unrollj)
                {
                    case 2:
                        reduceGroupEnergySimdBuffers<2>(nbatParams.nenergrp, nbatParams.neg_2log, out);
                        break;
                    case 4:
                        reduceGroupEnergySimdBuffers<4>(nbatParams.nenergrp, nbatParams.neg_2log, out);
                        break;
                    case 8:
                        reduceGroupEnergySimdBuffers<8>(nbatParams.nenergrp, nbatParams.neg_2log, out);
                        break;
                    default: GMX_RELEASE_ASSERT(false, "Unsupported j-unroll size");
                }
            }
        }
    }
    wallcycle_sub_stop(wcycle, WallCycleSubCounter::NonbondedKernel);

    if (stepWork.computeEnergy)
    {
        reduce_energies_over_lists(nbat, pairlists.ssize(), vVdw, vCoulomb);
    }
}

static void accountFlops(t_nrnb*                    nrnb,
                         const PairlistSet&         pairlistSet,
                         const nonbonded_verlet_t&  nbv,
                         const interaction_const_t& ic,
                         const gmx::StepWorkload&   stepWork)
{
    const bool usingGpuKernels = nbv.useGpu();

    int enr_nbnxn_kernel_ljc = eNRNB;
    if (EEL_RF(ic.eeltype) || ic.eeltype == CoulombInteractionType::Cut)
    {
        enr_nbnxn_kernel_ljc = eNR_NBNXN_LJ_RF;
    }
    else if ((!usingGpuKernels && nbv.kernelSetup().ewaldExclusionType == Nbnxm::EwaldExclusionType::Analytical)
             || (usingGpuKernels && Nbnxm::gpu_is_kernel_ewald_analytical(nbv.gpu_nbv)))
    {
        enr_nbnxn_kernel_ljc = eNR_NBNXN_LJ_EWALD;
    }
    else
    {
        enr_nbnxn_kernel_ljc = eNR_NBNXN_LJ_TAB;
    }
    int enr_nbnxn_kernel_lj = eNR_NBNXN_LJ;
    if (stepWork.computeEnergy)
    {
        /* In eNR_??? the nbnxn F+E kernels are always the F kernel + 1 */
        enr_nbnxn_kernel_ljc += 1;
        enr_nbnxn_kernel_lj += 1;
    }

    inc_nrnb(nrnb, enr_nbnxn_kernel_ljc, pairlistSet.natpair_ljq_);
    inc_nrnb(nrnb, enr_nbnxn_kernel_lj, pairlistSet.natpair_lj_);
    /* The Coulomb-only kernels are offset -eNR_NBNXN_LJ_RF+eNR_NBNXN_RF */
    inc_nrnb(nrnb, enr_nbnxn_kernel_ljc - eNR_NBNXN_LJ_RF + eNR_NBNXN_RF, pairlistSet.natpair_q_);

    if (ic.vdw_modifier == InteractionModifiers::ForceSwitch)
    {
        /* We add up the switch cost separately */
        inc_nrnb(nrnb,
                 eNR_NBNXN_ADD_LJ_FSW + (stepWork.computeEnergy ? 1 : 0),
                 pairlistSet.natpair_ljq_ + pairlistSet.natpair_lj_);
    }
    if (ic.vdw_modifier == InteractionModifiers::PotSwitch)
    {
        /* We add up the switch cost separately */
        inc_nrnb(nrnb,
                 eNR_NBNXN_ADD_LJ_PSW + (stepWork.computeEnergy ? 1 : 0),
                 pairlistSet.natpair_ljq_ + pairlistSet.natpair_lj_);
    }
    if (ic.vdwtype == VanDerWaalsType::Pme)
    {
        /* We add up the LJ Ewald cost separately */
        inc_nrnb(nrnb,
                 eNR_NBNXN_ADD_LJ_EWALD + (stepWork.computeEnergy ? 1 : 0),
                 pairlistSet.natpair_ljq_ + pairlistSet.natpair_lj_);
    }
}

void nonbonded_verlet_t::dispatchNonbondedKernel(gmx::InteractionLocality       iLocality,
                                                 const interaction_const_t&     ic,
                                                 const gmx::StepWorkload&       stepWork,
                                                 int                            clearF,
                                                 gmx::ArrayRef<const gmx::RVec> shiftvec,
                                                 gmx::ArrayRef<real> repulsionDispersionSR,
                                                 gmx::ArrayRef<real> CoulombSR,
                                                 t_nrnb*             nrnb) const
{
    const PairlistSet& pairlistSet = pairlistSets().pairlistSet(iLocality);

    switch (kernelSetup().kernelType)
    {
        case Nbnxm::KernelType::Cpu4x4_PlainC:
        case Nbnxm::KernelType::Cpu4xN_Simd_4xN:
        case Nbnxm::KernelType::Cpu4xN_Simd_2xNN:
            nbnxn_kernel_cpu(pairlistSet,
                             kernelSetup(),
                             nbat.get(),
                             ic,
                             shiftvec,
                             stepWork,
                             clearF,
                             CoulombSR.data(),
                             repulsionDispersionSR.data(),
                             wcycle_);
            break;

        case Nbnxm::KernelType::Gpu8x8x8:
            Nbnxm::gpu_launch_kernel(gpu_nbv, stepWork, iLocality);
            break;

        case Nbnxm::KernelType::Cpu8x8x8_PlainC:
            nbnxn_kernel_gpu_ref(pairlistSet.gpuList(),
                                 nbat.get(),
                                 &ic,
                                 shiftvec,
                                 stepWork,
                                 clearF,
                                 nbat->out[0].f,
                                 nbat->out[0].fshift.data(),
                                 CoulombSR.data(),
                                 repulsionDispersionSR.data());
            break;

        default: GMX_RELEASE_ASSERT(false, "Invalid nonbonded kernel type passed!");
    }

    accountFlops(nrnb, pairlistSet, *this, ic, stepWork);
}

void nonbonded_verlet_t::dispatchFreeEnergyKernel(gmx::InteractionLocality       iLocality,
                                                  const t_forcerec&              fr,
                                                  gmx::ArrayRef<const gmx::RVec> coords,
                                                  gmx::ForceWithShiftForces* forceWithShiftForces,
                                                  gmx::ArrayRef<const real>  chargeA,
                                                  gmx::ArrayRef<const real>  chargeB,
                                                  gmx::ArrayRef<const int>   typeA,
                                                  gmx::ArrayRef<const int>   typeB,
                                                  t_lambda*                  fepvals,
                                                  gmx::ArrayRef<const real>  lambda,
                                                  gmx_enerdata_t*            enerd,
                                                  const gmx::StepWorkload&   stepWork,
                                                  t_nrnb*                    nrnb)
{
    const auto nbl_fep = pairlistSets().pairlistSet(iLocality).fepLists();

    /* When the first list is empty, all are empty and there is nothing to do */
    if (!pairlistSets().params().haveFep || nbl_fep[0]->nrj == 0)
    {
        return;
    }

    int donb_flags = 0;
    /* Add short-range interactions */
    donb_flags |= GMX_NONBONDED_DO_SR;

    if (stepWork.computeForces)
    {
        donb_flags |= GMX_NONBONDED_DO_FORCE;
    }
    if (stepWork.computeVirial)
    {
        donb_flags |= GMX_NONBONDED_DO_SHIFTFORCE;
    }
    if (stepWork.computeEnergy)
    {
        donb_flags |= GMX_NONBONDED_DO_POTENTIAL;
    }

    gmx::EnumerationArray<FreeEnergyPerturbationCouplingType, real> dvdl_nb      = { 0 };
    int                                                             kernelFlags  = donb_flags;
    gmx::ArrayRef<const real>                                       kernelLambda = lambda;
    gmx::ArrayRef<real>                                             kernelDvdl   = dvdl_nb;

    gmx::ArrayRef<real> energygrp_elec = enerd->grpp.energyGroupPairTerms[NonBondedEnergyTerms::CoulombSR];
    gmx::ArrayRef<real> energygrp_vdw = enerd->grpp.energyGroupPairTerms[NonBondedEnergyTerms::LJSR];

    GMX_ASSERT(gmx_omp_nthreads_get(ModuleMultiThread::Nonbonded) == nbl_fep.ssize(),
               "Number of lists should be same as number of NB threads");

    wallcycle_sub_start(wcycle_, WallCycleSubCounter::NonbondedFep);
#pragma omp parallel for schedule(static) num_threads(nbl_fep.ssize())
    for (gmx::index th = 0; th < nbl_fep.ssize(); th++)
    {
        try
        {
            gmx_nb_free_energy_kernel(*nbl_fep[th],
                                      coords,
                                      forceWithShiftForces,
                                      fr.use_simd_kernels,
                                      fr.ntype,
                                      fr.rlist,
                                      *fr.ic,
                                      fr.shift_vec,
                                      fr.nbfp,
                                      fr.ljpme_c6grid,
                                      chargeA,
                                      chargeB,
                                      typeA,
                                      typeB,
                                      kernelFlags,
                                      kernelLambda,
                                      kernelDvdl,
                                      energygrp_elec,
                                      energygrp_vdw,
                                      nrnb);
        }
        GMX_CATCH_ALL_AND_EXIT_WITH_FATAL_ERROR
    }

    if (fepvals->sc_alpha != 0)
    {
        enerd->dvdl_nonlin[FreeEnergyPerturbationCouplingType::Vdw] +=
                dvdl_nb[FreeEnergyPerturbationCouplingType::Vdw];
        enerd->dvdl_nonlin[FreeEnergyPerturbationCouplingType::Coul] +=
                dvdl_nb[FreeEnergyPerturbationCouplingType::Coul];
    }
    else
    {
        enerd->dvdl_lin[FreeEnergyPerturbationCouplingType::Vdw] +=
                dvdl_nb[FreeEnergyPerturbationCouplingType::Vdw];
        enerd->dvdl_lin[FreeEnergyPerturbationCouplingType::Coul] +=
                dvdl_nb[FreeEnergyPerturbationCouplingType::Coul];
    }

    /* If we do foreign lambda and we have soft-core interactions
     * we have to recalculate the (non-linear) energies contributions.
     */
    if (fepvals->n_lambda > 0 && stepWork.computeDhdl && fepvals->sc_alpha != 0)
    {
        gmx::EnumerationArray<FreeEnergyPerturbationCouplingType, real> lam_i;
        kernelFlags = (donb_flags & ~(GMX_NONBONDED_DO_FORCE | GMX_NONBONDED_DO_SHIFTFORCE))
                      | GMX_NONBONDED_DO_FOREIGNLAMBDA;
        kernelLambda = lam_i;
        kernelDvdl   = dvdl_nb;
        gmx::ArrayRef<real> energygrp_elec =
                enerd->foreign_grpp.energyGroupPairTerms[NonBondedEnergyTerms::CoulombSR];
        gmx::ArrayRef<real> energygrp_vdw =
                enerd->foreign_grpp.energyGroupPairTerms[NonBondedEnergyTerms::LJSR];

        for (gmx::index i = 0; i < 1 + enerd->foreignLambdaTerms.numLambdas(); i++)
        {
            std::fill(std::begin(dvdl_nb), std::end(dvdl_nb), 0);
            for (int j = 0; j < static_cast<int>(FreeEnergyPerturbationCouplingType::Count); j++)
            {
                lam_i[j] = (i == 0 ? lambda[j] : fepvals->all_lambda[j][i - 1]);
            }
            reset_foreign_enerdata(enerd);
#pragma omp parallel for schedule(static) num_threads(nbl_fep.ssize())
            for (gmx::index th = 0; th < nbl_fep.ssize(); th++)
            {
                try
                {
                    gmx_nb_free_energy_kernel(*nbl_fep[th],
                                              coords,
                                              forceWithShiftForces,
                                              fr.use_simd_kernels,
                                              fr.ntype,
                                              fr.rlist,
                                              *fr.ic,
                                              fr.shift_vec,
                                              fr.nbfp,
                                              fr.ljpme_c6grid,
                                              chargeA,
                                              chargeB,
                                              typeA,
                                              typeB,
                                              kernelFlags,
                                              kernelLambda,
                                              kernelDvdl,
                                              energygrp_elec,
                                              energygrp_vdw,
                                              nrnb);
                }
                GMX_CATCH_ALL_AND_EXIT_WITH_FATAL_ERROR
            }

            sum_epot(enerd->foreign_grpp, enerd->foreign_term.data());
            enerd->foreignLambdaTerms.accumulate(
                    i,
                    enerd->foreign_term[F_EPOT],
                    dvdl_nb[FreeEnergyPerturbationCouplingType::Vdw]
                            + dvdl_nb[FreeEnergyPerturbationCouplingType::Coul]);
        }
    }
    wallcycle_sub_stop(wcycle_, WallCycleSubCounter::NonbondedFep);
}
