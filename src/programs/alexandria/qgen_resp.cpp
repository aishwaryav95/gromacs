/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2016, by the GROMACS development team, led by
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
/*! \internal \brief
 * Implements part of the alexandria program.
 * \author David van der Spoel <david.vanderspoel@icm.uu.se>
 */
#include "gmxpre.h"

#include "qgen_resp.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "gromacs/commandline/filenm.h"
#include "gromacs/fileio/confio.h"
#include "gromacs/fileio/pdbio.h"
#include "gromacs/fileio/xvgr.h"
#include "gromacs/gmxpreprocess/gpp_atomtype.h"
#include "gromacs/gmxpreprocess/toputil.h"
#include "gromacs/linearalgebra/matrix.h"
#include "gromacs/listed-forces/bonded.h"
#include "gromacs/math/units.h"
#include "gromacs/math/vec.h"
#include "gromacs/math/vectypes.h"
#include "gromacs/mdtypes/md_enums.h"
#include "gromacs/pbcutil/pbc.h"
#include "gromacs/random/random.h"
#include "gromacs/statistics/statistics.h"
#include "gromacs/topology/atomprop.h"
#include "gromacs/topology/atoms.h"
#include "gromacs/topology/symtab.h"
#include "gromacs/utility/cstringutil.h"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/futil.h"
#include "gromacs/utility/gmxomp.h"
#include "gromacs/utility/pleasecite.h"
#include "gromacs/utility/smalloc.h"
#include "gromacs/utility/stringutil.h"
#include "gromacs/utility/textreader.h"
#include "gromacs/utility/txtdump.h"

#include "nmsimplex.h"
#include "poldata.h"
#include "qgen_eem.h"
#include "stringutil.h"
#include "coulombintegrals/coulombintegrals.h"

namespace alexandria
{
QgenResp::QgenResp()
{
    rnd_                = nullptr;
    setOptions(eqdAXp, 0, false, 5, 100, -1, false,
               0, -3, 3, true, 0);
    _bAXpRESP           = false;
    _qfac               = 1e-3;
    _bHyper             = 0.1;
    _wtot               = 0;
    _pfac               = 1;
    _bEntropy           = false;
    _rDecrZeta          = true;
}

void QgenResp::setOptions(ChargeDistributionModel c,
                          unsigned int            seed,
                          bool                    fitZeta,
                          real                    zetaMin,
                          real                    zetaMax,
                          real                    deltaZeta,
                          bool                    randomZeta,
                          real                    qtot,
                          real                    qmin,
                          real                    qmax,
                          bool                    randomQ,
                          real                    watoms)
{
    _iDistributionModel = c;
    _bFitZeta           = fitZeta && (c != eqdAXp);
    _zmin               = zetaMin;
    _zmax               = zetaMax;
    _deltaZ             = deltaZeta;
    _bRandZeta          = randomZeta;
    _rDecrZeta          = true;

    _qtot               = qtot;
    _qmin               = qmin;
    _qmax               = qmax; /* e */
    _bRandQ             = randomQ;
    _watoms             = watoms;
    if (seed <= 0)
    {
        seed = gmx_rng_make_seed();
    }
    if (nullptr != rnd_)
    {
        gmx_rng_destroy(rnd_);
    }
    rnd_ = gmx_rng_init(seed);
}

QgenResp::~QgenResp()
{
    gmx_rng_destroy(rnd_);
}

void QgenResp::setAtomInfo(t_atoms                   *atoms,
                           const alexandria::Poldata &pd,
                           const rvec                 x[])
{
    for (int i = 0; (i < atoms->nr); i++)
    {
        ra_.push_back(RespAtom(atoms->atom[i].atomnumber,
                               atoms->atom[i].type,
                               0,
                               x[i]));
        if (findRAT(atoms->atom[i].type) == endRAT())
        {
            ratype_.push_back(RespAtomType(atoms->atom[i].type,
                                           *(atoms->atomtype[i]), pd,
                                           _iDistributionModel, _dzatoms));
        }
        // Now compute starting charge for atom, taking into account
        // the charges of the other "shells".
        RespAtomTypeIterator rat = findRAT(atoms->atom[i].type);
        GMX_RELEASE_ASSERT(rat != endRAT(), "Inconsistency setting atom info");
        double               q = atoms->atom[i].q;
        for (auto rz = rat->beginRZ(); rz < rat->endRZ(); ++rz)
        {
            q -= rz->q();
        }
        ra_.back().setCharge(q);
    }
}

void QgenResp::summary(FILE *fp)
{
    if (NULL != fp)
    {
        fprintf(fp, "There are %d atoms, %d atomtypes %d parameters for (R)ESP fitting.\n",
                static_cast<int>(nAtom()),
                static_cast<int>(nAtomType()),
                static_cast<int>(nParam()));
        for (size_t i = 0; (i < nAtom()); i++)
        {
            fprintf(fp, " %d", symmetricAtoms_[i]);
        }
        fprintf(fp, "\n");
    }
}

int QgenResp::addParam(size_t aindex, eParm eparm, size_t zz)
{
    int iParam = -1;
    if (eparm == eparmQ)
    {
        range_check(aindex, 0, nAtom());
        raparam_.push_back(RespParam(eparm, aindex, zz));
        if (debug)
        {
            fprintf(debug, "GRESP: Adding charge for atom %d\n",
                    static_cast<int>(aindex));
        }
        iParam = nParam()-1;
    }
    else if (_bFitZeta)
    {
        RespAtomTypeIterator rat = findRAT(aindex);
        GMX_RELEASE_ASSERT(rat != endRAT(), "Can not not find atomtype");
        GMX_RELEASE_ASSERT(zz < rat->getNZeta(), "Zeta out of range");
        // See if we have this one in the library already.
        RespParamIterator rap = std::find_if(raparam_.begin(), raparam_.end(),
                                             [eparm, aindex, zz](RespParam const &rp)
                                             { return (rp.eParam() == eparm &&
                                                       rp.aIndex() == aindex &&
                                                       rp.zIndex() == zz); });
        if (rap == raparam_.end())
        {
            raparam_.push_back(RespParam(eparm, aindex, zz));
            iParam = nParam()-1;
            if (debug)
            {
                fprintf(debug, "GRESP: Adding zeta %d for atom type %d\n",
                        static_cast<int>(zz), static_cast<int>(aindex));
            }
        }
        else
        {
            iParam = rap - raparam_.begin();
        }
    }
    return iParam;
}

void QgenResp::setAtomSymmetry(const std::vector<int> &symmetricAtoms)
{
    GMX_RELEASE_ASSERT(!ra_.empty(), "RespAtom vector not initialized");
    GMX_RELEASE_ASSERT(!ratype_.empty(), "RespAtomType vector not initialized");
    GMX_RELEASE_ASSERT(nParam() == 0, "There are parameters already in the Resp structure");
    GMX_RELEASE_ASSERT(symmetricAtoms.size() == nAtom(), "Please pass me a correct symmetric atoms vector");

    symmetricAtoms_ = symmetricAtoms;
    /* Map the symmetric atoms */
    for (size_t i = 0; (i < nAtom()); i++)
    {
        int                  atype = ra_[i].atype();
        RespAtomTypeIterator rai   = findRAT(atype);
        if (0 == i)
        {
            /* The first charge is not a free variable, it follows from the
             * total charge. Only add the zeta values here.
             */
            for (size_t zz = 0; (zz < rai->getNZeta()); zz++)
            {
                int iParam = addParam(atype, eparmZ, zz);
                (rai->beginRZ()+zz)->setZindex(iParam);
            }
        }
        else if (symmetricAtoms[i] == static_cast<int>(i))
        {
            // We optimize at most 1 charge per atom, so use index 0
            int iParam = addParam(i, eparmQ, 0);
            ra_[i].setQindex(iParam);
            // Check if we have this atype covered already
            if (rai != ratype_.end())
            {
                // New atom type
                for (size_t zz = 0; (zz < rai->getNZeta()); zz++)
                {
                    iParam = addParam(atype, eparmZ, zz);
                    (rai->beginRZ()+zz)->setZindex(iParam);
                }
            }
        }
        else if (symmetricAtoms[i] > static_cast<int>(i))
        {
            gmx_fatal(FARGS, "The symmetricAtoms array can not point to larger atom numbers");
        }
        else
        {
            // Symmetric atom
            ra_[i].setQindex(ra_[symmetricAtoms[i]].qIndex());
            if (debug)
            {
                fprintf(debug, "Atom %d is a copy of atom %d\n",
                        static_cast<int>(i+1), symmetricAtoms[i]+1);
            }
        }
    }
    if (debug)
    {
        size_t maxz = 0;
        for (size_t i = 0; (i < nAtomType()); i++)
        {
            if (ratype_[i].getNZeta() > maxz)
            {
                maxz = ratype_[i].getNZeta();
            }
        }

        fprintf(debug, "GRQ: %3s %5s", "nr", "type");
        fprintf(debug, " %8s %8s\n", "q", "zeta");
        for (size_t i = 0; (i < nAtom()); i++)
        {
            int                  atype = ra_[i].atype();
            RespAtomTypeIterator rai   = findRAT(atype);
            fprintf(debug, "GRQ: %3d %5s", static_cast<int>(i+1),
                    rai->getAtomtype().c_str());
            for (RowZetaQConstIterator zz = rai->beginRZ(); zz <  rai->endRZ(); ++zz)
            {
                fprintf(debug, " %8.4f %8.4f\n",
                        ra_[i].charge(), zz->zeta());
            }
            fprintf(debug, "\n");
        }
    }
    printf("There are %d variables to optimize for %d atoms.\n",
           static_cast<int>(nParam()), static_cast<int>(nAtom()));
}

void QgenResp::writeHisto(const std::string      &fn,
                          const std::string      &title,
                          const gmx_output_env_t *oenv)
{
    FILE       *fp;
    gmx_stats_t gs;
    real       *x, *y;
    int         nbin = 100;

    if (0 == fn.size())
    {
        return;
    }
    gs = gmx_stats_init();
    for (size_t i = 0; (i < nEsp()); i++)
    {
        gmx_stats_add_point(gs, i, gmx2convert(_potCalc[i], eg2cHartree_e), 0, 0);
    }

    gmx_stats_make_histogram(gs, 0, &nbin, ehistoY, 1, &x, &y);

    fp = xvgropen(fn.c_str(), title.c_str(), "Pot (1/a.u.)", "()", oenv);
    for (int i = 0; (i < nbin); i++)
    {
        fprintf(fp, "%10g  %10g\n", x[i], y[i]);
    }
    sfree(x);
    sfree(y);
    fclose(fp);
    gmx_stats_free(gs);
}

void QgenResp::writeDiffCube(QgenResp               &src,
                             const std::string      &cubeFn,
                             const std::string      &histFn,
                             const std::string      &title,
                             const gmx_output_env_t *oenv,
                             int                     rho)
{
    FILE       *fp;
    int         i, m, ix, iy, iz;
    real        pp, q, r, rmin;
    gmx_stats_t gst = NULL, ppcorr = NULL;

    if (0 != histFn.size())
    {
        gst    = gmx_stats_init();
        ppcorr = gmx_stats_init();
    }
    if (0 != cubeFn.size())
    {
        fp = gmx_ffopen(cubeFn.c_str(), "w");
        fprintf(fp, "%s\n", title.c_str());
        fprintf(fp, "POTENTIAL\n");
        fprintf(fp, "%5d%12.6f%12.6f%12.6f\n",
                static_cast<int>(nAtom()),
                gmx2convert(_origin[XX], eg2cBohr),
                gmx2convert(_origin[YY], eg2cBohr),
                gmx2convert(_origin[ZZ], eg2cBohr));
        fprintf(fp, "%5d%12.6f%12.6f%12.6f\n", _nxyz[XX],
                gmx2convert(_space[XX], eg2cBohr), 0.0, 0.0);
        fprintf(fp, "%5d%12.6f%12.6f%12.6f\n", _nxyz[YY],
                0.0, gmx2convert(_space[YY], eg2cBohr), 0.0);
        fprintf(fp, "%5d%12.6f%12.6f%12.6f\n", _nxyz[ZZ],
                0.0, 0.0, gmx2convert(_space[ZZ], eg2cBohr));

        for (size_t m = 0; (m < nAtom()); m++)
        {
            q = ra_[m].charge();
            fprintf(fp, "%5d%12.6f%12.6f%12.6f%12.6f\n",
                    ra_[m].atomnumber(), q,
                    gmx2convert(ra_[m].x()[XX], eg2cBohr),
                    gmx2convert(ra_[m].x()[YY], eg2cBohr),
                    gmx2convert(ra_[m].x()[ZZ], eg2cBohr));
        }

        for (ix = m = 0; ix < _nxyz[XX]; ix++)
        {
            for (iy = 0; iy < _nxyz[YY]; iy++)
            {
                for (iz = 0; iz < _nxyz[ZZ]; iz++, m++)
                {
                    if (src.nEsp() > 0)
                    {
                        pp = _potCalc[m] - src._pot[m];
                        if (NULL != ppcorr)
                        {
                            gmx_stats_add_point(ppcorr,
                                                gmx2convert(src._pot[m], eg2cHartree_e),
                                                gmx2convert(_potCalc[m], eg2cHartree_e), 0, 0);
                        }
                    }
                    else
                    {
                        if (rho == 0)
                        {
                            pp = gmx2convert(_potCalc[m], eg2cHartree_e);
                        }
                        else
                        {
                            pp = _rho[m]*pow(BOHR2NM, 3);
                        }
                    }
                    fprintf(fp, "%13.5e", pp);
                    if (iz % 6 == 5)
                    {
                        fprintf(fp, "\n");
                    }
                    if (NULL != gst)
                    {
                        rmin = 1000;
                        /* Add point to histogram! */
                        for (RespAtomIterator i = ra_.begin(); i < ra_.end(); ++i)
                        {
                            gmx::RVec dx;
                            rvec_sub(i->x(), _esp[m], dx);
                            r = norm(dx);
                            if (r < rmin)
                            {
                                rmin = r;
                            }
                        }
                        gmx_stats_add_point(gst, rmin, pp, 0, 0);
                    }
                }
                if ((iz % 6) != 0)
                {
                    fprintf(fp, "\n");
                }
            }
        }
        fclose(fp);
    }
    if (NULL != gst)
    {
        int   nb = 0;
        real *x  = NULL, *y = NULL;

        fp = xvgropen(histFn.c_str(), "Absolute deviation from QM", "Distance (nm)",
                      "Potential", oenv);
        gmx_stats_dump_xy(gst, fp);
        if (0)
        {
            gmx_stats_make_histogram(gst, 0.01, &nb, ehistoX, 0, &x, &y);
            gmx_stats_free(gst);
            for (i = 0; (i < nb); i++)
            {
                fprintf(fp, "%10g  %10g\n", x[i], y[i]);
            }
            sfree(x);
            sfree(y);
        }
        fclose(fp);
        fp = xvgropen("diff-pot.xvg", "Correlation between QM and Calc", "Pot (QM)",
                      "Pot (Calc)", oenv);
        gmx_stats_dump_xy(ppcorr, fp);
        fclose(fp);
    }
}

void QgenResp::writeCube(const std::string &fn, const std::string &title)
{
    QgenResp dummy;
    writeDiffCube(dummy,  fn, NULL, title, NULL, 0);
}

void QgenResp::writeRho(const std::string &fn, const std::string &title)
{
    QgenResp dummy;
    writeDiffCube(dummy,  fn, NULL, title, NULL, 1);
}

void QgenResp::readCube(const std::string &fn, bool bESPonly)
{
    int             natom, nxyz[DIM] = { 0, 0, 0 };
    double          space[DIM] = { 0, 0, 0 };

    gmx::TextReader tr(fn);
    std::string     tmp;
    int             line = 0;
    bool            bOK  = true;
    while (bOK && tr.readLine(&tmp))
    {
        while (!tmp.empty() && tmp[tmp.length()-1] == '\n')
        {
            tmp.erase(tmp.length()-1);
        }
        if (0 == line)
        {
            printf("%s\n", tmp.c_str());
        }
        else if (1 == line && tmp.compare("POTENTIAL") != 0)
        {
            bOK = false;
        }
        else if (2 == line)
        {
            double origin[DIM];
            bOK = (4 == sscanf(tmp.c_str(), "%d%lf%lf%lf",
                               &natom, &origin[XX], &origin[YY], &origin[ZZ]));
            if (bOK && !bESPonly)
            {
                _origin[XX] = origin[XX];
                _origin[YY] = origin[YY];
                _origin[ZZ] = origin[ZZ];
            }
        }
        else if (3 == line)
        {
            bOK = (2 == sscanf(tmp.c_str(), "%d%lf",
                               &nxyz[XX], &space[XX]));
        }
        else if (4 == line)
        {
            bOK = (2 == sscanf(tmp.c_str(), "%d%*s%lf",
                               &nxyz[YY], &space[YY]));
        }
        else if (5 == line)
        {
            bOK = (2 == sscanf(tmp.c_str(), "%d%*s%*s%lf",
                               &nxyz[ZZ], &space[ZZ]));
            if (bOK)
            {
                for (int m = 0; (m < DIM); m++)
                {
                    _nxyz[m]  = nxyz[m];
                    _space[m] = space[m];
                }
                for (int m = 0; (m < DIM); m++)
                {
                    _origin[m] = convert2gmx(_origin[m], eg2cBohr);
                    _space[m]  = convert2gmx(_space[m], eg2cBohr);
                }
            }
            _pot.clear();
        }
        else if (line >= 6 && line < 6+natom)
        {
            double lx, ly, lz, qq;
            int    anr, m = line - 6;
            bOK = (5 == sscanf(tmp.c_str(), "%d%lf%lf%lf%lf",
                               &anr, &qq, &lx, &ly, &lz));
            if (bOK)
            {
                if (!bESPonly)
                {
                    ra_[m].setAtomnumber(anr);
                    ra_[m].setCharge(qq);
                }
                RVec xx;
                xx[XX] = convert2gmx(lx, eg2cBohr);
                xx[YY] = convert2gmx(ly, eg2cBohr);
                xx[ZZ] = convert2gmx(lz, eg2cBohr);
                ra_[m].setX(xx);
            }
        }
        else if (line >= 6+natom)
        {
            std::vector<std::string> ss = gmx::splitString(tmp);
            for (const auto &s : ss)
            {
                _pot.push_back(convert2gmx(atof(s.c_str()), eg2cHartree_e));
            }
        }

        line++;
    }
    if (bOK)
    {
        _esp.clear();
        for (int ix = 0; ix < _nxyz[XX]; ix++)
        {
            for (int iy = 0; iy < _nxyz[YY]; iy++)
            {
                for (int iz = 0; iz < _nxyz[ZZ]; iz++)
                {
                    rvec e;
                    e[XX] = _origin[XX] + ix*_space[XX];
                    e[YY] = _origin[YY] + iy*_space[YY];
                    e[ZZ] = _origin[ZZ] + iz*_space[ZZ];

                    _esp.push_back(e);
                }
            }
        }
    }
    bOK = bOK && (_esp.size() == _pot.size());
    if (!bOK)
    {
        gmx_fatal(FARGS, "Error reading %s. Found %d potential values, %d coordinates and %d atoms",
                  fn.c_str(), static_cast<int>(_pot.size()), static_cast<int>(_esp.size()),
                  static_cast<int>(ra_.size()));
    }
}

void QgenResp::copyGrid(QgenResp &src)
{
    int m;

    for (m = 0; (m < DIM); m++)
    {
        _origin[m] = src._origin[m];
        _space[m]  = src._space[m];
        _nxyz[m]   = src._nxyz[m];
    }
    int nesp = src.nEsp();
    _esp.clear();
    _pot.resize(nesp, 0);
    _potCalc.resize(nesp, 0);
    for (m = 0; (m < nesp); m++)
    {
        _esp.push_back(src._esp[m]);
    }
}

void QgenResp::makeGrid(real spacing, matrix box, rvec x[])
{
    if (0 != nEsp())
    {
        fprintf(stderr, "Overwriting existing ESP grid\n");
    }
    if (spacing <= 0)
    {
        spacing = 0.1;
        fprintf(stderr, "spacing too small, setting it to %g\n", spacing);
    }
    for (size_t i = 0; (i < nAtom()); i++)
    {
        ra_[i].setX(x[i]);
    }
    for (int m = 0; (m < DIM); m++)
    {
        _nxyz[m]  = 1+(int) (box[m][m]/spacing);
        _space[m] = box[m][m]/_nxyz[m];
    }
    _esp.clear();
    _potCalc.clear();
    for (int i = 0; (i < _nxyz[XX]); i++)
    {
        rvec xyz;
        xyz[XX] = (i-0.5*_nxyz[XX])*_space[XX];
        for (int j = 0; (j < _nxyz[YY]); j++)
        {
            xyz[YY] = (j-0.5*_nxyz[YY])*_space[YY];
            for (int k = 0; (k < _nxyz[ZZ]); k++)
            {
                xyz[ZZ] = (k-0.5*_nxyz[ZZ])*_space[ZZ];
                _esp.push_back(xyz);
                _potCalc.push_back(0);
            }
        }
    }
}

void QgenResp::calcRho()
{
    double pi32 = pow(M_PI, -1.5);
    if (_rho.size() < nEsp())
    {
        _rho.resize(nEsp(), 0);
    }
    for (size_t i = 0; (i < _rho.size()); i++)
    {
        double V = 0;
        for (const auto &ra : ra_)
        {
            double               vv = 0;
            gmx::RVec            dx;
            rvec_sub(_esp[i], ra.x(), dx);
            double               r     = norm(dx);
            int                  atype = ra.atype();
            RespAtomTypeIterator rat   = findRAT(atype);
            GMX_RELEASE_ASSERT(rat == endRAT(), "Can not find atomtype");
            switch (_iDistributionModel)
            {
                case eqdYang:
                case eqdRappe:
                    vv = ra.charge()*Nuclear_SS(r,
                                                rat->beginRZ()->row(),
                                                rat->beginRZ()->zeta());
                    break;
                case eqdAXg:
                    vv = 0;
                    for (auto k = rat->beginRZ(); k < rat->endRZ(); ++k)
                    {
                        real z = k->zeta();
                        real q = k->q();
                        if (k == rat->endRZ()-1)
                        {
                            q = ra.charge();
                        }
                        if (z > 0 && q != 0)
                        {
                            vv -= (q*pi32*exp(-gmx::square(r*z))*
                                   pow(z, 3));
                        }
                    }
                    break;
                case eqdBultinck:
                case eqdAXp:
                case eqdAXs:
                default:
                    gmx_fatal(FARGS, "Krijg nou wat, iDistributionModel = %d!",
                              _iDistributionModel);
            }
            V  += vv;
        }
        _rho[i] = V;
    }
}

void QgenResp::calcPot()
{
    std::fill(_potCalc.begin(), _potCalc.end(), 0.0);
    int nthreads = gmx_omp_get_max_threads();
    for (auto &ra : ra_)
    {
        int                  atype = ra.atype();
        RespAtomTypeIterator rat   = findRAT(atype);
        gmx::RVec            rax   = ra.x();
#pragma omp parallel
        {
            int thread_id = gmx_omp_get_thread_num();
            int i0        = thread_id*nEsp()/nthreads;
            int i1        = std::min(nEsp(), (thread_id+1)*nEsp()/nthreads);
            for (int i = i0; (i < i1); i++)
            {
                double r2 = 0;
                for (int m = 0; m < DIM; m++)
                {
                    r2 += gmx::square(_esp[i][m] - rax[m]);
                }
                double r  = std::sqrt(r2);
                double vv = 0;
                for (auto k = rat->beginRZ(); k < rat->endRZ(); ++k)
                {
                    real q = k->q();
                    if (k == rat->endRZ()-1)
                    {
                        q = ra.charge();
                    }
                    switch (_iDistributionModel)
                    {
                        case eqdBultinck:
                        case eqdAXp:
                            if (r > 0.01)
                            {
                                vv += q/r;
                            }
                            break;
                        case eqdAXs:
                            vv += q*Nuclear_SS(r,
                                               k->row(),
                                               k->zeta());
                            break;
                        case eqdYang:
                        case eqdRappe:
                            vv += q*Nuclear_SS(r,
                                               rat->beginRZ()->row(),
                                               rat->beginRZ()->zeta());
                            break;
                        case eqdAXg:
                            vv += q*Nuclear_GG(r, k->zeta());
                            break;
                        default:
                            gmx_fatal(FARGS, "Krijg nou wat, iDistributionModel = %s!",
                                      getEemtypeName(_iDistributionModel));
                    }
                }
                _potCalc[i] += vv*ONE_4PI_EPS0;
            }
        }
    }
}

void QgenResp::warning(const std::string fn, int line)
{
    fprintf(stderr, "WARNING: It seems like you have two sets of ESP data in your file\n         %s\n", fn.c_str());
    fprintf(stderr, "         using the second set, starting at line %d\n", line);
}

void QgenResp::setVector(double *params)
{
    size_t n = 0;

    for (const auto &rp : raparam_)
    {
        if (rp.eParam() == eparmQ)
        {
            /* First do charges */
            int atom = rp.aIndex();
            params[n] = ra_[atom].charge();
            if (_bRandQ)
            {
                params[n] += 0.2*(gmx_rng_uniform_real(rnd_)-0.5);
            }
        }
        else
        {
            int                  atype = rp.aIndex();
            int                  zz    = rp.zIndex();
            RespAtomTypeIterator rai   = findRAT(atype);
            RowZetaQIterator     rzi   = rai->beginRZ() + zz;
            if (_bRandZeta)
            {
                real zmin = _zmin;
                real zmax = _zmax;

                if ((_deltaZ > 0) && (rai->getBRestrained()))
                {
                    zmin = rzi->zetaRef()-_deltaZ;
                    zmax = rzi->zetaRef()+_deltaZ;
                }
                /*if ((zz > 1) && (_rDecrZeta >= 0))
                    {
                    zmax = ra_[i].getZeta(zz-1)-_rDecrZeta;
                    if (zmax < zmin)
                    {
                    zmax = (zmin+ra_[i].getZeta(zz-1))/2;
                    }
                    }*/
                params[n] = zmin + (zmax-zmin)*gmx_rng_uniform_real(rnd_);
            }
            else
            {
                params[n] = rzi->zeta();
            }
        }
        n++;
    }
}

void QgenResp::getVector(double *params)
{
    double qtot = 0;
    for (auto &ra : ra_)
    {
        /* First do charges */
        int qi = ra.qIndex();
        if (qi >= 0)
        {
            ra.setCharge(params[qi]);
            qtot += params[qi];
        }
        // Make sure to add the charges for nuclei to qtot
        auto rat = findRAT(ra.atype());
        for (auto rz = rat->beginRZ(); rz < rat->endRZ()-1; ++rz)
        {
            qtot += rz->q();
        }
    }
    ra_[0].setCharge(_qtot-qtot);

    for (auto &rat : ratype_)
    {
        for (auto rz = rat.beginRZ(); rz < rat.endRZ(); ++rz)
        {
            int zi = rz->zIndex();
            if (zi >= 0)
            {
                rz->setZeta(params[zi]);
            }
        }
    }
}

void QgenResp::addEspPoint(double x, double y,
                           double z, double V)
{
    rvec e;
    e[XX] = x;
    e[YY] = y;
    e[ZZ] = z;

    _esp.push_back(e);
    _pot.push_back(V);
    _potCalc.push_back(0);
}

real QgenResp::myWeight(size_t iatom) const
{
    if (iatom < nAtom())
    {
        return _watoms;
    }
    else
    {
        return 1.0;
    }
}

void QgenResp::potLsq(gmx_stats_t lsq)
{
    for (size_t i = 0; (i < nEsp()); i++)
    {
        double w = myWeight( i);
        if (w > 0)
        {
            gmx_stats_add_point(lsq,
                                gmx2convert(_pot[i], eg2cHartree_e),
                                gmx2convert(_potCalc[i], eg2cHartree_e), 0, 0);
        }
    }
}

void QgenResp::calcRms()
{
    double pot2, s2, sum2, w, wtot, entropy;
    char   buf[STRLEN];

    pot2 = sum2 = wtot = entropy = 0;
    sprintf(buf, " - weight %g in fit", _watoms);
    for (size_t i = 0; (i < nEsp()); i++)
    {
        w = myWeight(i);
        if ((NULL != debug) && (i < 4*nAtom()))
        {
            fprintf(debug, "ESP %d QM: %g FIT: %g DIFF: %g%s\n",
                    static_cast<int>(i), _pot[i], _potCalc[i],
                    _pot[i]-_potCalc[i],
                    (i < nAtom())  ? buf : "");
        }
        s2    = w*gmx::square(_pot[i]-_potCalc[i]);
        if ((s2 > 0) && (_bEntropy))
        {
            entropy += s2*log(s2);
        }
        sum2 += s2;
        pot2 += w*gmx::square(_pot[i]);
        wtot += w;
    }
    _wtot = wtot;
    if (wtot > 0)
    {
        _rms     = gmx2convert(sqrt(sum2/wtot), eg2cHartree_e);
        _entropy = gmx2convert(entropy/wtot, eg2cHartree_e);
    }
    else
    {
        _rms     = 0;
        _entropy = 0;
    }
    _rrms = sqrt(sum2/pot2);
}

real QgenResp::getRms(real *wtot, real *rrms)
{
    calcRms();
    *wtot = _wtot;
    *rrms = _rrms;
    if (_bEntropy)
    {
        return _entropy;
    }
    else
    {
        return _rms;
    }
}

double QgenResp::calcPenalty()
{
    double p, b2;

    p = 0;
    /* Check for excessive charges */
    for (auto &ra : ra_)
    {
        real                 qi    = ra.charge();
        int                  atype = ra.atype();
        RespAtomTypeIterator rat   = findRAT(atype);
        for (auto z = rat->beginRZ(); z < rat->endRZ()-1; ++z)
        {
            qi += z->q();
        }
        if (qi < _qmin)
        {
            p += gmx::square(_qmin-qi);
        }
        else if (qi > _qmax)
        {
            p += gmx::square(_qmax-qi);
        }
        else if ((qi < -0.02) && (ra.atomnumber() == 1))
        {
            p += qi*qi;
        }
    }
    p *= _pfac;
    if (_bAXpRESP && (_iDistributionModel == eqdAXp))
    {
        b2 = gmx::square(_bHyper);
        for (size_t i = 0; (i < nAtom()); i++)
        {
            p += sqrt(gmx::square(ra_[i].charge()) + b2) - _bHyper;
        }
        p = (_qfac * p);
    }
    _penalty = p;

    return _penalty;
}

// Writen in C style, needed as an function argument
double chargeFunction(void *gr, double v[])
{
    QgenResp *resp = (QgenResp *)gr;
    real      rrms, rms  = 0;
    real      wtot;

    resp->getVector(v);
    resp->calcPot();
    double penalty = resp->calcPenalty();
    rms = resp->getRms(&wtot, &rrms);

    return rms + penalty;
}

void QgenResp::statistics(int len, char buf[])
{
    if (len >= 100)
    {
        sprintf(buf, "RMS: %10e [Hartree/e] RRMS: %10e Entropy: %10e Penalty: %10e",
                _rms, _rrms, _entropy, _penalty);
    }
    else
    {
        fprintf(stderr, "buflen too small (%d) in gmx_resp_statistics\n", len);
    }
}

int my_mc(void                *data,
          nm_target_func       func,
          std::vector<double> &start,
          int                  MAX_IT,
          double              *chi2_final)
{
    gmx_rng_t           rnd      = gmx_rng_init(gmx_rng_make_seed());
    real                chi2_min = func(data, start.data());
    std::vector<double> best     = start;
    real                chi2     = chi2_min;
    real                beta     = 2;

    for (int step = 0; step < MAX_IT; step++)
    {
        //printf("my_mc step %d chi2_min %g\n", step, chi2_min);
        for (size_t np = 0; np < start.size(); np++)
        {
            real delta = 0.2*gmx_rng_uniform_real(rnd) - 0.1;
            start[np] += delta;
            real chi2_test = func(data, start.data());
            if (chi2_test < chi2_min)
            {
                best     = start;
                chi2     = chi2_test;
                chi2_min = chi2_test;
            }
            else if (chi2_test < chi2 ||
                     exp(-beta*(chi2_test/chi2-1)) < gmx_rng_uniform_real(rnd))
            {
                chi2 = chi2_test;
            }
            else
            {
                start[np] -= delta;
            }
        }
    }
    start       = best;
    *chi2_final = chi2_min;

    return true;
}

void LeastSquaresFit(int      ncolumn,
                     int      nrow,
                     double **a,
                     double  *x,
                     double  *rhs)
{
    double **aT  = alloc_matrix(ncolumn, nrow);
    double **aTa = alloc_matrix(ncolumn, ncolumn);
    int      row;

    for (int c = 0; c < ncolumn; c++)
    {
        for (int r = 0; r < nrow; r++)
        {
            aT[c][r] = a[r][c];
        }
    }
    matrix_multiply(debug, nrow, ncolumn, a, aT, aTa);
    if ((row = matrix_invert(debug, ncolumn, aTa)) != 0)
    {
        int k = row - 1;
        for (int m = 0; (m < ncolumn); m++)
        {
            if (m == k)
            {
                continue;
            }
            bool   bSame = true;
            double bfac1 = 0, bfac2 = 0;
            for (int l = 0; bSame && (l < nrow); l++)
            {
                if ((a[m][l] != 0) || (a[k][l] != 0))
                {
                    if (a[m][l] != 0)
                    {
                        bfac2 = (1.0*a[k][l])/a[m][l];
                        if ((bfac1 == 0) && (bfac2 != 0))
                        {
                            bfac1 = bfac2;
                        }
                        else if (bfac1 != 0)
                        {
                            bSame = (bfac1 == bfac2);
                        }
                    }
                }
            }
            if (bSame)
            {
                gmx_fatal(FARGS, "Colums %d and %d are identical bfac1 = %g",
                          k + 1, m + 1, bfac1);
            }
        }
        gmx_fatal(FARGS, "Matrix inversion failed. Incorrect column = %d, ncolumn = %d.\nThis probably indicates that you do not have sufficient data points, or that some parameters are linearly dependent.",
                  row, ncolumn);
    }
    double  da0, chi2;
    double  a0    = 0;
    int     niter = 0;
    bool    bZero = false;
    double *aTx;
    snew(aTx, ncolumn);
    do
    {
        for (int i = 0; (i < ncolumn); i++)
        {
            aTx[i] = 0;
            for (int j = 0; (j < nrow); j++)
            {
                aTx[i] += aT[i][j]*(rhs[j]-a0);
            }
        }
        for (int i = 0; (i < ncolumn); i++)
        {
            x[i] = 0;
            for (int j = 0; (j < ncolumn); j++)
            {
                x[i] += aTa[i][j]*aTx[j];
            }
        }
        da0  = 0;
        chi2 = 0;
        if (bZero)
        {
            for (int j = 0; (j < nrow); j++)
            {
                double ax = a0;
                for (int i = 0; (i < ncolumn); i++)
                {
                    ax += x[i]*a[j][i];
                }
                da0  += (rhs[j]-ax);
                chi2 += gmx::square(rhs[j]-ax);
            }
            da0 = da0 / nrow;
            a0 += da0;
            niter++;
            printf("iter: %d, a0 = %g, chi2 = %g\n",
                   niter, a0, chi2/nrow);
        }
    }
    while ((fabs(da0) > 1e-5) && (niter < 1000));
    sfree(aTx);
    free_matrix(aT);
    free_matrix(aTa);
}

int QgenResp::optimizeCharges(int maxiter, real *rms)
{
    if (_bFitZeta)
    {
        std::vector<double> param;
        double              ccc;
        int                 bConv;
        char                buf[STRLEN];

        param.resize(nParam(), 0);
        setVector(param.data());

        //    bConv = nmsimplex(fp, (void *)this, chargeFunction,
        //                param.data(), nParam(),
        //                toler, 1, maxiter, &ccc);
        bConv = my_mc((void *)this, chargeFunction,
                      param, maxiter, &ccc);
        if (bConv)
        {
            statistics(STRLEN-1, buf);
        }
        else
        {
            printf("NM Simplex did not converge\n\n");
        }

        if (_bEntropy)
        {
            *rms = _entropy;
        }
        else
        {
            *rms = _rms;
        }

        getVector(param.data());

        if (bConv)
        {
            return eQGEN_OK;
        }
        else
        {
            return eQGEN_NOTCONVERGED;
        }
    }
    else
    {
        int nrow      = nEsp();
        // Increase number of rows for the symmetric atoms. E.g.
        // if we know that atoms 2, 3 and 4 have the same charge we
        // add two equation q2 - q3 = 0 and q2 - q4 = 0.
        // An extra row is needed to fix the total charge but this
        // is taken into account in raparam_ already, that is it has
        // at least one less charge parameter than there are atoms.
        int nqparm = std::count_if(raparam_.begin(), raparam_.end(),
                                   [](RespParam const &rp)
                                   { return (rp.eParam() == eparmQ); });
        nrow    += nAtom() - nqparm;
        int ncolumn  = nAtom();
        int j0       = 0;
        if (_watoms == 0)
        {
            j0    = nAtom();
            nrow -= nAtom();
        }
        double            **a = alloc_matrix(nrow, ncolumn);
        std::vector<double> rhs;

        for (size_t j = j0; j < nEsp(); j++)
        {
            rhs.push_back(_pot[j]);
        }

        for (size_t i = 0; i < nAtom(); i++)
        {
            int                  atype = ra_[i].atype();
            RespAtomTypeIterator rat   = findRAT(atype);
            RVec                 rx    = ra_[i].x();

            for (size_t j = j0; j < nEsp(); j++)
            {
                rvec dx;
                for (int m = 0; m < DIM; m++)
                {
                    dx[m] = _esp[j][m] - rx[m];
                }
                double r   = norm(dx);
                double r_1 = 0;
                if (r > 0)
                {
                    r_1 = 1.0/r;
                }
                for (auto k = rat->beginRZ(); k < rat->endRZ(); ++k)
                {
                    double pot = 0;
                    switch (_iDistributionModel)
                    {
                        case eqdAXp:
                            pot = r_1;
                            break;
                        case eqdAXg:
                            pot = Nuclear_GG(r, k->zeta());
                            break;
                        case eqdAXs:
                            pot = Nuclear_SS(r, k->row(), k->zeta());
                            break;
                        default:
                            gmx_fatal(FARGS, "Go to jail. Don't go throw start.");
                    }
                    if (k < rat->endRZ() - 1)
                    {
                        rhs[j-j0] -= k->q()*pot*ONE_4PI_EPS0;
                    }
                    else
                    {
                        a[j-j0][i] += pot*ONE_4PI_EPS0;
                    }
                }
                if (i == 0 && j < 4*nAtom() && debug)
                {
                    fprintf(debug, "j = %d r = %g AJI = %g dx = %g %g %g\n",
                            static_cast<int>(j), r, a[j-j0][i], dx[XX], dx[YY], dx[ZZ]);
                }

            }
        }
        // Add the equations to assure symmetric charges
        int    j1     = nEsp() - j0;
        double factor = 1000000;
        for (int i = 0; i < static_cast<int>(nAtom()); i++)
        {
            if (symmetricAtoms_[i] < i)
            {
                a[j1][i]                  =  factor;
                a[j1][symmetricAtoms_[i]] = -factor;
                rhs.push_back(0);
                j1++;
            }
        }
        GMX_RELEASE_ASSERT(j1 == static_cast<int>(rhs.size()), "Inconsistency adding equations for symmetric charges");
        GMX_RELEASE_ASSERT(j1 == nrow-1, "Something fishy adding equations for symmetric charges");
        // Use the last row for the total charge
        double qtot = 0;
        for (size_t i = 0; i < nAtom(); i++)
        {
            a[nrow-1][i] = factor;
            int                  atype = ra_[i].atype();
            RespAtomTypeIterator rat   = findRAT(atype);
            for (auto k = rat->beginRZ(); k < rat->endRZ() -1; ++k)
            {
                qtot += k->q();
            }
        }
        //        for(size_t j = j0; j < nEsp(); j++)
        //{
        //    a[j-j0][ncolumn-1] = -1;
        //}
        rhs.push_back(factor* (_qtot - qtot));
        if (debug)
        {
            for (int i = 0; i < nrow; i++)
            {
                fprintf(debug, "ROW");
                for (int j = 0; j < ncolumn; j++)
                {
                    fprintf(debug, "  %8g", a[i][j]);
                }
                fprintf(debug, "  %8g\n", rhs[i]);
            }
        }
        double  *x;
        snew(x, ncolumn);
        LeastSquaresFit(ncolumn, nrow, a, x, rhs.data());
        for (size_t i = 0; i < nAtom(); i++)
        {
            ra_[i].setCharge(x[i]);
            if (debug)
            {
                fprintf(debug, "Q[%d] = %g\n", static_cast<int>(i), x[i]);
            }
        }
        sfree(x);

        free_matrix(a);
        calcPot();
        real rrms, wtot;
        real rms = getRms(&wtot, &rrms);
        printf("RESP: RMS %g RRMS %g\n", rms, rrms);

        return eQGEN_OK;
    }
}

void QgenResp::potcomp(const std::string      &potcomp,
                       const std::string      &pdbdiff,
                       const gmx_output_env_t *oenv)
{
    double  pp, exp, eem;
    FILE   *fp;
    int     unit = eg2cHartree_e;

    if (0 != potcomp.size())
    {
        const char *pcleg[2] = { "Atoms", "ESP points" };
        fp = xvgropen(potcomp.c_str(), "Electrostatic potential", unit2string(unit), unit2string(unit), oenv);
        xvgr_legend(fp, 2, pcleg, oenv);
        fprintf(fp, "@type xy\n");
        for (size_t i = 0; (i < nEsp()); i++)
        {
            /* Conversion may or may not be in vain depending on unit */
            exp = gmx2convert(_pot[i], unit);
            eem = gmx2convert(_potCalc[i], unit);
            if (i == nAtom())
            {
                fprintf(fp, "&\n");
                fprintf(fp, "@type xy\n");
            }
            fprintf(fp, "%10.5e  %10.5e\n", exp, eem);
        }
        fprintf(fp, "&\n");
        fclose(fp);
    }
    if (0 != pdbdiff.c_str())
    {
        fp = fopen(pdbdiff.c_str(), "w");
        fprintf(fp, "REMARK All distances are scaled by a factor of two.\n");
        for (size_t i = 0; (i < nEsp()); i++)
        {
            exp = gmx2convert(_pot[i], eg2cHartree_e);
            eem = gmx2convert(_potCalc[i], eg2cHartree_e);
            pp  = _pot[i]-_potCalc[i];
            fprintf(fp, "%-6s%5u  %-4.4s%3.3s %c%4d%c   %8.3f%8.3f%8.3f%6.2f%6.2f\n",
                    "ATOM", 1, "HE", "HE", ' ', static_cast<int>(i+1),
                    ' ', 20*_esp[i][XX], 20*_esp[i][YY], 20*_esp[i][ZZ], 0.0, pp);
        }
        fclose(fp);
    }
}

double QgenResp::getAtomCharge(int atom) const
{
    range_check(atom, 0, nAtom());
    double                    q     = ra_[atom].charge();
    int                       atype = ra_[atom].atype();
    RespAtomTypeConstIterator rat   = findRAT(atype);
    for (auto z = rat->beginRZ(); z < rat->endRZ()-1; ++z)
    {
        q += z->q();
    }
    return q;
}

double QgenResp::getCharge(int atom, size_t zz) const
{
    range_check(atom, 0, nAtom());
    double                    q     = ra_[atom].charge();
    int                       atype = ra_[atom].atype();
    RespAtomTypeConstIterator rat   = findRAT(atype);
    if (zz < rat->getNZeta()-1)
    {
        q = (rat->beginRZ()+zz)->q();
    }
    return q;
}

double QgenResp::getZeta(int atom, int zz) const
{
    range_check(atom, 0, nAtom());
    int atype                     = ra_[atom].atype();
    RespAtomTypeConstIterator rat = findRAT(atype);
    range_check(zz, 0, rat->getNZeta());

    return (rat->beginRZ()+zz)->zeta();
}

void QgenResp::setCharge(int atom, int zz, double q)
{
    range_check(atom, 0, nAtom());
    int                  atype = ra_[atom].atype();
    RespAtomTypeIterator rat   = findRAT(atype);
    range_check(zz, 0, rat->getNZeta());
    (rat->beginRZ()+zz)->setQ(q);
}

void QgenResp::setZeta(int atom, int zz, double zeta)
{
    range_check(atom, 0, nAtom());
    int                  atype = ra_[atom].atype();
    RespAtomTypeIterator rat   = findRAT(atype);
    range_check(zz, 0, rat->getNZeta());
    (rat->beginRZ()+zz)->setZeta(zeta);
}

} // namespace
