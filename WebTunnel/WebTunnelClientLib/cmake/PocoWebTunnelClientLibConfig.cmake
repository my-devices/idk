include(CMakeFindDependencyMacro)
find_dependency(PocoFoundation)
find_dependency(PocoNet)
find_dependency(PocoWebTunnel)
include("${CMAKE_CURRENT_LIST_DIR}/WebTunnelClientLibTargets.cmake")