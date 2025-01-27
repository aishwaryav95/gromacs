/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team.
 * Copyright (c) 2012,2014,2015,2016,2017 by the GROMACS development team.
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
#ifndef GMX_MDTYPES_FCDATA_H
#define GMX_MDTYPES_FCDATA_H

#include <memory>
#include <vector>

#include "gromacs/math/vectypes.h"
#include "gromacs/topology/idef.h"
#include "gromacs/utility/arrayref.h"
#include "gromacs/utility/classhelpers.h"
#include "gromacs/utility/real.h"

enum class DistanceRestraintWeighting : int;
struct gmx_mtop_t;
struct gmx_multisim_t;
struct t_commrec;
struct t_inputrec;
class t_state;

typedef real rvec5[5];

/* Distance restraining stuff */
typedef struct t_disresdata
{
    DistanceRestraintWeighting dr_weighting; /* Weighting of pairs in one restraint              */
    bool                       dr_bMixed;    /* Use sqrt of the instantaneous times              *
                                              * the time averaged violation                      */
    real dr_fc;                              /* Force constant for disres,                       *
                                              * which is multiplied by a (possibly)              *
                                              * different factor for each restraint              */
    real  dr_tau;                            /* Time constant for disres		          */
    real  ETerm;                             /* multiplication factor for time averaging         */
    real  ETerm1;                            /* 1 - ETerm1                                       */
    real  exp_min_t_tau;                     /* Factor for slowly switching on the force         */
    int   nres;                              /* The number of distance restraints                */
    int   npair;                             /* The number of distance restraint pairs           */
    int   type_min;                          /* The minimum iparam type index for restraints     */
    real  sumviol;                           /* The sum of violations                            */
    real* rt;                                /* The instantaneous distance (npair)               */
    real* rm3tav;                            /* The time averaged distance (npair)               */
    real* Rtl_6;                             /* The instantaneous r^-6 (nres)                    */
    real* Rt_6;                              /* The instantaneous ensemble averaged r^-6 (nres)  */
    real* Rtav_6;                            /* The time and ensemble averaged r^-6 (nres)       */
    int   nsystems;                          /* The number of systems for ensemble averaging     */

    /* TODO: Implement a proper solution for parallel disre indexing */
    const t_iatom* forceatomsStart; /* Pointer to the start of the disre forceatoms */
} t_disresdata;

/* All coefficients for the matrix equation for the orientation tensor */
struct OriresMatEq
{
    real rhs[5];    /* The right hand side of the matrix equation */
    real mat[5][5]; /* The matrix                                 */
};

/* Orientation restraining stuff */
struct t_oriresdata
{
    /*! Constructor
     *
     * \param[in] fplog  Log file, can be nullptr
     * \param[in] mtop   The global topology
     * \param[in] ir     The input record
     * \param[in] cr     The commrec, can be nullptr when not running in parallel
     * \param[in] ms     The multisim communicator, pass nullptr to avoid ensemble averaging
     * \param[in,out] globalState  The global state, orientation restraint entires are added
     *
     * \throws InvalidInputError when there is domain decomposition, fewer than 5 restraints,
     *         periodic molecules or more than 1 molecule for a moleculetype with restraints.
     */
    t_oriresdata(FILE*                 fplog,
                 const gmx_mtop_t&     mtop,
                 const t_inputrec&     ir,
                 const t_commrec*      cr,
                 const gmx_multisim_t* ms,
                 t_state*              globalState);

    //! Destructor
    ~t_oriresdata();

    //! Force constant for the restraints
    real fc;
    //! Multiplication factor for time averaging
    real edt;
    //! 1 - edt
    real edt_1;
    //! Factor for slowly switching on the force
    real exp_min_t_tau;
    //! The number of orientation restraints
    const int numRestraints;
    //! The number of experiments
    int numExperiments;
    //! The minimum iparam type index for restraints
    int typeMin;
    //! The number of atoms for the fit
    int numReferenceAtoms;
    //! The masses of the reference atoms
    std::vector<real> mref;
    //! The reference coordinates for the fit
    std::vector<gmx::RVec> xref;
    //! Temporary array for fitting
    std::vector<gmx::RVec> xtmp;
    //! Rotation matrix to rotate to the reference coordinates
    matrix rotationMatrix;
    //! Array of order tensors, one for each experiment
    tensor* orderTensors = nullptr;
    //! The order tensor D for all restraints
    rvec5* DTensors = nullptr;
    //! The ensemble averaged D for all restraints
    rvec5* DTensorsEnsembleAv = nullptr;
    //! The time and ensemble averaged D restraints
    rvec5* DTensorsTimeAndEnsembleAv = nullptr;
    //! The calculated instantaneous orientations
    std::vector<real> orientations;
    //! The calculated emsemble averaged orientations
    gmx::ArrayRef<real> orientationsEnsembleAv;
    //! Buffer for the calculated emsemble averaged orientations, only used with ensemble averaging
    std::vector<real> orientationsEnsembleAvBuffer;
    //! The calculated time and ensemble averaged orientations
    gmx::ArrayRef<real> orientationsTimeAndEnsembleAv;
    //! The weighted (using kfac) RMS deviation
    std::vector<real> orientationsTimeAndEnsembleAvBuffer;
    //! Buffer for the weighted (using kfac) RMS deviation, only used with time averaging
    real rmsdev;
    //! An temporary array of matrix + rhs
    std::vector<OriresMatEq> tmpEq;
    //! The number of eigenvalues + eigenvectors per experiment
    static constexpr int c_numEigenRealsPerExperiment = 12;
    //! Eigenvalues/vectors, for output only (numExperiments x 12)
    std::vector<real> eigenOutput;

    // variables for diagonalization with diagonalize_orires_tensors()
    //! Tensor to diagonalize
    std::array<gmx::DVec, DIM> M;
    //! Eigenvalues
    std::array<double, DIM> eig_diag;
    //! Eigenvectors
    std::array<gmx::DVec, DIM> v;

    // Default copy and assign would be incorrect and manual versions are not yet implemented.
    GMX_DISALLOW_COPY_AND_ASSIGN(t_oriresdata);
};

/* Cubic spline table for tabulated bonded interactions */
struct bondedtable_t
{
    int               n;     /* n+1 is the number of points */
    real              scale; /* distance between two points */
    std::vector<real> data;  /* the actual table data, per point there are 4 numbers */
};

/*
 * Data struct used in the force calculation routines
 * for storing the tables for bonded interactions and
 * for storing information which is needed in following steps
 * (for instance for time averaging in distance retraints)
 * or for storing output, since force routines only return the potential.
 */
struct t_fcdata
{
    std::vector<bondedtable_t> bondtab;
    std::vector<bondedtable_t> angletab;
    std::vector<bondedtable_t> dihtab;

    // TODO: Convert to C++ and unique_ptr (currently this data is not freed)
    t_disresdata*                 disres = nullptr;
    std::unique_ptr<t_oriresdata> orires;
};

#endif
