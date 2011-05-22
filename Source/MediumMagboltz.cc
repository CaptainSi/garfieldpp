#include <iostream>
#include <iomanip>
#include <fstream>
#include <cmath>

#include <map>

#include <TMath.h>

#include "MediumMagboltz.hh"
#include "MagboltzInterface.hh"
#include "Random.hh"
#include "FundamentalConstants.hh"
#include "GarfieldConstants.hh"
#include "OpticalData.hh"

namespace Garfield {

MediumMagboltz::MediumMagboltz() :
  MediumGas(),
  eFinal(40.), eStep(eFinal / nEnergySteps), 
  eMinLog(10000.), 
  useAutoAdjust(true), 
  useCsOutput(false), 
  nTerms(0), useAnisotropic(true), 
  nPenning(0), 
  useDeexcitation(false), useRadTrap(true), 
  nDeexcitations(0), 
  nIonisationProducts(0), nDeexcitationProducts(0), 
  scaleExc(1.), useOpalBeaty(true), useGreenSawada(false),
  eFinalGamma(20.), eStepGamma(eFinalGamma / nEnergyStepsGamma) {
 
  className = "MediumMagboltz";
 
  // Set physical constants in Magboltz common blocks.
  cnsts_.echarg = ElementaryCharge;
  cnsts_.emass = ElectronMassGramme;
  cnsts_.amu = AtomicMassUnit;
  cnsts_.pir2 = BohrRadius * BohrRadius * Pi;  
  inpt_.ary = RydbergEnergy;
  
  // Set parameters in Magboltz common blocks.
  inpt_.nGas = nComponents;  
  inpt_.nStep = nEnergySteps;
  // Select the scattering model.
  inpt_.nAniso = 2;
  // Max. energy [eV]
  inpt_.efinal = eFinal;
  // Energy step size [eV]
  inpt_.estep = eStep;
  // Temperature and pressure
  inpt_.akt = BoltzmannConstant * temperature;
  inpt_.tempc = temperature - ZeroCelsius;
  inpt_.torr = pressure;
  // Disable Penning transfer.
  inpt_.ipen = 0;
 
  // Initialise Penning parameters
  for (int i = nMaxLevels; i--;) {
    rPenning[i] = 0.;
    lambdaPenning[i] = 0.;
  }  
 
  isChanged = true;

  EnableDrift();
  EnablePrimaryIonisation();
  microscopic = true;
  
  // Initialize the collision counters.
  nCollisionsDetailed.clear();
  for (int i = nCsTypes; i--;) nCollisions[i] = 0;
  for (int i = nCsTypesGamma; i--;) nPhotonCollisions[i] = 0; 
  
  ionProducts.clear();
  dxcProducts.clear();

}

bool 
MediumMagboltz::SetMaxElectronEnergy(const double e) {

  if (e <= Small) {
    std::cerr << className << "::SetMaxElectronEnergy:\n";
    std::cerr << "    Provided upper electron energy limit (" << e
              <<  " eV) is too small.\n";
    return false;
  }
  eFinal = e;
  
  // Determine the energy interval size.
  if (eFinal <= eMinLog) {
    eStep = eFinal / nEnergySteps;
  } else {
    eStep = eMinLog / nEnergySteps;
  }
  
  // Set max. energy and step size also in Magboltz common block.
  inpt_.efinal = eFinal;
  inpt_.estep = eStep;
  
  // Force recalculation of the scattering rates table.
  isChanged = true;

  return true;
  
}

bool 
MediumMagboltz::SetMaxPhotonEnergy(const double e) {

  if (e <= Small) {
    std::cerr << className << "::SetMaxPhotonEnergy:\n";
    std::cerr << "    Provided upper photon energy limit (" << e
              <<  " eV) is too small.\n";
    return false;
  }
  eFinalGamma = e;
  
  // Determine the energy interval size.
  eStepGamma = eFinalGamma / nEnergyStepsGamma;

  // Force recalculation of the scattering rates table.
  isChanged = true;
 
  return true;
  
}

void 
MediumMagboltz::SetSplittingFunctionOpalBeaty() {

  useOpalBeaty = true;
  useGreenSawada = false;

}

void
MediumMagboltz::SetSplittingFunctionGreenSawada() {

  useOpalBeaty = false;
  useGreenSawada = true;
  if (isChanged) return;
  
  bool allset = true;
  for (int i = 0; i < nComponents; ++i) {
    if (!hasGreenSawada[i]) {
      if (allset) {
        std::cout << className << "::SetSplittingFunctionGreenSawada:\n";
        allset = false;
      }
      std::cout << "    Fit parameters for " 
                << gas[i] << " not available.\n";
      std::cout << "    Opal-Beaty formula is used instead.\n";
    }
  }

}

void
MediumMagboltz::SetSplittingFunctionFlat() {

  useOpalBeaty = false;
  useGreenSawada = false;

}

void
MediumMagboltz::EnableDeexcitation() {

  std::cout << className << "::EnableDeexcitation:\n";
  if (usePenning) {
    std::cout << "    Penning transfer will be switched off.\n";
  }
  if (useRadTrap) {
    std::cout << "    Radiation trapping is switched on.\n";
  } else {
    std::cout << "    Radiation trapping is switched off.\n";
  }
  usePenning = false;
  useDeexcitation = true;
  isChanged = true;
  nDeexcitationProducts = 0;

}

void
MediumMagboltz::EnableRadiationTrapping() {

  useRadTrap = true;
  if (!useDeexcitation) {
    std::cout << className << "::EnableRadiationTrapping:\n";
    std::cout << "    Radiation trapping is enabled"
              << " but de-excitation is not.\n";
  } else {
    isChanged = true;
  }

}

void
MediumMagboltz::EnablePenningTransfer(const double r, 
                                        const double lambda) {

  if (r < 0. || r > 1.) {
    std::cerr << className << "::EnablePenningTransfer:\n";
    std::cerr << "    Penning transfer probability must be " 
              << " in the range [0, 1].\n";
    return;
  }

  rPenningGlobal = r;
  if (lambda < Small) {
    lambdaPenningGlobal = 0.;
  } else {
    lambdaPenningGlobal = lambda;
  }

  std::cout << className << "::EnablePenningTransfer:\n";
  std::cout << "    Global Penning transfer parameters set to: \n";
  std::cout << "    r      = " << rPenningGlobal << "\n";
  std::cout << "    lambda = " << lambdaPenningGlobal << " cm\n";

  for (int i = nTerms; i--;) { 
    rPenning[i] = rPenningGlobal;
    lambdaPenning[i] = lambdaPenningGlobal;
  }
  
  if (useDeexcitation) {
    std::cout << className << "::EnablePenningTransfer:\n";
    std::cout << "    Deexcitation handling will be switched off.\n"; 
  }
  usePenning = true;
  
}

void
MediumMagboltz::EnablePenningTransfer(const double r, 
                                        const double lambda, 
                                        std::string gasname) {

  if (r < 0. || r > 1.) {
    std::cerr << className << "::EnablePenningTransfer:\n";
    std::cerr << "    Penning transfer probability must be " 
              << " in the range [0, 1].\n";
    return;
  }

  // Get the "standard" name of this gas.
  if (!GetGasName(gasname, gasname)) {
    std::cerr << className << "::EnablePenningTransfer:\n";
    std::cerr << "    Unknown gas name.\n";
    return;
  }

  // Look for this gas in the present gas mixture.
  bool found = false;
  int iGas = -1;
  for (int i = nComponents; i--;) {
    if (gas[i] == gasname) {
      rPenningGas[i] = r;
      if (lambda < Small) {
        lambdaPenningGas[i] = 0.;
      } else {
        lambdaPenningGas[i] = lambda;
      }
      found = true;
      iGas = i;
      break;
    }
  }
  
  if (!found) {
    std::cerr << className << "::EnablePenningTransfer:\n";
    std::cerr << "    Specified gas (" << gasname 
              << ") is not part of the present gas mixture.\n";
    return;
  }

  // Make sure that the collision rate table is updated.
  if (isChanged) {
    if (!Mixer()) {
      std::cerr << className << "::EnablePenningTransfer:\n";
      std::cerr << "    Error calculating the collision rates table.\n";
      return;
    }
    isChanged = false;
  }

  int nLevelsFound = 0;
  for (int i = nTerms; i--;) {
    if (int(csType[i] / nCsTypes) == iGas) {
      if (csType[i] % nCsTypes == ElectronCollisionTypeExcitation) {
        ++nLevelsFound; 
      }
      rPenning[i] = rPenningGas[iGas];
      lambdaPenning[i] = lambdaPenningGas[iGas];
    }
  }

  if (nLevelsFound > 0) {
    std::cout << className << "::EnablePenningTransfer:\n";
    std::cout << "    Penning transfer parameters for " << nLevelsFound
              << " excitation levels set to:\n";
    std::cout << "      r      = " << rPenningGas[iGas] << "\n";
    std::cout << "      lambda = " << lambdaPenningGas[iGas] << " cm\n"; 
  } else {
    std::cerr << className << "::EnablePenningTransfer:\n";
    std::cerr << "    Specified gas (" << gasname 
              << ") has no excitation levels in the present energy range.\n";
  }

  usePenning = true; 

}
 
void
MediumMagboltz::DisablePenningTransfer() {

  for (int i = nTerms; i--;) {
    rPenning[i] = 0.;
    lambdaPenning[i] = 0.;
  }
  rPenningGlobal = 0.;
  lambdaPenningGlobal = 0.;

  for (int i = nMaxGases; i--;) {
    rPenningGas[i] = 0.;
    lambdaPenningGas[i] = 0.;
  }

  usePenning = false;    

}

void
MediumMagboltz::DisablePenningTransfer(std::string gasname) {

  // Get the "standard" name of this gas.
  if (!GetGasName(gasname, gasname)) {
    std::cerr << className << "::DisablePenningTransfer:\n";
    std::cerr << "    Gas " << gasname << " is not defined.\n";
    return;
  }

  // Look for this gas in the present gas mixture.
  bool found = false;
  int iGas = -1;
  for (int i = nComponents; i--;) {
    if (gas[i] == gasname) {
      rPenningGas[i] = 0.;
      lambdaPenningGas[i] = 0.;
      found = true;
      iGas = i;
      break;
    }
  }
  
  if (!found) {
    std::cerr << className << "::DisablePenningTransfer:\n";
    std::cerr << "    Specified gas (" << gasname 
              << ") is not part of the present gas mixture.\n";
    return;
  }
  
  int nLevelsFound = 0;
  for (int i = nTerms; i--;) {
    if (int(csType[i] / nCsTypes) == iGas) {
      rPenning[i] = 0.;
      lambdaPenning[i] = 0.;
    } else {
      if (csType[i] % nCsTypes == ElectronCollisionTypeExcitation && 
          rPenning[i] > Small) {
        ++nLevelsFound;
      }
    }
  }

  if (nLevelsFound <= 0) {
    // There are no more excitation levels with r > 0.
    std::cout << className << "::DisablePenningTransfer:\n";
    std::cout << "    Penning transfer globally switched off.\n";
    usePenning = false;
  }

}

void
MediumMagboltz::SetExcitationScalingFactor(const double r) {

  if (r <= 0.) {
    std::cerr << className << "::SetScalingFactor:\n";
    std::cerr << "    Incorrect value for scaling factor: " << r << "\n";
    return;
  }

  scaleExc = r;
  isChanged = true;

}

bool
MediumMagboltz::Initialise() {

  if (!isChanged) {
    if (debug) {
      std::cerr << className << "::Initialise:\n";
      std::cerr << "    Nothing changed.\n";
    }
    return true;
  }
  if (!Mixer()) {
    std::cerr << className << "::Initialise:\n";
    std::cerr << "    Error calculating the collision rates table.\n";
    return false;
  }
  isChanged = false;
  return true;

}

void
MediumMagboltz::PrintGas() {

  MediumGas::PrintGas();

  if (isChanged) {
    if (!Initialise()) return;
  }

  std::cout << className << "::PrintGas:\n";
  for (int i = 0; i < nTerms; ++i) {
    // Collision type
    int type = csType[i] % nCsTypes;
    int ngas = int(csType[i] / nCsTypes);
    // Description (from Magboltz)
    std::string descr = "                              ";
    for (int j = 30; j--;) descr[j] = description[i][j];
    // Threshold energy
    double e = rgas[ngas] * energyLoss[i];
    std::cout << "    Level " << i << ": " << descr << "\n";
    std::cout << "        Type " << type;
    if (type == ElectronCollisionTypeElastic) {
      std::cout << " (elastic)\n";
    } else if (type == ElectronCollisionTypeIonisation) {
      std::cout << " (ionisation)\n";
      std::cout << "        Ionisation threshold: " << e << " eV\n";
    } else if (type == ElectronCollisionTypeAttachment) {
      std::cout << " (attachment)\n";
    } else if (type == ElectronCollisionTypeInelastic) {
      std::cout << " (inelastic)\n";
      std::cout << "        Energy loss: " << e << " eV\n";
    } else if (type == ElectronCollisionTypeExcitation) {
      std::cout << " (excitation)\n";
      std::cout << "        Excitation energy: " << e << " eV\n";
    } else if (type == ElectronCollisionTypeSuperelastic) {
      std::cout << " (super-elastic)\n";
      std::cout << "        Energy gain: " << -e << " eV\n";
    } else {
      std::cout << " (unknown)\n";
    }
    if (type == ElectronCollisionTypeExcitation && 
        usePenning && e > minIonPot) {
      std::cout << "        Penning transfer coefficient: " 
                << rPenning[i] << "\n";
    } else if (type == ElectronCollisionTypeExcitation && 
               useDeexcitation) {
      const int idxc = iDeexcitation[i];
      if (idxc < 0 || idxc >= nDeexcitations) {
        std::cout << "        Deexcitation cascade not implemented.\n";
        continue;
      }
      if (deexcitations[idxc].osc > 0.) { 
        std::cout << "        Oscillator strength: " 
                  << deexcitations[idxc].osc << "\n";
      }
      std::cout << "        Decay channels:\n";
      for (int j = 0; j < deexcitations[idxc].nChannels; ++j) {
        if (deexcitations[idxc].type[j] == 0) {
          std::cout << "          Radiative decay to ";
          if (deexcitations[idxc].final[j] < 0) {
            std::cout << "ground state: ";
          } else {
            std::cout << deexcitations[deexcitations[idxc].final[j]].label
                      << ": ";
          }
        } else if (deexcitations[idxc].type[j] == 1) {
          if (deexcitations[idxc].final[j] < 0) {
            std::cout << "          Penning ionisation: ";
          } else {
            std::cout << "          Associative ionisation: ";
          }
        } else if (deexcitations[idxc].type[j] == -1) {
          if (deexcitations[idxc].final[j] >= 0) {
            std::cout << "          Collision-induced transition to "
                      << deexcitations[deexcitations[idxc].final[j]].label
                      << ": ";
          } else {
            std::cout << "          Loss: ";
          }
        }
        if (j == 0) {
          std::cout << std::setprecision(5) 
                    << deexcitations[idxc].p[j] * 100. << "%\n";
        } else {
          std::cout << std::setprecision(5) 
                    << (deexcitations[idxc].p[j] - 
                        deexcitations[idxc].p[j - 1]) * 100. << "%\n";
        }
      } 
    }
  }

}

double 
MediumMagboltz::GetElectronNullCollisionRate(const int band) {

  // If necessary, update the collision rates table.
  if (isChanged) {
    if (!Mixer()) {
      std::cerr << className << "::GetElectronNullCollisionRate:\n";
      std::cerr << "     Error calculating the collision rates table.\n";
      return 0.;
    }
    isChanged = false;
  }
 
  if (debug && band > 0) {
    std::cerr << className << "::GetElectronNullCollisionRate:\n";
    std::cerr << "    Warning: unexpected band index.\n";
  }
 
  return cfNull;
 
}

double 
MediumMagboltz::GetElectronCollisionRate(const double e, const int band) {

  // Check if the electron energy is within the currently set range.
  if (e <= 0.) {
    std::cerr << className << "::GetElectronCollisionRate:\n";
    std::cerr << "    Electron energy must be greater than zero.\n";
    return cfTot[0];
  }
  if (e > eFinal && useAutoAdjust) {    
    std::cerr << className << "::GetElectronCollisionRate:\n";
    std::cerr << "    Collision rate at " << e 
              << " eV is not included in the current table.\n";
    std::cerr << "    Increasing energy range to " << 1.05 * e
              << " eV.\n";
    SetMaxElectronEnergy(1.05 * e);    
  }

  // If necessary, update the collision rates table.
  if (isChanged) {
    if (!Mixer()) {
      std::cerr << className << "::GetElectronCollisionRate:\n";
      std::cerr << "    Error calculating the collision rates table.\n";
      return 0.;
    }
    isChanged = false;
  }

  if (debug && band > 0) {
    std::cerr << className << "::GetElectronCollisionRate:\n";
    std::cerr << "    Warning: unexpected band index.\n";
  }

  // Get the energy interval.
  int iE = 0;
  if (e <= eMinLog) {
    iE = int(e / eStep);
    if (iE >= nEnergySteps) return cfTot[nEnergySteps - 1];
    if (iE < 0) return cfTot[0]; 
    return cfTot[iE];
  }
  
  const double rLog = pow(eFinal / eMinLog, 1. / nEnergyStepsLog);
  iE = int(log(e / eMinLog) / log(rLog));
  return cfTotLog[iE];

}

bool 
MediumMagboltz::GetElectronCollision(const double e, int& type, int& level, 
                                     double& e1, 
                                     double& dx, double& dy, double& dz,
                                     int& nion, int& ndxc, int& band) {

  // Check if the electron energy is within the currently set range.
  if (e > eFinal && useAutoAdjust) {
    std::cerr << className << "::GetElectronCollision:\n";
    std::cerr << "    Provided electron energy  (" << e 
              << " eV) exceeds current energy range  (" << eFinal 
              << " eV).\n";
    std::cerr << "    Increasing energy range to " << 1.05 * e
              << " eV.\n";
    SetMaxElectronEnergy(1.05 * e);
  } else if (e <= 0.) {
    std::cerr << className << "::GetElectronCollision:\n";
    std::cerr << "    Electron energy must be greater than zero.\n";
    return false;
  }
  
    // If necessary, update the collision rates table.
  if (isChanged) {
    if (!Mixer()) {
      std::cerr << className << "::GetElectronCollision:\n";
      std::cerr << "    Error calculating the collision rates table.\n"; 
      return false;
    }
    isChanged = false;
  }

  if (debug && band > 0) {
    std::cerr << className << "::GetElectronCollision:\n";
    std::cerr << "    Warning: unexpected band index.\n";
  }

  double angCut = 1.;
  double angPar = 0.5;
  
  if (e <= eMinLog) {
    // Get the energy interval.
    int iE = int(e / eStep);
    if (iE >= nEnergySteps) iE = nEnergySteps - 1;
    if (iE < 0) iE = 0;

    // Sample the scattering process.
    const double r = RndmUniform();
    int iLow = 0;
    int iUp  = nTerms - 1;  
    if (r <= cf[iE][iLow]) {
      level = iLow;
    } else if (r >= cf[iE][iUp]) {
      level = iUp;
    } else {
      int iMid;
      while (iUp - iLow > 1) {
        iMid = (iLow + iUp) >> 1;
        if (r < cf[iE][iMid]) {
          iUp = iMid;
        } else {
          iLow = iMid;
        }
      }
      level = iUp;
    }
    // Get the angular distribution parameters.
    angCut = scatCut[iE][level];
    angPar = scatParameter[iE][level];
  } else {
    const double rLog = pow(eFinal / eMinLog, 1. / nEnergyStepsLog);
    // Get the energy interval.
    int iE = int(log(e / eMinLog) / log(rLog));
    if (iE < 0) iE = 0;
    if (iE >= nEnergyStepsLog) iE = nEnergyStepsLog - 1;
    // Sample the scattering process.
    const double r = RndmUniform();
    int iLow = 0;
    int iUp  = nTerms - 1;  
    if (r <= cfLog[iE][iLow]) {
      level = iLow;
    } else if (r >= cfLog[iE][iUp]) {
      level = iUp;
    } else {
      int iMid;
      while (iUp - iLow > 1) {
        iMid = (iLow + iUp) >> 1;
        if (r < cfLog[iE][iMid]) {
          iUp = iMid;
        } else {
          iLow = iMid;
        }
      }
      level = iUp;
    }
    // Get the angular distribution parameters.
    angCut = scatCutLog[iE][level];
    angPar = scatParameterLog[iE][level];
  }
  
  // Extract the collision type.
  type = csType[level] % nCsTypes;
  const int igas = int(csType[level] / nCsTypes);
  // Increase the collision counters.
  ++nCollisions[type];
  ++nCollisionsDetailed[level];

  // Get the energy loss for this process.
  double loss = energyLoss[level];
  nion = ndxc = 0;

  if (type == ElectronCollisionTypeIonisation) {
    // Sample the secondary electron energy according to 
    // the Opal-Beaty-Peterson parameterisation.
    double esec = 0.;
    if (useOpalBeaty) { 
      // Get the splitting parameter.
      const double w = wOpalBeaty[level];
      esec = w * tan(RndmUniform() * atan(0.5 * (e - loss) / w));
      // Rescaling (SST)
      // esec = w * pow(esec / w, 0.9524);
    } else if (useGreenSawada) {
      const double w = gsGreenSawada[igas] * e / (e + gbGreenSawada[igas]);
      const double esec0 = tsGreenSawada[igas] - 
                           taGreenSawada[igas] / (e + tbGreenSawada[igas]); 
      const double r = RndmUniform();
      esec = esec0 + w * tan((r - 1.) * atan(esec0 / w) + 
                             r * atan((0.5 * (e - loss) - esec0) / w));  
    } else { 
      esec = RndmUniform() * (e - loss);
    }
    if (esec <= 0) esec = Small;
    loss += esec;
    ionProducts.clear();
    // Add the secondary electron.
    ionProd newIonProd;
    newIonProd.type = IonProdTypeElectron;
    newIonProd.energy = esec;
    ionProducts.push_back(newIonProd);
    // Add the ion.
    newIonProd.type = IonProdTypeIon;
    newIonProd.energy = 0.;
    ionProducts.push_back(newIonProd);
    nIonisationProducts = nion = 2;
  } else if (type == ElectronCollisionTypeExcitation) {
    // if (gas[igas] == "CH4" && loss * rgas[igas] < 13.35 && e > 12.65) {
    //   if (RndmUniform() < 0.5) {
    //     loss = 8.55 + RndmUniform() * (13.3 - 8.55);
    //     loss /= rgas[igas];
    //   } else {
    //     loss = std::max(Small, RndmGaussian(loss * rgas[igas], 1.));
    //     loss /= rgas[igas];
    //   }
    // }
    // Follow the de-excitation cascade (if switched on).
    if (useDeexcitation && iDeexcitation[level] >= 0) {
      int fLevel = 0;
      ComputeDeexcitationInternal(iDeexcitation[level], fLevel);
      ndxc = nDeexcitationProducts;
    } else if (usePenning) {
      nDeexcitationProducts = 0;
      dxcProducts.clear();
      // Simplified treatment of Penning ionisation.
      // If the energy threshold of this level exceeds the 
      // ionisation potential of one of the gases,
      // create a new electron (with probability rPenning).
      if (energyLoss[level] * rgas[igas] > minIonPot && 
          RndmUniform() < rPenning[level]) {
        // The energy of the secondary electron is assumed to be given by
        // the difference of excitation and ionisation threshold.
        double esec = energyLoss[level] * rgas[igas] - minIonPot;
        if (esec <= 0) esec = Small;
        // Add the secondary electron to the list.
        dxcProd newDxcProd;
        newDxcProd.t = 0.;
        newDxcProd.s = -lambdaPenning[level] * log(RndmUniformPos());
        newDxcProd.energy = esec;
        newDxcProd.type = DxcProdTypeElectron;
        dxcProducts.push_back(newDxcProd);
        nDeexcitationProducts = ndxc = 1;
        ++nPenning;
      }
    }
  }

  // Make sure the energy loss is smaller than the energy.
  if (e < loss) loss = e - 0.0001;
  
  // Determine the scattering angle.
  double ctheta0 = 1. - 2. * RndmUniform();
  if (useAnisotropic) {
    switch (scatModel[level]) {
      case 0:
        break;
      case 1:
        ctheta0 = 1. - RndmUniform() * angCut;
        if (RndmUniform() > angPar) ctheta0 = -ctheta0;
        break;
      case 2:
        ctheta0 = (ctheta0 + angPar) / (1. + angPar * ctheta0);
        break;
      default:
        std::cerr << className << "::GetElectronCollision:\n";
        std::cerr << "    Unknown scattering model. \n";
        std::cerr << "    Using isotropic distribution.\n";
        break;
    }
  }

  const double s1 = rgas[igas];
  const double s2 = (s1 * s1) / (s1 - 1.);
  const double theta0 = acos(ctheta0);
  const double arg = std::max(1. - s1 * loss / e, Small);
  const double d = 1. - ctheta0 * sqrt(arg);

  // Update the energy. 
  e1 = std::max(e * (1. - loss / (s1 * e) - 2. * d / s2), Small);
  double q = std::min(sqrt((e / e1) * arg) / s1, 1.);
  const double theta = asin(q * sin(theta0));
  double ctheta = cos(theta);
  if (ctheta0 < 0.) {
    const double u = (s1 - 1.) * (s1 - 1.) / arg;
    if (ctheta0 * ctheta0 > u) ctheta = -ctheta;
  }
  const double stheta = sin(theta); 
  // Calculate the direction after the collision.
  dz = std::min(dz, 1.); 
  const double argZ = sqrt(dx * dx + dy * dy);

  // Azimuth is chosen at random.
  const double phi = TwoPi * RndmUniform();
  const double cphi = cos(phi);
  const double sphi = sin(phi); 

  if (argZ == 0.) {
    dz = ctheta;
    dx = cphi * stheta;
    dy = sphi * stheta;
  } else {
    const double a = stheta / argZ;
    const double dz1 = dz * ctheta + argZ * stheta * sphi;
    const double dy1 = dy * ctheta + a * (dx * cphi - dy * dz * sphi);
    const double dx1 = dx * ctheta - a * (dy * cphi + dx * dz * sphi);
    dz = dz1; dy = dy1; dx = dx1;
  }
  
  return true;

}

bool
MediumMagboltz::GetDeexcitationProduct(const int i, double& t, double& s,
                                         int& type, double& energy) {

  if (i < 0 || i >= nDeexcitationProducts || 
      !(useDeexcitation || usePenning)) return false;
  t = dxcProducts[i].t;
  s = dxcProducts[i].s;
  type = dxcProducts[i].type;
  energy = dxcProducts[i].energy;
  return true;

}

bool
MediumMagboltz::GetIonisationProduct(const int i, 
                                     int& type, double& energy) {

  if (i < 0 || i >= nIonisationProducts) {
    std::cerr << className << "::GetIonisationProduct:\n";
    std::cerr << "    Index out of range.\n";
    return false;
  }

  type = ionProducts[i].type;
  energy = ionProducts[i].energy;
  return true;

}

double 
MediumMagboltz::GetPhotonCollisionRate(const double e) {

  if (e <= 0.) {
    std::cerr << className << "::GetPhotonCollisionRate:\n";
    std::cerr << "    Photon energy must be greater than zero.\n";
    return cfTotGamma[0];
  }
  if (e > eFinalGamma && useAutoAdjust) {
    std::cerr << className << "::GetPhotonCollisionRate:\n";
    std::cerr << "    Collision rate at " << e 
              << " eV is not included in the current table.\n";
    std::cerr << "    Increasing energy range to " << 1.05 * e
              << " eV.\n";
    SetMaxPhotonEnergy(1.05 * e);
  }
    
  if (isChanged) {
    if (!Mixer()) {
      std::cerr << className << "::GetPhotonCollisionRate:\n";
      std::cerr << "     Error calculating the collision rates table.\n";
      return 0.;
    }
    isChanged = false;
  }

  int iE = int(e / eStepGamma);
  if (iE >= nEnergyStepsGamma) iE = nEnergyStepsGamma - 1;
  if (iE < 0) iE = 0;
  
  double cfSum = cfTotGamma[iE];
  if (useDeexcitation && useRadTrap && nDeexcitations > 0) {
    // Loop over the excitations.
    for (int i = nDeexcitations; i--;) {
      if (deexcitations[i].cf > 0. && 
          fabs(e - deexcitations[i].energy) <= deexcitations[i].width) {
        cfSum += deexcitations[i].cf * 
                 TMath::Voigt(e - deexcitations[i].energy,
                              deexcitations[i].sDoppler, 
                              2 * deexcitations[i].gPressure);
      }
    }
  }

  return cfSum;

}

bool
MediumMagboltz::GetPhotonCollision(const double e, int& type, int& level,
                                     double& e1, double& ctheta, 
                                     int& nsec, double& esec) {

  if (e > eFinalGamma && useAutoAdjust) {
    std::cerr << className << "::GetPhotonCollision:\n";
    std::cerr << "    Provided electron energy  (" << e 
              << " eV) exceeds current energy range  (" << eFinalGamma
              << " eV).\n";
    std::cerr << "    Increasing energy range to " << 1.05 * e
              << " eV.\n";
    SetMaxPhotonEnergy(1.05 * e);
  } else if (e <= 0.) {
    std::cerr << className << "::GetPhotonCollision:\n";
    std::cerr << "    Photon energy must be greater than zero.\n";
    return false;
  }
  
  if (isChanged) {
    if (!Mixer()) {
      std::cerr << "MediumMagboltz: Error calculating" 
                << " the collision rates table.\n";
      return false;
    }
    isChanged = false;
  }

  // Energy interval
  int iE = int(e / eStepGamma);
  if (iE >= nEnergyStepsGamma) iE = nEnergyStepsGamma - 1;
  if (iE < 0) iE = 0;
  
  double r = cfTotGamma[iE];
  if (useDeexcitation && useRadTrap && nDeexcitations > 0) {
    int nLines = 0;
    std::vector<double> pLine(0);
    std::vector<int> iLine(0);
    // Loop over the excitations.
    for (int i = nDeexcitations; i--;) {
      if (deexcitations[i].cf > 0. && 
          fabs(e - deexcitations[i].energy) <= deexcitations[i].width) {
        r += deexcitations[i].cf * 
             TMath::Voigt(e - deexcitations[i].energy,
                          deexcitations[i].sDoppler, 
                          2 * deexcitations[i].gPressure);
        pLine.push_back(r);
        iLine.push_back(i);
        ++nLines;
      }
    }
    r *= RndmUniform();
    if (nLines > 0 && r >= cfTotGamma[iE]) {
      // Photon is absorbed by a discrete line.
      for (int i = 0; i < nLines; ++i) {
        if (r <= pLine[i]) {
          ++nPhotonCollisions[PhotonCollisionTypeExcitation];
          int fLevel = 0;
          ComputeDeexcitationInternal(iLine[i], fLevel);
          type = PhotonCollisionTypeExcitation;
          nsec = nDeexcitationProducts;
          return true;
        }
      }
      std::cerr << className << "::GetPhotonCollision:\n";
      std::cerr << "    Random sampling of deexcitation line failed.\n";
      std::cerr << "    Program bug!\n";
      return false;
    }
  } else {
    r *= RndmUniform();
  }

  int iLow = 0;
  int iUp  = nPhotonTerms - 1;  
  if (r <= cfGamma[iE][iLow]) {
    level = iLow;
  } else if (r >= cfGamma[iE][iUp]) {
    level = iUp;
  } else {
    int iMid;
    while (iUp - iLow > 1) {
      iMid = (iLow + iUp) >> 1;
      if (r < cfGamma[iE][iMid]) {
        iUp = iMid;
      } else {
        iLow = iMid;
      }
    }
    level = iUp;
  }
 
  nsec = 0;  
  esec = e1 = 0.;
  type = csTypeGamma[level];
  // Collision type
  type = type % nCsTypesGamma;
  int ngas = int(csTypeGamma[level] / nCsTypesGamma);
  ++nPhotonCollisions[type];
  // Ionising collision
  if (type == 1) {
    esec = e - ionPot[ngas];
    if (esec < Small) esec = Small;
    nsec = 1;
  }

  // Determine the scattering angle
  ctheta = 1. - 2 * RndmUniform();
  
  return true;

}

void 
MediumMagboltz::ResetCollisionCounters() {

  for (int j = nCsTypes; j--;) nCollisions[j] = 0;
  nCollisionsDetailed.resize(nTerms);
  for (int j = nTerms; j--;) nCollisionsDetailed[j] = 0;
  nPenning = 0;
  for (int j = nCsTypesGamma; j--;) nPhotonCollisions[j] = 0;
  
}

int 
MediumMagboltz::GetNumberOfElectronCollisions() const {

  int ncoll = 0;
  for (int j = nCsTypes; j--;) ncoll += nCollisions[j];
  return ncoll;
  
}

int 
MediumMagboltz::GetNumberOfElectronCollisions(
        int& nElastic,   int& nIonisation, int& nAttachment, 
        int& nInelastic, int& nExcitation, int& nSuperelastic) const {

  nElastic      = nCollisions[ElectronCollisionTypeElastic];
  nIonisation   = nCollisions[ElectronCollisionTypeIonisation];
  nAttachment   = nCollisions[ElectronCollisionTypeAttachment]; 
  nInelastic    = nCollisions[ElectronCollisionTypeInelastic];
  nExcitation   = nCollisions[ElectronCollisionTypeExcitation]; 
  nSuperelastic = nCollisions[ElectronCollisionTypeSuperelastic];
  return nElastic + nIonisation + nAttachment +
         nInelastic + nExcitation + nSuperelastic;

}

int 
MediumMagboltz::GetNumberOfLevels() {

  if (isChanged) {
    if (!Mixer()) {
      std::cerr << className << "::GetNumberOfLevels:\n";
      std::cerr << "    Error calculating the collision rates table.\n";
      return 0;
    }
    isChanged = false;
  }

  return nTerms;

}

bool 
MediumMagboltz::GetLevel(const int i, int& ngas, int& type,
                           std::string& descr, double& e) {

  if (isChanged) {
    if (!Mixer()) {
      std::cerr << className << "::GetLevel:\n";
      std::cerr << "    Error calculating the collision rates table.\n";
      return false;
    }
    isChanged = false;
  }

  if (i < 0 || i >= nTerms) {
    std::cerr << className << "::GetLevel:\n";
    std::cerr << "    Requested level (" << i
              << ") does not exist.\n";
    return false;
  }  
  
  // Collision type
  type = csType[i] % nCsTypes;
  ngas = int(csType[i] / nCsTypes);
  // Description (from Magboltz)
  descr = "                              ";
  for (int j = 30; j--;) descr[j] = description[i][j];
  // Threshold energy
  e = rgas[ngas] * energyLoss[i];
  if (debug) {
    std::cout << className << "::GetLevel:\n";
    std::cout << "    Level " << i << ": " << descr << "\n";
    std::cout << "    Type " << type << "\n",
    std::cout << "    Threshold energy: " << e << " eV\n";   
    if (type == ElectronCollisionTypeExcitation && 
        usePenning && e > minIonPot) {
      std::cout << "    Penning transfer coefficient: " 
                << rPenning[i] << "\n";
    } else if (type == ElectronCollisionTypeExcitation && 
               useDeexcitation) {
      const int idxc = iDeexcitation[i];
      if (idxc < 0|| idxc >= nDeexcitations) {
        std::cout << "    Deexcitation cascade not implemented.\n";
        return true;
      }
      if (deexcitations[idxc].osc > 0.) { 
        std::cout << "    Oscillator strength: " 
                  << deexcitations[idxc].osc << "\n";
      }
      std::cout << "    Decay channels:\n";
      for (int j = 0; j < deexcitations[idxc].nChannels; ++j) {
        if (deexcitations[idxc].type[j] == 0) {
          std::cout << "      Radiative decay to ";
          if (deexcitations[idxc].final[j] < 0) {
            std::cout << "ground state: ";
          } else {
            std::cout << deexcitations[deexcitations[idxc].final[j]].label
                      << ": ";
          }
        } else if (deexcitations[idxc].type[j] == 1) {
          if (deexcitations[idxc].final[j] < 0) {
            std::cout << "      Penning ionisation: ";
          } else {
            std::cout << "      Associative ionisation: ";
          }
        } else if (deexcitations[idxc].type[j] == -1) {
          if (deexcitations[idxc].final[j] >= 0) {
            std::cout << "      Collision-induced transition to "
                      << deexcitations[deexcitations[idxc].final[j]].label
                      << ": ";
          } else {
            std::cout << "      Loss: ";
          }
        }
        if (j == 0) {
          std::cout << std::setprecision(5) 
                    << deexcitations[idxc].p[j] * 100. << "%\n";
        } else {
          std::cout << std::setprecision(5) 
                    << (deexcitations[idxc].p[j] - 
                        deexcitations[idxc].p[j - 1]) * 100. << "%\n";
        }
      } 
    }
  }

  return true;

}

int 
MediumMagboltz::GetNumberOfElectronCollisions(const int level) const {

  if (level < 0 || level >= nTerms) {
    std::cerr << className << "::GetNumberOfElectronCollisions:\n"; 
    std::cerr << "    Requested cross-section term (" 
              << level << ") does not exist.\n";
    return 0;
  }
  return nCollisionsDetailed[level];

}  

int
MediumMagboltz::GetNumberOfPhotonCollisions() const {

  int ncoll = 0;
  for (int j = nCsTypesGamma; j--;) ncoll += nPhotonCollisions[j];
  return ncoll;

}

int
MediumMagboltz::GetNumberOfPhotonCollisions(
    int& nElastic, int& nIonising, int& nInelastic) const {

  nElastic   = nPhotonCollisions[0];
  nIonising  = nPhotonCollisions[1];
  nInelastic = nPhotonCollisions[2];
  return nElastic + nIonising + nInelastic;

}

bool 
MediumMagboltz::GetGasNumberMagboltz(const std::string input, int& number) const {

  if (input == "") {
    number = 0; return false;
  }
 
  // CF4
  if (input == "CF4") { 
    number = 1; return true;
  }
  // Argon
  if (input == "Ar") {
    number = 2; return true;
  }
  // Helium 4
  if (input == "He" || input == "He-4") {
    number = 3; return true;
  }
  // Helium 3
  if (input == "He-3") {
    number = 4; return true;
  }
  // Neon
  if (input == "Ne") {
    number = 5; return true;
  }
  // Krypton
  if (input == "Kr") {
    number = 6; return true;
  }
  // Xenon
  if (input == "Xe") {
    number = 7; return true;
  }
  // Methane
  if (input == "CH4") {
    number = 8; return true;
  }
  // Ethane
  if (input == "C2H6") {
    number = 9; return true;
  }
  // Propane
  if (input == "C3H8") {
    number = 10; return true;
  }
  // Isobutane
  if (input == "iC4H10") {
    number = 11; return true;
  }
  // Carbon dioxide (CO2)
  if (input == "CO2") {
    number = 12; return true;
  }
  // Neopentane
  if (input == "neoC5H12") {
    number = 13; return true;
  }
  // Water
  if (input == "H2O") {
    number = 14; return true;
  }
  // Oxygen
  if (input == "O2") {
    number = 15; return true;
  }
  // Nitrogen
  if (input == "N2") {
    number = 16; return true;
  }
  // Nitric oxide (NO)
  if (input == "NO") {
    number = 17; return true;
  }
  // Nitrous oxide (N2O)
  if (input == "N2O") {
    number = 18; return true;
  }
  // Ethene (C2H4)
  if (input == "C2H4") {
    number = 19; return true;
  }
  // Acetylene (C2H2)
  if (input == "C2H2") {
    number = 20; return true;
  }
  // Hydrogen
  if (input == "H2") {
    number = 21; return true;
  }
  // Deuterium
  if (input == "D2") {
    number = 22; return true;
  }
  // Carbon monoxide (CO)
  if (input == "CO") {
    number = 23; return true;
  }
  // Methylal (dimethoxymethane, CH3-O-CH2-O-CH3, "hot" version)
  if (input == "Methylal") {
    number = 24; return true;
  }
  // DME
  if (input == "DME") {
    number = 25; return true;
  }
  // Reid step
  if (input == "Reid-Step") {
    number = 26; return true;
  }
  // Maxwell model
  if (input == "Maxwell-Model") {
    number = 27; return true;
  }
  // Reid ramp
  if (input == "Reid-Ramp") {
    number = 28; return true;
  }
  // C2F6
  if (input == "C2F6") {
    number = 29; return true;
  }
  // SF6
  if (input == "SF6") {
    number = 30; return true;
  }
  // NH3
  if (input == "NH3") {
    number = 31; return true;
  }
  // Propene
  if (input == "C3H6") {
    number = 32; return true;
  }
  // Cyclopropane
  if (input == "cC3H6") {
    number = 33; return true;
  }
  // Methanol
  if (input == "CH3OH") {
    number = 34; return true;
  }
  // Ethanol
  if (input == "C2H5OH") {
    number = 35; return true;
  }
  // Propanol
  if (input == "C3H7OH") {
    number = 36; return true;
  }
  // Cesium / Caesium.
  if (input == "Cs") {
    number = 37; return true;
  }
  // Fluorine
  if (input == "F2") {
    number = 38; return true;
  }
  if (input == "CS2") {
    number = 39; return true;
  }
  // COS
  if (input == "COS") {
    number = 40; return true;
  }
  // Deuterated methane
  if (input == "CD4") {
    number = 41; return true;
  }
  // BF3
  if (input == "BF3") {
    number = 42; return true;
  }
  // C2H2F4 (C2HF5).
  if (input == "C2HF5" || input == "C2H2F4") {
    number = 43; return true;
  }
  // CHF3
  if (input == "CHF3") {
    number = 50; return true;
  }
  // CF3Br
  if (input == "CF3Br") {
    number = 51; return true;
  }
  // C3F8
  if (input == "C3F8") {
    number = 52; return true;
  }
  // Ozone
  if (input == "O3") {
    number = 53; return true;
  }
  // Mercury
  if (input == "Hg") {
    number = 54; return true;
  }
  // H2S
  if (input == "H2S") {
    number = 55; return true;
  }
  // n-Butane
  if (input == "nC4H10") {
    number = 56; return true;
  }
  // n-Pentane
  if (input == "nC5H12") {
    number = 57; return true;
  }
  // Nitrogen
  if (input == "N2 (Phelps)") {
    number = 58; return true;
  }
  // Germane, GeH4
  if (input == "GeH4") {
    number = 59; return true;
  }
  // Silane, SiH4
  if (input == "SiH4") {
    number = 60; return true;
  }
  
  std::cerr << className << "::GetGasNumberMagboltz:\n";
  std::cerr << "    Gas " << input << " is not defined.\n";
  return false;
  
}

bool 
MediumMagboltz::Mixer() {

  // Set constants and parameters in Magboltz common blocks.
  cnsts_.echarg = ElementaryCharge;
  cnsts_.emass = ElectronMassGramme;
  cnsts_.amu = AtomicMassUnit;
  cnsts_.pir2 = BohrRadius * BohrRadius * Pi;  
  inpt_.ary = RydbergEnergy;

  inpt_.akt = BoltzmannConstant * temperature;
  inpt_.tempc = temperature - ZeroCelsius;
  inpt_.torr = pressure;

  inpt_.nGas = nComponents;
  inpt_.nStep = nEnergySteps;
  if (useAnisotropic) {
    inpt_.nAniso = 2;
  } else {
    inpt_.nAniso = 0;
  }
  
  // Calculate the atomic density (ideal gas law).
  const double dens = GetNumberDensity();
  // Prefactor for calculation of scattering rate from cross-section.
  const double prefactor = dens * SpeedOfLight * sqrt(2. / ElectronMass);

  // Fill the electron energy array, reset the collision rates.
  for (int i = nEnergySteps; i--;) {
    cfTot[i] = 0.;
    for (int j = nMaxLevels; j--;) {
      cf[i][j] = 0.;
      scatParameter[i][j] = 0.5;
      scatCut[i][j] = 1.;
    }
  }
  for (int i = nEnergyStepsLog; i--;) {
    cfTotLog[i] = 0.;
    for (int j = nMaxLevels; j--;) {
      cfLog[i][j] = 0.;
      scatParameter[i][j] = 0.5;
      scatCut[i][j] = 1.;
    }
  }

  nDeexcitations = 0;
  deexcitations.clear();
  for (int i = nMaxLevels; i--;) {
    scatModel[i] = 0;
    iDeexcitation[i] = -1;
    wOpalBeaty[i] = 1.;
  }

  minIonPot = -1.;
  for (int i = nMaxGases; i--;) {
    ionPot[i] = -1.;
    gsGreenSawada[i] = 1.;
    gbGreenSawada[i] = 0.;
    tsGreenSawada[i] = 0.;
    taGreenSawada[i] = 0.;
    tbGreenSawada[i] = 0.;
    hasGreenSawada[i] = false;
  }
  // Cross-sections
  // 0: total, 1: elastic, 
  // 2: ionisation, 3: attachment, 
  // 4, 5: unused
  static double q[nEnergySteps][6];
  // Parameters for scattering angular distribution
  static double pEqEl[nEnergySteps][6];
  // Inelastic cross-sections
  static double qIn[nEnergySteps][nMaxInelasticTerms];
  // Parameters for angular distribution in inelastic collisions
  static double pEqIn[nEnergySteps][nMaxInelasticTerms]; 
  // Penning transfer parameters
  static double penFra[nMaxInelasticTerms][3];
  // Description of cross-section terms
  static char scrpt[226][30];

  // Check the gas composition and establish the gas numbers.
  int gasNumber[nMaxGases];
  for (int i = 0; i < nComponents; ++i) {
    if (!GetGasNumberMagboltz(gas[i], gasNumber[i])) {
      std::cerr << className << "::Mixer:\n";
      std::cerr << "    Gas " << gas[i] << " has no corresponding"
                << " gas number in Magboltz.\n";
      return false;
    }
  }
  
  if (debug) {
    std::cout << className << "::Mixer:\n";
    std::cout << "    Creating table of collision rates with\n";
    std::cout << "      " << nEnergySteps 
                          << " linear energy steps between 0 and " 
                          << std::min(eFinal, eMinLog) << " eV\n";
    if (eFinal > eMinLog) {
      std::cout << "      " << nEnergyStepsLog 
                            << " logarithmic energy steps between "
                            << eMinLog << " and " << eFinal << " eV\n";
    }
  }
  nTerms = 0;
  
  std::ofstream outfile;
  if (useCsOutput) {
    outfile.open("cs.txt", std::ios::out);
    outfile << "# \n";
  }

  // Loop over the gases in the mixture.  
  for (int iGas = 0; iGas < nComponents; ++iGas) {
    if (eFinal <= eMinLog) {
      inpt_.efinal = eFinal;
    } else {
      inpt_.efinal = eMinLog;
    }
    inpt_.estep = eStep;
  
    // Number of inelastic cross-section terms
    long long nIn = 0;
    // Threshold energies
    double e[6] = {0., 0., 0., 0., 0., 0.};
    double eIn[nMaxInelasticTerms] = {0.};
    // Virial coefficient (not used)
    double virial = 0.;
    // Splitting function parameter
    double w = 0.;
    // Scattering algorithms
    long long kIn[nMaxInelasticTerms] = {0};
    long long kEl[6] = {0, 0, 0, 0, 0, 0};    
    char name[15];  

    // Retrieve the cross-section data for this gas from Magboltz.
    long long ngs = gasNumber[iGas];
    gasmix_(&ngs, q[0], qIn[0], &nIn, e, eIn, name, &virial, &w, 
            pEqEl[0], pEqIn[0], penFra[0], kEl, kIn, scrpt);
    if (debug) {
      const double massAmu = (2. / e[1]) * 
                             ElectronMass / AtomicMassUnitElectronVolt;
      std::cout << "    " << name << "\n";
      std::cout << "      mass:                 " << massAmu << " amu\n"; 
      std::cout << "      ionisation threshold: " << e[2] << " eV\n";
      std::cout << "      attachment threshold: " << e[3] << " eV\n";
      std::cout << "      splitting parameter:  " << w << " eV\n";
      if (e[3] > 0. || e[4] > 0.) {
        std::cout << "      cross-sections at minimum ionising energy:\n";
        std::cout << "        excitation: " << e[3] * 1.e18 << " Mbarn\n";
        std::cout << "        ionisation: " << e[4] * 1.e18 << " Mbarn\n";
      }
    }
    int np0 = nTerms;
    
    // Make sure there is still sufficient space.
    if (np0 + nIn + 2 >= nMaxLevels) {
      std::cerr << className << "::Mixer:\n";
      std::cerr << "    Max. number of levels (" << nMaxLevels 
                << ") exceeded.\n";
      return false;
    }
    
    double van = fraction[iGas] * prefactor;
        
    int np = np0;
    // Elastic scattering
    ++nTerms;
    scatModel[np] = kEl[1];
    const double r = 1. + e[1] / 2.;
    rgas[iGas] = r;
    energyLoss[np] = 0.; 
    for (int j = 0; j < 30; ++j) {
      description[np][j] = scrpt[1][j];
    }
    csType[np] = nCsTypes * iGas + ElectronCollisionTypeElastic;
    bool withIon = false, withAtt = false;
    // Ionisation
    if (eFinal >= e[2]) {
      withIon = true;
      ++nTerms; ++np;
      scatModel[np] = kEl[2];
      energyLoss[np] = e[2] / r;
      wOpalBeaty[np] = w;
      gsGreenSawada[iGas] = w;
      tbGreenSawada[iGas] = 2 * e[2];
      ionPot[iGas] = e[2];
      for (int j = 0; j < 30; ++j) {
        description[np][j] = scrpt[2][j];
      }
      csType[np] = nCsTypes * iGas + ElectronCollisionTypeIonisation;
    }
    // Attachment
    if (eFinal >= e[3]) {
      withAtt = true;
      ++nTerms; ++np;
      scatModel[np] = kEl[3];
      energyLoss[np] = 0.;
      for (int j = 0; j < 30; ++j) {
        description[np][j] = scrpt[3][j];
      }
      csType[np] = nCsTypes * iGas + ElectronCollisionTypeAttachment;
    }
    // Inelastic terms
    int nExc = 0, nSuperEl = 0;
    for (int j = 0; j < nIn; ++j) {
      ++np;
      scatModel[np] = kIn[j];
      energyLoss[np] = eIn[j] / r;
      for (int k = 0; k < 30; ++k) {
        description[np][k] = scrpt[6 + j][k];
      }
      if ((description[np][1] == 'E' && description[np][2] == 'X') ||
          (description[np][0] == 'E' && description[np][1] == 'X') ||
          (gas[iGas] == "N2" && eIn[j] > 6.)) {
        // Excitation
        csType[np] = nCsTypes * iGas + ElectronCollisionTypeExcitation;    
        ++nExc;
      } else if (eIn[j] < 0.) {
        // Super-elastic collision
        csType[np] = nCsTypes * iGas + ElectronCollisionTypeSuperelastic;
        ++nSuperEl;
      } else {
        // Inelastic collision
        csType[np] = nCsTypes * iGas + ElectronCollisionTypeInelastic;
      }
    }
    nTerms += nIn;
    // Loop over the energy table.
    for (int iE = 0; iE < nEnergySteps; ++iE) {
      np = np0;
      if (useCsOutput) {
        outfile << iE * eStep << "  " << q[iE][1] << "  " << q[iE][2] 
                << "  " << q[iE][3] << "  " << q[iE][4] << "  ";
      }
      // Elastic scattering
      cf[iE][np] = q[iE][1] * van;
      if (scatModel[np] == 1) {
        ComputeAngularCut(pEqEl[iE][1], scatCut[iE][np], 
                          scatParameter[iE][np]);
      } else if (scatModel[np] == 2) {
        scatParameter[iE][np] = pEqEl[iE][1];
      }
      // Ionisation
      if (withIon) {
        ++np;
        cf[iE][np] = q[iE][2] * van;
        if (scatModel[np] == 1) {
          ComputeAngularCut(pEqEl[iE][2], scatCut[iE][np], 
                            scatParameter[iE][np]);
        } else if (scatModel[np] == 2) {
          scatParameter[iE][np] = pEqEl[iE][2];
        }
      }
      // Attachment
      if (withAtt) {
        ++np;
        cf[iE][np] = q[iE][3] * van;
      }
      // Inelastic terms
      for (int j = 0; j < nIn; ++j) {
        ++np;
        if (useCsOutput) outfile << qIn[iE][j] << "  ";
        cf[iE][np] = qIn[iE][j] * van;
        // Scale the excitation cross-sections (for error estimates).
        cf[iE][np] *= scaleExc;
        // Temporary hack for methane dissociative excitations:
        if (description[np][5] == 'D' &&
            description[np][6] == 'I' &&
            description[np][7] == 'S') {
          // if (iE * eStep > 40.) {
          //   cf[iE][np] *= 0.8;
          // } else if (iE * eStep > 30.) {
          //   cf[iE][np] *= (1. - (iE * eStep - 30.) * 0.02);
          // }
        }
        if (cf[iE][np] < 0.) {
          std::cerr << className << "::Mixer:\n";
          std::cerr << "    Negative inelastic cross-section at " 
                    << iE * eStep << " eV.\n"; 
          std::cerr << "    Set to zero.\n";
          cf[iE][np] = 0.;
        }
        if (scatModel[np] == 1) {
          ComputeAngularCut(pEqIn[iE][j], scatCut[iE][np], 
                            scatParameter[iE][np]);
        } else if (scatModel[np] == 2) {
          scatParameter[iE][np] = pEqIn[iE][j];
        }
      }
      if (debug && nIn > 0 && iE == nEnergySteps - 1) {
        std::cout << "      " << nIn << " inelastic terms ("
                  << nExc << " excitations, " 
                  << nSuperEl << " superelastic, "
                  << nIn - nExc - nSuperEl << " other)\n";
      }
      if (useCsOutput) outfile << "\n";
    }
    
    if (eFinal <= eMinLog) continue;
    // Fill the high-energy part.
    const double rLog = pow(eFinal / eMinLog, 1. / nEnergyStepsLog);
    double emax = 0.5 * eMinLog * (1. + rLog);
    for (int iE = 0; iE < nEnergyStepsLog; ++iE) {
      inpt_.efinal = emax;
      inpt_.estep = emax / nEnergySteps;
      gasmix_(&ngs, q[0], qIn[0], &nIn, e, eIn, name, &virial, &w, 
              pEqEl[0], pEqIn[0], penFra[0], kEl, kIn, scrpt);
      np = np0;
      if (useCsOutput) {
        outfile << emax << "  " << q[nEnergySteps - 1][1] 
                        << "  " << q[nEnergySteps - 1][2] 
                        << "  " << q[nEnergySteps - 1][3] 
                        << "  " << q[nEnergySteps - 1][4] << "  ";
      }
      // Elastic scattering
      cfLog[iE][np] = q[nEnergySteps - 1][1] * van;
      if (scatModel[np] == 1) {
        ComputeAngularCut(pEqEl[nEnergySteps - 1][1], 
                          scatCutLog[iE][np], 
                          scatParameterLog[iE][np]);
      } else if (scatModel[np] == 2) {
        scatParameterLog[iE][np] = pEqEl[nEnergySteps - 1][1];
      }
      // Ionisation
      if (withIon) {
        ++np;
        cfLog[iE][np] = q[nEnergySteps - 1][2] * van;
        if (scatModel[np] == 1) {
          ComputeAngularCut(pEqEl[nEnergySteps - 1][2], 
                            scatCutLog[iE][np], 
                            scatParameterLog[iE][np]);
        } else if (scatModel[np] == 2) {
          scatParameterLog[iE][np] = pEqEl[nEnergySteps - 1][2];
        }
      }
      // Attachment
      if (withAtt) {
        ++np;
        cfLog[iE][np] = q[nEnergySteps - 1][3] * van;
      }
      // Inelastic terms
      for (int j = 0; j < nIn; ++j) {
        ++np;
        if (useCsOutput) outfile << qIn[nEnergySteps - 1][j] << "  ";
        cfLog[iE][np] = qIn[nEnergySteps - 1][j] * van;
        // Scale the excitation cross-sections (for error estimates).
        cfLog[iE][np] *= scaleExc;
        if (cfLog[iE][np] < 0.) {
          std::cerr << className << "::Mixer:\n";
          std::cerr << "    Negative inelastic cross-section at " 
                    << emax << " eV.\n"; 
          std::cerr << "    Set to zero.\n";
          cfLog[iE][np] = 0.;
        }
        if (scatModel[np] == 1) {
          ComputeAngularCut(pEqIn[nEnergySteps - 1][j], 
                            scatCutLog[iE][np], 
                            scatParameterLog[iE][np]);
        } else if (scatModel[np] == 2) {
          scatParameterLog[iE][np] = pEqIn[nEnergySteps - 1][j];
        }
      }
      if (useCsOutput) outfile << "\n";
      emax *= rLog;
    }
  }
  if (useCsOutput) outfile.close();
  
  // Find the smallest ionisation threshold.
  for (int i = nMaxGases; i--;) {
    if (ionPot[i] < 0.) continue;
    if (minIonPot < 0.) {
      minIonPot = ionPot[i];
    } else if (ionPot[i] < minIonPot) {
      minIonPot = ionPot[i];
    }
  }

  if (debug) {
    std::cout << className << "::Mixer:\n";
    std::cout << "    Lowest ionisation threshold in the mixture: " 
              << minIonPot << " eV\n";
  }

  for (int iE = nEnergySteps; iE--;) {
    // Calculate the total collision frequency.
    for (int k = nTerms; k--;) {
      if (cf[iE][k] < 0.) {
          std::cerr << className << "::Mixer:\n";
          std::cerr << "    Negative collision rate at " 
                    << iE * eStep << " eV. \n";
          std::cerr << "    Set to zero.\n";
          cf[iE][k] = 0.;
      }
      cfTot[iE] += cf[iE][k];
    }
    // Normalise the collision frequencies.
    if (cfTot[iE] > 0.) {
      for (int k = nTerms; k--;) cf[iE][k] /= cfTot[iE];
    }
    for (int k = 1; k < nTerms; ++k) {
      cf[iE][k] += cf[iE][k - 1];
    }
    const double eroot = sqrt(eStep * (iE + 0.5));
    cfTot[iE] *= eroot;
  }
  
  if (eFinal > eMinLog) {
    const double rLog = pow(eFinal / eMinLog, 1. / nEnergyStepsLog);
    for (int iE = nEnergyStepsLog; iE--;) {
      // Calculate the total collision frequency.
      for (int k = nTerms; k--;) {
        if (cfLog[iE][k] < 0.) {
          cfLog[iE][k] = 0.;
        }
        cfTotLog[iE] += cfLog[iE][k];
      }
      // Normalise the collision frequencies.
      if (cfTotLog[iE] > 0.) {
        for (int k = nTerms; k--;) cfLog[iE][k] /= cfTotLog[iE];
      }
      for (int k = 1; k < nTerms; ++k) {
        cfLog[iE][k] += cfLog[iE][k - 1];
      }
      const double eroot = sqrt(eMinLog * pow(rLog, iE) * 
                                (1. + rLog) / 2.);
      cfTotLog[iE] *= eroot;
    }
  }
  
  // Determine the null collision frequency.
  cfNull = 0.;
  for (int j = 0; j < nEnergySteps; ++j) {
    if (cfTot[j] >= cfNull) cfNull = cfTot[j];
  }
  for (int j = 0; j < nEnergyStepsLog; ++j) {
    if (cfTotLog[j] >= cfNull) cfNull = cfTotLog[j];
  } 

  // Reset the collision counters.
  nCollisionsDetailed.resize(nTerms);
  for (int j = nCsTypes; j--;) nCollisions[j] = 0;
  for (int j = nTerms; j--;) nCollisionsDetailed[j] = 0;
  
  if (debug) {
    std::cout << className << "::Mixer:\n";
    std::cout << "    Energy [eV]    Collision Rate [ns-1]\n";
    for (int i = 0; i < 8; ++i) {
      const double emax = std::min(eMinLog, eFinal); 
      std::cout << "    " << std::fixed << std::setw(10) << std::setprecision(2)  
                << (2 * i + 1) * emax / 16
                << "    " << std::setw(18) << std::setprecision(2)
                << cfTot[(i + 1) * nEnergySteps / 16] << "\n";
    }
    std::cout << std::resetiosflags(std::ios_base::floatfield);
  }

  // Set up the de-excitation channels.
  if (useDeexcitation) ComputeDeexcitationTable();
  // Fill the photon collision rates table.
  if (!ComputePhotonCollisionTable()) {
    std::cerr << className << "::Mixer:\n";
    std::cerr << "    Photon collision rates could not be calculated.\n"; 
    if (useDeexcitation) {
      std::cerr << "    Deexcitation handling is switched off.\n";
      useDeexcitation = false;
    }
  }
  
  // Reset the Penning transfer parameters.
  for (int i = nTerms; i--;) {
    rPenning[i] = rPenningGlobal;
    int iGas = int(csType[i] / nCsTypes);
    if (rPenningGas[iGas] > Small) {
      rPenning[i] = rPenningGas[iGas];
      lambdaPenning[i] = lambdaPenningGas[iGas];
    }
  }

  // Set the Green-Sawada splitting function parameters.
  SetupGreenSawada(); 
  
  return true;

}

void
MediumMagboltz::SetupGreenSawada() {

  for (int i = nComponents; i--;) {
    taGreenSawada[i] = 1000.;
    hasGreenSawada[i] = true;
    if (gas[i] == "He" || gas[i] == "He-3") {
      tsGreenSawada[i] = -2.25;
      gsGreenSawada[i] = 15.5;
      gbGreenSawada[i] = 24.5;
    } else if (gas[i] == "Ne") {
      tsGreenSawada[i] = -6.49;
      gsGreenSawada[i] = 24.3;
      gbGreenSawada[i] = 21.6;
    } else if (gas[i] == "Ar") {
      tsGreenSawada[i] =  6.87;
      gsGreenSawada[i] =  6.92;
      gbGreenSawada[i] =  7.85;
    } else if (gas[i] == "Kr") {
      tsGreenSawada[i] =  3.90;
      gsGreenSawada[i] =  7.95;
      gbGreenSawada[i] = 13.5;
    } else if (gas[i] == "Xe") {
      tsGreenSawada[i] =  3.81;
      gsGreenSawada[i] =  7.93;
      gbGreenSawada[i] = 11.5;
    } else if (gas[i] == "H2" || gas[i] == "D2") {
      tsGreenSawada[i] =  1.87;
      gsGreenSawada[i] =  7.07;
      gbGreenSawada[i] =  7.7;
    } else if (gas[i] == "N2") {
      tsGreenSawada[i] =  4.71;
      gsGreenSawada[i] = 13.8;
      gbGreenSawada[i] = 15.6;
    } else if (gas[i] == "O2") {
      tsGreenSawada[i] =  1.86;
      gsGreenSawada[i] = 18.5;
      gbGreenSawada[i] = 12.1;
    } else if (gas[i] == "CH4") {
      tsGreenSawada[i] =  3.45;
      gsGreenSawada[i] =  7.06;
      gbGreenSawada[i] = 12.5;
    } else if (gas[i] == "H20") {
      tsGreenSawada[i] =  1.28;
      gsGreenSawada[i] = 12.8;
      gbGreenSawada[i] = 12.6;
    } else if (gas[i] == "CO") {
      tsGreenSawada[i] =  2.03;
      gsGreenSawada[i] = 13.3;
      gbGreenSawada[i] = 14.0;
    } else if (gas[i] == "C2H2") {
      tsGreenSawada[i] =  1.37;
      gsGreenSawada[i] =  9.28;
      gbGreenSawada[i] =  5.8;
    } else if (gas[i] == "NO") {
      tsGreenSawada[i] = -4.30;
      gsGreenSawada[i] = 10.4;
      gbGreenSawada[i] =  9.5;
    } else if (gas[i] == "CO2") {
      tsGreenSawada[i] = -2.46;
      gsGreenSawada[i] = 12.3;
      gbGreenSawada[i] = 13.8;
    } else {
      taGreenSawada[i] = 0.;
      hasGreenSawada[i] = false;
      if (useGreenSawada || debug) {
        std::cout << className << "::SetupGreenSawada:\n";
        std::cout << "    Fit parameters for " 
                  << gas[i] << " not available.\n";
        std::cout << "    Opal-Beaty formula is used instead.\n";
      } 
    }
  }
    
} 
    
void 
MediumMagboltz::ComputeAngularCut(double parIn, double& cut, double &parOut) {

  // Set cuts on angular distribution and
  // renormalise forward scattering probability.

  if (parIn <= 1.) {
    cut = 1.;
    parOut = parIn;
    return;
  }

  const double rads = 2. / Pi;
  const double cns = parIn - 0.5;
  const double thetac = asin(2. * sqrt(cns - cns * cns));
  const double fac = (1. - cos(thetac)) / pow(sin(thetac), 2.); 
  parOut = cns * fac + 0.5;
  cut = thetac * rads;
  
}

void
MediumMagboltz::ComputeDeexcitationTable() {

  for (int i = nMaxLevels; i--;) iDeexcitation[i] = -1;
  deexcitations.clear();

  // Optical data (for quencher photoabsorption cs and ionisation yield)
  OpticalData optData;

  // Presence flags, concentrations and indices of "de-excitable" gases.
  bool withAr = false; double cAr = 0.; int iAr = 0;
  bool withNe = false; double cNe = 0.; int iNe = 0;

  std::map<std::string, int> mapLevels;
  // Make a mapping of all excitation levels.
  for (int i = 0; i < nTerms; ++i) {
    // Check if the level is an excitation.
    if (csType[i] % nCsTypes != ElectronCollisionTypeExcitation) continue;
    // Extract the index of the gas.
    const int ngas = int(csType[i] / nCsTypes);
    if (gas[ngas] == "Ar") {
      // Argon
      if (!withAr) {
        withAr = true;
        iAr = ngas;
        cAr = fraction[iAr];
      }
      // Get the level description (as specified in Magboltz).
      std::string level = "       ";
      for (int j = 0; j < 7; ++j) level[j] = description[i][5 + j];
      if      (level == "1S5    ") mapLevels["Ar_1S5"] = i;
      else if (level == "1S4    ") mapLevels["Ar_1S4"] = i;
      else if (level == "1S3    ") mapLevels["Ar_1S3"] = i;
      else if (level == "1S2    ") mapLevels["Ar_1S2"] = i;
      else if (level == "2P10   ") mapLevels["Ar_2P10"] = i;
      else if (level == "2P9    ") mapLevels["Ar_2P9"] = i;
      else if (level == "2P8    ") mapLevels["Ar_2P8"] = i;
      else if (level == "2P7    ") mapLevels["Ar_2P7"] = i;
      else if (level == "2P6    ") mapLevels["Ar_2P6"] = i;
      else if (level == "2P5    ") mapLevels["Ar_2P5"] = i;
      else if (level == "2P4    ") mapLevels["Ar_2P4"] = i;
      else if (level == "2P3    ") mapLevels["Ar_2P3"] = i;
      else if (level == "2P2    ") mapLevels["Ar_2P2"] = i;
      else if (level == "2P1    ") mapLevels["Ar_2P1"] = i;
      else if (level == "3D6    ") mapLevels["Ar_3D6"] = i;
      else if (level == "3D5    ") mapLevels["Ar_3D5"] = i;
      else if (level == "3D3    ") mapLevels["Ar_3D3"] = i;
      else if (level == "3D4!   ") mapLevels["Ar_3D4!"]= i;
      else if (level == "3D4    ") mapLevels["Ar_3D4"] = i;
      else if (level == "3D1!!  ") mapLevels["Ar_3D1!!"] = i;
      else if (level == "2S5    ") mapLevels["Ar_2S5"] = i;
      else if (level == "2S4    ") mapLevels["Ar_2S4"] = i;
      else if (level == "3D1!   ") mapLevels["Ar_3D1!"] = i;
      else if (level == "3D2    ") mapLevels["Ar_3D2"] = i;
      else if (level == "3S1!!!!") mapLevels["Ar_3S1!!!!"] = i;
      else if (level == "3S1!!  ") mapLevels["Ar_3S1!!"] = i;
      else if (level == "3S1!!! ") mapLevels["Ar_3S1!!!"] = i;
      else if (level == "2S3    ") mapLevels["Ar_2S3"] = i;
      else if (level == "2S2    ") mapLevels["Ar_2S2"] = i;
      else if (level == "3S1!   ") mapLevels["Ar_3S1!"] = i;
      else if (level == "4D5    ") mapLevels["Ar_4D5"] = i;
      else if (level == "3S4    ") mapLevels["Ar_3S4"] = i;
      else if (level == "4D2    ") mapLevels["Ar_4D2"] = i;
      else if (level == "4S1!   ") mapLevels["Ar_4S1!"] = i;
      else if (level == "3S2    ") mapLevels["Ar_3S2"] = i;
      else if (level == "5D5    ") mapLevels["Ar_5D5"] = i;
      else if (level == "4S4    ") mapLevels["Ar_4S4"] = i;
      else if (level == "5D2    ") mapLevels["Ar_5D2"] = i;
      else if (level == "6D5    ") mapLevels["Ar_6D5"] = i;
      else if (level == "5S1!   ") mapLevels["Ar_5S1!"] = i;
      else if (level == "4S2    ") mapLevels["Ar_4S2"] = i;
      else if (level == "5S4    ") mapLevels["Ar_5S4"] = i;
      else if (level == "6D2    ") mapLevels["Ar_6D2"] = i;
      else if (level == "HIGH   ") mapLevels["Ar_Higher"] = i;
      else {
        std::cerr << className << "::ComputeDeexcitationTable:\n";
        std::cerr << "    Unknown excitation level:\n";
        std::cerr << "      Ar " << level << "\n";
      }
    } else if (gas[ngas] == "Ne") {
      // Neon
      if (!withNe) {
        withNe = true;
        iNe = ngas;
        cNe = fraction[iNe];
      }
      std::string level = "       ";
      for (int j = 0; j < 7; ++j) level[j] = description[i][3 + j];
      if      (level == "  1S5  ") mapLevels["Ne_1S5"] = i;
      else if (level == "  1S4  ") mapLevels["Ne_1S4"] = i;
      else if (level == "  1S3  ") mapLevels["Ne_1S3"] = i;
      else if (level == "  1S2  ") mapLevels["Ne_1S2"] = i;
      else if (level == " 2P10  ") mapLevels["Ne_2P10"] = i;
      else if (level == "  2P9  ") mapLevels["Ne_2P9"] = i;
      else if (level == "  2P8  ") mapLevels["Ne_2P8"] = i;
      else if (level == "  2P7  ") mapLevels["Ne_2P7"] = i;
      else if (level == "  2P6  ") mapLevels["Ne_2P6"] = i;
      else if (level == "  2P5  ") mapLevels["Ne_2P5"] = i;
      else if (level == "  2P4  ") mapLevels["Ne_2P4"] = i;
      else if (level == "  2P3  ") mapLevels["Ne_2P3"] = i;
      else if (level == "  2P2  ") mapLevels["Ne_2P2"] = i;
      else if (level == "  2P1  ") mapLevels["Ne_2P1"] = i;
      else if (level == "  2S5  ") mapLevels["Ne_2S5"] = i;
      else if (level == "  2S4  ") mapLevels["Ne_2S4"] = i;
      else if (level == "  2S3  ") mapLevels["Ne_2S3"] = i;
      else if (level == "  2S2  ") mapLevels["Ne_2S2"] = i;
      else if (level == "  3D6  ") mapLevels["Ne_3D6"] = i;
      else if (level == "  3D5  ") mapLevels["Ne_3D5"] = i;
      else if (level == " 3D4!  ") mapLevels["Ne_3D4!"] = i;
      else if (level == "  3D4  ") mapLevels["Ne_3D4"] = i;
      else if (level == "  3D3  ") mapLevels["Ne_3D3"] = i;
      else if (level == "  3D2  ") mapLevels["Ne_3D2"] = i;
      else if (level == " 3D1!! ") mapLevels["Ne_3D1!!"] = i;
      else if (level == " 3D1!  ") mapLevels["Ne_3D1!"] = i;
      else if (level == "3S1!!!!") mapLevels["Ne_3S1!!!!"] = i;
      else if (level == "3S1!!! ") mapLevels["Ne_3S1!!!"] = i;
      else if (level == " 3S1!! ") mapLevels["Ne_3S1!!"] = i;
      else if (level == "  3S1! ") mapLevels["Ne_3S1!"] = i;
      else if (level == "SUM 3P1") mapLevels["Ne_3P10_3P6"] = i;
      else if (level == "SUM 3P5") mapLevels["Ne_3P5_3P2"] = i;
      else if (level == "  3P1  ") mapLevels["Ne_3P1"] = i;
      else if (level == "  3S4  ") mapLevels["Ne_3S4"] = i;
      else if (level == "  3S2  ") mapLevels["Ne_3S2"] = i;
      else if (level == "  4D5  ") mapLevels["Ne_4D5"] = i;
      else if (level == "  4D2  ") mapLevels["Ne_4D2"] = i;
      else if (level == " 4S1!  ") mapLevels["Ne_4S1!"] = i;
      else if (level == "  4S4  ") mapLevels["Ne_4S4"] = i;
      else if (level == "  5D5  ") mapLevels["Ne_5D5"] = i;
      else if (level == "  5D2  ") mapLevels["Ne_5D2"] = i;
      else if (level == "  4S2  ") mapLevels["Ne_4S2"] = i;
      else if (level == " 5S1!  ") mapLevels["Ne_5S1!"] = i;
      else if (level == "SUM S H") mapLevels["Ne_Sum_S_High"] = i;
      else if (level == "SUM D H") mapLevels["Ne_Sum_P_High"] = i;
      else {
        std::cerr << className << "::ComputeDeexcitationTable:\n";
        std::cerr << "    Unknown excitation level:\n";
        std::cerr << "      Ne " << level << "\n";
      }
      break;
    }
  }

  // Count the excited levels.
  std::map<std::string, int> mapDxc;
  std::map<std::string, int>::iterator itMap;
  nDeexcitations = 0;
  for (itMap = mapLevels.begin(); itMap != mapLevels.end(); itMap++) {
    std::string level = (*itMap).first;
    mapDxc[level] = nDeexcitations;
    iDeexcitation[(*itMap).second] = nDeexcitations;
    ++nDeexcitations;
  }

  // Conversion factor from oscillator strength to transition rate.
  const double f2A = 2. * SpeedOfLight * FineStructureConstant / 
                    (3. * ElectronMass * HbarC);

  // Radiative de-excitation channels
  // Transition rates (unless indicated otherwise) are taken from:
  //     NIST Atomic Spectra Database 
  // Transition rates for lines missing in the NIST database:
  //     O. Zatsarinny and K. Bartschat, J. Phys. B 39 (2006), 2145-2158
  // Oscillator strengths not included in the NIST database:
  //     J. Berkowitz, Atomic and Molecular Photoabsorption (2002)
  //     C.-M. Lee and K. T. Lu, Phys. Rev. A 8 (1973), 1241-1257
  deexcitation newDxc;
  for (itMap = mapLevels.begin(); itMap != mapLevels.end(); itMap++) {
    std::string level = (*itMap).first;
    newDxc.gas = int(csType[(*itMap).second] / nCsTypes);
    newDxc.level = (*itMap).second;
    newDxc.label = level;
    // Excitation energy
    newDxc.energy = energyLoss[(*itMap).second] * rgas[newDxc.gas];
    // Oscillator strength
    newDxc.osc = newDxc.cf = 0.;
    newDxc.sDoppler = newDxc.gPressure = newDxc.width = 0.;
    newDxc.p.clear(); newDxc.final.clear(); newDxc.type.clear();
    newDxc.nChannels = 0;
    if (level == "Ar_1S3" || level == "Ar_1S5") {
      // Radiative decay of metastables is neglected.
      newDxc.p.clear(); newDxc.final.clear(); newDxc.type.clear(); 
    } else if (level == "Ar_1S4") {
      // Oscillator strength from NIST database
      newDxc.osc = 0.0609;
      // Berkowitz: f = 0.058
      int nc = 1; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 0.119; newDxc.final[0] = -1;
    } else if (level == "Ar_1S2") {
      // Oscillator strength from NIST database
      newDxc.osc = 0.25;
      // Berkowitz: 0.2214
      int nc = 1; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 0.51; newDxc.final[0] = -1;
    } else if (level == "Ar_2P10") {
      int nc = 4; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 0.0189;  newDxc.final[0] = mapDxc["Ar_1S5"];
      newDxc.p[1] = 5.43e-3; newDxc.final[1] = mapDxc["Ar_1S4"];
      newDxc.p[2] = 9.8e-4;  newDxc.final[2] = mapDxc["Ar_1S3"];
      newDxc.p[3] = 1.9e-4;  newDxc.final[3] = mapDxc["Ar_1S2"];
    } else if (level == "Ar_2P9") {
      int nc = 1; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 0.0331; newDxc.final[0] = mapDxc["Ar_1S5"];
    } else if (level == "Ar_2P8") {
      int nc = 3; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 9.28e-3; newDxc.final[0] = mapDxc["Ar_1S5"];
      newDxc.p[1] = 0.0215;  newDxc.final[1] = mapDxc["Ar_1S4"];
      newDxc.p[2] = 1.47e-3; newDxc.final[2] = mapDxc["Ar_1S2"];
    } else if (level == "Ar_2P7") {
      int nc = 4; newDxc.nChannels = nc; 
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 5.18e-3; newDxc.final[0] = mapDxc["Ar_1S5"];
      newDxc.p[1] = 0.025;   newDxc.final[1] = mapDxc["Ar_1S4"];
      newDxc.p[2] = 2.43e-3; newDxc.final[2] = mapDxc["Ar_1S3"];
      newDxc.p[3] = 1.06e-3; newDxc.final[3] = mapDxc["Ar_1S2"];
    } else if (level == "Ar_2P6") {
      int nc = 3; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 0.0245;  newDxc.final[0] = mapDxc["Ar_1S5"];
      newDxc.p[1] = 4.9e-3;  newDxc.final[1] = mapDxc["Ar_1S4"];
      newDxc.p[2] = 5.03e-3; newDxc.final[2] = mapDxc["Ar_1S2"];
    } else if (level == "Ar_2P5") {
      int nc = 1; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 0.0402; newDxc.final[0] = mapDxc["Ar_1S4"];
    } else if (level == "Ar_2P4") {
      int nc = 4; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 6.25e-4; newDxc.final[0] = mapDxc["Ar_1S5"];
      newDxc.p[1] = 2.2e-5;  newDxc.final[1] = mapDxc["Ar_1S4"];
      newDxc.p[2] = 0.0186;  newDxc.final[2] = mapDxc["Ar_1S3"];
      newDxc.p[3] = 0.0139;  newDxc.final[3] = mapDxc["Ar_1S2"];
    } else if (level == "Ar_2P3") {
      int nc = 3; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 3.8e-3;  newDxc.final[0] = mapDxc["Ar_1S5"];
      newDxc.p[1] = 8.47e-3; newDxc.final[1] = mapDxc["Ar_1S4"];
      newDxc.p[2] = 0.0223;  newDxc.final[2] = mapDxc["Ar_1S3"];
    } else if (level == "Ar_2P2") {
      int nc = 4; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 6.39e-3; newDxc.final[0] = mapDxc["Ar_1S5"];
      newDxc.p[1] = 1.83e-3; newDxc.final[1] = mapDxc["Ar_1S4"];
      newDxc.p[2] = 0.0117;  newDxc.final[2] = mapDxc["Ar_1S3"];
      newDxc.p[3] = 0.0153;  newDxc.final[3] = mapDxc["Ar_1S2"];
    } else if (level == "Ar_2P1") {
      int nc = 2; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 2.36e-4; newDxc.final[0] = mapDxc["Ar_1S4"];
      newDxc.p[1] = 0.0445;  newDxc.final[1] = mapDxc["Ar_1S2"];
    } else if (level == "Ar_3D6") {
      int nc = 4; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      // Additional line (2P7) from Bartschat
      newDxc.p[0] = 8.1e-3;  newDxc.final[0] = mapDxc["Ar_2P10"];
      newDxc.p[1] = 7.73e-4; newDxc.final[1] = mapDxc["Ar_2P7"];
      newDxc.p[2] = 1.2e-4;  newDxc.final[2] = mapDxc["Ar_2P4"];
      newDxc.p[3] = 3.6e-4;  newDxc.final[3] = mapDxc["Ar_2P2"];
    } else if (level == "Ar_3D5") {
      // Oscillator strength from Berkowitz
      newDxc.osc = 0.0011;
      int nc = 10; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      // Additional lines (2P7, 2P6, 2P5, 2P1) from Bartschat
      newDxc.p[0] = 7.4e-3;  newDxc.final[0] = mapDxc["Ar_2P10"];
      newDxc.p[1] = 3.9e-5;  newDxc.final[1] = mapDxc["Ar_2P8"];
      newDxc.p[2] = 3.09e-4; newDxc.final[2] = mapDxc["Ar_2P7"];
      newDxc.p[3] = 1.37e-3; newDxc.final[3] = mapDxc["Ar_2P6"];
      newDxc.p[4] = 5.75e-4; newDxc.final[4] = mapDxc["Ar_2P5"];
      newDxc.p[5] = 3.2e-5;  newDxc.final[5] = mapDxc["Ar_2P4"];
      newDxc.p[6] = 1.4e-4;  newDxc.final[6] = mapDxc["Ar_2P3"];
      newDxc.p[7] = 1.7e-4;  newDxc.final[7] = mapDxc["Ar_2P2"];
      newDxc.p[8] = 2.49e-6; newDxc.final[8] = mapDxc["Ar_2P1"];
      // Transition probability to ground state calculated from osc. strength
      newDxc.p[9] = f2A * pow(newDxc.energy, 2) * newDxc.osc; 
      newDxc.final[9] = -1;
    } else if (level == "Ar_3D3") {
      int nc = 8; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      // Additional lines (2P9, 2P4) from Bartschat
      newDxc.p[0] = 4.9e-3;  newDxc.final[0] = mapDxc["Ar_2P10"];
      newDxc.p[1] = 9.82e-5; newDxc.final[1] = mapDxc["Ar_2P9"];
      newDxc.p[2] = 1.2e-4;  newDxc.final[2] = mapDxc["Ar_2P8"];
      newDxc.p[3] = 2.6e-4;  newDxc.final[3] = mapDxc["Ar_2P7"];
      newDxc.p[4] = 2.5e-3;  newDxc.final[4] = mapDxc["Ar_2P6"];
      newDxc.p[5] = 9.41e-5; newDxc.final[5] = mapDxc["Ar_2P4"];
      newDxc.p[6] = 3.9e-4;  newDxc.final[6] = mapDxc["Ar_2P3"];
      newDxc.p[7] = 1.1e-4;  newDxc.final[7] = mapDxc["Ar_2P2"];
    } else if (level == "Ar_3D4!") {
      int nc = 1; newDxc.nChannels = nc;
      // Transition probability for 2P9 transition from Bartschat
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 0.01593; newDxc.final[0] = mapDxc["Ar_2P9"];
    } else if (level == "Ar_3D4") {
      int nc = 4; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      // Additional lines (2P9, 2P3) from Bartschat
      newDxc.p[0] = 2.29e-3; newDxc.final[0] = mapDxc["Ar_2P9"];
      newDxc.p[1] = 0.011;   newDxc.final[1] = mapDxc["Ar_2P8"];
      newDxc.p[2] = 8.8e-5;  newDxc.final[2] = mapDxc["Ar_2P6"];
      newDxc.p[3] = 2.53e-6; newDxc.final[3] = mapDxc["Ar_2P3"];
    } else if (level == "Ar_3D1!!") {
      int nc = 8; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      // Additional lines (2P10, 2P6, 2P4 - 2P2) from Bartschat
      newDxc.p[0] = 5.85e-6; newDxc.final[0] = mapDxc["Ar_2P10"];
      newDxc.p[1] = 1.2e-4;  newDxc.final[1] = mapDxc["Ar_2P9"];
      newDxc.p[2] = 5.7e-3;  newDxc.final[2] = mapDxc["Ar_2P8"];
      newDxc.p[3] = 7.3e-3;  newDxc.final[3] = mapDxc["Ar_2P7"];
      newDxc.p[4] = 2.e-4;   newDxc.final[4] = mapDxc["Ar_2P6"];
      newDxc.p[5] = 1.54e-6; newDxc.final[5] = mapDxc["Ar_2P4"];
      newDxc.p[6] = 2.08e-5; newDxc.final[6] = mapDxc["Ar_2P3"];
      newDxc.p[7] = 6.75e-7; newDxc.final[7] = mapDxc["Ar_2P2"];
    } else if (level == "Ar_2S5") {
      int nc = 8; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 4.9e-3; newDxc.final[0] = mapDxc["Ar_2P10"];
      newDxc.p[1] = 0.011;  newDxc.final[1] = mapDxc["Ar_2P9"];
      newDxc.p[2] = 1.1e-3; newDxc.final[2] = mapDxc["Ar_2P8"];
      newDxc.p[3] = 4.6e-4; newDxc.final[3] = mapDxc["Ar_2P7"];
      newDxc.p[4] = 3.3e-3; newDxc.final[4] = mapDxc["Ar_2P6"];
      newDxc.p[5] = 5.9e-5; newDxc.final[5] = mapDxc["Ar_2P4"];
      newDxc.p[6] = 1.2e-4; newDxc.final[6] = mapDxc["Ar_2P3"];
      newDxc.p[7] = 3.1e-4; newDxc.final[7] = mapDxc["Ar_2P2"];
    } else if (level == "Ar_2S4") {
      // Oscillator strength from NIST database
      newDxc.osc = 0.027;
      // Berkowitz: f = 0.026;
      int nc = 10; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 0.077;   newDxc.final[0] = -1;
      newDxc.p[1] = 2.44e-3; newDxc.final[1] = mapDxc["Ar_2P10"];
      newDxc.p[2] = 8.9e-3;  newDxc.final[2] = mapDxc["Ar_2P8"];
      newDxc.p[3] = 4.6e-3;  newDxc.final[3] = mapDxc["Ar_2P7"];
      newDxc.p[4] = 2.7e-3;  newDxc.final[4] = mapDxc["Ar_2P6"];
      newDxc.p[5] = 1.3e-3;  newDxc.final[5] = mapDxc["Ar_2P5"];
      newDxc.p[6] = 4.5e-4;  newDxc.final[6] = mapDxc["Ar_2P4"];
      newDxc.p[7] = 2.9e-5;  newDxc.final[7] = mapDxc["Ar_2P3"];
      newDxc.p[8] = 3.e-5;   newDxc.final[8] = mapDxc["Ar_2P2"];
      newDxc.p[9] = 1.6e-4;  newDxc.final[9] = mapDxc["Ar_2P1"];
    } else if (level == "Ar_3D1!") {
      int nc = 4; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      // Additional line (2P6) from Bartschat
      newDxc.p[0] = 3.1e-3; newDxc.final[0] = mapDxc["Ar_2P9"];
      newDxc.p[1] = 2.e-3;  newDxc.final[1] = mapDxc["Ar_2P8"];
      newDxc.p[2] = 0.015;  newDxc.final[2] = mapDxc["Ar_2P6"];
      newDxc.p[3] = 9.8e-6; newDxc.final[3] = mapDxc["Ar_2P3"];
    } else if (level == "Ar_3D2") {
      // Oscillator strength from NIST database
      newDxc.osc = 0.0932;
      // Berkowitz: f = 0.09
      int nc = 10; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      // Additional lines (2P10, 2P6, 2P4-2P1) from Bartschat 
      newDxc.p[0] = 0.27;    newDxc.final[0] = -1;
      newDxc.p[1] = 1.35e-5; newDxc.final[1] = mapDxc["Ar_2P10"];
      newDxc.p[2] = 9.52e-4; newDxc.final[2] = mapDxc["Ar_2P8"];
      newDxc.p[3] = 0.011;   newDxc.final[3] = mapDxc["Ar_2P7"];
      newDxc.p[4] = 4.01e-5; newDxc.final[4] = mapDxc["Ar_2P6"];
      newDxc.p[5] = 4.3e-3;  newDxc.final[5] = mapDxc["Ar_2P5"];
      newDxc.p[6] = 8.96e-4; newDxc.final[6] = mapDxc["Ar_2P4"];
      newDxc.p[7] = 4.45e-5; newDxc.final[7] = mapDxc["Ar_2P3"];
      newDxc.p[8] = 5.87e-5; newDxc.final[8] = mapDxc["Ar_2P2"];
      newDxc.p[9] = 8.77e-4; newDxc.final[9] = mapDxc["Ar_2P1"];
    } else if (level == "Ar_3S1!!!!") {
      int nc = 8; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      // Additional lines (2P10, 2P9, 2P7, 2P6, 2P2) from Bartschat
      newDxc.p[0] = 7.51e-6; newDxc.final[0] = mapDxc["Ar_2P10"];
      newDxc.p[1] = 4.3e-5;  newDxc.final[1] = mapDxc["Ar_2P9"];
      newDxc.p[2] = 8.3e-4;  newDxc.final[2] = mapDxc["Ar_2P8"];
      newDxc.p[3] = 5.01e-5; newDxc.final[3] = mapDxc["Ar_2P7"];
      newDxc.p[4] = 2.09e-4; newDxc.final[4] = mapDxc["Ar_2P6"];
      newDxc.p[5] = 0.013;   newDxc.final[5] = mapDxc["Ar_2P4"];
      newDxc.p[6] = 2.2e-3;  newDxc.final[6] = mapDxc["Ar_2P3"];
      newDxc.p[7] = 3.35e-6; newDxc.final[7] = mapDxc["Ar_2P2"];
    } else if (level == "Ar_3S1!!") {
      int nc = 8; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      // Additional lines (2P10 - 2P8, 2P4, 2P3)
      newDxc.p[0] = 1.89e-4; newDxc.final[0] = mapDxc["Ar_2P10"];
      newDxc.p[1] = 1.52e-4; newDxc.final[1] = mapDxc["Ar_2P9"];
      newDxc.p[2] = 7.21e-4; newDxc.final[2] = mapDxc["Ar_2P8"];
      newDxc.p[3] = 3.69e-4; newDxc.final[3] = mapDxc["Ar_2P7"];
      newDxc.p[4] = 3.76e-3; newDxc.final[4] = mapDxc["Ar_2P6"];
      newDxc.p[5] = 1.72e-4; newDxc.final[5] = mapDxc["Ar_2P4"];
      newDxc.p[6] = 5.8e-4;  newDxc.final[6] = mapDxc["Ar_2P3"];
      newDxc.p[7] = 6.2e-3;  newDxc.final[7] = mapDxc["Ar_2P2"];
    } else if (level == "Ar_3S1!!!") {
      int nc = 4; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      // Additional lines (2P9, 2P8, 2P6) from Bartschat
      newDxc.p[0] = 7.36e-4; newDxc.final[0] = mapDxc["Ar_2P9"];
      newDxc.p[1] = 4.2e-5;  newDxc.final[1] = mapDxc["Ar_2P8"];
      newDxc.p[2] = 9.3e-5;  newDxc.final[2] = mapDxc["Ar_2P6"];
      newDxc.p[3] = 0.015;   newDxc.final[3] = mapDxc["Ar_2P3"];
    } else if (level == "Ar_2S3") {
      int nc = 4; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 3.26e-3; newDxc.final[0] = mapDxc["Ar_2P10"];
      newDxc.p[1] = 2.22e-3; newDxc.final[1] = mapDxc["Ar_2P7"];
      newDxc.p[2] = 0.01;    newDxc.final[2] = mapDxc["Ar_2P4"];
      newDxc.p[3] = 5.1e-3;  newDxc.final[3] = mapDxc["Ar_2P2"];
    } else if (level == "Ar_2S2") {
      // Oscillator strength from NIST database
      newDxc.osc = 0.0119;
      // Berkowitz: f = 0.012;
      int nc = 10; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 0.035;   newDxc.final[0] = -1;
      newDxc.p[1] = 1.76e-3; newDxc.final[1] = mapDxc["Ar_2P10"];
      newDxc.p[2] = 2.1e-4;  newDxc.final[2] = mapDxc["Ar_2P8"];
      newDxc.p[3] = 2.8e-4;  newDxc.final[3] = mapDxc["Ar_2P7"];
      newDxc.p[4] = 1.39e-3; newDxc.final[4] = mapDxc["Ar_2P6"];
      newDxc.p[5] = 3.8e-4;  newDxc.final[5] = mapDxc["Ar_2P5"];
      newDxc.p[6] = 2.0e-3;  newDxc.final[6] = mapDxc["Ar_2P4"];
      newDxc.p[7] = 8.9e-3;  newDxc.final[7] = mapDxc["Ar_2P3"];
      newDxc.p[8] = 3.4e-3;  newDxc.final[8] = mapDxc["Ar_2P2"];
      newDxc.p[9] = 1.9e-3;  newDxc.final[9] = mapDxc["Ar_2P1"];
    } else if (level == "Ar_3S1!") {
      // Oscillator strength from NIST database
      newDxc.osc = 0.106;
      // Berkowitz: f = 0.106
      int nc = 10; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      // Additional lines (2P10, 2P8, 2P7, 2P3) from Bartschat
      newDxc.p[0] = 0.313;   newDxc.final[0] = -1;
      newDxc.p[1] = 2.05e-5; newDxc.final[1] = mapDxc["Ar_2P10"];
      newDxc.p[2] = 8.33e-5; newDxc.final[2] = mapDxc["Ar_2P8"];
      newDxc.p[3] = 3.9e-4;  newDxc.final[3] = mapDxc["Ar_2P7"];
      newDxc.p[4] = 3.96e-4; newDxc.final[4] = mapDxc["Ar_2P6"];
      newDxc.p[5] = 4.2e-4;  newDxc.final[5] = mapDxc["Ar_2P5"];
      newDxc.p[6] = 4.5e-3;  newDxc.final[6] = mapDxc["Ar_2P4"];
      newDxc.p[7] = 4.84e-5; newDxc.final[7] = mapDxc["Ar_2P3"];
      newDxc.p[8] = 7.1e-3;  newDxc.final[8] = mapDxc["Ar_2P2"];
      newDxc.p[9] = 5.2e-3;  newDxc.final[9] = mapDxc["Ar_2P1"];
    } else if (level == "Ar_4D5") {
      // Oscillator strength from Berkowitz
      newDxc.osc = 0.0019;
      int nc = 7; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 2.78e-3; newDxc.final[0] = mapDxc["Ar_2P10"];
      newDxc.p[1] = 2.8e-4;  newDxc.final[1] = mapDxc["Ar_2P8"];
      newDxc.p[2] = 8.6e-4;  newDxc.final[2] = mapDxc["Ar_2P6"];
      newDxc.p[3] = 9.2e-4;  newDxc.final[3] = mapDxc["Ar_2P5"];
      newDxc.p[4] = 4.6e-4;  newDxc.final[4] = mapDxc["Ar_2P3"];
      newDxc.p[5] = 1.6e-4;  newDxc.final[5] = mapDxc["Ar_2P2"];
      // Transition probability to ground state calculated from osc. strength
      newDxc.p[6] = f2A * pow(newDxc.energy, 2) * newDxc.osc; 
      newDxc.final[6] = -1;
    } else if (level == "Ar_3S4") {
      // Oscillator strength from Berkowitz
      newDxc.osc = 0.0144;
      int nc = 10; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 4.21e-4; newDxc.final[0] = mapDxc["Ar_2P10"];
      newDxc.p[1] = 2.e-3;   newDxc.final[1] = mapDxc["Ar_2P8"];
      newDxc.p[2] = 1.7e-3;  newDxc.final[2] = mapDxc["Ar_2P7"];
      newDxc.p[3] = 7.2e-4;  newDxc.final[3] = mapDxc["Ar_2P6"];
      newDxc.p[4] = 3.5e-4;  newDxc.final[4] = mapDxc["Ar_2P5"];
      newDxc.p[5] = 1.2e-4;  newDxc.final[5] = mapDxc["Ar_2P4"];
      newDxc.p[6] = 4.2e-6;  newDxc.final[6] = mapDxc["Ar_2P3"];
      newDxc.p[7] = 3.3e-5;  newDxc.final[7] = mapDxc["Ar_2P2"];
      newDxc.p[8] = 9.7e-5;  newDxc.final[8] = mapDxc["Ar_2P1"];
      // Transition probability to ground state calculated from osc. strength
      newDxc.p[9] = f2A * pow(newDxc.energy, 2) * newDxc.osc; 
      newDxc.final[9] = -1;
    } else if (level == "Ar_4D2") {
      // Oscillator strength from Berkowitz
      newDxc.osc = 0.048;
      int nc = 2; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 1.7e-4; newDxc.final[0] = mapDxc["Ar_2P7"];
      // Transition probability to ground state calculated from osc. strength
      newDxc.p[1] = f2A * pow(newDxc.energy, 2) * newDxc.osc; 
      newDxc.final[1] = -1;
    } else if (level == "Ar_4S1!") {
      // Oscillator strength from Berkowitz
      newDxc.osc = 0.0209;
      int nc = 7; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 1.05e-3; newDxc.final[0] = mapDxc["Ar_2P10"];
      newDxc.p[1] = 3.1e-5;  newDxc.final[1] = mapDxc["Ar_2P8"];
      newDxc.p[2] = 2.5e-5;  newDxc.final[2] = mapDxc["Ar_2P7"];
      newDxc.p[3] = 4.0e-4;  newDxc.final[3] = mapDxc["Ar_2P6"];
      newDxc.p[4] = 5.8e-5;  newDxc.final[4] = mapDxc["Ar_2P5"];
      newDxc.p[5] = 1.2e-4;  newDxc.final[5] = mapDxc["Ar_2P3"];
      // Transition probability to ground state calculated from osc. strength
      newDxc.p[6] = f2A * pow(newDxc.energy, 2) * newDxc.osc; 
      newDxc.final[6] = -1;
    } else if (level == "Ar_3S2") {
      // Oscillator strength from Berkowitz
      newDxc.osc = 0.0221;
      int nc = 10; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 2.85e-4; newDxc.final[0] = mapDxc["Ar_2P10"];
      newDxc.p[1] = 5.1e-5;  newDxc.final[1] = mapDxc["Ar_2P8"];
      newDxc.p[2] = 5.3e-5;  newDxc.final[2] = mapDxc["Ar_2P7"];
      newDxc.p[3] = 1.6e-4;  newDxc.final[3] = mapDxc["Ar_2P6"];
      newDxc.p[4] = 1.5e-4;  newDxc.final[4] = mapDxc["Ar_2P5"];
      newDxc.p[5] = 6.0e-4;  newDxc.final[5] = mapDxc["Ar_2P4"];
      newDxc.p[6] = 2.48e-3; newDxc.final[6] = mapDxc["Ar_2P3"];
      newDxc.p[7] = 9.6e-4;  newDxc.final[7] = mapDxc["Ar_2P2"];
      newDxc.p[8] = 3.59e-4; newDxc.final[8] = mapDxc["Ar_2P1"];
      // Transition probability to ground state calculated from osc. strength
      newDxc.p[9] = f2A * pow(newDxc.energy, 2) * newDxc.osc; 
      newDxc.final[9] = -1;
    } else if (level == "Ar_5D5") {
      // Oscillator strength from Berkowitz
      newDxc.osc = 0.0041;
      int nc = 9; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 2.2e-3; newDxc.final[0] = mapDxc["Ar_2P10"];
      newDxc.p[1] = 1.1e-4; newDxc.final[1] = mapDxc["Ar_2P8"];
      newDxc.p[2] = 7.6e-5; newDxc.final[2] = mapDxc["Ar_2P7"];
      newDxc.p[3] = 4.2e-4; newDxc.final[3] = mapDxc["Ar_2P6"];
      newDxc.p[4] = 2.4e-4; newDxc.final[4] = mapDxc["Ar_2P5"];
      newDxc.p[5] = 2.1e-4; newDxc.final[5] = mapDxc["Ar_2P4"];
      newDxc.p[6] = 2.4e-4; newDxc.final[6] = mapDxc["Ar_2P3"];
      newDxc.p[7] = 1.2e-4; newDxc.final[7] = mapDxc["Ar_2P2"];
      // Transition probability to ground state calculated from osc. strength
      newDxc.p[8] = f2A * pow(newDxc.energy, 2) * newDxc.osc; 
      newDxc.final[8] = -1;
    } else if (level == "Ar_4S4") {
      // Oscillator strength from Berkowitz
      newDxc.osc = 0.0139;
      int nc = 7; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 1.9e-4; newDxc.final[0] = mapDxc["Ar_2P10"];
      newDxc.p[1] = 1.1e-3; newDxc.final[1] = mapDxc["Ar_2P8"];
      newDxc.p[2] = 5.2e-4; newDxc.final[2] = mapDxc["Ar_2P7"];
      newDxc.p[3] = 5.1e-4; newDxc.final[3] = mapDxc["Ar_2P6"];
      newDxc.p[4] = 9.4e-5; newDxc.final[4] = mapDxc["Ar_2P5"];
      newDxc.p[5] = 5.4e-5; newDxc.final[5] = mapDxc["Ar_2P4"];
      // Transition probability to ground state calculated from osc. strength
      newDxc.p[6] = f2A * pow(newDxc.energy, 2) * newDxc.osc; 
      newDxc.final[6] = -1;
    } else if (level == "Ar_5D2") {
      // Oscillator strength from Berkowitz
      newDxc.osc = 0.0426;
      int nc = 5; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 5.9e-5; newDxc.final[0] = mapDxc["Ar_2P8"];
      newDxc.p[1] = 9.0e-6; newDxc.final[1] = mapDxc["Ar_2P7"];
      newDxc.p[2] = 1.5e-4; newDxc.final[2] = mapDxc["Ar_2P5"];
      newDxc.p[3] = 3.1e-5; newDxc.final[3] = mapDxc["Ar_2P2"];
      // Transition probability to ground state calculated from osc. strength
      newDxc.p[4] = f2A * pow(newDxc.energy, 2) * newDxc.osc; 
      newDxc.final[4] = -1;
    } else if (level == "Ar_6D5") {
      // Oscillator strength from Lee and Lu 
      newDxc.osc = 0.00075;
      // Berkowitz estimates f = 0.0062 for the sum of
      // all "weak" nd levels with n = 6 and higher.
      int nc = 7; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 1.9e-3;  newDxc.final[0] = mapDxc["Ar_2P10"];
      newDxc.p[1] = 4.2e-4;  newDxc.final[1] = mapDxc["Ar_2P6"];
      newDxc.p[2] = 3.e-4;   newDxc.final[2] = mapDxc["Ar_2P5"];
      newDxc.p[3] = 5.1e-5;  newDxc.final[3] = mapDxc["Ar_2P4"];
      newDxc.p[4] = 6.6e-5;  newDxc.final[4] = mapDxc["Ar_2P3"];
      newDxc.p[5] = 1.21e-4; newDxc.final[5] = mapDxc["Ar_2P1"];
      // Transition probability to ground state calculated from osc. strength
      newDxc.p[6] = f2A * pow(newDxc.energy, 2) * newDxc.osc; 
      newDxc.final[6] = -1;
    } else if (level == "Ar_5S1!") {
      // Oscillator strength from Lee and Lu 
      newDxc.osc = 0.00051;
      // Berkowitz estimates f = 0.0562 for the sum 
      // of all nd' levels with n = 5 and higher.
      int nc = 2; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 7.7e-5; newDxc.final[0] = mapDxc["Ar_2P5"];
      // Transition probability to ground state calculated from osc. strength
      newDxc.p[1] = f2A * pow(newDxc.energy, 2) * newDxc.osc; 
      newDxc.final[1] = -1;
    } else if (level == "Ar_4S2") {
      // Oscillator strength from Lee and Lu 
      newDxc.osc = 0.00074;
      // Berkowitz estimates f = 0.0069 for the sum over all
      // ns' levels with n = 7 and higher.
      int nc = 8; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 4.5e-4; newDxc.final[0] = mapDxc["Ar_2P10"];
      newDxc.p[1] = 2.e-4;  newDxc.final[1] = mapDxc["Ar_2P8"];
      newDxc.p[2] = 2.1e-4; newDxc.final[2] = mapDxc["Ar_2P7"];
      newDxc.p[3] = 1.2e-4; newDxc.final[3] = mapDxc["Ar_2P5"];
      newDxc.p[4] = 1.8e-4; newDxc.final[4] = mapDxc["Ar_2P4"];
      newDxc.p[5] = 9.e-4;  newDxc.final[5] = mapDxc["Ar_2P3"];
      newDxc.p[6] = 3.3e-4; newDxc.final[6] = mapDxc["Ar_2P2"];
      // Transition probability to ground state calculated from osc. strength
      newDxc.p[7] = f2A * pow(newDxc.energy, 2) * newDxc.osc; 
      newDxc.final[7] = -1;
    } else if (level == "Ar_5S4") {
      // Oscillator strength from Lee and Lu 
      newDxc.osc = 0.0130;
      // Berkowitz estimates f = 0.0211 for the sum of all
      // ns levels with n = 8 and higher.
      newDxc.osc = 0.0211;
      int nc = 6; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      newDxc.p[0] = 3.6e-4; newDxc.final[0] = mapDxc["Ar_2P8"];
      newDxc.p[1] = 1.2e-4; newDxc.final[1] = mapDxc["Ar_2P6"];
      newDxc.p[2] = 1.5e-4; newDxc.final[2] = mapDxc["Ar_2P4"];
      newDxc.p[3] = 1.4e-4; newDxc.final[3] = mapDxc["Ar_2P3"];
      newDxc.p[4] = 7.5e-5; newDxc.final[4] = mapDxc["Ar_2P2"];
      // Transition probability to ground state calculated from osc. strength
      newDxc.p[5] = f2A * pow(newDxc.energy, 2) * newDxc.osc; 
      newDxc.final[5] = -1;
    } else if (level == "Ar_6D2") {
      // Oscillator strength from Lee and Lu 
      newDxc.osc = 0.0290;
      // Berkowitz estimates f = 0.0574 for the sum of all
      // "strong" nd levels with n = 6 and higher.
      newDxc.osc = 0.0574;
      int nc = 2; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, 0);
      // Additional line: 2P7
      newDxc.p[0] = 3.33e-3; newDxc.final[0] = mapDxc["Ar_2P7"];
      // Transition probability to ground state calculated from osc. strength
      newDxc.p[1] = f2A * pow(newDxc.energy, 2) * newDxc.osc; 
      newDxc.final[1] = -1;
    } else if (level == "Ar_Higher") {
      newDxc.osc = 0.;
      // This (artificial) level represents the sum of higher J = 1 states.
      // The deeexcitation cascade is simulated by allocating it 
      // with equal probability to one of the five nearest levels below.
      int nc = 5; newDxc.nChannels = nc;
      newDxc.p.resize(nc); newDxc.final.resize(nc); newDxc.type.resize(nc, -1);
      newDxc.p[0] = 100.; newDxc.final[0] = mapDxc["Ar_6D5"];
      newDxc.p[1] = 100.; newDxc.final[1] = mapDxc["Ar_5S1!"];
      newDxc.p[2] = 100.; newDxc.final[2] = mapDxc["Ar_4S2"];
      newDxc.p[3] = 100.; newDxc.final[3] = mapDxc["Ar_5S4"];
      newDxc.p[4] = 100.; newDxc.final[4] = mapDxc["Ar_6D2"];
    } else {
      std::cerr << className << "::ComputeDeexcitationTable:\n";
      std::cerr << "    Missing de-excitation data for level " 
                << level << ".\n";
      std::cerr << "    Program bug!\n";
      return;
    }
    deexcitations.push_back(newDxc);
  }
  
  if (debug) {
    std::cout << className << "::ComputeDeexcitationTable:\n"; 
    std::cout << "    Found " << nDeexcitations << " levels "
              << "with available radiative de-excitation data.\n";
  }

  // Collisional de-excitation channels
  
  if (withAr) {
    // Add the Ar dimer ground state.
    newDxc.label = "Ar_Dimer";
    newDxc.level = -1;
    newDxc.gas = iAr;
    newDxc.energy = 14.71;
    newDxc.osc = newDxc.cf = 0.;
    newDxc.sDoppler = newDxc.gPressure = newDxc.width = 0.;
    newDxc.p.clear(); newDxc.final.clear(); newDxc.type.clear();
    newDxc.nChannels = 0;
    mapDxc["Ar_Dimer"] = nDeexcitations;
    deexcitations.push_back(newDxc);
    ++nDeexcitations;
    // Add an Ar excimer level.
    newDxc.label = "Ar_Excimer";
    newDxc.level = -1;
    newDxc.gas = iAr;
    newDxc.energy = 14.71;
    newDxc.osc = newDxc.cf = 0.;
    newDxc.sDoppler = newDxc.gPressure = newDxc.width = 0.;
    newDxc.p.clear(); newDxc.final.clear(); newDxc.type.clear();
    newDxc.nChannels = 0;
    mapDxc["Ar_Excimer"] = nDeexcitations;
    deexcitations.push_back(newDxc);
    ++nDeexcitations;
    const bool useCollDxc = false;
    const double nAr = GetNumberDensity() * cAr;
    for (int j = nDeexcitations; j--;) {
      std::string level = deexcitations[j].label;
      if (level == "Ar_1S5" && useCollDxc) {
        // Two-body and three-body collision rates
        // K. Tachibana, Phys. Rev. A 34 (1986), 1007-1015
        // A. Bogaerts and R. Gijbels, Phys. Rev. A 52 (1995), 3743-3751
        const double fLoss2b = 2.3e-24 * nAr;
        const double fLoss3b = 1.4e-41 * pow(nAr, 2.);
        // Assume that three-body collisions lead to excimer formation.
        // Two-body collisions might lead to collisional mixing? 
        // For now: loss
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(mapDxc["Ar_Excimer"]);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].p.push_back(fLoss2b);
        deexcitations[j].p.push_back(fLoss3b);
        deexcitations[j].nChannels += 2;
      }
      if (level == "Ar_1S3" && useCollDxc) {
        // Two-body and three-body collision rates
        // K. Tachibana, Phys. Rev. A 34 (1986), 1007-1015
        // A. Bogaerts and R. Gijbels, Phys. Rev. A 52 (1995), 3743-3751
        const double fLoss2b = 4.3e-24 * nAr;
        const double fLoss3b = 1.5e-41 * pow(nAr, 2.);
        // Assume that three-body collisions lead to excimer formation.
        // Two-body collisions might lead to collisional mixing?
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(mapDxc["Ar_Excimer"]);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].p.push_back(fLoss2b);
        deexcitations[j].p.push_back(fLoss3b);
        deexcitations[j].nChannels += 2;
      }        
      if ((level == "Ar_4D5"  || level == "Ar_3S4" || level == "Ar_4D2" ||
           level == "Ar_4S1!" || level == "Ar_3S2" || level == "Ar_5D5" ||
           level == "Ar_4S4"  || level == "Ar_5D2" || level == "Ar_6D5" ||
           level == "Ar_5S1!" || level == "Ar_4S2" || level == "Ar_5S4" ||
           level == "Ar_6D2") && useCollDxc) {
        // Hornbeck-Molnar ionisation
        // P. Becker and F. Lampe, J. Chem. Phys. 42 (1965), 3857-3863
        // A. Bogaerts and R. Gijbels, Phys. Rev. A 52 (1995), 3743-3751
        // This value seems unrealistic, to be checked!
        const double fHM = 2.e-18 * nAr;
        deexcitations[j].final.push_back(mapDxc["Ar_Dimer"]);
        deexcitations[j].type.push_back(1);
        deexcitations[j].p.push_back(fHM);
        deexcitations[j].nChannels += 1;
      }
    }
  }

  // Collisional deexcitation by quenching gases.
  bool withCO2  = false; double cCO2  = 0.; int iCO2  = 0;
  bool withCH4  = false; double cCH4  = 0.; int iCH4  = 0;
  bool withC2H6 = false; double cC2H6 = 0.; int iC2H6 = 0;
  bool withC2H2 = false; double cC2H2 = 0.; int iC2H2 = 0;
  for (int i = nComponents; i--;) {
    if (gas[i] == "CO2") {
      withCO2 = true;
      cCO2 = fraction[i];
      iCO2 = i;
    } else if (gas[i] == "CH4") {
      withCH4 = true;
      cCH4 = fraction[i];
      iCH4 = i;
    } else if (gas[i] == "C2H6") {
      withC2H6 = true;
      cC2H6 = fraction[i];
      iC2H6 = i;
    } else if (gas[i] == "C2H2") {
      withC2H2 = true;
      cC2H2 = fraction[i];
      iC2H2 = i;
    }
  }

  if (withAr && withCO2) {
    // Partial density of CO2
    const double nQ = GetNumberDensity() * cCO2;
    for (int j = nDeexcitations; j--;) {
      std::string level = deexcitations[j].label;
      // Photoabsorption cross-section and ionisation yield
      double pacs = 0., eta = 0.;
      if (!optData.GetPhotoabsorptionCrossSection("CO2", 
                                                  deexcitations[j].energy,
                                                  pacs, eta)) {
        pacs = eta = 0.;
      }
      const double pPenning = pow(eta, 2. / 5.);
      if (level == "Ar_1S5") {
        // Rate constant from Velazco et al., J. Chem. Phys. 69 (1978)
        const double kQ = 5.3e-19;
        deexcitations[j].p.push_back(kQ * nQ);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 1;
      } else if (level == "Ar_1S4A") {
        // Rate constant from Velazco et al., J. Chem. Phys. 69 (1978)
        const double kQ = 5.0e-19;
        deexcitations[j].p.push_back(kQ * nQ);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 1;
      } else if (level == "Ar_1S3") {
        const double kQ = 5.9e-19;
        deexcitations[j].p.push_back(kQ * nQ);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 1;
      } else if (level == "Ar_1S2A") {
        const double kQ = 7.4e-19;
        deexcitations[j].p.push_back(kQ * nQ);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 1;
      } else if (level == "Ar_2P8") {
        // Rate constant from Sadeghi et al., J. Chem. Phys. 115 (2001)
        const double kQ = 6.4e-19;
        deexcitations[j].p.push_back(kQ * nQ);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 1;
      } else if (level == "Ar_2P6") {
        // Rate constant from Sadeghi et al.
        const double kQ = 6.1e-19;
        deexcitations[j].p.push_back(kQ * nQ);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 1;
      } else if (level == "Ar_2P5") {
        // Rate constant from Sadeghi et al.
        const double kQ = 6.6e-19;
        deexcitations[j].p.push_back(kQ * nQ);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 1;
      } else if (level == "Ar_2P1") {
        // Rate constant from Sadeghi et al.
        const double kQ = 6.2e-19;
        deexcitations[j].p.push_back(kQ * nQ);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 1;
      } else if (level == "Ar_2P10" || level == "Ar_2P9" ||
                 level == "Ar_2P7"  || level == "Ar_2P4" ||
                 level == "Ar_2P3"  || level == "Ar_2P2") {
        // Average of 4p rate constants from Sadeghi et al.
        const double kQ = 6.33e-19;
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 1;
        if (pPenning > 0.) {        
          deexcitations[j].p.push_back(kQ * nQ * pPenning);
          deexcitations[j].final.push_back(-1);
          deexcitations[j].type.push_back(1);
          deexcitations[j].nChannels += 1;
        } 
      } else if (deexcitations[j].osc > 0.) {
        // Higher resonance levels
        // Calculate rate constant from Watanabe-Katsuura formula
        const double m1 = ElectronMassGramme / (rgas[iAr]  - 1.);
        const double m2 = ElectronMassGramme / (rgas[iCO2] - 1.);
        // Compute the reduced mass.
        double mR = m1 * m2 / (m1 + m2);
        mR /= AtomicMassUnit;
        const double uA = (RydbergEnergy / deexcitations[j].energy) * 
                          deexcitations[j].osc;
        const double uQ = (2 * RydbergEnergy / deexcitations[j].energy) *
                          pacs / (4 * Pi2 * FineStructureConstant * 
                                  BohrRadius * BohrRadius);
        const double kQ = 2.591e-19 * 
                          pow(uA * uQ, 2. / 5.) * 
                          pow(temperature / mR, 3. / 10.);
        if (debug) {
          std::cout << className << "::ComputeDeexcitationTable:\n";
          std::cout << "    Rate constant for coll. deexcitation of\n"
                    << "    " << level << " by CO2 (W-K formula):\n"
                    << "      " << kQ << " cm3 ns-1\n";
        }
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_3D6"     || level == "Ar_3D3"     || 
                 level == "Ar_3D4!"    || level == "Ar_3D4"     ||
                 level == "Ar_3D1!!"   || level == "Ar_2S5"     || 
                 level == "Ar_3D1!"    || level == "Ar_3S1!!!!" || 
                 level == "Ar_3S1!!"   || level == "Ar_3S1!!!"  || 
                 level == "Ar_2S3") {
        // Non-resonant 3d and 5s levels
        // Collision radii
        const double rAr3d = 436.e-10;
        const double rCO2 = 165.e-10;
        // Hard sphere cross-section
        const double sigma = pow(rAr3d + rCO2, 2) * Pi;
        // Reduced mass
        const double m1 = ElectronMass / (rgas[iAr]  - 1.);
        const double m2 = ElectronMass / (rgas[iCO2] - 1.);
        const double mR = m1 * m2 / (m1 + m2);
        // Relative velocity
        const double vel = SpeedOfLight * 
                           sqrt(8. * BoltzmannConstant * temperature / 
                                (Pi * mR));
        const double kQ = sigma * vel;
        if (debug) {
          std::cout << className << "::ComputeDeexcitationTable:\n";
          std::cout << "    Rate constant for coll. deexcitation of\n"
                    << "    " << level << " by CO2 (hard sphere):\n"
                    << "      " << kQ << " cm3 ns-1\n";
        }
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      }
    }  
  } else if (withAr && withCH4) {
    // Partial density of methane
    const double nQ = GetNumberDensity() * cCH4;
    for (int j = nDeexcitations; j--;) {
      std::string level = deexcitations[j].label;
      // Photoabsorption cross-section and ionisation yield
      double pacs = 0., eta = 0.;
      if (!optData.GetPhotoabsorptionCrossSection("CH4", 
                                                  deexcitations[j].energy,
                                                  pacs, eta)) {
        pacs = eta = 0.;
      }
      const double pPenning = pow(eta, 2. / 5.);
      if (level == "Ar_1S5") {
        // Rate constant from Chen and Setser, J. Phys. Chem. 95 (1991)
        const double kQ = 4.55e-19;
        deexcitations[j].p.push_back(kQ * nQ);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 1;
      } else if (level == "Ar_1S4A") {
        // Rate constant from Velazco et al., J. Chem. Phys. 69 (1978)
        const double kQ = 4.5e-19;
        deexcitations[j].p.push_back(kQ * nQ);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 1;
      } else if (level == "Ar_1S3") {
        // Rate constant from Chen and Setser
        const double kQ = 5.30e-19;
        deexcitations[j].p.push_back(kQ * nQ);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 1;
      } else if (level == "Ar_1S2A") {
        // Rate constant from Velazco et al.
        const double kQ = 5.7e-19;
        deexcitations[j].p.push_back(kQ * nQ);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 1;
      } else if (level == "Ar_2P8") {
        // Rate constant from Sadeghi et al., J. Chem. Phys. 115 (2001)
        const double kQ = 7.4e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_2P6") {
        // Rate constant from Sadeghi et al.
        const double kQ = 3.4e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_2P5") {
        // Rate constant from Sadeghi et al.
        const double kQ = 6.0e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_2P1") {
        // Rate constant from Sadeghi et al.
        const double kQ = 9.3e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_2P10" || level == "Ar_2P9" ||
                 level == "Ar_2P7"  || level == "Ar_2P4" ||
                 level == "Ar_2P3"  || level == "Ar_2P2") {
        // Average of rate constants given by Sadeghi et al.
        const double kQ = 6.53e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (deexcitations[j].osc > 0.) {
        // Higher resonance levels
        // Calculate rate constant from Watanabe-Katsuura formula
        const double m1 = ElectronMassGramme / (rgas[iAr]  - 1.);
        const double m2 = ElectronMassGramme / (rgas[iCH4] - 1.);
        // Compute the reduced mass.
        double mR = m1 * m2 / (m1 + m2);
        mR /= AtomicMassUnit;
        const double uA = (RydbergEnergy / deexcitations[j].energy) * 
                          deexcitations[j].osc;
        const double uQ = (2 * RydbergEnergy / deexcitations[j].energy) *
                          pacs / (4 * Pi2 * FineStructureConstant * 
                                  BohrRadius * BohrRadius);
        const double kQ = 2.591e-19 * 
                          pow(uA * uQ, 2. / 5.) * 
                          pow(temperature / mR, 3. / 10.);  
        if (debug) {
          std::cout << className << "::ComputeDeexcitationTable:\n";
          std::cout << "    Rate constant for coll. deexcitation of\n"
                    << "    " << level << " by CH4 (W-K formula):\n"
                    << "      " << kQ << " cm3 ns-1\n";
        }
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_3D6"     || level == "Ar_3D3"     || 
                 level == "Ar_3D4!"    || level == "Ar_3D4"     ||
                 level == "Ar_3D1!!"   || level == "Ar_2S5"     || 
                 level == "Ar_3D1!"    || level == "Ar_3S1!!!!" || 
                 level == "Ar_3S1!!"   || level == "Ar_3S1!!!"  || 
                 level == "Ar_2S3") {
        // Non-resonant 3d and 5s levels
        // Collision radii
        const double rAr3d = 436.e-10;
        const double rCH4 = 190.e-10;
        // Hard sphere cross-section
        const double sigma = pow(rAr3d + rCH4, 2) * Pi;
        // Reduced mass
        const double m1 = ElectronMass / (rgas[iAr]  - 1.);
        const double m2 = ElectronMass / (rgas[iCH4] - 1.);
        const double mR = m1 * m2 / (m1 + m2);
        // Relative velocity
        const double vel = SpeedOfLight * 
                           sqrt(8. * BoltzmannConstant * temperature / 
                                (Pi * mR));
        const double kQ = sigma * vel;
        if (debug) {
          std::cout << className << "::ComputeDeexcitationTable:\n";
          std::cout << "    Rate constant for coll. deexcitation of\n"
                    << "    " << level << " by CH4 (hard sphere):\n"
                    << "      " << kQ << " cm3 ns-1\n";
        }
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      }
    }
  } else if (withAr && withC2H6) {
    // Partial density of ethane
    const double nQ = GetNumberDensity() * cC2H6;
    for (int j = nDeexcitations; j--;) {
      std::string level = deexcitations[j].label;
      // Photoabsorption cross-section and ionisation yield
      double pacs = 0., eta = 0.;
      if (!optData.GetPhotoabsorptionCrossSection("C2H6", 
                                                  deexcitations[j].energy,
                                                  pacs, eta)) {
        pacs = eta = 0.;
      }
      const double pPenning = pow(eta, 2. / 5.);
      if (level == "Ar_1S5") {
        // Rate constant from Chen and Setser, J. Phys. Chem. 95 (1991)
        const double kQ = 5.29e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_1S4A") {
        // Rate constant from Velazco et al., J. Chem. Phys. 69 (1978)
        const double kQ = 6.2e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_1S3") {
        // Rate constant from Chen and Setser
        const double kQ = 6.53e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_1S2A") {
        // Rate constant from Velazco et al.
        const double kQ = 10.7e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_2P8") {
        // Rate constant from Sadeghi et al., J. Chem. Phys. 115 (2001)
        const double kQ = 9.2e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_2P6") {
        // Rate constant from Sadeghi et al.
        const double kQ = 4.8e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_2P5") {
        // Rate constant from Sadeghi et al.
        const double kQ = 9.9e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_2P1") {
        // Rate constant from Sadeghi et al.
        const double kQ = 11.0e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_2P10" || level == "Ar_2P9" ||
                 level == "Ar_2P7"  || level == "Ar_2P4" ||
                 level == "Ar_2P3"  || level == "Ar_2P2") {
        // Average of rate constants given by Sadeghi et al.
        const double kQ = 8.7e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (deexcitations[j].osc > 0.) {
        // Higher resonance levels
        // Calculate rate constant from Watanabe-Katsuura formula
        const double m1 = ElectronMassGramme / (rgas[iAr]   - 1.);
        const double m2 = ElectronMassGramme / (rgas[iC2H6] - 1.);
        // Compute the reduced mass.
        double mR = m1 * m2 / (m1 + m2);
        mR /= AtomicMassUnit;
        const double uA = (RydbergEnergy / deexcitations[j].energy) * 
                          deexcitations[j].osc;
        const double uQ = (2 * RydbergEnergy / deexcitations[j].energy) *
                          pacs / (4 * Pi2 * FineStructureConstant * 
                                  BohrRadius * BohrRadius);
        const double kQ = 2.591e-19 * 
                          pow(uA * uQ, 2. / 5.) * 
                          pow(temperature / mR, 3. / 10.); 
        if (debug) {
          std::cout << className << "::ComputeDeexcitationTable:\n";
          std::cout << "    Rate constant for coll. deexcitation of\n"
                    << "    " << level << " by C2H6 (W-K formula):\n"
                    << "      " << kQ << " cm3 ns-1\n";
        }
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_3D6"     || level == "Ar_3D3"     || 
                 level == "Ar_3D4!"    || level == "Ar_3D4"     ||
                 level == "Ar_3D1!!"   || level == "Ar_2S5"     || 
                 level == "Ar_3D1!"    || level == "Ar_3S1!!!!" || 
                 level == "Ar_3S1!!"   || level == "Ar_3S1!!!"  || 
                 level == "Ar_2S3") {
        // Non-resonant 3d and 5s levels
        // Collision radii
        const double rAr3d = 436.e-10;
        const double rC2H6 = 195.e-10;
        // Hard sphere cross-section
        const double sigma = pow(rAr3d + rC2H6, 2) * Pi;
        // Reduced mass
        const double m1 = ElectronMass / (rgas[iAr]  - 1.);
        const double m2 = ElectronMass / (rgas[iC2H6] - 1.);
        const double mR = m1 * m2 / (m1 + m2);
        // Relative velocity
        const double vel = SpeedOfLight * 
                           sqrt(8. * BoltzmannConstant * temperature / 
                                (Pi * mR));
        const double kQ = sigma * vel;
        if (debug) {
          std::cout << className << "::ComputeDeexcitationTable:\n";
          std::cout << "    Rate constant for coll. deexcitation of\n"
                    << "    " << level << " by C2H6 (hard sphere):\n"
                    << "      " << kQ << " cm3 ns-1\n";
        }

        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      }
    }
  } else if (withAr && withC2H2) {
    // Partial density of acetylene
    const double nQ = GetNumberDensity() * cC2H2;
    for (int j = nDeexcitations; j--;) {
      std::string level = deexcitations[j].label;
      // Photoabsorption cross-section and ionisation yield
      double pacs = 0., eta = 0.;
      if (!optData.GetPhotoabsorptionCrossSection("C2H2", 
                                                  deexcitations[j].energy,
                                                  pacs, eta)) {
        pacs = eta = 0.;
      }
      const double pPenning = pow(eta, 2. / 5.);
      if (level == "Ar_1S5") {
        // Rate constant from Velazco et al., J. Chem. Phys. 69 (1978)
        const double kQ = 5.1e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_1S4A") {
        // Rate constant from Velazco et al.
        const double kQ = 4.6e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_1S3") {
        const double kQ = 5.1e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_1S2A") {
        // Rate constant from Velazco et al.
        const double kQ = 8.7e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_2P8") {
        // Rate constant from Sadeghi et al., J. Chem. Phys. 115 (2001)
        const double kQ = 5.0e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_2P6") {
        // Rate constant from Sadeghi et al.
        const double kQ = 5.7e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_2P5") {
        // Rate constant from Sadeghi et al.
        const double kQ = 6.0e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_2P1") {
        // Rate constant from Sadeghi et al.
        const double kQ = 5.3e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_2P10" || level == "Ar_2P9" ||
                 level == "Ar_2P7"  || level == "Ar_2P4" ||
                 level == "Ar_2P3"  || level == "Ar_2P2") {
        // Average of rate constants given by Sadeghi et al.
        const double kQ = 5.5e-19;
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (deexcitations[j].osc > 0.) {
        // Higher resonance levels
        // Compute rate constant according to Watanabe-Katsuura formula.
        const double m1 = ElectronMassGramme / (rgas[iAr]   - 1.);
        const double m2 = ElectronMassGramme / (rgas[iC2H2] - 1.);
        // Compute the reduced mass.
        double mR = m1 * m2 / (m1 + m2);
        mR /= AtomicMassUnit;
        const double uA = (RydbergEnergy / deexcitations[j].energy) * 
                          deexcitations[j].osc;
        const double uQ = (2 * RydbergEnergy / deexcitations[j].energy) *
                          pacs / (4 * Pi2 * FineStructureConstant * 
                                  BohrRadius * BohrRadius);
        const double kQ = 2.591e-19 * 
                          pow(uA * uQ, 2. / 5.) * 
                          pow(temperature / mR, 3. / 10.);  
        if (debug) {
          std::cout << className << "::ComputeDeexcitationTable:\n";
          std::cout << "    Rate constant for coll. deexcitation of\n"
                    << "    " << level << " by C2H2 (W-K formula):\n"
                    << "      " << kQ << " cm3 ns-1\n";
        }
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      } else if (level == "Ar_3D6"     || level == "Ar_3D3"     || 
                 level == "Ar_3D4!"    || level == "Ar_3D4"     ||
                 level == "Ar_3D1!!"   || level == "Ar_2S5"     || 
                 level == "Ar_3D1!"    || level == "Ar_3S1!!!!" || 
                 level == "Ar_3S1!!"   || level == "Ar_3S1!!!"  || 
                 level == "Ar_2S3") {
        // Non-resonant 3d and 5s levels
        // Collision radii
        const double rAr3d = 436.e-10;
        const double rC2H2 = 165.e-10;
        // Hard sphere cross-section
        const double sigma = pow(rAr3d + rC2H2, 2) * Pi;
        // Reduced mass
        const double m1 = ElectronMass / (rgas[iAr]  - 1.);
        const double m2 = ElectronMass / (rgas[iC2H2] - 1.);
        const double mR = m1 * m2 / (m1 + m2);
        // Relative velocity
        const double vel = SpeedOfLight * 
                           sqrt(8. * BoltzmannConstant * temperature / 
                                (Pi * mR));
        const double kQ = sigma * vel;
        if (debug) {
          std::cout << className << "::ComputeDeexcitationTable:\n";
          std::cout << "    Rate constant for coll. deexcitation of\n"
                    << "    " << level << " by C2H2 (hard sphere):\n"
                    << "      " << kQ << " cm3 ns-1\n";
        }
        deexcitations[j].p.push_back(kQ * nQ * pPenning);
        deexcitations[j].p.push_back(kQ * nQ * (1. - pPenning));
        deexcitations[j].final.push_back(-1);
        deexcitations[j].final.push_back(-1);
        deexcitations[j].type.push_back(1);
        deexcitations[j].type.push_back(-1);
        deexcitations[j].nChannels += 2;
      }
    }
  } else {
    std::cout << className << "::ComputeDeexcitationTable:\n";
    std::cout << "    No data on Penning effects found.\n";
  }

  if (debug) {
    std::cout << className << "::ComputeDeexcitationTable:\n";
    std::cout << "          Level    Energy [eV]   "
              << "                 Lifetimes [ns]\n";
    std::cout << "                                "
              << " Total    Radiative       "
              << " Collisional\n";
    std::cout << "                                     "
              << "                Ionisation      Other\n";
  }

  for (int i = 0; i < nDeexcitations; ++i) {
    // Calculate the total decay rate of each level.
    deexcitations[i].rate = 0.;
    double fRad = 0., fCollIon = 0., fCollOther = 0.;
    for (int j = deexcitations[i].nChannels; j--;) {
      deexcitations[i].rate += deexcitations[i].p[j];
      if (deexcitations[i].type[j] == 0) {
        fRad += deexcitations[i].p[j];
      } else if (deexcitations[i].type[j] == 1) {
        fCollIon += deexcitations[i].p[j];
      } else if (deexcitations[i].type[j] == -1) {
        fCollOther += deexcitations[i].p[j];
      }
    }
    if (deexcitations[i].rate > 0.) {
      // Print the radiative and collisional decay rates.
      if (debug) {
        std::cout << std::setw(15) << deexcitations[i].label << "  "
                  << std::fixed << std::setprecision(3)
                  << std::setw(10) << deexcitations[i].energy << "  " 
                  << std::setw(10) << 1. / deexcitations[i].rate << "  ";
        if (fRad > 0.) {
          std::cout << std::fixed << std::setprecision(3)
                    << std::setw(10) <<  1. / fRad << "  ";
        } else {
          std::cout << "----------  ";
        }
        if (fCollIon > 0.) {
          std::cout << std::fixed << std::setprecision(3)
                    << std::setw(10) << 1. / fCollIon << "  ";
        } else {
          std::cout << "----------  ";
        }
        if (fCollOther > 0.) {
          std::cout << std::fixed << std::setprecision(3)
                    << std::setw(10) << 1. / fCollOther << "\n";
        } else {
          std::cout << "----------  \n";
        }
      }
      // Normalize the decay rates.
      for (int j = 0; j < deexcitations[i].nChannels; ++j) {
        deexcitations[i].p[j] /= deexcitations[i].rate;
        if (j > 0) deexcitations[i].p[j] += deexcitations[i].p[j - 1];
      }
    }
  }


}

void
MediumMagboltz::ComputeDeexcitation(int iLevel, int& fLevel) {

  if (!useDeexcitation) {
    std::cerr << className << "::ComputeDeexcitation:\n";
    std::cerr << "    Deexcitation is disabled.\n";
    return;
  }
  
  // Make sure that the tables are updated.
  if (isChanged) {
    if (!Mixer()) {
      std::cerr << className << "::ComputeDeexcitation:\n";
      std::cerr << "    Error calculating the collision rates table.\n";
      return;
    }
    isChanged = false;
  }

  if (iLevel < 0 || iLevel >= nTerms) {
    std::cerr << className << "::ComputeDeexcitation:\n";
    std::cerr << "    Level index is out of range.\n";
    return;
  }

  iLevel = iDeexcitation[iLevel];
  if (iLevel < 0 || iLevel >= nDeexcitations) {
    std::cerr << className << "::ComputeDeexcitation:\n";
    std::cerr << "    Level is not deexcitable.\n";
    return;
  }

  ComputeDeexcitationInternal(iLevel, fLevel);
  if (fLevel >= 0 && fLevel < nDeexcitations) {
    fLevel = deexcitations[fLevel].level;
  }

}

void
MediumMagboltz::ComputeDeexcitationInternal(int iLevel, int& fLevel) {

  nDeexcitationProducts = 0;
  dxcProducts.clear();

  dxcProd newDxcProd;
  newDxcProd.t = 0.;

  fLevel = iLevel;
  while (iLevel >= 0 && iLevel < nDeexcitations) {
    if (deexcitations[iLevel].rate <= 0. || 
        deexcitations[iLevel].nChannels <= 0) {
      // This level is a dead end.
      fLevel = iLevel;
      return;
    }
    // Determine the de-excitation time.
    newDxcProd.t += -log(RndmUniformPos()) / deexcitations[iLevel].rate;
    // Select the transition.
    fLevel = -1;
    int type = 0;
    const double r = RndmUniform();
    for (int j = 0; j < deexcitations[iLevel].nChannels; ++j) {
      if (r <= deexcitations[iLevel].p[j]) {
        fLevel = deexcitations[iLevel].final[j];
        type = deexcitations[iLevel].type[j];
        break;
      }
    }
    if (type == 0) {
      // Radiative decay
      newDxcProd.type = DxcProdTypePhoton;
      newDxcProd.energy = deexcitations[iLevel].energy;
      if (fLevel >= 0) {
        // Decay to a lower lying excited state.
        newDxcProd.energy -= deexcitations[fLevel].energy;
        if (newDxcProd.energy < Small) newDxcProd.energy = Small;
        dxcProducts.push_back(newDxcProd);
        ++nDeexcitationProducts;
      } else {
        // Decay to ground state.
        double delta = RndmVoigt(0., 
                                 deexcitations[iLevel].sDoppler,
                                 deexcitations[iLevel].gPressure);
        while (newDxcProd.energy + delta < Small || 
               fabs(delta) >= deexcitations[iLevel].width) {
          delta = RndmVoigt(0., deexcitations[iLevel].sDoppler,
                                deexcitations[iLevel].gPressure);
        }
        newDxcProd.energy += delta;
        dxcProducts.push_back(newDxcProd);
        ++nDeexcitationProducts;
        // Deexcitation cascade is over.
        fLevel = iLevel;
        return;
      }
    } else if (type == 1) {
      // Ionisation electron
      newDxcProd.type = DxcProdTypeElectron;
      newDxcProd.energy = deexcitations[iLevel].energy;
      if (fLevel >= 0) {
        // Associative ionisation
        newDxcProd.energy -= deexcitations[fLevel].energy;
        if (newDxcProd.energy < Small) newDxcProd.energy = Small;
        ++nPenning;
        dxcProducts.push_back(newDxcProd);
        ++nDeexcitationProducts;
      } else {
        // Penning ionisation
        newDxcProd.energy -= minIonPot;
        if (newDxcProd.energy < Small) newDxcProd.energy = Small;
        ++nPenning;
        dxcProducts.push_back(newDxcProd);
        ++nDeexcitationProducts;
        // Deexcitation cascade is over.
        fLevel = iLevel;
        return; 
      }
    }
    // Proceed with the next level in the cascade. 
    iLevel = fLevel;
  }

}

bool
MediumMagboltz::ComputePhotonCollisionTable() {

  OpticalData data;
  double cs;
  double eta;

  // Atomic density
  const double dens = GetNumberDensity();
  
  // Reset the collision rate arrays.
  cfTotGamma.clear(); cfTotGamma.resize(nEnergyStepsGamma, 0.);
  cfGamma.clear(); cfGamma.resize(nEnergyStepsGamma);
  for (int j = nEnergyStepsGamma; j--;) cfGamma[j].clear();
  csTypeGamma.clear();

  nPhotonTerms = 0;
  for (int i = 0; i < nComponents; ++i) {
    const double prefactor = dens * SpeedOfLight * fraction[i];
    // Check if optical data for this gas is available.
    if (!data.IsAvailable(gas[i])) return false;
    csTypeGamma.push_back(i * nCsTypesGamma + PhotonCollisionTypeIonisation);
    csTypeGamma.push_back(i * nCsTypesGamma + PhotonCollisionTypeInelastic);
    nPhotonTerms += 2;
    for (int j = 0; j < nEnergyStepsGamma; ++j) {
      // Retrieve total photoabsorption cross-section and ionisation yield.
      data.GetPhotoabsorptionCrossSection(gas[i], j * eStepGamma, 
                                          cs, eta);
      cfTotGamma[j] += cs * prefactor;
      // Ionisation
      cfGamma[j].push_back(cs * prefactor * eta);
      // Inelastic absorption
      cfGamma[j].push_back(cs * prefactor * (1. - eta));
    }
  }

  // If requested, write the cross-sections to file.
  if (useCsOutput) {
    std::ofstream csfile;
    csfile.open("csgamma.txt", std::ios::out);
    for (int j = 0; j < nEnergyStepsGamma; ++j) {
      csfile << j * eStepGamma << "  ";
      for (int i = 0; i < nPhotonTerms; ++i) csfile << cfGamma[j][i] << "  ";
      csfile << "\n";
    }
    csfile.close();
  }

  // Calculate the cumulative rates.
  for (int j = 0; j < nEnergyStepsGamma; ++j) {
    for (int i = 0; i < nPhotonTerms; ++i) {
      if (i > 0) cfGamma[j][i] += cfGamma[j][i - 1];
    }
  }

  if (debug) {
    std::cout << className << "::ComputePhotonCollisionTable:\n";
    std::cout << "    Energy [eV]      Mean free path [um]\n";
    for (int i = 0; i < 10; ++i) { 
      const double imfp = cfTotGamma[(2 * i + 1) * nEnergyStepsGamma / 20] /
                          SpeedOfLight;
      std::cout << "    " << std::fixed << std::setw(10) 
                << std::setprecision(2) << (2 * i + 1) * eFinalGamma / 20
                << "    " << std::setw(18) << std::setprecision(4);
      if (imfp > 0.) {
        std::cout << 1.e4 / imfp << "\n";
      } else {
        std::cout << "------------\n";
      }
    }
    std::cout << std::resetiosflags(std::ios_base::floatfield);
  }

  if (!useDeexcitation) return true;

  // Conversion factor from oscillator strength to cross-section
  const double f2cs = FineStructureConstant * 2 * Pi2 * HbarC * HbarC / 
                      ElectronMass;
  // Discrete absorption lines 
  int nResonanceLines = 0;
  for (int i = 0; i < nDeexcitations; ++i) {
    if (deexcitations[i].osc < Small) continue;
    const double prefactor = dens * SpeedOfLight * 
                             fraction[deexcitations[i].gas];
    deexcitations[i].cf = prefactor *  f2cs * deexcitations[i].osc;
    // Compute the line width due to Doppler broadening.
    const double mgas = ElectronMass / (rgas[deexcitations[i].gas] - 1.);
    const double wDoppler = sqrt(BoltzmannConstant * temperature / mgas);
    deexcitations[i].sDoppler = wDoppler * deexcitations[i].energy;
    // Compute the half width at half maximum due to resonance broadening.
    //   A. W. Ali and H. R. Griem, Phys. Rev. 140, 1044
    //   A. W. Ali and H. R. Griem, Phys. Rev. 144, 366
    const double kResBroad = 1.92 * Pi * sqrt(1. / 3.);
    deexcitations[i].gPressure = kResBroad *
                                 FineStructureConstant * pow(HbarC, 3) * 
                                 deexcitations[i].osc * dens * 
                                 fraction[deexcitations[i].gas] / 
                                 (ElectronMass * deexcitations[i].energy);
    // Make an estimate for the width within which a photon can be 
    // absorbed by the line
    const int nWidths = 100;
    // Calculate the FWHM of the Voigt distribution according to the  
    // approximation formula given in 
    // Olivero and Longbothum, J. Quant. Spectr. Rad. Trans. 17, 233-236
    const double fwhmGauss = deexcitations[i].sDoppler * sqrt(2. * log(2.));
    const double fwhmLorentz = deexcitations[i].gPressure;
    const double fwhmVoigt = 0.5 * (1.0692 * fwhmLorentz + 
                                    sqrt(0.86639 * fwhmLorentz * fwhmLorentz +
                                         4 * fwhmGauss * fwhmGauss));
    deexcitations[i].width = nWidths * fwhmVoigt;
    ++nResonanceLines;
  }

  if (nResonanceLines <= 0) {
    std::cerr << className << "::ComputePhotonCollisionTable:\n";
    std::cerr << "    No resonance lines found.\n";
    return true;
  }

  if (debug) {
    std::cout << className << "::ComputePhotonCollisionTable:\n";
    std::cout << "    Discrete absorption lines:\n";
    std::cout << "      Energy [eV]        Line width (FWHM) [eV]  "
              << "    Mean free path [um]\n";
    std::cout << "                            Doppler    Pressure   "
              << "   (peak)     \n";
    for (int i = 0; i < nDeexcitations; ++i) {
      if (deexcitations[i].osc < Small) continue;
      const double imfpP = (deexcitations[i].cf / SpeedOfLight) * 
                           TMath::Voigt(0., 
                                        deexcitations[i].sDoppler,
                                        2 * deexcitations[i].gPressure);
      std::cout << "      " << std::fixed << std::setw(6) 
                << std::setprecision(3) 
                << deexcitations[i].energy << " +/- " 
                << std::scientific << std::setprecision(1)
                << deexcitations[i].width << "   "
                << std::setprecision(2)  
                << 2 * sqrt(2 * log(2.)) * deexcitations[i].sDoppler 
                << "   " << std::scientific << std::setprecision(3) 
                << 2 * deexcitations[i].gPressure << "  "
                << std::fixed << std::setw(10) << std::setprecision(4);
      if (imfpP > 0.) {
        std::cout << 1.e4 / imfpP;
      } else {
        std::cout << "----------";
      }
      std::cout << "\n";
    }
  }

  return true;

}

void
MediumMagboltz::RunMagboltz(const double e, 
                              const double bmag, const double btheta,
                              const int ncoll, bool verbose,
                              double& vx, double& vy, double& vz,
                              double& dl, double& dt,
                              double& alpha, double& eta,
                              double& vxerr, double& vyerr, double& vzerr,
                              double& dlerr, double& dterr, 
                              double& alphaerr, double& etaerr,
                              double& alphatof) {

  // Initialize the values.
  vx = vy = vz = 0.;
  dl = dt = 0.;
  alpha = eta = alphatof = 0.;
  vxerr = vyerr = vzerr = 0.;
  dlerr = dterr = 0.;
  alphaerr = etaerr = 0.;

  // Set input parameters in Magboltz common blocks.
  inpt_.nGas = nComponents;
  inpt_.nStep = 4000;
  inpt_.nAniso = 2;

  inpt_.tempc = temperature - ZeroCelsius;
  inpt_.torr = pressure;
  inpt_.ipen = 0;
  setp_.nmax = ncoll;

  setp_.efield = e;
  // Convert from Tesla to kGauss.
  bfld_.bmag = bmag * 10.;
  // Convert from radians to degree.
  bfld_.btheta = btheta * 180. / Pi;
  
  // Set the gas composition in Magboltz.
  for (int i = 0; i < nComponents; ++i) {
    int ng = 0;
    if (!GetGasNumberMagboltz(gas[i], ng)) {
      std::cerr << className << "::RunMagboltz:\n";
      std::cerr << "    Gas " << gas[i] << " has no corresponding"
                << " gas number in Magboltz.\n";
      return;
    }
    gasn_.ngasn[i] = ng;
    ratio_.frac[i] = 100 * fraction[i];
  }

  // Call Magboltz internal setup routine.
  setup1_();

  // Calculate the max. energy in the table.
  if (e * temperature / (293.15 * pressure) > 15) {
    // If E/p > 15 start with 8 eV.
    inpt_.efinal = 8.;
  } else {
    inpt_.efinal = 0.5;
  }
  setp_.estart = inpt_.efinal / 50.;

  long long ielow = 1;
  while (ielow == 1) {
    mixer_();
    if (bmag == 0. || btheta == 0. || fabs(btheta) == Pi) {
      elimit_(&ielow);
    } else if (btheta == HalfPi) {
      elimitb_(&ielow);
    } else {
      elimitc_(&ielow);
    }
    if (ielow == 1) {
      // Increase the max. energy.
      inpt_.efinal *= sqrt(2.);
      setp_.estart = inpt_.efinal / 50.;
    }
  }

  if (verbose) prnter_();
  
  // Run the Monte Carlo calculation.
  if (bmag == 0.) {
    monte_();
  } else if (btheta == 0. || btheta == Pi) {
    montea_();
  } else if (btheta == HalfPi) {
    monteb_();
  } else {
    montec_();
  }
  if (verbose) output_();

  // If attachment or ionisation rate is greater than sstmin,
  // include spatial gradients in the solution.
  const double sstmin = 30.;
  double alpp = ctowns_.alpha * 760. * temperature / (pressure * 293.15);
  double attp = ctowns_.att   * 760. * temperature / (pressure * 293.15);
  bool useSST = false;
  if (fabs(alpp - attp) > sstmin || alpp > sstmin || attp > sstmin) {
    useSST = true;
    if (bmag == 0.) {
      alpcalc_();
    } else if (btheta == 0. || btheta == Pi) {
      alpclca_();
    } else if (btheta == HalfPi) {
      alpclcb_();
    } else {
      alpclcc_();
    }
    // Calculate the (effective) TOF Townsend coefficient.
    double alphapt = tofout_.ralpha;
    double etapt   = tofout_.rattof;
    double fc1 = 1.e5 * tofout_.tofwr / (2. * tofout_.tofdl);
    double fc2 = 1.e12 * (alphapt - etapt) / tofout_.tofdl;
    alphatof = fc1 - sqrt(fc1 * fc1 - fc2);
  }
  if (verbose) output2_();

  // Convert to cm / ns.
  vx = vel_.wx * 1.e-9; vxerr = velerr_.dwx;
  vy = vel_.wy * 1.e-9; vyerr = velerr_.dwy;
  vz = vel_.wz * 1.e-9; vzerr = velerr_.dwz;

  dt = sqrt(0.2 * difvel_.diftr / vz) * 1.e-4; dterr = diferl_.dfter;
  dl = sqrt(0.2 * difvel_.difln / vz) * 1.e-4; dlerr = diferl_.dfler;
 
  alpha = ctowns_.alpha; alphaerr = ctwner_.alper;
  eta   = ctowns_.att;   etaerr = ctwner_.atter;
 
  // Print the results.
  if (verbose || debug) {
    std::cout << className << "::RunMagboltz:\n";
    std::cout << "    Results: \n";
    std::cout << "      Drift velocity along E:           " 
              << std::right << std::setw(10) << std::setprecision(6) 
              << vz << " cm/ns +/- "
              << std::setprecision(2) << vzerr << "%\n";
    std::cout << "      Drift velocity along Bt:          " 
              << std::right << std::setw(10) << std::setprecision(6)
              << vx << " cm/ns +/- "
              << std::setprecision(2) << vxerr << "%\n";
    std::cout << "      Drift velocity along ExB:         " 
              << std::right << std::setw(10) << std::setprecision(6)
              << vy << " cm/ns +/- "
              << std::setprecision(2) << vyerr << "%\n";
    std::cout << "      Longitudinal diffusion:           " 
              << std::right << std::setw(10) << std::setprecision(6)
              << dl << " cm1/2 +/- "
              << std::setprecision(2) << dlerr << "%\n";
    std::cout << "      Transverse diffusion:             " 
              << std::right << std::setw(10) << std::setprecision(6)
              << dt << " cm1/2 +/- "
              << std::setprecision(2) << dterr << "%\n";
    if (useSST) {
      std::cout << "      Townsend coefficient (SST):       " 
                << std::right << std::setw(10) << std::setprecision(6) 
                << alpha << " cm-1  +/- "
                << std::setprecision(2) << alphaerr << "%\n";
      std::cout << "      Attachment coefficient (SST):     " 
                << std::right << std::setw(10) << std::setprecision(6)
                << eta << " cm-1  +/- "
                << std::setprecision(2) << etaerr << "%\n";
      std::cout << "      Eff. Townsend coefficient (TOF):  " 
                << std::right << std::setw(10) << std::setprecision(6)
                << alphatof << " cm-1\n";
    } else {
      std::cout << "      Townsend coefficient:             " 
                << std::right << std::setw(10) << std::setprecision(6)
                << alpha << " cm-1  +/- "
                << std::setprecision(2) << alphaerr << "%\n";
      std::cout << "      Attachment coefficient:           "
                << std::right << std::setw(10) << std::setprecision(6)
                << eta << " cm-1  +/- "
                << std::setprecision(2) << etaerr << "%\n";
    }
  }

}

void 
MediumMagboltz::GenerateGasTable(const int numCollisions) {

  // Set the reference pressure and temperature.
  pressureTable = pressure;
  temperatureTable = temperature;

  // Initialize the parameter arrays.
  InitParamArrays(nEfields, nBfields, nAngles, tabElectronVelocityE,   0.);
  InitParamArrays(nEfields, nBfields, nAngles, tabElectronVelocityB,   0.);
  InitParamArrays(nEfields, nBfields, nAngles, tabElectronVelocityExB, 0.);
  InitParamArrays(nEfields, nBfields, nAngles, tabElectronDiffLong,    0.);
  InitParamArrays(nEfields, nBfields, nAngles, tabElectronDiffTrans,   0.);
  InitParamArrays(nEfields, nBfields, nAngles, tabElectronTownsend,   -30.);
  InitParamArrays(nEfields, nBfields, nAngles, tabTownsendNoPenning,  -30.);
  InitParamArrays(nEfields, nBfields, nAngles, tabElectronAttachment, -30.);

  hasElectronVelocityE   = true;
  hasElectronVelocityB   = true;
  hasElectronVelocityExB = true;
  hasElectronDiffLong    = true;
  hasElectronDiffTrans   = true;
  hasElectronTownsend    = true;
  hasElectronAttachment  = true;

  hasExcRates = false;
  tabExcRates.clear();
  excitationList.clear();
  nExcListElements = 0;
  hasIonRates = false;
  tabIonRates.clear();
  ionisationList.clear();
  nIonListElements = 0;

  hasIonMobility = false;
  hasIonDissociation = false;
  hasIonDiffLong = false;
  hasIonDiffTrans = false; 
  
  // gasBits = "TFTTFTFTTTFFFFFF";
  // The version number is 11 because there are slight
  // differences between the way these gas files are written
  // and the ones from Garfield. This is mainly in the way
  // the gas tables are stored.
  // versionNumber = 11;

  double vx = 0., vy = 0., vz = 0.;
  double difl = 0., dift = 0.;
  double alpha = 0., eta = 0.;
  double vxerr = 0., vyerr = 0., vzerr = 0.;
  double diflerr = 0., difterr = 0.;
  double alphaerr = 0., etaerr = 0.;
  double alphatof = 0.;

  bool verbose = false;
  if (debug) verbose = true;
  // Run through the grid of E- and B-fields and angles.
  for (int i = 0; i < nEfields; ++i) {
    for (int j = 0; j < nAngles; ++j) {
      for (int k = 0; k < nBfields; ++k) {
        if (debug) {
          std::cout << className << "::GenerateGasTable:\n";
          std::cout << "    E = " << eFields[i] 
                    << " V/cm, B = " << bFields[j] 
                    << " T, angle: " << bAngles[k] << " rad\n";
        }
        RunMagboltz(eFields[i], bFields[j], bAngles[k],
                    numCollisions, verbose,
                    vx, vy, vz,
                    difl, dift,
                    alpha, eta,
                    vxerr, vyerr, vzerr, 
                    diflerr, difterr, 
                    alphaerr, etaerr, alphatof);
        tabElectronVelocityE[j][k][i]   = vz;
        tabElectronVelocityExB[j][k][i] = vy;
        tabElectronVelocityB[j][k][i]   = vx;
        tabElectronDiffLong[j][k][i]    = difl;
        tabElectronDiffTrans[j][k][i]   = dift;
        if (alpha > 0.) {
          tabElectronTownsend[j][k][i]  = log(alpha);
          tabTownsendNoPenning[j][k][i] = log(alpha);
        } else {
          tabElectronTownsend[j][k][i]  = -30.;
          tabTownsendNoPenning[j][k][i] = -30.;
        }
        if (eta > 0.) {
          tabElectronAttachment[j][k][i] = log(eta);
        } else {
          tabElectronAttachment[j][k][i] = -30.;
        }
      }
    }
  }

}

}
