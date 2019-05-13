///////////////////////////////////////////////////////////////////////////////
//
// File CoupledLinearNS.cpp
//
// For more information, please see: http://www.nektar.info
//
// The MIT License
//
// Copyright (c) 2006 Division of Applied Mathematics, Brown University (USA),
// Department of Aeronautics, Imperial College London (UK), and Scientific
// Computing and Imaging Institute, University of Utah (USA).
//
// License for the specific language governing rights and limitations under
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//
// Description: Coupled  Solver for the Linearised Incompressible
// Navier Stokes equations
///////////////////////////////////////////////////////////////////////////////

#include <boost/algorithm/string.hpp>

#include <LibUtilities/TimeIntegration/TimeIntegrationWrapper.h>
#include "CoupledLinearNS_TT.h"
#include "CoupledLinearNS_trafoP.h"
#include <LibUtilities/BasicUtils/Timer.h>
#include <LocalRegions/MatrixKey.h>
#include <MultiRegions/GlobalLinSysDirectStaticCond.h>
#include "../Eigen/Dense"

using namespace std;

namespace Nektar
{

    string CoupledLinearNS_TT::className = SolverUtils::GetEquationSystemFactory().RegisterCreatorFunction("CoupledLinearisedNS_TT", CoupledLinearNS_TT::create);


    CoupledLinearNS_TT::CoupledLinearNS_TT(const LibUtilities::SessionReaderSharedPtr &pSession):
        UnsteadySystem(pSession),
        CoupledLinearNS(pSession),
        m_zeroMode(false)
    {



    }

    void CoupledLinearNS_TT::v_InitObject()
    {
        IncNavierStokes::v_InitObject();

        int  i;
        int  expdim = m_graph->GetMeshDimension();

        // Get Expansion list for orthogonal expansion at p-2
        const SpatialDomains::ExpansionMap &pressure_exp = GenPressureExp(m_graph->GetExpansions("u"));

        m_nConvectiveFields = m_fields.num_elements();
        if(boost::iequals(m_boundaryConditions->GetVariable(m_nConvectiveFields-1), "p"))
        {
            ASSERTL0(false,"Last field is defined as pressure but this is not suitable for this solver, please remove this field as it is implicitly defined");
        }
        // Decide how to declare explist for pressure. 
        if(expdim == 2)
        {
            int nz; 

            if(m_HomogeneousType == eHomogeneous1D)
            {
                ASSERTL0(m_fields.num_elements() > 2,"Expect to have three at least three components of velocity variables");
                LibUtilities::BasisKey Homo1DKey = m_fields[0]->GetHomogeneousBasis()->GetBasisKey();

                m_pressure = MemoryManager<MultiRegions::ExpList3DHomogeneous1D>::AllocateSharedPtr(m_session, Homo1DKey, m_LhomZ, m_useFFT,m_homogen_dealiasing, pressure_exp);

                ASSERTL1(m_npointsZ%2==0,"Non binary number of planes have been specified");
                nz = m_npointsZ/2;                

            }
            else
            {
                //m_pressure2 = MemoryManager<MultiRegions::ContField2D>::AllocateSharedPtr(m_session, pressure_exp);
                m_pressure = MemoryManager<MultiRegions::ExpList2D>::AllocateSharedPtr(m_session, pressure_exp);
                nz = 1;
            }

            Array<OneD, MultiRegions::ExpListSharedPtr> velocity(m_velocity.num_elements());
            for(i =0 ; i < m_velocity.num_elements(); ++i)
            {
                velocity[i] = m_fields[m_velocity[i]];
            }

            // Set up Array of mappings

            m_locToGloMap = Array<OneD, CoupledLocalToGlobalC0ContMapSharedPtr> (nz);
	//    AssemblyMapCGSharedPtr my_m_locToGloMap = MemoryManager<AssemblyMapCG>::AllocateSharedPtr(m_session);

            if(m_singleMode)
            {

                ASSERTL0(nz <=2 ,"For single mode calculation can only have  nz <= 2");
                if(m_session->DefinesSolverInfo("BetaZero"))
                {
                    m_zeroMode = true;
                }
                int nz_loc = 2;
                m_locToGloMap[0] = MemoryManager<CoupledLocalToGlobalC0ContMap>::AllocateSharedPtr(m_session,m_graph,m_boundaryConditions,velocity,m_pressure,nz_loc,m_zeroMode);
            }
            else 
            {
                // base mode
                int nz_loc = 1;
                m_locToGloMap[0] = MemoryManager<CoupledLocalToGlobalC0ContMap>::AllocateSharedPtr(m_session,m_graph,m_boundaryConditions,velocity,m_pressure,nz_loc);

                if(nz > 1)
                {
                    nz_loc = 2;
                    // Assume all higher modes have the same boundary conditions and re-use mapping
                    m_locToGloMap[1] = MemoryManager<CoupledLocalToGlobalC0ContMap>::AllocateSharedPtr(m_session,m_graph,m_boundaryConditions,velocity,m_pressure->GetPlane(2),nz_loc,false);
                    // Note high order modes cannot be singular
                    for(i = 2; i < nz; ++i)
                    {
                        m_locToGloMap[i] = m_locToGloMap[1];
                    }
                }
            }
        }
        else if (expdim == 3)
        {
            //m_pressure = MemoryManager<MultiRegions::ExpList3D>::AllocateSharedPtr(pressure_exp);
            ASSERTL0(false,"Setup mapping aray");
        }
        else
        {
            ASSERTL0(false,"Exp dimension not recognised");
        }

        // creation of the extrapolation object
        if(m_equationType == eUnsteadyNavierStokes)
        {
            std::string vExtrapolation = "Standard";

            if (m_session->DefinesSolverInfo("Extrapolation"))
            {
                vExtrapolation = m_session->GetSolverInfo("Extrapolation");
            }

            m_extrapolation = GetExtrapolateFactory().CreateInstance(
                vExtrapolation,
                m_session,
                m_fields,
                m_pressure,
                m_velocity,
                m_advObject);
        }


        int nel  = m_fields[0]->GetNumElmts();
//        int n_vel  = m_fields.num_elements();


/*	std::stringstream sstm_bwdtrans;
	sstm_bwdtrans << "bwdtrans.txt";
	std::string result_bwdtrans = sstm_bwdtrans.str();
//      std::ofstream myfile_bwdtrans (result_bwdtrans);
        std::ofstream myfile_bwdtrans ("bwdtrans.txt");
	if (myfile_bwdtrans.is_open())
	{
		for(int n = 0; n < nel; ++n)
		{
			int eid = n;
//	                locExp = m_fields[m_velocity[0]]->GetExp(eid);
	                int ncoeffs = m_fields[m_velocity[0]]->GetExp(eid)->GetNcoeffs();
	                int nphys   = m_fields[m_velocity[0]]->GetExp(eid)->GetTotPoints();
        		Array<OneD, NekDouble> coeffs(ncoeffs);
		        Array<OneD, NekDouble> phys  (nphys);

			for(int counter_ncoeffs = 0; counter_ncoeffs < ncoeffs; ++counter_ncoeffs)
			{

				Vmath::Zero(ncoeffs,coeffs,1);
		                coeffs[ counter_ncoeffs ] = 1.0;
		                m_fields[m_velocity[0]]->GetExp(eid)->BwdTrans(coeffs,phys);
	
				for(int counter_phys = 0; counter_phys < nphys; ++counter_phys)         
				{
					myfile_bwdtrans << std::setprecision(17) << phys[counter_phys] << " ";
				}
				myfile_bwdtrans << endl;
	
			}
			myfile_bwdtrans << endl;


        
		}
	        myfile_bwdtrans.close();
	}
	else std::cout << "Unable to open file";
*/
	std::stringstream sstm_cartmap0;
	sstm_cartmap0 << "cartmap0.txt";
//	std::string result_cartmap0 = sstm_cartmap0.str();
//	std::basic_string<char, std::char_traits<char>, std::allocator<char> > result_cartmap0 = sstm_cartmap0.str();
//        std::ofstream myfile_cartmap0 (result_cartmap0);
  /*      std::ofstream myfile_cartmap0 ("cartmap0.txt");
	if (myfile_cartmap0.is_open())
	{
		for(int n = 0; n < nel; ++n)
		{
			int eid = n;
	                StdRegions::StdExpansionSharedPtr locExp = m_fields[m_velocity[0]]->GetExp(eid);
	                int ncoeffs = m_fields[m_velocity[0]]->GetExp(eid)->GetNcoeffs();
	                int nphys   = m_fields[m_velocity[0]]->GetExp(eid)->GetTotPoints();
		        int pqsize  = m_pressure->GetExp(eid)->GetTotPoints();
                        Array<OneD, NekDouble> deriv  (pqsize);
        		Array<OneD, NekDouble> coeffs(ncoeffs);
		        Array<OneD, NekDouble> phys  (nphys);

			for(int counter_phys = 0; counter_phys < nphys; ++counter_phys)
			{

				Vmath::Zero(nphys,phys,1);
		                phys[ counter_phys ] = 1.0;
		                locExp->PhysDeriv(MultiRegions::DirCartesianMap[0],phys, deriv);
	
				for(int counter_deriv = 0; counter_deriv < pqsize; ++counter_deriv)         
				{
					myfile_cartmap0 << std::setprecision(17) << deriv[counter_deriv] << " ";
				}
				myfile_cartmap0 << endl;
	
			}
			myfile_cartmap0 << endl;


        
		}
	        myfile_cartmap0.close();
	}
	else std::cout << "Unable to open file";

	std::stringstream sstm_cartmap1;
	sstm_cartmap1 << "cartmap1.txt";
	std::string result_cartmap1 = sstm_cartmap1.str();
//        std::ofstream myfile_cartmap1 (result_cartmap1);
        std::ofstream myfile_cartmap1 ("cartmap1.txt");
	if (myfile_cartmap1.is_open())
	{
		for(int n = 0; n < nel; ++n)
		{
			int eid = n;
	                StdRegions::StdExpansionSharedPtr locExp = m_fields[m_velocity[0]]->GetExp(eid);
	                int ncoeffs = m_fields[m_velocity[0]]->GetExp(eid)->GetNcoeffs();
	                int nphys   = m_fields[m_velocity[0]]->GetExp(eid)->GetTotPoints();
		        int pqsize  = m_pressure->GetExp(eid)->GetTotPoints();
                        Array<OneD, NekDouble> deriv  (pqsize);
        		Array<OneD, NekDouble> coeffs(ncoeffs);
		        Array<OneD, NekDouble> phys  (nphys);

			for(int counter_phys = 0; counter_phys < nphys; ++counter_phys)
			{

				Vmath::Zero(nphys,phys,1);
		                phys[ counter_phys ] = 1.0;
		                locExp->PhysDeriv(MultiRegions::DirCartesianMap[1],phys, deriv);
	
				for(int counter_deriv = 0; counter_deriv < pqsize; ++counter_deriv)         
				{
					myfile_cartmap1 << std::setprecision(17) << deriv[counter_deriv] << " ";
				}
				myfile_cartmap1 << endl;
	
			}
			myfile_cartmap1 << endl;


        
		}
	        myfile_cartmap1.close();
	}
	else std::cout << "Unable to open file";
*/
        // locExp->IProductWRTBase(tmpphys,coeffs);                                for all tmpphys

/*	std::stringstream sstm_IP;
	sstm_IP << "IP.txt";
	std::string result_IP = sstm_IP.str();
//        std::ofstream myfile_IP (result_IP);
        std::ofstream myfile_IP ("IP.txt");
	if (myfile_IP.is_open())
	{
		for(int n = 0; n < nel; ++n)
		{
			int eid = n;
	                StdRegions::StdExpansionSharedPtr locExp = m_fields[m_velocity[0]]->GetExp(eid);
	                int ncoeffs = m_fields[m_velocity[0]]->GetExp(eid)->GetNcoeffs();
	                int nphys   = m_fields[m_velocity[0]]->GetExp(eid)->GetTotPoints();
		        int pqsize  = m_pressure->GetExp(eid)->GetTotPoints();
                        Array<OneD, NekDouble> deriv  (pqsize);
        		Array<OneD, NekDouble> coeffs(ncoeffs);
		        Array<OneD, NekDouble> phys  (nphys);

			for(int counter_phys = 0; counter_phys < nphys; ++counter_phys)
			{

				Vmath::Zero(nphys,phys,1);
		                phys[ counter_phys ] = 1.0;
		                locExp->IProductWRTBase(phys,coeffs);
	
				for(int counter_ncoeffs = 0; counter_ncoeffs < ncoeffs; ++counter_ncoeffs)         
				{
					myfile_IP << std::setprecision(17) << coeffs[counter_ncoeffs] << " ";
				}
				myfile_IP << endl;
	
			}
			myfile_IP << endl;


        
		}
	        myfile_IP.close();
	}
	else std::cout << "Unable to open file";
*/
/*	std::stringstream sstm_IP_d0;
	sstm_IP_d0 << "IP_d0.txt";
	std::string result_IP_d0 = sstm_IP_d0.str();
//        std::ofstream myfile_IP (result_IP);
        std::ofstream myfile_IP_d0 ("IP_d0.txt");
	if (myfile_IP_d0.is_open())
	{
		for(int n = 0; n < nel; ++n)
		{
			int eid = n;
	                StdRegions::StdExpansionSharedPtr locExp = m_fields[m_velocity[0]]->GetExp(eid);
	                int ncoeffs = m_fields[m_velocity[0]]->GetExp(eid)->GetNcoeffs();
	                int nphys   = m_fields[m_velocity[0]]->GetExp(eid)->GetTotPoints();
		        int pqsize  = m_pressure->GetExp(eid)->GetTotPoints();
                        Array<OneD, NekDouble> deriv  (pqsize);
        		Array<OneD, NekDouble> coeffs(ncoeffs);
		        Array<OneD, NekDouble> phys  (nphys);

			for(int counter_phys = 0; counter_phys < nphys; ++counter_phys)
			{

				Vmath::Zero(nphys,phys,1);
		                phys[ counter_phys ] = 1.0;
		                locExp->IProductWRTDerivBase(0, phys,coeffs);
	
				for(int counter_ncoeffs = 0; counter_ncoeffs < ncoeffs; ++counter_ncoeffs)         
				{
					myfile_IP_d0 << std::setprecision(17) << coeffs[counter_ncoeffs] << " ";
				}
				myfile_IP_d0 << endl;
	
			}
			myfile_IP_d0 << endl;


        
		}
	        myfile_IP_d0.close();
	}
	else std::cout << "Unable to open file";


	std::stringstream sstm_IP_d1;
	sstm_IP_d1 << "IP_d1.txt";
	std::string result_IP_d1 = sstm_IP_d1.str();
//        std::ofstream myfile_IP (result_IP);
        std::ofstream myfile_IP_d1 ("IP_d1.txt");
	if (myfile_IP_d1.is_open())
	{
		for(int n = 0; n < nel; ++n)
		{
			int eid = n;
	                StdRegions::StdExpansionSharedPtr locExp = m_fields[m_velocity[0]]->GetExp(eid);
	                int ncoeffs = m_fields[m_velocity[0]]->GetExp(eid)->GetNcoeffs();
	                int nphys   = m_fields[m_velocity[0]]->GetExp(eid)->GetTotPoints();
		        int pqsize  = m_pressure->GetExp(eid)->GetTotPoints();
                        Array<OneD, NekDouble> deriv  (pqsize);
        		Array<OneD, NekDouble> coeffs(ncoeffs);
		        Array<OneD, NekDouble> phys  (nphys);

			for(int counter_phys = 0; counter_phys < nphys; ++counter_phys)
			{

				Vmath::Zero(nphys,phys,1);
		                phys[ counter_phys ] = 1.0;
		                locExp->IProductWRTDerivBase(1, phys,coeffs);
	
				for(int counter_ncoeffs = 0; counter_ncoeffs < ncoeffs; ++counter_ncoeffs)         
				{
					myfile_IP_d1 << std::setprecision(17) << coeffs[counter_ncoeffs] << " ";
				}
				myfile_IP_d1 << endl;
	
			}
			myfile_IP_d1 << endl;


        
		}
	        myfile_IP_d1.close();
	}
	else std::cout << "Unable to open file";
*/

	//  m_pressure->GetExp(eid)->IProductWRTBase(deriv,pcoeffs);

/*	std::stringstream sstm_IPp;
	sstm_IPp << "IPp.txt";
	std::string result_IPp = sstm_IPp.str();
//        std::ofstream myfile_IPp (result_IPp);
        std::ofstream myfile_IPp ("IPp.txt");
	if (myfile_IPp.is_open())
	{
		for(int n = 0; n < nel; ++n)
		{
			int eid = n;
	                StdRegions::StdExpansionSharedPtr locExp = m_fields[m_velocity[0]]->GetExp(eid);
	                int ncoeffs = m_fields[m_velocity[0]]->GetExp(eid)->GetNcoeffs();
	                int nphys   = m_fields[m_velocity[0]]->GetExp(eid)->GetTotPoints();
		        int pqsize  = m_pressure->GetExp(eid)->GetTotPoints();
            		int psize   = m_pressure->GetExp(eid)->GetNcoeffs();
               		Array<OneD, NekDouble> pcoeffs(psize);
                        Array<OneD, NekDouble> deriv  (pqsize);
        		Array<OneD, NekDouble> coeffs(ncoeffs);
		        Array<OneD, NekDouble> phys  (nphys);

			for(int counter_phys = 0; counter_phys < pqsize; ++counter_phys)
			{

				Vmath::Zero(pqsize,deriv,1);
		                deriv[ counter_phys ] = 1.0;
		                m_pressure->GetExp(eid)->IProductWRTBase(deriv,pcoeffs);
	
				for(int counter_ncoeffs = 0; counter_ncoeffs < psize; ++counter_ncoeffs)         
				{
					myfile_IPp << std::setprecision(17) << pcoeffs[counter_ncoeffs] << " ";
				}
				myfile_IPp << endl;
	
			}
			myfile_IPp << endl;

		        Array<OneD, unsigned int> bmap,imap;

		        locExp->GetBoundaryMap(bmap);
		        locExp->GetInteriorMap(imap);

	        	int nbmap = bmap.num_elements();
		        int nimap = imap.num_elements(); 
        */


//		std::stringstream sstmb;
//		sstmb << "bmap_" << n << ".txt";
//		std::string resultb = sstmb.str();


//          std::ofstream myfile_bmap (resultb);
/*          std::ofstream myfile_bmap ("bmap.txt");
	  if (myfile_bmap.is_open())
	  {
	    for (int i = 0; i < nbmap; i++)
	    {

		    {
			myfile_bmap << std::setprecision(17) << bmap[i] << " ";
		    }

	    }
                myfile_bmap.close();
	  }
	  else std::cout << "Unable to open file";

		std::stringstream sstmi;
		sstmi << "imap_" << n << ".txt";
		std::string resulti = sstmi.str();
*/
//	    std::cout << "saving current imap " << resulti << std::endl;

//          std::ofstream myfile_imap (resulti);
/*          std::ofstream myfile_imap ("imap.txt");
	  if (myfile_imap.is_open())
	  {
	    for (int i = 0; i < nimap; i++)
	    {

		    {
			myfile_imap << std::setprecision(17) << imap[i] << " ";
		    }

	    }
                myfile_imap.close();
	  }
	  else std::cout << "Unable to open file";
*/
      //      StdRegions::ConstFactorMap factors;
      //      factors[StdRegions::eFactorLambda] = lambda/m_kinvis;
      //      LocalRegions::MatrixKey helmkey(StdRegions::eHelmholtz,
      //                                      locExp->DetShapeType(),
      //                                      *locExp,
      //                                      factors);

      //          DNekScalMat &HelmMat = *(locExp->as<LocalRegions::Expansion>()
      //                                         ->GetLocMatrix(helmkey));
/*
		}
	        myfile_IPp.close();
	}
	else std::cout << "Unable to open file";
*/
    }

    void CoupledLinearNS_TT::SetUpCoupledMatrix(const NekDouble lambda,  const Array< OneD, Array< OneD, NekDouble > > &Advfield, bool IsLinearNSEquation)
    {

        
        int nz;
        if(m_singleMode)
        {
            
            NekDouble lambda_imag; 
            
            // load imaginary component of any potential shift
            // Probably should be called from DriverArpack but not yet
            // clear how to do this
            m_session->LoadParameter("imagShift",lambda_imag,NekConstants::kNekUnsetDouble);
            nz = 1;
            m_mat  = Array<OneD, CoupledSolverMatrices> (nz);
            
            ASSERTL1(m_npointsZ <=2,"Only expected a maxmimum of two planes in single mode linear NS solver");
            
            if(m_zeroMode)
            {
                SetUpCoupledMatrix(lambda,Advfield,IsLinearNSEquation,0,m_mat[0],m_locToGloMap[0],lambda_imag);
            }
            else
            {
                NekDouble beta =  2*M_PI/m_LhomZ; 
                NekDouble lam = lambda + m_kinvis*beta*beta;
                
                SetUpCoupledMatrix(lam,Advfield,IsLinearNSEquation,1,m_mat[0],m_locToGloMap[0],lambda_imag);
            }
        }
        else 
        {
            int n;
            if(m_npointsZ > 1)
            { 
                nz = m_npointsZ/2;
            }
            else
            {
                nz =  1;
            }
            
            m_mat  = Array<OneD, CoupledSolverMatrices> (nz);
            
            // mean mode or 2D mode.
            SetUpCoupledMatrix(lambda,Advfield,IsLinearNSEquation,0,m_mat[0],m_locToGloMap[0]);
            
            for(n = 1; n < nz; ++n)
            {
                NekDouble beta = 2*M_PI*n/m_LhomZ;
                
                NekDouble lam = lambda + m_kinvis*beta*beta;
                
                SetUpCoupledMatrix(lam,Advfield,IsLinearNSEquation,n,m_mat[n],m_locToGloMap[n]);
            }
        }
        


    }

    void CoupledLinearNS_TT::SetUpCoupledMatrix(const NekDouble lambda,  const Array< OneD, Array< OneD, NekDouble > > &Advfield, bool IsLinearNSEquation,const int HomogeneousMode, CoupledSolverMatrices &mat, CoupledLocalToGlobalC0ContMapSharedPtr &locToGloMap, const NekDouble lambda_imag)
    {

	if(use_Newton)        
	{
		IsLinearNSEquation = true;
	}

        int  n,i,j,k,eid;
        int  nel  = m_fields[m_velocity[0]]->GetNumElmts();
        int  nvel   = m_velocity.num_elements();
        
        // if Advfield is defined can assume it is an Oseen or LinearNS equation
        bool AddAdvectionTerms = (Advfield ==  NullNekDoubleArrayofArray)? false: true; 
        //bool AddAdvectionTerms = true; // Temporary debugging trip
        
        // call velocity space Static condensation and fill block
        // matrices.  Need to set this up differently for Oseen and
        // Lin NS.  Ideally should make block diagonal for Stokes and
        // Oseen problems.
        DNekScalMatSharedPtr loc_mat;
        StdRegions::StdExpansionSharedPtr locExp;
        NekDouble one = 1.0;
        int nint,nbndry;
        int rows, cols;
        NekDouble zero = 0.0;
        Array<OneD, unsigned int> bmap,imap; 
        
        Array<OneD,unsigned int> nsize_bndry   (nel);
        Array<OneD,unsigned int> nsize_bndry_p1(nel);
        Array<OneD,unsigned int> nsize_int     (nel);
        Array<OneD,unsigned int> nsize_p       (nel);
        Array<OneD,unsigned int> nsize_p_m1    (nel);
        
	


        int nz_loc;
        
        if(HomogeneousMode) // Homogeneous mode flag
        {
            nz_loc = 2;
        }
        else
        {
            if(m_singleMode)
            {
                nz_loc = 2;
            }
            else
            {
                nz_loc = 1;
            }
        }
        
        // Set up block matrix sizes - 
        for(n = 0; n < nel; ++n)
        {
            eid = m_fields[m_velocity[0]]->GetOffset_Elmt_Id(n);
            nsize_bndry[n] = nvel*m_fields[m_velocity[0]]->GetExp(eid)->NumBndryCoeffs()*nz_loc;
            nsize_bndry_p1[n] = nsize_bndry[n]+nz_loc;
            nsize_int[n] = (nvel*m_fields[m_velocity[0]]->GetExp(eid)->GetNcoeffs()*nz_loc - nsize_bndry[n]);
            nsize_p[n] = m_pressure->GetExp(eid)->GetNcoeffs()*nz_loc;
            nsize_p_m1[n] = nsize_p[n]-nz_loc;
     /*       std::cout << "nsize_bndry[n] " << nsize_bndry[n] << std::endl;
            std::cout << "nsize_bndry_p1[n] " << nsize_bndry_p1[n] << std::endl;
            std::cout << "nsize_int[n] " << nsize_int[n] << std::endl;
            std::cout << "nsize_p[n] " << nsize_p[n] << std::endl;
            std::cout << "nsize_p_m1[n] " << nsize_p_m1[n] << std::endl; */
        }

	// need my mats for projection purposes: think about: have no splitting bnd/p/int could be beneficial for number of affine terms
	Eigen::MatrixXd D_all = Eigen::MatrixXd::Zero( nsize_int[0]*nel , nsize_int[0]*nel );
	Eigen::MatrixXd Dbnd_all = Eigen::MatrixXd::Zero( nsize_p[0]*nel , nsize_bndry[0]*nel );
	Eigen::MatrixXd Dint_all = Eigen::MatrixXd::Zero( nsize_p[0]*nel , nsize_int[0]*nel );
	Eigen::MatrixXd B_all = Eigen::MatrixXd::Zero( nsize_bndry[0]*nel , nsize_int[0]*nel );
	Eigen::MatrixXd C_all = Eigen::MatrixXd::Zero( nsize_bndry[0]*nel , nsize_int[0]*nel );
	Eigen::MatrixXd A_all = Eigen::MatrixXd::Zero( nsize_bndry[0]*nel , nsize_bndry[0]*nel );

	Eigen::MatrixXd D_adv_all = Eigen::MatrixXd::Zero( nsize_int[0]*nel , nsize_int[0]*nel );
	Eigen::MatrixXd B_adv_all = Eigen::MatrixXd::Zero( nsize_bndry[0]*nel , nsize_int[0]*nel );
	Eigen::MatrixXd C_adv_all = Eigen::MatrixXd::Zero( nsize_bndry[0]*nel , nsize_int[0]*nel );
	Eigen::MatrixXd A_adv_all = Eigen::MatrixXd::Zero( nsize_bndry[0]*nel , nsize_bndry[0]*nel );

	Eigen::MatrixXd D_no_adv_all = Eigen::MatrixXd::Zero( nsize_int[0]*nel , nsize_int[0]*nel );
	Eigen::MatrixXd B_no_adv_all = Eigen::MatrixXd::Zero( nsize_bndry[0]*nel , nsize_int[0]*nel );
	Eigen::MatrixXd C_no_adv_all = Eigen::MatrixXd::Zero( nsize_bndry[0]*nel , nsize_int[0]*nel );
	Eigen::MatrixXd A_no_adv_all = Eigen::MatrixXd::Zero( nsize_bndry[0]*nel , nsize_bndry[0]*nel );

//	Eigen::MatrixXd Dd_all = Eigen::MatrixXd::Zero(  nsize_int[0]*nel , nsize_int[0]*nel );
        
//	cout << typeid(nsize_int[0]).name() << endl;
//	cout << typeid(nsize_bndry[0]).name() << endl;
//	cout <<  Dd_all << endl;

        MatrixStorage blkmatStorage = eDIAGONAL;
        DNekScalBlkMatSharedPtr pAh = MemoryManager<DNekScalBlkMat>
        ::AllocateSharedPtr(nsize_bndry_p1,nsize_bndry_p1,blkmatStorage);
        mat.m_BCinv = MemoryManager<DNekScalBlkMat>
        ::AllocateSharedPtr(nsize_bndry,nsize_int,blkmatStorage);
        mat.m_Btilde = MemoryManager<DNekScalBlkMat>
        ::AllocateSharedPtr(nsize_bndry,nsize_int,blkmatStorage);
        mat.m_Cinv = MemoryManager<DNekScalBlkMat>
        ::AllocateSharedPtr(nsize_int,nsize_int,blkmatStorage);
        
        mat.m_D_bnd = MemoryManager<DNekScalBlkMat>
        ::AllocateSharedPtr(nsize_p,nsize_bndry,blkmatStorage);
        
        mat.m_D_int = MemoryManager<DNekScalBlkMat>
        ::AllocateSharedPtr(nsize_p,nsize_int,blkmatStorage);
        
        // Final level static condensation matrices. 
        DNekScalBlkMatSharedPtr pBh = MemoryManager<DNekScalBlkMat>
        ::AllocateSharedPtr(nsize_bndry_p1,nsize_p_m1,blkmatStorage);
        DNekScalBlkMatSharedPtr pCh = MemoryManager<DNekScalBlkMat>
        ::AllocateSharedPtr(nsize_p_m1,nsize_bndry_p1,blkmatStorage);
        DNekScalBlkMatSharedPtr pDh = MemoryManager<DNekScalBlkMat>
        ::AllocateSharedPtr(nsize_p_m1,nsize_p_m1,blkmatStorage);
        
        
        Timer timer;
        timer.Start();
        for(n = 0; n < nel; ++n)
        {
            eid = m_fields[m_velocity[0]]->GetOffset_Elmt_Id(n);
            nbndry = nsize_bndry[n];
            nint   = nsize_int[n];
              
	 //       cout << "nint: " << nint << endl;
	 //       cout << "nbndry: " << nbndry << endl;

            k = nsize_bndry_p1[n];
            DNekMatSharedPtr Ah = MemoryManager<DNekMat>::AllocateSharedPtr(k,k,zero);
            DNekMatSharedPtr A_adv = MemoryManager<DNekMat>::AllocateSharedPtr(nbndry+1,nbndry+1,zero);  // the +1 should not be necessary but would have to change indexing for that
            DNekMatSharedPtr A_no_adv = MemoryManager<DNekMat>::AllocateSharedPtr(nbndry+1,nbndry+1,zero);
            Array<OneD, NekDouble> Ah_data = Ah->GetPtr();
	    Array<OneD, NekDouble> A_adv_data = A_adv->GetPtr();
	    Array<OneD, NekDouble> A_no_adv_data = A_no_adv->GetPtr();
            int AhRows = k;
            DNekMatSharedPtr B  = MemoryManager<DNekMat>::AllocateSharedPtr(nbndry,nint,zero);
            DNekMatSharedPtr B_adv  = MemoryManager<DNekMat>::AllocateSharedPtr(nbndry,nint,zero);
            DNekMatSharedPtr B_no_adv  = MemoryManager<DNekMat>::AllocateSharedPtr(nbndry,nint,zero);
            Array<OneD, NekDouble> B_data = B->GetPtr();
	    Array<OneD, NekDouble> B_adv_data = B_adv->GetPtr();
	    Array<OneD, NekDouble> B_no_adv_data = B_no_adv->GetPtr();
            DNekMatSharedPtr C  = MemoryManager<DNekMat>::AllocateSharedPtr(nbndry,nint,zero);
            DNekMatSharedPtr C_adv  = MemoryManager<DNekMat>::AllocateSharedPtr(nbndry,nint,zero);
            DNekMatSharedPtr C_no_adv  = MemoryManager<DNekMat>::AllocateSharedPtr(nbndry,nint,zero);
            Array<OneD, NekDouble> C_data = C->GetPtr();
	    Array<OneD, NekDouble> C_adv_data = C_adv->GetPtr();
	    Array<OneD, NekDouble> C_no_adv_data = C_no_adv->GetPtr();
            DNekMatSharedPtr D  = MemoryManager<DNekMat>::AllocateSharedPtr(nint, nint,zero);
            DNekMatSharedPtr D_adv  = MemoryManager<DNekMat>::AllocateSharedPtr(nint, nint,zero);
            DNekMatSharedPtr D_no_adv  = MemoryManager<DNekMat>::AllocateSharedPtr(nint, nint,zero);
            Array<OneD, NekDouble> D_data = D->GetPtr();
            Array<OneD, NekDouble> D_adv_data = D_adv->GetPtr();
            Array<OneD, NekDouble> D_no_adv_data = D_no_adv->GetPtr();
            
            DNekMatSharedPtr Dbnd = MemoryManager<DNekMat>::AllocateSharedPtr(nsize_p[n],nsize_bndry[n],zero);
            DNekMatSharedPtr Dint = MemoryManager<DNekMat>::AllocateSharedPtr(nsize_p[n],nsize_int[n],zero);
            
            locExp = m_fields[m_velocity[0]]->GetExp(eid);
            locExp->GetBoundaryMap(bmap);
            locExp->GetInteriorMap(imap);
            StdRegions::ConstFactorMap factors;
            factors[StdRegions::eFactorLambda] = lambda/m_kinvis;
/*            LocalRegions::MatrixKey helmkey(StdRegions::eHelmholtz,
                                            locExp->DetShapeType(),
                                            *locExp,
                                            factors);
*/

            LocalRegions::MatrixKey helmkey(StdRegions::eHelmholtz,
                                            locExp->DetShapeType(),
                                            *locExp,
                                            factors);


/*            LocalRegions::MatrixKey helmkey(StdRegions::eLaplacian,
                                            locExp->DetShapeType(),
                                            *locExp,
                                            factors);

  */          
            
            int ncoeffs = m_fields[m_velocity[0]]->GetExp(eid)->GetNcoeffs();
            int nphys   = m_fields[m_velocity[0]]->GetExp(eid)->GetTotPoints();
            int nbmap = bmap.num_elements();
            int nimap = imap.num_elements(); 

	 //   std::cout << "nimap " << nimap  << std::endl;
            
            Array<OneD, NekDouble> coeffs(ncoeffs);
            Array<OneD, NekDouble> phys  (nphys);
            int psize   = m_pressure->GetExp(eid)->GetNcoeffs();
            int pqsize  = m_pressure->GetExp(eid)->GetTotPoints();
            
            Array<OneD, NekDouble> deriv  (pqsize);
            Array<OneD, NekDouble> pcoeffs(psize);
            
            if(AddAdvectionTerms == false) // use static condensed managed matrices
            {
                // construct velocity matrices using statically
                // condensed elemental matrices and then construct
                // pressure matrix systems
                DNekScalBlkMatSharedPtr CondMat; 
                CondMat = locExp->GetLocStaticCondMatrix(helmkey);
                
                for(k = 0; k < nvel*nz_loc; ++k)
                {
                    DNekScalMat &SubBlock = *CondMat->GetBlock(0,0);
                    rows = SubBlock.GetRows();
                    cols = SubBlock.GetColumns(); 
                    for(i = 0; i < rows; ++i)
                    {
                        for(j = 0; j < cols; ++j)
                        {
                            (*Ah)(i+k*rows,j+k*cols) = m_kinvis*SubBlock(i,j);
                        }                        
                    }
                }
                
                for(k = 0; k < nvel*nz_loc; ++k)
                {
                    DNekScalMat &SubBlock  = *CondMat->GetBlock(0,1);
                    DNekScalMat &SubBlock1 = *CondMat->GetBlock(1,0);
                    rows = SubBlock.GetRows();
                    cols = SubBlock.GetColumns(); 
                    for(i = 0; i < rows; ++i)
                    {
                        for(j = 0; j < cols; ++j)
                        {
                            (*B)(i+k*rows,j+k*cols) = SubBlock(i,j);
                            (*C)(i+k*rows,j+k*cols) = m_kinvis*SubBlock1(j,i);
                        }
                    }
                }
                
                for(k = 0; k < nvel*nz_loc; ++k)
                {
                    DNekScalMat &SubBlock = *CondMat->GetBlock(1,1);
                    NekDouble inv_kinvis = 1.0/m_kinvis;
                    rows = SubBlock.GetRows();
                    cols = SubBlock.GetColumns(); 
                    for(i = 0; i < rows; ++i)
                    {
                        for(j = 0; j < cols; ++j)
                        {
                            (*D)(i+k*rows,j+k*cols) = inv_kinvis*SubBlock(i,j);
                        }
                    }
                }
                
                
                // Loop over pressure space and construct boundary block matrices.         
                for(i = 0; i < bmap.num_elements(); ++i)
                {
                    // Fill element with mode
                    Vmath::Zero(ncoeffs,coeffs,1);
                    coeffs[bmap[i]] = 1.0;
                    m_fields[m_velocity[0]]->GetExp(eid)->BwdTrans(coeffs,phys);
                    
                    // Differentiation & Inner product wrt base. 
                    for(j = 0; j < nvel; ++j)
                    {
                        if( (nz_loc == 2)&&(j == 2)) // handle d/dz derivative
                        {
                            NekDouble beta =  -2*M_PI*HomogeneousMode/m_LhomZ;
                            
                            Vmath::Smul(m_fields[m_velocity[0]]->GetExp(eid)->GetTotPoints(), beta, phys,1,deriv,1);
                            
                            m_pressure->GetExp(eid)->IProductWRTBase(deriv,pcoeffs);
                            
                            Blas::Dcopy(psize,&(pcoeffs)[0],1,
                                        Dbnd->GetRawPtr() + 
                                        ((nz_loc*j+1)*bmap.num_elements()+i)*nsize_p[n],1);
                            
                            Vmath::Neg(psize,pcoeffs,1);
                            Blas::Dcopy(psize,&(pcoeffs)[0],1,
                                        Dbnd->GetRawPtr() + 
                                        ((nz_loc*j)*bmap.num_elements()+i)*nsize_p[n]+psize,1);
                            
                        }
                        else
                        {
                            if(j < 2) // required for mean mode of homogeneous expansion 
                            {
                                locExp->PhysDeriv(MultiRegions::DirCartesianMap[j],phys,deriv);		
                                m_pressure->GetExp(eid)->IProductWRTBase(deriv,pcoeffs);
                                // copy into column major storage. 
                                for(k = 0; k < nz_loc; ++k)
                                {
                                    Blas::Dcopy(psize,&(pcoeffs)[0],1,
                                                Dbnd->GetRawPtr() + 
                                                ((nz_loc*j+k)*bmap.num_elements()+i)*nsize_p[n]+ k*psize,1);
                                }
                            }
                        }
                    }
                }
                
                for(i = 0; i < imap.num_elements(); ++i)
                {
                    // Fill element with mode
                    Vmath::Zero(ncoeffs,coeffs,1);
                    coeffs[imap[i]] = 1.0;
                    m_fields[m_velocity[0]]->GetExp(eid)->BwdTrans(coeffs,phys);
                    
                    // Differentiation & Inner product wrt base. 
                    for(j = 0; j < nvel; ++j)
                    {
                        if( (nz_loc == 2)&&(j == 2)) // handle d/dz derivative
                        {
                            NekDouble beta = -2*M_PI*HomogeneousMode/m_LhomZ;
                            
                            Vmath::Smul(m_fields[m_velocity[0]]->GetExp(eid)->GetTotPoints(), beta, phys,1,deriv,1); 
                            
                            m_pressure->GetExp(eid)->IProductWRTBase(deriv,pcoeffs);
                            
                            Blas::Dcopy(psize,&(pcoeffs)[0],1,
                                        Dint->GetRawPtr() + 
                                        ((nz_loc*j+1)*imap.num_elements()+i)*nsize_p[n],1);
                            
                            Vmath::Neg(psize,pcoeffs,1);
                            Blas::Dcopy(psize,&(pcoeffs)[0],1,
                                        Dint->GetRawPtr() + 
                                        ((nz_loc*j)*imap.num_elements()+i)*nsize_p[n]+psize,1);
                            
                        }
                        else
                        {
                            if(j < 2) // required for mean mode of homogeneous expansion 
                            {
                                //m_fields[m_velocity[0]]->GetExp(eid)->PhysDeriv(j,phys, deriv);
                                locExp->PhysDeriv(MultiRegions::DirCartesianMap[j],phys,deriv);
                                
                                m_pressure->GetExp(eid)->IProductWRTBase(deriv,pcoeffs);
                                
                                // copy into column major storage. 
                                for(k = 0; k < nz_loc; ++k)
                                {
                                    Blas::Dcopy(psize,&(pcoeffs)[0],1,
                                                Dint->GetRawPtr() +
                                                ((nz_loc*j+k)*imap.num_elements()+i)*nsize_p[n]+ k*psize,1);
                                }
                            }
                        }
                    }
                }
            }
            else
            {
                // construct velocity matrices and pressure systems at
                // the same time resusing differential of velocity
                // space
                
	        LocalRegions::MatrixKey helmkey_l00(StdRegions::eLaplacian00,
                                            locExp->DetShapeType(),
                                            *locExp,
                                            factors);


		DNekScalMat &HelmMat = *(locExp->as<LocalRegions::Expansion>()
                                               ->GetLocMatrix(helmkey_l00));

                Array<OneD, const NekDouble> HelmMat_data = HelmMat.GetOwnedMatrix()->GetPtr();
              

/*		std::stringstream sstm_l00;
		sstm_l00 << "Lapl00_" << n << ".txt";
		std::string result_l00 = sstm_l00.str();
		const char* rr_l00 = result_l00.c_str();

        	  std::ofstream myfileHM_l00 (rr_l00);
		  if (myfileHM_l00.is_open())
		  {
		    for (int i = 0; i < HelmMat_data.num_elements(); i++)
		    {

			    {
				myfileHM_l00 << std::setprecision(17) << HelmMat_data[i] << " ";
			    }

		    }
        	        myfileHM_l00.close();
		  }
		  else std::cout << "Unable to open file";

*/



////////////////////////////////////////////////////////////////////////////////////////////////

/*
	        LocalRegions::MatrixKey helmkey_l11(StdRegions::eLaplacian11,
                                            locExp->DetShapeType(),
                                            *locExp,
                                            factors);


		HelmMat = *(locExp->as<LocalRegions::Expansion>()
                                               ->GetLocMatrix(helmkey_l11));

              
                HelmMat_data = HelmMat.GetOwnedMatrix()->GetPtr();
                

		std::stringstream sstm_l11;
		sstm_l11 << "Lapl11_" << n << ".txt";
		std::string result_l11 = sstm_l11.str();
		const char* rr_l11 = result_l11.c_str();

        	  std::ofstream myfileHM_l11 (rr_l11);
		  if (myfileHM_l11.is_open())
		  {
		    for (int i = 0; i < HelmMat_data.num_elements(); i++)
		    {

			    {
				myfileHM_l11 << std::setprecision(17) << HelmMat_data[i] << " ";
			    }

		    }
        	        myfileHM_l11.close();
		  }
		  else std::cout << "Unable to open file";
*/
////////////////////////////////////////////////////////////////////////////////////////////////

/*
	        LocalRegions::MatrixKey helmkey_l01(StdRegions::eLaplacian01,
                                            locExp->DetShapeType(),
                                            *locExp,
                                            factors);


		HelmMat = *(locExp->as<LocalRegions::Expansion>()
                                               ->GetLocMatrix(helmkey_l01));

              
                HelmMat_data = HelmMat.GetOwnedMatrix()->GetPtr();
                

		std::stringstream sstm_l01;
		sstm_l01 << "Lapl01_" << n << ".txt";
		std::string result_l01 = sstm_l01.str();
		const char* rr_l01 = result_l01.c_str();

        	  std::ofstream myfileHM_l01 (rr_l01);
		  if (myfileHM_l01.is_open())
		  {
		    for (int i = 0; i < HelmMat_data.num_elements(); i++)
		    {

			    {
				myfileHM_l01 << std::setprecision(17) << HelmMat_data[i] << " ";
			    }

		    }
        	        myfileHM_l01.close();
		  }
		  else std::cout << "Unable to open file";
*/
////////////////////////////////////////////////////////////////////////////////////////////////


/*	        LocalRegions::MatrixKey helmkey_w0(StdRegions::eWeakDeriv0,
                                            locExp->DetShapeType(),
                                            *locExp,
                                            factors);


		HelmMat = *(locExp->as<LocalRegions::Expansion>()
                                               ->GetLocMatrix(helmkey_w0));

              
                HelmMat_data = HelmMat.GetOwnedMatrix()->GetPtr();
                

		std::stringstream sstm_w0;
		sstm_w0 << "w0_" << n << ".txt";
		std::string result_w0 = sstm_w0.str();
		const char* rr_w0 = result_w0.c_str();

        	  std::ofstream myfileHM_w0 (rr_w0);
		  if (myfileHM_w0.is_open())
		  {
		    for (int i = 0; i < HelmMat_data.num_elements(); i++)
		    {

			    {
				myfileHM_w0 << std::setprecision(17) << HelmMat_data[i] << " ";
			    }

		    }
        	        myfileHM_w0.close();
		  }
		  else std::cout << "Unable to open file";
*/


////////////////////////////////////////////////////////////////////////////////////////////////


/*	        LocalRegions::MatrixKey helmkey_w1(StdRegions::eWeakDeriv1,
                                            locExp->DetShapeType(),
                                            *locExp,
                                            factors);


		HelmMat = *(locExp->as<LocalRegions::Expansion>()
                                               ->GetLocMatrix(helmkey_w1));

              
                HelmMat_data = HelmMat.GetOwnedMatrix()->GetPtr();
                

		std::stringstream sstm_w1;
		sstm_w1 << "w1_" << n << ".txt";
		std::string result_w1 = sstm_w1.str();
		const char* rr_w1 = result_w1.c_str();

        	  std::ofstream myfileHM_w1 (rr_w1);
		  if (myfileHM_w1.is_open())
		  {
		    for (int i = 0; i < HelmMat_data.num_elements(); i++)
		    {

			    {
				myfileHM_w1 << std::setprecision(17) << HelmMat_data[i] << " ";
			    }

		    }
        	        myfileHM_w1.close();
		  }
		  else std::cout << "Unable to open file";
*/


///////////////////////////////////////////////////////////////////////////////////////////////



                HelmMat = *(locExp->as<LocalRegions::Expansion>()
                                               ->GetLocMatrix(helmkey));

                DNekScalMatSharedPtr MassMat;
                
                HelmMat_data = HelmMat.GetOwnedMatrix()->GetPtr();
                NekDouble HelmMatScale = HelmMat.Scale();
                int HelmMatRows = HelmMat.GetRows();
                
                if((lambda_imag != NekConstants::kNekUnsetDouble)&&(nz_loc == 2))
                {
                    LocalRegions::MatrixKey masskey(StdRegions::eMass,
                                                    locExp->DetShapeType(),
                                                    *locExp);
                    MassMat = locExp->as<LocalRegions::Expansion>()
                                    ->GetLocMatrix(masskey);
                }
                
                Array<OneD, NekDouble> Advtmp;
                Array<OneD, Array<OneD, NekDouble> > AdvDeriv(nvel*nvel);
                // Use ExpList phys array for temporaary storage
                Array<OneD, NekDouble> tmpphys = m_fields[0]->UpdatePhys();
                int phys_offset = m_fields[m_velocity[0]]->GetPhys_Offset(eid);
                int nv;
                int npoints = locExp->GetTotPoints();
                

	/*	std::stringstream sstm;
		sstm << "HelmMat_" << n << ".txt";
		std::string result = sstm.str();
		const char* rr = result.c_str();

        	  std::ofstream myfileHM (rr);
		  if (myfileHM.is_open())
		  {
		    for (int i = 0; i < HelmMat_data.num_elements(); i++)
		    {

			    {
				myfileHM << std::setprecision(17) << HelmMat_data[i] << " ";
			    }

		    }
        	        myfileHM.close();
		  }
		  else std::cout << "Unable to open file";
*/

                // Calculate derivative of base flow 
                if(IsLinearNSEquation)
                {
                    int nv1;
                    int cnt = 0;
                    AdvDeriv[0] = Array<OneD, NekDouble>(nvel*nvel*npoints);
                    for(nv = 0; nv < nvel; ++nv)
                    {
                        for(nv1 = 0; nv1 < nvel; ++nv1)
                        {
                            if(cnt < nvel*nvel-1)
                            {
                                AdvDeriv[cnt+1] = AdvDeriv[cnt] + npoints;
                                ++cnt;
                            }
                            
                            if((nv1 == 2)&&(m_HomogeneousType == eHomogeneous1D))
                            {
                                Vmath::Zero(npoints,AdvDeriv[nv*nvel+nv1],1); // dU/dz = 0
                            }
                            else
                            {
                                locExp->PhysDeriv(MultiRegions::DirCartesianMap[nv1],Advfield[nv] + phys_offset, AdvDeriv[nv*nvel+nv1]);
                            }
                        }
                    }
                }
                
                
//	        cout << "nbmap: " << nbmap << endl;
//	        cout << "nbndry: " << nbndry << endl;

                for(i = 0; i < nbmap; ++i)
                {
                    
                    // Fill element with mode
                    Vmath::Zero(ncoeffs,coeffs,1);
                    coeffs[bmap[i]] = 1.0;
                    locExp->BwdTrans(coeffs,phys);
                    
                    for(k = 0; k < nvel*nz_loc; ++k)
                    {
                        for(j = 0; j < nbmap; ++j)
                        {
                            //                            Ah_data[i+k*nbmap + (j+k*nbmap)*AhRows] += m_kinvis*HelmMat(bmap[i],bmap[j]);
                            Ah_data[i+k*nbmap + (j+k*nbmap)*AhRows] += m_kinvis*HelmMatScale*HelmMat_data[bmap[i] + HelmMatRows*bmap[j]];
                            A_no_adv_data[i+k*nbmap + (j+k*nbmap)*AhRows] += m_kinvis*HelmMatScale*HelmMat_data[bmap[i] + HelmMatRows*bmap[j]];
                        }
                        
                        for(j = 0; j < nimap; ++j)
                        {
                            B_data[i+k*nbmap + (j+k*nimap)*nbndry] += m_kinvis*HelmMatScale*HelmMat_data[bmap[i]+HelmMatRows*imap[j]];
                            B_no_adv_data[i+k*nbmap + (j+k*nimap)*nbndry] += m_kinvis*HelmMatScale*HelmMat_data[bmap[i]+HelmMatRows*imap[j]];
                        }
                    }
                    
                    if((lambda_imag != NekConstants::kNekUnsetDouble)&&(nz_loc == 2))
                    {
                        for(k = 0; k < nvel; ++k)
                        {
                            for(j = 0; j < nbmap; ++j)
                            {
                                Ah_data[i+2*k*nbmap + (j+(2*k+1)*nbmap)*AhRows] -= lambda_imag*(*MassMat)(bmap[i],bmap[j]);
                            }
                            
                            for(j = 0; j < nbmap; ++j)
                            {
                                Ah_data[i+(2*k+1)*nbmap + (j+2*k*nbmap)*AhRows] += lambda_imag*(*MassMat)(bmap[i],bmap[j]);
                            }
                            
                            for(j = 0; j < nimap; ++j)
                            {
                                B_data[i+2*k*nbmap + (j+(2*k+1)*nimap)*nbndry] -= lambda_imag*(*MassMat)(bmap[i],imap[j]);
                            }
                            
                            for(j = 0; j < nimap; ++j)
                            {
                                B_data[i+(2*k+1)*nbmap + (j+2*k*nimap)*nbndry] += lambda_imag*(*MassMat)(bmap[i],imap[j]);
                            }
                            
                        }
                    }
                    
                    
                    
                    for(k = 0; k < nvel; ++k)
                    {
                        if((nz_loc == 2)&&(k == 2)) // handle d/dz derivative
                        { 
                            NekDouble beta = -2*M_PI*HomogeneousMode/m_LhomZ;
                            
                            // Real Component
                            Vmath::Smul(npoints,beta,phys,1,deriv,1);
                            
                            m_pressure->GetExp(eid)->IProductWRTBase(deriv,pcoeffs);
                            Blas::Dcopy(psize,&(pcoeffs)[0],1,
                                        Dbnd->GetRawPtr() + 
                                        ((nz_loc*k+1)*bmap.num_elements()+i)*nsize_p[n],1);
                            
                            // Imaginary Component
                            Vmath::Neg(psize,pcoeffs,1);
                            Blas::Dcopy(psize,&(pcoeffs)[0],1,
                                        Dbnd->GetRawPtr() + 
                                        ((nz_loc*k)*bmap.num_elements()+i)*nsize_p[n]+psize,1);
                            
                            // now do advection terms
                            Vmath::Vmul(npoints, 
                                        Advtmp = Advfield[k] + phys_offset,
                                        1,deriv,1,tmpphys,1);
                            
                            locExp->IProductWRTBase(tmpphys,coeffs);
                            
                            
                            // real contribution
                            for(nv = 0; nv < nvel; ++nv)
                            {
                                for(j = 0; j < nbmap; ++j)
                                {
                                    Ah_data[j+2*nv*nbmap + (i+(2*nv+1)*nbmap)*AhRows] +=
                                    coeffs[bmap[j]];
                                }
                                
                                for(j = 0; j < nimap; ++j)
                                {
                                    C_data[i+(2*nv+1)*nbmap + (j+2*nv*nimap)*nbndry] += 
                                    coeffs[imap[j]];
                                }
                            }
                            
                            Vmath::Neg(ncoeffs,coeffs,1);
                            // imaginary contribution
                            for(nv = 0; nv < nvel; ++nv)
                            {
                                for(j = 0; j < nbmap; ++j)
                                {
                                    Ah_data[j+(2*nv+1)*nbmap + (i+2*nv*nbmap)*AhRows] +=
                                    coeffs[bmap[j]];
                                }
                                
                                for(j = 0; j < nimap; ++j)
                                {
                                    C_data[i+2*nv*nbmap + (j+(2*nv+1)*nimap)*nbndry] += 
                                    coeffs[imap[j]];
                                }
                            }
                        }
                        else
                        {
                            if(k < 2)
                            {
                                locExp->PhysDeriv(MultiRegions::DirCartesianMap[k],phys, deriv);
                                Vmath::Vmul(npoints, 
                                            Advtmp = Advfield[k] + phys_offset,
                                            1,deriv,1,tmpphys,1);
                                locExp->IProductWRTBase(tmpphys,coeffs);
                                
                                
                                for(nv = 0; nv < nvel*nz_loc; ++nv)
                                {
                                    for(j = 0; j < nbmap; ++j)
                                    {
                                        Ah_data[j+nv*nbmap + (i+nv*nbmap)*AhRows] += coeffs[bmap[j]];
                                        A_adv_data[j+nv*nbmap + (i+nv*nbmap)*AhRows] += coeffs[bmap[j]];
                                    }
                                    
                                    for(j = 0; j < nimap; ++j)
                                    {
                                        C_data[i+nv*nbmap + (j+nv*nimap)*nbndry] += coeffs[imap[j]];
                                        C_adv_data[i+nv*nbmap + (j+nv*nimap)*nbndry] += coeffs[imap[j]];
                                    }
                                }
                                
                                // copy into column major storage. 
                                m_pressure->GetExp(eid)->IProductWRTBase(deriv,pcoeffs);
                                for(j = 0; j < nz_loc; ++j)
                                {
                                    Blas::Dcopy(psize,&(pcoeffs)[0],1, Dbnd->GetRawPtr() + ((nz_loc*k+j)*bmap.num_elements() + i)*nsize_p[n]+ j*psize,1);
                                }
                            }
                        }
                        
                        if(IsLinearNSEquation)
                        {
                            for(nv = 0; nv < nvel; ++nv)
                            {
                                // u' . Grad U terms 
                                Vmath::Vmul(npoints,phys,1,
                                            AdvDeriv[k*nvel+nv],
                                            1,tmpphys,1);
                                locExp->IProductWRTBase(tmpphys,coeffs);
                                
                                for(int n1 = 0; n1 < nz_loc; ++n1)
                                {
                                    for(j = 0; j < nbmap; ++j)
                                    {
                                        Ah_data[j+(k*nz_loc+n1)*nbmap + 
                                        (i+(nv*nz_loc+n1)*nbmap)*AhRows] +=
                                        coeffs[bmap[j]];
                                        A_adv_data[j+(k*nz_loc+n1)*nbmap + 
                                        (i+(nv*nz_loc+n1)*nbmap)*AhRows] +=
                                        coeffs[bmap[j]];
                                    }
                                    
                                    for(j = 0; j < nimap; ++j)
                                    {
                                        C_data[i+(nv*nz_loc+n1)*nbmap + 
                                        (j+(k*nz_loc+n1)*nimap)*nbndry] += 
                                        coeffs[imap[j]];
                                        C_adv_data[i+(nv*nz_loc+n1)*nbmap + 
                                        (j+(k*nz_loc+n1)*nimap)*nbndry] += 
                                        coeffs[imap[j]];
                                    }
                                }
                            }                            
                        }
                    }
                }
                
                
                for(i = 0; i < nimap; ++i)
                {
                    // Fill element with mode
                    Vmath::Zero(ncoeffs,coeffs,1);
                    coeffs[imap[i]] = 1.0;
                    locExp->BwdTrans(coeffs,phys);
                    
                    for(k = 0; k < nvel*nz_loc; ++k)
                    {
                        for(j = 0; j < nbmap; ++j) // C set up as transpose
                        {
                            C_data[j+k*nbmap + (i+k*nimap)*nbndry] += m_kinvis*HelmMatScale*HelmMat_data[imap[i]+HelmMatRows*bmap[j]];
                            C_no_adv_data[j+k*nbmap + (i+k*nimap)*nbndry] += m_kinvis*HelmMatScale*HelmMat_data[imap[i]+HelmMatRows*bmap[j]];
                        }
                        
                        for(j = 0; j < nimap; ++j)
                        {
                            D_data[i+k*nimap + (j+k*nimap)*nint] += m_kinvis*HelmMatScale*HelmMat_data[imap[i]+HelmMatRows*imap[j]];
                            D_no_adv_data[i+k*nimap + (j+k*nimap)*nint] += m_kinvis*HelmMatScale*HelmMat_data[imap[i]+HelmMatRows*imap[j]];
		//	    cout << "writing D_data at location " << i+k*nimap + (j+k*nimap)*nint << " the value " << m_kinvis*HelmMatScale*HelmMat_data[imap[i]+HelmMatRows*imap[j]] << endl;
                        }
                    }
                    
                    if((lambda_imag != NekConstants::kNekUnsetDouble)&&(nz_loc == 2))
                    {
                        for(k = 0; k < nvel; ++k)
                        {
                            for(j = 0; j < nbmap; ++j) // C set up as transpose
                            {
                                C_data[j+2*k*nbmap + (i+(2*k+1)*nimap)*nbndry] += lambda_imag*(*MassMat)(bmap[j],imap[i]);
                            }
                            
                            for(j = 0; j < nbmap; ++j) // C set up as transpose
                            {
                                C_data[j+(2*k+1)*nbmap + (i+2*k*nimap)*nbndry] -= lambda_imag*(*MassMat)(bmap[j],imap[i]);
                            }
                            
                            for(j = 0; j < nimap; ++j)
                            {
                                D_data[i+2*k*nimap + (j+(2*k+1)*nimap)*nint] -= lambda_imag*(*MassMat)(imap[i],imap[j]);
                            }
                            
                            for(j = 0; j < nimap; ++j)
                            {
                                D_data[i+(2*k+1)*nimap + (j+2*k*nimap)*nint] += lambda_imag*(*MassMat)(imap[i],imap[j]);
                            }
                        }
                    }
                    
                    
                    for(k = 0; k < nvel; ++k)
                    {
                        if((nz_loc == 2)&&(k == 2)) // handle d/dz derivative
                        { 
                            NekDouble beta = -2*M_PI*HomogeneousMode/m_LhomZ;
                            
                            // Real Component
                            Vmath::Smul(npoints,beta,phys,1,deriv,1);
                            m_pressure->GetExp(eid)->IProductWRTBase(deriv,pcoeffs);
                            Blas::Dcopy(psize,&(pcoeffs)[0],1,
                                        Dint->GetRawPtr() + 
                                        ((nz_loc*k+1)*imap.num_elements()+i)*nsize_p[n],1);
                            // Imaginary Component
                            Vmath::Neg(psize,pcoeffs,1);
                            Blas::Dcopy(psize,&(pcoeffs)[0],1,
                                        Dint->GetRawPtr() + 
                                        ((nz_loc*k)*imap.num_elements()+i)*nsize_p[n]+psize,1);
                            
                            // Advfield[k] *d/dx_k to all velocity
                            // components on diagonal
                            Vmath::Vmul(npoints, Advtmp = Advfield[k] + phys_offset,1,deriv,1,tmpphys,1);
                            locExp->IProductWRTBase(tmpphys,coeffs);
                            
                            // Real Components
                            for(nv = 0; nv < nvel; ++nv)
                            {
                                for(j = 0; j < nbmap; ++j)
                                {
                                    B_data[j+2*nv*nbmap + (i+(2*nv+1)*nimap)*nbndry] += 
                                    coeffs[bmap[j]];
                                }
                                
                                for(j = 0; j < nimap; ++j)
                                {
                                    D_data[j+2*nv*nimap + (i+(2*nv+1)*nimap)*nint] += 
                                    coeffs[imap[j]];
                                }
                            }
                            Vmath::Neg(ncoeffs,coeffs,1);
                            // Imaginary 
                            for(nv = 0; nv < nvel; ++nv)
                            {
                                for(j = 0; j < nbmap; ++j)
                                {
                                    B_data[j+(2*nv+1)*nbmap + (i+2*nv*nimap)*nbndry] += 
                                    coeffs[bmap[j]];
                                }
                                
                                for(j = 0; j < nimap; ++j)
                                {
                                    D_data[j+(2*nv+1)*nimap + (i+2*nv*nimap)*nint] += 
                                    coeffs[imap[j]];
                                }
                            }
                            
                        }
                        else
                        {
                            if(k < 2)
                            {
                                // Differentiation & Inner product wrt base. 
                                //locExp->PhysDeriv(k,phys, deriv);
                                locExp->PhysDeriv(MultiRegions::DirCartesianMap[k],phys,deriv);
                                Vmath::Vmul(npoints, 
                                            Advtmp = Advfield[k] + phys_offset,
                                            1,deriv,1,tmpphys,1);
                                locExp->IProductWRTBase(tmpphys,coeffs);
                                
                                
                                for(nv = 0; nv < nvel*nz_loc; ++nv)
                                {
                                    for(j = 0; j < nbmap; ++j)
                                    {
                                        B_data[j+nv*nbmap + (i+nv*nimap)*nbndry] += coeffs[bmap[j]];
					B_adv_data[j+nv*nbmap + (i+nv*nimap)*nbndry] += coeffs[bmap[j]];
                                    }
                                    
                                    for(j = 0; j < nimap; ++j)
                                    {
                                        D_data[j+nv*nimap + (i+nv*nimap)*nint] += coeffs[imap[j]];
                                        D_adv_data[j+nv*nimap + (i+nv*nimap)*nint] += coeffs[imap[j]];
	    //                            cout << "writing D_data advec at location " << j+nv*nimap + (i+nv*nimap)*nint << " the value " << coeffs[imap[j]] << endl;
                                    }
                                }
                                // copy into column major storage. 
                                m_pressure->GetExp(eid)->IProductWRTBase(deriv,pcoeffs);
                                for(j = 0; j < nz_loc; ++j)
                                {
                                    Blas::Dcopy(psize,&(pcoeffs)[0],1,
                                                Dint->GetRawPtr() + 
                                                ((nz_loc*k+j)*imap.num_elements() + i)*nsize_p[n]+j*psize,1);
                                }
                            }
                        }
                        
                        if(IsLinearNSEquation)
                        {
                            int n1;
                            for(nv = 0; nv < nvel; ++nv)
                            {
                                // u'.Grad U terms 
                                Vmath::Vmul(npoints,phys,1, 
                                            AdvDeriv[k*nvel+nv],
                                            1,tmpphys,1);
                                locExp->IProductWRTBase(tmpphys,coeffs);
                                
                                for(n1 = 0; n1 < nz_loc; ++n1)
                                {
                                    for(j = 0; j < nbmap; ++j)
                                    {
                                        B_data[j+(k*nz_loc+n1)*nbmap + 
                                        (i+(nv*nz_loc+n1)*nimap)*nbndry] += 
                                        coeffs[bmap[j]];
                                        B_adv_data[j+(k*nz_loc+n1)*nbmap + 
                                        (i+(nv*nz_loc+n1)*nimap)*nbndry] += 
                                        coeffs[bmap[j]];
                                    }
                                    
                                    for(j = 0; j < nimap; ++j)
                                    {
                                        D_data[j+(k*nz_loc+n1)*nimap +  
                                        (i+(nv*nz_loc+n1)*nimap)*nint] += 
                                        coeffs[imap[j]];
                                        D_adv_data[j+(k*nz_loc+n1)*nimap +  
                                        (i+(nv*nz_loc+n1)*nimap)*nint] += 
                                        coeffs[imap[j]];
                                    }
                                }
                            }
                        }
                    }
                }
                

		// from here starts the multilevel static condensation
		// so collect data for ROM
		

/*                cout << "D->GetRows() " << D->GetRows() << std::endl ;
                cout << "D->GetColumns() " << D->GetColumns() << std::endl ;
                cout << "Dbnd->GetRows() " << Dbnd->GetRows() << std::endl ;
                cout << "Dbnd->GetColumns() " << Dbnd->GetColumns() << std::endl ;
                cout << "Dint->GetRows() " << Dint->GetRows() << std::endl ;
                cout << "Dint->GetColumns() " << Dint->GetColumns() << std::endl ;
                cout << "B->GetRows() " << B->GetRows() << std::endl ;
                cout << "B->GetColumns() " << B->GetColumns() << std::endl ;
                cout << "C->GetRows() " << C->GetRows() << std::endl ;
                cout << "C->GetColumns() " << C->GetColumns() << std::endl ;
                cout << "Ah->GetRows() " << Ah->GetRows() << std::endl ;
                cout << "Ah->GetColumns() " << Ah->GetColumns() << std::endl ; */

//		Array<OneD, Array<OneD, NekDouble> > A_save = Ah;           
//		DNekMatSharedPtr D_save = D;
//     		DNekMat D_save1 = *D_save;
//		Array<OneD, Array<OneD, NekDouble> > D_save2 = D_save1;

		Eigen::MatrixXd D_Matrix(D->GetRows(), D->GetColumns());
		Eigen::MatrixXd D_adv_Matrix(D_adv->GetRows(), D_adv->GetColumns());
		Eigen::MatrixXd D_no_adv_Matrix(D_no_adv->GetRows(), D_no_adv->GetColumns());
		Eigen::MatrixXd Dbnd_Matrix(Dbnd->GetRows(), Dbnd->GetColumns());
		Eigen::MatrixXd Dint_Matrix(Dint->GetRows(), Dint->GetColumns());
		Eigen::MatrixXd B_Matrix(B->GetRows(), B->GetColumns());
		Eigen::MatrixXd B_adv_Matrix(B_adv->GetRows(), B_adv->GetColumns());
		Eigen::MatrixXd B_no_adv_Matrix(B_no_adv->GetRows(), B_no_adv->GetColumns());
		Eigen::MatrixXd C_Matrix(C->GetRows(), C->GetColumns());
		Eigen::MatrixXd C_adv_Matrix(C_adv->GetRows(), C_adv->GetColumns());
		Eigen::MatrixXd C_no_adv_Matrix(C_no_adv->GetRows(), C_no_adv->GetColumns());
		Eigen::MatrixXd A_Matrix(Ah->GetRows() - 1, Ah->GetColumns() - 1);
		Eigen::MatrixXd A_adv_Matrix(A_adv->GetRows() - 1, A_adv->GetColumns() - 1);
		Eigen::MatrixXd A_no_adv_Matrix(A_no_adv->GetRows() - 1, A_no_adv->GetColumns() - 1);

//		Eigen::VectorXd v(m_equ[0]->GetNpoints());

		for (int i_eig_dof = 0; i_eig_dof < D->GetRows(); i_eig_dof++)
		{
			for (int j_eig_dof = 0; j_eig_dof < D->GetColumns(); j_eig_dof++)
			{
				D_Matrix(i_eig_dof,j_eig_dof) = (*D)(i_eig_dof,j_eig_dof);
				D_adv_Matrix(i_eig_dof,j_eig_dof) = (*D_adv)(i_eig_dof,j_eig_dof);
				D_no_adv_Matrix(i_eig_dof,j_eig_dof) = (*D_no_adv)(i_eig_dof,j_eig_dof);    
			}
		}

		for (int i_eig_dof = 0; i_eig_dof < Dbnd->GetRows(); i_eig_dof++)
		{
			for (int j_eig_dof = 0; j_eig_dof < Dbnd->GetColumns(); j_eig_dof++)
			{
				Dbnd_Matrix(i_eig_dof,j_eig_dof) = (*Dbnd)(i_eig_dof,j_eig_dof); 
			}
		}

		for (int i_eig_dof = 0; i_eig_dof < Dint->GetRows(); i_eig_dof++)
		{
			for (int j_eig_dof = 0; j_eig_dof < Dint->GetColumns(); j_eig_dof++)
			{
				Dint_Matrix(i_eig_dof,j_eig_dof) = (*Dint)(i_eig_dof,j_eig_dof); 
			}
		}

		for (int i_eig_dof = 0; i_eig_dof < B->GetRows(); i_eig_dof++)
		{
			for (int j_eig_dof = 0; j_eig_dof < B->GetColumns(); j_eig_dof++)
			{
				B_Matrix(i_eig_dof,j_eig_dof) = (*B)(i_eig_dof,j_eig_dof);
				B_adv_Matrix(i_eig_dof,j_eig_dof) = (*B_adv)(i_eig_dof,j_eig_dof); 
				B_no_adv_Matrix(i_eig_dof,j_eig_dof) = (*B_no_adv)(i_eig_dof,j_eig_dof);  
			}
		}

		for (int i_eig_dof = 0; i_eig_dof < C->GetRows(); i_eig_dof++)
		{
			for (int j_eig_dof = 0; j_eig_dof < C->GetColumns(); j_eig_dof++)
			{
				C_Matrix(i_eig_dof,j_eig_dof) = (*C)(i_eig_dof,j_eig_dof);
				C_adv_Matrix(i_eig_dof,j_eig_dof) = (*C_adv)(i_eig_dof,j_eig_dof);
				C_no_adv_Matrix(i_eig_dof,j_eig_dof) = (*C_no_adv)(i_eig_dof,j_eig_dof);    
			}
		}

		for (int i_eig_dof = 0; i_eig_dof < Ah->GetRows() - 1; i_eig_dof++)
		{
			for (int j_eig_dof = 0; j_eig_dof < Ah->GetColumns() - 1; j_eig_dof++)
			{
				A_Matrix(i_eig_dof,j_eig_dof) = (*Ah)(i_eig_dof,j_eig_dof);
				A_adv_Matrix(i_eig_dof,j_eig_dof) = (*A_adv)(i_eig_dof,j_eig_dof);
				A_no_adv_Matrix(i_eig_dof,j_eig_dof) = (*A_no_adv)(i_eig_dof,j_eig_dof);    
			}
		}





//		cout << D_no_adv_Matrix << endl;
		// would also need to keep the advection and convection parts seperate

//		A_all.block(n*nsize_bndry[0], n*nsize_bndry[0], nsize_bndry[0], nsize_bndry[0]) = *Ah;  // can I do that directly ? no, probably not
		A_all.block(n*nsize_bndry[0], n*nsize_bndry[0], nsize_bndry[0], nsize_bndry[0]) = A_Matrix;
		A_adv_all.block(n*nsize_bndry[0], n*nsize_bndry[0], nsize_bndry[0], nsize_bndry[0]) = A_adv_Matrix;
		A_no_adv_all.block(n*nsize_bndry[0], n*nsize_bndry[0], nsize_bndry[0], nsize_bndry[0]) = A_no_adv_Matrix;

		B_all.block(n*nsize_bndry[0], n*nsize_int[0], nsize_bndry[0], nsize_int[0]) = B_Matrix;
		B_adv_all.block(n*nsize_bndry[0], n*nsize_int[0], nsize_bndry[0], nsize_int[0]) = B_adv_Matrix;
		B_no_adv_all.block(n*nsize_bndry[0], n*nsize_int[0], nsize_bndry[0], nsize_int[0]) = B_no_adv_Matrix;

		C_all.block(n*nsize_bndry[0], n*nsize_int[0], nsize_bndry[0], nsize_int[0]) = C_Matrix;
		C_adv_all.block(n*nsize_bndry[0], n*nsize_int[0], nsize_bndry[0], nsize_int[0]) = C_adv_Matrix;
		C_no_adv_all.block(n*nsize_bndry[0], n*nsize_int[0], nsize_bndry[0], nsize_int[0]) = C_no_adv_Matrix;

		D_all.block(n*nsize_int[0], n*nsize_int[0], nsize_int[0], nsize_int[0]) = D_Matrix;
		D_adv_all.block(n*nsize_int[0], n*nsize_int[0], nsize_int[0], nsize_int[0]) = D_adv_Matrix;
		D_no_adv_all.block(n*nsize_int[0], n*nsize_int[0], nsize_int[0], nsize_int[0]) = D_no_adv_Matrix;

		Dbnd_all.block(n*nsize_p[0], n*nsize_bndry[0], nsize_p[0], nsize_bndry[0]) = Dbnd_Matrix;
		Dint_all.block(n*nsize_p[0], n*nsize_int[0], nsize_p[0], nsize_int[0]) = Dint_Matrix;


/*		sing_B[curr_elem*nsize_bndry:curr_elem*nsize_bndry+nsize_bndry, curr_elem*nsize_int:curr_elem*nsize_int+nsize_int] = B_elem[curr_elem,:,:]
		sing_Btilde[curr_elem*nsize_bndry:curr_elem*nsize_bndry+nsize_bndry, curr_elem*nsize_int:curr_elem*nsize_int+nsize_int] = C_elem[curr_elem,:,:]
		sing_C[curr_elem*nsize_int:curr_elem*nsize_int+nsize_int, curr_elem*nsize_int:curr_elem*nsize_int+nsize_int] = D_elem[curr_elem,:,:]
		sing_Dbnd[curr_elem*nsize_p:curr_elem*nsize_p+nsize_p, curr_elem*nsize_bndry:curr_elem*nsize_bndry+nsize_bndry] = Dbnd_elem[curr_elem,:,:]
		sing_Dint[curr_elem*nsize_p:curr_elem*nsize_p+nsize_p, curr_elem*nsize_int:curr_elem*nsize_int+nsize_int] = Dint_elem[curr_elem,:,:] */


			// need my mats for projection purposes: think about: have no splitting bnd/p/int could be beneficial for number of affine terms
		/*	Eigen::MatrixXd D_all( nsize_int*nel , nsize_int*nel );
			Eigen::MatrixXd Dbnd_all( nsize_p*nel , nsize_bndry*nel );
			Eigen::MatrixXd Dint_all( nsize_p*nel , nsize_int*nel );
			Eigen::MatrixXd B_all( nsize_bndry*nel , nsize_int*nel );
			Eigen::MatrixXd C_all( nsize_bndry*nel , nsize_int*nel );
			Eigen::MatrixXd A_all( nsize_bndry*nel , nsize_bndry*nel );*/




            for(int n1 = 0; n1 < D->GetColumns(); ++n1)
            {
                for(int n2 = 0; n2 < D->GetRows(); ++n2)
                {
                    
//		    cout << "(*D)(n1,n2) " << (*D)(n1,n2) << std::endl ;

                }
            }

	




                D->Invert();
                (*B) = (*B)*(*D);
                
                
                // perform (*Ah) = (*Ah) - (*B)*(*C) but since size of
                // Ah is larger than (*B)*(*C) easier to call blas
                // directly
                Blas::Dgemm('N','T', B->GetRows(), C->GetRows(), 
                            B->GetColumns(), -1.0, B->GetRawPtr(),
                            B->GetRows(), C->GetRawPtr(), 
                            C->GetRows(), 1.0, 
                            Ah->GetRawPtr(), Ah->GetRows());
            }  



            mat.m_BCinv->SetBlock(n,n,loc_mat = MemoryManager<DNekScalMat>::AllocateSharedPtr(one,B));
            mat.m_Btilde->SetBlock(n,n,loc_mat = MemoryManager<DNekScalMat>::AllocateSharedPtr(one,C));
            mat.m_Cinv->SetBlock(n,n,loc_mat = MemoryManager<DNekScalMat>::AllocateSharedPtr(one,D));
            mat.m_D_bnd->SetBlock(n,n,loc_mat = MemoryManager<DNekScalMat>::AllocateSharedPtr(one, Dbnd));
            mat.m_D_int->SetBlock(n,n,loc_mat = MemoryManager<DNekScalMat>::AllocateSharedPtr(one, Dint));
            
            // Do matrix manipulations and get final set of block matries    
            // reset boundary to put mean mode into boundary system. 
            
            DNekMatSharedPtr Cinv,BCinv,Btilde; 
            DNekMat  DintCinvDTint, BCinvDTint_m_DTbnd, DintCinvBTtilde_m_Dbnd;
            
            Cinv   = D;
            BCinv  = B;  
            Btilde = C; 
            
            DintCinvDTint      = (*Dint)*(*Cinv)*Transpose(*Dint);
            BCinvDTint_m_DTbnd = (*BCinv)*Transpose(*Dint) - Transpose(*Dbnd);
            
            // This could be transpose of BCinvDint in some cases
            DintCinvBTtilde_m_Dbnd = (*Dint)*(*Cinv)*Transpose(*Btilde) - (*Dbnd); 
            
            // Set up final set of matrices. 
            DNekMatSharedPtr Bh = MemoryManager<DNekMat>::AllocateSharedPtr(nsize_bndry_p1[n],nsize_p_m1[n],zero);
            DNekMatSharedPtr Ch = MemoryManager<DNekMat>::AllocateSharedPtr(nsize_p_m1[n],nsize_bndry_p1[n],zero);
            DNekMatSharedPtr Dh = MemoryManager<DNekMat>::AllocateSharedPtr(nsize_p_m1[n], nsize_p_m1[n],zero);
            Array<OneD, NekDouble> Dh_data = Dh->GetPtr();
            
            // Copy matrices into final structures. 
            int n1,n2;
            for(n1 = 0; n1 < nz_loc; ++n1)
            {
                for(i = 0; i < psize-1; ++i)
                {
                    for(n2 = 0; n2 < nz_loc; ++n2)
                    {
                        for(j = 0; j < psize-1; ++j)
                        {
                            //(*Dh)(i+n1*(psize-1),j+n2*(psize-1)) = 
                            //-DintCinvDTint(i+1+n1*psize,j+1+n2*psize);
                            Dh_data[(i+n1*(psize-1)) + (j+n2*(psize-1))*Dh->GetRows()] = 
                            -DintCinvDTint(i+1+n1*psize,j+1+n2*psize);
                        }
                    }                    
                }
            }
            
            for(n1 = 0; n1 < nz_loc; ++n1)
            {
                for(i = 0; i < nsize_bndry_p1[n]-nz_loc; ++i)
                {
                    (*Ah)(i,nsize_bndry_p1[n]-nz_loc+n1) = BCinvDTint_m_DTbnd(i,n1*psize);
                    (*Ah)(nsize_bndry_p1[n]-nz_loc+n1,i) = DintCinvBTtilde_m_Dbnd(n1*psize,i);
                }
            }
            
            for(n1 = 0; n1 < nz_loc; ++n1)
            {
                for(n2 = 0; n2 < nz_loc; ++n2)
                {
                    (*Ah)(nsize_bndry_p1[n]-nz_loc+n1,nsize_bndry_p1[n]-nz_loc+n2) = 
                    -DintCinvDTint(n1*psize,n2*psize);
                }
            }
            
            for(n1 = 0; n1 < nz_loc; ++n1)
            {
                for(j = 0; j < psize-1; ++j)
                {
                    for(i = 0; i < nsize_bndry_p1[n]-nz_loc; ++i)
                    {
                        (*Bh)(i,j+n1*(psize-1)) = BCinvDTint_m_DTbnd(i,j+1+n1*psize);
                        (*Ch)(j+n1*(psize-1),i) = DintCinvBTtilde_m_Dbnd(j+1+n1*psize,i);
                    }
                }
            }
            
            for(n1 = 0; n1 < nz_loc; ++n1)
            {
                for(n2 = 0; n2 < nz_loc; ++n2)
                {
                    for(j = 0; j < psize-1; ++j)
                    {
                        (*Bh)(nsize_bndry_p1[n]-nz_loc+n1,j+n2*(psize-1)) = -DintCinvDTint(n1*psize,j+1+n2*psize);
                        (*Ch)(j+n2*(psize-1),nsize_bndry_p1[n]-nz_loc+n1) = -DintCinvDTint(j+1+n2*psize,n1*psize);
                    }
                }
            }
            
            // Do static condensation
            Dh->Invert();
            (*Bh) = (*Bh)*(*Dh);
            //(*Ah) = (*Ah) - (*Bh)*(*Ch);
            Blas::Dgemm('N','N', Bh->GetRows(), Ch->GetColumns(), Bh->GetColumns(), -1.0,
                        Bh->GetRawPtr(), Bh->GetRows(), Ch->GetRawPtr(), Ch->GetRows(), 
                        1.0, Ah->GetRawPtr(), Ah->GetRows());
            
            
	    for(int n1 = 0; n1 < Ah->GetColumns(); ++n1)
            {
                for(int n2 = 0; n2 < Ah->GetRows(); ++n2)
                {
                    
  //                  cout << "D->GetRows() " << D->GetRows() << std::endl ;
  //                  cout << "D->GetColumns() " << D->GetColumns() << std::endl ;
//		    if (n == 5) { cout << "(*Ah)(n1,n2) " << (*Ah)(n1,n2) << std::endl ; }

                }
            }


            // Set matrices for later inversion. Probably do not need to be 
            // attached to class
            pAh->SetBlock(n,n,loc_mat = MemoryManager<DNekScalMat>::AllocateSharedPtr(one,Ah));
            pBh->SetBlock(n,n,loc_mat = MemoryManager<DNekScalMat>::AllocateSharedPtr(one,Bh));
            pCh->SetBlock(n,n,loc_mat = MemoryManager<DNekScalMat>::AllocateSharedPtr(one,Ch));
            pDh->SetBlock(n,n,loc_mat = MemoryManager<DNekScalMat>::AllocateSharedPtr(one,Dh));    
        }
        timer.Stop();
//        cout << "Matrix Setup Costs: " << timer.TimePerTest(1) << endl;
        
	// end of the loop over the spectral elements
            
	    RB_A = A_all; // das ist ein bisschen doppelt, die RB mats sind im .h header definiert, die _all mats hier
	    RB_A_adv = A_adv_all;
	    RB_A_no_adv = A_no_adv_all;
	    RB_B = B_all;
	    RB_B_adv = B_adv_all;
	    RB_B_no_adv = B_no_adv_all;
	    RB_C = C_all;
	    RB_C_adv = C_adv_all;
	    RB_C_no_adv = C_no_adv_all;
	    RB_D = D_all;
	    RB_D_adv = D_adv_all;
	    RB_D_no_adv = D_no_adv_all;
	    RB_Dbnd = Dbnd_all;
	    RB_Dint = Dint_all;

//	cout << D_all << endl;

/*	cout << "current m_kinvis " << m_kinvis << endl;
	cout << "RB_A.norm() " << RB_A.norm() << endl;
	cout << "RB_A_adv.norm() " << RB_A_adv.norm() << endl;
	cout << "RB_A_no_adv.norm() " << RB_A_no_adv.norm() << endl;
	cout << "(RB_A - RB_A_adv - RB_A_no_adv).norm() " << (RB_A - RB_A_adv - RB_A_no_adv).norm() << endl;
	cout << "RB_B.norm() " << RB_B.norm() << endl;
	cout << "RB_B_adv.norm() " << RB_B_adv.norm() << endl;
	cout << "RB_B_no_adv.norm() " << RB_B_no_adv.norm() << endl;
	cout << "(RB_B - RB_B_adv - RB_B_no_adv).norm() " << (RB_B - RB_B_adv - RB_B_no_adv).norm() << endl;
	cout << "RB_C.norm() " << RB_C.norm() << endl;
	cout << "RB_C_adv.norm() " << RB_C_adv.norm() << endl;
	cout << "RB_C_no_adv.norm() " << RB_C_no_adv.norm() << endl;
	cout << "(RB_C - RB_C_adv - RB_C_no_adv).norm() " << (RB_C - RB_C_adv - RB_C_no_adv).norm() << endl;
	cout << "RB_D.norm() " << RB_D.norm() << endl;
	cout << "RB_D_adv.norm() " << RB_D_adv.norm() << endl;
	cout << "RB_D_no_adv.norm() " << RB_D_no_adv.norm() << endl;
	cout << "(RB_D - RB_D_adv - RB_D_no_adv).norm() " << (RB_D - RB_D_adv - RB_D_no_adv).norm() << endl;
*/
//        cout << RB_D << endl;
//        cout << RB_D_adv << endl;
//        cout << RB_D_no_adv << endl;
        
        timer.Start();
        // Set up global coupled boundary solver. 
        // This is a key to define the solution matrix type
        // currently we are giving it a argument of eLInearAdvectionReaction 
        // since this then makes the matrix storage of type eFull
        MultiRegions::GlobalLinSysKey key(StdRegions::eLinearAdvectionReaction,locToGloMap);
        mat.m_CoupledBndSys = MemoryManager<MultiRegions::GlobalLinSysDirectStaticCond>::AllocateSharedPtr(key,m_fields[0],pAh,pBh,pCh,pDh,locToGloMap);
        mat.m_CoupledBndSys->Initialise(locToGloMap);
        timer.Stop();
//        cout << "Multilevel condensation: " << timer.TimePerTest(1) << endl;
    }


    void CoupledLinearNS_TT::Solve(void)
    {

        const unsigned int ncmpt = m_velocity.num_elements();
        Array<OneD, Array<OneD, NekDouble> > forcing_phys(ncmpt);
        Array<OneD, Array<OneD, NekDouble> > forcing     (ncmpt);

        for(int i = 0; i < ncmpt; ++i)
        {
            forcing_phys[i] = Array<OneD, NekDouble> (m_fields[m_velocity[0]]->GetNpoints(), 0.0);
            forcing[i]      = Array<OneD, NekDouble> (m_fields[m_velocity[0]]->GetNcoeffs(),0.0);
        }

        std::vector<SolverUtils::ForcingSharedPtr>::const_iterator x;
        for (x = m_forcing.begin(); x != m_forcing.end(); ++x)
        {
            const NekDouble time = 0;
            (*x)->Apply(m_fields, forcing_phys, forcing_phys, time);
        }
        for (unsigned int i = 0; i < ncmpt; ++i)
        {
            bool waveSpace = m_fields[m_velocity[i]]->GetWaveSpace();
            m_fields[i]->SetWaveSpace(true);
            m_fields[i]->IProductWRTBase(forcing_phys[i], forcing[i]);
            m_fields[i]->SetWaveSpace(waveSpace);
        }

        SolveLinearNS(forcing);

    }

    void CoupledLinearNS_TT::SolveLinearNS(const Array<OneD, Array<OneD, NekDouble> > &forcing)
    {
        int i,n;
        Array<OneD,  MultiRegions::ExpListSharedPtr> vel_fields(m_velocity.num_elements());
        Array<OneD, Array<OneD, NekDouble> > force(m_velocity.num_elements());
        
        if(m_HomogeneousType == eHomogeneous1D)
        {
            int ncoeffsplane = m_fields[m_velocity[0]]->GetPlane(0)->GetNcoeffs();
            for(n = 0; n < m_npointsZ/2; ++n)
            {
                // Get the a Fourier mode of velocity and forcing components. 
                for(i = 0; i < m_velocity.num_elements(); ++i)
                {
                    vel_fields[i] = m_fields[m_velocity[i]]->GetPlane(2*n);
                    // Note this needs to correlate with how we pass forcing
                    force[i] = forcing[i] + 2*n*ncoeffsplane;
                }
                
                SolveLinearNS(force,vel_fields,m_pressure->GetPlane(2*n),n);
            }
            for(i = 0; i < m_velocity.num_elements(); ++i)
            {
                m_fields[m_velocity[i]]->SetPhysState(false);
            }
            m_pressure->SetPhysState(false);
        }
        else
        {
            for(i = 0; i < m_velocity.num_elements(); ++i)
            {
                vel_fields[i] = m_fields[m_velocity[i]];
                // Note this needs to correlate with how we pass forcing
                force[i] = forcing[i];
            }
            SolveLinearNS(force,vel_fields,m_pressure);
        }
    }
    
    void CoupledLinearNS_TT::SolveLinearNS(const Array<OneD, Array<OneD, NekDouble> > &forcing,  Array<OneD, MultiRegions::ExpListSharedPtr> &fields, MultiRegions::ExpListSharedPtr &pressure,  const int mode)
    {
        int i,j,k,n,eid,cnt,cnt1;
        int nbnd,nint,offset;
        int nvel = m_velocity.num_elements();
        int nel  = fields[0]->GetNumElmts();
        Array<OneD, unsigned int> bmap, imap; 
        
        Array<OneD, NekDouble > f_bnd(m_mat[mode].m_BCinv->GetRows());
        NekVector< NekDouble  > F_bnd(f_bnd.num_elements(), f_bnd, eWrapper);
        Array<OneD, NekDouble > f_int(m_mat[mode].m_BCinv->GetColumns());
        NekVector< NekDouble  > F_int(f_int.num_elements(),f_int, eWrapper);
        
        int nz_loc;
        int  nplanecoeffs = fields[m_velocity[0]]->GetNcoeffs();// this is fine since we pass the nplane coeff data. 
        
        if(mode) // Homogeneous mode flag
        {
            nz_loc = 2;
        }
        else
        {
            if(m_singleMode)
            {
                nz_loc = 2;
            }
            else
            {
                nz_loc = 1;
                if(m_HomogeneousType == eHomogeneous1D)
                {
                    // Zero fields to set complex mode to zero;
                    for(i = 0; i < fields.num_elements(); ++i)
                    {
                        Vmath::Zero(2*fields[i]->GetNcoeffs(),fields[i]->UpdateCoeffs(),1);
                    }
                    Vmath::Zero(2*pressure->GetNcoeffs(),pressure->UpdateCoeffs(),1);
                }
            }
        }

	// do need to write the forcing vectors
  /*        ofstream myfilef0 ("forcing0.txt");
	  if (myfilef0.is_open())
	  {
		for (int counter_row = 0; counter_row < forcing[0].num_elements(); ++counter_row)
		{
				myfilef0 << std::setprecision(17) << forcing[0][counter_row] << " ";
//				cout << "writing " << collected_trajectory_f1[counter_timesteps][counter_velocity] << " ";
			myfilef0 << "\n";
		}
                myfilef0.close();
	  }
	  else cout << "Unable to open file";

          ofstream myfilef1 ("forcing1.txt");
	  if (myfilef1.is_open())
	  {
		for (int counter_row = 0; counter_row < forcing[1].num_elements(); ++counter_row)
		{
				myfilef1 << std::setprecision(17) << forcing[1][counter_row] << " ";
//				cout << "writing " << collected_trajectory_f1[counter_timesteps][counter_velocity] << " ";
			myfilef1 << "\n";
		}
                myfilef1.close();
	  }
	  else cout << "Unable to open file";
*/        

        // Assemble f_bnd and f_int
        cnt = cnt1 = 0;
        for(i = 0; i < nel; ++i) // loop over elements
        {
            eid = fields[m_velocity[0]]->GetOffset_Elmt_Id(i);
            fields[m_velocity[0]]->GetExp(eid)->GetBoundaryMap(bmap);
            fields[m_velocity[0]]->GetExp(eid)->GetInteriorMap(imap);
            nbnd   = bmap.num_elements();
            nint   = imap.num_elements();
            offset = fields[m_velocity[0]]->GetCoeff_Offset(eid);
            
            for(j = 0; j < nvel; ++j) // loop over velocity fields 
            {
                for(n = 0; n < nz_loc; ++n)
                {
                    for(k = 0; k < nbnd; ++k)
                    {
                        f_bnd[cnt+k] = forcing[j][n*nplanecoeffs + 
                        offset+bmap[k]];
                    }
                    for(k = 0; k < nint; ++k)
                    {
                        f_int[cnt1+k] = forcing[j][n*nplanecoeffs + 
                        offset+imap[k]];
                    }
                    cnt  += nbnd;
                    cnt1 += nint;
                }
            }
        }
        
        Array<OneD, NekDouble > f_p(m_mat[mode].m_D_int->GetRows());
        NekVector<  NekDouble > F_p(f_p.num_elements(),f_p,eWrapper);
        NekVector<  NekDouble > F_p_tmp(m_mat[mode].m_Cinv->GetRows());
        
        // fbnd does not currently hold the pressure mean
        F_bnd = F_bnd - (*m_mat[mode].m_BCinv)*F_int;
        F_p_tmp = (*m_mat[mode].m_Cinv)*F_int;
        F_p = (*m_mat[mode].m_D_int) * F_p_tmp;
        
        // construct inner forcing 
        Array<OneD, NekDouble > bnd   (m_locToGloMap[mode]->GetNumGlobalCoeffs(),0.0);
        Array<OneD, NekDouble > fh_bnd(m_locToGloMap[mode]->GetNumGlobalCoeffs(),0.0);
        
        const Array<OneD,const int>& loctoglomap
        = m_locToGloMap[mode]->GetLocalToGlobalMap();
        const Array<OneD,const NekDouble>& loctoglosign
        = m_locToGloMap[mode]->GetLocalToGlobalSign();
        
/*          ofstream myfile1LocGloMap ("LocGloMap.txt");
	  if (myfile1LocGloMap.is_open())
	  {
		for (int counter = 0; counter < loctoglomap.num_elements(); ++counter)
		{
			myfile1LocGloMap << std::setprecision(17) << loctoglomap[counter] << " ";
			myfile1LocGloMap << "\n";
		}
                myfile1LocGloMap.close();
	  }
	  else cout << "Unable to open file";

          ofstream myfile2LocGloSign ("LocGloSign.txt");
	  if (myfile2LocGloSign.is_open())
	  {
		for (int counter = 0; counter < loctoglosign.num_elements(); ++counter)
		{
			myfile2LocGloSign << std::setprecision(17) << loctoglosign[counter] << " ";
			myfile2LocGloSign << "\n";
		}
                myfile2LocGloSign.close();
	  }
	  else cout << "Unable to open file";

        const Array<OneD,const int>& loctoglobndmap
        = m_locToGloMap[mode]->GetLocalToGlobalBndMap();
        const Array<OneD,const NekDouble>& loctoglobndsign
        = m_locToGloMap[mode]->GetLocalToGlobalBndSign();

          ofstream myfile1LocGloBndMap ("LocGloBndMap.txt");
	  if (myfile1LocGloBndMap.is_open())
	  {
		for (int counter = 0; counter < loctoglobndmap.num_elements(); ++counter)
		{
			myfile1LocGloBndMap << std::setprecision(17) << loctoglobndmap[counter] << " ";
			myfile1LocGloBndMap << "\n";
		}
                myfile1LocGloBndMap.close();
	  }
	  else cout << "Unable to open file";

          ofstream myfile2LocGloBndSign ("LocGloBndSign.txt");
	  if (myfile2LocGloBndSign.is_open())
	  {
		for (int counter = 0; counter < loctoglobndsign.num_elements(); ++counter)
		{
			myfile2LocGloBndSign << std::setprecision(17) << loctoglobndsign[counter] << " ";
			myfile2LocGloBndSign << "\n";
		}
                myfile2LocGloBndSign.close();
	  }
	  else cout << "Unable to open file";
*/
   /*         Array<OneD, Array<OneD, NekDouble> > glo_dof_to_phys_collector(number_of_global_dofs);
            Array<OneD, Array<OneD, NekDouble> > phys_to_glo_dof_collector(m_fields[0]->GetNpoints());
            Array<OneD, Array<OneD, NekDouble> > glo_dof_to_phys_collector_p(number_of_global_dofs);
            Array<OneD, Array<OneD, NekDouble> > phys_to_glo_dof_collector_p(pressure_field->GetNpoints());

	    // get a global_dof <-> phys map
	    for (int i_global_dofs = 0; i_global_dofs < number_of_global_dofs; i_global_dofs++)
	    {
              Array<OneD, NekDouble> init_global_test_field(number_of_global_dofs,0.0);
              Array<OneD, NekDouble> test_field(m_fields[0]->GetNpoints(),0.0);
              init_global_test_field[i_global_dofs] = 1.0;
              m_fields[0]->GlobalToLocal(init_global_test_field, local_test_field);
              m_fields[0]->BwdTrans(local_test_field, test_field);
	      glo_dof_to_phys_collector[i_global_dofs] = test_field;
	    } */

        offset = cnt = 0; 
        for(i = 0; i < nel; ++i)
        {
            eid  = fields[0]->GetOffset_Elmt_Id(i);
            nbnd = nz_loc*fields[0]->GetExp(eid)->NumBndryCoeffs(); 
            
            for(j = 0; j < nvel; ++j)
            {
                for(k = 0; k < nbnd; ++k)
                {
                    fh_bnd[loctoglomap[offset+j*nbnd+k]] += 
                    loctoglosign[offset+j*nbnd+k]*f_bnd[cnt+k];
                }
                cnt += nbnd;
            }
            
            nint    = pressure->GetExp(eid)->GetNcoeffs();
            offset += nvel*nbnd + nint*nz_loc; 
        }
        
        offset = cnt1 = 0; 
        for(i = 0; i <  nel; ++i)
        {
            eid  = fields[0]->GetOffset_Elmt_Id(i);
            nbnd = nz_loc*fields[0]->GetExp(eid)->NumBndryCoeffs(); 
            nint = pressure->GetExp(eid)->GetNcoeffs(); 
            
            for(n = 0; n < nz_loc; ++n)
            {
                for(j = 0; j < nint; ++j)
                {
                    fh_bnd[loctoglomap[offset + nvel*nbnd + n*nint+j]] = f_p[cnt1+j];
                }
                cnt1   += nint;
            }
            offset += nvel*nbnd + nz_loc*nint; 
        }
        
        //  Set Weak BC into f_bnd and Dirichlet Dofs in bnd
        const Array<OneD,const int>& bndmap
        = m_locToGloMap[mode]->GetBndCondCoeffsToGlobalCoeffsMap();
        
/*          ofstream myfilebndmap ("BndCondCoeffsToGlobalCoeffsMap.txt");
	  if (myfilebndmap.is_open())
	  {
		for (int counter = 0; counter < bndmap.num_elements(); ++counter)
		{
			{
				myfilebndmap << std::setprecision(17) << bndmap[counter] << " ";
			}
		}
                myfilebndmap.close();
	  }
	  else cout << "Unable to open file";   
*/
        // Forcing function with weak boundary conditions and
        // Dirichlet conditions
        int bndcnt=0;
        
        for(k = 0; k < nvel; ++k)
        {



            const Array<OneD, SpatialDomains::BoundaryConditionShPtr> bndConds = fields[k]->GetBndConditions();
            Array<OneD, const MultiRegions::ExpListSharedPtr> bndCondExp;
            if(m_HomogeneousType == eHomogeneous1D) 
            {
                bndCondExp = m_fields[k]->GetPlane(2*mode)->GetBndCondExpansions();
            }
            else
            {
                bndCondExp = m_fields[k]->GetBndCondExpansions();
            }
            
/*     		ofstream myfile_bnd_cond_elem ("bnd_cond_elem.txt");
		if (myfile_bnd_cond_elem.is_open())
	  	{
			myfile_bnd_cond_elem << std::setprecision(17) << bndCondExp.num_elements() << "\n";
                	myfile_bnd_cond_elem.close();
	  	}
	  	else cout << "Unable to open file"; 
*/
            for(i = 0; i < bndCondExp.num_elements(); ++i)
            {
                const Array<OneD, const NekDouble > bndCondCoeffs = bndCondExp[i]->GetCoeffs();

		std::stringstream sstm;
		sstm << "bndcond_k" << k << "_i_" << i << ".txt";
		std::string result = sstm.str();
		const char* rr = result.c_str();

//		std::string outname_txt = "bndcond_k" + std::to_string(k) + "_i_" + std::to_string(i) + ".txt";
//		const char* outname_t = outname_txt.c_str();
		const char* outname_t = rr;

       
/*     		ofstream myfile_t (outname_t);
		if (myfile_t.is_open())
	  	{
			for(int count_bnd = 0; count_bnd < bndCondCoeffs.num_elements(); count_bnd++)
			{
				myfile_t << std::setprecision(17) << bndCondCoeffs[count_bnd] << "\n";
			}
                	myfile_t.close();
	  	}
	  	else cout << "Unable to open file"; 
*/

//		cout << "i " << i << endl;
//		for(int count_bnd = 0; count_bnd < bndCondCoeffs.num_elements(); count_bnd++)
//		{
//			cout << "bndCondCoeffs[count_bnd] " << bndCondCoeffs[count_bnd] << endl;
//		}
 


               cnt = 0;
                for(n = 0; n < nz_loc; ++n)
                {
                    if(bndConds[i]->GetBoundaryConditionType() 
                        == SpatialDomains::eDirichlet)
                    {
                        for(j = 0; j < (bndCondExp[i])->GetNcoeffs(); j++)
                        {
                            if (m_equationType == eSteadyNavierStokes && m_initialStep == false)
                            {
                                //This condition set all the Dirichlet BC at 0 after
                                //the initial step of the Newton method
                                bnd[bndmap[bndcnt++]] = 0;
                            }
                            else
                            {
                                bnd[bndmap[bndcnt++]] = bndCondCoeffs[cnt++];
                            }
                        }
                    }
                    else
                    {                    
                        for(j = 0; j < (bndCondExp[i])->GetNcoeffs(); j++)
                        {
                            fh_bnd[bndmap[bndcnt++]]
                            += bndCondCoeffs[cnt++];
                        }
                    }
                }
            }
        }
        
//	cout <<	"m_locToGloMap[mode]->GetNumGlobalDirBndCoeffs() " << m_locToGloMap[mode]->GetNumGlobalDirBndCoeffs() << endl;
//	cout <<	"m_locToGloMap[mode]->GetNumGlobalCoeffs() " << m_locToGloMap[mode]->GetNumGlobalCoeffs() << endl;

/*          ofstream myfiledirbnd ("NumGlobalDirBndCoeffs.txt");
	  if (myfiledirbnd.is_open())
	  {
		myfiledirbnd << std::setprecision(17) << m_locToGloMap[mode]->GetNumGlobalDirBndCoeffs();
                myfiledirbnd.close();
	  }
	  else cout << "Unable to open file";   
*/

//	cout << "fh_bnd.num_elements() " << fh_bnd.num_elements() << std::endl;   
//	cout << "bnd.num_elements() " << bnd.num_elements() << std::endl;  
	
	for(i = 0; i <  fh_bnd.num_elements(); ++i)
	{
//		cout << "bnd[i] " << i << " " << bnd[i] << std::endl;   
	}
        m_mat[mode].m_CoupledBndSys->Solve(fh_bnd,bnd,m_locToGloMap[mode]);                 // actual multi-static-condensed solve here

/*	for(i = 0; i <  fh_bnd.num_elements(); ++i)
	{
		cout << "bnd[i] " << i << " " << bnd[i] << std::endl;   
	}
        
*/

        // unpack pressure and velocity boundary systems. 
        offset = cnt = 0; 
        int totpcoeffs = pressure->GetNcoeffs();
        Array<OneD, NekDouble> p_coeffs = pressure->UpdateCoeffs();
        for(i = 0; i <  nel; ++i)
        {
            eid  = fields[0]->GetOffset_Elmt_Id(i);
            nbnd = nz_loc*fields[0]->GetExp(eid)->NumBndryCoeffs(); 
            nint = pressure->GetExp(eid)->GetNcoeffs(); 
            
            for(j = 0; j < nvel; ++j)
            {
                for(k = 0; k < nbnd; ++k)
                {
                    f_bnd[cnt+k] = loctoglosign[offset+j*nbnd+k]*bnd[loctoglomap[offset + j*nbnd + k]];
/*		    cout << "writing " << loctoglosign[offset+j*nbnd+k]*bnd[loctoglomap[offset + j*nbnd + k]] << " to " << cnt+k << std::endl;
		    cout << "loctoglosign[offset+j*nbnd+k] " << loctoglosign[offset+j*nbnd+k] << " loctoglomap[offset + j*nbnd + k] " << loctoglomap[offset + j*nbnd + k] << std::endl;
		    cout << "nvel " << nvel << std::endl;
		    cout << "nint " << nint << std::endl;
		    cout << "nbnd " << nbnd << std::endl;
		    cout << "nz_loc " << nz_loc << std::endl; */
                }
                cnt += nbnd;
            }
            offset += nvel*nbnd + nint*nz_loc;
        }
        
/*	for(i = 0; i <  f_bnd.num_elements(); ++i)
	{
		cout << "f_bnd[i] " << i << " " << f_bnd[i] << std::endl;   
	}
*/

        pressure->SetPhysState(false);
        
        offset = cnt = cnt1 = 0;
        for(i = 0; i < nel; ++i)
        {
            eid  = fields[0]->GetOffset_Elmt_Id(i);
            nint = pressure->GetExp(eid)->GetNcoeffs(); 
            nbnd = fields[0]->GetExp(eid)->NumBndryCoeffs(); 
            cnt1 = pressure->GetCoeff_Offset(eid);

//	    cout << "nint " << nint << std::endl;
//	    cout << "nbnd " << nbnd << std::endl;
//	    cout << "cnt1 " << cnt1 << std::endl;
            
            for(n = 0; n < nz_loc; ++n)
            {
                for(j = 0; j < nint; ++j)
                {
                    p_coeffs[n*totpcoeffs + cnt1+j] = 
                    f_p[cnt+j] = bnd[loctoglomap[offset + 
                    (nvel*nz_loc)*nbnd + 
                    n*nint + j]];
                }
                cnt += nint;
            }
            offset += (nvel*nbnd + nint)*nz_loc;
        }
        
/*	for(i = 0; i <  f_p.num_elements(); ++i)
	{
		cout << "f_p[i] " << i << " " << f_p[i] << std::endl;   
	}
*/

        // Back solve first level of static condensation for interior
        // velocity space and store in F_int
        F_int = F_int + Transpose(*m_mat[mode].m_D_int)*F_p
        - Transpose(*m_mat[mode].m_Btilde)*F_bnd;
        F_int = (*m_mat[mode].m_Cinv)*F_int;

	// --- writing the fields in loc format from here on, so what i want is also the f_bnd, f_p, f_int
//	curr_f_bnd = f_bnd;
/*	cout << "f_bnd.num_elements() " << f_bnd.num_elements() << endl;
	cout << "f_p.num_elements() " << f_p.num_elements() << endl;
	cout << "f_int.num_elements() " << f_int.num_elements() << endl;

	cout << "RB_A.cols() " << RB_A.cols() << endl;
	cout << "RB_A.rows() " << RB_A.rows() << endl;
	cout << "RB_B.cols() " << RB_B.cols() << endl;
	cout << "RB_B.rows() " << RB_B.rows() << endl;
	cout << "RB_C.cols() " << RB_C.cols() << endl;
	cout << "RB_C.rows() " << RB_C.rows() << endl;
	cout << "RB_D.cols() " << RB_D.cols() << endl;
	cout << "RB_D.rows() " << RB_D.rows() << endl;
	cout << "RB_Dbnd.cols() " << RB_Dbnd.cols() << endl;
	cout << "RB_Dbnd.rows() " << RB_Dbnd.rows() << endl;
	cout << "RB_Dint.cols() " << RB_Dint.cols() << endl;
	cout << "RB_Dint.rows() " << RB_Dint.rows() << endl;
*/

	curr_f_bnd = Eigen::VectorXd::Zero(f_bnd.num_elements());
	for (int i_phys_dof = 0; i_phys_dof < f_bnd.num_elements(); i_phys_dof++)
	{
		curr_f_bnd(i_phys_dof) = f_bnd[i_phys_dof]; 
	}
	curr_f_p = Eigen::VectorXd::Zero(f_p.num_elements()); 
	for (int i_phys_dof = 0; i_phys_dof < f_p.num_elements(); i_phys_dof++)
	{
		curr_f_p(i_phys_dof) = f_p[i_phys_dof]; 
	}
	curr_f_int = Eigen::VectorXd::Zero(f_int.num_elements()); 
	for (int i_phys_dof = 0; i_phys_dof < f_int.num_elements(); i_phys_dof++)
	{
		curr_f_int(i_phys_dof) = f_int[i_phys_dof]; 
	}

        
        // Unpack solution from Bnd and F_int to v_coeffs 
        cnt = cnt1 = 0;
        for(i = 0; i < nel; ++i) // loop over elements
        {
            eid  = fields[m_velocity[0]]->GetOffset_Elmt_Id(i);
            fields[0]->GetExp(eid)->GetBoundaryMap(bmap);
            fields[0]->GetExp(eid)->GetInteriorMap(imap);
            nbnd   = bmap.num_elements();
            nint   = imap.num_elements();
            offset = fields[0]->GetCoeff_Offset(eid);
            
	//    cout << "offset " << offset << std::endl;
	//    cout << "nint " << nint << std::endl;

            for(j = 0; j < nvel; ++j) // loop over velocity fields 
            {
                for(n = 0; n < nz_loc; ++n)
                {
                    for(k = 0; k < nbnd; ++k)
                    {
                        fields[j]->SetCoeff(n*nplanecoeffs + 
                        offset+bmap[k],
                        f_bnd[cnt+k]);
                    }
                    
                    for(k = 0; k < nint; ++k)
                    {
                        fields[j]->SetCoeff(n*nplanecoeffs + 
                        offset+imap[k],
                        f_int[cnt1+k]);
                    }
                    cnt  += nbnd;
                    cnt1 += nint;
                }
            }
        }
        
        for(j = 0; j < nvel; ++j) 
        {
            fields[j]->SetPhysState(false);
        }
    }

    NekDouble CoupledLinearNS_TT::Get_m_kinvis(void)
    {
	return m_kinvis;
    }

    void CoupledLinearNS_TT::Set_m_kinvis(NekDouble input)
    {
	m_kinvis = input;
    }

    Eigen::MatrixXd CoupledLinearNS_TT::Get_no_advection_matrix(void)
    {
	Eigen::MatrixXd no_adv_matrix = Eigen::MatrixXd::Zero(M_truth_size, M_truth_size);
	switch(globally_connected) {
		case 0:
			no_adv_matrix.block(0, 0, RB_A.rows(), RB_A.cols()) = MtM * RB_A_no_adv;
			no_adv_matrix.block(0, RB_A.cols(), RB_Dbnd.cols(), RB_Dbnd.rows()) = -MtM * RB_Dbnd.transpose();
			no_adv_matrix.block(0, RB_A.cols() + RB_Dbnd.rows(), RB_B.rows(), RB_B.cols()) = MtM * RB_B_no_adv;
			no_adv_matrix.block(RB_A.rows(), 0, RB_Dbnd.rows(), RB_Dbnd.cols()) = -RB_Dbnd;
			no_adv_matrix.block(RB_A.rows(), RB_A.cols() + RB_Dbnd.rows(), RB_Dint.rows(), RB_Dint.cols()) = -RB_Dint;	
			no_adv_matrix.block(RB_A.rows() + RB_Dbnd.rows(), 0, RB_C.cols(), RB_C.rows()) = RB_C_no_adv.transpose();
			no_adv_matrix.block(RB_A.rows() + RB_Dbnd.rows(), RB_A.cols(), RB_Dint.cols(), RB_Dint.rows()) = -RB_Dint.transpose();
			no_adv_matrix.block(RB_A.rows() + RB_Dbnd.rows(), RB_A.cols() + RB_Dbnd.rows(), RB_D.rows(), RB_D.cols()) = RB_D_no_adv;
			break;
		case 1:
			no_adv_matrix.block(0, 0, nBndDofs, nBndDofs) = Mtrafo.transpose() * RB_A_no_adv * Mtrafo;
			no_adv_matrix.block(0, nBndDofs, nBndDofs, RB_Dbnd.rows()) = -Mtrafo.transpose() * RB_Dbnd.transpose();
			no_adv_matrix.block(0, nBndDofs + RB_Dbnd.rows(), nBndDofs, RB_B.cols()) = Mtrafo.transpose() * RB_B_no_adv;
			no_adv_matrix.block(nBndDofs, 0, RB_Dbnd.rows(), nBndDofs) = -RB_Dbnd * Mtrafo;
			no_adv_matrix.block(nBndDofs, nBndDofs + RB_Dbnd.rows(), RB_Dint.rows(), RB_Dint.cols()) = -RB_Dint;	
			no_adv_matrix.block(nBndDofs + RB_Dbnd.rows(), 0, RB_C.cols(), nBndDofs) = RB_C_no_adv.transpose() * Mtrafo;
			no_adv_matrix.block(nBndDofs + RB_Dbnd.rows(), nBndDofs, RB_Dint.cols(), RB_Dint.rows()) = -RB_Dint.transpose();
			no_adv_matrix.block(nBndDofs + RB_Dbnd.rows(), nBndDofs + RB_Dbnd.rows(), RB_D.rows(), RB_D.cols()) = RB_D_no_adv;
			break;
		case 2:
			no_adv_matrix.block(0, 0, RB_A.rows(), RB_A.cols()) = RB_A_no_adv;
			no_adv_matrix.block(0, RB_A.cols(), RB_Dbnd.cols(), RB_Dbnd.rows()) = -RB_Dbnd.transpose();
			no_adv_matrix.block(0, RB_A.cols() + RB_Dbnd.rows(), RB_B.rows(), RB_B.cols()) = RB_B_no_adv;
			no_adv_matrix.block(RB_A.rows(), 0, RB_Dbnd.rows(), RB_Dbnd.cols()) = -RB_Dbnd;
			no_adv_matrix.block(RB_A.rows(), RB_A.cols() + RB_Dbnd.rows(), RB_Dint.rows(), RB_Dint.cols()) = -RB_Dint;	
			no_adv_matrix.block(RB_A.rows() + RB_Dbnd.rows(), 0, RB_C.cols(), RB_C.rows()) = RB_C_no_adv.transpose();
			no_adv_matrix.block(RB_A.rows() + RB_Dbnd.rows(), RB_A.cols(), RB_Dint.cols(), RB_Dint.rows()) = -RB_Dint.transpose();
			no_adv_matrix.block(RB_A.rows() + RB_Dbnd.rows(), RB_A.cols() + RB_Dbnd.rows(), RB_D.rows(), RB_D.cols()) = RB_D_no_adv;
			break;
	}
	return no_adv_matrix;
    }

    Eigen::MatrixXd CoupledLinearNS_TT::Get_no_advection_matrix_pressure(void)
    {
	Eigen::MatrixXd no_adv_matrix = Eigen::MatrixXd::Zero(M_truth_size, M_truth_size);
	switch(globally_connected) {
		case 0:
			no_adv_matrix.block(0, RB_A.cols(), RB_Dbnd.cols(), RB_Dbnd.rows()) = -MtM * RB_Dbnd.transpose();
			no_adv_matrix.block(RB_A.rows(), 0, RB_Dbnd.rows(), RB_Dbnd.cols()) = -RB_Dbnd;
			no_adv_matrix.block(RB_A.rows(), RB_A.cols() + RB_Dbnd.rows(), RB_Dint.rows(), RB_Dint.cols()) = -RB_Dint;	
			no_adv_matrix.block(RB_A.rows() + RB_Dbnd.rows(), RB_A.cols(), RB_Dint.cols(), RB_Dint.rows()) = -RB_Dint.transpose();
			break;
		case 1:
			no_adv_matrix.block(0, nBndDofs, nBndDofs, RB_Dbnd.rows()) = -Mtrafo.transpose() * RB_Dbnd.transpose();
			no_adv_matrix.block(nBndDofs, 0, RB_Dbnd.rows(), nBndDofs) = -RB_Dbnd * Mtrafo;
			no_adv_matrix.block(nBndDofs, nBndDofs + RB_Dbnd.rows(), RB_Dint.rows(), RB_Dint.cols()) = -RB_Dint;	
			no_adv_matrix.block(nBndDofs + RB_Dbnd.rows(), nBndDofs, RB_Dint.cols(), RB_Dint.rows()) = -RB_Dint.transpose();
			break;
		case 2:
			no_adv_matrix.block(0, RB_A.cols(), RB_Dbnd.cols(), RB_Dbnd.rows()) = -RB_Dbnd.transpose();
			no_adv_matrix.block(RB_A.rows(), 0, RB_Dbnd.rows(), RB_Dbnd.cols()) = -RB_Dbnd;
			no_adv_matrix.block(RB_A.rows(), RB_A.cols() + RB_Dbnd.rows(), RB_Dint.rows(), RB_Dint.cols()) = -RB_Dint;	
			no_adv_matrix.block(RB_A.rows() + RB_Dbnd.rows(), RB_A.cols(), RB_Dint.cols(), RB_Dint.rows()) = -RB_Dint.transpose();
			break;
	}			


	return no_adv_matrix;
			
    }

    Eigen::MatrixXd CoupledLinearNS_TT::Get_no_advection_matrix_ABCD(void)
    {
	Eigen::MatrixXd no_adv_matrix = Eigen::MatrixXd::Zero(M_truth_size, M_truth_size);
	switch(globally_connected) {
		case 0:
			no_adv_matrix.block(0, 0, RB_A.rows(), RB_A.cols()) = MtM * RB_A_no_adv;
			no_adv_matrix.block(0, RB_A.cols() + RB_Dbnd.rows(), RB_B.rows(), RB_B.cols()) = MtM * RB_B_no_adv;
			no_adv_matrix.block(RB_A.rows() + RB_Dbnd.rows(), 0, RB_C.cols(), RB_C.rows()) = RB_C_no_adv.transpose();
			no_adv_matrix.block(RB_A.rows() + RB_Dbnd.rows(), RB_A.cols() + RB_Dbnd.rows(), RB_D.rows(), RB_D.cols()) = RB_D_no_adv;
			break;
		case 1:
			no_adv_matrix.block(0, 0, nBndDofs, nBndDofs) = Mtrafo.transpose() * RB_A_no_adv * Mtrafo;
			no_adv_matrix.block(0, nBndDofs + RB_Dbnd.rows(), nBndDofs, RB_B.cols()) = Mtrafo.transpose() * RB_B_no_adv;
			no_adv_matrix.block(nBndDofs + RB_Dbnd.rows(), 0, RB_C.cols(), nBndDofs) = RB_C_no_adv.transpose() * Mtrafo;
			no_adv_matrix.block(nBndDofs + RB_Dbnd.rows(), nBndDofs + RB_Dbnd.rows(), RB_D.rows(), RB_D.cols()) = RB_D_no_adv;
			break;
		case 2:
			no_adv_matrix.block(0, 0, RB_A.rows(), RB_A.cols()) = RB_A_no_adv;
			no_adv_matrix.block(0, RB_A.cols() + RB_Dbnd.rows(), RB_B.rows(), RB_B.cols()) = RB_B_no_adv;
			no_adv_matrix.block(RB_A.rows() + RB_Dbnd.rows(), 0, RB_C.cols(), RB_C.rows()) = RB_C_no_adv.transpose();
			no_adv_matrix.block(RB_A.rows() + RB_Dbnd.rows(), RB_A.cols() + RB_Dbnd.rows(), RB_D.rows(), RB_D.cols()) = RB_D_no_adv;
			break;
	}

	return no_adv_matrix;
    }
	
    Eigen::MatrixXd CoupledLinearNS_TT::Get_advection_matrix(void)
    {
	Eigen::MatrixXd adv_matrix = Eigen::MatrixXd::Zero(M_truth_size, M_truth_size);
	switch(globally_connected) {
		case 0:
			adv_matrix.block(0, 0, RB_A.rows(), RB_A.cols()) = MtM * RB_A_adv;
			adv_matrix.block(0, RB_A.cols() + RB_Dbnd.rows(), RB_B.rows(), RB_B.cols()) = MtM * RB_B_adv;
			adv_matrix.block(RB_A.rows() + RB_Dbnd.rows(), 0, RB_C.cols(), RB_C.rows()) = RB_C_adv.transpose();
			adv_matrix.block(RB_A.rows() + RB_Dbnd.rows(), RB_A.cols() + RB_Dbnd.rows(), RB_D.rows(), RB_D.cols()) = RB_D_adv;
			break;
		case 1:
			adv_matrix.block(0, 0, nBndDofs, nBndDofs) = Mtrafo.transpose() * RB_A_adv * Mtrafo;
			adv_matrix.block(0, nBndDofs + RB_Dbnd.rows(), nBndDofs, RB_B.cols()) = Mtrafo.transpose() * RB_B_adv;
			adv_matrix.block(nBndDofs + RB_Dbnd.rows(), 0, RB_C.cols(), nBndDofs) = RB_C_adv.transpose() * Mtrafo;
			adv_matrix.block(nBndDofs + RB_Dbnd.rows(), nBndDofs + RB_Dbnd.rows(), RB_D.rows(), RB_D.cols()) = RB_D_adv;
			break;
		case 2:
			adv_matrix.block(0, 0, RB_A.rows(), RB_A.cols()) = RB_A_adv;
			adv_matrix.block(0, RB_A.cols() + RB_Dbnd.rows(), RB_B.rows(), RB_B.cols()) = RB_B_adv;
			adv_matrix.block(RB_A.rows() + RB_Dbnd.rows(), 0, RB_C.cols(), RB_C.rows()) = RB_C_adv.transpose();
			adv_matrix.block(RB_A.rows() + RB_Dbnd.rows(), RB_A.cols() + RB_Dbnd.rows(), RB_D.rows(), RB_D.cols()) = RB_D_adv;
			break;
	}

	return adv_matrix;
    }	

    Eigen::MatrixXd CoupledLinearNS_TT::Get_complete_matrix(void)
    {
	Eigen::MatrixXd matrix = Eigen::MatrixXd::Zero(M_truth_size, M_truth_size);
	switch(globally_connected) {
		case 0:
			matrix.block(0, 0, RB_A.rows(), RB_A.cols()) = MtM * RB_A;
			matrix.block(0, RB_A.cols(), RB_Dbnd.cols(), RB_Dbnd.rows()) = -MtM * RB_Dbnd.transpose();
			matrix.block(0, RB_A.cols() + RB_Dbnd.rows(), RB_B.rows(), RB_B.cols()) = MtM * RB_B;
			matrix.block(RB_A.rows(), 0, RB_Dbnd.rows(), RB_Dbnd.cols()) = -RB_Dbnd;
			matrix.block(RB_A.rows(), RB_A.cols() + RB_Dbnd.rows(), RB_Dint.rows(), RB_Dint.cols()) = -RB_Dint;	
			matrix.block(RB_A.rows() + RB_Dbnd.rows(), 0, RB_C.cols(), RB_C.rows()) = RB_C.transpose();
			matrix.block(RB_A.rows() + RB_Dbnd.rows(), RB_A.cols(), RB_Dint.cols(), RB_Dint.rows()) = -RB_Dint.transpose();
			matrix.block(RB_A.rows() + RB_Dbnd.rows(), RB_A.cols() + RB_Dbnd.rows(), RB_D.rows(), RB_D.cols()) = RB_D;
			break;
		case 1:
			matrix.block(0, 0, nBndDofs, nBndDofs) = Mtrafo.transpose() * RB_A * Mtrafo;
			matrix.block(0, nBndDofs, nBndDofs, RB_Dbnd.rows()) = -Mtrafo.transpose() * RB_Dbnd.transpose();
			matrix.block(0, nBndDofs + RB_Dbnd.rows(), nBndDofs, RB_B.cols()) = Mtrafo.transpose() * RB_B;
			matrix.block(nBndDofs, 0, RB_Dbnd.rows(), nBndDofs) = -RB_Dbnd * Mtrafo;
			matrix.block(nBndDofs, nBndDofs + RB_Dbnd.rows(), RB_Dint.rows(), RB_Dint.cols()) = -RB_Dint;	
			matrix.block(nBndDofs + RB_Dbnd.rows(), 0, RB_C.cols(), nBndDofs) = RB_C.transpose() * Mtrafo;
			matrix.block(nBndDofs + RB_Dbnd.rows(), nBndDofs, RB_Dint.cols(), RB_Dint.rows()) = -RB_Dint.transpose();
			matrix.block(nBndDofs + RB_Dbnd.rows(), nBndDofs + RB_Dbnd.rows(), RB_D.rows(), RB_D.cols()) = RB_D;
			break;
		case 2:
			matrix.block(0, 0, RB_A.rows(), RB_A.cols()) = RB_A;
			matrix.block(0, RB_A.cols(), RB_Dbnd.cols(), RB_Dbnd.rows()) = -RB_Dbnd.transpose();
			matrix.block(0, RB_A.cols() + RB_Dbnd.rows(), RB_B.rows(), RB_B.cols()) = RB_B;
			matrix.block(RB_A.rows(), 0, RB_Dbnd.rows(), RB_Dbnd.cols()) = -RB_Dbnd;
			matrix.block(RB_A.rows(), RB_A.cols() + RB_Dbnd.rows(), RB_Dint.rows(), RB_Dint.cols()) = -RB_Dint;	
			matrix.block(RB_A.rows() + RB_Dbnd.rows(), 0, RB_C.cols(), RB_C.rows()) = RB_C.transpose();
			matrix.block(RB_A.rows() + RB_Dbnd.rows(), RB_A.cols(), RB_Dint.cols(), RB_Dint.rows()) = -RB_Dint.transpose();
			matrix.block(RB_A.rows() + RB_Dbnd.rows(), RB_A.cols() + RB_Dbnd.rows(), RB_D.rows(), RB_D.cols()) = RB_D;
			break;
	}			
	return matrix;
    }

    Eigen::MatrixXd CoupledLinearNS_TT::remove_cols_and_rows(Eigen::MatrixXd the_matrix, std::set<int> elements_to_be_removed)
    {
	Eigen::MatrixXd simplified_matrix = Eigen::MatrixXd::Zero(the_matrix.rows() - elements_to_be_removed.size(), the_matrix.cols() - elements_to_be_removed.size());
	int counter_row_simplified = 0;
	int counter_col_simplified = 0;
	for (int row_index=0; row_index < the_matrix.rows(); ++row_index)
	{
		if (!elements_to_be_removed.count(row_index))
		{
			for (int col_index=0; col_index < the_matrix.cols(); ++col_index)
			{
				if (!elements_to_be_removed.count(col_index))
				{
					simplified_matrix(counter_row_simplified, counter_col_simplified) = the_matrix(row_index, col_index);
					counter_col_simplified++;
				}			
			}
			counter_col_simplified = 0;
			counter_row_simplified++;
		}		
	}
	return simplified_matrix;
    }

    Eigen::VectorXd CoupledLinearNS_TT::remove_rows(Eigen::VectorXd the_vector, std::set<int> elements_to_be_removed)
    {
	Eigen::VectorXd simplified_vector = Eigen::VectorXd::Zero(the_vector.rows() - elements_to_be_removed.size());
	int counter_row_simplified = 0;
	for (int row_index=0; row_index < the_vector.rows(); ++row_index)
	{
		if (!elements_to_be_removed.count(row_index))
		{
			simplified_vector(counter_row_simplified) = the_vector(row_index);
			counter_row_simplified++;
		}		
	}
	return simplified_vector;
    }


	// since this function is virtual it actually can be instantiated here
    void CoupledLinearNS_TT::v_DoSolve(void)
    {


        switch(m_equationType)
        {
            case eUnsteadyStokes:
            case eUnsteadyNavierStokes:
                //AdvanceInTime(m_steps);
                UnsteadySystem::v_DoSolve();
                break;
            case eSteadyStokes:
            case eSteadyOseen:
            case eSteadyLinearisedNS:
            {
                Solve();				
                break;
            }
            case eSteadyNavierStokes:
            {	
                Timer Generaltimer;
                Generaltimer.Start();
                
                int Check(0);
                
                //Saving the init datas
                Checkpoint_Output(Check);
                Check++;
                
                cout<<"We execute INITIALLY SolveSteadyNavierStokes for m_kinvis = "<<m_kinvis<<" (<=> Re = "<<1/m_kinvis<<")"<<endl;
                SolveSteadyNavierStokes();
                
                while(m_kinvis > m_kinvisMin)
                {		
                    if (Check == 1)
                    {
                        cout<<"We execute SolveSteadyNavierStokes for m_kinvis = "<<m_kinvis<<" (<=> Re = "<<1/m_kinvis<<")"<<endl;
                        SolveSteadyNavierStokes();
                        Checkpoint_Output(Check);
                        Check++;
                    }
                    
                    Continuation();
                    
                    if (m_kinvis > m_kinvisMin)
                    {
                        cout<<"We execute SolveSteadyNavierStokes for m_kinvis = "<<m_kinvis<<" (<=> Re = "<<1/m_kinvis<<")"<<endl;
                        SolveSteadyNavierStokes();
                        Checkpoint_Output(Check);
                        Check++;
                    }
                }
                
                
                Generaltimer.Stop();
                cout<<"\nThe total calculation time is : " << Generaltimer.TimePerTest(1)/60 << " minute(s). \n\n";
                
                break;
            }
            case eNoEquationType:
            default:
                ASSERTL0(false,"Unknown or undefined equation type for CoupledLinearNS");
        }

    }

    void CoupledLinearNS_TT::DoInitialiseAdv(Array<OneD, NekDouble> myAdvField_x, Array<OneD, NekDouble> myAdvField_y)
    {
	// only covers case eSteadyOseen

	Array<OneD, Array<OneD, NekDouble> > myAdvField(2);
	myAdvField[0] = myAdvField_x;
	myAdvField[1] = myAdvField_y;

        std::vector<std::string> fieldStr;
        for(int i = 0; i < m_velocity.num_elements(); ++i)
        {
             fieldStr.push_back(m_boundaryConditions->GetVariable(m_velocity[i]));
        }
//	cout << "fieldStr[0] " << fieldStr[0] << endl;
//        EvaluateFunction(fieldStr,AdvField,"AdvectionVelocity"); // defined in EquationSystem

        SetUpCoupledMatrix(0.0, myAdvField, false);

    }

    void CoupledLinearNS_TT::setDBC(Eigen::MatrixXd collect_f_all)
    {
	no_dbc_in_loc = 0;
	no_not_dbc_in_loc = 0;
	for ( int index_c_f_bnd = 0; index_c_f_bnd < curr_f_bnd.size(); index_c_f_bnd++ )
	{
		if (collect_f_all(index_c_f_bnd,0) == collect_f_all(index_c_f_bnd,1))
		{
			no_dbc_in_loc++;
			elem_loc_dbc.insert(index_c_f_bnd);
		}
		else
		{
			no_not_dbc_in_loc++;
			elem_not_loc_dbc.insert(index_c_f_bnd);
		}
	}
    }

    void CoupledLinearNS_TT::setDBC_M(Eigen::MatrixXd collect_f_all)
    {
	// compute the dofs after Mtrafo multiplication to get global indices instead of local indices
	// Mtrafo = Eigen::MatrixXd (RB_A.rows(), nBndDofs);
	M_no_dbc_in_loc = 0;
	M_no_not_dbc_in_loc = 0;
	Eigen::VectorXd compare_vec1 = Mtrafo.transpose() * collect_f_all.block( 0, 0, curr_f_bnd.size(), 1);
	Eigen::VectorXd compare_vec2 = Mtrafo.transpose() * collect_f_all.block( 0, 1, curr_f_bnd.size(), 1);
	for ( int index_c_f_bnd = 0; index_c_f_bnd < compare_vec1.rows(); index_c_f_bnd++ )
	{
		if (compare_vec1(index_c_f_bnd) == compare_vec2(index_c_f_bnd))
		{
			M_no_dbc_in_loc++; // is actually no_dbc_in_global
			M_elem_loc_dbc.insert(index_c_f_bnd);
		}
		else
		{
			M_no_not_dbc_in_loc++;
			M_elem_not_loc_dbc.insert(index_c_f_bnd);
		}
	}
	M_truth_size = compare_vec1.rows() + curr_f_p.size() + curr_f_int.size();  // compare_vec1.rows() corresponds to nBndDofs
	M_truth_size_without_DBC = M_no_not_dbc_in_loc + curr_f_p.size() + curr_f_int.size();
	M_f_bnd_dbc = Eigen::VectorXd::Zero(M_no_dbc_in_loc);
	M_f_bnd_dbc_full_size = Eigen::VectorXd::Zero(M_truth_size);
	M_RB = Eigen::MatrixXd::Zero(M_truth_size_without_DBC, PODmodes.cols());
	// first multiply the bnd part in PODmodes with Mtrafo
	Eigen::MatrixXd M_PODmodes_bnd = Mtrafo.transpose() * PODmodes.block( 0, 0, curr_f_bnd.size(), RBsize );
	Eigen::MatrixXd M_collect_f_all_bnd = Mtrafo.transpose() * collect_f_all.block( 0, 0, curr_f_bnd.size(), Nmax );
	M_collect_f_all = Eigen::MatrixXd::Zero( M_truth_size , Nmax );
	int counter_all = 0;
	int counter_dbc = 0;
	for (int index=0; index < M_truth_size; ++index)  // take from the M_PODmodes_bnd if index is below compare_vec1.rows(), otherwise from PODmodes
	{
		if (!M_elem_loc_dbc.count(index))
		{
			if (index < compare_vec1.rows())
			{
				M_RB.row(counter_all) = M_PODmodes_bnd.row(index);
			}
			else
			{
				M_RB.row(counter_all) = PODmodes.row(index);
			}
			M_f_bnd_dbc_full_size(index) = 0;
			counter_all++;
		}
		else
		{
			M_f_bnd_dbc_full_size(index) = collect_f_all(index,0);
			M_f_bnd_dbc(counter_dbc) = collect_f_all(index,0);
			counter_dbc++;
		}
		if (index < compare_vec1.rows())
		{
			M_collect_f_all.row(index) = M_collect_f_all_bnd.row(index);
		}
		else
		{
			M_collect_f_all.row(index) = collect_f_all.row(index);
		}

	}
	elem_loc_dbc = M_elem_loc_dbc;
	f_bnd_dbc_full_size = M_f_bnd_dbc_full_size;
	RB = M_RB; // could be discussed if desired like that...
	// does not diminish size  collect_f_all = M_collect_f_all; // could be discussed if desired like that...
    }

    void CoupledLinearNS_TT::v_DoInitialise(void)
    {
        switch(m_equationType)
        {
            case eUnsteadyStokes:
            case eUnsteadyNavierStokes:
            {
                
//                LibUtilities::TimeIntegrationMethod intMethod;
//                std::string TimeIntStr = m_session->GetSolverInfo("TIMEINTEGRATIONMETHOD");
//                int i;
//                for(i = 0; i < (int) LibUtilities::SIZE_TimeIntegrationMethod; ++i)
//                {
//                    if(boost::iequals(LibUtilities::TimeIntegrationMethodMap[i],TimeIntStr))
//                    {
//                        intMethod = (LibUtilities::TimeIntegrationMethod)i;
//                        break;
//                    }
//                }
//
//                ASSERTL0(i != (int) LibUtilities::SIZE_TimeIntegrationMethod, "Invalid time integration type.");
//
//                m_integrationScheme = LibUtilities::GetTimeIntegrationWrapperFactory().CreateInstance(LibUtilities::TimeIntegrationMethodMap[intMethod]);
                
                // Could defind this from IncNavierStokes class? 
                m_ode.DefineOdeRhs(&CoupledLinearNS::EvaluateAdvection, this);
                
                m_ode.DefineImplicitSolve(&CoupledLinearNS::SolveUnsteadyStokesSystem,this);
                
                // Set initial condition using time t=0
                
                SetInitialConditions(0.0);
                
            }
        case eSteadyStokes:
            SetUpCoupledMatrix(0.0);
            break;
        case eSteadyOseen:
            {

		Array<OneD, Array<OneD, NekDouble> > AdvField(m_velocity.num_elements());
                for(int i = 0; i < m_velocity.num_elements(); ++i)
                {
                    AdvField[i] = Array<OneD, NekDouble> (m_fields[m_velocity[i]]->GetTotPoints(),0.0);
                }
                
                ASSERTL0(m_session->DefinesFunction("AdvectionVelocity"),
                         "Advection Velocity section must be defined in "
                         "session file.");
                
                std::vector<std::string> fieldStr;
                for(int i = 0; i < m_velocity.num_elements(); ++i)
                {
                    fieldStr.push_back(m_boundaryConditions->GetVariable(m_velocity[i]));
                }

//		cout << "fieldStr[0] " << fieldStr[0] << endl;
//		cout << "fieldStr.size() " << fieldStr.size() << endl;

                EvaluateFunction(fieldStr,AdvField,"AdvectionVelocity"); // defined in EquationSystem

//		cout << "AdvField.num_elements() " << AdvField.num_elements() << endl;
//		cout << "AdvField[0].num_elements() " << AdvField[0].num_elements() << endl;                                

//		DefineRBspace();

                SetUpCoupledMatrix(0.0, AdvField, false);

            }
            break;
        case eSteadyNavierStokes:
            {				
                m_session->LoadParameter("KinvisMin", m_kinvisMin);
                m_session->LoadParameter("KinvisPercentage", m_KinvisPercentage);
                m_session->LoadParameter("Tolerence", m_tol);
                m_session->LoadParameter("MaxIteration", m_maxIt);
                m_session->LoadParameter("MatrixSetUpStep", m_MatrixSetUpStep);
                m_session->LoadParameter("Restart", m_Restart);
                
                
                DefineForcingTerm();
                
                if (m_Restart == 1)
                {
                    ASSERTL0(m_session->DefinesFunction("Restart"),
                             "Restart section must be defined in session file.");
                    
                    Array<OneD, Array<OneD, NekDouble> > Restart(m_velocity.num_elements());
                    for(int i = 0; i < m_velocity.num_elements(); ++i)
                    {
                        Restart[i] = Array<OneD, NekDouble> (m_fields[m_velocity[i]]->GetTotPoints(),0.0);
                    }
                    std::vector<std::string> fieldStr;
                    for(int i = 0; i < m_velocity.num_elements(); ++i)
                    {
                        fieldStr.push_back(m_boundaryConditions->GetVariable(m_velocity[i]));
                    }
                    EvaluateFunction(fieldStr, Restart, "Restart");
                    
                    for(int i = 0; i < m_velocity.num_elements(); ++i)
                    {
                        m_fields[m_velocity[i]]->FwdTrans_IterPerExp(Restart[i], m_fields[m_velocity[i]]->UpdateCoeffs());
                    }
                    cout << "Saving the RESTART file for m_kinvis = "<< m_kinvis << " (<=> Re = " << 1/m_kinvis << ")" <<endl;
                }
                else //We solve the Stokes Problem
                {
                    
                    /*Array<OneD, Array<OneD, NekDouble> >ZERO(m_velocity.num_elements());
                     *					
                     *					for(int i = 0; i < m_velocity.num_elements(); ++i)
                     *					{				
                     *						ZERO[i] = Array<OneD, NekDouble> (m_fields[m_velocity[i]]->GetTotPoints(),0.0);
                     *						m_fields[m_velocity[i]]->FwdTrans(ZERO[i], m_fields[m_velocity[i]]->UpdateCoeffs());
                     }*/
                    
                    SetUpCoupledMatrix(0.0);						
                    m_initialStep = true;
                    m_counter=1;
                    //SolveLinearNS(m_ForcingTerm_Coeffs);
                    Solve();
                    m_initialStep = false;
                    cout << "Saving the Stokes Flow for m_kinvis = "<< m_kinvis << " (<=> Re = " << 1/m_kinvis << ")" <<endl;
                }
            }
            break;
        case eSteadyLinearisedNS:
            {                
                SetInitialConditions(0.0);
                
                Array<OneD, Array<OneD, NekDouble> > AdvField(m_velocity.num_elements());
                for(int i = 0; i < m_velocity.num_elements(); ++i)
                {
                    AdvField[i] = Array<OneD, NekDouble> (m_fields[m_velocity[i]]->GetTotPoints(),0.0);
                }
                
                ASSERTL0(m_session->DefinesFunction("AdvectionVelocity"),
                         "Advection Velocity section must be defined in "
                         "session file.");
                
                std::vector<std::string> fieldStr;
                for(int i = 0; i < m_velocity.num_elements(); ++i)
                {
                    fieldStr.push_back(m_boundaryConditions->GetVariable(m_velocity[i]));
                }
                EvaluateFunction(fieldStr,AdvField,"AdvectionVelocity");
                
                SetUpCoupledMatrix(m_lambda,AdvField,true);
            }
            break;
        case eNoEquationType:
        default:
            ASSERTL0(false,"Unknown or undefined equation type for CoupledLinearNS");
        }

    }

    Eigen::VectorXd CoupledLinearNS_TT::trafoSnapshot(Eigen::VectorXd RB_via_POD, double kInvis)
    {

	


	return RB_via_POD;
    }

    void CoupledLinearNS_TT::set_MtM()
    {
	nBndDofs = m_locToGloMap[0]->GetNumGlobalBndCoeffs();  // number of global bnd dofs
        const Array<OneD,const int>& loctoglobndmap = m_locToGloMap[0]->GetLocalToGlobalBndMap();
        const Array<OneD,const NekDouble>& loctoglobndsign = m_locToGloMap[0]->GetLocalToGlobalBndSign();
//	Eigen::MatrixXd Mtrafo(RB_A.rows(), nBndDofs);
	M_truth_size = curr_f_bnd.size() + curr_f_p.size() + curr_f_int.size();  // compare_vec1.rows() corresponds to nBndDofs
	if (debug_mode)
	{
		cout << "Local dof size, also M_truth_size is " << curr_f_bnd.size() + curr_f_p.size() + curr_f_int.size() << endl;
	}
	M_truth_size_without_DBC = no_not_dbc_in_loc + curr_f_p.size() + curr_f_int.size();
	Mtrafo = Eigen::MatrixXd (RB_A.rows(), nBndDofs);
	Array<OneD, MultiRegions::ExpListSharedPtr> m_fields = UpdateFields();
        int  nel  = m_fields[0]->GetNumElmts(); // number of spectral elements
	int nsize_bndry_p1 = loctoglobndmap.num_elements() / nel;
	int nsize_bndry = nsize_bndry_p1-1;
	for (int curr_elem = 0; curr_elem < nel; curr_elem++)
	{
		int cnt = curr_elem*nsize_bndry_p1;
		int cnt_no_pp = curr_elem*nsize_bndry;
		for ( int index_ele = 0; index_ele < nsize_bndry_p1; index_ele++ )
		{
			int gid1 = loctoglobndmap[cnt+index_ele];
			int sign1 = loctoglobndsign[cnt+index_ele];
			if ((gid1 >= 0) && (index_ele < nsize_bndry))
			{
				Mtrafo(cnt_no_pp + index_ele, gid1) = sign1;
			}
		}
	}
	MtM = Mtrafo * Mtrafo.transpose();
	f_bnd_dbc = Eigen::VectorXd::Zero(no_dbc_in_loc);
	f_bnd_dbc_full_size = Eigen::VectorXd::Zero(PODmodes.rows());
	RB = Eigen::MatrixXd::Zero(PODmodes.rows() - no_dbc_in_loc, PODmodes.cols());
	int counter_all = 0;
	int counter_dbc = 0;
	for (int index=0; index < PODmodes.rows(); ++index)
	{
		if (!elem_loc_dbc.count(index))
		{
			RB.row(counter_all) = PODmodes.row(index);
			f_bnd_dbc_full_size(index) = 0;
			counter_all++;
		}
		else
		{
			f_bnd_dbc_full_size(index) = collect_f_all(index,0);
			f_bnd_dbc(counter_dbc) = collect_f_all(index,0);
			counter_dbc++;
		}
	}

    }

    void CoupledLinearNS_TT::gen_phys_base_vecs()
    {
	int RBsize = RB.cols();
	PhysBaseVec_x = Array<OneD, Array<OneD, double> > (RBsize); 
	PhysBaseVec_y = Array<OneD, Array<OneD, double> > (RBsize);
	
	if (debug_mode)
	{
		cout << " number of local dofs per velocity direction " << GetNcoeffs() << endl;
		cout << " number of quadrature dofs per velocity direction " << GetNpoints() << endl;
	}

	for (int curr_trafo_iter=0; curr_trafo_iter < RBsize; curr_trafo_iter++)
	{
		Eigen::VectorXd f_bnd = PODmodes.block(0, curr_trafo_iter, curr_f_bnd.size(), 1);
		Eigen::VectorXd f_int = PODmodes.block(curr_f_bnd.size()+curr_f_p.size(), curr_trafo_iter, curr_f_int.size(), 1);
		Array<OneD, MultiRegions::ExpListSharedPtr> fields = UpdateFields(); 
	        Array<OneD, unsigned int> bmap, imap; 
		Array<OneD, double> field_0(GetNcoeffs());
		Array<OneD, double> field_1(GetNcoeffs());
		Array<OneD, double> curr_PhysBaseVec_x(GetNpoints(), 0.0);
		Array<OneD, double> curr_PhysBaseVec_y(GetNpoints(), 0.0);
	        int cnt = 0;
		int cnt1 = 0;
		int nvel = 2;
	        int nz_loc = 1;
	        int  nplanecoeffs = fields[0]->GetNcoeffs();
		int  nel  = m_fields[0]->GetNumElmts();
	        for(int i = 0; i < nel; ++i) 
	        {
	            int eid  = i;
	            fields[0]->GetExp(eid)->GetBoundaryMap(bmap);
	            fields[0]->GetExp(eid)->GetInteriorMap(imap);
	            int nbnd   = bmap.num_elements();
	            int nint   = imap.num_elements();
	            int offset = fields[0]->GetCoeff_Offset(eid);
	            
	            for(int j = 0; j < nvel; ++j)
	            {
	                for(int n = 0; n < nz_loc; ++n)
	                {
	                    for(int k = 0; k < nbnd; ++k)
	                    {
	                        fields[j]->SetCoeff(n*nplanecoeffs + offset+bmap[k], f_bnd(cnt+k));
	                    }
	                    
	                    for(int k = 0; k < nint; ++k)
	                    {
	                        fields[j]->SetCoeff(n*nplanecoeffs + offset+imap[k], f_int(cnt1+k));
	                    }
	                    cnt  += nbnd;
	                    cnt1 += nint;
	                }
	            }
	        }
		Array<OneD, double> test_nn = fields[0]->GetCoeffs();
		fields[0]->BwdTrans_IterPerExp(fields[0]->GetCoeffs(), curr_PhysBaseVec_x);
		fields[1]->BwdTrans_IterPerExp(fields[1]->GetCoeffs(), curr_PhysBaseVec_y);
		PhysBaseVec_x[curr_trafo_iter] = curr_PhysBaseVec_x;
		PhysBaseVec_y[curr_trafo_iter] = curr_PhysBaseVec_y;
		
	}

	eigen_phys_basis_x = Eigen::MatrixXd::Zero(GetNpoints(), RBsize);
	eigen_phys_basis_y = Eigen::MatrixXd::Zero(GetNpoints(), RBsize);
	for (int index_phys_base=0; index_phys_base<GetNpoints(); index_phys_base++)
	{
		for (int index_RBsize=0; index_RBsize<RBsize; index_RBsize++)
		{
			eigen_phys_basis_x(index_phys_base,index_RBsize) = PhysBaseVec_x[index_RBsize][index_phys_base];
			eigen_phys_basis_y(index_phys_base,index_RBsize) = PhysBaseVec_y[index_RBsize][index_phys_base];
		}
	}

	Eigen::VectorXd curr_col = eigen_phys_basis_x.col(0);
	double norm_curr_col = curr_col.norm();
	eigen_phys_basis_x.col(0) = curr_col / norm_curr_col;
	curr_col = eigen_phys_basis_y.col(0);
	norm_curr_col = curr_col.norm();
	eigen_phys_basis_y.col(0) = curr_col / norm_curr_col;
	Eigen::VectorXd orthogonal_complement;
	
	for (int orth_iter=1; orth_iter<RBsize; orth_iter++)
	{
		curr_col = eigen_phys_basis_x.col(orth_iter);
		Eigen::MatrixXd leftmostCols = eigen_phys_basis_x.leftCols(orth_iter);
		orthogonal_complement = curr_col - leftmostCols * leftmostCols.transpose() * curr_col;
		norm_curr_col = orthogonal_complement.norm();
		eigen_phys_basis_x.col(orth_iter) = orthogonal_complement / norm_curr_col;
		curr_col = eigen_phys_basis_y.col(orth_iter);
		leftmostCols = eigen_phys_basis_y.leftCols(orth_iter);
		orthogonal_complement = curr_col - leftmostCols * leftmostCols.transpose() * curr_col;
		norm_curr_col = orthogonal_complement.norm();
		eigen_phys_basis_y.col(orth_iter) = orthogonal_complement / norm_curr_col;
	}

	orth_PhysBaseVec_x = Array<OneD, Array<OneD, double> > (RBsize); 
	orth_PhysBaseVec_y = Array<OneD, Array<OneD, double> > (RBsize); 
	for (int index_RBsize=0; index_RBsize<RBsize; index_RBsize++)
	{
		Array<OneD, double> curr_iter_x(GetNpoints());
		Array<OneD, double> curr_iter_y(GetNpoints());
		for (int index_phys_base=0; index_phys_base<GetNpoints(); index_phys_base++)	
		{
			curr_iter_x[index_phys_base] = eigen_phys_basis_x(index_phys_base,index_RBsize);
			curr_iter_y[index_phys_base] = eigen_phys_basis_y(index_phys_base,index_RBsize);			
		}
		orth_PhysBaseVec_x[index_RBsize] = curr_iter_x;
		orth_PhysBaseVec_y[index_RBsize] = curr_iter_y;			
	}
    }

    void CoupledLinearNS_TT::gen_proj_adv_terms()
    {
	RBsize = RB.cols();
	adv_mats_proj_x = Array<OneD, Eigen::MatrixXd > (RBsize);
	adv_mats_proj_y = Array<OneD, Eigen::MatrixXd > (RBsize);
	adv_vec_proj_x = Array<OneD, Eigen::VectorXd > (RBsize);
	adv_vec_proj_y = Array<OneD, Eigen::VectorXd > (RBsize);
	adv_vec_proj_x_newton = Array<OneD, Eigen::VectorXd > (RBsize);
	adv_vec_proj_y_newton = Array<OneD, Eigen::VectorXd > (RBsize);
	adv_vec_proj_x_newton_RB = Array<OneD, Eigen::MatrixXd > (RBsize);
	adv_vec_proj_y_newton_RB = Array<OneD, Eigen::MatrixXd > (RBsize);


	Array<OneD, double> PhysBase_zero(GetNpoints(), 0.0);
	for(int trafo_iter = 0; trafo_iter < RBsize; trafo_iter++)
	{
		Array<OneD, double> curr_PhysBaseVec_x = orth_PhysBaseVec_x[trafo_iter];
		Array<OneD, double> curr_PhysBaseVec_y = orth_PhysBaseVec_y[trafo_iter];

		InitObject();

		DoInitialiseAdv(curr_PhysBaseVec_x, PhysBase_zero); // call with parameter in phys state
		// needs to be replaced with a more gen. term		Eigen::MatrixXd adv_matrix = Eigen::MatrixXd::Zero(RB_A.rows() + RB_Dbnd.rows() + RB_C.cols(), RB_A.cols() + RB_Dbnd.rows() + RB_B.cols() );
		Eigen::MatrixXd adv_matrix;
		adv_matrix = Get_advection_matrix();
		Eigen::VectorXd add_to_rhs_adv(M_truth_size); // probably need this for adv and non-adv
		add_to_rhs_adv = adv_matrix * f_bnd_dbc_full_size;
		Eigen::MatrixXd adv_matrix_simplified = remove_cols_and_rows(adv_matrix, elem_loc_dbc);

		adv_vec_proj_x_newton_RB[trafo_iter] = Eigen::MatrixXd::Zero(RBsize,RBsize);


		if (use_Newton)
		{
//			Eigen::VectorXd add_to_rhs_adv_newton(M_truth_size); 
//			add_to_rhs_adv_newton = adv_matrix * PODmodes * PODmodes.transpose() * collect_f_all.col(3);      
//			Eigen::VectorXd adv_rhs_add_newton = remove_rows(add_to_rhs_adv_newton, elem_loc_dbc);
			// alt: not working
//			adv_rhs_add_newton = adv_matrix_simplified * remove_rows(collect_f_all.col(3), elem_loc_dbc);
			// end alt
//			Eigen::VectorXd adv_rhs_proj_newton = RB.transpose() * adv_rhs_add_newton;
//			adv_vec_proj_x_newton[trafo_iter] = adv_rhs_proj_newton;

			for(int RB_counter = 0; RB_counter < RBsize; RB_counter++)
			{			
				Eigen::VectorXd add_to_rhs_adv_newton_RB(M_truth_size); 
				add_to_rhs_adv_newton_RB = adv_matrix * PODmodes.col(RB_counter);      
//				add_to_rhs_adv_newton_RB = adv_matrix_simplified * RB.col(RB_counter);
				Eigen::VectorXd adv_rhs_add_newton = remove_rows(add_to_rhs_adv_newton_RB, elem_loc_dbc);
				Eigen::VectorXd adv_rhs_proj_newton = RB.transpose() * adv_rhs_add_newton;

				adv_vec_proj_x_newton_RB[trafo_iter].col(RB_counter) = adv_rhs_proj_newton;
			}
		}


		Eigen::VectorXd adv_rhs_add = remove_rows(add_to_rhs_adv, elem_loc_dbc);
		Eigen::MatrixXd adv_mat_proj = RB.transpose() * adv_matrix_simplified * RB;
		Eigen::VectorXd adv_rhs_proj = RB.transpose() * adv_rhs_add;

		adv_mats_proj_x[trafo_iter] = adv_mat_proj;
		adv_vec_proj_x[trafo_iter] = adv_rhs_proj;

		adv_vec_proj_y_newton_RB[trafo_iter] = Eigen::MatrixXd::Zero(RBsize,RBsize);
		DoInitialiseAdv(PhysBase_zero , curr_PhysBaseVec_y ); // call with parameter in phys state
		adv_matrix = Get_advection_matrix();
		add_to_rhs_adv = adv_matrix * f_bnd_dbc_full_size;   
		adv_matrix_simplified = remove_cols_and_rows(adv_matrix, elem_loc_dbc);
		adv_rhs_add = remove_rows(add_to_rhs_adv, elem_loc_dbc);
		adv_mat_proj = RB.transpose() * adv_matrix_simplified * RB;
		adv_rhs_proj = RB.transpose() * adv_rhs_add;
		adv_mats_proj_y[trafo_iter] = adv_mat_proj;
		adv_vec_proj_y[trafo_iter] = adv_rhs_proj;

		if (use_Newton)
		{
//			Eigen::VectorXd add_to_rhs_adv_newton(M_truth_size); 
//			add_to_rhs_adv_newton = adv_matrix  * PODmodes * PODmodes.transpose() *  collect_f_all.col(3);      
//			Eigen::VectorXd adv_rhs_add_newton = remove_rows(add_to_rhs_adv_newton, elem_loc_dbc);
//			Eigen::VectorXd adv_rhs_proj_newton = RB.transpose() * adv_rhs_add_newton;
//			adv_vec_proj_y_newton[trafo_iter] = adv_rhs_proj_newton;


			for(int RB_counter = 0; RB_counter < RBsize; RB_counter++)
			{			
				Eigen::VectorXd add_to_rhs_adv_newton_RB(M_truth_size); 
				add_to_rhs_adv_newton_RB = adv_matrix * PODmodes.col(RB_counter);      
//				add_to_rhs_adv_newton_RB = adv_matrix_simplified * RB.col(RB_counter);
				Eigen::VectorXd adv_rhs_add_newton = remove_rows(add_to_rhs_adv_newton_RB, elem_loc_dbc);
				Eigen::VectorXd adv_rhs_proj_newton = RB.transpose() * adv_rhs_add_newton;

				adv_vec_proj_y_newton_RB[trafo_iter].col(RB_counter) = adv_rhs_proj_newton;
			}
		}
	}
    }


    Eigen::MatrixXd CoupledLinearNS_TT::DoTrafo(Array<OneD, Array<OneD, NekDouble> > snapshot_x_collection, Array<OneD, Array<OneD, NekDouble> > snapshot_y_collection, Array<OneD, NekDouble> param_vector)
    {
	int Nmax = param_vector.num_elements();
	Eigen::MatrixXd collect_f_bnd( curr_f_bnd.size() , Nmax );
	Eigen::MatrixXd collect_f_p( curr_f_p.size() , Nmax );
	Eigen::MatrixXd collect_f_int( curr_f_int.size() , Nmax );
	for (int i=0; i<Nmax; i++)
	{
		Set_m_kinvis( param_vector[i] );	
//		cout << "CLNS_trafo.Get_m_kinvis " << CLNS_trafo.Get_m_kinvis() << endl;
	//	CLNS_trafo.DoInitialise();
		DoInitialiseAdv(snapshot_x_collection[i], snapshot_y_collection[i]); // replaces .DoInitialise();
		DoSolve();

		// compare the accuracy
		Array<OneD, MultiRegions::ExpListSharedPtr> m_fields_t = UpdateFields();
		m_fields_t[0]->BwdTrans(m_fields_t[0]->GetCoeffs(), m_fields_t[0]->UpdatePhys());
		m_fields_t[1]->BwdTrans(m_fields_t[1]->GetCoeffs(), m_fields_t[1]->UpdatePhys());
		Array<OneD, NekDouble> out_field_trafo_x(GetNpoints(), 0.0);
		Array<OneD, NekDouble> out_field_trafo_y(GetNpoints(), 0.0);

		Eigen::VectorXd csx0_trafo(GetNpoints());
		Eigen::VectorXd csy0_trafo(GetNpoints());
		Eigen::VectorXd csx0(GetNpoints());
		Eigen::VectorXd csy0(GetNpoints());

		CopyFromPhysField(0, out_field_trafo_x); 
		CopyFromPhysField(1, out_field_trafo_y);
		for( int index_conv = 0; index_conv < GetNpoints(); ++index_conv)
		{
			csx0_trafo(index_conv) = out_field_trafo_x[index_conv];
			csy0_trafo(index_conv) = out_field_trafo_y[index_conv];
			csx0(index_conv) = snapshot_x_collection[i][index_conv];
			csy0(index_conv) = snapshot_y_collection[i][index_conv];
		}

		if (debug_mode)
		{
			cout << "csx0.norm() " << csx0.norm() << endl;
			cout << "csx0_trafo.norm() " << csx0_trafo.norm() << endl;
			cout << "csy0.norm() " << csy0.norm() << endl;
			cout << "csy0_trafo.norm() " << csy0_trafo.norm() << endl;
		}
		
		Eigen::VectorXd trafo_f_bnd = curr_f_bnd;
		Eigen::VectorXd trafo_f_p = curr_f_p;
		Eigen::VectorXd trafo_f_int = curr_f_int;

		collect_f_bnd.col(i) = trafo_f_bnd;
		collect_f_p.col(i) = trafo_f_p;
		collect_f_int.col(i) = trafo_f_int;

	}
	Eigen::MatrixXd collect_f_all( curr_f_bnd.size()+curr_f_p.size()+curr_f_int.size() , Nmax );
	collect_f_all.block(0,0,collect_f_bnd.rows(),collect_f_bnd.cols()) = collect_f_bnd;
	collect_f_all.block(collect_f_bnd.rows(),0,collect_f_p.rows(),collect_f_p.cols()) = collect_f_p;
	collect_f_all.block(collect_f_bnd.rows()+collect_f_p.rows(),0,collect_f_int.rows(),collect_f_int.cols()) = collect_f_int;

	return collect_f_all;
    }

    double CoupledLinearNS_TT::Geo_T(double w, int elemT, int index)
    {
/*
def Geo_T(w, elemT, index): # index 0: det, index 1,2,3,4: mat_entries
	if elemT == 0:
		T = np.matrix([[1, 0], [0, 1/w]])
	if elemT == 1:
		T = np.matrix([[1, 0], [0, 2/(3-w)]])
	if elemT == 2:
		T = np.matrix([[1, 0], [-(1-w)/2, 1]])
	if elemT == 3:
		T = np.matrix([[1, 0], [-(w-1)/2, 1]])
	if elemT == 4:
		T = np.matrix([[1, 0], [0, 1]])			
	if index == 0:
		return 1/np.linalg.det(T)
	if index == 1:
		return T[0,0]
	if index == 2:
		return T[0,1]
	if index == 3:
		return T[1,0]
	if index == 4:
		return T[1,1]


*/
	Eigen::Matrix2d T;
	if (elemT == 0) 
	{
		T << 1, 0, 0, 1/w;
	}
	else if (elemT == 1) 
	{
		T << 1, 0, 0, 2/(3-w);
	}
	else if (elemT == 2) 
	{
		T << 1, 0, -(1-w)/2, 1;
	}
	else if (elemT == 3) 
	{
		T << 1, 0, -(w-1)/2, 1;
	}
	else if (elemT == 4) 
	{
		T << 1, 0, 0, 1;
	}

//	cout << T << endl;

	if (index == 0) 
	{
		return (T(0,0)*T(1,1) - T(0,1)*T(1,0)); // det
	}
	else if (index == 1) 
	{
		return T(0,0);
	}
	else if (index == 2) 
	{
		return T(0,1);
	}
	else if (index == 3) 
	{
		return T(1,0);
	}
	else if (index == 4) 
	{
		return T(1,1);
	}

	return 0;


    }


    Eigen::MatrixXd CoupledLinearNS_TT::project_onto_basis( Array<OneD, NekDouble> snapshot_x, Array<OneD, NekDouble> snapshot_y)
    {
	Eigen::VectorXd c_snapshot_x(GetNpoints());
	Eigen::VectorXd c_snapshot_y(GetNpoints());
	for (int i = 0; i < GetNpoints(); ++i)
	{
		c_snapshot_x(i) = snapshot_x[i];
		c_snapshot_y(i) = snapshot_y[i];
	}
	Eigen::VectorXd curr_x_proj = eigen_phys_basis_x.transpose() * c_snapshot_x;
	Eigen::VectorXd curr_y_proj = eigen_phys_basis_y.transpose() * c_snapshot_y;
	curr_xy_projected = Eigen::MatrixXd::Zero(curr_x_proj.rows(), 2);
	curr_xy_projected.col(0) = curr_x_proj;
	curr_xy_projected.col(1) = curr_y_proj;

	return curr_xy_projected;

    }

    int CoupledLinearNS_TT::get_curr_elem_pos(int curr_elem)
    {
	// find within Array<OneD, std::set<int> > elements_trafo; the current entry
	for(int i = 0; i < elements_trafo.num_elements(); ++i)
	{
		if (elements_trafo[i].count(curr_elem))
		{
			return i;
		}
	}
    }

    void CoupledLinearNS_TT::trafo_current_para(Array<OneD, NekDouble> snapshot_x, Array<OneD, NekDouble> snapshot_y, Array<OneD, NekDouble> parameter_of_interest)
    {

	double w = parameter_of_interest[0];	
	double mKinvis = parameter_of_interest[1];
	StdRegions::StdExpansionSharedPtr locExp;
        Array<OneD, unsigned int> bmap,imap; 

	// verify and transform to bnd / p / int the snapshot data

        int nz_loc;
        nz_loc = 1;

        int  nel  = m_fields[m_velocity[0]]->GetNumElmts();
        int  nvel   = m_velocity.num_elements();
        int nsize_bndry = nvel*m_fields[m_velocity[0]]->GetExp(0)->NumBndryCoeffs()*nz_loc;
        int nsize_bndry_p1 = nsize_bndry+nz_loc;
        int nsize_int = (nvel*m_fields[m_velocity[0]]->GetExp(0)->GetNcoeffs()*nz_loc - nsize_bndry);
        int nsize_p = m_pressure->GetExp(0)->GetNcoeffs()*nz_loc;
        int nsize_p_m1 = nsize_p-nz_loc;
        int Ahrows = nsize_bndry_p1;

	for (int curr_elem = 0; curr_elem < m_fields[0]->GetNumElmts(); ++curr_elem)
	{
		int curr_elem_pos = get_curr_elem_pos(curr_elem);
		double detT = Geo_T(w, curr_elem_pos, 0);
		double Ta = Geo_T(w, curr_elem_pos, 1);
		double Tb = Geo_T(w, curr_elem_pos, 2);
		double Tc = Geo_T(w, curr_elem_pos, 3);
		double Td = Geo_T(w, curr_elem_pos, 4);
		double c00 = Ta*Ta + Tb*Tb;
		double c01 = Ta*Tc + Tb*Td;
		double c11 = Tc*Tc + Td*Td;
                locExp = m_fields[m_velocity[0]]->GetExp(curr_elem);
                locExp->GetBoundaryMap(bmap);
                locExp->GetInteriorMap(imap);
                int ncoeffs = m_fields[m_velocity[0]]->GetExp(curr_elem)->GetNcoeffs();
                int nphys   = m_fields[m_velocity[0]]->GetExp(curr_elem)->GetTotPoints();
		int pqsize  = m_pressure->GetExp(curr_elem)->GetTotPoints();

//		cout << "ncoeffs " << ncoeffs << endl;
//		cout << "nphys " << nphys << endl;
//		cout << "pqsize " << pqsize << endl;   // pqsize == nphys and ncoeffs == nphys / 2 when?

                int nbmap = bmap.num_elements();
                int nimap = imap.num_elements();
		Array<OneD, double> curr_snap_x_part(nphys, 0.0);
		Array<OneD, double> curr_snap_y_part(nphys, 0.0);
		for (int i = 0; i < nphys; ++i)
		{
			curr_snap_x_part[i] = snapshot_x[curr_elem*nphys + i];
			curr_snap_y_part[i] = snapshot_y[curr_elem*nphys + i];
		}

		Array<OneD, double> Ah_ele_vec(Ahrows*Ahrows, 0.0);
		Array<OneD, double> B_ele_vec(nsize_bndry*nsize_int, 0.0);
		Array<OneD, double> C_ele_vec(nsize_bndry*nsize_int, 0.0);
		Array<OneD, double> D_ele_vec(nsize_int*nsize_int, 0.0);
		Array<OneD, double> Dbnd_ele_vec(nsize_p*nsize_bndry, 0.0);
		Array<OneD, double> Dint_ele_vec(nsize_p*nsize_int, 0.0);

		for (int i = 0; i < nbmap; ++i)
		{
			Array<OneD, double> coeffs(ncoeffs, 0.0);
			Array<OneD, double> adv_x_coeffs(ncoeffs, 0.0);
			Array<OneD, double> adv_y_coeffs(ncoeffs, 0.0);
			Array<OneD, double> coeffs_0_0(ncoeffs, 0.0);
			Array<OneD, double> coeffs_0_1(ncoeffs, 0.0);
			Array<OneD, double> coeffs_1_0(ncoeffs, 0.0);
			Array<OneD, double> coeffs_1_1(ncoeffs, 0.0);
			Array<OneD, double> phys(nphys, 0.0);
			Array<OneD, double> deriv_0(pqsize, 0.0);
			Array<OneD, double> deriv_1(pqsize, 0.0);

			coeffs[bmap[i]] = 1.0;
			m_fields[m_velocity[0]]->GetExp(curr_elem)->BwdTrans(coeffs,phys);
			locExp->PhysDeriv(MultiRegions::DirCartesianMap[0], phys, deriv_0);
			locExp->PhysDeriv(MultiRegions::DirCartesianMap[1], phys, deriv_1);

			locExp->IProductWRTDerivBase(0, deriv_0, coeffs_0_0);
			locExp->IProductWRTDerivBase(1, deriv_0, coeffs_0_1);
			locExp->IProductWRTDerivBase(0, deriv_1, coeffs_1_0);
			locExp->IProductWRTDerivBase(1, deriv_1, coeffs_1_1);

			for (int k = 0; k < 2; ++k)
			{
				for (int j = 0; j < nbmap; ++j)
				{
					Ah_ele_vec[ i+k*nbmap + (j+k*nbmap)*Ahrows ] += mKinvis * detT * ( c00*coeffs_0_0[int(bmap[j])] + c01*(coeffs_0_1[int(bmap[j])] + coeffs_1_0[int(bmap[j])]) + c11*coeffs_1_1[int(bmap[j])] );
				}
				for (int j = 0; j < nimap; ++j)
				{
					B_ele_vec[i+k*nbmap + (j+k*nimap)*nsize_bndry] += mKinvis * detT * ( c00*coeffs_0_0[int(imap[j])] + c01*(coeffs_0_1[int(imap[j])] + coeffs_1_0[int(imap[j])]) + c11*coeffs_1_1[int(imap[j])] );
				}
				if (k == 0)
				{
					Array<OneD, double> tmpphys_x(nphys, 0.0);
					std::transform( curr_snap_x_part.begin(), curr_snap_x_part.end(), deriv_0.begin(), tmpphys_x.begin(),  std::multiplies<double>() ); 
/*					for (int il = 0; il < nphys; ++il)
					{
						cout << "curr_snap_x_part[il] " << curr_snap_x_part[il] << endl;
						cout << "deriv_0[il] " << deriv_0[il] << endl;
						cout << "tmpphys_x[il] " << tmpphys_x[il] << endl;
					} */
					Array<OneD, double> tmpphys_y(nphys, 0.0);
					std::transform( curr_snap_x_part.begin(), curr_snap_x_part.end(), deriv_1.begin(), tmpphys_y.begin(),  std::multiplies<double>() ); 
					locExp->IProductWRTBase(tmpphys_x, adv_x_coeffs);
					locExp->IProductWRTBase(tmpphys_y, adv_y_coeffs);
					for (int nv = 0; nv < 2; ++nv)
					{
						for (int j = 0; j < nbmap; ++j)
						{
							Ah_ele_vec[ j+nv*nbmap + (i+nv*nbmap)*Ahrows ] += detT * (Ta * adv_x_coeffs[int(bmap[j])] + Tc * adv_y_coeffs[int(bmap[j])]);
						}
						for (int j = 0; j < nimap; ++j)
						{
							C_ele_vec[ i+nv*nbmap + (j+nv*nimap)*nsize_bndry ] += detT * (Ta * adv_x_coeffs[int(imap[j])] + Tc * adv_y_coeffs[int(imap[j])]);
						}
					}
			            	int psize   = m_pressure->GetExp(curr_elem)->GetNcoeffs();
			               	Array<OneD, NekDouble> pcoeffs_x(psize);
			               	Array<OneD, NekDouble> pcoeffs_y(psize);
					m_pressure->GetExp(curr_elem)->IProductWRTBase(deriv_0,pcoeffs_x);
					m_pressure->GetExp(curr_elem)->IProductWRTBase(deriv_1,pcoeffs_y);
					for (int il = 0; il < nsize_p; ++il)
					{
						Dbnd_ele_vec[ (k*nbmap + i)*nsize_p + il ] = detT * (Ta * pcoeffs_x[il] + Tc * pcoeffs_y[il]);
					}
				}
				if (k == 1)
				{
					Array<OneD, double> tmpphys_x(nphys, 0.0);
					std::transform( curr_snap_y_part.begin(), curr_snap_y_part.end(), deriv_0.begin(), tmpphys_x.begin(),  std::multiplies<double>() ); 
/*					for (int il = 0; il < nphys; ++il)
					{
						cout << "curr_snap_x_part[il] " << curr_snap_x_part[il] << endl;
						cout << "deriv_0[il] " << deriv_0[il] << endl;
						cout << "tmpphys_x[il] " << tmpphys_x[il] << endl;
					} */
					Array<OneD, double> tmpphys_y(nphys, 0.0);
					std::transform( curr_snap_y_part.begin(), curr_snap_y_part.end(), deriv_1.begin(), tmpphys_y.begin(),  std::multiplies<double>() ); 
					locExp->IProductWRTBase(tmpphys_x, adv_x_coeffs);
					locExp->IProductWRTBase(tmpphys_y, adv_y_coeffs);
					for (int nv = 0; nv < 2; ++nv)
					{
						for (int j = 0; j < nbmap; ++j)
						{
							Ah_ele_vec[ j+nv*nbmap + (i+nv*nbmap)*Ahrows ] += detT * (Tb * adv_x_coeffs[int(bmap[j])] + Td * adv_y_coeffs[int(bmap[j])]);
						}
						for (int j = 0; j < nimap; ++j)
						{
							C_ele_vec[ i+nv*nbmap + (j+nv*nimap)*nsize_bndry ] += detT * (Tb * adv_x_coeffs[int(imap[j])] + Td * adv_y_coeffs[int(imap[j])]);
						}
					}
			            	int psize   = m_pressure->GetExp(curr_elem)->GetNcoeffs();
			               	Array<OneD, NekDouble> pcoeffs_x(psize);
			               	Array<OneD, NekDouble> pcoeffs_y(psize);
					m_pressure->GetExp(curr_elem)->IProductWRTBase(deriv_0,pcoeffs_x);
					m_pressure->GetExp(curr_elem)->IProductWRTBase(deriv_1,pcoeffs_y);
					for (int il = 0; il < nsize_p; ++il)
					{
						Dbnd_ele_vec[ (k*nbmap + i)*nsize_p + il ] = detT * (Tb * pcoeffs_x[il] + Td * pcoeffs_y[il]);
					}


				}
			} //for (int k = 0; k < 2; ++k)


		} // for (int i = 0; i < nbmap; ++i)
		for (int i = 0; i < nimap; ++i)
		{
			
			Array<OneD, double> coeffs(ncoeffs, 0.0);
			Array<OneD, double> adv_x_coeffs(ncoeffs, 0.0);
			Array<OneD, double> adv_y_coeffs(ncoeffs, 0.0);
			Array<OneD, double> coeffs_0_0(ncoeffs, 0.0);
			Array<OneD, double> coeffs_0_1(ncoeffs, 0.0);
			Array<OneD, double> coeffs_1_0(ncoeffs, 0.0);
			Array<OneD, double> coeffs_1_1(ncoeffs, 0.0);
			Array<OneD, double> phys(nphys, 0.0);
			Array<OneD, double> deriv_0(pqsize, 0.0);
			Array<OneD, double> deriv_1(pqsize, 0.0);
			coeffs[imap[i]] = 1.0;
			m_fields[m_velocity[0]]->GetExp(curr_elem)->BwdTrans(coeffs,phys);
			locExp->PhysDeriv(MultiRegions::DirCartesianMap[0], phys, deriv_0);
			locExp->PhysDeriv(MultiRegions::DirCartesianMap[1], phys, deriv_1);

			locExp->IProductWRTDerivBase(0, deriv_0, coeffs_0_0);
			locExp->IProductWRTDerivBase(1, deriv_0, coeffs_0_1);
			locExp->IProductWRTDerivBase(0, deriv_1, coeffs_1_0);
			locExp->IProductWRTDerivBase(1, deriv_1, coeffs_1_1);

			for (int k = 0; k < 2; ++k)
			{
				for (int j = 0; j < nbmap; ++j)
				{
					C_ele_vec[ j+k*nbmap + (i+k*nimap)*nsize_bndry ] += mKinvis * detT * ( c00*coeffs_0_0[int(bmap[j])] + c01*(coeffs_0_1[int(bmap[j])] + coeffs_1_0[int(bmap[j])]) + c11*coeffs_1_1[int(bmap[j])] );
				}
				for (int j = 0; j < nimap; ++j)
				{
					D_ele_vec[i+k*nimap + (j+k*nimap)*nsize_int] += mKinvis * detT * ( c00*coeffs_0_0[int(imap[j])] + c01*(coeffs_0_1[int(imap[j])] + coeffs_1_0[int(imap[j])]) + c11*coeffs_1_1[int(imap[j])] );
				}
				if (k == 0)
				{
					Array<OneD, double> tmpphys_x(nphys, 0.0);
					std::transform( curr_snap_x_part.begin(), curr_snap_x_part.end(), deriv_0.begin(), tmpphys_x.begin(),  std::multiplies<double>() ); 
					Array<OneD, double> tmpphys_y(nphys, 0.0);
					std::transform( curr_snap_x_part.begin(), curr_snap_x_part.end(), deriv_1.begin(), tmpphys_y.begin(),  std::multiplies<double>() ); 
					locExp->IProductWRTBase(tmpphys_x, adv_x_coeffs);
					locExp->IProductWRTBase(tmpphys_y, adv_y_coeffs);
					for (int nv = 0; nv < 2; ++nv)
					{
						for (int j = 0; j < nbmap; ++j)
						{
							B_ele_vec[ j+nv*nbmap + (i+nv*nimap)*nsize_bndry ] += detT * (Ta * adv_x_coeffs[int(bmap[j])] + Tc * adv_y_coeffs[int(bmap[j])]);
						}
						for (int j = 0; j < nimap; ++j)
						{
							D_ele_vec[ j+nv*nimap + (i+nv*nimap)*nsize_int ]  += detT * (Ta * adv_x_coeffs[int(imap[j])] + Tc * adv_y_coeffs[int(imap[j])]);
						}
					}
			            	int psize   = m_pressure->GetExp(curr_elem)->GetNcoeffs();
			               	Array<OneD, NekDouble> pcoeffs_x(psize);
			               	Array<OneD, NekDouble> pcoeffs_y(psize);
					m_pressure->GetExp(curr_elem)->IProductWRTBase(deriv_0,pcoeffs_x);
					m_pressure->GetExp(curr_elem)->IProductWRTBase(deriv_1,pcoeffs_y);
					for (int il = 0; il < nsize_p; ++il)
					{
						Dint_ele_vec[ (k*nimap + i)*nsize_p + il ] = detT * (Ta * pcoeffs_x[il] + Tc * pcoeffs_y[il]);
					}
				}
				if (k == 1)
				{
					Array<OneD, double> tmpphys_x(nphys, 0.0);
					std::transform( curr_snap_y_part.begin(), curr_snap_y_part.end(), deriv_0.begin(), tmpphys_x.begin(),  std::multiplies<double>() ); 
					Array<OneD, double> tmpphys_y(nphys, 0.0);
					std::transform( curr_snap_y_part.begin(), curr_snap_y_part.end(), deriv_1.begin(), tmpphys_y.begin(),  std::multiplies<double>() ); 
					locExp->IProductWRTBase(tmpphys_x, adv_x_coeffs);
					locExp->IProductWRTBase(tmpphys_y, adv_y_coeffs);
					for (int nv = 0; nv < 2; ++nv)
					{
						for (int j = 0; j < nbmap; ++j)
						{
							B_ele_vec[ j+nv*nbmap + (i+nv*nimap)*nsize_bndry ] += detT * (Tb * adv_x_coeffs[int(bmap[j])] + Td * adv_y_coeffs[int(bmap[j])]);
						}
						for (int j = 0; j < nimap; ++j)
						{
							D_ele_vec[ j+nv*nimap + (i+nv*nimap)*nsize_int ]  += detT * (Tb * adv_x_coeffs[int(imap[j])] + Td * adv_y_coeffs[int(imap[j])]);
						}
					}
			            	int psize   = m_pressure->GetExp(curr_elem)->GetNcoeffs();
			               	Array<OneD, NekDouble> pcoeffs_x(psize);
			               	Array<OneD, NekDouble> pcoeffs_y(psize);
					m_pressure->GetExp(curr_elem)->IProductWRTBase(deriv_0,pcoeffs_x);
					m_pressure->GetExp(curr_elem)->IProductWRTBase(deriv_1,pcoeffs_y);
					for (int il = 0; il < nsize_p; ++il)
					{
						Dint_ele_vec[ (k*nimap + i)*nsize_p + il ] = detT * (Tb * pcoeffs_x[il] + Td * pcoeffs_y[il]);
					}
				}
			} //for (int k = 0; k < 2; ++k)


		}
 
	}

/*

		for i in range(0, nimap):
			coeffs = 0*np.arange(ncoeffs*1.0)
			coeffs[int(imap[i])] = 1.0
			phys = np.dot(coeffs, loc_bwd_mat)
			deriv_0 = np.dot(phys, (loc_cm0))
			deriv_1 = np.dot(phys, (loc_cm1))
			coeffs_0_0 = np.dot(deriv_0, loc_IP_d0)
			coeffs_0_1 = np.dot(deriv_0, loc_IP_d1)
			coeffs_1_0 = np.dot(deriv_1, loc_IP_d0)
			coeffs_1_1 = np.dot(deriv_1, loc_IP_d1)
			for k in range(0, 2):
				for j in range(0, nbmap):
					C_ele_vec[j+k*nbmap + (i+k*nimap)*nsize_bndry] += mKinvis * detT * ( c00*coeffs_0_0[int(bmap[j])] + c01*(coeffs_0_1[int(bmap[j])] + coeffs_1_0[int(bmap[j])]) + c11*coeffs_1_1[int(bmap[j])] )
				for j in range(0, nimap):
					D_ele_vec[i+k*nimap + (j+k*nimap)*nsize_int] += mKinvis * detT * ( c00*coeffs_0_0[int(imap[j])] + c01*(coeffs_0_1[int(imap[j])] + coeffs_1_0[int(imap[j])]) + c11*coeffs_1_1[int(imap[j])] )
			for k in range(0, 2):
				if (k == 0):
					deriv = np.dot(phys, (loc_cm0))
					deriv_y = np.dot(phys, (loc_cm1))
					tmpphys = u_x[(curr_elem*nphys) : (curr_elem*nphys + nphys)] * deriv
					tmpphys_y = u_x[(curr_elem*nphys) : (curr_elem*nphys + nphys)] * deriv_y
					coeffs = np.dot(tmpphys, loc_IP)
					coeffs_y = np.dot(tmpphys_y, loc_IP)
					for nv in range(0, 2):
						for j in range(0, nbmap):
							B_ele_vec[ j+nv*nbmap + (i+nv*nimap)*nsize_bndry ] += detT * (Ta * coeffs[int(bmap[j])] + Tc * coeffs_y[int(bmap[j])])
						for j in range(0, nimap):
							D_ele_vec[ j+nv*nimap + (i+nv*nimap)*nsize_int ] += detT * (Ta * coeffs[int(imap[j])] + Tc * coeffs_y[int(imap[j])])
					pcoeff = np.dot(deriv, loc_IPp)
					pcoeff_y = np.dot(deriv_y, loc_IPp)
					Dint_ele_vec[ (k*nimap + i)*nsize_p : ((k*nimap + i)*nsize_p + nsize_p) ] = detT * (Ta * pcoeff + Tc * pcoeff_y)
				if (k == 1):
					deriv = np.dot(phys, (loc_cm1))
					tmpphys = u_y[(curr_elem*nphys) : (curr_elem*nphys + nphys)] * deriv
					coeffs = np.dot(tmpphys, loc_IP)
					for nv in range(0, 2):
						for j in range(0, nbmap):
							B_ele_vec[ j+nv*nbmap + (i+nv*nimap)*nsize_bndry ] += detT * (Tb * coeffs[int(bmap[j])] + Td * coeffs[int(bmap[j])])
						for j in range(0, nimap):
							D_ele_vec[ j+nv*nimap + (i+nv*nimap)*nsize_int ] += detT * (Tb * coeffs[int(imap[j])] + Td * coeffs[int(imap[j])])
					pcoeff = np.dot(deriv, loc_IPp)
					Dint_ele_vec[ (k*nimap + i)*nsize_p : ((k*nimap + i)*nsize_p + nsize_p) ] = detT * (Tb * pcoeff + Td * pcoeff)

		for i in range(0, nbmap):
			coeffs = 0*np.arange(ncoeffs*1.0)
			coeffs[int(bmap[i])] = 1.0
			phys = np.dot(coeffs, loc_bwd_mat)
			deriv_0 = np.dot(phys, (loc_cm0))
			deriv_1 = np.dot(phys, (loc_cm1))
			coeffs_0_0 = np.dot(deriv_0, loc_IP_d0)
			coeffs_0_1 = np.dot(deriv_0, loc_IP_d1)
			coeffs_1_0 = np.dot(deriv_1, loc_IP_d0)
			coeffs_1_1 = np.dot(deriv_1, loc_IP_d1)
			for k in range(0, 2):
				for j in range(0, nbmap):
					Ah_ele_vec[ i+k*nbmap + (j+k*nbmap)*Ahrows ] += mKinvis * detT * ( c00*coeffs_0_0[int(bmap[j])] + c01*(coeffs_0_1[int(bmap[j])] + coeffs_1_0[int(bmap[j])]) + c11*coeffs_1_1[int(bmap[j])] )
				for j in range(0, nimap):
					B_ele_vec[i+k*nbmap + (j+k*nimap)*nsize_bndry] += mKinvis * detT * ( c00*coeffs_0_0[int(imap[j])] + c01*(coeffs_0_1[int(imap[j])] + coeffs_1_0[int(imap[j])]) + c11*coeffs_1_1[int(imap[j])] )
			for k in range(0, 2):
				if (k == 0):
					deriv = np.dot(phys, (loc_cm0))
					deriv_y = np.dot(phys, (loc_cm1))
					tmpphys = u_x[(curr_elem*nphys) : (curr_elem*nphys + nphys)] * deriv
					tmpphys_y = u_x[(curr_elem*nphys) : (curr_elem*nphys + nphys)] * deriv_y
					coeffs = np.dot(tmpphys, loc_IP)
					coeffs_y = np.dot(tmpphys_y, loc_IP)
					for nv in range(0, 2):
						for j in range(0, nbmap):
							Ah_ele_vec[ j+nv*nbmap + (i+nv*nbmap)*Ahrows ] += detT * (Ta * coeffs[int(bmap[j])] + Tc * coeffs_y[int(bmap[j])])
						for j in range(0, nimap):
							C_ele_vec[ i+nv*nbmap + (j+nv*nimap)*nsize_bndry ] += detT * (Ta * coeffs[int(imap[j])] + Tc * coeffs_y[int(imap[j])])
					pcoeff = np.dot(deriv, loc_IPp)
					pcoeff_y = np.dot(deriv_y, loc_IPp)
					Dbnd_ele_vec[ (k*nbmap + i)*nsize_p : ((k*nbmap + i)*nsize_p + nsize_p) ] = detT * (Ta * pcoeff + Tc * pcoeff_y)
				if (k == 1):
					deriv = np.dot(phys, (loc_cm1))
					tmpphys = u_y[(curr_elem*nphys) : (curr_elem*nphys + nphys)] * deriv
					coeffs = np.dot(tmpphys, loc_IP)
					for nv in range(0, 2):
						for j in range(0, nbmap):
							Ah_ele_vec[ j+nv*nbmap + (i+nv*nbmap)*Ahrows ] += detT * (Tb * coeffs[int(bmap[j])] + Td * coeffs[int(bmap[j])])
						for j in range(0, nimap):
							C_ele_vec[ i+nv*nbmap + (j+nv*nimap)*nsize_bndry ] += detT * (Tb * coeffs[int(imap[j])] + Td * coeffs[int(imap[j])])
					pcoeff = np.dot(deriv, loc_IPp) 
					Dbnd_ele_vec[ (k*nbmap + i)*nsize_p : ((k*nbmap + i)*nsize_p + nsize_p) ] = detT * (Tb * pcoeff + Td * pcoeff)											


*/

/*	for curr_elem in range(0, num_elem):
		Ah_ele_vec = 0*np.arange(Ahrows*Ahrows*1.0)
		H1_bnd_ele_vec = 0*np.arange(Ahrows*Ahrows*1.0)
		B_ele_vec = 0*np.arange(nsize_bndry*nsize_int*1.0)
		C_ele_vec = 0*np.arange(nsize_bndry*nsize_int*1.0)
		D_ele_vec = 0*np.arange(nsize_int*nsize_int*1.0)
		DnoK_ele_vec = 0*np.arange(nsize_int*nsize_int*1.0)
		Dadv_ele_vec = 0*np.arange(nsize_int*nsize_int*1.0)
		H1_int_ele_vec = 0*np.arange(nsize_int*nsize_int*1.0)
		D1_ele_vec = 0*np.arange(nsize_int*nsize_int*1.0)
		Dbnd_ele_vec = 0*np.arange(nsize_bndry*nsize_p*1.0)
		Dint_ele_vec = 0*np.arange(nsize_int*nsize_p*1.0)
		loc_bwd_mat = bwdtrans[(curr_elem*ncoeffs) : (curr_elem*ncoeffs + ncoeffs) ,:]
		loc_cm0 = cartmap0[(curr_elem*nphys) : (curr_elem*nphys + nphys) ,:]
		loc_cm1 = cartmap1[(curr_elem*nphys) : (curr_elem*nphys + nphys) ,:]
		loc_IP = IP[(curr_elem*nphys) : (curr_elem*nphys + nphys) ,:]
		loc_IP_d0 = IP_d0[(curr_elem*nphys) : (curr_elem*nphys + nphys) ,:]
		loc_IP_d1 = IP_d1[(curr_elem*nphys) : (curr_elem*nphys + nphys) ,:]
		loc_IPp = IPp[(curr_elem*nphys) : (curr_elem*nphys + nphys) ,:]
		HelmMat = HelmMats[curr_elem,:]
		L00 = Lapl00[curr_elem,:]
		L11 = Lapl11[curr_elem,:]
		mimic_L00 = 0*Lapl00[curr_elem,:]
		curr_elem_pos = get_curr_elem_pos(curr_elem, geo_trafo_list)
		detT = Geo_T(w, curr_elem_pos, 0)
		Ta = Geo_T(w, curr_elem_pos, 1)
		Tb = Geo_T(w, curr_elem_pos, 2)
		Tc = Geo_T(w, curr_elem_pos, 3)
		Td = Geo_T(w, curr_elem_pos, 4)
		c00 = Ta*Ta + Tb*Tb
		c01 = Ta*Tc + Tb*Td
		c11 = Tc*Tc + Td*Td			
*/
    }


    void CoupledLinearNS_TT::do_geo_trafo()
    {
 
	Eigen::MatrixXd collect_f_bnd( curr_f_bnd.size() , Nmax );
	Eigen::MatrixXd collect_f_p( curr_f_p.size() , Nmax );
	Eigen::MatrixXd collect_f_int( curr_f_int.size() , Nmax );
        for(int i = 0; i < Nmax; ++i)
	{
//		cout << "general_param_vector[i][0] " << general_param_vector[i][0] << endl;
//		cout << "general_param_vector[i][1] " << general_param_vector[i][1] << endl;
//		cout << "curr_f_bnd.size() " << curr_f_bnd.size() << endl;
//		cout << "curr_f_p.size() " << curr_f_p.size() << endl;
//		cout << "curr_f_int.size() " << curr_f_int.size() << endl;
		// identify the geometry one - introduce in the .h some vars keeping the para_type
		// here now [0] is geometry 'w' and [1] is k_invis
		trafo_current_para(snapshot_x_collection[i], snapshot_y_collection[i], general_param_vector[i]); // setting collect_f_all, making use of snapshot_x_collection, snapshot_y_collection
	}
    }

    void CoupledLinearNS_TT::compute_snapshots_geometry_params()
    {
	// generate matrices for affine form 
	// do full order solves using the affine form
	// can check that against simulations with appropriate .xml
	// cout << "starting compute geometry snapshots" << endl;
	// Nmax should be available as the total number of to be computed snapshots
	Array<OneD, NekDouble> zero_phys_init(GetNpoints(), 0.0);
	snapshot_x_collection = Array<OneD, Array<OneD, NekDouble> > (Nmax);
	snapshot_y_collection = Array<OneD, Array<OneD, NekDouble> > (Nmax);
        for(int i = 0; i < Nmax; ++i)
	{
		// what is the current parameter vector ?
		// cout << "general_param_vector[i][0] " << general_param_vector[i][0] << endl;
		// cout << "general_param_vector[i][1] " << general_param_vector[i][1] << endl;
		
		// assuming the loaded configuration is the reference config...
		// but then, how to solve this ?? -- do not have a solvable large-scale system ??
		cout << "Error: compute_snapshots_geometry_params() not implemented :) " << endl;
	}
    }

    void CoupledLinearNS_TT::online_phase()
    {
	Eigen::MatrixXd mat_compare = Eigen::MatrixXd::Zero(f_bnd_dbc_full_size.rows(), 3);  // is of size M_truth_size
	// start sweeping 
	for (int iter_index = 0; iter_index < Nmax; ++iter_index)
	{
		int current_index = iter_index;
		double current_nu = param_vector[current_index];
		Set_m_kinvis( current_nu );
		if (use_Newton)
		{
			DoInitialiseAdv(snapshot_x_collection[current_index], snapshot_y_collection[current_index]);
		}
		Eigen::MatrixXd curr_xy_proj = project_onto_basis(snapshot_x_collection[current_index], snapshot_y_collection[current_index]);
		Eigen::MatrixXd affine_mat_proj = gen_affine_mat_proj(current_nu);
		Eigen::VectorXd affine_vec_proj = gen_affine_vec_proj(current_nu, current_index);
		Eigen::VectorXd solve_affine = affine_mat_proj.colPivHouseholderQr().solve(affine_vec_proj);
		cout << "solve_affine " << solve_affine << endl;
		Eigen::VectorXd repro_solve_affine = RB * solve_affine;
		Eigen::VectorXd reconstruct_solution = reconstruct_solution_w_dbc(repro_solve_affine);
		if (globally_connected == 1)
		{
			mat_compare.col(0) = M_collect_f_all.col(current_index);
		}
		else
		{
			mat_compare.col(0) = collect_f_all.col(current_index);
			if (debug_mode)
			{
				Eigen::VectorXd current_f_all = Eigen::VectorXd::Zero(collect_f_all.rows());
				current_f_all = collect_f_all.col(current_index);
				Eigen::VectorXd current_f_all_wo_dbc = remove_rows(current_f_all, elem_loc_dbc);
				Eigen::VectorXd proj_current_f_all_wo_dbc = RB.transpose() * current_f_all_wo_dbc;
				cout << "proj_current_f_all_wo_dbc " << proj_current_f_all_wo_dbc << endl;
				Eigen::VectorXd correctRHS = affine_mat_proj * proj_current_f_all_wo_dbc;
				Eigen::VectorXd correction_RHS = correctRHS - affine_vec_proj;
				cout << "correctRHS " << correctRHS << endl;
				cout << "correction_RHS " << correction_RHS << endl;
			}
		}
		mat_compare.col(1) = reconstruct_solution; // sembra abbastanza bene
		mat_compare.col(2) = mat_compare.col(1) - mat_compare.col(0);
//		cout << mat_compare << endl;
		cout << "relative error norm: " << mat_compare.col(2).norm() / mat_compare.col(0).norm() << " of snapshot number " << iter_index << endl;


		if (debug_mode)
		{
			cout << "snapshot_x_collection.num_elements() " << snapshot_x_collection.num_elements() << " snapshot_x_collection[0].num_elements() " << snapshot_x_collection[0].num_elements() << endl;
		}

		if (write_ROM_field)
		{
			recover_snapshot_data(reconstruct_solution, current_index);
		}


	}
    }

    void CoupledLinearNS_TT::recover_snapshot_data(Eigen::VectorXd reconstruct_solution, int current_index)
    {

	Eigen::VectorXd f_bnd = reconstruct_solution.head(curr_f_bnd.size());
	Eigen::VectorXd f_int = reconstruct_solution.tail(curr_f_int.size());
	Array<OneD, MultiRegions::ExpListSharedPtr> fields = UpdateFields(); 
	Array<OneD, unsigned int> bmap, imap; 
	Array<OneD, double> field_0(GetNcoeffs());
	Array<OneD, double> field_1(GetNcoeffs());
	Array<OneD, double> curr_PhysBaseVec_x(GetNpoints(), 0.0);
	Array<OneD, double> curr_PhysBaseVec_y(GetNpoints(), 0.0);
	int cnt = 0;
	int cnt1 = 0;
	int nvel = 2;
	int nz_loc = 1;
	int  nplanecoeffs = fields[0]->GetNcoeffs();
	int  nel  = m_fields[0]->GetNumElmts();
	for(int i = 0; i < nel; ++i) 
	{
	      int eid  = i;
	      fields[0]->GetExp(eid)->GetBoundaryMap(bmap);
	      fields[0]->GetExp(eid)->GetInteriorMap(imap);
	      int nbnd   = bmap.num_elements();
	      int nint   = imap.num_elements();
	      int offset = fields[0]->GetCoeff_Offset(eid);
	            
	      for(int j = 0; j < nvel; ++j)
	      {
	           for(int n = 0; n < nz_loc; ++n)
	           {
	                    for(int k = 0; k < nbnd; ++k)
	                    {
	                        fields[j]->SetCoeff(n*nplanecoeffs + offset+bmap[k], f_bnd(cnt+k));
	                    }
	                    
	                    for(int k = 0; k < nint; ++k)
	                    {
	                        fields[j]->SetCoeff(n*nplanecoeffs + offset+imap[k], f_int(cnt1+k));
	                    }
	                    cnt  += nbnd;
	                    cnt1 += nint;
	           }
	      }
	}
	Array<OneD, double> test_nn = fields[0]->GetCoeffs();
	fields[0]->BwdTrans_IterPerExp(fields[0]->GetCoeffs(), curr_PhysBaseVec_x);
	fields[1]->BwdTrans_IterPerExp(fields[1]->GetCoeffs(), curr_PhysBaseVec_y);

	Eigen::VectorXd eigen_phys_basis_x = Eigen::VectorXd::Zero(GetNpoints());
	Eigen::VectorXd eigen_phys_basis_y = Eigen::VectorXd::Zero(GetNpoints());
	Eigen::VectorXd eigen_phys_basis_x_snap = Eigen::VectorXd::Zero(GetNpoints());
	Eigen::VectorXd eigen_phys_basis_y_snap = Eigen::VectorXd::Zero(GetNpoints());

	for (int index_phys_base=0; index_phys_base<GetNpoints(); index_phys_base++)
	{
		eigen_phys_basis_x(index_phys_base) = curr_PhysBaseVec_x[index_phys_base];
		eigen_phys_basis_y(index_phys_base) = curr_PhysBaseVec_y[index_phys_base];
		
		eigen_phys_basis_x_snap(index_phys_base) = snapshot_x_collection[current_index][index_phys_base];
		eigen_phys_basis_y_snap(index_phys_base) = snapshot_y_collection[current_index][index_phys_base];

	}

	if (debug_mode)
	{
		cout << "eigen_phys_basis_x.norm() " << eigen_phys_basis_x.norm() << endl;
		cout << "eigen_phys_basis_x_snap.norm() " << eigen_phys_basis_x_snap.norm() << endl;
		cout << "(eigen_phys_basis_x - eigen_phys_basis_x_snap).norm() " << (eigen_phys_basis_x - eigen_phys_basis_x_snap).norm() << endl;
		cout << "eigen_phys_basis_y.norm() " << eigen_phys_basis_y.norm() << endl;
		cout << "eigen_phys_basis_y_snap.norm() " << eigen_phys_basis_y_snap.norm() << endl;
		cout << "(eigen_phys_basis_y - eigen_phys_basis_y_snap).norm() " << (eigen_phys_basis_y - eigen_phys_basis_y_snap).norm() << endl;
	}




        std::vector<Array<OneD, NekDouble> > fieldcoeffs(m_fields.num_elements()+1);
        std::vector<std::string> variables(m_fields.num_elements()+1);
        int i;
        
        for(i = 0; i < m_fields.num_elements(); ++i)
        {
            fieldcoeffs[i] = fields[i]->UpdateCoeffs();
            variables[i]   = m_boundaryConditions->GetVariable(i);
	    if (debug_mode)
	    {
		    cout << "variables[i] " << variables[i] << endl;
	    }
        }

	if (debug_mode)
	{
		cout << "m_singleMode " << m_singleMode << endl;	
	}

	fieldcoeffs[i] = Array<OneD, NekDouble>(m_fields[0]->GetNcoeffs(), 0.0);  
        variables[i] = "p"; 

	WriteFld("Test.fld",m_fields[0],fieldcoeffs,variables);

/*        fieldcoeffs[i] = Array<OneD, NekDouble>(m_fields[0]->GetNcoeffs());  
        // project pressure field to velocity space        
        if(m_singleMode==true)
        {
            Array<OneD, NekDouble > tmpfieldcoeffs (m_fields[0]->GetNcoeffs()/2);
            m_pressure->GetPlane(0)->BwdTrans_IterPerExp(m_pressure->GetPlane(0)->GetCoeffs(), m_pressure->GetPlane(0)->UpdatePhys());
            m_pressure->GetPlane(1)->BwdTrans_IterPerExp(m_pressure->GetPlane(1)->GetCoeffs(), m_pressure->GetPlane(1)->UpdatePhys()); 
            m_fields[0]->GetPlane(0)->FwdTrans_IterPerExp(m_pressure->GetPlane(0)->GetPhys(),fieldcoeffs[i]);
            m_fields[0]->GetPlane(1)->FwdTrans_IterPerExp(m_pressure->GetPlane(1)->GetPhys(),tmpfieldcoeffs);
            for(int e=0; e<m_fields[0]->GetNcoeffs()/2; e++)
            {
                fieldcoeffs[i][e+m_fields[0]->GetNcoeffs()/2] = tmpfieldcoeffs[e];
            }          
        }
        else
        {
            m_pressure->BwdTrans_IterPerExp(m_pressure->GetCoeffs(),m_pressure->UpdatePhys());
            m_fields[0]->FwdTrans_IterPerExp(m_pressure->GetPhys(),fieldcoeffs[i]);
        }
        variables[i] = "p"; 
        
        std::string outname = m_sessionName + ".fld";
        
        WriteFld(outname,m_fields[0],fieldcoeffs,variables);
*/




    }
	
    void CoupledLinearNS_TT::offline_phase()
    {
	int load_snapshot_data_from_files = m_session->GetParameter("load_snapshot_data_from_files");
	int number_of_snapshots = m_session->GetParameter("number_of_snapshots");
	if (m_session->DefinesParameter("parameter_space_dimension")) 
	{
		parameter_space_dimension = m_session->GetParameter("parameter_space_dimension");	
	}
	else
	{
		parameter_space_dimension = 1;
	}
	double POD_tolerance = m_session->GetParameter("POD_tolerance");
	ref_param_index = m_session->GetParameter("ref_param_index");
	ref_param_nu = m_session->GetParameter("ref_param_nu");
	if (m_session->DefinesParameter("globally_connected")) // this sets how the truth system global coupling is enforced
	{
		globally_connected = m_session->GetParameter("globally_connected");
	}
	else
	{
		globally_connected = 0;
	}
	if (m_session->DefinesParameter("use_Newton")) // set if Newton or Oseen iteration
	{
		use_Newton = m_session->GetParameter("use_Newton");
	}
	else
	{
		use_Newton = 0;
	}
	if (m_session->DefinesParameter("debug_mode")) // debug_mode with many extra information but very slow
	{
		debug_mode = m_session->GetParameter("debug_mode");
	}
	else
	{
		debug_mode = 0;
	} 
	if (m_session->DefinesParameter("write_ROM_field")) 
	{
		write_ROM_field = m_session->GetParameter("write_ROM_field");
	}
	else
	{
		write_ROM_field = 0;
	} 
	if (m_session->DefinesParameter("snapshot_computation_plot_rel_errors")) 
	{
		snapshot_computation_plot_rel_errors = m_session->GetParameter("snapshot_computation_plot_rel_errors");
	}
	else
	{
		snapshot_computation_plot_rel_errors = 0;
	} 
	Nmax = number_of_snapshots;
	if (parameter_space_dimension == 1)
	{
		param_vector = Array<OneD, NekDouble> (Nmax);
	        for(int i = 0; i < number_of_snapshots; ++i)
	        {
			// generate the correct string
			std::stringstream sstm;
			sstm << "param" << i;
			std::string result = sstm.str();
		//	const char* rr = result.c_str();
		        param_vector[i] = m_session->GetParameter(result);
	        }
	}
	else if (parameter_space_dimension == 2)
	{
		general_param_vector = Array<OneD, Array<OneD, NekDouble> > (Nmax);
		int number_of_snapshots_dir0 = m_session->GetParameter("number_of_snapshots_dir0");
		int number_of_snapshots_dir1 = m_session->GetParameter("number_of_snapshots_dir1");
		Array<OneD, NekDouble> index_vector(parameter_space_dimension, 0.0);

//		for(int i = 0; i < parameter_space_dimension; ++i)
//		{
//			cout << "psdiv " << index_vector[i] << endl;
//		}
		int i_all = 0;
//		general_param_vector[i_all] = Array<OneD, NekDouble> (parameter_space_dimension);
		Array<OneD, NekDouble> parameter_point(parameter_space_dimension, 0.0);
		for(int i = 0; i < Nmax; ++i)
		{
			parameter_point = Array<OneD, NekDouble> (parameter_space_dimension, 0.0);
			general_param_vector[i] = parameter_point;
		}
		for(int i0 = 0; i0 < number_of_snapshots_dir0; ++i0)
		{
			// generate the correct string
			std::stringstream sstm;
			sstm << "param" << i0 << "_dir0";
			std::string result = sstm.str();
		        

			for(int i1 = 0; i1 < number_of_snapshots_dir1; ++i1)
			{
				// generate the correct string
				std::stringstream sstm1;
				sstm1 << "param" << i1 << "_dir1";
				std::string result1 = sstm1.str();
				general_param_vector[i_all][0] = m_session->GetParameter(result);
			        general_param_vector[i_all][1] = m_session->GetParameter(result1);
				i_all = i_all + 1;
//				general_param_vector[i_all] = Array<OneD, NekDouble> (parameter_space_dimension);
			}
		}
		int type_para1 = m_session->GetParameter("type_para1");
//		cout << "type para1 " << type_para1 << endl;
		int type_para2 = m_session->GetParameter("type_para2");
		//cout << "type para2 " << type_para2 << endl;
		int number_elem_trafo = m_session->GetParameter("number_elem_trafo");

		elements_trafo = Array<OneD, std::set<int> > (number_elem_trafo);
//	    <P> elem_1 = {32,30,11,10,9,8,17,16,15,14,25,24,23,22}   	</P>   
//          <P> elem_2 = {12, 28, 34, 13,21,20,18,19,26,27}   		</P>   
//	    <P> elem_3 = {29, 31}   	</P>   
//	    <P> elem_4 = {33, 35}   	</P>   
//	    <P> elem_5 = {0,1,2,3,4,5,6,7}  
		elements_trafo[0].insert(32); // of course, this should work automatically
		elements_trafo[0].insert(30);
		elements_trafo[0].insert(11);
		elements_trafo[0].insert(10);
		elements_trafo[0].insert(9);
		elements_trafo[0].insert(8);
		elements_trafo[0].insert(17);
		elements_trafo[0].insert(16);
		elements_trafo[0].insert(15);
		elements_trafo[0].insert(14);
		elements_trafo[0].insert(25);
		elements_trafo[0].insert(24);
		elements_trafo[0].insert(23);
		elements_trafo[0].insert(22);

		elements_trafo[1].insert(12);
		elements_trafo[1].insert(28);
		elements_trafo[1].insert(34);
		elements_trafo[1].insert(13);
		elements_trafo[1].insert(21);
		elements_trafo[1].insert(20);
		elements_trafo[1].insert(18);
		elements_trafo[1].insert(19);
		elements_trafo[1].insert(26);
		elements_trafo[1].insert(27);

		elements_trafo[2].insert(29);
		elements_trafo[2].insert(31);

		elements_trafo[3].insert(33);
		elements_trafo[3].insert(35);

		elements_trafo[4].insert(0);
		elements_trafo[4].insert(1);
		elements_trafo[4].insert(2);
		elements_trafo[4].insert(3);
		elements_trafo[4].insert(4);
		elements_trafo[4].insert(5);
		elements_trafo[4].insert(6);
		elements_trafo[4].insert(7);

/*	def Geo_T(w, elemT, index): # index 0: det, index 1,2,3,4: mat_entries
	if elemT == 0:
		T = np.matrix([[1, 0], [0, 1/w]])
	if elemT == 1:
		T = np.matrix([[1, 0], [0, 2/(3-w)]])
	if elemT == 2:
		T = np.matrix([[1, 0], [-(1-w)/2, 1]])
	if elemT == 3:
		T = np.matrix([[1, 0], [-(w-1)/2, 1]])
	if elemT == 4:
		T = np.matrix([[1, 0], [0, 1]])			
	if index == 0:
		return 1/np.linalg.det(T)
	if index == 1:
		return T[0,0]
	if index == 2:
		return T[0,1]
	if index == 3:
		return T[1,0]
	if index == 4:
		return T[1,1]	*/

//		elements_trafo_matrix = Array<OneD, Eigen::Matrix2d > (number_elem_trafo); // but how to do this with symbolic computation?
		 

	}
//	cout << "parameter vector generated" << endl;
//	cout << "general_param_vector.num_elements() " << general_param_vector.num_elements() << endl;
//	cout << "general_param_vector[0].num_elements() " << general_param_vector[0].num_elements() << endl;
//	cout << "general_param_vector[1].num_elements() " << general_param_vector[1].num_elements() << endl;
//	cout << "general_param_vector[2].num_elements() " << general_param_vector[2].num_elements() << endl;
//	cout << "general_param_vector[19].num_elements() " << general_param_vector[19].num_elements() << endl;
	for(int i0 = 0; i0 < general_param_vector.num_elements(); ++i0)
	{
		for(int i1 = 0; i1 < general_param_vector[i0].num_elements(); ++i1)
		{
//			cout << "general_param_vector[i0][i1] " << general_param_vector[i0][i1] << endl;
		}
	}

//	cout << Geo_T( 0.2 , 0, 0) << endl;;

	InitObject();
	if ( load_snapshot_data_from_files )
	{
//		if (parameter_space_dimension == 1)
//		{
			load_snapshots(number_of_snapshots);
//		}
//		else if (parameter_space_dimension == 2)
//		{
//			load_snapshots_geometry_params(); // not needed ?
//		}
	}
	else
	{
		if (parameter_space_dimension == 1)
		{
			compute_snapshots(number_of_snapshots); // use something new for CMAME example
		}
		else if (parameter_space_dimension == 2)
		{
			compute_snapshots_geometry_params(); // currently not implemented
		}
	}

	cout << "snapshots available" << endl;

	DoInitialise(); 
	DoSolve();

	cout << "DoInitialise and DoSolve done" << endl;
	cout << "parameter_space_dimension " << parameter_space_dimension << endl;

	m_session->SetSolverInfo("SolverType", "CoupledLinearisedNS_trafoP");
	if (parameter_space_dimension == 1)
	{
		CoupledLinearNS_trafoP babyCLNS_trafo(m_session);
		babyCLNS_trafo.InitObject();
		babyCLNS_trafo.use_Newton = use_Newton;
		babyCLNS_trafo.debug_mode = debug_mode;
		collect_f_all = babyCLNS_trafo.DoTrafo(snapshot_x_collection, snapshot_y_collection, param_vector);
	}
	else if (parameter_space_dimension == 2)
	{
		do_geo_trafo(); // setting collect_f_all, making use of snapshot_x_collection, snapshot_y_collection
	}
	Eigen::BDCSVD<Eigen::MatrixXd> svd_collect_f_all(collect_f_all, Eigen::ComputeThinU);
	cout << "svd_collect_f_all.singularValues() " << svd_collect_f_all.singularValues() << endl << endl;
	Eigen::VectorXd singular_values = svd_collect_f_all.singularValues();
	if (debug_mode)
	{
		cout << "sum singular values " << singular_values.sum() << endl << endl;
	}
	Eigen::VectorXd rel_singular_values = singular_values / singular_values.sum();
	cout << "relative singular value percents: " << rel_singular_values << endl;
	// determine RBsize corresponding to the chosen POD_tolerance
	RBsize = 1; // the case of using all POD modes
	Eigen::VectorXd cum_rel_singular_values = Eigen::VectorXd::Zero(singular_values.rows());
	for (int i = 0; i < singular_values.rows(); ++i)
	{
		cum_rel_singular_values(i) = singular_values.head(i+1).sum() / singular_values.sum();
		if (cum_rel_singular_values(i) < POD_tolerance)
		{
			RBsize = i+2;
		}		
	}
	if (debug_mode)
	{
		cout << "cumulative relative singular value percentages: " << cum_rel_singular_values << endl;
		cout << "RBsize: " << RBsize << endl;
	}
	


	Eigen::MatrixXd collect_f_all_PODmodes = svd_collect_f_all.matrixU(); // this is a local variable...
	// here probably limit to something like 99.99 percent of PODenergy, this will set RBsize
	setDBC(collect_f_all); // agnostic to RBsize
	Array<OneD, MultiRegions::ExpListSharedPtr> m_fields = UpdateFields();
        int  nel  = m_fields[0]->GetNumElmts(); // number of spectral elements
//	PODmodes = Eigen::MatrixXd::Zero(collect_f_all_PODmodes.rows(), collect_f_all_PODmodes.cols());
	PODmodes = Eigen::MatrixXd::Zero(collect_f_all_PODmodes.rows(), RBsize);  
	PODmodes = collect_f_all_PODmodes.leftCols(RBsize);
	set_MtM();
	cout << "RBsize: " << RBsize << endl;
	if (globally_connected == 1)
	{
		setDBC_M(collect_f_all);
	}
	if (debug_mode)
	{
		cout << "M_no_dbc_in_loc " << M_no_dbc_in_loc << endl;
		cout << "no_dbc_in_loc " << no_dbc_in_loc << endl;
		cout << "M_no_not_dbc_in_loc " << M_no_not_dbc_in_loc << endl;
		cout << "no_not_dbc_in_loc " <<	no_not_dbc_in_loc << endl;
	}
	//Eigen::VectorXd f_bnd_dbc_full_size = CLNS.f_bnd_dbc_full_size;
	// c_f_all_PODmodes_wo_dbc becomes CLNS.RB
	Eigen::MatrixXd c_f_all_PODmodes_wo_dbc = RB;
	if (debug_mode)
	{
		cout << "c_f_all_PODmodes_wo_dbc.rows() " << c_f_all_PODmodes_wo_dbc.rows() << endl;
		cout << "c_f_all_PODmodes_wo_dbc.cols() " << c_f_all_PODmodes_wo_dbc.cols() << endl;
	}
	gen_phys_base_vecs();
	if (debug_mode)	
	{
		cout << "finished gen_phys_base_vecs " << endl;
	}
	gen_proj_adv_terms();
	if (debug_mode)	
	{
		cout << "finished gen_proj_adv_terms " << endl;
	}
	gen_reference_matrices();
	if (debug_mode)	
	{
		cout << "finished gen_reference_matrices " << endl;
	}
    }

    Eigen::MatrixXd CoupledLinearNS_TT::gen_affine_mat_proj(double current_nu)
    {
	Eigen::MatrixXd recovered_affine_adv_mat_proj_xy = Eigen::MatrixXd::Zero(RBsize, RBsize);
	for (int i = 0; i < RBsize; ++i)
	{
		recovered_affine_adv_mat_proj_xy += adv_mats_proj_x[i] * curr_xy_projected(i,0) + adv_mats_proj_y[i] * curr_xy_projected(i,1);
	}
	Eigen::MatrixXd affine_mat_proj = the_const_one_proj + current_nu * the_ABCD_one_proj + recovered_affine_adv_mat_proj_xy;

	if (debug_mode)
	{
		Eigen::MatrixXd affine_mat = Get_complete_matrix();
		cout << "affine_mat.rows() " << affine_mat.rows() << " affine_mat.cols() " << affine_mat.cols() << endl;
		cout << "affine_mat_proj.rows() " << affine_mat_proj.rows() << " affine_mat_proj.cols() " << affine_mat_proj.cols() << endl;
		cout << "RB.rows() " << RB.rows() << " RB.cols() " << RB.cols() << endl;
		cout << "PODmodes.rows() " << PODmodes.rows() << " PODmodes.cols() " << PODmodes.cols() << endl;
		cout << "affine_mat.norm() "  << affine_mat.norm() << endl;
		cout << "affine_mat_proj.norm() " << affine_mat_proj.norm() << endl;
		Eigen::MatrixXd reproj_affine_mat = Eigen::MatrixXd::Zero(RB.rows(), RB.rows());
		reproj_affine_mat = RB * affine_mat_proj * RB.transpose();
		cout << "reproj_affine_mat.norm() " << reproj_affine_mat.norm() << endl;
		Eigen::MatrixXd affine_matrix_simplified = remove_cols_and_rows(affine_mat, elem_loc_dbc);
		cout << "affine_matrix_simplified.norm() "  << affine_matrix_simplified.norm() << endl;
		Eigen::MatrixXd reduced_affine_mat = Eigen::MatrixXd::Zero(RB.cols(), RB.cols());
		reduced_affine_mat = RB.transpose() * affine_matrix_simplified * RB;
		cout << "reduced_affine_mat.norm() "  << reduced_affine_mat.norm() << endl;

	}

	return affine_mat_proj;
    }

    Eigen::VectorXd CoupledLinearNS_TT::gen_affine_vec_proj(double current_nu, int current_index)
    {
	Eigen::VectorXd recovered_affine_adv_rhs_proj_xy = Eigen::VectorXd::Zero(RBsize); 
	for (int i = 0; i < RBsize; ++i)
	{
		recovered_affine_adv_rhs_proj_xy -= adv_vec_proj_x[i] * curr_xy_projected(i,0) + adv_vec_proj_y[i] * curr_xy_projected(i,1);
	}	
	Eigen::VectorXd add_rhs_Newton = Eigen::VectorXd::Zero(RBsize); 
	Eigen::VectorXd recovered_affine_adv_rhs_proj_xy_newton = Eigen::VectorXd::Zero(RBsize);
	Eigen::MatrixXd recovered_affine_adv_rhs_proj_xy_newton_RB = Eigen::MatrixXd::Zero(RBsize,RBsize);  
	if (use_Newton)
	{
		// can I build the Newton-required term from recovered_affine_adv_mat_proj_xy and curr_xy_projected ?
		Eigen::MatrixXd recovered_affine_adv_mat_proj_xy = Eigen::MatrixXd::Zero(RBsize, RBsize);

		for (int i = 0; i < RBsize; ++i)
		{
			recovered_affine_adv_mat_proj_xy += adv_mats_proj_x[i] * curr_xy_projected(i,0) + adv_mats_proj_y[i] * curr_xy_projected(i,1);
			recovered_affine_adv_rhs_proj_xy_newton_RB -= adv_vec_proj_x_newton_RB[i] * curr_xy_projected(i,0) + adv_vec_proj_y_newton_RB[i] * curr_xy_projected(i,1);
		}
//		cout << "recovered_affine_adv_mat_proj_xy.rows() " << recovered_affine_adv_mat_proj_xy.rows() << " recovered_affine_adv_mat_proj_xy.cols() " << recovered_affine_adv_mat_proj_xy.cols() << endl;
//		cout << "collect_f_all.rows() " << collect_f_all.rows() << " collect_f_all.cols() " << collect_f_all.cols() << endl;
		Eigen::VectorXd current_f_all = Eigen::VectorXd::Zero(collect_f_all.rows());
		current_f_all = collect_f_all.col(current_index);
		Eigen::VectorXd current_f_all_wo_dbc = remove_rows(current_f_all, elem_loc_dbc);
		Eigen::VectorXd proj_current_f_all_wo_dbc = RB.transpose() * current_f_all_wo_dbc;
		proj_current_f_all_wo_dbc = PODmodes.transpose() * current_f_all;
//		cout << "proj_current_f_all_wo_dbc " << proj_current_f_all_wo_dbc << endl;

		if (use_Newton)
		{
			add_rhs_Newton = recovered_affine_adv_rhs_proj_xy_newton_RB.transpose() * proj_current_f_all_wo_dbc;
//			cout << "add_rhs_Newton " << add_rhs_Newton << endl;
			add_rhs_Newton = recovered_affine_adv_rhs_proj_xy_newton_RB * proj_current_f_all_wo_dbc;
//			cout << "add_rhs_Newton 2 " << add_rhs_Newton << endl;
		}


		for (int i = 0; i < RBsize; ++i)
		{
//			recovered_affine_adv_rhs_proj_xy_newton -= adv_vec_proj_x_newton[i] * curr_xy_projected(i,0) + adv_vec_proj_y_newton[i] * curr_xy_projected(i,1);
//			cout << "adv_vec_proj_x_newton_RB.rows() " << adv_vec_proj_x_newton_RB[i].rows() << " adv_vec_proj_x_newton_RB.cols() " << adv_vec_proj_x_newton_RB[i].cols() << endl;
//			cout << "adv_vec_proj_y_newton_RB.rows() " << adv_vec_proj_y_newton_RB[i].rows() << " adv_vec_proj_y_newton_RB.cols() " << adv_vec_proj_y_newton_RB[i].cols() << endl;
//			recovered_affine_adv_rhs_proj_xy_newton_RB -= adv_vec_proj_x_newton_RB[i] * curr_xy_projected(i,0) + adv_vec_proj_y_newton_RB[i] * curr_xy_projected(i,1);
		}	

//		cout << "recovered_affine_adv_rhs_proj_xy_newton " << -0.5*recovered_affine_adv_rhs_proj_xy_newton << endl;


	}
//	return -the_const_one_rhs_proj - current_nu * the_ABCD_one_rhs_proj + recovered_affine_adv_rhs_proj_xy  -0.5*recovered_affine_adv_rhs_proj_xy_newton ;
	return -the_const_one_rhs_proj - current_nu * the_ABCD_one_rhs_proj + recovered_affine_adv_rhs_proj_xy  -0.5*add_rhs_Newton ;  
    }

    Eigen::VectorXd CoupledLinearNS_TT::reconstruct_solution_w_dbc(Eigen::VectorXd reprojected_solve)
    {
	Eigen::VectorXd reconstruct_solution = Eigen::VectorXd::Zero(f_bnd_dbc_full_size.rows());  // is of size M_truth_size
	int counter_wo_dbc = 0;
	for (int row_index=0; row_index < f_bnd_dbc_full_size.rows(); ++row_index)
	{
		if (!elem_loc_dbc.count(row_index))
		{
			reconstruct_solution(row_index) = reprojected_solve(counter_wo_dbc);
			counter_wo_dbc++;
		}
		else
		{
			reconstruct_solution(row_index) = f_bnd_dbc_full_size(row_index);
		}
	}
	return reconstruct_solution;
    }

    void CoupledLinearNS_TT::gen_reference_matrices()
    {
	double current_nu = ref_param_nu;
	int current_index = ref_param_index;
	Set_m_kinvis( current_nu );
	DoInitialiseAdv(snapshot_x_collection[current_index], snapshot_y_collection[current_index]);
	the_const_one = Get_no_advection_matrix_pressure();
	the_ABCD_one = Get_no_advection_matrix_ABCD();
	the_const_one_simplified = remove_cols_and_rows(the_const_one, elem_loc_dbc);
	the_ABCD_one_simplified = remove_cols_and_rows(the_ABCD_one, elem_loc_dbc);
	the_const_one_proj = RB.transpose() * the_const_one_simplified * RB;
	the_ABCD_one_proj = RB.transpose() * the_ABCD_one_simplified * RB;
	the_ABCD_one_rhs = the_ABCD_one * f_bnd_dbc_full_size;
	the_const_one_rhs = the_const_one * f_bnd_dbc_full_size;
	the_ABCD_one_rhs_simplified = remove_rows(the_ABCD_one_rhs, elem_loc_dbc);
	the_const_one_rhs_simplified = remove_rows(the_const_one_rhs, elem_loc_dbc);
	the_ABCD_one_rhs_proj = RB.transpose() * the_ABCD_one_rhs_simplified;
	the_const_one_rhs_proj = RB.transpose() * the_const_one_rhs_simplified;
//	cout << "the_const_one_rhs_proj " << the_const_one_rhs_proj << endl;
    }


    void CoupledLinearNS_TT::compute_snapshots(int number_of_snapshots)
    {
	CoupledLinearNS_trafoP babyCLNS_trafo(m_session);
	babyCLNS_trafo.InitObject();
	babyCLNS_trafo.use_Newton = use_Newton;
	babyCLNS_trafo.snapshot_computation_plot_rel_errors = snapshot_computation_plot_rel_errors;
	Array<OneD, NekDouble> zero_phys_init(GetNpoints(), 0.0);
	snapshot_x_collection = Array<OneD, Array<OneD, NekDouble> > (number_of_snapshots);
	snapshot_y_collection = Array<OneD, Array<OneD, NekDouble> > (number_of_snapshots);
        for(int i = 0; i < number_of_snapshots; ++i)
	{

		Array<OneD, Array<OneD, NekDouble> > converged_solution = babyCLNS_trafo.DoSolve_at_param(zero_phys_init, zero_phys_init, param_vector[i]);
		snapshot_x_collection[i] = Array<OneD, NekDouble> (GetNpoints(), 0.0);
		snapshot_y_collection[i] = Array<OneD, NekDouble> (GetNpoints(), 0.0);
		for (int j=0; j < GetNpoints(); ++j)
		{
			snapshot_x_collection[i][j] = converged_solution[0][j];
			snapshot_y_collection[i][j] = converged_solution[1][j];
		}
	}


    }

    void CoupledLinearNS_TT::load_snapshots(int number_of_snapshots)
    {
	// fill the fields snapshot_x_collection and snapshot_y_collection

	int nvelo = 2;
        Array<OneD, Array<OneD, NekDouble> > test_load_snapshot(nvelo); // for a 2D problem

        for(int i = 0; i < nvelo; ++i)
        {
            test_load_snapshot[i] = Array<OneD, NekDouble> (GetNpoints(), 0.0);  // number of phys points
        }
               
        std::vector<std::string> fieldStr;
        for(int i = 0; i < nvelo; ++i)
        {
           fieldStr.push_back(m_session->GetVariable(i));
        }

	snapshot_x_collection = Array<OneD, Array<OneD, NekDouble> > (number_of_snapshots);
	snapshot_y_collection = Array<OneD, Array<OneD, NekDouble> > (number_of_snapshots);

        for(int i = 0; i < number_of_snapshots; ++i)
        {
		// generate the correct string
		std::stringstream sstm;
		sstm << "TestSnap" << i+1;
		std::string result = sstm.str();
		const char* rr = result.c_str();

	        EvaluateFunction(fieldStr, test_load_snapshot, result);
		snapshot_x_collection[i] = Array<OneD, NekDouble> (GetNpoints(), 0.0);
		snapshot_y_collection[i] = Array<OneD, NekDouble> (GetNpoints(), 0.0);
		for (int j=0; j < GetNpoints(); ++j)
		{
			snapshot_x_collection[i][j] = test_load_snapshot[0][j];
			snapshot_y_collection[i][j] = test_load_snapshot[1][j];
		}
        }
    }

    void CoupledLinearNS_TT::trafoSnapshot_simple(Eigen::MatrixXd RB_in)
    {
	// moved to CoupledLinearNS_trafoP
    }


   void CoupledLinearNS_TT::DefineRBspace(Eigen::MatrixXd RB_in)
    {
	// unused
	
    }

    void CoupledLinearNS_TT::v_Output(void)
    {    
        std::vector<Array<OneD, NekDouble> > fieldcoeffs(m_fields.num_elements()+1);
        std::vector<std::string> variables(m_fields.num_elements()+1);
        int i;
        
        for(i = 0; i < m_fields.num_elements(); ++i)
        {
            fieldcoeffs[i] = m_fields[i]->UpdateCoeffs();
            variables[i]   = m_boundaryConditions->GetVariable(i);
        }


        fieldcoeffs[i] = Array<OneD, NekDouble>(m_fields[0]->GetNcoeffs());  
        // project pressure field to velocity space        
        if(m_singleMode==true)
        {
            Array<OneD, NekDouble > tmpfieldcoeffs (m_fields[0]->GetNcoeffs()/2);
            m_pressure->GetPlane(0)->BwdTrans_IterPerExp(m_pressure->GetPlane(0)->GetCoeffs(), m_pressure->GetPlane(0)->UpdatePhys());
            m_pressure->GetPlane(1)->BwdTrans_IterPerExp(m_pressure->GetPlane(1)->GetCoeffs(), m_pressure->GetPlane(1)->UpdatePhys()); 
            m_fields[0]->GetPlane(0)->FwdTrans_IterPerExp(m_pressure->GetPlane(0)->GetPhys(),fieldcoeffs[i]);
            m_fields[0]->GetPlane(1)->FwdTrans_IterPerExp(m_pressure->GetPlane(1)->GetPhys(),tmpfieldcoeffs);
            for(int e=0; e<m_fields[0]->GetNcoeffs()/2; e++)
            {
                fieldcoeffs[i][e+m_fields[0]->GetNcoeffs()/2] = tmpfieldcoeffs[e];
            }          
        }
        else
        {
            m_pressure->BwdTrans_IterPerExp(m_pressure->GetCoeffs(),m_pressure->UpdatePhys());
            m_fields[0]->FwdTrans_IterPerExp(m_pressure->GetPhys(),fieldcoeffs[i]);
        }
        variables[i] = "p"; 
        
        std::string outname = m_sessionName + ".fld";
        
        WriteFld(outname,m_fields[0],fieldcoeffs,variables);

/*
//        Array<OneD, NekDouble> glo_fieldcoeffs(m_fields[0]->GetNcoeffs(), 1234.5678);
        Array<OneD, NekDouble> glo_fieldcoeffs(fieldcoeffs[2].num_elements(), 1234.5678);
	m_fields[0]->LocalToGlobal(fieldcoeffs[2], glo_fieldcoeffs);
//	m_pressure->LocalToGlobal(fieldcoeffs[2], glo_fieldcoeffs); This method is not defined or valid for this class type


	int first_ngc_index = 0;

	while(glo_fieldcoeffs[first_ngc_index] > 1234.5679 || glo_fieldcoeffs[first_ngc_index] < 1234.5677)
	{
		first_ngc_index++;
	}

	// can construct a glodofphys here by going with FwdTrans_IterPerExp and LocalToGlobal

        Array<OneD, Array<OneD, NekDouble> > glo_coll_fieldcoeffs(m_fields[0]->GetNpoints());
	for(int counter_phys_index = 0; counter_phys_index < m_fields[0]->GetNpoints(); counter_phys_index++)
	{
	        Array<OneD, NekDouble> phys_fieldcoeffs(m_fields[0]->GetNpoints(), 0.0);
	        Array<OneD, NekDouble> loc_fieldcoeffs(m_fields[0]->GetNcoeffs(), 0.0);
	        Array<OneD, NekDouble> glo_fieldcoeffs(first_ngc_index, 0.0);
		phys_fieldcoeffs[counter_phys_index] = 1.0;
                m_fields[0]->FwdTrans_IterPerExp(phys_fieldcoeffs, loc_fieldcoeffs);
		m_fields[0]->LocalToGlobal(loc_fieldcoeffs, glo_fieldcoeffs);
//		cout << "cgi: " << counter_global_index << " value " << glo_fieldcoeffs[counter_global_index] << endl;
	}

          ofstream myfilegdp ("phys_glo_dof.txt");
	  if (myfilegdp.is_open())
	  {
		for (int i_global_dofs1 = 0; i_global_dofs1 < first_ngc_index; i_global_dofs1++)
		{
			for (int i_phys_dofs2 = 0; i_phys_dofs2 < m_fields[0]->GetNpoints(); i_phys_dofs2++)
			{
				myfilegdp << std::setprecision(17) << glo_coll_fieldcoeffs[i_phys_dofs2][i_global_dofs1] << "\t";
			}
			myfilegdp << "\n";
		}
                myfilegdp.close();
	  }
	  else cout << "Unable to open file"; 

        Array<OneD, Array<OneD, NekDouble> > glo_coll_fieldcoeffs_gdp(m_fields[0]->GetNpoints());
	for(int counter_glo_index = 0; counter_glo_index < first_ngc_index; counter_glo_index++)
	{
	        Array<OneD, NekDouble> phys_fieldcoeffs(m_fields[0]->GetNpoints(), 0.0);
	        Array<OneD, NekDouble> loc_fieldcoeffs(m_fields[0]->GetNcoeffs(), 0.0);
	        Array<OneD, NekDouble> glo_fieldcoeffs(first_ngc_index, 0.0);
		glo_fieldcoeffs[counter_glo_index] = 1.0;
                m_fields[0]->GlobalToLocal(glo_fieldcoeffs, loc_fieldcoeffs);
		m_fields[0]->BwdTrans_IterPerExp(loc_fieldcoeffs, phys_fieldcoeffs);
		
		glo_coll_fieldcoeffs_gdp[counter_glo_index] = phys_fieldcoeffs;
//		cout << "cgi: " << counter_global_index << " value " << glo_fieldcoeffs[counter_global_index] << endl;
	}

          ofstream myfilegdpr ("glo_dof_phys.txt");
	  if (myfilegdpr.is_open())
	  {
		for (int i_global_dofs1 = 0; i_global_dofs1 < first_ngc_index; i_global_dofs1++)
		{
			for (int i_phys_dofs2 = 0; i_phys_dofs2 < m_fields[0]->GetNpoints(); i_phys_dofs2++)
			{
				myfilegdpr << std::setprecision(17) << glo_coll_fieldcoeffs_gdp[i_global_dofs1][i_phys_dofs2] << "\t";
			}
			myfilegdpr << "\n";
		}
                myfilegdpr.close();
	  }
	  else cout << "Unable to open file"; 

	// compute the dof correction

        Array<OneD, Array<OneD, NekDouble> > glo_coll_fieldcoeffs_dof_corr(m_fields[0]->GetNpoints());
	for(int counter_glo_index = 0; counter_glo_index < first_ngc_index; counter_glo_index++)
	{
	        Array<OneD, NekDouble> phys_fieldcoeffs(m_fields[0]->GetNpoints(), 0.0);
	        Array<OneD, NekDouble> loc_fieldcoeffs(m_fields[0]->GetNcoeffs(), 0.0);
	        Array<OneD, NekDouble> glo_fieldcoeffs(first_ngc_index, 0.0);
		glo_fieldcoeffs[counter_glo_index] = 1.0;
                m_fields[0]->GlobalToLocal(glo_fieldcoeffs, loc_fieldcoeffs);
		m_fields[0]->BwdTrans(loc_fieldcoeffs, phys_fieldcoeffs);
                m_fields[0]->FwdTrans_IterPerExp(phys_fieldcoeffs, loc_fieldcoeffs);
		m_fields[0]->LocalToGlobal(loc_fieldcoeffs, glo_fieldcoeffs);
		glo_coll_fieldcoeffs_dof_corr[counter_glo_index] = glo_fieldcoeffs;
//		cout << "cgi: " << counter_global_index << " value " << glo_fieldcoeffs[counter_global_index] << endl;
	}

          ofstream myfilegdprdc ("glo_dof_corr.txt");
	  if (myfilegdprdc.is_open())
	  {
		for (int i_global_dofs1 = 0; i_global_dofs1 < first_ngc_index; i_global_dofs1++)
		{
			for (int i_phys_dofs2 = 0; i_phys_dofs2 < first_ngc_index; i_phys_dofs2++)
			{
				myfilegdprdc << std::setprecision(17) << glo_coll_fieldcoeffs_dof_corr[i_global_dofs1][i_phys_dofs2] << "\t";
			}
			myfilegdprdc << "\n";
		}
                myfilegdprdc.close();
	  }
	  else cout << "Unable to open file"; 





	// construct a LocalGloMapA 

        Array<OneD, Array<OneD, NekDouble> > glo_coll_fieldcoeffsA(m_fields[0]->GetNcoeffs());
	for(int counter_loc_index = 0; counter_loc_index < m_fields[0]->GetNcoeffs(); counter_loc_index++)
	{
	        Array<OneD, NekDouble> loc_fieldcoeffs(m_fields[0]->GetNcoeffs(), 0.0);
	        Array<OneD, NekDouble> glo_fieldcoeffs(first_ngc_index, 0.0);
		loc_fieldcoeffs[counter_loc_index] = 1.0;
		m_fields[0]->LocalToGlobal(loc_fieldcoeffs, glo_fieldcoeffs);
		glo_coll_fieldcoeffsA[counter_loc_index] = glo_fieldcoeffs;
//		cout << "cgi: " << counter_global_index << " value " << glo_fieldcoeffs[counter_global_index] << endl;
	}

          ofstream myfilegdpA ("LocGloMapMatA.txt");
	  if (myfilegdpA.is_open())
	  {
		for (int i_global_dofs1 = 0; i_global_dofs1 < first_ngc_index; i_global_dofs1++)
		{
			for (int i_phys_dofs2 = 0; i_phys_dofs2 < m_fields[0]->GetNcoeffs(); i_phys_dofs2++)
			{
				myfilegdpA << std::setprecision(17) << glo_coll_fieldcoeffsA[i_phys_dofs2][i_global_dofs1] << "\t";
			}
			myfilegdpA << "\n";
		}
                myfilegdpA.close();
	  }
	  else cout << "Unable to open file"; 

        std::string outname_txt = m_sessionName + ".txt";
	const char* outname_t = outname_txt.c_str();

        Array<OneD, NekDouble> glo_fieldcoeffs_x(first_ngc_index, 0.0);
        Array<OneD, NekDouble> glo_fieldcoeffs_y(first_ngc_index, 0.0);
	m_fields[0]->LocalToGlobal(m_fields[0]->GetCoeffs(), glo_fieldcoeffs_x);
	m_fields[1]->LocalToGlobal(m_fields[1]->GetCoeffs(), glo_fieldcoeffs_y);


          ofstream myfile_t (outname_t);
	  if (myfile_t.is_open())
	  {
		for (int i_glo_dofs = 0; i_glo_dofs < first_ngc_index; i_glo_dofs++)
		{
			myfile_t << std::setprecision(17) << glo_fieldcoeffs_x[i_glo_dofs] << "\n";
		}
		for (int i_glo_dofs = 0; i_glo_dofs < first_ngc_index; i_glo_dofs++)
		{
			myfile_t << std::setprecision(17) << glo_fieldcoeffs_y[i_glo_dofs] << "\n";
		}
                myfile_t.close();
	  }
	  else cout << "Unable to open file"; 
*/




//	const std::vector< std::string > filnam = m_session->GetFilenames();
//	cout << "loaded filename: " << filnam[0] << endl;  

    }

}

/**
 * $Log: CoupledLinearNS.cpp,v $
 **/
