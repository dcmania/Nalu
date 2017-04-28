/*------------------------------------------------------------------------*/
/*  Copyright 2014 National Renewable Energy Laboratory.                  */
/*  This software is released under the license detailed                  */
/*  in the file, LICENSE, which is located in the top-level Nalu          */
/*  directory structure                                                   */
/*------------------------------------------------------------------------*/

#include "MomentumMassElemKernel.h"
#include "AlgTraits.h"
#include "master_element/MasterElement.h"
#include "TimeIntegrator.h"
#include "SolutionOptions.h"

// template and scratch space
#include "BuildTemplates.h"
#include "ScratchViews.h"

// stk_mesh/base/fem
#include <stk_mesh/base/Entity.hpp>
#include <stk_mesh/base/MetaData.hpp>
#include <stk_mesh/base/BulkData.hpp>
#include <stk_mesh/base/Field.hpp>

namespace sierra {
namespace nalu {

template<typename AlgTraits>
MomentumMassElemKernel<AlgTraits>::MomentumMassElemKernel(
  const stk::mesh::BulkData& bulkData,
  SolutionOptions& solnOpts,
  ElemDataRequests& dataPreReqs,
  const bool lumpedMass)
  : Kernel(),
    lumpedMass_(lumpedMass),
    ipNodeMap_(sierra::nalu::get_volume_master_element(AlgTraits::topo_)->ipNodeMap())
{

  const stk::mesh::MetaData& metaData = bulkData.mesh_meta_data();
  VectorFieldType *velocity = metaData.get_field<VectorFieldType>(
    stk::topology::NODE_RANK, "velocity");
  ScalarFieldType *density = metaData.get_field<ScalarFieldType>(
    stk::topology::NODE_RANK, "density");

  velocityN_ = &(velocity->field_of_state(stk::mesh::StateN));
  velocityNp1_ = &(velocity->field_of_state(stk::mesh::StateNP1));
  if (velocity->number_of_states() == 2)
    velocityNm1_ = velocityN_;
  else
    velocityNm1_ = &(velocity->field_of_state(stk::mesh::StateNM1));

  densityN_ = &(density->field_of_state(stk::mesh::StateN));
  densityNp1_ = &(density->field_of_state(stk::mesh::StateNP1));
  if (density->number_of_states() == 2)
    densityNm1_ = densityN_;
  else
    densityNm1_ = &(density->field_of_state(stk::mesh::StateNM1));

  Gjp_ = metaData.get_field<VectorFieldType>(stk::topology::NODE_RANK, "dpdx");
  coordinates_ = metaData.get_field<VectorFieldType>(
    stk::topology::NODE_RANK, solnOpts.get_coordinates_name());

  MasterElement* meSCV = sierra::nalu::get_volume_master_element(AlgTraits::topo_);

  // compute shape function
  if ( lumpedMass_ )
    meSCV->shifted_shape_fcn(&v_shape_function_(0,0));
  else
    meSCV->shape_fcn(&v_shape_function_(0,0));

  // add master elements
  dataPreReqs.add_cvfem_volume_me(meSCV);

  // fields and data
  dataPreReqs.add_gathered_nodal_field(*coordinates_, AlgTraits::nDim_);
  dataPreReqs.add_gathered_nodal_field(*densityNm1_, 1);
  dataPreReqs.add_gathered_nodal_field(*densityN_, 1);
  dataPreReqs.add_gathered_nodal_field(*densityNp1_, 1);
  dataPreReqs.add_gathered_nodal_field(*velocityNm1_, AlgTraits::nDim_);
  dataPreReqs.add_gathered_nodal_field(*velocityN_, AlgTraits::nDim_);
  dataPreReqs.add_gathered_nodal_field(*velocityNp1_, AlgTraits::nDim_);
  dataPreReqs.add_gathered_nodal_field(*Gjp_, AlgTraits::nDim_);
  dataPreReqs.add_master_element_call(SCV_VOLUME);
}

template<typename AlgTraits>
MomentumMassElemKernel<AlgTraits>::~MomentumMassElemKernel()
{}

template<typename AlgTraits>
void
MomentumMassElemKernel<AlgTraits>::setup(const TimeIntegrator& timeIntegrator)
{
  dt_ = timeIntegrator.get_time_step();
  gamma1_ = timeIntegrator.get_gamma1();
  gamma2_ = timeIntegrator.get_gamma2();
  gamma3_ = timeIntegrator.get_gamma3(); // gamma3 may be zero
}

template<typename AlgTraits>
void
MomentumMassElemKernel<AlgTraits>::execute(
  SharedMemView<double**>& lhs,
  SharedMemView<double *>& rhs,
  stk::mesh::Entity /* element */,
  ScratchViews& scratchViews)
{
  SharedMemView<double*>& v_densityNm1 = scratchViews.get_scratch_view_1D(*densityNm1_);
  SharedMemView<double*>& v_densityN = scratchViews.get_scratch_view_1D(*densityN_);
  SharedMemView<double*>& v_densityNp1 = scratchViews.get_scratch_view_1D(*densityNp1_);
  SharedMemView<double**>& v_velocityNm1 = scratchViews.get_scratch_view_2D(*velocityNm1_);
  SharedMemView<double**>& v_velocityN = scratchViews.get_scratch_view_2D(*velocityN_);
  SharedMemView<double**>& v_velocityNp1 = scratchViews.get_scratch_view_2D(*velocityNp1_);
  SharedMemView<double**>& v_Gpdx = scratchViews.get_scratch_view_2D(*Gjp_);

  SharedMemView<double*>& v_scv_volume = scratchViews.scv_volume;

  for (int ip=0; ip < AlgTraits::numScvIp_; ++ip) {
    const int nearestNode = ipNodeMap_[ip];

    double rhoNm1 = 0.0;
    double rhoN   = 0.0;
    double rhoNp1 = 0.0;
    for (int j=0; j < AlgTraits::nDim_; j++) {
      v_uNm1_(j) = 0.0;
      v_uN_(j) = 0.0;
      v_uNp1_(j) = 0.0;
      v_Gjp_(j) = 0.0;
    }

    for (int ic=0; ic < AlgTraits::nodesPerElement_; ++ic) {
      const double r = v_shape_function_(ip, ic);

      rhoNm1 += r * v_densityNm1(ic);
      rhoN   += r * v_densityN(ic);
      rhoNp1 += r * v_densityNp1(ic);
      for (int j=0; j < AlgTraits::nDim_; j++) {
        v_uNm1_(j) += r * v_velocityNm1(ic, j);
        v_uN_(j)   += r * v_velocityN(ic, j);
        v_uNp1_(j) += r * v_velocityNp1(ic, j);
        v_Gjp_(j)  += r * v_Gpdx(ic, j);
      }
    }

    const double scV = v_scv_volume(ip);
    const int nnNdim = nearestNode * AlgTraits::nDim_;
    // Compute RHS
    for (int j=0; j < AlgTraits::nDim_; ++j) {
      rhs(nnNdim + j) +=
        - ( gamma1_ * rhoNp1 * v_uNp1_(j) +
            gamma2_ * rhoN   * v_uN_(j) +
            gamma3_ * rhoNm1 * v_uNm1_(j)) * scV / dt_
        - v_Gjp_(j) * scV;
    }

    // Compute LHS
    for (int ic=0; ic < AlgTraits::nodesPerElement_; ++ic) {
      const int icNdim = ic * AlgTraits::nDim_;
      const double r = v_shape_function_(ip, ic);
      const double lhsfac = r * gamma1_ * rhoNp1 * scV / dt_;

      for (int j=0; j<AlgTraits::nDim_; ++j) {
        const int indexNN = nnNdim + j;
        lhs(indexNN,icNdim+j) += lhsfac;
      }
    }
  }
}

INSTANTIATE_KERNEL(MomentumMassElemKernel);

}  // nalu
}  // sierra