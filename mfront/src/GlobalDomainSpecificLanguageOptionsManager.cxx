/*!
 * \file   mfront/src/GlobalDomainSpecificLanguageOptionsManager.cxx
 * \brief    
 * \author Thomas Helfer
 * \date   17/01/2022
 * \copyright Copyright (C) 2006-2018 CEA/DEN, EDF R&D. All rights
 * reserved.
 * This project is publicly released under either the GNU GPL Licence
 * or the CECILL-A licence. A copy of thoses licences are delivered
 * with the sources of TFEL. CEA or EDF may also distribute this
 * project under specific licensing conditions.
 */

#include "TFEL/Utilities/CxxTokenizer.hxx"
#include "MFront/GlobalDomainSpecificLanguageOptionsManager.hxx"

namespace mfront {

  static void add_option(tfel::utilities::DataMap& options,
                         const std::string& o,
                         const std::string& v) {
    if (options.find(o) != options.end()) {
      tfel::raise("add_option: option '" + o + "' already defined");
    }
    tfel::utilities::CxxTokenizer t;
    t.parseString(v);
    auto b = t.begin();
    options.insert({o, tfel::utilities::Data::read(b, t.end())});
  }

  GlobalDomainSpecificLanguageOptionsManager&
  GlobalDomainSpecificLanguageOptionsManager::get() {
    static GlobalDomainSpecificLanguageOptionsManager m;
    return m;
  } // end of get

  void GlobalDomainSpecificLanguageOptionsManager::addDSLOption(
      const std::string& n, const std::string& v) {
    this->addMaterialPropertyDSLOption(n, v);
    this->addBehaviourDSLOption(n, v);
    this->addModelDSLOption(n, v);
  } // end of addDSLOption

  void GlobalDomainSpecificLanguageOptionsManager::addMaterialPropertyDSLOption(
      const std::string& n, const std::string& v) {
    add_option(this->material_property_dsl_options, n, v);
  }  // end of addMaterialPropertyDSLOption

  void GlobalDomainSpecificLanguageOptionsManager::addBehaviourDSLOption(
      const std::string& n, const std::string& v) {
    add_option(this->behaviour_dsl_options, n, v);
  }  // end of addBehaviourDSLOption

  void GlobalDomainSpecificLanguageOptionsManager::addModelDSLOption(
      const std::string& n, const std::string& v) {
    add_option(this->model_dsl_options, n, v);
  }  // end of addModelDSLOption

  tfel::utilities::DataMap
  GlobalDomainSpecificLanguageOptionsManager::getMaterialPropertyDSLOptions()
      const {
    return this->material_property_dsl_options;
  }

  tfel::utilities::DataMap
  GlobalDomainSpecificLanguageOptionsManager::getBehaviourDSLOptions() const {
    return this->behaviour_dsl_options;
  }  // end of getBehaviourDSLOptions

  tfel::utilities::DataMap
  GlobalDomainSpecificLanguageOptionsManager::getModelDSLOptions()
      const {
    return this->model_dsl_options;
  }  // end of getModelDSLOptions

  GlobalDomainSpecificLanguageOptionsManager::
      GlobalDomainSpecificLanguageOptionsManager() = default;

  GlobalDomainSpecificLanguageOptionsManager::
      ~GlobalDomainSpecificLanguageOptionsManager() = default;

}  // namespace mfront