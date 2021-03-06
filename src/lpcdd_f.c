
/*
 *  rcdd an R interface to cddlib
 *  Copyright (C) 2005    Charles J. Geyer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  A copy of the GNU General Public License is available via WWW at
 *  http://www.gnu.org/copyleft/gpl.html.  You can also obtain it by
 *  writing to the Free Software Foundation, Inc., 59 Temple Place,
 *  Suite 330, Boston, MA  02111-1307  USA.
 */

#include "setoper.h"
#include "cdd.h"
#include <R.h>
#include <Rinternals.h>
#include "mycddio_f.h"
#include <string.h>
#ifdef WOOF
#include <stdio.h>
#endif /* WOOF */
#include "rcdd.h"

SEXP lpcdd_f(SEXP hrep, SEXP objfun, SEXP minimize, SEXP solver)
{
    GetRNGstate();
    if (! isMatrix(hrep))
        error("'hrep' must be matrix");
    if (! isReal(hrep))
        error("'hrep' must be double");
    if (! isReal(objfun))
        error("'objfun' must be double");
    if (! isLogical(minimize))
        error("'minimize' must be logical");
    if (! isString(solver))
        error("'solver' must be character");

    if (LENGTH(minimize) != 1)
        error("'minimize' must be scalar");
    if (LENGTH(solver) != 1)
        error("'solver' must be scalar");

    SEXP hrep_dim;
    PROTECT(hrep_dim = getAttrib(hrep, R_DimSymbol));
    int nrow = INTEGER(hrep_dim)[0];
    int ncol = INTEGER(hrep_dim)[1];
    UNPROTECT(1);

    if (nrow <= 0)
        error("no rows in 'hrep'");
    if (ncol <= 2)
        error("no cols in hrep[ , - c(1, 2)]");
    if (LENGTH(objfun) != ncol - 1)
        error("length(objfun) != ncol(hrep) - 1");

    for (int i = 0; i < nrow * ncol; ++i)
        if (! R_finite(REAL(hrep)[i]))
            error("'hrep' not finite-valued");

    for (int i = 0; i < nrow; ++i) {
        double foo = REAL(hrep)[i];
        if (! (foo == 0.0 || foo == 1.0))
            error("column one of 'hrep' not zero-or-one valued");
    }

    ddf_set_global_constants();

    myfloat value;
    ddf_init(value);

    ddf_MatrixPtr mf = ddf_CreateMatrix(nrow, ncol - 1);
    /* note our matrix has one more column than Fukuda's */

    mf->representation = ddf_Inequality;
    mf->numbtype = ddf_Real;

    /* linearity */
    for (int i = 0; i < nrow; ++i) {
        double foo = REAL(hrep)[i];
        if (foo == 1.0)
            set_addelem(mf->linset, i + 1);
        /* note conversion from zero-origin to one-origin indexing */
    }

    /* matrix */
    for (int j = 1, k = nrow; j < ncol; ++j)
        for (int i = 0; i < nrow; ++i, ++k) {
            ddf_set_d(value, REAL(hrep)[k]);
            ddf_set(mf->matrix[i][j - 1], value);
            /* note our matrix has one more column than Fukuda's */
        }

    /* rowvec */
    for (int j = 0; j < ncol - 1; ++j) {
        ddf_set_d(value, REAL(objfun)[j]);
        ddf_set(mf->rowvec[j], value);
    }

    if(LOGICAL(minimize)[0])
        mf->objective = ddf_LPmin;
    else
        mf->objective = ddf_LPmax;

    ddf_ErrorType err = ddf_NoError;
    ddf_LPPtr lp = ddf_Matrix2LP(mf, &err);

    if (err != ddf_NoError) {
        rrf_WriteErrorMessages(err);
        ddf_FreeLPData(lp);
        ddf_FreeMatrix(mf);
        ddf_clear(value);
        ddf_free_global_constants();
        error("failed");
    }

    ddf_LPSolverType sol = ddf_DualSimplex;
    const char *sol_str = CHAR(STRING_ELT(solver, 0));
    if(strcmp(sol_str, "DualSimplex") == 0)
        sol = ddf_DualSimplex;
    else if(strcmp(sol_str, "CrissCross") == 0)
        sol = ddf_CrissCross;
    else
        error("solver not recognized");

#ifdef WOOF
    ddf_WriteLP(stderr, lp);
#endif /* WOOF */

    /* make map of rows of lp to rows of mf
     * Fukuda duplicates each linearity row of mf, writing each equality
     * as two inequalities.  Have to keep track of which are the two
     * inequality rows so can combine the Lagrange multipliers for the
     * two inequality rows (of lp) to make the Lagrange multiplier for the
     * equality row (of mf).  Note that the R user only knows about mf,
     * which has the dimensions of the input H-rep matrix.
     *
     * Problem: have to decide whether to use 0-origin or 1-origin indexing.
     * When we are told what the Lagrange multipliers are, the indexing is
     * 1-origin.  That's what Fukuda uses for rowsets.  On the other hand
     * C uses 0-origin.  Let's us use that.  Thus we convert from 1-origin
     * to 0-origin before lookup in the map we are about to create (not after).
     *
     * mapto is an integer vector of length lp->m.  It takes values in
     * 0 ... mf->rowsize except that we are not sure that it is a function.
     * Something may not add up.  Will allow value -1 to mean undefined
     * (e. g., there is row of lp that does not correspond to any row of mf).
     *
     * Code to set up mapto should be copied as closely as possible from the
     * code in 154--194 lines of the file cddlp.c (version 094f) in the
     * function dd_Matrix2LP which creates lp.  Specifically (?) see the
     * lines 173--188.  Note that dd_CreateLPData isn't given the data of
     * mf so all copying of rows of mf to rows of lp occurs in dd_Matrix2LP.
     *
     * Meaning of mapto[j] is that row j (0-origin indexing) of lp->A
     * corresponds to row mapto[j] (also 0-origin indexing) of mf->matrix
     * where "corresponds" means that the rows are identical or one is the
     * negative of the other.  To repeat mapto[j] == -1 means no row of
     * original matrix corresponds.
     */

    int len_mapto = lp->m;
    int *mapto = (int *) R_alloc(len_mapto, sizeof(int));
    for (int j = 0; j < len_mapto; j++)
        mapto[j] = -1;
    {
       int irev = mf->rowsize;                   /* cf line 174 cddlp_f.c */
       for (int i = 1; i <= mf->rowsize; i++) {  /* cf line 175 cddlp_f.c */
           if (set_member(i, mf->linset)) {      /* cf line 176 cddlp_f.c */
               irev = irev + 1;                  /* cf line 177 cddlp_f.c */
               mapto[irev - 1] = i - 1;          /* cf line 181 cddlp_f.c */
           }                                     /* cf line 184 cddlp_f.c */
           mapto[i - 1] = i - 1;                 /* cf line 186 cddlp_f.c */
       }                                         /* cf line 189 cddlp_f.c */
    }

#ifdef WOOF
    fprintf(stderr, "mf->rowsize = %d\n", mf->rowsize);
    fprintf(stderr, "mf->colsize = %d\n", mf->colsize);

    fprintf(stderr, "lp->eqnumber = %d\n", lp->eqnumber);
    fprintf(stderr, "lp->m = %d\n", lp->m);
    fprintf(stderr, "lp->d = %d\n", lp->d);
    /* lp->equalityset;  */
    /* lp->A */
    fprintf(stderr, "correspondence of lp rows to mf rows (0-origin index)\n");
    for (int j = 0; j < len_mapto; j++)
        fprintf(stderr, "    lp row %d to mf row %d\n", j, mapto[j]);
#endif /* WOOF */

    ddf_LPSolve(lp, sol, &err); 

    if (err != ddf_NoError) {
        rrf_WriteErrorMessages(err);
        ddf_FreeLPData(lp);
        ddf_FreeMatrix(mf);
        ddf_clear(value);
        ddf_free_global_constants();
        error("failed");
    }

#ifdef WOOF
    switch (lp->LPS) {
        case ddf_LPSundecided:
            fprintf(stderr, "    ddf_LPSundecided\n");
            break;
        case ddf_Optimal:
            fprintf(stderr, "    ddf_Optimal\n");
            break;
        case ddf_Inconsistent:
            fprintf(stderr, "    ddf_Inconsistent\n");
            break;
        case ddf_DualInconsistent:
            fprintf(stderr, "    ddf_DualInconsistent\n");
            break;
        case ddf_StrucInconsistent:
            fprintf(stderr, "    ddf_StrucInconsistent\n");
            break;
        case ddf_StrucDualInconsistent:
            fprintf(stderr, "    ddf_StrucDualInconsistent\n");
            break;
        case ddf_Unbounded:
            fprintf(stderr, "    ddf_Unbounded\n");
            break;
        case ddf_DualUnbounded:
            fprintf(stderr, "    ddf_DualUnbounded\n");
            break;
        default:
            fprintf(stderr, "    default  WTF???\n");
    }
#endif /* WOOF */

    SEXP result;
    if (lp->LPS == ddf_Optimal) {
        SEXP resultnames;
        PROTECT(result = allocVector(VECSXP, 4));
        PROTECT(resultnames = allocVector(STRSXP, 4));

        SET_STRING_ELT(resultnames, 0, mkChar("solution.type"));
        SET_STRING_ELT(resultnames, 1, mkChar("primal.solution"));
        SET_STRING_ELT(resultnames, 2, mkChar("dual.solution"));
        SET_STRING_ELT(resultnames, 3, mkChar("optimal.value"));
        namesgets(result, resultnames);

        SEXP foo, bar, baz;
        SET_VECTOR_ELT(result, 0, ScalarString(mkChar("Optimal")));
        PROTECT(foo = allocVector(REALSXP, ncol - 2));
        PROTECT(bar = allocVector(REALSXP, nrow));
        PROTECT(baz = allocVector(REALSXP, 1));
        SET_VECTOR_ELT(result, 1, foo);
        SET_VECTOR_ELT(result, 2, bar);
        SET_VECTOR_ELT(result, 3, baz);

        if (lp->d != ncol - 1)
            error("Can't happen.  Dimension changed.");
        for (int j = 1; j < ncol - 1; j++) {
            double ax = ddf_get_d(lp->sol[j]);
            REAL(foo)[j - 1] = ax;
        }
        for (int j = 0; j < nrow; j++) {
            REAL(bar)[j] = 0.0;
        }
        for (int j = 1; j < ncol - 1; j++) {
            int qux = lp->nbindex[j + 1];
            if (qux > 0) {
                if (1 <= qux && qux <= len_mapto && mapto[qux - 1] != -1) {
                    int the_row = mapto[qux - 1];
                    int the_sign = mapto[qux - 1] == qux - 1;
                    if (! (0 <= the_row && the_row < nrow))
                        error("Can't happen.  Map mapto maps out of bounds");
                    double ax = ddf_get_d(lp->dsol[j]);
                    if (the_sign)
                        REAL(bar)[the_row] += ax;
                    else
                        REAL(bar)[the_row] -= ax;
                } else {
#ifdef WOOF_WOOF
                    fprintf(stderr, "First place with this error message\n");
#endif /* WOOF_WOOF */
                    error("Can't happen.  Dual solution index out of bounds");
                }
            }
        }
        {
            double ax = ddf_get_d(lp->optvalue);
            REAL(baz)[0] = ax;
        }

        UNPROTECT(4);
    } else if (lp->LPS == ddf_Inconsistent) {
        SEXP resultnames;
        PROTECT(result = allocVector(VECSXP, 2));
        PROTECT(resultnames = allocVector(STRSXP, 2));

        SET_STRING_ELT(resultnames, 0, mkChar("solution.type"));
        SET_STRING_ELT(resultnames, 1, mkChar("dual.direction"));
        namesgets(result, resultnames);

        SEXP foo;
        SET_VECTOR_ELT(result, 0, ScalarString(mkChar("Inconsistent")));
        PROTECT(foo = allocVector(REALSXP, nrow));
        SET_VECTOR_ELT(result, 1, foo);

        if (lp->d != ncol - 1)
            error("Can't happen.  Dimension changed.");
        for (int j = 0; j < nrow; j++) {
            REAL(foo)[j] = 0.0;
        }
        for (int j = 1; j < ncol - 1; j++) {
            int qux = lp->nbindex[j + 1];
            if (qux > 0) {
                if (1 <= qux && qux <= len_mapto && mapto[qux - 1] != -1) {
                    int the_row = mapto[qux - 1];
                    int the_sign = mapto[qux - 1] == qux - 1;
                    if (! (0 <= the_row && the_row < nrow))
                        error("Can't happen.  Map mapto maps out of bounds");
                    double ax = ddf_get_d(lp->dsol[j]);
                    if (the_sign)
                        REAL(foo)[the_row] += ax;
                    else
                        REAL(foo)[the_row] -= ax;
                } else {
#ifdef WOOF_WOOF
                    fprintf(stderr, "Second place with this error message\n");
#endif /* WOOF_WOOF */
                    error("Can't happen.  Dual solution index out of bounds");
                }
            }
        }
        {
            int qux = lp->re;
            if (1 <= qux && qux <= len_mapto && mapto[qux - 1] != -1) {
                int the_row = mapto[qux - 1];
                int the_sign = mapto[qux - 1] == qux - 1;
#ifdef WOOF_WOOF
                fprintf(stderr, "qux = %d\n", qux);
                fprintf(stderr, "the_row = %d\n", the_row);
                fprintf(stderr, "the_sign = %d\n", the_sign);
#endif /* WOOF_WOOF */
                if (! (0 <= the_row && the_row < nrow))
                    error("Can't happen.  Map mapto maps out of bounds");
                double ax = 1.0;
                if (the_sign)
                    REAL(foo)[the_row] += ax;
                else
                    REAL(foo)[the_row] -= ax;
            } else {
#ifdef WOOF_WOOF
                fprintf(stderr, "Third place with this error message\n");
                fprintf(stderr, "qux = %d\n", qux);
                fprintf(stderr, "nrow  = %d\n", nrow);
#endif /* WOOF_WOOF */
                error("Can't happen.  Dual solution index out of bounds");
            }
        }

        UNPROTECT(2);
    } else if (lp->LPS == ddf_DualInconsistent ||
        lp->LPS == ddf_StrucDualInconsistent) {
        SEXP resultnames;
        PROTECT(result = allocVector(VECSXP, 2));
        PROTECT(resultnames = allocVector(STRSXP, 2));

        SET_STRING_ELT(resultnames, 0, mkChar("solution.type"));
        SET_STRING_ELT(resultnames, 1, mkChar("primal.direction"));
        namesgets(result, resultnames);

        SEXP foo;
        if (lp->LPS == ddf_DualInconsistent)
            SET_VECTOR_ELT(result, 0, ScalarString(mkChar("DualInconsistent")));
        else
            SET_VECTOR_ELT(result, 0,
                ScalarString(mkChar("StrucDualInconsistent")));
        PROTECT(foo = allocVector(REALSXP, ncol - 2));
        SET_VECTOR_ELT(result, 1, foo);

        if (lp->d != ncol - 1)
            error("Can't happen.  Dimension changed.");
        for (int j = 1; j < ncol - 1; j++) {
            double ax = ddf_get_d(lp->sol[j]);
            REAL(foo)[j - 1] = ax;
        }

        UNPROTECT(2);
    } else {
        SEXP resultnames;
        PROTECT(result = allocVector(VECSXP, 1));
        PROTECT(resultnames = allocVector(STRSXP, 1));

        SET_STRING_ELT(resultnames, 0, mkChar("solution.type"));
        namesgets(result, resultnames);

        switch (lp->LPS) {
            case ddf_LPSundecided:
                SET_VECTOR_ELT(result, 0,
                    ScalarString(mkChar("LPSundecided")));
                break;
            case ddf_StrucInconsistent:
                SET_VECTOR_ELT(result, 0,
                    ScalarString(mkChar("StrucInconsistent")));
                break;
            case ddf_Unbounded:
                SET_VECTOR_ELT(result, 0,
                    ScalarString(mkChar("Unbounded")));
                break;
            case ddf_DualUnbounded:
                SET_VECTOR_ELT(result, 0,
                    ScalarString(mkChar("DualUnbounded")));
                break;
            default:
                error("unrecognized solution type");
        }

        UNPROTECT(1);
    }

    ddf_FreeLPData(lp);
    ddf_FreeMatrix(mf);
    ddf_clear(value);
    ddf_free_global_constants();

    PutRNGstate();

    UNPROTECT(1);
    return result;
}

