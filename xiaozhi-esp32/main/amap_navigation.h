#ifndef AMAP_NAVIGATION_H
#define AMAP_NAVIGATION_H

#include <string>

namespace AmapNavigation {

std::string PlanRoute(const std::string& origin,
                      const std::string& destination,
                      const std::string& mode,
                      const std::string& city);

std::string GetCurrentLocation();

} // namespace AmapNavigation

#endif // AMAP_NAVIGATION_H
