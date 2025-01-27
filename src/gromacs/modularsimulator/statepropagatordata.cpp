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
 * \brief Defines the state for the modular simulator
 *
 * \author Pascal Merz <pascal.merz@me.com>
 * \ingroup module_modularsimulator
 */

#include "gmxpre.h"

#include "gromacs/utility/enumerationhelpers.h"
#include "statepropagatordata.h"

#include "gromacs/commandline/filenm.h"
#include "gromacs/domdec/collect.h"
#include "gromacs/domdec/domdec.h"
#include "gromacs/fileio/confio.h"
#include "gromacs/math/vec.h"
#include "gromacs/mdlib/gmx_omp_nthreads.h"
#include "gromacs/mdlib/mdatoms.h"
#include "gromacs/mdlib/mdoutf.h"
#include "gromacs/mdlib/stat.h"
#include "gromacs/mdlib/update.h"
#include "gromacs/mdtypes/checkpointdata.h"
#include "gromacs/mdtypes/commrec.h"
#include "gromacs/mdtypes/forcebuffers.h"
#include "gromacs/mdtypes/forcerec.h"
#include "gromacs/mdtypes/inputrec.h"
#include "gromacs/mdtypes/mdatom.h"
#include "gromacs/mdtypes/mdrunoptions.h"
#include "gromacs/mdtypes/state.h"
#include "gromacs/pbcutil/pbc.h"
#include "gromacs/topology/atoms.h"
#include "gromacs/topology/topology.h"
#include "gromacs/trajectory/trajectoryframe.h"

#include "freeenergyperturbationdata.h"
#include "modularsimulator.h"
#include "simulatoralgorithm.h"

namespace gmx
{
StatePropagatorData::StatePropagatorData(int                numAtoms,
                                         FILE*              fplog,
                                         const t_commrec*   cr,
                                         t_state*           globalState,
                                         bool               useGPU,
                                         bool               canMoleculesBeDistributedOverPBC,
                                         bool               writeFinalConfiguration,
                                         const std::string& finalConfigurationFilename,
                                         const t_inputrec*  inputrec,
                                         const t_mdatoms*   mdatoms,
                                         const gmx_mtop_t&  globalTop) :
    totalNumAtoms_(numAtoms),
    localNAtoms_(0),
    box_{ { 0 } },
    previousBox_{ { 0 } },
    ddpCount_(0),
    element_(std::make_unique<Element>(this,
                                       fplog,
                                       cr,
                                       inputrec->nstxout,
                                       inputrec->nstvout,
                                       inputrec->nstfout,
                                       inputrec->nstxout_compressed,
                                       canMoleculesBeDistributedOverPBC,
                                       writeFinalConfiguration,
                                       finalConfigurationFilename,
                                       inputrec,
                                       globalTop)),
    vvResetVelocities_(false),
    isRegularSimulationEnd_(false),
    lastStep_(-1),
    globalState_(globalState)
{
    bool stateHasVelocities;
    // Local state only becomes valid now.
    if (DOMAINDECOMP(cr))
    {
        auto localState = std::make_unique<t_state>();
        dd_init_local_state(*cr->dd, globalState, localState.get());
        stateHasVelocities = ((localState->flags & enumValueToBitMask(StateEntry::V)) != 0);
        setLocalState(std::move(localState));
    }
    else
    {
        state_change_natoms(globalState, globalState->natoms);
        f_.resize(globalState->natoms);
        localNAtoms_ = globalState->natoms;
        x_           = globalState->x;
        v_           = globalState->v;
        copy_mat(globalState->box, box_);
        stateHasVelocities = ((globalState->flags & enumValueToBitMask(StateEntry::V)) != 0);
        previousX_.resizeWithPadding(localNAtoms_);
        ddpCount_ = globalState->ddp_count;
        copyPosition();
    }
    if (useGPU)
    {
        changePinningPolicy(&x_, gmx::PinningPolicy::PinnedIfSupported);
    }

    if (DOMAINDECOMP(cr) && MASTER(cr))
    {
        xGlobal_.resizeWithPadding(totalNumAtoms_);
        previousXGlobal_.resizeWithPadding(totalNumAtoms_);
        vGlobal_.resizeWithPadding(totalNumAtoms_);
        fGlobal_.resizeWithPadding(totalNumAtoms_);
    }

    if (!inputrec->bContinuation)
    {
        if (stateHasVelocities)
        {
            auto v = velocitiesView().paddedArrayRef();
            // Set the velocities of vsites, shells and frozen atoms to zero
            for (int i = 0; i < mdatoms->homenr; i++)
            {
                if (mdatoms->ptype[i] == ParticleType::Shell)
                {
                    clear_rvec(v[i]);
                }
                else if (mdatoms->cFREEZE)
                {
                    for (int m = 0; m < DIM; m++)
                    {
                        if (inputrec->opts.nFreeze[mdatoms->cFREEZE[i]][m])
                        {
                            v[i][m] = 0;
                        }
                    }
                }
            }
        }
        if (inputrec->eI == IntegrationAlgorithm::VV)
        {
            vvResetVelocities_ = true;
        }
    }
}

StatePropagatorData::Element* StatePropagatorData::element()
{
    return element_.get();
}

void StatePropagatorData::setup()
{
    if (element_)
    {
        element_->elementSetup();
    }
}

ArrayRefWithPadding<RVec> StatePropagatorData::positionsView()
{
    return x_.arrayRefWithPadding();
}

ArrayRefWithPadding<const RVec> StatePropagatorData::constPositionsView() const
{
    return x_.constArrayRefWithPadding();
}

ArrayRefWithPadding<RVec> StatePropagatorData::previousPositionsView()
{
    return previousX_.arrayRefWithPadding();
}

ArrayRefWithPadding<const RVec> StatePropagatorData::constPreviousPositionsView() const
{
    return previousX_.constArrayRefWithPadding();
}

ArrayRefWithPadding<RVec> StatePropagatorData::velocitiesView()
{
    return v_.arrayRefWithPadding();
}

ArrayRefWithPadding<const RVec> StatePropagatorData::constVelocitiesView() const
{
    return v_.constArrayRefWithPadding();
}

ForceBuffersView& StatePropagatorData::forcesView()
{
    return f_.view();
}

const ForceBuffersView& StatePropagatorData::constForcesView() const
{
    return f_.view();
}

rvec* StatePropagatorData::box()
{
    return box_;
}

const rvec* StatePropagatorData::constBox() const
{
    return box_;
}

rvec* StatePropagatorData::previousBox()
{
    return previousBox_;
}

const rvec* StatePropagatorData::constPreviousBox() const
{
    return previousBox_;
}

int StatePropagatorData::localNumAtoms() const
{
    return localNAtoms_;
}

int StatePropagatorData::totalNumAtoms() const
{
    return totalNumAtoms_;
}

std::unique_ptr<t_state> StatePropagatorData::localState()
{
    auto state   = std::make_unique<t_state>();
    state->flags = enumValueToBitMask(StateEntry::X) | enumValueToBitMask(StateEntry::V)
                   | enumValueToBitMask(StateEntry::Box);
    state_change_natoms(state.get(), localNAtoms_);
    state->x = x_;
    state->v = v_;
    copy_mat(box_, state->box);
    state->ddp_count       = ddpCount_;
    state->ddp_count_cg_gl = ddpCountCgGl_;
    state->cg_gl           = cgGl_;
    return state;
}

void StatePropagatorData::setLocalState(std::unique_ptr<t_state> state)
{
    localNAtoms_ = state->natoms;
    x_.resizeWithPadding(localNAtoms_);
    previousX_.resizeWithPadding(localNAtoms_);
    v_.resizeWithPadding(localNAtoms_);
    x_ = state->x;
    v_ = state->v;
    copy_mat(state->box, box_);
    copyPosition();
    ddpCount_     = state->ddp_count;
    ddpCountCgGl_ = state->ddp_count_cg_gl;
    cgGl_         = state->cg_gl;

    if (vvResetVelocities_)
    {
        /* DomDec runs twice early in the simulation, once at setup time, and once before the first
         * step. Every time DD runs, it sets a new local state here. We are saving a backup during
         * setup time (ok for non-DD cases), so we need to update our backup to the DD state before
         * the first step here to avoid resetting to an earlier DD state. This is done before any
         * propagation that needs to be reset, so it's not very safe but correct for now.
         * TODO: Get rid of this once input is assumed to be at half steps
         */
        velocityBackup_ = v_;
    }
}

t_state* StatePropagatorData::globalState()
{
    return globalState_;
}

ForceBuffers* StatePropagatorData::forcePointer()
{
    return &f_;
}

void StatePropagatorData::copyPosition()
{
    int nth = gmx_omp_nthreads_get(ModuleMultiThread::Update);

#pragma omp parallel for num_threads(nth) schedule(static) default(none) shared(nth)
    for (int th = 0; th < nth; th++)
    {
        int start_th, end_th;
        getThreadAtomRange(nth, th, localNAtoms_, &start_th, &end_th);
        copyPosition(start_th, end_th);
    }

    /* Box is changed in update() when we do pressure coupling,
     * but we should still use the old box for energy corrections and when
     * writing it to the energy file, so it matches the trajectory files for
     * the same timestep above. Make a copy in a separate array.
     */
    copy_mat(box_, previousBox_);
}

void StatePropagatorData::copyPosition(int start, int end)
{
    for (int i = start; i < end; ++i)
    {
        previousX_[i] = x_[i];
    }
}

void StatePropagatorData::Element::scheduleTask(Step step,
                                                Time gmx_unused            time,
                                                const RegisterRunFunction& registerRunFunction)
{
    if (statePropagatorData_->vvResetVelocities_)
    {
        statePropagatorData_->vvResetVelocities_ = false;
        registerRunFunction([this]() { statePropagatorData_->resetVelocities(); });
    }
    // copy x -> previousX
    registerRunFunction([this]() { statePropagatorData_->copyPosition(); });
    // if it's a write out step, keep a copy for writeout
    if (step == writeOutStep_ || (step == lastStep_ && writeFinalConfiguration_))
    {
        registerRunFunction([this]() { saveState(); });
    }
}

void StatePropagatorData::Element::saveState()
{
    GMX_ASSERT(!localStateBackup_, "Save state called again before previous state was written.");
    localStateBackup_ = statePropagatorData_->localState();
    if (freeEnergyPerturbationData_)
    {
        localStateBackup_->fep_state    = freeEnergyPerturbationData_->currentFEPState();
        ArrayRef<const real> lambdaView = freeEnergyPerturbationData_->constLambdaView();
        std::copy(lambdaView.begin(), lambdaView.end(), localStateBackup_->lambda.begin());
        localStateBackup_->flags |=
                enumValueToBitMask(StateEntry::Lambda) | enumValueToBitMask(StateEntry::FepState);
    }
}

std::optional<SignallerCallback> StatePropagatorData::Element::registerTrajectorySignallerCallback(TrajectoryEvent event)
{
    if (event == TrajectoryEvent::StateWritingStep)
    {
        return [this](Step step, Time /*unused*/) { this->writeOutStep_ = step; };
    }
    return std::nullopt;
}

std::optional<ITrajectoryWriterCallback>
StatePropagatorData::Element::registerTrajectoryWriterCallback(TrajectoryEvent event)
{
    if (event == TrajectoryEvent::StateWritingStep)
    {
        return [this](gmx_mdoutf* outf, Step step, Time time, bool writeTrajectory, bool gmx_unused writeLog) {
            if (writeTrajectory)
            {
                write(outf, step, time);
            }
        };
    }
    return std::nullopt;
}

void StatePropagatorData::Element::write(gmx_mdoutf_t outf, Step currentStep, Time currentTime)
{
    wallcycle_start(mdoutf_get_wcycle(outf), WallCycleCounter::Traj);
    unsigned int mdof_flags = 0;
    if (do_per_step(currentStep, nstxout_))
    {
        mdof_flags |= MDOF_X;
    }
    if (do_per_step(currentStep, nstvout_))
    {
        mdof_flags |= MDOF_V;
    }
    if (do_per_step(currentStep, nstfout_))
    {
        mdof_flags |= MDOF_F;
    }
    if (do_per_step(currentStep, nstxout_compressed_))
    {
        mdof_flags |= MDOF_X_COMPRESSED;
    }
    if (do_per_step(currentStep, mdoutf_get_tng_box_output_interval(outf)))
    {
        mdof_flags |= MDOF_BOX;
    }
    if (do_per_step(currentStep, mdoutf_get_tng_lambda_output_interval(outf)))
    {
        mdof_flags |= MDOF_LAMBDA;
    }
    if (do_per_step(currentStep, mdoutf_get_tng_compressed_box_output_interval(outf)))
    {
        mdof_flags |= MDOF_BOX_COMPRESSED;
    }
    if (do_per_step(currentStep, mdoutf_get_tng_compressed_lambda_output_interval(outf)))
    {
        mdof_flags |= MDOF_LAMBDA_COMPRESSED;
    }

    if (mdof_flags == 0)
    {
        wallcycle_stop(mdoutf_get_wcycle(outf), WallCycleCounter::Traj);
        return;
    }
    GMX_ASSERT(localStateBackup_, "Trajectory writing called, but no state saved.");

    // TODO: This is only used for CPT - needs to be filled when we turn CPT back on
    ObservablesHistory* observablesHistory = nullptr;

    mdoutf_write_to_trajectory_files(fplog_,
                                     cr_,
                                     outf,
                                     static_cast<int>(mdof_flags),
                                     statePropagatorData_->totalNumAtoms_,
                                     currentStep,
                                     currentTime,
                                     localStateBackup_.get(),
                                     statePropagatorData_->globalState_,
                                     observablesHistory,
                                     statePropagatorData_->f_.view().force(),
                                     &dummyCheckpointDataHolder_);

    if (currentStep != lastStep_ || !isRegularSimulationEnd_)
    {
        localStateBackup_.reset();
    }
    wallcycle_stop(mdoutf_get_wcycle(outf), WallCycleCounter::Traj);
}

void StatePropagatorData::Element::elementSetup()
{
    if (statePropagatorData_->vvResetVelocities_)
    {
        // MD-VV does the first velocity half-step only to calculate the constraint virial,
        // then resets the velocities since the input is assumed to be positions and velocities
        // at full time step. TODO: Change this to have input at half time steps.
        statePropagatorData_->velocityBackup_ = statePropagatorData_->v_;
    }
}

void StatePropagatorData::resetVelocities()
{
    v_ = velocityBackup_;
}

namespace
{
/*!
 * \brief Enum describing the contents StatePropagatorData::Element writes to modular checkpoint
 *
 * When changing the checkpoint content, add a new element just above Count, and adjust the
 * checkpoint functionality.
 */
enum class CheckpointVersion
{
    Base, //!< First version of modular checkpointing
    Count //!< Number of entries. Add new versions right above this!
};
constexpr auto c_currentVersion = CheckpointVersion(int(CheckpointVersion::Count) - 1);
} // namespace

template<CheckpointDataOperation operation>
void StatePropagatorData::doCheckpointData(CheckpointData<operation>* checkpointData)
{
    checkpointVersion(checkpointData, "StatePropagatorData version", c_currentVersion);
    checkpointData->scalar("numAtoms", &totalNumAtoms_);

    if (operation == CheckpointDataOperation::Read)
    {
        xGlobal_.resizeWithPadding(totalNumAtoms_);
        vGlobal_.resizeWithPadding(totalNumAtoms_);
    }

    checkpointData->arrayRef("positions", makeCheckpointArrayRef<operation>(xGlobal_));
    checkpointData->arrayRef("velocities", makeCheckpointArrayRef<operation>(vGlobal_));
    checkpointData->tensor("box", box_);
    checkpointData->scalar("ddpCount", &ddpCount_);
    checkpointData->scalar("ddpCountCgGl", &ddpCountCgGl_);
    checkpointData->arrayRef("cgGl", makeCheckpointArrayRef<operation>(cgGl_));
}

void StatePropagatorData::Element::saveCheckpointState(std::optional<WriteCheckpointData> checkpointData,
                                                       const t_commrec*                   cr)
{
    if (DOMAINDECOMP(cr))
    {
        // Collect state from all ranks into global vectors
        dd_collect_vec(cr->dd,
                       statePropagatorData_->ddpCount_,
                       statePropagatorData_->ddpCountCgGl_,
                       statePropagatorData_->cgGl_,
                       statePropagatorData_->x_,
                       statePropagatorData_->xGlobal_);
        dd_collect_vec(cr->dd,
                       statePropagatorData_->ddpCount_,
                       statePropagatorData_->ddpCountCgGl_,
                       statePropagatorData_->cgGl_,
                       statePropagatorData_->v_,
                       statePropagatorData_->vGlobal_);
    }
    else
    {
        // Everything is local - copy local vectors into global ones
        statePropagatorData_->xGlobal_.resizeWithPadding(statePropagatorData_->totalNumAtoms());
        statePropagatorData_->vGlobal_.resizeWithPadding(statePropagatorData_->totalNumAtoms());
        std::copy(statePropagatorData_->x_.begin(),
                  statePropagatorData_->x_.end(),
                  statePropagatorData_->xGlobal_.begin());
        std::copy(statePropagatorData_->v_.begin(),
                  statePropagatorData_->v_.end(),
                  statePropagatorData_->vGlobal_.begin());
    }
    if (MASTER(cr))
    {
        statePropagatorData_->doCheckpointData<CheckpointDataOperation::Write>(&checkpointData.value());
    }
}

/*!
 * \brief Update the legacy global state
 *
 * When restoring from checkpoint, data will be distributed during domain decomposition at setup stage.
 * Domain decomposition still uses the legacy global t_state object so make sure it's up-to-date.
 */
static void updateGlobalState(t_state*                      globalState,
                              const PaddedHostVector<RVec>& x,
                              const PaddedHostVector<RVec>& v,
                              const tensor                  box,
                              int                           ddpCount,
                              int                           ddpCountCgGl,
                              const std::vector<int>&       cgGl)
{
    globalState->x = x;
    globalState->v = v;
    copy_mat(box, globalState->box);
    globalState->ddp_count       = ddpCount;
    globalState->ddp_count_cg_gl = ddpCountCgGl;
    globalState->cg_gl           = cgGl;
}

void StatePropagatorData::Element::restoreCheckpointState(std::optional<ReadCheckpointData> checkpointData,
                                                          const t_commrec*                  cr)
{
    if (MASTER(cr))
    {
        statePropagatorData_->doCheckpointData<CheckpointDataOperation::Read>(&checkpointData.value());
    }

    // Copy data to global state to be distributed by DD at setup stage
    if (DOMAINDECOMP(cr) && MASTER(cr))
    {
        updateGlobalState(statePropagatorData_->globalState_,
                          statePropagatorData_->xGlobal_,
                          statePropagatorData_->vGlobal_,
                          statePropagatorData_->box_,
                          statePropagatorData_->ddpCount_,
                          statePropagatorData_->ddpCountCgGl_,
                          statePropagatorData_->cgGl_);
    }
    // Everything is local - copy global vectors to local ones
    if (!DOMAINDECOMP(cr))
    {
        statePropagatorData_->x_.resizeWithPadding(statePropagatorData_->totalNumAtoms_);
        statePropagatorData_->v_.resizeWithPadding(statePropagatorData_->totalNumAtoms_);
        std::copy(statePropagatorData_->xGlobal_.begin(),
                  statePropagatorData_->xGlobal_.end(),
                  statePropagatorData_->x_.begin());
        std::copy(statePropagatorData_->vGlobal_.begin(),
                  statePropagatorData_->vGlobal_.end(),
                  statePropagatorData_->v_.begin());
    }
}

const std::string& StatePropagatorData::Element::clientID()
{
    return StatePropagatorData::checkpointID();
}

void StatePropagatorData::Element::trajectoryWriterTeardown(gmx_mdoutf* gmx_unused outf)
{
    // Note that part of this code is duplicated in do_md_trajectory_writing.
    // This duplication is needed while both legacy and modular code paths are in use.
    // TODO: Remove duplication asap, make sure to keep in sync in the meantime.
    if (!writeFinalConfiguration_ || !isRegularSimulationEnd_)
    {
        return;
    }

    GMX_ASSERT(localStateBackup_, "Final trajectory writing called, but no state saved.");

    wallcycle_start(mdoutf_get_wcycle(outf), WallCycleCounter::Traj);
    if (DOMAINDECOMP(cr_))
    {
        auto globalXRef =
                MASTER(cr_) ? statePropagatorData_->globalState_->x : gmx::ArrayRef<gmx::RVec>();
        dd_collect_vec(cr_->dd,
                       localStateBackup_->ddp_count,
                       localStateBackup_->ddp_count_cg_gl,
                       localStateBackup_->cg_gl,
                       localStateBackup_->x,
                       globalXRef);
        auto globalVRef =
                MASTER(cr_) ? statePropagatorData_->globalState_->v : gmx::ArrayRef<gmx::RVec>();
        dd_collect_vec(cr_->dd,
                       localStateBackup_->ddp_count,
                       localStateBackup_->ddp_count_cg_gl,
                       localStateBackup_->cg_gl,
                       localStateBackup_->v,
                       globalVRef);
    }
    else
    {
        // We have the whole state locally: copy the local state pointer
        statePropagatorData_->globalState_ = localStateBackup_.get();
    }

    if (MASTER(cr_))
    {
        fprintf(stderr, "\nWriting final coordinates.\n");
        if (canMoleculesBeDistributedOverPBC_ && !systemHasPeriodicMolecules_)
        {
            // Make molecules whole only for confout writing
            do_pbc_mtop(pbcType_,
                        localStateBackup_->box,
                        &top_global_,
                        statePropagatorData_->globalState_->x.rvec_array());
        }
        write_sto_conf_mtop(finalConfigurationFilename_.c_str(),
                            *top_global_.name,
                            top_global_,
                            statePropagatorData_->globalState_->x.rvec_array(),
                            statePropagatorData_->globalState_->v.rvec_array(),
                            pbcType_,
                            localStateBackup_->box);
    }
    wallcycle_stop(mdoutf_get_wcycle(outf), WallCycleCounter::Traj);
}

std::optional<SignallerCallback> StatePropagatorData::Element::registerLastStepCallback()
{
    return [this](Step step, Time /*time*/) {
        lastStep_               = step;
        isRegularSimulationEnd_ = (step == lastPlannedStep_);
    };
}

StatePropagatorData::Element::Element(StatePropagatorData* statePropagatorData,
                                      FILE*                fplog,
                                      const t_commrec*     cr,
                                      int                  nstxout,
                                      int                  nstvout,
                                      int                  nstfout,
                                      int                  nstxout_compressed,
                                      bool                 canMoleculesBeDistributedOverPBC,
                                      bool                 writeFinalConfiguration,
                                      std::string          finalConfigurationFilename,
                                      const t_inputrec*    inputrec,
                                      const gmx_mtop_t&    globalTop) :
    statePropagatorData_(statePropagatorData),
    nstxout_(nstxout),
    nstvout_(nstvout),
    nstfout_(nstfout),
    nstxout_compressed_(nstxout_compressed),
    writeOutStep_(-1),
    freeEnergyPerturbationData_(nullptr),
    isRegularSimulationEnd_(false),
    lastStep_(-1),
    canMoleculesBeDistributedOverPBC_(canMoleculesBeDistributedOverPBC),
    systemHasPeriodicMolecules_(inputrec->bPeriodicMols),
    pbcType_(inputrec->pbcType),
    lastPlannedStep_(inputrec->nsteps + inputrec->init_step),
    writeFinalConfiguration_(writeFinalConfiguration),
    finalConfigurationFilename_(std::move(finalConfigurationFilename)),
    fplog_(fplog),
    cr_(cr),
    top_global_(globalTop)
{
}
void StatePropagatorData::Element::setFreeEnergyPerturbationData(FreeEnergyPerturbationData* freeEnergyPerturbationData)
{
    freeEnergyPerturbationData_ = freeEnergyPerturbationData;
}

ISimulatorElement* StatePropagatorData::Element::getElementPointerImpl(
        LegacySimulatorData gmx_unused*        legacySimulatorData,
        ModularSimulatorAlgorithmBuilderHelper gmx_unused* builderHelper,
        StatePropagatorData*                               statePropagatorData,
        EnergyData gmx_unused*      energyData,
        FreeEnergyPerturbationData* freeEnergyPerturbationData,
        GlobalCommunicationHelper gmx_unused* globalCommunicationHelper)
{
    statePropagatorData->element()->setFreeEnergyPerturbationData(freeEnergyPerturbationData);
    return statePropagatorData->element();
}

void StatePropagatorData::readCheckpointToTrxFrame(t_trxframe* trxFrame, ReadCheckpointData readCheckpointData)
{
    StatePropagatorData statePropagatorData;
    statePropagatorData.doCheckpointData(&readCheckpointData);

    trxFrame->natoms = statePropagatorData.totalNumAtoms_;
    trxFrame->bX     = true;
    trxFrame->x  = makeRvecArray(statePropagatorData.xGlobal_, statePropagatorData.totalNumAtoms_);
    trxFrame->bV = true;
    trxFrame->v  = makeRvecArray(statePropagatorData.vGlobal_, statePropagatorData.totalNumAtoms_);
    trxFrame->bF = false;
    trxFrame->bBox = true;
    copy_mat(statePropagatorData.box_, trxFrame->box);
}

const std::string& StatePropagatorData::checkpointID()
{
    static const std::string identifier = "StatePropagatorData";
    return identifier;
}

} // namespace gmx
