/*!
 * \file   mfront/src/SymbolsGenerator.cxx
 * \brief
 * \author Thomas Helfer
 * \date   08/07/2018
 * \copyright Copyright (C) 2006-2018 CEA/DEN, EDF R&D. All rights
 * reserved.
 * This project is publicly released under either the GNU GPL Licence
 * or the CECILL-A licence. A copy of thoses licences are delivered
 * with the sources of TFEL. CEA or EDF may also distribute this
 * project under specific licensing conditions.
 */

#include <sstream>
#include "TFEL/Raise.hxx"
#include "TFEL/Config/GetTFELVersion.h"
#include "TFEL/Utilities/StringAlgorithms.hxx"
#include "TFEL/System/System.hxx"
#include "MFront/DSLUtilities.hxx"
#include "MFront/FileDescription.hxx"
#include "MFront/BehaviourDescription.hxx"
#include "MFront/BehaviourMaterialProperty.hxx"
#include "MFront/StandardBehaviourInterface.hxx"
#include "MFront/SymbolsGenerator.hxx"

namespace mfront {

  static bool shallRemoveTemperatureFromExternalStateVariables(
      const BehaviourDescription& bd) {
    return bd.isTemperatureDefinedAsTheFirstExternalStateVariable();
  }  // end of shallRemoveTemperatureFromExternalStateVariables

  std::string SymbolsGenerator::getSymbolName(
      const StandardBehaviourInterface& i,
      const std::string& n,
      const Hypothesis h) const {
    if (h != ModellingHypothesis::UNDEFINEDHYPOTHESIS) {
      return i.getFunctionNameBasis(n) + "_" + ModellingHypothesis::toString(h);
    }
    return i.getFunctionNameBasis(n);
  }  // end of getSymbolName

  void SymbolsGenerator::generateGeneralSymbols(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& bd,
      const FileDescription& fd,
      const std::set<Hypothesis>& mhs,
      const std::string& name) const {
    this->writeFileDescriptionSymbols(out, i, fd, name);
    this->writeBuildIdentifierSymbol(out, i, bd, name);
    this->writeEntryPointSymbol(out, i, name);
    this->writeTFELVersionSymbol(out, i, name);
    this->writeMaterialSymbol(out, i, bd, name);
    this->writeMaterialKnowledgeTypeSymbol(out, i, name);
    this->writeInterfaceNameSymbols(out, i, bd, fd, name);
    this->writeSourceFileSymbols(out, i, bd, fd, name);
    this->writeSupportedModellingHypothesis(out, i, bd, mhs, name);
    this->writeMainVariablesSymbols(out, i, bd, name);
    this->writeTangentOperatorSymbols(out, i, bd, name);
    this->writeBehaviourTypeSymbols(out, i, bd, name);
    this->writeBehaviourKinematicSymbols(out, i, bd, name);
    this->writeSymmetryTypeSymbols(out, i, bd, name);
    this->writeElasticSymmetryTypeSymbols(out, i, bd, name);
    this->writeSpecificSymbols(out, i, bd, fd, name);
    this->writeTemperatureRemovedFromExternalStateVariablesSymbol(out, i, bd,
                                                                  name);
  }

  void SymbolsGenerator::writeFileDescriptionSymbols(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const FileDescription& fd,
      const std::string& n) const {
    mfront::writeFileDescriptionSymbols(out, i.getFunctionNameBasis(n), fd);
  }  // end of writeFileDescriptionSymbols

  void SymbolsGenerator::writeBuildIdentifierSymbol(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& d,
      const std::string& n) const {
    mfront::writeBuildIdentifierSymbol(out, i.getFunctionNameBasis(n), d);
  }  // end of writeBuildIdentifierSymbol

  void SymbolsGenerator::writeEntryPointSymbol(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const std::string& n) const {
    mfront::writeEntryPointSymbol(out, i.getFunctionNameBasis(n));
  }  // end of writeEntryPointSymbol

  void SymbolsGenerator::writeTFELVersionSymbol(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const std::string& n) const {
    mfront::writeTFELVersionSymbol(out, i.getFunctionNameBasis(n));
  }  // end of writeTFELVersionSymbol

  void SymbolsGenerator::writeMaterialSymbol(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& mb,
      const std::string& n) const {
    mfront::writeMaterialSymbol(out, i.getFunctionNameBasis(n),
                                mb.getMaterialName());
  }  // end of writeEntryPointSymbol

  void SymbolsGenerator::writeMainVariablesSymbols(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& bd,
      const std::string& name) const {
    const auto& mvs = bd.getMainVariables();
    std::vector<std::string> dvnames;
    std::vector<int> dvtypes;
    std::vector<std::string> thnames;
    std::vector<int> thtypes;
    auto nmvs = decltype(mvs.size()){};
    for (const auto& mv : mvs) {
      const auto& dv = mv.first;
      const auto& th = mv.second;
      if (dv.arraySize != th.arraySize) {
        tfel::raise(
            "SymbolsGenerator::writeMainVariablesSymbols: "
            "array size for driving variable '" +
            dv.name +
            "' "
            "does not match the array size of the associated "
            "thermodynamic forces '" +
            th.name + "'");
      }
      const auto dvname = dv.getExternalName();
      const auto dvtype = dv.getVariableTypeIdentifier();
      const auto thname = th.getExternalName();
      const auto thtype = th.getVariableTypeIdentifier();
      if (dvname == "Strain") {
        if (thname != "Stress") {
          tfel::raise(
              "SymbolsGenerator::writeMainVariablesSymbols: "
              "the thermodynamic force associated with the "
              "strain is not the stress");
        }
        if (dvtype != 1) {
          tfel::raise(
              "SymbolsGenerator::writeMainVariablesSymbols: "
              "invalid type for the strain");
        }
        if (thtype != 1) {
          tfel::raise(
              "SymbolsGenerator::writeMainVariablesSymbols: "
              "invalid type for the stress");
        }
        if (dv.arraySize != 1) {
          tfel::raise(
              "SymbolsGenerator::writeMainVariablesSymbols: "
              "the strain shall not be declared as an array");
        }
        if (th.arraySize != 1) {
          tfel::raise(
              "SymbolsGenerator::writeMainVariablesSymbols: "
              "the stress shall not be declared as an array");
        }
        if ((bd.isStrainMeasureDefined()) &&
            (bd.getStrainMeasure() != BehaviourDescription::LINEARISED)) {
          if (this->handleStrainMeasure()) {
            // strain measure is handled by the interface
            dvnames.push_back("DeformationGradient");
            thnames.push_back("Stress");
            dvtypes.push_back(3);
            thtypes.push_back(thtype);
          } else {
            // strain measure is handled by the calling finite element solver
            dvnames.push_back(dvname);
            thnames.push_back(thname);
            dvtypes.push_back(dvtype);
            thtypes.push_back(thtype);
          }
        } else {
          dvnames.push_back(dvname);
          thnames.push_back(thname);
          dvtypes.push_back(dvtype);
          thtypes.push_back(thtype);
        }
      } else {
        if (dv.arraySize == 1u) {
          dvnames.push_back(dvname);
          thnames.push_back(thname);
          dvtypes.push_back(dvtype);
          thtypes.push_back(thtype);
        } else {
          for (unsigned short idx = 0; idx != dv.arraySize; ++idx) {
            std::ostringstream nb;
            nb << '[' << idx << ']';
            dvnames.push_back(dvname + nb.str());
            thnames.push_back(thname + nb.str());
            dvtypes.push_back(dvtype);
            thtypes.push_back(thtype);
          }
        }
      }
      nmvs += dv.arraySize;
    }
    const auto fn = i.getFunctionNameBasis(name);
    out << "MFRONT_SHAREDOBJ unsigned short " << fn
        << "_nMainVariables = " << nmvs << ";\n";
    out << "MFRONT_SHAREDOBJ unsigned short " << fn << "_nGradients = " << nmvs
        << ";\n\n";
    this->writeArrayOfIntsSymbol(out, fn + "_GradientsTypes", dvtypes);
    this->writeArrayOfStringsSymbol(out, fn + "_Gradients", dvnames);
    out << "MFRONT_SHAREDOBJ unsigned short " << fn
        << "_nThermodynamicForces = " << nmvs << ";\n\n";
    this->writeArrayOfIntsSymbol(out, fn + "_ThermodynamicForcesTypes",
                                 thtypes);
    this->writeArrayOfStringsSymbol(out, fn + "_ThermodynamicForces", thnames);
  }  // end of writeMainVariablesSymbols

  void SymbolsGenerator::writeTangentOperatorSymbols(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& bd,
      const std::string& name) const {
    auto write_impl = [this, &out, &i,
                       &name](const std::vector<std::string>& bns) {
      const auto fn = i.getFunctionNameBasis(name);
      out << "MFRONT_SHAREDOBJ unsigned short " << fn
          << "_nTangentOperatorBlocks = " << bns.size() << ";\n\n";
      this->writeArrayOfStringsSymbol(out, fn + "_TangentOperatorBlocks", bns);
    };
    auto default_impl = [&bd, &write_impl] {
      std::vector<std::string> bns;
      for (const auto& b : bd.getTangentOperatorBlocks()) {
        bns.push_back(b.first.getExternalName());
        bns.push_back(b.second.getExternalName());
      }
      write_impl(bns);
    };
    auto empty_impl = [this, &i, &name, &out] {
      // strain measure is handled by the interface
      // we don't write any tangent operator here
      // as the consistent tangent operator
      // in finite strain is very dependant of the
      // interface is generally not related
      // the derivative of a flux with respect to
      // a gradient
      const auto fn = i.getFunctionNameBasis(name);
      out << "MFRONT_SHAREDOBJ unsigned short " << fn
          << "_nTangentOperatorBlocks = 0;\n\n";
      this->writeArrayOfStringsSymbol(out, fn + "_TangentOperatorBlocks", {});
    };
    if (bd.getBehaviourType() ==
        BehaviourDescription::STANDARDSTRAINBASEDBEHAVIOUR) {
      if ((bd.isStrainMeasureDefined()) &&
          (bd.getStrainMeasure() != BehaviourDescription::LINEARISED)) {
        if (this->handleStrainMeasure()) {
          empty_impl();
        } else {
          default_impl();
        }
      } else {
        default_impl();
      }
    } else if (bd.getBehaviourType() ==
               BehaviourDescription::STANDARDFINITESTRAINBEHAVIOUR) {
      empty_impl();
    } else {
      default_impl();
    }
  }  // end of writeTangentOperatorSymbols

  void SymbolsGenerator::writeMaterialKnowledgeTypeSymbol(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const std::string& n) const {
    mfront::writeMaterialKnowledgeTypeSymbol(out, i.getFunctionNameBasis(n),
                                             BEHAVIOUR);
  }  // end of writeEntryPointSymbol

  void SymbolsGenerator::generateSymbols(std::ostream& out,
                                         const StandardBehaviourInterface& i,
                                         const BehaviourDescription& bd,
                                         const FileDescription& fd,
                                         const std::string& name,
                                         const Hypothesis h) const {
    this->writeIsUsableInPurelyImplicitResolutionSymbols(out, i, bd, name, h);
    this->writeMaterialPropertiesSymbols(out, i, bd, name, h);
    this->writeStateVariablesSymbols(out, i, bd, name, h);
    this->writeExternalStateVariablesSymbols(out, i, bd, name, h);
    this->writeParametersSymbols(out, i, bd, name, h);
    this->writeParameterDefaultValueSymbols(out, i, bd, name, h);
    this->writeBoundsSymbols(out, i, bd, name, h);
    this->writePhysicalBoundsSymbols(out, i, bd, name, h);
    this->writeRequirementsSymbols(out, i, bd, name, h);
    this->writeInitializeFunctionsSymbols(out, i, bd, name, h);
    this->writePostProcessingsSymbols(out, i, bd, name, h);
    this->writeAdditionalSymbols(out, i, bd, fd, name, h);
    this->writeComputesInternalEnergySymbol(out, i, bd, name, h);
    this->writeComputesDissipatedEnergySymbol(out, i, bd, name, h);
  }

  void SymbolsGenerator::writeComputesInternalEnergySymbol(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& bd,
      const std::string& name,
      const Hypothesis h) const {
    const auto b = bd.hasCode(h, BehaviourData::ComputeInternalEnergy) ? 1 : 0;
    out << "MFRONT_SHAREDOBJ unsigned short " << this->getSymbolName(i, name, h)
        << "_ComputesInternalEnergy = " << b << ";\n\n";
  }  // end of writeComputesInternalEnergySymbol

  void SymbolsGenerator::writeComputesDissipatedEnergySymbol(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& bd,
      const std::string& name,
      const Hypothesis h) const {
    const auto b =
        bd.hasCode(h, BehaviourData::ComputeDissipatedEnergy) ? 1 : 0;
    out << "MFRONT_SHAREDOBJ unsigned short " << this->getSymbolName(i, name, h)
        << "_ComputesDissipatedEnergy = " << b << ";\n\n";
  }  // end of writeComputesDissipatedEnergySymbol

  void SymbolsGenerator::writeTemperatureRemovedFromExternalStateVariablesSymbol(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& bd,
      const std::string& name) const {
    if (shallRemoveTemperatureFromExternalStateVariables(bd)) {
      out << "MFRONT_SHAREDOBJ unsigned short " << i.getFunctionNameBasis(name)
          << "_TemperatureRemovedFromExternalStateVariables = 1u;\n";
    } else {
      out << "MFRONT_SHAREDOBJ unsigned short " << i.getFunctionNameBasis(name)
          << "_TemperatureRemovedFromExternalStateVariables = 0u;\n";
    }
  }  // end of writeTemperatureRemovedFromExternalStateVariablesSymbol

  void SymbolsGenerator::writeSpecificSymbols(std::ostream&,
                                              const StandardBehaviourInterface&,
                                              const BehaviourDescription&,
                                              const FileDescription&,
                                              const std::string&) const {
  }  // end of writeSpecificSymbols

  void SymbolsGenerator::writeSupportedModellingHypothesis(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription&,
      const std::set<Hypothesis>& mhs,
      const std::string& name) const {
    if (mhs.empty()) {
      out << "MFRONT_SHAREDOBJ unsigned short " << i.getFunctionNameBasis(name)
          << "_nModellingHypotheses = 0u;\n\n"
          << "MFRONT_SHAREDOBJ const char * const * "
          << i.getFunctionNameBasis(name) << "_ModellingHypotheses = 0;\n\n";
    } else {
      out << "MFRONT_SHAREDOBJ unsigned short " << i.getFunctionNameBasis(name)
          << "_nModellingHypotheses = " << mhs.size() << "u;\n\n"
          << "MFRONT_SHAREDOBJ const char * \n"
          << i.getFunctionNameBasis(name) << "_ModellingHypotheses["
          << mhs.size() << "u] = {";
      for (auto ph = mhs.begin(); ph != mhs.end();) {
        out << "\"" << ModellingHypothesis::toString(*ph) << "\"";
        if (++ph != mhs.end()) {
          out << ",\n";
        }
      }
      out << "};\n\n";
    }
  }  // end of writeSupportedModellingHypothesis

  void SymbolsGenerator::writeMaterialPropertiesSymbols(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& mb,
      const std::string& name,
      const Hypothesis h) const {
    using namespace std;
    auto throw_if = [](const bool b, const std::string_view m) {
      if (b) {
        tfel::raise("SymbolsGenerator::writeMaterialPropertiesSymbols: " +
                    std::string{m});
      }
    };
    const auto mprops = i.buildMaterialPropertiesList(mb, h);
    for (const auto& mp : mprops.first) {
      if (SupportedTypes::getTypeFlag(mp.type) != SupportedTypes::SCALAR) {
        throw_if(
            true,
            "internal error: the material properties shall all be scalars");
      }
    }
    if (mprops.first.empty()) {
      out << "MFRONT_SHAREDOBJ unsigned short "
          << this->getSymbolName(i, name, h) << "_nMaterialProperties = 0u;\n\n"
          << "MFRONT_SHAREDOBJ const char * const *"
          << this->getSymbolName(i, name, h)
          << "_MaterialProperties = nullptr;\n\n";
    } else {
      const auto& last = mprops.first.back();
      SupportedTypes::TypeSize s;
      s = last.offset;
      s += SupportedTypes::getTypeSize(last.type, last.arraySize);
      s -= mprops.second;
      vector<BehaviourMaterialProperty>::size_type ib =
          0; /* index of the first element which
              * is not imposed by the material properties */
      bool found = false;
      for (decltype(mprops.first.size()) idx = 0;
           (idx != mprops.first.size()) && (!found); ++idx) {
        if (mprops.first[idx].offset == mprops.second) {
          ib = idx;
          found = true;
        }
      }
      if (!found) {
        out << "MFRONT_SHAREDOBJ unsigned short "
            << this->getSymbolName(i, name, h)
            << "_nMaterialProperties = 0u;\n\n";
        out << "MFRONT_SHAREDOBJ const char * const *"
            << this->getSymbolName(i, name, h)
            << "_MaterialProperties = nullptr;\n\n";
      } else {
        out << "MFRONT_SHAREDOBJ unsigned short "
            << this->getSymbolName(i, name, h) << "_nMaterialProperties = " << s
            << "u;\n\n";
        out << "MFRONT_SHAREDOBJ const char *"
            << this->getSymbolName(i, name, h) << "_MaterialProperties[" << s
            << "u] = {";
        for (auto idx = ib; idx != mprops.first.size();) {
          const auto& m = mprops.first[idx];
          if (m.arraySize == 1u) {
            out << "\"" << m.name << "\"";
          } else {
            for (unsigned short j = 0; j != m.arraySize;) {
              out << "\"" << m.name << "[" << j << "]\"";
              if (++j != m.arraySize) {
                out << ",\n";
              }
            }
          }
          if (++idx != mprops.first.size()) {
            out << ",\n";
          }
        }
        out << "};\n\n";
      }
    }
  }  // end of UMATInterface::writeMaterialPropertiesSymbol

  void SymbolsGenerator::writeVariablesTypesSymbol(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const std::string& name,
      const Hypothesis h,
      const VariableDescriptionContainer& variables,
      const std::string& variables_identifier) const {
    if (!variables.empty()) {
      out << "MFRONT_SHAREDOBJ int " << this->getSymbolName(i, name, h) << "_"
          << variables_identifier << "Types [] = {";
      for (auto p = variables.begin(); p != variables.end();) {
        const auto t = p->getVariableTypeIdentifier();
        for (unsigned short is = 0; is != p->arraySize;) {
          out << t;
          if (++is != p->arraySize) {
            out << ",";
          }
        }
        if (++p != variables.end()) {
          out << ",";
        }
      }
      out << "};\n\n";
    } else {
      out << "MFRONT_SHAREDOBJ const int * " << this->getSymbolName(i, name, h)
          << "_" << variables_identifier << "Types  = nullptr;\n\n";
    }
  }  // end of writeVariablesTypesSymbol

  void SymbolsGenerator::writeStateVariablesSymbols(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& mb,
      const std::string& name,
      const Hypothesis h) const {
    const auto& d = mb.getBehaviourData(h);
    const auto& persistentVarsHolder = d.getPersistentVariables();
    const unsigned short nStateVariables = static_cast<unsigned short>(
        persistentVarsHolder.getNumberOfVariables());
    out << "MFRONT_SHAREDOBJ unsigned short " << this->getSymbolName(i, name, h)
        << "_nInternalStateVariables = " << nStateVariables << ";\n";
    std::vector<std::string> stateVariablesNames;
    mb.getExternalNames(stateVariablesNames, h, persistentVarsHolder);
    this->writeExternalNames(out, i, name, h, stateVariablesNames,
                             "InternalStateVariables");
    this->writeVariablesTypesSymbol(out, i, name, h, persistentVarsHolder,
                                    "InternalStateVariables");
  }  // end of writeStateVariablesSymbols

  void SymbolsGenerator::writeExternalStateVariablesSymbols(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& mb,
      const std::string& name,
      const Hypothesis h) const {
    const auto& d = mb.getBehaviourData(h);
    auto esvs = d.getExternalStateVariables();
    if (shallRemoveTemperatureFromExternalStateVariables(mb)) {
      // removing the temperature
      esvs.erase(esvs.begin());
    }
    out << "MFRONT_SHAREDOBJ unsigned short " << this->getSymbolName(i, name, h)
        << "_nExternalStateVariables = " << esvs.getNumberOfVariables()
        << ";\n";
    this->writeExternalNames(out, i, name, h, mb.getExternalNames(h, esvs),
                             "ExternalStateVariables");
    this->writeVariablesTypesSymbol(out, i, name, h, esvs,
                                    "ExternalStateVariables");
  }  // end of writeExternalStateVariablesSymbols

  void SymbolsGenerator::writeInitializeFunctionsSymbols(
      std::ostream&,
      const StandardBehaviourInterface&,
      const BehaviourDescription&,
      const std::string&,
      const Hypothesis) const {}  // end of writeInitializeFunctionsSymbols

  void SymbolsGenerator::writePostProcessingsSymbols(
      std::ostream&,
      const StandardBehaviourInterface&,
      const BehaviourDescription&,
      const std::string&,
      const Hypothesis) const {}  // end of writePostProcessingsSymbols

  void SymbolsGenerator::writeParametersSymbols(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& mb,
      const std::string& name,
      const Hypothesis h) const {
    if (!areParametersTreatedAsStaticVariables(mb)) {
      mfront::writeParametersDeclarationSymbols(
          out, this->getSymbolName(i, name, h),
          mb.getBehaviourData(h).getParameters());
    } else {
      mfront::writeParametersDeclarationSymbols(
          out, this->getSymbolName(i, name, h), {});
    }
  }  // end of writeParametersSymbols

  void SymbolsGenerator::writeParameterDefaultValueSymbols(
      std::ostream& os,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& mb,
      const std::string& name,
      const Hypothesis h) const {
    if (areParametersTreatedAsStaticVariables(mb)) {
      return;
    }
    auto throw_if = [](const bool b, const std::string& m) {
      tfel::raise_if(
          b, "SymbolsGenerator::writeParameterDefaultValueSymbols: " + m);
    };
    const auto n = this->getSymbolName(i, name, h);
    for (const auto& p : mb.getBehaviourData(h).getParameters()) {
      if (p.type == "int") {
        throw_if(p.arraySize != 1u,
                 "unsupported parameters array of type '" + p.type + "'");
        os << "MFRONT_SHAREDOBJ int " << n << "_" << p.getExternalName()
           << "_ParameterDefaultValue  = "
           << mb.getIntegerParameterDefaultValue(h, p.name) << ";\n\n";
      } else if (p.type == "ushort") {
        throw_if(p.arraySize != 1u,
                 "unsupported parameters array of type '" + p.type + "'");
        os << "MFRONT_SHAREDOBJ unsigned short " << n << "_"
           << p.getExternalName() << "_ParameterDefaultValue  = "
           << mb.getUnsignedShortParameterDefaultValue(h, p.name) << ";\n\n";
      } else {
        const auto f = SupportedTypes::getTypeFlag(p.type);
        throw_if(f != SupportedTypes::SCALAR,
                 "unsupported paramaeter type '" + p.type + "'");
        const auto prec = os.precision();
        os.precision(14);
        if (p.arraySize == 1u) {
          os << "MFRONT_SHAREDOBJ double " << n << "_" << p.getExternalName()
             << "_ParameterDefaultValue"
             << " = " << mb.getFloattingPointParameterDefaultValue(h, p.name)
             << ";\n\n";
        } else {
          for (unsigned short is = 0; is != p.arraySize; ++is) {
            os << "MFRONT_SHAREDOBJ double " << n << "_" << p.getExternalName()
               << "_mfront_index_" << is << "_ParameterDefaultValue = "
               << mb.getFloattingPointParameterDefaultValue(h, p.name, is)
               << ";\n\n";
          }
        }
        os.precision(prec);
      }
    }
  }  // end of writeParameterDefaultValueSymbols

  void SymbolsGenerator::writeBoundsSymbols(std::ostream& os,
                                            const StandardBehaviourInterface& i,
                                            const BehaviourDescription& mb,
                                            const std::string& name,
                                            const Hypothesis h) const {
    const auto& d = mb.getBehaviourData(h);
    const auto n = this->getSymbolName(i, name, h);
    mfront::writeBoundsSymbols(os, n, d.getMaterialProperties());
    mfront::writeBoundsSymbols(os, n, d.getPersistentVariables());
    mfront::writeBoundsSymbols(os, n, d.getExternalStateVariables());
    if (!areParametersTreatedAsStaticVariables(mb)) {
      mfront::writeBoundsSymbols(os, n, d.getParameters());
    }
  }  // end of writeBoundsSymbols

  void SymbolsGenerator::writePhysicalBoundsSymbols(
      std::ostream& os,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& mb,
      const std::string& name,
      const Hypothesis h) const {
    const auto& d = mb.getBehaviourData(h);
    const auto n = this->getSymbolName(i, name, h);
    mfront::writePhysicalBoundsSymbols(os, n, d.getMaterialProperties());
    mfront::writePhysicalBoundsSymbols(os, n, d.getPersistentVariables());
    mfront::writePhysicalBoundsSymbols(os, n, d.getExternalStateVariables());
    if (!areParametersTreatedAsStaticVariables(mb)) {
      mfront::writePhysicalBoundsSymbols(os, n, d.getParameters());
    }
  }  // end of writePhysicalBoundsSymbols

  void SymbolsGenerator::writeRequirementsSymbols(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& mb,
      const std::string& name,
      const Hypothesis h) const {
    out << "MFRONT_SHAREDOBJ unsigned short "
        << this->getSymbolName(i, name, h);
    out << "_requiresStiffnessTensor = ";
    if (mb.getAttribute(BehaviourDescription::requiresStiffnessTensor, false)) {
      out << "1";
    } else {
      out << "0";
    }
    out << ";\n";
    out << "MFRONT_SHAREDOBJ unsigned short "
        << this->getSymbolName(i, name, h);
    out << "_requiresThermalExpansionCoefficientTensor = ";
    if (mb.getAttribute(
            BehaviourDescription::requiresThermalExpansionCoefficientTensor,
            false)) {
      out << "1";
    } else {
      out << "0";
    }
    out << ";\n";

  }  // end of writeRequirementsSymbols

  void SymbolsGenerator::writeIsUsableInPurelyImplicitResolutionSymbols(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& mb,
      const std::string& name,
      const Hypothesis h) const {
    const auto& d = mb.getBehaviourData(h);
    out << "MFRONT_SHAREDOBJ unsigned short " << this->getSymbolName(i, name, h)
        << "_UsableInPurelyImplicitResolution = ";
    if (d.isUsableInPurelyImplicitResolution()) {
      out << "1;\n\n";
    } else {
      out << "0;\n\n";
    }
  }  // end of
     // SymbolsGenerator::writeIsUsableInPurelyImplicitResolution

  void SymbolsGenerator::writeSymmetryTypeSymbols(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& mb,
      const std::string& name) const {
    out << "MFRONT_SHAREDOBJ unsigned short " << i.getFunctionNameBasis(name)
        << "_SymmetryType = ";
    if (mb.getSymmetryType() == mfront::ISOTROPIC) {
      out << "0u;\n\n";
    } else if (mb.getSymmetryType() == mfront::ORTHOTROPIC) {
      out << "1u;\n\n";
    } else {
      tfel::raise(
          "SymbolsGenerator::writeSymmetryTypeSymbols: "
          "unsupported behaviour type.\n"
          "only isotropic or orthotropic behaviours "
          "are supported at this time.");
    }
  }  // end of writeSymmetryTypeSymbols

  void SymbolsGenerator::writeElasticSymmetryTypeSymbols(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription& mb,
      const std::string& name) const {
    out << "MFRONT_SHAREDOBJ unsigned short " << i.getFunctionNameBasis(name)
        << "_ElasticSymmetryType = ";
    if (mb.getElasticSymmetryType() == mfront::ISOTROPIC) {
      out << "0u;\n\n";
    } else if (mb.getElasticSymmetryType() == mfront::ORTHOTROPIC) {
      out << "1u;\n\n";
    } else {
      tfel::raise(
          "SymbolsGenerator::writeElasticSymmetryTypeSymbols: "
          "unsupported behaviour type.\n"
          "only isotropic or orthotropic behaviours are "
          "supported at this time.");
    }
  }  // end of writeElasticSymmetryTypeSymbols

  void SymbolsGenerator::writeSourceFileSymbols(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription&,
      const mfront::FileDescription& fd,
      const std::string& name) const {
    using namespace tfel::system;
    using namespace tfel::utilities;
    out << "MFRONT_SHAREDOBJ const char *\n"
        << i.getFunctionNameBasis(name) << "_src = \""
        << tokenize(fd.fileName, dirSeparator()).back() << "\";\n\n";
  }

  void SymbolsGenerator::writeInterfaceNameSymbols(
      std::ostream& out,
      const StandardBehaviourInterface& i,
      const BehaviourDescription&,
      const mfront::FileDescription&,
      const std::string& name) const {
    writeInterfaceSymbol(out, i.getFunctionNameBasis(name),
                         i.getInterfaceName() + i.getInterfaceVersion());
  }  // end of writeInterfaceNameSymbols

  void SymbolsGenerator::writeExternalNames(std::ostream& os,
                                            const StandardBehaviourInterface& i,
                                            const std::string& name,
                                            const Hypothesis& h,
                                            const std::vector<std::string>& v,
                                            const std::string& t) const {
    this->writeArrayOfStringsSymbol(
        os, this->getSymbolName(i, name, h) + "_" + t, v);
  }  // end of writeExternalNames

  void SymbolsGenerator::writeArrayOfStringsSymbol(
      std::ostream& os,
      const std::string& s,
      const std::vector<std::string>& v) const {
    mfront::writeArrayOfStringsSymbol(os, s, v);
  }  // end of writeArrayOfStringsSymbol

  void SymbolsGenerator::writeArrayOfIntsSymbol(
      std::ostream& f, const std::string& s, const std::vector<int>& v) const {
    if (v.empty()) {
      f << "MFRONT_SHAREDOBJ const int * " << s << " = nullptr;\n\n";
    } else {
      auto i = decltype(v.size()){};
      auto p = v.begin();
      f << "MFRONT_SHAREDOBJ int " << s << "[" << v.size() << "] = {";
      while (p != v.end()) {
        f << *p;
        if (++p != v.end()) {
          if (i % 5 == 0) {
            f << ",\n";
          } else {
            f << ",";
          }
        }
        ++i;
      }
      f << "};\n";
    }
  }  // end of writeArrayOfIntsSymbol

  SymbolsGenerator::~SymbolsGenerator() = default;

}  // end of namespace mfront
