/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team.
 * Copyright (c) 2013,2014,2015,2016,2017 by the GROMACS development team.
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
#include "gmxpre.h"

#include "force.h"

#include <cassert>
#include <cmath>
#include <cstring>

#include "gromacs/domdec/dlbtiming.h"
#include "gromacs/domdec/domdec.h"
#include "gromacs/domdec/domdec_struct.h"
#include "gromacs/ewald/ewald.h"
#include "gromacs/ewald/long_range_correction.h"
#include "gromacs/ewald/pme.h"
#include "gromacs/gmxlib/network.h"
#include "gromacs/gmxlib/nrnb.h"
#include "gromacs/math/vec.h"
#include "gromacs/math/vecdump.h"
#include "gromacs/mdlib/forcerec_threading.h"
#include "gromacs/mdtypes/commrec.h"
#include "gromacs/mdtypes/enerdata.h"
#include "gromacs/mdtypes/forceoutput.h"
#include "gromacs/mdtypes/forcerec.h"
#include "gromacs/mdtypes/inputrec.h"
#include "gromacs/mdtypes/interaction_const.h"
#include "gromacs/mdtypes/md_enums.h"
#include "gromacs/mdtypes/mdatom.h"
#include "gromacs/mdtypes/simulation_workload.h"
#include "gromacs/pbcutil/ishift.h"
#include "gromacs/pbcutil/pbc.h"
#include "gromacs/timing/wallcycle.h"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/smalloc.h"

using gmx::ArrayRef;
using gmx::RVec;

static void clearEwaldThreadOutput(ewald_corr_thread_t* ewc_t)
{
    ewc_t->Vcorr_q                                        = 0;
    ewc_t->Vcorr_lj                                       = 0;
    ewc_t->dvdl[FreeEnergyPerturbationCouplingType::Coul] = 0;
    ewc_t->dvdl[FreeEnergyPerturbationCouplingType::Vdw]  = 0;
    clear_mat(ewc_t->vir_q);
    clear_mat(ewc_t->vir_lj);
}

static void reduceEwaldThreadOuput(int nthreads, gmx::ArrayRef<ewald_corr_thread_t> ewc_t)
{
    ewald_corr_thread_t& dest = ewc_t[0];

    for (int t = 1; t < nthreads; t++)
    {
        dest.Vcorr_q += ewc_t[t].Vcorr_q;
        dest.Vcorr_lj += ewc_t[t].Vcorr_lj;
        dest.dvdl[FreeEnergyPerturbationCouplingType::Coul] +=
                ewc_t[t].dvdl[FreeEnergyPerturbationCouplingType::Coul];
        dest.dvdl[FreeEnergyPerturbationCouplingType::Vdw] +=
                ewc_t[t].dvdl[FreeEnergyPerturbationCouplingType::Vdw];
        m_add(dest.vir_q, ewc_t[t].vir_q, dest.vir_q);
        m_add(dest.vir_lj, ewc_t[t].vir_lj, dest.vir_lj);
    }
}

void calculateLongRangeNonbondeds(t_forcerec*                    fr,
                                  const t_inputrec&              ir,
                                  const t_commrec*               cr,
                                  t_nrnb*                        nrnb,
                                  gmx_wallcycle*                 wcycle,
                                  const t_mdatoms*               md,
                                  gmx::ArrayRef<const RVec>      coordinates,
                                  gmx::ForceWithVirial*          forceWithVirial,
                                  gmx_enerdata_t*                enerd,
                                  const matrix                   box,
                                  gmx::ArrayRef<const real>      lambda,
                                  gmx::ArrayRef<const gmx::RVec> mu_tot,
                                  const gmx::StepWorkload&       stepWork,
                                  const DDBalanceRegionHandler&  ddBalanceRegionHandler)
{
    const bool computePmeOnCpu = (EEL_PME(fr->ic->eeltype) || EVDW_PME(fr->ic->vdwtype))
                                 && thisRankHasDuty(cr, DUTY_PME)
                                 && (pme_run_mode(fr->pmedata) == PmeRunMode::CPU);

    const bool haveEwaldSurfaceTerm = haveEwaldSurfaceContribution(ir);

    /* Do long-range electrostatics and/or LJ-PME
     * and compute PME surface terms when necessary.
     */
    if ((computePmeOnCpu || fr->ic->eeltype == CoulombInteractionType::Ewald || haveEwaldSurfaceTerm)
        && stepWork.computeNonbondedForces)
    {
        int  status = 0;
        real Vlr_q = 0, Vlr_lj = 0;

        /* We reduce all virial, dV/dlambda and energy contributions, except
         * for the reciprocal energies (Vlr_q, Vlr_lj) into the same struct.
         */
        ewald_corr_thread_t& ewaldOutput = fr->ewc_t[0];
        clearEwaldThreadOutput(&ewaldOutput);

        if (EEL_PME_EWALD(fr->ic->eeltype) || EVDW_PME(fr->ic->vdwtype))
        {
            /* Calculate the Ewald surface force and energy contributions, when necessary */
            if (haveEwaldSurfaceTerm)
            {
                wallcycle_sub_start(wcycle, WallCycleSubCounter::EwaldCorrection);

                int nthreads = fr->nthread_ewc;
#pragma omp parallel for num_threads(nthreads) schedule(static)
                for (int t = 0; t < nthreads; t++)
                {
                    try
                    {
                        ewald_corr_thread_t& ewc_t = fr->ewc_t[t];
                        if (t > 0)
                        {
                            clearEwaldThreadOutput(&ewc_t);
                        }

                        /* Threading is only supported with the Verlet cut-off
                         * scheme and then only single particle forces (no
                         * exclusion forces) are calculated, so we can store
                         * the forces in the normal, single forceWithVirial->force_ array.
                         */
                        ewald_LRcorrection(
                                md->homenr,
                                cr,
                                nthreads,
                                t,
                                *fr,
                                ir,
                                md->chargeA ? gmx::constArrayRefFromArray(md->chargeA, md->nr)
                                            : gmx::ArrayRef<const real>{},
                                md->chargeB ? gmx::constArrayRefFromArray(md->chargeB, md->nr)
                                            : gmx::ArrayRef<const real>{},
                                (md->nChargePerturbed != 0),
                                coordinates,
                                box,
                                mu_tot,
                                forceWithVirial->force_,
                                &ewc_t.Vcorr_q,
                                lambda[static_cast<int>(FreeEnergyPerturbationCouplingType::Coul)],
                                &ewc_t.dvdl[FreeEnergyPerturbationCouplingType::Coul]);
                    }
                    GMX_CATCH_ALL_AND_EXIT_WITH_FATAL_ERROR
                }
                if (nthreads > 1)
                {
                    reduceEwaldThreadOuput(nthreads, fr->ewc_t);
                }
                wallcycle_sub_stop(wcycle, WallCycleSubCounter::EwaldCorrection);
            }

            if (EEL_PME_EWALD(fr->ic->eeltype) && fr->n_tpi == 0)
            {
                /* This is not in a subcounter because it takes a
                   negligible and constant-sized amount of time */
                ewaldOutput.Vcorr_q += ewald_charge_correction(
                        cr,
                        fr,
                        lambda[static_cast<int>(FreeEnergyPerturbationCouplingType::Coul)],
                        box,
                        &ewaldOutput.dvdl[FreeEnergyPerturbationCouplingType::Coul],
                        ewaldOutput.vir_q);
            }

            if (computePmeOnCpu)
            {
                /* Do reciprocal PME for Coulomb and/or LJ. */
                assert(fr->n_tpi >= 0);
                if (fr->n_tpi == 0 || stepWork.stateChanged)
                {
                    /* With domain decomposition we close the CPU side load
                     * balancing region here, because PME does global
                     * communication that acts as a global barrier.
                     */
                    ddBalanceRegionHandler.closeAfterForceComputationCpu();

                    wallcycle_start(wcycle, WallCycleCounter::PmeMesh);
                    status = gmx_pme_do(
                            fr->pmedata,
                            gmx::constArrayRefFromArray(coordinates.data(), md->homenr - fr->n_tpi),
                            forceWithVirial->force_,
                            md->chargeA ? gmx::constArrayRefFromArray(md->chargeA, md->nr)
                                        : gmx::ArrayRef<const real>{},
                            md->chargeB ? gmx::constArrayRefFromArray(md->chargeB, md->nr)
                                        : gmx::ArrayRef<const real>{},
                            md->sqrt_c6A ? gmx::constArrayRefFromArray(md->sqrt_c6A, md->nr)
                                         : gmx::ArrayRef<const real>{},
                            md->sqrt_c6B ? gmx::constArrayRefFromArray(md->sqrt_c6B, md->nr)
                                         : gmx::ArrayRef<const real>{},
                            md->sigmaA ? gmx::constArrayRefFromArray(md->sigmaA, md->nr)
                                       : gmx::ArrayRef<const real>{},
                            md->sigmaB ? gmx::constArrayRefFromArray(md->sigmaB, md->nr)
                                       : gmx::ArrayRef<const real>{},
                            box,
                            cr,
                            DOMAINDECOMP(cr) ? dd_pme_maxshift_x(*cr->dd) : 0,
                            DOMAINDECOMP(cr) ? dd_pme_maxshift_y(*cr->dd) : 0,
                            nrnb,
                            wcycle,
                            ewaldOutput.vir_q,
                            ewaldOutput.vir_lj,
                            &Vlr_q,
                            &Vlr_lj,
                            lambda[static_cast<int>(FreeEnergyPerturbationCouplingType::Coul)],
                            lambda[static_cast<int>(FreeEnergyPerturbationCouplingType::Vdw)],
                            &ewaldOutput.dvdl[FreeEnergyPerturbationCouplingType::Coul],
                            &ewaldOutput.dvdl[FreeEnergyPerturbationCouplingType::Vdw],
                            stepWork);
                    wallcycle_stop(wcycle, WallCycleCounter::PmeMesh);
                    if (status != 0)
                    {
                        gmx_fatal(FARGS, "Error %d in reciprocal PME routine", status);
                    }

                    /* We should try to do as little computation after
                     * this as possible, because parallel PME synchronizes
                     * the nodes, so we want all load imbalance of the
                     * rest of the force calculation to be before the PME
                     * call.  DD load balancing is done on the whole time
                     * of the force call (without PME).
                     */
                }
                if (fr->n_tpi > 0)
                {
                    /* Determine the PME grid energy of the test molecule
                     * with the PME grid potential of the other charges.
                     */
                    Vlr_q = gmx_pme_calc_energy(
                            fr->pmedata,
                            coordinates.subArray(md->homenr - fr->n_tpi, fr->n_tpi),
                            gmx::arrayRefFromArray(md->chargeA + md->homenr - fr->n_tpi, fr->n_tpi));
                }
            }
        }

        if (fr->ic->eeltype == CoulombInteractionType::Ewald)
        {
            Vlr_q = do_ewald(ir,
                             coordinates,
                             forceWithVirial->force_,
                             gmx::arrayRefFromArray(md->chargeA, md->nr),
                             gmx::arrayRefFromArray(md->chargeB, md->nr),
                             box,
                             cr,
                             md->homenr,
                             ewaldOutput.vir_q,
                             fr->ic->ewaldcoeff_q,
                             lambda[static_cast<int>(FreeEnergyPerturbationCouplingType::Coul)],
                             &ewaldOutput.dvdl[FreeEnergyPerturbationCouplingType::Coul],
                             fr->ewald_table.get());
        }

        /* Note that with separate PME nodes we get the real energies later */
        // TODO it would be simpler if we just accumulated a single
        // long-range virial contribution.
        forceWithVirial->addVirialContribution(ewaldOutput.vir_q);
        forceWithVirial->addVirialContribution(ewaldOutput.vir_lj);
        enerd->dvdl_lin[FreeEnergyPerturbationCouplingType::Coul] +=
                ewaldOutput.dvdl[FreeEnergyPerturbationCouplingType::Coul];
        enerd->dvdl_lin[FreeEnergyPerturbationCouplingType::Vdw] +=
                ewaldOutput.dvdl[FreeEnergyPerturbationCouplingType::Vdw];
        enerd->term[F_COUL_RECIP] = Vlr_q + ewaldOutput.Vcorr_q;
        enerd->term[F_LJ_RECIP]   = Vlr_lj + ewaldOutput.Vcorr_lj;

        if (debug)
        {
            fprintf(debug,
                    "Vlr_q = %g, Vcorr_q = %g, Vlr_corr_q = %g\n",
                    Vlr_q,
                    ewaldOutput.Vcorr_q,
                    enerd->term[F_COUL_RECIP]);
            pr_rvecs(debug, 0, "vir_el_recip after corr", ewaldOutput.vir_q, DIM);
            fprintf(debug,
                    "Vlr_lj: %g, Vcorr_lj = %g, Vlr_corr_lj = %g\n",
                    Vlr_lj,
                    ewaldOutput.Vcorr_lj,
                    enerd->term[F_LJ_RECIP]);
            pr_rvecs(debug, 0, "vir_lj_recip after corr", ewaldOutput.vir_lj, DIM);
        }
    }

    if (debug)
    {
        print_nrnb(debug, nrnb);
    }
}
