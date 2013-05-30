/*

Copyright (c) 2005-2013, University of Oxford.
All rights reserved.

University of Oxford means the Chancellor, Masters and Scholars of the
University of Oxford, having an administrative office at Wellington
Square, Oxford OX1 2JD, UK.

This file is part of Chaste.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
 * Neither the name of the University of Oxford nor the names of its
   contributors may be used to endorse or promote products derived from this
   software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef ABSTRACTCARDIACMECHANICSSOLVER_HPP_
#define ABSTRACTCARDIACMECHANICSSOLVER_HPP_

#include <map>
#include "IncompressibleNonlinearElasticitySolver.hpp"
#include "CompressibleNonlinearElasticitySolver.hpp"
#include "QuadraticBasisFunction.hpp"
#include "LinearBasisFunction.hpp"
#include "AbstractContractionModel.hpp"
#include "FibreReader.hpp"
#include "FineCoarseMeshPair.hpp"
#include "AbstractCardiacMechanicsSolverInterface.hpp"
#include "HeartRegionCodes.hpp"
#include "ElectroMechanicsProblemDefinition.hpp"


/**
 *  This struct is used to collect the three things that are stored for
 *  each physical quadrature point: a contraction model, the stretch at that
 *  point, and the stretch at the last time-step (used to compute
 *  stretch rate).
 */
typedef struct DataAtQuadraturePoint_
{
    AbstractContractionModel* ContractionModel; /**< Pointer to contraction model at this quadrature point */
    bool Active;/**<whether this quad point is active or not*/
    double Stretch; /**< Stretch (in fibre direction) at this quadrature point */
    double StretchLastTimeStep; /**< Stretch (in fibre direction) at the previous timestep, at this quadrature point */

} DataAtQuadraturePoint;


/**
 *  AbstractCardiacMechanicsSolver
 *
 *  Base class to implicit and explicit cardiac mechanics solvers. Inherits from IncompressibleNonlinearElasticitySolver
 *  or CompressibleNonlinearElasticityAssembler (depending on what the template parameter ELASTICITY_SOLVER
 *  is), and also from AbstractCardiacMechanicsSolverInterface which just declares this classes
 *  main public methods.
 *
 *  Overloads AddActiveStressAndStressDerivative() which adds on the active tension term to the stress. The
 *  child classes hold the contraction models and need to implement a method for getting the active tension from
 *  the model.
 */
template<class ELASTICITY_SOLVER, unsigned DIM>
class AbstractCardiacMechanicsSolver : public ELASTICITY_SOLVER, public AbstractCardiacMechanicsSolverInterface<DIM>
{
protected:
    static const unsigned NUM_VERTICES_PER_ELEMENT = ELASTICITY_SOLVER::NUM_VERTICES_PER_ELEMENT; /**< Useful const from base class */


    /**
     *  A map from the index of a quadrature point to the data (contraction
     *  model, stretch, stretch at the last time-step) at that quad point.
     *  Note that there is no vector of all the quadrature points of the mesh;
     *  the quad point index is the index that would be obtained by looping over
     *  elements and then looping over quad points.
     *
     *  DISTRIBUTED - only holds data for the quad points within elements
     *  owned by this process.
     */
    std::map<unsigned,DataAtQuadraturePoint> mQuadPointToDataAtQuadPointMap;

    /**
     *  An iterator to the map, used to avoid having to repeatedly search the map.
     */
    std::map<unsigned,DataAtQuadraturePoint>::iterator mMapIterator;

    /** A mesh pair object that can be set by the user to inform the solver about the electrics mesh. */
    FineCoarseMeshPair<DIM>* mpMeshPair;

    /** Total number of quad points in the (mechanics) mesh */
    unsigned mTotalQuadPoints;

    /** Current time */
    double mCurrentTime;
    /** Time to which the solver has been asked to solve to */
    double mNextTime;
    /** Time used to integrate the contraction model */
    double mOdeTimestep;

    /** The fibre-sheet-normal directions (in a matrix), if constant (defaults to the identity, ie fibres in the X-direction, sheet in the XY plane) */
    c_matrix<double,DIM,DIM> mConstantFibreSheetDirections;

    /**
     * The fibre-sheet-normal directions (matrices), one for each element. Only non-NULL if SetVariableFibreSheetDirections()
     * is called, if not mConstantFibreSheetDirections is used instead
     */
    std::vector<c_matrix<double,DIM,DIM> >* mpVariableFibreSheetDirections;

    /**
     *  Whether the fibre-sheet directions that where read in where define per element or per quadrature point.
     *  Only valid if mpVariableFibreSheetDirections!=NULL
     */
    bool mFibreSheetDirectionsDefinedByQuadraturePoint;

    /** The fibre direction for the current element being assembled on */
    c_vector<double,DIM> mCurrentElementFibreDirection;

    /** The sheet direction for the current element being assembled on */
    c_vector<double,DIM> mCurrentElementSheetDirection;

    /** The sheet normal direction for the current element being assembled on */
    c_vector<double,DIM> mCurrentElementSheetNormalDirection;


    /**
     *  This class contains all the information about the electro mechanics problem (except the material law)
     */
    ElectroMechanicsProblemDefinition<DIM>& mrElectroMechanicsProblemDefinition;

    /**
     *  @return whether the solver is implicit or not (i.e. whether the contraction model depends on lambda (and depends on
     *  lambda at the current time)). For whether dTa_dLam dependent terms need to be added to the Jacbobian
     */
    virtual bool IsImplicitSolver()=0;


    /**
     *  Overloaded AddActiveStressAndStressDerivative(), which calls on the contraction model to get
     *  the active stress and add it on to the stress tensor
     *
     *  @param rC The Lagrangian deformation tensor (F^T F)
     *  @param elementIndex Index of the current element
     *  @param currentQuadPointGlobalIndex The index (assuming an outer loop over elements and an inner
     *    loop over quadrature points), of the current quadrature point.
     *  @param rT The stress to be added to
     *  @param rDTdE the stress derivative to be added to, assuming
     *    the final parameter is true
     *  @param addToDTdE A boolean flag saying whether the stress derivative is
     *   required or not.
     */
    void AddActiveStressAndStressDerivative(c_matrix<double,DIM,DIM>& rC,
                                            unsigned elementIndex,
                                            unsigned currentQuadPointGlobalIndex,
                                            c_matrix<double,DIM,DIM>& rT,
                                            FourthOrderTensor<DIM,DIM,DIM,DIM>& rDTdE,
                                            bool addToDTdE);


    /**
     *  Over-ridden method which sets up an internal variable in the parent class, using
     *  the provided fibre-sheet direction information.
     *
     *  @param elementIndex element global index
     *  @param currentQuadPointGlobalIndex quad point global index
     */
    void SetupChangeOfBasisMatrix(unsigned elementIndex, unsigned currentQuadPointGlobalIndex);

    /**
     * Sets relevant data at all quad points, including whether it is an active region or not.
     * The contraction model is set to NULL.
     * At the end, it calls InitialiseContractionModels in the child class to assign a proper model.
     */
    void Initialise();

    /**
     * Must assign a contraction model at each quad point.
     * It has to assign fake-bath models to non-active regions.
     */
    virtual void InitialiseContractionModels() = 0;


    /**
     *  Pure method called in AbstractCardiacMechanicsSolver::AddActiveStressAndStressDerivative(), which needs to provide
     *  the active tension (and other info if implicit (if the contraction model depends on stretch
     *  or stretch rate)) at a particular quadrature point. Takes in the current fibre stretch.
     *
     *  @param currentFibreStretch The stretch in the fibre direction
     *  @param currentQuadPointGlobalIndex quadrature point the integrand is currently being evaluated at in AssembleOnElement
     *  @param assembleJacobian  A bool stating whether to assemble the Jacobian matrix.
     *  @param rActiveTension The returned active tension
     *  @param rDerivActiveTensionWrtLambda The returned dT_dLam, derivative of active tension wrt stretch. Only should be set in implicit solvers
     *  @param rDerivActiveTensionWrtDLambdaDt The returned dT_dLamDot, derivative of active tension wrt stretch rate.  Only should be set in implicit solver
     */
    virtual void GetActiveTensionAndTensionDerivs(double currentFibreStretch,
                                                  unsigned currentQuadPointGlobalIndex,
                                                  bool assembleJacobian,
                                                  double& rActiveTension,
                                                  double& rDerivActiveTensionWrtLambda,
                                                  double& rDerivActiveTensionWrtDLambdaDt)=0;
public:
    /**
     * Constructor
     *
     * @param rQuadMesh A reference to the mesh.
     * @param contractionModelName the name of the contraction model to be used
     * @param rProblemDefinition Object defining body force and boundary conditions
     * @param outputDirectory The output directory, relative to TEST_OUTPUT
     */
    AbstractCardiacMechanicsSolver(QuadraticMesh<DIM>& rQuadMesh,
                                   ElectroMechanicsProblemDefinition<DIM>& rProblemDefinition,
                                   std::string outputDirectory);

    /**
     *  Destructor
     */
    virtual ~AbstractCardiacMechanicsSolver();

    /**
     * Sets the fine-coarse mesh pair object so that the solver knows about electrics too.
     * It checks that the coarse mesh of the fine-mesh pair has the same number of elements as
     * the quad mesh of this object and throws an exception otherwise.
     *
     * @param pMeshPair the FineCoarseMeshPair object to be set
     */
    void SetFineCoarseMeshPair(FineCoarseMeshPair<DIM>* pMeshPair);

    /** @return the total number of quad points in the mesh. Pure, implemented in concrete solver */
    unsigned GetTotalNumQuadPoints()
    {
        return mTotalQuadPoints;
    }

    /** @return the quadrature rule used in the elements. */
    virtual GaussianQuadratureRule<DIM>* GetQuadratureRule()
    {
        return this->mpQuadratureRule;
    }

    /**
     * @return access mQuadPointToDataAtQuadPointMap. See doxygen for this variable
     */
    std::map<unsigned,DataAtQuadraturePoint>& rGetQuadPointToDataAtQuadPointMap()
    {
        return mQuadPointToDataAtQuadPointMap;
    }


    /**
     *  Set a constant fibre-sheet-normal direction (a matrix) to something other than the default (fibres in X-direction,
     *  sheet in the XY plane)
     *  @param rFibreSheetMatrix The fibre-sheet-normal matrix (fibre dir the first column, normal-to-fibre-in sheet in second
     *  column, sheet-normal in third column).
     */
    void SetConstantFibreSheetDirections(const c_matrix<double,DIM,DIM>& rFibreSheetMatrix);

    /**
     *  Set a variable fibre-sheet-normal direction (matrices), from file.
     *  If the second parameter is false, there should be one fibre-sheet definition for each element; otherwise
     *  there should be one fibre-sheet definition for each *quadrature point* in the mesh.
     *  In the first case, the file should be a .ortho file (ie each line has the fibre dir, sheet dir, normal dir
     *  for that element), in the second it should have .orthoquad as the format.
     *
     *  @param rOrthoFile the file containing the fibre/sheet directions
     *  @param definedPerQuadraturePoint whether the fibre-sheet definitions are for each quadrature point in the mesh
     *   (if not, one for each element is assumed).
     */
    void SetVariableFibreSheetDirections(const FileFinder& rOrthoFile, bool definedPerQuadraturePoint);

    /**
     *  Set the intracellular Calcium concentrations and voltages at each quad point. Pure.
     *
     *  Implicit solvers (for contraction models which are functions of stretch (and maybe
     *  stretch rate) would integrate the contraction model with this Ca/V/t using the current
     *  stretch (ie inside AssembleOnElement, ie inside GetActiveTensionAndTensionDerivs).
     *  Explicit solvers (for contraction models which are NOT functions of stretch can immediately
     *  integrate the contraction models to get the active tension.
     *
     *  @param rCalciumConcentrations Reference to a vector of intracellular calcium concentrations at each quadrature point
     *  @param rVoltages Reference to a vector of voltages at each quadrature point
     */
    void SetCalciumAndVoltage(std::vector<double>& rCalciumConcentrations,
                              std::vector<double>& rVoltages);

    /**
     *  Solve for the deformation, integrating the contraction model ODEs.
     *
     *  @param time the current time
     *  @param nextTime the next time
     *  @param odeTimestep the ODE timestep
     */
    virtual void Solve(double time, double nextTime, double odeTimestep)=0;



    /**
     *  Compute the deformation gradient, and stretch in the fibre direction, for each element in the mesh.
     *  Note: using quadratic interpolation for position, the deformation gradients and stretches
     *  actually vary linearly in each element. However, for computational efficiency reasons, when computing
     *  deformation gradients and stretches to pass back to the electrophysiology solver, we just assume
     *  they are constant in each element (ie ignoring the quadratic correction to the displacement). This means
     *  that  the (const) deformation gradient and stretch for each element can be computed in advance and
     *  stored, and we don't have to worry about interpolation onto the precise location of the cell-model (electrics-mesh)
     *  node, just which element it is in, and ditto the electric mesh element centroid.
     *
     *  To compute this (elementwise-)constant F (and from it the constant stretch), we just have to compute
     *  F using the deformed positions at the vertices only, with linear bases, rather than all the
     *  nodes and quadratic bases.
     *
     *  @param rDeformationGradients A reference of a std::vector in which the deformation gradient
     *  in each element will be returned. Must be allocated prior to being passed in.
     *  @param rStretches A reference of a std::vector in which the stretch in each element will be returned.
     *  Must be allocated prior to being passed in.
     */
    void ComputeDeformationGradientAndStretchInEachElement(std::vector<c_matrix<double,DIM,DIM> >& rDeformationGradients,
                                                           std::vector<double>& rStretches);
};


template<class ELASTICITY_SOLVER,unsigned DIM>
AbstractCardiacMechanicsSolver<ELASTICITY_SOLVER,DIM>::AbstractCardiacMechanicsSolver(QuadraticMesh<DIM>& rQuadMesh,
                                                                                      ElectroMechanicsProblemDefinition<DIM>& rProblemDefinition,
                                                                                      std::string outputDirectory)
   : ELASTICITY_SOLVER(rQuadMesh,
                       rProblemDefinition,
                       outputDirectory),
     mpMeshPair(NULL),
     mCurrentTime(DBL_MAX),
     mNextTime(DBL_MAX),
     mOdeTimestep(DBL_MAX),
     mrElectroMechanicsProblemDefinition(rProblemDefinition)
{
}

template<class ELASTICITY_SOLVER,unsigned DIM>
void AbstractCardiacMechanicsSolver<ELASTICITY_SOLVER,DIM>::Initialise()
{
    // compute total num quad points
    unsigned num_quad_pts_per_element = this->mpQuadratureRule->GetNumQuadPoints();
    mTotalQuadPoints = this->mrQuadMesh.GetNumElements()*num_quad_pts_per_element;

    std::vector<ElementAndWeights<DIM> > fine_elements = mpMeshPair->rGetElementsAndWeights();
    assert(fine_elements.size()==mTotalQuadPoints);
    assert(mpMeshPair!=NULL);

    for (typename AbstractTetrahedralMesh<DIM, DIM>::ElementIterator iter = this->mrQuadMesh.GetElementIteratorBegin();
         iter != this->mrQuadMesh.GetElementIteratorEnd();
         ++iter)
    {
        Element<DIM, DIM>& element = *iter;

        if (element.GetOwnership() == true)
        {
            for(unsigned j=0; j<num_quad_pts_per_element; j++)
            {
                unsigned quad_pt_global_index = element.GetIndex()*num_quad_pts_per_element + j;

                DataAtQuadraturePoint data_at_quad_point;
                data_at_quad_point.ContractionModel = NULL;
                data_at_quad_point.Stretch = 1.0;
                data_at_quad_point.StretchLastTimeStep = 1.0;

                if (mpMeshPair->GetFineMesh().GetElement(fine_elements[quad_pt_global_index].ElementNum)->GetUnsignedAttribute() == HeartRegionCode::GetValidBathId())
                {
                    data_at_quad_point.Active = false;//bath
                }
                else
                {
                    data_at_quad_point.Active = true;//tissue
                }
                mQuadPointToDataAtQuadPointMap[quad_pt_global_index] = data_at_quad_point;
            }
        }
    }

    // initialise the iterator to point at the beginning
    mMapIterator = mQuadPointToDataAtQuadPointMap.begin();

    // initialise fibre/sheet direction matrix to be the identity, fibres in X-direction, and sheet in XY-plane
    mConstantFibreSheetDirections = zero_matrix<double>(DIM,DIM);
    for(unsigned i=0; i<DIM; i++)
    {
        mConstantFibreSheetDirections(i,i) = 1.0;
    }

    mpVariableFibreSheetDirections = NULL;

    InitialiseContractionModels();
}

template<class ELASTICITY_SOLVER,unsigned DIM>
void AbstractCardiacMechanicsSolver<ELASTICITY_SOLVER,DIM>::SetFineCoarseMeshPair(FineCoarseMeshPair<DIM>* pMeshPair)
{
    assert(pMeshPair!=NULL);
    if (pMeshPair->GetCoarseMesh().GetNumElements() != this->mrQuadMesh.GetNumElements())
    {
        EXCEPTION("When setting a mesh pair into the solver, the coarse mesh of the mesh pair must be the same as the quadratic mesh");
    }
    mpMeshPair = pMeshPair;
}

template<class ELASTICITY_SOLVER,unsigned DIM>
AbstractCardiacMechanicsSolver<ELASTICITY_SOLVER,DIM>::~AbstractCardiacMechanicsSolver()
{
    for(std::map<unsigned,DataAtQuadraturePoint>::iterator iter = mQuadPointToDataAtQuadPointMap.begin();
        iter != mQuadPointToDataAtQuadPointMap.end();
        iter++)
    {
        delete iter->second.ContractionModel;
    }

    if(mpVariableFibreSheetDirections)
    {
        delete mpVariableFibreSheetDirections;
    }
}



template<class ELASTICITY_SOLVER,unsigned DIM>
void AbstractCardiacMechanicsSolver<ELASTICITY_SOLVER,DIM>::SetCalciumAndVoltage(std::vector<double>& rCalciumConcentrations,
                                                                                 std::vector<double>& rVoltages)

{
    assert(rCalciumConcentrations.size() == mTotalQuadPoints);
    assert(rVoltages.size() == mTotalQuadPoints);

    ContractionModelInputParameters input_parameters;

    for(unsigned i=0; i<rCalciumConcentrations.size(); i++)
    {
        input_parameters.intracellularCalciumConcentration = rCalciumConcentrations[i];
        input_parameters.voltage = rVoltages[i];

///\todo #1828 / #1211 don't pass in entire vector
        std::map<unsigned,DataAtQuadraturePoint>::iterator iter = mQuadPointToDataAtQuadPointMap.find(i);
        if(iter != mQuadPointToDataAtQuadPointMap.end())
        {
            iter->second.ContractionModel->SetInputParameters(input_parameters);
        }
    }
}


template<class ELASTICITY_SOLVER,unsigned DIM>
void AbstractCardiacMechanicsSolver<ELASTICITY_SOLVER,DIM>::SetupChangeOfBasisMatrix(unsigned elementIndex,
                                                                                     unsigned currentQuadPointGlobalIndex)
{
    if(!mpVariableFibreSheetDirections) // constant fibre directions
    {
        this->mChangeOfBasisMatrix = mConstantFibreSheetDirections;
    }
    else if(!mFibreSheetDirectionsDefinedByQuadraturePoint) // fibre directions defined for each mechanics mesh element
    {
        this->mChangeOfBasisMatrix = (*mpVariableFibreSheetDirections)[elementIndex];
    }
    else // fibre directions defined for each mechanics mesh quadrature point
    {
        this->mChangeOfBasisMatrix = (*mpVariableFibreSheetDirections)[currentQuadPointGlobalIndex];
    }
}



template<class ELASTICITY_SOLVER,unsigned DIM>
void AbstractCardiacMechanicsSolver<ELASTICITY_SOLVER,DIM>::AddActiveStressAndStressDerivative(c_matrix<double,DIM,DIM>& rC,
                                                                                               unsigned elementIndex,
                                                                                               unsigned currentQuadPointGlobalIndex,
                                                                                               c_matrix<double,DIM,DIM>& rT,
                                                                                               FourthOrderTensor<DIM,DIM,DIM,DIM>& rDTdE,
                                                                                               bool addToDTdE)
{
    for(unsigned i=0; i<DIM; i++)
    {
        mCurrentElementFibreDirection(i) = this->mChangeOfBasisMatrix(i,0);
    }

    //Compute the active tension and add to the stress and stress-derivative
    double I4_fibre = inner_prod(mCurrentElementFibreDirection, prod(rC, mCurrentElementFibreDirection));
    double lambda_fibre = sqrt(I4_fibre);

    double active_tension = 0;
    double d_act_tension_dlam = 0.0;     // Set and used if assembleJacobian==true
    double d_act_tension_d_dlamdt = 0.0; // Set and used if assembleJacobian==true

    GetActiveTensionAndTensionDerivs(lambda_fibre, currentQuadPointGlobalIndex, addToDTdE,
                                     active_tension, d_act_tension_dlam, d_act_tension_d_dlamdt);


    double detF = sqrt(Determinant(rC));
    rT += (active_tension*detF/I4_fibre)*outer_prod(mCurrentElementFibreDirection,mCurrentElementFibreDirection);

    // amend the stress and dTdE using the active tension
    double dTdE_coeff1 = -2*active_tension*detF/(I4_fibre*I4_fibre); // note: I4_fibre*I4_fibre = lam^4
    double dTdE_coeff2 = active_tension*detF/I4_fibre;
    double dTdE_coeff_s1 = 0.0; // only set non-zero if we apply cross fibre tension (in 2/3D)
    double dTdE_coeff_s2 = 0.0; // only set non-zero if we apply cross fibre tension (in 2/3D)
    double dTdE_coeff_s3 = 0.0; // only set non-zero if we apply cross fibre tension and implicit (in 2/3D)
    double dTdE_coeff_n1 = 0.0; // only set non-zero if we apply cross fibre tension in 3D
    double dTdE_coeff_n2 = 0.0; // only set non-zero if we apply cross fibre tension in 3D
    double dTdE_coeff_n3 = 0.0; // only set non-zero if we apply cross fibre tension in 3D and implicit

    if(IsImplicitSolver())
    {
        double dt = mNextTime-mCurrentTime;
        //std::cout << "d sigma / d lamda = " << d_act_tension_dlam << ", d sigma / d lamdat = " << d_act_tension_d_dlamdt << "\n" << std::flush;
        dTdE_coeff1 += (d_act_tension_dlam + d_act_tension_d_dlamdt/dt)*detF/(lambda_fibre*I4_fibre); // note: I4_fibre*lam = lam^3
    }

    bool apply_cross_fibre_tension = (this->mrElectroMechanicsProblemDefinition.GetApplyCrossFibreTension()) && (DIM > 1);
    if(apply_cross_fibre_tension)
    {
        double sheet_cross_fraction = mrElectroMechanicsProblemDefinition.GetSheetTensionFraction();

        for(unsigned i=0; i<DIM; i++)
        {
            mCurrentElementSheetDirection(i) = this->mChangeOfBasisMatrix(i,1);
        }

        double I4_sheet = inner_prod(mCurrentElementSheetDirection, prod(rC, mCurrentElementSheetDirection));

        // amend the stress and dTdE using the active tension
        dTdE_coeff_s1 = -2*sheet_cross_fraction*detF*active_tension/(I4_sheet*I4_sheet); // note: I4*I4 = lam^4

        if(IsImplicitSolver())
        {
            double dt = mNextTime-mCurrentTime;
            dTdE_coeff_s3 = sheet_cross_fraction*(d_act_tension_dlam + d_act_tension_d_dlamdt/dt)*detF/(lambda_fibre*I4_sheet); // note: I4*lam = lam^3
        }

        rT += sheet_cross_fraction*(active_tension*detF/I4_sheet)*outer_prod(mCurrentElementSheetDirection,mCurrentElementSheetDirection);

        dTdE_coeff_s2 = active_tension*sheet_cross_fraction*detF/I4_sheet;

        if (DIM>2)
        {
            double sheet_normal_cross_fraction = mrElectroMechanicsProblemDefinition.GetSheetNormalTensionFraction();
            for(unsigned i=0; i<DIM; i++)
            {
                mCurrentElementSheetNormalDirection(i) = this->mChangeOfBasisMatrix(i,2);
            }

            double I4_sheet_normal = inner_prod(mCurrentElementSheetNormalDirection, prod(rC, mCurrentElementSheetNormalDirection));

            dTdE_coeff_n1 =-2*sheet_normal_cross_fraction*detF*active_tension/(I4_sheet_normal*I4_sheet_normal); // note: I4*I4 = lam^4

            rT += sheet_normal_cross_fraction*(active_tension*detF/I4_sheet_normal)*outer_prod(mCurrentElementSheetNormalDirection,mCurrentElementSheetNormalDirection);

            dTdE_coeff_n2 = active_tension*sheet_normal_cross_fraction*detF/I4_sheet_normal;
            if(IsImplicitSolver())
            {
                double dt = mNextTime-mCurrentTime;
                dTdE_coeff_n3 = sheet_normal_cross_fraction*(d_act_tension_dlam + d_act_tension_d_dlamdt/dt)*detF/(lambda_fibre*I4_sheet_normal); // note: I4*lam = lam^3
            }
        }
    }


    if(addToDTdE)
    {
        c_matrix<double,DIM,DIM> invC = Inverse(rC);

        for (unsigned M=0; M<DIM; M++)
        {
            for (unsigned N=0; N<DIM; N++)
            {
                for (unsigned P=0; P<DIM; P++)
                {
                    for (unsigned Q=0; Q<DIM; Q++)
                    {
                        rDTdE(M,N,P,Q) +=   dTdE_coeff1 * mCurrentElementFibreDirection(M)
                                                        * mCurrentElementFibreDirection(N)
                                                        * mCurrentElementFibreDirection(P)
                                                        * mCurrentElementFibreDirection(Q)

                                         +  dTdE_coeff2 * mCurrentElementFibreDirection(M)
                                                        * mCurrentElementFibreDirection(N)
                                                        * invC(P,Q);
                        if(apply_cross_fibre_tension)
                        {
                            rDTdE(M,N,P,Q) += dTdE_coeff_s1 * mCurrentElementSheetDirection(M)
                                                            * mCurrentElementSheetDirection(N)
                                                            * mCurrentElementSheetDirection(P)
                                                            * mCurrentElementSheetDirection(Q)

                                           +  dTdE_coeff_s2 * mCurrentElementSheetDirection(M)
                                                            * mCurrentElementSheetDirection(N)
                                                            * invC(P,Q)

                                           + dTdE_coeff_s3 * mCurrentElementSheetDirection(M)
                                                           * mCurrentElementSheetDirection(N)
                                                           * mCurrentElementFibreDirection(P)
                                                           * mCurrentElementFibreDirection(Q);
                            if (DIM>2)
                            {
                                rDTdE(M,N,P,Q) += dTdE_coeff_n1 * mCurrentElementSheetNormalDirection(M)
                                                                * mCurrentElementSheetNormalDirection(N)
                                                                * mCurrentElementSheetNormalDirection(P)
                                                                * mCurrentElementSheetNormalDirection(Q)

                                                + dTdE_coeff_n2 * mCurrentElementSheetNormalDirection(M)
                                                                * mCurrentElementSheetNormalDirection(N)
                                                                * invC(P,Q)

                                                + dTdE_coeff_n3 * mCurrentElementSheetNormalDirection(M)
                                                                * mCurrentElementSheetNormalDirection(N)
                                                                * mCurrentElementFibreDirection(P)
                                                                * mCurrentElementFibreDirection(Q);
                            }
                        }
                    }
                }
            }
        }
    }

//    ///\todo #2180 The code below applies a cross fibre tension in the 2D case. Things that need doing:
//    // * Refactor the common code between the block below and the block above to avoid duplication.
//    // * Handle the 3D case.
//    if(this->mrElectroMechanicsProblemDefinition.GetApplyCrossFibreTension() && DIM > 1)
//    {
//        double sheet_cross_fraction = mrElectroMechanicsProblemDefinition.GetSheetTensionFraction();
//
//        for(unsigned i=0; i<DIM; i++)
//        {
//            mCurrentElementSheetDirection(i) = this->mChangeOfBasisMatrix(i,1);
//        }
//
//        double I4_sheet = inner_prod(mCurrentElementSheetDirection, prod(rC, mCurrentElementSheetDirection));
//
//        // amend the stress and dTdE using the active tension
//        double dTdE_coeff_s1 = -2*sheet_cross_fraction*detF*active_tension/(I4_sheet*I4_sheet); // note: I4*I4 = lam^4
//
//        ///\todo #2180 The code below is specific to the implicit cardiac mechanics solver. Currently
//        // the cross-fibre code is only tested using the explicit solver so the code below fails coverage.
//        // This will need to be added back in once an implicit test is in place.
//        double lambda_sheet = sqrt(I4_sheet);
//        if(IsImplicitSolver())
//        {
//           double dt = mNextTime-mCurrentTime;
//           dTdE_coeff_s1 += (d_act_tension_dlam + d_act_tension_d_dlamdt/dt)/(lambda_sheet*I4_sheet); // note: I4*lam = lam^3
//        }
//
//        rT += sheet_cross_fraction*(active_tension*detF/I4_sheet)*outer_prod(mCurrentElementSheetDirection,mCurrentElementSheetDirection);
//
//        double dTdE_coeff_s2 = active_tension*detF/I4_sheet;
//        if(addToDTdE)
//        {
//           for (unsigned M=0; M<DIM; M++)
//           {
//               for (unsigned N=0; N<DIM; N++)
//               {
//                   for (unsigned P=0; P<DIM; P++)
//                   {
//                       for (unsigned Q=0; Q<DIM; Q++)
//                       {
//                           rDTdE(M,N,P,Q) +=  dTdE_coeff_s1 * mCurrentElementSheetDirection(M)
//                                                            * mCurrentElementSheetDirection(N)
//                                                            * mCurrentElementSheetDirection(P)
//                                                            * mCurrentElementSheetDirection(Q)
//
//                                           +  dTdE_coeff_s2 * mCurrentElementFibreDirection(M)
//                                                            * mCurrentElementFibreDirection(N)
//                                                            * invC(P,Q);
//
//                       }
//                   }
//               }
//           }
//        }
//    }
}




template<class ELASTICITY_SOLVER,unsigned DIM>
void AbstractCardiacMechanicsSolver<ELASTICITY_SOLVER,DIM>::ComputeDeformationGradientAndStretchInEachElement(
    std::vector<c_matrix<double,DIM,DIM> >& rDeformationGradients,
    std::vector<double>& rStretches)
{
    assert(rStretches.size()==this->mrQuadMesh.GetNumElements());

    // this will only work currently if the coarse mesh fibre info is defined per element, not per quad point
    assert(!mpVariableFibreSheetDirections || !mFibreSheetDirectionsDefinedByQuadraturePoint);

    static c_matrix<double,DIM,NUM_VERTICES_PER_ELEMENT> element_current_displacements;
    static c_matrix<double,DIM,NUM_VERTICES_PER_ELEMENT> grad_lin_phi;
    static c_matrix<double,DIM,DIM> F;      // the deformation gradient, F = dx/dX, F_{iM} = dx_i/dX_M

    static c_matrix<double,DIM,DIM> jacobian;
    static c_matrix<double,DIM,DIM> inverse_jacobian;
    double jacobian_determinant;
    ChastePoint<DIM> quadrature_point; // not needed, but has to be passed in

    // loop over all the elements
    for(unsigned elem_index=0; elem_index<this->mrQuadMesh.GetNumElements(); elem_index++)
    {
        Element<DIM,DIM>* p_elem = this->mrQuadMesh.GetElement(elem_index);

        // get the fibre direction for this element
        SetupChangeOfBasisMatrix(elem_index, 0); // 0 is quad index, and doesn't matter as checked that fibres not defined by quad pt above.
        for(unsigned i=0; i<DIM; i++)
        {
            mCurrentElementFibreDirection(i) = this->mChangeOfBasisMatrix(i,0);
        }

        // get the displacement at this element
        for (unsigned II=0; II<NUM_VERTICES_PER_ELEMENT; II++)
        {
            for (unsigned JJ=0; JJ<DIM; JJ++)
            {
                // mProblemDimension = DIM for compressible elasticity and DIM+1 for incompressible elasticity
                element_current_displacements(JJ,II) = this->mCurrentSolution[this->mProblemDimension*p_elem->GetNodeGlobalIndex(II) + JJ];
            }
        }

        // set up the linear basis functions
        this->mrQuadMesh.GetInverseJacobianForElement(elem_index, jacobian, jacobian_determinant, inverse_jacobian);
        LinearBasisFunction<DIM>::ComputeTransformedBasisFunctionDerivatives(quadrature_point, inverse_jacobian, grad_lin_phi);

        F = identity_matrix<double>(DIM,DIM);

        // loop over the vertices and interpolate F, the deformation gradient
        // (note: could use matrix-mult if this becomes inefficient
        for (unsigned node_index=0; node_index<NUM_VERTICES_PER_ELEMENT; node_index++)
        {
            for (unsigned i=0; i<DIM; i++)
            {
                for (unsigned M=0; M<DIM; M++)
                {
                    F(i,M) += grad_lin_phi(M,node_index)*element_current_displacements(i,node_index);
                }
            }
        }

        rDeformationGradients[elem_index] = F;

        // compute and save the stretch: m^T C m = ||Fm||
        c_vector<double,DIM> deformed_fibre = prod(F, mCurrentElementFibreDirection);
        rStretches[elem_index] = norm_2(deformed_fibre);
    }
}




template<class ELASTICITY_SOLVER,unsigned DIM>
void AbstractCardiacMechanicsSolver<ELASTICITY_SOLVER,DIM>::SetVariableFibreSheetDirections(const FileFinder& rOrthoFile, bool definedPerQuadraturePoint)
{
    mFibreSheetDirectionsDefinedByQuadraturePoint = definedPerQuadraturePoint;

    FibreReader<DIM> reader(rOrthoFile, ORTHO);

    unsigned num_entries = reader.GetNumLinesOfData();

    if (!mFibreSheetDirectionsDefinedByQuadraturePoint && (num_entries!=this->mrQuadMesh.GetNumElements()) )
    {
        EXCEPTION("Number of entries defined at top of file " << rOrthoFile.GetAbsolutePath() <<
                  " does not match number of elements in the mesh, " << "found " <<  num_entries <<
                  ", expected " << this->mrQuadMesh.GetNumElements());
    }

    if (mFibreSheetDirectionsDefinedByQuadraturePoint && (num_entries!=mTotalQuadPoints) )
    {
        EXCEPTION("Number of entries defined at top of file " << rOrthoFile.GetAbsolutePath() <<
                  " does not match number of quadrature points defined, " << "found " <<  num_entries <<
                  ", expected " << mTotalQuadPoints);
    }

    mpVariableFibreSheetDirections = new std::vector<c_matrix<double,DIM,DIM> >(num_entries, zero_matrix<double>(DIM,DIM));
    for(unsigned index=0; index<num_entries; index++)
    {
        reader.GetFibreSheetAndNormalMatrix(index, (*mpVariableFibreSheetDirections)[index] );
    }
}



template<class ELASTICITY_SOLVER,unsigned DIM>
void AbstractCardiacMechanicsSolver<ELASTICITY_SOLVER,DIM>::SetConstantFibreSheetDirections(const c_matrix<double,DIM,DIM>& rFibreSheetMatrix)
{
    mConstantFibreSheetDirections = rFibreSheetMatrix;
    // check orthogonality
    c_matrix<double,DIM,DIM>  temp = prod(trans(rFibreSheetMatrix),rFibreSheetMatrix);
    for(unsigned i=0; i<DIM; i++)
    {
        for(unsigned j=0; j<DIM; j++)
        {
            double val = (i==j ? 1.0 : 0.0);
            if(fabs(temp(i,j)-val)>1e-4)
            {
                EXCEPTION("The given fibre-sheet matrix, " << rFibreSheetMatrix << ", is not orthogonal");
            }
        }
    }
}


#endif /*ABSTRACTCARDIACMECHANICSSOLVER_HPP_*/
