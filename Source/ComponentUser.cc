#include <iostream>

#include "ComponentUser.hh"

namespace Garfield {

ComponentUser::ComponentUser() :
  ComponentBase(), 
  hasField(false), field(0), 
  hasPotential(false), potential(0),
  nWeightingFields(0)  {
  
  wfields.clear();
  className = "ComponentUser";

}

void 
ComponentUser::ElectricField(const double x, const double y, const double z,
                             double& ex, double& ey, double& ez, 
                             Medium*& m, int& status) {

  if (!hasField) {
    ex = ey = ez = 0.;
    m = 0;
    status = -10;
    return;
  }
  
  field(x, y, z, ex, ey, ez);  
  if (!GetMedium(x, y, z, m)) {
    if (debug) {
      std::cerr << className << "::ElectricField:\n";
      std::cerr << "    (" << x << ", " << y << ", " << z << ")" 
                << " is not inside a medium.\n";
    }
    status = -6;
    m = 0;
    return;
  }
  
  if (m->IsDriftable()) {
    status = 0;
  } else {
    status = -5;
  }

}

void 
ComponentUser::ElectricField(const double x, const double y, const double z, 
                             double& ex, double& ey, double& ez, double& v, 
                             Medium*& m, int& status) {

  if (!hasField) {
    ex = ey = ez = v = 0.;  
    m = 0;
    status = -10;
    return;
  }
  field(x, y, z, ex, ey, ez);
  
  if (hasPotential) {
    potential(x, y, z, v);  
  } else {
    v = 0.;
  }  
    
  if (!GetMedium(x, y, z, m)) {
    if (debug) {
      std::cerr << className << "::ElectricField:\n";
      std::cerr << "    (" << x << ", " << y << ", " << z << ")" 
                << " is not inside a medium.\n";
    }
    status = -6;
    m = 0;
    return;
  }
  
  if (m->IsDriftable()) {
    status = 0;
  } else {
    status = -5;
  }

}

bool 
ComponentUser::GetVoltageRange(double& vmin, double& vmax) {

  vmin = vmax = 0.;
  return false;

}

void 
ComponentUser::WeightingField(const double x, const double y, const double z,
                              double& wx, double& wy, double& wz,
                              const std::string label) {

  wx = wy = wz = 0.;
  double fx = 0., fy = 0., fz = 0.;
  for (int i = nWeightingFields; i--;) {
    if (label == wfields[i].label) {
      wfields[i].field(x, y, z, fx, fy, fz);
      wx += fx; wy += fy; wz += fz;
    }
  }

}

void 
ComponentUser::SetElectricField(
    void (*f)(const double, const double, const double, double&, double&, double&)) {

  if (f == 0) {
    std::cerr << className << "::SetElectricField:\n";
    std::cerr << "    Function pointer is null.\n";
    return;
  }
  field = f;
  hasField = true;
  ready = true;
  
}

void 
ComponentUser::SetPotential(
    void (*f)(const double, const double, const double, double&)) {

  if (f == 0) {
    std::cerr << className << "::SetPotential:\n";
    std::cerr << "    Function pointer is null.\n";
    return;
  }
  potential = f;
  hasPotential = true;

}

void 
ComponentUser::AddWeightingField(void (*f)(const double, const double, const double, double&, double&, double&),
                                 const std::string label) {
                                 
  if (f == 0) {
    std::cerr << className << "::AddWeightingField:\n";
    std::cerr << "    Function pointer is null.\n";
    return;
  }
  wfield newField;
  newField.field = f;
  newField.label = label;
  wfields.push_back(newField);
  ++nWeightingFields;

}

void
ComponentUser::Reset() {

  field = 0; 
  potential = 0;
  hasField = false;
  hasPotential = false;
  ready = false;
  
}

void
ComponentUser::UpdatePeriodicity() {

  if (debug) {
    std::cerr << className << "::UpdatePeriodicity:\n";
    std::cerr << "    Periodicities are not supported.\n";
  }

}

}
