/*!
 * \file   mfront/src/RungeKuttaDSLBase.cxx
 * \brief
 * \author Thomas Helfer
 * \date   10/11/2006
 * \copyright Copyright (C) 2006-2018 CEA/DEN, EDF R&D. All rights
 * reserved.
 * This project is publicly released under either the GNU GPL Licence
 * or the CECILL-A licence. A copy of thoses licences are delivered
 * with the sources of TFEL. CEA or EDF may also distribute this
 * project under specific licensing conditions.
 */

#include <string>
#include <sstream>
#include <stdexcept>
#include "TFEL/Raise.hxx"
#include "TFEL/Glossary/Glossary.hxx"
#include "TFEL/Glossary/GlossaryEntry.hxx"
#include "TFEL/Utilities/StringAlgorithms.hxx"
#include "MFront/DSLUtilities.hxx"
#include "MFront/DSLFactory.hxx"
#include "MFront/PedanticMode.hxx"
#include "MFront/MFrontDebugMode.hxx"
#include "MFront/MFrontLogStream.hxx"
#include "MFront/PerformanceProfiling.hxx"
#include "MFront/RungeKuttaDSLBase.hxx"

// fixing a bug on current glibc++ cygwin versions (19/08/2015)
#if defined __CYGWIN__ && (!defined _GLIBCXX_USE_C99)
#include <sstream>
namespace std {
  template <typename T>
  std::string to_string(const T& v) {
    std::ostringstream s;
    s << v;
    return s.str();
  }
}  // namespace std
#endif /* defined __CYGWIN__ &&  (!defined _GLIBCXX_USE_C99) */

namespace mfront {

  static std::set<std::string> getVariablesUsedDuringIntegration(
      const BehaviourDescription& mb, const RungeKuttaDSLBase::Hypothesis h) {
    const auto& d = mb.getBehaviourData(h);
    //! all registred members used in this block
    auto uvs = d.getCodeBlock(BehaviourData::ComputeDerivative).members;
    if (d.hasCode(BehaviourData::ComputeThermodynamicForces)) {
      const auto& uvs2 =
          d.getCodeBlock(BehaviourData::ComputeThermodynamicForces).members;
      uvs.insert(uvs2.begin(), uvs2.end());
    }
    // variables used to compute the stiffness tensor
    if ((mb.getAttribute<bool>(BehaviourDescription::computesStiffnessTensor,
                               false)) &&
        (!mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      const auto& mps = mb.getElasticMaterialProperties();
      for (const auto& mp : mps) {
        if (mp.is<BehaviourDescription::ExternalMFrontMaterialProperty>()) {
          const auto& cmp =
              mp.get<BehaviourDescription::ExternalMFrontMaterialProperty>();
          for (const auto& i : mb.getMaterialPropertyInputs(*(cmp.mpd))) {
            uvs.insert(i.name);
          }
        }
        if (mp.is<BehaviourDescription::AnalyticMaterialProperty>()) {
          const auto& amp =
              mp.get<BehaviourDescription::AnalyticMaterialProperty>();
          for (const auto& i :
               mb.getMaterialPropertyInputs(amp.getVariablesNames())) {
            uvs.insert(i.name);
          }
        }
      }
    }
    return uvs;
  }

  static void writeExternalVariablesCurrentValues(
      std::ostream& f,
      const BehaviourDescription& mb,
      const RungeKuttaDSLBase::Hypothesis h,
      const std::string& p) {
    const auto t = ((p == "0") ? "(t/this->dt)"
                               : ((p == "1") ? "((t+dt_)/this->dt)"
                                             : "((t+" + p + "*dt_)/this->dt)"));
    const auto& d = mb.getBehaviourData(h);
    const auto uvs = getVariablesUsedDuringIntegration(mb, h);
    for (const auto& mv : mb.getMainVariables()) {
      const auto& dv = mv.first;
      if (uvs.find(dv.name) != uvs.end()) {
        if (Gradient::isIncrementKnown(dv)) {
          f << "this->" + dv.name + "_ = this->" + dv.name + "+(this->d" +
                   dv.name + ")*"
            << t << ";\n";
        } else {
          f << "this->" + dv.name + "_ = this->" + dv.name + "0+(this->" +
                   dv.name + "1-this->" + dv.name + "0)*"
            << t << ";\n";
        }
      }
    }
    for (const auto& v : d.getExternalStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        f << "this->" << v.name << "_ = this->" << v.name << "+(this->d"
          << v.name << ")*" << t << ";\n";
      }
    }
  }

  static void writeExternalVariableCurrentValue2(std::ostream& f,
                                                 const std::string& n,
                                                 const std::string& p,
                                                 const bool b) {
    if (p == "0") {
      f << "this->" << n << "_ = this->" << n << ";\n";
    } else if (p == "1") {
      if (b) {
        f << "this->" << n << "_ = this->" << n << "+this->d" << n << ";\n";
      } else {
        f << "this->" << n << "_ = this->" << n << "1;\n";
      }
    } else {
      if (b) {
        f << "this->" << n << "_ = this->" << n << "+" << p << "*(this->d" << n
          << ");\n";
      } else {
        f << "this->" << n << "_ = this->" << n << "+" << p << "*(this->" << n
          << "1-this->" << n << "0);\n";
      }
    }
  }

  /*!
   * \brief modifier used for evaluating the stiffness tensor during the
   * the time step
   */
  static std::function<
      std::string(const BehaviourDescription::MaterialPropertyInput&)>&
  modifyVariableForStiffnessTensorComputation(const std::string& cn) {
    using MaterialPropertyInput = BehaviourDescription::MaterialPropertyInput;
    using Modifier = std::function<std::string(const MaterialPropertyInput&)>;
    static Modifier m =
        [cn](const BehaviourDescription::MaterialPropertyInput& i) {
          if ((i.category == MaterialPropertyInput::TEMPERATURE) ||
              (i.category == MaterialPropertyInput::EXTERNALSTATEVARIABLE)) {
            return "this->" + i.name + "_";
          } else if (i.category ==
                     MaterialPropertyInput::
                         AUXILIARYSTATEVARIABLEFROMEXTERNALMODEL) {
            return "(this->" + i.name + "+(t/(this->dt))*(this->d" + i.name +
                   "))";
          } else if ((i.category == MaterialPropertyInput::MATERIALPROPERTY) ||
                     (i.category == MaterialPropertyInput::PARAMETER)) {
            return "this->" + i.name;
          } else if (i.category == MaterialPropertyInput::STATICVARIABLE) {
            return cn + "::" + i.name;
          } else {
            tfel::raise(
                "modifyVariableForStiffnessTensorComputation: "
                "unsupported input type for "
                "variable '" +
                i.name + "'");
          }
        };
    return m;
  }  // end of modifyVariableForStiffnessTensorComputation

  /*!
   * \brief modifier used for evaluating the stiffness tensor at the end of
   * the time step.
   */
  static std::function<
      std::string(const BehaviourDescription::MaterialPropertyInput&)>&
  modifyVariableForStiffnessTensorComputation2(const std::string& cn) {
    using MaterialPropertyInput = BehaviourDescription::MaterialPropertyInput;
    using Modifier = std::function<std::string(const MaterialPropertyInput&)>;
    static Modifier m = [cn](const BehaviourDescription::MaterialPropertyInput&
                                 i) {
      if ((i.category == MaterialPropertyInput::TEMPERATURE) ||
          (i.category ==
           MaterialPropertyInput::AUXILIARYSTATEVARIABLEFROMEXTERNALMODEL) ||
          (i.category == MaterialPropertyInput::EXTERNALSTATEVARIABLE)) {
        return "this->" + i.name + "+this->d" + i.name;
      } else if ((i.category == MaterialPropertyInput::MATERIALPROPERTY) ||
                 (i.category == MaterialPropertyInput::PARAMETER)) {
        return "this->" + i.name;
      } else if (i.category == MaterialPropertyInput::STATICVARIABLE) {
        return cn + "::" + i.name;
      } else {
        tfel::raise(
            "modifyVariableForStiffnessTensorComputation2: "
            "unsupported input type for "
            "variable '" +
            i.name + "'");
      }
    };
    return m;
  }  // end of modifyVariableForStiffnessTensorComputation2

  static void writeExternalVariablesCurrentValues2(
      std::ostream& f,
      const BehaviourDescription& mb,
      const RungeKuttaDSLBase::Hypothesis h,
      const std::string& p) {
    const auto& d = mb.getBehaviourData(h);
    const auto uvs = getVariablesUsedDuringIntegration(mb, h);
    for (const auto& mv : mb.getMainVariables()) {
      const auto& dv = mv.first;
      if (uvs.find(dv.name) != uvs.end()) {
        writeExternalVariableCurrentValue2(f, dv.name, p,
                                           Gradient::isIncrementKnown(dv));
      }
    }
    for (const auto& v : d.getExternalStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        writeExternalVariableCurrentValue2(f, v.name, p, true);
      }
    }
  }  // end of writeExternalVariablesCurrentValues2

  RungeKuttaDSLBase::RungeKuttaDSLBase(const DSLOptions& opts)
      : BehaviourDSLBase<RungeKuttaDSLBase>(opts) {
    this->useStateVarTimeDerivative = true;
    // parameters
    this->reserveName("dtmin");
    this->reserveName("epsilon");
    // Default state vars
    this->reserveName("dt_");
    this->reserveName("corrector");
    this->reserveName("dtprec");
    this->reserveName("converged");
    this->reserveName("error");
    this->reserveName("failed");
    this->reserveName("cste1_2");
    this->reserveName("cste1_4");
    this->reserveName("cste3_8");
    this->reserveName("cste3_32");
    this->reserveName("cste12_13");
    this->reserveName("cste1932_2197");
    this->reserveName("cste7200_2197");
    this->reserveName("cste7296_2197");
    this->reserveName("cste439_216");
    this->reserveName("cste3680_513");
    this->reserveName("cste845_4104");
    this->reserveName("cste8_27");
    this->reserveName("cste3544_2565");
    this->reserveName("cste1859_4104");
    this->reserveName("cste11_40");
    this->reserveName("cste16_135");
    this->reserveName("cste6656_12825");
    this->reserveName("cste28561_56430");
    this->reserveName("cste9_50");
    this->reserveName("cste2_55");
    this->reserveName("cste1_360");
    this->reserveName("cste128_4275");
    this->reserveName("cste2197_75240");
    this->reserveName("cste1_50");
    this->reserveName("rk_update_error");
    this->reserveName("rk_error");
    // CallBacks
    this->registerNewCallBack(
        "@UsableInPurelyImplicitResolution",
        &RungeKuttaDSLBase::treatUsableInPurelyImplicitResolution);
    this->registerNewCallBack("@MaterialLaw",
                              &RungeKuttaDSLBase::treatMaterialLaw);
    this->registerNewCallBack("@Algorithm", &RungeKuttaDSLBase::treatAlgorithm);
    this->registerNewCallBack("@TangentOperator",
                              &RungeKuttaDSLBase::treatTangentOperator);
    this->registerNewCallBack(
        "@IsTangentOperatorSymmetric",
        &RungeKuttaDSLBase::treatIsTangentOperatorSymmetric);
    this->registerNewCallBack("@Derivative",
                              &RungeKuttaDSLBase::treatDerivative);
    this->registerNewCallBack("@Epsilon", &RungeKuttaDSLBase::treatEpsilon);
    this->registerNewCallBack(
        "@StressErrorNormalizationFactor",
        &RungeKuttaDSLBase::treatStressErrorNormalizationFactor);
    this->registerNewCallBack(
        "@StressErrorNormalisationFactor",
        &RungeKuttaDSLBase::treatStressErrorNormalizationFactor);
    this->registerNewCallBack("@MinimalTimeStep",
                              &RungeKuttaDSLBase::treatMinimalTimeStep);
    this->disableCallBack("@Integrator");
    this->disableCallBack("@ComputedVar");
    this->registerNewCallBack("@ComputeStiffnessTensor",
                              &RungeKuttaDSLBase::treatComputeStiffnessTensor);
    this->mb.setIntegrationScheme(BehaviourDescription::EXPLICITSCHEME);
  }

  std::string RungeKuttaDSLBase::getCodeBlockTemplate(
      const std::string& c, const MFrontTemplateGenerationOptions& o) const {
    constexpr auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    if (c == BehaviourData::ComputePredictionOperator) {
      return "@PredictionOperator{}\n";
    } else if (c == BehaviourData::ComputeThermodynamicForces) {
      return "@ComputeStress{}\n";
    } else if (c == BehaviourData::ComputeDerivative) {
      const auto ivs = this->mb.getBehaviourData(h).getIntegrationVariables();
      if (ivs.empty()) {
        return "@Derivative{}\n";
      } else {
        auto i = std::string("@Derivative{\n");
        for (const auto& v : ivs) {
          if (o.useUnicodeSymbols) {
            i += "d\u209C" + displayName(v) + " = ;\n";
          } else {
            i += "d" + v.name + " = ;\n";
          }
        }
        i += "}\n";
        return i;
      }
    } else if (c == BehaviourData::ComputeTangentOperator) {
      return "@TangentOperator{}\n";
    }
    return "";
  }  // end of getCodeBlockTemplate

  void RungeKuttaDSLBase::treatUpdateAuxiliaryStateVariables() {
    this->treatCodeBlock(*this, BehaviourData::UpdateAuxiliaryStateVariables,
                         &RungeKuttaDSLBase::standardModifier, true, true);
  }  // end of treatUpdateAuxiliaryStateVarBase

  void RungeKuttaDSLBase::treatComputeFinalThermodynamicForces() {
    this->treatCodeBlock(*this, BehaviourData::ComputeFinalThermodynamicForces,
                         &RungeKuttaDSLBase::standardModifier, true, true);
  }  // end of treatUpdateAuxiliaryStateVarBase

  std::string RungeKuttaDSLBase::computeThermodynamicForcesVariableModifier1(
      const Hypothesis h, const std::string& var, const bool addThisPtr) {
    const auto& d = this->mb.getBehaviourData(h);
    if ((this->mb.isGradientName(var)) ||
        (this->mb.isGradientIncrementName(var)) ||
        (d.isIntegrationVariableName(var)) ||
        (d.isExternalStateVariableName(var))) {
      if (addThisPtr) {
        return "this->" + var + "_";
      } else {
        return var + "_";
      }
    }
    if (var == "dT") {
      this->declareExternalStateVariableProbablyUnusableInPurelyImplicitResolution(
          h, var.substr(1));
      if (addThisPtr) {
        return "(this->" + var + ")/(this->dt)";
      } else {
        return "(" + var + ")/(this->dt)";
      }
    }
    if (this->mb.isExternalStateVariableIncrementName(h, var)) {
      this->declareExternalStateVariableProbablyUnusableInPurelyImplicitResolution(
          h, var.substr(1));
      const auto& v = d.getExternalStateVariables().getVariable(var.substr(1));
      if (v.arraySize > 1) {
        if (addThisPtr) {
          return "(real(1)/(this->dt)) * (this->" + var + ")";
        } else {
          return "(real(1)/(this->dt)) * " + var;
        }
      } else {
        if (addThisPtr) {
          return "(this->" + var + ")/(this->dt)";
        } else {
          return "(" + var + ")/(this->dt)";
        }
      }
    }
    if (addThisPtr) {
      return "this->" + var;
    }
    return var;
  }  // end of computeThermodynamicForcesVariableModifier1

  std::string RungeKuttaDSLBase::computeThermodynamicForcesVariableModifier2(
      const Hypothesis h, const std::string& var, const bool addThisPtr) {
    const auto& d = this->mb.getBehaviourData(h);
    if ((this->mb.isGradientName(var)) ||
        (d.isExternalStateVariableName(var))) {
      if (addThisPtr) {
        return "this->" + var + "+this->d" + var;
      } else {
        return var + "+d" + var;
      }
    }
    if ((d.isExternalStateVariableIncrementName(var)) || (var == "dT") ||
        (this->mb.isGradientIncrementName(var))) {
      if ((d.isExternalStateVariableIncrementName(var)) || (var == "dT")) {
        this->declareExternalStateVariableProbablyUnusableInPurelyImplicitResolution(
            h, var.substr(1));
      }
      if (addThisPtr) {
        return "(this->" + var + ")/(this->dt)";
      } else {
        return "(" + var + ")/(this->dt)";
      }
    }
    if (addThisPtr) {
      return "this->" + var;
    }
    return var;
  }  // end of computeThermodynamicForcesVariableModifier2

  void RungeKuttaDSLBase::treatComputeThermodynamicForces() {
    if (this->mb.getMainVariables().empty()) {
      this->throwRuntimeError(
          "RungeKuttaDSLBase::treatComputeThermodynamicForces",
          "no thermodynamic force defined");
    }
    this->treatCodeBlock(
        *this, BehaviourData::ComputeThermodynamicForces,
        BehaviourData::ComputeFinalThermodynamicForces,
        &RungeKuttaDSLBase::computeThermodynamicForcesVariableModifier1,
        &RungeKuttaDSLBase::computeThermodynamicForcesVariableModifier2, true,
        true);
  }  // end of treatComputeThermodynamicForces

  void RungeKuttaDSLBase::treatUnknownVariableMethod(const Hypothesis h,
                                                     const std::string& n) {
    auto throw_if = [this](const bool b, const std::string& m) {
      if (b) {
        this->throwRuntimeError("RungeKuttaDSLBase::treatUnknowVariableMethod",
                                m);
      }
    };
    if (this->mb.isIntegrationVariableName(h, n)) {
      if (this->current->value == "setErrorNormalisationFactor") {
        ++(this->current);
        this->checkNotEndOfFile("RungeKuttaDSLBase::treatUnknowVariableMethod");
        this->readSpecifiedToken("RungeKuttaDSLBase::treatUnknowVariableMethod",
                                 "(");
        this->checkNotEndOfFile("RungeKuttaDSLBase::treatUnknowVariableMethod");
        auto enf = this->current->value;
        ++(this->current);
        if ((this->mb.isMaterialPropertyName(h, enf)) ||
            (this->mb.isLocalVariableName(h, enf))) {
          enf = "this->" + enf;
        } else {
          // the current value shall be a number
          auto value = double{};
          try {
            value = tfel::utilities::convert<double>(enf);
          } catch (...) {
            throw_if(true, "Failed to read error normalisation factor.");
          }
          throw_if(value < 0., "invalid error normalisation factor.");
        }
        this->checkNotEndOfFile("RungeKuttaDSLBase::treatUnknowVariableMethod");
        this->readSpecifiedToken("RungeKuttaDSLBase::treatUnknowVariableMethod",
                                 ")");
        try {
          this->mb.setVariableAttribute(
              h, n, VariableDescription::errorNormalisationFactor,
              VariableAttribute(enf), false);
        } catch (std::exception& e) {
          throw_if(true,
                   "error normalisation factor can't be set "
                   "for variable '" +
                       n + "' (" + std::string(e.what()) + ").");
        }
        this->checkNotEndOfFile("RungeKuttaDSLBase::treatUnknowVariableMethod");
        return;
      }
    }
    BehaviourDSLCommon::treatUnknownVariableMethod(h, n);
  }  // end of treatUnknowVariableMethod

  void RungeKuttaDSLBase::treatDerivative() {
    this->treatCodeBlock(
        *this, BehaviourData::ComputeDerivative,
        &RungeKuttaDSLBase::computeThermodynamicForcesVariableModifier1, true,
        true);
  }  // end of treatDerivative

  void RungeKuttaDSLBase::treatEpsilon() {
    const auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    if (this->mb.hasParameter(h, "epsilon")) {
      this->throwRuntimeError("RungeKuttaDSLBase::treatEpsilon",
                              "value already specified.");
    }
    double epsilon;
    this->checkNotEndOfFile("RungeKuttaDSLBase::treatEpsilon",
                            "Cannot read epsilon value.");
    std::istringstream flux(current->value);
    flux >> epsilon;
    if ((flux.fail()) || (!flux.eof())) {
      this->throwRuntimeError("RungeKuttaDSLBase::treatEpsilon",
                              "Failed to read epsilon value.");
    }
    if (epsilon < 0) {
      this->throwRuntimeError("RungeKuttaDSLBase::treatEpsilon",
                              "Epsilon value must be positive.");
    }
    ++(this->current);
    this->readSpecifiedToken("RungeKuttaDSLBase::treatEpsilon", ";");
    this->mb.addParameter(h, VariableDescription("real", "epsilon", 1u, 0u),
                          BehaviourData::ALREADYREGISTRED);
    this->mb.setParameterDefaultValue(h, "epsilon", epsilon);
  }  // end of treatEpsilon


  void RungeKuttaDSLBase::treatStressErrorNormalizationFactor() {
    const auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    if (!this->mb.hasAttribute(BehaviourData::algorithm)) {
      this->throwRuntimeError(
          "RungeKuttaDSLBase::treatStressErrorNormalizationFactor",
          "the Runge-Kutta algorithm has not been set.");
    }
    const auto& algorithm =
        this->mb.getAttribute<std::string>(BehaviourData::algorithm);
    if (algorithm != "RungeKuttaCastem") {
      this->throwRuntimeError(
          "RungeKuttaDSLBase::treatStressErrorNormalizationFactor",
          "defining the normalization factor for the stress error is only "
          "meaningful if the Runge-Kutta algorithm has not been set.");
    }
    if (this->mb.hasParameter(h, "stress_error_normalization_factor")) {
      this->throwRuntimeError(
          "RungeKuttaDSLBase::treatStressErrorNormalizationFactor",
          "value already specified.");
    }
    double stress_error_normalization_factor;
    this->checkNotEndOfFile(
        "RungeKuttaDSLBase::treatStressErrorNormalizationFactor",
        "Cannot read the normalization factor for the stress error value.");
    std::istringstream flux(current->value);
    flux >> stress_error_normalization_factor;
    if ((flux.fail()) || (!flux.eof())) {
      this->throwRuntimeError(
          "RungeKuttaDSLBase::treatStressErrorNormalizationFactor",
          "Failed to read the normalization factor for the stress error "
          "value.");
    }
    if (stress_error_normalization_factor < 0) {
      this->throwRuntimeError(
          "RungeKuttaDSLBase::treatStressErrorNormalizationFactor",
          "StressErrorNormalizationFactor value must be positive.");
    }
    ++(this->current);
    this->readSpecifiedToken(
        "RungeKuttaDSLBase::treatStressErrorNormalizationFactor", ";");
    auto v = VariableDescription("stress", "stress_error_normalization_factor",
                                 1u, 0u);
    this->mb.addParameter(h, v);
    this->mb.setParameterDefaultValue(h, "stress_error_normalization_factor",
                                      stress_error_normalization_factor);
  }  // end of treatStressErrorNormalizationFactor

  void RungeKuttaDSLBase::treatMinimalTimeStep() {
    const Hypothesis h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    if (this->mb.hasParameter(h, "dtmin")) {
      this->throwRuntimeError("RungeKuttaDSLBase::treatMinimalTimeStep",
                              "value already specified.");
    }
    double dtmin;
    this->checkNotEndOfFile("RungeKuttaDSLBase::treatMinimalTimeStep",
                            "Cannot read dtmin value.");
    std::istringstream flux(current->value);
    flux >> dtmin;
    if ((flux.fail()) || (!flux.eof())) {
      this->throwRuntimeError("RungeKuttaDSLBase::treatMinimalTimeStep",
                              "Failed to read dtmin value.");
    }
    if (dtmin < 0) {
      this->throwRuntimeError("RungeKuttaDSLBase::treatMinimalTimeStep",
                              "MinimalTimeStep value must be positive.");
    }
    ++(this->current);
    this->readSpecifiedToken("RungeKuttaDSLBase::treatMinimalTimeStep", ";");
    this->mb.addParameter(h, VariableDescription("real", "dtmin", 1u, 0u),
                          BehaviourData::ALREADYREGISTRED);
    this->mb.setParameterDefaultValue(h, "dtmin", dtmin);
  }  // end of treatEpsilon

  void RungeKuttaDSLBase::setDefaultAlgorithm() {
    using ushort = unsigned short;
    this->mb.setAttribute(BehaviourData::algorithm,
                          std::string("RungeKutta5/4"), false);
    this->mb.setAttribute(BehaviourData::numberOfEvaluations, ushort(6u),
                          false);
  }  // end of setDefaultAlgorithm

  void RungeKuttaDSLBase::treatAlgorithm() {
    using ushort = unsigned short;
    const Hypothesis h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    this->checkNotEndOfFile("RungeKuttaDSLBase::treatAlgorithm",
                            "Cannot read algorithm name.");
    if (this->mb.hasAttribute(BehaviourData::algorithm)) {
      this->throwRuntimeError(
          "RungeKuttaDSLBase::treatAlgorithm",
          "algorith already specified. This may be the second "
          "time that the @Algorithm keyword is used, or the default "
          "algorithm was selected when registring a state variable "
          "(keyword @StateVariable)");
    }
    if (this->current->value == "euler") {
      this->mb.setAttribute(BehaviourData::algorithm, std::string("Euler"),
                            false);
      this->mb.setAttribute(BehaviourData::numberOfEvaluations, ushort(0u),
                            false);
    } else if (this->current->value == "rk2") {
      this->mb.setAttribute(BehaviourData::algorithm,
                            std::string("RungeKutta2"), false);
      this->mb.setAttribute(BehaviourData::numberOfEvaluations, ushort(1u),
                            false);
    } else if (this->current->value == "rk4") {
      this->mb.setAttribute(BehaviourData::algorithm,
                            std::string("RungeKutta4"), false);
      this->mb.setAttribute(BehaviourData::numberOfEvaluations, ushort(4u),
                            false);
    } else if (this->current->value == "rk42") {
      this->mb.setAttribute(BehaviourData::algorithm,
                            std::string("RungeKutta4/2"), false);
      this->mb.setAttribute(BehaviourData::numberOfEvaluations, ushort(4u),
                            false);
    } else if (this->current->value == "rk54") {
      this->setDefaultAlgorithm();
    } else if (this->current->value == "rkCastem") {
      const auto bt = this->mb.getBehaviourType();
      if ((bt != BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) &&
          (bt != BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR)) {
        this->throwRuntimeError("RungeKuttaDSLBase::treatAlgorithm",
                                "the rkCastem algorithm is only meaningful for "
                                "small strain and finite strain behaviours.");
      }
      this->reserveName("ra");
      this->reserveName("sqra");
      this->reserveName("errabs");
      this->reserveName("asig");
      this->reserveName("sigf");
      this->mb.addStaticVariable(
          h, StaticVariableDescription("real", "rkcastem_div", 0u, 7L));
      this->mb.addStaticVariable(
          h, StaticVariableDescription("real", "rkcastem_rmin", 0u, 0.7L));
      this->mb.addStaticVariable(
          h, StaticVariableDescription("real", "rkcastem_rmax", 0u, 1.3L));
      this->mb.addStaticVariable(
          h, StaticVariableDescription("real", "rkcastem_fac", 0u, 3L));
      this->mb.addStaticVariable(
          h, StaticVariableDescription("real", "rkcastem_borne", 0u, 2.L));
      this->mb.setAttribute(BehaviourData::algorithm,
                            std::string("RungeKuttaCastem"), false);
      this->mb.setAttribute(BehaviourData::numberOfEvaluations, ushort(5u),
                            false);
    } else {
      this->throwRuntimeError("RungeKuttaDSLBase::treatAlgorithm",
                              this->current->value +
                                  " is not a valid algorithm name"
                                  "Supported algorithms are : 'euler', 'rk2',"
                                  " 'rk4', 'rk42' , 'rk54' and 'rkCastem'");
    }
    ++this->current;
    this->readSpecifiedToken("RungeKuttaDSLBase::treatAlgorithm", ";");
  }

  void RungeKuttaDSLBase::completeVariableDeclaration() {
    const auto uh = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    if (getVerboseMode() >= VERBOSE_DEBUG) {
      getLogStream()
          << "RungeKuttaDSLBase::completeVariableDeclaration: begin\n";
    }
    BehaviourDSLCommon::completeVariableDeclaration();
    // driving variables
    for (const auto& v : this->mb.getMainVariables()) {
      const auto& dv = v.first;
      this->mb.addLocalVariable(
          uh, VariableDescription(dv.type, dv.name + "_", 1u, 0u));
      this->mb.addLocalVariable(
          uh,
          VariableDescription(SupportedTypes::getTimeDerivativeType(dv.type),
                              "d" + dv.name + "_", 1u, 0u));
    }
    // algorithm
    if (!this->mb.hasAttribute(BehaviourData::algorithm)) {
      this->setDefaultAlgorithm();
    }
    const auto n = this->mb.getAttribute<unsigned short>(
        BehaviourData::numberOfEvaluations);
    // some checks
    for (const auto& h : this->mb.getDistinctModellingHypotheses()) {
      const auto& d = this->mb.getBehaviourData(h);
      // creating local variables
      const auto& ivs = d.getStateVariables();
      const auto& evs = d.getExternalStateVariables();
      for (const auto& iv : ivs) {
        for (unsigned short i = 0u; i != n; ++i) {
          const auto currentVarName =
              "d" + iv.name + "_K" + std::to_string(i + 1u);
          if (getVerboseMode() >= VERBOSE_DEBUG) {
            auto& log = getLogStream();
            log << "registring variable '" << currentVarName << "'";
            if (h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
              log << " for default hypothesis\n";
            } else {
              log << " for the '" << ModellingHypothesis::toString(h)
                  << "' hypothesis\n";
            }
          }
          this->mb.addLocalVariable(
              h,
              VariableDescription(iv.type, currentVarName, iv.arraySize, 0u));
        }
        const auto currentVarName = iv.name + "_";
        if (getVerboseMode() >= VERBOSE_DEBUG) {
          auto& log = getLogStream();
          log << "registring variable '" << currentVarName << "'";
          if (h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
            log << " for default hypothesis\n";
          } else {
            log << " for the '" << ModellingHypothesis::toString(h)
                << "' hypothesis\n";
          }
        }
        this->mb.addLocalVariable(
            h, VariableDescription(iv.type, currentVarName, iv.arraySize, 0u));
      }
      for (const auto& ev : evs) {
        const auto currentVarName = ev.name + "_";
        if (getVerboseMode() >= VERBOSE_DEBUG) {
          auto& log = getLogStream();
          log << "registring variable '" << currentVarName << "'";
          if (h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
            log << " for default hypothesis\n";
          } else {
            log << " for the '" << ModellingHypothesis::toString(h)
                << "' hypothesis\n";
          }
        }
        this->mb.addLocalVariable(
            h, VariableDescription(ev.type, currentVarName, ev.arraySize, 0u));
      }
    }
    // declare the precision used by the algorithm
    if (!this->mb.hasParameter(uh, "epsilon")) {
      this->mb.addParameter(uh, VariableDescription("real", "epsilon", 1u, 0u),
                            BehaviourData::ALREADYREGISTRED);
      this->mb.setParameterDefaultValue(uh, "epsilon", 1.e-8);
    }
    if (this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) {
      auto D = VariableDescription("StiffnessTensor", "D", 1u, 0u);
      D.description =
          "stiffness tensor computed from elastic "
          "material properties";
      this->mb.addLocalVariable(uh, D, BehaviourData::ALREADYREGISTRED);
    }
    if (getVerboseMode() >= VERBOSE_DEBUG) {
      getLogStream() << "RungeKuttaDSLBase::completeVariableDeclaration: end\n";
    }
  }

  void RungeKuttaDSLBase::endsInputFileProcessing() {
    if (getVerboseMode() >= VERBOSE_DEBUG) {
      getLogStream() << "RungeKuttaDSLBase::endsInputFileProcessing: begin\n";
    }
    BehaviourDSLCommon::endsInputFileProcessing();
    const auto uh = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    CodeBlock
        ib;  // code inserted at before of the local variable initialisation
    CodeBlock
        ie;  // code inserted at the end of the local variable initialisation
    if (!this->mb.hasAttribute(BehaviourData::algorithm)) {
      this->setDefaultAlgorithm();
    }
    const auto& algorithm =
        this->mb.getAttribute<std::string>(BehaviourData::algorithm);
    if (algorithm == "RungeKuttaCastem") {
      const auto bt = this->mb.getBehaviourType();
      if ((bt != BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) &&
          (bt != BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR)) {
        this->throwRuntimeError("RungeKuttaDSLBase::endsInputFileProcessing",
                                "the rkCastem algorithm is only meaningful for "
                                "small strain and finite strain behaviours.");
      }
    }
    // some checks
    for (const auto& h : this->mb.getDistinctModellingHypotheses()) {
      const auto& d = this->mb.getBehaviourData(h);
      if (!this->mb.getMainVariables().empty()) {
        if (!d.hasCode(BehaviourData::ComputeFinalThermodynamicForces)) {
          this->throwRuntimeError(
              "RungeKuttaDSLBase::endsInputFileProcessing",
              "@ComputeFinalThermodynamicForces was not defined.");
        }
      }
      if (!d.hasCode(BehaviourData::ComputeDerivative)) {
        this->throwRuntimeError("RungeKuttaDSLBase::endsInputFileProcessing",
                                "@Derivative was not defined.");
      }
      auto uvs = d.getCodeBlock(BehaviourData::ComputeDerivative).members;
      if (d.hasCode(BehaviourData::ComputeThermodynamicForces)) {
        const auto& uvs2 =
            d.getCodeBlock(BehaviourData::ComputeThermodynamicForces).members;
        uvs.insert(uvs2.begin(), uvs2.end());
      }
      CodeBlock icb;  // code inserted at the beginning of the local variable
                      // initialisation
      // creating local variables
      const auto& ivs = d.getStateVariables();
      const auto& evs = d.getExternalStateVariables();
      for (const auto& iv : ivs) {
        if (uvs.find(iv.name) != uvs.end()) {
          const auto currentVarName = iv.name + "_";
          if (this->mb.useDynamicallyAllocatedVector(iv.arraySize)) {
            icb.code += "this->" + currentVarName + ".resize(" +
                        std::to_string(iv.arraySize) + ");\n";
          }
          if ((algorithm != "RungeKutta4/2") &&
              (algorithm != "RungeKutta5/4")) {
            icb.code +=
                "this->" + currentVarName + " = this->" + iv.name + ";\n";
          }
        }
      }
      // driving variables
      if ((algorithm != "RungeKutta4/2") && (algorithm != "RungeKutta5/4")) {
        for (const auto& vm : this->mb.getMainVariables()) {
          const auto& dv = vm.first;
          if (uvs.find(dv.name) != uvs.end()) {
            ib.code += "this->" + dv.name + "_ = this->" + dv.name + ";\n";
          }
        }
      }
      for (const auto& ev : evs) {
        if (uvs.find(ev.name) != uvs.end()) {
          const auto currentVarName = ev.name + "_";
          if (this->mb.useDynamicallyAllocatedVector(ev.arraySize)) {
            icb.code += "this->" + currentVarName + ".resize(" +
                        std::to_string(ev.arraySize) + ");\n";
          }
          if ((algorithm != "RungeKutta4/2") &&
              (algorithm != "RungeKutta5/4")) {
            icb.code +=
                "this->" + currentVarName + " = this->" + ev.name + ";\n";
          }
        }
      }
      this->mb.setCode(h, BehaviourData::BeforeInitializeLocalVariables, icb,
                       BehaviourData::CREATEORAPPEND, BehaviourData::AT_END);
    }
    // create the compute final stress code is necessary
    this->setComputeFinalThermodynamicForcesFromComputeFinalThermodynamicForcesCandidateIfNecessary();
    // minimal time step
    if (this->mb.hasParameter(uh, "dtmin")) {
      ib.code += "if(this->dt < " + this->mb.getClassName() + "::dtmin){\n";
      ib.code += "this->dt = " + this->mb.getClassName() + "::dtmin;\n";
      ib.code += "}\n";
    } else {
      ib.code += "if(this->dt < 100 * std::numeric_limits<time>::min()){\n";
      ib.code += "string msg(\"" + this->mb.getClassName() +
                 "::" + this->mb.getClassName() + "\");\n";
      ib.code += "msg += \"time step too small.\";\n";
      ib.code += "throw(runtime_error(msg));\n";
      ib.code += "}\n";
    }
    this->mb.setCode(uh, BehaviourData::BeforeInitializeLocalVariables, ib,
                     BehaviourData::CREATEORAPPEND,
                     BehaviourData::AT_BEGINNING);
    // part introduced at the end of the initialize local variables
    for (const auto& vm : this->mb.getMainVariables()) {
      const auto& dv = vm.first;
      if (Gradient::isIncrementKnown(dv)) {
        ie.code += "this->d" + dv.name + "_ = (this->d" + dv.name +
                   ") / (this->dt);\n";
      } else {
        ie.code += "this->d" + dv.name + "_ = (this->" + dv.name + "1-this->" +
                   dv.name + "0)/(this->dt);\n";
      }
    }
    this->mb.setCode(uh, BehaviourData::AfterInitializeLocalVariables, ie,
                     BehaviourData::CREATEORAPPEND, BehaviourData::BODY);
    // minimal tangent operator if mandatory
    this->setMinimalTangentOperator();
    if (getVerboseMode() >= VERBOSE_DEBUG) {
      getLogStream() << "RungeKuttaDSLBase::endsInputFileProcessing: end\n";
    }
  }  // end of endsInputFileProcessing

  void RungeKuttaDSLBase::writeBehaviourParserSpecificIncludes(
      std::ostream& os) const {
    auto b1 = false;
    auto b2 = false;
    this->checkBehaviourFile(os);
    os << "#include\"TFEL/Math/General/Abs.hxx\"\n\n";
    this->mb.requiresTVectorOrVectorIncludes(b1, b2);
    if (b1) {
      os << "#include\"TFEL/Math/tvector.hxx\"\n";
    }
    if (b2) {
      os << "#include\"TFEL/Math/vector.hxx\"\n";
    }
  }

  void RungeKuttaDSLBase::writeBehaviourLocalVariablesInitialisation(
      std::ostream& os, const Hypothesis h) const {
    using Modifier = std::function<std::string(const MaterialPropertyInput&)>;
    if (this->mb.getAttribute(BehaviourDescription::computesStiffnessTensor,
                              false)) {
      os << "// the stiffness tensor at the beginning of the time step\n";
      Modifier bts = [this](const MaterialPropertyInput& i) {
        if ((i.category == MaterialPropertyInput::TEMPERATURE) ||
            (i.category ==
             MaterialPropertyInput::AUXILIARYSTATEVARIABLEFROMEXTERNALMODEL) ||
            (i.category == MaterialPropertyInput::EXTERNALSTATEVARIABLE) ||
            (i.category == MaterialPropertyInput::MATERIALPROPERTY) ||
            (i.category == MaterialPropertyInput::PARAMETER)) {
          return "this->" + i.name;
        } else if (i.category == MaterialPropertyInput::STATICVARIABLE) {
          return this->mb.getClassName() + "::" + i.name;
        } else {
          this->throwRuntimeError(
              "RungeKuttaDSLBase::"
              "writeBehaviourLocalVariablesInitialisation",
              "unsupported input type for variable '" + i.name + "'");
        }
      };
      this->writeStiffnessTensorComputation(os, "this->D", bts);
    }
    BehaviourDSLCommon::writeBehaviourLocalVariablesInitialisation(os, h);
  }  // end of writeBehaviourLocalVariablesInitialisation

  void RungeKuttaDSLBase::writeBehaviourParserSpecificTypedefs(
      std::ostream& os) const {
    this->checkBehaviourFile(os);
  }

  void RungeKuttaDSLBase::writeBehaviourParserSpecificMembers(
      std::ostream& os, const Hypothesis h) const {
    this->checkBehaviourFile(os);
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "bool\ncomputeThermodynamicForces(){\n"
         << "using namespace std;\n"
         << "using namespace tfel::math;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
      os << this->mb.getCode(h, BehaviourData::ComputeThermodynamicForces)
         << '\n'
         << "return true;\n"
         << "} // end of " << this->mb.getClassName()
         << "::computeThermodynamicForces\n\n";
    }
    if (!this->mb.getMainVariables().empty()) {
      os << "bool\ncomputeFinalThermodynamicForces(){\n"
         << "using namespace std;\n"
         << "using namespace tfel::math;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
      os << this->mb.getCode(h, BehaviourData::ComputeFinalThermodynamicForces)
         << '\n'
         << "return true;\n"
         << "} // end of " << this->mb.getClassName()
         << "::computeFinalThermodynamicForces\n\n";
    }
    os << "bool\ncomputeDerivative(){\n"
       << "using namespace std;\n"
       << "using namespace tfel::math;\n";
    writeMaterialLaws(os, this->mb.getMaterialLaws());
    os << this->mb.getCode(h, BehaviourData::ComputeDerivative) << '\n'
       << "return true;\n"
       << "} // end of " << this->mb.getClassName()
       << "::computeDerivative\n\n";
  }  // end of writeBehaviourParserSpecificMembers

  void RungeKuttaDSLBase::writeBehaviourUpdateStateVariables(
      std::ostream&, const Hypothesis) const {
    // Disabled (makes no sense for this parser)
  }  // end of writeBehaviourUpdateStateVariables

  void RungeKuttaDSLBase::writeBehaviourUpdateAuxiliaryStateVariables(
      std::ostream& os, const Hypothesis h) const {
    if (this->mb.hasCode(h, BehaviourData::UpdateAuxiliaryStateVariables)) {
      os << "/*!\n"
         << "* \\brief Update auxiliary state variables at end of integration\n"
         << "*/\n"
         << "void\n"
         << "updateAuxiliaryStateVariables(const real dt_)"
         << "{\n"
         << "static_cast<void>(dt_);\n"
         << "using namespace std;\n"
         << "using namespace tfel::math;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
      os << this->mb.getCode(h, BehaviourData::UpdateAuxiliaryStateVariables)
         << '\n'
         << "}\n\n";
    }
  }  // end of  RungeKuttaDSLBase::writeBehaviourUpdateAuxiliaryStateVariables

  void RungeKuttaDSLBase::writeBehaviourEulerIntegrator(
      std::ostream& os, const Hypothesis h) const {
    const auto btype = this->mb.getBehaviourTypeFlag();
    const auto& d = this->mb.getBehaviourData(h);
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "this->computeThermodynamicForces();\n";
    }
    os << "if(!this->computeDerivative()){\n";
    if (this->mb.useQt()) {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType,use_qt>::FAILURE;\n";
    } else {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType,false>::FAILURE;\n";
    }
    os << "}\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->" << v.name << " += "
         << "this->dt*(this->d" << v.name << ");\n";
    }
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      os << "// updating stiffness tensor at the end of the time step\n";
      auto md =
          modifyVariableForStiffnessTensorComputation2(this->mb.getClassName());
      this->writeStiffnessTensorComputation(os, "this->D", md);
    }
    if (!this->mb.getMainVariables().empty()) {
      os << "// update the thermodynamic forces\n"
         << "this->computeFinalThermodynamicForces();\n";
    }
    if (d.hasCode(BehaviourData::UpdateAuxiliaryStateVariables)) {
      os << "this->updateAuxiliaryStateVariables(this->dt);\n";
    }
  }  // end of writeBehaviourEulerIntegrator

  void RungeKuttaDSLBase::writeBehaviourRK2Integrator(
      std::ostream& os, const Hypothesis h) const {
    const auto btype = this->mb.getBehaviourTypeFlag();
    const auto& d = this->mb.getBehaviourData(h);
    auto uvs = d.getCodeBlock(BehaviourData::ComputeDerivative).members;
    if (d.hasCode(BehaviourData::ComputeThermodynamicForces)) {
      const auto& uvs2 =
          d.getCodeBlock(BehaviourData::ComputeThermodynamicForces).members;
      uvs.insert(uvs2.begin(), uvs2.end());
    }
    os << "constexpr auto cste1_2 = NumericType{1}/NumericType{2};\n"
       << "// Compute K1's values\n";
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "this->computeThermodynamicForces();\n";
    }
    os << "if(!this->computeDerivative()){\n";
    if (this->mb.useQt()) {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType,use_qt>::FAILURE;\n";
    } else {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType,false>::FAILURE;\n";
    }
    os << "}\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K1 = (this->dt)*(this->d" << v.name
         << ");\n";
    }
    for (const auto& iv : d.getStateVariables()) {
      if (uvs.find(iv.name) != uvs.end()) {
        os << "this->" << iv.name << "_ += cste1_2*(this->d" << iv.name
           << "_K1);\n";
      }
    }
    writeExternalVariablesCurrentValues2(os, this->mb, h, "cste1_2");
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      os << "// updating the stiffness tensor\n";
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "this->computeThermodynamicForces();\n";
    }
    os << "if(!this->computeDerivative()){\n";
    if (this->mb.useQt()) {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType,use_qt>::FAILURE;\n";
    } else {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType,false>::FAILURE;\n";
    }
    os << "}\n";
    os << "// Final Step\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->" << v.name << " += "
         << "this->dt*(this->d" << v.name << ");\n";
    }
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      os << "// updating stiffness tensor at the end of the time step\n";
      auto m =
          modifyVariableForStiffnessTensorComputation2(this->mb.getClassName());
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    if (!this->mb.getMainVariables().empty()) {
      os << "// update the thermodynamic forces\n"
         << "this->computeFinalThermodynamicForces();\n";
    }
    if (d.hasCode(BehaviourData::UpdateAuxiliaryStateVariables)) {
      os << "this->updateAuxiliaryStateVariables(this->dt);\n";
    }
  }  // end of writeBehaviourRK2Integrator

  void RungeKuttaDSLBase::writeBehaviourRK54Integrator(
      std::ostream& os, const Hypothesis h) const {
    auto get_enf = [](const VariableDescription& v) {
      return v.getAttribute<std::string>(
          VariableDescription::errorNormalisationFactor);
    };
    const auto& d = this->mb.getBehaviourData(h);
    auto shallUpdateInternalStateValues = [this, d, h] {
      const auto uvs = getVariablesUsedDuringIntegration(this->mb, h);
      for (const auto& v : d.getIntegrationVariables()) {
        if (uvs.find(v.name) != uvs.end()) {
          return true;
        }
      }
      return false;
    }();
    auto shallUpdateExternalStateValues = [this, d, h] {
      const auto uvs = getVariablesUsedDuringIntegration(this->mb, h);
      for (const auto& mv : this->mb.getMainVariables()) {
        const auto& dv = mv.first;
        if (uvs.find(dv.name) != uvs.end()) {
          return true;
        }
      }
      for (const auto& v : d.getExternalStateVariables()) {
        if (uvs.find(v.name) != uvs.end()) {
          return true;
        }
      }
      return false;
    }();
    //! all registred variables used in ComputeDerivatives and
    //! ComputeThermodynamicForces blocks
    auto uvs = d.getCodeBlock(BehaviourData::ComputeDerivative).members;
    if (d.hasCode(BehaviourData::ComputeThermodynamicForces)) {
      const auto& uvs2 =
          d.getCodeBlock(BehaviourData::ComputeThermodynamicForces).members;
      uvs.insert(uvs2.begin(), uvs2.end());
    }
    ErrorEvaluation eev;
    auto svsize = d.getStateVariables().getTypeSize();
    if (svsize.getValueForDimension(1) >= 20) {
      eev = MAXIMUMVALUEERROREVALUATION;
    } else {
      eev = ERRORSUMMATIONEVALUATION;
    }
    if (shallUpdateExternalStateValues) {
      os << "constexpr auto cste1_2         = "
            "NumericType{1}/NumericType{2};\n"
         << "constexpr auto cste3_8         = "
            "NumericType{3}/NumericType{8};\n"
         << "constexpr auto cste12_13       = "
            "NumericType(12)/NumericType(13);\n";
    }
    if (shallUpdateInternalStateValues) {
      os << "constexpr auto cste3544_2565   = "
            "NumericType(3544)/NumericType(2565);\n"
         << "constexpr auto cste11_40       = "
            "NumericType(11)/NumericType(40);\n"
         << "constexpr auto cste1859_4104   = "
            "NumericType(1859)/NumericType(4104);\n"
         << "constexpr auto cste8_27        = "
            "NumericType(8)/NumericType(27);\n"
         << "constexpr auto cste845_4104    = "
            "NumericType(845)/NumericType(4104);\n"
         << "constexpr auto cste3680_513    = "
            "NumericType(3680)/NumericType(513);\n"
         << "constexpr auto cste439_216     = "
            "NumericType(439)/NumericType(216);\n"
         << "constexpr auto cste7296_2197   = "
            "NumericType(7296)/NumericType(2197);\n"
         << "constexpr auto cste7200_2197   = "
            "NumericType(7200)/NumericType(2197);\n"
         << "constexpr auto cste3_32        = "
            "NumericType{3}/NumericType{32};\n"
         << "constexpr auto cste1932_2197   = "
            "NumericType(1932)/NumericType(2197);\n";
    }
    os << "constexpr auto cste1_4         = "
          "NumericType{1}/NumericType{4};\n"
       << "constexpr auto cste16_135      = "
          "NumericType(16)/NumericType(135);\n"
       << "constexpr auto cste6656_12825  = "
          "NumericType(6656)/NumericType(12825);\n"
       << "constexpr auto cste28561_56430 = "
          "NumericType(28561)/NumericType(56430);\n"
       << "constexpr auto cste9_50        = "
          "NumericType(9)/NumericType(50);\n"
       << "constexpr auto cste2_55        = "
          "NumericType(2)/NumericType(55);\n"
       << "constexpr auto cste1_360       = "
          "NumericType(1)/NumericType(360);\n"
       << "constexpr auto cste128_4275    = "
          "NumericType(128)/NumericType(4275);\n"
       << "constexpr auto cste2197_75240  = "
          "NumericType(2197)/NumericType(75240);\n"
       << "constexpr auto cste1_50        = "
          "NumericType(1)/NumericType(50);\n"
       << "time t      = time(0);\n"
       << "time dt_    = this->dt;\n"
       << "time dtprec = 100* (this->dt) * "
          "std::numeric_limits<NumericType>::epsilon();\n"
       << "auto error = NumericType{};\n"
       << "bool converged = false;\n";
    if (getDebugMode()) {
      os << "cout << endl << \"" << this->mb.getClassName()
         << "::integrate() : beginning of resolution\" << endl;\n";
    }
    os << "while(!converged){\n";
    if (getDebugMode()) {
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : from \" << t <<  \" to \" << t+dt_ << \" with "
            "time step \" << dt_ << endl;\n";
    }
    os << "bool failed = false;\n";
    os << "// Compute K1's values\n";
    writeExternalVariablesCurrentValues(os, this->mb, h, "0");
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      os << "// updating the stiffness tensor\n";
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ = this->" << v.name << ";\n";
      }
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "failed = !this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n";
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K1's stress\" << endl;\n";
      os << "}\n";
    }
    os << "if(!failed){\n";
    os << "failed = !this->computeDerivative();\n";
    if (getDebugMode()) {
      os << "if(failed){\n";
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K1's derivative\" << "
            "endl;\n";
      os << "}\n";
    }
    os << "if(!failed){\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K1 = (dt_)*(this->d" << v.name << ");\n";
    }
    os << "}\n";
    os << "}\n\n";
    os << "if(!failed){\n";
    os << "// Compute K2's values\n";
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ += cste1_4*(this->d" << v.name
           << "_K1);\n";
      }
    }
    writeExternalVariablesCurrentValues(os, this->mb, h, "cste1_4");
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating the stiffness tensor\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    os << "// update the thermodynamic forces\n";
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "failed = !this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n";
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K2's stress\" << endl;\n";
      os << "}\n";
    }
    os << "if(!failed){\n";
    os << "failed = !this->computeDerivative();\n";
    if (getDebugMode()) {
      os << "if(failed){\n";
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K2's derivative\" << "
            "endl;\n";
      os << "}\n";
    }
    os << "if(!failed){\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K2 = (dt_)*(this->d" << v.name << ");\n";
    }
    os << "}\n"
       << "}\n"
       << "}\n\n"
       << "if(!failed){\n"
       << "// Compute K3's values\n";
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ = this->" << v.name
           << "+cste3_32*(this->d" << v.name << "_K1+3*(this->d" << v.name
           << "_K2));\n";
      }
    }
    writeExternalVariablesCurrentValues(os, this->mb, h, "cste3_8");
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating the stiffness tensor\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "// update the thermodynamic forces\n"
         << "failed = !this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n";
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K3's stress\" << endl;\n";
      os << "}\n";
    }
    os << "if(!failed){\n";
    os << "failed = !this->computeDerivative();\n";
    if (getDebugMode()) {
      os << "if(failed){\n";
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K3's derivative\" << "
            "endl;\n";
      os << "}\n";
    }
    os << "if(!failed){\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K3 = (dt_)*(this->d" << v.name << ");\n";
    }
    os << "}\n";
    os << "}\n";
    os << "}\n\n";

    os << "if(!failed){\n";
    os << "// Compute K4's values\n";
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ = this->" << v.name
           << "+cste1932_2197*(this->d" << v.name << "_K1)"
           << "-cste7200_2197*(this->d" << v.name << "_K2)"
           << "+cste7296_2197*(this->d" << v.name << "_K3);\n";
      }
    }
    writeExternalVariablesCurrentValues(os, this->mb, h, "cste12_13");
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating the stiffness tensor\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    os << "// update the thermodynamic forces\n";
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "failed = !this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n";
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K4's stress\" << endl;\n";
      os << "}\n";
    }
    os << "if(!failed){\n";
    os << "failed = !this->computeDerivative();\n";
    if (getDebugMode()) {
      os << "if(failed){\n";
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K4's derivatives\" << "
            "endl;\n";
      os << "}\n";
    }
    os << "if(!failed){\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K4 = (dt_)*(this->d" << v.name << ");\n";
    }
    os << "}\n"
       << "}\n"
       << "}\n\n"
       << "if(!failed){\n"
       << "// Compute K5's values\n";
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ = this->" << v.name
           << "+cste439_216*(this->d" << v.name << "_K1)"
           << "-8*(this->d" << v.name << "_K2)"
           << "+cste3680_513*(this->d" << v.name << "_K3)"
           << "-cste845_4104*(this->d" << v.name << "_K4);\n";
      }
    }
    writeExternalVariablesCurrentValues(os, this->mb, h, "1");
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating the stiffness tensor\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    os << "// update the thermodynamic forces\n";
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "failed = !this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n";
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K5's stress\" << endl;\n";
      os << "}\n";
    }
    os << "if(!failed){\n";
    os << "failed = !this->computeDerivative();\n";
    if (getDebugMode()) {
      os << "if(failed){\n";
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K5's derivatives\" << "
            "endl;\n";
      os << "}\n";
    }
    os << "if(!failed){\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K5 = (dt_)*(this->d" << v.name << ");\n";
    }
    os << "}\n"
       << "}\n"
       << "}\n\n"
       << "if(!failed){\n"
       << "// Compute K6's values\n";
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ = this->" << v.name
           << "-cste8_27*(this->d" << v.name << "_K1)"
           << "+2*(this->d" << v.name << "_K2)"
           << "-cste3544_2565*(this->d" << v.name << "_K3)"
           << "+cste1859_4104*(this->d" << v.name << "_K4)"
           << "-cste11_40*(this->d" << v.name << "_K5);\n";
      }
    }
    writeExternalVariablesCurrentValues(os, this->mb, h, "cste1_2");
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating the stiffness tensor\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "// update the thermodynamic forces\n"
         << "failed = !this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K6's stress\" << endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n"
       << "failed = !this->computeDerivative();\n";
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K6's derivatives\" << "
            "endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K6 = (dt_)*(this->d" << v.name << ");\n";
    }
    os << "}\n"
       << "}\n"
       << "}\n\n"
       << "if(!failed){\n"
       << "// Computing the error\n";
    if (eev == ERRORSUMMATIONEVALUATION) {
      for (auto p = std::begin(d.getStateVariables());
           p != std::end(d.getStateVariables()); ++p) {
        const auto& v = *p;
        if (v.arraySize == 1u) {
          if (p == d.getStateVariables().begin()) {
            os << "error  = ";
          } else {
            os << "error += ";
          }
          if (v.hasAttribute(VariableDescription::errorNormalisationFactor)) {
            os << "(";
          }
          os << "tfel::math::base_type_cast(tfel::math::abs("
             << "cste1_360*(this->d" << v.name << "_K1)"
             << "-cste128_4275*(this->d" << v.name << "_K3)"
             << "-cste2197_75240*(this->d" << v.name << "_K4)"
             << "+cste1_50*(this->d" << v.name << "_K5)"
             << "+cste2_55*(this->d" << v.name << "_K6)))";
          if (v.hasAttribute(VariableDescription::errorNormalisationFactor)) {
            os << "))/(" << get_enf(v) << ")";
          }
          os << ";\n";
        } else {
          if (this->mb.useDynamicallyAllocatedVector(v.arraySize)) {
            if (p == d.getStateVariables().begin()) {
              os << "error  = NumericType(0);\n";
            }
            os << "for(unsigned short idx=0;idx!=" << v.arraySize
               << ";++idx){\n";
            os << "error += ";
            if (v.hasAttribute(VariableDescription::errorNormalisationFactor)) {
              os << "(";
            }
            os << "tfel::math::base_type_cast(tfel::math::abs(";
            os << "cste1_360*(this->d" << v.name << "_K1[idx])"
               << "-cste128_4275*(this->d" << v.name << "_K3[idx])"
               << "-cste2197_75240*(this->d" << v.name << "_K4[idx])"
               << "+cste1_50*(this->d" << v.name << "_K5[idx])"
               << "+cste2_55*(this->d" << v.name << "_K6[idx])))";
            if (v.hasAttribute(VariableDescription::errorNormalisationFactor)) {
              os << ")/(" << get_enf(v) << ")";
            }
            os << ";\n";
            os << "}\n";
          } else {
            for (unsigned short i = 0; i != v.arraySize; ++i) {
              if ((p == d.getStateVariables().begin()) && (i == 0)) {
                os << "error  = ";
              } else {
                os << "error += ";
              }
              if (v.hasAttribute(
                      VariableDescription::errorNormalisationFactor)) {
                os << "(";
              }
              os << "tfel::math::base_type_case(tfel::math::abs("
                 << "cste1_360*(this->d" << v.name << "_K1[" << i << "])"
                 << "-cste128_4275*(this->d" << v.name << "_K3[" << i << "])"
                 << "-cste2197_75240*(this->d" << v.name << "_K4[" << i << "])"
                 << "+cste1_50*(this->d" << v.name << "_K5[" << i << "])"
                 << "+cste2_55*(this->d" << v.name << "_K6[" << i << "])))";
              if (v.hasAttribute(
                      VariableDescription::errorNormalisationFactor)) {
                os << ")/(" << get_enf(v) << ")";
              }
              os << ";\n";
            }
          }
        }
      }
      os << "error/=" << svsize << ";\n";
    } else if (eev == MAXIMUMVALUEERROREVALUATION) {
      os << "error  = NumericType(0);\n"
         << "auto rk_update_error = [&error](const auto rk_error){\n"
         << "if(!ieee754::isfinite(error)){return;}\n"
         << "if(!ieee754::isfinite(rk_error)){\n"
         << "error = tfel::math::base_type_cast(rk_error);\n"
         << "return;\n"
         << "}\n"
         << "error = std::max(error, tfel::math::base_type_cast(rk_error));\n"
         << "};\n";
      for (const auto& v : d.getStateVariables()) {
        if (v.arraySize == 1u) {
          os << "rk_update_error(";
          if (v.hasAttribute(VariableDescription::errorNormalisationFactor)) {
            os << "(";
          }
          os << "tfel::math::abs("
             << "cste1_360*(this->d" << v.name << "_K1)"
             << "-cste128_4275*(this->d" << v.name << "_K3)"
             << "-cste2197_75240*(this->d" << v.name << "_K4)"
             << "+cste1_50*(this->d" << v.name << "_K5)"
             << "+cste2_55*(this->d" << v.name << "_K6))";
          if (v.hasAttribute(VariableDescription::errorNormalisationFactor)) {
            os << ")/(" << get_enf(v) << ")";
          }
          os << ");\n";
        } else {
          if (this->mb.useDynamicallyAllocatedVector(v.arraySize)) {
            os << "for(unsigned short idx=0;idx!=" << v.arraySize
               << ";++idx){\n";
            os << "rk_update_error(";
            if (v.hasAttribute(VariableDescription::errorNormalisationFactor)) {
              os << "(";
            }
            os << "tfel::math::abs("
               << "cste1_360*(this->d" << v.name << "_K1[idx])"
               << "-cste128_4275*(this->d" << v.name << "_K3[idx])"
               << "-cste2197_75240*(this->d" << v.name << "_K4[idx])"
               << "+cste1_50*(this->d" << v.name << "_K5[idx])"
               << "+cste2_55*(this->d" << v.name << "_K6[idx]))";
            if (v.hasAttribute(VariableDescription::errorNormalisationFactor)) {
              os << ")/(" << get_enf(v) << ")";
            }
            os << ");\n";
            os << "}\n";
          } else {
            for (unsigned short i = 0; i != v.arraySize; ++i) {
              os << "rk_update_error(";
              if (v.hasAttribute(
                      VariableDescription::errorNormalisationFactor)) {
                os << "(";
              }
              os << "tfel::math::abs("
                 << "cste1_360*(this->d" << v.name << "_K1[" << i << "])"
                 << "-cste128_4275*(this->d" << v.name << "_K3[" << i << "])"
                 << "-cste2197_75240*(this->d" << v.name << "_K4[" << i << "])"
                 << "+cste1_50*(this->d" << v.name << "_K5[" << i << "])"
                 << "+cste2_55*(this->d" << v.name << "_K6[" << i << "]))";
              if (v.hasAttribute(
                      VariableDescription::errorNormalisationFactor)) {
                os << ")/(" << get_enf(v) << ")";
              }
              os << ");\n";
            }
          }
        }
      }
    } else {
      this->throwRuntimeError("RungeKuttaDSLBase::writeBehaviourRK54Integrator",
                              "internal error, unsupported error evaluation.");
    }
    os << "if(!ieee754::isfinite(error)){\n"
       << "throw(tfel::material::DivergenceException());\n"
       << "}\n";
    if (getDebugMode()) {
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : error \" << error << endl;\n";
    }
    os << "// test for convergence\n"
       << "if(error<this->epsilon){\n"
       << "// Final Step\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->" << v.name << " += cste16_135*(this->d" << v.name << "_K1)"
         << "+cste6656_12825*(this->d" << v.name << "_K3)"
         << "+cste28561_56430*(this->d" << v.name << "_K4)"
         << "-cste9_50*(this->d" << v.name << "_K5)"
         << "+cste2_55*(this->d" << v.name << "_K6);\n";
    }
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating stiffness tensor at the end of the time step\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    if (!this->mb.getMainVariables().empty()) {
      os << "// update the thermodynamic forces\n"
         << "this->computeFinalThermodynamicForces();\n";
    }
    if (d.hasCode(BehaviourData::UpdateAuxiliaryStateVariables)) {
      os << "this->updateAuxiliaryStateVariables(dt_);\n";
    }
    os << "t += dt_;\n"
       << "if(tfel::math::abs(this->dt-t)<dtprec){\n"
       << "converged=true;\n"
       << "}\n"
       << "}\n"
       << "if(!converged){\n"
       << "// time multiplier\n"
       << "real corrector;\n"
       << "if(error < 100*std::numeric_limits<real>::min()){\n"
       << "corrector=real(10);\n"
       << "} else {\n"
       << "corrector = 0.8*pow(this->epsilon/error,0.2);\n"
       << "}\n"
       << "if(corrector<real(0.1f)){\n";
    if (getDebugMode()) {
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : reducing time step by a factor 10\" << endl;\n";
    }
    os << "dt_ *= real(0.1f);\n";
    os << "} else if(corrector>real(10)){\n";
    if (getDebugMode()) {
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : increasing time step by a factor 10\" << endl;\n";
    }
    os << "dt_ *= real(10);\n";
    os << "} else {\n";
    if (getDebugMode()) {
      os << "if(corrector<1){\n";
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : reducing time step by a factor \"   << corrector "
            "<< endl;\n";
      os << "} else {\n";
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : increasing time step by a factor \" << corrector "
            "<< endl;\n";
      os << "}\n";
    }
    os << "dt_ *= corrector;\n"
       << "}\n"
       << "if(dt_<dtprec){\n"
       << "throw(tfel::material::DivergenceException());\n"
       << "}\n"
       << "if((tfel::math::abs(this->dt-t-dt_)<2*dtprec)||(t+dt_>this->dt)){\n"
       << "dt_=this->dt-t;\n"
       << "}\n"
       << "}\n"
       << "} else {\n";
    if (getDebugMode()) {
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failure detected, reducing time step by a factor "
            "10\" << endl;";
    }
    os << "// failed is true\n"
       << "dt_ *= real(0.1f);\n"
       << "if(dt_<dtprec){\n"
       << "throw(tfel::material::DivergenceException());\n"
       << "}\n"
       << "}\n"
       << "}\n";
  }  // end of writeBehaviourRK54Integrator

  // Older versions of TFEL assumed that a variable called young was always
  // defined to define the factor used to normalize the stress error in
  // the rkCastem algorithm.
  //
  // For backward compatibility, the findErrorNormalizationFactor looks if such
  // a variable exists and makes some consistency checks.
  //
  // Otherwise, one checks if the stiffness matrix is computed.
  //
  // see Issue 183 for details (https://github.com/thelfer/tfel/issues/183)
  static std::string findStressErrorNormalizationFactor(
      const BehaviourDescription& bd) {
    using tfel::glossary::Glossary;
    //
    auto checkVariableDefinition = [](const auto& v) {
      if (v.getTypeFlag() != SupportedTypes::SCALAR) {
        tfel::raise("findStressErrorNormalizationFactor: the `" + v.name +
                    "` variable is not "
                    "defined as a scalar.");
      }
      if ((getPedanticMode()) && (v.type != "stress")) {
        auto& log = getLogStream();
        log << "findStressErrorNormalizationFactor: "
            << "inconsistent type for variable '" << v.name << "' ('" << v.type
            << "' vs 'stress')\n";
      }
    };
    auto testInGivenVariableCategory = [&bd, &checkVariableDefinition](
                                           const std::string& k,
                                           const std::string& n,
                                           const std::string& e) {
      const auto exists = bd.checkVariableExistence(n, k, false);
      if (exists.first) {
        if (!exists.second) {
          tfel::raise("findStressErrorNormalizationFactor: the `" + n +
                      "` variable is not "
                      "defined for all modelling hypotheses as a '" +
                      k + "'.");
        }
        const auto& mh = bd.getDistinctModellingHypotheses();
        for (const auto& h : mh) {
          const auto& v = bd.getBehaviourData(h).getVariables(k).getVariable(n);
          checkVariableDefinition(v);
          if (!e.empty()) {
            if ((getPedanticMode()) && (v.getExternalName() != e)) {
              auto& log = getLogStream();
              log << "findStressErrorNormalizationFactor: "
                  << "inconsistent external name for variable '" << v.name
                  << "' ('" << v.type << "' vs '" << e << "')\n";
            }
          }
        }
        return true;
      }
      return false;
    };
    auto checkVariable = [bd, testInGivenVariableCategory,
                          checkVariableDefinition](const std::string& n) {
      const auto exists = bd.checkVariableExistence(n);
      if (exists.first) {
        if (!exists.second) {
          tfel::raise(
              "findStressErrorNormalizationFactor: the `" + n +
              "` variable is not defined for all modelling hypotheses.");
        }
        if (testInGivenVariableCategory("MaterialProperty", n,
                                        Glossary::YoungModulus)) {
          return "this->"+n;
        }
        if (testInGivenVariableCategory("Parameter", n,
                                        Glossary::YoungModulus)) {
          return "this->"+n;
        }
        tfel::raise(
            "findStressErrorNormalizationFactor: the `" + n +
            "` variable is not defined as a material property or a parameter");
      }
      // look if young is defined as a local variable
      const auto& mh = bd.getDistinctModellingHypotheses();
      const auto found_as_local_variable =
          bd.isLocalVariableName(*(mh.begin()), n);
      for (const auto& h : mh) {
        const auto is_local_variable = bd.isLocalVariableName(h, n);
        if (found_as_local_variable != is_local_variable) {
          tfel::raise("findStressErrorNormalizationFactor: the `" + n +
                      "` variable is not defined as a local variable in all "
                      "modelling hypotheses");
        }
        if (is_local_variable) {
          const auto& v =
              bd.getBehaviourData(h).getLocalVariables().getVariable(n);
          checkVariableDefinition(v);
        }
      }
      if (found_as_local_variable) {
        return "this->"+n;
      }
      // look if young is defined as a static variable
      const auto found_as_static_variable =
          bd.isStaticVariableName(*(mh.begin()), n);
      for (const auto& h : mh) {
        const auto is_static_variable = bd.isStaticVariableName(h, n);
        if (found_as_static_variable != is_static_variable) {
          tfel::raise("findStressErrorNormalizationFactor: the `" + n +
                      "` variable is not defined as a static variable in all "
                      "modelling hypotheses");
        }
        if (is_static_variable) {
          const auto& v =
              bd.getBehaviourData(h).getStaticVariables().get(n);
          checkVariableDefinition(v);
        }
      }
      if (found_as_static_variable) {
        return bd.getClassName() + "::"+n;
      }
      tfel::raise("findStressErrorNormalizationFactor: the `" + n +
                  "` variable is not defined as a material property, a static "
                  "variable, a parameter or a local variable");
    };
    //
    if (bd.isNameReserved("stress_error_normalization_factor")) {
      const auto senf_exists =
          bd.checkVariableExistence("stress_error_normalization_factor");
      if (!senf_exists.first) {
        tfel::raise(
            "findStressErrorNormalizationFactor: the "
            "`stress_error_normalization_factor` variable is not defined "
            "as a paramter.");
      }
      if (!senf_exists.second) {
        tfel::raise(
            "findStressErrorNormalizationFactor: the "
            "`stress_error_normalization_factor` variable is not defined "
            "for all modelling hypotheses.");
      }
      if (testInGivenVariableCategory(
              "Parameter", "stress_error_normalization_factor", "")) {
        return "this->stress_error_normalization_factor";
      }
      tfel::raise(
          "findStressErrorNormalizationFactor: the "
          "`stress_error_normalization_factor` variable is not "
          "defined as a material property or a parameter");
    }
    //
    if (bd.isNameReserved("young")) {
      checkVariable("young");
    }
    // look if the stiffness tensor shall be provided by the solver
    if (bd.getAttribute<bool>(BehaviourDescription::requiresStiffnessTensor,
                              false)) {
      return "this->D(0,0)";
    }
    // look if the stiffness tensor is computed
    if (bd.getAttribute<bool>(BehaviourDescription::computesStiffnessTensor,
                              false)) {
      return "this->D(0,0)";
    }
    // look if a variable with the YoungModulus glossary name is defined
    auto contains = [](const VariableDescriptionContainer& v) {
      return findByExternalName(v, Glossary::YoungModulus) != v.end();
    };
    const auto& mhs = bd.getDistinctModellingHypotheses();
    const auto& bdata = bd.getBehaviourData(*(mhs.begin()));
    if (contains(bdata.getMaterialProperties())) {
      const auto& v = bdata.getMaterialProperties().getVariableByExternalName(
          Glossary::YoungModulus);
      checkVariable(v.name);
      return "this->" + v.name;
    }
    if (contains(bdata.getParameters())) {
      const auto& v = bdata.getParameters().getVariableByExternalName(
          Glossary::YoungModulus);
      checkVariable(v.name);
      return "this->" + v.name;
    }
    if (contains(bdata.getLocalVariables())) {
      const auto& v = bdata.getLocalVariables().getVariableByExternalName(
          Glossary::YoungModulus);
      checkVariable(v.name);
      return "this->" + v.name;
    }
    // nothing worked...
    tfel::raise(
        "findStressErrorNormalizationFactor: "
        "no appropriate error normalization factor found");
  }  // end of findStressErrorNormalizationFactor

  void RungeKuttaDSLBase::writeBehaviourRKCastemIntegrator(
      std::ostream& os, const Hypothesis h) const {
    using namespace std;
    const auto& d = this->mb.getBehaviourData(h);
    //! all registred variables used in ComputeDerivatives and
    //! ComputeThermodynamicForces blocks
    auto uvs = d.getCodeBlock(BehaviourData::ComputeDerivative).members;
    if (d.hasCode(BehaviourData::ComputeThermodynamicForces)) {
      const auto& uvs2 =
          d.getCodeBlock(BehaviourData::ComputeThermodynamicForces).members;
      uvs.insert(uvs2.begin(), uvs2.end());
    }
    SupportedTypes::TypeSize stateVarsSize;
    for (const auto& v : d.getStateVariables()) {
      stateVarsSize += this->getTypeSize(v.type, v.arraySize);
    }
    os << "constexpr auto cste1_2 = NumericType{1}/NumericType{2};\n"
       << "constexpr auto cste1_4 = NumericType{1}/NumericType{4};\n"
       << "constexpr auto cste1_6 = NumericType(1)/NumericType(6);\n"
       << "time t   = time(0);\n"
       << "time dt_ = this->dt;\n"
       << "StressStensor sigf;\n"
       << "stress errabs;\n"
       << "bool failed = false\n;";
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "failed=!this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing initial stress\" << endl;\n"
         << "}\n";
    }
    os << "if(failed){\n"
       << "throw(tfel::material::DivergenceException());\n"
       << "}\n"
       << "const auto asig = "
       << "tfel::math::power<1, 2>((this->sig) | (this->sig));\n";
    const auto error_normalization_factor =
        findStressErrorNormalizationFactor(this->mb);
    os << "if ((" << error_normalization_factor
       << ")*NumericType(1.e-3)>asig){\n"
       << "  errabs = (" << error_normalization_factor
       << ") * NumericType(1.e-3) * (this->epsilon);\n"
       << "}else{\n"
       << "  errabs = (this->epsilon) * asig;\n"
       << "}\n\n"
       << "time dtprec = 100 * (this->dt) * "
          "std::numeric_limits<NumericType>::epsilon();\n"
       << "bool converged = false;\n";
    if (getDebugMode()) {
      os << "cout << endl << \"" << this->mb.getClassName()
         << "::integrate() : beginning of resolution\" << endl;\n";
    }
    os << "while(!converged){\n"
       << "if(dt_< dtprec){\n"
       << "cout<<\" dt \"<<this->dt<<\" t \"<<t<<\" dt_ \"<<dt_<<endl;\n"
       << "string msg(\"" << this->mb.getClassName() << "\");\n"
       << "msg += \" time step too small \"; \n"
       << "throw(runtime_error(msg)); \n"
       << "} \n";
    if (getDebugMode()) {
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : from \" << t <<  \" to \" << t+dt_ << \" with "
            "time step \" << dt_ << endl;\n";
    }
    os << "// Compute K1's values => y in castem \n";
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ = this->" << v.name << ";\n";
      }
    }
    writeExternalVariablesCurrentValues(os, this->mb, h, "0");
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating the stiffness tensor\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "failed = !this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K1's stress\" << endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n"
       << "failed = !this->computeDerivative();\n";
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K1's derivatives\" << "
            "endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n"
       << "// Compute y'1*dt=f(y)*dt in castem\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K1 = (dt_)*(this->d" << v.name << ");\n";
    }
    os << "}\n";
    os << "}\n\n";
    os << "if(!failed){\n";
    os << "// Compute K2's values => y1 in castem\n";
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ += cste1_2*(this->d" << v.name
           << "_K1);\n";
      }
    }
    writeExternalVariablesCurrentValues(os, this->mb, h, "cste1_2");
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating the stiffness tensor\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "// update the thermodynamic forces\n"
         << "failed = !this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K2's stress\" << endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n"
       << "failed = !this->computeDerivative();\n";
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K2's derivatives\" << "
            "endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n"
       << "// Compute y'2*dt=f(y1)*dt in castem\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K2 = (dt_)*(this->d" << v.name << ");\n";
    }
    os << "}\n"
       << "}\n"
       << "}\n\n"
       << "// Compute K3's values => y12 in castem\n"
       << "if(!failed){\n";
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ = this->" << v.name
           << "+cste1_4*(this->d" << v.name << "_K1 + this->d" << v.name
           << "_K2);\n";
      }
    }
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating the stiffness tensor\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "// update the thermodynamic forces\n"
         << "failed = !this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K3's stress\" << endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n"
       << "failed = !this->computeDerivative();\n";
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K3's derivatives\" << "
            "endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n"
       << "// Compute y'3*dt=f(y12)*dt in castem\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K3 = (dt_)*(this->d" << v.name << ");\n";
    }
    os << "}\n"
       << "}\n"
       << "}\n\n"
       << "if(!failed){\n"
       << "// Compute K4's values => y13 = y12+y'3*dt/2 in castem\n";
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ += cste1_2*(this->d" << v.name
           << "_K3);\n";
      }
    }
    writeExternalVariablesCurrentValues(os, this->mb, h, "1");
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating the stiffness tensor\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "// update the thermodynamic forces\n"
         << "failed = !this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K4's stress\" << endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n"
       << "failed = !this->computeDerivative();\n";
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K4's derivatives\" << "
            "endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n"
       << "// Compute y'4*dt=f(y13)*dt in castem\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K4 = (dt_)*(this->d" << v.name << ");\n";
    }
    os << "}\n"
       << "}\n"
       << "}\n\n"
       << "if(!failed){\n"
       << "// Compute K5's values => yf = y12+0.5*(y'3+y'4)*dt/2 in castem\n"
       << "//                     => yf = y+0.5*(y'1+y'2+y'3+y'4)*dt/2 in "
          "castem\n";
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ = this->" << v.name
           << "+cste1_4*(this->d" << v.name << "_K1 + this->d" << v.name
           << "_K2 + this->d" << v.name << "_K3 + this->d" << v.name
           << "_K4);\n";
      }
    }
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating the stiffness tensor\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "// update the thermodynamic forces\n"
         << "failed = !this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K5's stress\" << endl;\n"
         << "}\n";
    }
    os << "// Saving stresses obtained with yf\n"
       << "sigf=this->sig;\n"
       << "if(!failed){\n"
       << "failed = !this->computeDerivative();\n";
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K5's derivatives\" << "
            "endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n"
       << "// Compute y'5*dt=f(yf)*dt in castem\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K5 = (dt_)*(this->d" << v.name << ");\n";
    }
    os << "}\n"
       << "}\n"
       << "}\n\n"
       << "if(!failed){\n"
       << "// Compute K6's values => y5 = y+1/6*(y'1+4*y'3+y'5)*dt in castem\n";
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ = this->" << v.name
           << "+cste1_6*(this->d" << v.name << "_K1 + NumericType(4)*this->d"
           << v.name << "_K3 + this->d" << v.name << "_K5);\n";
      }
    }
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating the stiffness tensor\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "// update the thermodynamic forces\n"
         << "failed = !this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing criterium stress\" << "
            "endl;\n"
         << "}\n";
    }
    os << "}\n\n"
       << "if(!failed){\n"
       << "// Computing the error\n"
       << "const auto ra = "
       << "tfel::math::power<1, "
       << "2>(((sigf)-(this->sig))|((sigf)-(this->sig))) / errabs;\n"
       << "const auto sqra = tfel::math::power<1, 2>(ra);\n"
       << "// test for convergence\n"
       << "if ((sqra>" << this->mb.getClassName()
       << "::rkcastem_div)||(!ieee754::isfinite(ra))){\n"
       << "dt_ /= " << this->mb.getClassName() << "::rkcastem_div;\n";
    if (getDebugMode()) {
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : multiplying time step by a factor \" << 1/("
         << this->mb.getClassName() << "::rkcastem_div) << endl;\n";
    }
    os << "} else if (ra> " << this->mb.getClassName() << "::rkcastem_borne){\n"
       << "dt_ /= sqra;\n";
    if (getDebugMode()) {
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : multiplying time step by a factor \" << 1/sqra << "
            "endl;\n";
    }
    os << "}else{\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->" << v.name << " += cste1_4*(this->d" << v.name
         << "_K1 + this->d" << v.name << "_K2 + this->d" << v.name
         << "_K3 + this->d" << v.name << "_K4);\n";
    }
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating stiffness tensor at the end of the time step\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    if (!this->mb.getMainVariables().empty()) {
      os << "this->computeFinalThermodynamicForces();\n";
    }
    if (this->mb.hasCode(h, BehaviourData::UpdateAuxiliaryStateVariables)) {
      os << "this->updateAuxiliaryStateVariables(dt_);\n";
    }
    os << "t += dt_;\n"
       << "if(tfel::math::abs(this->dt-t)<dtprec){\n"
       << "converged=true;\n"
       << "}\n"
       << "if(!converged){\n"
       << "if ((" << this->mb.getClassName() << "::rkcastem_fac)*sqra < 1){\n"
       << "dt_ *= " << this->mb.getClassName() << "::rkcastem_fac;\n";
    if (getDebugMode()) {
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : multiplying time step by a factor \" << "
         << this->mb.getClassName() << "::rkcastem_fac << endl;\n";
    }
    os << "}else if ((sqra< " << this->mb.getClassName() << "::rkcastem_rmin)||"
       << "(sqra>" << this->mb.getClassName() << "::rkcastem_rmax)){\n";
    os << "dt_ /= sqra;\n";
    if (getDebugMode()) {
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : multiplying time step by a factor \" << 1/sqra << "
            "endl;\n";
    }
    os << "}\n"
       << "}\n"
       << "}\n"
       << "} else { \n"
       << "dt_ /=  " << this->mb.getClassName() << "::rkcastem_div;\n";
    if (getDebugMode()) {
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : multiplying time step by a factor \" << 1/("
         << this->mb.getClassName() << "::rkcastem_fac) << endl;\n";
    }
    os << "}\n"
       << "if(dt_<dtprec){\n"
       << "throw(tfel::material::DivergenceException());\n"
       << "}\n"
       << "if(!converged){\n"
       << "if((tfel::math::abs(this->dt-t-dt_)<2*dtprec) || "
          "(t+dt_>this->dt)){\n"
       << "dt_=this->dt-t;\n"
       << "}\n"
       << "}\n"
       << "}\n";
  }  // end of writeBehaviourRKCastemIntegrator

  void RungeKuttaDSLBase::writeBehaviourRK42Integrator(
      std::ostream& os, const Hypothesis h) const {
    auto get_enf = [](const VariableDescription& v) {
      return v.getAttribute<std::string>(
          VariableDescription::errorNormalisationFactor);
    };
    const auto& d = this->mb.getBehaviourData(h);
    auto uvs = d.getCodeBlock(BehaviourData::ComputeDerivative).members;
    if (d.hasCode(BehaviourData::ComputeThermodynamicForces)) {
      const auto& uvs2 =
          d.getCodeBlock(BehaviourData::ComputeThermodynamicForces).members;
      uvs.insert(uvs2.begin(), uvs2.end());
    }
    ErrorEvaluation eev;
    auto svsize = d.getStateVariables().getTypeSize();
    if (svsize.getValueForDimension(1) >= 20) {
      eev = MAXIMUMVALUEERROREVALUATION;
    } else {
      eev = ERRORSUMMATIONEVALUATION;
    }
    SupportedTypes::TypeSize stateVarsSize;
    for (const auto& v : d.getStateVariables()) {
      stateVarsSize += this->getTypeSize(v.type, v.arraySize);
    }
    os << "constexpr auto cste1_2 = NumericType{1}/NumericType{2};\n"
       << "constexpr auto cste1_6  = NumericType(1)/NumericType(6);\n"
       << "constexpr auto cste1_3  = NumericType(1)/NumericType(3);\n"
       << "time t   = time(0);\n"
       << "time dt_ = this->dt;\n"
       << "time dtprec = 100 * (this->dt) * "
          "std::numeric_limits<NumericType>::epsilon();\n"
       << "auto error = NumericType{};\n"
       << "bool converged = false;\n";
    if (getDebugMode()) {
      os << "cout << endl << \"" << this->mb.getClassName()
         << "::integrate() : beginning of resolution\" << endl;\n";
    }
    os << "while(!converged){\n";
    if (getDebugMode()) {
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : from \" << t <<  \" to \" << t+dt_ << \" with "
            "time step \" << dt_ << endl;\n";
    }
    os << "bool failed = false;\n";
    os << "// Compute K1's values\n";
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ = this->" << v.name << ";\n";
      }
    }
    writeExternalVariablesCurrentValues(os, this->mb, h, "0");
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating the stiffness tensor\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "failed = !this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K1's stress\" << endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n"
       << "failed = !this->computeDerivative();\n";
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K1's derivatives\" << "
            "endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K1 = (dt_)*(this->d" << v.name << ");\n";
    }
    os << "}\n"
       << "}\n\n"
       << "if(!failed){\n"
       << "// Compute K2's values\n";
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ += cste1_2*(this->d" << v.name
           << "_K1);\n";
      }
    }
    writeExternalVariablesCurrentValues(os, this->mb, h, "cste1_2");
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating the stiffness tensor\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "// update the thermodynamic forces\n"
         << "failed = !this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K2's stress\" << endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n"
       << "failed = !this->computeDerivative();\n";
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K2's derivatives\" << "
            "endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K2 = (dt_)*(this->d" << v.name << ");\n";
    }
    os << "}\n"
       << "}\n"
       << "}\n\n"
       << "// Compute K3's values\n"
       << "if(!failed){\n";
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ = this->" << v.name
           << "+cste1_2*(this->d" << v.name << "_K2);\n";
      }
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "// update the thermodynamic forces\n"
         << "failed = !this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K3's stress\" << endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n"
       << "failed = !this->computeDerivative();\n";
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K3's derivatives\" << "
            "endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K3 = (dt_)*(this->d" << v.name << ");\n";
    }
    os << "}\n"
       << "}\n"
       << "}\n\n"
       << "if(!failed){\n"
       << "// Compute K4's values\n";
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ = this->" << v.name << "+this->d"
           << v.name << "_K3;\n";
      }
    }
    writeExternalVariablesCurrentValues(os, this->mb, h, "1");
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating the stiffness tensor\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "// update the thermodynamic forces\n"
         << "failed = !this->computeThermodynamicForces();\n";
    }
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K4's stress\" << endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n"
       << "failed = !this->computeDerivative();\n";
    if (getDebugMode()) {
      os << "if(failed){\n"
         << "cout << \"" << this->mb.getClassName()
         << "::integrate() : failed while computing K4's derivatives\" << "
            "endl;\n"
         << "}\n";
    }
    os << "if(!failed){\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K4 = (dt_)*(this->d" << v.name << ");\n";
    }
    os << "}\n"
       << "}\n"
       << "}\n\n"
       << "if(!failed){\n"
       << "// Computing the error\n";
    if (eev == ERRORSUMMATIONEVALUATION) {
      bool first = true;
      for (const auto& v : d.getStateVariables()) {
        if (v.arraySize == 1u) {
          if (first) {
            os << "error  = ";
            first = false;
          } else {
            os << "error += ";
          }
          os << "tfel::math::base_type_cast(tfel::math::abs(";
          if (v.hasAttribute(VariableDescription::errorNormalisationFactor)) {
            os << "(";
          }
          os << "cste1_6*(this->d" << v.name << "_K1+"
             << "this->d" << v.name << "_K4-"
             << "this->d" << v.name << "_K2-"
             << "this->d" << v.name << "_K3)))";
          if (v.hasAttribute(VariableDescription::errorNormalisationFactor)) {
            os << ")/(" << get_enf(v) << ")";
          }
          os << ";\n";
        } else {
          if (this->mb.useDynamicallyAllocatedVector(v.arraySize)) {
            if (first) {
              os << "error  = NumericType(0);\n";
              first = false;
            }
            os << "for(unsigned short idx=0;idx!=" << v.arraySize
               << ";++idx){\n"
               << "error += tfel::math::base_type_cast(tfel::math::abs(";
            if (v.hasAttribute(VariableDescription::errorNormalisationFactor)) {
              os << "(";
            }
            os << "cste1_6*(this->d" << v.name << "_K1[idx]+"
               << "this->d" << v.name << "_K4[idx]-"
               << "this->d" << v.name << "_K2[idx]-"
               << "this->d" << v.name << "_K3[idx])))";
            if (v.hasAttribute(VariableDescription::errorNormalisationFactor)) {
              os << ")/(" << get_enf(v) << ")";
            }
            os << ";\n";
            os << "}\n";
          } else {
            for (unsigned short i = 0; i != v.arraySize; ++i) {
              if (first) {
                os << "error  = ";
                first = false;
              } else {
                os << "error += ";
              }
              os << "tfel::math::base_type_cast(tfel::math::abs(";
              if (v.hasAttribute(
                      VariableDescription::errorNormalisationFactor)) {
                os << "(";
              }
              os << "cste1_6*(this->d" << v.name << "_K1[" << i << "]+"
                 << "this->d" << v.name << "_K4[" << i << "]-"
                 << "this->d" << v.name << "_K2[" << i << "]-"
                 << "this->d" << v.name << "_K3[" << i << "])))";
              if (v.hasAttribute(
                      VariableDescription::errorNormalisationFactor)) {
                os << ")/(" << get_enf(v) << ")";
              }
              os << ";\n";
            }
          }
        }
      }
      os << "error/=" << stateVarsSize << ";\n";
    } else if (eev == MAXIMUMVALUEERROREVALUATION) {
      os << "error  = NumericType(0);\n"
         << "auto rk_update_error = [&error](const auto rk_error){\n"
         << "if(!ieee754::isfinite(error)){return;}\n"
         << "if(!ieee754::isfinite(rk_error)){\n"
         << "error = rk_error;\n"
         << "return;\n"
         << "}\n"
         << "error = std::max(error, tfel::math::base_type_cast(rk_error));\n"
         << "};\n";
      for (const auto& v : d.getStateVariables()) {
        if (v.arraySize == 1u) {
          os << "rk_update_error(tfel::math::abs(";
          if (v.hasAttribute(VariableDescription::errorNormalisationFactor)) {
            os << "(";
          }
          os << "cste1_6*(this->d" << v.name << "_K1+"
             << "this->d" << v.name << "_K4-"
             << "this->d" << v.name << "_K2-"
             << "this->d" << v.name << "_K3))";
          if (v.hasAttribute(VariableDescription::errorNormalisationFactor)) {
            os << ")/(" << get_enf(v) << ")";
          }
          os << ");\n";
        } else {
          if (this->mb.useDynamicallyAllocatedVector(v.arraySize)) {
            os << "for(unsigned short idx=0;idx!=" << v.arraySize
               << ";++idx){\n";
            os << "rk_update_error(tfel::math::abs(";
            if (v.hasAttribute(VariableDescription::errorNormalisationFactor)) {
              os << "(";
            }
            os << "cste1_6*(this->d" << v.name << "_K1[idx]+"
               << "this->d" << v.name << "_K4[idx]-"
               << "this->d" << v.name << "_K2[idx]-"
               << "this->d" << v.name << "_K3[idx]))";
            if (v.hasAttribute(VariableDescription::errorNormalisationFactor)) {
              os << ")/(" << get_enf(v) << ")";
            }
            os << ");\n";
            os << "}\n";
          } else {
            for (unsigned short i = 0; i != v.arraySize; ++i) {
              os << "rk_update_error(tfel::math::abs(";
              if (v.hasAttribute(
                      VariableDescription::errorNormalisationFactor)) {
                os << "(";
              }
              os << "cste1_6*(this->d" << v.name << "_K1[" << i << "]+"
                 << "this->d" << v.name << "_K4[" << i << "]-"
                 << "this->d" << v.name << "_K2[" << i << "]-"
                 << "this->d" << v.name << "_K3[" << i << "]))";
              if (v.hasAttribute(
                      VariableDescription::errorNormalisationFactor)) {
                os << ")/(" << get_enf(v) << ")";
              }
              os << ");\n";
            }
          }
        }
      }
    } else {
      this->throwRuntimeError("RungeKuttaDSLBase::writeBehaviourRK42Integrator",
                              "internal error, unsupported error evaluation");
    }
    os << "if(!ieee754::isfinite(error)){\n"
       << "throw(tfel::material::DivergenceException());\n"
       << "}\n";
    if (getDebugMode()) {
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : error \" << error << endl;\n";
    }
    os << "// test for convergence\n"
       << "if(error<this->epsilon){\n"
       << "// Final Step\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->" << v.name << " += cste1_6*(this->d" << v.name
         << "_K1 + this->d" << v.name << "_K4)+"
         << "    cste1_3*(this->d" << v.name << "_K3 + this->d" << v.name
         << "_K2);\n";
    }
    if (!this->mb.getMainVariables().empty()) {
      os << "this->computeFinalThermodynamicForces();\n";
    }
    if (this->mb.hasCode(h, BehaviourData::UpdateAuxiliaryStateVariables)) {
      os << "this->updateAuxiliaryStateVariables(dt_);\n";
    }
    os << "t += dt_;\n"
       << "if(tfel::math::abs(this->dt-t) < dtprec){\n"
       << "converged=true;\n"
       << "}\n"
       << "}\n"
       << "if(!converged){"
       << "// time multiplier\n"
       << "real corrector;\n"
       << "if(error<100*std::numeric_limits<real>::min()){\n"
       << "corrector=real(10.);\n"
       << "} else {\n"
       << "corrector = 0.8*pow((this->epsilon)/error,1./3.);\n"
       << "}\n"
       << "if(corrector<=real(0.1f)){\n"
       << "dt_ *= real(0.1f);\n";
    if (getDebugMode()) {
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : reducing time step by a factor 10\" << endl;\n";
    }
    os << "} else if(corrector>real(10)){\n";
    os << "dt_ *= real(10);\n";
    if (getDebugMode()) {
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : multiplying time step by a factor 10\" << endl;\n";
    }
    os << "} else {\n";
    os << "dt_ *= corrector;\n";
    if (getDebugMode()) {
      os << "if(corrector<1){\n";
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : reducing time step by a factor \"   << corrector "
            "<< endl;\n";
      os << "} else {\n";
      os << "cout << \"" << this->mb.getClassName()
         << "::integrate() : increasing time step by a factor \" << corrector "
            "<< endl;\n";
      os << "}\n";
    }
    os << "}\n"
       << "if(dt_<dtprec){\n"
       << "throw(tfel::material::DivergenceException());\n"
       << "}\n"
       << "if((tfel::math::abs(this->dt-t-dt_)<2*dtprec)||(t+dt_>this->dt)){\n"
       << "dt_=this->dt-t;\n"
       << "}\n"
       << "}\n"
       << "} else {\n"
       << "// failed is true\n"
       << "dt_ *= real(0.1f);\n"
       << "if(dt_<dtprec){\n"
       << "throw(tfel::material::DivergenceException());\n"
       << "}\n"
       << "}\n"
       << "}\n";
  }  // end of writeBehaviourRK42Integrator

  void RungeKuttaDSLBase::writeBehaviourRK4Integrator(
      std::ostream& os, const Hypothesis h) const {
    const auto btype = this->mb.getBehaviourTypeFlag();
    const auto& d = this->mb.getBehaviourData(h);
    auto uvs = d.getCodeBlock(BehaviourData::ComputeDerivative).members;
    if (d.hasCode(BehaviourData::ComputeThermodynamicForces)) {
      const auto& uvs2 =
          d.getCodeBlock(BehaviourData::ComputeThermodynamicForces).members;
      uvs.insert(uvs2.begin(), uvs2.end());
    }
    os << "constexpr auto cste1_2 = NumericType{1}/NumericType{2};\n"
       << "// Compute K1's values\n";
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "this->computeThermodynamicForces();\n";
    }
    os << "if(!this->computeDerivative()){\n";
    if (this->mb.useQt()) {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType,use_qt>::FAILURE;\n";
    } else {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType,false>::FAILURE;\n";
    }
    os << "}\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K1 = (this->dt)*(this->d" << v.name
         << ");\n";
    }
    writeExternalVariablesCurrentValues2(os, this->mb, h, "cste1_2");
    for (const auto& iv : d.getStateVariables()) {
      if (uvs.find(iv.name) != uvs.end()) {
        os << "this->" << iv.name << "_ += cste1_2*(this->d" << iv.name
           << "_K1);\n";
      }
    }
    if ((this->mb.getAttribute<bool>(
            BehaviourDescription::computesStiffnessTensor, false)) &&
        (!this->mb.areElasticMaterialPropertiesConstantDuringTheTimeStep())) {
      auto m =
          modifyVariableForStiffnessTensorComputation(this->mb.getClassName());
      os << "// updating the stiffness tensor\n";
      this->writeStiffnessTensorComputation(os, "this->D", m);
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "// update the thermodynamic forces\n"
         << "this->computeThermodynamicForces();\n\n";
    }
    os << "// Compute K2's values\n"
       << "if(!this->computeDerivative()){\n";
    if (this->mb.useQt()) {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType,use_qt>::FAILURE;\n";
    } else {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType,false>::FAILURE;\n";
    }
    os << "}\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K2 = (this->dt)*(this->d" << v.name
         << ");\n";
    }
    for (const auto& iv : d.getStateVariables()) {
      if (uvs.find(iv.name) != uvs.end()) {
        os << "this->" << iv.name << "_ = "
           << "this->" << iv.name << "+ cste1_2*(this->d" << iv.name
           << "_K2);\n";
      }
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "// update the thermodynamic forces\n"
         << "this->computeThermodynamicForces();\n\n";
    }
    os << "// Compute K3's values\n"
       << "if(!this->computeDerivative()){\n";
    if (this->mb.useQt()) {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType,use_qt>::FAILURE;\n";
    } else {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType,false>::FAILURE;\n";
    }
    os << "}\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K3 = (this->dt)*(this->d" << v.name
         << ");\n";
    }
    for (const auto& v : d.getStateVariables()) {
      if (uvs.find(v.name) != uvs.end()) {
        os << "this->" << v.name << "_ = "
           << "this->" << v.name << "+ (this->d" << v.name << "_K3);\n";
      }
    }
    if (this->mb.hasCode(h, BehaviourData::ComputeThermodynamicForces)) {
      os << "// update the thermodynamic forces\n"
         << "this->computeThermodynamicForces();\n\n";
    }
    os << "// Compute K4's values\n"
       << "if(!this->computeDerivative()){\n";
    if (this->mb.useQt()) {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType,use_qt>::FAILURE;\n";
    } else {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType,false>::FAILURE;\n";
    }
    os << "}\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->d" << v.name << "_K4 = (this->dt)*(this->d" << v.name
         << ");\n";
    }
    os << "// Final Step\n";
    for (const auto& v : d.getStateVariables()) {
      os << "this->" << v.name << " += "
         << "(this->d" << v.name << "_K1+this->d" << v.name << "_K4)/6+\n";
      os << "(this->d" << v.name << "_K2+this->d" << v.name << "_K3)/3;\n";
    }
    if (!this->mb.getMainVariables().empty()) {
      os << "// update the thermodynamic forces\n"
         << "this->computeFinalThermodynamicForces();\n";
    }
    if (this->mb.hasCode(h, BehaviourData::UpdateAuxiliaryStateVariables)) {
      os << "this->updateAuxiliaryStateVariables(this->dt);\n";
    }
  }  // end of writeBehaviourRK4Integrator

  void RungeKuttaDSLBase::writeBehaviourIntegrator(std::ostream& os,
                                                   const Hypothesis h) const {
    const auto btype = this->mb.getBehaviourTypeFlag();
    const auto& algorithm =
        this->mb.getAttribute<std::string>(BehaviourData::algorithm);
    const auto& d = this->mb.getBehaviourData(h);
    this->checkBehaviourFile(os);
    os << "/*!\n"
       << "* \\brief Integrate behaviour law over the time step\n"
       << "*/\n"
       << "IntegrationResult\n";
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
    os << "using namespace std;\n"
       << "using namespace tfel::math;\n";
    if ((this->mb.getBehaviourType() ==
         BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) ||
        (this->mb.getBehaviourType() ==
         BehaviourDescription::COHESIVEZONEMODEL)) {
      if (this->mb.useQt()) {
        os << "if(smflag!=MechanicalBehaviour<" << btype
           << ",hypothesis, NumericType,use_qt>::STANDARDTANGENTOPERATOR){\n"
           << "throw(runtime_error(\"invalid tangent operator flag\"));\n"
           << "}\n";
      } else {
        os << "if(smflag!=MechanicalBehaviour<" << btype
           << ",hypothesis, NumericType,false>::STANDARDTANGENTOPERATOR){\n"
           << "throw(runtime_error(\"invalid tangent operator flag\"));\n"
           << "}\n";
      }
    }
    if (this->mb.getAttribute(BehaviourData::profiling, false)) {
      writeStandardPerformanceProfilingBegin(os, mb.getClassName(),
                                             BehaviourData::Integrator);
    }
    if (algorithm == "Euler") {
      this->writeBehaviourEulerIntegrator(os, h);
    } else if (algorithm == "RungeKutta2") {
      this->writeBehaviourRK2Integrator(os, h);
    } else if (algorithm == "RungeKutta4/2") {
      this->writeBehaviourRK42Integrator(os, h);
    } else if (algorithm == "RungeKutta5/4") {
      this->writeBehaviourRK54Integrator(os, h);
    } else if (algorithm == "RungeKuttaCastem") {
      this->writeBehaviourRKCastemIntegrator(os, h);
    } else if (algorithm == "RungeKutta4") {
      this->writeBehaviourRK4Integrator(os, h);
    } else {
      this->throwRuntimeError(
          "RungeKuttaDSLBase::writeBehaviourIntegrator",
          "internal error\n'" + algorithm +
              "' is not a known algorithm. "
              "This shall not happen at this stage."
              " Please contact MFront developper to help them debug this.");
    }
    for (const auto& v : d.getPersistentVariables()) {
      this->writePhysicalBoundsChecks(os, v, false);
    }
    for (const auto& v : d.getPersistentVariables()) {
      this->writeBoundsChecks(os, v, false);
    }
    if (this->mb.getAttribute(BehaviourData::profiling, false)) {
      writeStandardPerformanceProfilingEnd(os);
    }
    os << "if(smt!=NOSTIFFNESSREQUESTED){\n";
    if (this->mb.hasAttribute(h, BehaviourData::hasConsistentTangentOperator)) {
      if (this->mb.getBehaviourType() ==
          BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) {
        os << "if(!this->computeConsistentTangentOperator(smflag,smt)){\n";
      } else {
        os << "if(!this->computeConsistentTangentOperator(smt)){\n";
      }
      if (this->mb.useQt()) {
        os << "return MechanicalBehaviour<" << btype
           << ",hypothesis, NumericType,use_qt>::FAILURE;\n";
      } else {
        os << "return MechanicalBehaviour<" << btype
           << ",hypothesis, NumericType,false>::FAILURE;\n";
      }
      os << "}\n";
    } else {
      os << "string msg(\"" << this->mb.getClassName() << "::integrate : \");\n"
         << "msg +=\"unimplemented feature\";\n"
         << "throw(runtime_error(msg));\n";
    }
    os << "}\n";
    if (this->mb.useQt()) {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType,use_qt>::SUCCESS;\n";
    } else {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType,false>::SUCCESS;\n";
    }
    os << "} // end of " << this->mb.getClassName() << "::integrate\n\n";
  }  // end of void RungeKuttaDSLBase::writeBehaviourIntegrator()

  void RungeKuttaDSLBase::getSymbols(
      std::map<std::string, std::string>& symbols,
      const Hypothesis h,
      const std::string& n) {
    BehaviourDSLCommon::getSymbols(symbols, h, n);
    const auto& d = this->mb.getBehaviourData(h);
    if (n == BehaviourData::ComputeDerivative) {
      for (const auto& mv : this->mb.getMainVariables()) {
        getTimeDerivativeSymbol(symbols, mv.first);
      }
      getTimeDerivativeSymbols(symbols, d.getIntegrationVariables());
      getTimeDerivativeSymbols(symbols, d.getExternalStateVariables());
    } else {
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
    }
  }  // end of getSymbols

  RungeKuttaDSLBase::~RungeKuttaDSLBase() = default;

}  // end of namespace mfront
