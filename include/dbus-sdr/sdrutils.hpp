/*
// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include <boost/algorithm/string.hpp>
#include <boost/bimap.hpp>
#include <boost/container/flat_map.hpp>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <ipmid/api.hpp>
#include <ipmid/types.hpp>
#include <map>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus/match.hpp>
#include <string>
#include <vector>

#pragma once

static constexpr bool debug = false;

struct CmpStrVersion
{
    bool operator()(std::string a, std::string b) const
    {
        return strverscmp(a.c_str(), b.c_str()) < 0;
    }
};

using SensorSubTree = boost::container::flat_map<
    std::string,
    boost::container::flat_map<std::string, std::vector<std::string>>,
    CmpStrVersion>;

using SensorNumMap = boost::bimap<int, std::string>;

static constexpr uint16_t maxSensorsPerLUN = 255;
static constexpr uint16_t maxIPMISensors = (maxSensorsPerLUN * 3);
static constexpr uint16_t lun1Sensor0 = 0x100;
static constexpr uint16_t lun3Sensor0 = 0x300;
static constexpr uint16_t invalidSensorNumber = 0xFFFF;
static constexpr uint8_t reservedSensorNumber = 0xFF;

namespace details
{
// Enable/disable the logging of stats instrumentation
static constexpr bool enableInstrumentation = false;

class IPMIStatsEntry
{
  private:
    int numReadings = 0;
    int numMissings = 0;
    int numStreakRead = 0;
    int numStreakMiss = 0;
    double minValue = 0.0;
    double maxValue = 0.0;
    std::string sensorName;

  public:
    const std::string& getName(void) const
    {
        return sensorName;
    }

    void updateName(std::string_view name)
    {
        sensorName = name;
    }

    // Returns true if this is the first successful reading
    // This is so the caller can log the coefficients used
    bool updateReading(double reading, int raw)
    {
        if constexpr (!enableInstrumentation)
        {
            return false;
        }

        bool first = ((numReadings == 0) && (numMissings == 0));

        // Sensors can use "nan" to indicate unavailable reading
        if (!(std::isfinite(reading)))
        {
            // Only show this if beginning a new streak
            if (numStreakMiss == 0)
            {
                std::cerr << "IPMI sensor " << sensorName
                          << ": Missing reading, byte=" << raw
                          << ", Reading counts good=" << numReadings
                          << " miss=" << numMissings
                          << ", Prior good streak=" << numStreakRead << "\n";
            }

            numStreakRead = 0;
            ++numMissings;
            ++numStreakMiss;

            return first;
        }

        // Only show this if beginning a new streak and not the first time
        if ((numStreakRead == 0) && (numReadings != 0))
        {
            std::cerr << "IPMI sensor " << sensorName
                      << ": Recovered reading, value=" << reading
                      << " byte=" << raw
                      << ", Reading counts good=" << numReadings
                      << " miss=" << numMissings
                      << ", Prior miss streak=" << numStreakMiss << "\n";
        }

        // Initialize min/max if the first successful reading
        if (numReadings == 0)
        {
            std::cerr << "IPMI sensor " << sensorName
                      << ": First reading, value=" << reading << " byte=" << raw
                      << "\n";

            minValue = reading;
            maxValue = reading;
        }

        numStreakMiss = 0;
        ++numReadings;
        ++numStreakRead;

        // Only provide subsequent output if new min/max established
        if (reading < minValue)
        {
            std::cerr << "IPMI sensor " << sensorName
                      << ": Lowest reading, value=" << reading
                      << " byte=" << raw << "\n";

            minValue = reading;
        }

        if (reading > maxValue)
        {
            std::cerr << "IPMI sensor " << sensorName
                      << ": Highest reading, value=" << reading
                      << " byte=" << raw << "\n";

            maxValue = reading;
        }

        return first;
    }
};

class IPMIStatsTable
{
  private:
    std::vector<IPMIStatsEntry> entries;

  private:
    void padEntries(size_t index)
    {
        char hexbuf[16];

        // Pad vector until entries[index] becomes a valid index
        while (entries.size() <= index)
        {
            // As name not known yet, use human-readable hex as name
            IPMIStatsEntry newEntry;
            sprintf(hexbuf, "0x%02zX", entries.size());
            newEntry.updateName(hexbuf);

            entries.push_back(std::move(newEntry));
        }
    }

  public:
    void wipeTable(void)
    {
        entries.clear();
    }

    const std::string& getName(size_t index)
    {
        padEntries(index);
        return entries[index].getName();
    }

    void updateName(size_t index, std::string_view name)
    {
        padEntries(index);
        entries[index].updateName(name);
    }

    bool updateReading(size_t index, double reading, int raw)
    {
        padEntries(index);
        return entries[index].updateReading(reading, raw);
    }
};

// Store information for threshold sensors and they are not used by VR
// sensors. These objects are global singletons, used from a variety of places.
inline IPMIStatsTable sdrStatsTable;

/**
 * Search ObjectMapper for sensors and update them to subtree.
 *
 * The function will search for sensors under either
 * /xyz/openbmc_project/sensors or /xyz/openbmc_project/extsensors. It will
 * optionally search VR typed sensors under /xyz/openbmc_project/vr
 *
 * @return the updated amount of times any of "sensors" or "extsensors" sensor
 * paths updated successfully, previous amount if all failed. The "vr"
 * sensor path is optional, and does not participate in the return value.
 */
uint16_t getSensorSubtree(std::shared_ptr<SensorSubTree>& subtree);

bool getSensorNumMap(std::shared_ptr<SensorNumMap>& sensorNumMap);
} // namespace details

bool getSensorSubtree(SensorSubTree& subtree);

#ifdef FEATURE_HYBRID_SENSORS
ipmi::sensor::IdInfoMap::const_iterator
    findStaticSensor(const std::string& path);
#endif

struct CmpStr
{
    bool operator()(const char* a, const char* b) const
    {
        return std::strcmp(a, b) < 0;
    }
};

static constexpr size_t sensorTypeCodes = 0;
static constexpr size_t sensorEventTypeCodes = 1;

enum class SensorTypeCodes : uint8_t
{
    reserved = 0x0,
    temperature = 0x1,
    voltage = 0x2,
    current = 0x3,
    fan = 0x4,
    other = 0xB,
    memory = 0x0c,
    power_unit = 0x09,
    buttons = 0x14,
    watchdog2 = 0x23,
};

enum class SensorEventTypeCodes : uint8_t
{
    unspecified = 0x00,
    threshold = 0x01,
    sensorSpecified = 0x6f
};

const static boost::container::flat_map<
    const char*, std::pair<SensorTypeCodes, SensorEventTypeCodes>, CmpStr>
    sensorTypes{
        {{"temperature", std::make_pair(SensorTypeCodes::temperature,
                                        SensorEventTypeCodes::threshold)},
         {"voltage", std::make_pair(SensorTypeCodes::voltage,
                                    SensorEventTypeCodes::threshold)},
         {"current", std::make_pair(SensorTypeCodes::current,
                                    SensorEventTypeCodes::threshold)},
         {"fan_tach", std::make_pair(SensorTypeCodes::fan,
                                     SensorEventTypeCodes::threshold)},
         {"fan_pwm", std::make_pair(SensorTypeCodes::fan,
                                    SensorEventTypeCodes::threshold)},
         {"power", std::make_pair(SensorTypeCodes::other,
                                  SensorEventTypeCodes::threshold)},
         {"memory", std::make_pair(SensorTypeCodes::memory,
                                   SensorEventTypeCodes::sensorSpecified)},
         {"state", std::make_pair(SensorTypeCodes::power_unit,
                                  SensorEventTypeCodes::sensorSpecified)},
         {"buttons", std::make_pair(SensorTypeCodes::buttons,
                                    SensorEventTypeCodes::sensorSpecified)},
         {"watchdog", std::make_pair(SensorTypeCodes::watchdog2,
                                     SensorEventTypeCodes::sensorSpecified)}}};

std::string getSensorTypeStringFromPath(const std::string& path);

uint8_t getSensorTypeFromPath(const std::string& path);

uint16_t getSensorNumberFromPath(const std::string& path);

uint8_t getSensorEventTypeFromPath(const std::string& path);

std::string getPathFromSensorNumber(uint16_t sensorNum);

namespace ipmi
{
std::map<std::string, std::vector<std::string>>
    getObjectInterfaces(const char* path);

std::map<std::string, Value> getEntityManagerProperties(const char* path,
                                                        const char* interface);

const std::string* getSensorConfigurationInterface(
    const std::map<std::string, std::vector<std::string>>&
        sensorInterfacesResponse);

void updateIpmiFromAssociation(const std::string& path,
                               const DbusInterfaceMap& sensorMap,
                               uint8_t& entityId, uint8_t& entityInstance);
} // namespace ipmi
