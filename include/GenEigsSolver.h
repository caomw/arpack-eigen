#ifndef GEN_EIGS_SOLVER_H
#define GEN_EIGS_SOLVER_H

#include <Eigen/Core>

#include <vector>
#include <cmath>
#include <complex>
#include <stdexcept>

#include "SelectionRule.h"
#include "UpperHessenbergQR.h"
#include "UpperHessenbergEigen.h"
#include "DoubleShiftQR.h"
#include "MatOp/DenseGenMatProd.h"
#include "MatOp/DenseGenRealShiftSolve.h"
#include "MatOp/DenseGenComplexShiftSolve.h"


///
/// \ingroup EigenSolver
///
/// This class implements the eigen solver for general real matrices.
///
/// Most of the background information documented in the SymEigsSolver class
/// also applies to the GenEigsSolver class here, except that the eigenvalues
/// and eigenvectors of a general matrix can now be complex-valued.
///
/// \tparam Scalar        The element type of the matrix.
///                       Currently supported types are `float`, `double` and `long double`.
/// \tparam SelectionRule An enumeration value indicating the selection rule of
///                       the requested eigenvalues, for example `LARGEST_MAGN`
///                       to retrieve eigenvalues with the largest magnitude.
///                       The full list of enumeration values can be found in
///                       SelectionRule.h .
/// \tparam OpType        The name of the matrix operation class. Users could either
///                       use the DenseGenMatProd wrapper class, or define their
///                       own that impelemnts all the public member functions as in
///                       DenseGenMatProd.
///
/// An example that illustrates the usage of GenEigsSolver is give below:
///
/// \code{.cpp}
/// #include <Eigen/Core>
/// #include <GenEigsSolver.h>  // Also includes <MatOp/DenseGenMatProd.h>
/// #include <iostream>
///
/// int main()
/// {
///     // We are going to calculate the eigenvalues of M
///     Eigen::MatrixXd M = Eigen::MatrixXd::Random(10, 10);
///
///     // Construct matrix operation object using the wrapper class
///     DenseGenMatProd<double> op(M);
///
///     // Construct eigen solver object, requesting the largest
///     // (in magnitude, or norm) three eigenvalues
///     GenEigsSolver< double, LARGEST_MAGN, DenseGenMatProd<double> > eigs(&op, 3, 6);
///
///     // Initialize and compute
///     eigs.init();
///     int nconv = eigs.compute();
///
///     // Retrieve results
///     Eigen::VectorXcd evalues;
///     if(nconv > 0)
///         evalues = eigs.eigenvalues();
///
///     std::cout << "Eigenvalues found:\n" << evalues << std::endl;
///
///     return 0;
/// }
/// \endcode
///
template < typename Scalar = double,
           int SelectionRule = LARGEST_MAGN,
           typename OpType = DenseGenMatProd<double> >
class GenEigsSolver
{
private:
    typedef Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic> Matrix;
    typedef Eigen::Matrix<Scalar, Eigen::Dynamic, 1> Vector;
    typedef Eigen::Array<Scalar, Eigen::Dynamic, 1> Array;
    typedef Eigen::Array<bool, Eigen::Dynamic, 1> BoolArray;
    typedef Eigen::Map<Matrix> MapMat;
    typedef Eigen::Map<Vector> MapVec;

    typedef std::complex<Scalar> Complex;
    typedef Eigen::Matrix<Complex, Eigen::Dynamic, Eigen::Dynamic> ComplexMatrix;
    typedef Eigen::Matrix<Complex, Eigen::Dynamic, 1> ComplexVector;

protected:
    OpType *op;             // object to conduct matrix operation,
                            // e.g. matrix-vector product
    const int dim_n;        // dimension of matrix A
    const int nev;          // number of eigenvalues requested

private:
    const int ncv;          // number of ritz values
    int nmatop;             // number of matrix operations called
    int niter;              // number of restarting iterations

protected:
    Matrix fac_V;           // V matrix in the Arnoldi factorization
    Matrix fac_H;           // H matrix in the Arnoldi factorization
    Vector fac_f;           // residual in the Arnoldi factorization

    ComplexVector ritz_val; // ritz values
    ComplexMatrix ritz_vec; // ritz vectors

private:
    BoolArray ritz_conv;    // indicator of the convergence of ritz values

    const Scalar prec;      // precision parameter used to test convergence
                            // prec = epsilon^(2/3)
                            // epsilon is the machine precision,
                            // e.g. ~= 1e-16 for the "double" type

    // Arnoldi factorization starting from step-k
    void factorize_from(int from_k, int to_m, const Vector &fk)
    {
        if(to_m <= from_k) return;

        fac_f = fk;

        Vector w(dim_n);
        Scalar beta = 0.0;
        // Keep the upperleft k x k submatrix of H and set other elements to 0
        fac_H.rightCols(ncv - from_k).setZero();
        fac_H.block(from_k, 0, ncv - from_k, from_k).setZero();
        for(int i = from_k; i <= to_m - 1; i++)
        {
            beta = fac_f.norm();
            fac_V.col(i).noalias() = fac_f / beta; // The (i+1)-th column
            fac_H(i, i - 1) = beta;

            // w = A * v, v = fac_V.col(i)
            op->perform_op(&fac_V(0, i), w.data());
            nmatop++;

            // First i+1 columns of V
            MapMat Vs(fac_V.data(), dim_n, i + 1);
            // h = fac_H(0:i, i)
            MapVec h(&fac_H(0, i), i + 1);
            h.noalias() = Vs.transpose() * w;

            fac_f = w - Vs * h;
            // Correct f if it is not orthogonal to V
            // Typically the largest absolute value occurs in
            // the first element, i.e., <v1, f>, so we use this
            // to test the orthogonality
            Scalar v1f = fac_f.dot(fac_V.col(0));
            if(v1f > prec || v1f < -prec)
            {
                Vector Vf(i + 1);
                Vf.tail(i) = fac_V.block(0, 1, dim_n, i).transpose() * fac_f;
                Vf[0] = v1f;
                fac_f -= Vs * Vf;
            }
        }
    }

    static bool is_complex(Complex v, Scalar eps)
    {
        return std::abs(v.imag()) > eps;
    }

    static bool is_conj(Complex v1, Complex v2, Scalar eps)
    {
        return std::abs(v1 - std::conj(v2)) < eps;
    }

    // Implicitly restarted Arnoldi factorization
    void restart(int k)
    {
        if(k >= ncv)
            return;

        DoubleShiftQR<Scalar> decomp_ds;
        UpperHessenbergQR<Scalar> decomp_hb;
        Matrix Q = Matrix::Identity(ncv, ncv);

        for(int i = k; i < ncv; i++)
        {
            if(is_complex(ritz_val[i], prec) && is_conj(ritz_val[i], ritz_val[i + 1], prec))
            {
                // H - mu * I = Q1 * R1
                // H <- R1 * Q1 + mu * I = Q1' * H * Q1
                // H - conj(mu) * I = Q2 * R2
                // H <- R2 * Q2 + conj(mu) * I = Q2' * H * Q2
                //
                // (H - mu * I) * (H - conj(mu) * I) = Q1 * Q2 * R2 * R1 = Q * R
                Scalar s = 2 * ritz_val[i].real();
                Scalar t = std::norm(ritz_val[i]);

                decomp_ds.compute(fac_H, s, t);

                // Q -> Q * Qi
                decomp_ds.apply_YQ(Q);
                // H -> Q'HQ
                // Matrix Q = Matrix::Identity(ncv, ncv);
                // decomp_ds.apply_YQ(Q);
                // fac_H = Q.transpose() * fac_H * Q;
                fac_H = decomp_ds.matrix_QtHQ();

                i++;
            } else {
                // QR decomposition of H - mu * I, mu is real
                fac_H.diagonal().array() -= ritz_val[i].real();
                decomp_hb.compute(fac_H);

                // Q -> Q * Qi
                decomp_hb.apply_YQ(Q);
                // H -> Q'HQ = RQ + mu * I
                fac_H = decomp_hb.matrix_RQ();
                fac_H.diagonal().array() += ritz_val[i].real();
            }
        }
        // V -> VQ
        // Q has some elements being zero
        // The first (ncv - k + i) elements of the i-th column of Q are non-zero
        Matrix Vs(dim_n, k + 1);
        int nnz;
        for(int i = 0; i < k; i++)
        {
            nnz = ncv - k + i + 1;
            MapMat V(fac_V.data(), dim_n, nnz);
            MapVec q(&Q(0, i), nnz);
            Vs.col(i).noalias() = V * q;
        }
        Vs.col(k).noalias() = fac_V * Q.col(k);
        fac_V.leftCols(k + 1).noalias() = Vs;

        Vector fk = fac_f * Q(ncv - 1, k - 1) + fac_V.col(k) * fac_H(k, k - 1);
        factorize_from(k, ncv, fk);
        retrieve_ritzpair();
    }

    // Calculate the number of converged Ritz values
    int num_converged(Scalar tol)
    {
        // thresh = tol * max(prec, abs(theta)), theta for ritz value
        Array thresh = tol * ritz_val.head(nev).array().abs().max(prec);
        Array resid = ritz_vec.template bottomRows<1>().transpose().array().abs() * fac_f.norm();
        // Converged "wanted" ritz values
        ritz_conv = (resid < thresh);

        return ritz_conv.cast<int>().sum();
    }

    // Return the adjusted nev for restarting
    int nev_adjusted(int nconv)
    {
        int nev_new = nev;

        // Increase nev by one if ritz_val[nev - 1] and
        // ritz_val[nev] are conjugate pairs
        if(is_complex(ritz_val[nev - 1], prec) &&
           is_conj(ritz_val[nev - 1], ritz_val[nev], prec))
        {
            nev_new = nev + 1;
        }
        // Adjust nev_new again, according to dnaup2.f line 660~674 in ARPACK
        nev_new = nev_new + std::min(nconv, (ncv - nev_new) / 2);
        if(nev_new == 1 && ncv >= 6)
            nev_new = ncv / 2;
        else if(nev_new == 1 && ncv > 3)
            nev_new = 2;

        if(nev_new > ncv - 2)
            nev_new = ncv - 2;

        // Examine conjugate pairs again
        if(is_complex(ritz_val[nev_new - 1], prec) &&
           is_conj(ritz_val[nev_new - 1], ritz_val[nev_new], prec))
        {
            nev_new++;
        }

        return nev_new;
    }

    // Retrieve and sort ritz values and ritz vectors
    void retrieve_ritzpair()
    {
        UpperHessenbergEigen<Scalar> decomp(fac_H);
        ComplexVector evals = decomp.eigenvalues();
        ComplexMatrix evecs = decomp.eigenvectors();

        SortEigenvalue<Complex, SelectionRule> sorting(evals.data(), evals.size());
        std::vector<int> ind = sorting.index();

        // Copy the ritz values and vectors to ritz_val and ritz_vec, respectively
        for(int i = 0; i < ncv; i++)
        {
            ritz_val[i] = evals[ind[i]];
        }
        for(int i = 0; i < nev; i++)
        {
            ritz_vec.col(i) = evecs.col(ind[i]);
        }
    }

protected:
    // Sort the first nev Ritz pairs in decreasing magnitude order
    // This is used to return the final results
    virtual void sort_ritzpair()
    {
        SortEigenvalue<Complex, LARGEST_MAGN> sorting(ritz_val.data(), nev);
        std::vector<int> ind = sorting.index();

        ComplexVector new_ritz_val(ncv);
        ComplexMatrix new_ritz_vec(ncv, nev);
        BoolArray new_ritz_conv(nev);

        for(int i = 0; i < nev; i++)
        {
            new_ritz_val[i] = ritz_val[ind[i]];
            new_ritz_vec.col(i) = ritz_vec.col(ind[i]);
            new_ritz_conv[i] = ritz_conv[ind[i]];
        }

        ritz_val.swap(new_ritz_val);
        ritz_vec.swap(new_ritz_vec);
        ritz_conv.swap(new_ritz_conv);
    }

public:
    ///
    /// Constructor to create a solver object.
    ///
    /// \param op_  Pointer to the matrix operation object, which should implement
    ///             the matrix-vector multiplication operation of \f$A\f$:
    ///             calculating \f$Ay\f$ for any vector \f$y\f$. Users could either
    ///             create the object from the DenseGenMatProd wrapper class, or
    ///             define their own that impelemnts all the public member functions
    ///             as in DenseGenMatProd.
    /// \param nev_ Number of eigenvalues requested. This should satisfy \f$1\le nev \le n-2\f$,
    ///             where \f$n\f$ is the size of matrix.
    /// \param ncv_ Parameter that controls the convergence speed of the algorithm.
    ///             Typically a larger `ncv_` means faster convergence, but it may
    ///             also result in greater memory use and more matrix operations
    ///             in each iteration. This parameter must satisfy \f$nev+2 \le ncv \le n\f$,
    ///             and is advised to take \f$ncv \ge 2\cdot nev + 1\f$.
    ///
    GenEigsSolver(OpType *op_, int nev_, int ncv_) :
        op(op_),
        dim_n(op->rows()),
        nev(nev_),
        ncv(ncv_ > dim_n ? dim_n : ncv_),
        nmatop(0),
        niter(0),
        prec(std::pow(std::numeric_limits<Scalar>::epsilon(), Scalar(2.0 / 3)))
    {
        if(nev_ < 1 || nev_ > dim_n - 2)
            throw std::invalid_argument("nev must satisfy 1 <= nev <= n - 2, n is the size of matrix");

        if(ncv_ < nev_ + 2 || ncv_ > dim_n)
            throw std::invalid_argument("ncv must satisfy nev + 2 <= ncv <= n, n is the size of matrix");
    }

    ///
    /// Providing the initial residual vector for the algorithm.
    ///
    /// \param init_resid Pointer to the initial residual vector.
    ///
    /// **ARPACK-Eigen** (and also **ARPACK**) uses an iterative algorithm
    /// to find eigenvalues. This function allows the user to provide the initial
    /// residual vector.
    ///
    void init(const Scalar *init_resid)
    {
        // Reset all matrices/vectors to zero
        fac_V.resize(dim_n, ncv);
        fac_H.resize(ncv, ncv);
        fac_f.resize(dim_n);
        ritz_val.resize(ncv);
        ritz_vec.resize(ncv, nev);
        ritz_conv.resize(nev);

        fac_V.setZero();
        fac_H.setZero();
        fac_f.setZero();
        ritz_val.setZero();
        ritz_vec.setZero();
        ritz_conv.setZero();

        Vector v(dim_n);
        std::copy(init_resid, init_resid + dim_n, v.data());
        Scalar vnorm = v.norm();
        if(vnorm < prec)
            throw std::invalid_argument("initial residual vector cannot be zero");
        v /= vnorm;

        Vector w(dim_n);
        op->perform_op(v.data(), w.data());
        nmatop++;

        fac_H(0, 0) = v.dot(w);
        fac_f = w - v * fac_H(0, 0);
        fac_V.col(0) = v;
    }

    ///
    /// Providing a random initial residual vector.
    ///
    /// This overloaded function generates a random initial residual vector
    /// for the algorithm. Elements in the vector follow independent Uniform(-0.5, 0.5)
    /// distributions.
    ///
    void init()
    {
        Vector init_resid = Vector::Random(dim_n);
        init_resid.array() -= 0.5;
        init(init_resid.data());
    }

    ///
    /// Conducting the major computation procedure.
    ///
    /// \param maxit Maximum number of iterations allowed in the algorithm.
    /// \param tol Precision parameter for the calculated eigenvalues.
    ///
    /// \return Number of converged eigenvalues.
    ///
    int compute(int maxit = 1000, Scalar tol = 1e-10)
    {
        // The m-step Arnoldi factorization
        factorize_from(1, ncv, fac_f);
        retrieve_ritzpair();
        // Restarting
        int i, nconv, nev_adj;
        for(i = 0; i < maxit; i++)
        {
            nconv = num_converged(tol);
            if(nconv >= nev)
                break;

            nev_adj = nev_adjusted(nconv);
            restart(nev_adj);
        }
        // Sorting results
        sort_ritzpair();

        niter += i + 1;

        return std::min(nev, nconv);
    }

    ///
    /// Returning the number of iterations used in the computation.
    ///
    int num_iterations() { return niter; }

    ///
    /// Returning the number of matrix operations used in the computation.
    ///
    int num_operations() { return nmatop; }

    ///
    /// Returning the converged eigenvalues.
    ///
    /// \return A complex-valued vector containing the eigenvalues.
    /// Returned vector type will be `Eigen::Vector<std::complex<Scalar>, ...>`, depending on
    /// the template parameter `Scalar` defined.
    ///
    ComplexVector eigenvalues()
    {
        int nconv = ritz_conv.cast<int>().sum();
        ComplexVector res(nconv);

        if(!nconv)
            return res;

        int j = 0;
        for(int i = 0; i < nev; i++)
        {
            if(ritz_conv[i])
            {
                res[j] = ritz_val[i];
                j++;
            }
        }

        return res;
    }

    ///
    /// Returning the eigenvectors associated with the converged eigenvalues.
    ///
    /// \return A complex-valued matrix containing the eigenvectors.
    /// Returned matrix type will be `Eigen::Matrix<std::complex<Scalar>, ...>`,
    /// depending on the template parameter `Scalar` defined.
    ///
    ComplexMatrix eigenvectors()
    {
        int nconv = ritz_conv.cast<int>().sum();
        ComplexMatrix res(dim_n, nconv);

        if(!nconv)
            return res;

        ComplexMatrix ritz_vec_conv(ncv, nconv);
        int j = 0;
        for(int i = 0; i < nev; i++)
        {
            if(ritz_conv[i])
            {
                ritz_vec_conv.col(j) = ritz_vec.col(i);
                j++;
            }
        }

        res.noalias() = fac_V * ritz_vec_conv;

        return res;
    }
};





///
/// \ingroup EigenSolver
///
/// This class implements the eigen solver for general real matrices with
/// a real shift value in the **shift-and-invert mode**. The background
/// knowledge of the shift-and-invert mode can be found in the documentation
/// of the SymEigsShiftSolver class.
///
/// \tparam Scalar        The element type of the matrix.
///                       Currently supported types are `float`, `double` and `long double`.
/// \tparam SelectionRule An enumeration value indicating the selection rule of
///                       the shifted-and-inverted eigenvalues.
///                       The full list of enumeration values can be found in
///                       SelectionRule.h .
/// \tparam OpType        The name of the matrix operation class. Users could either
///                       use the DenseGenRealShiftSolve wrapper class, or define their
///                       own that impelemnts all the public member functions as in
///                       DenseGenRealShiftSolve.
///
template <typename Scalar = double,
          int SelectionRule = LARGEST_MAGN,
          typename OpType = DenseGenRealShiftSolve<double> >
class GenEigsRealShiftSolver: public GenEigsSolver<Scalar, SelectionRule, OpType>
{
private:
    typedef std::complex<Scalar> Complex;
    typedef Eigen::Array<Complex, Eigen::Dynamic, 1> ComplexArray;

    Scalar sigma;

    // First transform back the ritz values, and then sort
    void sort_ritzpair()
    {
        // The eigenvalus we get from the iteration is nu = 1 / (lambda - sigma)
        // So the eigenvalues of the original problem is lambda = 1 / nu + sigma
        ComplexArray ritz_val_org = Scalar(1.0) / this->ritz_val.head(this->nev).array() + sigma;
        this->ritz_val.head(this->nev) = ritz_val_org;
        GenEigsSolver<Scalar, SelectionRule, OpType>::sort_ritzpair();
    }
public:
    ///
    /// Constructor to create a eigen solver object using the shift-and-invert mode.
    ///
    /// \param op_    Pointer to the matrix operation object. This class should implement
    ///               the shift-solve operation of \f$A\f$: calculating
    ///               \f$(A-\sigma I)^{-1}y\f$ for any vector \f$y\f$. Users could either
    ///               create the object from the DenseGenRealShiftSolve wrapper class, or
    ///               define their own that impelemnts all the public member functions
    ///               as in DenseGenRealShiftSolve.
    /// \param nev_   Number of eigenvalues requested. This should satisfy \f$1\le nev \le n-2\f$,
    ///               where \f$n\f$ is the size of matrix.
    /// \param ncv_   Parameter that controls the convergence speed of the algorithm.
    ///               Typically a larger `ncv_` means faster convergence, but it may
    ///               also result in greater memory use and more matrix operations
    ///               in each iteration. This parameter must satisfy \f$nev+2 \le ncv \le n\f$,
    ///               and is advised to take \f$ncv \ge 2\cdot nev + 1\f$.
    /// \param sigma_ The real-valued shift.
    ///
    GenEigsRealShiftSolver(OpType *op_, int nev_, int ncv_, Scalar sigma_) :
        GenEigsSolver<Scalar, SelectionRule, OpType>(op_, nev_, ncv_),
        sigma(sigma_)
    {
        this->op->set_shift(sigma);
    }
};





///
/// \ingroup EigenSolver
///
/// This class implements the eigen solver for general real matrices with
/// a complex shift value in the **shift-and-invert mode**. The background
/// knowledge of the shift-and-invert mode can be found in the documentation
/// of the SymEigsShiftSolver class.
///
/// \tparam Scalar        The element type of the matrix.
///                       Currently supported types are `float`, `double` and `long double`.
/// \tparam SelectionRule An enumeration value indicating the selection rule of
///                       the shifted-and-inverted eigenvalues.
///                       The full list of enumeration values can be found in
///                       SelectionRule.h .
/// \tparam OpType        The name of the matrix operation class. Users could either
///                       use the DenseGenComplexShiftSolve wrapper class, or define their
///                       own that impelemnts all the public member functions as in
///                       DenseGenComplexShiftSolve.
///
template <typename Scalar = double,
          int SelectionRule = LARGEST_MAGN,
          typename OpType = DenseGenComplexShiftSolve<double> >
class GenEigsComplexShiftSolver: public GenEigsSolver<Scalar, SelectionRule, OpType>
{
private:
    typedef Eigen::Array<Scalar, Eigen::Dynamic, 1> Array;
    typedef std::complex<Scalar> Complex;
    typedef Eigen::Array<Complex, Eigen::Dynamic, 1> ComplexArray;

    Scalar sigmar;
    Scalar sigmai;

    // First transform back the ritz values, and then sort
    void sort_ritzpair()
    {
        // The eigenvalus we get from the iteration is
        //     nu = 0.5 * (1 / (lambda - sigma)) + 1 / (lambda - conj(sigma)))
        // So the eigenvalues of the original problem is
        //                       1 \pm sqrt(1 - 4 * nu^2 * sigmai^2)
        //     lambda = sigmar + -----------------------------------
        //                                     2 * nu
        // We need to rule out the wrong one
        // Let v1 be the first eigenvector, then A * v1 = lambda1 * v1
        // and inv(A - r * I) * v1 = 1 / (lambda1 - r) * v1
        // where r is any real value.
        // We can use this identity to test which lambda to choose
        ComplexArray nu = this->ritz_val.head(this->nev).array();
        ComplexArray tmp1 = Scalar(0.5) / nu + sigmar;
        ComplexArray tmp2 = (Scalar(1) / nu / nu - 4 * sigmai * sigmai).sqrt() * Scalar(0.5);

        ComplexArray root1 = tmp1 + tmp2;
        ComplexArray root2 = tmp1 - tmp2;

        ComplexArray v = this->fac_V * this->ritz_vec.col(0);
        Array v_real = v.real();
        Array v_imag = v.imag();
        Array lhs_real(this->dim_n), lhs_imag(this->dim_n);

        this->op->set_shift(sigmar, 0);
        this->op->perform_op(v_real.data(), lhs_real.data());
        this->op->perform_op(v_imag.data(), lhs_imag.data());

        ComplexArray rhs1 = v / (root1[0] - Complex(sigmar, 0));
        ComplexArray rhs2 = v / (root2[0] - Complex(sigmar, 0));

        Scalar err1 = (rhs1.real() - lhs_real).abs().sum() + (rhs1.imag() - lhs_imag).abs().sum();
        Scalar err2 = (rhs2.real() - lhs_real).abs().sum() + (rhs2.imag() - lhs_imag).abs().sum();

        if(err1 < err2)
        {
            this->ritz_val.head(this->nev) = root1;
        } else {
            this->ritz_val.head(this->nev) = root2;
        }
        GenEigsSolver<Scalar, SelectionRule, OpType>::sort_ritzpair();
    }
public:
    ///
    /// Constructor to create a eigen solver object using the shift-and-invert mode.
    ///
    /// \param op_     Pointer to the matrix operation object. This class should implement
    ///                the complex shift-solve operation of \f$A\f$: calculating
    ///                \f$\mathrm{Re}\{(A-\sigma I)^{-1}y\}\f$ for any vector \f$y\f$. Users could either
    ///                create the object from the DenseGenComplexShiftSolve wrapper class, or
    ///                define their own that impelemnts all the public member functions
    ///                as in DenseGenComplexShiftSolve.
    /// \param nev_    Number of eigenvalues requested. This should satisfy \f$1\le nev \le n-2\f$,
    ///                where \f$n\f$ is the size of matrix.
    /// \param ncv_    Parameter that controls the convergence speed of the algorithm.
    ///                Typically a larger `ncv_` means faster convergence, but it may
    ///                also result in greater memory use and more matrix operations
    ///                in each iteration. This parameter must satisfy \f$nev+2 \le ncv \le n\f$,
    ///                and is advised to take \f$ncv \ge 2\cdot nev + 1\f$.
    /// \param sigmar_ The real part of the shift.
    /// \param sigmai_ The imaginary part of the shift.
    ///
    GenEigsComplexShiftSolver(OpType *op_, int nev_, int ncv_, Scalar sigmar_, Scalar sigmai_) :
        GenEigsSolver<Scalar, SelectionRule, OpType>(op_, nev_, ncv_),
        sigmar(sigmar_), sigmai(sigmai_)
    {
        this->op->set_shift(sigmar, sigmai);
    }
};

#endif // GEN_EIGS_SOLVER_H
