/*!
 * \file   mfront/src/GenericModelInterface.cxx
 * \brief
 * \author Thomas Helfer
 * \date   28/04/2022
 * \copyright Copyright (C) 2006-2018 CEA/DEN, EDF R&D. All rights
 * reserved.
 * This project is publicly released under either the GNU GPL Licence
 * or the CECILL-A licence. A copy of thoses licences are delivered
 * with the sources of TFEL. CEA or EDF may also distribute this
 * project under specific licensing conditions.
 */

#include <fstream>
#include <algorithm>
#include "TFEL/Raise.hxx"
#include "TFEL/Config/GetInstallPath.hxx"
#include "TFEL/System/System.hxx"
#include "TFEL/Material/ModellingHypothesis.hxx"
#include "MFront/MFrontDebugMode.hxx"
#include "MFront/DSLUtilities.hxx"
#include "MFront/FileDescription.hxx"
#include "MFront/SupportedTypes.hxx"
#include "MFront/VariableDescription.hxx"
#include "MFront/TargetsDescription.hxx"
#include "MFront/GenericBehaviourSymbolsGenerator.hxx"
#include "MFront/GenericBehaviourInterface.hxx"
#include "MFront/GenericModelInterface.hxx"

namespace mfront {

  static bool isRealParameter(const VariableDescription& p) {
    return (p.type == "double") || (p.type == "real");
  } // end of isRealParameter

  static bool hasRealParameters(const ModelDescription& md) {
    if (areParametersTreatedAsStaticVariables(md)) {
      return false;
    }
    for (const auto& p : md.parameters) {
      if (isRealParameter(p)) {
        return true;
      }
    }
    return false;
  } // end of hasRealParameters

  static std::string getHeaderGuard(const ModelDescription& md) {
    auto header = std::string{"LIB_GENERICMODEL_"};
    if (!md.library.empty()) {
      header += "_";
      header += makeUpperCase(md.library);
    }
    if (!md.material.empty()) {
      header += "_";
      header += makeUpperCase(md.material);
    }
    header += "_";
    header += makeUpperCase(md.className);
    header += "_HXX";
    return header;
  }  // end of getHeaderGuard

  static void writeScalarStandardTypedefs(std::ostream& os,
                                          const ModelDescription& /* md */) {
    const auto use_qt = "false";  // useQuantities(md) ? "true" : "false";
    for (const auto& a : getScalarTypeAliases()) {
      os << "using " << a << " [[maybe_unused]] = "
         << "typename tfel::config::ScalarTypes<double, " << use_qt
         << ">::" << a << ";\n";
    }
  }  // end of writeScalarStandardTypedefs

  GenericModelInterface::GenericModelInterface() = default;

  std::string GenericModelInterface::getLibraryName(
      const ModelDescription& md) const {
    if (md.library.empty()) {
      if (!md.material.empty()) {
        return md.material + "-generic";
      } else {
        return "Model";
      }
    }
    return md.library + "-generic";
  }  // end of getLibraryName

  std::string GenericModelInterface::getName() {
    return "generic";
  }  // end of getName

  std::pair<bool, GenericModelInterface::tokens_iterator>
  GenericModelInterface::treatKeyword(const std::string& k,
                                      const std::vector<std::string>& i,
                                      tokens_iterator current,
                                      const tokens_iterator) {
    tfel::raise_if(
        (std::find(i.begin(), i.end(), "generic") != i.end()) ||
            (std::find(i.begin(), i.end(), "Generic") != i.end()),
        "GenericModelInterface::treatKeyword: unsupported keyword '" + k + "'");
    return {false, current};
  }  // end of treatKeyword

  void GenericModelInterface::declareReservedNames(std::set<std::string>&) {
  }  // end of declareReservedNames

  void GenericModelInterface::writeOutputFiles(const FileDescription& fd,
                                               const ModelDescription& md) {
    this->writeHeaderFile(fd, md);
    this->writeSourceFile(fd, md);
  }  // end of writeOutputFiles

  void GenericModelInterface::writeHeaderFile(const FileDescription& fd,
                                              const ModelDescription& md) {
    using tfel::material::ModellingHypothesis;
    auto raise = [](const std::string& m) {
      tfel::raise("GenericBehaviourInterface::writeHeaderFile: " + m);
    };
    tfel::system::systemCall::mkdir("include/MFront");
    tfel::system::systemCall::mkdir("include/MFront/GenericModel");
    const auto name = md.library + md.className;
    const auto header = name + "-generic.hxx";
    std::ofstream os("include/MFront/GenericModel/" + header);
    if (!os) {
      raise("could not open file '" + header + "'");
    }
    os << "/*!\n"
       << " * \\file   " << header << '\n'
       << " * \\brief  This file declares the generic interface for the "
       << md.className << " model\n"
       << " * \\author " << fd.authorName << '\n'
       << " * \\date   " << fd.date << '\n'
       << " */\n\n";

    const auto hg = getHeaderGuard(md);
    os << "#ifndef " << hg << "\n"
       << "#define " << hg << "\n\n"
       << "#include\"TFEL/Config/TFELConfig.hxx\"\n"
       << "#include\"MFront/GenericBehaviour/BehaviourData.h\"\n\n";

    writeExportDirectives(os, false);

    os << "#ifdef __cplusplus\n"
       << "extern \"C\"{\n"
       << "#endif /* __cplusplus */\n\n";

    if (hasRealParameters(md)) {
      os << "/*!\n"
         << " * \\brief function allowing to modify the parameters of the "
         << name << " model\n"
         << " * \\param[in] n: name of the parameter\n"
         << " * \\param[in] v: value of the parameter\n"
         << " */\n"
         << "MFRONT_SHAREDOBJ int\n"
         << name << "_setParameter(const char *const,const double);\n\n";
    }

    for (const auto& h : ModellingHypothesis::getModellingHypotheses()) {
      const auto f = name + "_" + ModellingHypothesis::toString(h);
      os << "/*!\n"
         << " * \\brief function implementing the " << name << " model for the "
         << ModellingHypothesis::toString(h) << " modelling hypothesis\n"
         << " * \\param[in,out] d: material data\n"
         << " */\n"
         << "MFRONT_SHAREDOBJ int " << f
         << "(mfront_gb_BehaviourData* const);\n\n";
    }

    os << "#ifdef __cplusplus\n"
       << "}\n"
       << "#endif /* __cplusplus */\n\n"
       << "#endif /* " << hg << " */\n";

  }  // end of writeHeaderFile

  void GenericModelInterface::writeSourceFile(const FileDescription& fd,
                                              const ModelDescription& md) {
    using tfel::material::ModellingHypothesis;
    constexpr auto uh = ModellingHypothesis::UNDEFINEDHYPOTHESIS;
    auto raise = [](const std::string& m) {
      tfel::raise("GenericBehaviourInterface::writeSourceFile: " + m);
    };
    auto getVariablePosition = [&raise](const auto& variables, const auto& n) {
      const auto pv = std::find_if(
          variables.begin(), variables.end(), [&n, &raise](const auto o) {
            if (o.getTypeFlag() != SupportedTypes::SCALAR) {
              raise("unsupported type for variable " + o.name);
            }
            return o.name == n;
          });
      if (pv == variables.end()) {
        raise("variable " + n + " not found");
      }
      return pv - variables.begin();
    };  // end of getVariablePosition
    tfel::system::systemCall::mkdir("src");
    const auto name = md.library + md.className;
    const auto header = name + "-generic.hxx";
    const auto src = name + "-generic.cxx";
    const auto cn = md.className + "RealParametersInitializer";
    std::ofstream os("src/" + src);
    if (!os) {
      raise("could not open file '" + src + "'");
    }
    os << "/*!\n"
       << " * \\file   " << src << '\n'
       << " * \\brief  This file declares the generic interface for the "
       << md.className << " model\n"
       << " * \\author " << fd.authorName << '\n'
       << " * \\date   " << fd.date << '\n'
       << " */\n\n"
       << "#include <cmath>\n"
       << "#include <cstring>\n"
       << "#include <sstream>\n"
       << "#include \"TFEL/Config/TFELTypes.hxx\"\n"
       << "#include \"TFEL/Math/Array/View.hxx\"\n"
       << "#include \"MFront/GenericBehaviour/Integrate.hxx\"\n\n"
       << "#include \"MFront/GenericModel/" << header << "\"\n\n";
    //
    const auto mhs = ModellingHypothesis::getModellingHypotheses();
    const auto hypotheses =
        std::set<ModellingHypothesis::Hypothesis>{mhs.begin(), mhs.end()};
    const auto bd = convertToBehaviourDescription(md);
    GenericBehaviourSymbolsGenerator sg;
    GenericBehaviourInterface bi;
    sg.generateGeneralSymbols(os, bi, bd, fd, hypotheses, name);
    os << '\n';
    sg.generateSymbols(os, bi, bd, fd, name, uh);
    //
    if (!md.sources.empty()) {
      os << md.sources << "\n\n";
    }
    //
    const auto has_constructor =
        (!md.constantMaterialProperties.empty()) || (hasRealParameters(md));
    //
    os << "namespace mfront::gm{\n\n";
    //
    if (hasRealParameters(md)) {
      os << "struct " << cn << "{\n";
      writeScalarStandardTypedefs(os, md);
      os << '\n';
      os << "//! \\return the unique instance of the " << cn << " class\n"
         << "static " << cn << "& get(){\n"
         << "static " << cn << " parameters;\n"
         << "return parameters;\n"
         << "}\n\n";
      //
      for (const auto& p : md.parameters) {
        if (!isRealParameter(p)) {
          continue;
        }
        os << p.type << " " << p.name << " = "
           << p.getAttribute<double>(VariableDescription::defaultValue) << ";\n";
      }
      os << "private:\n"
         << cn << "() = default;\n"
         << "};\n\n";
    }
    //
    os << "struct " << md.className << "{\n\n";
    writeScalarStandardTypedefs(os, md);
    os << '\n';
    if (has_constructor) {
      os << md.className << "(";
      if (!md.constantMaterialProperties.empty()) {
        os << "const mfront_gb_BehaviourData& mfront_model_data";
      }
      os << ")\n:";
      auto first = true;
      for (const auto& mp : md.constantMaterialProperties) {
        os << (first ? "" : ",\n");
        os << mp.name << "(mfront_model_data.s1.material_properties["
           << getVariablePosition(md.constantMaterialProperties, mp.name)
           << "])";
        first = false;
      }
      for (const auto& p : md.parameters) {
        if (!isRealParameter(p)) {
          continue;
        }
        os << (first ? "" : ",\n");
        os << p.name << "(" << cn << "::get()." << p.name << ")";
        first = false;
      }
      os << "\n"
         << "{}\n\n";
    }
    for (const auto& f : md.functions) {
      os << "void execute_" << f.name
         << "(mfront_gb_BehaviourData& mfront_model_data) const{\n";
      os << "using namespace std;\n";
      os << "using namespace tfel::math;\n";
      if (f.useTimeIncrement) {
        os << "const auto dt = time{mfront_model_data.dt};\n";
      }
      for (const auto& mv : f.modifiedVariables) {
        const auto& v = md.outputs.getVariable(mv);
        const auto pos = getVariablePosition(md.outputs, mv);
        os << "auto " << v.name << " = " << v.type
           << "{mfront_model_data.s0.internal_state_variables[" << pos
           << "]};\n";
      }
      for (const auto& mv : f.usedVariables) {
        const auto [n, vdepth] = md.decomposeVariableName(mv);
        const auto [v, type, pos] = [&md, &getVariablePosition, n = n]()
            -> std::tuple<VariableDescription, std::string, std::ptrdiff_t> {
          if (md.outputs.contains(n)) {
            const auto vpos = getVariablePosition(md.outputs, n);
            return {md.outputs.getVariable(n), "internal", vpos};
          }
          const auto vpos = getVariablePosition(md.inputs, n);
          return {md.inputs.getVariable(n), "external", vpos};
        }();
        if (vdepth > 1) {
          raise("unsupported depth");
        }
        if (vdepth == 1) {
          os << "const auto " << v.name << "_1 = " << v.type
             << "{mfront_model_data.s0." << type << "_state_variables[" << pos
             << "]};\n";
        } else {
          os << "const auto " << v.name << " = " << v.type
             << "{mfront_model_data.s1." << type << "_state_variables[" << pos
             << "]};\n";
        }
      }
      os << f.body << '\n';
      for (const auto& mv : f.modifiedVariables) {
        const auto [n, vdepth] = md.decomposeVariableName(mv);
        const auto& v = md.outputs.getVariable(mv);
        os << "tfel::math::map<" << v.type
           << ">(mfront_model_data.s1.internal_state_variables[0]) = " << v.name
           << ";\n";
      }
      os << "} // end of execute_" << f.name << "\n\n";
    }
    os << "private:\n";
    if (!md.members.empty()) {
      os << md.members << "\n\n";
    }
    if (!md.privateCode.empty()) {
      os << md.privateCode << "\n\n";
    }
    // material properties
    for (const auto& mp : md.constantMaterialProperties) {
      if (!getDebugMode()) {
        os << "#line " << mp.lineNumber << " \"" << fd.fileName << "\"\n";
      }
      os << "const " << mp.type << " " << mp.name << ";\n";
    }
    // parameters
    for (const auto& p : md.parameters) {
      if (!getDebugMode()) {
        os << "#line " << p.lineNumber << " \"" << fd.fileName << "\"\n";
      }
      if ((isRealParameter(p)) &&
          (!areParametersTreatedAsStaticVariables(md))) {
        os << "const " << p.type << " " << p.name << ";\n";
        continue;
      }
      if (p.type == "string") {
        os << "const std::string " << p.name << " = "
           << p.getAttribute<std::string>(VariableDescription::defaultValue);
      } else if (isRealParameter(p)) {
        os << "static constexpr " << p.type << " " << p.name << " = "
           << p.getAttribute<double>(VariableDescription::defaultValue);
      } else if (p.type == "bool") {
        os << "static constexpr " << p.type << " " << p.name << " = "
           << p.getAttribute<bool>(VariableDescription::defaultValue);
      } else {
        raise("parameter type '" + p.type + "' is not supported.\n");
      }
      os << ";\n";
    }
    // static variables
    for (const auto& v : md.staticVars) {
      if (!getDebugMode()) {
        os << "#line " << v.lineNumber << " \"" << fd.fileName << "\"\n";
      }
      os << "static constexpr " << v.type << " " << v.name << " = "  //
         << v.value << ";\n";
    }
    os << "}; // end of struct " << md.className << "\n\n";
    //

    os << "static int " << name
       << "_implementation(mfront_gb_BehaviourData& d){\n";
    os << "try{\n";
    if (!md.constantMaterialProperties.empty()) {
      os << "const " << md.className << " m(d);\n";
    } else {
      os << "const " << md.className << " m;\n";
    }
    for (const auto& f : md.functions) {
      os << "m.execute_" << f.name << "(d);\n";
    }
    os << "} catch(...){\n"
       << "mfront::gb::reportFailureByException(d);\n"
       << "*(d.rdt) = 0.1;\n"
       << "return -1;"
       << "}\n"
       << "return 1;\n"
       << "}\n\n";
    os << "} // end of namespace mfront::gm\n\n";
    //
    if (hasRealParameters(md)) {
      os << "MFRONT_SHAREDOBJ int\n"
         << name << "_setParameter(const char *const n, const double v){\n"
         << "auto& parameters = mfront::gm::" << cn << "::get();\n";
      auto first = true;
      for (const auto& p : md.parameters) {
        if (!isRealParameter(p)) {
          continue;
        }
        const auto en = p.getExternalName();
        os << (first ? "if" : "} else if")  //
           << "(std::strcmp(\"" << en << "\", n) == 0){\n"
           << "parameters." << p.name << " = v;\n"
           << "return 0;\n";
        first = false;
      }
      os << "}\n"
         << "return -1;\n"
         << "} // end of " << name << "_setParameter\n\n";
    }
    //
    for (const auto& h : ModellingHypothesis::getModellingHypotheses()) {
      const auto fct = name + "_" + ModellingHypothesis::toString(h);
      os << "int " << fct << "(mfront_gb_BehaviourData* const d){\n"
         << "return mfront::gm::" << name << "_implementation(*d);\n"
         << "}\n\n";
    }
  }  // end of writeSourceFile

  void GenericModelInterface::getTargetsDescription(
      TargetsDescription& td, const ModelDescription& md) {
    using tfel::material::ModellingHypothesis;
    const auto lib = this->getLibraryName(md);
    const auto name = md.library + md.className;
    const auto tfel_config = tfel::getTFELConfigExecutableName();
    auto& l = td.getLibrary(lib);
    insert_if(l.cppflags,
              "$(shell " + tfel_config + " --cppflags --compiler-flags)");
    insert_if(l.include_directories,
              "$(shell " + tfel_config + " --include-path)");
    insert_if(l.sources, name + "-generic.cxx");
    td.headers.push_back("MFront/GenericModel/" + name + "-generic.hxx");
    insert_if(l.link_directories,
              "$(shell " + tfel_config + " --library-path)");
    insert_if(l.link_libraries, "$(shell " + tfel_config +
                                    " --library-dependency "
                                    "--material --mfront-profiling)");
    for (const auto& h : ModellingHypothesis::getModellingHypotheses()) {
      insert_if(l.epts, name + "_" + ModellingHypothesis::toString(h));
    }
  }  // end of getTargetsDescription

  GenericModelInterface::~GenericModelInterface() = default;

}  // end of namespace mfront
