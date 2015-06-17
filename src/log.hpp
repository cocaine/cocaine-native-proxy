#pragma once

#include <swarm/logger.hpp>

#define CP_LOG(verbosity, ...) BH_LOG(logger(), verbosity, __VA_ARGS__)

#define CP_DEBUG(...)  CP_LOG(SWARM_LOG_DEBUG,   __VA_ARGS__)
#define CP_NOTICE(...) CP_LOG(SWARM_LOG_NOTICE,  __VA_ARGS__)
#define CP_INFO(...)   CP_LOG(SWARM_LOG_INFO,    __VA_ARGS__)
#define CP_WARN(...)   CP_LOG(SWARM_LOG_WARNING, __VA_ARGS__)
#define CP_ERROR(...)  CP_LOG(SWARM_LOG_ERROR,   __VA_ARGS__)
