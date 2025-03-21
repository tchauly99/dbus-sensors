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

#include <unistd.h>

#include <TachSensor.hpp>
#include <Utils.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <gpiod.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <fstream>
#include <iostream>
#include <istream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

static constexpr unsigned int pwmPollMs = 500;

TachSensor::TachSensor(const std::string& path, const std::string& objectType,
                       sdbusplus::asio::object_server& objectServer,
                       std::shared_ptr<sdbusplus::asio::connection>& conn,
                       std::unique_ptr<PresenceSensor>&& presenceSensor,
                       std::optional<RedundancySensor>* redundancy,
                       boost::asio::io_service& io, const std::string& fanName,
                       std::vector<thresholds::Threshold>&& thresholdsIn,
                       const std::string& sensorConfiguration,
                       const std::pair<double, double>& limits,
                       const PowerState& powerState,
                       const std::optional<std::string>& ledIn) :
    Sensor(escapeName(fanName), std::move(thresholdsIn), sensorConfiguration,
           objectType, false, false, limits.second, limits.first, conn,
           powerState),
    objServer(objectServer), redundancy(redundancy),
    presence(std::move(presenceSensor)), inputDev(io), waitTimer(io),
    path(path), led(ledIn)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
    {
        std::cerr << "Tach sensor failed to open file\n";
    }
    inputDev.assign(fd);

    sensorInterface = objectServer.add_interface(
        "/xyz/openbmc_project/sensors/fan_tach/" + name,
        "xyz.openbmc_project.Sensor.Value");

    for (const auto& threshold : thresholds)
    {
        std::string interface = thresholds::getInterface(threshold.level);
        thresholdInterfaces[static_cast<size_t>(threshold.level)] =
            objectServer.add_interface(
                "/xyz/openbmc_project/sensors/fan_tach/" + name, interface);
    }
    association = objectServer.add_interface(
        "/xyz/openbmc_project/sensors/fan_tach/" + name,
        association::interface);

    if (presence)
    {
        itemIface =
            objectServer.add_interface("/xyz/openbmc_project/inventory/" + name,
                                       "xyz.openbmc_project.Inventory.Item");
        itemIface->register_property("PrettyName",
                                     std::string()); // unused property
        itemIface->register_property("Present", true);
        itemIface->initialize();
        itemAssoc = objectServer.add_interface(
            "/xyz/openbmc_project/inventory/" + name, association::interface);
        itemAssoc->register_property(
            "associations",
            std::vector<Association>{
                {"sensors", "inventory",
                 "/xyz/openbmc_project/sensors/fan_tach/" + name}});
        itemAssoc->initialize();
    }
    setInitialProperties(sensor_paths::unitRPMs);
    setupRead();
}

TachSensor::~TachSensor()
{
    // close the input dev to cancel async operations
    inputDev.close();
    waitTimer.cancel();
    for (const auto& iface : thresholdInterfaces)
    {
        objServer.remove_interface(iface);
    }
    objServer.remove_interface(sensorInterface);
    objServer.remove_interface(association);
    objServer.remove_interface(itemIface);
    objServer.remove_interface(itemAssoc);
}

void TachSensor::setupRead(void)
{
    boost::asio::async_read_until(inputDev, readBuf, '\n',
                                  [&](const boost::system::error_code& ec,
                                      std::size_t /*bytes_transfered*/) {
        handleResponse(ec);
    });
}

void TachSensor::handleResponse(const boost::system::error_code& err)
{
    if (err == boost::system::errc::bad_file_descriptor)
    {
        return; // we're being destroyed
    }
    bool missing = false;
    size_t pollTime = pwmPollMs;
    if (presence)
    {
        if (!presence->getValue())
        {
            markAvailable(false);
            missing = true;
            pollTime = sensorFailedPollTimeMs;
        }
        itemIface->set_property("Present", !missing);
    }
    std::istream responseStream(&readBuf);
    if (!missing)
    {
        if (!err)
        {
            std::string response;
            try
            {
                std::getline(responseStream, response);
                rawValue = std::stod(response);
                responseStream.clear();
                updateValue(rawValue);
            }
            catch (const std::invalid_argument&)
            {
                incrementError();
                pollTime = sensorFailedPollTimeMs;
            }
        }
        else
        {
            incrementError();
            pollTime = sensorFailedPollTimeMs;
        }
    }
    responseStream.clear();
    inputDev.close();

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
    {
        return; // we're no longer valid
    }
    inputDev.assign(fd);
    waitTimer.expires_from_now(boost::posix_time::milliseconds(pollTime));
    waitTimer.async_wait([&](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            return; // we're being canceled
        }
        setupRead();
    });
}

void TachSensor::checkThresholds(void)
{
    bool status = thresholds::checkThresholds(this);

    if (redundancy && *redundancy)
    {
        (*redundancy)
            ->update("/xyz/openbmc_project/sensors/fan_tach/" + name, !status);
    }

    bool curLed = !status;
    if (led && ledState != curLed)
    {
        ledState = curLed;
        setLed(dbusConnection, *led, curLed);
    }
}

PresenceSensor::PresenceSensor(const std::string& gpioName, bool inverted,
                               boost::asio::io_service& io,
                               const std::string& name) :
    gpioLine(gpiod::find_line(gpioName)),
    gpioFd(io), name(name)
{
    if (!gpioLine)
    {
        std::cerr << "Error requesting gpio: " << gpioName << "\n";
        status = false;
        return;
    }

    try
    {
        gpioLine.request({"FanSensor", gpiod::line_request::EVENT_BOTH_EDGES,
                          inverted ? gpiod::line_request::FLAG_ACTIVE_LOW : 0});
        status = gpioLine.get_value();

        int gpioLineFd = gpioLine.event_get_fd();
        if (gpioLineFd < 0)
        {
            std::cerr << "Failed to get " << gpioName << " fd\n";
            return;
        }

        gpioFd.assign(gpioLineFd);
    }
    catch (const std::system_error&)
    {
        std::cerr << "Error reading gpio: " << gpioName << "\n";
        status = false;
        return;
    }

    monitorPresence();
}

PresenceSensor::~PresenceSensor()
{
    gpioFd.close();
    gpioLine.release();
}

void PresenceSensor::monitorPresence(void)
{
    gpioFd.async_wait(boost::asio::posix::stream_descriptor::wait_read,
                      [this](const boost::system::error_code& ec) {
        if (ec == boost::system::errc::bad_file_descriptor)
        {
            return; // we're being destroyed
        }
        if (ec)
        {
            std::cerr << "Error on presence sensor " << name << " \n";
            ;
        }
        else
        {
            read();
        }
        monitorPresence();
    });
}

void PresenceSensor::read(void)
{
    gpioLine.event_read();
    status = gpioLine.get_value();
    // Read is invoked when an edge event is detected by monitorPresence
    if (status)
    {
        logFanInserted(name);
    }
    else
    {
        logFanRemoved(name);
    }
}

bool PresenceSensor::getValue(void)
{
    return status;
}

RedundancySensor::RedundancySensor(size_t count,
                                   const std::vector<std::string>& children,
                                   sdbusplus::asio::object_server& objectServer,
                                   const std::string& sensorConfiguration) :
    count(count),
    iface(objectServer.add_interface(
        "/xyz/openbmc_project/control/FanRedundancy/Tach",
        "xyz.openbmc_project.Control.FanRedundancy")),
    association(objectServer.add_interface(
        "/xyz/openbmc_project/control/FanRedundancy/Tach",
        association::interface)),
    objectServer(objectServer)
{
    createAssociation(association, sensorConfiguration);
    iface->register_property("Collection", children);
    iface->register_property("Status", std::string("Full"));
    iface->register_property("AllowedFailures", static_cast<uint8_t>(count));
    iface->initialize();
}
RedundancySensor::~RedundancySensor()
{
    objectServer.remove_interface(association);
    objectServer.remove_interface(iface);
}
void RedundancySensor::update(const std::string& name, bool failed)
{
    statuses[name] = failed;
    size_t failedCount = 0;

    std::string newState = redundancy::full;
    for (const auto& status : statuses)
    {
        if (status.second)
        {
            failedCount++;
        }
        if (failedCount > count)
        {
            newState = redundancy::failed;
            break;
        }
        if (failedCount)
        {
            newState = redundancy::degraded;
        }
    }
    if (state != newState)
    {
        if (state == redundancy::full)
        {
            logFanRedundancyLost();
        }
        else if (newState == redundancy::full)
        {
            logFanRedundancyRestored();
        }
        state = newState;
        iface->set_property("Status", state);
    }
}
