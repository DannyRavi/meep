/* Copyright (C) 2004 Massachusetts Institute of Technology.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <math.h>

#include "meep/mympi.hpp"
#include "bicgstab.hpp"

#include "config.h"

/* bicgstab() implements an iterative solver for non-symmetric linear
   operators, using the algorithm described in:

      Gerard L. G. Sleijpen and Diederik R. Fokkema, "BiCGSTAB(L) for
      linear equations involving unsymmetric matrices with complex
      spectrum," Electronic Trans. on Numerical Analysis 1, 11-32
      (1993).

   and also:

      Gerard L.G. Sleijpen, Henk A. van der Vorst, and Diederik
      R. Fokkema, " BiCGstab(L) and other Hybrid Bi-CG Methods,"
      Numerical Algorithms 7, 75-109 (1994).

   This is a generalization of the stabilized biconjugate-gradient
   (BiCGSTAB) algorithm descrbed by van der Vorst (and also described
   in the book _Templates for the Solution of Linear Systems_ by
   Barrett et al.)  BiCGSTAB(1) is equivalent to BiCGSTAB, and
   BiCGSTAB(2) is a slightly more efficient version of the BiCGSTAB2
   algorithm by Gutknecht, while BiCGSTAB(L>2) is a further
   generalization.

   The reason that we use this generalization of BiCGSTAB is that the
   BiCGSTAB(1) algorithm was observed by Sleijpen and Fokkema to have
   poor (or even failing) convergence when the linear operator has
   near-pure imaginary eigenvalues.  This is precisely the case for
   our problem (the eigenvalues of the timestep operator are i*omega),
   and we observed precisely such stagnation of convergence.  The
   BiCGSTAB(2) algorithm was reported to fix most such convergence
   problems. */

/* Other variations to explore:

   G. L. G. Sleijpen and H. A. van der Vorst, "Reliable updated
   residuals in hybrid Bi-CG methods," Computing 56 (2), 141-163
   (1996).

   G. L. G. Sleijpen and H. A. van der Vorst, "Maintaining convergence
   properties of BiCGstab methods in finite precision arithmetic,"
   Numerical Algorithms 10, 203-223 (1995).

   See also code on Sleijpen's web page:
                 http://www.math.uu.nl/people/sleijpen/

*/

namespace meep {

#ifdef HAVE_CBLAS_DDOT
   extern "C" double cblas_ddot(int, const double*, int, const double*, int);
#  define dot(n, x, y) sum_to_all(cblas_ddot(n, x, 1, y, 1))
#else
static double dot(int n, const double *x, const double *y)
{
  double sum = 0;
  for (int i = 0; i < n; ++i) sum += x[i] * y[i];
  return sum_to_all(sum);
}
#endif

static double norm2(int n, const double *x) { return sqrt(dot(n, x, x)); }

#if defined(HAVE_CBLAS_DAXPY)
   extern "C" void cblas_daxpy(const int N, const double alpha, const double *X, const int incX, double *Y, const int incY);
#  define xpay(n,x,a,y) cblas_daxpy(n, a,y,1, x,1)
#else
static double xpay(int n, double *x, double a, const double *y) {
  for (int m = 0; m < n; ++m) x[m] += a * y[m];
}
#endif

/* BiCGSTAB(L) algorithm for the n-by-n problem Ax = b */
int bicgstabL(const int L, const int n, double *x,
	      bicgstab_op A, void *Adata, const double *b,
	      const double tol,
	      int *iters,
	      double *work,
	      const bool quiet)
{
  if (!work) return (2*L+3)*n; // required workspace

  double **r = new (double*)[L+1];
  double **u = new (double*)[L+1];
  for (int i = 0; i <= L; ++i) {
    r[i] = work + i * n;
    u[i] = work + (L+1 + i) * n;
  }

  double bnrm = norm2(n, b);
  if (bnrm == 0.0) bnrm = 1.0;
  
  int iter = 0;

  double *gamma = new double[L + 1];
  double *gamma_p = new double[L + 1];
  double *gamma_pp = new double[L + 1];

  double *tau = new double[L * L];
  double *sigma = new double[L + 1];

  int ierr = 0; // error code to return, if any
  const double breaktol = 1e-30;

  /**** FIXME: check for breakdown conditions(?) during iteration  ****/

  // rtilde = r[0] = b - Ax
  double *rtilde = work + (2*L+2) * n;
  A(x, r[0], Adata);
  for (int m = 0; m < n; ++m) rtilde[m] = r[0][m] = b[m] - r[0][m];

  memset(u[0], 0, sizeof(double) * n); // u[0] = 0

  double rho = 1.0, alpha = 0, omega = 1;

  double resid;
  while ((resid = norm2(n, r[0])) > tol * bnrm) {
    ++iter;
    if (!quiet) master_printf("residual[%d] = %g\n", iter, resid / bnrm);

    rho = -omega * rho;
    for (int j = 0; j < L; ++j) {
      if (fabs(rho) < breaktol) { ierr = -1; goto finish; }
      double rho1 = dot(n, r[j], rtilde);
      double beta = alpha * rho1 / rho;
      rho = rho1;
      for (int i = 0; i <= j; ++i)
	for (int m = 0; m < n; ++m) u[i][m] = r[i][m] - beta * u[i][m];
      A(u[j], u[j+1], Adata);
      alpha = rho / dot(n, u[j+1], rtilde);
      for (int i = 0; i <= j; ++i)
	xpay(n, r[i], -alpha, u[i+1]);
      A(r[j], r[j+1], Adata);
      xpay(n, x, alpha, u[0]);
    }

    for (int j = 1; j <= L; ++j) {
      for (int i = 1; i < j; ++i) {
	int ij = (j-1)*L + (i-1);
	tau[ij] = dot(n, r[j], r[i]) / sigma[i];
	xpay(n, r[j], -tau[ij], r[i]);
      }
      sigma[j] = dot(n, r[j],r[j]);
      gamma_p[j] = dot(n, r[0], r[j]) / sigma[j];
    }

    omega = gamma[L] = gamma_p[L];
    for (int j = L-1; j >= 1; --j) {
      gamma[j] = gamma_p[j];
      for (int i = j+1; i <= L; ++i)
	gamma[j] -= tau[(i-1)*L + (j-1)] * gamma[i];
    }
    for (int j = 1; j < L; ++j) {
      gamma_pp[j] = gamma[j+1];
      for (int i = j+1; i < L; ++i)
	gamma_pp[j] += tau[(i-1)*L + (j-1)] * gamma[i+1];
    }
    
    xpay(n, x, gamma[1], r[0]);
    xpay(n, r[0], -gamma_p[L], r[L]);
    xpay(n, u[0], -gamma[L], u[L]);
    for (int j = 1; j < L; ++j) { /* TODO: use blas DGEMV (for L > 2) */
      xpay(n, x, gamma_pp[j], r[j]);
      xpay(n, r[0], -gamma_p[j], r[j]);
      xpay(n, u[0], -gamma[j], u[j]);
    }

    if (iter == *iters) { ierr = 1; break; }
  }

  if (!quiet) master_printf("final residual = %g\n", norm2(n, r[0]) / bnrm);

 finish:
  delete[] sigma;
  delete[] tau;
  delete[] gamma_pp;
  delete[] gamma_p;
  delete[] gamma;
  delete[] u;
  delete[] r;

  *iters = iter;
  return ierr;
}

} // namespace meep