/*!
 * \file   mfront/include/MFront/BehaviourDSLCommon.hxx
 * \brief
 *
 * \author Thomas Helfer
 * \date   05 mai 2008
 * \copyright Copyright (C) 2006-2018 CEA/DEN, EDF R&D. All rights
 * reserved.
 * This project is publicly released under either the GNU GPL Licence
 * or the CECILL-A licence. A copy of thoses licences are delivered
 * with the sources of TFEL. CEA or EDF may also distribute this
 * project under specific licensing conditions.
 */

#ifndef LIB_MFRONT_MFRONTBEHAVIOURDSLCOMMON_HXX
#define LIB_MFRONT_MFRONTBEHAVIOURDSLCOMMON_HXX

#include <set>
#include <map>
#include <vector>
#include <string>
#include <memory>
#include <fstream>
#include <functional>

#include "MFront/MFrontConfig.hxx"
#include "MFront/DSLBase.hxx"
#include "MFront/SupportedTypes.hxx"
#include "MFront/AbstractBehaviourDSL.hxx"
#include "MFront/BehaviourDescription.hxx"

namespace mfront {

  // forward declaration
  struct AbstractBehaviourInterface;
  // forward declaration
  struct AbstractBehaviourBrick;

  /*!
   * \return if the given name is valid
   * \param[in] n: behaviour name
   */
  MFRONT_VISIBILITY_EXPORT bool isValidBehaviourName(const std::string&);

  /*!
   * \brief this class provides most functionnalities used by mechanical
   * behaviour parsers.
   */
  struct MFRONT_VISIBILITY_EXPORT BehaviourDSLCommon
      : public virtual AbstractBehaviourDSL,
        public DSLBase,
        public SupportedTypes {
    //! \return a validator for the options passed to the DSL
    static tfel::utilities::DataMapValidator getDSLOptionsValidator();
    //! a simple alias
    using Hook = std::function<void()>;
    //! a simple alias
    using OrthotropicAxesConvention = tfel::material::OrthotropicAxesConvention;
    //! \return the behaviour description
    const BehaviourDescription& getBehaviourDescription() const override final;

    std::shared_ptr<MaterialPropertyDescription>
    handleMaterialPropertyDescription(const std::string&) override;
    std::string getMaterialKnowledgeIdentifier() const override;
    std::string getMaterialName() const override;
    std::string getOverridableVariableNameByExternalName(
        const std::string&) const override;
    void overrideByAParameter(const std::string&, const double) override;
    std::map<std::string, double> getOverridenParameters() const override;

    void analyseFile(const std::string&,
                     const std::vector<std::string>&,
                     const std::map<std::string, std::string>&) override;
    void importFile(const std::string&,
                    const std::vector<std::string>&,
                    const std::map<std::string, std::string>&) override;
    void analyseString(const std::string&) override;

    void endsInputFileProcessing() override;
    /*!
     * \brief method called when a new gradient or a new thermodynamic force is
     * defined. It declares as many pair of gradient and thermodynamic force as
     * possible.
     */
    virtual void declareMainVariables();

    virtual void addHook(const std::string&, const Hook);
    /*!
     * \brief return the list of keywords usable with this parser
     * \param[out] k : the list of keywords registred for this parser
     */
    void getKeywordsList(std::vector<std::string>&) const override;

    void addMaterialProperties(const VariableDescriptionContainer&) override;
    void addParameters(const VariableDescriptionContainer&) override;
    void addStateVariables(const VariableDescriptionContainer&) override;
    void addAuxiliaryStateVariables(
        const VariableDescriptionContainer&) override;
    void addExternalStateVariables(
        const VariableDescriptionContainer&) override;
    void addLocalVariables(const VariableDescriptionContainer&) override;
    void addIntegrationVariables(const VariableDescriptionContainer&) override;

   protected:
    //! \brief a simple alias
    using CallBack = std::function<void()>;
    //! \brief create a variable modifier from a method
    struct TFEL_VISIBILITY_LOCAL StandardVariableModifier final
        : public VariableModifier {
      //! a simple alias
      using FunctionType = std::function<std::string(
          const Hypothesis, const std::string&, const bool)>;
      /*!
       * \brief constructor from a std::function
       * \param[in] h: hypothesis
       * \param[in] f: function object
       */
      StandardVariableModifier(const Hypothesis, const FunctionType);
      /*!
       * \param[in] v : the variable name
       * \param[in] b : true if "this" shall be added
       */
      std::string exe(const std::string&, const bool) override;
      //! destructor
      ~StandardVariableModifier() override;

     private:
      const Hypothesis hypothesis;
      FunctionType fct;
    };
    struct TFEL_VISIBILITY_LOCAL StandardWordAnalyser final
        : public WordAnalyser {
      //! a simple alias
      using FunctionType =
          std::function<void(CodeBlock&, const Hypothesis, const std::string&)>;
      /*!
       * \brief constructor
       * \param[in] h: hypothesis
       * \param[in] f: function object
       */
      StandardWordAnalyser(const Hypothesis, const FunctionType);
      /*!
       * \param[in] cb : code block
       * \param[in] k : the current word
       */
      void exe(CodeBlock&, const std::string&) override;
      //! destructor
      ~StandardWordAnalyser() override;

     private:
      const Hypothesis hypothesis;
      const FunctionType fct;
    };
    /*!
     * \brief structure holding options passed to code blocks
     */
    struct CodeBlockOptions {
      //! a simple alias
      using Mode = BehaviourData::Mode;
      //! a simple alias
      using Position = BehaviourData::Position;
      //! constructor
      CodeBlockOptions();
      CodeBlockOptions(CodeBlockOptions&&) = default;
      CodeBlockOptions(const CodeBlockOptions&) = default;
      CodeBlockOptions& operator=(CodeBlockOptions&&) = default;
      CodeBlockOptions& operator=(const CodeBlockOptions&) = default;
      ~CodeBlockOptions();
      //! position where the code block will be inserted (body by defaut)
      Position p;
      //! insertion mode (create or append by default)
      Mode m;
      //! list of hypothesis
      std::set<Hypothesis> hypotheses;
      //! list of untreated options
      std::vector<tfel::utilities::Token> untreated;
    };
    /*!
     * \brief constructor
     * \param[in] opts: options passed to the DSL
     */
    BehaviourDSLCommon(const DSLOptions&);
    //
    void addExternalMFrontFile(const std::string&,
                               const std::vector<std::string>&) override;
    const MaterialKnowledgeDescription& getMaterialKnowledgeDescription()
        const override;
    std::vector<DSLOptionDescription> getDSLOptions() const override;
    DSLOptions buildDSLOptions() const override;

    bool useQt() const override;
    void disableQuantitiesUsageIfNotAlreadySet() override;
    std::string getClassName() const override;
    void addMaterialLaw(const std::string&) override;
    void appendToIncludes(const std::string&) override;
    void appendToMembers(const std::string&) override;
    void appendToPrivateCode(const std::string&) override;
    void appendToSources(const std::string&) override;

    std::set<Hypothesis> getModellingHypothesesToBeTreated() const;

    /*!
     * \return if the generation of the source file is required
     *
     * The source file contains implementations of functions common to all
     * interfaces.
     */
    virtual bool isSrcFileRequired() const;

    virtual void analyse();

    virtual void treatDisabledCallBack();

    virtual void disableCallBack(const std::string&);
    /*!
     * \brief add a new call-back associated with a keyword
     *
     * \param[in] k: keyword
     * \param[in] c: call-back
     * \param[in] b: if true, allow the given call-back to override an existing
     * call-back, if any.
     */
    virtual void addCallBack(const std::string&,
                             const CallBack,
                             const bool = false);

    /*!
     * \brief get all symbols required to interpret the given code block.
     * \param[out] symbols: symbols
     * \param[in] h: modelling hypothesis
     * \param[in] n: name of the code block
     */
    virtual void getSymbols(std::map<std::string, std::string>&,
                            const Hypothesis,
                            const std::string&);
    /*!
     * \param[out] o : options to be read
     * \param[in]  s : allow specialisation
     */
    virtual void readCodeBlockOptions(CodeBlockOptions&, const bool);
    /*!
     * \brief read the next code block and adds it tho the mechanical
     * behaviour
     * \param[in] child : a pointer to this
     * \param[in] n     : name of the method read
     * \param[in] m     : modifier
     * \param[in] b     : add "this->" in front of variables
     * \param[in] s     : allow specialisation
     */
    template <typename T, typename T2>
    CodeBlockOptions treatCodeBlock(T&,
                                    const std::string&,
                                    std::string (T2::*)(const Hypothesis,
                                                        const std::string&,
                                                        const bool),
                                    const bool,
                                    const bool);
    /*!
     * \brief read the next code block and adds it tho the mechanical
     * behaviour
     * \param[in] child : a pointer to this
     * \param[in] n     : name of the method read
     * \param[in] m     : modifier
     * \param[in] b     : add "this->" in front of variables
     */
    template <typename T, typename T2>
    void treatCodeBlock(T&,
                        const CodeBlockOptions&,
                        const std::string&,
                        std::string (T2::*)(const Hypothesis,
                                            const std::string&,
                                            const bool),
                        const bool);
    /*!
     * \brief read the next code block and adds it tho the mechanical
     * behaviour
     * \param[in] child : a pointer to this
     * \param[in] n     : name of the method read
     * \param[in] m     : modifier
     * \param[in] a     : word analyser
     * \param[in] b     : add "this->" in front of variables
     * \param[in] s     : allow specialisation
     */
    template <typename T, typename T2, typename T3>
    CodeBlockOptions treatCodeBlock(
        T&,
        const std::string&,
        std::string (T2::*)(const Hypothesis, const std::string&, const bool),
        void (T3::*)(const Hypothesis, const std::string&),
        const bool,
        const bool);
    /*!
     * \brief read the next code block and adds it tho the mechanical
     * behaviour
     * \param[in] child : a pointer to this
     * \param[in] n     : name of the method read
     * \param[in] m     : modifier
     * \param[in] a     : word analyser
     * \param[in] b     : add "this->" in front of variables
     * \param[in] s     : allow specialisation
     */
    template <typename T, typename T2, typename T3>
    void treatCodeBlock(T&,
                        const CodeBlockOptions&,
                        const std::string&,
                        std::string (T2::*)(const Hypothesis,
                                            const std::string&,
                                            const bool),
                        void (T3::*)(const Hypothesis, const std::string&),
                        const bool);
    /*!
     * \brief read the next code block and adds it tho the mechanical
     * behaviour
     * \param[in] child : a pointer to this
     * \param[in] n1    : name of the first method read
     * \param[in] n2    : name of the second method read
     * \param[in] m1    : modifier
     * \param[in] m2    : modifier
     * \param[in] b     : add "this->" in front of variables
     * \param[in] s     : allow specialisation
     */
    template <typename T, typename T2>
    CodeBlockOptions treatCodeBlock(
        T&,
        const std::string&,
        const std::string&,
        std::string (T2::*)(const Hypothesis, const std::string&, const bool),
        std::string (T2::*)(const Hypothesis, const std::string&, const bool),
        const bool,
        const bool);
    /*!
     * \brief read the next code block and adds it tho the mechanical
     * behaviour
     * \param[in] child : a pointer to this
     * \param[in] n1    : name of the first method read
     * \param[in] n2    : name of the second method read
     * \param[in] m1    : modifier
     * \param[in] m2    : modifier
     * \param[in] b     : add "this->" in front of variables
     */
    template <typename T, typename T2>
    void treatCodeBlock(
        T&,
        const CodeBlockOptions&,
        const std::string&,
        const std::string&,
        std::string (T2::*)(const Hypothesis, const std::string&, const bool),
        std::string (T2::*)(const Hypothesis, const std::string&, const bool),
        const bool);
    /*!
     * \brief read the next code block and adds it tho the mechanical
     * behaviour
     * \param[in] n     : name of the method read
     * \param[in] m     : modifier
     * \param[in] b     : add "this->" in front of variables
     * \param[in] s     : allow specialisation
     */
    CodeBlockOptions treatCodeBlock(
        const std::string&,
        std::function<
            std::string(const Hypothesis, const std::string&, const bool)>,
        const bool,
        const bool);
    /*!
     * \brief read the next code block and adds it tho the mechanical
     * behaviour
     * \param[in] n     : name of the method read
     * \param[in] m     : modifier
     * \param[in] b     : add "this->" in front of variables
     */
    void treatCodeBlock(const CodeBlockOptions&,
                        const std::string&,
                        std::function<std::string(
                            const Hypothesis, const std::string&, const bool)>,
                        const bool);
    /*!
     * \brief read the next code block and adds it tho the mechanical
     * behaviour
     * \param[in] n     : name of the method read
     * \param[in] m     : modifier
     * \param[in] a     : word analyser
     * \param[in] b     : add "this->" in front of variables
     * \param[in] s     : allow specialisation
     */
    CodeBlockOptions treatCodeBlock(
        const std::string&,
        std::function<
            std::string(const Hypothesis, const std::string&, const bool)>,
        std::function<void(CodeBlock&, const Hypothesis, const std::string&)>,
        const bool,
        const bool);
    /*!
     * \brief read the next code block and adds it tho the mechanical
     * behaviour
     * \param[in] n     : name of the method read
     * \param[in] m     : modifier
     * \param[in] a     : word analyser
     * \param[in] b     : add "this->" in front of variables
     * \param[in] s     : allow specialisation
     */
    void treatCodeBlock(
        const CodeBlockOptions&,
        const std::string&,
        std::function<
            std::string(const Hypothesis, const std::string&, const bool)>,
        std::function<void(CodeBlock&, const Hypothesis, const std::string&)>,
        const bool);
    /*!
     * \brief read the next code block and adds it tho the mechanical
     * behaviour
     * \param[in] n1    : name of the first method read
     * \param[in] n2    : name of the second method read
     * \param[in] m1    : modifier
     * \param[in] m2    : modifier
     * \param[in] b     : add "this->" in front of variables
     * \param[in] s     : allow specialisation
     */
    CodeBlockOptions treatCodeBlock(
        const std::string&,
        const std::string&,
        std::function<
            std::string(const Hypothesis, const std::string&, const bool)>,
        std::function<
            std::string(const Hypothesis, const std::string&, const bool)>,
        const bool,
        const bool);
    /*!
     * \brief read the next code block and adds it tho the mechanical
     * behaviour
     * \param[in] n1    : name of the first method read
     * \param[in] n2    : name of the second method read
     * \param[in] m1    : modifier
     * \param[in] m2    : modifier
     * \param[in] b     : add "this->" in front of variables
     */
    void treatCodeBlock(const CodeBlockOptions&,
                        const std::string&,
                        const std::string&,
                        std::function<std::string(
                            const Hypothesis, const std::string&, const bool)>,
                        std::function<std::string(
                            const Hypothesis, const std::string&, const bool)>,
                        const bool);
    /*!
     * \brief read the next code block for the given hypothesis
     * \param[in] h: hypothesis
     * \param[in] n: name of the method read
     * \param[in] m: modifier
     * \param[in] a: word analyser
     * \param[in] b: add "this->" in front of variables
     */
    CodeBlock readNextBlock(
        const Hypothesis,
        const std::string&,
        std::function<
            std::string(const Hypothesis, const std::string&, const bool)>,
        std::function<void(CodeBlock&, const Hypothesis, const std::string&)>,
        const bool);
    /*!
     * \brief read the next code block for the given hypothesis
     * \param[in] h: hypothesis
     * \param[in] n: name of the method read
     * \param[in] m: modifier
     * \param[in] b: add "this->" in front of variables
     */
    CodeBlock readNextBlock(
        const Hypothesis,
        const std::string&,
        std::function<
            std::string(const Hypothesis, const std::string&, const bool)>,
        const bool);
    //
    using DSLBase::readNextBlock;
    //! \brief throw an exception is some options were not recognized
    void treatUnsupportedCodeBlockOptions(const CodeBlockOptions&);
    //
    void addStaticVariableDescription(
        const StaticVariableDescription&) override;
    std::map<std::string, int> getIntegerConstants() const override;
    int getIntegerConstant(const std::string&) const override;
    /*!
     * \brief disable the declaration of new variables
     * \param[in] h : modelling hypothesis
     */
    virtual void disableVariableDeclaration();
    //! \brief method called at the end of the input file processing.
    virtual void completeVariableDeclaration();
    //
    void generateOutputFiles() override;
    //! \brief write the header files declaring the slip systems
    virtual void generateSlipSystemsFiles();
    //
    std::set<Hypothesis> getDefaultModellingHypotheses() const override;
    bool isModellingHypothesisSupported(const Hypothesis) const override;
    /*!
     * \brief the standard variable modifier
     * \param[in] h : modelling hypothesis
     * \param[in] v : variable name
     * \param[in] b : if true, shall add the "this" qualifier
     */
    virtual std::string standardModifier(const Hypothesis,
                                         const std::string&,
                                         const bool);

    virtual std::string predictionOperatorVariableModifier(const Hypothesis,
                                                           const std::string&,
                                                           const bool);

    virtual std::string tangentOperatorVariableModifier(const Hypothesis,
                                                        const std::string&,
                                                        const bool);
    /*!
     * \brief extract a material property from a token. If the token
     * is a string, it is interpred as a mfront file name. Otherwise,
     * the token is converted to a scalar.
     * \param[in] m: calling method
     * \param[in] t: token
     * \return a material property
     */
    BehaviourDescription::MaterialProperty extractMaterialProperty(
        const std::string&, const tfel::utilities::Token&);
    /*!
     * \brief read an an array of material properties. String are
     * interpreted as mfront file name. Other tokens are interpreted
     * as long double.
     * \param[in] m: calling method
     * \return the array of material properties
     */
    virtual std::vector<BehaviourDescription::MaterialProperty>
    readMaterialPropertyOrArrayOfMaterialProperties(const std::string& m);
    /*!
     *
     */
    virtual void readStringList(std::vector<std::string>&);
    /*!
     * \return a list of hypotheses
     * \note by default, the returning set contains UNDEFINEDHYPOTHESIS
     */
    virtual std::set<Hypothesis> readHypothesesList();
    /*!
     * read a list of hypotheses
     * \param[out] h : list of hypotheses
     * \note by default, the returning set contains UNDEFINEDHYPOTHESIS
     */
    virtual void readHypothesesList(std::set<Hypothesis>&);
    /*!
     * \brief append the given modelling hypothesis to the set of hypothesis
     * \param[out] h : list of hypotheses
     * \param[in]  v : hypothesis to be inserted
     */
    void appendToHypothesesList(std::set<Hypothesis>&,
                                const std::string&) const;
    /*!
     * First read a set of Hypothesis. Then read a list variables and
     * assign them to mechanical data associated with those hypotheses.
     * \param[out] v  : the declared variables
     * \param[out] h  : modelling hypothesis on which the variables were
     * declared
     * \param[in]  m  : method used to assign the variables
     * \param[in]  b1 : allows variables to be declared as array
     */
    virtual void readVariableList(
        VariableDescriptionContainer&,
        std::set<Hypothesis>&,
        void (BehaviourDescription::*)(const Hypothesis,
                                       const VariableDescriptionContainer&,
                                       const BehaviourData::RegistrationStatus),
        const bool);

    /*!
     * Assign a list variables to mechanical data associated with the given
     * hypotheses.
     * \param[out] h : modelling hypothesis on which the variables were
     * declared \param[out] v : the declared variables \param[in]  m : method
     * used to assign the variables
     */
    virtual void addVariableList(const std::set<Hypothesis>&,
                                 const VariableDescriptionContainer&,
                                 void (BehaviourDescription::*)(
                                     const Hypothesis,
                                     const VariableDescriptionContainer&,
                                     const BehaviourData::RegistrationStatus));

    /*!
     * set the interfaces to be used
     */
    void setInterfaces(const std::set<std::string>&) override;
    /*!
     * \brief register a name.
     * \param[in] n : name
     */
    void reserveName(const std::string&) override;
    /*!
     * \brief look if a name is reserved
     * \param[in] n : name
     */
    bool isNameReserved(const std::string&) const override;
    //! \brief register the default variable names
    virtual void registerDefaultVarNames();
    void setMaterial(const std::string&) override;
    void setMaterialKnowledgeIdentifier(const std::string&) override;
    //!\brief treat the `@Gradient` keyword.
    virtual void treatGradient();
    /*!
     * \brief treat the `@ThermodynamicForce` keyword. Also treat the `@Flux`
     * keyword.
     */
    virtual void treatThermodynamicForce();
    //!\brief treat the `@TangentOperatorBlock` keyword.
    virtual void treatTangentOperatorBlock();
    //!\brief treat the `@SpeedOfSound` keyword.
    virtual void treatSpeedOfSound();
    //!\brief treat the `@TangentOperatorBlocks` keyword.
    virtual void treatTangentOperatorBlocks();
    //!\brief treat the `@AdditionalTangentOperatorBlock` keyword.
    virtual void treatAdditionalTangentOperatorBlock();
    //!\brief treat the `@AdditionalTangentOperatorBlocks` keyword.
    virtual void treatAdditionalTangentOperatorBlocks();
    //!\brief treat the `@Brick` keyword
    virtual void treatBrick();
    //!\brief treat the `@Model` keyword
    virtual void treatModel();
    /*!
     * \brief alternative treatment of the `@Model` keyword
     *
     * This alternative treatment is meant to be used by simple point-wise
     * models.
     */
    virtual void treatModel2();
    /*!
     * \brief get a model description from an mfront file
     * \param[in] m: file
     * \return a model description
     */
    virtual ModelDescription getModelDescription(const std::string&);
    //! \brief treat the `@Private` keyword
    void treatPrivate() override;
    //! \brief treat the `@Members` keyword
    void treatMembers() override;
    //! \brief treat the `@InitializeFunction` keyword
    virtual void treatInitializeFunction();
    //! \brief treat the `@PostProcessing` keyword
    virtual void treatPostProcessing();
    //! \brief treat the `@StrainMeasure` keyword
    virtual void treatStrainMeasure();
    /*!
     * \brief treat the `@TangentOperator` keyword
     * \note this method read the code block options and determines
     * which tangent operator is computed (for finite strain behaviours)
     * and then calls the `readTangentOperatorCodeBlock` method
     */
    virtual void treatTangentOperator();
    /*!
     * \brief read a code block describing the computation of one of the
     * tangent operator
     * \param[in] o: code block options
     * \param[in] n: name of the code block
     */
    virtual void readTangentOperatorCodeBlock(const CodeBlockOptions&,
                                              const std::string&);
    //! \brief treat the `@IsTangentOperatorSymmetric` keyword
    virtual void treatIsTangentOperatorSymmetric();
    //! \brief treat the `@Library` keyword
    virtual void treatLibrary();
    //! \brief treat the `@Profiling` keyword
    virtual void treatProfiling();
    //! \brief treat the `@ModellingHypothesis` keyword
    virtual void treatModellingHypothesis();
    //! \brief treat the `@ModellingHypotheses` keyword
    virtual void treatModellingHypotheses();
    //! \brief treat the `@UpdateAuxiliaryStateVariables` keyword
    virtual void treatUpdateAuxiliaryStateVariables();
    //! \brief treat the `@Initialize` keyword
    virtual void treatInitialize();
    //! \brief treat the `@InternalEnergy` keyword
    virtual void treatInternalEnergy();
    //! \brief treat the `@DissipatedEnergy` keyword
    virtual void treatDissipatedEnergy();
    //! \brief treat the `@ComputeStressFreeExpansion` keyword
    virtual void treatComputeStressFreeExpansion();
    //! \brief treat the `@UsableInPurelyImplicitResolution` keyword
    virtual void treatUsableInPurelyImplicitResolution();
    //! \brief treat the `@Parameter` keyword
    virtual void treatParameter();
    //! \brief treat the `@LocalVariables` keyword
    virtual void treatLocalVar();
    //! handle the `@ComputeThermalExpansion` keyword
    virtual void treatComputeThermalExpansion();
    //! handle the `@ComputeStiffnessTensor` keyword
    virtual void treatComputeStiffnessTensor();
    //! handle the `@ElasticMaterialProperties` keyword
    virtual void treatElasticMaterialProperties();
    /*!
     * \brief read the elastic material properties and assign them to
     * the behaviour Description
     */
    virtual void readElasticMaterialProperties();
    //! \brief handle the `@HillTensor` keyword
    virtual void treatHillTensor();
    //! \brief handle the `@InitLocalVariables` keyword
    virtual void treatInitLocalVariables();
    //! \brief handle the `@OrthotropicBehaviour` keyword
    virtual void treatOrthotropicBehaviour();
    //! \brief handle the `@IsotropicElasticBehaiour` keyword
    virtual void treatIsotropicElasticBehaviour();
    //! \brief handle the `@IsotropicBehaviour` keyword
    virtual void treatIsotropicBehaviour();
    //! \brief handle the `@RequireStiffnessOperator` keyword
    virtual void treatRequireStiffnessOperator();
    //! \brief handle the `@RequireStiffnessTensor` keyword
    virtual void treatRequireStiffnessTensor();

    virtual void treatStiffnessTensorOption();
    //! \brief handle the `@RequireThermalExpansionCoefficientTensor` keyword
    virtual void treatRequireThermalExpansionCoefficientTensor();
    //! \brief handle the `@Behaviour` keyword
    virtual void treatBehaviour();
    //! \brief handle the `@Interface` keyword
    virtual void treatInterface();
    //! \brief handle the `@StateVariable` keyword
    virtual void treatStateVariable();
    //! \brief handle the `@AuxiliaryStateVariable` keyword
    virtual void treatAuxiliaryStateVariable();
    //! \brief handle the `@ExternalStateVariable` keyword
    virtual void treatExternalStateVariable();
    //! \brief handle the `@InitializeFunctionVariable` keyword
    virtual void treatInitializeFunctionVariable();
    //! \brief handle the `@PostProcessingVariable` keyword
    virtual void treatPostProcessingVariable();
    //! \brief treat the `@MinimalTimeStepScalingFactor` keyword
    virtual void treatMinimalTimeStepScalingFactor();
    //! \brief treat the `@MaximalTimeStepScalingFactor` keyword
    virtual void treatMaximalTimeStepScalingFactor();
    //! \brief treat the `@APrioriTimeStepScalingFactor` keyword
    virtual void treatAPrioriTimeStepScalingFactor();
    //! \brief treat the `@Integrator` keyword
    virtual void treatIntegrator();
    //! \brief treat the `@APosterioriTimeStepScalingFactor` keyword
    virtual void treatAPosterioriTimeStepScalingFactor();
    //! \brief treat the `@MaterialProperty` and the `@Coef` keywords
    virtual void treatCoef();
    //! \brief treat the `@UseQt` keyword
    virtual void treatUseQt();
    //! \brief treat the `@Bounds` keyword
    virtual void treatBounds();
    //! \brief treat the `@PhysicalBounds` keyword
    virtual void treatPhysicalBounds();
    //! \brief treat the `@PredictionOperator` keyword
    virtual void treatPredictionOperator();
    //! \brief treat the `@Swelling` keyword
    virtual void treatSwelling();
    //! \brief treat the `@AxialGrowth` keyword
    virtual void treatAxialGrowth();
    //! \brief treat the `@Relocation` keyword
    virtual void treatRelocation();
    //! \brief treat the `@CrystalStructure` keyword
    virtual void treatCrystalStructure();
    //! \brief treat the `@SlipSystem` keyword
    virtual void treatSlipSystem();
    //! \brief treat the `@SlipSystems` keyword
    virtual void treatSlipSystems();
    //! \brief treat the `@InteractionMatrix` keyword
    virtual void treatInteractionMatrix();
    /*!
     * \brief treat the `@DislocationsMeanFreePathInteractionMatrix`
     * keyword.
     */
    virtual void treatDislocationsMeanFreePathInteractionMatrix();
    /*!
     * \brief read a swelling description.
     *
     * An array is expected at the current point of the file. Each
     * token of the array is analysed throw the
     * readStressFreeExpansionHandler method.
     */
    virtual std::vector<BehaviourData::StressFreeExpansionHandler>
    readStressFreeExpansionHandler();
    /*!
     * \brief extract a swelling description from a token
     * \param[in] t: treated token
     *
     * - if the token is a string, a mfront file is treated.
     * - if the token is not a string, one expects an external state
     *   variable name
     */
    virtual BehaviourData::StressFreeExpansionHandler
    readStressFreeExpansionHandler(const tfel::utilities::Token&);
    //! \return the name of the behaviour file
    virtual std::string getBehaviourFileName() const;
    //! \return the name of the behaviour data file
    virtual std::string getBehaviourDataFileName() const;
    //! \return the name of the integration data file
    virtual std::string getIntegrationDataFileName() const;
    //! \return the name of the source file
    virtual std::string getSrcFileName() const;
    /*!
     * write the given variable declaration
     * \param[out] f                 : output file
     * \param[in]  v                 : variable to be declared
     * \param[in]  prefix            : prefix added to variable's names
     * \param[in]  suffix            : suffix added to variable's names
     * \param[in]  useTimeDerivative : declare time derivative of the
     * variables
     */
    virtual void writeVariableDeclaration(std::ostream&,
                                          const VariableDescription&,
                                          const std::string&,
                                          const std::string&,
                                          const std::string&,
                                          const bool) const;
    /*!
     * write the given variables declaration
     * \param[out] f                 : output file
     * \param[in]  v                 : variables to be declared
     * \param[in]  prefix            : prefix added to variable's names
     * \param[in]  suffix            : suffix added to variable's names
     * \param[in]  useTimeDerivative : declare time derivative of the
     * variables
     */
    virtual void writeVariablesDeclarations(std::ostream&,
                                            const VariableDescriptionContainer&,
                                            const std::string&,
                                            const std::string&,
                                            const std::string&,
                                            const bool) const;

    virtual void writeIncludes(std::ostream&) const;

    virtual void writeNamespaceBegin(std::ostream&) const;

    virtual void writeNamespaceEnd(std::ostream&) const;

    virtual void writeTypeAliases(std::ostream&) const;

    virtual void checkBehaviourDataFile(std::ostream&) const;

    virtual void writeBehaviourDataTypeAliases(std::ostream&) const;

    virtual void writeBehaviourDataStandardTFELIncludes(std::ostream&) const;

    virtual void writeBehaviourDataFileHeader(std::ostream&) const;

    virtual void writeBehaviourDataFileHeaderBegin(std::ostream&) const;

    virtual void writeBehaviourDataFileHeaderEnd(std::ostream&) const;

    virtual void writeBehaviourDataClassHeader(std::ostream&) const;

    virtual void writeBehaviourDataDisabledConstructors(std::ostream&) const;

    virtual void writeBehaviourDataConstructors(std::ostream&,
                                                const Hypothesis) const;
    /*!
     * write interface's setters for the main variables
     */
    virtual void writeBehaviourDataMainVariablesSetters(std::ostream&) const;

    virtual void writeBehaviourDataClassBegin(std::ostream&,
                                              const Hypothesis) const;

    virtual void writeBehaviourDataClassEnd(std::ostream&) const;

    virtual void writeBehaviourDataDefaultMembers(std::ostream&) const;

    virtual void writeBehaviourDataMaterialProperties(std::ostream&,
                                                      const Hypothesis) const;

    virtual void writeBehaviourDataStateVariables(std::ostream&,
                                                  const Hypothesis) const;

    virtual void writeBehaviourDataAssignementOperator(std::ostream&,
                                                       const Hypothesis) const;

    virtual void writeBehaviourDataOutputOperator(std::ostream&,
                                                  const Hypothesis) const;

    virtual void writeBehaviourDataInitializeMethods(std::ostream&,
                                                     const Hypothesis) const;

    virtual void writeBehaviourDataExport(std::ostream&,
                                          const Hypothesis) const;

    virtual void writeBehaviourDataPublicMembers(std::ostream&) const;

    virtual void writeBehaviourDataFileBegin(std::ostream&) const;

    virtual void writeBehaviourDataFileEnd(std::ostream&) const;

    virtual void writeBehaviourDataClass(std::ostream&, const Hypothesis) const;

    virtual void writeBehaviourDataForwardDeclarations(std::ostream&) const;

    virtual void checkIntegrationDataFile(std::ostream&) const;

    virtual void writeIntegrationDataTypeAliases(std::ostream&) const;

    virtual void writeIntegrationDataStandardTFELIncludes(std::ostream&) const;

    virtual void writeIntegrationDataFileHeader(std::ostream&) const;

    virtual void writeIntegrationDataFileHeaderBegin(std::ostream&) const;

    virtual void writeIntegrationDataFileHeaderEnd(std::ostream&) const;

    virtual void writeIntegrationDataClassHeader(std::ostream&) const;

    virtual void writeIntegrationDataDisabledConstructors(std::ostream&) const;

    virtual void writeIntegrationDataConstructors(std::ostream&,
                                                  const Hypothesis) const;
    /*!
     * write interface's setters for the main variables
     */
    virtual void writeIntegrationDataMainVariablesSetters(std::ostream&) const;

    virtual void writeIntegrationDataScaleOperators(std::ostream&,
                                                    const Hypothesis) const;

    virtual void writeIntegrationDataUpdateDrivingVariablesMethod(
        std::ostream&) const;

    virtual void writeIntegrationDataClassBegin(std::ostream&,
                                                const Hypothesis) const;

    virtual void writeIntegrationDataClassEnd(std::ostream&) const;

    virtual void writeIntegrationDataDefaultMembers(std::ostream&) const;

    virtual void writeIntegrationDataExternalStateVariables(
        std::ostream&, const Hypothesis) const;

    virtual void writeIntegrationDataFileBegin(std::ostream&) const;

    virtual void writeIntegrationDataFileEnd(std::ostream&) const;

    virtual void writeIntegrationDataClass(std::ostream&,
                                           const Hypothesis) const;

    virtual void writeIntegrationDataForwardDeclarations(std::ostream&) const;

    virtual void writeIntegrationDataOutputOperator(std::ostream&,
                                                    const Hypothesis) const;

    virtual void checkBehaviourFile(std::ostream&) const;

    virtual void writeBehaviourTypeAliases(std::ostream&) const;

    virtual void writeBehaviourFileHeader(std::ostream&) const;

    virtual void writeBehaviourFileHeaderBegin(std::ostream&) const;

    virtual void writeBehaviourFileHeaderEnd(std::ostream&) const;

    virtual void writeBehaviourFileBegin(std::ostream&) const;

    virtual void writeBehaviourFileEnd(std::ostream&) const;

    virtual void writeBehaviourClass(std::ostream&, const Hypothesis) const;

    virtual void writeBehaviourForwardDeclarations(std::ostream&) const;

    virtual void writeBehaviourProfiler(std::ostream&) const;

    virtual void writeBehaviourParserSpecificInheritanceRelationship(
        std::ostream&, const Hypothesis) const;

    virtual void writeBehaviourParserSpecificTypedefs(std::ostream&) const;

    virtual void writeBehaviourParserSpecificMembers(std::ostream&,
                                                     const Hypothesis) const;

    virtual void writeBehaviourParserSpecificIncludes(std::ostream&) const;

    virtual void writeBehaviourClassBegin(std::ostream&,
                                          const Hypothesis) const;

    virtual void writeBehaviourFriends(std::ostream&, const Hypothesis) const;

    virtual void writeBehaviourGetModellingHypothesis(std::ostream&) const;

    virtual void writeBehaviourClassEnd(std::ostream&) const;

    virtual void writeBehaviourOutOfBoundsPolicyVariable(std::ostream&) const;

    virtual void writeBehaviourSetOutOfBoundsPolicy(std::ostream&) const;

    virtual void writeBehaviourCheckBounds(std::ostream&,
                                           const Hypothesis) const;
    /*!
     * \brief write the checks associated to a bound
     * \param[out] os: output stream
     * \param[in]  v:  variable description
     * \param[in]  b:  if true, checks are written also on the variable
     * updated with its increment \note if the variable has no bounds, nothing
     * is done
     */
    virtual void writeBoundsChecks(std::ostream&,
                                   const VariableDescription&,
                                   const bool) const;
    /*!
     * \brief write the checks associated to a physical bound
     * \param[out] os: output stream
     * \param[in]  v:  variable description
     * \param[in]  b:  if true, checks are written also on the variable
     * updated with its increment \note if the variable has no physical
     * bounds, nothing is done
     */
    virtual void writePhysicalBoundsChecks(std::ostream&,
                                           const VariableDescription&,
                                           const bool) const;

    virtual void writeBehaviourDisabledConstructors(std::ostream&) const;

    virtual void writeBehaviourConstructors(std::ostream&,
                                            const Hypothesis) const;

    //! \return behaviour constructor initializers.
    virtual std::string getBehaviourConstructorsInitializers(
        const Hypothesis) const;
    //! \return local variables initalizers.
    virtual std::string getLocalVariablesInitializers(const Hypothesis) const;
    /*!
     * \brief write the arguments of a material property, including
     * the the surrounding parenthesis. Those arguments are used to
     * evaluate the material property and/or check its bounds.
     * \param[out] out: output stream
     * \param[in]  m:   material property description
     * \param[in]  f:   function converting input variable name.
     * The function f can be used to specify how evaluate a variable value.
     * For example, if we want to evaluate the variable name 'V' at
     * the end of the time step, we could make f return V+dV
     */
    virtual void writeExternalMFrontMaterialPropertyArguments(
        std::ostream&,
        const BehaviourDescription::MaterialProperty&,
        std::function<std::string(const MaterialPropertyInput&)>&) const;
    /*!
     * \brief write the bounds checks to a material property
     * \param[out] out: output stream
     * \param[in]  m:   material property description
     * \param[in]  f:   function converting input variable name.
     * The function f can be used to specify how evaluate a variable value.
     * For example, if we want to evaluate the variable name 'V' at
     * the end of the time step, we could make f return V+dV
     */
    virtual void writeMaterialPropertyCheckBoundsEvaluation(
        std::ostream&,
        const BehaviourDescription::MaterialProperty&,
        std::function<std::string(const MaterialPropertyInput&)>&) const;
    /*!
     * \brief write the call to a material property
     * \param[out] out: output stream
     * \param[in]  m:   material property description
     * \param[in]  f:   function converting input variable name.
     * The function f can be used to specify how evaluate a variable value.
     * For example, if we want to evaluate the variable name 'V' at
     * the end of the time step, we could make f return V+dV
     */
    void writeMaterialPropertyEvaluation(
        std::ostream&,
        const BehaviourDescription::MaterialProperty&,
        std::function<std::string(const MaterialPropertyInput&)>&)
        const override;
    /*!
     * \brief write the evoluation of a thermal expansion coefficient
     * \param[out] out: output stream
     * \param[in]  mpd: material property
     * \param[in] T:    temperature at which the thermal expansion
     * coefficient is computed
     * \param[in] idx:  index
     * \param[in] s:    suffix
     */
    virtual void writeThermalExpansionCoefficientComputation(
        std::ostream&,
        const BehaviourDescription::MaterialProperty&,
        const std::string&,
        const std::string&,
        const std::string&) const;
    virtual void writeThermalExpansionCoefficientsComputations(
        std::ostream&,
        const BehaviourDescription::MaterialProperty&,
        const std::string& = "") const;
    virtual void writeThermalExpansionComputation(
        std::ostream&,
        const std::string&,
        const std::string&,
        const std::string& = "") const;
    /*!
     * \brief write the behaviour's computeStressFreeExpansion method, if
     * mandatory.
     * \param[in] h : modelling hypothesis
     */
    virtual void writeBehaviourComputeStressFreeExpansion(
        std::ostream&, const Hypothesis) const;
    /*!
     * \brief write the stiffness tensor computation evaluation
     * from the elastic material properties.
     * \param[out] out: output stream
     * \param[in]  D:   name of the stiffness tensor variable
     * \param[in]  f:   function used to handle the variables of the material
     * properties.
     */
    virtual void writeStiffnessTensorComputation(
        std::ostream&,
        const std::string&,
        std::function<std::string(const MaterialPropertyInput&)>&) const;
    /*!
     * \brief write the Hill tensor computation evaluation
     * from the elastic material properties.
     * \param[out] out: output stream
     * \param[in]  H:   name of the Hill tensor variable to be computed
     * \param[in]  h:   Hill tensor definition
     * \param[in]  f:   function used to handle the variables of the material
     * properties.
     */
    virtual void writeHillTensorComputation(
        std::ostream&,
        const std::string&,
        const BehaviourDescription::HillTensor&,
        std::function<std::string(const MaterialPropertyInput&)>&) const;
    /*!
     * \brief write the initalize methods.
     *
     * It always create an `initialize` method which contains `.
     * This method is called after that the main variables were set.
     */
    virtual void writeBehaviourInitializeMethods(std::ostream&,
                                                 const Hypothesis) const;
    /*!
     * write part of the constructor specific to the parser
     * \param[in] h : modelling hypothesis
     */
    virtual void writeBehaviourParserSpecificInitializeMethodPart(
        std::ostream&, const Hypothesis) const;

    /*!
     * \param[in] h: modelling hypothesis
     */
    virtual std::string getIntegrationVariablesIncrementsInitializers(
        const Hypothesis) const;

    virtual void writeBehaviourIntegrationVariablesIncrements(
        std::ostream&, const Hypothesis) const;

    virtual void writeBehaviourLocalVariables(std::ostream&,
                                              const Hypothesis) const;

    virtual void writeBehaviourIntegrationVariables(std::ostream&,
                                                    const Hypothesis) const;

    virtual void writeBehaviourParameters(std::ostream&,
                                          const Hypothesis) const;

    virtual void writeBehaviourStaticVariables(std::ostream&,
                                               const Hypothesis) const;

    virtual void writeBehaviourAdditionalMembers(std::ostream&,
                                                 const Hypothesis) const;

    virtual void writeBehaviourPrivate(std::ostream&, const Hypothesis) const;

    virtual void writeBehaviourUpdateIntegrationVariables(
        std::ostream&, const Hypothesis) const;

    virtual void writeBehaviourUpdateStateVariables(std::ostream&,
                                                    const Hypothesis) const;

    virtual void writeBehaviourUpdateAuxiliaryStateVariables(
        std::ostream&, const Hypothesis) const;
    /*!
     * \brief write the computeInternalEnergy method
     * \param[in] h: modelling hypothesis
     */
    virtual void writeBehaviourComputeInternalEnergy(std::ostream&,
                                                     const Hypothesis) const;
    /*!
     * \brief write the computeDissipatedEnergy method
     * \param[in] h: modelling hypothesis
     */
    virtual void writeBehaviourComputeDissipatedEnergy(std::ostream&,
                                                       const Hypothesis) const;
    /*!
     * \brief write the computeSpeedOfSound method
     * \param[in] h: modelling hypothesis
     */
    virtual void writeBehaviourComputeSpeedOfSound(std::ostream&,
                                                   const Hypothesis) const;
    //! \brief write the getTimeStepScalingFactor method
    virtual void writeBehaviourGetTimeStepScalingFactor(std::ostream&) const;
    //! \brief write the integrate method
    virtual void writeBehaviourIntegrator(std::ostream&,
                                          const Hypothesis) const;
    /*!
     * \brief write the computeAPrioriTimeStepsScalingFactor method
     */
    virtual void writeBehaviourComputeAPrioriTimeStepScalingFactor(
        std::ostream&) const;
    /*!
     * \brief write the computeAPrioriTimeStepsScalingFactorII method
     */
    virtual void writeBehaviourComputeAPrioriTimeStepScalingFactorII(
        std::ostream&, const Hypothesis) const;
    /*!
     * \brief write the computeAPosterioriTimeStepsScalingFactor method
     */
    virtual void writeBehaviourComputeAPosterioriTimeStepScalingFactor(
        std::ostream&) const;
    /*!
     * \brief write the computeAPosterioriTimeStepsScalingFactorII method
     */
    virtual void writeBehaviourComputeAPosterioriTimeStepScalingFactorII(
        std::ostream&, const Hypothesis) const;

    virtual void writeBehaviourUpdateExternalStateVariables(
        std::ostream&, const Hypothesis) const;

    virtual void writeBehaviourInitializeFunctions(std::ostream&,
                                                   const Hypothesis) const;

    virtual void writeBehaviourPostProcessings(std::ostream&,
                                               const Hypothesis) const;

    virtual void writeBehaviourOutputOperator(std::ostream&,
                                              const Hypothesis) const;

    virtual void writeBehaviourDestructor(std::ostream&) const;

    virtual void writeBehaviourTraits(std::ostream&) const;

    /*!
     * \param[out] out: os file stream
     * \param[in]  h:   modelling hypothesis
     * \param[in]  b    true if the behaviour is defined for the given
     * modelling hypothesis
     */
    virtual void writeBehaviourTraitsSpecialisation(std::ostream&,
                                                    const Hypothesis,
                                                    const bool) const;

    virtual void writeBehaviourIncludes(std::ostream&) const;

    virtual void writeBehaviourLocalVariablesInitialisation(
        std::ostream&, const Hypothesis) const;

    virtual void writeBehaviourParameterInitialisation(std::ostream&,
                                                       const Hypothesis) const;

    virtual void writeBehaviourParametersInitializers(std::ostream&) const;

    virtual void writeBehaviourParametersInitializer(std::ostream&,
                                                     const Hypothesis) const;

    virtual void checkSrcFile(std::ostream&) const;

    virtual void writeSrcFileHeader(std::ostream&) const;

    virtual void writeSrcFileUserDefinedCode(std::ostream&) const;

    virtual void writeSrcFileBehaviourProfiler(std::ostream&) const;

    virtual void writeSrcFileParametersInitializers(std::ostream&) const;

    virtual void writeSrcFileParametersInitializer(std::ostream&,
                                                   const Hypothesis) const;

    /*!
     * \brief write the source file
     */
    virtual void writeSrcFile(std::ostream&) const;

    virtual void writeBehaviourComputePredictionOperator(
        std::ostream&, const Hypothesis) const;

    /*!
     * \brief write the methods associated with the computation of the tangent
     * operator
     * \param[in,out] os: output file stream
     * \param[in] h: modelling hypothesis
     * \note this method calls `writeBehaviourComputeTangentOperatorBody` to
     * write the body of the method. The body of the method follows a preamble
     * containing some `using` statements (for `std` and `tfel::math`
     * namespaces) and the declaration of material laws.
     */
    virtual void writeBehaviourComputeTangentOperator(std::ostream&,
                                                      const Hypothesis) const;
    /*!
     * \brief write the body of a method computing the tangent operator
     * \param[in,out] os: output file stream
     * \param[in] h: modelling hypothesis
     * \param[in] n: code block name
     */
    virtual void writeBehaviourComputeTangentOperatorBody(
        std::ostream&, const Hypothesis, const std::string&) const;

    //! \brief write the code returning the tangent operator
    virtual void writeBehaviourGetTangentOperator(std::ostream&) const;
    /*!
     * \brief write the code declaring with the tangent operator and its
     * blocks, if required.
     */
    virtual void writeBehaviourTangentOperator(std::ostream&) const;
    /*!
     * \brief write the call to a model
     * \param[out] out: output stream
     * \param[in,out] tmpnames: temporary names
     * \param[in]  h:   hypothesis
     * \param[in]  md:  model description
     * \param[in]  vo:  name of the variable containing the result
     * \param[in]  vs:  name of the variable containing the value of the
     *                  variable modified by the model at the beginning
     *                  of the time step
     * \param[in]  bn:  base name for temporary variable
     */
    virtual void writeModelCall(std::ostream&,
                                std::vector<std::string>&,
                                const Hypothesis,
                                const ModelDescription&,
                                const std::string&,
                                const std::string&,
                                const std::string&) const;
    //! \brief treat methods associated with a gradient
    virtual void treatGradientMethod();
    //! \brief treat methods associated with a thermodynamic force
    virtual void treatThermodynamicForceMethod();
    /*!
     * \brief treat methods associated with a parameter
     * \param[in] h : modelling hypothesis
     */
    virtual void treatParameterMethod(const Hypothesis);
    //! \brief analyse the `setGlossaryNameMethod` and returns its argument
    virtual std::string treatSetGlossaryNameMethod();
    //! \brief analyse the `setEntryNameMethod` and returns its argument
    virtual std::string treatSetEntryNameMethod();
    /*!
     * \return true if the the given variable may have methods
     * \param[in] h : modelling hypothesis
     * \param[in] n : name
     */
    virtual bool isCallableVariable(const Hypothesis, const std::string&) const;
    /*!
     * treat a method
     * \param[in] h : modelling hypothesis
     */
    virtual void treatVariableMethod(const Hypothesis);
    /*!
     * \param[in] h : modelling hypothesis
     * \param[in] n : variable name
     */
    virtual void treatUnknownVariableMethod(const Hypothesis,
                                            const std::string&);
    //! method called when an unknown keyword is parsed
    void treatUnknownKeyword() override;
    //! destructor
    ~BehaviourDSLCommon() override;
    /*!
     * \param[in] h : modelling hypothesis
     * \param[in] n : variable name
     */
    virtual void
    declareExternalStateVariableProbablyUnusableInPurelyImplicitResolution(
        const Hypothesis, const std::string&);
    /*!
     * \brief if no tangent operator was provided, but that the
     * behaviour requires a stiffness matrix, this method creates a
     * minimal tangent operator for elasticity.
     * \note This method is not trivial because one has to take care
     * not to create artifical mechanical data specialisation
     * \note This method is meant to be used in the
     * endsInputFileProcessing method.
     */
    virtual void setMinimalTangentOperator();
    /*!
     * \brief if the compte final thermodynamic forces code is not available,
     * create it from the ComputeFinalThermodynamicForcesCandidate code if it
     * is available. \note This method is not trivial because one has to take
     * care not to create artifical mechanical data specialisation \note This
     * method is meant to be used in the endsInputFileProcessing method.
     */
    virtual void
    setComputeFinalThermodynamicForcesFromComputeFinalThermodynamicForcesCandidateIfNecessary();
    /*!
     * \brief perform pedantic checks
     */
    virtual void doPedanticChecks() const;
    /*!
     * \return true if the user defined a block of code computing the
     * tangent operator
     * \param[in] h : modelling hypothesis
     */
    virtual bool hasUserDefinedTangentOperatorCode(const Hypothesis) const;
    //! \brief behaviour description
    BehaviourDescription mb;
    //! \brief registred bricks
    std::vector<std::shared_ptr<AbstractBehaviourBrick>> bricks;
    //! \brief the list of registred keywords
    std::set<std::string> registredKeyWords;
    //! \brief list of registred interfaces, indexed by their name
    std::map<std::string, std::shared_ptr<AbstractBehaviourInterface>>
        interfaces;
    //! \brief list of call backs
    std::map<std::string, CallBack> callBacks;
    //! \brief hooks assigned to callbacks
    std::map<std::string, std::vector<Hook>> hooks;
    //! \brief list of declared gradients
    std::vector<Gradient> gradients;
    //! \brief list of declared thermodynamic forces
    std::vector<ThermodynamicForce> thermodynamic_forces;

    bool useStateVarTimeDerivative;
    bool explicitlyDeclaredUsableInPurelyImplicitResolution;
  };  // end of struct BehaviourDSLCommon

}  // end of namespace mfront

#include "MFront/BehaviourDSLCommon.ixx"

#endif /* LIB_MFRONT_MFRONTBEHAVIOURDSLCOMMON_HXX */
