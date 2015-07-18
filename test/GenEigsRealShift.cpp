#include <Eigen/Core>
#include <iostream>

#include <GenEigsSolver.h>
#include <MatOp/DenseGenRealShiftSolve.h>

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

typedef Eigen::MatrixXd Matrix;
typedef Eigen::VectorXd Vector;
typedef Eigen::MatrixXcd ComplexMatrix;
typedef Eigen::VectorXcd ComplexVector;

template <int SelectionRule>
void run_test(Matrix &mat, int k, int m, double sigma)
{
    DenseGenRealShiftSolve<double> op(mat);
    GenEigsRealShiftSolver<double, SelectionRule, DenseGenRealShiftSolve<double>> eigs(&op, k, m, sigma);
    eigs.init();
    int nconv = eigs.compute();
    int niter = eigs.num_iterations();
    int nops = eigs.num_operations();

    REQUIRE( nconv > 0 );

    ComplexVector evals = eigs.eigenvalues();
    ComplexMatrix evecs = eigs.eigenvectors();

    ComplexMatrix err = mat * evecs - evecs * evals.asDiagonal();

    INFO( "nconv = " << nconv );
    INFO( "niter = " << niter );
    INFO( "nops = " << nops );
    INFO( "||AU - UD||_inf = " << err.array().abs().maxCoeff() );
    REQUIRE( err.array().abs().maxCoeff() == Approx(0.0) );
}

TEST_CASE("Eigensolver of general real matrix", "[eigs_gen]")
{
    srand(123);
    Matrix A = Eigen::MatrixXd::Random(10, 10);

    int k = 3;
    int m = 6;
    double sigma = 0.0;

    SECTION( "Largest Magnitude" )
    {
        run_test<LARGEST_MAGN>(A, k, m, sigma);
    }
    SECTION( "Largest Real Part" )
    {
        run_test<LARGEST_REAL>(A, k, m, sigma);
    }
    SECTION( "Largest Imaginary Part" )
    {
        run_test<LARGEST_IMAG>(A, k, m, sigma);
    }
    SECTION( "Smallest Magnitude" )
    {
        run_test<SMALLEST_MAGN>(A, k, m, sigma);
    }
    SECTION( "Smallest Real Part" )
    {
        run_test<SMALLEST_REAL>(A, k, m, sigma);
    }
    SECTION( "Smallest Imaginary Part" )
    {
        run_test<SMALLEST_IMAG>(A, k, m, sigma);
    }
}
