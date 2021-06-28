/******************************************************************************
Copyright (c) 2021, Farbod Farshidian. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

 * Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#include "ocs2_legged_robot_example/constraint/LeggedRobotConstraintAD.h"

#include <ocs2_centroidal_model/AccessHelperFunctions.h>
#include <ocs2_centroidal_model/ModelHelperFunctions.h>
#include <ocs2_core/misc/Numerics.h>
#include <ocs2_pinocchio_interface/PinocchioEndEffectorKinematicsCppAd.h>

namespace ocs2 {
namespace legged_robot {

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
LeggedRobotConstraintAD::LeggedRobotConstraintAD(const SwitchedModelModeScheduleManager& modeScheduleManager,
                                                 const SwingTrajectoryPlanner& swingTrajectoryPlanner,
                                                 const PinocchioInterface& pinocchioInterface,
                                                 const CentroidalModelPinocchioMappingCppAd& pinocchioMappingCppAd, ModelSettings options)
    : modeScheduleManagerPtr_(&modeScheduleManager),
      swingTrajectoryPlannerPtr_(&swingTrajectoryPlanner),
      options_(std::move(options)),
      equalityStateInputConstraintCollectionPtr_(new ocs2::StateInputConstraintCollection),
      inequalityStateInputConstraintCollectionPtr_(new ocs2::StateInputConstraintCollection) {
  initializeConstraintTerms(pinocchioInterface, pinocchioMappingCppAd);
  collectConstraintPointers();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
LeggedRobotConstraintAD::LeggedRobotConstraintAD(const LeggedRobotConstraintAD& rhs)
    : ConstraintBase(rhs),
      modeScheduleManagerPtr_(rhs.modeScheduleManagerPtr_),
      swingTrajectoryPlannerPtr_(rhs.swingTrajectoryPlannerPtr_),
      options_(rhs.options_),
      equalityStateInputConstraintCollectionPtr_(rhs.equalityStateInputConstraintCollectionPtr_->clone()),
      inequalityStateInputConstraintCollectionPtr_(rhs.inequalityStateInputConstraintCollectionPtr_->clone()) {
  collectConstraintPointers();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void LeggedRobotConstraintAD::initializeConstraintTerms(const PinocchioInterface& pinocchioInterface,
                                                        const CentroidalModelPinocchioMappingCppAd& pinocchioMappingCppAd) {
  const auto& info = pinocchioMappingCppAd.getCentroidalModelInfo();

  for (size_t i = 0; i < info.numThreeDofContacts; i++) {
    const auto& footName = CONTACT_NAMES_3_DOF_[i];

    // Friction cone constraint (state-input inequality)
    if (options_.enforceFrictionConeConstraint_) {
      FrictionConeConstraint::Config frictionConeConConfig(options_.frictionCoefficient_);
      std::unique_ptr<FrictionConeConstraint> frictionConeConstraintPtr(new FrictionConeConstraint(std::move(frictionConeConConfig), i));
      inequalityStateInputConstraintCollectionPtr_->add(footName + "_eeFrictionCone", std::move(frictionConeConstraintPtr));
    }

    // Zero force constraint (state-input equality)
    std::unique_ptr<ZeroForceConstraint> zeroForceConstraintPtr(new ZeroForceConstraint(i));

    // end-effector kinematics
    auto velocityUpdateCallback = [&info](ad_vector_t state, PinocchioInterfaceTpl<ad_scalar_t>& pinocchioInterfaceAd) {
      const ad_vector_t q = centroidal_model::getGeneralizedCoordinates(state, info);
      updateCentroidalDynamics(pinocchioInterfaceAd, info, q);
    };
    PinocchioEndEffectorKinematicsCppAd eeKinematics(pinocchioInterface, pinocchioMappingCppAd, {footName}, info.stateDim, info.inputDim,
                                                     velocityUpdateCallback, footName, "/tmp/ocs2", options_.recompileLibraries_, true);

    // Zero velocity constraint (state-input equality)
    auto eeZeroVelConConfig = [](scalar_t positionErrorGain) {
      EndEffectorLinearConstraint::Config config;
      config.b.setZero(3);
      config.Av.setIdentity(3, 3);
      if (!numerics::almost_eq(positionErrorGain, 0.0)) {
        config.Ax.setZero(3, 3);
        config.Ax(2, 2) = positionErrorGain;
      }
      return config;
    };
    std::unique_ptr<EndEffectorLinearConstraint> eeZeroVelConstraintPtr(
        new EndEffectorLinearConstraint(eeKinematics, 3, eeZeroVelConConfig(options_.positionErrorGain_)));

    // Normal velocity constraint (state-input equality)
    std::unique_ptr<EndEffectorLinearConstraint> eeNormalVelConstraintPtr(new EndEffectorLinearConstraint(eeKinematics, 1));

    // Equalities
    equalityStateInputConstraintCollectionPtr_->add(footName + "_eeZeroForce", std::move(zeroForceConstraintPtr));
    equalityStateInputConstraintCollectionPtr_->add(footName + "_eeZeroVelocity", std::move(eeZeroVelConstraintPtr));
    equalityStateInputConstraintCollectionPtr_->add(footName + "_eeNormalVelocity", std::move(eeNormalVelConstraintPtr));
  }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void LeggedRobotConstraintAD::collectConstraintPointers() {
  eeFrictionConeConstraints_.resize(centroidalModelInfo.numThreeDofContacts);
  eeZeroForceConstraints_.resize(centroidalModelInfo.numThreeDofContacts);
  eeZeroVelocityConstraints_.resize(centroidalModelInfo.numThreeDofContacts);
  eeNormalVelocityConstraints_.resize(centroidalModelInfo.numThreeDofContacts);

  for (size_t i = 0; i < centroidalModelInfo.numThreeDofContacts; i++) {
    const auto& footName = CONTACT_NAMES_3_DOF_[i];
    // Inequalities
    if (options_.enforceFrictionConeConstraint_) {
      eeFrictionConeConstraints_[i] =
          &inequalityStateInputConstraintCollectionPtr_->get<FrictionConeConstraint>(footName + "_eeFrictionCone");
    }
    // State input equalities
    eeZeroForceConstraints_[i] = &equalityStateInputConstraintCollectionPtr_->get<ZeroForceConstraint>(footName + "_eeZeroForce");
    eeZeroVelocityConstraints_[i] =
        &equalityStateInputConstraintCollectionPtr_->get<EndEffectorLinearConstraint>(footName + "_eeZeroVelocity");
    eeNormalVelocityConstraints_[i] =
        &equalityStateInputConstraintCollectionPtr_->get<EndEffectorLinearConstraint>(footName + "_eeNormalVelocity");
  }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void LeggedRobotConstraintAD::updateStateInputEqualityConstraints(scalar_t time) {
  // EE normal velocity config
  auto eeNormalVelConConfig = [&](size_t footIndex) {
    EndEffectorLinearConstraint::Config config;
    config.b = (vector_t(1) << -swingTrajectoryPlannerPtr_->getZvelocityConstraint(footIndex, time)).finished();
    config.Av = (matrix_t(1, 3) << 0.0, 0.0, 1.0).finished();
    if (!numerics::almost_eq(options_.positionErrorGain_, 0.0)) {
      config.b(0) -= options_.positionErrorGain_ * swingTrajectoryPlannerPtr_->getZpositionConstraint(footIndex, time);
      config.Ax = (matrix_t(1, 3) << 0.0, 0.0, options_.positionErrorGain_).finished();
    }
    return config;
  };

  const auto contactFlags = modeScheduleManagerPtr_->getContactFlags(time);
  for (size_t i = 0; i < centroidalModelInfo.numThreeDofContacts; i++) {
    // zero force
    eeZeroForceConstraints_[i]->setActivity(!contactFlags[i]);

    // zero velocity
    eeZeroVelocityConstraints_[i]->setActivity(contactFlags[i]);

    // normal velocity
    eeNormalVelocityConstraints_[i]->setActivity(!contactFlags[i]);
    if (!contactFlags[i]) {
      eeNormalVelocityConstraints_[i]->configure(eeNormalVelConConfig(i));
    }
  }  // end of i loop
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void LeggedRobotConstraintAD::updateInequalityConstraints(scalar_t time) {
  if (options_.enforceFrictionConeConstraint_) {
    // friction cone
    const auto contactFlags = modeScheduleManagerPtr_->getContactFlags(time);
    for (size_t i = 0; i < centroidalModelInfo.numThreeDofContacts; i++) {
      eeFrictionConeConstraints_[i]->setActivity(contactFlags[i]);
    }  // end of i loop
  }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
vector_t LeggedRobotConstraintAD::stateInputEqualityConstraint(scalar_t time, const vector_t& state, const vector_t& input) {
  updateStateInputEqualityConstraints(time);
  return equalityStateInputConstraintCollectionPtr_->getValue(time, state, input);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
VectorFunctionLinearApproximation LeggedRobotConstraintAD::stateInputEqualityConstraintLinearApproximation(scalar_t time,
                                                                                                           const vector_t& state,
                                                                                                           const vector_t& input) {
  updateStateInputEqualityConstraints(time);
  return equalityStateInputConstraintCollectionPtr_->getLinearApproximation(time, state, input);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
vector_t LeggedRobotConstraintAD::inequalityConstraint(scalar_t time, const vector_t& state, const vector_t& input) {
  updateInequalityConstraints(time);
  return inequalityStateInputConstraintCollectionPtr_->getValue(time, state, input);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
VectorFunctionQuadraticApproximation LeggedRobotConstraintAD::inequalityConstraintQuadraticApproximation(scalar_t time,
                                                                                                         const vector_t& state,
                                                                                                         const vector_t& input) {
  updateInequalityConstraints(time);
  return inequalityStateInputConstraintCollectionPtr_->getQuadraticApproximation(time, state, input);
}

}  // namespace legged_robot
}  // end of namespace ocs2
