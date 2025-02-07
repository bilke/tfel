/*!
 * \file   mfront/include/MFront/ModelInterfaceProxy.ixx
 * \brief
 *
 * \author Thomas Helfer
 * \date   09 nov 2006
 * \copyright Copyright (C) 2006-2018 CEA/DEN, EDF R&D. All rights
 * reserved.
 * This project is publicly released under either the GNU GPL Licence
 * or the CECILL-A licence. A copy of thoses licences are delivered
 * with the sources of TFEL. CEA or EDF may also distribute this
 * project under specific licensing conditions.
 */

#ifndef LIB_MFRONTMODELINTERFACEPROXY_IXX
#define LIB_MFRONTMODELINTERFACEPROXY_IXX

namespace mfront {

  template <typename Interface>
  ModelInterfaceProxy<Interface>::ModelInterfaceProxy() {
    auto& mbif = ModelInterfaceFactory::getModelInterfaceFactory();
    mbif.registerInterfaceCreator(Interface::getName(), &createInterface);
  }

  template <typename Interface>
  std::shared_ptr<AbstractModelInterface>
  ModelInterfaceProxy<Interface>::createInterface() {
    return std::make_shared<Interface>();
  }

}  // end of namespace mfront

#endif /* LIB_MFRONTMODELINTERFACEPROXY_IXX */
