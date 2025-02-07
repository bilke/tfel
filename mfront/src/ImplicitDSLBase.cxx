/*!
 * \file   mfront/src/ImplicitDSLBase.cxx
 * \brief
 *
 * \author Thomas Helfer
 * \date   21 sep 2008
 * \copyright Copyright (C) 2006-2018 CEA/DEN, EDF R&D. All rights
 * reserved.
 * This project is publicly released under either the GNU GPL Licence
 * or the CECILL-A licence. A copy of thoses licences are delivered
 * with the sources of TFEL. CEA or EDF may also distribute this
 * project under specific licensing conditions.
 */

#include <cmath>
#include <limits>
#include <cstdlib>
#include <sstream>
#include <iomanip>

#include "TFEL/Raise.hxx"
#include "TFEL/Glossary/Glossary.hxx"
#include "TFEL/Glossary/GlossaryEntry.hxx"
#include "TFEL/Utilities/Data.hxx"
#include "TFEL/Utilities/StringAlgorithms.hxx"
#include "TFEL/UnicodeSupport/UnicodeSupport.hxx"
#include "TFEL/Material/FiniteStrainBehaviourTangentOperator.hxx"
#include "MFront/DSLUtilities.hxx"
#include "MFront/MFrontDebugMode.hxx"
#include "MFront/NonLinearSystemSolver.hxx"
#include "MFront/NonLinearSystemSolverBase.hxx"
#include "MFront/NonLinearSystemSolverFactory.hxx"
#include "MFront/UserDefinedNonLinearSystemSolver.hxx"
#include "MFront/PerformanceProfiling.hxx"
#include "MFront/AbstractBehaviourBrick.hxx"
#include "MFront/ImplicitDSLBase.hxx"

namespace mfront {

  static void writeVariablesOffsets(
      std::ostream& os, const VariableDescriptionContainer& variables) {
    auto n = SupportedTypes::TypeSize();
    for (const auto& v : variables) {
      os << "constexpr auto " << v.name << "_offset = " << n << ";\n";
      n += SupportedTypes::getTypeSize(v.type, v.arraySize);
    }
  }  // end of writeVariablesOffsets

  static void writeIgnoreVariablesOffsets(
      std::ostream& os, const VariableDescriptionContainer& variables) {
    for (const auto& v : variables) {
      os << "static_cast<void>(" << v.name << "_offset);\n";
    }
  }  // end of writeIgnoreVariablesOffsets

  static void declareViewsFromArrayOfVariables(
      std::ostream& os,
      const VariableDescriptionContainer& variables,
      const std::function<std::string(const std::string&)>&
          variable_name_generator,
      const std::string& array_name,
      const bool use_qt) {
    auto n = SupportedTypes::TypeSize();
    for (const auto& v : variables) {
      const auto& vname = variable_name_generator(v.name);
      if (v.arraySize == 1u) {
        if (SupportedTypes::getTypeFlag(v.type) == SupportedTypes::SCALAR) {
          if (use_qt) {
            os << "tfel::math::scalar_view<" << v.type << "> " << vname << "("
               << array_name << "(" << n << "));\n";
          } else {
            os << "real& " << vname << "(" << array_name << "(" << n << "));\n";
          }
        } else {
          os << "auto " << vname << " = tfel::math::map<" << v.type << ", " << n
             << ">(" << array_name << ");\n";
        }
      } else {
        os << "auto " << vname << " = tfel::math::map<" << v.arraySize << ", "
           << v.type << ", " << n << ">(" << array_name << ");\n";
      }
      n += SupportedTypes::getTypeSize(v.type, v.arraySize);
    }
  }  // end of declareViewsFromArrayOfVariables

  ImplicitDSLBase::ImplicitDSLBase(const DSLOptions& opts)
      : BehaviourDSLBase<ImplicitDSLBase>(opts) {
    constexpr auto uh = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    // dynamically allocated vectors are not yet allowed in implicit
    // parsers
    this->mb.areDynamicallyAllocatedVectorsAllowed(false);
    // parameters
    this->reserveName("epsilon");
    this->reserveName("\u03B5");
    this->reserveName("theta");
    this->reserveName("\u03B8");
    this->reserveName("iterMax");
    this->reserveName("numerical_jacobian_epsilon");
    this->reserveName("maximum_increment_value_per_iteration");
    this->reserveName("jacobianComparisonCriterion");
    this->reserveName("perturbatedSystemEvaluation");
    // additional reserve name
    this->reserveName("vect_e");
    this->reserveName("tzeros");
    this->reserveName("zeros_1");
    this->reserveName("jacobian_invert");
    this->reserveName("njacobian");
    this->reserveName("partial_jacobian");
    this->reserveName("jacobian_permutation");
    this->reserveName("error_p");
    this->reserveName("idx");
    this->reserveName("idx2");
    this->reserveName("idx3");
    this->reserveName("getPartialJacobianInvert");
    this->reserveName("GetPartialJacobianInvert");
    this->reserveName("TinyMatrixSolve");
    this->mb.registerMemberName(uh, "iter");
    this->mb.registerMemberName(uh, "zeros");
    this->mb.registerMemberName(uh, "delta_zeros");
    this->mb.registerMemberName(uh, "fzeros");
    this->mb.registerMemberName(uh, "jacobian");
    this->mb.registerMemberName(uh, "computePartialJacobianInvert");
    this->mb.registerMemberName(uh, "computeNumericalJacobian");
    this->mb.registerMemberName(uh, "additionalConvergenceChecks");
    // CallBacks
    this->registerNewCallBack(
        "@UsableInPurelyImplicitResolution",
        &ImplicitDSLBase::treatUsableInPurelyImplicitResolution);
    this->registerNewCallBack("@MaterialLaw",
                              &ImplicitDSLBase::treatMaterialLaw);
    this->registerNewCallBack("@Predictor", &ImplicitDSLBase::treatPredictor);
    this->registerNewCallBack("@Theta", &ImplicitDSLBase::treatTheta);
    this->registerNewCallBack("@Epsilon", &ImplicitDSLBase::treatEpsilon);
    this->registerNewCallBack(
        "@AdditionalConvergenceChecks",
        &ImplicitDSLBase::treatAdditionalConvergenceChecks);
    this->registerNewCallBack(
        "@PerturbationValueForNumericalJacobianComputation",
        &ImplicitDSLBase::
            treatPerturbationValueForNumericalJacobianComputation);
    this->registerNewCallBack("@IterMax", &ImplicitDSLBase::treatIterMax);
    this->registerNewCallBack("@MaximumNumberOfIterations",
                              &ImplicitDSLBase::treatIterMax);
    this->registerNewCallBack("@Algorithm", &ImplicitDSLBase::treatAlgorithm);
    this->registerNewCallBack("@TangentOperator",
                              &ImplicitDSLBase::treatTangentOperator);
    this->registerNewCallBack(
        "@IsTangentOperatorSymmetric",
        &ImplicitDSLBase::treatIsTangentOperatorSymmetric);
    this->registerNewCallBack("@InitJacobian",
                              &ImplicitDSLBase::treatInitJacobian);
    this->registerNewCallBack("@InitializeJacobian",
                              &ImplicitDSLBase::treatInitJacobian);
    this->registerNewCallBack("@InitJacobianInvert",
                              &ImplicitDSLBase::treatInitJacobianInvert);
    this->registerNewCallBack("@InitializeJacobianInvert",
                              &ImplicitDSLBase::treatInitJacobianInvert);
    this->registerNewCallBack("@ProcessNewCorrection",
                              &ImplicitDSLBase::treatProcessNewCorrection);
    this->registerNewCallBack("@RejectCurrentCorrection",
                              &ImplicitDSLBase::treatRejectCurrentCorrection);
    this->registerNewCallBack("@ProcessNewEstimate",
                              &ImplicitDSLBase::treatProcessNewEstimate);
    this->registerNewCallBack(
        "@CompareToNumericalJacobian",
        &ImplicitDSLBase::treatCompareToNumericalJacobian);
    this->registerNewCallBack(
        "@JacobianComparisonCriterion",
        &ImplicitDSLBase::treatJacobianComparisonCriterion);
    this->registerNewCallBack(
        "@JacobianComparisonCriterium",
        &ImplicitDSLBase::treatJacobianComparisonCriterion);
    this->registerNewCallBack(
        "@MaximumIncrementValuePerIteration",
        &ImplicitDSLBase::treatMaximumIncrementValuePerIteration);
    this->registerNewCallBack("@IntegrationVariable",
                              &ImplicitDSLBase::treatIntegrationVariable);
    this->registerNewCallBack("@ComputeStiffnessTensor",
                              &ImplicitDSLBase::treatComputeStiffnessTensor);
    this->registerNewCallBack("@ElasticMaterialProperties",
                              &ImplicitDSLBase::treatElasticMaterialProperties);
    this->registerNewCallBack(
        "@NumericallyComputedJacobianBlocks",
        &ImplicitDSLBase::treatNumericallyComputedJacobianBlocks);
    this->registerNewCallBack("@HillTensor", &ImplicitDSLBase::treatHillTensor);
    this->disableCallBack("@ComputedVar");
    //    this->disableCallBack("@UseQt");
    this->mb.setIntegrationScheme(BehaviourDescription::IMPLICITSCHEME);
  }  // end of ImplicitDSLBase

  const NonLinearSystemSolver& ImplicitDSLBase::getSolver() const {
    if (this->solver == nullptr) {
      this->throwRuntimeError("ImplicitBase::getSolver", "no solver defined");
    }
    return *(this->solver);
  }

  std::string ImplicitDSLBase::getCodeBlockTemplate(
      const std::string& c, const MFrontTemplateGenerationOptions& o) const {
    constexpr auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    if (c == BehaviourData::ComputePredictionOperator) {
      return "@PredictionOperator{}\n";
    } else if (c == BehaviourData::ComputeThermodynamicForces) {
      return "@ComputeStress{}\n";
    } else if (c == BehaviourData::Integrator) {
      // implicit system
      auto ivs = this->mb.getBehaviourData(h).getIntegrationVariables();
      const auto mivs = [this] {
        auto livs = this->mb.getBehaviourData(h).getIntegrationVariables();
        // remove managed integration variables
        const auto mivs_names = [this] {
          auto lmivs = std::vector<std::string>{};
          for (const auto& pb : this->bricks) {
            //! \return a description of the brick
            const auto d = pb->getDescription();
            lmivs.insert(lmivs.end(), d.managedIntegrationVariables.begin(),
                         d.managedIntegrationVariables.end());
          }
          return lmivs;
        }();
        livs.erase(std::remove_if(
                       livs.begin(), livs.end(),
                       [&mivs_names](const VariableDescription& v) {
                         return std::find(mivs_names.begin(), mivs_names.end(),
                                          v.name) != mivs_names.end();
                       }),
                   livs.end());
        return livs;
      }();
      if (ivs.empty()) {
        return "@Integrator{}\n";
      }
      auto i = std::string("@Integrator{\n");
      for (const auto& v1 : mivs) {
        const auto v1n = o.useUnicodeSymbols ? displayName(v1) : v1.name;
        i += "// implicit equation associated with variable " + v1n + "\n";
        if (v1.arraySize == 1u) {
          i += "f" + v1n + " += ;\n";
          i += "// jacobian blocks\n";
          if ((this->solver != nullptr) && (this->solver->usesJacobian())) {
            for (const auto& v2 : ivs) {
              const auto v2n = o.useUnicodeSymbols ? displayName(v2) : v2.name;
              const auto j = o.useUnicodeSymbols
                                 ? "\u2202f" + v1n + "\u2215\u2202\u0394" + v2n
                                 : "df" + v1n + "_d" + v2n;
              const auto op = v1.name == v2.name ? "+=" : "=";
              if (v2.arraySize == 1u) {
                i += j + " " + op + " ;\n";
              } else {
                i += "for(ushort i = 0; i != " + std::to_string(v2.arraySize) +
                     ";++i){\n";
                i += j + "(i) " + op + " ;\n";
                i += "}\n";
              }
            }
          }
        } else {
          i += "for(ushort i = 0; i != " + std::to_string(v1.arraySize) +
               ";++i){\n";
          i += "f" + v1n + "(i) += ;\n";
          i += "// jacobian blocks\n";
          if ((this->solver != nullptr) && (this->solver->usesJacobian())) {
            for (const auto& v2 : ivs) {
              const auto v2n = o.useUnicodeSymbols ? displayName(v2) : v2.name;
              const auto j = o.useUnicodeSymbols
                                 ? "\u2202f" + v1n + "\u2215\u2202\u0394" + v2n
                                 : "df" + v1n + "_d" + v2n;
              const auto op = v1.name == v2.name ? "+=" : "=";
              if (v2.arraySize == 1u) {
                i += j + "(i) " + op + " ;\n";
              } else {
                i += "for(ushort j = 0; j != " + std::to_string(v2.arraySize) +
                     ";++j){\n";
                i += j + "(i,j) " + op + " ;\n";
                i += "}\n";
              }
            }
          }
          i += "}\n";
        }
      }
      i += "}\n";
      return i;
    } else if (c == BehaviourData::ComputeTangentOperator) {
      return "@TangentOperator{}\n";
    }
    return "";
  }  // end of getCodeBlockTemplate

  void ImplicitDSLBase::treatUnknownKeyword() {
    --(this->current);
    const auto& key = this->current->value;
    ++(this->current);
    if (this->solver != nullptr) {
      const auto r = this->solver->treatSpecificKeywords(
          this->mb, key, this->current, this->tokens.end());
      if (r.first) {
        this->current = r.second;
        return;
      }
    }
    BehaviourDSLCommon::treatUnknownKeyword();
  }  // end of treatUnknownKeyword

  void ImplicitDSLBase::treatProcessNewCorrection() {
    this->treatCodeBlock(*this, BehaviourData::ProcessNewCorrection,
                         &ImplicitDSLBase::standardModifier, true, true);
  }  // end of treatProcessNewCorrection

  void ImplicitDSLBase::treatRejectCurrentCorrection() {
    this->treatCodeBlock(*this, BehaviourData::RejectCurrentCorrection,
                         &ImplicitDSLBase::standardModifier, true, true);
  }  // end of treatRejectCurrentCorrection

  void ImplicitDSLBase::treatProcessNewEstimate() {
    this->treatCodeBlock(*this, BehaviourData::ProcessNewEstimate,
                         &ImplicitDSLBase::standardModifier, true, true);
  }  // end of treatProcessNewEstimate

  void ImplicitDSLBase::treatStateVariable() {
    VariableDescriptionContainer v;
    auto hs = std::set<Hypothesis>{};
    this->readVariableList(v, hs, &BehaviourDescription::addStateVariables,
                           true);
    for (const auto h : hs) {
      for (const auto& iv : v) {
        this->mb.reserveName(h, "f" + iv.name);
        this->mb.reserveName(h, iv.name + "_offset");
        this->mb.reserveName(h, "delta_d" + iv.name);
      }
    }
  }

  void ImplicitDSLBase::treatIntegrationVariable() {
    VariableDescriptionContainer v;
    auto hs = std::set<Hypothesis>{};
    this->readVariableList(
        v, hs, &BehaviourDescription::addIntegrationVariables, true);
    for (const auto h : hs) {
      for (const auto& iv : v) {
        this->mb.reserveName(h, "f" + iv.name);
        this->mb.reserveName(h, iv.name + "_offset");
        this->mb.reserveName(h, "delta_d" + iv.name);
      }
    }
  }

  void ImplicitDSLBase::treatInitJacobian() {
    if (this->solver == nullptr) {
      this->throwRuntimeError(
          "ImplicitDSLBase::treatInitJacobian",
          "undefined algorithm. Please use the '@Algorithm' "
          "keyword to define one.");
    }
    if (!this->solver->allowsJacobianInitialisation()) {
      this->throwRuntimeError("ImplicitDSLBase::treatInitJacobian",
                              "@InitJacobian can not be used with "
                              "the current algorithm.");
    }
    this->treatCodeBlock(*this, BehaviourData::InitializeJacobian,
                         &ImplicitDSLBase::standardModifier, true, true);
  }  // end of treatInitJacobian

  void ImplicitDSLBase::treatInitJacobianInvert() {
    if (this->solver == nullptr) {
      this->throwRuntimeError(
          "ImplicitDSLBase::treatInitJacobianInvert",
          "undefined algorithm. Please use the '@Algorithm' "
          "keyword to define one.");
    }
    if (!this->solver->allowsJacobianInvertInitialisation()) {
      this->throwRuntimeError("ImplicitDSLBase::treatInitJacobianInvert",
                              "@InitJacobianInvert can not be used with "
                              "the current algorithm.");
    }
    this->treatCodeBlock(*this, BehaviourData::InitializeJacobianInvert,
                         &ImplicitDSLBase::standardModifier, true, true);
  }  // end of treatInitJacobianInvert

  void ImplicitDSLBase::treatUnknownVariableMethod(const Hypothesis h,
                                                   const std::string& n) {
    if ((this->mb.isIntegrationVariableName(h, n)) ||
        ((n[0] == 'f') &&
         (this->mb.isIntegrationVariableName(h, n.substr(1))))) {
      if (this->current->value == "setNormalisationFactor") {
        auto var = std::string{};
        ++(this->current);
        this->checkNotEndOfFile("ImplicitDSLBase::treatUnknowVariableMethod");
        this->readSpecifiedToken("ImplicitDSLBase::treatUnknowVariableMethod",
                                 "(");
        this->checkNotEndOfFile("ImplicitDSLBase::treatUnknowVariableMethod");
        var = this->current->value;
        if ((this->mb.isMaterialPropertyName(h, var)) ||
            (this->mb.isLocalVariableName(h, var))) {
          var = "this->" + var;
        } else {
          // var shall be a number
          double value;
          std::istringstream flux(var);
          flux >> value;
          if (flux.fail()) {
            this->throwRuntimeError(
                "ImplicitDSLBase::treatUnknowVariableMethod",
                "Failed to read normalisation factor.");
          }
          if (value <= 0.) {
            this->throwRuntimeError(
                "ImplicitDSLBase::treatUnknowVariableMethod",
                "invalid normalisation factor.");
          }
        }
        ++(this->current);
        this->readSpecifiedToken("ImplicitDSLBase::treatUnknowVariableMethod",
                                 ")");
        this->mb.setAttribute(h, n + "_normalisation_factor", var);
        return;
      } else if (this->current->value ==
                 "setMaximumIncrementValuePerIteration") {
        ++(this->current);
        this->checkNotEndOfFile("ImplicitDSLBase::treatUnknowVariableMethod");
        this->readSpecifiedToken("ImplicitDSLBase::treatUnknowVariableMethod",
                                 "(");
        this->checkNotEndOfFile("ImplicitDSLBase::treatUnknowVariableMethod");
        const auto var = this->current->value;
        double value;
        std::istringstream flux(var);
        flux >> value;
        if (flux.fail()) {
          this->throwRuntimeError(
              "ImplicitDSLBase::treatUnknowVariableMethod",
              "Failed to read maximum increment value per iteration from '" +
                  var + "'.");
        }
        if (value <= 0.) {
          this->throwRuntimeError(
              "ImplicitDSLBase::treatUnknowVariableMethod",
              "invalid maximum increment value per iteration.");
        }
        ++(this->current);
        this->checkNotEndOfFile("ImplicitDSLBase::treatUnknowVariableMethod");
        this->readSpecifiedToken("ImplicitDSLBase::treatUnknowVariableMethod",
                                 ")");
        VariableDescription miv(
            "real", n + "_maximum_increment_value_per_iteration", 1u, 0u);
        miv.description =
            "maximum increment allowed per iteration for variable '" + n + "'";
        this->mb.addParameter(h, miv);
        this->mb.setParameterDefaultValue(
            h, n + "_maximum_increment_value_per_iteration", value);
        return;
      }
    }
    BehaviourDSLCommon::treatUnknownVariableMethod(h, n);
  }  // end of treatUnknowVariableMethod

  bool ImplicitDSLBase::isCallableVariable(const Hypothesis h,
                                           const std::string& n) const {
    if (n.empty()) {
      this->throwRuntimeError("ImplicitDSLBase::isCallableVariable",
                              "empty variable name '" + n + "'");
    }
    if ((n[0] == 'f') && (this->mb.isIntegrationVariableName(h, n.substr(1)))) {
      return true;
    }
    return BehaviourDSLCommon::isCallableVariable(h, n);
  }  // end of isCallableVariable

  void ImplicitDSLBase::treatCompareToNumericalJacobian() {
    const auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    this->checkNotEndOfFile(
        "ImplicitDSLBase::treatCompareToNumericalJacobian : ",
        "Expected 'true' or 'false'.");
    if (this->current->value == "true") {
      this->mb.setAttribute(h, BehaviourData::compareToNumericalJacobian, true);
    } else if (this->current->value == "false") {
      this->mb.setAttribute(h, BehaviourData::compareToNumericalJacobian,
                            false);
    } else {
      this->throwRuntimeError(
          "ImplicitDSLBase::treatCompareToNumericalJacobian",
          "Expected to read 'true' or 'false' instead of '" +
              this->current->value + ".");
    }
    ++(this->current);
    this->readSpecifiedToken("ImplicitDSLBase::treatCompareToNumericalJacobian",
                             ";");
  }  // end of treatCompareToNumericalJacobian

  void ImplicitDSLBase::treatJacobianComparisonCriterion() {
    const auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    if (!this->mb.getAttribute(h, BehaviourData::compareToNumericalJacobian,
                               false)) {
      this->throwRuntimeError(
          "ImplicitDSLBase::treatJacobianComparisonCriterion",
          "must call '@CompareToNumericalJacobian' first");
    }
    this->checkNotEndOfFile("ImplicitDSLBase::treatJacobianComparisonCriterion",
                            "Cannot read jacobianComparisonCriterion value.");
    double jacobianComparisonCriterion = this->readDouble();
    if (jacobianComparisonCriterion < 0) {
      this->throwRuntimeError(
          "ImplicitDSLBase::treatJacobianComparisonCriterion",
          "JacobianComparisonCriterion value must be positive.");
    }
    this->readSpecifiedToken(
        "ImplicitDSLBase::treatJacobianComparisonCriterion", ";");
    this->mb.addParameter(
        h, VariableDescription("real", "jacobianComparisonCriterion", 1u, 0u),
        BehaviourData::ALREADYREGISTRED);
    this->mb.setParameterDefaultValue(h, "jacobianComparisonCriterion",
                                      jacobianComparisonCriterion);
  }  // ImplicitDSLBase::treatJacobianComparisonCriterion

  void ImplicitDSLBase::setNonLinearSolver(
      std::shared_ptr<NonLinearSystemSolver> s, const std::string& name) {
    this->solver = s;
    for (const auto& n : this->solver->getReservedNames()) {
      this->reserveName(n);
    }
    for (const auto& n : this->solver->getMemberNames()) {
      constexpr auto uh = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
      this->mb.registerMemberName(uh, n);
    }
    this->mb.setAttribute(BehaviourData::algorithm, name, false);
  }  // end of setNonLinearSolver

  void ImplicitDSLBase::setNonLinearSolver(const std::string& s) {
    const auto& f =
        NonLinearSystemSolverFactory::getNonLinearSystemSolverFactory();
    this->setNonLinearSolver(f.getSolver(s), s);
  }  // end of setNonLinearSolver

  void ImplicitDSLBase::treatAlgorithm() {
    if (this->solver != nullptr) {
      this->throwRuntimeError("ImplicitDSLBase::treatAlgorithm",
                              "an algorithm has already been defined.");
    }
    this->checkNotEndOfFile("ImplicitDSLBase::treatAlgorithm",
                            "Cannot read algorithm name.");
    const auto& s = this->current->value;
    ++this->current;
    this->checkNotEndOfFile("ImplicitDSLBase::treatAlgorithm");
    if (s == "UserDefined") {
      const auto d = [this] {
        using namespace tfel::utilities;
        DataParsingOptions o;
        o.allowMultipleKeysInMap = true;
        const auto opts =
            Data::read(this->current, this->tokens.end(), o).get<DataMap>();
        return opts;
      }();
      this->setNonLinearSolver(
          std::make_shared<UserDefinedNonLinearSystemSolver>(d), "UserDefined");
      this->readSpecifiedToken("ImplicitDSLBase::treatAlgorithm", ";");
    } else {
      this->readSpecifiedToken("ImplicitDSLBase::treatAlgorithm", ";");
      this->setNonLinearSolver(s);
    }
  }  // end of treatAlgorithm

  void ImplicitDSLBase::treatTheta() {
    const auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    this->checkNotEndOfFile("ImplicitDSLBase::treatTheta",
                            "Cannot read theta value.");
    const auto theta = this->readDouble();
    if ((theta < 0.) || (theta > 1.)) {
      this->throwRuntimeError(
          "ImplicitDSLBase::treatTheta",
          "Theta value must be positive and smaller than 1.");
    }
    this->readSpecifiedToken("ImplicitDSLBase::treatTheta", ";");
    VariableDescription tv("NumericType", "\u03B8", "theta", 1u, 0u);
    tv.description = "theta value used by the implicit scheme";
    this->mb.addParameter(h, tv, BehaviourData::ALREADYREGISTRED);
    this->mb.setParameterDefaultValue(h, "theta", theta);
    this->mb.setEntryName(h, "theta", "theta");
  }  // end of treatTheta

  void ImplicitDSLBase::treatEpsilon() {
    const auto uh = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    this->checkNotEndOfFile("ImplicitDSLBase::treatEpsilon",
                            "Cannot read epsilon value.");
    const auto epsilon = this->readDouble();
    if (epsilon < 0) {
      this->throwRuntimeError("ImplicitDSLBase::treatEpsilon",
                              "Epsilon value must be positive.");
    }
    this->readSpecifiedToken("ImplicitDSLBase::treatEpsilon", ";");
    VariableDescription e("NumericType", "\u03B5", "epsilon", 1u, 0u);
    e.description =
        "value used to stop the iteration of the implicit algorithm";
    this->mb.addParameter(uh, e, BehaviourData::ALREADYREGISTRED);
    this->mb.setParameterDefaultValue(uh, "epsilon", epsilon);
    this->mb.setEntryName(uh, "epsilon", "epsilon");
  }  // ImplicitDSLBase::treatEpsilon

  void ImplicitDSLBase::treatAdditionalConvergenceChecks() {
    this->treatCodeBlock(*this, BehaviourData::AdditionalConvergenceChecks,
                         &ImplicitDSLBase::standardModifier, true, true);
  }  // end of treatAdditionalConvergenceChecks()

  void
  ImplicitDSLBase::treatPerturbationValueForNumericalJacobianComputation() {
    const auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    this->checkNotEndOfFile(
        "ImplicitDSLBase::"
        "treatPerturbationValueForNumericalJacobianComputation",
        "Cannot read epsilon value.");
    const auto epsilon = this->readDouble();
    if (epsilon < 0) {
      this->throwRuntimeError(
          "ImplicitDSLBase::"
          "treatPerturbationValueForNumericalJacobianComputation",
          "Epsilon value must be positive.");
    }
    this->readSpecifiedToken(
        "ImplicitDSLBase::"
        "treatPerturbationValueForNumericalJacobianComputation",
        ";");
    VariableDescription e("NumericType", "numerical_jacobian_epsilon", 1u, 0u);
    e.description =
        "perturbation value used to compute a finite difference approximation "
        "of the jacobian";
    this->mb.addParameter(h, e, BehaviourData::ALREADYREGISTRED);
    this->mb.setParameterDefaultValue(h, "numerical_jacobian_epsilon", epsilon);
  }  // ImplicitDSLBase::treatEpsilon

  void ImplicitDSLBase::treatIterMax() {
    const auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    const auto iterMax =
        this->readUnsignedShort("ImplicitDSLBase::treatIterMax");
    if (iterMax == 0) {
      this->throwRuntimeError("ImplicitDSLBase::treatIterMax",
                              "invalid value for parameter 'iterMax'");
    }
    this->readSpecifiedToken("ImplicitDSLBase::treatIterMax", ";");
    this->mb.addParameter(h, VariableDescription("ushort", "iterMax", 1u, 0u),
                          BehaviourData::ALREADYREGISTRED);
    this->mb.setParameterDefaultValue(h, "iterMax", iterMax);
  }  // end of treatIterMax

  std::string ImplicitDSLBase::tangentOperatorVariableModifier(
      const Hypothesis h, const std::string& var, const bool addThisPtr) {
    const auto& d = this->mb.getBehaviourData(h);
    if (d.isIntegrationVariableIncrementName(var)) {
      const auto v = var.substr(1);
      if (this->mb.hasAttribute(h, v + "_normalisation_factor")) {
        const auto& s = d.getStateVariableDescription(v);
        const auto& nf =
            this->mb.getAttribute<std::string>(h, v + "_normalisation_factor");
        if (s.arraySize == 1u) {
          if (addThisPtr) {
            return "((" + nf + ")*(this->" + var + "))";
          } else {
            return "(" + nf + ")*var";
          }
        } else {
          if (addThisPtr) {
            return "(" + nf + ")*(this->" + var + ")";
          } else {
            return "(" + nf + ")*var";
          }
        }
      }
    }
    if (addThisPtr) {
      return "(this->" + var + ")";
    } else {
      return var;
    }
  }  // end of tangentOperatorVariableModifier

  std::string ImplicitDSLBase::integratorVariableModifier(
      const Hypothesis h, const std::string& var, const bool addThisPtr) {
    const auto& d = this->mb.getBehaviourData(h);
    if (d.isIntegrationVariableIncrementName(var)) {
      if (this->mb.hasAttribute(h, var.substr(1) + "_normalisation_factor")) {
        const auto& s = d.getStateVariableDescription(var.substr(1));
        const auto& nf = this->mb.getAttribute<std::string>(
            h, var.substr(1) + "_normalisation_factor");
        if (s.arraySize == 1u) {
          if (addThisPtr) {
            return "((" + nf + ")*(this->" + var + "))";
          } else {
            return "(" + nf + ")*var";
          }
        } else {
          if (addThisPtr) {
            return "(" + nf + ")*(this->" + var + ")";
          } else {
            return "(" + nf + ")*var";
          }
        }
      }
    }
    if (addThisPtr) {
      return "(this->" + var + ")";
    } else {
      return var;
    }
  }  // end of integratorVariableModifier

  std::string ImplicitDSLBase::computeThermodynamicForcesVariableModifier1(
      const Hypothesis h, const std::string& var, const bool addThisPtr) {
    const auto& d = this->mb.getBehaviourData(h);
    if (this->mb.isGradientName(var) || (d.isExternalStateVariableName(var))) {
      if (addThisPtr) {
        return "(this->" + var + "+(this->theta)*(this->d" + var + "))";
      } else {
        return "(" + var + "+(this->theta)*d" + var + ")";
      }
    }
    if (d.isIntegrationVariableName(var)) {
      if (this->mb.hasAttribute(h, var + "_normalisation_factor")) {
        const auto& nf = this->mb.getAttribute<std::string>(
            h, var + "_normalisation_factor");
        if (addThisPtr) {
          return "(this->" + var + "+(this->theta)*((" + nf + ")*(this->d" +
                 var + ")))";
        } else {
          return "(" + var + "+(" + nf + ")*(this->theta)*d" + var + ")";
        }
      } else {
        if (addThisPtr) {
          return "(this->" + var + "+(this->theta)*(this->d" + var + "))";
        } else {
          return "(" + var + "+(this->theta)*d" + var + ")";
        }
      }
    }
    if ((d.isExternalStateVariableIncrementName(var)) || (var == "dT")) {
      this->mb
          .declareExternalStateVariableProbablyUnusableInPurelyImplicitResolution(
              h, var.substr(1));
    }
    if (addThisPtr) {
      return "this->" + var;
    }
    return var;
  }  // end of computeThermodynamicForcesVariableModifier1

  std::string ImplicitDSLBase::computeThermodynamicForcesVariableModifier2(
      const Hypothesis h, const std::string& var, const bool addThisPtr) {
    const auto& d = this->mb.getBehaviourData(h);
    if ((this->mb.isGradientName(var)) ||
        (d.isExternalStateVariableName(var))) {
      if (addThisPtr) {
        return "(this->" + var + "+this->d" + var + ")";
      } else {
        return "(" + var + "+d" + var + ")";
      }
    }
    if ((d.isExternalStateVariableIncrementName(var)) || (var == "dT")) {
      this->mb
          .declareExternalStateVariableProbablyUnusableInPurelyImplicitResolution(
              h, var.substr(1));
    }
    if (addThisPtr) {
      return "this->" + var;
    }
    return var;
  }  // end of computeThermodynamicForcesVariableModifier2

  void ImplicitDSLBase::integratorAnalyser(const Hypothesis h,
                                           const std::string& w) {
    const auto is_jacobian_part = [this, h, &w] {
      const auto& d = this->mb.getBehaviourData(h);
      for (const auto& v1 : d.getIntegrationVariables()) {
        for (const auto& v2 : d.getIntegrationVariables()) {
          // symbolic names are not treated here, because they were already
          // treated previously
          if (w == "df" + v1.name + "_dd" + v2.name) {
            auto previous = this->current;
            if (previous == this->begin()) {
              return true;
            }
            --previous;
            if (previous->value == ".") {
              return false;
            } else if (previous->value == "->") {
              if (previous == this->begin()) {
                return false;
              }
              --previous;
              return previous->value == "this";
            } else if (previous->value == "::") {
              if (previous == this->begin()) {
                return false;
              }
              --previous;
              return previous->value == this->mb.getClassName();
            }
            return true;
          }
        }
      }
      return false;
    }();
    if (is_jacobian_part) {
      this->jacobianPartsUsedInIntegrator.insert(w);
    }
  }  // end of integratorAnalyser

  void ImplicitDSLBase::predictorAnalyser(const Hypothesis h,
                                          const std::string& w) {
    const auto& d = this->mb.getBehaviourData(h);
    if (d.isIntegrationVariableIncrementName(w)) {
      this->integrationVariablesIncrementsUsedInPredictor.insert(w);
    }
  }  // end of predictorAnalyser

  void ImplicitDSLBase::treatIntegrator() {
    this->treatCodeBlock(*this, BehaviourData::Integrator,
                         &ImplicitDSLBase::integratorVariableModifier,
                         &ImplicitDSLBase::integratorAnalyser, true);
  }  // end of treatIntegrator

  void ImplicitDSLBase::treatPredictor() {
    this->treatCodeBlock(*this, BehaviourData::ComputePredictor,
                         &ImplicitDSLBase::standardModifier,
                         &ImplicitDSLBase::predictorAnalyser, true);
  }  // end of treatPredictor

  void ImplicitDSLBase::treatComputeThermodynamicForces() {
    /*
     * Most behaviours will only rely the @ComputeStress keyword to
     * estimate stresses at the middle of the time step and at the
     * end.  However, some finite strains behaviours must compute a
     * stress measure during the iterations which is not the Cauchy
     * stress. Thus, @ComputeStress also defines an
     * ComputeFinalStressCandidate code block which will be used if
     * the user does not provide an alternative through the
     * @ComputeFinalStress
     */
    this->treatCodeBlock(
        *this, BehaviourData::ComputeThermodynamicForces,
        BehaviourData::ComputeFinalThermodynamicForcesCandidate,
        &ImplicitDSLBase::computeThermodynamicForcesVariableModifier1,
        &ImplicitDSLBase::computeThermodynamicForcesVariableModifier2, true,
        true);
  }  // end of treatComputeThermodynamicForces

  void ImplicitDSLBase::treatComputeFinalThermodynamicForces() {
    this->treatCodeBlock(
        *this, BehaviourData::ComputeFinalThermodynamicForces,
        &ImplicitDSLBase::computeThermodynamicForcesVariableModifier2, true,
        true);
  }  // end of treatComputeFinalThermodynamicForces

  void ImplicitDSLBase::readTangentOperatorCodeBlock(const CodeBlockOptions& o,
                                                     const std::string& n) {
    this->treatUnsupportedCodeBlockOptions(o);
    // variable modifier
    std::function<std::string(const Hypothesis, const std::string&, const bool)>
        m = [this](const Hypothesis h, const std::string& v, const bool b) {
          return this->tangentOperatorVariableModifier(h, v, b);
        };
    // analyser
    std::set<std::string> get_integration_variables_derivatives_names;
    std::map<std::string, VariableDescription>
        get_integration_variables_derivatives;
    for (const auto& to : this->mb.getTangentOperatorBlocks()) {
      const auto& name = to.second.name;
      get_integration_variables_derivatives
          ["getIntegrationVariablesDerivatives_" + name] = to.second;
      get_integration_variables_derivatives_names.insert(
          "getIntegrationVariablesDerivatives_" + name);
    }
    std::function<void(CodeBlock&, const Hypothesis, const std::string&)> a =
        [this, get_integration_variables_derivatives_names,
         get_integration_variables_derivatives](
            CodeBlock& cb, const Hypothesis h, const std::string& v) {
          using Derivative =
              std::pair<VariableDescription, VariableDescription>;
          const auto& d = this->mb.getBehaviourData(h);
          std::map<std::string, Derivative> implicit_equation_derivatives;
          std::map<std::string, Derivative> jacobian_invert_blocks;
          for (const auto& to : this->mb.getTangentOperatorBlocks()) {
            for (const auto& iv : d.getIntegrationVariables()) {
              const auto n1 = "df" + iv.name + "_dd" + to.second.name;
              const auto derivative = std::make_pair(iv, to.second);
              implicit_equation_derivatives.insert({n1, derivative});
            }
          }
          for (const auto& iv1 : d.getIntegrationVariables()) {
            for (const auto& iv2 : d.getIntegrationVariables()) {
              const auto n1 = "iJ_" + iv1.name + "_" + iv2.name;
              const auto derivative = std::make_pair(iv1, iv2);
              jacobian_invert_blocks.insert({n1, derivative});
            }
          }
          const auto an = CodeBlock::requires_jacobian_decomposition;
          const auto bn = CodeBlock::uses_get_partial_jacobian_invert;
          const auto cn = CodeBlock::used_get_integration_variables_derivatives;
          const auto dn = CodeBlock::
              used_implicit_equations_derivatives_with_respect_to_gradients_or_external_state_variables;
          const auto iJn = CodeBlock::used_jacobian_invert_blocks;
          const auto pd = implicit_equation_derivatives.find(v);
          const auto piJ = jacobian_invert_blocks.find(v);
          // the jacobian must be decomposed
          if ((v == "getPartialJacobianInvert") ||
              (pd != implicit_equation_derivatives.end()) ||
              (piJ != jacobian_invert_blocks.end()) ||
              (get_integration_variables_derivatives_names.count(v) != 0)) {
            if (cb.attributes.count(an) == 0) {
              cb.attributes[an] = true;
            } else {
              if (!cb.attributes[an].is<bool>()) {
                this->throwRuntimeError(
                    "ImplicitDSLBase::readTangentOperatorCodeBlock",
                    "invalid type for attribute '" + std::string(an) + "'");
              }
              if (!cb.attributes[an].get<bool>()) {
                this->throwRuntimeError(
                    "ImplicitDSLBase::readTangentOperatorCodeBlock",
                    "inconsistent value for attribute '" + std::string(an) +
                        "'");
              }
            }
          }
          //
          if (v == "getPartialJacobianInvert") {
            if (cb.attributes.count(bn) == 0) {
              cb.attributes[bn] = true;
            } else {
              if (!cb.attributes[bn].is<bool>()) {
                this->throwRuntimeError(
                    "ImplicitDSLBase::readTangentOperatorCodeBlock",
                    "invalid type for attribute '" + std::string(bn) + "'");
              }
              if (!cb.attributes[bn].get<bool>()) {
                this->throwRuntimeError(
                    "ImplicitDSLBase::readTangentOperatorCodeBlock",
                    "inconsistent value for attribute '" + std::string(bn) +
                        "'");
              }
            }
          }
          // a getIntegrationVariablesDerivative_X has been found
          if (get_integration_variables_derivatives_names.count(v) != 0) {
            const auto& dv = get_integration_variables_derivatives.at(v);
            // we check that at least one derivative of one implicit equation
            // has been defined
            if (cb.attributes.count(dn) == 0) {
              this->throwRuntimeError(
                  "ImplicitDSLBase::readTangentOperatorCodeBlock",
                  "no derivative of an implicit equation with respect "
                  "to '" +
                      displayName(dv) +
                      "' before the call to "
                      "'getIntegrationVariablesDerivatives_" +
                      displayName(dv) + "'");
            } else {
              if (!cb.attributes[dn].is<std::vector<Derivative>>()) {
                this->throwRuntimeError(
                    "ImplicitDSLBase::readTangentOperatorCodeBlock",
                    "invalid type for attribute '" + std::string(dn) + "'");
              }
              const auto& derivatives =
                  cb.attributes[dn].get<std::vector<Derivative>>();
              const auto p =
                  std::find_if(derivatives.begin(), derivatives.end(),
                               [&dv](const Derivative& d1) {
                                 return d1.second.name == dv.name;
                               });
              if (p == derivatives.end()) {
                this->throwRuntimeError(
                    "ImplicitDSLBase::readTangentOperatorCodeBlock",
                    "no derivative of an implicit equation with respect "
                    "to '" +
                        displayName(dv) +
                        "' before the call to "
                        "'getIntegrationVariablesDerivatives_" +
                        displayName(dv) + "'");
              }
            }
            if (cb.attributes.count(cn) == 0) {
              cb.attributes[cn] = std::vector<VariableDescription>(1u, dv);
            } else {
              if (!cb.attributes[cn].is<std::vector<VariableDescription>>()) {
                this->throwRuntimeError(
                    "ImplicitDSLBase::readTangentOperatorCodeBlock",
                    "invalid type for attribute '" + std::string(cn) + "'");
              }
              auto& variables =
                  cb.attributes[cn].get<std::vector<VariableDescription>>();
              const auto p = std::find_if(variables.begin(), variables.end(),
                                          [&dv](const VariableDescription& vv) {
                                            return vv.name == dv.name;
                                          });
              if (p == variables.end()) {
                variables.push_back(dv);
              }
            }
          }
          auto add_derivative_in_attribute = [this, &cb](
                                                 const char* const attribute,
                                                 const Derivative& derivative) {
            if (cb.attributes.count(attribute) == 0) {
              cb.attributes[attribute] =
                  std::vector<Derivative>(1u, derivative);
            } else {
              if (!cb.attributes[attribute].is<std::vector<Derivative>>()) {
                this->throwRuntimeError(
                    "ImplicitDSLBase::readTangentOperatorCodeBlock",
                    "invalid type for attribute '" + std::string(attribute) +
                        "'");
              }
              auto& derivatives =
                  cb.attributes[attribute].get<std::vector<Derivative>>();
              const auto p = std::find_if(
                  derivatives.begin(), derivatives.end(),
                  [&derivative](const Derivative& d1) {
                    return (d1.first.name == derivative.first.name) &&
                           (d1.second.name == derivative.second.name);
                  });
              if (p == derivatives.end()) {
                derivatives.push_back(derivative);
              }
            }
          };
          // a derivative of an implicit equation derivative has been found
          if (pd != implicit_equation_derivatives.end()) {
            add_derivative_in_attribute(dn, pd->second);
          }
          // the invert of a jacobian block has been found
          if (piJ != jacobian_invert_blocks.end()) {
            add_derivative_in_attribute(iJn, piJ->second);
          }
        };
    this->treatCodeBlock(o, n, m, a, true);
  }  // end of readTangentOperatorCodeBlock

  void ImplicitDSLBase::treatMaximumIncrementValuePerIteration() {
    const auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    this->checkNotEndOfFile(
        "ImplicitDSLBase::treatMaximumIncrementValuePerIteration",
        "Cannot read value value.");
    const auto value = tfel::utilities::convert<double>(current->value);
    ++(this->current);
    if (value <= 0) {
      this->throwRuntimeError(
          "ImplicitDSLBase::treatMaximumIncrementValuePerIteration",
          "Value must be positive.");
    }
    this->readSpecifiedToken(
        "ImplicitDSLBase::MaximumIncrementValuePerIteration", ";");
    this->mb.addParameter(
        h,
        VariableDescription("real", "maximum_increment_value_per_iteration", 1u,
                            0u),
        BehaviourData::ALREADYREGISTRED);
    this->mb.setParameterDefaultValue(
        h, "maximum_increment_value_per_iteration", value);
  }  // end of treatMaximumIncrementValuePerIteration

  void ImplicitDSLBase::treatNumericallyComputedJacobianBlocks() {
    const std::string m =
        "ImplicitDSLBase::treatNumericallyComputedJacobianBlocks";
    auto throw_if = [this, m](const bool b, const std::string& msg) {
      if (b) {
        this->throwRuntimeError(m, msg);
      }
    };
    for (const auto& h : this->readHypothesesList()) {
      const auto as = this->readList(m, "{", "}", false);
      this->readSpecifiedToken(m, ";");
      throw_if(as.empty(), "no block defined");
      const auto jbs = [&as] {
        auto r = std::vector<std::string>(as.size());
        std::transform(as.begin(), as.end(), r.begin(),
                       [](const tfel::utilities::Token& t) { return t.value; });
        return r;
      }();
      std::for_each(jbs.begin(), jbs.end(), [throw_if](const std::string& jb) {
        throw_if(jb.empty(), "empty jacobian block");
        throw_if(jb.size() < 6, "invalid jacobian block '" + jb + "'");
        throw_if(jb[0] != 'd', "invalid jacobian block '" + jb + "'");
        throw_if(jb[1] != 'f', "invalid jacobian block '" + jb + "'");
        const auto p = jb.find('_');
        throw_if(p == std::string::npos, "invalid jacobian block '" + jb + "'");
        throw_if(p + 2 >= jb.size(), "invalid jacobian block '" + jb + "'");
        throw_if(jb[p + 1] != 'd', "invalid jacobian block '" + jb + "'");
        throw_if(jb[p + 2] != 'd', "invalid jacobian block '" + jb + "'");
        const auto n = jb.substr(2, p - 2);
        const auto d = jb.substr(p + 3);
        throw_if(n.empty(), "invalid jacobian block '" + jb + "'");
        throw_if(d.empty(), "invalid jacobian block '" + jb + "'");
      });
      for (const auto& jb : jbs) {
        throw_if(std::count(jbs.begin(), jbs.end(), jb) > 1,
                 "jacobian block '" + jb + "' multiply delcared");
      }
      if (this->mb.hasAttribute(
              h, BehaviourData::numericallyComputedJacobianBlocks)) {
        auto cjbs = this->mb.getAttribute<std::vector<std::string>>(
            h, BehaviourData::numericallyComputedJacobianBlocks);
        for (const auto& jb : jbs) {
          throw_if(std::find(cjbs.begin(), cjbs.end(), jb) != cjbs.end(),
                   "jacobian block '" + jb + "' multiply delcared");
          cjbs.push_back(jb);
        }
        this->mb.updateAttribute(
            h, BehaviourData::numericallyComputedJacobianBlocks, cjbs);
      } else {
        this->mb.setAttribute(
            h, BehaviourData::numericallyComputedJacobianBlocks, jbs);
      }
    }
  }  // end of treatNumericallyComputedJacobianBlocks

  void ImplicitDSLBase::completeVariableDeclaration() {
    using namespace tfel::glossary;
    const auto uh = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    if (this->solver == nullptr) {
      this->setNonLinearSolver("NewtonRaphson");
    }
    BehaviourDSLCommon::completeVariableDeclaration();
    if (this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) {
      auto D = VariableDescription("StiffnessTensor", "D", 1u, 0u);
      D.description =
          "stiffness tensor computed from elastic "
          "material properties";
      this->mb.addLocalVariable(uh, D, BehaviourData::ALREADYREGISTRED);
      auto D_tdt =
          this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep()
              ? VariableDescription("StiffnessTensor&", "D_tdt", 1u, 0u)
              : VariableDescription("StiffnessTensor", "D_tdt", 1u, 0u);
      D_tdt.description =
          "stiffness tensor computed from elastic "
          "material properties";
      this->mb.addLocalVariable(uh, D_tdt, BehaviourData::ALREADYREGISTRED);
    }
    if ((!this->mb.getAttribute(BehaviourDescription::computesStiffnessTensor,
                                false)) &&
        (this->mb.getElasticSymmetryType() == ISOTROPIC) &&
        (this->mb.areElasticMaterialPropertiesDefined())) {
      auto add_lv = [this](BehaviourDescription& bd, const std::string& t,
                           const std::string& sn, const std::string& n,
                           const std::string& g, const std::string d) {
        auto r = bd.checkVariableExistence(n, "Parameter", false);
        if (!r.first) {
          auto v = (!sn.empty()) ? VariableDescription(t, sn, n, 1u, 0u)
                                 : VariableDescription(t, n, 1u, 0u);
          v.description = d;
          bd.addLocalVariable(uh, v, BehaviourData::UNREGISTRED);
        } else {
          if (!r.second) {
            this->throwRuntimeError(
                "ImplicitDSLBase::completeVariableDeclaration",
                "Parameter '" + n + "' is not defined for all hypotheses");
          }
          if (!g.empty()) {
            bd.checkVariableGlossaryName(n, g);
          }
        }
      };
      add_lv(this->mb, "stress", "", "young", Glossary::YoungModulus,
             "Young modulus at t+theta*dt");
      add_lv(this->mb, "real", "\u03BD", "nu", Glossary::PoissonRatio,
             "Poisson ratio at t+theta*dt");
      add_lv(this->mb, "stress", "\u03BB", "lambda",
             Glossary::FirstLameCoefficient,
             "first Lamé coefficient at t+theta*dt");
      add_lv(this->mb, "stress", "\u03BC", "mu", Glossary::ShearModulus,
             "shear modulus at t+theta*dt");
      add_lv(this->mb, "stress", "", "young_tdt", "", "Young modulus at t+dt");
      add_lv(this->mb, "real", "\u03BD\u2091\u209C\u209B", "nu_tdt", "",
             "Poisson ratio at t+dt");
      add_lv(this->mb, "stress", "\u03BB\u2091\u209C\u209B", "lambda_tdt", "",
             "first Lamé coefficient at t+dt");
      add_lv(this->mb, "stress", "\u03BC\u2091\u209C\u209B", "mu_tdt", "",
             "shear modulus at t+dt");
    }
    for (const auto& ht : this->mb.getHillTensors()) {
      if ((this->mb.getBehaviourType() !=
           BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) &&
          (this->mb.getBehaviourType() !=
           BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR)) {
        this->throwRuntimeError(
            "ImplicitDSLBase::completeVariableDeclaration",
            "Hill tensors shall only be defined for finite strain "
            "or small strain behaviours");
      }
      if (ht.symbolic_form.empty()) {
        const auto hn = ht.name + "_tdt";
        auto H_tdt =
            this->mb.areMaterialPropertiesConstantDuringTheTimeStep(ht.c)
                ? VariableDescription("tfel::math::st2tost2<N,real>&", hn, 1u,
                                      0u)
                : VariableDescription("tfel::math::st2tost2<N,real>", hn, 1u,
                                      0u);
        H_tdt.description =
            "Hill tensor '" + ht.name + "' at the end of the time step";
        this->mb.addLocalVariable(uh, H_tdt);
      } else {
        const auto hsn = ht.symbolic_form + "\u2091\u209C\u209B";
        const auto hn = ht.name + "_tdt";
        auto H_tdt =
            this->mb.areMaterialPropertiesConstantDuringTheTimeStep(ht.c)
                ? VariableDescription("tfel::math::st2tost2<N,real>&", hsn, hn,
                                      1u, 0u)
                : VariableDescription("tfel::math::st2tost2<N,real>", hsn, hn,
                                      1u, 0u);
        H_tdt.description =
            "Hill tensor '" + hsn + "' at the end of the time step";
        this->mb.addLocalVariable(uh, H_tdt);
      }
    }
    // creating default parameters if not explicitely specified by the user
    if (!this->mb.hasParameter(uh, "epsilon")) {
      VariableDescription e("NumericType", "\u03B5", "epsilon", 1u, 0u);
      e.description =
          "value used to stop the iteration of the implicit algorithm";
      this->mb.addParameter(uh, e, BehaviourData::ALREADYREGISTRED);
      this->mb.setEntryName(uh, "epsilon", "epsilon");
      this->mb.setParameterDefaultValue(uh, "epsilon", 1.e-8);
    }
    if (!this->mb.hasParameter(uh, "theta")) {
      VariableDescription tv("NumericType", "\u03B8", "theta", 1u, 0u);
      tv.description = "theta value used by the implicit scheme";
      this->mb.addParameter(uh, tv, BehaviourData::ALREADYREGISTRED);
      this->mb.setEntryName(uh, "theta", "theta");
      this->mb.setParameterDefaultValue(uh, "theta", 0.5);
    }
    this->solver->completeVariableDeclaration(this->mb);
    if ((this->mb.getAttribute(uh, BehaviourData::compareToNumericalJacobian,
                               false)) ||
        (this->solver->usesJacobian())) {
      const std::string nje = "numerical_jacobian_epsilon";
      if (!this->mb.hasParameter(uh, nje)) {
        const auto eps = 0.1 * this->mb.getFloattingPointParameterDefaultValue(
                                   uh, "epsilon");
        VariableDescription v("NumericType", nje, 1u, 0u);
        v.description =
            "perturbation value used to compute a numerical "
            "approximation of the jacobian";
        this->mb.addParameter(uh, v, BehaviourData::ALREADYREGISTRED);
        this->mb.setParameterDefaultValue(uh, nje, eps);
      }
    }
    if (!this->mb.hasParameter(uh, "iterMax")) {
      ushort iterMax = 100u;
      VariableDescription v("ushort", "iterMax", 1u, 0u);
      v.description = "maximum number of iterations allowed";
      this->mb.addParameter(uh, v, BehaviourData::ALREADYREGISTRED);
      this->mb.setParameterDefaultValue(uh, "iterMax", iterMax);
    }
    if (this->mb.getAttribute(uh, BehaviourData::compareToNumericalJacobian,
                              false)) {
      if (!this->mb.hasParameter(uh, "jacobianComparisonCriterion")) {
        VariableDescription v("real", "jacobianComparisonCriterion", 1u, 0u);
        v.description =
            "criterion value used to compare the jacobian "
            "provided by the user to its numerical approximation";
        this->mb.addParameter(uh, v, BehaviourData::ALREADYREGISTRED);
        this->mb.setParameterDefaultValue(
            uh, "jacobianComparisonCriterion",
            this->mb.getFloattingPointParameterDefaultValue(uh, "epsilon"));
      }
    }
    // reserve names for jacobian blocks
    const auto mh = this->mb.getDistinctModellingHypotheses();
    for (const auto& h : mh) {
      const auto& d = this->mb.getBehaviourData(h);
      for (const auto& v1 : d.getIntegrationVariables()) {
        for (const auto& v2 : d.getIntegrationVariables()) {
          // Things are a bit tricky here because of the undefined hypothesis
          // case which forwards everything to specialised hypothesis.
          if ((h != uh) && (mh.find(uh) != mh.end()) &&  //
              ((this->mb.getBehaviourData(uh)
                    .getIntegrationVariables()
                    .contains(v1.name)) &&
               (this->mb.getBehaviourData(uh)
                    .getIntegrationVariables()
                    .contains(v2.name)))) {
            continue;
          }
          this->mb.reserveName(h, "df" + v1.name + "_dd" + v2.name);
        }
      }
    }
    std::set<std::string>
        gradients_or_external_state_variables_in_tangent_operator_blocks;
    for (const auto& to : mb.getTangentOperatorBlocks()) {
      gradients_or_external_state_variables_in_tangent_operator_blocks.insert(
          to.second.name);
    }
    for (const auto& vn :
         gradients_or_external_state_variables_in_tangent_operator_blocks) {
      this->reserveName("GetIntegrationVariablesDerivatives_" + vn);
      this->reserveName("getIntegrationVariablesDerivatives_" + vn);
      this->reserveName("dfzeros_dd" + vn);
      std::set<std::string> implicit_equation_derivatives;
      for (const auto h : this->mb.getDistinctModellingHypotheses()) {
        const auto& d = this->mb.getBehaviourData(h);
        for (const auto& v : d.getIntegrationVariables()) {
          implicit_equation_derivatives.insert("df" + v.name + "_dd" + vn);
        }
      }
      for (const auto& ieq_derivative : implicit_equation_derivatives) {
        this->mb.reserveName(uh, ieq_derivative);
      }
    }
    std::set<std::string> jacobian_invert_blocks;
    for (const auto h : this->mb.getDistinctModellingHypotheses()) {
      const auto& d = this->mb.getBehaviourData(h);
      for (const auto& iv1 : d.getIntegrationVariables()) {
        for (const auto& iv2 : d.getIntegrationVariables()) {
          jacobian_invert_blocks.insert("iJ_" + iv1.name + "_" + iv2.name);
        }
      }
    }
    for (const auto& iJb : jacobian_invert_blocks) {
      this->mb.reserveName(uh, iJb);
    }
  }  // end of completeVariableDeclaration()

  void ImplicitDSLBase::endsInputFileProcessing() {
    using namespace tfel::glossary;
    using namespace tfel::material;
    auto throw_if = [this](const bool c, const std::string& m) {
      if (c) {
        this->throwRuntimeError("ImplicitDSLBase::endsInputFileProcessing", m);
      }
    };
    const auto uh = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    BehaviourDSLCommon::endsInputFileProcessing();
    // Supported modelling hypothesis
    const auto mh = this->mb.getDistinctModellingHypotheses();
    // check that at leas an integration variable is
    for (const auto& h : mh) {
      const auto n = mfront::getTypeSize(
          this->mb.getBehaviourData(h).getIntegrationVariables());
      if (n.isNull()) {
        tfel::raise(
            "ImplicitDSLBase::endsInputFileProcessing: "
            "no integration variables defined");
      }
    }
    // consistency checks
    if (this->mb.getAttribute(uh, BehaviourData::compareToNumericalJacobian,
                              false)) {
      throw_if((!this->solver->usesJacobian()) ||
                   (this->solver->requiresNumericalJacobian()),
               "@CompareToNumericalJacobian can only be used with solver using "
               "an analytical jacobian (or an approximation of it");
    }
    // create the compute final stress code is necessary
    this->setComputeFinalThermodynamicForcesFromComputeFinalThermodynamicForcesCandidateIfNecessary();
    // correct prediction to take into account normalisation factors
    for (const auto& h : mh) {
      if (this->mb.hasAttribute(
              h, BehaviourData::numericallyComputedJacobianBlocks)) {
        throw_if((!this->solver->usesJacobian()) ||
                     (this->solver->requiresNumericalJacobian()),
                 "numerically computed jacobian blocks can only be "
                 "used with solver using an analytical jacobian "
                 "(or an approximation of it");
        auto decompose =
            [throw_if](
                const std::string& jb) -> std::pair<std::string, std::string> {
          throw_if(jb.empty(), "empty jacobian block");
          throw_if(jb.size() < 6, "invalid jacobian block '" + jb + "'");
          throw_if(jb[0] != 'd', "invalid jacobian block '" + jb + "'");
          throw_if(jb[1] != 'f', "invalid jacobian block '" + jb + "'");
          const auto p = jb.find('_');
          throw_if(p == std::string::npos,
                   "invalid jacobian block '" + jb + "'");
          throw_if(p + 2 >= jb.size(), "invalid jacobian block '" + jb + "'");
          throw_if(jb[p + 1] != 'd', "invalid jacobian block '" + jb + "'");
          throw_if(jb[p + 2] != 'd', "invalid jacobian block '" + jb + "'");
          const auto n = jb.substr(2, p - 2);
          const auto d = jb.substr(p + 3);
          throw_if(n.empty(), "invalid jacobian block '" + jb + "'");
          throw_if(d.empty(), "invalid jacobian block '" + jb + "'");
          return {n, d};
        };
        const auto& jbs = this->mb.getAttribute<std::vector<std::string>>(
            h, BehaviourData::numericallyComputedJacobianBlocks);
        for (const auto& jb : jbs) {
          const auto nd = decompose(jb);
          throw_if(!this->mb.isIntegrationVariableName(h, nd.first),
                   "invalid jacobian block '" + jb +
                       "', "
                       "'" +
                       nd.first + "' is not an integration variable");
          throw_if(!this->mb.isIntegrationVariableName(h, nd.second),
                   "invalid jacobian block '" + jb +
                       "', "
                       "'" +
                       nd.second + "' is not an integration variable");
        }
      }
      if (this->mb.hasCode(h, BehaviourData::ComputePredictor)) {
        CodeBlock predictor;
        const auto& d = this->mb.getBehaviourData(h);
        for (const auto& v : d.getIntegrationVariables()) {
          if (this->integrationVariablesIncrementsUsedInPredictor.find(
                  'd' + v.name) !=
              this->integrationVariablesIncrementsUsedInPredictor.end()) {
            if (this->mb.hasAttribute(h, v.name + "_normalisation_factor")) {
              const auto& nf = this->mb.getAttribute<std::string>(
                  h, v.name + "_normalisation_factor");
              predictor.code += "this->d" + v.name + " /= " + nf + ";\n";
            }
          }
        }
        this->mb.setCode(h, BehaviourData::ComputePredictor, predictor,
                         BehaviourData::CREATEORAPPEND, BehaviourData::AT_END);
      }
    }
    if (!this->mb.areAllMechanicalDataSpecialised()) {
      if (this->mb.hasCode(uh, BehaviourData::ComputePredictor)) {
        CodeBlock predictor;
        const auto& d = this->mb.getBehaviourData(uh);
        for (const auto& v : d.getIntegrationVariables()) {
          if (this->integrationVariablesIncrementsUsedInPredictor.find(
                  'd' + v.name) !=
              this->integrationVariablesIncrementsUsedInPredictor.end()) {
            if (this->mb.hasAttribute(uh, v.name + "_normalisation_factor")) {
              const auto& nf = this->mb.getAttribute<std::string>(
                  uh, v.name + "_normalisation_factor");
              predictor.code += "this->d" + v.name + " /= " + nf + ";\n";
            }
          }
        }
        this->mb.setCode(uh, BehaviourData::ComputePredictor, predictor,
                         BehaviourData::CREATEORAPPEND, BehaviourData::AT_END);
      }
    }
    // minimal tangent operator
    this->setMinimalTangentOperator();
  }  // end of endsInputFileProcessing()

  void ImplicitDSLBase::writeBehaviourParserSpecificInheritanceRelationship(
      std::ostream& os, const Hypothesis h) const {
    os << ",\npublic "
       << this->solver->getExternalAlgorithmClassName(this->mb, h);
  }  // end of writeBehaviourParserSpecificInheritanceRelationship

  void ImplicitDSLBase::writeBehaviourFriends(std::ostream& os,
                                              const Hypothesis h) const {
    BehaviourDSLCommon::writeBehaviourFriends(os, h);
    const auto hn = [&h]() -> std::string {
      if (h == tfel::material::ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
        return "hypothesis";
      }
      return "ModellingHypothesis::" +
             tfel::material::ModellingHypothesis::toUpperCaseString(h);
    }();
    const auto n =
        mfront::getTypeSize(
            this->mb.getBehaviourData(h).getIntegrationVariables())
            .asString({"ModellingHypothesisToSpaceDimension<" + hn + ">::value",
                       "ModellingHypothesisToStensorSize<" + hn + ">::value",
                       "ModellingHypothesisToTensorSize<" + hn + ">::value"});
    os << "friend struct tfel::math::TinyNonLinearSolverBase<" << n
       << ", NumericType, " << this->mb.getClassName() << ">;\n";
  }  // end of writeBehaviourFriends

  void ImplicitDSLBase::writeDerivativeView(
      std::ostream& os, const DerivativeViewDescription& d) const {
    const auto v1 = d.first_variable;
    const auto v2 = d.second_variable;
    const auto dr = d.derivative_row_position;
    const auto dc = d.derivative_column_position;
    if ((v1.arraySize == 1u) && (v2.arraySize == 1u)) {
      os << "auto " << d.derivative_name << " = tfel::math::map_derivative<"
         << dr << ", " << dc << ", " << v1.type << ", " << v2.type << ">("
         << d.matrix_name << ");\n";
    } else if ((v1.arraySize != 1u) && (v2.arraySize == 1u)) {
      os << "auto " << d.derivative_name << " = [&" << d.matrix_name
         << "](const ushort mfront_idx){\n"
         << "return tfel::math::map_derivative<" << v1.type << ", " << v2.type
         << ">(" << d.matrix_name << ", " << dr << " + mfront_idx * "
         << this->getTypeSize(v1.type, 1u) << ", " << dc << ");\n"
         << "};\n";
    } else if ((v1.arraySize == 1u) && (v2.arraySize != 1u)) {
      os << "auto " << d.derivative_name << " = [&" << d.matrix_name
         << "](const ushort mfront_idx){\n"
         << "return tfel::math::map_derivative<" << v1.type << ", " << v2.type
         << ">(" << d.matrix_name << ", " << dr << ", " << dc
         << " + mfront_idx * " << this->getTypeSize(v2.type, 1u) << ");\n"
         << "};\n";
    } else {
      os << "auto " << d.derivative_name << " = [&" << d.matrix_name
         << "](const ushort mfront_idx, const ushort "
            "mfront_idx2){\n"
         << "return tfel::math::map_derivative<" << v1.type << ", " << v2.type
         << ">(" << d.matrix_name << ", " << dr << " + mfront_idx * "
         << this->getTypeSize(v1.type, 1u) << ", " << dc << " + mfront_idx2 * "
         << this->getTypeSize(v2.type, 1u) << ");\n"
         << "};\n";
    }
  }  // end of writeDerivativeView

  void ImplicitDSLBase::writeBehaviourLocalVariablesInitialisation(
      std::ostream& os, const Hypothesis h) const {
    using Modifier = std::function<std::string(const MaterialPropertyInput&)>;
    Modifier mts = [this](const MaterialPropertyInput& i) -> std::string {
      if ((i.category == MaterialPropertyInput::TEMPERATURE) ||
          (i.category ==
           MaterialPropertyInput::AUXILIARYSTATEVARIABLEFROMEXTERNALMODEL) ||
          (i.category == MaterialPropertyInput::EXTERNALSTATEVARIABLE)) {
        return "this->" + i.name + "+(this->theta)*(this->d" + i.name + ')';
      } else if ((i.category == MaterialPropertyInput::MATERIALPROPERTY) ||
                 (i.category == MaterialPropertyInput::STATEVARIABLE) ||
                 (i.category == MaterialPropertyInput::PARAMETER)) {
        return "this->" + i.name;
      } else if (i.category == MaterialPropertyInput::STATICVARIABLE) {
        return this->mb.getClassName() + "::" + i.name;
      } else {
        this->throwRuntimeError(
            "ImplicitDSLBase::writeBehaviourLocalVariablesInitialisation",
            "unsupported input type for variable '" + i.name + "'");
      }
    };
    Modifier ets = [this](const MaterialPropertyInput& i) -> std::string {
      if ((i.category == MaterialPropertyInput::TEMPERATURE) ||
          (i.category ==
           MaterialPropertyInput::AUXILIARYSTATEVARIABLEFROMEXTERNALMODEL) ||
          (i.category == MaterialPropertyInput::EXTERNALSTATEVARIABLE)) {
        return "this->" + i.name + "+this->d" + i.name;
      } else if ((i.category == MaterialPropertyInput::MATERIALPROPERTY) ||
                 (i.category == MaterialPropertyInput::STATEVARIABLE) ||
                 (i.category == MaterialPropertyInput::PARAMETER)) {
        return "this->" + i.name;
      } else if (i.category == MaterialPropertyInput::STATICVARIABLE) {
        return this->mb.getClassName() + "::" + i.name;
      } else {
        this->throwRuntimeError(
            "ImplicitDSLBase::writeBehaviourLocalVariablesInitialisation",
            "unsupported input type for variable '" + i.name + "'");
      }
    };
    if (this->mb.getAttribute(BehaviourDescription::computesStiffnessTensor,
                              false)) {
      os << "// updating the stiffness tensor at the middle of the time "
            "step\n";
      this->writeStiffnessTensorComputation(os, "this->D", mts);
      if (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep()) {
        os << "// stiffness tensor at the end of the time step\n";
        this->writeStiffnessTensorComputation(os, "this->D_tdt", ets);
      }
    }
    for (const auto& ht : this->mb.getHillTensors()) {
      if ((this->mb.getBehaviourType() !=
           BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) &&
          (this->mb.getBehaviourType() !=
           BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR)) {
        this->throwRuntimeError(
            "ImplicitDSLBase::writeBehaviourLocalVariablesInitialisation",
            "Hill tensors shall only be defined for finite strain "
            "or small strain behaviours");
      }
      this->writeHillTensorComputation(os, "this->" + ht.name, ht, mts);
      if (!this->mb.areMaterialPropertiesConstantDuringTheTimeStep(ht.c)) {
        this->writeHillTensorComputation(os, "this->" + ht.name + "_tdt", ht,
                                         ets);
      }
    }
    if ((!this->mb.getAttribute(BehaviourDescription::computesStiffnessTensor,
                                false)) &&
        (this->mb.getElasticSymmetryType() == ISOTROPIC) &&
        (this->mb.areElasticMaterialPropertiesDefined())) {
      const auto& emps = this->mb.getElasticMaterialProperties();
      if (emps.size() != 2u) {
        this->throwRuntimeError(
            "ImplicitDSLBase::writeBehaviourLocalVariablesInitialisation",
            "invalid number of material properties");
      }
      if (!emps[0].is<BehaviourDescription::ConstantMaterialProperty>()) {
        this->writeMaterialPropertyCheckBoundsEvaluation(os, emps[0], mts);
      }
      if (!emps[1].is<BehaviourDescription::ConstantMaterialProperty>()) {
        this->writeMaterialPropertyCheckBoundsEvaluation(os, emps[1], mts);
      }
      if (!emps[0].is<BehaviourDescription::ConstantMaterialProperty>()) {
        os << "this->young = stress(";
        this->writeMaterialPropertyEvaluation(os, emps[0], mts);
        os << ");\n";
      }
      if (!emps[1].is<BehaviourDescription::ConstantMaterialProperty>()) {
        os << "this->nu = ";
        this->writeMaterialPropertyEvaluation(os, emps[1], mts);
        os << ";\n";
      }
      os << "this->lambda = computeLambda(young,nu);\n";
      os << "this->mu = computeMu(young,nu);\n";
      if (!this->mb.isMaterialPropertyConstantDuringTheTimeStep(emps[0])) {
        this->writeMaterialPropertyCheckBoundsEvaluation(os, emps[0], ets);
        os << "this->young_tdt = stress(";
        this->writeMaterialPropertyEvaluation(os, emps[0], ets);
        os << ");\n";
      } else {
        os << "this->young_tdt  = this->young;\n";
      }
      if (!this->mb.isMaterialPropertyConstantDuringTheTimeStep(emps[1])) {
        this->writeMaterialPropertyCheckBoundsEvaluation(os, emps[1], ets);
        os << "this->nu_tdt = ";
        this->writeMaterialPropertyEvaluation(os, emps[1], ets);
        os << ";\n";
      } else {
        os << "this->nu_tdt     = this->nu;\n";
      }
      if (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep()) {
        os << "this->lambda_tdt = computeLambda(young_tdt,nu_tdt);\n";
        os << "this->mu_tdt     = computeMu(young_tdt,nu_tdt);\n";
      } else {
        os << "this->lambda_tdt = this->lambda;\n";
        os << "this->mu_tdt     = this->mu;\n";
      }
    }
    BehaviourDSLCommon::writeBehaviourLocalVariablesInitialisation(os, h);
  }  // end of writeBehaviourLocalVariablesInitialisation

  void ImplicitDSLBase::writeBehaviourParserSpecificIncludes(
      std::ostream& os) const {
    this->checkBehaviourFile(os);
    os << "#include\"TFEL/Math/Array/ViewsArray.hxx\"\n"
       << "#include\"TFEL/Math/tvector.hxx\"\n"
       << "#include\"TFEL/Math/Vector/tvectorIO.hxx\"\n"
       << "#include\"TFEL/Math/tmatrix.hxx\"\n"
       << "#include\"TFEL/Math/Matrix/tmatrixIO.hxx\"\n"
       << "#include\"TFEL/Math/st2tost2.hxx\"\n"
       << "#include\"TFEL/Math/ST2toST2/ST2toST2ConceptIO.hxx\"\n";
    for (const auto& h : this->solver->getSpecificHeaders()) {
      os << "#include\"" << h << "\"\n";
    }
  }  // end of writeBehaviourParserSpecificIncludes()

  void ImplicitDSLBase::writeBehaviourParserSpecificTypedefs(
      std::ostream& os) const {
    this->checkBehaviourFile(os);
    BehaviourDSLCommon::writeBehaviourParserSpecificTypedefs(os);
  }  // end of writeBehaviourParserSpecificTypedefs()

  void ImplicitDSLBase::writeBehaviourParserSpecificMembers(
      std::ostream& os, const Hypothesis h) const {
    using Modifier = std::function<std::string(const MaterialPropertyInput&)>;
    Modifier mts = [this](const MaterialPropertyInput& i) -> std::string {
      if ((i.category == MaterialPropertyInput::TEMPERATURE) ||
          (i.category ==
           MaterialPropertyInput::AUXILIARYSTATEVARIABLEFROMEXTERNALMODEL) ||
          (i.category == MaterialPropertyInput::STATEVARIABLE) ||
          (i.category == MaterialPropertyInput::EXTERNALSTATEVARIABLE)) {
        return "this->" + i.name + " + (this->theta) * (this->d" + i.name + ')';
      } else if ((i.category == MaterialPropertyInput::MATERIALPROPERTY) ||
                 (i.category == MaterialPropertyInput::PARAMETER)) {
        return "this->" + i.name;
      } else if (i.category == MaterialPropertyInput::STATICVARIABLE) {
        return this->mb.getClassName() + "::" + i.name;
      } else {
        this->throwRuntimeError(
            "ImplicitDSLBase::writeBehaviourLocalVariablesInitialisation",
            "unsupported input type for variable '" + i.name + "'");
      }
    };
    Modifier ets = [this](const MaterialPropertyInput& i) -> std::string {
      if ((i.category == MaterialPropertyInput::TEMPERATURE) ||
          (i.category ==
           MaterialPropertyInput::AUXILIARYSTATEVARIABLEFROMEXTERNALMODEL) ||
          (i.category == MaterialPropertyInput::STATEVARIABLE) ||
          (i.category == MaterialPropertyInput::EXTERNALSTATEVARIABLE)) {
        return "this->" + i.name + "+this->d" + i.name;
      } else if ((i.category == MaterialPropertyInput::MATERIALPROPERTY) ||
                 (i.category == MaterialPropertyInput::PARAMETER)) {
        return "this->" + i.name;
      } else if (i.category == MaterialPropertyInput::STATICVARIABLE) {
        return this->mb.getClassName() + "::" + i.name;
      } else {
        this->throwRuntimeError(
            "ImplicitDSLBase::writeBehaviourLocalVariablesInitialisation",
            "unsupported input type for variable '" + i.name + "'");
      }
    };
    //
    BehaviourDSLCommon::writeBehaviourParserSpecificMembers(os, h);
    // updating material properties
    os << "// updating material properties, in mandatory\n"
       << "void updateMaterialPropertiesDependantOnStateVariables(){\n"
       << "using namespace std;\n"
       << "using namespace tfel::math;\n"
       << "using std::vector;\n";
    writeMaterialLaws(os, this->mb.getMaterialLaws());
    if (this->mb.getAttribute(BehaviourDescription::computesStiffnessTensor,
                              false)) {
      if (this->mb.areElasticMaterialPropertiesDependantOnStateVariables()) {
        os << "// updating the stiffness tensor at the middle of the time "
              "step\n";
        this->writeStiffnessTensorComputation(os, "this->D", mts);
        os << "// stiffness tensor at the end of the time step\n";
        this->writeStiffnessTensorComputation(os, "this->D_tdt", ets);
      }
    }
    for (const auto& ht : this->mb.getHillTensors()) {
      if ((this->mb.getBehaviourType() !=
           BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) &&
          (this->mb.getBehaviourType() !=
           BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR)) {
        this->throwRuntimeError(
            "ImplicitDSLBase::writeBehaviourLocalVariablesInitialisation",
            "Hill tensors shall only be defined for finite strain "
            "or small strain behaviours");
      }
      if (this->mb.areMaterialPropertiesDependantOnStateVariables(ht.c)) {
        this->writeHillTensorComputation(os, "this->" + ht.name, ht, mts);
        this->writeHillTensorComputation(os, "this->" + ht.name + "_tdt", ht,
                                         ets);
      }
    }
    if ((!this->mb.getAttribute(BehaviourDescription::computesStiffnessTensor,
                                false)) &&
        (this->mb.getElasticSymmetryType() == ISOTROPIC) &&
        (this->mb.areElasticMaterialPropertiesDefined())) {
      const auto& emps = this->mb.getElasticMaterialProperties();
      if (emps.size() != 2u) {
        this->throwRuntimeError(
            "ImplicitDSLBase::writeBehaviourLocalVariablesInitialisation",
            "invalid number of material properties");
      }
      if (this->mb.isMaterialPropertyDependantOnStateVariables(emps[0])) {
        this->writeMaterialPropertyCheckBoundsEvaluation(os, emps[0], mts);
        this->writeMaterialPropertyCheckBoundsEvaluation(os, emps[0], ets);
        os << "this->young = stress(";
        this->writeMaterialPropertyEvaluation(os, emps[0], mts);
        os << ");\n";
        os << "this->young_tdt = stress(";
        this->writeMaterialPropertyEvaluation(os, emps[0], ets);
        os << ");\n";
      }
      if (this->mb.isMaterialPropertyDependantOnStateVariables(emps[1])) {
        this->writeMaterialPropertyCheckBoundsEvaluation(os, emps[1], mts);
        this->writeMaterialPropertyCheckBoundsEvaluation(os, emps[1], ets);
        os << "this->nu = ";
        this->writeMaterialPropertyEvaluation(os, emps[1], mts);
        os << ";\n";
        os << "this->nu_tdt = ";
        this->writeMaterialPropertyEvaluation(os, emps[1], ets);
        os << ";\n";
      }
      if ((this->mb.isMaterialPropertyDependantOnStateVariables(emps[0])) ||
          (this->mb.isMaterialPropertyDependantOnStateVariables(emps[1]))) {
        os << "this->lambda=computeLambda(young,nu);\n"
           << "this->mu=computeMu(young,nu);\n"
           << "this->lambda_tdt = computeLambda(young_tdt,nu_tdt);\n"
           << "this->mu_tdt     = computeMu(young_tdt,nu_tdt);\n";
      }
    }
    os << "}\n\n";
    //
    const auto& d = this->mb.getBehaviourData(h);
    SupportedTypes::TypeSize n;
    const auto n3 = d.getIntegrationVariables().getTypeSize();
    if ((this->solver->usesJacobian()) &&
        (!this->solver->requiresNumericalJacobian())) {
      for (const auto& v : d.getIntegrationVariables()) {
        SupportedTypes::TypeSize n2;
        for (const auto& v2 : d.getIntegrationVariables()) {
          if ((v.arraySize == 1u) && (v2.arraySize == 1u)) {
            n2 += this->getTypeSize(v2.type, v2.arraySize);
            continue;
          }
          os << "/*!\n"
             << " * \\return the part of the jacobian matrix "
             << "corresponding to the derivative "
             << "of variable " << v.name << " by variable " << v2.name << "\n"
             << " */\n";
          if ((v.arraySize != 1u) && (v2.arraySize == 1u)) {
            os << "tfel::math::derivative_view_from_tiny_matrix<" << n3 << ", "
               << v.type << "," << v2.type << "> "
               << "df" << v.name << "_dd" << v2.name << "(tfel::math::tmatrix<"
               << n3 << "," << n3 << ", NumericType>& tjacobian,"
               << "const ushort mfront_idx){\n"
               << "return tfel::math::map_derivative<" << v.type << ","
               << v2.type << ">(tjacobian, " << n << " + mfront_idx * ("
               << this->getTypeSize(v.type, 1u) << "), " << n2 << ");\n"
               << "}\n\n";
            os << "tfel::math::derivative_view_from_tiny_matrix<" << n3 << ", "
               << v.type << "," << v2.type << "> "
               << "df" << v.name << "_dd" << v2.name
               << "(const ushort mfront_idx){\n"
               << "return tfel::math::map_derivative<" << v.type << ","
               << v2.type << ">(this->jacobian, " << n << " + mfront_idx * ("
               << this->getTypeSize(v.type, 1u) << "), " << n2 << ");\n"
               << "}\n\n";
          } else if ((v.arraySize == 1u) && (v2.arraySize != 1u)) {
            os << "tfel::math::derivative_view_from_tiny_matrix<" << n3 << ", "
               << v.type << "," << v2.type << "> "
               << "df" << v.name << "_dd" << v2.name << "(tfel::math::tmatrix<"
               << n3 << "," << n3 << ", NumericType>& tjacobian,"
               << "const ushort mfront_idx){\n"
               << "return tfel::math::map_derivative<" << v.type << ","
               << v2.type << ">(tjacobian, " << n << ", " << n2
               << " + mfront_idx * (" << this->getTypeSize(v2.type, 1u)
               << "));\n"
               << "}\n\n";
            os << "tfel::math::derivative_view_from_tiny_matrix<" << n3 << ", "
               << v.type << "," << v2.type << "> "
               << "df" << v.name << "_dd" << v2.name
               << "(const ushort mfront_idx){\n"
               << "return tfel::math::map_derivative<" << v.type << ","
               << v2.type << ">(this->jacobian, " << n << ", " << n2
               << " + mfront_idx * (" << this->getTypeSize(v2.type, 1u)
               << "));\n"
               << "}\n\n";
          } else {
            os << "tfel::math::derivative_view_from_tiny_matrix<" << n3 << ", "
               << v.type << "," << v2.type << "> "
               << "df" << v.name << "_dd" << v2.name << "(tfel::math::tmatrix<"
               << n3 << "," << n3 << ", NumericType>& tjacobian,"
               << "const ushort mfront_idx, "
               << "const ushort mfront_idx2){\n"
               << "return tfel::math::map_derivative<" << v.type << ","
               << v2.type << ">(tjacobian, " << n << " + mfront_idx * ("
               << this->getTypeSize(v.type, 1u) << "), " << n2
               << " + mfront_idx2 * (" << this->getTypeSize(v2.type, 1u)
               << "));\n"
               << "}\n\n";
            os << "tfel::math::derivative_view_from_tiny_matrix<" << n3 << ", "
               << v.type << "," << v2.type << "> "
               << "df" << v.name << "_dd" << v2.name
               << "(const ushort mfront_idx, "
               << "const ushort mfront_idx2){\n"
               << "return tfel::math::map_derivative<" << v.type << ","
               << v2.type << ">(this->jacobian, " << n << " + mfront_idx * ("
               << this->getTypeSize(v.type, 1u) << "), " << n2
               << " + mfront_idx2 * (" << this->getTypeSize(v2.type, 1u)
               << "));\n"
               << "}\n\n";
          }
          n2 += this->getTypeSize(v2.type, v2.arraySize);
        }
        n += this->getTypeSize(v.type, v.arraySize);
      }
    }
    // size of linear system
    n = n3;
    this->solver->writeSpecificMembers(os, this->mb, h);
    os << "SMType stiffness_matrix_type;\n";
    //
    if (this->solver->usesJacobian()) {
      // compute the numerical part of the jacobian.  This method is
      // used to compute a numerical approximation of the jacobian for
      // the solvers based on it. For solvers providing an analytical
      // jacobian, or an approximation of it, this method is used to
      // compare the jacobain to its numerical approximation
      this->writeComputeNumericalJacobian(os, h);
    }
    if (this->solver->usesJacobian()) {
      // for solver based on the jacobian matrix, the get partial
      // jacobian invert method can be used to compute the tangent
      // operator.
      this->writeComputePartialJacobianInvert(os, h);
    }
    // additional convergence checks
    if (this->mb.hasCode(h, BehaviourData::AdditionalConvergenceChecks)) {
      os << "void additionalConvergenceChecks(bool& converged, "
         << "const NumericType& error){\n"
         << "using namespace std;\n"
         << "using namespace tfel::math;\n"
         << "using std::vector;\n"
         << "const auto smt = this->stiffness_matrix_type;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
      os << this->mb.getCode(h, BehaviourData::AdditionalConvergenceChecks)
         << '\n'
         << "static_cast<void>(converged);\n"
         << "static_cast<void>(error);\n"
         << "static_cast<void>(smt);\n"
         << "} // end of additionalConvergenceChecks\n\n";
    }
    // compute stress
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "void computeThermodynamicForces(){\n"
         << "using namespace std;\n"
         << "using namespace tfel::math;\n"
         << "using std::vector;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
      os << this->mb.getCode(h, BehaviourData::ComputeThermodynamicForces)
         << "\n} // end of " << this->mb.getClassName()
         << "::computeThermodynamicForces\n\n";
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeFinalThermodynamicForces)) {
      os << "void computeFinalThermodynamicForces(){\n"
         << "using namespace std;\n"
         << "using namespace tfel::math;\n"
         << "using std::vector;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
      os << this->mb.getCode(h, BehaviourData::ComputeFinalThermodynamicForces)
         << "\n} // end of " << this->mb.getClassName()
         << "::computeFinalThermodynamicForces\n\n";
    }
  }  // end of writeBehaviourParserSpecificMembers

  static std::string getDerivativeTypeHolder(const VariableDescription& v1,
                                             const VariableDescription& v2) {
    if (v2.arraySize != 1u) {
      tfel::raise("getDerivativeType: unsupported case");
    }
    const auto dtype =
        "tfel::math::derivative_type<" + v1.type + ", " + v2.type + ">";
    if (v1.arraySize == 1u) {
      return dtype;
    }
    return "tfel::math::tvector<" + std::to_string(v1.arraySize) + ", " +
           dtype + ">";
  }  // end of getVariableDerivativeTypeHolder

  void ImplicitDSLBase::writeBehaviourComputeTangentOperatorBody(
      std::ostream& os, const Hypothesis h, const std::string& n) const {
    using Derivative = std::pair<VariableDescription, VariableDescription>;
    const auto& d = this->mb.getBehaviourData(h);
    const auto& isvs = d.getIntegrationVariables();
    const auto nivs = mfront::getTypeSize(isvs);
    const auto& cb = this->mb.getCodeBlock(h, n);
    if (getAttribute(cb, CodeBlock::requires_jacobian_decomposition, false)) {
      os << "TinyPermutation<" << nivs << "> jacobian_permutation;\n"
         << "if(!TinyMatrixSolve<" << nivs << ", NumericType, false>"
         << "::decomp(this->jacobian,jacobian_permutation)){\n"
         << "return false;\n"
         << "}\n";
    }
    if (hasAttribute<std::vector<Derivative>>(
            cb, CodeBlock::used_jacobian_invert_blocks)) {
      const auto& invert_jacobian_blocks =
          getAttribute<std::vector<Derivative>>(
              cb, CodeBlock::used_jacobian_invert_blocks);
      os << "auto jacobian_invert = "
         << "tfel::math::tmatrix<" << nivs << ", " << nivs
         << ", NumericType>{};\n"
         << "std::fill(jacobian_invert.begin(), jacobian_invert.end(), "
            "NumericType(0));\n"
         << "for(typename tfel::math::tmatrix<" << nivs << ", " << nivs
         << ", NumericType>::size_type mfront_idx=0; mfront_idx != " << nivs
         << "; ++mfront_idx){\n"
         << "jacobian_invert(mfront_idx, mfront_idx) = NumericType(1);\n"
         << "}\n"
         << "if(!TinyMatrixSolve<" << nivs
         << ", NumericType, false>::back_substitute("
         << "this->jacobian, jacobian_permutation, jacobian_invert)){\n"
         << "return false;\n"
         << "}\n";
      auto cr = SupportedTypes::TypeSize{};  // current row
      auto cc = SupportedTypes::TypeSize{};  // current column
      for (const auto& iv1 : isvs) {
        for (const auto& iv2 : isvs) {
          if (std::find_if(invert_jacobian_blocks.begin(),
                           invert_jacobian_blocks.end(),
                           [&iv1, &iv2](const Derivative& derivative) {
                             return (derivative.first.name == iv1.name) &&
                                    (derivative.second.name == iv2.name);
                           }) == invert_jacobian_blocks.end()) {
            continue;
          }
          auto derivative_view = DerivativeViewDescription{};
          derivative_view.derivative_name = "iJ_" + iv1.name + "_" + iv2.name;
          derivative_view.matrix_name = "jacobian_invert";
          derivative_view.first_variable = iv1;
          derivative_view.second_variable = iv2;
          derivative_view.matrix_number_of_rows = nivs;
          derivative_view.matrix_number_of_columns = nivs;
          derivative_view.derivative_row_position = cr;
          derivative_view.derivative_column_position = cc;
          this->writeDerivativeView(os, derivative_view);
          // update the row position
          cc += mfront::getTypeSize(iv2);
        }
        cr += mfront::getTypeSize(iv1);
      }
    }
    // treating getPartialJacobianInvert
    if (getAttribute(cb, CodeBlock::uses_get_partial_jacobian_invert, false)) {
      if (!getAttribute(cb, CodeBlock::requires_jacobian_decomposition,
                        false)) {
        tfel::raise(
            "writeBehaviourComputeTangentOperatorBody: "
            "inconsistent attribute declarations for code block '" +
            n +
            "'. If the attribute `uses_get_partial_jacobian_invert` "
            "is set to true, then the attribute "
            "`requires_jacobian_decomposition` must also be set to true");
      }
      const auto v2 = VariableDescription{"StrainStensor", "\u03B5\u1D57\u1D52",
                                          "eto", 1u, 0u};
      os << "struct TFEL_VISIBILITY_LOCAL GetPartialJacobianInvert{\n"
         << "GetPartialJacobianInvert(" << this->mb.getClassName()
         << "& mfront_behaviour_argument,\n"
         << "const tfel::math::TinyPermutation<" << nivs
         << ">& mfront_permutation_argument,\n"
         << "bool& mfront_success_argument)\n"
         << ": mfront_get_partial_jacobian_invert_behaviour("
         << "mfront_behaviour_argument),\n"
         << "mfront_get_partial_jacobian_invert_permutation("
         << "mfront_permutation_argument),\n"
         << "mfront_success_reference(mfront_success_argument)"
         << "{}\n";
      for (size_type i = 0; i != isvs.size(); ++i) {
        os << "void operator()(";
        for (size_type i2 = 0; i2 <= i;) {
          const auto& v = isvs[i2];
          os << getDerivativeTypeHolder(v, v2) << "& ";
          os << "partial_jacobian_" << v.name;
          if (++i2 <= i) {
            os << ",\n";
          }
        }
        os << "){\n"
           << "if(!this->mfront_get_partial_jacobian_invert_behaviour."
           << "computePartialJacobianInvert("
           << "this->mfront_get_partial_jacobian_invert_permutation, ";
        for (size_type i2 = 0; i2 <= i;) {
          const auto& v = isvs[i2];
          os << "partial_jacobian_" << v.name;
          if (++i2 <= i) {
            os << ",\n";
          }
        }
        os << ")){\n"
           << "this->mfront_success_reference = false;"
           << "};\n"
           << "}\n";
      }
      os << "private:\n"
         << this->mb.getClassName() << "& "
         << "mfront_get_partial_jacobian_invert_behaviour;\n"
         << "const tfel::math::TinyPermutation<" << nivs << ">& "
         << "mfront_get_partial_jacobian_invert_permutation;\n"
         << "bool& mfront_success_reference;\n"
         << "}; // end of struct GetPartialJacobianInvert\n"
         << "GetPartialJacobianInvert "
            "getPartialJacobianInvert(*this, jacobian_permutation, "
            "mfront_success);\n";
    }
    const auto attr = CodeBlock::
        used_implicit_equations_derivatives_with_respect_to_gradients_or_external_state_variables;
    if (hasAttribute<std::vector<Derivative>>(cb, attr)) {
      std::vector<
          std::pair<VariableDescription, std::vector<VariableDescription>>>
          implicit_equations_derivatives;
      for (const auto& derivative :
           getAttribute<std::vector<Derivative>>(cb, attr)) {
        const auto p = std::find_if(
            implicit_equations_derivatives.begin(),
            implicit_equations_derivatives.end(),
            [&derivative](
                const std::pair<VariableDescription,
                                std::vector<VariableDescription>>& d2) {
              return derivative.second.name == d2.first.name;
            });
        if (p == implicit_equations_derivatives.end()) {
          implicit_equations_derivatives.push_back(
              {derivative.second,
               std::vector<VariableDescription>{1u, derivative.first}});
        } else {
          p->second.push_back(derivative.first);
        }
      }  //
      for (const auto& derivatives : implicit_equations_derivatives) {
        auto nc = mfront::getTypeSize(derivatives.first);
        const auto rhs_type = [&nivs, &nc] {
          std::ostringstream rhs_os;
          rhs_os << "tfel::math::tmatrix<" << nivs << ", " << nc
                 << ", NumericType>";
          return rhs_os.str();
        }();
        const auto m = "dfzeros_dd" + derivatives.first.name;
        os << rhs_type << m << ";\n"
           << "std::fill(" << m << ".begin(), " << m
           << ".end(), NumericType(0));\n";
        // creating the view for the derivatives
        auto cr = SupportedTypes::TypeSize{};  // current row
        for (const auto& iv : isvs) {
          if (std::find_if(derivatives.second.begin(), derivatives.second.end(),
                           [&iv](const VariableDescription& iv2) {
                             return iv2.name == iv.name;
                           }) == derivatives.second.end()) {
            cr += mfront::getTypeSize(iv);
            continue;
          }
          auto derivative_view = DerivativeViewDescription{};
          derivative_view.derivative_name =
              "df" + iv.name + "_dd" + derivatives.first.name;
          derivative_view.matrix_name = m;
          derivative_view.first_variable = iv;
          derivative_view.second_variable = derivatives.first;
          derivative_view.matrix_number_of_rows = nivs;
          derivative_view.matrix_number_of_columns = nc;
          derivative_view.derivative_row_position = cr;
          derivative_view.derivative_column_position =
              SupportedTypes::TypeSize{};
          this->writeDerivativeView(os, derivative_view);
          // update the row position
          cr += mfront::getTypeSize(iv);
        }
      }
    }
    // treating getIntegrationDerivatives
    if (hasAttribute<std::vector<VariableDescription>>(
            cb, CodeBlock::used_get_integration_variables_derivatives)) {
      const auto& get_integration_variables_derivatives =
          getAttribute<std::vector<VariableDescription>>(
              cb, CodeBlock::used_get_integration_variables_derivatives);
      if (!hasAttribute<std::vector<Derivative>>(cb, attr)) {
        this->throwRuntimeError(
            "ImplicitDSLBase::writeBehaviourComputeTangentOperatorBody",
            "internal error, no implicit equation derivative with respect "
            "to "
            "a gradient or and external state variable detected");
      }
      const auto& implicit_equations_derivatives =
          getAttribute<std::vector<Derivative>>(cb, attr);
      for (const auto& givd : get_integration_variables_derivatives) {
        std::vector<Derivative> derivatives;
        std::copy_if(implicit_equations_derivatives.begin(),
                     implicit_equations_derivatives.end(),
                     std::back_inserter(derivatives),
                     [&givd](const Derivative& v) {
                       return givd.name == v.second.name;
                     });
        if (derivatives.empty()) {
          this->throwRuntimeError(
              "ImplicitDSLBase::writeBehaviourComputeTangentOperatorBody",
              "internal error, no implicit equation derivative with "
              "respect to "
              "'" +
                  displayName(givd) + "' defined");
        }
        auto nc = mfront::getTypeSize(givd);
        const auto rhs_type = [&nivs, &nc] {
          std::ostringstream rhs_os;
          rhs_os << "tfel::math::tmatrix<" << nivs << ", " << nc
                 << ", NumericType>";
          return rhs_os.str();
        }();
        const auto m = "dfzeros_dd" + givd.name;
        os << "struct TFEL_VISIBILITY_LOCAL GetIntegrationVariablesDerivatives_"
           << givd.name << "{\n"
           << "GetIntegrationVariablesDerivatives_" << givd.name << "("
           << this->mb.getClassName() << "& b,\n"
           << "const tfel::math::TinyPermutation<" << nivs << ">& p,\n"
           << rhs_type << "& v,\n"
           << "bool& mfront_success_argument)\n"
           << ": behaviour(b),\n"
           << "permutation(p)\n,"
           << "rhs(v),\n"
           << "mfront_success_reference(mfront_success_argument)\n"
           << "{}\n";
        for (size_type i = 0; i != isvs.size(); ++i) {
          os << "void operator()(";
          for (size_type i2 = 0; i2 <= i;) {
            const auto& v = isvs[i2];
            os << getDerivativeTypeHolder(v, givd) << "& "
               << "integration_variable_derivative_d" << v.name << "_dd"
               << givd.name;
            if (++i2 <= i) {
              os << ",\n";
            }
          }
          os << "){\n"
             << rhs_type << " lhs(-(this->rhs));\n"
             << "if(!tfel::math::TinyMatrixSolve<" << nivs
             << ", NumericType, false>"
             << "::back_substitute(this->behaviour.jacobian, "
             << "this->permutation, lhs)){\n"
             << "this->mfront_success_reference=false;\n"
             << "}\n";
          auto cr = SupportedTypes::TypeSize{};  // current row
          for (size_type i2 = 0; i2 <= i; ++i2) {
            const auto& v = isvs[i2];
            auto derivative_view = DerivativeViewDescription{};
            derivative_view.derivative_name =
                "integration_variable_derivative_d" + v.name + "_dd" +
                givd.name + "_view";
            derivative_view.matrix_name = "lhs";
            derivative_view.first_variable = v;
            derivative_view.second_variable = givd;
            derivative_view.matrix_number_of_rows = nivs;
            derivative_view.matrix_number_of_columns = nc;
            derivative_view.derivative_row_position = cr;
            derivative_view.derivative_column_position =
                SupportedTypes::TypeSize{};
            this->writeDerivativeView(os, derivative_view);
            // update the row position
            cr += mfront::getTypeSize(v);
            // assign the view to the ouptut derivatives
            if (v.arraySize == 1u) {
              os << "integration_variable_derivative_d" << v.name << "_dd"
                 << givd.name << " = integration_variable_derivative_d"
                 << v.name << "_dd" << givd.name << "_view;\n";
            } else {
              os << "for(typename " << rhs_type
                 << "::size_type mfront_idx; mfront_idx !=" << v.arraySize
                 << "; ++mfront_idx){\n"
                 << "integration_variable_derivative_d" << v.name << "_dd"
                 << givd.name
                 << "(mfront_idx) = integration_variable_derivative_d" << v.name
                 << "_dd" << givd.name << "_view(mfront_idx);"
                 << "}\n";
            }
          }
          os << "}\n";
        }
        os << "private:\n"
           << this->mb.getClassName() << "& behaviour;\n"
           << "const tfel::math::TinyPermutation<" << nivs
           << ">& permutation;\n"
           << rhs_type << "& rhs;\n"
           << "bool& mfront_success_reference;\n"
           << "};\n"
           << "GetIntegrationVariablesDerivatives_" << givd.name << " "
           << "getIntegrationVariablesDerivatives_" << givd.name
           << "(*this, jacobian_permutation," << m << ", mfront_success);\n";
      }
    }
    //
    BehaviourDSLCommon::writeBehaviourComputeTangentOperatorBody(os, h, n);
  }  // end of writeBehaviourComputeTangentOperatorBody

  void ImplicitDSLBase::writeComputePartialJacobianInvert(
      std::ostream& os, const Hypothesis h) const {
    this->checkBehaviourFile(os);
    const auto& d = this->mb.getBehaviourData(h);
    const auto& isvs = d.getIntegrationVariables();
    const auto n = mfront::getTypeSize(isvs);
    const auto v2 = VariableDescription{"StrainStensor", "\u03B5\u1D57\u1D52",
                                        "eto", 1u, 0u};
    for (size_type i = 0; i != isvs.size(); ++i) {
      os << "[[nodiscard]] bool\ncomputePartialJacobianInvert("
            "const tfel::math::TinyPermutation<"
         << n << ">& jacobian_permutation, ";
      for (size_type i2 = 0; i2 <= i;) {
        const auto& v = isvs[i2];
        os << getDerivativeTypeHolder(v, v2) << "& ";
        os << "partial_jacobian_" << v.name;
        if (++i2 <= i) {
          os << ",\n";
        }
      }
      os << ")\n"
         << "{\n"
         << "using namespace tfel::math;\n"
         << "for(ushort mfront_idx=0; mfront_idx !=StensorSize; "
            "++mfront_idx){\n"
         << "tvector<" << n << ", NumericType> vect_e(NumericType(0));\n"
         << "vect_e(mfront_idx) = NumericType(1);\n"
         << "if(!TinyMatrixSolve<" << n << ", NumericType, false>"
         << "::back_substitute(this->jacobian, "
         << "jacobian_permutation, vect_e)){\n"
         << "return false;\n"
         << "}\n";
      SupportedTypes::TypeSize n2;
      for (size_type i2 = 0; i2 <= i; ++i2) {
        const auto& v = d.getIntegrationVariables()[i2];
        const auto flag = SupportedTypes::getTypeFlag(v.type);
        if (flag == SupportedTypes::SCALAR) {
          if (v.arraySize == 1u) {
            os << "partial_jacobian_" << v.name << "(mfront_idx)=vect_e(" << n2
               << ");\n";
          } else {
            os << "for(ushort mfront_idx2=0; mfront_idx2!=" << v.arraySize
               << "; ++mfront_idx2){\n"
               << "partial_jacobian_" << v.name
               << "(mfront_idx2)(mfront_idx)=vect_e(" << n2
               << "+mfront_idx2);\n"
               << "}\n";
          }
          n2 += this->getTypeSize(v.type, v.arraySize);
        } else if ((flag == SupportedTypes::TVECTOR) ||
                   (flag == SupportedTypes::STENSOR) ||
                   (flag == SupportedTypes::TENSOR)) {
          if (v.arraySize == 1u) {
            os << "for(ushort mfront_idx2=" << n2;
            os << "; mfront_idx2!=";
            n2 += this->getTypeSize(v.type, v.arraySize);
            os << n2 << "; ++mfront_idx2){\n";
            os << "partial_jacobian_" << v.name
               << "(mfront_idx2,mfront_idx)=vect_e(mfront_idx2);\n";
            os << "}\n";
          } else {
            const auto size = this->getTypeSize(v.type, 1u);
            os << "for(ushort mfront_idx2=0; mfront_idx2!=" << v.arraySize
               << "; ++mfront_idx2){\n"
               << "for(ushort mfront_idx3=0; mfront_idx3!= " << size
               << "; ++mfront_idx3){\n"
               << "partial_jacobian_" << v.name
               << "(mfront_idx2)(mfront_idx3,mfront_idx) = "
               << "vect_e(" << n2 << "+ mfront_idx3 + mfront_idx2 * " << size
               << ");\n"
               << "}\n"
               << "}\n";
            n2 += this->getTypeSize(v.type, v.arraySize);
          }
        } else {
          this->throwRuntimeError(
              "ImplicitDSLBase::writeComputePartialJacobianInvert",
              "internal error, tag unsupported");
        }
      }
      os << "}\n";
      for (size_type i2 = 0; i2 <= i; ++i2) {
        const auto& v = d.getIntegrationVariables()[i2];
        if (this->mb.hasAttribute(h, v.name + "_normalisation_factor")) {
          const auto& nf = this->mb.getAttribute<std::string>(
              h, v.name + "_normalisation_factor");
          os << "partial_jacobian_" << v.name << " /= " << nf << ";\n";
        }
      }
      os << "return true;\n"
         << "}\n\n";
    }
  }  // end of writeComputePartialJacobianInvert

  void ImplicitDSLBase::writeComputeNumericalJacobian(
      std::ostream& os, const Hypothesis h) const {
    const auto& d = this->mb.getBehaviourData(h);
    const auto n = d.getIntegrationVariables().getTypeSize();
    this->checkBehaviourFile(os);
    os << "void computeNumericalJacobian("
       << "tfel::math::tmatrix<" << n << "," << n
       << ", NumericType>& njacobian)\n"
       << "{\n"
       << "using namespace std;\n"
       << "using namespace tfel::math;\n"
       << "tvector<" << n << ", NumericType> tzeros(this->zeros);\n"
       << "tvector<" << n << ", NumericType> tfzeros(this->fzeros);\n"
       << "tmatrix<" << n << "," << n
       << ", NumericType> tjacobian(this->jacobian);\n"
       << "for(ushort mfront_idx = 0; mfront_idx != " << n
       << "; ++mfront_idx){\n"
       << "this->zeros(mfront_idx) -= this->numerical_jacobian_epsilon;\n";
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "this->computeThermodynamicForces();\n";
    }
    os << "this->computeFdF(true);\n"
       << "this->zeros = tzeros;\n"
       << "tvector<" << n << ", NumericType> tfzeros2(this->fzeros);\n"
       << "this->zeros(mfront_idx) += this->numerical_jacobian_epsilon;\n";
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "this->computeThermodynamicForces();\n";
    }
    os << "this->computeFdF(true);\n"
       << "this->fzeros = "
          "(this->fzeros-tfzeros2) / (2 * "
          "(this->numerical_jacobian_epsilon));\n"
       << "for(ushort mfront_idx2 = 0; mfront_idx2!= " << n
       << "; ++mfront_idx2){\n"
       << "njacobian(mfront_idx2,mfront_idx) = this->fzeros(mfront_idx2);\n"
       << "}\n"
       << "this->zeros    = tzeros;\n"
       << "this->fzeros   = tfzeros;\n"
       << "}\n"
       << "if(&njacobian!=&(this->jacobian)){\n"
       << "this->jacobian = tjacobian;\n"
       << "}\n"
       << "}\n\n";
  }  // end of writeComputeNumericalJacobian

  void ImplicitDSLBase::writeBehaviourIntegrator(std::ostream& os,
                                                 const Hypothesis h) const {
    const auto btype = this->mb.getBehaviourTypeFlag();
    const auto& d = this->mb.getBehaviourData(h);
    const auto n2 = mfront::getTypeSize(d.getIntegrationVariables());
    this->checkBehaviourFile(os);
    os << "/*!\n"
       << " * \\brief Integrate behaviour law over the time step\n"
       << " */\n"
       << "IntegrationResult ";
    if (this->mb.hasAttribute(h, BehaviourData::hasConsistentTangentOperator)) {
      os << "integrate(const SMFlag smflag,const SMType smt) override{\n";
    } else {
      if ((this->mb.getBehaviourType() ==
           BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) ||
          (this->mb.getBehaviourType() ==
           BehaviourDescription::COHESIVEZONEMODEL)) {
        os << "integrate(const SMFlag smflag,const SMType smt) override{\n";
      } else {
        os << "integrate(const SMFlag,const SMType smt) override{\n";
      }
    }
    os << "using namespace std;\n";
    os << "using namespace tfel::math;\n";
    writeMaterialLaws(os, this->mb.getMaterialLaws());
    os << "this->stiffness_matrix_type = smt;" << '\n';
    if (!this->mb.getMainVariables().empty()) {
      if ((this->mb.getBehaviourType() ==
           BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) ||
          (this->mb.getBehaviourType() ==
           BehaviourDescription::COHESIVEZONEMODEL) ||
          (this->mb.getBehaviourType() ==
           BehaviourDescription::GENERALBEHAVIOUR)) {
        if (this->mb.useQt()) {
          os << "tfel::raise_if(smflag!=MechanicalBehaviour<" << btype
             << ",hypothesis, NumericType, use_qt>::STANDARDTANGENTOPERATOR,\n"
             << "\"invalid tangent operator flag\");\n";
        } else {
          os << "tfel::raise_if(smflag!=MechanicalBehaviour<" << btype
             << ",hypothesis, NumericType, false>::STANDARDTANGENTOPERATOR,\n"
             << "\"invalid tangent operator flag\");\n";
        }
      }
    }
    if (this->mb.hasCode(h, BehaviourData::ComputePredictor)) {
      os << this->mb.getCode(h, BehaviourData::ComputePredictor) << '\n';
    }
    if (this->mb.getAttribute(BehaviourData::profiling, false)) {
      writeStandardPerformanceProfilingBegin(os, mb.getClassName(),
                                             BehaviourData::Integrator);
    }
    this->solver->initializeNumericalParameters(os, this->mb, h);
    os << "if(!this->solveNonLinearSystem()){\n";
    if (this->mb.useQt()) {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType, use_qt>::FAILURE;\n";
    } else {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType, false>::FAILURE;\n";
    }
    os << "}\n";
    // We compute the numerical jacobian even if we converged since
    // most of the time, this tangent operator will be computed
    // using the partial jacobian invert. We consider very unlikely
    // that a user may use a numerical jacobian and provide an
    // analytic definition of the tangent operator
    os << "if (this->stiffness_matrix_type != NOSTIFFNESSREQUESTED){\n"
       << "this->updateOrCheckJacobian();\n"
       << "}\n";
    if (this->mb.getAttribute(BehaviourData::profiling, false)) {
      writeStandardPerformanceProfilingEnd(os);
    }
    for (const auto& v : d.getIntegrationVariables()) {
      if (this->mb.hasAttribute(h, v.name + "_normalisation_factor")) {
        const auto& nf = this->mb.getAttribute<std::string>(
            h, v.name + "_normalisation_factor");
        os << "this->d" << v.name << " *= " << nf << ";\n";
      }
    }
    os << "this->updateIntegrationVariables();\n";
    os << "this->updateStateVariables();\n";
    if (this->mb.hasCode(h, BehaviourData::ComputeFinalThermodynamicForces)) {
      os << "this->computeFinalThermodynamicForces();\n";
    }
    os << "this->updateAuxiliaryStateVariables();\n";
    for (const auto& v : d.getPersistentVariables()) {
      this->writePhysicalBoundsChecks(os, v, false);
    }
    for (const auto& v : d.getPersistentVariables()) {
      this->writeBoundsChecks(os, v, false);
    }
    if (!this->mb.getMainVariables().empty()) {
      os << "if(smt!=NOSTIFFNESSREQUESTED){\n";
      if (this->mb.hasAttribute(h,
                                BehaviourData::hasConsistentTangentOperator)) {
        if (this->mb.getBehaviourType() ==
            BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) {
          os << "if(!this->computeConsistentTangentOperator(smflag,smt)){\n";
        } else {
          os << "if(!this->computeConsistentTangentOperator(smt)){\n";
        }
        if (this->mb.useQt()) {
          os << "return MechanicalBehaviour<" << btype
             << ",hypothesis, NumericType, use_qt>::FAILURE;\n";
        } else {
          os << "return MechanicalBehaviour<" << btype
             << ",hypothesis, NumericType, false>::FAILURE;\n";
        }
        os << "}\n";
      } else {
        os << "string msg(\"" << this->mb.getClassName()
           << "::integrate : \");\n";
        os << "msg +=\"unimplemented feature\";\n";
        os << "throw(runtime_error(msg));\n";
      }
      os << "}\n";
    }
    if (this->mb.useQt()) {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType, use_qt>::SUCCESS;\n";
    } else {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType, false>::SUCCESS;\n";
    }
    os << "} // end of " << this->mb.getClassName() << "::integrate\n\n";
    //
    os << "bool computeResidual(){\n";
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "this->computeThermodynamicForces();\n";
    }
    os << "return this->computeFdF(false);\n"
       << "}\n";
    //
    os << "//! \\return the norm of the residual\n"
       << "NumericType computeResidualNorm() {\n"
       << "return tfel::math::norm(this->fzeros) / (" << n2 << ");\n"
       << "}\n"
       << "/*!\n"
       << " * \\brief check the convergence of the method\n"
       << " * \\param[in] error: current error\n"
       << " */\n"
       << "bool checkConvergence(const NumericType error) {\n"
       << "auto converged = error < this->epsilon;\n";
    if (this->mb.hasCode(h, BehaviourData::AdditionalConvergenceChecks)) {
      os << "auto mfront_internals_converged = converged;\n"
         << "this->additionalConvergenceChecks(converged, error);\n"
         << "if((mfront_internals_converged) && (!converged)){\n"
         << "this->is_delta_zeros_defined = false;\n"
         << "}\n";
    }
    os << "return converged;\n"
       << "}\n"
       << "/*!\n"
       << " * \\brief update the jacobian matrix if required.\n"
       << " * \\param[in] converged: boolean stating if the method is\n"
       << " * called after the convergence of the algorithm or before "
       << " * the computation of the next correction.\n"
       << " *\n"
       << " * This method can be used to compute the jacobian or part "
       << " * of the jacobian numerically. If the jacobian was computed\n"
       << " * in `computeResidual`, this method can be used to compare it "
       << " * to a numerical approximation.\n"
       << " */\n"
       << "void updateOrCheckJacobian(){\n";
    if (this->solver->requiresNumericalJacobian()) {
      os << "this->computeNumericalJacobian(this->jacobian);\n";
    } else {
      NonLinearSystemSolverBase::writeEvaluateNumericallyComputedBlocks(os, mb,
                                                                        h);
      if (mb.hasAttribute(h, BehaviourData::compareToNumericalJacobian)) {
        os << "tfel::math::tmatrix<" << n2 << ", " << n2
           << ", NumericType> njacobian;\n";
        NonLinearSystemSolverBase::writeComparisonToNumericalJacobian(
            os, mb, h, "njacobian");
      }
    }
    os << "}\n"
       << "/*!\n"
       << " * \\brief method meant to set bounds on some components "
       << " * of the current Newton correction or to implement a line "
       << " * search.\n"
       << " */\n"
       << "void processNewCorrection()\n"
       << "{\n";
    if (this->mb.hasCode(h, BehaviourData::ProcessNewCorrection)) {
      os << "using namespace std;\n"
         << "using namespace tfel::math;\n"
         << "using std::vector;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
      writeVariablesOffsets(os, d.getIntegrationVariables());
      declareViewsFromArrayOfVariables(
          os, d.getIntegrationVariables(),
          [](const std::string& n) { return "delta_d" + n; },
          "this->delta_zeros", this->mb.useQt());
    }
    NonLinearSystemSolverBase::writeLimitsOnIncrementValues(
        os, this->mb, h, "this->delta_zeros");
    if (this->mb.hasCode(h, BehaviourData::ProcessNewCorrection)) {
      os << this->mb.getCodeBlock(h, BehaviourData::ProcessNewCorrection).code
         << "\n";
      writeIgnoreVariablesOffsets(os, d.getIntegrationVariables());
      for (const auto& v : d.getIntegrationVariables()) {
        os << "static_cast<void>(delta_d" << v.name << ");\n";
      }
    }
    os << "}\n"
       << "/*!\n"
       << " * \\brief method called when the current Newton is rejected\n"
       << " */\n";
    if (this->mb.hasCode(h, BehaviourData::RejectCurrentCorrection)) {
      os << "void rejectCurrentCorrection()\n"
         << "{\n"
         << "using namespace std;\n"
         << "using namespace tfel::math;\n"
         << "using std::vector;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
      writeVariablesOffsets(os, d.getIntegrationVariables());
      declareViewsFromArrayOfVariables(
          os, d.getIntegrationVariables(),
          [](const std::string& n) { return "delta_d" + n; },
          "this->delta_zeros", this->mb.useQt());
      os << this->mb.getCodeBlock(h, BehaviourData::RejectCurrentCorrection)
                .code
         << "\n";
      writeIgnoreVariablesOffsets(os, d.getIntegrationVariables());
      for (const auto& v : d.getIntegrationVariables()) {
        os << "static_cast<void>(delta_d" << v.name << ");\n";
      }
      os << "}\n";
    }
    os << "/*!\n"
       << " * \\brief method meant to process the new estimate.\n"
       << " *\n"
       << " * This method may be called to apply bounds on the "
       << " * new estimate.\n"
       << " */\n"
       << "void processNewEstimate(){\n";
    if (this->mb.hasCode(h, BehaviourData::ProcessNewEstimate)) {
      os << "using namespace std;\n"
         << "using namespace tfel::math;\n"
         << "using std::vector;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
    }
    NonLinearSystemSolverBase::
        writeLimitsOnIncrementValuesBasedOnStateVariablesPhysicalBounds(
            os, this->mb, h);
    NonLinearSystemSolverBase::
        writeLimitsOnIncrementValuesBasedOnIntegrationVariablesIncrementsPhysicalBounds(
            os, this->mb, h);
    if (this->mb.hasCode(h, BehaviourData::ProcessNewEstimate)) {
      os << this->mb.getCodeBlock(h, BehaviourData::ProcessNewEstimate).code
         << "\n";
    }
    os << "this->updateMaterialPropertiesDependantOnStateVariables();\n"
       << "}\n";
    if (getDebugMode()) {
      os << "//! \\brief method called when the resolution begins\n"
         << "void reportBeginningOfResolution() const {\n"
         << "std::cout << '\\n' << \"" << mb.getClassName()
         << "::integrate() : beginning of resolution\\n\";\n"
         << "}\n"
         << "//! \\brief method called when the resolution succeeds\n"
         << "void reportSuccess() const {\n"
         << "std::cout << \"" << this->mb.getClassName()
         << "::integrate() : convergence after \" "
         << "<< this->iter << \" iterations\\n\\n\";\n"
         << "}\n"
         << "//! \brief method called when the resolution fails\n"
         << "void reportFailure() const {\n"
         << "std::cout << \"" << this->mb.getClassName()
         << "::integrate() : no convergence after \" "
         << "<< this->iter << \" iterations\\n\\n\";\n"
         << "std::cout << *this << '\\n';\n"
         << "}\n"
         << "/*!\n"
         << " * \\brief method called with the evaluation of the\n"
         << " * residual failed.\n "
         << " */\n"
         << "void reportInvalidResidualEvaluation() const {"
         << "std::cout << '\\n' << \"" << mb.getClassName()
         << "::integrate() : computFdF returned false or the norm of the "
         << "residual is not finite, dividing increment by "
         << "two\\n\\n\";\n"
         << "}\n"
         << "/*!\n"
         << " * \brief method called when the computation of a new correction\n"
         << " * failed.\n"
         << " */\n"
         << "void reportNewCorrectionComputationFailure() const {\n"
         << "std::cout << '\\n' << \"" << mb.getClassName()
         << "::integrate(): failed to compute a new correction\";\n"
         << "}\n"
         << "/*!\n"
         << " * \\brief method called after a standard Newton step\n"
         << " * \\param[in] e: error\n"
         << " */\n"
         << "void reportStandardIteration(const NumericType error) const {\n"
         << "std::cout << \"" << mb.getClassName()
         << "::integrate() : iteration \" "
         << "<< this->iter << \" : \" << error << '\\n';\n"
         << "}\n";
    }
    os << "/*!\n"
       << " * \\brief solve linear system\n"
       << " * \\return true on success\n"
       << " * \\param[in] m: matrix\n"
       << " * \\param[in,out] v: right hand side on input, solution on output\n"
       << " */\n"
       << "bool solveLinearSystem("
       << "tfel::math::tmatrix<" << n2 << ", " << n2
       << ", NumericType>& mfront_matrix,"
       << "tfel::math::tvector<" << n2 << ", NumericType>& mfront_vector)"
       << "const noexcept{\n"
       << "auto mfront_success = true;\n";
    if (mb.getAttribute(BehaviourData::profiling, false)) {
      writeStandardPerformanceProfilingBegin(os, mb.getClassName(),
                                             "TinyMatrixSolve", "lu");
    }
    os << "mfront_success = "
       << this->solver->getExternalAlgorithmClassName(this->mb, h)
       << "::solveLinearSystem(mfront_matrix, mfront_vector);\n";
    if (mb.getAttribute(BehaviourData::profiling, false)) {
      writeStandardPerformanceProfilingEnd(os);
    }
    os << "return mfront_success;\n"
       << "}\n";
    this->writeComputeFdF(os, h);
  }  // end of writeBehaviourIntegrator

  void ImplicitDSLBase::writeComputeFdF(std::ostream& os,
                                        const Hypothesis h) const {
    const auto& d = this->mb.getBehaviourData(h);
    auto jp(this->jacobianPartsUsedInIntegrator);
    if (this->solver
            ->requiresJacobianToBeReinitialisedToIdentityAtEachIterations()) {
      for (const auto& v : d.getIntegrationVariables()) {
        jp.insert("df" + v.name + "_dd" + v.name);
      }
    }
    //
    os << "/*!\n"
       << "* \\brief compute fzeros and jacobian\n"
       << "*/\n"
       << "bool computeFdF(const bool perturbatedSystemEvaluation){\n"
       << "using namespace std;\n"
       << "using namespace tfel::math;\n"
       << "using std::vector;\n";
    if (this->mb.getAttribute(BehaviourData::profiling, false)) {
      writeStandardPerformanceProfiling(os, mb.getClassName(), "ComputeFdF");
    }
    writeMaterialLaws(os, this->mb.getMaterialLaws());
    os << "// silent compiler warning\n"
       << "static_cast<void>(perturbatedSystemEvaluation); \n";
    writeVariablesOffsets(os, d.getIntegrationVariables());
    declareViewsFromArrayOfVariables(
        os, d.getIntegrationVariables(),
        [](const std::string& n) { return "f" + n; }, "this->fzeros",
        this->mb.useQt());
    if ((this->solver->usesJacobian()) &&
        (!(this->solver->requiresNumericalJacobian()))) {
      auto n = SupportedTypes::TypeSize();
      for (const auto& v : d.getIntegrationVariables()) {
        auto n2 = SupportedTypes::TypeSize();
        for (const auto& v2 : d.getIntegrationVariables()) {
          if ((v.arraySize == 1u) && (v2.arraySize == 1u)) {
            os << "// derivative of residual f" << v.name
               << " with respect to variable " << v2.name << "\n"
               << NonLinearSystemSolverBase::getJacobianPart(v, v2, n, n2);
          }
          n2 += this->getTypeSize(v2.type, v2.arraySize);
        }
        n += this->getTypeSize(v.type, v.arraySize);
      }
    }
    if (this->solver
            ->requiresJacobianToBeReinitialisedToIdentityAtEachIterations()) {
      os << "// setting jacobian to identity\n"
         << "std::fill(this->jacobian.begin(),this->jacobian.end(), "
            "NumericType(0))"
            ";\n"
         << "for(ushort mfront_idx = 0; mfront_idx != "
         << mfront::getTypeSize(d.getIntegrationVariables())
         << "; ++mfront_idx){\n"
         << "this->jacobian(mfront_idx, mfront_idx) = NumericType(1);\n"
         << "}\n";
    }
    os << "// setting f values to zeros\n"
       << "this->fzeros = this->zeros;\n";
    for (const auto& v : d.getIntegrationVariables()) {
      if (this->mb.hasAttribute(h, v.name + "_normalisation_factor")) {
        const auto& nf = this->mb.getAttribute<std::string>(
            h, v.name + "_normalisation_factor");
        os << "f" << v.name << " *= " << nf << ";\n";
      }
    }
    os << this->mb.getCodeBlock(h, BehaviourData::Integrator).code << "\n";
    for (const auto& v : d.getIntegrationVariables()) {
      if (this->mb.hasAttribute(h, 'f' + v.name + "_normalisation_factor")) {
        const auto& nf = this->mb.getAttribute<std::string>(
            h, 'f' + v.name + "_normalisation_factor");
        os << "f" << v.name << "*= NumericType(1)/(" << nf << ");\n";
      }
    }
    if ((this->solver->usesJacobian()) &&
        (!(this->solver->requiresNumericalJacobian()))) {
      for (const auto& v : d.getIntegrationVariables()) {
        for (const auto& v2 : d.getIntegrationVariables()) {
          if ((v.arraySize == 1u) && (v2.arraySize == 1u)) {
            os << "static_cast<void>(df" << v.name << "_dd" << v2.name
               << "); /* suppress potential warnings */\n";
          }
          if (jp.find("df" + v.name + "_dd" + v2.name) != jp.end()) {
            const bool bfnf = this->mb.hasAttribute(
                h, 'f' + v.name + "_normalisation_factor");
            const bool bvnf =
                this->mb.hasAttribute(h, v2.name + "_normalisation_factor");
            if (bfnf) {
              const auto& fnf = this->mb.getAttribute<std::string>(
                  h, 'f' + v.name + "_normalisation_factor");
              if (bvnf) {
                const auto& vnf = this->mb.getAttribute<std::string>(
                    h, v2.name + "_normalisation_factor");
                if ((v.arraySize != 1u) && (v2.arraySize != 1u)) {
                  os << "for(ushort mfront_idx=0; mfront_idx !=" << v.arraySize
                     << "; ++mfront_idx){\n";
                  os << "for(ushort mfront_idx2=0; mfront_idx2!="
                     << v2.arraySize << "; ++mfront_idx2){\n";
                  os << "df" << v.name << "_dd" << v2.name
                     << "(mfront_idx,mfront_idx2) *= " << vnf << "/(" << fnf
                     << ");\n";
                  os << "}\n";
                  os << "}\n";
                } else if ((v.arraySize != 1u) && (v2.arraySize == 1u)) {
                  os << "for(ushort mfront_idx=0; mfront_idx !=" << v.arraySize
                     << "; ++mfront_idx){\n";
                  os << "df" << v.name << "_dd" << v2.name
                     << "(mfront_idx) *= " << vnf << "/(" << fnf << ");\n";
                  os << "}\n";
                } else if ((v.arraySize == 1u) && (v2.arraySize != 1u)) {
                  os << "for(ushort mfront_idx=0; mfront_idx !=" << v2.arraySize
                     << "; ++mfront_idx){\n";
                  os << "df" << v.name << "_dd" << v2.name
                     << "(mfront_idx) *= " << vnf << "/(" << fnf << ");\n";
                  os << "}\n";
                } else if ((v.arraySize == 1u) && (v2.arraySize == 1u)) {
                  os << "df" << v.name << "_dd" << v2.name << " *= " << vnf
                     << "/(" << fnf << ");\n";
                }
              } else {
                if ((v.arraySize != 1u) && (v2.arraySize != 1u)) {
                  os << "for(ushort mfront_idx=0; mfront_idx !=" << v.arraySize
                     << "; ++mfront_idx){\n";
                  os << "for(ushort mfront_idx2=0; mfront_idx2!="
                     << v2.arraySize << "; ++mfront_idx2){\n";
                  os << "df" << v.name << "_dd" << v2.name
                     << "(mfront_idx,mfront_idx2) *= "
                     << "NumericType(1)/(" << fnf << ");\n";
                  os << "}\n";
                  os << "}\n";
                } else if ((v.arraySize != 1u) && (v2.arraySize == 1u)) {
                  os << "for(ushort mfront_idx=0; mfront_idx !=" << v.arraySize
                     << "; ++mfront_idx){\n";
                  os << "df" << v.name << "_dd" << v2.name << "(mfront_idx) *= "
                     << "NumericType(1)/(" << fnf << ");\n";
                  os << "}\n";
                } else if ((v.arraySize == 1u) && (v2.arraySize != 1u)) {
                  os << "for(ushort mfront_idx=0; mfront_idx !=" << v2.arraySize
                     << "; ++mfront_idx){\n";
                  os << "df" << v.name << "_dd" << v2.name << "(mfront_idx) *= "
                     << "NumericType(1)/(" << fnf << ");\n";
                  os << "}\n";
                } else if ((v.arraySize == 1u) && (v2.arraySize == 1u)) {
                  os << "df" << v.name << "_dd" << v2.name << " *= "
                     << "NumericType(1)/(" << fnf << ");\n";
                }
              }
            } else {
              if (bvnf) {
                const auto& vnf = this->mb.getAttribute<std::string>(
                    h, v2.name + "_normalisation_factor");
                if ((v.arraySize != 1u) && (v2.arraySize != 1u)) {
                  os << "for(ushort mfront_idx=0; mfront_idx !=" << v.arraySize
                     << "; ++mfront_idx){\n";
                  os << "for(ushort mfront_idx2=0; mfront_idx2!="
                     << v2.arraySize << "; ++mfront_idx2){\n";
                  os << "df" << v.name << "_dd" << v2.name
                     << "(mfront_idx,mfront_idx2) *= " << vnf << ";\n";
                  os << "}\n";
                  os << "}\n";
                } else if ((v.arraySize != 1u) && (v2.arraySize == 1u)) {
                  os << "for(ushort mfront_idx=0; mfront_idx !=" << v.arraySize
                     << "; ++mfront_idx){\n";
                  os << "df" << v.name << "_dd" << v2.name
                     << "(mfront_idx) *= " << vnf << ";\n";
                  os << "}\n";
                } else if ((v.arraySize == 1u) && (v2.arraySize != 1u)) {
                  os << "for(ushort mfront_idx=0; mfront_idx !=" << v2.arraySize
                     << "; ++mfront_idx){\n";
                  os << "df" << v.name << "_dd" << v2.name
                     << "(mfront_idx) *= " << vnf << ";\n";
                  os << "}\n";
                } else if ((v.arraySize == 1u) && (v2.arraySize == 1u)) {
                  os << "df" << v.name << "_dd" << v2.name << " *= " << vnf
                     << ";\n";
                }
              }
            }
          }
        }
      }
    }
    writeIgnoreVariablesOffsets(os, d.getIntegrationVariables());
    os << "return true;\n"
       << "}\n\n";
  }  // end of writeBehaviourIntegrator

  std::string ImplicitDSLBase::getIntegrationVariablesIncrementsInitializers(
      const Hypothesis h) const {
    SupportedTypes::TypeSize n;
    std::ostringstream init;
    bool first = true;
    for (const auto& v :
         this->mb.getBehaviourData(h).getIntegrationVariables()) {
      const auto flag = getTypeFlag(v.type);
      if (!first) {
        init << ",\n";
      }
      if (v.arraySize == 1u) {
        if (flag == SupportedTypes::SCALAR) {
          init << "d" << v.name << "(this->zeros(" << n << "))";
        } else {
          init << "d" << v.name << "(tfel::math::map<" << v.type << ", " << n
               << ">(this->zeros))";
        }
      } else {
        init << "d" << v.name << "(tfel::math::map<" << v.arraySize << ", "
             << v.type << ", " << n << ">(this->zeros))";
      }
      n += this->getTypeSize(v.type, v.arraySize);
      first = false;
    }
    return init.str();
  }  // end of getIntegrationVariableIncrementsInitializers

  std::string ImplicitDSLBase::getLocalVariablesInitializers(
      const Hypothesis) const {
    auto init = std::string{};
    auto append = [&init](const std::string& s) {
      if (s.empty()) {
        return;
      }
      if (!init.empty()) {
        init += ",\n";
      }
      init += s;
    };
    if (this->mb.getAttribute(BehaviourDescription::computesStiffnessTensor,
                              false)) {
      if (this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep()) {
        append("D_tdt(D)");
      }
    }
    for (const auto& ht : this->mb.getHillTensors()) {
      if ((this->mb.getBehaviourType() !=
           BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) &&
          (this->mb.getBehaviourType() !=
           BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR)) {
        this->throwRuntimeError(
            "ImplicitDSLBase::getLocalVariablesInitializers",
            "Hill tensors shall only be defined for finite strain "
            "or small strain behaviours");
      }
      if (this->mb.areMaterialPropertiesConstantDuringTheTimeStep(ht.c)) {
        append(ht.name + "_tdt(" + ht.name + ")");
      }
    }
    return init;
  }  // end of getLocalVariablesInitializers

  void ImplicitDSLBase::writeBehaviourParserSpecificInitializeMethodPart(
      std::ostream& os, const Hypothesis h) const {
    this->solver->writeSpecificInitializeMethodPart(os, mb, h);
  }

  void ImplicitDSLBase::writeBehaviourIntegrationVariablesIncrements(
      std::ostream& os, const Hypothesis h) const {
    const auto& d = this->mb.getBehaviourData(h);
    this->checkBehaviourFile(os);
    for (const auto& v : d.getIntegrationVariables()) {
      if ((!getDebugMode()) && (v.lineNumber != 0u)) {
        os << "#line " << v.lineNumber << " \"" << this->fd.fileName << "\"\n";
      }
      if (v.arraySize == 1u) {
        if (SupportedTypes::getTypeFlag(v.type) == SupportedTypes::SCALAR) {
          if (this->mb.useQt()) {
            os << "tfel::math::scalar_view<" << v.type << "> "
               << "d" << v.name << ";\n";
          } else {
            os << "real& d" << v.name << ";\n";
          }
        } else {
          os << "tfel::math::View<" << v.type << "> d" << v.name << ";\n";
        }
      } else {
        os << "tfel::math::ViewsFixedSizeVector<" << v.type << ", ushort, "
           << v.arraySize << "> d" << v.name << ";\n";
      }
    }
    os << '\n';
  }

  void ImplicitDSLBase::getSymbols(std::map<std::string, std::string>& symbols,
                                   const Hypothesis h,
                                   const std::string& n) {
    using namespace tfel::material;
    BehaviourDSLCommon::getSymbols(symbols, h, n);
    const auto& d = this->mb.getBehaviourData(h);
    for (const auto& v : d.getIntegrationVariables()) {
      getIncrementSymbol(symbols, v);
    }
    for (const auto& mv : this->mb.getMainVariables()) {
      if (Gradient::isIncrementKnown(mv.first)) {
        getIncrementSymbol(symbols, mv.first);
      } else {
        mfront::addSymbol(symbols, displayName(mv.first) + "\u2080",
                          mv.first.name + "0");
        mfront::addSymbol(symbols, displayName(mv.first) + "\u2081",
                          mv.first.name + "1");
      }
    }
    mfront::getIncrementSymbols(symbols, d.getExternalStateVariables());
    mfront::addSymbol(symbols, "\u0394t", "dt");
    if (n == BehaviourData::Integrator) {
      for (const auto& v1 : d.getIntegrationVariables()) {
        if (!v1.symbolic_form.empty()) {
          addSymbol(symbols, "f" + v1.symbolic_form, "f" + v1.name);
        }
        for (const auto& v2 : d.getIntegrationVariables()) {
          const auto& s1 =
              !v1.symbolic_form.empty() ? v1.symbolic_form : v1.name;
          const auto& s2 =
              !v2.symbolic_form.empty() ? v2.symbolic_form : v2.name;
          addSymbol(symbols, "\u2202f" + s1 + "\u2215\u2202\u0394" + s2,
                    "df" + v1.name + "_dd" + v2.name);
        }
      }
    }
    if (n == BehaviourData::ProcessNewCorrection) {
      for (const auto& v : d.getIntegrationVariables()) {
        const auto& s = !v.symbolic_form.empty() ? v.symbolic_form : v.name;
        addSymbol(symbols, "\u03B4\u0394" + s, "delta_d" + v.name);
      }
    }
    const auto is_tangent_operator_code_block = [this, &n] {
      if (this->mb.getBehaviourType() ==
          BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) {
        // all available tangent operators for finite strain  behaviours
        for (const auto& t : getFiniteStrainBehaviourTangentOperatorFlags()) {
          const auto ktype =
              convertFiniteStrainBehaviourTangentOperatorFlagToString(t);
          const auto bn =
              std::string(BehaviourData::ComputeTangentOperator) + '-' + ktype;
          if (n == bn) {
            return true;
          }
        }
        return false;
      } else {
        return n == BehaviourData::ComputeTangentOperator;
      }
    }();
    if (is_tangent_operator_code_block) {
      for (const auto& to : this->mb.getTangentOperatorBlocks()) {
        const auto& sname = to.second.symbolic_form;
        if (!sname.empty()) {
          const auto& name = to.second.name;
          addSymbol(symbols, "getIntegrationVariablesDerivatives_" + sname,
                    "getIntegrationVariablesDerivatives_" + name);
        }
        for (const auto& iv : d.getIntegrationVariables()) {
          const auto s1 = iv.symbolic_form.empty() ? iv.name : iv.symbolic_form;
          const auto s2 = to.second.symbolic_form.empty()
                              ? to.second.name
                              : to.second.symbolic_form;
          addSymbol(symbols, "\u2202f" + s1 + "\u2215\u2202\u0394" + s2,
                    "df" + iv.name + "_dd" + to.second.name);
        }
      }
      for (const auto& iv1 : d.getIntegrationVariables()) {
        for (const auto& iv2 : d.getIntegrationVariables()) {
          if ((iv1.symbolic_form.empty()) && (iv2.symbolic_form.empty())) {
            continue;
          }
          const auto s1 =
              iv1.symbolic_form.empty() ? iv1.name : iv1.symbolic_form;
          const auto s2 =
              iv2.symbolic_form.empty() ? iv2.name : iv2.symbolic_form;
          addSymbol(symbols, "iJ_" + s1 + "_" + s2,
                    "iJ_" + iv1.name + "_" + iv2.name);
        }
      }
    }
  }  // end of getSymbols

  ImplicitDSLBase::~ImplicitDSLBase() = default;

}  // end of namespace mfront
