#ifndef RCDD_API_H
#define RCDD_API_H

#include "rcdd.h"
#include <R.h>
#include <Rdefines.h>
#include <Rconfig.h>
#include <R_ext/Rdynload.h>

SEXP attribute_hidden lpcdd(SEXP hrep, SEXP objfun, SEXP minimize, SEXP solver)
  {
    static SEXP(*fun)(SEXP, SEXP, SEXP, SEXP) = NULL;
    if(fun == NULL) fun = (SEXP(*)(SEXP, SEXP, SEXP, SEXP)) R_GetCCallable("rcdd", "lpcdd");
    return fun(hrep, objfun, minimize, solver);
  }

SEXP attribute_hidden lpcdd_f(SEXP hrep, SEXP objfun, SEXP minimize, SEXP solver)
{
  static SEXP(*fun)(SEXP, SEXP, SEXP, SEXP) = NULL;
  if(fun == NULL) fun = (SEXP(*)(SEXP, SEXP, SEXP, SEXP)) R_GetCCallable("rcdd", "lpcdd_f");
  return fun(hrep, objfun, minimize, solver);
}

#endif