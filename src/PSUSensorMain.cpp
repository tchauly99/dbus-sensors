/*
// Copyright (c) 2019 Intel Corporation
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

#include <PSUEvent.hpp>
#include <PSUSensor.hpp>
#include <Utils.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

static constexpr bool debug = false;

static constexpr auto sensorTypes{std::to_array<const char*>(
    {"xyz.openbmc_project.Configuration.ADM1266",
     "xyz.openbmc_project.Configuration.ADM1272",
     "xyz.openbmc_project.Configuration.ADM1275",
     "xyz.openbmc_project.Configuration.ADM1278",
     "xyz.openbmc_project.Configuration.ADM1293",
     "xyz.openbmc_project.Configuration.ADS7830",
     "xyz.openbmc_project.Configuration.BMR490",
     "xyz.openbmc_project.Configuration.DPS800",
     "xyz.openbmc_project.Configuration.INA219",
     "xyz.openbmc_project.Configuration.INA230",
     "xyz.openbmc_project.Configuration.IPSPS",
     "xyz.openbmc_project.Configuration.ISL68137",
     "xyz.openbmc_project.Configuration.ISL68220",
     "xyz.openbmc_project.Configuration.ISL68223",
     "xyz.openbmc_project.Configuration.ISL69225",
     "xyz.openbmc_project.Configuration.ISL69243",
     "xyz.openbmc_project.Configuration.ISL69260",
     "xyz.openbmc_project.Configuration.LM25066",
     "xyz.openbmc_project.Configuration.MAX16601",
     "xyz.openbmc_project.Configuration.MAX20710",
     "xyz.openbmc_project.Configuration.MAX20730",
     "xyz.openbmc_project.Configuration.MAX20734",
     "xyz.openbmc_project.Configuration.MAX20796",
     "xyz.openbmc_project.Configuration.MAX34451",
     "xyz.openbmc_project.Configuration.MP2971",
     "xyz.openbmc_project.Configuration.MP2973",
     "xyz.openbmc_project.Configuration.MP5023",
     "xyz.openbmc_project.Configuration.pmbus",
     "xyz.openbmc_project.Configuration.PXE1610",
     "xyz.openbmc_project.Configuration.RAA228000",
     "xyz.openbmc_project.Configuration.RAA228228",
     "xyz.openbmc_project.Configuration.RAA228620",
     "xyz.openbmc_project.Configuration.RAA229001",
     "xyz.openbmc_project.Configuration.RAA229004",
     "xyz.openbmc_project.Configuration.RAA229126",
     "xyz.openbmc_project.Configuration.TPS546D24",
     "xyz.openbmc_project.Configuration.XDPE12284"})};

// clang-format off
static constexpr auto pmbusNames{std::to_array<const char*>({
    "adm1266",
    "adm1272",
    "adm1275",
    "adm1278",
    "adm1293",
    "ads7830",
    "bmr490",
    "dps800",
    "ina219",
    "ina230",
    "ipsps1",
    "isl68137",
    "isl68220",
    "isl68223",
    "isl69225",
    "isl69243",
    "isl69260",
    "lm25066",
    "max16601",
    "max20710",
    "max20730",
    "max20734",
    "max20796",
    "max34451",
    "mp2971",
    "mp2973",
    "mp5023",
    "pmbus",
    "pxe1610",
    "raa228000",
    "raa228228",
    "raa228620",
    "raa229001",
    "raa229004",
    "raa229126",
    "tps546d24",
    "xdpe12284"
})};
//clang-format on

namespace fs = std::filesystem;

static boost::container::flat_map<std::string, std::shared_ptr<PSUSensor>>
    sensors;
static boost::container::flat_map<std::string, std::unique_ptr<PSUCombineEvent>>
    combineEvents;
static boost::container::flat_map<std::string, std::unique_ptr<PwmSensor>>
    pwmSensors;
static boost::container::flat_map<std::string, std::string> sensorTable;
static boost::container::flat_map<std::string, PSUProperty> labelMatch;
static boost::container::flat_map<std::string, std::string> pwmTable;
static boost::container::flat_map<std::string, std::vector<std::string>>
    eventMatch;
static boost::container::flat_map<
    std::string,
    boost::container::flat_map<std::string, std::vector<std::string>>>
    groupEventMatch;
static boost::container::flat_map<std::string, std::vector<std::string>>
    limitEventMatch;

static std::vector<PSUProperty> psuProperties;

// Function CheckEvent will check each attribute from eventMatch table in the
// sysfs. If the attributes exists in sysfs, then store the complete path
// of the attribute into eventPathList.
void checkEvent(
    const std::string& directory,
    const boost::container::flat_map<std::string, std::vector<std::string>>&
        eventMatch,
    boost::container::flat_map<std::string, std::vector<std::string>>&
        eventPathList)
{
    for (const auto& match : eventMatch)
    {
        const std::vector<std::string>& eventAttrs = match.second;
        const std::string& eventName = match.first;
        for (const auto& eventAttr : eventAttrs)
        {
            std::string eventPath = directory;
            eventPath += "/";
            eventPath += eventAttr;

            std::ifstream eventFile(eventPath);
            if (!eventFile.good())
            {
                continue;
            }

            eventPathList[eventName].push_back(eventPath);
        }
    }
}

// Check Group Events which contains more than one targets in each combine
// events.
void checkGroupEvent(
    const std::string& directory,
    const boost::container::flat_map<
        std::string,
        boost::container::flat_map<std::string, std::vector<std::string>>>&
        groupEventMatch,
    boost::container::flat_map<
        std::string,
        boost::container::flat_map<std::string, std::vector<std::string>>>&
        groupEventPathList)
{
    for (const auto& match : groupEventMatch)
    {
        const std::string& groupEventName = match.first;
        const boost::container::flat_map<std::string, std::vector<std::string>>
            events = match.second;
        boost::container::flat_map<std::string, std::vector<std::string>>
            pathList;
        for (const auto& match : events)
        {
            const std::string& eventName = match.first;
            const std::vector<std::string>& eventAttrs = match.second;
            for (const auto& eventAttr : eventAttrs)
            {
                std::string eventPath = directory;
                eventPath += "/";
                eventPath += eventAttr;
                std::ifstream eventFile(eventPath);
                if (!eventFile.good())
                {
                    continue;
                }

                pathList[eventName].push_back(eventPath);
            }
        }
        groupEventPathList[groupEventName] = pathList;
    }
}

// Function checkEventLimits will check all the psu related xxx_input attributes
// in sysfs to see if xxx_crit_alarm xxx_lcrit_alarm xxx_max_alarm
// xxx_min_alarm exist, then store the existing paths of the alarm attributes
// to eventPathList.
void checkEventLimits(
    const std::string& sensorPathStr,
    const boost::container::flat_map<std::string, std::vector<std::string>>&
        limitEventMatch,
    boost::container::flat_map<std::string, std::vector<std::string>>&
        eventPathList)
{
    auto attributePartPos = sensorPathStr.find_last_of('_');
    if (attributePartPos == std::string::npos)
    {
        // There is no '_' in the string, skip it
        return;
    }
    auto attributePart =
        std::string_view(sensorPathStr).substr(attributePartPos + 1);
    if (attributePart != "input")
    {
        // If the sensor is not xxx_input, skip it
        return;
    }

    auto prefixPart = sensorPathStr.substr(0, attributePartPos + 1);
    for (const auto& limitMatch : limitEventMatch)
    {
        const std::vector<std::string>& limitEventAttrs = limitMatch.second;
        const std::string& eventName = limitMatch.first;
        for (const auto& limitEventAttr : limitEventAttrs)
        {
            auto limitEventPath = prefixPart + limitEventAttr;
            std::ifstream eventFile(limitEventPath);
            if (!eventFile.good())
            {
                continue;
            }
            eventPathList[eventName].push_back(limitEventPath);
        }
    }
}

static void
    checkPWMSensor(const fs::path& sensorPath, std::string& labelHead,
                   const std::string& interfacePath,
                   std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
                   sdbusplus::asio::object_server& objectServer,
                   const std::string& psuName)
{
    for (const auto& pwmName : pwmTable)
    {
        if (pwmName.first != labelHead)
        {
            continue;
        }

        const std::string& sensorPathStr = sensorPath.string();
        const std::string& pwmPathStr =
            boost::replace_all_copy(sensorPathStr, "input", "target");
        std::ifstream pwmFile(pwmPathStr);
        if (!pwmFile.good())
        {
            continue;
        }

        auto findPWMSensor = pwmSensors.find(psuName + labelHead);
        if (findPWMSensor != pwmSensors.end())
        {
            continue;
        }

        pwmSensors[psuName + labelHead] = std::make_unique<PwmSensor>(
            "Pwm_" + psuName + "_" + pwmName.second, pwmPathStr, dbusConnection,
            objectServer, interfacePath + "_" + pwmName.second, "PSU");
    }
}

static void createSensorsCallback(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const ManagedObjectType& sensorConfigs,
    const std::shared_ptr<boost::container::flat_set<std::string>>&
        sensorsChanged)
{
    int numCreated = 0;
    bool firstScan = sensorsChanged == nullptr;

    std::vector<fs::path> pmbusPaths;
    if (!findFiles(fs::path("/sys/class/hwmon"), "name", pmbusPaths))
    {
        std::cerr << "No PSU sensors in system\n";
        return;
    }

    boost::container::flat_set<std::string> directories;
    for (const auto& pmbusPath : pmbusPaths)
    {
        boost::container::flat_map<std::string, std::vector<std::string>>
            eventPathList;
        boost::container::flat_map<
            std::string,
            boost::container::flat_map<std::string, std::vector<std::string>>>
            groupEventPathList;

        std::ifstream nameFile(pmbusPath);
        if (!nameFile.good())
        {
            std::cerr << "Failure finding pmbus path " << pmbusPath << "\n";
            continue;
        }

        std::string pmbusName;
        std::getline(nameFile, pmbusName);
        nameFile.close();

        if (std::find(pmbusNames.begin(), pmbusNames.end(), pmbusName) ==
            pmbusNames.end())
        {
            // To avoid this error message, add your driver name to
            // the pmbusNames vector at the top of this file.
            std::cerr << "Driver name " << pmbusName
                      << " not found in sensor whitelist\n";
            continue;
        }

        auto directory = pmbusPath.parent_path();

        auto ret = directories.insert(directory.string());
        if (!ret.second)
        {
            std::cerr << "Duplicate path " << directory.string() << "\n";
            continue; // check if path has already been searched
        }

        fs::path device = directory / "device";
        std::string deviceName = fs::canonical(device).stem();
        auto findHyphen = deviceName.find('-');
        if (findHyphen == std::string::npos)
        {
            std::cerr << "found bad device" << deviceName << "\n";
            continue;
        }
        std::string busStr = deviceName.substr(0, findHyphen);
        std::string addrStr = deviceName.substr(findHyphen + 1);

        size_t bus = 0;
        size_t addr = 0;

        try
        {
            bus = std::stoi(busStr);
            addr = std::stoi(addrStr, nullptr, 16);
        }
        catch (const std::invalid_argument&)
        {
            std::cerr << "Error parsing bus " << busStr << " addr " << addrStr
                      << "\n";
            continue;
        }

        const std::pair<std::string, boost::container::flat_map<
                                         std::string, BasicVariantType>>*
            baseConfig = nullptr;
        const SensorData* sensorData = nullptr;
        const std::string* interfacePath = nullptr;
        const char* sensorType = nullptr;
        size_t thresholdConfSize = 0;

        for (const std::pair<sdbusplus::message::object_path, SensorData>&
                 sensor : sensorConfigs)
        {
            sensorData = &(sensor.second);
            for (const char* type : sensorTypes)
            {
                auto sensorBase = sensorData->find(type);
                if (sensorBase != sensorData->end())
                {
                    baseConfig = &(*sensorBase);
                    sensorType = type;
                    break;
                }
            }
            if (baseConfig == nullptr)
            {
                std::cerr << "error finding base configuration for "
                          << deviceName << "\n";
                continue;
            }

            auto configBus = baseConfig->second.find("Bus");
            auto configAddress = baseConfig->second.find("Address");

            if (configBus == baseConfig->second.end() ||
                configAddress == baseConfig->second.end())
            {
                std::cerr << "error finding necessary entry in configuration\n";
                continue;
            }

            const uint64_t* confBus = std::get_if<uint64_t>(&(configBus->second));
            const uint64_t* confAddr = std::get_if<uint64_t>(&(configAddress->second));
            if (confBus == nullptr || confAddr == nullptr)
            {
                std::cerr
                    << "Cannot get bus or address, invalid configuration\n";
                continue;
            }

            if ((*confBus != bus) || (*confAddr != addr))
            {
                std::cerr << "Configuration skipping " << *confBus << "-"
                          << *confAddr << " because not " << bus << "-" << addr
                          << "\n";
                continue;
            }

            std::vector<thresholds::Threshold> confThresholds;
            if (!parseThresholdsFromConfig(*sensorData, confThresholds))
            {
                std::cerr << "error populating total thresholds\n";
            }
            thresholdConfSize = confThresholds.size();

            interfacePath = &(sensor.first.str);
            break;
        }
        if (interfacePath == nullptr)
        {
            // To avoid this error message, add your export map entry,
            // from Entity Manager, to sensorTypes at the top of this file.
            std::cerr << "failed to find match for " << deviceName << "\n";
            continue;
        }

        auto findPSUName = baseConfig->second.find("Name");
        if (findPSUName == baseConfig->second.end())
        {
            std::cerr << "could not determine configuration name for "
                      << deviceName << "\n";
            continue;
        }
        const std::string* psuName = std::get_if<std::string>(&(findPSUName->second));
        if (psuName == nullptr)
        {
            std::cerr << "Cannot find psu name, invalid configuration\n";
            continue;
        }

        // on rescans, only update sensors we were signaled by
        if (!firstScan)
        {
            std::string psuNameStr = "/" + escapeName(*psuName);
            auto it =
                std::find_if(sensorsChanged->begin(), sensorsChanged->end(),
                             [psuNameStr](std::string& s) {
                                 return boost::ends_with(s, psuNameStr);
                             });

            if (it == sensorsChanged->end())
            {
                continue;
            }
            sensorsChanged->erase(it);
        }
        checkEvent(directory.string(), eventMatch, eventPathList);
        checkGroupEvent(directory.string(), groupEventMatch,
                        groupEventPathList);

        PowerState readState = PowerState::always;
        auto findPowerOn = baseConfig->second.find("PowerState");
        if (findPowerOn != baseConfig->second.end())
        {
            std::string powerState =
                std::visit(VariantToStringVisitor(), findPowerOn->second);
            setReadState(powerState, readState);
        }

        /* Check if there are more sensors in the same interface */
        int i = 1;
        std::vector<std::string> psuNames;
        do
        {
            // Individual string fields: Name, Name1, Name2, Name3, ...
            psuNames.push_back(
                escapeName(std::get<std::string>(findPSUName->second)));
            findPSUName = baseConfig->second.find("Name" + std::to_string(i++));
        } while (findPSUName != baseConfig->second.end());

        std::vector<fs::path> sensorPaths;
        if (!findFiles(directory, R"(\w\d+_input$)", sensorPaths, 0))
        {
            std::cerr << "No PSU non-label sensor in PSU\n";
            continue;
        }

        /* read max value in sysfs for in, curr, power, temp, ... */
        if (!findFiles(directory, R"(\w\d+_max$)", sensorPaths, 0))
        {
            if constexpr (debug)
            {
                std::cerr << "No max name in PSU \n";
            }
        }

        /* The poll rate for the sensors */
        double pollRate = 0.0;
        auto pollRateObj = baseConfig->second.find("PollRate");

        if (pollRateObj != baseConfig->second.end())
        {
            pollRate =
                std::visit(VariantToDoubleVisitor(), pollRateObj->second);
            if (pollRate <= 0.0)
            {
                pollRate = PSUSensor::defaultSensorPoll;
            }
        }

        /* Find array of labels to be exposed if it is defined in config */
        std::vector<std::string> findLabels;
        auto findLabelObj = baseConfig->second.find("Labels");
        if (findLabelObj != baseConfig->second.end())
        {
            findLabels =
                std::get<std::vector<std::string>>(findLabelObj->second);
        }

        std::regex sensorNameRegEx("([A-Za-z]+)[0-9]*_");
        std::smatch matches;

        for (const auto& sensorPath : sensorPaths)
        {
            bool maxLabel = false;
            std::string labelHead;
            std::string sensorPathStr = sensorPath.string();
            std::string sensorNameStr = sensorPath.filename();
            std::string sensorNameSubStr{""};
            if (std::regex_search(sensorNameStr, matches, sensorNameRegEx))
            {
                // hwmon *_input filename without number:
                // in, curr, power, temp, ...
                sensorNameSubStr = matches[1];
            }
            else
            {
                std::cerr << "Could not extract the alpha prefix from "
                          << sensorNameStr;
                continue;
            }

            std::string labelPath;

            /* find and differentiate _max and _input to replace "label" */
            size_t pos = sensorPathStr.find('_');
            if (pos != std::string::npos)
            {

                std::string sensorPathStrMax = sensorPathStr.substr(pos);
                if (sensorPathStrMax.compare("_max") == 0)
                {
                    labelPath =
                        boost::replace_all_copy(sensorPathStr, "max", "label");
                    maxLabel = true;
                }
                else
                {
                    labelPath = boost::replace_all_copy(sensorPathStr, "input",
                                                        "label");
                    maxLabel = false;
                }
            }
            else
            {
                continue;
            }

            std::ifstream labelFile(labelPath);
            if (!labelFile.good())
            {
                if constexpr (debug)
                {
                    std::cerr << "Input file " << sensorPath
                              << " has no corresponding label file\n";
                }
                // hwmon *_input filename with number:
                // temp1, temp2, temp3, ...
                labelHead = sensorNameStr.substr(0, sensorNameStr.find('_'));
            }
            else
            {
                std::string label;
                std::getline(labelFile, label);
                labelFile.close();
                auto findSensor = sensors.find(label);
                if (findSensor != sensors.end())
                {
                    continue;
                }

                // hwmon corresponding *_label file contents:
                // vin1, vout1, ...
                labelHead = label.substr(0, label.find(' '));
            }

            /* append "max" for labelMatch */
            if (maxLabel)
            {
                labelHead.insert(0, "max");
            }

            if constexpr (debug)
            {
                std::cerr << "Sensor type=\"" << sensorNameSubStr
                          << "\" label=\"" << labelHead << "\"\n";
            }

            checkPWMSensor(sensorPath, labelHead, *interfacePath,
                           dbusConnection, objectServer, psuNames[0]);

            if (!findLabels.empty())
            {
                /* Check if this labelHead is enabled in config file */
                if (std::find(findLabels.begin(), findLabels.end(),
                              labelHead) == findLabels.end())
                {
                    if constexpr (debug)
                    {
                        std::cerr << "could not find " << labelHead
                                  << " in the Labels list\n";
                    }
                    continue;
                }
            }

            auto findProperty = labelMatch.find(labelHead);
            if (findProperty == labelMatch.end())
            {
                if constexpr (debug)
                {
                    std::cerr << "Could not find matching default property for "
                              << labelHead << "\n";
                }
                continue;
            }

            // Protect the hardcoded labelMatch list from changes,
            // by making a copy and modifying that instead.
            // Avoid bleedthrough of one device's customizations to
            // the next device, as each should be independently customizable.
            psuProperties.push_back(findProperty->second);
            auto psuProperty = psuProperties.rbegin();

            // Use label head as prefix for reading from config file,
            // example if temp1: temp1_Name, temp1_Scale, temp1_Min, ...
            std::string keyName = labelHead + "_Name";
            std::string keyScale = labelHead + "_Scale";
            std::string keyMin = labelHead + "_Min";
            std::string keyMax = labelHead + "_Max";
            std::string keyOffset = labelHead + "_Offset";
            std::string keyPowerState = labelHead + "_PowerState";

            bool customizedName = false;
            auto findCustomName = baseConfig->second.find(keyName);
            if (findCustomName != baseConfig->second.end())
            {
                try
                {
                    psuProperty->labelTypeName = std::visit(
                        VariantToStringVisitor(), findCustomName->second);
                }
                catch (const std::invalid_argument&)
                {
                    std::cerr << "Unable to parse " << keyName << "\n";
                    continue;
                }

                // All strings are valid, including empty string
                customizedName = true;
            }

            bool customizedScale = false;
            auto findCustomScale = baseConfig->second.find(keyScale);
            if (findCustomScale != baseConfig->second.end())
            {
                try
                {
                    psuProperty->sensorScaleFactor = std::visit(
                        VariantToUnsignedIntVisitor(), findCustomScale->second);
                }
                catch (const std::invalid_argument&)
                {
                    std::cerr << "Unable to parse " << keyScale << "\n";
                    continue;
                }

                // Avoid later division by zero
                if (psuProperty->sensorScaleFactor > 0)
                {
                    customizedScale = true;
                }
                else
                {
                    std::cerr << "Unable to accept " << keyScale << "\n";
                    continue;
                }
            }

            auto findCustomMin = baseConfig->second.find(keyMin);
            if (findCustomMin != baseConfig->second.end())
            {
                try
                {
                    psuProperty->minReading = std::visit(
                        VariantToDoubleVisitor(), findCustomMin->second);
                }
                catch (const std::invalid_argument&)
                {
                    std::cerr << "Unable to parse " << keyMin << "\n";
                    continue;
                }
            }

            auto findCustomMax = baseConfig->second.find(keyMax);
            if (findCustomMax != baseConfig->second.end())
            {
                try
                {
                    psuProperty->maxReading = std::visit(
                        VariantToDoubleVisitor(), findCustomMax->second);
                }
                catch (const std::invalid_argument&)
                {
                    std::cerr << "Unable to parse " << keyMax << "\n";
                    continue;
                }
            }

            auto findCustomOffset = baseConfig->second.find(keyOffset);
            if (findCustomOffset != baseConfig->second.end())
            {
                try
                {
                    psuProperty->sensorOffset = std::visit(
                        VariantToDoubleVisitor(), findCustomOffset->second);
                }
                catch (const std::invalid_argument&)
                {
                    std::cerr << "Unable to parse " << keyOffset << "\n";
                    continue;
                }
            }

            // if we find label head power state set ，override the powerstate.
            auto findPowerState = baseConfig->second.find(keyPowerState);
            if (findPowerState != baseConfig->second.end())
            {
                std::string powerState = std::visit(VariantToStringVisitor(),
                                                    findPowerState->second);
                setReadState(powerState, readState);
            }
            if (!(psuProperty->minReading < psuProperty->maxReading))
            {
                std::cerr << "Min must be less than Max\n";
                continue;
            }

            // If the sensor name is being customized by config file,
            // then prefix/suffix composition becomes not necessary,
            // and in fact not wanted, because it gets in the way.
            std::string psuNameFromIndex;
            if (!customizedName)
            {
                /* Find out sensor name index for this label */
                std::regex rgx("[A-Za-z]+([0-9]+)");
                size_t nameIndex{0};
                if (std::regex_search(labelHead, matches, rgx))
                {
                    nameIndex = std::stoi(matches[1]);

                    // Decrement to preserve alignment, because hwmon
                    // human-readable filenames and labels use 1-based
                    // numbering, but the "Name", "Name1", "Name2", etc. naming
                    // convention (the psuNames vector) uses 0-based numbering.
                    if (nameIndex > 0)
                    {
                        --nameIndex;
                    }
                }
                else
                {
                    nameIndex = 0;
                }

                if (psuNames.size() <= nameIndex)
                {
                    std::cerr << "Could not pair " << labelHead
                              << " with a Name field\n";
                    continue;
                }

                psuNameFromIndex = psuNames[nameIndex];

                if constexpr (debug)
                {
                    std::cerr << "Sensor label head " << labelHead
                              << " paired with " << psuNameFromIndex
                              << " at index " << nameIndex << "\n";
                }
            }

            checkEventLimits(sensorPathStr, limitEventMatch, eventPathList);

            // Similarly, if sensor scaling factor is being customized,
            // then the below power-of-10 constraint becomes unnecessary,
            // as config should be able to specify an arbitrary divisor.
            unsigned int factor = psuProperty->sensorScaleFactor;
            if (!customizedScale)
            {
                // Preserve existing usage of hardcoded labelMatch table below
                factor = std::pow(10.0, factor);

                /* Change first char of substring to uppercase */
                char firstChar =
                    static_cast<char>(std::toupper(sensorNameSubStr[0]));
                std::string strScaleFactor =
                    firstChar + sensorNameSubStr.substr(1) + "ScaleFactor";

                // Preserve existing configs by accepting earlier syntax,
                // example CurrScaleFactor, PowerScaleFactor, ...
                auto findScaleFactor = baseConfig->second.find(strScaleFactor);
                if (findScaleFactor != baseConfig->second.end())
                {
                    factor = std::visit(VariantToIntVisitor(),
                                        findScaleFactor->second);
                }

                if constexpr (debug)
                {
                    std::cerr << "Sensor scaling factor " << factor
                              << " string " << strScaleFactor << "\n";
                }
            }

            std::vector<thresholds::Threshold> sensorThresholds;
            if (!parseThresholdsFromConfig(*sensorData, sensorThresholds,
                                           &labelHead))
            {
                std::cerr << "error populating thresholds for "
                          << sensorNameSubStr << "\n";
            }

            auto findSensorUnit = sensorTable.find(sensorNameSubStr);
            if (findSensorUnit == sensorTable.end())
            {
                std::cerr << sensorNameSubStr
                          << " is not a recognized sensor type\n";
                continue;
            }

            if constexpr (debug)
            {
                std::cerr << "Sensor properties: Name \""
                          << psuProperty->labelTypeName << "\" Scale "
                          << psuProperty->sensorScaleFactor << " Min "
                          << psuProperty->minReading << " Max "
                          << psuProperty->maxReading << " Offset "
                          << psuProperty->sensorOffset << "\n";
            }

            std::string sensorName = psuProperty->labelTypeName;
            if (customizedName)
            {
                if (sensorName.empty())
                {
                    // Allow selective disabling of an individual sensor,
                    // by customizing its name to an empty string.
                    std::cerr << "Sensor disabled, empty string\n";
                    continue;
                }
            }
            else
            {
                // Sensor name not customized, do prefix/suffix composition,
                // preserving default behavior by using psuNameFromIndex.
                sensorName =
                    psuNameFromIndex + " " + psuProperty->labelTypeName;
            }

            if constexpr (debug)
            {
                std::cerr << "Sensor name \"" << sensorName << "\" path \""
                          << sensorPathStr << "\" type \"" << sensorType
                          << "\"\n";
            }
            // destruct existing one first if already created
            sensors[sensorName] = nullptr;
            sensors[sensorName] = std::make_shared<PSUSensor>(
                sensorPathStr, sensorType, objectServer, dbusConnection, io,
                sensorName, std::move(sensorThresholds), *interfacePath,
                readState, findSensorUnit->second, factor,
                psuProperty->maxReading, psuProperty->minReading,
                psuProperty->sensorOffset, labelHead, thresholdConfSize,
                pollRate);
            sensors[sensorName]->setupRead();
            ++numCreated;
            if constexpr (debug)
            {
                std::cerr << "Created " << numCreated << " sensors so far\n";
            }
        }

        // OperationalStatus event
        combineEvents[*psuName + "OperationalStatus"] = nullptr;
        combineEvents[*psuName + "OperationalStatus"] =
            std::make_unique<PSUCombineEvent>(objectServer, dbusConnection, io,
                                              *psuName, readState,
                                              eventPathList, groupEventPathList,
                                              "OperationalStatus", pollRate);
    }

    if constexpr (debug)
    {
        std::cerr << "Created total of " << numCreated << " sensors\n";
    }
    return;
}

void createSensors(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    const std::shared_ptr<boost::container::flat_set<std::string>>&
        sensorsChanged)
{
    auto getter = std::make_shared<GetSensorConfiguration>(
        dbusConnection, [&io, &objectServer, &dbusConnection, sensorsChanged](
                            const ManagedObjectType& sensorConfigs) {
            createSensorsCallback(io, objectServer, dbusConnection,
                                  sensorConfigs, sensorsChanged);
        });
    getter->getConfiguration(
        std::vector<std::string>(sensorTypes.begin(), sensorTypes.end()));
}

void propertyInitialize(void)
{
    sensorTable = {{"power", sensor_paths::unitWatts},
                   {"curr", sensor_paths::unitAmperes},
                   {"temp", sensor_paths::unitDegreesC},
                   {"in", sensor_paths::unitVolts},
                   {"fan", sensor_paths::unitRPMs}};

    labelMatch = {
        {"pin", PSUProperty("Input Power", 3000, 0, 6, 0)},
        {"pin1", PSUProperty("Input Power", 3000, 0, 6, 0)},
        {"pin2", PSUProperty("Input Power", 3000, 0, 6, 0)},
        {"pout1", PSUProperty("Output Power", 3000, 0, 6, 0)},
        {"pout2", PSUProperty("Output Power", 3000, 0, 6, 0)},
        {"pout3", PSUProperty("Output Power", 3000, 0, 6, 0)},
        {"power1", PSUProperty("Output Power", 3000, 0, 6, 0)},
        {"power2", PSUProperty("Output Power", 3000, 0, 6, 0)},
        {"power3", PSUProperty("Output Power", 3000, 0, 6, 0)},
        {"power4", PSUProperty("Output Power", 3000, 0, 6, 0)},
        {"maxpin", PSUProperty("Max Input Power", 3000, 0, 6, 0)},
        {"vin", PSUProperty("Input Voltage", 300, 0, 3, 0)},
        {"maxvin", PSUProperty("Max Input Voltage", 300, 0, 3, 0)},
        {"vout1", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout2", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout3", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout4", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout5", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout6", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout7", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout8", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout9", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout10", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout11", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout12", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout13", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout14", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout15", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout16", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout17", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout18", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout19", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout20", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout21", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout22", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout23", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout24", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout25", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout26", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout27", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout28", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout29", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout30", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout31", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vout32", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"vmon", PSUProperty("Auxiliary Input Voltage", 255, 0, 3, 0)},
        {"in0", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"in1", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"in2", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"in3", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"in4", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"in5", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"in6", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"in7", PSUProperty("Output Voltage", 255, 0, 3, 0)},
        {"iin", PSUProperty("Input Current", 20, 0, 3, 0)},
        {"iin1", PSUProperty("Input Current", 20, 0, 3, 0)},
        {"iin2", PSUProperty("Input Current", 20, 0, 3, 0)},
        {"iout1", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"iout2", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"iout3", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"iout4", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"iout5", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"iout6", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"iout7", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"iout8", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"iout9", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"iout10", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"iout11", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"iout12", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"iout13", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"iout14", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"curr1", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"curr2", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"curr3", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"curr4", PSUProperty("Output Current", 255, 0, 3, 0)},
        {"maxiout1", PSUProperty("Max Output Current", 255, 0, 3, 0)},
        {"temp1", PSUProperty("Temperature", 127, -128, 3, 0)},
        {"temp2", PSUProperty("Temperature", 127, -128, 3, 0)},
        {"temp3", PSUProperty("Temperature", 127, -128, 3, 0)},
        {"temp4", PSUProperty("Temperature", 127, -128, 3, 0)},
        {"temp5", PSUProperty("Temperature", 127, -128, 3, 0)},
        {"temp6", PSUProperty("Temperature", 127, -128, 3, 0)},
        {"maxtemp1", PSUProperty("Max Temperature", 127, -128, 3, 0)},
        {"fan1", PSUProperty("Fan Speed 1", 30000, 0, 0, 0)},
        {"fan2", PSUProperty("Fan Speed 2", 30000, 0, 0, 0)}};

    pwmTable = {{"fan1", "Fan_1"}, {"fan2", "Fan_2"}};

    limitEventMatch = {{"PredictiveFailure", {"max_alarm", "min_alarm"}},
                       {"Failure", {"crit_alarm", "lcrit_alarm"}}};

    eventMatch = {{"PredictiveFailure", {"power1_alarm"}},
                  {"Failure", {"in2_alarm"}},
                  {"ACLost", {"in1_beep"}},
                  {"ConfigureError", {"in1_fault"}}};

    groupEventMatch = {{"FanFault",
                        {{"fan1", {"fan1_alarm", "fan1_fault"}},
                         {"fan2", {"fan2_alarm", "fan2_fault"}}}}};
}

int main()
{
    boost::asio::io_service io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);

    systemBus->request_name("xyz.openbmc_project.PSUSensor");
    sdbusplus::asio::object_server objectServer(systemBus);
    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> matches;
    auto sensorsChanged =
        std::make_shared<boost::container::flat_set<std::string>>();

    propertyInitialize();

    io.post([&]() { createSensors(io, objectServer, systemBus, nullptr); });
    boost::asio::deadline_timer filterTimer(io);
    std::function<void(sdbusplus::message::message&)> eventHandler =
        [&](sdbusplus::message::message& message) {
            if (message.is_method_error())
            {
                std::cerr << "callback method error\n";
                return;
            }
            sensorsChanged->insert(message.get_path());
            filterTimer.expires_from_now(boost::posix_time::seconds(3));
            filterTimer.async_wait([&](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    return;
                }
                if (ec)
                {
                    std::cerr << "timer error\n";
                }
                createSensors(io, objectServer, systemBus, sensorsChanged);
            });
        };

    for (const char* type : sensorTypes)
    {
        auto match = std::make_unique<sdbusplus::bus::match::match>(
            static_cast<sdbusplus::bus::bus&>(*systemBus),
            "type='signal',member='PropertiesChanged',path_namespace='" +
                std::string(inventoryPath) + "',arg0namespace='" + type + "'",
            eventHandler);
        matches.emplace_back(std::move(match));
    }

    setupManufacturingModeMatch(*systemBus);
    io.run();
}
