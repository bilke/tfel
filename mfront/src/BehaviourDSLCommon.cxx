/*!
 * \file   mfront/src/BehaviourDSLCommon.cxx
 * \brief
 *
 * \author Thomas Helfer
 * \date   05/05/2008
 * \copyright Copyright (C) 2006-2018 CEA/DEN, EDF R&D. All rights
 * reserved.
 * This project is publicly released under either the GNU GPL Licence
 * or the CECILL-A licence. A copy of thoses licences are delivered
 * with the sources of TFEL. CEA or EDF may also distribute this
 * project under specific licensing conditions.
 */

#include <algorithm>
#include <stdexcept>
#include <iterator>
#include <sstream>
#include <utility>
#include <vector>
#include <limits>
#include <cctype>
#include <cmath>

#include "TFEL/Raise.hxx"
#include "TFEL/System/System.hxx"
#include "TFEL/UnicodeSupport/UnicodeSupport.hxx"
#include "TFEL/Glossary/Glossary.hxx"
#include "TFEL/Glossary/GlossaryEntry.hxx"
#include "TFEL/Math/General/IEEE754.hxx"
#include "TFEL/Material/FiniteStrainBehaviourTangentOperator.hxx"
#include "TFEL/Utilities/Data.hxx"
#include "TFEL/Utilities/StringAlgorithms.hxx"
#include "TFEL/Math/Evaluator.hxx"

#include "MFront/MFront.hxx"
#include "MFront/MFrontHeader.hxx"
#include "MFront/DSLUtilities.hxx"
#include "MFront/MFrontUtilities.hxx"
#include "MFront/MFrontDebugMode.hxx"
#include "MFront/PedanticMode.hxx"
#include "MFront/MFrontLogStream.hxx"
#include "MFront/SearchPathsHandler.hxx"
#include "MFront/AbstractBehaviourInterface.hxx"
#include "MFront/MFrontMaterialPropertyInterface.hxx"
#include "MFront/PerformanceProfiling.hxx"
#include "MFront/BehaviourInterfaceFactory.hxx"
#include "MFront/FiniteStrainBehaviourTangentOperatorConversionPath.hxx"
#include "MFront/AbstractBehaviourBrick.hxx"
#include "MFront/BehaviourBrickFactory.hxx"
#include "MFront/TargetsDescription.hxx"
#include "MFront/MaterialPropertyDescription.hxx"
#include "MFront/ModelDSL.hxx"
#include "MFront/MFrontModelInterface.hxx"
#include "MFront/GlobalDomainSpecificLanguageOptionsManager.hxx"
#include "MFront/BehaviourDSLCommon.hxx"

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

  tfel::utilities::DataMapValidator
  BehaviourDSLCommon::getDSLOptionsValidator() {
    return DSLBase::getDSLOptionsValidator().addDataTypeValidator<bool>(
        BehaviourDescription::
            automaticDeclarationOfTheTemperatureAsFirstExternalStateVariable);
  }  // end of getDSLOptionsValidator

  BehaviourDSLCommon::StandardVariableModifier::StandardVariableModifier(
      const Hypothesis h, const FunctionType f)
      : hypothesis(h),
        fct(f) {}  // end of StandardVariableModifier::StandardVariableModifier

  std::string BehaviourDSLCommon::StandardVariableModifier::exe(
      const std::string& v, const bool b) {
    return (this->fct)(this->hypothesis, v, b);
  }  // end of StandardVariableModifier::exe

  BehaviourDSLCommon::StandardVariableModifier::~StandardVariableModifier() =
      default;

  BehaviourDSLCommon::StandardWordAnalyser::StandardWordAnalyser(
      const Hypothesis h, const FunctionType f)
      : hypothesis(h),
        fct(f) {}  // end of StandardWordAnalyser::StandardWordAnalyser

  void BehaviourDSLCommon::StandardWordAnalyser::exe(CodeBlock& c,
                                                     const std::string& v) {
    this->fct(c, this->hypothesis, v);
  }  // end of StandardWordAnalyser::exe

  BehaviourDSLCommon::StandardWordAnalyser::~StandardWordAnalyser() = default;

  bool isValidBehaviourName(const std::string& n) {
    return tfel::utilities::CxxTokenizer::isValidIdentifier(n, false);
  }

  BehaviourDSLCommon::BehaviourDSLCommon(const DSLOptions& opts)
      : DSLBase(opts),
        mb(tfel::utilities::extract(
            opts,
            {BehaviourDescription::
                 automaticDeclarationOfTheTemperatureAsFirstExternalStateVariable})),
        useStateVarTimeDerivative(false),
        explicitlyDeclaredUsableInPurelyImplicitResolution(false) {
    //
    using MemberFunc = void (BehaviourDSLCommon::*)();
    constexpr auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    //
    DSLBase::handleDSLOptions(this->mb, opts);
    // By default, a behaviour can be used in a purely implicit resolution
    this->mb.setUsableInPurelyImplicitResolution(h, true);
    // reserve names
    for (const auto& n : DSLBase::getDefaultReservedNames()) {
      this->reserveName(n);
    }
    // register behaviours specific names
    this->registerDefaultVarNames();
    this->reserveName("minimal_time_step_scaling_factor");
    this->reserveName("maximal_time_step_scaling_factor");
    this->reserveName("current_time_step_scaling_factor");
    this->reserveName("v_sound");
    this->reserveName(tfel::unicode::getMangledString("vₛ"));
    this->reserveName("rho_m0");
    this->reserveName(tfel::unicode::getMangledString("ρₘ₀"));
    // default call backs
    auto add = [this](const std::string& k, const MemberFunc f) {
      this->callBacks.insert({k, [this, f] { (this->*f)(); }});
      this->registredKeyWords.insert(k);
    };
    add(";", &BehaviourDSLCommon::treatLonelySeparator);
    add("@DSL", &BehaviourDSLCommon::treatDSL);
    add("@Parser", &BehaviourDSLCommon::treatDSL);
    add("@Model", &BehaviourDSLCommon::treatModel);
    add("@Brick", &BehaviourDSLCommon::treatBrick);
    add("@ModellingHypothesis", &BehaviourDSLCommon::treatModellingHypothesis);
    add("@ModellingHypotheses", &BehaviourDSLCommon::treatModellingHypotheses);
    add("@Import", &BehaviourDSLCommon::treatImport);
    add("@Material", &BehaviourDSLCommon::treatMaterial);
    add("@Library", &BehaviourDSLCommon::treatLibrary);
    add("@Profiling", &BehaviourDSLCommon::treatProfiling);
    add("@Behaviour", &BehaviourDSLCommon::treatBehaviour);
    add("@StrainMeasure", &BehaviourDSLCommon::treatStrainMeasure);
    add("@Author", &BehaviourDSLCommon::treatAuthor);
    add("@Date", &BehaviourDSLCommon::treatDate);
    add("@Initialize", &BehaviourDSLCommon::treatInitialize);
    add("@MFront", &BehaviourDSLCommon::treatMFront);
    add("@Link", &BehaviourDSLCommon::treatLink);
    add("@Includes", &BehaviourDSLCommon::treatIncludes);
    add("@Members", &BehaviourDSLCommon::treatMembers);
    add("@Coef", &BehaviourDSLCommon::treatCoef);
    add("@MaterialProperty", &BehaviourDSLCommon::treatCoef);
    add("@LocalVar", &BehaviourDSLCommon::treatLocalVar);
    add("@LocalVariable", &BehaviourDSLCommon::treatLocalVar);
    add("@Parameter", &BehaviourDSLCommon::treatParameter);
    add("@StateVar", &BehaviourDSLCommon::treatStateVariable);
    add("@StateVariable", &BehaviourDSLCommon::treatStateVariable);
    add("@AuxiliaryStateVar", &BehaviourDSLCommon::treatAuxiliaryStateVariable);
    add("@AuxiliaryStateVariable",
        &BehaviourDSLCommon::treatAuxiliaryStateVariable);
    add("@ExternalStateVar", &BehaviourDSLCommon::treatExternalStateVariable);
    add("@ExternalStateVariable",
        &BehaviourDSLCommon::treatExternalStateVariable);
    add("@InitLocalVars", &BehaviourDSLCommon::treatInitLocalVariables);
    add("@InitLocalVariables", &BehaviourDSLCommon::treatInitLocalVariables);
    add("@InitializeLocalVariables",
        &BehaviourDSLCommon::treatInitLocalVariables);
    add("@MinimalTimeStepScalingFactor",
        &BehaviourDSLCommon::treatMinimalTimeStepScalingFactor);
    add("@MaximalTimeStepScalingFactor",
        &BehaviourDSLCommon::treatMaximalTimeStepScalingFactor);
    add("@APrioriTimeStepScalingFactor",
        &BehaviourDSLCommon::treatAPrioriTimeStepScalingFactor);
    add("@Integrator", &BehaviourDSLCommon::treatIntegrator);
    add("@APosterioriTimeStepScalingFactor",
        &BehaviourDSLCommon::treatAPosterioriTimeStepScalingFactor);
    add("@Interface", &BehaviourDSLCommon::treatInterface);
    add("@StaticVar", &BehaviourDSLCommon::treatStaticVar);
    add("@StaticVariable", &BehaviourDSLCommon::treatStaticVar);
    add("@IntegerConstant", &BehaviourDSLCommon::treatIntegerConstant);
    add("@UseQt", &BehaviourDSLCommon::treatUseQt);
    add("@Description", &BehaviourDSLCommon::treatDescription);
    add("@Bounds", &BehaviourDSLCommon::treatBounds);
    add("@PhysicalBounds", &BehaviourDSLCommon::treatPhysicalBounds);
    add("@RequireStiffnessOperator",
        &BehaviourDSLCommon::treatRequireStiffnessOperator);
    add("@RequireStiffnessTensor",
        &BehaviourDSLCommon::treatRequireStiffnessTensor);
    add("@RequireThermalExpansionCoefficientTensor",
        &BehaviourDSLCommon::treatRequireThermalExpansionCoefficientTensor);
    add("@OrthotropicBehaviour",
        &BehaviourDSLCommon::treatOrthotropicBehaviour);
    add("@IsotropicElasticBehaviour",
        &BehaviourDSLCommon::treatIsotropicElasticBehaviour);
    add("@IsotropicBehaviour", &BehaviourDSLCommon::treatIsotropicBehaviour);
    add("@PredictionOperator", &BehaviourDSLCommon::treatPredictionOperator);
    add("@Private", &BehaviourDSLCommon::treatPrivate);
    add("@Sources", &BehaviourDSLCommon::treatSources);
    add("@UpdateAuxiliaryStateVars",
        &BehaviourDSLCommon::treatUpdateAuxiliaryStateVariables);
    add("@UpdateAuxiliaryStateVariables",
        &BehaviourDSLCommon::treatUpdateAuxiliaryStateVariables);
    add("@ComputeThermalExpansion",
        &BehaviourDSLCommon::treatComputeThermalExpansion);
    add("@ComputeStressFreeExpansion",
        &BehaviourDSLCommon::treatComputeStressFreeExpansion);
    add("@Swelling", &BehaviourDSLCommon::treatSwelling);
    add("@AxialGrowth", &BehaviourDSLCommon::treatAxialGrowth);
    add("@Relocation", &BehaviourDSLCommon::treatRelocation);
    add("@InternalEnergy", &BehaviourDSLCommon::treatInternalEnergy);
    add("@DissipatedEnergy", &BehaviourDSLCommon::treatDissipatedEnergy);
    add("@CrystalStructure", &BehaviourDSLCommon::treatCrystalStructure);
    add("@SlipSystem", &BehaviourDSLCommon::treatSlipSystem);
    add("@GlidingSystem", &BehaviourDSLCommon::treatSlipSystem);
    add("@SlidingSystem", &BehaviourDSLCommon::treatSlipSystem);
    add("@SlipSystems", &BehaviourDSLCommon::treatSlipSystems);
    add("@GlidingSystems", &BehaviourDSLCommon::treatSlipSystems);
    add("@SlidingSystems", &BehaviourDSLCommon::treatSlipSystems);
    add("@InteractionMatrix", &BehaviourDSLCommon::treatInteractionMatrix);
    add("@SpeedOfSound", &BehaviourDSLCommon::treatSpeedOfSound);
    add("@DislocationsMeanFreePathInteractionMatrix",
        &BehaviourDSLCommon::treatDislocationsMeanFreePathInteractionMatrix);
    add("@InitializeFunctionVariable",
        &BehaviourDSLCommon::treatInitializeFunctionVariable);
    add("@InitializeFunction", &BehaviourDSLCommon::treatInitializeFunction);
    add("@PostProcessingVariable",
        &BehaviourDSLCommon::treatPostProcessingVariable);
    add("@PostProcessing", &BehaviourDSLCommon::treatPostProcessing);
  }  // end of BehaviourDSLCommon

  std::vector<AbstractDSL::DSLOptionDescription>
  BehaviourDSLCommon::getDSLOptions() const {
    auto opts = DSLBase::getDSLOptions();
    opts.push_back(
        {BehaviourDescription::
             automaticDeclarationOfTheTemperatureAsFirstExternalStateVariable,
         "boolean stating if the temperature shall be automatically declared "
         "as "
         "an external state variable"});
    return opts;
  }  // end of getDSLOptions

  AbstractDSL::DSLOptions BehaviourDSLCommon::buildDSLOptions() const {
    return DSLBase::buildCommonDSLOptions(this->mb);
  }  // end of buildDSLOptions

  std::string BehaviourDSLCommon::getMaterialKnowledgeIdentifier() const {
    if (this->mb.isBehaviourNameDefined()) {
      return this->mb.getBehaviourName();
    }
    return {};
  }  // end of getMaterialKnowledgeIdentifier

  std::string BehaviourDSLCommon::getMaterialName() const {
    return this->mb.getMaterialName();
  }  // end of getMaterialName(

  void BehaviourDSLCommon::analyse() {
    const auto& mh = ModellingHypothesis::getModellingHypotheses();
    std::vector<std::string> hn(mh.size());
    std::vector<Hypothesis>::const_iterator pmh;
    std::vector<std::string>::iterator phn;
    for (pmh = mh.begin(), phn = hn.begin(); pmh != mh.end(); ++pmh, ++phn) {
      *phn = ModellingHypothesis::toString(*pmh);
    }
    // strip comments from file
    this->stripComments();
    // begin treatement
    this->current = this->tokens.begin();
    while (this->current != this->tokens.end()) {
      if (find(hn.begin(), hn.end(), this->current->value) != hn.end()) {
        const auto h = ModellingHypothesis::fromString(this->current->value);
        ++(this->current);
        this->checkNotEndOfFile("BehaviourDSLCommon::analyse");
        this->readSpecifiedToken("BehaviourDSLCommon::analyse", "::");
        const auto v = tfel::unicode::getMangledString(this->current->value);
        if (!this->isCallableVariable(h, v)) {
          this->throwRuntimeError("BehaviourDSLCommon::analyse",
                                  "no variable named '" + v +
                                      "' for hypothesis '" +
                                      ModellingHypothesis::toString(h) + "'");
        }
        if (this->mb.isParameterName(h, v)) {
          this->treatParameterMethod(h);
        } else {
          this->treatVariableMethod(h);
        }
      } else {
        const auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
        const auto v = tfel::unicode::getMangledString(this->current->value);
        if (this->isCallableVariable(h, v)) {
          const auto isGradient = [this, &v] {
            for (const auto& g : this->gradients) {
              if (g.name == v) {
                return true;
              }
            }
            return this->mb.isGradientName(v);
          }();
          const auto isThermodynamicForce = [this, &v] {
            for (const auto& f : this->thermodynamic_forces) {
              if (f.name == v) {
                return true;
              }
            }
            return this->mb.isThermodynamicForceName(v);
          }();
          if (isGradient) {
            this->treatGradientMethod();
          } else if (isThermodynamicForce) {
            this->treatThermodynamicForceMethod();
          } else if (this->mb.isParameterName(h, v)) {
            this->treatParameterMethod(h);
          } else {
            this->treatVariableMethod(h);
          }
        } else {
          const auto k = this->current->value;
          const auto l = this->current->line;
          CallBack handler;
          auto p = this->callBacks.find(k);
          if (p == this->callBacks.end()) {
            if (getVerboseMode() >= VERBOSE_DEBUG) {
              auto& log = getLogStream();
              log << "treating unknown keyword\n";
            }
            handler = [this] { this->treatUnknownKeyword(); };
          } else {
            if (getVerboseMode() >= VERBOSE_DEBUG) {
              auto& log = getLogStream();
              log << "treating keyword : " << this->current->value << '\n';
            }
            handler = p->second;
          }
          this->currentComment = this->current->comment;
          ++(this->current);
          try {
            handler();
            const auto ph = this->hooks.find(k);
            if (ph != this->hooks.end()) {
              for (auto& hook : ph->second) {
                hook();
              }
            }
          } catch (std::exception& e) {
            std::ostringstream msg;
            msg << "BehaviourDSLCommon::analyse: "
                << "error while treating keyword '" << k << "' at line '" << l
                << "' of file '" << this->fd.fileName << "'.\n"
                << e.what();
            tfel::raise(msg.str());
          } catch (...) {
            this->currentComment.clear();
            throw;
          }
          this->currentComment.clear();
        }
      }
    }
  }  // end of analyse

  void BehaviourDSLCommon::importFile(
      const std::string& fn,
      const std::vector<std::string>& ecmds,
      const std::map<std::string, std::string>& s) {
    this->fd.fileName = fn;
    this->openFile(this->fd.fileName, ecmds, s);
    this->analyse();
  }  // end of importFile

  void BehaviourDSLCommon::analyseString(const std::string& s) {
    this->fd.fileName = "user defined string";
    this->parseString(s);
    this->analyse();
  }  // end of analyseString

  void BehaviourDSLCommon::getKeywordsList(std::vector<std::string>& k) const {
    for (const auto& c : this->callBacks) {
      k.push_back(c.first);
    }
  }  // end of getKeywordsList

  void BehaviourDSLCommon::addCallBack(const std::string& k,
                                       const CallBack c,
                                       const bool b) {
    if (!this->callBacks.insert({k, c}).second) {
      if (!b) {
        this->throwRuntimeError("BehaviourDSLCommon::addCallBack",
                                "callback '" + k + "' already registred");
      } else {
        this->callBacks[k] = c;
      }
    } else {
      this->registredKeyWords.insert(k);
    }
  }  // end of addCallBack

  void BehaviourDSLCommon::addHook(const std::string& k, const Hook h) {
    if (this->callBacks.find(k) == this->callBacks.end()) {
      this->throwRuntimeError("BehaviourDSLCommon::addHook",
                              "no callback called '" + k + "'");
    }
    this->hooks[k].push_back(h);
  }  // end of addHook

  void BehaviourDSLCommon::setMaterial(const std::string& m) {
    if (!isValidMaterialName(m)) {
      this->throwRuntimeError("BehaviourDSLCommon::setMaterial",
                              "invalid material name '" + m + "'");
    }
    this->mb.setMaterialName(m);
    if (!isValidIdentifier(this->mb.getClassName())) {
      this->throwRuntimeError("BehaviourDSLCommon::setMaterial",
                              "resulting class name is not valid (read '" +
                                  this->mb.getClassName() + "')");
    }
  }  // end of setMaterial

  void BehaviourDSLCommon::treatDisabledCallBack() {
    --(this->current);
    tfel::raise("The keyword: '" + this->current->value +
                "' has been disabled");
  }  // end of treatDisabledCallBack

  void BehaviourDSLCommon::disableCallBack(const std::string& k) {
    auto c = [this] { this->treatDisabledCallBack(); };
    auto p = this->callBacks.find(k);
    if (p == this->callBacks.end()) {
      this->callBacks.insert({k, c});
      this->registredKeyWords.insert(k);
      return;
    }
    p->second = c;
  }  // end of disableCallBack

  BehaviourDSLCommon::CodeBlockOptions BehaviourDSLCommon::treatCodeBlock(
      const std::string& n,
      std::function<
          std::string(const Hypothesis, const std::string&, const bool)> m,
      const bool b,
      const bool s) {
    CodeBlockOptions o;
    this->readCodeBlockOptions(o, s);
    this->treatUnsupportedCodeBlockOptions(o);
    this->treatCodeBlock(o, n, m, b);
    return o;
  }

  BehaviourDSLCommon::CodeBlockOptions BehaviourDSLCommon::treatCodeBlock(
      const std::string& n,
      std::function<
          std::string(const Hypothesis, const std::string&, const bool)> m,
      std::function<void(CodeBlock&, const Hypothesis, const std::string&)> a,
      const bool b,
      const bool s) {
    CodeBlockOptions o;
    this->readCodeBlockOptions(o, s);
    this->treatUnsupportedCodeBlockOptions(o);
    this->treatCodeBlock(o, n, m, a, b);
    return o;
  }  // end of treatCodeBlock

  void BehaviourDSLCommon::treatCodeBlock(
      const BehaviourDSLCommon::CodeBlockOptions& o,
      const std::string& n,
      std::function<
          std::string(const Hypothesis, const std::string&, const bool)> m,
      std::function<void(CodeBlock&, const Hypothesis, const std::string&)> a,
      const bool b) {
    const auto beg = this->current;
    for (const auto h : o.hypotheses) {
      this->current = beg;
      const auto c = this->readNextBlock(h, n, m, a, b);
      this->mb.setCode(h, n, c, o.m, o.p);
    }
  }  // end of treatCodeBlock

  void BehaviourDSLCommon::treatCodeBlock(
      const BehaviourDSLCommon::CodeBlockOptions& o,
      const std::string& n,
      std::function<
          std::string(const Hypothesis, const std::string&, const bool)> m,
      const bool b) {
    this->disableVariableDeclaration();
    const auto beg = this->current;
    for (const auto h : o.hypotheses) {
      this->current = beg;
      const auto c = this->readNextBlock(h, n, m, b);
      this->mb.setCode(h, n, c, o.m, o.p);
    }
  }  // end of treatCodeBlock

  CodeBlock BehaviourDSLCommon::readNextBlock(
      const Hypothesis h,
      const std::string& n,
      std::function<
          std::string(const Hypothesis, const std::string&, const bool)> m,
      std::function<void(CodeBlock&, const Hypothesis, const std::string&)> a,
      const bool b) {
    this->disableVariableDeclaration();
    const auto& md = this->mb.getBehaviourData(h);
    auto vm = std::make_shared<StandardVariableModifier>(h, m);
    auto wa = std::make_shared<StandardWordAnalyser>(h, a);
    CodeBlockParserOptions option;
    option.qualifyStaticVariables = b;
    option.qualifyMemberVariables = b;
    option.modifier = vm;
    option.analyser = wa;
    option.mn = md.getRegistredMembersNames();
    option.smn = md.getRegistredStaticMembersNames();
    this->getSymbols(option.symbols, h, n);
    const auto& c = this->readNextBlock(option);
    return c;
  }  // end of readNextBlock

  CodeBlock BehaviourDSLCommon::readNextBlock(
      const Hypothesis h,
      const std::string& n,
      std::function<
          std::string(const Hypothesis, const std::string&, const bool)> m,
      const bool b) {
    this->disableVariableDeclaration();
    const auto& md = this->mb.getBehaviourData(h);
    auto vm = std::make_shared<StandardVariableModifier>(h, m);
    CodeBlockParserOptions option;
    option.qualifyStaticVariables = b;
    option.qualifyMemberVariables = b;
    option.modifier = vm;
    option.mn = md.getRegistredMembersNames();
    option.smn = md.getRegistredStaticMembersNames();
    this->getSymbols(option.symbols, h, n);
    const auto c = this->readNextBlock(option);
    return c;
  }  // end of readNextBlock

  BehaviourDSLCommon::CodeBlockOptions BehaviourDSLCommon::treatCodeBlock(
      const std::string& n1,
      const std::string& n2,
      std::function<
          std::string(const Hypothesis, const std::string&, const bool)> m1,
      std::function<
          std::string(const Hypothesis, const std::string&, const bool)> m2,
      const bool b,
      const bool s) {
    using std::shared_ptr;
    CodeBlockOptions o;
    this->readCodeBlockOptions(o, s);
    this->treatUnsupportedCodeBlockOptions(o);
    this->treatCodeBlock(o, n1, n2, m1, m2, b);
    return o;
  }  // end of treatCodeBlock

  void BehaviourDSLCommon::treatCodeBlock(
      const BehaviourDSLCommon::CodeBlockOptions& o,
      const std::string& n1,
      const std::string& n2,
      std::function<
          std::string(const Hypothesis, const std::string&, const bool)> m1,
      std::function<
          std::string(const Hypothesis, const std::string&, const bool)> m2,
      const bool b) {
    const auto beg = this->current;
    this->disableVariableDeclaration();
    for (const auto& h : o.hypotheses) {
      const auto& md = this->mb.getBehaviourData(h);
      this->current = beg;
      CodeBlock c1;
      CodeBlock c2;
      CodeBlockParserOptions o1;
      o1.qualifyStaticVariables = b;
      o1.qualifyMemberVariables = b;
      o1.modifier = std::make_shared<StandardVariableModifier>(h, m1);
      o1.mn = md.getRegistredMembersNames();
      o1.smn = md.getRegistredStaticMembersNames();
      this->getSymbols(o1.symbols, h, n1);
      CodeBlockParserOptions o2;
      o2.qualifyStaticVariables = b;
      o2.qualifyMemberVariables = b;
      o2.modifier = std::make_shared<StandardVariableModifier>(h, m2);
      o2.mn = md.getRegistredMembersNames();
      o2.smn = md.getRegistredStaticMembersNames();
      this->getSymbols(o2.symbols, h, n1);
      this->readNextBlock(c1, c2, o1, o2);
      this->mb.setCode(h, n1, c1, o.m, o.p);
      this->mb.setCode(h, n2, c2, o.m, o.p);
    }
  }  // end of treatCodeBlock

  void BehaviourDSLCommon::addMaterialProperties(
      const VariableDescriptionContainer& mps) {
    constexpr auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    const auto d = this->getBehaviourDSLDescription();
    tfel::raise_if(!d.allowUserDefinedMaterialProperties,
                   "BehaviourDSLCommon::addMaterialProperties: "
                   "adding material properties is not allowed");
    this->mb.addMaterialProperties(h, mps);
  }  // end of addMaterialProperties

  void BehaviourDSLCommon::addParameters(
      const VariableDescriptionContainer& params) {
    constexpr auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    const auto d = this->getBehaviourDSLDescription();
    tfel::raise_if(!d.allowUserDefinedParameters,
                   "BehaviourDSLCommon::addParameters: "
                   "adding parameters is not allowed");
    this->mb.addParameters(h, params);
  }  // end of addParameters

  void BehaviourDSLCommon::addStateVariables(
      const VariableDescriptionContainer& isvs) {
    constexpr auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    const auto d = this->getBehaviourDSLDescription();
    tfel::raise_if(!d.allowUserDefinedStateVariables,
                   "BehaviourDSLCommon::addStateVariables: "
                   "adding state variables is not allowed");
    this->mb.addStateVariables(h, isvs);
  }  // end of addStateVariables

  void BehaviourDSLCommon::addAuxiliaryStateVariables(
      const VariableDescriptionContainer& isvs) {
    constexpr auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    const auto d = this->getBehaviourDSLDescription();
    tfel::raise_if(!d.allowUserDefinedAuxiliaryStateVariables,
                   "BehaviourDSLCommon::addAuxiliaryStateVariables: "
                   "adding auxiliary state variables is not allowed");
    this->mb.addAuxiliaryStateVariables(h, isvs);
  }  // end of addAuxiliaryStateVariables

  void BehaviourDSLCommon::addExternalStateVariables(
      const VariableDescriptionContainer& esvs) {
    constexpr auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    const auto d = this->getBehaviourDSLDescription();
    tfel::raise_if(!d.allowUserDefinedExternalStateVariables,
                   "BehaviourDSLCommon::addExternalStateVariables: "
                   "adding external state variables is not allowed");
    this->mb.addExternalStateVariables(h, esvs);
  }  // end of addExternalStateVariables

  void BehaviourDSLCommon::addLocalVariables(
      const VariableDescriptionContainer& lvs) {
    constexpr auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    const auto d = this->getBehaviourDSLDescription();
    tfel::raise_if(!d.allowUserDefinedLocalVariables,
                   "BehaviourDSLCommon::addLocalVariables: "
                   "adding local variables is not allowed");
    this->mb.addLocalVariables(h, lvs);
  }  // end of addLocalVariables

  void BehaviourDSLCommon::addIntegrationVariables(
      const VariableDescriptionContainer& ivs) {
    constexpr auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    const auto d = this->getBehaviourDSLDescription();
    tfel::raise_if(!d.allowUserDefinedIntegrationVariables,
                   "BehaviourDSLCommon::addIntegrationVariables: "
                   "adding integration variables is not allowed");
    this->mb.addIntegrationVariables(h, ivs);
  }  // end of addIntegrationVariables

  void BehaviourDSLCommon::writeModelCall(std::ostream& out,
                                          std::vector<std::string>& tmpnames,
                                          const Hypothesis h,
                                          const ModelDescription& md,
                                          const std::string& vo,
                                          const std::string& vs,
                                          const std::string& bn) const {
    auto throw_if = [this](const bool b, const std::string& m) {
      if (b) {
        this->throwRuntimeError("BehaviourDSLCommon::writeModelCall", m);
      }
    };
    auto write_variable = [throw_if, &md, &out](const std::string& v,
                                                const unsigned short d) {
      if (d == 0) {
        out << "this->" << v << "+this->d" << v;
      } else if (d == 1) {
        out << "this->" << v;
      } else {
        throw_if(true, "invalid depth for the temperature '" + v +
                           "' "
                           "in model '" +
                           md.className + "'");
      }
    };
    const auto& bd = this->mb.getBehaviourData(h);
    throw_if(md.outputs.size() != 1u,
             "invalid number of outputs for model '" + md.className + "'");
    throw_if(!md.constantMaterialProperties.empty(),
             "constant material properties are not supported yet");
    throw_if(md.functions.size() != 1u,
             "invalid number of functions in model '" + md.className + "'");
    const auto& f = md.functions[0];
    throw_if(f.modifiedVariables.empty(),
             "no modified variable for function '" + f.name + "'");
    throw_if(f.modifiedVariables.size() != 1u,
             "invalid number of functions in model '" + md.className + "'");
    throw_if(f.name.empty(), "unnamed function");
    throw_if((f.usedVariables.empty()) && (!f.useTimeIncrement),
             "no used variable for function '" + f.name + "'");
    const auto sm = this->getTemporaryVariableName(tmpnames, bn);
    out << "// updating " << vs << "\n"
        << "mfront::" << md.className << "<NumericType> " << sm << ";\n"
        << "" << sm << ".setOutOfBoundsPolicy(this->policy);\n"
        << "this->" << vo << " = " << sm << "." << f.name << "(";
    const auto args = [&f] {
      auto a = std::vector<std::string>{};
      for (const auto& uv : f.usedVariables) {
        a.push_back(uv);
      }
      if (f.useTimeIncrement) {
        a.emplace_back("dt");
      }
      return a;
    }();
    const auto asvn = bd.getExternalNames(bd.getAuxiliaryStateVariables());
    for (auto pa = std::begin(args); pa != std::end(args);) {
      if (*pa == "dt") {
        out << "this->dt";
        ++pa;
        continue;
      }
      const auto a = md.decomposeVariableName(*pa);
      const auto& ea = md.getVariableDescription(a.first).getExternalName();
      if (ea == bd.getExternalName(vs)) {
        throw_if(a.second != 1, "invalid depth for variable '" + a.first +
                                    "' "
                                    "in model '" +
                                    md.className + "'");
        out << "this->" << vs;
      } else if (std::find(std::begin(asvn), std::end(asvn), ea) !=
                 std::end(asvn)) {
        const auto& av =
            bd.getAuxiliaryStateVariableDescriptionByExternalName(ea);
        throw_if(!av.getAttribute<bool>("ComputedByExternalModel", false),
                 "only auxiliary state variable computed by a model are "
                 "allowed here");
        write_variable(av.name, a.second);
      } else {
        const auto& en =
            bd.getExternalStateVariableDescriptionByExternalName(ea);
        write_variable(en.name, a.second);
      }
      if (++pa != std::end(args)) {
        out << ",";
      }
    }
    out << ");\n";
  }

  BehaviourDSLCommon::CodeBlockOptions::CodeBlockOptions()
      : p(BehaviourData::BODY), m(BehaviourData::CREATE) {
    this->hypotheses.insert(ModellingHypothesis::UNDEFINEDHYPOTHESIS);
  }  // end of CodeBlockOptions::CodeBlockOptions

  BehaviourDSLCommon::CodeBlockOptions::~CodeBlockOptions() = default;

  const BehaviourDescription& BehaviourDSLCommon::getBehaviourDescription()
      const {
    return this->mb;
  }  // end of getBehaviourDescription

  void BehaviourDSLCommon::addExternalMFrontFile(
      const std::string& f, const std::vector<std::string>& vinterfaces) {
    this->mb.addExternalMFrontFile(f, vinterfaces);
  }  // end of addExternalMFrontFile

  const MaterialKnowledgeDescription&
  BehaviourDSLCommon::getMaterialKnowledgeDescription() const {
    return this->mb;
  }  // end of getMaterialKnowledgeDescription

  std::string BehaviourDSLCommon::getClassName() const {
    return this->mb.getClassName();
  }  // end of getClassName

  void BehaviourDSLCommon::addMaterialLaw(const std::string& m) {
    this->mb.addMaterialLaw(m);
  }  // end of addMaterialLaw

  void BehaviourDSLCommon::appendToIncludes(const std::string& c) {
    this->mb.appendToIncludes(c);
  }  // end of appendToIncludes

  void BehaviourDSLCommon::appendToMembers(const std::string& c) {
    this->mb.appendToMembers(ModellingHypothesis::UNDEFINEDHYPOTHESIS, c, true);
  }  // end of appendToMembers

  void BehaviourDSLCommon::appendToPrivateCode(const std::string& c) {
    this->mb.appendToPrivateCode(ModellingHypothesis::UNDEFINEDHYPOTHESIS, c,
                                 true);
  }  // end of appendToPrivateCode

  void BehaviourDSLCommon::appendToSources(const std::string& c) {
    this->mb.appendToSources(c);
  }  // end of appendToSources

  void BehaviourDSLCommon::appendToHypothesesList(std::set<Hypothesis>& h,
                                                  const std::string& v) const {
    if (v == ".+") {
      const auto& ash = ModellingHypothesis::getModellingHypotheses();
      for (const auto& lh : ash) {
        this->appendToHypothesesList(h, ModellingHypothesis::toString(lh));
      }
    } else {
      const auto nh = ModellingHypothesis::fromString(v);
      if (!this->isModellingHypothesisSupported(nh)) {
        this->throwRuntimeError(
            "BehaviourDSLCommon::appendToHypothesesList",
            "hypothesis '" + v + "' is not supported by this parser");
      }
      if (this->mb.areModellingHypothesesDefined()) {
        const auto& bh = this->mb.getModellingHypotheses();
        if (bh.find(nh) == bh.end()) {
          this->throwRuntimeError(
              "BehaviourDSLCommon::appendToHypothesesList",
              "hypothesis '" + v +
                  "' is not supported by the "
                  "behaviour (This means that one of the "
                  "'@ModellingHypothesis' or '@ModellingHypotheses'"
                  "keyword was used earlier)");
        }
      }
      if (!h.insert(nh).second) {
        this->throwRuntimeError("BehaviourDSLCommon::appendToHypothesesList",
                                "hypothesis '" + v + "' multiply defined");
      }
    }
  }  // end of appendToHypothesesList

  void BehaviourDSLCommon::getSymbols(
      std::map<std::string, std::string>& symbols,
      const Hypothesis h,
      const std::string&) {
    addSymbol(symbols, "I\u2082", "Stensor::Id()");
    addSymbol(symbols, "I\u2084", "Stensor4::Id()");
    addSymbol(symbols, "\u2297", "^");
    addSymbol(symbols, "\u22C5", "*");
    this->mb.getSymbols(symbols, h);
  }  // end of getSymbols

  void BehaviourDSLCommon::readCodeBlockOptions(CodeBlockOptions& o,
                                                const bool s) {
    using namespace tfel::utilities;
    using namespace tfel::material;
    auto cposition = false;
    auto cmode = false;
    const auto dh = [this] {
      if (this->mb.areModellingHypothesesDefined()) {
        const auto mh = this->mb.getModellingHypotheses();
        if (mh.size() == 1) {
          return *(mh.begin());
        }
      }
      return ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    }();
    o.hypotheses.clear();
    if (this->current == this->tokens.end()) {
      o.hypotheses.insert(dh);
      return;
    }
    if (this->current->value != "<") {
      o.hypotheses.insert(dh);
      return;
    }
    auto options = std::vector<Token>{};
    this->readList(options, "BehaviourDSLCommon::readCodeBlockOptions", "<",
                   ">", true);
    for (const auto& t : options) {
      if (t.value == "Append") {
        if (cmode) {
          this->throwRuntimeError("BehaviourDSLCommon::readCodeBlockOptions",
                                  "insertion mode already specificed");
        }
        cmode = true;
        o.m = BehaviourData::CREATEORAPPEND;
      } else if (t.value == "Replace") {
        if (cmode) {
          this->throwRuntimeError("BehaviourDSLCommon::readCodeBlockOptions",
                                  "insertion mode already specificed");
        }
        cmode = true;
        o.m = BehaviourData::CREATEORREPLACE;
      } else if (t.value == "Create") {
        if (cmode) {
          this->throwRuntimeError("BehaviourDSLCommon::readCodeBlockOptions",
                                  "insertion mode already specificed");
        }
        cmode = true;
        o.m = BehaviourData::CREATE;
      } else if (t.value == "Body") {
        if (cposition) {
          this->throwRuntimeError("BehaviourDSLCommon::readCodeBlockOptions",
                                  "insertion position already specificed");
        }
        cposition = true;
        o.p = BehaviourData::BODY;
      } else if (t.value == "AtBeginning") {
        if (cposition) {
          this->throwRuntimeError("BehaviourDSLCommon::readCodeBlockOptions",
                                  "insertion position already specificed");
        }
        cposition = true;
        o.p = BehaviourData::AT_BEGINNING;
      } else if (t.value == "AtEnd") {
        if (cposition) {
          this->throwRuntimeError("BehaviourDSLCommon::readCodeBlockOptions",
                                  "insertion position already specificed");
        }
        cposition = true;
        o.p = BehaviourData::AT_END;
      } else if ((t.flag == Token::String) &&
                 (t.value.substr(1, t.value.size() - 2) == "+")) {
        this->appendToHypothesesList(o.hypotheses,
                                     t.value.substr(1, t.value.size() - 2));
      } else if (ModellingHypothesis::isModellingHypothesis(t.value)) {
        this->appendToHypothesesList(o.hypotheses, t.value);
      } else {
        o.untreated.push_back(t);
      }
    }
    if (o.hypotheses.empty()) {
      o.hypotheses.insert(dh);
    }
    // checks
    if (!s) {
      if (o.hypotheses.size() != 1u) {
        this->throwRuntimeError("BehaviourDSLCommon::readCodeBlockOptions: ",
                                "specialisation is not allowed");
      }
      if (*(o.hypotheses.begin()) != dh) {
        this->throwRuntimeError("BehaviourDSLCommon::readCodeBlockOptions: ",
                                "specialisation is not allowed");
      }
    }
  }  // end of readCodeBlockOptions

  std::shared_ptr<MaterialPropertyDescription>
  BehaviourDSLCommon::handleMaterialPropertyDescription(const std::string& f) {
    return DSLBase::handleMaterialPropertyDescription(f);
  }  // end of handleMaterialPropertyDescription

  ModelDescription BehaviourDSLCommon::getModelDescription(
      const std::string& f) {
    if (getVerboseMode() >= VERBOSE_DEBUG) {
      getLogStream() << "BehaviourDSLCommon::getModelDescription: "
                     << "treating file '" << f << "'\n";
    }
    const auto& global_options =
        GlobalDomainSpecificLanguageOptionsManager::get();
    const auto path = SearchPathsHandler::search(f);
    auto dsl = ModelDSL{tfel::utilities::merge(
        global_options.getModelDSLOptions(), this->buildDSLOptions(), true)};
    // getting informations the source files
    try {
      dsl.setInterfaces({"mfront"});
      dsl.analyseFile(path, {}, {});
      const auto t = dsl.getTargetsDescription();
      if (!t.specific_targets.empty()) {
        this->throwRuntimeError("BehaviourDSLCommon::getModelDescription",
                                "error while treating file '" + f +
                                    "'.\n"
                                    "Specific targets are not supported");
      }
      for (const auto& h : t.headers) {
        this->appendToIncludes("#include\"" + h + "\"");
      }
      this->atds.push_back(std::move(t));
      this->mb.addExternalMFrontFile(path, {"mfront"});
    } catch (std::exception& e) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::getModelDescription",
          "error while treating file '" + f + "'\n" + std::string(e.what()));
    } catch (...) {
      this->throwRuntimeError("BehaviourDSLCommon::getModelDescription",
                              "error while treating file '" + f + "'");
    }
    const auto& md = dsl.getModelDescription();
    this->reserveName(md.className);
    if (getVerboseMode() >= VERBOSE_DEBUG) {
      getLogStream() << "BehaviourDSLCommon::getModelDescription: "
                     << "end of file '" << f << "' treatment\n";
    }
    return md;
  }  // end of getModelDescription

  void BehaviourDSLCommon::declareMainVariables() {
    decltype(this->gradients.size()) n =
        std::min(this->gradients.size(), this->thermodynamic_forces.size());
    while (n != 0) {
      this->mb.addMainVariable(this->gradients.front(),
                               this->thermodynamic_forces.front());
      this->gradients.erase(this->gradients.begin());
      this->thermodynamic_forces.erase(this->thermodynamic_forces.begin());
      --n;
    }
  }  // end of declareMainVariables

  void BehaviourDSLCommon::treatGradient() {
    VariableDescriptionContainer ngradients;
    this->readVarList(ngradients, true);
    std::for_each(ngradients.begin(), ngradients.end(),
                  [this](const VariableDescription& v) {
                    Gradient g(v);
                    Gradient::setIsIncrementKnownAttribute(g, true);
                    this->gradients.emplace_back(std::move(g));
                  });
    this->declareMainVariables();
  }  // end of treatGradient

  void BehaviourDSLCommon::treatThermodynamicForce() {
    VariableDescriptionContainer ntfs;
    this->readVarList(ntfs, true);
    std::for_each(ntfs.begin(), ntfs.end(),
                  [this](const VariableDescription& f) {
                    this->thermodynamic_forces.emplace_back(f);
                  });
    this->declareMainVariables();
  }  // end of treatThermodynamicForce

  void BehaviourDSLCommon::treatSpeedOfSound() {
    this->treatCodeBlock(*this, BehaviourData::ComputeSpeedOfSound,
                         &BehaviourDSLCommon::standardModifier, true, true);
  }  // end of treatSpeedOfSound

  void BehaviourDSLCommon::treatTangentOperatorBlock() {
    const char* const m = "BehaviourDSLCommon::treatTangentOperatorBlock";
    this->checkNotEndOfFile(m);
    const auto b = this->current->value;
    ++(this->current);
    this->checkNotEndOfFile(m);
    this->readSpecifiedToken(m, ";");
    this->mb.setTangentOperatorBlocks(std::vector<std::string>(1u, b));
  }  // end of treatTangentOperatorBlock

  void BehaviourDSLCommon::treatTangentOperatorBlocks() {
    const char* const m = "BehaviourDSLCommon::treatTangentOperatorBlocks";
    this->checkNotEndOfFile(m);
    auto values = std::vector<tfel::utilities::Token>{};
    this->checkNotEndOfFile(m);
    this->readList(values, m, "{", "}", false);
    this->checkNotEndOfFile(m);
    this->readSpecifiedToken(m, ";");
    auto blocks = std::vector<std::string>{};
    for (const auto& v : values) {
      blocks.push_back(v.value);
    }
    this->mb.setTangentOperatorBlocks(blocks);
  }  // end of treatTangentOperatorBlocks

  void BehaviourDSLCommon::treatAdditionalTangentOperatorBlock() {
    const char* const m =
        "BehaviourDSLCommon::treatAdditionalTangentOperatorBlock";
    this->checkNotEndOfFile(m);
    const auto b = this->current->value;
    ++(this->current);
    this->checkNotEndOfFile(m);
    this->readSpecifiedToken(m, ";");
    this->mb.addTangentOperatorBlock(b);
  }  // end of treatAdditionalTangentOperatorBlock

  void BehaviourDSLCommon::treatAdditionalTangentOperatorBlocks() {
    const char* const m =
        "BehaviourDSLCommon::treatAdditionalTangentOperatorBlocks";
    this->checkNotEndOfFile(m);
    auto values = std::vector<tfel::utilities::Token>{};
    this->checkNotEndOfFile(m);
    this->readList(values, m, "{", "}", false);
    this->checkNotEndOfFile(m);
    this->readSpecifiedToken(m, ";");
    for (const auto& v : values) {
      this->mb.addTangentOperatorBlock(v.value);
    }
  }  // end of treatAdditionalTangentOperatorBlock

  void BehaviourDSLCommon::treatModel() {
    if (getVerboseMode() >= VERBOSE_DEBUG) {
      getLogStream() << "BehaviourDSLCommon::treatModel: begin\n";
    }
    auto md = this->getModelDescription(
        this->readString("BehaviourDSLCommon::treatModel"));
    this->mb.addModelDescription(md);
    if (getVerboseMode() >= VERBOSE_DEBUG) {
      getLogStream() << "BehaviourDSLCommon::treatModel: end\n";
    }
    this->readSpecifiedToken("BehaviourDSLCommon::treatModel", ";");
  }  // end of treatModel

  void BehaviourDSLCommon::treatModel2() {
    if (getVerboseMode() >= VERBOSE_DEBUG) {
      getLogStream() << "BehaviourDSLCommon::treatModel2: begin\n";
    }
    this->checkNotEndOfFile("BehaviourDSLCommon::treatModel2",
                            "Unexpected end of file.");
    if (current->flag == tfel::utilities::Token::String) {
      this->treatModel();
    } else {
      this->treatBehaviour();
    }
    if (getVerboseMode() >= VERBOSE_DEBUG) {
      getLogStream() << "BehaviourDSLCommon::treatModel2: end\n";
    }
  }  // end of treatModel

  void BehaviourDSLCommon::treatUnsupportedCodeBlockOptions(
      const CodeBlockOptions& o) {
    if (o.untreated.empty()) {
      return;
    }
    std::ostringstream msg;
    if (o.untreated.size() == 1u) {
      msg << "option '" << o.untreated[0].value << "' is invalid";
    } else {
      msg << "the";
      for (const auto& opt : o.untreated) {
        msg << " '" << opt.value << "'";
      }
      msg << " options are invalid";
    }
    this->throwRuntimeError(
        "BehaviourDSLCommon::"
        "treatUnsupportedCodeBlockOptions",
        msg.str());
  }  // end of treatUnsupportedCodeBlockOptions

  void BehaviourDSLCommon::addStaticVariableDescription(
      const StaticVariableDescription& v) {
    this->mb.addStaticVariable(ModellingHypothesis::UNDEFINEDHYPOTHESIS, v);
  }  // end of addStaticVariableDescription

  std::map<std::string, int> BehaviourDSLCommon::getIntegerConstants() const {
    return this->mb.getIntegerConstants(
        ModellingHypothesis::UNDEFINEDHYPOTHESIS);
  }

  int BehaviourDSLCommon::getIntegerConstant(const std::string& n) const {
    return this->mb.getIntegerConstant(ModellingHypothesis::UNDEFINEDHYPOTHESIS,
                                       n);
  }  // end of getIntegerConstant

  std::set<BehaviourDSLCommon::Hypothesis>
  BehaviourDSLCommon::getDefaultModellingHypotheses() const {
    // see the method documentation
    return {ModellingHypothesis::AXISYMMETRICALGENERALISEDPLANESTRAIN,
            ModellingHypothesis::AXISYMMETRICAL,
            ModellingHypothesis::PLANESTRAIN,
            ModellingHypothesis::GENERALISEDPLANESTRAIN,
            ModellingHypothesis::TRIDIMENSIONAL};
  }  // end of getDefaultModellingHypotheses

  bool BehaviourDSLCommon::isModellingHypothesisSupported(
      const Hypothesis h) const {
    const auto mhs =
        this->getBehaviourDSLDescription().supportedModellingHypotheses;
    return std::find(mhs.cbegin(), mhs.cend(), h) != mhs.end();
  }  // end of isModellingHypothesesSupported

  std::string BehaviourDSLCommon::getBehaviourFileName() const {
    return "TFEL/Material/" + this->mb.getClassName() + ".hxx";
  }  // end of getBehaviourFileName

  std::string BehaviourDSLCommon::getBehaviourDataFileName() const {
    return "TFEL/Material/" + this->mb.getClassName() + "BehaviourData.hxx";
  }  // end of getBehaviourDataFileName

  std::string BehaviourDSLCommon::getIntegrationDataFileName() const {
    return "TFEL/Material/" + this->mb.getClassName() + "IntegrationData.hxx";
  }  // end of getIntegrationDataFileName

  bool BehaviourDSLCommon::isSrcFileRequired() const {
    const auto profiling =
        this->mb.getAttribute(BehaviourData::profiling, false);
    const auto parameters =
        ((this->mb.hasParameters()) &&
         (!areParametersTreatedAsStaticVariables(this->mb)));
    const auto user_defined_sources = !this->mb.getSources().empty();
    return profiling || parameters || user_defined_sources;
  }  // end of isSrcFileRequired

  std::string BehaviourDSLCommon::getSrcFileName() const {
    return this->mb.getClassName() + ".cxx";
  }  // end of getSrcFileName

  void BehaviourDSLCommon::analyseFile(
      const std::string& fileName_,
      const std::vector<std::string>& ecmds,
      const std::map<std::string, std::string>& s) {
    this->importFile(fileName_, ecmds, s);
    // Adding some stuff
    this->endsInputFileProcessing();
    // setting the name of the output files
    // targets description
    for (const auto& i : this->interfaces) {
      i.second->getTargetsDescription(this->td, this->mb);
    }
    if (this->isSrcFileRequired()) {
      for (auto& l : this->td.libraries) {
        insert_if(this->td.getLibrary(l.name).sources, this->getSrcFileName());
      }
    }
    insert_if(this->td.headers, this->getBehaviourFileName());
    insert_if(this->td.headers, this->getBehaviourDataFileName());
    insert_if(this->td.headers, this->getIntegrationDataFileName());
    this->completeTargetsDescription();
  }

  void BehaviourDSLCommon::disableVariableDeclaration() {
    if (this->mb.allowsNewUserDefinedVariables()) {
      this->completeVariableDeclaration();
      this->mb.disallowNewUserDefinedVariables();
    }
  }  // end of disableVariableDeclaration

  void BehaviourDSLCommon::completeVariableDeclaration() {
    using namespace mfront::bbrick;
    if (getVerboseMode() >= VERBOSE_DEBUG) {
      getLogStream()
          << "BehaviourDSLCommon::completeVariableDeclaration: begin\n";
    }
    if ((!this->gradients.empty()) || (!this->thermodynamic_forces.empty())) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::completeVariableDeclaration",
          "The number of gradients does not match the number of "
          "thermodynamic forces");
    }
    // defining modelling hypotheses
    if (!this->mb.areModellingHypothesesDefined()) {
      auto dmh = this->getDefaultModellingHypotheses();
      // taking into account restrictin du to the `Plate` othotropic
      // axes convention
      if ((this->mb.getSymmetryType() == mfront::ORTHOTROPIC) &&
          (this->mb.getOrthotropicAxesConvention() ==
           OrthotropicAxesConvention::PLATE)) {
        for (auto ph = dmh.begin(); ph != dmh.end();) {
          if ((*ph != ModellingHypothesis::TRIDIMENSIONAL) &&
              (*ph != ModellingHypothesis::PLANESTRESS) &&
              (*ph != ModellingHypothesis::PLANESTRAIN) &&
              (*ph != ModellingHypothesis::GENERALISEDPLANESTRAIN)) {
            ph = dmh.erase(ph);
          } else {
            ++ph;
          }
        }
      }
      this->mb.setModellingHypotheses(dmh);
    }
    const auto& mh = this->mb.getModellingHypotheses();
    // treating bricks
    for (const auto& pb : this->bricks) {
      pb->completeVariableDeclaration();
    }
    if (getVerboseMode() >= VERBOSE_DEBUG) {
      auto& log = getLogStream();
      log << "behaviour '" << this->mb.getClassName()
          << "' supports the following hypotheses: \n";
      for (const auto& h : mh) {
        log << " - " << ModellingHypothesis::toString(h);
        if (this->mb.hasSpecialisedMechanicalData(h)) {
          log << " (specialised)";
        }
        log << '\n';
      }
    }
    // time step scaling factors
    if (!this->mb.hasParameter(ModellingHypothesis::UNDEFINEDHYPOTHESIS,
                               "minimal_time_step_scaling_factor")) {
      VariableDescription e("time", "minimal_time_step_scaling_factor", 1u, 0u);
      e.description = "minimal value for the time step scaling factor";
      this->mb.addParameter(ModellingHypothesis::UNDEFINEDHYPOTHESIS, e,
                            BehaviourData::ALREADYREGISTRED);
      this->mb.setParameterDefaultValue(
          ModellingHypothesis::UNDEFINEDHYPOTHESIS,
          "minimal_time_step_scaling_factor", 0.1);
      this->mb.setEntryName(ModellingHypothesis::UNDEFINEDHYPOTHESIS,
                            "minimal_time_step_scaling_factor",
                            "minimal_time_step_scaling_factor");
    }
    if (!this->mb.hasParameter(ModellingHypothesis::UNDEFINEDHYPOTHESIS,
                               "maximal_time_step_scaling_factor")) {
      VariableDescription e("time", "maximal_time_step_scaling_factor", 1u, 0u);
      e.description = "maximal value for the time step scaling factor";
      this->mb.addParameter(ModellingHypothesis::UNDEFINEDHYPOTHESIS, e,
                            BehaviourData::ALREADYREGISTRED);
      this->mb.setParameterDefaultValue(
          ModellingHypothesis::UNDEFINEDHYPOTHESIS,
          "maximal_time_step_scaling_factor",
          std::numeric_limits<double>::max());
      this->mb.setEntryName(ModellingHypothesis::UNDEFINEDHYPOTHESIS,
                            "maximal_time_step_scaling_factor",
                            "maximal_time_step_scaling_factor");
    }
    // incompatible options
    if ((this->mb.getAttribute(BehaviourDescription::computesStiffnessTensor,
                               false)) &&
        (this->mb.getAttribute(BehaviourDescription::requiresStiffnessTensor,
                               false))) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::completeVariableDeclaration",
          "internal error, incompatible options for stiffness tensor");
    }
    // check of stiffness tensor requirement
    if ((this->mb.getBehaviourType() ==
         BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) ||
        (this->mb.getBehaviourType() ==
         BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR)) {
      if ((mh.find(ModellingHypothesis::PLANESTRESS) != mh.end()) ||
          (mh.find(ModellingHypothesis::AXISYMMETRICALGENERALISEDPLANESTRESS) !=
           mh.end())) {
        if (this->mb.getAttribute(BehaviourDescription::requiresStiffnessTensor,
                                  false)) {
          if (!this->mb.hasAttribute(
                  BehaviourDescription::requiresUnAlteredStiffnessTensor)) {
            this->throwRuntimeError(
                "BehaviourDSLCommon::completeVariableDeclaration",
                "No option was given to the '@RequireStiffnessTensor' "
                "keyword.\n"
                "For plane stress hypotheses, it is required to precise "
                "whether "
                "the expected stiffness tensor is 'Altered' (the plane stress "
                "hypothesis is taken into account) or 'UnAltered' (the "
                "stiffness "
                "tensor is the same as in plane strain)");
          }
        }
      }
    }
    if (this->mb.getSymmetryType() == mfront::ORTHOTROPIC) {
      // if no orthotropic axes convention is defined, one can't compute
      // stiffness tensor, thermal expansion or stress free expansion
      // correctly, except for the 3D modelling hypothesis
      for (const auto h : this->mb.getDistinctModellingHypotheses()) {
        if (((this->mb.areElasticMaterialPropertiesDefined()) &&
             (this->mb.getElasticMaterialProperties().size() == 9u)) ||
            ((this->mb.areThermalExpansionCoefficientsDefined()) &&
             (this->mb.getThermalExpansionCoefficients().size() == 3u)) ||
            (this->mb.isStressFreeExansionAnisotropic(h))) {
          if (this->mb.getOrthotropicAxesConvention() ==
              OrthotropicAxesConvention::DEFAULT) {
            // in this case, only tridimensional case is supported
            if (h != ModellingHypothesis::TRIDIMENSIONAL) {
              this->throwRuntimeError(
                  "BehaviourDSLCommon::completeVariableDeclaration",
                  "An orthotropic axes convention must be choosen when "
                  "using one of @ComputeStiffnessTensor, "
                  "@ComputeThermalExpansion, @Swelling, @AxilalGrowth keywords "
                  "in behaviours which "
                  "shall be valid in other modelling hypothesis than "
                  "'Tridimensional'. This message was triggered because "
                  "either the thermal expansion or to the stiffness tensor "
                  "is orthotropic.\n"
                  "Either restrict the validity of the behaviour to "
                  "'Tridimensional' (see @ModellingHypothesis) or "
                  "choose and orthotropic axes convention as on option "
                  "to the @OrthotropicBehaviour keyword");
            }
          }
        }
      }
    }
    if (getVerboseMode() >= VERBOSE_DEBUG) {
      getLogStream()
          << "BehaviourDSLCommon::completeVariableDeclaration: end\n";
    }
  }  // end of completeVariableDeclaration

  void BehaviourDSLCommon::endsInputFileProcessing() {
    if (getVerboseMode() >= VERBOSE_DEBUG) {
      getLogStream() << "BehaviourDSLCommon::endsInputFileProcessing: begin\n";
    }
    this->disableVariableDeclaration();
    // restrictions on user defined compute stress free expansion
    for (const auto h : this->mb.getDistinctModellingHypotheses()) {
      const auto& d = this->mb.getBehaviourData(h);
      if (d.hasCode(BehaviourData::ComputeStressFreeExpansion)) {
        const auto& cb =
            d.getCodeBlock(BehaviourData::ComputeStressFreeExpansion);
        for (const auto& v : cb.members) {
          if (d.isLocalVariableName(v)) {
            this->throwRuntimeError(
                "BehaviourDSLCommon::"
                "endsInputFileProcessing: ",
                "local variables can't be used in "
                "@ComputeStressFreeExpansion blocks "
                "(local variables are not initialized yet "
                "when the stress free expansions "
                "are computed)");
          }
        }
      }
    }
    if (this->mb.areSlipSystemsDefined()) {
      this->mb.appendToIncludes("#include \"TFEL/Material/" +
                                this->mb.getClassName() + "SlipSystems.hxx\"");
    }
    //
    if (this->mb.getBehaviourType() ==
        BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) {
      const auto mhs = this->getModellingHypothesesToBeTreated();
      for (const auto h :
           {ModellingHypothesis::PLANESTRESS,
            ModellingHypothesis::AXISYMMETRICALGENERALISEDPLANESTRESS}) {
        if (mhs.find(h) != mhs.end()) {
          if (!this->mb.hasSpecialisedMechanicalData(h)) {
            this->mb.specialize(h);
          }
        }
      }
    }
    // calling interfaces
    if (getPedanticMode()) {
      this->doPedanticChecks();
    }
    for (const auto& pb : this->bricks) {
      pb->endTreatment();
    }
    if (getVerboseMode() >= VERBOSE_DEBUG) {
      getLogStream() << "BehaviourDSLCommon::endsInputFileProcessing: end\n";
    }
  }  // end of endsInputFileProcessing

  /*!
   * \return the "true" integration variables (state variables are excluded)
   * \param[in] md : mechanical behaviour data
   */
  static VarContainer getIntegrationVariables(const BehaviourData& md) {
    const auto& ivs = md.getIntegrationVariables();
    VarContainer v;
    for (const auto& iv : ivs) {
      if (!md.isStateVariableName(iv.name)) {
        v.push_back(iv);
      }
    }
    return v;
  }  // end of getIntegrationVariables

  /*!
   * \brief various checks
   * \param[in] v  : variables
   * \param[in] t  : variable type
   * \param[in] uv : list of all used variables
   * \param[in] b1 : check if the variable is used
   * \param[in] b2 : check if the variable increment (or rate) is used
   * \param[in] b3 : check if glossary name is declared
   * \param[in] b4 : check if variable is used in more than one code block (test
   * for local variables)
   */
  static void performPedanticChecks(
      const BehaviourData& md,
      const VarContainer& v,
      const std::string& t,
      const std::map<std::string, unsigned short>& uv,
      const bool b1 = true,
      const bool b2 = true,
      const bool b3 = true,
      const bool b4 = false) {
    using namespace tfel::glossary;
    const auto& glossary = Glossary::getGlossary();
    auto& log = getLogStream();
    for (const auto& vd : v) {
      if (b1) {
        const auto p = uv.find(vd.name);
        if (p == uv.end()) {
          log << "- " << t << " '" << vd.name << "' is unused.\n";
        } else {
          if (b4 && p->second == 1) {
            log << "- " << t << " '" << vd.name
                << "' is used in one code block only.\n";
          }
        }
      }
      if (b2) {
        if (uv.find("d" + vd.name) == uv.end()) {
          log << "- " << t << " increment 'd" << vd.name << "' is unused.\n";
        }
      }
      if (b3) {
        if ((!md.hasGlossaryName(vd.name)) && (!md.hasEntryName(vd.name))) {
          log << "- " << t << " '" << vd.name << "' has no glossary name.\n";
        }
      }
      if (vd.description.empty()) {
        auto hasDoc = false;
        if (md.hasGlossaryName(vd.name)) {
          const auto& e =
              glossary.getGlossaryEntry(md.getExternalName(vd.name));
          hasDoc = (!e.getShortDescription().empty()) ||
                   (!e.getDescription().empty());
        }
        if (!hasDoc) {
          log << "- " << t << " '" << vd.name << "' has no description.\n";
        }
      }
    }
  }

  /*!
   * \brief various checks on static variables
   * \param[in] v  : variables
   * \param[in] uv : list of all static variables
   */
  static void performPedanticChecks(
      const StaticVarContainer& v,
      const std::map<std::string, unsigned short>& uv) {
    auto& log = getLogStream();
    for (const auto& vd : v) {
      if (uv.find(vd.name) == uv.end()) {
        log << "- static variable '" << vd.name << "' is unused.\n";
      }
    }
  }

  void BehaviourDSLCommon::doPedanticChecks() const {
    const auto& hs = this->mb.getDistinctModellingHypotheses();
    auto& log = getLogStream();
    log << "\n* Pedantic checks\n";
    for (auto h : hs) {
      const auto& md = this->mb.getBehaviourData(h);
      // checks if variables are used
      if (h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
        log << "\n** Beginning pedantic checks for default modelling "
               "hypothesis\n\n";
      } else {
        log << "\n** Beginning pedantic checks for modelling hypothesis '"
            << ModellingHypothesis::toString(h) << "'\n\n";
      }
      // getting all used variables
      const auto& cbs = md.getCodeBlockNames();
      auto members =
          std::map<std::string, unsigned short>{};  // variable names and counts
      auto smembers =
          std::map<std::string,
                   unsigned short>{};  // static variable nanes and counts
      for (const auto& cbs_pcbs : cbs) {
        const auto& cb = md.getCodeBlock(cbs_pcbs);
        if (cb.description.empty()) {
          log << "- code block '" << cbs_pcbs << "' has no description\n";
        }
        for (const auto& v : cb.members) {
          if (members.count(v) == 0) {
            members[v] = 1;
          } else {
            ++(members[v]);
          }
        }
        for (const auto& v : cb.staticMembers) {
          if (smembers.count(v) == 0) {
            smembers[v] = 1;
          } else {
            ++(smembers[v]);
          }
        }
      }
      performPedanticChecks(md, md.getMaterialProperties(), "material property",
                            members, true, false, true);
      const auto& ivs = getIntegrationVariables(md);
      performPedanticChecks(md, ivs, "integration variable", members, false,
                            true, false);
      performPedanticChecks(md, md.getStateVariables(), "state variable",
                            members);
      performPedanticChecks(md, md.getAuxiliaryStateVariables(),
                            "auxiliary state variable", members, true, false);
      performPedanticChecks(md, md.getExternalStateVariables(),
                            "external state variable", members);
      performPedanticChecks(md, md.getLocalVariables(), "local variable",
                            members, true, false, false, true);
      performPedanticChecks(md, md.getParameters(), "parameter", members, true,
                            false);
      performPedanticChecks(md.getStaticVariables(), smembers);
    }
    log << "\n# End of pedantic checks\n";
  }  // end of pedanticChecks

  std::set<BehaviourDSLCommon::Hypothesis>
  BehaviourDSLCommon::getModellingHypothesesToBeTreated() const {
    // modelling hypotheses handled by the interfaces (if at least one
    // interface is defined), or by the behaviour
    std::set<Hypothesis> mhs;
    if (this->interfaces.empty()) {
      const auto& bh = this->mb.getModellingHypotheses();
      mhs.insert(bh.begin(), bh.end());
    } else {
      // calling the interfaces
      for (const auto& i : this->interfaces) {
        const auto& ih = i.second->getModellingHypothesesToBeTreated(this->mb);
        mhs.insert(ih.begin(), ih.end());
      }
    }
    return mhs;
  }  // end of getModellingHypothesesToBeTreated

  void BehaviourDSLCommon::generateOutputFiles() {
    tfel::system::systemCall::mkdir("src");
    tfel::system::systemCall::mkdir("include");
    tfel::system::systemCall::mkdir("include/TFEL/");
    tfel::system::systemCall::mkdir("include/TFEL/Material");
    //! generating sources du to external material properties and models
    std::ofstream behaviourFile("include/" + this->getBehaviourFileName());
    std::ofstream behaviourDataFile("include/" +
                                    this->getBehaviourDataFileName());
    std::ofstream integrationDataFile("include/" +
                                      this->getIntegrationDataFileName());
    behaviourFile.precision(14);
    behaviourDataFile.precision(14);
    integrationDataFile.precision(14);
    if (!behaviourFile) {
      this->throwRuntimeError("BehaviourDSLCommon::generateOutputFiles",
                              "unable to open '" +
                                  this->getBehaviourFileName() +
                                  "' "
                                  "for writing output file");
    }
    if (!behaviourDataFile) {
      this->throwRuntimeError("BehaviourDSLCommon::generateOutputFiles",
                              "unable to open '" +
                                  this->getBehaviourDataFileName() +
                                  "' "
                                  "for writing output file");
    }
    if (!integrationDataFile) {
      this->throwRuntimeError("BehaviourDSLCommon::generateOutputFiles",
                              "unable to open '" +
                                  this->getIntegrationDataFileName() +
                                  "' "
                                  "for writing output file");
    }
    for (const auto& em : this->mb.getExternalMFrontFiles()) {
      this->callMFront(em.second, {em.first});
    }
    auto write_classes = [this, &behaviourFile, &behaviourDataFile,
                          &integrationDataFile](const Hypothesis h) {
      const auto n = h == ModellingHypothesis::UNDEFINEDHYPOTHESIS
                         ? "default hypothesis"
                         : "'" + ModellingHypothesis::toString(h) + "'";
      if (getVerboseMode() >= VERBOSE_DEBUG) {
        auto& log = getLogStream();
        log << "BehaviourDSLCommon::generateOutputFiles: "
            << "treating " + n + "\n";
      }
      // Generating BehaviourData's outputClass
      if (getVerboseMode() >= VERBOSE_DEBUG) {
        auto& log = getLogStream();
        log << "BehaviourDSLCommon::generateOutputFiles: "
            << "writing behaviour data for " + n + "\n";
      }
      this->writeBehaviourDataClass(behaviourDataFile, h);
      // Generating IntegrationData's outputClass
      if (getVerboseMode() >= VERBOSE_DEBUG) {
        auto& log = getLogStream();
        log << "BehaviourDSLCommon::generateOutputFiles: "
            << "writing integration data for " + n + "\n";
      }
      this->writeIntegrationDataClass(integrationDataFile, h);
      // Generating Behaviour's outputFile
      if (getVerboseMode() >= VERBOSE_DEBUG) {
        auto& log = getLogStream();
        log << "BehaviourDSLCommon::generateOutputFiles: "
            << "writing behaviour class for " + n + "\n";
      }
      this->writeBehaviourClass(behaviourFile, h);
    };
    // generate outpout files
    this->writeBehaviourDataFileBegin(behaviourDataFile);
    this->writeIntegrationDataFileBegin(integrationDataFile);
    this->writeBehaviourFileBegin(behaviourFile);
    if (this->mb.areSlipSystemsDefined()) {
      this->generateSlipSystemsFiles();
    }
    const auto mhs = this->getModellingHypothesesToBeTreated();
    if (!this->mb.areAllMechanicalDataSpecialised(mhs)) {
      write_classes(ModellingHypothesis::UNDEFINEDHYPOTHESIS);
    }
    for (const auto& h : mhs) {
      if (mb.hasSpecialisedMechanicalData(h)) {
        write_classes(h);
      }
    }
    this->writeBehaviourDataFileEnd(behaviourDataFile);
    this->writeIntegrationDataFileEnd(integrationDataFile);
    this->writeBehaviourFileEnd(behaviourFile);
    // Generating behaviour's source file
    if (this->isSrcFileRequired()) {
      std::ofstream srcFile("src/" + this->getSrcFileName());
      if (!srcFile) {
        this->throwRuntimeError("BehaviourDSLCommon::generateOutputFiles",
                                "unable to open '" + this->getSrcFileName() +
                                    "' "
                                    "for writing output file");
      }
      srcFile.precision(14);
      if (getVerboseMode() >= VERBOSE_DEBUG) {
        auto& log = getLogStream();
        log << "BehaviourDSLCommon::generateOutputFiles : writing source "
               "file\n";
      }
      this->writeSrcFile(srcFile);
      srcFile.close();
    }
    // calling the interfaces
    for (const auto& i : this->interfaces) {
      if (getVerboseMode() >= VERBOSE_DEBUG) {
        auto& log = getLogStream();
        log << "BehaviourDSLCommon::generateOutputFiles : "
            << "calling interface '" << i.first << "'\n";
      }
      i.second->endTreatment(this->mb, this->fd);
    }
    behaviourFile.close();
    behaviourDataFile.close();
    integrationDataFile.close();
  }

  void BehaviourDSLCommon::generateSlipSystemsFiles() {
    using SlipSystemsDescription = BehaviourDescription::SlipSystemsDescription;
    using vector = SlipSystemsDescription::vector;
    using tensor = SlipSystemsDescription::tensor;
    auto throw_if = [this](const bool b, const std::string& m) {
      if (b) {
        this->throwRuntimeError(
            "FiniteStrainSingleCrystalBrick::"
            "generateSlipSystems",
            m);
      }
    };
    auto write_vector = [](std::ostream& out, const std::string& v,
                           const std::vector<vector>& ts) {
      for (decltype(ts.size()) i = 0; i != ts.size(); ++i) {
        const auto& t = ts[i];
        out << v << "[" << i << "] = vector{"
            << "real(" << t[0] << "),"
            << "real(" << t[1] << "),"
            << "real(" << t[2] << ")};\n";
      }
    };
    auto write_tensor = [](std::ostream& out, const std::string& mu,
                           const std::vector<tensor>& ts) {
      for (decltype(ts.size()) i = 0; i != ts.size(); ++i) {
        const auto& t = ts[i];
        out << mu << "[" << i << "] = tensor{"
            << "real(" << t[0] << "),"
            << "real(" << t[1] << "),"
            << "real(" << t[2] << "),"
            << "real(" << t[3] << "),"
            << "real(" << t[4] << "),"
            << "real(" << t[5] << "),"
            << "real(" << t[6] << "),"
            << "real(" << t[7] << "),"
            << "real(" << t[8] << ")};\n";
      }
    };
    auto write_stensor = [](std::ostream& out, const std::string& mus,
                            const std::vector<tensor>& ts) {
      constexpr const auto cste = tfel::math::Cste<long double>::sqrt2 / 2;
      for (decltype(ts.size()) i = 0; i != ts.size(); ++i) {
        const auto& t = ts[i];
        out << mus << "[" << i << "] = stensor{"
            << "real(" << t[0] << "),"
            << "real(" << t[1] << "),"
            << "real(" << t[2] << "),"
            << "real(" << (t[3] + t[4]) * cste << "),"
            << "real(" << (t[5] + t[6]) * cste << "),"
            << "real(" << (t[7] + t[8]) * cste << ")};\n";
      }
    };
    const auto& sss = this->mb.getSlipSystems();
    const auto nb = sss.getNumberOfSlipSystemsFamilies();
    const auto ims = sss.getInteractionMatrixStructure();
    const auto cn = this->mb.getClassName() + "SlipSystems";
    tfel::system::systemCall::mkdir("include");
    tfel::system::systemCall::mkdir("include/TFEL/");
    tfel::system::systemCall::mkdir("include/TFEL/Material");
    auto file = "include/TFEL/Material/" + cn + ".hxx";
    std::ofstream out(file);
    throw_if(!out, "can't open file '" + file + "'");
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << "/*!\n"
        << "* \\file   " << file << '\n'
        << "* \\brief  "
        << "this file decares the " << cn << " class.\n"
        << "*         File generated by " << MFrontHeader::getVersionName()
        << " "
        << "version " << MFrontHeader::getVersionNumber() << '\n';
    if (!this->fd.authorName.empty()) {
      out << "* \\author " << this->fd.authorName << '\n';
    }
    if (!this->fd.date.empty()) {
      out << "* \\date   " << this->fd.date << '\n';
    }
    out << " */\n\n";
    out << "#ifndef LIB_TFEL_MATERIAL_" << makeUpperCase(cn) << "_HXX\n"
        << "#define LIB_TFEL_MATERIAL_" << makeUpperCase(cn) << "_HXX\n\n"
        << "#if (defined _WIN32 || defined _WIN64)\n"
        << "#ifdef min\n"
        << "#undef min\n"
        << "#endif /* min */\n"
        << "#ifdef max\n"
        << "#undef max\n"
        << "#endif /* max */\n"
        << "#ifdef small\n"
        << "#undef small\n"
        << "#endif /* small */\n"
        << "#endif /* (defined _WIN32 || defined _WIN64) */\n\n"
        << "#include\"TFEL/Raise.hxx\"\n"
        << "#include\"TFEL/Math/tvector.hxx\"\n"
        << "#include\"TFEL/Math/stensor.hxx\"\n"
        << "#include\"TFEL/Math/tensor.hxx\"\n\n"
        << "namespace tfel::material{\n\n"
        << "template<typename real>\n"
        << "struct " << cn << '\n'
        << "{\n"
        << "//! a simple alias\n"
        << "using tensor = tfel::math::tensor<3u,real>;\n"
        << "//! a simple alias\n"
        << "using vector = tfel::math::tvector<3u,real>;\n"
        << "//! a simple alias\n"
        << "using stensor = tfel::math::stensor<3u,real>;\n";
    auto nss = size_type{};
    for (size_type idx = 0; idx != nb; ++idx) {
      nss += sss.getNumberOfSlipSystems(idx);
    }
    if (nb == 1u) {
      const auto nss0 = sss.getNumberOfSlipSystems(0);
      out << "//! number of sliding systems\n"
          << "static constexpr unsigned short Nss"
          << " = " << nss << ";\n"
          << "//! number of sliding systems (first and uniq family)\n"
          << "static constexpr unsigned short Nss0"
          << " = " << nss0 << ";\n";
    } else {
      for (size_type idx = 0; idx != nb; ++idx) {
        out << "//! number of sliding systems\n"
            << "static constexpr unsigned short Nss" << idx << " = "
            << sss.getNumberOfSlipSystems(idx) << ";\n";
      }
      out << "static constexpr unsigned short Nss = ";
      for (size_type idx = 0; idx != nb;) {
        out << "Nss" << idx;
        if (++idx != nb) {
          out << "+";
        }
      }
      out << ";\n";
    }
    out << "//! tensor of directional sense\n"
        << "tfel::math::tvector<Nss,tensor> mu;\n"
        << "//! symmetric tensor of directional sense\n"
        << "tfel::math::tvector<Nss,stensor> mus;\n"
        << "//! normal to slip plane\n"
        << "tfel::math::tvector<Nss,vector> np;\n"
        << "//! unit vector in the slip direction\n"
        << "tfel::math::tvector<Nss,vector> ns;\n";
    for (size_type idx = 0; idx != nb; ++idx) {
      out << "//! tensor of directional sense\n"
          << "tfel::math::tvector<Nss" << idx << ",tensor> mu" << idx << ";\n"
          << "//! symmetric tensor of directional sense\n"
          << "tfel::math::tvector<Nss" << idx << ",stensor> mus" << idx << ";\n"
          << "//! normal to slip plane\n"
          << "tfel::math::tvector<Nss" << idx << ",vector> np" << idx << ";\n"
          << "//! unit vector in the slip direction\n"
          << "tfel::math::tvector<Nss" << idx << ",vector> ns" << idx << ";\n";
    }
    if (this->mb.hasInteractionMatrix()) {
      out << "//! interaction matrix\n"
          << "tfel::math::tmatrix<Nss,Nss,real> mh;\n";
      out << "//! interaction matrix\n"
          << "tfel::math::tmatrix<Nss,Nss,real> him;\n";
    }
    if (this->mb.hasDislocationsMeanFreePathInteractionMatrix()) {
      out << "tfel::math::tmatrix<Nss,Nss,real> dim;\n";
    }
    if (nb != 1u) {
      out << "/*!\n"
          << " * \\return the gobal index of the jth system of ith family\n"
          << " * \\param[in] i: slip system family\n"
          << " * \\param[in] j: local slip system index\n"
          << " */\n"
          << "constexpr unsigned short offset(const unsigned short,\nconst "
             "unsigned short) const;\n";
      for (size_type i = 0; i != nb; ++i) {
        out << "/*!\n"
            << " * \\return the gobal index of the ith system of " << i
            << "th family\n"
            << " * \\param[in] i: local slip system index\n"
            << " */\n"
            << "constexpr unsigned short offset" << i
            << "(const unsigned short) const;\n";
      }
    }
    out << "/*!\n"
        << " * \\return true if two systems are coplanar\n"
        << " * \\param[in] i: first slip system index\n"
        << " * \\param[in] j: second slip system index\n"
        << " */\n"
        << "bool areCoplanar(const unsigned short,\n"
        << "                 const unsigned short) const;\n"
        << "/*!\n"
        << " * \\return an interaction matrix\n"
        << " * \\param[in] m: coefficients of the interaction matrix\n"
        << " */\n"
        << "constexpr tfel::math::tmatrix<Nss, Nss, real>\n"
        << "buildInteractionMatrix("
        << "const tfel::math::fsarray<" << ims.rank() << ", real>&) const;\n"
        << "//! return the unique instance of the class\n"
        << "static const " << cn << "&\n"
        << "getSlidingSystems();\n"
        << "//! return the unique instance of the class\n"
        << "static const " << cn << "&\n"
        << "getSlipSystems();\n"
        << "//! return the unique instance of the class\n"
        << "static const " << cn << "&\n"
        << "getGlidingSystems();\n"
        << "private:\n"
        << "//! Constructor\n"
        << cn << "();\n"
        << "//! move constructor (disabled)\n"
        << cn << "(" << cn << "&&) = delete;\n"
        << "//! copy constructor (disabled)\n"
        << cn << "(const " << cn << "&) = delete;\n"
        << "//! move operator (disabled)\n"
        << cn << "&\n"
        << "operator=(" << cn << "&&) = delete;\n"
        << "//! copy constructor (disabled)\n"
        << cn << "&\n"
        << "operator=(const " << cn << "&) = delete;\n"
        << "}; // end of struct " << cn << "\n\n"
        << "//! a simple alias\n"
        << "template<typename real>\n"
        << "using " << this->mb.getClassName() << "SlidingSystems "
        << "= " << cn << "<real>;\n\n"
        << "//! a simple alias\n"
        << "template<typename real>\n"
        << "using " << this->mb.getClassName() << "GlidingSystems "
        << "= " << cn << "<real>;\n\n"
        << "} // end of namespace tfel::material\n\n"
        << "#include\"TFEL/Material/" << cn << ".ixx\"\n\n"
        << "#endif /* LIB_TFEL_MATERIAL_" << makeUpperCase(cn) << "_HXX */\n";
    out.close();
    file = "include/TFEL/Material/" + cn + ".ixx";
    out.open(file);
    throw_if(!out, "can't open file '" + file + "'");
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.precision(std::numeric_limits<long double>::digits10);
    out << "/*!\n"
        << "* \\file   " << file << '\n'
        << "* \\brief  "
        << "this file implements the " << cn << " class.\n"
        << "*         File generated by " << MFrontHeader::getVersionName()
        << " "
        << "version " << MFrontHeader::getVersionNumber() << '\n';
    if (!this->fd.authorName.empty()) {
      out << "* \\author " << this->fd.authorName << '\n';
    }
    if (!this->fd.date.empty()) {
      out << "* \\date   " << this->fd.date << '\n';
    }
    out << " */\n\n";
    out << "#ifndef LIB_TFEL_MATERIAL_" << makeUpperCase(cn) << "_IXX\n"
        << "#define LIB_TFEL_MATERIAL_" << makeUpperCase(cn) << "_IXX\n\n"
        << "#include\"TFEL/Math/General/MathConstants.hxx\"\n\n"
        << "namespace tfel::material{\n\n"
        << "template<typename real>\n"
        << "const " << cn << "<real>&\n"
        << cn << "<real>::getSlidingSystems(){\n"
        << "static const " << cn << " i;\n"
        << "return i;\n"
        << "} // end of " << cn << "::getSlidingSystems\n\n"
        << "template<typename real>\n"
        << "const " << cn << "<real>&\n"
        << cn << "<real>::getSlipSystems(){\n"
        << "return " << cn << "<real>::getSlidingSystems();\n"
        << "} // end of " << cn << "::getSlipSystems\n\n"
        << "template<typename real>\n"
        << "const " << cn << "<real>&\n"
        << cn << "<real>::getGlidingSystems(){\n"
        << "return " << cn << "<real>::getSlidingSystems();\n"
        << "} // end of " << cn << "::getGlidingSystems\n\n"
        << "template<typename real>\n"
        << cn << "<real>::" << cn << "(){\n";
    std::vector<tensor> gots;
    std::vector<vector> gnss;
    std::vector<vector> gnps;
    // orientation tensors
    for (size_type idx = 0; idx != nb; ++idx) {
      const auto& ots = sss.getOrientationTensors(idx);
      write_tensor(out, "this->mu" + std::to_string(idx), ots);
      gots.insert(gots.end(), ots.begin(), ots.end());
    }
    write_tensor(out, "this->mu", gots);
    // symmetric orientation tensors
    for (size_type idx = 0; idx != nb; ++idx) {
      write_stensor(out, "this->mus" + std::to_string(idx),
                    sss.getOrientationTensors(idx));
    }
    write_stensor(out, "this->mus", gots);
    // normal to slip planes
    for (size_type idx = 0; idx != nb; ++idx) {
      const auto& nps = sss.getSlipPlaneNormals(idx);
      write_vector(out, "this->np" + std::to_string(idx), nps);
      gnps.insert(gnps.end(), nps.begin(), nps.end());
    }
    write_vector(out, "this->np", gnps);
    // slip direction
    for (size_type idx = 0; idx != nb; ++idx) {
      const auto& nss2 = sss.getSlipDirections(idx);
      write_vector(out, "this->ns" + std::to_string(idx), nss2);
      gnss.insert(gnss.end(), nss2.begin(), nss2.end());
    }
    write_vector(out, "this->ns", gnss);

    auto write_imatrix = [&out, &sss, &nb, &nss, &ims](
                             const std::vector<long double>& m,
                             const std::string& n) {
      auto count = size_type{};  // number of terms of the matrix treated so far
      out << "this->" << n << " = {";
      for (size_type idx = 0; idx != nb; ++idx) {
        const auto gsi = sss.getSlipSystems(idx);
        for (size_type idx2 = 0; idx2 != gsi.size(); ++idx2) {
          for (size_type jdx = 0; jdx != nb; ++jdx) {
            const auto gsj = sss.getSlipSystems(jdx);
            for (size_type jdx2 = 0; jdx2 != gsj.size(); ++jdx2) {
              const auto r = ims.getRank(gsi[idx2], gsj[jdx2]);
              out << "real(" << m[r] << ")";
              if (++count != nss * nss) {
                out << ",";
              }
            }
          }
          out << '\n';
        }
      }
      out << "};\n";
    };
    if (this->mb.hasInteractionMatrix()) {
      write_imatrix(sss.getInteractionMatrix(), "mh");
      write_imatrix(sss.getInteractionMatrix(), "him");
    }
    if (this->mb.hasDislocationsMeanFreePathInteractionMatrix()) {
      write_imatrix(sss.getDislocationsMeanFreePathInteractionMatrix(), "dim");
    }
    out << "} // end of " << cn << "::" << cn << "\n\n";
    if (nb != 1u) {
      out << "template<typename real>\n"
          << "constexpr unsigned short\n"
          << cn << "<real>::offset(const unsigned short i,\n"
          << "const unsigned short j\n) const{\n"
          << "const auto oi = [&i]() -> unsigned short{\n"
          << "switch(i){\n";
      for (size_type i = 0; i != nb; ++i) {
        out << "case " << i << ":\n";
        if (i == 0) {
          out << "return 0;\n"
              << "break;\n";
        } else {
          out << "return ";
          for (size_type j = 0; j != i;) {
            out << "Nss" << j;
            if (++j != i) {
              out << "+";
            }
          }
          out << ";\n"
              << "break;\n";
        }
      }
      out << "default:\n"
          << "tfel::raise<std::out_of_range>(\"" << cn
          << "::offset: :\"\n\"invalid index"
          << "\");\n"
          << "}\n"
          << "}();\n"
          << "return oi+j;\n"
          << "} // end of offset\n\n";
      for (size_type i = 0; i != nb; ++i) {
        out << "template<typename real>\n"
            << "constexpr unsigned short\n"
            << cn << "<real>::offset" << i
            << "(const unsigned short i) const{\n";
        if (i != 0) {
          out << "constexpr unsigned short o = ";
          for (size_type j = 0; j != i;) {
            out << "Nss" << j;
            if (++j != i) {
              out << "+";
            }
          }
          out << ";\n"
              << "return o+i;\n";
        } else {
          out << "return i;\n";
        }
        out << "} // end of offset" << i << "\n\n";
      }
    }
    out << "template<typename real>\n"
        << "bool " << cn << "<real>::areCoplanar(const unsigned short i,\n"
        << "                                     const unsigned short j) "
           "const{\n";
    std::vector<std::vector<bool>> are_coplanar(nss, std::vector<bool>(nss));
    auto i = size_type{};
    for (size_type idx = 0; idx != nb; ++idx) {
      const auto gsi = sss.getSlipSystems(idx);
      for (size_type idx2 = 0; idx2 != gsi.size(); ++idx2, ++i) {
        auto j = size_type{};
        for (size_type jdx = 0; jdx != nb; ++jdx) {
          const auto gsj = sss.getSlipSystems(jdx);
          for (size_type jdx2 = 0; jdx2 != gsj.size(); ++jdx2, ++j) {
            if (gsi[idx2].is<SlipSystemsDescription::system3d>()) {
              const auto& si =
                  gsi[idx2].get<SlipSystemsDescription::system3d>();
              const auto& sj =
                  gsj[jdx2].get<SlipSystemsDescription::system3d>();
              const auto& ni = si.plane;
              const auto& nj = sj.plane;
              are_coplanar[i][j] =
                  (std::equal(ni.begin(), ni.end(), nj.begin()) ||
                   std::equal(
                       ni.begin(), ni.end(), nj.begin(),
                       [](const int a, const int b) { return a == -b; }));
            } else {
              const auto& si =
                  gsi[idx2].get<SlipSystemsDescription::system4d>();
              const auto& sj =
                  gsj[jdx2].get<SlipSystemsDescription::system4d>();
              const auto& ni = si.plane;
              const auto& nj = sj.plane;
              are_coplanar[i][j] =
                  (std::equal(ni.begin(), ni.end(), nj.begin()) ||
                   std::equal(
                       ni.begin(), ni.end(), nj.begin(),
                       [](const int a, const int b) { return a == -b; }));
            }
          }
        }
      }
    }
    out << "const auto mi = std::min(i,j);\n"
        << "const auto mj = std::max(i,j);\n"
        << "switch(mi){\n";
    for (i = 0; i != nss; ++i) {
      out << "case " << i << ":\n";
      if (i + 1 == nss) {
        out << "return (mi==" << nss - 1 << ")&&(mj==" << nss - 1 << ");\n";
      } else {
        out << "switch (mj){\n";
        for (size_type j = i; j != nss; ++j) {
          out << "case " << j << ":\n"
              << "return " << (are_coplanar[i][j] ? "true" : "false") << ";\n"
              << "break;\n";
        }
        out << "default:\n"
            << "return false;\n"
            << "}\n";
      }
      out << "break;\n";
    }
    out << "default:\n"
        << "break;\n"
        << "}\n"
        << "return false;\n"
        << "}\n\n";
    // buildInteractionMatrix
    auto count = size_type{};  // number of terms of the matrix treated so far
    out << "template<typename real>\n"
        << "constexpr "
        << "tfel::math::tmatrix<" << cn << "<real>::Nss," << cn
        << "<real>::Nss,real>\n"
        << cn << "<real>::buildInteractionMatrix("
        << "const tfel::math::fsarray<" << ims.rank() << ", real>& m) const{\n"
        << "return {";
    for (size_type idx = 0; idx != nb; ++idx) {
      const auto gsi = sss.getSlipSystems(idx);
      for (size_type idx2 = 0; idx2 != gsi.size(); ++idx2) {
        for (size_type jdx = 0; jdx != nb; ++jdx) {
          const auto gsj = sss.getSlipSystems(jdx);
          for (size_type jdx2 = 0; jdx2 != gsj.size(); ++jdx2) {
            const auto r = ims.getRank(gsi[idx2], gsj[jdx2]);
            out << "m[" << r << "]";
            if (++count != nss * nss) {
              out << ",";
            }
          }
        }
        out << '\n';
      }
    }
    out << "};\n"
        << "} // end of buildInteractionMatrix\n\n"
        << "} // end of namespace tfel::material\n\n"
        << "#endif /* LIB_TFEL_MATERIAL_" << makeUpperCase(cn) << "_IXX */\n";
  }

  void BehaviourDSLCommon::
      declareExternalStateVariableProbablyUnusableInPurelyImplicitResolution(
          const Hypothesis h, const std::string& n) {
    if (!this->explicitlyDeclaredUsableInPurelyImplicitResolution) {
      this->mb.setUsableInPurelyImplicitResolution(h, false);
    }
    this->mb
        .declareExternalStateVariableProbablyUnusableInPurelyImplicitResolution(
            h, n);
  }  // end of
     // declareExternalStateVariableProbablyUnusableInPurelyImplicitResolution

  std::string BehaviourDSLCommon::standardModifier(const Hypothesis h,
                                                   const std::string& var,
                                                   const bool addThisPtr) {
    if ((this->mb.isExternalStateVariableIncrementName(h, var)) ||
        (var == "dT")) {
      this->declareExternalStateVariableProbablyUnusableInPurelyImplicitResolution(
          h, var.substr(1));
    }
    if (addThisPtr) {
      return "this->" + var;
    }
    return var;
  }  // end of standardModifier

  std::string BehaviourDSLCommon::tangentOperatorVariableModifier(
      const Hypothesis h, const std::string& var, const bool addThisPtr) {
    return this->standardModifier(h, var, addThisPtr);
  }  // end of tangentOperatorVariableModifier

  void BehaviourDSLCommon::treatStrainMeasure() {
    this->checkNotEndOfFile("BehaviourDSLCommon::treatStrainMeasure",
                            "Expected strain measure name.");
    const auto fs = this->current->value;
    if ((fs != "Hencky") && (fs != "GreenLagrange") && (fs != "Linearised") &&
        (fs != "Linearized")) {
      this->throwRuntimeError("BehaviourDSLCommon::treatStrainMeasure",
                              "unsupported strain measure '" + fs + "'");
    }
    ++(this->current);
    this->checkNotEndOfFile("BehaviourDSLCommon::treatStrainMeasure",
                            "Expected ';' or options.");
    const auto opts = [this] {
      if (this->current->value == "{") {
        return tfel::utilities::Data::read(this->current, this->tokens.end())
            .get<tfel::utilities::DataMap>();
      }
      return tfel::utilities::DataMap{};
    }();
    this->readSpecifiedToken("BehaviourDSLCommon::treatStrainMeasure", ";");
    for (const auto& o : opts) {
      if ((o.first != "save_strain") && (o.first != "save_stress")) {
        this->throwRuntimeError("BehaviourDSLCommon::treatStrainMeasure",
                                "invalid option '" + o.first + "'");
      }
      if (!o.second.is<bool>()) {
        this->throwRuntimeError("BehaviourDSLCommon::treatStrainMeasure",
                                "invalid type for option '" + o.first +
                                    "', expected a boolean value");
      }
    }
    if (fs == "Hencky") {
      this->mb.setStrainMeasure(BehaviourDescription::HENCKY);
    } else if (fs == "GreenLagrange") {
      this->mb.setStrainMeasure(BehaviourDescription::GREENLAGRANGE);
    } else if ((fs == "Linearised") || (fs == "Linearized")) {
      this->mb.setStrainMeasure(BehaviourDescription::LINEARISED);
    } else {
      this->throwRuntimeError("BehaviourDSLCommon::treatStrainMeasure",
                              "unsupported strain measure '" + fs + "'");
    }
    if (opts.count("save_strain") != 0) {
      this->mb.setSaveStrainMeasure(
          opts.find("save_strain")->second.get<bool>());
    }
    if (opts.count("save_stress") != 0) {
      this->mb.setSaveDualStress(opts.find("save_stress")->second.get<bool>());
    }
  }  // end of treatStrainMeasure

  void BehaviourDSLCommon::treatPostProcessing() {
    auto hs = std::set<Hypothesis>{};
    this->readHypothesesList(hs);
    this->checkNotEndOfFile("BehaviourDSLCommon::treatPostProcessing");
    const auto pname = this->current->value;
    if (!this->isValidIdentifier(pname)) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::treatPostProcessing",
          "invalid post-processing name (read '" + pname + "').");
    }
    ++(this->current);
    this->checkNotEndOfFile("BehaviourDSLCommon::treatPostProcessing");
    const auto beg = this->current;
    for (const auto& h : hs) {
      const auto& d = this->mb.getBehaviourData(h);
      const auto& pvariables = d.getPostProcessingVariables();
      auto used_post_processing_variables = std::vector<VariableDescription>{};
      this->current = beg;
      CodeBlockParserOptions o;
      o.mn = d.getRegistredMembersNames();
      o.smn = d.getRegistredStaticMembersNames();
      o.qualifyStaticVariables = true;
      o.qualifyMemberVariables = true;
      o.analyser = std::make_shared<StandardWordAnalyser>(
          h, [&used_post_processing_variables, &pvariables](
                 CodeBlock&, const Hypothesis, const std::string& w) {
            if (pvariables.contains(w)) {
              const auto p = std::find_if(
                  used_post_processing_variables.begin(),
                  used_post_processing_variables.end(),
                  [&w](const VariableDescription& v) { return v.name == w; });
              if (p == used_post_processing_variables.end()) {
                used_post_processing_variables.push_back(
                    pvariables.getVariable(w));
              }
            }
          });
      o.modifier = std::make_shared<StandardVariableModifier>(
          h, [this](const Hypothesis hv, const std::string& v, const bool b) {
            return this->standardModifier(hv, v, b);
          });
      auto c = this->readNextBlock(o);
      c.attributes[CodeBlock::used_postprocessing_variables] =
          used_post_processing_variables;
      this->mb.addPostProcessing(h, pname, c);
      this->mb.registerMemberName(h, "execute" + pname + "PostProcessing");
    }
  }  // end of treatPostProcessing

  void BehaviourDSLCommon::treatInitializeFunction() {
    auto hs = std::set<Hypothesis>{};
    this->readHypothesesList(hs);
    this->checkNotEndOfFile("BehaviourDSLCommon::treatInitializeFunction");
    const auto iname = this->current->value;
    if (!this->isValidIdentifier(iname)) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::treatInitializeFunction",
          "invalid post-processing name (read '" + iname + "').");
    }
    ++(this->current);
    this->checkNotEndOfFile("BehaviourDSLCommon::treatInitializeFunction");
    const auto beg = this->current;
    for (const auto& h : hs) {
      const auto& d = this->mb.getBehaviourData(h);
      const auto& pvariables = d.getInitializeFunctionVariables();
      auto used_initialize_function_variables =
          std::vector<VariableDescription>{};
      this->current = beg;
      CodeBlockParserOptions o;
      o.mn = d.getRegistredMembersNames();
      o.smn = d.getRegistredStaticMembersNames();
      o.qualifyStaticVariables = true;
      o.qualifyMemberVariables = true;
      o.analyser = std::make_shared<StandardWordAnalyser>(
          h, [&used_initialize_function_variables, &pvariables](
                 CodeBlock&, const Hypothesis, const std::string& w) {
            if (pvariables.contains(w)) {
              const auto p = std::find_if(
                  used_initialize_function_variables.begin(),
                  used_initialize_function_variables.end(),
                  [&w](const VariableDescription& v) { return v.name == w; });
              if (p == used_initialize_function_variables.end()) {
                used_initialize_function_variables.push_back(
                    pvariables.getVariable(w));
              }
            }
          });
      o.modifier = std::make_shared<StandardVariableModifier>(
          h, [this](const Hypothesis hv, const std::string& v, const bool b) {
            return this->standardModifier(hv, v, b);
          });
      auto c = this->readNextBlock(o);
      c.attributes[CodeBlock::used_initialize_function_variables] =
          used_initialize_function_variables;
      this->mb.addInitializeFunction(h, iname, c);
      this->mb.registerMemberName(h, "execute" + iname + "InitializeFunction");
    }
  }  // end of treatInitializeFunction

  void BehaviourDSLCommon::treatPrivate() {
    auto hs = std::set<Hypothesis>{};
    this->readHypothesesList(hs);
    const auto beg = this->current;
    for (const auto& h : hs) {
      const auto& d = this->mb.getBehaviourData(h);
      this->current = beg;
      CodeBlockParserOptions o;
      o.mn = d.getRegistredMembersNames();
      o.smn = d.getRegistredStaticMembersNames();
      o.qualifyStaticVariables = true;
      o.qualifyMemberVariables = true;
      o.modifier = std::make_shared<StandardVariableModifier>(
          h, [this](const Hypothesis hv, const std::string& v, const bool b) {
            return this->standardModifier(hv, v, b);
          });
      this->mb.appendToPrivateCode(h, this->readNextBlock(o).code, true);
    }
  }  // end of treatPrivate

  void BehaviourDSLCommon::treatMembers() {
    auto hs = std::set<Hypothesis>{};
    this->readHypothesesList(hs);
    const auto beg = this->current;
    for (const auto& h : hs) {
      const auto& d = this->mb.getBehaviourData(h);
      this->current = beg;
      CodeBlockParserOptions o;
      o.mn = d.getRegistredMembersNames();
      o.smn = d.getRegistredStaticMembersNames();
      o.qualifyStaticVariables = true;
      o.qualifyMemberVariables = true;
      o.modifier = std::make_shared<StandardVariableModifier>(
          h, [this](const Hypothesis hv, const std::string& v, const bool b) {
            return this->standardModifier(hv, v, b);
          });
      this->mb.appendToMembers(h, this->readNextBlock(o).code, true);
    }
  }  // end of treatMembers

  void BehaviourDSLCommon::treatBrick() {
    using Parameters = AbstractBehaviourBrick::Parameters;
    auto& f = BehaviourBrickFactory::getFactory();
    auto parameters = Parameters{};
    this->checkNotEndOfFile("BehaviourDSLCommon::treatBehaviourBrick",
                            "Expected brick name or '<'.");
    if (this->current->value == "<") {
      auto options = std::vector<tfel::utilities::Token>{};
      this->readList(options, "BehaviourDSLCommon::treatBehaviourBrick", "<",
                     ">", true);
      for (const auto& o : options) {
        const auto pos = o.value.find('=');
        if (pos != std::string::npos) {
          if (pos == 0) {
            this->throwRuntimeError("BehaviourDSLCommon::treatBehaviourBrick",
                                    "no parameter name given");
          }
          // extracting the name
          const auto& n = o.value.substr(0, pos);
          if (pos == o.value.size()) {
            this->throwRuntimeError("BehaviourDSLCommon::treatBehaviourBrick",
                                    "no option given to the "
                                    "parameter '" +
                                        n + "'");
          }
          // extracting the option
          parameters.insert({n, o.value.substr(pos + 1)});
        } else {
          parameters.insert({o.value, ""});
        }
      }
    }
    this->checkNotEndOfFile("BehaviourDSLCommon::treatBehaviourBrick",
                            "Expected brick name.");
    const auto b = [this]() -> std::string {
      if (this->current->flag == tfel::utilities::Token::String) {
        return this->readString("BehaviourDSLCommon::treatBehaviourBrick");
      }
      const auto r = this->current->value;
      ++(this->current);
      return r;
    }();
    const auto d = [this] {
      using namespace tfel::utilities;
      if ((this->current != this->tokens.end()) &&
          (this->current->value == "{")) {
        DataParsingOptions o;
        o.allowMultipleKeysInMap = true;
        return Data::read(this->current, this->tokens.end(), o).get<DataMap>();
      }
      return DataMap();
    }();
    const auto br = f.get(b, *this, this->mb);
    br->initialize(parameters, d);
    this->readSpecifiedToken("BehaviourDSLCommon::treatBehaviourBrick", ";");
    this->bricks.push_back(std::move(br));
  }  // end of treatBrick

  void BehaviourDSLCommon::treatTangentOperator() {
    using namespace tfel::material;
    using namespace tfel::utilities;
    if (this->mb.getMainVariables().empty()) {
      this->throwRuntimeError("BehaviourDSLCommon::treatTangentOperator",
                              "no thermodynamic force defined");
    }
    CodeBlockOptions o;
    this->readCodeBlockOptions(o, true);
    if (this->mb.getBehaviourType() ==
        BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) {
      auto po = o.untreated.begin();
      const auto poe = o.untreated.end();
      auto ktype = std::string{};
      while (po != poe) {
        const auto& opt = *po;
        if (opt.flag != Token::Standard) {
          continue;
        }
        for (const auto& to : getFiniteStrainBehaviourTangentOperatorFlags()) {
          if (opt.value ==
              convertFiniteStrainBehaviourTangentOperatorFlagToString(to)) {
            ktype = opt.value;
            break;
          }
        }
        if (!ktype.empty()) {
          o.untreated.erase(po);
          break;
        }
        ++po;
      }
      if (ktype.empty()) {
        std::ostringstream msg;
        msg << "Undefined tangent operator type '" + ktype +
                   "'. Valid tangent operator type are :\n";
        for (const auto& to : getFiniteStrainBehaviourTangentOperatorFlags()) {
          msg << "- "
              << convertFiniteStrainBehaviourTangentOperatorFlagToString(to)
              << " : " << getFiniteStrainBehaviourTangentOperatorDescription(to)
              << '\n';
        }
        this->throwRuntimeError("BehaviourDSLCommon::treatTangentOperator",
                                msg.str());
      }
      this->readTangentOperatorCodeBlock(
          o, std::string(BehaviourData::ComputeTangentOperator) + "-" + ktype);
      for (const auto& h : o.hypotheses) {
        if (!this->mb.hasAttribute(
                h, BehaviourData::hasConsistentTangentOperator)) {
          this->mb.setAttribute(h, BehaviourData::hasConsistentTangentOperator,
                                true);
        }
      }
    } else {
      this->readTangentOperatorCodeBlock(o,
                                         BehaviourData::ComputeTangentOperator);
      for (const auto& h : o.hypotheses) {
        this->mb.setAttribute(h, BehaviourData::hasConsistentTangentOperator,
                              true);
      }
    }
  }  // end of treatTangentOperator

  void BehaviourDSLCommon::readTangentOperatorCodeBlock(
      const CodeBlockOptions& o, const std::string& n) {
    this->treatUnsupportedCodeBlockOptions(o);
    this->treatCodeBlock(*this, o, n,
                         &BehaviourDSLCommon::tangentOperatorVariableModifier,
                         true);
  }  // end of readTangentOperatorCodeBlock

  void BehaviourDSLCommon::treatIsTangentOperatorSymmetric() {
    auto hs = std::set<Hypothesis>{};
    this->readHypothesesList(hs);
    this->checkNotEndOfFile(
        "BehaviourDSLCommon::treatIsTangentOperatorSymmetric : ",
        "Expected 'true' or 'false'.");
    auto b = this->readBooleanValue(
        "BehaviourDSLCommon::treatIsTangentOperatorSymmetric");
    this->readSpecifiedToken(
        "BehaviourDSLCommon::treatIsTangentOperatorSymmetric", ";");
    for (const auto& h : hs) {
      this->mb.setAttribute(
          h, BehaviourData::isConsistentTangentOperatorSymmetric, b);
    }
  }  // end of treatTangentOperator

  void BehaviourDSLCommon::treatLibrary() {
    const auto& l = this->readOnlyOneToken();
    if (!isValidLibraryName(l)) {
      this->throwRuntimeError("BehaviourDSLCommon::treatLibrary",
                              "invalid library name '" + l + "'");
    }
    this->mb.setLibrary(l);
  }  // end of treatLibrary

  void BehaviourDSLCommon::treatComputeThermalExpansion() {
    using namespace tfel::utilities;
    using ExternalMFrontMaterialProperty =
        BehaviourDescription::ExternalMFrontMaterialProperty;
    const std::string m("BehaviourDSLCommon::treatComputeThermalExpansion");
    const auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    const auto n = "thermal_expansion_reference_temperature";
    auto throw_if = [this, m](const bool b, const std::string& msg) {
      if (b) {
        this->throwRuntimeError(m, msg);
      }
    };
    auto addTref = [this, throw_if, h, n](const double v) {
      if (this->mb.hasParameter(h, n)) {
        const auto Tref = this->mb.getFloattingPointParameterDefaultValue(h, n);
        throw_if(tfel::math::ieee754::fpclassify(Tref - v) != FP_ZERO,
                 "inconsistent reference temperature");
      } else {
        VariableDescription Tref("temperature", n, 1u, 0u);
        Tref.description =
            "value of the reference temperature for "
            "the computation of the thermal expansion";
        this->mb.addParameter(h, Tref, BehaviourData::ALREADYREGISTRED);
        this->mb.setParameterDefaultValue(h, n, v);
        this->mb.setEntryName(h, n, "ThermalExpansionReferenceTemperature");
      }
    };
    auto addTi = [this](const double v) {
      VariableDescription Ti("temperature",
                             "initial_geometry_reference_temperature", 1u, 0u);
      Ti.description =
          "value of the temperature when the initial geometry was measured";
      this->mb.addParameter(h, Ti, BehaviourData::ALREADYREGISTRED);
      this->mb.setParameterDefaultValue(
          h, "initial_geometry_reference_temperature", v);
      this->mb.setEntryName(h, "initial_geometry_reference_temperature",
                            "ReferenceTemperatureForInitialGeometry");
    };  // end of addTi
    throw_if(
        this->mb.getAttribute<bool>(
            BehaviourDescription::requiresThermalExpansionCoefficientTensor,
            false),
        "@ComputeThermalExpansion can be used along with "
        "@RequireThermalExpansionCoefficientTensor");
    const auto& acs = this->readMaterialPropertyOrArrayOfMaterialProperties(m);
    this->checkNotEndOfFile(m);
    if (this->current->value == "{") {
      const auto data =
          Data::read(this->current, this->tokens.end()).get<DataMap>();
      throw_if(data.size() != 1u,
               "invalid number of data. "
               "Only the 'reference_temperature' is expected");
      const auto pd = data.begin();
      throw_if(pd->first != "reference_temperature",
               "the only data expected is "
               "'reference_temperature' (read '" +
                   pd->first + "')");
      throw_if(!pd->second.is<double>(),
               "invalid type for data 'reference_temperature'");
      addTref(pd->second.get<double>());
    }
    this->readSpecifiedToken(m, ";");
    throw_if((acs.size() != 1u) && (acs.size() != 3u),
             "invalid number of file names given");
    if (acs.size() == 3u) {
      // the material shall have been declared orthotropic
      throw_if(this->mb.getSymmetryType() != mfront::ORTHOTROPIC,
               "the mechanical behaviour must be orthotropic "
               "to give more than one thermal expansion coefficient.");
    }
    for (const auto& a : acs) {
      if (a.is<ExternalMFrontMaterialProperty>()) {
        const auto& mpd = *(a.get<ExternalMFrontMaterialProperty>().mpd);
        if (mpd.staticVars.contains("ReferenceTemperature")) {
          const auto Tref = mpd.staticVars.get("ReferenceTemperature");
          addTref(Tref.value);
        } else {
          if (getVerboseMode() != VERBOSE_QUIET) {
            auto& os = getLogStream();
            os << "no reference temperature in material property '";
            if (mpd.material.empty()) {
              os << mpd.material << '_';
            }
            os << mpd.law << "'\n";
          }
        }
      }
    }
    if (acs.size() == 1u) {
      this->mb.setThermalExpansionCoefficient(acs.front());
    } else {
      this->mb.setThermalExpansionCoefficients(acs[0], acs[1], acs[2]);
    }
    if (!this->mb.hasParameter(h, n)) {
      addTref(293.15);
    }
    if (!this->mb.hasParameter(h, "initial_geometry_reference_temperature")) {
      addTi(293.15);
    }
  }  // end of treatComputeThermalExpansion

  void BehaviourDSLCommon::treatElasticMaterialProperties() {
    if (this->mb.getAttribute<bool>(
            BehaviourDescription::requiresStiffnessTensor, false)) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::treatElasticMaterialProperties",
          "@ElasticMaterialProperties can not be used along with "
          "@RequireStiffnessTensor");
    }
    this->readElasticMaterialProperties();
  }

  BehaviourDescription::MaterialProperty
  BehaviourDSLCommon::extractMaterialProperty(const std::string& m,
                                              const tfel::utilities::Token& t) {
    if (t.flag == tfel::utilities::Token::String) {
      // file name of formula
      const auto f = t.value.substr(1, t.value.size() - 2);
      if (tfel::utilities::ends_with(f, ".mfront")) {
        // file name
        BehaviourDescription::ExternalMFrontMaterialProperty mp;
        mp.mpd = this->handleMaterialPropertyDescription(f);
        return std::move(mp);
      } else {
        BehaviourDescription::AnalyticMaterialProperty mp;
        mp.f = f;
        return std::move(mp);
      }
    }
    BehaviourDescription::ConstantMaterialProperty mp;
    try {
      mp.value = std::stold(t.value);
    } catch (std::exception& e) {
      this->throwRuntimeError(m, "can't convert token '" + t.value +
                                     "' to long double "
                                     "(" +
                                     std::string(e.what()) + ")");
    }
    return std::move(mp);
  }

  std::vector<BehaviourDescription::MaterialProperty>
  BehaviourDSLCommon::readMaterialPropertyOrArrayOfMaterialProperties(
      const std::string& m) {
    auto mps = std::vector<BehaviourDescription::MaterialProperty>{};
    this->checkNotEndOfFile(m);
    if (this->current->value == "{") {
      auto mpv = std::vector<tfel::utilities::Token>{};
      this->readList(mpv, m, "{", "}", false);
      for (const auto& t : mpv) {
        mps.push_back(this->extractMaterialProperty(m, t));
      }
    } else {
      mps.push_back(this->extractMaterialProperty(m, *(this->current)));
      ++(this->current);
    }
    return mps;
  }

  void BehaviourDSLCommon::readElasticMaterialProperties() {
    const auto& emps = this->readMaterialPropertyOrArrayOfMaterialProperties(
        "BehaviourDSLCommon::readElasticMaterialProperties");
    this->readSpecifiedToken(
        "BehaviourDSLCommon::readElasticMaterialProperties", ";");
    if ((emps.size() != 2u) && (emps.size() != 9u)) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::readElasticMaterialProperties",
          "invalid number of file names given");
    }
    if (emps.size() == 9u) {
      // the material shall have been declared orthotropic
      if (this->mb.getSymmetryType() != mfront::ORTHOTROPIC) {
        this->throwRuntimeError(
            "BehaviourDSLCommon::readElasticMaterialProperties",
            "the mechanical behaviour must be orthotropic to give more than "
            "two elastic material properties.");
      }
      setElasticSymmetryType(this->mb, mfront::ORTHOTROPIC);
    } else {
      setElasticSymmetryType(this->mb, mfront::ISOTROPIC);
    }
    this->mb.setElasticMaterialProperties(emps);
  }

  void BehaviourDSLCommon::treatComputeStiffnessTensor() {
    if (this->mb.getAttribute<bool>(
            BehaviourDescription::requiresStiffnessTensor, false)) {
      this->throwRuntimeError("BehaviourDSLCommon::treatComputeStiffnessTensor",
                              "@ComputeStiffnessTensor can be used along with "
                              "@RequireStiffnessTensor");
    }
    if (this->current->value == "<") {
      this->treatStiffnessTensorOption();
    }
    this->readElasticMaterialProperties();
    this->mb.setAttribute(BehaviourDescription::computesStiffnessTensor, true,
                          false);
  }  // end of treatComputeStiffnessTensor

  void BehaviourDSLCommon::treatHillTensor() {
    if (this->mb.getSymmetryType() != mfront::ORTHOTROPIC) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::treatHillTensor",
          "the mechanical behaviour must be orthotropic to define "
          "a Hill tensor.");
    }
    this->checkNotEndOfFile("BehaviourDSLCommon::treatModellingHypothesis");
    // variable name
    if (!this->isValidIdentifier(this->current->value)) {
      this->throwRuntimeError("BehaviourDSLCommon::treatHillTensor: ",
                              "variable name is not valid "
                              "(read '" +
                                  this->current->value + "').");
    }
    auto v = VariableDescription{"tfel::math::st2tost2<N,real>",
                                 this->current->value, 1u, this->current->line};
    v.description = "Hill tensor";
    ++(this->current);
    // Hill coefficients
    const auto& hcs = this->readMaterialPropertyOrArrayOfMaterialProperties(
        "BehaviourDSLCommon::treatHillTensor");
    this->readSpecifiedToken("BehaviourDSLCommon::treatHillTensor", ";");
    if (hcs.size() != 6u) {
      this->throwRuntimeError("BehaviourDSLCommon::treatHillTensor",
                              "invalid number of hill coefficients");
    }
    this->mb.addHillTensor(v, hcs);
  }  // end of treatHillTensor

  void BehaviourDSLCommon::treatModellingHypothesis() {
    this->checkNotEndOfFile("BehaviourDSLCommon::treatModellingHypothesis");
    const auto h = ModellingHypothesis::fromString(this->current->value);
    ++(this->current);
    this->checkNotEndOfFile("BehaviourDSLCommon::treatModellingHypothesis");
    this->readSpecifiedToken("BehaviourDSLCommon::treatModellingHypothesis",
                             ";");
    if (!this->isModellingHypothesisSupported(h)) {
      this->throwRuntimeError("BehaviourDSLCommon::treatModellingHypothesis",
                              "unsupported modelling hypothesis '" +
                                  ModellingHypothesis::toString(h) + "'");
    }
    std::set<Hypothesis> hypotheses;
    hypotheses.insert(h);
    this->mb.setModellingHypotheses(hypotheses);
  }  // end of treatModellingHypothesis

  void BehaviourDSLCommon::treatModellingHypotheses() {
    using namespace tfel::utilities;
    auto hypotheses = std::set<Hypothesis>{};
    auto values = std::vector<Token>{};
    this->checkNotEndOfFile("BehaviourDSLCommon::treatModellingHypotheses");
    this->readList(values, "BehaviourDSLCommon::treatModellingHypotheses", "{",
                   "}", false);
    this->checkNotEndOfFile("BehaviourDSLCommon::treatModellingHypotheses");
    this->readSpecifiedToken("BehaviourDSLCommon::treatModellingHypotheses",
                             ";");
    for (const auto& v : values) {
      if (v.flag == Token::String) {
        this->appendToHypothesesList(hypotheses,
                                     v.value.substr(1, v.value.size() - 2));
      } else {
        this->appendToHypothesesList(hypotheses, v.value);
      }
    }
    if (hypotheses.empty()) {
      this->throwRuntimeError("BehaviourDSLCommon::treatModellingHypotheses",
                              "no hypothesis declared");
    }
    this->mb.setModellingHypotheses(hypotheses);
  }  // end of treatModellingHypotheses

  void BehaviourDSLCommon::treatUsableInPurelyImplicitResolution() {
    const auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    this->readSpecifiedToken(
        "BehaviourDSLCommon::treatUsableInPurelyImplicitResolution", ";");
    if (this->explicitlyDeclaredUsableInPurelyImplicitResolution) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::treatUsableInPurelyImplicitResolution",
          "keyword '@UsableInPurelyImplicitResolution' already called");
    }
    this->explicitlyDeclaredUsableInPurelyImplicitResolution = true;
    this->mb.setUsableInPurelyImplicitResolution(h, true);
  }  // end of treatUsableInPurelyImplicitResolution

  void BehaviourDSLCommon::treatParameterMethod(const Hypothesis h) {
    using namespace tfel::utilities;
    const auto n = tfel::unicode::getMangledString(this->current->value);
    ++(this->current);
    this->checkNotEndOfFile("BehaviourDSLCommon::treatParameterMethod");
    this->readSpecifiedToken("BehaviourDSLCommon::treatParameterMethod", ".");
    this->checkNotEndOfFile("BehaviourDSLCommon::treatParameterMethod");
    if (this->current->value == "setDefaultValue") {
      ++(this->current);
      this->checkNotEndOfFile("BehaviourDSLCommon::treatParameterMethod");
      this->readSpecifiedToken("BehaviourDSLCommon::treatParameterMethod", "(");
      this->checkNotEndOfFile("BehaviourDSLCommon::treatParameterMethod");
      double value = tfel::utilities::convert<double>(this->current->value);
      ++(this->current);
      this->checkNotEndOfFile("BehaviourDSLCommon::treatParameterMethod");
      this->readSpecifiedToken("BehaviourDSLCommon::treatParameterMethod", ")");
      this->checkNotEndOfFile("BehaviourDSLCommon::treatParameterMethod");
      this->readSpecifiedToken("BehaviourDSLCommon::treatParameterMethod", ";");
      this->mb.setParameterDefaultValue(h, n, value);
    } else {
      --(this->current);
      --(this->current);
      this->treatVariableMethod(h);
    }
  }  // end of treatParameterMethod

  bool BehaviourDSLCommon::isCallableVariable(const Hypothesis h,
                                              const std::string& n) const {
    if (h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
      for (const auto& g : this->gradients) {
        if (g.name == n) {
          return true;
        }
      }
      for (const auto& f : this->thermodynamic_forces) {
        if (f.name == n) {
          return true;
        }
      }
      if (this->mb.isGradientName(n) || this->mb.isThermodynamicForceName(n)) {
        return true;
      }
    }
    return ((this->mb.isMaterialPropertyName(h, n)) ||
            (this->mb.isStateVariableName(h, n)) ||
            (this->mb.isAuxiliaryStateVariableName(h, n)) ||
            (this->mb.isExternalStateVariableName(h, n)) ||
            (this->mb.isLocalVariableName(h, n)) ||
            (this->mb.isStaticVariableName(h, n)) ||
            (this->mb.isParameterName(h, n)) ||
            (this->mb.isInitializeFunctionVariableName(h, n)) ||
            (this->mb.isPostProcessingVariableName(h, n)) ||
            (this->mb.isIntegrationVariableName(h, n)));
  }  // end of isCallableVariable

  std::string BehaviourDSLCommon::treatSetGlossaryNameMethod() {
    using namespace tfel::utilities;
    using namespace tfel::glossary;
    const auto& glossary = Glossary::getGlossary();
    this->checkNotEndOfFile("BehaviourDSLCommon::treatSetGlossaryMethod");
    this->readSpecifiedToken("BehaviourDSLCommon::treatSetGlossaryMethod",
                             "setGlossaryName");
    this->readSpecifiedToken("BehaviourDSLCommon::treatSetGlossaryMethod", "(");
    this->checkNotEndOfFile("BehaviourDSLCommon::treatSetGlossaryMethod");
    if (this->current->flag != Token::String) {
      this->throwRuntimeError("BehaviourDSLCommon::treatSetGlossaryMethod: ",
                              "expected to read a string");
    }
    const auto& g =
        this->current->value.substr(1, this->current->value.size() - 2);
    if (!glossary.contains(g)) {
      this->throwRuntimeError("BehaviourDSLCommon::treatSetGlossaryMethod: ",
                              "'" + g + "' is not a glossary name");
    }
    ++(this->current);
    this->checkNotEndOfFile("BehaviourDSLCommon::treatSetGlossaryMethod");
    this->readSpecifiedToken("BehaviourDSLCommon::treatSetGlossaryMethod", ")");
    return g;
  }  // end of treatSetGlossaryNameMethod

  std::string BehaviourDSLCommon::treatSetEntryNameMethod() {
    using namespace tfel::utilities;
    this->checkNotEndOfFile("BehaviourDSLCommon::treatSetEntryNameMethod");
    this->readSpecifiedToken("BehaviourDSLCommon::treatSetEntryNameMethod",
                             "setEntryName");
    this->checkNotEndOfFile("BehaviourDSLCommon::treatSetEntryNameMethod");
    this->readSpecifiedToken("BehaviourDSLCommon::treatSetEntryNameMethod",
                             "(");
    this->checkNotEndOfFile("BehaviourDSLCommon::treatSetEntryNameMethod");
    if (this->current->flag != Token::String) {
      this->throwRuntimeError("BehaviourDSLCommon::treatSetEntryNameMethod: ",
                              "expected to read a string");
    }
    const auto& e =
        this->current->value.substr(1, this->current->value.size() - 2);
    if (!this->isValidIdentifier(e)) {
      this->throwRuntimeError("BehaviourDSLCommon::treatSetEntryNameMethod: ",
                              "invalid entry name '" + e + "'");
    }
    ++(this->current);
    this->readSpecifiedToken("BehaviourDSLCommon::treatSetEntryNameMethod",
                             ")");
    return e;
  }  // end of treatSetEntryNameMethod

  void BehaviourDSLCommon::treatGradientMethod() {
    const auto n = tfel::unicode::getMangledString(this->current->value);
    ++(this->current);
    this->checkNotEndOfFile("BehaviourDSLCommon::treatGradientMethod");
    this->readSpecifiedToken("BehaviourDSLCommon::treatGradientMethod", ".");
    this->checkNotEndOfFile("BehaviourDSLCommon::treatGradientMethod");
    if (this->current->value == "setGlossaryName") {
      const auto gn = this->treatSetGlossaryNameMethod();
      bool treated = false;
      for (auto& g : this->gradients) {
        if (g.name == n) {
          g.setGlossaryName(gn);
          treated = true;
          break;
        }
      }
      if (!treated) {
        if (!this->mb.isGradientName(n)) {
          this->throwRuntimeError(
              "BehaviourDSLCommon::treatGradientMethod",
              "invalid call, '" + n + "' is not a registred gradient");
        }
        this->mb.setGlossaryName(n, gn);
      }
    } else if (this->current->value == "setEntryName") {
      const auto e = this->treatSetEntryNameMethod();
      bool treated = false;
      for (auto& g : this->gradients) {
        if (g.name == n) {
          g.setEntryName(e);
          treated = true;
          break;
        }
      }
      if (!treated) {
        if (!this->mb.isGradientName(n)) {
          this->throwRuntimeError(
              "BehaviourDSLCommon::treatGradientMethod",
              "invalid call, '" + n + "' is not a registred gradient");
        }
        this->mb.setEntryName(n, e);
      }
    } else {
      this->throwRuntimeError(
          "BehaviourDSLCommon::treatGradientMethod",
          "unsupported method '" + this->current->value + "'");
    }
    this->checkNotEndOfFile("BehaviourDSLCommon::treatGradientMethod");
    this->readSpecifiedToken("BehaviourDSLCommon::treatGradientMethod", ";");
  }  // end of treatGradientMethod

  void BehaviourDSLCommon::treatThermodynamicForceMethod() {
    const auto n = tfel::unicode::getMangledString(this->current->value);
    ++(this->current);
    this->checkNotEndOfFile(
        "BehaviourDSLCommon::treatThermodynamicForceMethod");
    this->readSpecifiedToken(
        "BehaviourDSLCommon::treatThermodynamicForceMethod", ".");
    this->checkNotEndOfFile(
        "BehaviourDSLCommon::treatThermodynamicForceMethod");
    if (this->current->value == "setGlossaryName") {
      const auto gn = this->treatSetGlossaryNameMethod();
      bool treated = false;
      for (auto& f : this->thermodynamic_forces) {
        if (f.name == n) {
          f.setGlossaryName(gn);
          treated = true;
          break;
        }
      }
      if (!treated) {
        if (!this->mb.isThermodynamicForceName(n)) {
          this->throwRuntimeError(
              "BehaviourDSLCommon::treatThermodynamicForceMethod",
              "invalid call, '" + n +
                  "' is not a registred thermodynamic force");
        }
        this->mb.setGlossaryName(n, gn);
      }
    } else if (this->current->value == "setEntryName") {
      const auto e = this->treatSetEntryNameMethod();
      bool treated = false;
      for (auto& f : this->thermodynamic_forces) {
        if (f.name == n) {
          f.setEntryName(e);
          treated = true;
          break;
        }
      }
      if (!treated) {
        if (!this->mb.isThermodynamicForceName(n)) {
          this->throwRuntimeError(
              "BehaviourDSLCommon::treatThermodynamicForceMethod",
              "invalid call, '" + n +
                  "' is not a registred thermodynamic force");
        }
        this->mb.setEntryName(n, e);
      }
    } else {
      this->throwRuntimeError(
          "BehaviourDSLCommon::treatThermodynamicForceMethod",
          "unsupported method '" + this->current->value + "'");
    }
    this->checkNotEndOfFile(
        "BehaviourDSLCommon::treatThermodynamicForceMethod");
    this->readSpecifiedToken(
        "BehaviourDSLCommon::treatThermodynamicForceMethod", ";");
  }  // end of treatThermodynamicForceMethod

  void BehaviourDSLCommon::treatVariableMethod(const Hypothesis h) {
    const auto n = tfel::unicode::getMangledString(this->current->value);
    ++(this->current);
    this->checkNotEndOfFile("BehaviourDSLCommon::treatVariableMethod");
    this->readSpecifiedToken("BehaviourDSLCommon::treatVariableMethod", ".");
    this->checkNotEndOfFile("BehaviourDSLCommon::treatVariableMethod");
    if (this->current->value == "setGlossaryName") {
      this->mb.setGlossaryName(h, n, this->treatSetGlossaryNameMethod());
    } else if (this->current->value == "setEntryName") {
      this->mb.setEntryName(h, n, this->treatSetEntryNameMethod());
    } else {
      this->treatUnknownVariableMethod(h, n);
    }
    this->checkNotEndOfFile("BehaviourDSLCommon::treatVariableMethod");
    this->readSpecifiedToken("BehaviourDSLCommon::treatVariableMethod", ";");
  }  // end of treatVariableMethod

  void BehaviourDSLCommon::treatUnknownVariableMethod(const Hypothesis,
                                                      const std::string& n) {
    this->throwRuntimeError(
        "BehaviourDSLCommon::treatUnknownVariableMethod : ",
        "unknown method '" + this->current->value + "' for variable '" + n +
            "', "
            "valid methods are 'setGlossaryName' or 'setEntryName'");
  }  // end of treatUnknownVariableMethod

  void BehaviourDSLCommon::treatUnknownKeyword() {
    TokensContainer::const_iterator p2;
    auto treated = false;
    --(this->current);
    const auto key = this->current->value;
    ++(this->current);
    this->checkNotEndOfFile("BehaviourDSLCommon::treatUnknownKeyword");
    for (const auto& b : bricks) {
      auto p = b->treatKeyword(key, this->current, this->tokens.end());
      if (p.first) {
        if (treated) {
          if (p2 != p.second) {
            this->throwRuntimeError("BehaviourDSLCommon::treatUnknownKeyword",
                                    "the keyword '" + key +
                                        "' has been treated "
                                        "by two interfaces/analysers but "
                                        "results were differents");
          }
        }
        p2 = p.second;
        treated = true;
      }
    }
    if (!treated) {
      if (this->current->value == "[") {
        ++(this->current);
        this->checkNotEndOfFile("BehaviourDSLCommon::treatUnknownKeyword");
        auto s = std::vector<std::string>{};
        while (this->current->value != "]") {
          this->checkNotEndOfFile("BehaviourDSLCommon::treatUnknownKeyword");
          const auto t = [this]() -> std::string {
            if (this->current->flag == tfel::utilities::Token::String) {
              return this->current->value.substr(
                  1, this->current->value.size() - 2);
            }
            return this->current->value;
          }();
          ++(this->current);
          this->checkNotEndOfFile("BehaviourDSLCommon::treatUnknownKeyword");
          if (std::find(s.begin(), s.end(), t) == s.end()) {
            s.push_back(t);
          }
          if (this->current->value != "]") {
            this->readSpecifiedToken("BehaviourDSLCommon::treatUnknownKeyword",
                                     ",");
            this->checkNotEndOfFile("BehaviourDSLCommon::treatUnknownKeyword");
            if (this->current->value == "]") {
              this->throwRuntimeError("BehaviourDSLCommon::treatUnknownKeyword",
                                      "unexpected token ']'");
            }
          }
        }
        ++(this->current);
        for (auto& i : this->interfaces) {
          auto p = i.second->treatKeyword(this->mb, key, s, this->current,
                                          this->tokens.end());
          if (p.first) {
            if (treated) {
              if (p2 != p.second) {
                this->throwRuntimeError(
                    "BehaviourDSLCommon::treatUnknownKeyword",
                    "the keyword '" + key +
                        "' has been treated "
                        "by two interfaces/analysers but "
                        "results were differents");
              }
            }
            p2 = p.second;
            treated = true;
          }
        }
        if (!treated) {
          this->ignoreKeyWord(key);
          return;
        }
      } else {
        for (const auto& i : this->interfaces) {
          auto p = i.second->treatKeyword(this->mb, key, {}, this->current,
                                          this->tokens.end());
          if (p.first) {
            if (treated) {
              if (p2 != p.second) {
                this->throwRuntimeError(
                    "BehaviourDSLCommon::treatUnknownKeyword",
                    "the keyword '" + key +
                        "' has been treated "
                        "by two interfaces/analysers but "
                        "results were differents");
              }
            }
            p2 = p.second;
            treated = true;
          }
        }
      }
    }
    if (!treated) {
      DSLBase::treatUnknownKeyword();
    }
    this->current = p2;
  }  // end of treatUnknownKeyword

  void BehaviourDSLCommon::treatUseQt() {
    this->checkNotEndOfFile("BehaviourDSLCommon::treatUseQt",
                            "Expected 'true' or 'false'.");
    this->mb.setUseQt(this->readBooleanValue("BehaviourDSLCommon::treatUseQt"));
    this->readSpecifiedToken("BehaviourDSLCommon::treatUseQt", ";");
  }  // end of treatUseQt

  void BehaviourDSLCommon::treatIsotropicBehaviour() {
    if (this->mb.getSymmetryType() != mfront::ISOTROPIC) {
      this->throwRuntimeError("BehaviourDSLCommon::treatIsotropicBehaviour",
                              "this behaviour has been declared orthotropic");
    }
    this->readSpecifiedToken("BehaviourDSLCommon::treatIsotropicBehaviour",
                             ";");
  }  // end of treatIsotropicBehaviour

  void BehaviourDSLCommon::treatOrthotropicBehaviour() {
    using namespace tfel::material;
    auto c = OrthotropicAxesConvention::DEFAULT;
    this->checkNotEndOfFile("BehaviourDSLCommon::treatOrthotropicBehaviour");
    if (this->current->value == "<") {
      this->readSpecifiedToken("BehaviourDSLCommon::treatOrthotropicBehaviour",
                               "<");
      this->checkNotEndOfFile("BehaviourDSLCommon::treatOrthotropicBehaviour");
      if (this->current->value == "Pipe") {
        ++this->current;
        c = OrthotropicAxesConvention::PIPE;
      } else if (this->current->value == "Plate") {
        ++this->current;
        c = OrthotropicAxesConvention::PLATE;
      } else if (this->current->value == "Default") {
        ++this->current;
      } else {
        this->throwRuntimeError("BehaviourDSLCommon::treatOrthotropicBehaviour",
                                "unsupported orthotropic axes convention");
      }
      this->readSpecifiedToken("BehaviourDSLCommon::treatOrthotropicBehaviour",
                               ">");
    }
    this->readSpecifiedToken("BehaviourDSLCommon::treatOrthotropicBehaviour",
                             ";");
    this->mb.setSymmetryType(mfront::ORTHOTROPIC);
    this->mb.setOrthotropicAxesConvention(c);
  }  // end of treatOrthotropicBehaviour

  void BehaviourDSLCommon::treatIsotropicElasticBehaviour() {
    this->readSpecifiedToken(
        "BehaviourDSLCommon::treatIsotropicElasticBehaviour", ";");
    if (this->mb.getSymmetryType() != mfront::ORTHOTROPIC) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::treatIsotropicElasticBehaviour",
          "this behaviour has not been declared orthotropic");
    }
    this->mb.setElasticSymmetryType(mfront::ISOTROPIC);
  }  // end of treatIsotropicElasticBehaviour

  void BehaviourDSLCommon::treatRequireStiffnessOperator() {
    if (getVerboseMode() >= VERBOSE_LEVEL2) {
      getLogStream() << "BehaviourDSLCommon::treatRequireStiffnessOperator : "
                     << "@RequireStiffnessOperator is deprecated\n"
                     << "You shall use @RequireStiffnessTensor instead\n";
    }
    this->treatRequireStiffnessTensor();
  }  // end of treatRequireStiffnessOperator

  void BehaviourDSLCommon::treatStiffnessTensorOption() {
    this->readSpecifiedToken("BehaviourDSLCommon::treatStiffnessTensorOption",
                             "<");
    this->checkNotEndOfFile("BehaviourDSLCommon::treatStiffnessTensorOption");
    if (this->current->value == "UnAltered") {
      this->mb.setAttribute(
          BehaviourDescription::requiresUnAlteredStiffnessTensor, true, false);
    } else if (this->current->value == "Altered") {
      this->mb.setAttribute(
          BehaviourDescription::requiresUnAlteredStiffnessTensor, false, false);
    } else {
      this->throwRuntimeError(
          "BehaviourDSLCommon::treatStiffnessTensorOption : ",
          "expected 'Altered' or 'UnAltered' option "
          "(read '" +
              this->current->value + "')");
    }
    ++(this->current);
    this->readSpecifiedToken("BehaviourDSLCommon::treatStiffnessTensorOption",
                             ">");
  }

  void BehaviourDSLCommon::treatRequireStiffnessTensor() {
    if (this->mb.hasAttribute(BehaviourDescription::computesStiffnessTensor)) {
      this->throwRuntimeError("BehaviourDSLCommon::treatRequireStiffnessTensor",
                              "@RequireStiffnessTensor can be used along with "
                              "@ComputeStiffnessTensor");
    }
    this->checkNotEndOfFile("BehaviourDSLCommon::treatRequireStiffnessTensor");
    if (this->current->value == "<") {
      this->treatStiffnessTensorOption();
    }
    this->readSpecifiedToken("BehaviourDSLCommon::treatRequireStiffnessTensor",
                             ";");
    this->mb.setAttribute(BehaviourDescription::requiresStiffnessTensor, true,
                          false);
  }  // end of treatRequireStiffnessTensor

  void BehaviourDSLCommon::treatRequireThermalExpansionCoefficientTensor() {
    this->readSpecifiedToken(
        "BehaviourDSLCommon::treatRequireThermalExpansionCoefficientTensor",
        ";");
    this->mb.setAttribute(
        BehaviourDescription::requiresThermalExpansionCoefficientTensor, true,
        false);
  }  // end of treatRequireThermalExpansionCoefficientTensor

  void BehaviourDSLCommon::setMaterialKnowledgeIdentifier(
      const std::string& i) {
    if (!isValidBehaviourName(i)) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::setMaterialKnowledgeIdentifier",
          "invalid behaviour name '" + i + "'");
    }
    this->mb.setBehaviourName(i);
    if (!isValidIdentifier(this->mb.getClassName())) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::setMaterialKnowledgeIdentifier",
          "resulting class name is not valid (read '" +
              this->mb.getClassName() + "')");
    }
  }  // end of setMaterialKnowledgeIdentifier

  void BehaviourDSLCommon::treatBehaviour() {
    const auto& b = this->readOnlyOneToken();
    if (!isValidBehaviourName(b)) {
      this->throwRuntimeError("BehaviourDSLCommon::treatBehaviour",
                              "invalid behaviour name '" + b + "'");
    }
    if (this->overriden_implementation_name.empty()) {
      this->setMaterialKnowledgeIdentifier(b);
    }
  }  // end of treatBehaviour

  void BehaviourDSLCommon::readStringList(std::vector<std::string>& cont) {
    this->checkNotEndOfFile("BehaviourDSLCommon::readStringList",
                            "Cannot read interface name.");
    auto endOfTreatment = false;
    while ((this->current != this->tokens.end()) && (!endOfTreatment)) {
      const auto s = this->current->value;
      if (!isValidIdentifier(s)) {
        --(this->current);
        this->throwRuntimeError(
            "BehaviourDSLCommon::readStringList",
            "interface name is not valid (read '" + s + "')");
      }
      ++(this->current);
      this->checkNotEndOfFile("BehaviourDSLCommon::readStringList");
      if (this->current->value == ",") {
        ++(this->current);
      } else if (this->current->value == ";") {
        endOfTreatment = true;
        ++(this->current);
      } else {
        this->throwRuntimeError("BehaviourDSLCommon::readStringList",
                                "',' or ';' expected after '" + s + "'");
      }
      if (find(cont.begin(), cont.end(), s) != cont.end()) {
        this->throwRuntimeError("BehaviourDSLCommon::readStringList",
                                "'" + s + "' has already been registred.\n");
      }
      cont.push_back(s);
    }
    if (!endOfTreatment) {
      --(this->current);
      this->throwRuntimeError("BehaviourDSLCommon::readStringList",
                              "Expected ';' before end of file.");
    }
  }

  std::set<BehaviourDSLCommon::Hypothesis>
  BehaviourDSLCommon::readHypothesesList() {
    std::set<Hypothesis> mh;
    this->readHypothesesList(mh);
    return mh;
  }  // end of readHypothesesList

  void BehaviourDSLCommon::readHypothesesList(std::set<Hypothesis>& h) {
    h.clear();
    if (this->current == this->tokens.end()) {
      h.insert(ModellingHypothesis::UNDEFINEDHYPOTHESIS);
      return;
    }
    if (this->current->value != "<") {
      h.insert(ModellingHypothesis::UNDEFINEDHYPOTHESIS);
      return;
    }
    auto values = std::vector<tfel::utilities::Token>{};
    this->readList(values, "BehaviourDSLCommon::readHypothesesList", "<", ">",
                   true);
    for (const auto& v : values) {
      if (v.flag == tfel::utilities::Token::String) {
        this->appendToHypothesesList(h, v.value.substr(1, v.value.size() - 2));
      } else {
        this->appendToHypothesesList(h, v.value);
      }
    }
    if (h.empty()) {
      h.insert(ModellingHypothesis::UNDEFINEDHYPOTHESIS);
    }
  }  // end of readHypothesesList

  void BehaviourDSLCommon::readVariableList(
      VariableDescriptionContainer& v,
      std::set<Hypothesis>& h,
      void (BehaviourDescription::*m)(const Hypothesis,
                                      const VariableDescriptionContainer&,
                                      const BehaviourData::RegistrationStatus),
      const bool b1) {
    h.clear();
    v.clear();
    this->readHypothesesList(h);
    this->readVarList(v, b1);
    this->addVariableList(h, v, m);
  }  // end of readVariableList

  void BehaviourDSLCommon::addVariableList(
      const std::set<Hypothesis>& hypotheses,
      const VariableDescriptionContainer& v,
      void (BehaviourDescription::*m)(
          const Hypothesis,
          const VariableDescriptionContainer&,
          const BehaviourData::RegistrationStatus)) {
    for (const auto& h : hypotheses) {
      (this->mb.*m)(h, v, BehaviourData::UNREGISTRED);
    }
  }  // end of addVariableList

  void BehaviourDSLCommon::treatCoef() {
    VarContainer v;
    auto h = std::set<Hypothesis>{};
    this->readVariableList(v, h, &BehaviourDescription::addMaterialProperties,
                           true);
  }  // end of treatCoef

  void BehaviourDSLCommon::treatLocalVar() {
    VarContainer v;
    auto h = std::set<Hypothesis>{};
    this->readVariableList(v, h, &BehaviourDescription::addLocalVariables,
                           true);
  }  // end of treatLocalVar

  void BehaviourDSLCommon::treatInterface() {
    auto& mbif = BehaviourInterfaceFactory::getBehaviourInterfaceFactory();
    auto inames = std::vector<std::string>{};
    this->readStringList(inames);
    for (const auto& i : inames) {
      if (this->interfaces.count(i) == 0) {
        this->interfaces.insert({i, mbif.getInterface(i)});
      }
    }
  }  // end of treatInterface

  void BehaviourDSLCommon::setInterfaces(const std::set<std::string>& inames) {
    auto& mbif = BehaviourInterfaceFactory::getBehaviourInterfaceFactory();
    for (const auto& i : inames) {
      if (this->interfaces.count(i) == 0) {
        this->interfaces.insert({i, mbif.getInterface(i)});
      }
    }
  }  // end of setInterfaces

  void BehaviourDSLCommon::treatAPrioriTimeStepScalingFactor() {
    this->treatCodeBlock(*this, BehaviourData::APrioriTimeStepScalingFactor,
                         &BehaviourDSLCommon::standardModifier, true, true);
  }

  void BehaviourDSLCommon::treatIntegrator() {
    this->treatCodeBlock(*this, BehaviourData::Integrator,
                         &BehaviourDSLCommon::standardModifier, true, true);
  }  // end of treatIntegrator

  void BehaviourDSLCommon::treatAPosterioriTimeStepScalingFactor() {
    this->treatCodeBlock(*this, BehaviourData::APosterioriTimeStepScalingFactor,
                         &BehaviourDSLCommon::standardModifier, true, true);
  }

  void BehaviourDSLCommon::treatStateVariable() {
    VarContainer v;
    auto h = std::set<Hypothesis>{};
    this->readVariableList(v, h, &BehaviourDescription::addStateVariables,
                           true);
  }  // end of treatStateVariable

  void BehaviourDSLCommon::treatAuxiliaryStateVariable() {
    VarContainer v;
    auto h = std::set<Hypothesis>{};
    this->readVariableList(
        v, h, &BehaviourDescription::addAuxiliaryStateVariables, true);
  }  // end of treatAuxiliaryStateVariable

  void BehaviourDSLCommon::treatExternalStateVariable() {
    VarContainer v;
    auto h = std::set<Hypothesis>{};
    this->readVariableList(
        v, h, &BehaviourDescription::addExternalStateVariables, true);
  }  // end of treatExternalStateVariable()

  void BehaviourDSLCommon::treatInitializeFunctionVariable() {
    auto v = VariableDescriptionContainer{};
    auto hypotheses = std::set<Hypothesis>{};
    this->readHypothesesList(hypotheses);
    this->readVarList(v, true);
    for (const auto& h : hypotheses) {
      this->mb.addInitializeFunctionVariables(h, v);
    }
  }  // end of treatInitializeFunctionVariable()

  void BehaviourDSLCommon::treatPostProcessingVariable() {
    auto v = VariableDescriptionContainer{};
    auto hypotheses = std::set<Hypothesis>{};
    this->readHypothesesList(hypotheses);
    this->readVarList(v, true);
    for (const auto& h : hypotheses) {
      this->mb.addPostProcessingVariables(h, v);
    }
  }  // end of treatPostProcessingVariable()

  void BehaviourDSLCommon::treatBounds() {
    auto hs = std::set<Hypothesis>{};
    this->readHypothesesList(hs);
    auto b = this->current;
    for (const auto& h : hs) {
      this->current = b;
      const auto r = this->readVariableBounds();
      const auto v = extractVariableNameAndArrayPosition(r.first);
      if (std::get<1>(v)) {
        this->mb.setBounds(h, std::get<0>(v), std::get<2>(v), r.second);
      } else {
        this->mb.setBounds(h, std::get<0>(v), r.second);
      }
    }
    this->readSpecifiedToken("BehaviourDSLCommon::treatBounds", ";");
  }  // end of treatBounds

  void BehaviourDSLCommon::treatPhysicalBounds() {
    auto hs = std::set<Hypothesis>{};
    this->readHypothesesList(hs);
    auto b = current;
    for (const auto& h : hs) {
      this->current = b;
      const auto bounds = this->readVariableBounds();
      this->mb.setPhysicalBounds(h, bounds.first, bounds.second);
    }
    this->readSpecifiedToken("BehaviourDSLCommon::treatBounds", ";");
  }  // end of treatPhysicalBounds

  void BehaviourDSLCommon::registerDefaultVarNames() {
    const auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    // all available tangent operators for finite strain behaviours
    const auto tos =
        tfel::material::getFiniteStrainBehaviourTangentOperatorFlags();
    // stiffness tensor
    this->mb.registerMemberName(h, "D");
    // stiffness tensor at the end of the time step
    this->mb.registerMemberName(h, "D_tdt");
    // tangent operator
    this->mb.registerMemberName(h, "Dt");
    this->reserveName("N");
    this->reserveName("Type");
    this->reserveName("use_qt");
    this->reserveName("src1");
    this->reserveName("src2");
    this->reserveName("policy_value");
    this->reserveName("integrate");
    this->reserveName("Psi_s");
    this->reserveName("Psi_d");
    this->reserveName("thermal_expansion_reference_temperature");
    this->reserveName("initial_geometry_reference_temperature");
    this->mb.registerMemberName(h, "computeThermodynamicForces");
    this->mb.registerMemberName(h, "computeFinalThermodynamicForces");
    this->mb.registerMemberName(h, "computeStressFreeExpansion");
    this->mb.registerMemberName(h, "computeInternalEnergy");
    this->mb.registerMemberName(h, "computeDissipatedEnergy");
    this->mb.registerMemberName(h, "computeFdF");
    this->mb.registerMemberName(h, "updateIntegrationVariables");
    this->mb.registerMemberName(h, "updateStateVariables");
    this->mb.registerMemberName(h, "updateAuxiliaryStateVariables");
    this->mb.registerMemberName(h, "getTangentOperator");
    this->mb.registerMemberName(h, "getMinimalTimeStepScalingFactor");
    this->mb.registerMemberName(h, "computeAPrioriTimeStepScalingFactor");
    this->mb.registerMemberName(h, "computeAPrioriTimeStepScalingFactorII");
    this->mb.registerMemberName(h, "computeAPosterioriTimeStepScalingFactor");
    this->mb.registerMemberName(h, "computeAPosterioriTimeStepScalingFactorII");
    this->reserveName("computeTangentOperator_");
    this->mb.registerMemberName(h, "computeConsistentTangentOperator");
    for (const auto& to : tos) {
      const auto ktype =
          convertFiniteStrainBehaviourTangentOperatorFlagToString(to);
      this->mb.registerMemberName(h, ktype);
      this->mb.registerMemberName(h,
                                  "computeConsistentTangentOperator_" + ktype);
      this->mb.registerMemberName(h, "tangentOperator_" + ktype);
    }
    this->reserveName("tangentOperator_sk2");
    this->reserveName("computePredictionOperator");
    for (const auto& to : tos) {
      const auto ktype =
          convertFiniteStrainBehaviourTangentOperatorFlagToString(to);
      this->mb.registerMemberName(h, "computePredictionOperator_" + ktype);
    }
    this->reserveName("smt");
    this->reserveName("smflag");
    this->reserveName("dl0_l0");
    this->reserveName("dl1_l0");
    this->reserveName("dl01_l0");
    this->reserveName("alpha_Ti");
    this->reserveName("alpha0_Ti");
    this->reserveName("alpha1_Ti");
    this->reserveName("alpha2_Ti");
    this->reserveName("alpha_T_t");
    this->reserveName("alpha_T_t_dt");
    this->reserveName("alpha0_T_t");
    this->reserveName("alpha0_T_t_dt");
    this->reserveName("alpha1_T_t");
    this->reserveName("alpha1_T_t_dt");
    this->reserveName("alpha2_T_t");
    this->reserveName("alpha2_T_t_dt");
    this->reserveName("StressFreeExpansionType");
    this->reserveName("behaviourData");
    this->reserveName("time_scaling_factor");
    this->reserveName("mp_bounds_check_status");
    this->reserveName("initial_state");
  }  // end of registerDefaultVarNames

  bool BehaviourDSLCommon::useQt() const {
    return this->mb.useQt();
  }  // end of useQt

  void BehaviourDSLCommon::disableQuantitiesUsageIfNotAlreadySet() {
    this->mb.disableQuantitiesUsageIfNotAlreadySet();
  }  // end of disableQuantitiesUsageIfNotAlreadySet

  void BehaviourDSLCommon::reserveName(const std::string& n) {
    this->mb.reserveName(ModellingHypothesis::UNDEFINEDHYPOTHESIS, n);
  }

  bool BehaviourDSLCommon::isNameReserved(const std::string& n) const {
    return this->mb.isNameReserved(n);
  }

  void BehaviourDSLCommon::writeVariablesDeclarations(
      std::ostream& f,
      const VariableDescriptionContainer& v,
      const std::string& prefix,
      const std::string& suffix,
      const std::string& fileName,
      const bool useTimeDerivative) const {
    for (const auto& e : v) {
      this->writeVariableDeclaration(f, e, prefix, suffix, fileName,
                                     useTimeDerivative);
    }
  }  // end of writeVariablesDeclarations

  void BehaviourDSLCommon::writeVariableDeclaration(
      std::ostream& f,
      const VariableDescription& v,
      const std::string& prefix,
      const std::string& suffix,
      const std::string& fileName,
      const bool useTimeDerivative) const {
    const auto n = prefix + v.name + suffix;
    const auto t =
        (!useTimeDerivative) ? v.type : this->getTimeDerivativeType(v.type);
    if ((!getDebugMode()) && (v.lineNumber != 0u)) {
      f << "#line " << v.lineNumber << " \"" << fileName << "\"\n";
    }
    if (v.arraySize == 1u) {
      f << t << " " << n << ";\n";
    } else {
      if (this->mb.useDynamicallyAllocatedVector(v.arraySize)) {
        f << "tfel::math::runtime_array<" << t << " > " << n << ";\n";
      } else {
        f << "tfel::math::fsarray<" << v.arraySize << ", " << t << " > " << n
          << ";\n";
      }
    }
  }  // end of writeVariableDeclaration

  void BehaviourDSLCommon::writeIncludes(std::ostream& file) const {
    if ((!file) || (!file.good())) {
      this->throwRuntimeError("BehaviourDSLCommon::writeIncludes",
                              "output file is not valid");
    }
    const auto& h = this->mb.getIncludes();
    if (!h.empty()) {
      file << h << '\n';
    }
  }

  void BehaviourDSLCommon::writeNamespaceBegin(std::ostream& file) const {
    if ((!file) || (!file.good())) {
      this->throwRuntimeError("BehaviourDSLCommon::writeNamespaceBegin",
                              "output file is not valid");
    }
    file << "namespace tfel::material{\n\n";
  }

  void BehaviourDSLCommon::writeNamespaceEnd(std::ostream& file) const {
    if ((!file) || (!file.good())) {
      this->throwRuntimeError("BehaviourDSLCommon::writeNamespaceEnd",
                              "output file is not valid");
    }
    file << "} // end of namespace tfel::material\n\n";
  }

  void BehaviourDSLCommon::writeTypeAliases(std::ostream& file) const {
    if ((!file) || (!file.good())) {
      this->throwRuntimeError("BehaviourDSLCommon::writeTypeAliases",
                              "output file is not valid");
    }
    file << "using ushort =  unsigned short;\n";
    if (this->mb.useQt()) {
      file << "using Types = tfel::config::Types<N, NumericType, use_qt>;\n";
    } else {
      file << "using Types = tfel::config::Types<N, NumericType, false>;\n";
    }
    file << "using Type = NumericType;\n";
    for (const auto& a : getTypeAliases()) {
      file << "using " << a << " = typename Types::" << a << ";\n";
    }
    // tangent operator
    if (this->mb.hasTangentOperator()) {
      file << "using TangentOperator = " << this->mb.getTangentOperatorType()
           << ";\n";
    }
    // physical constants
    if (this->mb.useQt()) {
      file << "using PhysicalConstants = "
           << "tfel::PhysicalConstants<NumericType, use_qt>;\n";
    } else {
      file << "using PhysicalConstants = "
           << "tfel::PhysicalConstants<NumericType, false>;\n";
    }
  }  // end of writeTypeAliases

  std::string BehaviourDSLCommon::getIntegrationVariablesIncrementsInitializers(
      const Hypothesis h) const {
    std::ostringstream f;
    const auto& vc = this->mb.getBehaviourData(h).getIntegrationVariables();
    for (auto p = vc.begin(); p != vc.end(); ++p) {
      const auto& v = *p;
      const auto flag = getTypeFlag(v.type);
      const auto n = v.name;
      const auto t = (!this->useStateVarTimeDerivative)
                         ? v.type
                         : this->getTimeDerivativeType(v.type);
      if (p != vc.begin()) {
        f << ",\n";
      }
      if (flag == SupportedTypes::SCALAR) {
        if (this->mb.useDynamicallyAllocatedVector(v.arraySize)) {
          f << "d" << n << "(" << v.arraySize << "," << t << "(0))";
        } else {
          f << "d" << n << "(" << t << "(0))";
        }
      } else {
        const auto traits = "MathObjectTraits<" + t + ">";
        if (v.arraySize == 1u) {
          f << "d" << n << "(typename tfel::math::" + traits + "::NumType(0))";
        } else {
          if (this->mb.useDynamicallyAllocatedVector(v.arraySize)) {
            f << "d" << n << "(" << v.arraySize << "," << t
              << "(typename tfel::math::" + traits + "::NumType(0)))";
          } else {
            f << "d" << n << "(" << t
              << "(typename tfel::math::" + traits + "::NumType(0)))";
          }
        }
      }
    }
    return f.str();
  }  // end of SupportedTypes::getIntegrationVariablesInitializers

  void BehaviourDSLCommon::checkBehaviourDataFile(std::ostream& os) const {
    if ((!os) || (!os.good())) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::checkBehaviourDataOutputFile",
          "output file is not valid");
    }
  }

  void BehaviourDSLCommon::writeBehaviourDataFileHeader(
      std::ostream& os) const {
    this->checkBehaviourDataFile(os);
    os << "/*!\n"
       << "* \\file   " << this->getBehaviourDataFileName() << '\n'
       << "* \\brief  "
       << "this file implements the " << this->mb.getClassName()
       << "BehaviourData"
       << " class.\n"
       << "*         File generated by " << MFrontHeader::getVersionName()
       << " "
       << "version " << MFrontHeader::getVersionNumber() << '\n';
    if (!this->fd.authorName.empty()) {
      os << "* \\author " << this->fd.authorName << '\n';
    }
    if (!this->fd.date.empty()) {
      os << "* \\date   " << this->fd.date << '\n';
    }
    os << " */\n\n";
  }

  void BehaviourDSLCommon::writeBehaviourDataFileHeaderBegin(
      std::ostream& os) const {
    this->checkBehaviourDataFile(os);
    os << "#ifndef LIB_TFELMATERIAL_" << makeUpperCase(this->mb.getClassName())
       << "_BEHAVIOUR_DATA_HXX\n";
    os << "#define LIB_TFELMATERIAL_" << makeUpperCase(this->mb.getClassName())
       << "_BEHAVIOUR_DATA_HXX\n\n";
  }

  void BehaviourDSLCommon::writeBehaviourDataFileHeaderEnd(
      std::ostream& os) const {
    this->checkBehaviourDataFile(os);
    os << "#endif /* LIB_TFELMATERIAL_"
       << makeUpperCase(this->mb.getClassName()) << "_BEHAVIOUR_DATA_HXX */\n";
  }

  static bool hasVariableOfType(const BehaviourData& bd,
                                const SupportedTypes::TypeFlag f) {
    auto update = [f](const auto& variables) {
      const auto& flags = SupportedTypes::getTypeFlags();
      for (const auto& v : variables) {
        const auto pf = flags.find(v.type);
        if (pf == flags.end()) {
          continue;
        }
        if (pf->second == f) {
          return true;
        }
      }
      return false;
    };
    if ((update(bd.getMaterialProperties())) ||
        (update(bd.getIntegrationVariables())) ||
        (update(bd.getStateVariables())) ||
        (update(bd.getAuxiliaryStateVariables())) ||
        //        (update(bd.getLocalVariables())) ||
        (update(bd.getExternalStateVariables())) ||
        (update(bd.getInitializeFunctionVariables())) ||
        (update(bd.getPostProcessingVariables()))) {
      return true;
    }
    return false;
  }  // end of hasVariableOfType

  static bool hasVariableOfType(const BehaviourDescription& bd,
                                const SupportedTypes::TypeFlag f) {
    using ModellingHypothesis = BehaviourDescription::ModellingHypothesis;
    for (const auto& mv : bd.getMainVariables()) {
      if (mv.first.getTypeFlag() == f) {
        return true;
      }
      if (mv.second.getTypeFlag() == f) {
        return true;
      }
    }
    if (!bd.areAllMechanicalDataSpecialised()) {
      return hasVariableOfType(
          bd.getBehaviourData(ModellingHypothesis::UNDEFINEDHYPOTHESIS), f);
    }
    for (const auto& h : bd.getDistinctModellingHypotheses()) {
      if (hasVariableOfType(bd.getBehaviourData(h), f)) {
        return true;
      }
    }
    return false;
  }  // end of requiresTVectorOrVectorIncludes

  void BehaviourDSLCommon::writeBehaviourDataStandardTFELIncludes(
      std::ostream& os) const {
    auto b1 = false;
    auto b2 = false;
    this->checkBehaviourDataFile(os);
    os << "#include<limits>\n"
       << "#include<string>\n"
       << "#include<sstream>\n"
       << "#include<iostream>\n"
       << "#include<stdexcept>\n"
       << "#include<algorithm>\n\n"
       << "#include\"TFEL/Raise.hxx\"\n"
       << "#include\"TFEL/PhysicalConstants.hxx\"\n"
       << "#include\"TFEL/Config/TFELConfig.hxx\"\n"
       << "#include\"TFEL/Config/TFELTypes.hxx\"\n"
       << "#include\"TFEL/TypeTraits/IsFundamentalNumericType.hxx\"\n"
       << "#include\"TFEL/TypeTraits/IsReal.hxx\"\n"
       << "#include\"TFEL/Math/General/Abs.hxx\"\n"
       << "#include\"TFEL/Math/General/IEEE754.hxx\"\n"
       << "#include\"TFEL/Math/Array/ViewsArrayIO.hxx\"\n"
       << "#include\"TFEL/Math/Array/fsarrayIO.hxx\"\n"
       << "#include\"TFEL/Math/Array/runtime_arrayIO.hxx\"\n"
       << "#include\"TFEL/Math/fsarray.hxx\"\n"
       << "#include\"TFEL/Math/runtime_array.hxx\"\n";
    if (this->mb.useQt()) {
      os << "#include\"TFEL/Math/qt.hxx\"\n";
      os << "#include\"TFEL/Math/Quantity/qtIO.hxx\"\n";
    }
    this->mb.requiresTVectorOrVectorIncludes(b1, b2);
    if (b1) {
      os << "#include\"TFEL/Math/tvector.hxx\"\n"
         << "#include\"TFEL/Math/Vector/tvectorIO.hxx\"\n";
    }
    if (b2) {
      os << "#include\"TFEL/Math/vector.hxx\"\n";
    }
    os << "#include\"TFEL/Math/tmatrix.hxx\"\n"
       << "#include\"TFEL/Math/Matrix/tmatrixIO.hxx\"\n";
    if (hasVariableOfType(this->mb, SupportedTypes::STENSOR)) {
      os << "#include\"TFEL/Math/stensor.hxx\"\n"
         << "#include\"TFEL/Math/Stensor/StensorConceptIO.hxx\"\n"
         << "#include\"TFEL/Math/st2tost2.hxx\"\n"
         << "#include\"TFEL/Math/ST2toST2/ST2toST2ConceptIO.hxx\"\n";
    }
    if (hasVariableOfType(this->mb, SupportedTypes::TENSOR)) {
      os << "#include\"TFEL/Math/tensor.hxx\"\n"
         << "#include\"TFEL/Math/Tensor/TensorConceptIO.hxx\"\n"
         << "#include\"TFEL/Math/t2tot2.hxx\"\n"
         << "#include\"TFEL/Math/T2toT2/T2toT2ConceptIO.hxx\"\n";
    }
    if ((hasVariableOfType(this->mb, SupportedTypes::STENSOR)) &&
        (hasVariableOfType(this->mb, SupportedTypes::TENSOR))) {
      os << "#include\"TFEL/Math/t2tost2.hxx\"\n"
         << "#include\"TFEL/Math/T2toST2/T2toST2ConceptIO.hxx\"\n"
         << "#include\"TFEL/Math/st2tot2.hxx\"\n"
         << "#include\"TFEL/Math/ST2toT2/ST2toT2ConceptIO.hxx\"\n";
    }
    if ((this->mb.getBehaviourType() ==
         BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) ||
        (this->mb.getBehaviourType() ==
         BehaviourDescription::GENERALBEHAVIOUR)) {
      os << "#include\"TFEL/Math/ST2toST2/ConvertToTangentModuli.hxx\"\n"
         << "#include\"TFEL/Math/ST2toST2/"
            "ConvertSpatialModuliToKirchhoffJaumanRateModuli.hxx\"\n"
         << "#include\"TFEL/Material/"
            "FiniteStrainBehaviourTangentOperator.hxx\"\n";
    }
    os << "#include\"TFEL/Material/ModellingHypothesis.hxx\"\n\n";
  }  // end of writeBehaviourDataStandardTFELIncludes

  void BehaviourDSLCommon::writeBehaviourDataDefaultMembers(
      std::ostream& os) const {
    this->checkBehaviourDataFile(os);
    if (this->mb.getAttribute(BehaviourDescription::requiresStiffnessTensor,
                              false)) {
      os << "//! stiffness tensor computed by the calling solver\n"
         << "StiffnessTensor D;\n";
    }
    if (this->mb.getAttribute(
            BehaviourDescription::requiresThermalExpansionCoefficientTensor,
            false)) {
      os << "ThermalExpansionCoefficientTensor A;\n";
    }
    for (const auto& mv : this->mb.getMainVariables()) {
      if (Gradient::isIncrementKnown(mv.first)) {
        os << mv.first.type << " " << mv.first.name << ";\n\n";
      } else {
        os << mv.first.type << " " << mv.first.name << "0;\n\n";
      }
      os << mv.second.type << " " << mv.second.name << ";\n\n";
    }
  }

  void BehaviourDSLCommon::writeBehaviourDataTypeAliases(
      std::ostream& os) const {
    this->checkBehaviourDataFile(os);
    os << "static constexpr unsigned short TVectorSize = N;\n"
       << "using StensorDimeToSize = tfel::math::StensorDimeToSize<N>;\n"
       << "static constexpr unsigned short StensorSize = "
       << "StensorDimeToSize::value;\n"
       << "using TensorDimeToSize = tfel::math::TensorDimeToSize<N>;\n"
       << "static constexpr unsigned short TensorSize = "
       << "TensorDimeToSize::value;\n\n";
    this->writeTypeAliases(os);
    os << '\n';
  }

  void BehaviourDSLCommon::writeBehaviourDataDisabledConstructors(
      std::ostream& os) const {
    this->checkBehaviourDataFile(os);
  }

  void BehaviourDSLCommon::writeBehaviourDataConstructors(
      std::ostream& os, const Hypothesis h) const {
    const auto& md = this->mb.getBehaviourData(h);
    this->checkBehaviourDataFile(os);
    os << "/*!\n"
       << "* \\brief Default constructor\n"
       << "*/\n"
       << this->mb.getClassName() << "BehaviourData()\n"
       << "{}\n\n"
       << "/*!\n"
       << "* \\brief copy constructor\n"
       << "*/\n"
       << this->mb.getClassName() << "BehaviourData(const "
       << this->mb.getClassName() << "BehaviourData& src)\n"
       << ": ";
    auto first = true;
    if (this->mb.getAttribute(BehaviourDescription::requiresStiffnessTensor,
                              false)) {
      os << "D(src.D)";
      first = false;
    }
    if (this->mb.getAttribute(
            BehaviourDescription::requiresThermalExpansionCoefficientTensor,
            false)) {
      if (!first) {
        os << ",\n";
      }
      os << "A(src.A)";
      first = false;
    }
    for (const auto& mv : this->mb.getMainVariables()) {
      if (!first) {
        os << ",\n";
      }
      if (Gradient::isIncrementKnown(mv.first)) {
        os << mv.first.name << "(src." << mv.first.name << "),\n";
      } else {
        os << mv.first.name << "0(src." << mv.first.name << "0),\n";
      }
      os << mv.second.name << "(src." << mv.second.name << ")";
      first = false;
    }
    for (const auto& v : md.getMaterialProperties()) {
      if (!first) {
        os << ",\n";
      }
      os << v.name << "(src." << v.name << ")";
      first = false;
    }
    for (const auto& v : md.getStateVariables()) {
      if (!first) {
        os << ",\n";
      }
      os << v.name << "(src." << v.name << ")";
      first = false;
    }
    for (const auto& v : md.getAuxiliaryStateVariables()) {
      if (!first) {
        os << ",\n";
      }
      os << v.name << "(src." << v.name << ")";
      first = false;
    }
    for (const auto& v : md.getExternalStateVariables()) {
      if (!first) {
        os << ",\n";
      }
      os << v.name << "(src." << v.name << ")";
      first = false;
    }
    os << "\n{}\n\n";
    // Creating constructor for external interfaces
    for (const auto& i : this->interfaces) {
      if (i.second->isBehaviourConstructorRequired(h, this->mb)) {
        i.second->writeBehaviourDataConstructor(os, h, this->mb);
      }
    }
  }  // end of writeBehaviourDataConstructors

  void BehaviourDSLCommon::writeBehaviourDataAssignementOperator(
      std::ostream& os, const Hypothesis h) const {
    const auto& md = this->mb.getBehaviourData(h);
    this->checkBehaviourDataFile(os);
    os << "/*\n"
       << "* \\brief Assignement operator\n"
       << "*/\n"
       << this->mb.getClassName() << "BehaviourData&\n"
       << "operator=(const " << this->mb.getClassName()
       << "BehaviourData& src){\n";
    for (const auto& dv : this->mb.getMainVariables()) {
      if (Gradient::isIncrementKnown(dv.first)) {
        os << "this->" << dv.first.name << " = src." << dv.first.name << ";\n";
      } else {
        os << "this->" << dv.first.name << "0 = src." << dv.first.name
           << "0;\n";
      }
      os << "this->" << dv.second.name << " = src." << dv.second.name << ";\n";
    }
    for (const auto& mp : md.getMaterialProperties()) {
      os << "this->" << mp.name << " = src." << mp.name << ";\n";
    }
    for (const auto& iv : md.getStateVariables()) {
      os << "this->" << iv.name << " = src." << iv.name << ";\n";
    }
    for (const auto& iv : md.getAuxiliaryStateVariables()) {
      os << "this->" << iv.name << " = src." << iv.name << ";\n";
    }
    for (const auto& ev : md.getExternalStateVariables()) {
      os << "this->" << ev.name << " = src." << ev.name << ";\n";
    }
    os << "return *this;\n"
       << "}\n\n";
  }  // end of writeBehaviourAssignementOperator

  void BehaviourDSLCommon::writeBehaviourDataExport(std::ostream& os,
                                                    const Hypothesis h) const {
    this->checkBehaviourDataFile(os);
    for (const auto& i : this->interfaces) {
      i.second->exportMechanicalData(os, h, this->mb);
    }
  }

  void BehaviourDSLCommon::writeBehaviourDataInitializeMethods(
      std::ostream& os, const Hypothesis h) const {
    this->checkBehaviourDataFile(os);
    const auto& d = this->mb.getBehaviourData(h);
    for (const auto& n : d.getUserDefinedInitializeCodeBlocksNames()) {
      const auto& c = d.getUserDefinedInitializeCodeBlock(n);
      os << "void initialize" << n << "(){\n"
         << "using namespace std;\n"
         << "using namespace tfel::math;\n"
         << "using std::vector;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
      os << c.code;
      os << "} // end of initialize" << n << "\n\n";
    }
  }  // end of writeBehaviourDataInitializeMethods

  void BehaviourDSLCommon::writeBehaviourDataPublicMembers(
      std::ostream& os) const {
    this->checkBehaviourDataFile(os);
    if (this->mb.getAttribute(BehaviourDescription::requiresStiffnessTensor,
                              false)) {
      os << "StiffnessTensor& getStiffnessTensor()\n"
         << "{\nreturn this->D;\n}\n\n"
         << "const StiffnessTensor& getStiffnessTensor() const\n"
         << "{\nreturn this->D;\n}\n\n";
    }
    if (this->mb.getAttribute(
            BehaviourDescription::requiresThermalExpansionCoefficientTensor,
            false)) {
      os << "ThermalExpansionCoefficientTensor& "
         << "getThermalExpansionCoefficientTensor()\n"
         << "{\nreturn this->A;\n}\n\n"
         << "const ThermalExpansionCoefficientTensor& "
         << "getThermalExpansionCoefficientTensor() const\n"
         << "{\nreturn this->A;\n}\n\n";
    }
  }  // end of writeBehaviourDataPublicMembers

  void BehaviourDSLCommon::writeBehaviourDataClassHeader(
      std::ostream& os) const {
    this->checkBehaviourDataFile(os);
    os << "/*!\n"
       << "* \\class " << this->mb.getClassName() << "BehaviourData\n"
       << "* \\brief This class implements the " << this->mb.getClassName()
       << "BehaviourData"
       << " .\n"
       << "* \\tparam H: modelling hypothesis.\n"
       << "* \\tparam NumericType: numerical type.\n"
       << "* \\tparam use_qt: conditional saying if quantities are use.\n";
    if (!this->fd.authorName.empty()) {
      os << "* \\author " << this->fd.authorName << '\n';
    }
    if (!this->fd.date.empty()) {
      os << "* \\date   " << this->fd.date << '\n';
    }
    os << "*/\n";
  }

  void BehaviourDSLCommon::writeBehaviourDataForwardDeclarations(
      std::ostream& os) const {
    this->checkBehaviourDataFile(os);
    os << "//! \\brief forward declaration\n"
       << "template<ModellingHypothesis::Hypothesis hypothesis,typename,bool>\n"
       << "class " << this->mb.getClassName() << "BehaviourData;\n\n"
       << "//! \\brief forward declaration\n"
       << "template<ModellingHypothesis::Hypothesis hypothesis, "
       << "typename NumericType,bool use_qt>\n"
       << "class " << this->mb.getClassName() << "IntegrationData;\n\n";
    if (this->mb.useQt()) {
      os << "//! \\brief forward declaration\n";
      os << "template<ModellingHypothesis::Hypothesis hypothesis, "
         << "typename NumericType, bool use_qt>\n";
      os << "std::ostream&\n operator <<(std::ostream&,";
      os << "const " << this->mb.getClassName()
         << "BehaviourData<hypothesis, NumericType, use_qt>&);\n\n";
    } else {
      os << "//! \\brief forward declaration\n";
      os << "template<ModellingHypothesis::Hypothesis hypothesis,"
         << "typename NumericType>\n";
      os << "std::ostream&\n operator <<(std::ostream&,";
      os << "const " << this->mb.getClassName()
         << "BehaviourData<hypothesis, NumericType,false>&);\n\n";
    }
    // maintenant, il faut déclarer toutes les spécialisations partielles...
    for (const auto& h : this->mb.getModellingHypotheses()) {
      if (this->mb.hasSpecialisedMechanicalData(h)) {
        if (this->mb.useQt()) {
          os << "//! \\brief forward declaration\n"
             << "template<typename NumericType,bool use_qt>\n"
             << "std::ostream&\n operator <<(std::ostream&,"
             << "const " << this->mb.getClassName()
             << "BehaviourData<ModellingHypothesis::"
             << ModellingHypothesis::toUpperCaseString(h)
             << ", NumericType, use_qt>&);\n\n";
        } else {
          os << "//! \\brief forward declaration\n"
             << "template<typename NumericType>\n"
             << "std::ostream&\n operator <<(std::ostream&,"
             << "const " << this->mb.getClassName()
             << "BehaviourData<ModellingHypothesis::"
             << ModellingHypothesis::toUpperCaseString(h)
             << ", NumericType, false>&);\n\n";
        }
      }
    }
  }

  void BehaviourDSLCommon::writeBehaviourDataClassBegin(
      std::ostream& os, const Hypothesis h) const {
    this->checkBehaviourDataFile(os);
    if (h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
      if (this->mb.useQt()) {
        os << "template<ModellingHypothesis::Hypothesis hypothesis,"
           << "typename NumericType,bool use_qt>\n";
        os << "class " << this->mb.getClassName() << "BehaviourData\n";
      } else {
        os << "template<ModellingHypothesis::Hypothesis hypothesis, "
           << "typename NumericType>\n";
        os << "class " << this->mb.getClassName()
           << "BehaviourData<hypothesis, NumericType,false>\n";
      }
    } else {
      if (this->mb.useQt()) {
        os << "template<typename NumericType,bool use_qt>\n";
        os << "class " << this->mb.getClassName()
           << "BehaviourData<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, use_qt>\n";
      } else {
        os << "template<typename NumericType>\n";
        os << "class " << this->mb.getClassName()
           << "BehaviourData<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, false>\n";
      }
    }
    os << "{\n\n";
    if (h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
      os << "static constexpr ModellingHypothesis::Hypothesis hypothesis = "
         << "ModellingHypothesis::" << ModellingHypothesis::toUpperCaseString(h)
         << ";\n";
    }
    os << "static constexpr unsigned short N = "
       << "ModellingHypothesisToSpaceDimension<hypothesis>::value;\n"
       << "static_assert(N==1||N==2||N==3);\n"
       << "static_assert(tfel::typetraits::"
       << "IsFundamentalNumericType<NumericType>::cond);\n"
       << "static_assert(tfel::typetraits::IsReal<NumericType>::cond);\n\n"
       << "friend std::ostream& operator<< <>(std::ostream&,const "
       << this->mb.getClassName() << "BehaviourData&);\n\n"
       << "/* integration data is declared friend to access"
       << "   driving variables at the beginning of the time step */\n";
    if (this->mb.useQt()) {
      os << "friend class " << this->mb.getClassName()
         << "IntegrationData<hypothesis, NumericType, use_qt>;\n\n";
    } else {
      os << "friend class " << this->mb.getClassName()
         << "IntegrationData<hypothesis, NumericType, false>;\n\n";
    }
  }

  void BehaviourDSLCommon::writeBehaviourDataClassEnd(std::ostream& os) const {
    this->checkBehaviourDataFile(os);
    os << "}; // end of " << this->mb.getClassName() << "BehaviourData"
       << "class\n\n";
  }

  void BehaviourDSLCommon::writeBehaviourDataMaterialProperties(
      std::ostream& os, const Hypothesis h) const {
    this->checkBehaviourDataFile(os);
    this->writeVariablesDeclarations(
        os, this->mb.getBehaviourData(h).getMaterialProperties(), "", "",
        this->fd.fileName, false);
    os << '\n';
  }

  void BehaviourDSLCommon::writeBehaviourDataStateVariables(
      std::ostream& os, const Hypothesis h) const {
    this->checkBehaviourDataFile(os);
    const auto& d = this->mb.getBehaviourData(h);
    this->writeVariablesDeclarations(os, d.getStateVariables(), "", "",
                                     this->fd.fileName, false);
    this->writeVariablesDeclarations(os, d.getAuxiliaryStateVariables(), "", "",
                                     this->fd.fileName, false);
    this->writeVariablesDeclarations(os, d.getExternalStateVariables(), "", "",
                                     this->fd.fileName, false);
    os << '\n';
  }

  void BehaviourDSLCommon::writeBehaviourDataOutputOperator(
      std::ostream& os, const Hypothesis h) const {
    this->checkBehaviourFile(os);
    const auto& d = this->mb.getBehaviourData(h);
    if (h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
      if (this->mb.useQt()) {
        os << "template<ModellingHypothesis::Hypothesis hypothesis,"
           << "typename NumericType, bool use_qt>\n";
        os << "std::ostream&\n";
        os << "operator <<(std::ostream& os,";
        os << "const " << this->mb.getClassName()
           << "BehaviourData<hypothesis, NumericType, use_qt>& b)\n";
      } else {
        os << "template<ModellingHypothesis::Hypothesis hypothesis, "
           << "typename NumericType>\n";
        os << "std::ostream&\n";
        os << "operator <<(std::ostream& os,";
        os << "const " << this->mb.getClassName()
           << "BehaviourData<hypothesis, NumericType, false>& b)\n";
      }
    } else {
      if (this->mb.useQt()) {
        os << "template<typename NumericType, bool use_qt>\n";
        os << "std::ostream&\n";
        os << "operator <<(std::ostream& os,";
        os << "const " << this->mb.getClassName()
           << "BehaviourData<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, use_qt>& b)\n";
      } else {
        os << "template<typename NumericType>\n";
        os << "std::ostream&\n";
        os << "operator <<(std::ostream& os,";
        os << "const " << this->mb.getClassName()
           << "BehaviourData<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, false>& b)\n";
      }
    }
    os << "{\n";
    for (const auto& v : this->mb.getMainVariables()) {
      if (Gradient::isIncrementKnown(v.first)) {
        os << "os << \"" << displayName(v.first) << " : \" << b."
           << v.first.name << " << '\\n';\n";
      } else {
        if (getUnicodeOutputOption()) {
          os << "os << \"" << displayName(v.first) << "\u2080 : \" << b."
             << v.first.name << "0 << '\\n';\n";
        } else {
          os << "os << \"" << displayName(v.first) << "0 : \" << b."
             << v.first.name << "0 << '\\n';\n";
        }
      }
      os << "os << \"" << displayName(v.second) << " : \" << b."
         << v.second.name << " << '\\n';\n";
    }
    for (const auto& v : d.getMaterialProperties()) {
      os << "os << \"" << displayName(v) << " : \" << b." << v.name
         << " << '\\n';\n";
    }
    for (const auto& v : d.getStateVariables()) {
      os << "os << \"" << displayName(v) << " : \" << b." << v.name
         << " << '\\n';\n";
    }
    for (const auto& v : d.getAuxiliaryStateVariables()) {
      os << "os << \"" << displayName(v) << " : \" << b." << v.name
         << " << '\\n';\n";
    }
    for (const auto& v : d.getExternalStateVariables()) {
      os << "os << \"" << displayName(v) << " : \" << b." << v.name
         << " << '\\n';\n";
    }
    os << "return os;\n"
       << "}\n\n";
  }  //  BehaviourDSLCommon::writeBehaviourDataOutputOperator

  void BehaviourDSLCommon::writeBehaviourDataFileBegin(std::ostream& os) const {
    this->checkBehaviourDataFile(os);
    this->writeBehaviourDataFileHeader(os);
    this->writeBehaviourDataFileHeaderBegin(os);
    this->writeBehaviourDataStandardTFELIncludes(os);
    this->writeIncludes(os);
    // includes specific to interfaces
    for (const auto& i : this->interfaces) {
      i.second->writeInterfaceSpecificIncludes(os, this->mb);
    }
    this->writeNamespaceBegin(os);
    this->writeBehaviourDataForwardDeclarations(os);
  }  // end of writeBehaviourDataFile

  void BehaviourDSLCommon::writeBehaviourDataClass(std::ostream& os,
                                                   const Hypothesis h) const {
    this->checkBehaviourDataFile(os);
    this->writeBehaviourDataClassBegin(os, h);
    this->writeBehaviourDataTypeAliases(os);
    os << "protected:\n\n";
    this->writeBehaviourDataDefaultMembers(os);
    this->writeBehaviourDataMaterialProperties(os, h);
    this->writeBehaviourDataStateVariables(os, h);
    os << "public:\n\n";
    this->writeBehaviourDataDisabledConstructors(os);
    this->writeBehaviourDataConstructors(os, h);
    this->writeBehaviourDataMainVariablesSetters(os);
    this->writeBehaviourDataPublicMembers(os);
    this->writeBehaviourDataAssignementOperator(os, h);
    this->writeBehaviourDataInitializeMethods(os, h);
    this->writeBehaviourDataExport(os, h);
    this->writeBehaviourDataClassEnd(os);
    this->writeBehaviourDataOutputOperator(os, h);
  }

  void BehaviourDSLCommon::writeBehaviourDataFileEnd(std::ostream& os) const {
    this->writeNamespaceEnd(os);
    this->writeBehaviourDataFileHeaderEnd(os);
  }  // end of writeBehaviourDataFileEnd

  void BehaviourDSLCommon::checkBehaviourFile(std::ostream& os) const {
    if ((!os) || (!os.good())) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::checkBehaviourDataOutputFile",
          "output file is not valid");
    }
  }

  void BehaviourDSLCommon::writeBehaviourForwardDeclarations(
      std::ostream& os) const {
    this->checkBehaviourFile(os);
    os << "//! \\brief forward declaration\n"
       << "template<ModellingHypothesis::Hypothesis, "
       << "typename NumericType, bool use_qt>\n"
       << "struct " << this->mb.getClassName() << ";\n\n";
    if (this->mb.useQt()) {
      os << "//! \\brief forward declaration\n"
         << "template<ModellingHypothesis::Hypothesis hypothesis, "
         << "typename NumericType, bool use_qt>\n"
         << "std::ostream&\n operator <<(std::ostream&,"
         << "const " << this->mb.getClassName()
         << "<hypothesis, NumericType, use_qt>&);\n\n";
    } else {
      os << "//! \\brief forward declaration\n"
         << "template<ModellingHypothesis::Hypothesis hypothesis, "
         << "typename NumericType>\n"
         << "std::ostream&\n operator <<(std::ostream&,"
         << "const " << this->mb.getClassName()
         << "<hypothesis, NumericType, false>&);\n\n";
    }
    // maintenant, il faut déclarer toutes les spécialisations partielles...
    const auto& mh = this->mb.getModellingHypotheses();
    for (const auto& h : mh) {
      if (this->mb.hasSpecialisedMechanicalData(h)) {
        if (this->mb.useQt()) {
          os << "//! \\brief forward declaration\n"
             << "template<typename NumericType,bool use_qt>\n"
             << "std::ostream&\n operator <<(std::ostream&,"
             << "const " << this->mb.getClassName() << "<ModellingHypothesis::"
             << ModellingHypothesis::toUpperCaseString(h)
             << ", NumericType, use_qt>&);\n\n";
        } else {
          os << "//! \\brief forward declaration\n"
             << "template<typename NumericType>\n"
             << "std::ostream&\n operator <<(std::ostream&,"
             << "const " << this->mb.getClassName() << "<ModellingHypothesis::"
             << ModellingHypothesis::toUpperCaseString(h)
             << ", NumericType, false>&);\n\n";
        }
      }
    }
  }  // end of writeBehaviourClassForwardDeclarations

  void BehaviourDSLCommon::writeBehaviourClassBegin(std::ostream& os,
                                                    const Hypothesis h) const {
    this->checkBehaviourFile(os);
    os << "/*!\n";
    os << "* \\class " << this->mb.getClassName() << '\n';
    os << "* \\brief This class implements the " << this->mb.getClassName()
       << " behaviour.\n";
    os << "* \\tparam hypothesis: modelling hypothesis.\n";
    os << "* \\tparam NumericType: numerical type.\n";
    if (this->mb.useQt()) {
      os << "* \\tparam use_qt: conditional saying if quantities are use.\n";
    }
    if (!this->fd.authorName.empty()) {
      os << "* \\author " << this->fd.authorName << '\n';
    }
    if (!this->fd.date.empty()) {
      os << "* \\date   " << this->fd.date << '\n';
    }
    if (!this->fd.description.empty()) {
      os << this->fd.description << '\n';
    }
    os << "*/\n";
    const auto btype = this->mb.getBehaviourTypeFlag();
    if (h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
      if (this->mb.useQt()) {
        os << "template<ModellingHypothesis::Hypothesis hypothesis, "
           << "typename NumericType, bool use_qt>\n"
           << "struct " << this->mb.getClassName() << " final\n"
           << ": public MechanicalBehaviour<" << btype
           << ",hypothesis, NumericType, use_qt>,\n";
        if (this->mb.getAttribute(BehaviourData::profiling, false)) {
          os << "public " << this->mb.getClassName() << "Profiler,\n";
        }
        os << "public " << this->mb.getClassName()
           << "BehaviourData<hypothesis, NumericType, use_qt>,\n";
        os << "public " << this->mb.getClassName()
           << "IntegrationData<hypothesis, NumericType, use_qt>";
        this->writeBehaviourParserSpecificInheritanceRelationship(os, h);
      } else {
        os << "template<ModellingHypothesis::Hypothesis hypothesis,"
           << "typename NumericType>\n";
        os << "struct " << this->mb.getClassName()
           << "<hypothesis, NumericType, false> final\n";
        os << ": public MechanicalBehaviour<" << btype
           << ",hypothesis, NumericType, false>,\n";
        if (this->mb.getAttribute(BehaviourData::profiling, false)) {
          os << "public " << this->mb.getClassName() << "Profiler,\n";
        }
        os << "public " << this->mb.getClassName()
           << "BehaviourData<hypothesis, NumericType, false>,\n";
        os << "public " << this->mb.getClassName()
           << "IntegrationData<hypothesis, NumericType, false>";
        this->writeBehaviourParserSpecificInheritanceRelationship(os, h);
      }
    } else {
      if (this->mb.useQt()) {
        os << "template<typename NumericType,bool use_qt>\n";
        os << "struct " << this->mb.getClassName() << "<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, use_qt> final\n";
        os << ": public MechanicalBehaviour<" << btype
           << ",ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, use_qt>,\n";
        if (this->mb.getAttribute(BehaviourData::profiling, false)) {
          os << "public " << this->mb.getClassName() << "Profiler,\n";
        }
        os << "public " << this->mb.getClassName()
           << "BehaviourData<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, use_qt>,\n";
        os << "public " << this->mb.getClassName()
           << "IntegrationData<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, use_qt>";
        this->writeBehaviourParserSpecificInheritanceRelationship(os, h);
      } else {
        os << "template<typename NumericType>\n";
        os << "struct " << this->mb.getClassName() << "<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, false> final\n";
        os << ": public MechanicalBehaviour<" << btype
           << ",ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, false>,\n";
        if (this->mb.getAttribute(BehaviourData::profiling, false)) {
          os << "public " << this->mb.getClassName() << "Profiler,\n";
        }
        os << "public " << this->mb.getClassName()
           << "BehaviourData<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, false>,\n";
        os << "public " << this->mb.getClassName()
           << "IntegrationData<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, false>";
        this->writeBehaviourParserSpecificInheritanceRelationship(os, h);
      }
    }
    os << "{\n\n";
    if (h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
      os << "static constexpr ModellingHypothesis::Hypothesis hypothesis = "
         << "ModellingHypothesis::" << ModellingHypothesis::toUpperCaseString(h)
         << ";\n";
    }
    os << "static constexpr unsigned short N = "
          "ModellingHypothesisToSpaceDimension<hypothesis>::value;\n\n";
    os << "static_assert(N==1||N==2||N==3);\n";
    os << "static_assert(tfel::typetraits::"
       << "IsFundamentalNumericType<NumericType>::cond);\n";
    os << "static_assert(tfel::typetraits::IsReal<NumericType>::cond);\n\n";
  }

  void BehaviourDSLCommon::writeBehaviourFriends(std::ostream& os,
                                                 const Hypothesis) const {
    os << "friend std::ostream& operator<< <>(std::ostream&,const "
       << this->mb.getClassName() << "&);\n\n";
  }  // end of writeBehaviourFriends

  void BehaviourDSLCommon::writeBehaviourFileHeader(std::ostream& os) const {
    this->checkBehaviourFile(os);
    os << "/*!\n"
       << "* \\file   " << this->getBehaviourFileName() << '\n'
       << "* \\brief  "
       << "this file implements the " << this->mb.getClassName()
       << " Behaviour.\n"
       << "*         File generated by " << MFrontHeader::getVersionName()
       << " "
       << "version " << MFrontHeader::getVersionNumber() << '\n';
    if (!this->fd.authorName.empty()) {
      os << "* \\author " << this->fd.authorName << '\n';
    }
    if (!this->fd.date.empty()) {
      os << "* \\date   " << this->fd.date << '\n';
    }
    os << " */\n\n";
  }

  void BehaviourDSLCommon::writeBehaviourFileHeaderBegin(
      std::ostream& os) const {
    this->checkBehaviourFile(os);
    os << "#ifndef LIB_TFELMATERIAL_" << makeUpperCase(this->mb.getClassName())
       << "_HXX\n"
       << "#define LIB_TFELMATERIAL_" << makeUpperCase(this->mb.getClassName())
       << "_HXX\n\n";
  }

  void BehaviourDSLCommon::writeBehaviourFileHeaderEnd(std::ostream& os) const {
    this->checkBehaviourFile(os);
    os << "#endif /* LIB_TFELMATERIAL_"
       << makeUpperCase(this->mb.getClassName()) << "_HXX */\n";
  }

  void BehaviourDSLCommon::writeBehaviourClassEnd(std::ostream& os) const {
    this->checkBehaviourFile(os);
    os << "}; // end of " << this->mb.getClassName() << " class\n\n";
  }

  void BehaviourDSLCommon::treatUpdateAuxiliaryStateVariables() {
    this->treatCodeBlock(*this, BehaviourData::UpdateAuxiliaryStateVariables,
                         &BehaviourDSLCommon::standardModifier, true, true);
  }  // end of treatUpdateAuxiliaryStateVarBase

  void BehaviourDSLCommon::treatComputeStressFreeExpansion() {
    this->treatCodeBlock(*this, BehaviourData::ComputeStressFreeExpansion,
                         &BehaviourDSLCommon::standardModifier, true, true);
  }  // end of treatComputeStressFreeExpansion

  void BehaviourDSLCommon::treatSwelling() {
    using VolumeSwelling = BehaviourData::VolumeSwellingStressFreeExpansion;
    using IsotropicSwelling = BehaviourData::IsotropicStressFreeExpansion;
    using OrthotropicSwelling = BehaviourData::OrthotropicStressFreeExpansion;
    using OrthotropicSwellingII =
        BehaviourData::OrthotropicStressFreeExpansionII;
    auto throw_if = [this](const bool b, const std::string& m) {
      if (b) {
        this->throwRuntimeError("BehaviourDSLCommon::treatSwelling", m);
      }
    };
    enum { VOLUME, LINEAR, ORTHOTROPIC, UNDEFINED } etype = UNDEFINED;
    const auto uh = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    throw_if((this->mb.getBehaviourType() !=
              BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) &&
                 (this->mb.getBehaviourType() !=
                  BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR),
             "the @Swelling keyword is only valid for small or "
             "finite strain behaviours");
    this->checkNotEndOfFile("BehaviourDSLCommon::treatSwelling");
    if (this->current->value == "<") {
      auto options = std::vector<tfel::utilities::Token>{};
      this->readList(options, "BehaviourDSLCommon::treatSwelling", "<", ">",
                     true);
      for (const auto& o : options) {
        this->checkNotEndOfFile("BehaviourDSLCommon::treatSwelling");
        if (o.value == "Orthotropic") {
          throw_if(etype != UNDEFINED,
                   "error while treating option "
                   "'Orthotropic', swelling type already defined");
          etype = ORTHOTROPIC;
        } else if (o.value == "Volume") {
          throw_if(etype != UNDEFINED,
                   "error while treating option "
                   "'Volume', swelling type already defined");
          etype = VOLUME;
        } else if (o.value == "Linear") {
          throw_if(etype != UNDEFINED,
                   "error while treating option "
                   "'Linear', swelling type already defined");
          etype = LINEAR;
        } else {
          throw_if(true, "unsupported option '" + o.value + "'");
        }
      }
    }
    throw_if(etype == UNDEFINED,
             "the user must explicitly state if "
             "what kind of swelling is expected using"
             "one of the options 'Linear', 'Volume' or 'Orthotropic'");
    const auto sd = this->readStressFreeExpansionHandler();
    this->readSpecifiedToken("BehaviourDSLCommon::treatSwelling", ";");
    if (sd.size() == 1) {
      throw_if(sd[0].is<BehaviourData::NullExpansion>(),
               "a null swelling is not allowed here");
      if (etype == VOLUME) {
        VolumeSwelling vs = {sd[0]};
        this->mb.addStressFreeExpansion(uh, vs);
      } else if (etype == LINEAR) {
        IsotropicSwelling is = {sd[0]};
        this->mb.addStressFreeExpansion(uh, is);
      } else if (etype == ORTHOTROPIC) {
        throw_if(!sd[0].is<BehaviourData::SFED_ESV>(),
                 "one expects a external state variable name here");
        OrthotropicSwellingII os = {sd[0].get<BehaviourData::SFED_ESV>()};
        this->mb.addStressFreeExpansion(uh, os);
      } else {
        throw_if(true, "internal error");
      }
    } else if (sd.size() == 3) {
      throw_if(etype != ORTHOTROPIC,
               "the 'Orthotropic' option must be "
               "used for an orthotropic swelling");
      throw_if(sd[0].is<BehaviourData::NullExpansion>() &&
                   sd[1].is<BehaviourData::NullExpansion>() &&
                   sd[2].is<BehaviourData::NullExpansion>(),
               "all swelling component are null");
      OrthotropicSwelling os = {sd[0], sd[1], sd[2]};
      this->mb.addStressFreeExpansion(uh, os);
    } else {
      throw_if(true, "invalid number of swelling handler (shall be 1 or 3, " +
                         std::to_string(sd.size()) + " given)");
    }
  }  // end of treatSwelling

  BehaviourData::StressFreeExpansionHandler
  BehaviourDSLCommon::readStressFreeExpansionHandler(
      const tfel::utilities::Token& t) {
    auto throw_if = [this](const bool b, const std::string& m) {
      if (b) {
        this->throwRuntimeError(
            "BehaviourDSLCommon::readStressFreeExpansionHandler", m);
      }
    };
    if (t.flag == tfel::utilities::Token::String) {
      // using an external model
      const auto md =
          this->getModelDescription(t.value.substr(1, t.value.size() - 2));
      // check that the variable
      auto ptr = std::make_shared<ModelDescription>(md);
      return {ptr};
    }
    if (t.value == "0") {
      return {BehaviourData::NullExpansion{}};
    }
    throw_if(!CxxTokenizer::isValidIdentifier(t.value, true),
             "unexpected token '" + t.value +
                 "', expected "
                 "external state variable name");
    // using an external state variable
    // defining modelling hypotheses
    if (!this->mb.areModellingHypothesesDefined()) {
      this->mb.setModellingHypotheses(this->getDefaultModellingHypotheses());
    }
    for (const auto h : this->mb.getDistinctModellingHypotheses()) {
      throw_if(!this->mb.isExternalStateVariableName(h, t.value),
               "no external state variable named '" + t.value +
                   "' "
                   "has been declared");
    }
    return {BehaviourData::SFED_ESV{t.value}};
  }  // end of readStressFreeExpansionHandler

  std::vector<BehaviourData::StressFreeExpansionHandler>
  BehaviourDSLCommon::readStressFreeExpansionHandler() {
    auto throw_if = [this](const bool b, const std::string& m) {
      if (b) {
        this->throwRuntimeError(
            "BehaviourDSLCommon::readStressFreeExpansionHandler", m);
      }
    };
    auto sda = std::vector<tfel::utilities::Token>{};
    auto sd = std::vector<BehaviourData::StressFreeExpansionHandler>{};
    this->checkNotEndOfFile("BehaviourDSLCommon::treatSwelling");
    if (this->current->value == "{") {
      this->readList(sda, "BehaviourDSLCommon::readCodeBlockOptions", "{", "}",
                     true);
    } else {
      sda.push_back(*(this->current));
      ++(this->current);
    }
    if (sda.size() == 1u) {
      sd.push_back(this->readStressFreeExpansionHandler(sda[0]));
    } else if (sda.size() == 3u) {
      throw_if(this->mb.getSymmetryType() != mfront::ORTHOTROPIC,
               "orthotropic swelling is only  supported for "
               "orthotropic behaviours");
      sd.push_back(this->readStressFreeExpansionHandler(sda[0]));
      sd.push_back(this->readStressFreeExpansionHandler(sda[1]));
      sd.push_back(this->readStressFreeExpansionHandler(sda[2]));
    } else {
      throw_if(true,
               "invalid number of swelling description "
               "(expected one or three descriptions)");
    }
    return sd;
  }  // end of readStressFreeExpansionHandler

  void BehaviourDSLCommon::treatAxialGrowth() {
    using AxialGrowth = BehaviourData::AxialGrowth;
    const auto uh = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    auto throw_if = [this](const bool b, const std::string& m) {
      if (b) {
        this->throwRuntimeError("BehaviourDSLCommon::treatAxialGrowth", m);
      }
    };
    throw_if((this->mb.getBehaviourType() !=
              BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) &&
                 (this->mb.getBehaviourType() !=
                  BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR),
             "the @AxialGrowth keyword is only valid for small or "
             "finite strain behaviours");
    throw_if(this->mb.getSymmetryType() != mfront::ORTHOTROPIC,
             "@AxialGrowth is only valid for orthotropic behaviour");
    this->checkNotEndOfFile("BehaviourDSLCommon::treatAxialGrowth");
    auto s = this->readStressFreeExpansionHandler(*(this->current));
    ++(this->current);
    this->readSpecifiedToken("BehaviourDSLCommon::treatAxialGrowth", ";");
    this->mb.addStressFreeExpansion(uh, AxialGrowth{s});
  }  // end of treatAxialGrowth

  void BehaviourDSLCommon::treatRelocation() {
    using Relocation = BehaviourData::Relocation;
    auto throw_if = [this](const bool b, const std::string& m) {
      if (b) {
        this->throwRuntimeError("BehaviourDSLCommon::treatRelocation", m);
      }
    };
    throw_if((this->mb.getBehaviourType() !=
              BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) &&
                 (this->mb.getBehaviourType() !=
                  BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR),
             "the @Relocation keyword is only valid for small or "
             "finite strain behaviours");
    if (!this->mb.areModellingHypothesesDefined()) {
      this->mb.setModellingHypotheses(this->getDefaultModellingHypotheses());
    }
    const auto& mh = this->mb.getModellingHypotheses();
    throw_if(
        (mh.find(ModellingHypothesis::AXISYMMETRICALGENERALISEDPLANESTRESS) ==
         mh.end()) &&
            (mh.find(
                 ModellingHypothesis::AXISYMMETRICALGENERALISEDPLANESTRAIN) ==
             mh.end()) &&
            (mh.find(ModellingHypothesis::GENERALISEDPLANESTRAIN) == mh.end()),
        "the @Relocation keyword has not effect on this behaviour as the none "
        "of "
        "the following hypothesis is supported:\n"
        "- AxisymmetricalGeneralisedPlaneStress\n"
        "- AxisymmetricalGeneralisedPlaneStrain\n"
        "- GeneralisedPlaneStrain");
    this->checkNotEndOfFile("BehaviourDSLCommon::treatRelocation");
    const auto s = this->readStressFreeExpansionHandler(*(this->current));
    ++(this->current);
    this->readSpecifiedToken("BehaviourDSLCommon::treatRelocation", ";");
    auto add = [this, &mh, &s](const Hypothesis h) {
      if (mh.find(h) != mh.end()) {
        this->mb.addStressFreeExpansion(h, Relocation{s});
      }
    };
    add(ModellingHypothesis::AXISYMMETRICALGENERALISEDPLANESTRESS);
    add(ModellingHypothesis::AXISYMMETRICALGENERALISEDPLANESTRAIN);
    add(ModellingHypothesis::GENERALISEDPLANESTRAIN);
  }  // end of treatRelocation

  void BehaviourDSLCommon::writeBehaviourUpdateIntegrationVariables(
      std::ostream& os, const Hypothesis h) const {
    const auto& d = this->mb.getBehaviourData(h);
    this->checkBehaviourFile(os);
    os << "/*!\n"
       << "* \\brief Update internal variables at end of integration\n"
       << "*/\n"
       << "void updateIntegrationVariables()";
    if (!d.getIntegrationVariables().empty()) {
      os << "{\n";
      for (const auto& v : d.getIntegrationVariables()) {
        if (!d.isStateVariableName(v.name)) {
          if (d.isMemberUsedInCodeBlocks(v.name)) {
            os << "this->" << v.name << " += "
               << "this->d" << v.name << ";\n";
          }
        }
      }
      os << "}\n\n";
    } else {
      os << "\n{}\n\n";
    }
  }  // end of writeBehaviourUpdateIntegrationVariables

  void BehaviourDSLCommon::writeBehaviourUpdateStateVariables(
      std::ostream& os, const Hypothesis h) const {
    const auto& d = this->mb.getBehaviourData(h);
    this->checkBehaviourFile(os);
    os << "/*!\n"
       << "* \\brief Update internal variables at end of integration\n"
       << "*/\n"
       << "void updateStateVariables()";
    if (!d.getStateVariables().empty()) {
      os << "{\n";
      for (const auto& v : d.getStateVariables()) {
        os << "this->" << v.name << " += "
           << "this->d" << v.name << ";\n";
      }
      os << "}\n\n";
    } else {
      os << "\n{}\n\n";
    }
  }  // end of writeBehaviourUpdateStateVariables

  void BehaviourDSLCommon::writeBehaviourUpdateAuxiliaryStateVariables(
      std::ostream& os, const Hypothesis h) const {
    os << "/*!\n"
       << "* \\brief Update auxiliary state variables at end of integration\n"
       << "*/\n"
       << "void updateAuxiliaryStateVariables()";
    const auto& em = this->mb.getModelsDescriptions();
    if ((this->mb.hasCode(h, BehaviourData::UpdateAuxiliaryStateVariables)) ||
        (!em.empty())) {
      os << "{\n"
         << "using namespace std;\n"
         << "using namespace tfel::math;\n";
      for (const auto& m : em) {
        if (m.outputs.size() == 1) {
          const auto vn = m.outputs[0].name;
          os << "this->" << vn << " += this->d" << vn << ";\n";
        } else {
          this->throwRuntimeError(
              "BehaviourDSLCommon::writeBehaviourUpdateAuxiliaryStateVariables",
              "only models with one output are supported");
        }
      }
      if (this->mb.hasCode(h, BehaviourData::UpdateAuxiliaryStateVariables)) {
        writeMaterialLaws(os, this->mb.getMaterialLaws());
        os << this->mb.getCode(h, BehaviourData::UpdateAuxiliaryStateVariables)
           << "\n";
      }
      os << "}\n\n";
    } else {
      os << "\n{}\n\n";
    }
  }  // end of  BehaviourDSLCommon::writeBehaviourUpdateAuxiliaryStateVariables

  void BehaviourDSLCommon::writeBehaviourComputeInternalEnergy(
      std::ostream& os, const Hypothesis h) const {
    os << "/*!\n"
       << "* \\brief Update the internal energy at end of the time step\n"
       << "* \\param[in] Psi_s: internal energy at end of the time step\n"
       << "*/\n"
       << "void computeInternalEnergy(stress& Psi_s) const";
    if (this->mb.hasCode(h, BehaviourData::ComputeInternalEnergy)) {
      os << "{\n"
         << "using namespace std;\n"
         << "using namespace tfel::math;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
      os << this->mb.getCode(h, BehaviourData::ComputeInternalEnergy)
         << "\n}\n\n";
    } else {
      os << "\n{\nPsi_s=stress{0};\n}\n\n";
    }
  }  // end of writeBehaviourComputeInternalEnergy

  void BehaviourDSLCommon::writeBehaviourComputeDissipatedEnergy(
      std::ostream& os, const Hypothesis h) const {
    os << "/*!\n"
       << "* \\brief Update the dissipated energy at end of the time step\n"
       << "* \\param[in] Psi_d: dissipated energy at end of the time step\n"
       << "*/\n"
       << "void computeDissipatedEnergy(stress& Psi_d) const";
    if (this->mb.hasCode(h, BehaviourData::ComputeDissipatedEnergy)) {
      os << "{\n"
         << "using namespace std;\n"
         << "using namespace tfel::math;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
      os << this->mb.getCode(h, BehaviourData::ComputeDissipatedEnergy)
         << "\n}\n\n";
    } else {
      os << "\n{\nPsi_d=stress{0};\n}\n\n";
    }
  }  // end of writeBehaviourComputeDissipatedEnergy

  void BehaviourDSLCommon::writeBehaviourComputeSpeedOfSound(
      std::ostream& os, const Hypothesis h) const {
    os << "/*!\n"
       << "* \\brief compute the sound velocity\n"
       << "* \\param[in] rho_m0: mass density in the reference configuration\n"
       << "*/\n";
    if (this->mb.hasCode(h, BehaviourData::ComputeSpeedOfSound)) {
      const auto vs = tfel::unicode::getMangledString("vₛ");
      const auto rho_m0 = tfel::unicode::getMangledString("ρₘ₀");
      os << "speed computeSpeedOfSound(const massdensity& rho_m0) const {\n"
         << "using namespace std;\n"
         << "using namespace tfel::math;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
      os << "const auto " << rho_m0 << " = rho_m0;\n"
         << "auto v_sound = speed{};\n"
         << "auto& " << vs << " = v_sound;\n"
         << this->mb.getCode(h, BehaviourData::ComputeSpeedOfSound)
         << "static_cast<void>(" << rho_m0 << ");\n"
         << "static_cast<void>(" << vs << ");\n"
         << "return v_sound;\n"
         << "}\n\n";
    } else {
      os << "speed computeSpeedOfSound(const massdensity&) const {\n"
         << "return speed(0);\n"
         << "\n}\n\n";
    }
  }  // end of writeBehaviourComputeSpeedOfSound

  bool BehaviourDSLCommon::hasUserDefinedTangentOperatorCode(
      const Hypothesis h) const {
    using tfel::material::getFiniteStrainBehaviourTangentOperatorFlags;
    if (this->mb.getBehaviourType() ==
        BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) {
      // all available tangent operators for finite strain behaviours
      const auto tos = getFiniteStrainBehaviourTangentOperatorFlags();
      // search tangent operators defined by the user
      for (const auto& t : tos) {
        const auto ktype =
            convertFiniteStrainBehaviourTangentOperatorFlagToString(t);
        if (this->mb.hasCode(
                h, std::string(BehaviourData::ComputeTangentOperator) + '-' +
                       ktype)) {
          return true;
        }
      }
    } else {
      if (this->mb.hasCode(h, BehaviourData::ComputeTangentOperator)) {
        return true;
      }
    }
    return false;
  }  // end of hasUserDefinedTangentOperatorCode

  void BehaviourDSLCommon::writeBehaviourIntegrator(std::ostream& os,
                                                    const Hypothesis h) const {
    const auto btype = this->mb.getBehaviourTypeFlag();
    this->checkBehaviourFile(os);
    os << "/*!\n"
       << "* \\brief Integrate behaviour  over the time step\n"
       << "*/\n";
    if (!this->mb.getMainVariables().empty()) {
      os << "IntegrationResult\n"
         << "integrate(const SMFlag smflag, const SMType smt) override{\n";
    } else {
      os << "IntegrationResult\n"
         << "integrate(const SMFlag, const SMType) override{\n";
    }
    os << "using namespace std;\n"
       << "using namespace tfel::math;\n";
    writeMaterialLaws(os, this->mb.getMaterialLaws());
    if (!this->mb.getMainVariables().empty()) {
      if ((this->mb.getBehaviourType() ==
           BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) ||
          (this->mb.getBehaviourType() ==
           BehaviourDescription::COHESIVEZONEMODEL) ||
          (this->mb.getBehaviourType() ==
           BehaviourDescription::GENERALBEHAVIOUR)) {
        if (this->mb.useQt()) {
          os << "raise_if(smflag!=MechanicalBehaviour<" << btype
             << ",hypothesis, NumericType, use_qt>::STANDARDTANGENTOPERATOR,\n"
             << "\"invalid tangent operator flag\");\n";
        } else {
          os << "raise_if(smflag!=MechanicalBehaviour<" << btype
             << ",hypothesis, NumericType, false>::STANDARDTANGENTOPERATOR,\n"
             << "\"invalid tangent operator flag\");\n";
        }
      }
      os << "bool computeTangentOperator_ = smt!=NOSTIFFNESSREQUESTED;\n";
    }
    if (this->mb.hasCode(h, BehaviourData::ComputePredictor)) {
      os << this->mb.getCode(h, BehaviourData::ComputePredictor) << '\n';
    }
    if (this->mb.hasCode(h, BehaviourData::Integrator)) {
      os << this->mb.getCode(h, BehaviourData::Integrator) << '\n';
    }
    os << "this->updateIntegrationVariables();\n"
       << "this->updateStateVariables();\n"
       << "this->updateAuxiliaryStateVariables();\n";
    for (const auto& v :
         this->mb.getBehaviourData(h).getPersistentVariables()) {
      this->writePhysicalBoundsChecks(os, v, false);
    }
    for (const auto& v :
         this->mb.getBehaviourData(h).getPersistentVariables()) {
      this->writeBoundsChecks(os, v, false);
    }
    if (!this->mb.getMainVariables().empty()) {
      if (this->hasUserDefinedTangentOperatorCode(h)) {
        os << "if(computeTangentOperator_){\n";
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
        os << "}\n"
           << "}\n";
      }
    }
    if (this->mb.useQt()) {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType, use_qt>::SUCCESS;\n";
    } else {
      os << "return MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType, false>::SUCCESS;\n";
    }
    os << "}\n\n";
  }

  void BehaviourDSLCommon::writeBehaviourDisabledConstructors(
      std::ostream& os) const {
    this->checkBehaviourFile(os);
    os << "//! \\brief Default constructor (disabled)\n"
       << this->mb.getClassName() << "() =delete ;\n"
       << "//! \\brief Copy constructor (disabled)\n"
       << this->mb.getClassName() << "(const " << this->mb.getClassName()
       << "&) = delete;\n"
       << "//! \\brief Assignement operator (disabled)\n"
       << this->mb.getClassName() << "& operator = (const "
       << this->mb.getClassName() << "&) = delete;\n\n";
  }

  void BehaviourDSLCommon::writeBehaviourSetOutOfBoundsPolicy(
      std::ostream& os) const {
    this->checkBehaviourFile(os);
    os << "/*!\n"
       << " * \\brief set the policy for \"out of bounds\" conditions\n"
       << " */\n";
    if (allowRuntimeModificationOfTheOutOfBoundsPolicy(this->mb)) {
      os << "void\n"
         << "setOutOfBoundsPolicy(const OutOfBoundsPolicy policy_value){\n"
         << "  this->policy = policy_value;\n"
         << "} // end of setOutOfBoundsPolicy\n\n";
    } else {
      os << "void\n"
         << "setOutOfBoundsPolicy(const OutOfBoundsPolicy) const {\n"
         << "} // end of setOutOfBoundsPolicy\n\n";
    }
  }  // end of writeBehaviourSetOutOfBoundsPolicy

  static void writeBoundsChecks(std::ostream& os,
                                const VariableDescription& v,
                                const std::string& n,
                                const bool b) {
    if (!v.hasBounds()) {
      return;
    }
    const auto& bounds = v.getBounds();
    if (bounds.boundsType == VariableBoundsDescription::LOWER) {
      os << "BoundsCheck<N>::lowerBoundCheck(\"" << n << "\",this->" << n << ","
         << "static_cast<real>(" << bounds.lowerBound << "),this->policy);\n";
      if (b) {
        os << "BoundsCheck<N>::lowerBoundCheck(\"" << n << "+d" << n
           << "\",this->" << n << "+this->d" << n << ","
           << "static_cast<real>(" << bounds.lowerBound << "),this->policy);\n";
      }
    } else if (bounds.boundsType == VariableBoundsDescription::UPPER) {
      os << "BoundsCheck<N>::upperBoundCheck(\"" << n << "\",this->" << n << ","
         << "static_cast<real>(" << bounds.upperBound << "),this->policy);\n";
      if (b) {
        os << "BoundsCheck<N>::upperBoundCheck(\"" << n << "+d" << n
           << "\",this->" << n << "+this->d" << n << ","
           << "static_cast<real>(" << bounds.upperBound << "),this->policy);\n";
      }
    } else if (bounds.boundsType == VariableBoundsDescription::LOWERANDUPPER) {
      os << "BoundsCheck<N>::lowerAndUpperBoundsChecks(\"" << n << "\",this->"
         << n << ","
         << "static_cast<real>(" << bounds.lowerBound << "),"
         << "static_cast<real>(" << bounds.upperBound << "),this->policy);\n";
      if (b) {
        os << "BoundsCheck<N>::lowerAndUpperBoundsChecks(\"" << n << "+d" << n
           << "\",this->" << n << "+this->d" << n << ","
           << "static_cast<real>(" << bounds.lowerBound << "),"
           << "static_cast<real>(" << bounds.upperBound << "),this->policy);\n";
      }
    } else {
      tfel::raise(
          "BehaviourDSLCommon::writeBoundsChecks: "
          "internal error (unsupported bounds type)");
    }
  }  // end of writeBoundsChecks

  void BehaviourDSLCommon::writeBoundsChecks(std::ostream& os,
                                             const VariableDescription& v,
                                             const bool b) const {
    if (v.arraySize == 1u) {
      mfront::writeBoundsChecks(os, v, v.name, b);
    } else {
      for (unsigned short i = 0; i != v.arraySize; ++i) {
        mfront::writeBoundsChecks(os, v, v.name + '[' + std::to_string(i) + ']',
                                  b);
      }
    }
  }  // end of writeBoundsChecks

  static void writePhysicalBoundsChecks(std::ostream& os,
                                        const VariableDescription& v,
                                        const std::string& n,
                                        const bool b) {
    if (!v.hasPhysicalBounds()) {
      return;
    }
    const auto& bounds = v.getPhysicalBounds();
    if (bounds.boundsType == VariableBoundsDescription::LOWER) {
      os << "BoundsCheck<N>::lowerBoundCheck(\"" << n << "\",this->" << n << ","
         << "static_cast<real>(" << bounds.lowerBound << "));\n";
      if (b) {
        os << "BoundsCheck<N>::lowerBoundCheck(\"" << n << "+d" << n
           << "\",this->" << n << "+this->d" << n << ","
           << "static_cast<real>(" << bounds.lowerBound << "));\n";
      }
    } else if (bounds.boundsType == VariableBoundsDescription::UPPER) {
      os << "BoundsCheck<N>::upperBoundCheck(\"" << n << "\",this->" << n << ","
         << "static_cast<real>(" << bounds.upperBound << "));\n";
      if (b) {
        os << "BoundsCheck<N>::upperBoundCheck(\"" << n << "+d" << n
           << "\",this->" << n << "+this->d" << n << ","
           << "static_cast<real>(" << bounds.upperBound << "));\n";
      }
    } else if (bounds.boundsType == VariableBoundsDescription::LOWERANDUPPER) {
      os << "BoundsCheck<N>::lowerAndUpperBoundsChecks(\"" << n << "\",this->"
         << n << ","
         << "static_cast<real>(" << bounds.lowerBound << "),"
         << "static_cast<real>(" << bounds.upperBound << "));\n";
      if (b) {
        os << "BoundsCheck<N>::lowerAndUpperBoundsChecks(\"" << n << "+d" << n
           << "\",this->" << n << "+this->d" << n << ","
           << "static_cast<real>(" << bounds.lowerBound << "),"
           << "static_cast<real>(" << bounds.upperBound << "));\n";
      }
    } else {
      tfel::raise(
          "BehaviourDSLCommon::writePhysicalBoundsChecks: "
          "internal error (unsupported bounds type)");
    }
  }  // end of writePhysicalBoundsChecks

  void BehaviourDSLCommon::writePhysicalBoundsChecks(
      std::ostream& os, const VariableDescription& v, const bool b) const {
    if (v.arraySize == 1u) {
      mfront::writePhysicalBoundsChecks(os, v, v.name, b);
    } else {
      for (unsigned short i = 0; i != v.arraySize; ++i) {
        mfront::writePhysicalBoundsChecks(
            os, v, v.name + '[' + std::to_string(i) + ']', b);
      }
    }
  }  // end of writePhysicalBoundsChecks

  void BehaviourDSLCommon::writeBehaviourCheckBounds(std::ostream& os,
                                                     const Hypothesis h) const {
    auto write_physical_bounds =
        [this, &os](const VariableDescriptionContainer& c, const bool b) {
          for (const auto& v : c) {
            this->writePhysicalBoundsChecks(os, v, b);
          }
        };
    auto write_bounds = [this, &os](const VariableDescriptionContainer& c,
                                    const bool b) {
      for (const auto& v : c) {
        this->writeBoundsChecks(os, v, b);
      }
    };
    const auto& md = this->mb.getBehaviourData(h);
    this->checkBehaviourFile(os);
    os << "//! \\brief check physical bounds and standard bounds\n"
       << "void checkBounds() const{\n";
    write_physical_bounds(md.getMaterialProperties(), false);
    write_physical_bounds(md.getPersistentVariables(), false);
    write_physical_bounds(md.getExternalStateVariables(), true);
    write_physical_bounds(md.getLocalVariables(), false);
    const auto check_bounds =
        !((!allowRuntimeModificationOfTheOutOfBoundsPolicy(this->mb)) &&
          (getDefaultOutOfBoundsPolicy(this->mb) == tfel::material::None));
    if (check_bounds) {
      write_bounds(md.getMaterialProperties(), false);
      write_bounds(md.getPersistentVariables(), false);
      write_bounds(md.getExternalStateVariables(), true);
      write_bounds(md.getLocalVariables(), false);
    }
    os << "} // end of checkBounds\n\n";
  }  // end of writeBehaviourCheckBounds

  std::string BehaviourDSLCommon::getBehaviourConstructorsInitializers(
      const Hypothesis h) const {
    // variable initialisation
    auto init = std::string();
    auto append = [&init](const std::string& s) {
      if (s.empty()) {
        return;
      }
      if (!init.empty()) {
        init += ",\n";
      }
      init += s;
    };
    append(this->getIntegrationVariablesIncrementsInitializers(h));
    append(this->getLocalVariablesInitializers(h));
    // tangent operator blocks
    const auto& blocks = this->mb.getTangentOperatorBlocks();
    if (this->mb.hasTrivialTangentOperatorStructure()) {
      tfel::raise_if(
          ((blocks.size() != 1u) || (blocks.front().first.arraySize != 1u) ||
           (blocks.front().second.arraySize != 1u)),
          "BehaviourDSLCommon::getBehaviourConstructorsInitializers: internal "
          "error");
      if (this->mb.getBehaviourType() !=
          BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) {
        append(this->mb.getTangentOperatorBlockName(blocks.front()) + "(Dt)");
      }
    } else {
      auto o = SupportedTypes::TypeSize{};
      // write blocks
      for (const auto& b : blocks) {
        const auto& v1 = b.first;
        const auto& v2 = b.second;
        if ((v1.arraySize != 1u) || (v2.arraySize != 1u)) {
          break;
        }
        const auto bn = this->mb.getTangentOperatorBlockName(b);
        if ((v1.getTypeFlag() == SupportedTypes::SCALAR) &&
            (v2.getTypeFlag() == SupportedTypes::SCALAR)) {
          append(bn + "(Dt[" + o.asString() + "])");
        } else {
          if (o.isNull()) {
            append(bn + "(Dt.begin())");
          } else {
            append(bn + "(Dt.begin()+" + o.asString() + ")");
          }
        }
        o += SupportedTypes::TypeSize::getDerivativeSize(v1.getTypeSize(),
                                                         v2.getTypeSize());
      }
    }
    return init;
  }  // end of getBehaviourConstructorsInitializers

  std::string BehaviourDSLCommon::getLocalVariablesInitializers(
      const Hypothesis) const {
    return {};
  }  // end of getLocalVariablesInitializers

  void BehaviourDSLCommon::writeBehaviourConstructors(
      std::ostream& os, const Hypothesis h) const {
    auto tmpnames = std::vector<std::string>{};
    auto write_body = [this, &os, &tmpnames, h] {
      os << "using namespace std;\n"
         << "using namespace tfel::math;\n"
         << "using std::vector;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
      this->writeBehaviourParameterInitialisation(os, h);
      // calling models
      for (const auto& m : this->mb.getModelsDescriptions()) {
        if (m.outputs.size() == 1) {
          const auto vn = m.outputs[0].name;
          this->writeModelCall(os, tmpnames, h, m, "d" + vn, vn, "em");
          os << "this->d" << vn << " -= this->" << vn << ";\n";
        } else {
          this->throwRuntimeError(
              "BehaviourDSLCommon::writeBehaviourInitializeMethod",
              "only models with one output are supported yet");
        }
      }
      this->writeBehaviourLocalVariablesInitialisation(os, h);
    };
    this->checkBehaviourFile(os);
    // initializers
    const auto& init = this->getBehaviourConstructorsInitializers(h);
    // writing constructors
    os << "/*!\n"
       << "* \\brief Constructor\n"
       << "*/\n";
    if (this->mb.useQt()) {
      os << this->mb.getClassName() << "("
         << "const " << this->mb.getClassName()
         << "BehaviourData<hypothesis, NumericType, use_qt>& src1,\n"
         << "const " << this->mb.getClassName()
         << "IntegrationData<hypothesis, NumericType, use_qt>& src2)\n"
         << ": " << this->mb.getClassName()
         << "BehaviourData<hypothesis, NumericType, use_qt>(src1),\n"
         << this->mb.getClassName()
         << "IntegrationData<hypothesis, NumericType, use_qt>(src2)";
    } else {
      os << this->mb.getClassName() << "("
         << "const " << this->mb.getClassName()
         << "BehaviourData<hypothesis, NumericType, false>& src1,\n"
         << "const " << this->mb.getClassName()
         << "IntegrationData<hypothesis, NumericType, false>& src2)\n"
         << ": " << this->mb.getClassName()
         << "BehaviourData<hypothesis, NumericType, false>(src1),\n"
         << this->mb.getClassName()
         << "IntegrationData<hypothesis, NumericType, false>(src2)";
    }
    if (!init.empty()) {
      os << ",\n" << init;
    }
    os << "\n{\n";
    write_body();
    os << "}\n\n";
    // constructor specific to interfaces
    for (const auto& i : this->interfaces) {
      if (i.second->isBehaviourConstructorRequired(h, this->mb)) {
        i.second->writeBehaviourConstructorHeader(os, this->mb, h, init);
        os << "\n{\n";
        write_body();
        i.second->writeBehaviourConstructorBody(os, this->mb, h);
        os << "}\n\n";
      }
    }
  }

  void BehaviourDSLCommon::writeHillTensorComputation(
      std::ostream& out,
      const std::string& H,
      const BehaviourDescription::HillTensor& h,
      std::function<std::string(const MaterialPropertyInput&)>& f) const {
    auto throw_if = [this](const bool b, const std::string& m) {
      if (b) {
        this->throwRuntimeError(
            "BehaviourDSLCommon::writeHillTensorComputation", m);
      }
    };
    throw_if(this->mb.getSymmetryType() == mfront::ISOTROPIC,
             "material is not orthotropic");
    for (decltype(h.c.size()) i = 0; i != h.c.size(); ++i) {
      this->writeMaterialPropertyCheckBoundsEvaluation(out, h.c[i], f);
    }
    if (this->mb.getOrthotropicAxesConvention() ==
        OrthotropicAxesConvention::PIPE) {
      out << H << " = tfel::material::computeHillTensor<hypothesis,"
          << "OrthotropicAxesConvention::PIPE, real>(";
    } else if (this->mb.getOrthotropicAxesConvention() ==
               OrthotropicAxesConvention::PLATE) {
      out << H << " = tfel::material::computeHillTensor<hypothesis,"
          << "OrthotropicAxesConvention::PLATE, real>(";
    } else {
      out << H << " = tfel::material::computeHillTensor<hypothesis,"
          << "OrthotropicAxesConvention::DEFAULT, real>(";
    }
    for (decltype(h.c.size()) i = 0; i != h.c.size();) {
      this->writeMaterialPropertyEvaluation(out, h.c[i], f);
      if (++i != h.c.size()) {
        out << ",\n";
      }
    }
    out << ");\n";
  }  // end of writeHillTensorComputation

  void BehaviourDSLCommon::writeStiffnessTensorComputation(
      std::ostream& out,
      const std::string& D,
      std::function<std::string(const MaterialPropertyInput&)>& f) const {
    const auto& emps = this->mb.getElasticMaterialProperties();
    if ((this->mb.getSymmetryType() == mfront::ISOTROPIC) &&
        (this->mb.getElasticSymmetryType() != mfront::ISOTROPIC)) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::writeStiffnessTensorComputation",
          "inconsistent symmetry type for the material and "
          "the elastic behaviour.");
    }
    bool ua = true;
    if (!this->mb.hasAttribute(
            BehaviourDescription::requiresUnAlteredStiffnessTensor)) {
      const auto& mh = this->mb.getModellingHypotheses();
      if ((mh.find(ModellingHypothesis::PLANESTRESS) != mh.end()) ||
          (mh.find(ModellingHypothesis::AXISYMMETRICALGENERALISEDPLANESTRESS) !=
           mh.end())) {
        this->throwRuntimeError(
            "BehaviourDSLCommon::writeStiffnessTensorComputation",
            "For plane stress hypotheses, it is required to precise whether "
            "the expected stiffness tensor is 'Altered' (the plane stress "
            "hypothesis is taken into account) or 'UnAltered' (the stiffness "
            "tensor is the same as in plane strain). "
            "See the '@ComputeStiffnessTensor' documentation");
      }
    } else {
      ua = this->mb.getAttribute<bool>(
          BehaviourDescription::requiresUnAlteredStiffnessTensor);
    }
    if (this->mb.getElasticSymmetryType() == mfront::ISOTROPIC) {
      if (emps.size() != 2u) {
        this->throwRuntimeError(
            "BehaviourDSLCommon::writeStiffnessTensorComputation",
            "invalid number of material properties");
      }
      this->writeMaterialPropertyCheckBoundsEvaluation(out, emps[0], f);
      this->writeMaterialPropertyCheckBoundsEvaluation(out, emps[1], f);
      if (ua) {
        out << "tfel::material::computeIsotropicStiffnessTensor<hypothesis,"
               "StiffnessTensorAlterationCharacteristic::"
               "UNALTERED"
            << ">(" << D << ",";
      } else {
        out << "tfel::material::computeIsotropicStiffnessTensor<hypothesis,"
               "StiffnessTensorAlterationCharacteristic::"
               "ALTERED"
            << ">(" << D << ", ";
      }
      out << "stress(";
      this->writeMaterialPropertyEvaluation(out, emps[0], f);
      out << "), \n";
      this->writeMaterialPropertyEvaluation(out, emps[1], f);
      out << ");\n";
    } else if (this->mb.getElasticSymmetryType() == mfront::ORTHOTROPIC) {
      if (emps.size() != 9u) {
        this->throwRuntimeError(
            "BehaviourDSLCommon::writeStiffnessTensorComputation",
            "invalid number of material properties");
      }
      for (decltype(emps.size()) i = 0; i != emps.size(); ++i) {
        this->writeMaterialPropertyCheckBoundsEvaluation(out, emps[i], f);
      }
      if (ua) {
        if (this->mb.getOrthotropicAxesConvention() ==
            OrthotropicAxesConvention::PIPE) {
          out << "tfel::material::computeOrthotropicStiffnessTensor<hypothesis,"
              << "StiffnessTensorAlterationCharacteristic::UNALTERED,"
              << "OrthotropicAxesConvention::PIPE>(" << D << ",";
        } else if (this->mb.getOrthotropicAxesConvention() ==
                   OrthotropicAxesConvention::PLATE) {
          out << "tfel::material::computeOrthotropicStiffnessTensor<hypothesis,"
              << "StiffnessTensorAlterationCharacteristic::UNALTERED,"
              << "OrthotropicAxesConvention::PLATE>(" << D << ",";
        } else {
          out << "tfel::material::computeOrthotropicStiffnessTensor<hypothesis,"
              << "StiffnessTensorAlterationCharacteristic::UNALTERED,"
              << "OrthotropicAxesConvention::DEFAULT>(" << D << ",";
        }
      } else {
        if (this->mb.getOrthotropicAxesConvention() ==
            OrthotropicAxesConvention::PIPE) {
          out << "tfel::material::computeOrthotropicStiffnessTensor<hypothesis,"
              << "StiffnessTensorAlterationCharacteristic::ALTERED,"
              << "OrthotropicAxesConvention::PIPE>(" << D << ",";
        } else if (this->mb.getOrthotropicAxesConvention() ==
                   OrthotropicAxesConvention::PLATE) {
          out << "tfel::material::computeOrthotropicStiffnessTensor<hypothesis,"
              << "StiffnessTensorAlterationCharacteristic::ALTERED,"
              << "OrthotropicAxesConvention::PLATE>(" << D << ",";
        } else {
          out << "tfel::material::computeOrthotropicStiffnessTensor<hypothesis,"
              << "StiffnessTensorAlterationCharacteristic::ALTERED,"
              << "OrthotropicAxesConvention::DEFAULT>(" << D << ",";
        }
      }
      for (decltype(emps.size()) i = 0; i != emps.size();) {
        if ((i == 0) || (i == 1) || (i == 2) ||  //
            (i == 6) || (i == 7) || (i == 8)) {
          out << "stress(";
          this->writeMaterialPropertyEvaluation(out, emps[i], f);
          out << ")";
        } else {
          this->writeMaterialPropertyEvaluation(out, emps[i], f);
        }
        if (++i != emps.size()) {
          out << ",\n";
        }
      }
      out << ");\n";
    } else {
      this->throwRuntimeError(
          "BehaviourDSLCommon::writeStiffnessTensorComputation",
          "unsupported elastic symmetry type");
    }
  }  // end of writeStiffnessTensorComputation

  void BehaviourDSLCommon::writeExternalMFrontMaterialPropertyArguments(
      std::ostream& out,
      const BehaviourDescription::MaterialProperty& m,
      std::function<std::string(const MaterialPropertyInput&)>& f) const {
    const auto& cmp =
        m.get<BehaviourDescription::ExternalMFrontMaterialProperty>();
    const auto& mpd = *(cmp.mpd);
    out << '(';
    const auto use_qt = this->mb.useQt();
    if (!mpd.inputs.empty()) {
      const auto& inputs = this->mb.getMaterialPropertyInputs(mpd);
      auto pi = std::begin(inputs);
      const auto pie = std::end(inputs);
      while (pi != pie) {
        if (use_qt) {
          out << "tfel::math::base_type_cast(" << f(*pi) << ")";
        } else {
          out << f(*pi);
        }
        if (++pi != pie) {
          out << ",";
        }
      }
    }
    out << ")";
  }

  void BehaviourDSLCommon::writeMaterialPropertyCheckBoundsEvaluation(
      std::ostream& out,
      const BehaviourDescription::MaterialProperty& m,
      std::function<std::string(const MaterialPropertyInput&)>& f) const {
    if (m.is<BehaviourDescription::ExternalMFrontMaterialProperty>()) {
      const auto& cmp =
          m.get<BehaviourDescription::ExternalMFrontMaterialProperty>();
      const auto& mpd = *(cmp.mpd);
      if ((!hasBounds(mpd.inputs)) && (!(hasPhysicalBounds(mpd.inputs)))) {
        return;
      }
      const auto& n = MFrontMaterialPropertyInterface().getFunctionName(mpd);
      out << "{\n // check bounds for material property '" << n << "'\n"
          << "const auto " << n << "_bounds_check_status = " << n
          << "_checkBounds";
      this->writeExternalMFrontMaterialPropertyArguments(out, cmp, f);
      out << ";\n"
          << "if(" << n << "_bounds_check_status!=0){\n"
          << "// physical bounds\n"
          << "tfel::raise_if<OutOfBoundsException>(" << n
          << "_bounds_check_status<0,\n"
          << "\"" << this->mb.getClassName()
          << ": a variable is out of its physical bounds \"\n"
          << "\"when calling the material property '" << n << "'\");\n"
          << "} else {\n"
          << "// standard bounds\n"
          << "if(this->policy==Strict){\n"
          << "tfel::raise<OutOfBoundsException>(\"" << this->mb.getClassName()
          << ": "
          << "a variable is out of its bounds \"\n"
          << "\"when calling the material property '" << n << "'\");\n"
          << "} else if(this->policy==Warning){\n"
          << "std::cerr << \"" << this->mb.getClassName() << ": "
          << "a variable is out of its bounds \"\n"
          << "\"when calling the material property '" << n << "'\\n\";\n"
          << "}\n"
          << "}\n"
          << "}\n";
    } else if (!((m.is<BehaviourDescription::ConstantMaterialProperty>()) ||
                 (m.is<BehaviourDescription::AnalyticMaterialProperty>()))) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::writeMaterialPropertyCheckBoundsEvaluation",
          "unsupported material property type");
    }
  }  // end of writeMaterialPropertyEvaluation

  void BehaviourDSLCommon::writeMaterialPropertyEvaluation(
      std::ostream& out,
      const BehaviourDescription::MaterialProperty& m,
      std::function<std::string(const MaterialPropertyInput&)>& f) const {
    if (m.is<BehaviourDescription::ConstantMaterialProperty>()) {
      const auto& cmp = m.get<BehaviourDescription::ConstantMaterialProperty>();
      if (!cmp.name.empty()) {
        out << "this->" << cmp.name;
      } else {
        out << cmp.value;
      }
    } else if (m.is<BehaviourDescription::ExternalMFrontMaterialProperty>()) {
      const auto& cmp =
          m.get<BehaviourDescription::ExternalMFrontMaterialProperty>();
      const auto& mpd = *(cmp.mpd);
      out << MFrontMaterialPropertyInterface().getFunctionName(mpd);
      this->writeExternalMFrontMaterialPropertyArguments(out, cmp, f);
    } else if (m.is<BehaviourDescription::AnalyticMaterialProperty>()) {
      const auto& amp = m.get<BehaviourDescription::AnalyticMaterialProperty>();
      tfel::math::Evaluator e(amp.f);
      auto mi = std::map<std::string, std::string>{};
      for (const auto& i :
           this->mb.getMaterialPropertyInputs(e.getVariablesNames())) {
        mi[i.name] = f(i);
      }
      out << e.getCxxFormula(mi);
    } else if (m.empty()) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::writeMaterialPropertyEvaluation",
          "empty material property");
    } else {
      this->throwRuntimeError(
          "BehaviourDSLCommon::writeMaterialPropertyEvaluation",
          "unsupported material property type");
    }
  }  // end of writeMaterialPropertyEvaluation

  void BehaviourDSLCommon::writeThermalExpansionCoefficientComputation(
      std::ostream& out,
      const BehaviourDescription::MaterialProperty& a,
      const std::string& T,
      const std::string& idx,
      const std::string& s) const {
    auto throw_if = [this](const bool b, const std::string& m) {
      if (b) {
        this->throwRuntimeError(
            "BehaviourDSLCommon::"
            "writeThermalExpansionCoefficientComputation",
            m);
      }
    };
    out << "const thermalexpansion alpha" << s;
    if (!idx.empty()) {
      out << "_" << idx;
    }
    out << " = ";
    if (a.is<BehaviourDescription::ConstantMaterialProperty>()) {
      const auto& cmp = a.get<BehaviourDescription::ConstantMaterialProperty>();
      if (cmp.name.empty()) {
        out << cmp.value << ";\n";
      } else {
        out << "this->" << cmp.name << ";\n";
      }
    } else if (a.is<BehaviourDescription::ExternalMFrontMaterialProperty>()) {
      const auto& mpd =
          *(a.get<BehaviourDescription::ExternalMFrontMaterialProperty>().mpd);
      const auto inputs = this->mb.getMaterialPropertyInputs(mpd);
      if (this->mb.useQt()) {
        out << "thermalexpansion(";
      }
      out << MFrontMaterialPropertyInterface().getFunctionName(mpd) << '(';
      for (auto pi = inputs.begin(); pi != inputs.end();) {
        const auto c = pi->category;
        if (c == BehaviourDescription::MaterialPropertyInput::TEMPERATURE) {
          out << "tfel::math::base_type_cast(" << T << ")";
        } else if ((c == BehaviourDescription::MaterialPropertyInput::
                             MATERIALPROPERTY) ||
                   (c ==
                    BehaviourDescription::MaterialPropertyInput::PARAMETER)) {
          out << "tfel::math::base_type_cast(this->" << pi->name << ")";
        } else if (c == BehaviourDescription::MaterialPropertyInput::
                            STATICVARIABLE) {
          out << "tfel::math::base_type_cast(" << this->mb.getClassName()
              << "::" << pi->name << ")";
        } else {
          throw_if(true,
                   "thermal expansion coefficients must depend "
                   "on the temperature only");
        }
        if (++pi != inputs.end()) {
          out << ",";
        }
      }
      if (this->mb.useQt()) {
        out << ")";
      }
      out << ");\n";
    } else if (a.is<BehaviourDescription::AnalyticMaterialProperty>()) {
      const auto& amp = a.get<BehaviourDescription::AnalyticMaterialProperty>();
      auto m = std::map<std::string, std::string>{};
      for (const auto& i :
           this->mb.getMaterialPropertyInputs(amp.getVariablesNames())) {
        const auto c = i.category;
        if (c == BehaviourDescription::MaterialPropertyInput::TEMPERATURE) {
          m.insert({"T", "tfel::math::base_type_cast(" + T + ")"});
        } else if ((c == BehaviourDescription::MaterialPropertyInput::
                             MATERIALPROPERTY) ||
                   (c ==
                    BehaviourDescription::MaterialPropertyInput::PARAMETER)) {
          m.insert(
              {i.name, "tfel::math::base_type_cast(this->" + i.name + ")"});
        } else if (c == BehaviourDescription::MaterialPropertyInput::
                            STATICVARIABLE) {
          m.insert({i.name, "tfel::math::base_type_cast(" +
                                this->mb.getClassName() + "::" + i.name + ")"});
        } else {
          throw_if(true,
                   "thermal expansion coefficients must depend "
                   "on the temperature only");
        }
      }
      tfel::math::Evaluator ev(amp.f);
      if (this->mb.useQt()) {
        out << "thermalexpansion(";
      }
      out << ev.getCxxFormula(m);
      if (this->mb.useQt()) {
        out << ")";
      }
      out << ";\n";
    } else {
      throw_if(true, "unsupported material property type");
    }
  }  // end of writeThermalExpansionCoefficientComputation

  void BehaviourDSLCommon::writeThermalExpansionCoefficientsComputations(
      std::ostream& out,
      const BehaviourDescription::MaterialProperty& a,
      const std::string& suffix) const {
    this->writeThermalExpansionCoefficientComputation(
        out, a, "this->initial_geometry_reference_temperature", "",
        suffix + "_Ti");
    this->writeThermalExpansionCoefficientComputation(out, a, "this->T", "t",
                                                      suffix + "_T");
    this->writeThermalExpansionCoefficientComputation(
        out, a, "this->T+this->dT", "t_dt", suffix + "_T");
  }  // end of writeThermalExpansionCoefficientComputation

  void BehaviourDSLCommon::writeThermalExpansionComputation(
      std::ostream& out,
      const std::string& t,
      const std::string& c,
      const std::string& suffix) const {
    const auto Tref = "this->thermal_expansion_reference_temperature";
    const auto T = (t == "t") ? "this->T" : "this->T+this->dT";
    if (t == "t") {
      out << "dl0_l0";
    } else {
      out << "dl1_l0";
    }
    out << "[" << c << "] += 1/(1+alpha" << suffix
        << "_Ti * (this->initial_geometry_reference_temperature-" << Tref
        << "))*("
        << "alpha" << suffix << "_T_" << t << " * (" << T << "-" << Tref << ")-"
        << "alpha" << suffix
        << "_Ti * (this->initial_geometry_reference_temperature-" << Tref
        << "));\n";
  }  // end of writeThermalExpansionComputation

  void BehaviourDSLCommon::writeBehaviourComputeStressFreeExpansion(
      std::ostream& os, const Hypothesis h) const {
    auto tmpnames = std::vector<std::string>{};
    auto throw_if = [this](const bool b, const std::string& m) {
      if (b) {
        this->throwRuntimeError(
            "BehaviourDSLCommon::writeBehaviourComputeStressFreeExpansion", m);
      }
    };
    auto eval = [](std::ostream& out,
                   const BehaviourDescription::MaterialProperty& mp,
                   const std::string& c, const bool b) {
      const auto& cmp =
          mp.get<BehaviourDescription::ConstantMaterialProperty>();
      const auto Tref = "this->thermal_expansion_reference_temperature";
      const auto i = b ? "1" : "0";
      const auto T = b ? "this->T+this->dT" : "this->T";
      if (cmp.name.empty()) {
        out << "dl" << i << "_l0"
            << "[" << c << "] += " << cmp.value << "/(1+" << cmp.value
            << "*(this->initial_geometry_reference_temperature-" << Tref << "))"
            << "*(" << T << "-this->initial_geometry_reference_temperature);\n";
      } else {
        out << "dl" << i << "_l0"
            << "[" << c << "] += (this->" << cmp.name << ")/(1+(this->"
            << cmp.name << ")*(this->initial_geometry_reference_temperature-"
            << Tref << "))"
            << "*(" << T << "-this->initial_geometry_reference_temperature);\n";
      }
    };
    if (!this->mb.requiresStressFreeExpansionTreatment(h)) {
      return;
    }
    if (this->mb.areThermalExpansionCoefficientsDefined()) {
      throw_if((this->mb.getBehaviourType() !=
                BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) &&
                   (this->mb.getBehaviourType() !=
                    BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR),
               "only finite strain or small strain behaviour are supported");
      if (this->mb.getSymmetryType() == mfront::ORTHOTROPIC) {
        if ((this->mb.getOrthotropicAxesConvention() ==
             OrthotropicAxesConvention::DEFAULT) &&
            (this->mb.getThermalExpansionCoefficients().size() == 3u)) {
          // in this case, only tridimensional case is supported
          for (const auto mh : this->mb.getDistinctModellingHypotheses()) {
            throw_if(mh != ModellingHypothesis::TRIDIMENSIONAL,
                     "an orthotropic axes convention must be choosen when "
                     "using @ComputeThermalExpansion keyword in behaviours "
                     "which shall be valid in other modelling hypothesis "
                     "than 'Tridimensional'.\n"
                     "Either restrict the validity of the behaviour to "
                     "'Tridimensional' (see @ModellingHypothesis) or "
                     "choose and orthotropic axes convention as on option "
                     "to the @OrthotropicBehaviour keyword");
          }
        }
      }
    }
    this->checkBehaviourFile(os);
    os << "void\n"
       << "computeStressFreeExpansion(std::pair<StressFreeExpansionType,"
          "StressFreeExpansionType>& dl01_l0)\n{\n";
    os << "using namespace std;\n";
    os << "using namespace tfel::math;\n";
    os << "using std::vector;\n";
    writeMaterialLaws(os, this->mb.getMaterialLaws());
    os << "auto& dl0_l0 = dl01_l0.first;\n";
    os << "auto& dl1_l0 = dl01_l0.second;\n";
    os << "dl0_l0 = StressFreeExpansionType(typename "
          "StressFreeExpansionType::value_type(0));\n";
    os << "dl1_l0 = StressFreeExpansionType(typename "
          "StressFreeExpansionType::value_type(0));\n";
    if (this->mb.hasCode(h, BehaviourData::ComputeStressFreeExpansion)) {
      os << this->mb.getCode(h, BehaviourData::ComputeStressFreeExpansion)
         << '\n';
    }
    if (this->mb.areThermalExpansionCoefficientsDefined()) {
      const auto& acs = this->mb.getThermalExpansionCoefficients();
      if (acs.size() == 1u) {
        const auto& a = acs.front();
        if (a.is<BehaviourDescription::ConstantMaterialProperty>()) {
          eval(os, a, "0", false);
        } else {
          this->writeThermalExpansionCoefficientsComputations(os, acs.front());
          this->writeThermalExpansionComputation(os, "t", "0");
        }
        os << "dl0_l0[1] += dl0_l0[0];\n"
           << "dl0_l0[2] += dl0_l0[0];\n";
        if (a.is<BehaviourDescription::ConstantMaterialProperty>()) {
          eval(os, a, "0", true);
        } else {
          this->writeThermalExpansionComputation(os, "t_dt", "0");
        }
        os << "dl1_l0[1] += dl1_l0[0];\n"
           << "dl1_l0[2] += dl1_l0[0];\n";
      } else if (acs.size() == 3u) {
        throw_if(this->mb.getSymmetryType() != mfront::ORTHOTROPIC,
                 "invalid number of thermal expansion coefficients");
        for (size_t i = 0; i != 3; ++i) {
          if (!acs[i].is<BehaviourDescription::ConstantMaterialProperty>()) {
            this->writeThermalExpansionCoefficientsComputations(
                os, acs[i], std::to_string(i));
          }
        }
        for (size_t i = 0; i != 3; ++i) {
          const auto idx = std::to_string(i);
          if (acs[i].is<BehaviourDescription::ConstantMaterialProperty>()) {
            eval(os, acs[i], idx, false);
            eval(os, acs[i], idx, true);
          } else {
            this->writeThermalExpansionComputation(os, "t", idx, idx);
            this->writeThermalExpansionComputation(os, "t_dt", idx, idx);
          }
        }
      } else {
        throw_if(true, "unsupported behaviour symmetry");
      }
    }
    for (const auto& d : this->mb.getStressFreeExpansionDescriptions(h)) {
      if (d.is<BehaviourData::AxialGrowth>()) {
        throw_if((this->mb.getBehaviourType() !=
                  BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) &&
                     (this->mb.getBehaviourType() !=
                      BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR),
                 "only finite strain or small strain behaviour are supported");
        throw_if(this->mb.getSymmetryType() != mfront::ORTHOTROPIC,
                 "axial growth is only supported for orthotropic behaviours");
        const auto& s = d.get<BehaviourData::AxialGrowth>();
        throw_if(s.sfe.is<BehaviourData::NullExpansion>(),
                 "null swelling is not supported here");
        // The z-axis is supposed to be aligned with the second
        // direction of orthotropy.
        if (s.sfe.is<BehaviourData::SFED_ESV>()) {
          const auto ev = s.sfe.get<BehaviourData::SFED_ESV>().vname;
          os << "dl0_l0[1]+=this->" << ev << ";\n"
             << "dl0_l0[0]+=real(1)/std::sqrt(1+this->" << ev << ")-real(1);\n"
             << "dl0_l0[2]+=real(1)/std::sqrt(1+this->" << ev << ")-real(1);\n"
             << "dl1_l0[1]+=this->" << ev << "+this->d" << ev << ";\n"
             << "dl1_l0[0]+=real(1)/std::sqrt(1+this->" << ev << "+this->d"
             << ev << ")-real(1);\n"
             << "dl1_l0[2]+=real(1)/std::sqrt(1+this->" << ev << "+this->d"
             << ev << ")-real(1);\n";
        } else if (s.sfe.is<std::shared_ptr<ModelDescription>>()) {
          const auto& md = *(s.sfe.get<std::shared_ptr<ModelDescription>>());
          throw_if(
              md.outputs.size() != 1u,
              "invalid number of outputs for model '" + md.className + "'");
          const auto vs = md.className + "_" + md.outputs[0].name;
          os << "dl0_l0[1]+=this->" << vs << ";\n"
             << "dl0_l0[0]+=real(1)/std::sqrt(1+this->" << vs << ")-real(1);\n"
             << "dl0_l0[2]+=real(1)/std::sqrt(1+this->" << vs << ")-real(1);\n";
          this->writeModelCall(os, tmpnames, h, md, vs, vs, "sfeh");
          os << "dl1_l0[1]+=this->" << vs << ";\n"
             << "dl1_l0[0]+=real(1)/std::sqrt(1+this->" << vs << ")-real(1);\n"
             << "dl1_l0[2]+=real(1)/std::sqrt(1+this->" << vs << ")-real(1);\n";
        } else {
          throw_if(true, "internal error, unsupported stress free expansion");
        }
      } else if (d.is<BehaviourData::Relocation>()) {
        throw_if((this->mb.getBehaviourType() !=
                  BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) &&
                     (this->mb.getBehaviourType() !=
                      BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR),
                 "only finite strain or small strain behaviour are supported");
        const auto& s = d.get<BehaviourData::Relocation>();
        throw_if(s.sfe.is<BehaviourData::NullExpansion>(),
                 "null swelling is not supported here");
        if (s.sfe.is<BehaviourData::SFED_ESV>()) {
          const auto ev = s.sfe.get<BehaviourData::SFED_ESV>().vname;
          if ((h ==
               ModellingHypothesis::AXISYMMETRICALGENERALISEDPLANESTRESS) ||
              (h ==
               ModellingHypothesis::AXISYMMETRICALGENERALISEDPLANESTRAIN)) {
            os << "dl0_l0[0]+=this->" << ev << "/2;\n"
               << "dl0_l0[2]+=this->" << ev << "/2;\n"
               << "dl1_l0[0]+=(this->" << ev << "+this->d" << ev << ")/2;\n"
               << "dl1_l0[2]+=(this->" << ev << "+this->d" << ev << ")/2;\n";
          }
          if ((h == ModellingHypothesis::GENERALISEDPLANESTRAIN) ||
              (h == ModellingHypothesis::PLANESTRAIN) ||
              (h == ModellingHypothesis::PLANESTRESS)) {
            os << "dl0_l0[0]+=this->" << ev << "/2;\n"
               << "dl0_l0[1]+=this->" << ev << "/2;\n"
               << "dl1_l0[0]+=(this->" << ev << "+this->d" << ev << ")/2;\n"
               << "dl1_l0[1]+=(this->" << ev << "+this->d" << ev << ")/2;\n";
          }
        } else if (s.sfe.is<std::shared_ptr<ModelDescription>>()) {
          const auto& md = *(s.sfe.get<std::shared_ptr<ModelDescription>>());
          throw_if(
              md.outputs.size() != 1u,
              "invalid number of outputs for model '" + md.className + "'");
          const auto vs = md.className + "_" + md.outputs[0].name;
          if ((h ==
               ModellingHypothesis::AXISYMMETRICALGENERALISEDPLANESTRESS) ||
              (h ==
               ModellingHypothesis::AXISYMMETRICALGENERALISEDPLANESTRAIN)) {
            os << "dl0_l0[0]+=(this->" << vs << ")/2;\n"
               << "dl0_l0[2]+=(this->" << vs << ")/2;\n";
          }
          this->writeModelCall(os, tmpnames, h, md, vs, vs, "sfeh");
          if ((h == ModellingHypothesis::GENERALISEDPLANESTRAIN) ||
              (h == ModellingHypothesis::PLANESTRAIN) ||
              (h == ModellingHypothesis::PLANESTRESS)) {
            os << "dl0_l0[0]+=(this->" << vs << ")/2;\n"
               << "dl0_l0[1]+=(this->" << vs << ")/2;\n";
          }
        } else {
          throw_if(true, "internal error, unsupported stress free expansion");
        }
      } else if (d.is<BehaviourData::OrthotropicStressFreeExpansion>()) {
        using StressFreeExpansionHandler =
            BehaviourData::StressFreeExpansionHandler;
        const auto& s = d.get<BehaviourData::OrthotropicStressFreeExpansion>();
        auto write = [this, &os, &tmpnames, throw_if, h](
                         const StressFreeExpansionHandler& sfe,
                         const char* const c) {
          if (sfe.is<BehaviourData::SFED_ESV>()) {
            const auto& ev = sfe.get<BehaviourData::SFED_ESV>().vname;
            os << "dl0_l0[" << c << "]+=this->" << ev << ";\n";
            os << "dl1_l0[" << c << "]+=this->" << ev << "+this->d" << ev
               << ";\n";
          } else if (sfe.is<std::shared_ptr<ModelDescription>>()) {
            const auto& md = *(sfe.get<std::shared_ptr<ModelDescription>>());
            throw_if(
                md.outputs.size() != 1u,
                "invalid number of outputs for model '" + md.className + "'");
            const auto vs = md.className + "_" + md.outputs[0].name;
            os << "dl0_l0[" << c << "]+=this->" << vs << ";\n";
            this->writeModelCall(os, tmpnames, h, md, vs, vs, "sfeh");
            os << "dl1_l0[" << c << "]+=this->" << vs << ";\n";
          } else if (!sfe.is<BehaviourData::NullExpansion>()) {
            throw_if(true, "internal error, unsupported stress free expansion");
          }
        };
        throw_if((this->mb.getBehaviourType() !=
                  BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) &&
                     (this->mb.getBehaviourType() !=
                      BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR),
                 "only finite strain or small strain behaviour are supported");
        throw_if(this->mb.getSymmetryType() != mfront::ORTHOTROPIC,
                 "orthotropic stress free expansion is only supported "
                 "for orthotropic behaviours");
        throw_if(s.sfe0.is<BehaviourData::NullExpansion>() &&
                     s.sfe1.is<BehaviourData::NullExpansion>() &&
                     s.sfe2.is<BehaviourData::NullExpansion>(),
                 "null swelling is not supported here");
        write(s.sfe0, "0");
        write(s.sfe1, "1");
        write(s.sfe2, "2");
      } else if (d.is<BehaviourData::OrthotropicStressFreeExpansionII>()) {
        const auto& s =
            d.get<BehaviourData::OrthotropicStressFreeExpansionII>();
        const auto& ev = s.esv.vname;
        throw_if((this->mb.getBehaviourType() !=
                  BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) &&
                     (this->mb.getBehaviourType() !=
                      BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR),
                 "only finite strain or small strain behaviour are supported");
        throw_if(this->mb.getSymmetryType() != mfront::ORTHOTROPIC,
                 "orthotropic stress free expansion is only supported "
                 "for orthotropic behaviours");
        os << "dl0_l0[0]+=this->" << ev << "[0];\n"
           << "dl0_l0[1]+=this->" << ev << "[1];\n"
           << "dl0_l0[2]+=this->" << ev << "[2];\n"
           << "dl1_l0[0]+=this->" << ev << "[0]+this->d" << ev << "[0];\n"
           << "dl1_l0[1]+=this->" << ev << "[1]+this->d" << ev << "[1];\n"
           << "dl1_l0[2]+=this->" << ev << "[2]+this->d" << ev << "[2];\n";
      } else if (d.is<BehaviourData::IsotropicStressFreeExpansion>()) {
        throw_if((this->mb.getBehaviourType() !=
                  BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) &&
                     (this->mb.getBehaviourType() !=
                      BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR),
                 "only finite strain or small strain behaviour are supported");
        const auto& s = d.get<BehaviourData::IsotropicStressFreeExpansion>();
        throw_if(s.sfe.is<BehaviourData::NullExpansion>(),
                 "null swelling is not supported here");
        if (s.sfe.is<BehaviourData::SFED_ESV>()) {
          const auto ev = s.sfe.get<BehaviourData::SFED_ESV>().vname;
          os << "dl0_l0[0]+=this->" << ev << ";\n"
             << "dl0_l0[1]+=this->" << ev << ";\n"
             << "dl0_l0[2]+=this->" << ev << ";\n"
             << "dl1_l0[0]+=this->" << ev << "+this->d" << ev << ";\n"
             << "dl1_l0[1]+=this->" << ev << "+this->d" << ev << ";\n"
             << "dl1_l0[2]+=this->" << ev << "+this->d" << ev << ";\n";
        } else if (s.sfe.is<std::shared_ptr<ModelDescription>>()) {
          const auto& md = *(s.sfe.get<std::shared_ptr<ModelDescription>>());
          throw_if(
              md.outputs.size() != 1u,
              "invalid number of outputs for model '" + md.className + "'");
          const auto vs = md.className + "_" + md.outputs[0].name;
          os << "dl0_l0[0]+=this->" << vs << ";\n"
             << "dl0_l0[1]+=this->" << vs << ";\n"
             << "dl0_l0[2]+=this->" << vs << ";\n";
          this->writeModelCall(os, tmpnames, h, md, vs, vs, "sfeh");
          os << "dl1_l0[0]+=this->" << vs << ";\n"
             << "dl1_l0[1]+=this->" << vs << ";\n"
             << "dl1_l0[2]+=this->" << vs << ";\n";
        } else {
          throw_if(true, "internal error, unsupported stress free expansion");
        }
      } else if (d.is<BehaviourData::VolumeSwellingStressFreeExpansion>()) {
        throw_if((this->mb.getBehaviourType() !=
                  BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) &&
                     (this->mb.getBehaviourType() !=
                      BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR),
                 "only finite strain or small strain behaviour are supported");
        const auto& s =
            d.get<BehaviourData::VolumeSwellingStressFreeExpansion>();
        throw_if(s.sfe.is<BehaviourData::NullExpansion>(),
                 "null swelling is not supported here");
        if (s.sfe.is<BehaviourData::SFED_ESV>()) {
          const auto ev = s.sfe.get<BehaviourData::SFED_ESV>().vname;
          os << "dl0_l0[0]+=this->" << ev << "/3;\n"
             << "dl0_l0[1]+=this->" << ev << "/3;\n"
             << "dl0_l0[2]+=this->" << ev << "/3;\n"
             << "dl1_l0[0]+=(this->" << ev << "+this->d" << ev << ")/3;\n"
             << "dl1_l0[1]+=(this->" << ev << "+this->d" << ev << ")/3;\n"
             << "dl1_l0[2]+=(this->" << ev << "+this->d" << ev << ")/3;\n";
        } else if (s.sfe.is<std::shared_ptr<ModelDescription>>()) {
          const auto& md = *(s.sfe.get<std::shared_ptr<ModelDescription>>());
          throw_if(
              md.outputs.size() != 1u,
              "invalid number of outputs for model '" + md.className + "'");
          const auto vs = md.className + "_" + md.outputs[0].name;
          os << "dl0_l0[0]+=this->" << vs << "/3;\n"
             << "dl0_l0[1]+=this->" << vs << "/3;\n"
             << "dl0_l0[2]+=this->" << vs << "/3;\n";
          this->writeModelCall(os, tmpnames, h, md, vs, vs, "sfeh");
          os << "dl1_l0[0]+=this->" << vs << "/3;\n"
             << "dl1_l0[1]+=this->" << vs << "/3;\n"
             << "dl1_l0[2]+=this->" << vs << "/3;\n";
        } else {
          throw_if(true, "internal error, unsupported stress free expansion");
        }
      } else {
        throw_if(true,
                 "internal error, unsupported stress "
                 "free expansion description");
      }
    }
    if (this->mb.getSymmetryType() == mfront::ORTHOTROPIC) {
      if (this->mb.getOrthotropicAxesConvention() ==
          OrthotropicAxesConvention::PIPE) {
        os << "tfel::material::convertStressFreeExpansionStrain<hypothesis,"
              "tfel::material::OrthotropicAxesConvention::"
              "PIPE>(dl0_l0);\n"
           << "tfel::material::convertStressFreeExpansionStrain<hypothesis,"
              "tfel::material::OrthotropicAxesConvention::"
              "PIPE>(dl1_l0);\n";
      } else if (this->mb.getOrthotropicAxesConvention() ==
                 OrthotropicAxesConvention::PLATE) {
        os << "tfel::material::convertStressFreeExpansionStrain<hypothesis,"
              "tfel::material::OrthotropicAxesConvention::"
              "PLATE>(dl0_l0);\n"
           << "tfel::material::convertStressFreeExpansionStrain<hypothesis,"
              "tfel::material::OrthotropicAxesConvention::"
              "PLATE>(dl1_l0);\n";
      } else {
        throw_if(this->mb.getOrthotropicAxesConvention() !=
                     OrthotropicAxesConvention::DEFAULT,
                 "internal error, unsupported orthotropic axes convention");
        for (const auto mh : this->mb.getDistinctModellingHypotheses()) {
          throw_if(mh != ModellingHypothesis::TRIDIMENSIONAL,
                   "an orthotropic axes convention must be choosen when "
                   "defining a stress free expansion in behaviours "
                   "which shall be valid in other modelling hypothesis "
                   "than 'Tridimensional'.\n"
                   "Either restrict the validity of the behaviour to "
                   "'Tridimensional' (see @ModellingHypothesis) or "
                   "choose and orthotropic axes convention as on option "
                   "to the @OrthotropicBehaviour keyword");
        }
      }
    }
    os << "}\n\n";
  }  // end of writeBehaviourComputeStressFreeExpansion

  void BehaviourDSLCommon::writeBehaviourInitializeMethods(
      std::ostream& os, const Hypothesis h) const {
    this->checkBehaviourFile(os);
    os << "/*!\n"
       << " * \\ brief initialize the behaviour with user code\n"
       << " */\n"
       << "void initialize(){\n"
       << "using namespace std;\n"
       << "using namespace tfel::math;\n"
       << "using std::vector;\n";
    writeMaterialLaws(os, this->mb.getMaterialLaws());
    if (this->mb.hasCode(h, BehaviourData::BeforeInitializeLocalVariables)) {
      if (this->mb.getAttribute(BehaviourData::profiling, false)) {
        writeStandardPerformanceProfilingBegin(
            os, this->mb.getClassName(),
            BehaviourData::BeforeInitializeLocalVariables, "binit");
      }
      os << this->mb
                .getCodeBlock(h, BehaviourData::BeforeInitializeLocalVariables)
                .code
         << '\n';
      if (this->mb.getAttribute(BehaviourData::profiling, false)) {
        writeStandardPerformanceProfilingEnd(os);
      }
    }
    if (this->mb.hasCode(h, BehaviourData::InitializeLocalVariables)) {
      if (this->mb.getAttribute(BehaviourData::profiling, false)) {
        writeStandardPerformanceProfilingBegin(
            os, this->mb.getClassName(),
            BehaviourData::InitializeLocalVariables, "init");
      }
      os << this->mb.getCodeBlock(h, BehaviourData::InitializeLocalVariables)
                .code
         << '\n';
      if (this->mb.getAttribute(BehaviourData::profiling, false)) {
        writeStandardPerformanceProfilingEnd(os);
      }
    }
    if (this->mb.hasCode(h, BehaviourData::AfterInitializeLocalVariables)) {
      if (this->mb.getAttribute(BehaviourData::profiling, false)) {
        writeStandardPerformanceProfilingBegin(
            os, this->mb.getClassName(),
            BehaviourData::AfterInitializeLocalVariables, "ainit");
      }
      os << this->mb
                .getCodeBlock(h, BehaviourData::AfterInitializeLocalVariables)
                .code
         << '\n';
      if (this->mb.getAttribute(BehaviourData::profiling, false)) {
        writeStandardPerformanceProfilingEnd(os);
      }
    }
    this->writeBehaviourParserSpecificInitializeMethodPart(os, h);
    os << "}\n\n";
  }  // end of void BehaviourDSLCommon::writeBehaviourInitializeMethod

  void BehaviourDSLCommon::writeBehaviourLocalVariablesInitialisation(
      std::ostream& os, const Hypothesis h) const {
    const auto& md = this->mb.getBehaviourData(h);
    this->checkBehaviourFile(os);
    for (const auto& v : md.getLocalVariables()) {
      if (this->mb.useDynamicallyAllocatedVector(v.arraySize)) {
        os << "this->" << v.name << ".resize(" << v.arraySize << ");\n";
      }
    }
  }

  void BehaviourDSLCommon::writeBehaviourParameterInitialisation(
      std::ostream& os, const Hypothesis h) const {
    constexpr auto uh = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    const auto use_static_variables =
        areParametersTreatedAsStaticVariables(this->mb);
    this->checkBehaviourFile(os);
    const auto& d = this->mb.getBehaviourData(h);
    for (const auto& p : d.getParameters()) {
      if (use_static_variables) {
        if (!p.getAttribute<bool>(
                VariableDescription::variableDeclaredInBaseClass, false)) {
          continue;
        }
        if (p.type == "int") {
          os << "this->" << p.name << " = "
             << this->mb.getIntegerParameterDefaultValue(h, p.name) << ";\n";
        } else if (p.type == "ushort") {
          os << "this->" << p.name << " = "
             << this->mb.getUnsignedShortParameterDefaultValue(h, p.name)
             << ";\n";
        } else {
          const auto f = SupportedTypes::getTypeFlag(p.type);
          if (f != SupportedTypes::SCALAR) {
            this->throwRuntimeError(
                "BehaviourDSLCommon::writeBehaviourParameterInitialisation",
                "unsupported parameter type '" + p.type +
                    "' "
                    "for parameter '" +
                    p.name + "'");
          }
          if (p.arraySize == 1u) {
            os << "this->" << p.name << " = " << p.type << "("
               << this->mb.getFloattingPointParameterDefaultValue(h, p.name)
               << ");\n";
          } else {
            for (unsigned short i = 0; i != p.arraySize; ++i) {
              os << "this->" << p.name << "[" << i << "] = " << p.type << "("
                 << this->mb.getFloattingPointParameterDefaultValue(h, p.name,
                                                                    i)
                 << ");\n";
            }
          }
        }
      } else {
        const auto getter = [this, h, &p] {
          if ((h == uh) || (this->mb.hasParameter(uh, p.name))) {
            return this->mb.getClassName() + "ParametersInitializer::get()";
          }
          return this->mb.getClassName() + ModellingHypothesis::toString(h) +
                 "ParametersInitializer::get()";
        }();
        if (p.arraySize == 1u) {
          os << "this->" << p.name << " = ";
          if (this->mb.useQt()) {
            os << p.type << "(" << getter << "." << p.name << ");\n";
          } else {
            os << getter << "." << p.name << ";\n";
          }
        } else {
          os << "this->" << p.name << " = "
             << "tfel::math::map<tfel::math::fsarray<" << p.arraySize << ", "
             << p.type << ">>(" << getter << "." << p.name << ".data());\n";
        }
      }
    }
  }  // end of writeBehaviourParameterInitialisation

  void BehaviourDSLCommon::writeBehaviourDataMainVariablesSetters(
      std::ostream& os) const {
    this->checkBehaviourDataFile(os);
    for (const auto& i : this->interfaces) {
      i.second->writeBehaviourDataMainVariablesSetters(os, this->mb);
      os << '\n';
    }
  }  // end of writeBehaviourDataMainVariablesSetters

  void BehaviourDSLCommon::writeIntegrationDataMainVariablesSetters(
      std::ostream& os) const {
    this->checkIntegrationDataFile(os);
    for (const auto& i : this->interfaces) {
      i.second->writeIntegrationDataMainVariablesSetters(os, this->mb);
      os << '\n';
    }
  }  // end of writeIntegrationDataMainVariablesSetters

  void BehaviourDSLCommon::writeBehaviourGetModellingHypothesis(
      std::ostream& os) const {
    this->checkBehaviourFile(os);
    os << "/*!\n"
       << "* \\return the modelling hypothesis\n"
       << "*/\n"
       << "constexpr ModellingHypothesis::Hypothesis\ngetModellingHypothesis() "
          "const{\n"
       << "return hypothesis;\n"
       << "} // end of getModellingHypothesis\n\n";
  }  // end of writeBehaviourGetModellingHypothesis();

  void BehaviourDSLCommon::writeBehaviourLocalVariables(
      std::ostream& os, const Hypothesis h) const {
    this->checkBehaviourFile(os);
    const auto& md = this->mb.getBehaviourData(h);
    this->writeVariablesDeclarations(os, md.getLocalVariables(), "", "",
                                     this->fd.fileName, false);
    os << '\n';
  }

  void BehaviourDSLCommon::writeBehaviourIntegrationVariables(
      std::ostream& os, const Hypothesis h) const {
    this->checkBehaviourFile(os);
    const auto& md = this->mb.getBehaviourData(h);
    for (const auto& v : md.getIntegrationVariables()) {
      if (!md.isStateVariableName(v.name)) {
        if (md.isMemberUsedInCodeBlocks(v.name)) {
          this->writeVariableDeclaration(os, v, "", "", this->fd.fileName,
                                         false);
        }
      }
    }
    os << '\n';
  }  // end od BehaviourDSLCommon::writeBehaviourIntegrationVariables

  void BehaviourDSLCommon::writeBehaviourParameters(std::ostream& os,
                                                    const Hypothesis h) const {
    this->checkBehaviourFile(os);
    const auto use_static_variables =
        areParametersTreatedAsStaticVariables(this->mb);
    const auto& d = this->mb.getBehaviourData(h);
    for (const auto& p : d.getParameters()) {
      if (p.getAttribute<bool>(VariableDescription::variableDeclaredInBaseClass,
                               false)) {
        continue;
      }
      if (!getDebugMode()) {
        if (p.lineNumber != 0u) {
          os << "#line " << p.lineNumber << " \"" << this->fd.fileName
             << "\"\n";
        }
      }
      if (use_static_variables) {
        os << "static constexpr ";
        if (p.type == "int") {
          os << "int " << p.name << " = "
             << this->mb.getIntegerParameterDefaultValue(h, p.name) << ";\n";
        } else if (p.type == "ushort") {
          os << "unsigned short " << p.name << " = "
             << this->mb.getUnsignedShortParameterDefaultValue(h, p.name)
             << ";\n";
        } else {
          const auto f = SupportedTypes::getTypeFlag(p.type);
          if (f != SupportedTypes::SCALAR) {
            this->throwRuntimeError(
                "BehaviourDSLCommon::writeBehaviourParameters",
                "unsupported parameter type '" + p.type +
                    "' "
                    "for parameter '" +
                    p.name + "'");
          }
          if (p.arraySize == 1u) {
            os << p.type << " " << p.name << " = " << p.type << "("
               << this->mb.getFloattingPointParameterDefaultValue(h, p.name)
               << ");\n";
          } else {
            os << "tfel::math::fsarray<" << p.arraySize << ", " << p.type
               << "> " << p.name << " = {";
            for (unsigned short i = 0; i != p.arraySize;) {
              os << p.type << "("
                 << this->mb.getFloattingPointParameterDefaultValue(h, p.name,
                                                                    i)
                 << ")";
              if (++i != p.arraySize) {
                os << ", ";
              }
            }
            os << "};\n";
          }
        }
      } else {
        if (p.arraySize == 1) {
          os << p.type << " " << p.name << ";\n";
        } else {
          os << "tfel::math::fsarray<" << p.arraySize << "," << p.type << "> "
             << p.name << ";\n";
        }
      }
    }
    os << '\n';
  }  // end of writeBehaviourParameters

  void BehaviourDSLCommon::writeBehaviourOutOfBoundsPolicyVariable(
      std::ostream& os) const {
    this->checkBehaviourFile(os);
    const auto p = getDefaultOutOfBoundsPolicyAsString(this->mb);
    os << "//! policy for treating out of bounds conditions\n";
    if (allowRuntimeModificationOfTheOutOfBoundsPolicy(this->mb)) {
      os << "OutOfBoundsPolicy policy = tfel::material::" << p << ";\n\n";
    } else {
      os << "static constexpr auto policy = tfel::material::" << p << ";\n\n";
    }
  }  // end of writeBehaviourOutOfBoundsPolicyVariable

  void BehaviourDSLCommon::writeBehaviourStaticVariables(
      std::ostream& os, const Hypothesis h) const {
    const auto& md = this->mb.getBehaviourData(h);
    this->checkBehaviourFile(os);
    for (const auto& v : md.getStaticVariables()) {
      if (!getDebugMode()) {
        if (v.lineNumber != 0u) {
          os << "#line " << v.lineNumber << " \"" << this->fd.fileName
             << "\"\n";
        }
      }
      os << "static constexpr auto " << v.name << " = "  //
         << v.type << "{" << v.value << "};\n";
    }
    os << '\n';
  }

  void BehaviourDSLCommon::writeBehaviourIntegrationVariablesIncrements(
      std::ostream& os, const Hypothesis h) const {
    const auto& md = this->mb.getBehaviourData(h);
    this->checkBehaviourFile(os);
    this->writeVariablesDeclarations(os, md.getIntegrationVariables(), "d", "",
                                     this->fd.fileName,
                                     this->useStateVarTimeDerivative);
    os << '\n';
  }

  void BehaviourDSLCommon::writeBehaviourOutputOperator(
      std::ostream& os, const Hypothesis h) const {
    const auto& md = this->mb.getBehaviourData(h);
    this->checkBehaviourFile(os);
    if (h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
      if (this->mb.useQt()) {
        os << "template<ModellingHypothesis::Hypothesis hypothesis, "
           << "typename NumericType, bool use_qt>\n"
           << "std::ostream&\n"
           << "operator <<(std::ostream& os,"
           << "const " << this->mb.getClassName()
           << "<hypothesis, NumericType, use_qt>& b)\n";
      } else {
        os << "template<ModellingHypothesis::Hypothesis hypothesis, "
           << "typename NumericType>\n"
           << "std::ostream&\n"
           << "operator <<(std::ostream& os,"
           << "const " << this->mb.getClassName()
           << "<hypothesis, NumericType, false>& b)\n";
      }
    } else {
      if (this->mb.useQt()) {
        os << "template<typename NumericType,bool use_qt>\n"
           << "std::ostream&\n"
           << "operator <<(std::ostream& os,"
           << "const " << this->mb.getClassName() << "<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, use_qt>& b)\n";
      } else {
        os << "template<typename NumericType>\n"
           << "std::ostream&\n"
           << "operator <<(std::ostream& os,"
           << "const " << this->mb.getClassName() << "<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, false>& b)\n";
      }
    }
    os << "{\n";
    for (const auto& v : this->mb.getMainVariables()) {
      if (Gradient::isIncrementKnown(v.first)) {
        os << "os << \"" << displayName(v.first) << " : \" << b."
           << v.first.name << " << '\\n';\n";
        if (getUnicodeOutputOption()) {
          os << "os << \"\u0394" << displayName(v.first) << " : \" << b.d"
             << v.first.name << " << '\\n';\n";
        } else {
          os << "os << \"d" << displayName(v.first) << " : \" << b.d"
             << v.first.name << " << '\\n';\n";
        }
      } else {
        if (getUnicodeOutputOption()) {
          os << "os << \"" << displayName(v.first) << "\u2080 : \" << b."
             << v.first.name << "0 << '\\n';\n"
             << "os << \"" << displayName(v.first) << "\u2081 : \" << b."
             << v.first.name << "1 << '\\n';\n";
        } else {
          os << "os << \"" << displayName(v.first) << "0 : \" << b."
             << v.first.name << "0 << '\\n';\n"
             << "os << \"" << displayName(v.first) << "1 : \" << b."
             << v.first.name << "1 << '\\n';\n";
        }
      }
      os << "os << \"" << displayName(v.second) << " : \" << b."
         << v.second.name << " << '\\n';\n";
    }
    if (getUnicodeOutputOption()) {
      os << "os << \"\u0394t : \" << b.dt << '\\n';\n";
    } else {
      os << "os << \"dt : \" << b.dt << '\\n';\n";
    }
    for (const auto& v : md.getMaterialProperties()) {
      os << "os << \"" << displayName(v) << " : \" << b." << v.name
         << " << '\\n';\n";
    }
    for (const auto& v : md.getStateVariables()) {
      os << "os << \"" << displayName(v) << " : \" << b." << v.name
         << " << '\\n';\n";
      if (getUnicodeOutputOption()) {
        os << "os << \"\u0394" << displayName(v) << " : \" << b.d" << v.name
           << " << '\\n';\n";
      } else {
        os << "os << \"d" << displayName(v) << " : \" << b.d" << v.name
           << " << '\\n';\n";
      }
    }
    for (const auto& v : md.getAuxiliaryStateVariables()) {
      os << "os << \"" << displayName(v) << " : \" << b." << v.name
         << " << '\\n';\n";
    }
    for (const auto& v : md.getExternalStateVariables()) {
      os << "os << \"" << displayName(v) << " : \" << b." << v.name
         << " << '\\n';\n";
      if (getUnicodeOutputOption()) {
        os << "os << \"\u0394" << displayName(v) << " : \" << b.d" << v.name
           << " << '\\n';\n";
      } else {
        os << "os << \"d" << displayName(v) << " : \" << b.d" << v.name
           << " << '\\n';\n";
      }
    }
    for (const auto& v : md.getLocalVariables()) {
#pragma message("BehaviourDSLCommon: handle LocalDataStructure properly")
      if ((v.type.size() >= 7) && (v.type.substr(0, 7) != "struct{")) {
        os << "os << \"" << displayName(v) << " : \" << b." << v.name
           << " << '\\n';\n";
      }
    }
    for (const auto& v : md.getParameters()) {
      os << "os << \"" << displayName(v) << " : \" << b." << v.name
         << " << '\\n';\n";
    }
    os << "return os;\n"
       << "}\n\n";
  }

  void BehaviourDSLCommon::writeBehaviourDestructor(std::ostream& os) const {
    this->checkBehaviourFile(os);
    os << "//!\n"
       << "~" << this->mb.getClassName() << "()\n"
       << " override = default;\n\n";
  }

  void BehaviourDSLCommon::writeBehaviourUpdateExternalStateVariables(
      std::ostream& os, const Hypothesis h) const {
    const auto& md = this->mb.getBehaviourData(h);
    this->checkBehaviourFile(os);
    os << "void updateExternalStateVariables(){\n";
    for (const auto& v : this->mb.getMainVariables()) {
      if (Gradient::isIncrementKnown(v.first)) {
        os << "this->" << v.first.name << "  += this->d" << v.first.name
           << ";\n";
      } else {
        os << "this->" << v.first.name << "0  = this->" << v.first.name
           << "1;\n";
      }
    }
    for (const auto& v : md.getExternalStateVariables()) {
      os << "this->" << v.name << " += this->d" << v.name << ";\n";
    }
    os << "}\n\n";
  }

  void BehaviourDSLCommon::writeBehaviourInitializeFunctions(
      std::ostream& os, const Hypothesis h) const {
    for (const auto& i : this->interfaces) {
      i.second->writeBehaviourInitializeFunctions(os, this->mb, h);
    }
  }  // end of writeBehaviourInitializeFunctions

  void BehaviourDSLCommon::writeBehaviourPostProcessings(
      std::ostream& os, const Hypothesis h) const {
    for (const auto& i : this->interfaces) {
      i.second->writeBehaviourPostProcessings(os, this->mb, h);
    }
  }  // end of writeBehaviourPostProcessings

  void BehaviourDSLCommon::writeBehaviourIncludes(std::ostream& os) const {
    this->checkBehaviourFile(os);
    os << "#include<string>\n"
       << "#include<iostream>\n"
       << "#include<limits>\n"
       << "#include<stdexcept>\n"
       << "#include<algorithm>\n\n"
       << "#include\"TFEL/Raise.hxx\"\n"
       << "#include\"TFEL/PhysicalConstants.hxx\"\n"
       << "#include\"TFEL/Config/TFELConfig.hxx\"\n"
       << "#include\"TFEL/Config/TFELTypes.hxx\"\n"
       << "#include\"TFEL/TypeTraits/IsFundamentalNumericType.hxx\"\n"
       << "#include\"TFEL/TypeTraits/IsReal.hxx\"\n"
       << "#include\"TFEL/Math/General/IEEE754.hxx\"\n"
       << "#include\"TFEL/Material/MaterialException.hxx\"\n"
       << "#include\"TFEL/Material/MechanicalBehaviour.hxx\"\n"
       << "#include\"TFEL/Material/MechanicalBehaviourTraits.hxx\"\n"
       << "#include\"TFEL/Material/OutOfBoundsPolicy.hxx\"\n"
       << "#include\"TFEL/Material/BoundsCheck.hxx\"\n"
       << "#include\"TFEL/Material/IsotropicPlasticity.hxx\"\n"
       << "#include\"TFEL/Material/Lame.hxx\"\n"
       << "#include\"TFEL/Material/Hosford1972YieldCriterion.hxx\"\n";
    if (this->mb.getSymmetryType() == ORTHOTROPIC) {
      os << "#include\"TFEL/Material/OrthotropicPlasticity.hxx\"\n"
         << "#include\"TFEL/Material/"
            "OrthotropicStressLinearTransformation.hxx\"\n"
         << "#include\"TFEL/Material/Hill.hxx\"\n"
         << "#include\"TFEL/Material/Barlat2004YieldCriterion.hxx\"\n"
         << "#include\"TFEL/Material/OrthotropicAxesConvention.hxx\"\n";
    }
    if (this->mb.getAttribute(BehaviourDescription::computesStiffnessTensor,
                              false)) {
      os << "#include\"TFEL/Material/StiffnessTensor.hxx\"\n";
    }
    if ((this->mb.isStrainMeasureDefined()) &&
        (this->mb.getStrainMeasure() == BehaviourDescription::HENCKY)) {
      os << "#include\"TFEL/Material/"
            "LogarithmicStrainComputeAxialStrainIncrementElasticPrediction."
            "hxx\"\n";
    }
    if (this->mb.getAttribute<bool>(BehaviourData::profiling, false)) {
      os << "#include\"MFront/BehaviourProfiler.hxx\"\n";
    }
    os << "#include\"" << this->getBehaviourDataFileName() << "\"\n"
       << "#include\"" << this->getIntegrationDataFileName() << "\"\n";
    os << '\n';
  }

  void BehaviourDSLCommon::writeBehaviourAdditionalMembers(
      std::ostream& os, const Hypothesis h) const {
    this->checkBehaviourFile(os);
    const auto& m = this->mb.getMembers(h);
    if (!m.empty()) {
      os << m << "\n\n";
    }
  }

  void BehaviourDSLCommon::writeBehaviourPrivate(std::ostream& os,
                                                 const Hypothesis h) const {
    this->checkBehaviourFile(os);
    const auto& c = this->mb.getPrivateCode(h);
    if (!c.empty()) {
      os << c << "\n\n";
    }
  }  // end of void BehaviourDSLCommon::writeBehaviourPrivate

  void BehaviourDSLCommon::writeBehaviourTypeAliases(std::ostream& os) const {
    using namespace tfel::material;
    this->checkBehaviourFile(os);
    const auto btype = this->mb.getBehaviourTypeFlag();
    os << "static constexpr unsigned short TVectorSize = N;\n"
       << "typedef tfel::math::StensorDimeToSize<N> StensorDimeToSize;\n"
       << "static constexpr unsigned short StensorSize = "
       << "StensorDimeToSize::value;\n"
       << "typedef tfel::math::TensorDimeToSize<N> TensorDimeToSize;\n"
       << "static constexpr unsigned short TensorSize = "
       << "TensorDimeToSize::value;\n\n";
    this->writeTypeAliases(os);
    os << '\n' << "public :\n\n";
    const auto qt = this->mb.useQt() ? "use_qt" : "false";
    os << "typedef " << this->mb.getClassName()
       << "BehaviourData<hypothesis, NumericType, " << qt
       << "> BehaviourData;\n"
       << "typedef " << this->mb.getClassName()
       << "IntegrationData<hypothesis, NumericType, " << qt
       << "> IntegrationData;\n"
       << "typedef typename MechanicalBehaviour<" << btype
       << ",hypothesis, NumericType, " << qt << ">::SMFlag SMFlag;\n"
       << "typedef typename MechanicalBehaviour<" << btype
       << ",hypothesis, NumericType, " << qt << ">::SMType SMType;\n"
       << "using MechanicalBehaviour<" << btype << ",hypothesis, NumericType, "
       << qt << ">::ELASTIC;\n"
       << "using MechanicalBehaviour<" << btype << ",hypothesis, NumericType, "
       << qt << ">::SECANTOPERATOR;\n"
       << "using MechanicalBehaviour<" << btype << ",hypothesis, NumericType, "
       << qt << ">::TANGENTOPERATOR;\n"
       << "using MechanicalBehaviour<" << btype << ",hypothesis, NumericType, "
       << qt << ">::CONSISTENTTANGENTOPERATOR;\n"
       << "using MechanicalBehaviour<" << btype << ",hypothesis, NumericType, "
       << qt << ">::NOSTIFFNESSREQUESTED;\n";
    if ((this->mb.getBehaviourType() ==
         BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) ||
        (this->mb.getBehaviourType() ==
         BehaviourDescription::COHESIVEZONEMODEL)) {
      os << "using MechanicalBehaviour<" << btype
         << ",hypothesis, NumericType, " << qt
         << ">::STANDARDTANGENTOPERATOR;\n";
    } else if (this->mb.getBehaviourType() ==
               BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) {
      for (const auto& toflag :
           getFiniteStrainBehaviourTangentOperatorFlags()) {
        os << "using MechanicalBehaviour<" << btype
           << ",hypothesis, NumericType, " << qt << ">::"
           << convertFiniteStrainBehaviourTangentOperatorFlagToString(toflag)
           << ";\n";
      }
    }
    os << "using IntegrationResult = typename MechanicalBehaviour<" << btype
       << ",hypothesis, NumericType, " << qt << ">::IntegrationResult;\n\n"
       << "using MechanicalBehaviour<" << btype << ",hypothesis, NumericType, "
       << qt << ">::SUCCESS;\n"
       << "using MechanicalBehaviour<" << btype << ",hypothesis, NumericType, "
       << qt << ">::FAILURE;\n"
       << "using MechanicalBehaviour<" << btype << ",hypothesis, NumericType, "
       << qt << ">::UNRELIABLE_RESULTS;\n\n";
    if ((this->mb.getBehaviourType() ==
         BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) ||
        (this->mb.getBehaviourType() ==
         BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR)) {
      os << "using StressFreeExpansionType = "
         << this->mb.getStressFreeExpansionType() << ";\n\n";
    }
  }  // end of writeBehaviourTypeAliases

  void BehaviourDSLCommon::writeBehaviourTraits(std::ostream& os) const {
    constexpr auto uh = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    this->checkBehaviourFile(os);
    const auto& ah = ModellingHypothesis::getModellingHypotheses();
    // writing partial specialisations
    if (this->mb.getModellingHypotheses().size() >= 4u) {
      // on définit toutes les hypothèses par défaut
      this->writeBehaviourTraitsSpecialisation(os, uh, true);
      // unsupported hypothesis
      for (const auto h : ah) {
        if (this->mb.isModellingHypothesisSupported(h)) {
          if (this->mb.hasSpecialisedMechanicalData(h)) {
            this->writeBehaviourTraitsSpecialisation(os, h, true);
          }
        } else {
          this->writeBehaviourTraitsSpecialisation(os, h, false);
        }
      }
    } else {
      // on exclut toutes les hypothèses par défaut
      this->writeBehaviourTraitsSpecialisation(os, uh, false);
      // unsupported hypothesis
      for (const auto h : this->mb.getModellingHypotheses()) {
        this->writeBehaviourTraitsSpecialisation(os, h, true);
      }
    }
  }

  void BehaviourDSLCommon::writeBehaviourTraitsSpecialisation(
      std::ostream& os, const Hypothesis h, const bool b) const {
    auto boolean = [](const bool bv) { return bv ? "true" : "false"; };
    auto get_boolean_attribute = [boolean, &b, &h,
                                  this](const char* const attribute) {
      if (b) {
        if (this->mb.getAttribute<bool>(h, attribute, false)) {
          return boolean(true);
        }
      }
      return boolean(false);
    };
    auto has_code = [boolean, &b, &h, this](const char* const cb) {
      if (b) {
        if (this->mb.hasCode(h, cb)) {
          return boolean(true);
        }
      }
      return boolean(false);
    };
    SupportedTypes::TypeSize coefSize;
    SupportedTypes::TypeSize stateVarsSize;
    SupportedTypes::TypeSize externalStateVarsSize;
    if (b) {
      const auto& d = this->mb.getBehaviourData(h);
      for (const auto& m : d.getMaterialProperties()) {
        coefSize += this->getTypeSize(m.type, m.arraySize);
      }
      for (const auto& v : d.getPersistentVariables()) {
        stateVarsSize += this->getTypeSize(v.type, v.arraySize);
      }
      for (const auto& v : d.getExternalStateVariables()) {
        externalStateVarsSize += this->getTypeSize(v.type, v.arraySize);
      }
    }
    os << "/*!\n"
       << "* Partial specialisation for " << this->mb.getClassName() << ".\n"
       << "*/\n";
    if (h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
      if (this->mb.useQt()) {
        os << "template<ModellingHypothesis::Hypothesis hypothesis, "
           << "typename NumericType,bool use_qt>\n"
           << "class MechanicalBehaviourTraits<" << this->mb.getClassName()
           << "<hypothesis, NumericType, use_qt> >\n";
      } else {
        os << "template<ModellingHypothesis::Hypothesis hypothesis, "
           << "typename NumericType>\n"
           << "class MechanicalBehaviourTraits<" << this->mb.getClassName()
           << "<hypothesis, NumericType, false> >\n";
      }
    } else {
      if (this->mb.useQt()) {
        os << "template<typename NumericType,bool use_qt>\n"
           << "class MechanicalBehaviourTraits<" << this->mb.getClassName()
           << "<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, use_qt> >\n";
      } else {
        os << "template<typename NumericType>\n"
           << "class MechanicalBehaviourTraits<" << this->mb.getClassName()
           << "<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, false> >\n";
      }
    }
    os << "{\n"
       << "using size_type = unsigned short;\n";
    if (b) {
      if (h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
        os << "static constexpr unsigned short N = "
           << "ModellingHypothesisToSpaceDimension<hypothesis>::value;\n";
      } else {
        os << "static constexpr unsigned short N = "
              "ModellingHypothesisToSpaceDimension<"
           << "ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h) << ">::value;\n";
      }
      os << "static constexpr unsigned short TVectorSize = N;\n"
         << "typedef tfel::math::StensorDimeToSize<N> StensorDimeToSize;\n"
         << "static constexpr unsigned short StensorSize = "
         << "StensorDimeToSize::value;\n"
         << "typedef tfel::math::TensorDimeToSize<N> TensorDimeToSize;\n"
         << "static constexpr unsigned short TensorSize = "
         << "TensorDimeToSize::value;\n";
    }
    os << "public:\n";
    os << "static constexpr bool is_defined = " << boolean(b) << ";\n";
    if (this->mb.useQt()) {
      os << "static constexpr bool use_quantities = use_qt;\n";
    } else {
      os << "static constexpr bool use_quantities = false;\n";
    }
    if (this->mb.getSymmetryType() == mfront::ORTHOTROPIC) {
      os << "//! orthotropic axes convention\n";
      if (this->mb.getOrthotropicAxesConvention() ==
          OrthotropicAxesConvention::DEFAULT) {
        os << "static constexpr OrthotropicAxesConvention oac = "
           << "OrthotropicAxesConvention::DEFAULT;\n";
      } else if (this->mb.getOrthotropicAxesConvention() ==
                 OrthotropicAxesConvention::PIPE) {
        os << "static constexpr OrthotropicAxesConvention oac = "
           << "OrthotropicAxesConvention::PIPE;\n";
      } else if (this->mb.getOrthotropicAxesConvention() ==
                 OrthotropicAxesConvention::PLATE) {
        os << "static constexpr OrthotropicAxesConvention oac = "
           << "OrthotropicAxesConvention::PLATE;\n";
      } else {
        this->throwRuntimeError(
            "BehaviourDSLCommon::writeBehaviourTraitsSpecialisation",
            "internal error : unsupported orthotropic axes convention");
      }
    }
    os << "static constexpr bool hasStressFreeExpansion = "
       << boolean((b) && (this->mb.requiresStressFreeExpansionTreatment(h)))
       << ";\n";
    os << "static constexpr bool handlesThermalExpansion = "
       << boolean(this->mb.areThermalExpansionCoefficientsDefined()) << ";\n";
    if (b) {
      os << "static constexpr unsigned short dimension = N;\n";
    } else {
      os << "static constexpr unsigned short dimension = 0u;\n";
    }
    os << "static constexpr size_type material_properties_nb = " << coefSize
       << ";\n"
       << "static constexpr size_type internal_variables_nb  = "
       << stateVarsSize << ";\n";
    if (this->mb.getAttribute(
            BehaviourDescription::
                automaticDeclarationOfTheTemperatureAsFirstExternalStateVariable,
            true)) {
      SupportedTypes::TypeSize externalStateVarsSize2 = externalStateVarsSize;
      if (b) {
        externalStateVarsSize2 -=
            SupportedTypes::TypeSize(SupportedTypes::SCALAR);
      }
      os << "static constexpr size_type external_variables_nb  = "
         << externalStateVarsSize << ";\n"
         << "static constexpr size_type external_variables_nb2 = "
         << externalStateVarsSize2 << ";\n";
    } else {
      os << "static constexpr size_type external_variables_nb  = "
         << externalStateVarsSize << ";\n"
         << "static constexpr size_type external_variables_nb2 = "
         << externalStateVarsSize << ";\n";
    }
    os << "static constexpr bool hasConsistentTangentOperator = "
       << get_boolean_attribute(BehaviourData::hasConsistentTangentOperator)
       << ";\n";
    os << "static constexpr bool isConsistentTangentOperatorSymmetric = "
       << get_boolean_attribute(
              BehaviourData::isConsistentTangentOperatorSymmetric)
       << ";\n";
    os << "static constexpr bool hasPredictionOperator = "
       << get_boolean_attribute(BehaviourData::hasPredictionOperator) << ";\n";
    os << "static constexpr bool hasAPrioriTimeStepScalingFactor = "
       << get_boolean_attribute(BehaviourData::hasAPrioriTimeStepScalingFactor)
       << ";\n";
    // internal enery
    os << "static constexpr bool hasComputeInternalEnergy = "
       << has_code(BehaviourData::ComputeInternalEnergy) << ";\n";
    // dissipated energy
    os << "static constexpr bool hasComputeDissipatedEnergy = "
       << has_code(BehaviourData::ComputeDissipatedEnergy) << ";\n";
    // name of the class
    os << "/*!\n"
       << "* \\return the name of the class.\n"
       << "*/\n"
       << "static const char* getName(){\n"
       << "return \"" << this->mb.getClassName() << "\";\n"
       << "}\n\n"
       << "};\n\n";
  }

  void BehaviourDSLCommon::writeBehaviourParserSpecificInheritanceRelationship(
      std::ostream& os, const Hypothesis) const {
    os << '\n';
  }

  void BehaviourDSLCommon::writeBehaviourParserSpecificTypedefs(
      std::ostream&) const {
    // Empty member meant to be overriden in Child if necessary
  }

  void BehaviourDSLCommon::writeBehaviourParserSpecificMembers(
      std::ostream&, const Hypothesis) const {
    // Empty member meant to be overriden in Child if necessary
  }

  void BehaviourDSLCommon::writeBehaviourParserSpecificIncludes(
      std::ostream&) const {
    // Empty member meant to be overriden in Child if necessary
  }

  void BehaviourDSLCommon::writeBehaviourParametersInitializers(
      std::ostream& os) const {
    if ((areParametersTreatedAsStaticVariables(this->mb)) ||
        (!this->mb.hasParameters())) {
      return;
    }
    auto mh = this->mb.getDistinctModellingHypotheses();
    mh.insert(ModellingHypothesis::UNDEFINEDHYPOTHESIS);
    for (const auto h : mh) {
      if (this->mb.hasParameters(h)) {
        this->writeBehaviourParametersInitializer(os, h);
      }
    }
  }  // end of writeBehaviourParametersInitializers

  void BehaviourDSLCommon::writeBehaviourParametersInitializer(
      std::ostream& os, const Hypothesis h) const {
    // useless and paranoid test
    if ((areParametersTreatedAsStaticVariables(this->mb)) ||
        (!this->mb.hasParameters())) {
      return;
    }
    const auto& md = this->mb.getBehaviourData(h);
    const auto& params = md.getParameters();
    std::string cname(this->mb.getClassName());
    if (h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
      cname += ModellingHypothesis::toString(h);
    }
    cname += "ParametersInitializer";
    bool rp = false;
    bool ip = false;
    bool up = false;
    bool rp2 = false;
    bool ip2 = false;
    bool up2 = false;
    this->checkBehaviourFile(os);
    os << "struct " << cname << '\n'
       << "{\n"
       << "static " << cname << "&\n"
       << "get();\n\n";
    for (const auto& p : params) {
      if (p.type == "int") {
        ip = true;
        if ((h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) ||
            ((h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) &&
             (!this->mb.hasParameter(ModellingHypothesis::UNDEFINEDHYPOTHESIS,
                                     p.name)))) {
          ip2 = true;
          os << "int " << p.name << ";\n";
        }
      } else if (p.type == "ushort") {
        up = true;
        if ((h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) ||
            ((h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) &&
             (!this->mb.hasParameter(ModellingHypothesis::UNDEFINEDHYPOTHESIS,
                                     p.name)))) {
          up2 = true;
          os << "unsigned short " << p.name << ";\n";
        }
      } else {
        const auto f = SupportedTypes::getTypeFlag(p.type);
        if (f != SupportedTypes::SCALAR) {
          this->throwRuntimeError(
              "BehaviourDSLCommon::writeBehaviourParametersInitializer",
              "invalid type for parameter '" + p.name + "' ('" + p.type + "')");
        }
        rp = true;
        if ((h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) ||
            ((h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) &&
             (!this->mb.hasParameter(ModellingHypothesis::UNDEFINEDHYPOTHESIS,
                                     p.name)))) {
          rp2 = true;
          if (p.arraySize == 1) {
            os << "double " << p.name << ";\n";
          } else {
            os << "tfel::math::fsarray<" << p.arraySize << ",double> " << p.name
               << ";\n";
          }
        }
      }
    }
    if (!params.empty()) {
      os << '\n';
    }
    if (rp) {
      os << "void set(const char* const,const double);\n\n";
    }
    if (ip) {
      os << "void set(const char* const,const int);\n\n";
    }
    if (up) {
      os << "void set(const char* const,const unsigned short);\n\n";
    }
    if (rp2) {
      os << "/*!\n"
         << " * \\brief convert a string to double\n"
         << " * \\param[in] p : parameter\n"
         << " * \\param[in] v : value\n"
         << " */\n"
         << "static double getDouble(const std::string&,const std::string&);\n";
    }
    if (ip2) {
      os << "/*!\n"
         << " * \\brief convert a string to int\n"
         << " * \\param[in] p : parameter\n"
         << " * \\param[in] v : value\n"
         << " */\n"
         << "static int getInt(const std::string&,const std::string&);\n";
    }
    if (up2) {
      os << "/*!\n"
         << " * \\brief convert a string to unsigned short\n"
         << " * \\param[in] p : parameter\n"
         << " * \\param[in] v : value\n"
         << " */\n"
         << "static unsigned short getUnsignedShort(const std::string&,const "
            "std::string&);\n";
    }
    os << "private :\n\n"
       << cname << "();\n\n"
       << cname << "(const " << cname << "&);\n\n"
       << cname << "&\n"
       << "operator=(const " << cname << "&);\n";
    if (allowsParametersInitializationFromFile(this->mb)) {
      os << "/*!\n"
         << " * \\brief read the parameters from the given file\n"
         << " * \\param[out] pi : parameters initializer\n"
         << " * \\param[in]  fn : file name\n"
         << " */\n"
         << "static void readParameters(" << cname << "&,const char* const);\n";
    }
    os << "};\n\n";
  }  // end of writeBehaviourParametersInitializer

  void BehaviourDSLCommon::writeBehaviourParserSpecificInitializeMethodPart(
      std::ostream&, const Hypothesis) const {
    // Empty member meant to be overriden in Child if necessary
  }

  void BehaviourDSLCommon::writeBehaviourFileBegin(std::ostream& os) const {
    this->checkBehaviourFile(os);
    this->writeBehaviourFileHeader(os);
    this->writeBehaviourFileHeaderBegin(os);
    this->writeBehaviourIncludes(os);
    this->writeBehaviourParserSpecificIncludes(os);
    this->writeIncludes(os);
    // includes specific to interfaces
    for (const auto& i : this->interfaces) {
      i.second->writeInterfaceSpecificIncludes(os, this->mb);
    }
    this->writeNamespaceBegin(os);
    this->writeBehaviourParametersInitializers(os);
    this->writeBehaviourForwardDeclarations(os);
    this->writeBehaviourProfiler(os);
  }  // end of writeBehaviourFileBegin

  void BehaviourDSLCommon::writeBehaviourProfiler(std::ostream& os) const {
    if (this->mb.getAttribute(BehaviourData::profiling, false)) {
      this->checkBehaviourFile(os);
      os << "/*!\n"
         << " * " << this->mb.getClassName() << " profiler\n"
         << " */\n"
         << "struct " << this->mb.getClassName() << "Profiler\n"
         << "{\n"
         << "//! return the profiler associated with the behaviour\n"
         << "static mfront::BehaviourProfiler& getProfiler();\n"
         << "}; // end of struct " << this->mb.getClassName() << "Profiler\n\n";
    }
  }  // end of writeBehaviourProfiler

  void BehaviourDSLCommon::writeBehaviourClass(std::ostream& os,
                                               const Hypothesis h) const {
    this->checkBehaviourFile(os);
    this->writeBehaviourClassBegin(os, h);
    this->writeBehaviourFriends(os, h);
    this->writeBehaviourTypeAliases(os);
    os << "private :\n\n";
    this->writeBehaviourParserSpecificTypedefs(os);
    this->writeBehaviourStaticVariables(os, h);
    this->writeBehaviourIntegrationVariables(os, h);
    this->writeBehaviourIntegrationVariablesIncrements(os, h);
    this->writeBehaviourLocalVariables(os, h);
    this->writeBehaviourParameters(os, h);
    this->writeBehaviourTangentOperator(os);
    this->writeBehaviourParserSpecificMembers(os, h);
    this->writeBehaviourUpdateIntegrationVariables(os, h);
    this->writeBehaviourUpdateStateVariables(os, h);
    this->writeBehaviourUpdateAuxiliaryStateVariables(os, h);
    this->writeBehaviourAdditionalMembers(os, h);
    this->writeBehaviourPrivate(os, h);
    this->writeBehaviourDisabledConstructors(os);
    // from this point, all is public
    os << "public:\n\n";
    this->writeBehaviourConstructors(os, h);
    this->writeBehaviourInitializeFunctions(os, h);
    this->writeBehaviourComputeStressFreeExpansion(os, h);
    this->writeBehaviourInitializeMethods(os, h);
    this->writeBehaviourSetOutOfBoundsPolicy(os);
    this->writeBehaviourGetModellingHypothesis(os);
    this->writeBehaviourCheckBounds(os, h);
    this->writeBehaviourComputePredictionOperator(os, h);
    this->writeBehaviourGetTimeStepScalingFactor(os);
    this->writeBehaviourComputeAPrioriTimeStepScalingFactor(os);
    this->writeBehaviourIntegrator(os, h);
    this->writeBehaviourComputeAPosterioriTimeStepScalingFactor(os);
    this->writeBehaviourComputeInternalEnergy(os, h);
    this->writeBehaviourComputeDissipatedEnergy(os, h);
    this->writeBehaviourComputeTangentOperator(os, h);
    this->writeBehaviourComputeSpeedOfSound(os, h);
    this->writeBehaviourGetTangentOperator(os);
    this->writeBehaviourUpdateExternalStateVariables(os, h);
    this->writeBehaviourPostProcessings(os, h);
    this->writeBehaviourDestructor(os);
    this->checkBehaviourFile(os);
    os << "private:\n\n";
    this->writeBehaviourComputeAPrioriTimeStepScalingFactorII(os, h);
    this->writeBehaviourComputeAPosterioriTimeStepScalingFactorII(os, h);
    this->writeBehaviourOutOfBoundsPolicyVariable(os);
    this->writeBehaviourClassEnd(os);
    this->writeBehaviourOutputOperator(os, h);
  }

  void BehaviourDSLCommon::writeBehaviourFileEnd(std::ostream& os) const {
    this->checkBehaviourFile(os);
    this->writeBehaviourTraits(os);
    this->writeNamespaceEnd(os);
    this->writeBehaviourFileHeaderEnd(os);
  }  // end of writeBehaviourFileBegin

  static bool hasUserDefinedPredictionOperatorCode(
      const BehaviourDescription& mb,
      const tfel::material::ModellingHypothesis::Hypothesis h) {
    using tfel::material::getFiniteStrainBehaviourTangentOperatorFlags;
    if (mb.getBehaviourType() ==
        BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) {
      // all available tangent operators for finite strain behaviours
      const auto tos = getFiniteStrainBehaviourTangentOperatorFlags();
      // search tangent operators defined by the user
      for (const auto& t : tos) {
        const auto ktype =
            convertFiniteStrainBehaviourTangentOperatorFlagToString(t);
        if (mb.hasCode(
                h, std::string(BehaviourData::ComputePredictionOperator) + '-' +
                       ktype)) {
          return true;
        }
      }
    } else {
      if (mb.hasCode(h, BehaviourData::ComputePredictionOperator)) {
        return true;
      }
    }
    return false;
  }  // end of hasUserDefinedTangentOperatorCode

  void BehaviourDSLCommon::writeBehaviourComputePredictionOperator(
      std::ostream& os, const Hypothesis h) const {
    using namespace tfel::material;
    const auto btype = this->mb.getBehaviourTypeFlag();
    if ((!this->mb.getAttribute<bool>(h, BehaviourData::hasPredictionOperator,
                                      false)) &&
        (this->mb.hasCode(h, BehaviourData::ComputePredictionOperator))) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::writeBehaviourComputePredictionOperator : ",
          "attribute 'hasPredictionOperator' is set but no associated code "
          "defined");
    }
    if (!hasUserDefinedPredictionOperatorCode(this->mb, h)) {
      os << "IntegrationResult computePredictionOperator(const SMFlag,const "
            "SMType) override{\n"
         << "tfel::raise(\"" << this->mb.getClassName()
         << "::computePredictionOperator: \"\n"
         << "\"unsupported prediction operator flag\");\n"
         << "}\n\n";
      return;
    }
    if (this->mb.getBehaviourType() ==
        BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) {
      // all available tangent operators for finite strain behaviours
      const auto tos(getFiniteStrainBehaviourTangentOperatorFlags());
      // all known converters
      const auto converters = FiniteStrainBehaviourTangentOperatorConversion::
          getAvailableFiniteStrainBehaviourTangentOperatorConversions();
      // tangent operators defined by the user
      std::vector<FiniteStrainBehaviourTangentOperatorBase::Flag> ktos;
      for (const auto& t : tos) {
        const auto ktype =
            convertFiniteStrainBehaviourTangentOperatorFlagToString(t);
        if (this->mb.hasCode(
                h, std::string(BehaviourData::ComputePredictionOperator) + '-' +
                       ktype)) {
          ktos.push_back(t);
        }
      }
      if (!ktos.empty()) {
        // computing all the conversion paths starting from user defined ones
        std::vector<FiniteStrainBehaviourTangentOperatorConversionPath> paths;
        for (const auto& k : ktos) {
          const auto kpaths =
              FiniteStrainBehaviourTangentOperatorConversionPath::
                  getConversionsPath(k, ktos, converters);
          paths.insert(paths.end(), kpaths.begin(), kpaths.end());
        }
        for (const auto& t : tos) {
          const auto ktype =
              convertFiniteStrainBehaviourTangentOperatorFlagToString(t);
          if (std::find(ktos.begin(), ktos.end(), t) != ktos.end()) {
            os << "IntegrationResult\ncomputePredictionOperator_" << ktype
               << "(const SMType smt){\n"
               << "using namespace std;\n"
               << "using namespace tfel::math;\n"
               << "using std::vector;\n";
            writeMaterialLaws(os, this->mb.getMaterialLaws());
            os << this->mb.getCode(
                      h, std::string(BehaviourData::ComputePredictionOperator) +
                             "-" + ktype)
               << '\n'
               << "return SUCCESS;\n"
               << "}\n\n";
          } else {
            if ((h ==
                 ModellingHypothesis::AXISYMMETRICALGENERALISEDPLANESTRESS) ||
                (h == ModellingHypothesis::PLANESTRESS)) {
              os << "IntegrationResult computePredictionOperator_" << ktype
                 << "(const SMType){\n"
                 << "tfel::raise(\"" << this->mb.getClassName()
                 << "::computePredictionOperator_" << ktype << ": \"\n"
                 << "\"computing the prediction operator '" << ktype
                 << "' is not supported\");\n"
                 << "}\n\n";
            } else {
              const auto path =
                  FiniteStrainBehaviourTangentOperatorConversionPath::
                      getShortestPath(paths, t);
              if (path.empty()) {
                os << "IntegrationResult computePredictionOperator_" << ktype
                   << "(const SMType){\n"
                   << "tfel::raise(\"" << this->mb.getClassName()
                   << "::computePredictionOperator_" << ktype << ": \"\n"
                   << "\"computing the prediction operator '" << ktype
                   << "' is not supported\");\n"
                   << "}\n\n";
              } else {
                os << "IntegrationResult computePredictionOperator_" << ktype
                   << "(const SMType smt){\n";
                auto pc = path.begin();
                os << "using namespace tfel::math;\n";
                os << "// computing "
                   << convertFiniteStrainBehaviourTangentOperatorFlagToString(
                          pc->from())
                   << '\n';
                const auto k =
                    convertFiniteStrainBehaviourTangentOperatorFlagToString(
                        pc->from());
                os << "this->computePredictionOperator_" << k << "(smt);\n"
                   << "const "
                   << getFiniteStrainBehaviourTangentOperatorFlagType(
                          pc->from())
                   << "<N,stress>"
                   << " tangentOperator_"
                   << convertFiniteStrainBehaviourTangentOperatorFlagToString(
                          pc->from())
                   << " = this->Dt.template get<"
                   << getFiniteStrainBehaviourTangentOperatorFlagType(
                          pc->from())
                   << "<N,stress> >();\n";
                for (; pc != path.end();) {
                  const auto converter = *pc;
                  if (++pc == path.end()) {
                    os << converter.getFinalConversion() << '\n';
                  } else {
                    os << converter.getIntermediateConversion() << '\n';
                  }
                }
                os << "return SUCCESS;\n"
                   << "}\n\n";
              }
            }
          }
        }
        os << "IntegrationResult computePredictionOperator(const SMFlag "
              "smflag,const SMType smt) override{\n"
           << "using namespace std;\n"
           << "switch(smflag){\n";
        for (const auto& t : tos) {
          const auto ktype =
              convertFiniteStrainBehaviourTangentOperatorFlagToString(t);
          os << "case " << ktype << ":\n"
             << "return this->computePredictionOperator_" << ktype
             << "(smt);\n";
        }
        os << "}\n"
           << "tfel::raise(\"" << this->mb.getClassName()
           << "::computePredictionOperator: \"\n"
           << "\"unsupported prediction operator flag\");\n"
           << "}\n\n";
      }
    } else {
      os << "IntegrationResult\n"
         << "computePredictionOperator(const SMFlag smflag,const SMType smt) "
            "override{\n"
         << "using namespace std;\n"
         << "using namespace tfel::math;\n"
         << "using std::vector;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
      if ((this->mb.getBehaviourType() ==
           BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) ||
          (this->mb.getBehaviourType() ==
           BehaviourDescription::COHESIVEZONEMODEL) ||
          (this->mb.getBehaviourType() ==
           BehaviourDescription::GENERALBEHAVIOUR)) {
        if (mb.useQt()) {
          os << "tfel::raise_if(smflag!=MechanicalBehaviour<" << btype
             << ",hypothesis, NumericType, use_qt>::STANDARDTANGENTOPERATOR,\n"
             << "\"invalid prediction operator flag\");\n";
        } else {
          os << "tfel::raise_if(smflag!=MechanicalBehaviour<" << btype
             << ",hypothesis, NumericType, false>::STANDARDTANGENTOPERATOR,\n"
             << "\"invalid prediction operator flag\");\n";
        }
      }
      os << this->mb.getCode(h, BehaviourData::ComputePredictionOperator)
         << "return SUCCESS;\n"
         << "}\n\n";
    }
  }  // end of writeBehaviourComputePredictionOperator

  void BehaviourDSLCommon::writeBehaviourComputeTangentOperator(
      std::ostream& os, const Hypothesis h) const {
    using namespace tfel::material;
    if (this->mb.getBehaviourType() ==
        BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) {
      // all available tangent operators for finite strain behaviours
      const auto tos(getFiniteStrainBehaviourTangentOperatorFlags());
      // all known converters
      const auto converters = FiniteStrainBehaviourTangentOperatorConversion::
          getAvailableFiniteStrainBehaviourTangentOperatorConversions();
      // tangent operators defined by the user
      std::vector<FiniteStrainBehaviourTangentOperatorBase::Flag> ktos;
      for (const auto& t : tos) {
        const auto ktype =
            convertFiniteStrainBehaviourTangentOperatorFlagToString(t);
        if (this->mb.hasCode(
                h, std::string(BehaviourData::ComputeTangentOperator) + '-' +
                       ktype)) {
          ktos.push_back(t);
        }
      }
      if (!ktos.empty()) {
        // computing all the conversion paths starting from user defined ones
        std::vector<FiniteStrainBehaviourTangentOperatorConversionPath> paths;
        for (const auto& k : ktos) {
          const auto kpaths =
              FiniteStrainBehaviourTangentOperatorConversionPath::
                  getConversionsPath(k, ktos, converters);
          paths.insert(paths.end(), kpaths.begin(), kpaths.end());
        }
        for (const auto& t : tos) {
          const auto ktype =
              convertFiniteStrainBehaviourTangentOperatorFlagToString(t);
          if (find(ktos.begin(), ktos.end(), t) != ktos.end()) {
            os << "bool computeConsistentTangentOperator_" << ktype
               << "(const SMType smt){\n"
               << "using namespace std;\n"
               << "using namespace tfel::math;\n"
               << "using std::vector;\n";
            os << "auto mfront_success = true;\n";
            writeMaterialLaws(os, this->mb.getMaterialLaws());
            this->writeBehaviourComputeTangentOperatorBody(
                os, h,
                std::string(BehaviourData::ComputeTangentOperator) + "-" +
                    ktype);
            os << "return mfront_success;\n"
               << "}\n\n";
          } else {
            if ((h ==
                 ModellingHypothesis::AXISYMMETRICALGENERALISEDPLANESTRESS) ||
                (h == ModellingHypothesis::PLANESTRESS)) {
              os << "bool computeConsistentTangentOperator_" << ktype
                 << "(const SMType){\n"
                 << "tfel::raise(\"" << this->mb.getClassName()
                 << "::computeConsistentTangentOperator_" << ktype << ": \"\n"
                 << "\"computing the tangent operator '" << ktype
                 << "' is not supported\");\n"
                 << "}\n\n";
            } else {
              const auto path =
                  FiniteStrainBehaviourTangentOperatorConversionPath::
                      getShortestPath(paths, t);
              if (path.empty()) {
                os << "bool computeConsistentTangentOperator_" << ktype
                   << "(const SMType){\n"
                   << "tfel::raise(\"" << this->mb.getClassName()
                   << "::computeConsistentTangentOperator_" << ktype << ": \"\n"
                   << "\"computing the tangent operator '" << ktype
                   << "' is not supported\");\n"
                   << "}\n\n";
              } else {
                os << "bool computeConsistentTangentOperator_" << ktype
                   << "(const SMType smt){\n";
                auto pc = path.begin();
                os << "using namespace tfel::math;\n";
                os << "// computing "
                   << convertFiniteStrainBehaviourTangentOperatorFlagToString(
                          pc->from())
                   << '\n';
                const auto k =
                    convertFiniteStrainBehaviourTangentOperatorFlagToString(
                        pc->from());
                os << "this->computeConsistentTangentOperator_" << k
                   << "(smt);\n"
                   << "const "
                   << getFiniteStrainBehaviourTangentOperatorFlagType(
                          pc->from())
                   << "<N,stress>"
                   << " tangentOperator_"
                   << convertFiniteStrainBehaviourTangentOperatorFlagToString(
                          pc->from())
                   << " = this->Dt.template get<"
                   << getFiniteStrainBehaviourTangentOperatorFlagType(
                          pc->from())
                   << "<N,stress> >();\n";
                for (; pc != path.end();) {
                  const auto converter = *pc;
                  if (++pc == path.end()) {
                    os << converter.getFinalConversion() << '\n';
                  } else {
                    os << converter.getIntermediateConversion() << '\n';
                  }
                }
                os << "return true;\n"
                   << "}\n\n";
              }
            }
          }
        }
        os << "bool computeConsistentTangentOperator(const SMFlag smflag,const "
              "SMType smt){\n"
           << "switch(smflag){\n";
        for (const auto& t : tos) {
          const auto ktype =
              convertFiniteStrainBehaviourTangentOperatorFlagToString(t);
          os << "case " << ktype << ":\n"
             << "return this->computeConsistentTangentOperator_" << ktype
             << "(smt);\n";
        }
        os << "}\n"
           << "tfel::raise(\"" << this->mb.getClassName()
           << "::computeConsistentTangentOperator: \"\n"
           << "\"unsupported tangent operator flag\");\n"
           << "}\n\n";
      }
    } else {
      if (this->mb.hasCode(h, BehaviourData::ComputeTangentOperator)) {
        os << "bool computeConsistentTangentOperator(const SMType smt){\n"
           << "using namespace std;\n"
           << "using namespace tfel::math;\n"
           << "using std::vector;\n";
        os << "auto mfront_success = true;\n";
        writeMaterialLaws(os, this->mb.getMaterialLaws());
        this->writeBehaviourComputeTangentOperatorBody(
            os, h, BehaviourData::ComputeTangentOperator);
        os << "return mfront_success;\n"
           << "}\n\n";
      }
    }
  }  // end of writeBehaviourComputeTangentOperator

  void BehaviourDSLCommon::writeBehaviourComputeTangentOperatorBody(
      std::ostream& os, const Hypothesis h, const std::string& n) const {
    os << this->mb.getCode(h, n) << '\n';
  }  // end of writeBehaviourComputeTangentOperatorBody

  void BehaviourDSLCommon::writeBehaviourGetTangentOperator(
      std::ostream& os) const {
    this->checkBehaviourFile(os);
    if (this->mb.hasTangentOperator()) {
      os << "const TangentOperator& getTangentOperator() const{\n"
         << "return this->Dt;\n"
         << "}\n\n";
    }
  }  // end of writeBehaviourComputeTangentOperator()

  void BehaviourDSLCommon::writeBehaviourGetTimeStepScalingFactor(
      std::ostream& os) const {
    this->checkBehaviourFile(os);
    os << "time getMinimalTimeStepScalingFactor() const noexcept override{\n"
          "  return this->minimal_time_step_scaling_factor;\n"
          "}\n\n";
  }

  void BehaviourDSLCommon::writeBehaviourComputeAPrioriTimeStepScalingFactor(
      std::ostream& os) const {
    this->checkBehaviourFile(os);
    os << "std::pair<bool, time>\n"
          "computeAPrioriTimeStepScalingFactor(const time "
          "current_time_step_scaling_factor) const override{\n"
          "const auto time_scaling_factor = "
          "this->computeAPrioriTimeStepScalingFactorII();\n"
          "return {time_scaling_factor.first,\n"
          "        std::min(std::min(std::max(time_scaling_factor.second,\n"
          "                                   "
          "this->minimal_time_step_scaling_factor),\n"
          "                          this->maximal_time_step_scaling_factor),\n"
          "                  current_time_step_scaling_factor)};\n"
          "}\n\n";
  }  // end of
     // writeBehaviourComputeAPrioriTimeStepScalingFactor

  void BehaviourDSLCommon::writeBehaviourComputeAPrioriTimeStepScalingFactorII(
      std::ostream& os, const Hypothesis h) const {
    this->checkBehaviourFile(os);
    os << "std::pair<bool, time> computeAPrioriTimeStepScalingFactorII() "
          "const{\n";
    if (this->mb.hasCode(h, BehaviourData::APrioriTimeStepScalingFactor)) {
      os << "using namespace std;\n"
         << "using namespace tfel::math;\n"
         << "using std::vector;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
      os << this->mb.getCode(h, BehaviourData::APrioriTimeStepScalingFactor)
         << '\n';
    }
    os << "return {true, this->maximal_time_step_scaling_factor};\n"
       << "}\n\n";
  }

  void
  BehaviourDSLCommon::writeBehaviourComputeAPosterioriTimeStepScalingFactor(
      std::ostream& os) const {
    this->checkBehaviourFile(os);
    os << "std::pair<bool, time>\n"
          "computeAPosterioriTimeStepScalingFactor(const time "
          "current_time_step_scaling_factor) const override{\n"
          "const auto time_scaling_factor = "
          "this->computeAPosterioriTimeStepScalingFactorII();\n"
          "return {time_scaling_factor.first,\n"
          "        std::min(std::min(std::max(time_scaling_factor.second,\n"
          "                                   "
          "this->minimal_time_step_scaling_factor),\n"
          "                          this->maximal_time_step_scaling_factor),\n"
          "                 current_time_step_scaling_factor)};\n"
          "}\n\n";
  }  // end of
     // writeBehaviourComputeAPosterioriTimeStepScalingFactor

  void
  BehaviourDSLCommon::writeBehaviourComputeAPosterioriTimeStepScalingFactorII(
      std::ostream& os, const Hypothesis h) const {
    this->checkBehaviourFile(os);
    os << "std::pair<bool, time> computeAPosterioriTimeStepScalingFactorII() "
          "const{\n";
    if (this->mb.hasCode(h, BehaviourData::APosterioriTimeStepScalingFactor)) {
      os << "using namespace std;\n"
         << "using namespace tfel::math;\n"
         << "using std::vector;\n";
      writeMaterialLaws(os, this->mb.getMaterialLaws());
      os << this->mb.getCode(h, BehaviourData::APosterioriTimeStepScalingFactor)
         << '\n';
    }
    os << "return {true,this->maximal_time_step_scaling_factor};\n"
       << "}\n\n";
  }  // end of
     // writeBehaviourComputeAPosterioriTimeStepScalingFactor

  void BehaviourDSLCommon::writeBehaviourTangentOperator(
      std::ostream& os) const {
    this->checkBehaviourFile(os);
    if (!this->mb.hasTangentOperator()) {
      return;
    }
    const auto& blocks = this->mb.getTangentOperatorBlocks();
    os << "//! Tangent operator;\n"
       << "TangentOperator Dt;\n";
    if (this->mb.hasTrivialTangentOperatorStructure()) {
      tfel::raise_if(
          ((blocks.size() != 1u) || (blocks.front().first.arraySize != 1u) ||
           (blocks.front().second.arraySize != 1u)),
          "BehaviourDSLCommon::writeBehaviourTangentOperator: internal error");
      if (this->mb.getBehaviourType() !=
          BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) {
        os << "//! alias to the tangent operator;\n"
           << "TangentOperator& "
           << this->mb.getTangentOperatorBlockName(blocks.front()) << ";\n";
      }
      return;
    }
    // write blocks
    for (const auto& b : blocks) {
      const auto& v1 = b.first;
      const auto& v2 = b.second;
      if ((v1.arraySize != 1u) || (v2.arraySize != 1u)) {
        break;
      }
      const auto bn = this->mb.getTangentOperatorBlockName(b);
      if ((v1.getTypeFlag() == SupportedTypes::SCALAR) &&
          (v2.getTypeFlag() == SupportedTypes::SCALAR)) {
        os << "real& " << bn << ";\n";
      } else {
        os << "tfel::math::View<tfel::math::derivative_type<" << v1.type << ","
           << v2.type << ">> " << bn << ";\n";
      }
    }
  }  // end of writeBehaviourTangentOperator()

  void BehaviourDSLCommon::checkIntegrationDataFile(std::ostream& os) const {
    if ((!os) || (!os.good())) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::checkIntegrationDataOutputFile",
          "output file is not valid");
    }
  }

  void BehaviourDSLCommon::writeIntegrationDataFileHeader(
      std::ostream& os) const {
    this->checkIntegrationDataFile(os);
    os << "/*!\n";
    os << "* \\file   " << this->getIntegrationDataFileName() << '\n';
    os << "* \\brief  "
       << "this file implements the " << this->mb.getClassName()
       << "IntegrationData"
       << " class.\n";
    os << "*         File generated by ";
    os << MFrontHeader::getVersionName() << " ";
    os << "version " << MFrontHeader::getVersionNumber();
    os << '\n';
    if (!this->fd.authorName.empty()) {
      os << "* \\author " << this->fd.authorName << '\n';
    }
    if (!this->fd.date.empty()) {
      os << "* \\date   " << this->fd.date << '\n';
    }
    os << " */\n\n";
  }

  void BehaviourDSLCommon::writeIntegrationDataFileHeaderBegin(
      std::ostream& os) const {
    this->checkIntegrationDataFile(os);
    os << "#ifndef LIB_TFELMATERIAL_" << makeUpperCase(this->mb.getClassName())
       << "_INTEGRATION_DATA_HXX\n"
       << "#define LIB_TFELMATERIAL_" << makeUpperCase(this->mb.getClassName())
       << "_INTEGRATION_DATA_HXX\n\n";
  }

  void BehaviourDSLCommon::writeIntegrationDataFileHeaderEnd(
      std::ostream& os) const {
    this->checkIntegrationDataFile(os);
    os << "#endif /* LIB_TFELMATERIAL_"
       << makeUpperCase(this->mb.getClassName())
       << "_INTEGRATION_DATA_HXX */\n";
  }

  void BehaviourDSLCommon::writeIntegrationDataStandardTFELIncludes(
      std::ostream& os) const {
    bool b1 = false;
    bool b2 = false;
    this->checkIntegrationDataFile(os);
    os << "#include<string>\n"
       << "#include<iostream>\n"
       << "#include<limits>\n"
       << "#include<stdexcept>\n"
       << "#include<algorithm>\n\n"
       << "#include\"TFEL/Raise.hxx\"\n"
       << "#include\"TFEL/PhysicalConstants.hxx\"\n"
       << "#include\"TFEL/Config/TFELConfig.hxx\"\n"
       << "#include\"TFEL/Config/TFELTypes.hxx\"\n"
       << "#include\"TFEL/TypeTraits/IsFundamentalNumericType.hxx\"\n"
       << "#include\"TFEL/TypeTraits/IsScalar.hxx\"\n"
       << "#include\"TFEL/TypeTraits/IsReal.hxx\"\n"
       << "#include\"TFEL/TypeTraits/Promote.hxx\"\n"
       << "#include\"TFEL/Math/General/IEEE754.hxx\"\n";
    this->mb.requiresTVectorOrVectorIncludes(b1, b2);
    if (b1) {
      os << "#include\"TFEL/Math/tvector.hxx\"\n"
         << "#include\"TFEL/Math/Vector/tvectorIO.hxx\"\n";
    }
    if (b2) {
      os << "#include\"TFEL/Math/vector.hxx\"\n";
    }
    os << "#include\"TFEL/Math/stensor.hxx\"\n"
       << "#include\"TFEL/Math/st2tost2.hxx\"\n";
    if (this->mb.getBehaviourType() ==
        BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) {
      os << "#include\"TFEL/Math/tensor.hxx\"\n"
         << "#include\"TFEL/Math/t2tot2.hxx\"\n"
         << "#include\"TFEL/Math/t2tost2.hxx\"\n"
         << "#include\"TFEL/Math/st2tot2.hxx\"\n";
    }
  }

  void BehaviourDSLCommon::writeIntegrationDataDefaultMembers(
      std::ostream& os) const {
    this->checkIntegrationDataFile(os);
    os << "protected: \n\n";
    for (const auto& v : this->mb.getMainVariables()) {
      if (Gradient::isIncrementKnown(v.first)) {
        os << "/*!\n"
           << " * \\brief " << v.first.name << " increment\n"
           << " */\n"
           << v.first.type << " d" << v.first.name << ";\n\n";
      } else {
        os << "/*!\n"
           << " * \\brief " << v.first.name << " at the end of the time step\n"
           << " */\n"
           << v.first.type << " " << v.first.name << "1;\n\n";
      }
    }
    os << "/*!\n"
       << " * \\brief time increment\n"
       << " */\n"
       << "time dt;\n\n";
  }

  void BehaviourDSLCommon::writeIntegrationDataTypeAliases(
      std::ostream& os) const {
    this->checkIntegrationDataFile(os);
    os << "static constexpr unsigned short TVectorSize = N;\n"
       << "typedef tfel::math::StensorDimeToSize<N> StensorDimeToSize;\n"
       << "static constexpr unsigned short StensorSize = "
       << "StensorDimeToSize::value;\n"
       << "typedef tfel::math::TensorDimeToSize<N> TensorDimeToSize;\n"
       << "static constexpr unsigned short TensorSize = "
       << "TensorDimeToSize::value;\n\n";
    this->writeTypeAliases(os);
    os << '\n';
  }

  void BehaviourDSLCommon::writeIntegrationDataDisabledConstructors(
      std::ostream& os) const {
    this->checkIntegrationDataFile(os);
  }

  void BehaviourDSLCommon::writeIntegrationDataConstructors(
      std::ostream& os, const Hypothesis h) const {
    const auto& md = this->mb.getBehaviourData(h);
    this->checkIntegrationDataFile(os);
    os << "/*!\n"
       << "* \\brief Default constructor\n"
       << "*/\n"
       << this->mb.getClassName() << "IntegrationData()\n"
       << "{}\n\n"
       << "/*!\n"
       << "* \\brief Copy constructor\n"
       << "*/\n"
       << this->mb.getClassName() << "IntegrationData(const "
       << this->mb.getClassName() << "IntegrationData& src)\n"
       << ": ";
    for (const auto& v : this->mb.getMainVariables()) {
      if (Gradient::isIncrementKnown(v.first)) {
        os << "d" << v.first.name << "(src.d" << v.first.name << "),\n";
      } else {
        os << v.first.name << "1(src." << v.first.name << "1),\n";
      }
    }
    os << "dt(src.dt)";
    for (const auto& v : md.getExternalStateVariables()) {
      os << ",\nd" << v.name << "(src.d" << v.name << ")";
    }
    os << "\n{}\n\n";
    // Creating constructor for external interfaces
    for (const auto& i : this->interfaces) {
      if (i.second->isBehaviourConstructorRequired(h, this->mb)) {
        i.second->writeIntegrationDataConstructor(os, h, this->mb);
      }
    }
  }

  void BehaviourDSLCommon::writeIntegrationDataScaleOperators(
      std::ostream& os, const Hypothesis h) const {
    const auto& md = this->mb.getBehaviourData(h);
    bool iknown = true;
    for (const auto& v : this->mb.getMainVariables()) {
      iknown = Gradient::isIncrementKnown(v.first);
    }
    this->checkIntegrationDataFile(os);
    os << "/*\n"
       << "* \\brief scale the integration data by a scalar.\n"
       << "*/\n"
       << "template<typename Scal>\n"
       << "typename std::enable_if<\n"
       << "tfel::typetraits::IsFundamentalNumericType<Scal>::cond&&\n"
       << "tfel::typetraits::IsScalar<Scal>::cond&&\n"
       << "tfel::typetraits::IsReal<Scal>::cond&&\n"
       << "std::is_same<NumericType,"
       << "typename tfel::typetraits::Promote"
       << "<NumericType,Scal>::type>::value,\n"
       << this->mb.getClassName() << "IntegrationData&\n"
       << ">::type\n";
    if (!iknown) {
      if (this->mb.useQt()) {
        os << "scale(const " << this->mb.getClassName()
           << "BehaviourData<hypothesis, NumericType, use_qt>& behaviourData, "
              "const "
              "Scal time_scaling_factor){\n";
      } else {
        os << "scale(const " << this->mb.getClassName()
           << "BehaviourData<hypothesis, NumericType, false>& behaviourData, "
              "const Scal "
              "time_scaling_factor){\n";
      }
    } else {
      if (this->mb.useQt()) {
        os << "scale(const " << this->mb.getClassName()
           << "BehaviourData<hypothesis, NumericType, use_qt>&, const Scal "
              "time_scaling_factor){\n";
      } else {
        os << "scale(const " << this->mb.getClassName()
           << "BehaviourData<hypothesis, NumericType, false>&, const Scal "
              "time_scaling_factor){\n";
      }
    }
    os << "this->dt   *= time_scaling_factor;\n";
    for (const auto& v : this->mb.getMainVariables()) {
      if (Gradient::isIncrementKnown(v.first)) {
        os << "this->d" << v.first.name << " *= time_scaling_factor;\n";
      } else {
        os << "this->" << v.first.name
           << "1 = (1-time_scaling_factor)*(behaviourData." << v.first.name
           << "0)+time_scaling_factor*(this->" << v.first.name << "1);\n";
      }
    }
    for (const auto& v : md.getExternalStateVariables()) {
      os << "this->d" << v.name << " *= time_scaling_factor;\n";
    }
    os << "return *this;\n"
       << "}\n\n";
  }  // end of writeIntegrationDataScaleOpeartors

  void BehaviourDSLCommon::writeIntegrationDataUpdateDrivingVariablesMethod(
      std::ostream& os) const {
    bool iknown = true;
    for (const auto& v : this->mb.getMainVariables()) {
      iknown = Gradient::isIncrementKnown(v.first);
    }
    this->checkIntegrationDataFile(os);
    os << "/*!\n"
       << "* \\brief update the driving variable in case of substepping.\n"
       << "*/\n"
       << this->mb.getClassName() << "IntegrationData&\n";
    if (!iknown) {
      if (this->mb.useQt()) {
        os << "updateDrivingVariables(const " << this->mb.getClassName()
           << "BehaviourData<hypothesis, NumericType, use_qt>& "
              "behaviourData){\n";
      } else {
        os << "updateDrivingVariables(const " << this->mb.getClassName()
           << "BehaviourData<hypothesis, NumericType, false>& "
              "behaviourData){\n";
      }
    } else {
      if (this->mb.useQt()) {
        os << "updateDrivingVariables(const " << this->mb.getClassName()
           << "BehaviourData<hypothesis, NumericType, use_qt>&){\n";
      } else {
        os << "updateDrivingVariables(const " << this->mb.getClassName()
           << "BehaviourData<hypothesis, NumericType, false>&){\n";
      }
    }
    for (const auto& v : this->mb.getMainVariables()) {
      if (!Gradient::isIncrementKnown(v.first)) {
        os << "this->" << v.first.name << "1 += "
           << "this->" << v.first.name << "1 - (behaviourData." << v.first.name
           << "0);\n";
      }
    }
    os << "return *this;\n"
       << "}\n\n";
  }  // end of writeIntegrationUpdateDrivingVariablesMethod

  void BehaviourDSLCommon::writeIntegrationDataClassHeader(
      std::ostream& os) const {
    this->checkIntegrationDataFile(os);
    os << "/*!\n"
       << "* \\class " << this->mb.getClassName() << "IntegrationData\n"
       << "* \\brief This class implements the " << this->mb.getClassName()
       << "IntegrationData"
       << " behaviour.\n"
       << "* \\tparam N: space dimension.\n"
       << "* \\tparam NumericType: numerical type.\n"
       << "* \\tparam use_qt: conditional saying if quantities are use.\n";
    if (!this->fd.authorName.empty()) {
      os << "* \\author " << this->fd.authorName << '\n';
    }
    if (!this->fd.date.empty()) {
      os << "* \\date   " << this->fd.date << '\n';
    }
    os << "*/\n";
  }

  void BehaviourDSLCommon::writeIntegrationDataForwardDeclarations(
      std::ostream& os) const {
    this->checkIntegrationDataFile(os);
    os << "//! \\brief forward declaration\n"
       << "template<ModellingHypothesis::Hypothesis hypothesis, "
       << "typename NumericType, bool use_qt>\n"
       << "class " << this->mb.getClassName() << "IntegrationData;\n\n";
    if (this->mb.useQt()) {
      os << "//! \\brief forward declaration\n"
         << "template<ModellingHypothesis::Hypothesis hypothesis, "
         << "typename NumericType, bool use_qt>\n"
         << "std::ostream&\n operator <<(std::ostream&,"
         << "const " << this->mb.getClassName()
         << "IntegrationData<hypothesis, NumericType, use_qt>&);\n\n";
    } else {
      os << "//! \\brief forward declaration\n"
         << "template<ModellingHypothesis::Hypothesis hypothesis, "
         << "typename NumericType>\n"
         << "std::ostream&\n operator <<(std::ostream&,"
         << "const " << this->mb.getClassName()
         << "IntegrationData<hypothesis, NumericType, false>&);\n\n";
    }
    // maintenant, il faut déclarer toutes les spécialisations partielles...
    const auto& mh = this->mb.getModellingHypotheses();
    for (const auto& h : mh) {
      if (this->mb.hasSpecialisedMechanicalData(h)) {
        if (this->mb.useQt()) {
          os << "//! \\brief forward declaration\n"
             << "template<typename NumericType,bool use_qt>\n"
             << "std::ostream&\n operator <<(std::ostream&,"
             << "const " << this->mb.getClassName()
             << "IntegrationData<ModellingHypothesis::"
             << ModellingHypothesis::toUpperCaseString(h)
             << ", NumericType, use_qt>&);\n\n";
        } else {
          os << "//! \\brief forward declaration\n"
             << "template<typename NumericType>\n"
             << "std::ostream&\n operator <<(std::ostream&,"
             << "const " << this->mb.getClassName()
             << "IntegrationData<ModellingHypothesis::"
             << ModellingHypothesis::toUpperCaseString(h)
             << ", NumericType, false>&);\n\n";
        }
      }
    }
  }

  void BehaviourDSLCommon::writeIntegrationDataClassBegin(
      std::ostream& os, const Hypothesis h) const {
    this->checkIntegrationDataFile(os);
    if (h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
      if (this->mb.useQt()) {
        os << "template<ModellingHypothesis::Hypothesis hypothesis, "
           << "typename NumericType, bool use_qt>\n";
        os << "class " << this->mb.getClassName() << "IntegrationData\n";
      } else {
        os << "template<ModellingHypothesis::Hypothesis hypothesis, "
           << "typename NumericType>\n";
        os << "class " << this->mb.getClassName()
           << "IntegrationData<hypothesis, NumericType, false>\n";
      }
    } else {
      if (this->mb.useQt()) {
        os << "template<typename NumericType,bool use_qt>\n";
        os << "class " << this->mb.getClassName()
           << "IntegrationData<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, use_qt>\n";
      } else {
        os << "template<typename NumericType>\n";
        os << "class " << this->mb.getClassName()
           << "IntegrationData<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, false>\n";
      }
    }
    os << "{\n\n";
    if (h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
      os << "static constexpr ModellingHypothesis::Hypothesis hypothesis = "
         << "ModellingHypothesis::" << ModellingHypothesis::toUpperCaseString(h)
         << ";\n";
    }
    os << "static constexpr unsigned short N = "
          "ModellingHypothesisToSpaceDimension<hypothesis>::value;\n";
    os << "static_assert(N==1||N==2||N==3);\n";
    os << "static_assert(tfel::typetraits::"
       << "IsFundamentalNumericType<NumericType>::cond);\n";
    os << "static_assert(tfel::typetraits::IsReal<NumericType>::cond);\n\n";
    os << "friend std::ostream& operator<< <>(std::ostream&,const ";
    os << this->mb.getClassName() << "IntegrationData&);\n\n";
  }

  void BehaviourDSLCommon::writeIntegrationDataOutputOperator(
      std::ostream& os, const Hypothesis h) const {
    const auto& md = this->mb.getBehaviourData(h);
    this->checkBehaviourFile(os);
    if (h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
      if (this->mb.useQt()) {
        os << "template<ModellingHypothesis::Hypothesis hypothesis, "
           << "typename NumericType,bool use_qt>\n"
           << "std::ostream&\n"
           << "operator <<(std::ostream& os,"
           << "const " << this->mb.getClassName()
           << "IntegrationData<hypothesis, NumericType, use_qt>& b)\n";
      } else {
        os << "template<ModellingHypothesis::Hypothesis hypothesis, "
           << "typename NumericType>\n"
           << "std::ostream&\n"
           << "operator <<(std::ostream& os,"
           << "const " << this->mb.getClassName()
           << "IntegrationData<hypothesis, NumericType, false>& b)\n";
      }
    } else {
      if (this->mb.useQt()) {
        os << "template<typename NumericType,bool use_qt>\n"
           << "std::ostream&\n"
           << "operator <<(std::ostream& os,"
           << "const " << this->mb.getClassName()
           << "IntegrationData<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, use_qt>& b)\n";
      } else {
        os << "template<typename NumericType>\n"
           << "std::ostream&\n"
           << "operator <<(std::ostream& os,"
           << "const " << this->mb.getClassName()
           << "IntegrationData<ModellingHypothesis::"
           << ModellingHypothesis::toUpperCaseString(h)
           << ", NumericType, false>& b)\n";
      }
    }
    os << "{\n";
    for (const auto& v : this->mb.getMainVariables()) {
      if (Gradient::isIncrementKnown(v.first)) {
        if (getUnicodeOutputOption()) {
          os << "os << \"\u0394" << displayName(v.first) << " : \" << b.d"
             << v.first.name << " << '\\n';\n";
        } else {
          os << "os << \"d" << displayName(v.first) << " : \" << b.d"
             << v.first.name << " << '\\n';\n";
        }
      } else {
        if (getUnicodeOutputOption()) {
          os << "os << \"" << displayName(v.first) << "\u2081 : \" << b."
             << v.first.name << "1 << '\\n';\n";
        } else {
          os << "os << \"" << displayName(v.first) << "1 : \" << b."
             << v.first.name << "1 << '\\n';\n";
        }
      }
      os << "os << \"" << displayName(v.second) << " : \" << b."
         << v.second.name << " << '\\n';\n";
    }
    if (getUnicodeOutputOption()) {
      os << "os << \"\u0394t : \" << b.dt << '\\n';\n";
    } else {
      os << "os << \"dt : \" << b.dt << '\\n';\n";
    }
    for (const auto& ev : md.getExternalStateVariables()) {
      if (getUnicodeOutputOption()) {
        os << "os << \"\u0394" << displayName(ev) << " : \" << b.d" << ev.name
           << " << '\\n';\n";
      } else {
        os << "os << \"d" << displayName(ev) << " : \" << b.d" << ev.name
           << " << '\\n';\n";
      }
    }
    os << "return os;\n";
    os << "}\n\n";
  }  // end of writeIntegrationDataOutputOperator

  void BehaviourDSLCommon::writeIntegrationDataClassEnd(
      std::ostream& os) const {
    this->checkIntegrationDataFile(os);
    os << "}; // end of " << this->mb.getClassName() << "IntegrationData"
       << "class\n\n";
  }

  void BehaviourDSLCommon::writeIntegrationDataExternalStateVariables(
      std::ostream& os, const Hypothesis h) const {
    const auto& md = this->mb.getBehaviourData(h);
    this->checkIntegrationDataFile(os);
    this->writeVariablesDeclarations(os, md.getExternalStateVariables(), "d",
                                     "", this->fd.fileName, false);
  }

  void BehaviourDSLCommon::writeIntegrationDataFileBegin(
      std::ostream& os) const {
    this->checkIntegrationDataFile(os);
    this->writeIntegrationDataFileHeader(os);
    this->writeIntegrationDataFileHeaderBegin(os);
    this->writeIntegrationDataStandardTFELIncludes(os);
    this->writeIncludes(os);
    // includes specific to interfaces
    for (const auto& i : this->interfaces) {
      i.second->writeInterfaceSpecificIncludes(os, this->mb);
    }
    this->writeNamespaceBegin(os);
    this->writeIntegrationDataForwardDeclarations(os);
  }  // end of writeIntegrationDataFile

  void BehaviourDSLCommon::writeIntegrationDataClass(std::ostream& os,
                                                     const Hypothesis h) const {
    this->checkIntegrationDataFile(os);
    this->writeIntegrationDataClassBegin(os, h);
    this->writeIntegrationDataTypeAliases(os);
    this->writeIntegrationDataDefaultMembers(os);
    this->writeIntegrationDataExternalStateVariables(os, h);
    this->writeIntegrationDataDisabledConstructors(os);
    os << "public:\n\n";
    this->writeIntegrationDataConstructors(os, h);
    this->writeIntegrationDataMainVariablesSetters(os);
    this->writeIntegrationDataScaleOperators(os, h);
    this->writeIntegrationDataUpdateDrivingVariablesMethod(os);
    this->writeIntegrationDataClassEnd(os);
    this->writeIntegrationDataOutputOperator(os, h);
  }

  void BehaviourDSLCommon::writeIntegrationDataFileEnd(std::ostream& os) const {
    this->checkIntegrationDataFile(os);
    this->writeNamespaceEnd(os);
    this->writeIntegrationDataFileHeaderEnd(os);
  }  // end of writeIntegrationDataFileEnd

  void BehaviourDSLCommon::checkSrcFile(std::ostream& os) const {
    if ((!os) || (!os.good())) {
      this->throwRuntimeError("BehaviourDSLCommon::checkSrcFile",
                              "output file is not valid");
    }
  }

  void BehaviourDSLCommon::writeSrcFileHeader(std::ostream& os) const {
    this->checkSrcFile(os);
    os << "/*!\n"
       << "* \\file   " << this->getSrcFileName() << '\n'
       << "* \\brief  "
       << "this file implements the " << this->mb.getClassName()
       << " Behaviour.\n"
       << "*         File generated by " << MFrontHeader::getVersionName()
       << " "
       << "version " << MFrontHeader::getVersionNumber() << '\n';
    if (!this->fd.authorName.empty()) {
      os << "* \\author " << this->fd.authorName << '\n';
    }
    if (!this->fd.date.empty()) {
      os << "* \\date   " << this->fd.date << '\n';
    }
    os << " */\n\n";
    if (this->mb.hasParameters()) {
      os << "#include<string>\n"
         << "#include<cstring>\n"
         << "#include<sstream>\n"
         << "#include<fstream>\n"
         << "#include<stdexcept>\n\n";
    }
    os << "#include\"TFEL/Raise.hxx\"\n"
       << "#include\"" << this->getBehaviourDataFileName() << "\"\n"
       << "#include\"" << this->getIntegrationDataFileName() << "\"\n"
       << "#include\"" << this->getBehaviourFileName() << "\"\n\n";
  }  // end of writeSrcFileHeader()

  void BehaviourDSLCommon::writeSrcFileUserDefinedCode(std::ostream& os) const {
    this->checkSrcFile(os);
    const auto& s = this->mb.getSources();
    if (!s.empty()) {
      os << s << "\n\n";
    }
  }  // end of writeSrcFileUserDefinedCode

  void BehaviourDSLCommon::writeSrcFileParametersInitializers(
      std::ostream& os) const {
    // useless and paranoid test
    if ((areParametersTreatedAsStaticVariables(this->mb)) ||
        (!this->mb.hasParameters())) {
      return;
    }
    auto hs = this->mb.getDistinctModellingHypotheses();
    hs.insert(ModellingHypothesis::UNDEFINEDHYPOTHESIS);
    for (const auto& h : hs) {
      if (this->mb.hasParameters(h)) {
        this->writeSrcFileParametersInitializer(os, h);
      }
    }
  }  // end of writeSrcFileParametersInitializer

  static void BehaviourDSLCommon_writeConverter(std::ostream& f,
                                                const std::string& cname,
                                                const std::string& type,
                                                const std::string& type2) {
    f << type << '\n'
      << cname << "::get" << type2 << "(const std::string& n,\n"
      << "const std::string& v)\n"
      << "{\n"
      << type << " value;\n"
      << "std::istringstream converter(v);\n"
      << "converter >> value;\n"
      << "tfel::raise_if(!converter||(!converter.eof()),\n"
      << "\"" << cname << "::get" << type2 << ": \"\n"
      << "\"can't convert '\"+v+\"' to " << type << " "
      << "for parameter '\"+ n+\"'\");\n"
      << "return value;\n"
      << "}\n\n";
  }

  void BehaviourDSLCommon::writeSrcFileParametersInitializer(
      std::ostream& os, const Hypothesis h) const {
    // useless and paranoid test
    if ((areParametersTreatedAsStaticVariables(this->mb)) ||
        (!this->mb.hasParameters())) {
      return;
    }
    this->checkBehaviourFile(os);
    // treating the default case
    bool rp = false;   // real    parameter found
    bool ip = false;   // integer parameter found
    bool up = false;   // unsigned short parameter found
    bool rp2 = false;  // real    parameter found
    bool ip2 = false;  // integer parameter found
    bool up2 = false;  // unsigned short parameter found
    std::string cname(this->mb.getClassName());
    if (h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
      cname += ModellingHypothesis::toString(h);
    }
    cname += "ParametersInitializer";
    std::string dcname(this->mb.getClassName() + "ParametersInitializer");
    os << cname << "&\n"
       << cname << "::get()\n"
       << "{\n"
       << "static " << cname << " i;\n"
       << "return i;\n"
       << "}\n\n";
    os << cname << "::" << cname << "()\n"
       << "{\n";
    for (const auto& p : this->mb.getBehaviourData(h).getParameters()) {
      if (p.type == "int") {
        ip = true;
        if ((h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) ||
            ((h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) &&
             (!this->mb.hasParameter(ModellingHypothesis::UNDEFINEDHYPOTHESIS,
                                     p.name)))) {
          ip2 = true;
          os << "this->" << p.name << " = "
             << this->mb.getIntegerParameterDefaultValue(h, p.name) << ";\n";
        }
      } else if (p.type == "ushort") {
        up = true;
        if ((h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) ||
            ((h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) &&
             (!this->mb.hasParameter(ModellingHypothesis::UNDEFINEDHYPOTHESIS,
                                     p.name)))) {
          up2 = true;
          os << "this->" << p.name << " = "
             << this->mb.getUnsignedShortParameterDefaultValue(h, p.name)
             << ";\n";
        }
      } else {
        const auto f = SupportedTypes::getTypeFlag(p.type);
        if (f != SupportedTypes::SCALAR) {
          this->throwRuntimeError(
              "BehaviourDSLCommon::writeSrcFileParametersInitializer",
              "unsupported parameter type '" + p.type +
                  "' "
                  "for parameter '" +
                  p.name + "'");
        }
        rp = true;
        if ((h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) ||
            ((h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) &&
             (!this->mb.hasParameter(ModellingHypothesis::UNDEFINEDHYPOTHESIS,
                                     p.name)))) {
          rp2 = true;
          if (p.arraySize == 1u) {
            os << "this->" << p.name << " = "
               << this->mb.getFloattingPointParameterDefaultValue(h, p.name)
               << ";\n";
          } else {
            for (unsigned short i = 0; i != p.arraySize; ++i) {
              os << "this->" << p.name << "[" << i << "] = "
                 << this->mb.getFloattingPointParameterDefaultValue(h, p.name,
                                                                    i)
                 << ";\n";
            }
          }
        }
      }
    }
    if (allowsParametersInitializationFromFile(this->mb)) {
      os << "// Reading parameters from a file\n";
      os << cname << "::readParameters(*this,\""
         << getParametersFileName(this->mb) << "\");\n";
      if (h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
        os << cname << "::readParameters(*this,\""
           << getParametersFileName(this->mb, h) << "\");\n";
      }
    }
    os << "}\n\n";
    auto write_if = [&os](bool& b) {
      if (b) {
        os << "if(";
        b = false;
      } else {
        os << "} else if(";
      }
    };
    if (rp) {
      os << "void\n"
         << cname << "::set(const char* const key,\nconst double v)"
         << "{\n"
         << "using namespace std;\n";
      bool first = true;
      for (const auto& p : this->mb.getBehaviourData(h).getParameters()) {
        if ((p.type == "int") || (p.type == "ushort")) {
          continue;
        }
        const auto f = SupportedTypes::getTypeFlag(p.type);
        if (f != SupportedTypes::SCALAR) {
          this->throwRuntimeError(
              "BehaviourDSLCommon::writeSrcFileParametersInitializer",
              "unsupported parameter type '" + p.type +
                  "' "
                  "for parameter '" +
                  p.name + "'");
        }
        if (p.arraySize == 1u) {
          write_if(first);
          os << "::strcmp(\"" + this->mb.getExternalName(h, p.name) +
                    "\",key)==0){\n";
          if ((h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) ||
              ((h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) &&
               (!this->mb.hasParameter(ModellingHypothesis::UNDEFINEDHYPOTHESIS,
                                       p.name)))) {
            os << "this->" << p.name << " = v;\n";
          } else {
            os << dcname << "::get().set(\""
               << this->mb.getExternalName(h, p.name) << "\",v);\n";
          }
        } else {
          for (unsigned short i = 0; i != p.arraySize; ++i) {
            write_if(first);
            const auto vn = p.name + '[' + std::to_string(i) + ']';
            const auto en = this->mb.getExternalName(h, p.name) + '[' +
                            std::to_string(i) + ']';
            os << "::strcmp(\"" + en + "\",key)==0){\n";
            if ((h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) ||
                ((h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) &&
                 (!this->mb.hasParameter(
                     ModellingHypothesis::UNDEFINEDHYPOTHESIS, p.name)))) {
              os << "this->" << vn << " = v;\n";
            } else {
              os << dcname << "::get().set(\"" << en << "\",v);\n";
            }
          }
        }
      }
      os << "} else {\n";
      os << "tfel::raise(\"" << cname << "::set: \"\n"
         << "\" no parameter named "
         << "'\"+std::string(key)+\"'\");\n"
         << "}\n"
         << "}\n\n";
    }
    if (ip) {
      os << "void\n"
         << cname << "::set(const char* const key,\nconst int v)"
         << "{\n"
         << "using namespace std;\n";
      bool first = true;
      for (const auto& p : this->mb.getBehaviourData(h).getParameters()) {
        if (p.type == "int") {
          write_if(first);
          os << "::strcmp(\"" + this->mb.getExternalName(h, p.name) +
                    "\",key)==0){\n";
          if ((h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) ||
              ((h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) &&
               (!this->mb.hasParameter(ModellingHypothesis::UNDEFINEDHYPOTHESIS,
                                       p.name)))) {
            os << "this->" << p.name << " = v;\n";
          } else {
            os << dcname << "::get().set(\""
               << this->mb.getExternalName(h, p.name) << "\",v);\n";
          }
        }
      }
      os << "} else {\n";
      os << "tfel::raise(\"" << cname << "::set: \"\n"
         << "\"no parameter named "
         << "'\"+std::string(key)+\"'\");\n"
         << "}\n"
         << "}\n\n";
    }
    if (up) {
      os << "void\n"
         << cname << "::set(const char* const key,\nconst unsigned short v)"
         << "{\n"
         << "using namespace std;\n";
      bool first = true;
      for (const auto& p : this->mb.getBehaviourData(h).getParameters()) {
        if (p.type == "ushort") {
          write_if(first);
          os << "::strcmp(\"" + this->mb.getExternalName(h, p.name) +
                    "\",key)==0){\n";
          if ((h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) ||
              ((h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) &&
               (!this->mb.hasParameter(ModellingHypothesis::UNDEFINEDHYPOTHESIS,
                                       p.name)))) {
            os << "this->" << p.name << " = v;\n";
          } else {
            os << dcname << "::get().set(\""
               << this->mb.getExternalName(h, p.name) << "\",v);\n";
          }
        }
      }
      os << "} else {\n";
      os << "tfel::raise(\"" << cname << "::set: \"\n"
         << "\"no parameter named '\"+std::string(key)+\"'\");\n"
         << "}\n"
         << "}\n\n";
    }
    if (allowsParametersInitializationFromFile(this->mb)) {
      if (rp2) {
        BehaviourDSLCommon_writeConverter(os, cname, "double", "Double");
      }
      if (ip2) {
        BehaviourDSLCommon_writeConverter(os, cname, "int", "Int");
      }
      if (up2) {
        BehaviourDSLCommon_writeConverter(os, cname, "unsigned short",
                                          "UnsignedShort");
      }
      os << "void\n" << cname << "::readParameters(" << cname << "&";
      if (rp2 || ip2 || up2) {
        os << " pi";
      }
      os << ",const char* const fn)"
         << "{\n"
         << "auto tokenize = [](const std::string& line){\n"
         << "std::istringstream tokenizer(line);\n"
         << "std::vector<std::string> tokens;\n"
         << "std::copy(std::istream_iterator<std::string>(tokenizer),\n"
         << "std::istream_iterator<std::string>(),\n"
         << "std::back_inserter(tokens));\n"
         << "return tokens;\n"
         << "};\n"
         << "std::ifstream f(fn);\n"
         << "if(!f){\n"
         << "return;\n"
         << "}\n"
         << "size_t ln = 1u;\n"
         << "while(!f.eof()){\n"
         << "auto line = std::string{};\n"
         << "std::getline(f,line);\n"
         << "auto tokens = tokenize(line);\n"
         << "auto throw_if = [ln,line,fn](const bool c,const std::string& m){\n"
         << "tfel::raise_if(c,\"" << cname << "::readParameters: \"\n"
         << "\"error at line '\"+std::to_string(ln)+\"' \"\n"
         << "\"while reading parameter file '\"+std::string(fn)+\"'\"\n"
         << "\"(\"+m+\")\");\n"
         << "};\n"
         << "if(tokens.empty()){\n"
         << "continue;\n"
         << "}\n"
         << "if(tokens[0][0]=='#'){\n"
         << "continue;\n"
         << "}\n"
         << "throw_if(tokens.size()!=2u,\"invalid number of tokens\");\n";
      bool first = true;
      for (const auto& p : this->mb.getBehaviourData(h).getParameters()) {
        const auto b =
            ((h == ModellingHypothesis::UNDEFINEDHYPOTHESIS) ||
             ((h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) &&
              (!this->mb.hasParameter(ModellingHypothesis::UNDEFINEDHYPOTHESIS,
                                      p.name))));
        auto write = [this, &os, &p, &b, &dcname, &cname](
                         const std::string& vn, const std::string& en) {
          os << "\"" << en << "\"==tokens[0]){\n";
          if (b) {
            os << "pi." << vn << " = ";
            if (p.type == "int") {
              os << cname << "::getInt(tokens[0],tokens[1]);\n";
            } else if (p.type == "ushort") {
              os << cname << "::getUnsignedShort(tokens[0],tokens[1]);\n";
            } else {
              const auto f = SupportedTypes::getTypeFlag(p.type);
              if (f != SupportedTypes::SCALAR) {
                this->throwRuntimeError(
                    "BehaviourDSLCommon::writeSrcFileParametersInitializer",
                    "invalid parameter type '" + p.type + "'");
              }
              os << cname << "::getDouble(tokens[0],tokens[1]);\n";
            }
          } else {
            os << dcname << "::get().set(\"" << en << "\",\n";
            if (p.type == "int") {
              os << dcname << "::getInt(tokens[0],tokens[1])";
            } else if (p.type == "ushort") {
              os << dcname << "::getUnsignedShort(tokens[0],tokens[1])";
            } else {
              const auto f = SupportedTypes::getTypeFlag(p.type);
              if (f != SupportedTypes::SCALAR) {
                this->throwRuntimeError(
                    "BehaviourDSLCommon::writeSrcFileParametersInitializer",
                    "invalid parameter type '" + p.type + "'");
              }
              os << dcname << "::getDouble(tokens[0],tokens[1])";
            }
            os << ");\n";
          }
        };
        if (p.arraySize == 1u) {
          write_if(first);
          write(p.name, this->mb.getExternalName(h, p.name));
        } else {
          for (unsigned short i = 0; i != p.arraySize; ++i) {
            const auto vn = p.name + '[' + std::to_string(i) + ']';
            const auto en = this->mb.getExternalName(h, p.name) + '[' +
                            std::to_string(i) + ']';
            write_if(first);
            write(vn, en);
          }
        }
      }
      os << "} else {\n"
         << "throw_if(true,\"invalid parameter '\"+tokens[0]+\"'\");\n"
         << "}\n"
         << "}\n"
         << "}\n\n";
    }
  }  // end of writeSrcFileParametersInitializer

  void BehaviourDSLCommon::writeSrcFileBehaviourProfiler(
      std::ostream& os) const {
    if (this->mb.getAttribute(BehaviourData::profiling, false)) {
      this->checkSrcFile(os);
      os << "mfront::BehaviourProfiler&\n"
         << this->mb.getClassName() << "Profiler::getProfiler()\n"
         << "{\n"
         << "static mfront::BehaviourProfiler profiler(\""
         << this->mb.getClassName() << "\");\n;"
         << "return profiler;\n"
         << "}\n\n";
    }
  }  // end of writeSrcFileBehaviourProfiler

  void BehaviourDSLCommon::writeSrcFile(std::ostream& os) const {
    this->writeSrcFileHeader(os);
    this->writeSrcFileUserDefinedCode(os);
    this->writeNamespaceBegin(os);
    this->writeSrcFileBehaviourProfiler(os);
    this->writeSrcFileParametersInitializers(os);
    this->writeNamespaceEnd(os);
  }  // end of writeSrcFile

  std::string BehaviourDSLCommon::predictionOperatorVariableModifier(
      const Hypothesis h, const std::string& var, const bool addThisPtr) {
    if (this->mb.isIntegrationVariableIncrementName(h, var)) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::predictionOperatorVariableModifier : ",
          "integration variable '" + var +
              "' can't be used in @PredictionOperator");
    }
    if (addThisPtr) {
      return "(this->" + var + ")";
    }
    return var;
  }  // end of predictionOperatorVariableModifier

  void BehaviourDSLCommon::treatProfiling() {
    const auto b = this->readBooleanValue("BehaviourDSLCommon::treatProfiling");
    this->readSpecifiedToken("BehaviourDSLCommon::treatProfiling", ";");
    this->mb.setAttribute(BehaviourData::profiling, b, false);
  }  // end of treatProfiling

  void BehaviourDSLCommon::treatPredictionOperator() {
    using namespace tfel::material;
    using namespace tfel::utilities;
    if (this->mb.getMainVariables().empty()) {
      this->throwRuntimeError("BehaviourDSLCommon::treatTangentOperator",
                              "no thermodynamic force defined");
    }
    CodeBlockOptions o;
    this->readCodeBlockOptions(o, true);
    if (this->mb.getBehaviourType() ==
        BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) {
      bool found = false;
      if (o.untreated.size() != 1u) {
        std::ostringstream msg;
        msg << "tangent operator type is undefined. Valid tanget operator type "
               "are :\n";
        for (const auto& to : getFiniteStrainBehaviourTangentOperatorFlags()) {
          msg << "- "
              << convertFiniteStrainBehaviourTangentOperatorFlagToString(to)
              << " : " << getFiniteStrainBehaviourTangentOperatorDescription(to)
              << '\n';
        }
        this->throwRuntimeError("BehaviourDSLCommon::treatPredictionOperator",
                                msg.str());
      }
      if (o.untreated[0].flag != Token::Standard) {
        this->throwRuntimeError(
            "BehaviourDSLCommon::treatPredictionOperator",
            "invalid option '" + o.untreated[0].value + "'");
      }
      const auto& ktype = o.untreated[0].value;
      for (const auto& to : getFiniteStrainBehaviourTangentOperatorFlags()) {
        if (ktype ==
            convertFiniteStrainBehaviourTangentOperatorFlagToString(to)) {
          found = true;
          break;
        }
      }
      if (!found) {
        std::ostringstream msg;
        msg << "invalid tangent operator type '" + ktype +
                   "'. Valid tanget operator type are :\n";
        for (const auto& to : getFiniteStrainBehaviourTangentOperatorFlags()) {
          msg << "- "
              << convertFiniteStrainBehaviourTangentOperatorFlagToString(to)
              << " : " << getFiniteStrainBehaviourTangentOperatorDescription(to)
              << '\n';
        }
        this->throwRuntimeError("BehaviourDSLCommon::treatPredictionOperator",
                                msg.str());
      }
      const auto po =
          std::string(BehaviourData::ComputePredictionOperator) + "-" + ktype;
      this->treatCodeBlock(
          *this, o, po, &BehaviourDSLCommon::predictionOperatorVariableModifier,
          true);
      for (const auto& h : o.hypotheses) {
        if (!this->mb.hasAttribute(h, BehaviourData::hasPredictionOperator)) {
          this->mb.setAttribute(h, BehaviourData::hasPredictionOperator, true);
        }
      }
    } else {
      this->treatUnsupportedCodeBlockOptions(o);
      this->treatCodeBlock(
          *this, o, BehaviourData::ComputePredictionOperator,
          &BehaviourDSLCommon::predictionOperatorVariableModifier, true);
      for (const auto& h : o.hypotheses) {
        this->mb.setAttribute(h, BehaviourData::hasPredictionOperator, true);
      }
    }
  }  // end of treatPredictionOperator

  void BehaviourDSLCommon::treatParameter() {
    auto throw_if = [this](const bool b, const std::string& m) {
      if (b) {
        this->throwRuntimeError("BehaviourDSLCommon::treatParameter", m);
      }
    };
    std::set<Hypothesis> mh;
    this->readHypothesesList(mh);
    auto endOfTreatment = false;
    while ((this->current != this->tokens.end()) && (!endOfTreatment)) {
      const auto vtype = [this]() -> std::string {
        const auto otype = this->readVariableTypeIfPresent();
        if (!otype) {
          return "real";
        }
        return *otype;
      }();
      this->checkNotEndOfFile("BehaviourDSLCommon::treatParameter");
      const auto sname = this->current->value;
      const auto vname = tfel::unicode::getMangledString(sname);
      throw_if(!isValidIdentifier(vname),
               "variable given is not valid (read "
               "'" +
                   sname + "').");
      const auto lineNumber = this->current->line;
      ++(this->current);
      this->checkNotEndOfFile("BehaviourDSLCommon::treatParameter");
      const auto arraySize = this->readArrayOfVariablesSize(sname, true);
      this->checkNotEndOfFile("BehaviourDSLCommon::treatParameter");
      if ((this->current->value == "=") || (this->current->value == "{") ||
          (this->current->value == "(")) {
        std::string ci;  // closing initializer
        if (this->current->value == "{") {
          ci = "}";
        }
        if (this->current->value == "(") {
          throw_if(arraySize != 1u,
                   "invalid initalisation syntax for "
                   "the default values of an array of parameters.\n"
                   "Unexpected token '" +
                       current->value + "'");
          ci = ")";
        }
        ++(this->current);
        this->checkNotEndOfFile("BehaviourDSLCommon::treatParameter");
        if (arraySize != 1u) {
          if (ci != "}") {
            this->readSpecifiedToken("BehaviourDSLCommon::treatParameter", "{");
          }
          --(this->current);
          const auto r =
              this->readArrayOfDouble("BehaviourDSLCommon::treatParameter");
          throw_if(
              r.size() != arraySize,
              "number of values given does not match the numberf of parameters "
              "(" +
                  std::to_string(r.size()) + " vs +" +
                  std::to_string(arraySize) + ").\n");
          for (const auto& h : mh) {
            VariableDescription p;
            if (vname == sname) {
              p = VariableDescription(vtype, vname, arraySize, lineNumber);
            } else {
              p = VariableDescription(vtype, sname, vname, arraySize,
                                      lineNumber);
            }
            p.description = this->currentComment;
            this->mb.addParameter(h, p);
            for (decltype(r.size()) i = 0; i != r.size(); ++i) {
              this->mb.setParameterDefaultValue(h, vname, i, r[i]);
            }
          }
        } else {
          double value;
          std::istringstream converter(this->current->value);
          converter >> value;
          if (!converter || (!converter.eof())) {
            this->throwRuntimeError(
                "BehaviourDSLCommon::treatParameter",
                "could not read default value for parameter '" + sname + "'");
          }
          ++(this->current);
          this->checkNotEndOfFile("BehaviourDSLCommon::treatParameter");
          if (!ci.empty()) {
            this->readSpecifiedToken("BehaviourDSLCommon::treatParameter", ci);
          }
          for (const auto& h : mh) {
            VariableDescription p;
            if (vname == sname) {
              p = VariableDescription(vtype, vname, 1u, lineNumber);
            } else {
              p = VariableDescription(vtype, sname, vname, 1u, lineNumber);
            }
            p.description = this->currentComment;
            this->mb.addParameter(h, p);
            this->mb.setParameterDefaultValue(h, vname, value);
          }
        }
      } else {
        throw_if(arraySize != 1,
                 "default values of parameters array "
                 "must be defined with the array. "
                 "Unexpected token '" +
                     current->value + "'");
        for (const auto& h : mh) {
          VariableDescription p;
          if (vname == sname) {
            p = VariableDescription(vtype, vname, 1u, lineNumber);
          } else {
            p = VariableDescription(vtype, sname, vname, 1u, lineNumber);
          }
          p.description = this->currentComment;
          this->mb.addParameter(h, p);
        }
      }
      if (this->current->value == ",") {
        ++(this->current);
      } else if (this->current->value == ";") {
        endOfTreatment = true;
        ++(this->current);
      } else {
        throw_if(true, "',' or ';' expected after '" + sname + "', read '" +
                           this->current->value + "'");
      }
    }
    if (!endOfTreatment) {
      --(this->current);
      throw_if(true, "expected ';' before end of file");
    }
  }  // end of treatParameter

  void BehaviourDSLCommon::treatInitLocalVariables() {
    this->treatCodeBlock(*this, BehaviourData::InitializeLocalVariables,
                         &BehaviourDSLCommon::standardModifier, true, true);
  }  // end of BehaviourDSLCommon:treatInitLocalVariables

  void BehaviourDSLCommon::treatMinimalTimeStepScalingFactor() {
    const auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    double r_dt;
    this->checkNotEndOfFile(
        "BehaviourDSLCommon::treatMinimalTimeStepScalingFactor",
        "Cannot read value.");
    std::istringstream flux(current->value);
    flux >> r_dt;
    if ((flux.fail()) || (!flux.eof())) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::treatMinimalTimeStepScalingFactor",
          "Failed to read value.");
    }
    if (r_dt < 10 * std::numeric_limits<double>::min()) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::treatMinimalTimeStepScalingFactor",
          "minimal time step scaling factor either too "
          "low value or negative.");
    }
    ++(this->current);
    this->readSpecifiedToken(
        "BehaviourDSLCommon::treatMinimalTimeStepScalingFactor", ";");
    VariableDescription e("real", "minimal_time_step_scaling_factor", 1u, 0u);
    e.description = "minimal value for the time step scaling factor";
    this->mb.addParameter(h, e, BehaviourData::ALREADYREGISTRED);
    this->mb.setParameterDefaultValue(h, "minimal_time_step_scaling_factor",
                                      r_dt);
    this->mb.setEntryName(h, "minimal_time_step_scaling_factor",
                          "minimal_time_step_scaling_factor");
  }  // end of treatMinimalTimeStepScalingFactor

  void BehaviourDSLCommon::treatMaximalTimeStepScalingFactor() {
    const auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    double r_dt;
    this->checkNotEndOfFile(
        "BehaviourDSLCommon::treatMaximalTimeStepScalingFactor",
        "Cannot read value.");
    std::istringstream flux(current->value);
    flux >> r_dt;
    if ((flux.fail()) || (!flux.eof())) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::treatMaximalTimeStepScalingFactor",
          "Failed to read value.");
    }
    if (r_dt < 1) {
      this->throwRuntimeError(
          "BehaviourDSLCommon::treatMaximalTimeStepScalingFactor",
          "maximal time step scaling factor value either too "
          "low or negative.");
    }
    ++(this->current);
    this->readSpecifiedToken(
        "BehaviourDSLCommon::treatMaximalTimeStepScalingFactor", ";");
    VariableDescription e("real", "maximal_time_step_scaling_factor", 1u, 0u);
    e.description = "maximal value for the time step scaling factor";
    this->mb.addParameter(h, e, BehaviourData::ALREADYREGISTRED);
    this->mb.setParameterDefaultValue(h, "maximal_time_step_scaling_factor",
                                      r_dt);
    this->mb.setEntryName(h, "maximal_time_step_scaling_factor",
                          "maximal_time_step_scaling_factor");
  }  // end of treatMaximalTimeStepScalingFactor

  void BehaviourDSLCommon::setMinimalTangentOperator() {
    if (this->mb.getBehaviourType() !=
        BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) {
      for (const auto& h : this->mb.getDistinctModellingHypotheses()) {
        // basic check
        if (this->mb.hasAttribute(
                h, BehaviourData::hasConsistentTangentOperator)) {
          if (!this->mb.hasCode(h, BehaviourData::ComputeTangentOperator)) {
            this->throwRuntimeError(
                "BehaviourDSLCommon::setMinimalTangentOperator",
                "behaviour has attribute 'hasConsistentTangentOperator' but "
                "no associated code");
          }
        }
      }
      if (this->mb.getAttribute(BehaviourDescription::requiresStiffnessTensor,
                                false)) {
        if (this->mb.getBehaviourType() ==
            BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) {
          const auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
          // if the user provided a tangent operator, it won't be
          // overriden

          CodeBlock tangentOperator;
          std::ostringstream code;
          code << "if(smt==ELASTIC){\n"
               << "this->Dt = this->D;\n"
               << "} else {\n"
               << "return false;\n"
               << "}\n";
          tangentOperator.code = code.str();
          this->mb.setCode(h, BehaviourData::ComputeTangentOperator,
                           tangentOperator, BehaviourData::CREATEBUTDONTREPLACE,
                           BehaviourData::BODY);
          this->mb.setAttribute(h, BehaviourData::hasConsistentTangentOperator,
                                true, true);
        }
      }
    }
  }  // end of setMinimalTangentOperator

  void BehaviourDSLCommon::treatInitialize() {
    CodeBlockOptions o;
    this->readCodeBlockOptions(o, true);
    //
    auto id = std::string{};
    auto po = o.untreated.begin();
    const auto poe = o.untreated.end();
    while (po != poe) {
      const auto& opt = *po;
      if (!ModellingHypothesis::isModellingHypothesis(opt.value)) {
        id = opt.value;
        if (!isValidIdentifier(id)) {
          this->throwRuntimeError(
              "BehaviourDSLCommon::treatInitialize",
              "initialize function identifier is invalid (read '" + id + "').");
        }
        o.untreated.erase(po);
        break;
      }
      ++po;
    }
    if (id.empty()) {
      this->throwRuntimeError("BehaviourDSLCommon::treatInitialize",
                              "no initialize function identifier given");
    }
    this->treatUnsupportedCodeBlockOptions(o);
    const auto n =
        std::string(BehaviourData::UserDefinedInitializeCodeBlock) + id;
    std::function<std::string(const Hypothesis, const std::string&, const bool)>
        m = [this](const Hypothesis h, const std::string& sv, const bool bv) {
          return this->standardModifier(h, sv, bv);
        };
    for (const auto h : o.hypotheses) {
      const auto& d = this->mb.getBehaviourData(h);
      const auto c = this->readNextBlock(h, n, m, true);
      for (const auto& mn : c.members) {
        if ((d.isStateVariableIncrementName(mn)) ||
            (d.isExternalStateVariableIncrementName(mn))) {
          this->throwRuntimeError("BehaviourDSLCommon::treatInitialize",
                                  "variable '" + mn + "' is not allowed here");
        }
      }
      this->mb.setCode(h, n, c, o.m, o.p);
    }
  }  // end of treatInitialize

  void BehaviourDSLCommon::treatInternalEnergy() {
    this->treatCodeBlock(*this, BehaviourData::ComputeInternalEnergy,
                         &BehaviourDSLCommon::standardModifier, true, true);
  }  // end of treatInternalEnergy

  void BehaviourDSLCommon::treatDissipatedEnergy() {
    this->treatCodeBlock(*this, BehaviourData::ComputeDissipatedEnergy,
                         &BehaviourDSLCommon::standardModifier, true, true);
  }  // end of treatDissipatedEnergy

  static BehaviourDescription::SlipSystem readSlipSystem(
      BehaviourDSLCommon::CxxTokenizer::const_iterator& p,
      const BehaviourDSLCommon::CxxTokenizer::const_iterator pe) {
    using tfel::material::SlipSystemsDescription;
    using tfel::utilities::CxxTokenizer;
    auto throw_if = [](const bool c, const std::string& msg) {
      tfel::raise_if(c, "readSlipSystem: " + msg);
    };
    const auto direction =
        CxxTokenizer::readList("readSlipSystem", "<", ">", p, pe);
    const auto plane =
        CxxTokenizer::readList("readSlipSystem", "{", "}", p, pe);
    throw_if(plane.size() != direction.size(),
             "plane and direction don't match in size");
    throw_if((plane.size() != 3u) && (plane.size() != 4u),
             "invalid definition of a plane "
             "(must be an array of 3 or 4 integers, read '" +
                 std::to_string(plane.size()) + "' values)");
    if (plane.size() == 3u) {
      SlipSystemsDescription::system3d s3d;
      for (tfel::math::tvector<3u, int>::size_type i = 0; i != 3; ++i) {
        s3d.plane[i] = std::stoi(plane[i].value);
        s3d.burgers[i] = std::stoi(direction[i].value);
      }
      return {s3d};
    }
    SlipSystemsDescription::system4d s4d;
    for (tfel::math::tvector<4u, int>::size_type i = 0; i != 4; ++i) {
      s4d.plane[i] = std::stoi(plane[i].value);
      s4d.burgers[i] = std::stoi(direction[i].value);
    }
    return {s4d};
  }

  void BehaviourDSLCommon::treatSlipSystem() {
    const auto s = readSlipSystem(this->current, this->tokens.end());
    this->mb.setSlipSystems({1u, s});
    this->readSpecifiedToken("BehaviourDSLCommon::treatSlipSystem", ";");
  }  // end of treatSlipSystem()

  void BehaviourDSLCommon::treatSlipSystems() {
    const std::string m = "BehaviourDSLCommon::treatSlipSystems";
    std::vector<BehaviourDescription::SlipSystem> ss;
    this->readSpecifiedToken(m, "{");
    this->checkNotEndOfFile(m, "expected token");
    while (this->current->value != "}") {
      this->checkNotEndOfFile(m, "expected slip system");
      ss.push_back(readSlipSystem(this->current, this->tokens.end()));
      this->checkNotEndOfFile(m, "expected ',' or '}'");
      if (this->current->value != "}") {
        this->readSpecifiedToken(m, ",");
        this->checkNotEndOfFile(m, "expected slip system");
        if (this->current->value == "}") {
          this->throwRuntimeError(m, "unexpected token '}'");
        }
      }
    }
    this->readSpecifiedToken(m, "}");
    this->readSpecifiedToken(m, ";");
    this->mb.setSlipSystems(ss);
  }  // end of treatSlipSystems

  void BehaviourDSLCommon::treatCrystalStructure() {
    using tfel::material::CrystalStructure;
    this->checkNotEndOfFile("BehaviourDSLCommon::treatCrystalStructure",
                            "expected crystal structure");
    if (this->current->value == "Cubic") {
      this->mb.setCrystalStructure(CrystalStructure::Cubic);
    } else if (this->current->value == "FCC") {
      this->mb.setCrystalStructure(CrystalStructure::FCC);
    } else if (this->current->value == "BCC") {
      this->mb.setCrystalStructure(CrystalStructure::BCC);
    } else if (this->current->value == "HCP") {
      this->mb.setCrystalStructure(CrystalStructure::HCP);
    } else {
      this->throwRuntimeError("BehaviourDSLCommon::treatCrystalStructure",
                              "unsupported crystal structure "
                              "'" +
                                  this->current->value + "'");
    }
    ++(this->current);
    this->readSpecifiedToken("BehaviourDSLCommon::treatCrystalStructure", ";");
  }  // end of treatCrystalStructure

  void BehaviourDSLCommon::treatInteractionMatrix() {
    auto throw_if = [this](const bool b, const std::string& m) {
      if (b) {
        this->throwRuntimeError("BehaviourDSLCommon::treatInteractionMatrix",
                                m);
      }
    };
    throw_if(!this->mb.areSlipSystemsDefined(),
             "slip systems have not been defined");
    const auto& im = this->mb.getInteractionMatrixStructure();
    const auto r = im.rank();
    const auto mv =
        CxxTokenizer::readArray("BehaviourDSLCommon::treatInteractionMatrix",
                                this->current, this->tokens.end());
    this->readSpecifiedToken("BehaviourDSLCommon::treatInteractionMatrix", ";");
    throw_if(mv.size() != r,
             "the number of values does "
             "not match the number of independent coefficients "
             "in the interaction matrix");
    auto imv = std::vector<long double>{};
    imv.reserve((mv.size()));
    for (const auto& v : mv) {
      imv.push_back(tfel::utilities::convert<long double>(v));
    }
    this->mb.setInteractionMatrix(imv);
  }  // end of treatInteractionMatrix

  void BehaviourDSLCommon::treatDislocationsMeanFreePathInteractionMatrix() {
    auto throw_if = [this](const bool b, const std::string& m) {
      if (b) {
        this->throwRuntimeError(
            "BehaviourDSLCommon::"
            "treatDislocationsMeanFreePathInteractionMatrix",
            m);
      }
    };
    throw_if(!this->mb.areSlipSystemsDefined(),
             "slip systems have not been defined");
    const auto& im = this->mb.getInteractionMatrixStructure();
    const auto r = im.rank();
    const auto mv = CxxTokenizer::readArray(
        "BehaviourDSLCommon::"
        "treatDislocationsMeanFreePathInteractionMatrix",
        this->current, this->tokens.end());
    this->readSpecifiedToken(
        "BehaviourDSLCommon::"
        "treatDislocationsMeanFreePathInteractionMatrix",
        ";");
    throw_if(mv.size() != r,
             "the number of values does "
             "not match the number of independent coefficients "
             "in the interaction matrix");
    auto imv = std::vector<long double>{};
    imv.reserve((mv.size()));
    for (const auto& v : mv) {
      imv.push_back(tfel::utilities::convert<long double>(v));
    }
    this->mb.setDislocationsMeanFreePathInteractionMatrix(imv);
  }  // end of treatDislocationsMeanFreePathInteractionMatrix

  void BehaviourDSLCommon::
      setComputeFinalThermodynamicForcesFromComputeFinalThermodynamicForcesCandidateIfNecessary() {
    // first treating specialised mechanical data
    for (const auto& h : this->mb.getDistinctModellingHypotheses()) {
      if (h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
        if (!this->mb.hasCode(h,
                              BehaviourData::ComputeFinalThermodynamicForces)) {
          if (this->mb.hasCode(
                  h, BehaviourData::ComputeFinalThermodynamicForcesCandidate)) {
            this->mb.setCode(
                h, BehaviourData::ComputeFinalThermodynamicForces,
                this->mb.getCodeBlock(
                    h, BehaviourData::ComputeFinalThermodynamicForcesCandidate),
                BehaviourData::CREATE, BehaviourData::BODY);
          }
        }
      }
    }
    // now treating the default hypothesis case
    if (!this->mb.areAllMechanicalDataSpecialised()) {
      const auto h = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
      if (!this->mb.hasCode(h,
                            BehaviourData::ComputeFinalThermodynamicForces)) {
        if (this->mb.hasCode(
                h, BehaviourData::ComputeFinalThermodynamicForcesCandidate)) {
          this->mb.setCode(
              h, BehaviourData::ComputeFinalThermodynamicForces,
              this->mb.getCodeBlock(
                  h, BehaviourData::ComputeFinalThermodynamicForcesCandidate),
              BehaviourData::CREATEBUTDONTREPLACE, BehaviourData::BODY);
        }
      }
    }
  }  // end of
     // setComputeFinalThermodynamicForcesFromComputeFinalThermodynamicForcesCandidateIfNecessary

  std::string BehaviourDSLCommon::getOverridableVariableNameByExternalName(
      const std::string& en) const {
    constexpr auto uh = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    const auto& d = this->mb.getBehaviourData(uh);
    const auto pp = findByExternalName(d.getParameters(), en);
    if (pp != d.getParameters().end()) {
      return pp->name;
    }
    const auto pmp = findByExternalName(d.getMaterialProperties(), en);
    if (pmp == d.getMaterialProperties().end()) {
      tfel::raise(
          "BehaviourDSLCommon::getOverridableVariableNameByExternalName: "
          "no overridable variable associated with external name '" +
          en + "'");
    }
    return pmp->name;
  }  // end of getOverridableVariableNameByExternalName

  void BehaviourDSLCommon::overrideByAParameter(const std::string& n,
                                                const double v) {
    this->mb.overrideByAParameter(n, v);
  }  // end of overrideByAParameter

  std::map<std::string, double> BehaviourDSLCommon::getOverridenParameters()
      const {
    return this->mb.getOverridenParameters();
  }  // end of getOverridenParameters

  BehaviourDSLCommon::~BehaviourDSLCommon() = default;

}  // end of namespace mfront
