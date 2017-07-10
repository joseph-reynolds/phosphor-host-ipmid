#include "chassishandler.h"
#include "host-ipmid/ipmid-api.h"
#include "types.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <mapper.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <limits.h>
#include <string.h>
#include <endian.h>
#include <sstream>
#include <array>
#include <fstream>
#include <experimental/filesystem>
#include <string>

#include <phosphor-logging/log.hpp>
#include <phosphor-logging/elog-errors.hpp>
#include <xyz/openbmc_project/State/Host/server.hpp>
#include "xyz/openbmc_project/Common/error.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/object.hpp>

#include "config.h"

//Defines
#define SET_PARM_VERSION                     0x01
#define SET_PARM_BOOT_FLAGS_PERMANENT        0x40 //boot flags data1 7th bit on
#define SET_PARM_BOOT_FLAGS_VALID_ONE_TIME   0x80 //boot flags data1 8th bit on
#define SET_PARM_BOOT_FLAGS_VALID_PERMANENT  0xC0 //boot flags data1 7 & 8 bit on

constexpr size_t SIZE_MAC  = 18;
constexpr size_t SIZE_BOOT_OPTION = (uint8_t)BootOptionResponseSize::
        OPAL_NETWORK_SETTINGS;//Maximum size of the boot option parametrs
constexpr size_t SIZE_PREFIX = 7;
constexpr size_t MAX_PREFIX_VALUE = 32;
constexpr size_t SIZE_COOKIE = 4;
constexpr size_t SIZE_VERSION = 2;
constexpr auto   MAC_ADDRESS_FORMAT = "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx";
constexpr auto   IP_ADDRESS_FORMAT = "%d.%d.%d.%d";
constexpr auto   PREFIX_FORMAT = "%hhd";
constexpr auto   ADDR_TYPE_FORMAT = "%hhx";
//PetiBoot-Specific
static constexpr uint8_t net_conf_initial_bytes[] = {0x80,0x21, 0x70 ,0x62 ,0x21,
        0x00 ,0x01 ,0x06 ,0x04};

static constexpr size_t COOKIE_OFFSET = 1;
static constexpr size_t VERSION_OFFSET = 5;
static constexpr size_t MAC_OFFSET = 9;
static constexpr size_t ADDRTYPE_OFFSET = 16;
static constexpr size_t IPADDR_OFFSET = 17;
static constexpr size_t PREFIX_OFFSET = 21;
static constexpr size_t GATEWAY_OFFSET = 22;


void register_netfn_chassis_functions() __attribute__((constructor));

// Host settings in dbus
// Service name should be referenced by connection name got via object mapper
const char *settings_object_name  =  "/org/openbmc/settings/host0";
const char *settings_intf_name    =  "org.freedesktop.DBus.Properties";
const char *host_intf_name        =  "org.openbmc.settings.Host";


constexpr auto MAPPER_BUS_NAME = "xyz.openbmc_project.ObjectMapper";
constexpr auto MAPPER_OBJ = "/xyz/openbmc_project/object_mapper";
constexpr auto MAPPER_INTF = "xyz.openbmc_project.ObjectMapper";

constexpr auto SETTINGS_ROOT = "/";
constexpr auto SETTINGS_MATCH = "host0";
constexpr auto PROP_INTF = "org.freedesktop.DBus.Properties";

constexpr auto IP_INTERFACE = "xyz.openbmc_project.Network.IP";
constexpr auto MAC_INTERFACE = "xyz.openbmc_project.Network.MACAddress";

constexpr auto METHOD_GET = "Get";
constexpr auto METHOD_GET_ALL = "GetAll";
constexpr auto METHOD_SET = "Set";

typedef struct
{
    uint8_t cap_flags;
    uint8_t fru_info_dev_addr;
    uint8_t sdr_dev_addr;
    uint8_t sel_dev_addr;
    uint8_t system_management_dev_addr;
    uint8_t bridge_dev_addr;
}__attribute__((packed)) ipmi_chassis_cap_t;

typedef struct
{
    uint8_t cur_power_state;
    uint8_t last_power_event;
    uint8_t misc_power_state;
    uint8_t front_panel_button_cap_status;
}__attribute__((packed)) ipmi_get_chassis_status_t;

// Phosphor Host State manager
namespace State = sdbusplus::xyz::openbmc_project::State::server;

namespace fs = std::experimental::filesystem;

using namespace phosphor::logging;
using namespace sdbusplus::xyz::openbmc_project::Common::Error;

/** @brief Gets the dbus object info implementing the given interface
 *         from the given subtree.
 *  @param[in] interface - Dbus interface.
 *  @param[in] serviceRoot - subtree from where the search should start.
 *  @param[in] match - identifier for object.
 *  @return On success returns the object having objectpath and servicename.
 */

//TODO There may be cases where an interface is implemented by multiple
//  objects,to handle such cases we are interested on that object
//  which are on interested busname.
//  Currently mapper doesn't give the readable busname(gives busid) so we can't
//  use busname to find the object,will do later once the support is there.

ipmi::DbusObjectInfo getDbusObject(const std::string& interface,
                                   const std::string& serviceRoot = SETTINGS_ROOT,
                                   const std::string& match = "")
{
    std::vector<std::string>interfaces;
    interfaces.emplace_back(interface);

    auto bus = sdbusplus::bus::new_default();
    auto depth = 0;

    auto mapperCall = bus.new_method_call(MAPPER_BUS_NAME,
                                          MAPPER_OBJ,
                                          MAPPER_INTF,
                                          "GetSubTree");

    mapperCall.append(serviceRoot);
    mapperCall.append(depth);
    mapperCall.append(interfaces);

    auto mapperReply = bus.call(mapperCall);
    if (mapperReply.is_method_error())
    {
        log<level::ERR>("Error in mapper call");
        elog<InternalFailure>();
    }

    ipmi::ObjectTree objectTree;
    mapperReply.read(objectTree);

    if (objectTree.empty())
    {
        log<level::ERR>("No Object have impelmented the interface",
                        entry("INTERFACE=%s", interface.c_str()));
        elog<InternalFailure>();
    }

    ipmi::DbusObjectInfo objectInfo;

    // if match is empty then return the first object
    if(match == "")
    {
        objectInfo =  make_pair(objectTree.begin()->first,
            objectTree.begin()->second.begin()->first);
        return objectInfo;
    }

    // else search the match string in the object path
    auto objectFound = false;
    for (auto& object : objectTree)
    {
        if(object.first.find(match)!= std::string::npos)
        {
            objectFound = true;
            objectInfo = make_pair(object.first, object.second.begin()->first);
            break;
        }
    }

    if(!objectFound)
    {
        log<level::ERR>("Failed to find object which matches",
                        entry("MATCH=%s",match.c_str()));
        elog<InternalFailure>();
    }
    return objectInfo;

}

/** @brief Gets the value associated with the given object
 *         and the interface.
 *  @param[in] service - Dbus service name.
 *  @param[in] objPath - Dbus object path.
 *  @param[in] interface - Dbus interface.
 *  @param[in] property - name of the property.
 *  @return On success returns the value of the property.
 */
std::string getDbusProperty(const std::string& service,
                            const std::string& objPath,
                            const std::string& interface,
                            const std::string& property)
{

    sdbusplus::message::variant<std::string> name;

    auto bus = sdbusplus::bus::new_default();

    auto method = bus.new_method_call(
                      service.c_str(),
                      objPath.c_str(),
                      PROP_INTF,
                      METHOD_GET);

    method.append(interface, property);

    auto reply = bus.call(method);

    if (reply.is_method_error())
    {
         log<level::ERR>("Failed to get property",
                        entry("PROPERTY=%s", property.c_str()),
                        entry("PATH=%s", objPath.c_str()),
                        entry("INTERFACE=%s", interface.c_str()));
        elog<InternalFailure>();
    }

    reply.read(name);

    return name.get<std::string>();
}

/** @brief Gets all the properties associated with the given object
 *         and the interface.
 *  @param[in] service - Dbus service name.
 *  @param[in] objPath - Dbus object path.
 *  @param[in] interface - Dbus interface.
 *  @return On success returns the map of name value pair.
 */
ipmi::PropertyMap getAllDbusProperties(const std::string& service,
                                       const std::string& objPath,
                                       const std::string& interface)
{
    ipmi::PropertyMap properties;
    auto bus = sdbusplus::bus::new_default();

    auto method = bus.new_method_call(
                      service.c_str(),
                      objPath.c_str(),
                      PROP_INTF,
                      METHOD_GET_ALL);

    method.append(interface);

    auto reply = bus.call(method);

    if (reply.is_method_error())
    {
         log<level::ERR>("Failed to get all properties",
                        entry("PATH=%s", objPath.c_str()),
                        entry("INTERFACE=%s", interface.c_str()));
        elog<InternalFailure>();
    }

    reply.read(properties);
    return properties;
}

/** @brief Sets the property value of the given object.
 *  @param[in] service - Dbus service name.
 *  @param[in] objPath - Dbus object path.
 *  @param[in] interface - Dbus interface.
 *  @param[in] property - name of the property.
 *  @param[in] value - value which needs to be set.
 */
void setDbusProperty(const std::string& service,
                     const std::string& objPath,
                     const std::string& interface,
                     const std::string& property,
                     const ipmi::Value& value)
{
    auto bus = sdbusplus::bus::new_default();

    auto method = bus.new_method_call(
                      service.c_str(),
                      objPath.c_str(),
                      PROP_INTF,
                      METHOD_SET);

    method.append(interface);
    method.append(property, value);

    if (!bus.call(method))
    {
        log<level::ERR>("Failed to set property",
                        entry("PROPERTY=%s", property.c_str()),
                        entry("PATH=%s",objPath.c_str()),
                        entry("INTERFACE=%s",interface.c_str()));
        elog<InternalFailure>();
    }

}

//TODO : Can remove the below function as we have
//       new functions which uses sdbusplus.
//
//       openbmc/openbmc#1489
int dbus_get_property(const char *name, char **buf)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    sd_bus *bus = NULL;
    char *temp_buf = NULL;
    char *connection = NULL;
    int r;

    // Get the system bus where most system services are provided.
    bus = ipmid_get_sd_bus_connection();

    r = mapper_get_service(bus, settings_object_name, &connection);
    if (r < 0) {
        fprintf(stderr, "Failed to get %s connection: %s\n",
                settings_object_name, strerror(-r));
        goto finish;
    }

    /*
     * Bus, service, object path, interface and method are provided to call
     * the method.
     * Signatures and input arguments are provided by the arguments at the
     * end.
     */
    r = sd_bus_call_method(bus,
                           connection,                                 /* service to contact */
                           settings_object_name,                       /* object path */
                           settings_intf_name,                         /* interface name */
                           "Get",                                      /* method name */
                           &error,                                     /* object to return error in */
                           &m,                                         /* return message on success */
                           "ss",                                       /* input signature */
                           host_intf_name,                             /* first argument */
                           name);                                      /* second argument */

    if (r < 0) {
        fprintf(stderr, "Failed to issue method call: %s\n", error.message);
        goto finish;
    }

    /*
     * The output should be parsed exactly the same as the output formatting
     * specified.
     */
    r = sd_bus_message_read(m, "v", "s", &temp_buf);
    if (r < 0) {
        fprintf(stderr, "Failed to parse response message: %s\n", strerror(-r));
        goto finish;
    }

    *buf = strdup(temp_buf);
    /*    *buf = (char*) malloc(strlen(temp_buf));
    if (*buf) {
        strcpy(*buf, temp_buf);
    }
     */
    printf("IPMID boot option property get: {%s}.\n", (char *) temp_buf);

finish:
    sd_bus_error_free(&error);
    sd_bus_message_unref(m);
    free(connection);

    return r;
}

//TODO : Can remove the below function as we have
//       new functions which uses sdbusplus.
//
//       openbmc/openbmc#1489

int dbus_set_property(const char * name, const char *value)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    sd_bus *bus = NULL;
    char *connection = NULL;
    int r;

    // Get the system bus where most system services are provided.
    bus = ipmid_get_sd_bus_connection();

    r = mapper_get_service(bus, settings_object_name, &connection);
    if (r < 0) {
        fprintf(stderr, "Failed to get %s connection: %s\n",
                settings_object_name, strerror(-r));
        goto finish;
    }

    /*
     * Bus, service, object path, interface and method are provided to call
     * the method.
     * Signatures and input arguments are provided by the arguments at the
     * end.
     */
    r = sd_bus_call_method(bus,
                           connection,                                 /* service to contact */
                           settings_object_name,                       /* object path */
                           settings_intf_name,                         /* interface name */
                           "Set",                                      /* method name */
                           &error,                                     /* object to return error in */
                           &m,                                         /* return message on success */
                           "ssv",                                      /* input signature */
                           host_intf_name,                             /* first argument */
                           name,                                       /* second argument */
                           "s",                                        /* third argument */
                           value);                                     /* fourth argument */

    if (r < 0) {
        fprintf(stderr, "Failed to issue method call: %s\n", error.message);
        goto finish;
    }

    printf("IPMID boot option property set: {%s}.\n", value);

    finish:
    sd_bus_error_free(&error);
    sd_bus_message_unref(m);
    free(connection);

    return r;
}

struct get_sys_boot_options_t {
    uint8_t parameter;
    uint8_t set;
    uint8_t block;
}  __attribute__ ((packed));

struct get_sys_boot_options_response_t {
    uint8_t version;
    uint8_t parm;
    uint8_t data[SIZE_BOOT_OPTION];
}  __attribute__ ((packed));

struct set_sys_boot_options_t {
    uint8_t parameter;
    uint8_t data[SIZE_BOOT_OPTION];
}  __attribute__ ((packed));


int getHostNetworkData(get_sys_boot_options_response_t* respptr)
{
    ipmi::PropertyMap properties;
    int rc = 0;

    try
    {
        //TODO There may be cases where an interface is implemented by multiple
        // objects,to handle such cases we are interested on that object
        //  which are on interested busname.
        //  Currenlty mapper doesn't give the readable busname(gives busid)
        //  so we can't match with bus name so giving some object specific info
        //  as SETTINGS_MATCH.
        //  Later SETTINGS_MATCH will be replaced with busname.

        auto ipObjectInfo = getDbusObject(IP_INTERFACE, SETTINGS_ROOT,
                                          SETTINGS_MATCH);
        auto macObjectInfo = getDbusObject(MAC_INTERFACE, SETTINGS_ROOT,
                                           SETTINGS_MATCH);

        properties  = getAllDbusProperties(ipObjectInfo.second,
                                           ipObjectInfo.first, IP_INTERFACE);
        auto MACAddress =
            getDbusProperty(macObjectInfo.second, macObjectInfo.first,
                            MAC_INTERFACE, "MACAddress");

        sscanf(MACAddress.c_str(), MAC_ADDRESS_FORMAT,
               (respptr->data + MAC_OFFSET),
               (respptr->data + MAC_OFFSET + 1),
               (respptr->data + MAC_OFFSET + 2),
               (respptr->data + MAC_OFFSET + 3),
               (respptr->data + MAC_OFFSET + 4),
               (respptr->data + MAC_OFFSET + 5));


        respptr->data[MAC_OFFSET + 6] = 0x00;

        uint8_t addrType = (properties["Origin"].get<std::string>() ==
            "xyz.openbmc_project.Network.IP.AddressOrigin.Static") ? 1 : 0;

        memcpy(respptr->data + ADDRTYPE_OFFSET, &addrType, sizeof(addrType));

        // ipaddress and gateway would be in IPv4 format

        inet_pton(AF_INET, properties["Address"].get<std::string>().c_str(),
                  (respptr->data + IPADDR_OFFSET));

        uint8_t prefix = properties["PrefixLength"].get<uint8_t>();
        memcpy(respptr->data + PREFIX_OFFSET, &prefix, sizeof(prefix));

        inet_pton(AF_INET, properties["Gateway"].get<std::string>().c_str(),
                  (respptr->data + GATEWAY_OFFSET));

    }
    catch (InternalFailure& e)
    {
        commit<InternalFailure>();
        memset(respptr->data, 0, SIZE_BOOT_OPTION);
        rc = -1;
        return rc;
    }

    //PetiBoot-Specific
    //If sucess then copy the first 9 bytes to the data
    //else set the respptr to 0

    memcpy(respptr->data, net_conf_initial_bytes,
           sizeof(net_conf_initial_bytes));

#ifdef _IPMI_DEBUG_
    printf("\n===Printing the IPMI Formatted Data========\n");

    for (uint8_t pos = 0; pos < index; pos++)
    {
        printf("%02x ", respptr->data[pos]);
    }
#endif


    return rc;
}

int setHostNetworkData(set_sys_boot_options_t* reqptr)
{
    using namespace std::string_literals;
    std::string host_network_config;
    char mac[SIZE_MAC] = {0};
    char ipAddress[INET_ADDRSTRLEN] = {0};
    char gateway[INET_ADDRSTRLEN] = {0};
    char dhcp {0};
    std::string addressOrigin =
        "xyz.openbmc_project.Network.IP.AddressOrigin.Static";
    uint8_t prefix {0};
    uint32_t zeroCookie = 0;

    //cookie starts from second byte
    // version starts from sixth byte

    try
    {
        do
        {
            // cookie ==  0x21 0x70 0x62 0x21
            if (memcmp(&(reqptr->data[COOKIE_OFFSET]),
                        (net_conf_initial_bytes + COOKIE_OFFSET),
                        SIZE_COOKIE) != 0)
            {
                //cookie == 0
                if (memcmp(&(reqptr->data[COOKIE_OFFSET]),
                            &zeroCookie,
                            SIZE_COOKIE) == 0)
                {
                    // need to zero out the network settings.
                    break;
                }

                log<level::ERR>("Invalid Cookie");
                elog<InternalFailure>();
            }

            // vesion == 0x00 0x01
            if (memcmp(&(reqptr->data[VERSION_OFFSET]),
                        (net_conf_initial_bytes + VERSION_OFFSET),
                        SIZE_VERSION) != 0)
            {

                log<level::ERR>("Invalid Version");
                elog<InternalFailure>();
            }

            snprintf(mac, SIZE_MAC, MAC_ADDRESS_FORMAT,
                     reqptr->data[MAC_OFFSET],
                     reqptr->data[MAC_OFFSET + 1],
                     reqptr->data[MAC_OFFSET + 2],
                     reqptr->data[MAC_OFFSET + 3],
                     reqptr->data[MAC_OFFSET + 4],
                     reqptr->data[MAC_OFFSET + 5]);

            memcpy(&dhcp, &(reqptr->data[ADDRTYPE_OFFSET]),
                   sizeof(decltype(dhcp)));

            if (dhcp)
            {
                addressOrigin =
                    "xyz.openbmc_project.Network.IP.AddressOrigin.DHCP";
            }

            snprintf(ipAddress, INET_ADDRSTRLEN, IP_ADDRESS_FORMAT,
                     reqptr->data[IPADDR_OFFSET],
                     reqptr->data[IPADDR_OFFSET + 1],
                     reqptr->data[IPADDR_OFFSET + 2],
                     reqptr->data[IPADDR_OFFSET + 3]);


            memcpy(&prefix, &(reqptr->data[PREFIX_OFFSET]),
                   sizeof(decltype(prefix)));

            snprintf(gateway, INET_ADDRSTRLEN, IP_ADDRESS_FORMAT,
                     reqptr->data[GATEWAY_OFFSET],
                     reqptr->data[GATEWAY_OFFSET + 1],
                     reqptr->data[GATEWAY_OFFSET + 2],
                     reqptr->data[GATEWAY_OFFSET + 3]);
        } while(0);

        //Cookie == 0 or it is a valid cookie
        host_network_config += "ipaddress="s + ipAddress +
            ",prefix="s + std::to_string(prefix) + ",gateway="s + gateway +
            ",mac="s + mac + ",addressOrigin="s + addressOrigin;

        log<level::DEBUG>("Network configuration changed",
                entry("NETWORKCONFIG=%s", host_network_config.c_str()));

        auto ipObjectInfo = getDbusObject(IP_INTERFACE, SETTINGS_ROOT,
                                          SETTINGS_MATCH);
        auto macObjectInfo = getDbusObject(MAC_INTERFACE, SETTINGS_ROOT,
                                           SETTINGS_MATCH);
        // set the dbus property
        setDbusProperty(ipObjectInfo.second, ipObjectInfo.first,
                IP_INTERFACE, "Address", std::string(ipAddress));
        setDbusProperty(ipObjectInfo.second, ipObjectInfo.first,
                IP_INTERFACE, "PrefixLength", prefix);
        setDbusProperty(ipObjectInfo.second, ipObjectInfo.first,
                IP_INTERFACE, "Origin", addressOrigin);
        setDbusProperty(ipObjectInfo.second, ipObjectInfo.first,
                IP_INTERFACE, "Gateway", std::string(gateway));
        setDbusProperty(ipObjectInfo.second, ipObjectInfo.first,
                IP_INTERFACE, "Type",
                std::string("xyz.openbmc_project.Network.IP.Protocol.IPv4"));
        setDbusProperty(macObjectInfo.second, macObjectInfo.first,
                MAC_INTERFACE,"MACAddress", std::string(mac));

    }
    catch (InternalFailure& e)
    {
        commit<InternalFailure>();
        return -1;
    }

    return 0;
}

ipmi_ret_t ipmi_chassis_wildcard(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                                 ipmi_request_t request,
                                 ipmi_response_t response,
                                 ipmi_data_len_t data_len,
                                 ipmi_context_t context)
{
    printf("Handling CHASSIS WILDCARD Netfn:[0x%X], Cmd:[0x%X]\n",netfn, cmd);
    // Status code.
    ipmi_ret_t rc = IPMI_CC_INVALID;
    *data_len = 0;
    return rc;
}

ipmi_ret_t ipmi_get_chassis_cap(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                                ipmi_request_t request, ipmi_response_t response,
                                ipmi_data_len_t data_len, ipmi_context_t context)
{
    // sd_bus error
    ipmi_ret_t rc = IPMI_CC_OK;

    ipmi_chassis_cap_t chassis_cap{};

    *data_len = sizeof(ipmi_chassis_cap_t);

    // TODO: need future work. Get those flag from MRW.

    // capabilities flags
    // [7..4] - reserved
    // [3] – 1b = provides power interlock  (IPM 1.5)
    // [2] – 1b = provides Diagnostic Interrupt (FP NMI)
    // [1] – 1b = provides “Front Panel Lockout” (indicates that the chassis has capabilities
    //            to lock out external power control and reset button or front panel interfaces
    //            and/or detect tampering with those interfaces).
    // [0] -1b = Chassis provides intrusion (physical security) sensor.
    // set to default value 0x0.
    chassis_cap.cap_flags = 0x0;

    // Since we do not have a separate SDR Device/SEL Device/ FRU repository.
    // The 20h was given as those 5 device addresses.
    // Chassis FRU info Device Address
    chassis_cap.fru_info_dev_addr = 0x20;

    // Chassis SDR Device Address
    chassis_cap.sdr_dev_addr = 0x20;

    // Chassis SEL Device Address
    chassis_cap.sel_dev_addr = 0x20;

    // Chassis System Management Device Address
    chassis_cap.system_management_dev_addr = 0x20;

    // Chassis Bridge Device Address.
    chassis_cap.bridge_dev_addr = 0x20;

    memcpy(response, &chassis_cap, *data_len);

    return rc;
}

//------------------------------------------
// Calls into Host State Manager Dbus object
//------------------------------------------
int initiate_state_transition(State::Host::Transition transition)
{
    // OpenBMC Host State Manager dbus framework
    constexpr auto HOST_STATE_MANAGER_ROOT  = "/xyz/openbmc_project/state/host0";
    constexpr auto HOST_STATE_MANAGER_IFACE = "xyz.openbmc_project.State.Host";
    constexpr auto DBUS_PROPERTY_IFACE      = "org.freedesktop.DBus.Properties";
    constexpr auto PROPERTY                 = "RequestedHostTransition";

    // sd_bus error
    int rc = 0;
    char  *busname = NULL;

    // SD Bus error report mechanism.
    sd_bus_error bus_error = SD_BUS_ERROR_NULL;

    // Gets a hook onto either a SYSTEM or SESSION bus
    sd_bus *bus_type = ipmid_get_sd_bus_connection();
    rc = mapper_get_service(bus_type, HOST_STATE_MANAGER_ROOT, &busname);
    if (rc < 0)
    {
        log<level::ERR>("Failed to get bus name",
                        entry("ERROR=%s, OBJPATH=%s",
                              strerror(-rc), HOST_STATE_MANAGER_ROOT));
        return rc;
    }

    // Convert to string equivalent of the passed in transition enum.
    auto request = State::convertForMessage(transition);

    rc = sd_bus_call_method(bus_type,                // On the system bus
                            busname,                 // Service to contact
                            HOST_STATE_MANAGER_ROOT, // Object path
                            DBUS_PROPERTY_IFACE,     // Interface name
                            "Set",                   // Method to be called
                            &bus_error,              // object to return error
                            nullptr,                 // Response buffer if any
                            "ssv",                   // Takes 3 arguments
                            HOST_STATE_MANAGER_IFACE,
                            PROPERTY,
                            "s", request.c_str());
    if(rc < 0)
    {
        log<level::ERR>("Failed to initiate transition",
                        entry("ERROR=%s, REQUEST=%s",
                              bus_error.message, request.c_str()));
    }
    else
    {
        log<level::INFO>("Transition request initiated successfully");
    }

    sd_bus_error_free(&bus_error);
    free(busname);

    return rc;
}

struct hostPowerPolicyTypeMap_t
{
    uint8_t policyNum;
    char    policyName[19];
};

hostPowerPolicyTypeMap_t g_hostPowerPolicyTypeMap_t[] = {

        {0x00, "LEAVE_OFF"},
        {0x01, "RESTORE_LAST_STATE"},
        {0x02, "ALWAYS_POWER_ON"},
        {0x03, "UNKNOWN"}
};

uint8_t get_host_power_policy(char *p)
{

    hostPowerPolicyTypeMap_t *s = g_hostPowerPolicyTypeMap_t;

    while (s->policyNum != 0x03) {
        if (!strcmp(s->policyName,p))
            break;
        s++;
    }

    return s->policyNum;
}

//----------------------------------------------------------------------
// Get Chassis Status commands
//----------------------------------------------------------------------
ipmi_ret_t ipmi_get_chassis_status(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                                   ipmi_request_t request,
                                   ipmi_response_t response,
                                   ipmi_data_len_t data_len,
                                   ipmi_context_t context)
{
    const char  *objname = "/org/openbmc/control/power0";
    const char  *intf = "org.openbmc.control.Power";

    sd_bus *bus = NULL;
    sd_bus_message *reply = NULL;
    int r = 0;
    int pgood = 0;
    char *busname = NULL;
    ipmi_ret_t rc = IPMI_CC_OK;
    ipmi_get_chassis_status_t chassis_status{};

    char *p = NULL;
    uint8_t s = 0;

    // Get the system bus where most system services are provided.
    bus = ipmid_get_sd_bus_connection();

    *data_len = 4;

    r = mapper_get_service(bus, objname, &busname);
    if (r < 0) {
        fprintf(stderr, "Failed to get bus name, return value: %s.\n", strerror(-r));
        rc = IPMI_CC_UNSPECIFIED_ERROR;
        goto finish;
    }

    r = sd_bus_get_property(bus, busname, objname, intf, "pgood", NULL, &reply, "i");
    if (r < 0) {
        fprintf(stderr, "Failed to call sd_bus_get_property:%d,  %s\n", r, strerror(-r));
        fprintf(stderr, "Bus: %s, Path: %s, Interface: %s\n",
                busname, objname, intf);
        rc = IPMI_CC_UNSPECIFIED_ERROR;
        goto finish;
    }

    r = sd_bus_message_read(reply, "i", &pgood);
    if (r < 0) {
        fprintf(stderr, "Failed to read sensor: %s\n", strerror(-r));
        rc = IPMI_CC_UNSPECIFIED_ERROR;
        goto finish;
    }

    printf("pgood is 0x%02x\n", pgood);

    // Get Power Policy
    r = dbus_get_property("power_policy",&p);

    if (r < 0) {
        fprintf(stderr, "Dbus get property(power_policy) failed for get_sys_boot_options.\n");
        rc = IPMI_CC_UNSPECIFIED_ERROR;
    } else {
        s = get_host_power_policy(p);
    }

    if (p)
    {
        free(p);
        p = NULL;
    }

    // Current Power State
    // [7] reserved
    // [6..5] power restore policy
    //          00b = chassis stays powered off after AC/mains returns
    //          01b = after AC returns, power is restored to the state that was
    //          in effect when AC/mains was lost.
    //          10b = chassis always powers up after AC/mains returns
    //          11b = unknow
    //        Set to 00b, by observing the hardware behavior.
    //        Do we need to define a dbus property to identify the restore policy?

    // [4] power control fault
    //       1b = controller attempted to turn system power on or off, but
    //       system did not enter desired state.
    //       Set to 0b, since We don't support it..

    // [3] power fault
    //       1b = fault detected in main power subsystem.
    //       set to 0b. for we don't support it.

    // [2] 1b = interlock (chassis is presently shut down because a chassis
    //       panel interlock switch is active). (IPMI 1.5)
    //       set to 0b,  for we don't support it.

    // [1] power overload
    //      1b = system shutdown because of power overload condition.
    //       set to 0b,  for we don't support it.

    // [0] power is on
    //       1b = system power is on
    //       0b = system power is off(soft-off S4/S5, or mechanical off)

    chassis_status.cur_power_state = ((s & 0x3)<<5) | (pgood & 0x1);

    // Last Power Event
    // [7..5] – reserved
    // [4] – 1b = last ‘Power is on’ state was entered via IPMI command
    // [3] – 1b = last power down caused by power fault
    // [2] – 1b = last power down caused by a power interlock being activated
    // [1] – 1b = last power down caused by a Power overload
    // [0] – 1b = AC failed
    // set to 0x0,  for we don't support these fields.

    chassis_status.last_power_event = 0;

    // Misc. Chassis State
    // [7] – reserved
    // [6] – 1b = Chassis Identify command and state info supported (Optional)
    //       0b = Chassis Identify command support unspecified via this command.
    //       (The Get Command Support command , if implemented, would still
    //       indicate support for the Chassis Identify command)
    // [5..4] – Chassis Identify State. Mandatory when bit[6] =1b, reserved (return
    //          as 00b) otherwise. Returns the present chassis identify state.
    //           Refer to the Chassis Identify command for more info.
    //         00b = chassis identify state = Off
    //         01b = chassis identify state = Temporary(timed) On
    //         10b = chassis identify state = Indefinite On
    //         11b = reserved
    // [3] – 1b = Cooling/fan fault detected
    // [2] – 1b = Drive Fault
    // [1] – 1b = Front Panel Lockout active (power off and reset via chassis
    //       push-buttons disabled.)
    // [0] – 1b = Chassis Intrusion active
    //  set to 0,  for we don't support them.
    chassis_status.misc_power_state = 0;

    //  Front Panel Button Capabilities and disable/enable status(Optional)
    //  set to 0,  for we don't support them.
    chassis_status.front_panel_button_cap_status = 0;

    // Pack the actual response
    memcpy(response, &chassis_status, *data_len);

finish:
    free(busname);
    reply = sd_bus_message_unref(reply);

    return rc;
}

//-------------------------------------------------------------
// Send a command to SoftPowerOff application to stop any timer
//-------------------------------------------------------------
int stop_soft_off_timer()
{
    constexpr auto iface            = "org.freedesktop.DBus.Properties";
    constexpr auto soft_off_iface   = "xyz.openbmc_project.Ipmi.Internal."
            "SoftPowerOff";

    constexpr auto property         = "ResponseReceived";
    constexpr auto value            = "xyz.openbmc_project.Ipmi.Internal."
            "SoftPowerOff.HostResponse.HostShutdown";

    // Get the system bus where most system services are provided.
    auto bus = ipmid_get_sd_bus_connection();

    // Get the service name
    // TODO openbmc/openbmc#1661 - Mapper refactor
    //
    // See openbmc/openbmc#1743 for some details but high level summary is that
    // for now the code will directly call the soft off interface due to a
    // race condition with mapper usage
    //
    //char *busname = nullptr;
    //auto r = mapper_get_service(bus, SOFTOFF_OBJPATH, &busname);
    //if (r < 0)
    //{
    //    fprintf(stderr, "Failed to get %s bus name: %s\n",
    //            SOFTOFF_OBJPATH, strerror(-r));
    //    return r;
    //}

    // No error object or reply expected.
    int rc = sd_bus_call_method(bus, SOFTOFF_BUSNAME, SOFTOFF_OBJPATH, iface,
                                "Set", nullptr, nullptr, "ssv",
                                soft_off_iface, property, "s", value);
    if (rc < 0)
    {
        fprintf(stderr, "Failed to set property in SoftPowerOff object: %s\n",
                strerror(-rc));
    }

    //TODO openbmc/openbmc#1661 - Mapper refactor
    //free(busname);
    return rc;
}

//----------------------------------------------------------------------
// Create file to indicate there is no need for softoff notification to host
//----------------------------------------------------------------------
void indicate_no_softoff_needed()
{
    fs::path path{HOST_INBAND_REQUEST_DIR};
    if (!fs::is_directory(path))
    {
        fs::create_directory(path);
    }

    // Add the host instance (default 0 for now) to the file name
    std::string file{HOST_INBAND_REQUEST_FILE};
    auto size = std::snprintf(nullptr,0,file.c_str(),0);
    size++; // null
    std::unique_ptr<char[]> buf(new char[size]);
    std::snprintf(buf.get(),size,file.c_str(),0);

    // Append file name to directory and create it
    path /= buf.get();
    std::ofstream(path.c_str());
}

//----------------------------------------------------------------------
// Chassis Control commands
//----------------------------------------------------------------------
ipmi_ret_t ipmi_chassis_control(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                                ipmi_request_t request,
                                ipmi_response_t response,
                                ipmi_data_len_t data_len,
                                ipmi_context_t context)
{
    // Error from power off.
    int rc = 0;

    // No response for this command.
    *data_len = 0;

    // Catch the actual operaton by peeking into request buffer
    uint8_t chassis_ctrl_cmd = *(uint8_t *)request;
    printf("Chassis Control Command: Operation:[0x%X]\n",chassis_ctrl_cmd);

    switch(chassis_ctrl_cmd)
    {
        case CMD_POWER_ON:
            rc = initiate_state_transition(State::Host::Transition::On);
            break;
        case CMD_POWER_OFF:
            // Need to Nudge SoftPowerOff application that it needs to stop the
            // watchdog timer if running.
            rc = stop_soft_off_timer();
            // Only request the Off transition if the soft power off
            // application is not running
            if (rc < 0)
            {
                log<level::INFO>("Did not find soft off service so request "
                                 "Host:Transition:Off");

                // First create a file to indicate to the soft off application
                // that it should not run since this is a direct user initiated
                // power off request (i.e. a power off request that is not
                // originating via a soft power off SMS request)
                indicate_no_softoff_needed();

                // Now request the shutdown
                rc = initiate_state_transition(State::Host::Transition::Off);
            }
            else
            {
                log<level::INFO>("Soft off is running, so let that stop "
                                 "the host");
            }

            break;

        case CMD_HARD_RESET:
        case CMD_POWER_CYCLE:
            // SPEC has a section that says certain implementations can trigger
            // PowerOn if power is Off when a command to power cycle is
            // requested

            // First create a file to indicate to the soft off application
            // that it should not run since this is a direct user initiated
            // power reboot request (i.e. a reboot request that is not
            // originating via a soft power off SMS request)
            indicate_no_softoff_needed();

            rc = initiate_state_transition(State::Host::Transition::Reboot);
            break;
        default:
        {
            fprintf(stderr, "Invalid Chassis Control command:[0x%X] received\n",chassis_ctrl_cmd);
            rc = -1;
        }
    }

    return ( (rc < 0) ? IPMI_CC_INVALID : IPMI_CC_OK);
}

struct bootOptionTypeMap_t {
    uint8_t ipmibootflag;
    char    dbusname[8];
};

#define INVALID_STRING "Invalid"
// dbus supports this list of boot devices.
bootOptionTypeMap_t g_bootOptionTypeMap_t[] = {

        {0x01, "Network"},
        {0x02, "Disk"},
        {0x03, "Safe"},
        {0x05, "CDROM"},
        {0x06, "Setup"},
        {0x00, "Default"},
        {0xFF, INVALID_STRING}
};

uint8_t get_ipmi_boot_option(char *p) {

    bootOptionTypeMap_t *s = g_bootOptionTypeMap_t;

    while (s->ipmibootflag != 0xFF) {
        if (!strcmp(s->dbusname,p))
            break;
        s++;
    }

    if (!s->ipmibootflag)
        printf("Failed to find Sensor Type %s\n", p);

    return s->ipmibootflag;
}

char* get_boot_option_by_ipmi(uint8_t p) {

    bootOptionTypeMap_t *s = g_bootOptionTypeMap_t;

    while (s->ipmibootflag != 0xFF) {

        if (s->ipmibootflag == p)
            break;

        s++;
    }


    if (!s->ipmibootflag)
        printf("Failed to find Sensor Type 0x%x\n", p);

    return s->dbusname;
}

ipmi_ret_t ipmi_chassis_get_sys_boot_options(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                                             ipmi_request_t request,
                                             ipmi_response_t response,
                                             ipmi_data_len_t data_len,
                                             ipmi_context_t context)
{
    ipmi_ret_t rc = IPMI_CC_PARM_NOT_SUPPORTED;
    char *p = NULL;
    get_sys_boot_options_response_t *resp = (get_sys_boot_options_response_t *) response;
    get_sys_boot_options_t *reqptr = (get_sys_boot_options_t*) request;
    uint8_t s;

    printf("IPMI GET_SYS_BOOT_OPTIONS\n");

    memset(resp,0,sizeof(*resp));
    resp->version   = SET_PARM_VERSION;
    resp->parm      = 5;
    resp->data[0]   = SET_PARM_BOOT_FLAGS_VALID_ONE_TIME;


    /*
     * Parameter #5 means boot flags. Please refer to 28.13 of ipmi doc.
     * This is the only parameter used by petitboot.
     */
    if ( reqptr->parameter == static_cast<uint8_t>
    ( BootOptionParameter::BOOT_FLAGS )) {

        *data_len = static_cast<uint8_t>(BootOptionResponseSize::BOOT_FLAGS);
        /* Get the boot device */
        int r = dbus_get_property("boot_flags",&p);

        if (r < 0) {
            fprintf(stderr, "Dbus get property(boot_flags) failed for get_sys_boot_options.\n");
            rc = IPMI_CC_UNSPECIFIED_ERROR;

        } else {

            s = get_ipmi_boot_option(p);
            resp->data[1] = (s << 2);
            rc = IPMI_CC_OK;

        }

        if (p)
        {
            free(p);
            p = NULL;
        }

        /* Get the boot policy */
        r = dbus_get_property("boot_policy",&p);

        if (r < 0) {
            fprintf(stderr, "Dbus get property(boot_policy) failed for get_sys_boot_options.\n");
            rc = IPMI_CC_UNSPECIFIED_ERROR;

        } else {

            printf("BootPolicy is [%s]\n", p);
            resp->data[0] = (strncmp(p,"ONETIME",strlen("ONETIME"))==0) ?
                    SET_PARM_BOOT_FLAGS_VALID_ONE_TIME:
                    SET_PARM_BOOT_FLAGS_VALID_PERMANENT;
            rc = IPMI_CC_OK;

        }


    } else if ( reqptr->parameter == static_cast<uint8_t>
    ( BootOptionParameter::OPAL_NETWORK_SETTINGS )) {

        *data_len = static_cast<uint8_t>(BootOptionResponseSize::OPAL_NETWORK_SETTINGS);

        resp->parm = static_cast<uint8_t>(BootOptionParameter::OPAL_NETWORK_SETTINGS);

        int ret = getHostNetworkData(resp);

        if (ret < 0) {

            fprintf(stderr, "getHostNetworkData failed for get_sys_boot_options.\n");
            rc = IPMI_CC_UNSPECIFIED_ERROR;

        }else
            rc = IPMI_CC_OK;
    }

    else {
        fprintf(stderr, "Unsupported parameter 0x%x\n", reqptr->parameter);
    }

    if (p)
        free(p);

    if (rc == IPMI_CC_OK)
    {
        *data_len += 2;
    }

    return rc;
}



ipmi_ret_t ipmi_chassis_set_sys_boot_options(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                                             ipmi_request_t request,
                                             ipmi_response_t response,
                                             ipmi_data_len_t data_len,
                                             ipmi_context_t context)
{
    ipmi_ret_t rc = IPMI_CC_OK;
    char *s;
    set_sys_boot_options_t *reqptr = (set_sys_boot_options_t *) request;

    printf("IPMI SET_SYS_BOOT_OPTIONS reqptr->parameter =[%d]\n",reqptr->parameter);

    // This IPMI command does not have any resposne data
    *data_len = 0;

    /*  000101
     * Parameter #5 means boot flags. Please refer to 28.13 of ipmi doc.
     * This is the only parameter used by petitboot.
     */

    if (reqptr->parameter == (uint8_t)BootOptionParameter::BOOT_FLAGS) {

        s = get_boot_option_by_ipmi(((reqptr->data[1] & 0x3C) >> 2));

        printf("%d: %s\n", __LINE__, s);
        if (!strcmp(s,INVALID_STRING)) {

            rc = IPMI_CC_PARM_NOT_SUPPORTED;

        } else {

            int r = dbus_set_property("boot_flags",s);

            if (r < 0) {
                fprintf(stderr, "Dbus set property(boot_flags) failed for set_sys_boot_options.\n");
                rc = IPMI_CC_UNSPECIFIED_ERROR;
            }
        }

        /* setting the boot policy */
        s = (char *)(((reqptr->data[0] & SET_PARM_BOOT_FLAGS_PERMANENT) ==
                SET_PARM_BOOT_FLAGS_PERMANENT) ?"PERMANENT":"ONETIME");

        printf ( "\nBoot Policy is %s",s);
        int r = dbus_set_property("boot_policy",s);

        if (r < 0) {
            fprintf(stderr, "Dbus set property(boot_policy) failed for set_sys_boot_options.\n");
            rc = IPMI_CC_UNSPECIFIED_ERROR;
        }

    } else if (reqptr->parameter ==
            (uint8_t)BootOptionParameter::OPAL_NETWORK_SETTINGS) {

        int ret = setHostNetworkData(reqptr);
        if (ret < 0) {
            fprintf(stderr, "setHostNetworkData failed for set_sys_boot_options.\n");
            rc = IPMI_CC_UNSPECIFIED_ERROR;
        }
    }
    else {
        fprintf(stderr, "Unsupported parameter 0x%x\n", reqptr->parameter);
        rc = IPMI_CC_PARM_NOT_SUPPORTED;
    }

    return rc;
}

void register_netfn_chassis_functions()
{
    // <Wildcard Command>
    printf("Registering NetFn:[0x%X], Cmd:[0x%X]\n",NETFUN_CHASSIS, IPMI_CMD_WILDCARD);
    ipmi_register_callback(NETFUN_CHASSIS, IPMI_CMD_WILDCARD, NULL, ipmi_chassis_wildcard,
                           PRIVILEGE_USER);

    // Get Chassis Capabilities
    printf("Registering NetFn:[0x%X], Cmd:[0x%X]\n",NETFUN_CHASSIS, IPMI_CMD_GET_CHASSIS_CAP);
    ipmi_register_callback(NETFUN_CHASSIS, IPMI_CMD_GET_CHASSIS_CAP, NULL, ipmi_get_chassis_cap,
                           PRIVILEGE_USER);

    // <Get System Boot Options>
    printf("Registering NetFn:[0x%X], Cmd:[0x%X]\n",NETFUN_CHASSIS, IPMI_CMD_GET_SYS_BOOT_OPTIONS);
    ipmi_register_callback(NETFUN_CHASSIS, IPMI_CMD_GET_SYS_BOOT_OPTIONS, NULL,
                           ipmi_chassis_get_sys_boot_options, PRIVILEGE_OPERATOR);

    // <Get Chassis Status>
    printf("Registering NetFn:[0x%X], Cmd:[0x%X]\n",NETFUN_CHASSIS, IPMI_CMD_CHASSIS_STATUS);
    ipmi_register_callback(NETFUN_CHASSIS, IPMI_CMD_CHASSIS_STATUS, NULL, ipmi_get_chassis_status,
                           PRIVILEGE_USER);

    // <Chassis Control>
    printf("Registering NetFn:[0x%X], Cmd:[0x%X]\n",NETFUN_CHASSIS, IPMI_CMD_CHASSIS_CONTROL);
    ipmi_register_callback(NETFUN_CHASSIS, IPMI_CMD_CHASSIS_CONTROL, NULL, ipmi_chassis_control,
                           PRIVILEGE_OPERATOR);

    // <Set System Boot Options>
    printf("Registering NetFn:[0x%X], Cmd:[0x%X]\n", NETFUN_CHASSIS, IPMI_CMD_SET_SYS_BOOT_OPTIONS);
    ipmi_register_callback(NETFUN_CHASSIS, IPMI_CMD_SET_SYS_BOOT_OPTIONS, NULL,
                           ipmi_chassis_set_sys_boot_options, PRIVILEGE_OPERATOR);
}
