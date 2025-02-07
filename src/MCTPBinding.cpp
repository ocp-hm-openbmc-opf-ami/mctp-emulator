#include "MCTPBinding.hpp"

#include <endian.h>

#include <boost/asio/io_service.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus.hpp>
#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/MCTP/Base/server.hpp>
#include <xyz/openbmc_project/MCTP/Endpoint/server.hpp>
#include <xyz/openbmc_project/MCTP/SupportedMessageTypes/server.hpp>

#include "libmctp-msgtypes.h"
#include "libmctp-vdpci.h"

using json = nlohmann::json;
using mctp_base = sdbusplus::xyz::openbmc_project::MCTP::server::Base;
using mctp_endpoint = sdbusplus::xyz::openbmc_project::MCTP::server::Endpoint;
using mctp_msg_types =
    sdbusplus::xyz::openbmc_project::MCTP::server::SupportedMessageTypes;

std::unordered_map<std::string, mctp_base::BindingModeTypes>
    stringToBindingModeMap = {
        {"busowner", mctp_base::BindingModeTypes::BusOwner},
        {"endpoint", mctp_base::BindingModeTypes::Endpoint}};

std::string epReqRespFile = "/usr/share/mctp-emulator/req_resp_";

std::string hotSwappableDataFile =
    "/usr/share/mctp-emulator/hot_swappable_endpoints.json";
std::string uuidIntf = "xyz.openbmc_project.Common.UUID";

std::string pciVdMsgIntf = "xyz.openbmc_project.MCTP.PCIVendorDefined";
std::string mctpDevObj = "/xyz/openbmc_project/mctp/device/";
std::string mctpBaseObj = "/xyz/openbmc_project/mctp";

constexpr const std::string_view addIface = "AdditionalInterfaces";

EndpointInterfaceMap endpointInterface;

static std::string uuid;
std::string uuidCommonIntf = "xyz.openbmc_project.Common.UUID";
constexpr sd_id128_t mctpdAppId = SD_ID128_MAKE(29, 1f, 30, a7, 33, dd, 4c, 25,
                                                8e, 89, 24, 05, b1, 67, be, 87);

std::shared_ptr<sdbusplus::asio::dbus_interface> endpointIntf;
std::shared_ptr<sdbusplus::asio::dbus_interface> mctpInterface;
std::string mctpIntf = "xyz.openbmc_project.MCTP.Base";
bool timerExpired = true;

static std::unique_ptr<boost::asio::steady_timer> delayTimer;
static std::vector<std::pair<
    int, std::tuple<uint8_t, uint8_t, uint8_t, bool, std::vector<uint8_t>>>>
    respQueue;

constexpr int retryTimeMilliSec = 10;

void MctpBinding::addEndpoints(std::string file, std::optional<uint8_t> destId)
{
    std::ifstream jsonFile(file);
    if (!jsonFile.good())
    {
        std::cerr << "unable to open " << file << "\n";
    }
    json endpoints = nullptr;
    uint8_t dstEid;
    std::string dstUuid;
    mctp_base::BindingModeTypes mode;
    uint16_t networkId;
    json msgType;
    bool mctpControl;
    bool pldm;
    bool ncsi;
    bool ethernet;
    bool nvmeMgmtMsg;
    bool spdm;
    bool securedMsg;
    bool cxlFmApi;
    bool cxlCci;
    bool vdpci;
    bool vdiana;
    std::string vendorID = "0x8086";
    std::vector<uint16_t> msgTypeProperty;
    try
    {
        endpoints = json::parse(jsonFile, nullptr, false);
    }
    catch (json::exception& e)
    {
        std::cerr << "Error parsing " << file << "\n"
                  << "message: " << e.what() << '\n'
                  << "exception id: " << e.id << std::endl;
        return;
    }
    // Create an interface for each of the endpoints parsed in given json
    // file
    auto enpointObjManager =
        std::make_shared<sdbusplus::server::manager::manager>(
            *bus, mctpBaseObj.c_str());

    for (auto iter : endpoints["Endpoints"])
    {
        std::string mctpEpObj;
        // If destId has been provided, add data for just that endpoint.
        // Otherwise add all endpoints in json file
        if (iter["Eid"] == destId || !destId.has_value())
        {
            try
            {
                dstEid = iter["Eid"];
                mctpEpObj = mctpDevObj + std::to_string(dstEid);
                dstUuid = iter["Uuid"];
                mode = stringToBindingModeMap.at(iter["Mode"]);
                networkId = iter["NetworkId"];
                msgType = iter["SupportedMessageTypes"];
                mctpControl = msgType["MctpControl"];
                pldm = msgType["PLDM"];
                ncsi = msgType["NCSI"];
                ethernet = msgType["Ethernet"];
                nvmeMgmtMsg = msgType["NVMeMgmtMsg"];
                spdm = msgType["SPDM"];
                securedMsg = msgType["SECUREDMSG"];
                cxlFmApi = msgType["CXLFMAPI"];
                cxlCci = msgType["CXLCCI"];
                vdpci = msgType["VDPCI"];
                vdiana = msgType["VDIANA"];
                if (vdpci == true)
                {
                    json vdpcimt = iter["VDPCIMT"];
                    msgTypeProperty = vdpcimt.at("CapabilitySets")
                                          .get<std::vector<uint16_t>>();
                }

                if (iter.contains(addIface))
                {
                    // Additional interfaces are to be populated on the endpoint
                    // object
                    for (auto& it : iter[std::string(addIface).c_str()].items())
                    {
                        for (auto& ifaceList : it.value().items())
                        {
                            auto epIntf = objectServer->add_interface(
                                mctpEpObj, ifaceList.key());

                            for (auto& propertyList : ifaceList.value().items())
                            {
                                std::string propertyName(propertyList.key());
                                // TODO: Property value could be of any type,
                                // deduce the property type using type() API
                                // uint8_t should suffice for now
                                uint8_t propertyValue = propertyList.value();
                                epIntf->register_property(propertyName,
                                                          propertyValue);
                            }
                            epIntf->initialize();
                        }
                    }
                }
            }
            catch (json::exception& e)
            {
                std::cerr << "message: " << e.what() << '\n'
                          << "exception id: " << e.id << std::endl;
                continue;
            }
            catch (std::out_of_range& e)
            {
                std::cerr << "message: " << e.what() << std::endl;
                continue;
            }
            std::shared_ptr<sdbusplus::asio::dbus_interface> epIntf;
            std::shared_ptr<sdbusplus::asio::dbus_interface> msgTypeIntf;
            std::shared_ptr<sdbusplus::asio::dbus_interface> vendorDefMsgIntf;
            std::shared_ptr<sdbusplus::asio::dbus_interface> uuidEndPointIntf;

            epIntf = objectServer->add_interface(mctpEpObj,
                                                 mctp_endpoint::interface);
            epIntf->register_property(
                "Mode", mctp_base::convertBindingModeTypesToString(mode));
            epIntf->register_property("NetworkId", networkId);
            epIntf->initialize();
            endpointInterface.emplace(dstEid, epIntf);

            msgTypeIntf = objectServer->add_interface(
                mctpEpObj, mctp_msg_types::interface);
            msgTypeIntf->register_property("MctpControl", mctpControl);
            msgTypeIntf->register_property("PLDM", pldm);
            msgTypeIntf->register_property("NCSI", ncsi);
            msgTypeIntf->register_property("Ethernet", ethernet);
            msgTypeIntf->register_property("NVMeMgmtMsg", nvmeMgmtMsg);
            msgTypeIntf->register_property("SPDM", spdm);
            msgTypeIntf->register_property("SECUREDMSG", securedMsg);
            msgTypeIntf->register_property("CXLFMAPI", cxlFmApi);
            msgTypeIntf->register_property("CXLCCI", cxlCci);
            msgTypeIntf->register_property("VDPCI", vdpci);
            msgTypeIntf->register_property("VDIANA", vdiana);
            msgTypeIntf->initialize();
            msgInterfaces.emplace(dstEid, msgTypeIntf);

            if (vdpci == true)
            {
                vendorDefMsgIntf =
                    objectServer->add_interface(mctpEpObj, pciVdMsgIntf);
                vendorDefMsgIntf->register_property("VendorID", vendorID);
                vendorDefMsgIntf->register_property("MessageTypeProperty",
                                                    msgTypeProperty);
                vendorDefMsgIntf->initialize();
                vendorInterfaces.emplace(dstEid, vendorDefMsgIntf);
            }

            uuidEndPointIntf = objectServer->add_interface(mctpEpObj, uuidIntf);
            uuidEndPointIntf->register_property("UUID", dstUuid);
            uuidEndPointIntf->initialize();
            uuidInterfaces.emplace(dstEid, uuidEndPointIntf);

            phosphor::logging::log<phosphor::logging::level::INFO>(
                ("mctp-emulator: Added Endpoint " + std::to_string(dstEid))
                    .c_str());
        }
    }
}

void MctpBinding::getSystemAppUuid(void)
{
    sd_id128_t id;
    char tempStr[33];
    std::array<uint8_t, 4> separatorOffset = {8, 13, 18, 23};

    if (sd_id128_get_machine_app_specific(mctpdAppId, &id))
    {
        throw std::system_error(
            std::make_error_code(std::errc::address_not_available));
    }

    uuid = sd_id128_to_string(id, tempStr);

    for (auto& offset : separatorOffset)
    {
        uuid.insert(offset, "-");
    }
}

bool MctpBinding::removeInterface(mctp_eid_t dstEid,
                                  EndpointInterfaceMap& interfaces)
{
    auto iter = interfaces.find(dstEid);
    if (iter != interfaces.end())
    {
        objectServer->remove_interface(iter->second);
        interfaces.erase(iter);
        return true;
    }
    return false;
}

static void sendMessageReceivedSignal(uint8_t msgType, uint8_t srcEid,
                                      uint8_t msgTag, bool tagOwner,
                                      std::vector<uint8_t> response)
{
    auto msgSignal = bus->new_signal("/xyz/openbmc_project/mctp",
                                     mctpIntf.c_str(), "MessageReceivedSignal");
    msgSignal.append(msgType, srcEid, msgTag, tagOwner, response);
    msgSignal.signal_send();
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "Response signal sent");
}

static std::string getMessageType(uint8_t msgType)
{
    // TODO: Support for OEM message types
    std::string msgTypeValue = "Unknown";
    switch (msgType)
    {
        case MCTP_MESSAGE_TYPE_MCTP_CTRL: // 0x00
            msgTypeValue = "MctpControl";
            break;
        case MCTP_MESSAGE_TYPE_PLDM: // 0x01
            msgTypeValue = "PLDM";
            break;
        case MCTP_MESSAGE_TYPE_NCSI: // 0x02
            msgTypeValue = "NCSI";
            break;
        case MCTP_MESSAGE_TYPE_ETHERNET: // 0x03
            msgTypeValue = "Ethernet";
            break;
        case MCTP_MESSAGE_TYPE_NVME: // 0x04
            msgTypeValue = "NVMeMgmtMsg";
            break;
        case MCTP_MESSAGE_TYPE_SPDM: // 0x05
            msgTypeValue = "SPDM";
            break;
        case MCTP_MESSAGE_TYPE_SECUREDMSG: // 0x06
            msgTypeValue = "SECUREDMSG";
            break;
        case MCTP_MESSAGE_TYPE_CXL_FM_API: // 0x07
            msgTypeValue = "CXLFMAPI";
            break;
        case MCTP_MESSAGE_TYPE_CXL_CCI: // 0x08
            msgTypeValue = "CXLCCI";
            break;
        case MCTP_MESSAGE_TYPE_VDPCI: // 0x7E
            msgTypeValue = "VDPCI";
            break;
        case MCTP_MESSAGE_TYPE_VDIANA: // 0x7F
            msgTypeValue = "VDIANA";
            break;
    }
    phosphor::logging::log<phosphor::logging::level::INFO>(
        ("Message Type: " + msgTypeValue).c_str());
    return msgTypeValue;
}

void processResponse()
{
    timerExpired = false;
    delayTimer->expires_after(std::chrono::milliseconds(retryTimeMilliSec));
    delayTimer->async_wait([](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            // timer aborted do nothing
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "delayTimer operation_aborted");
            return;
        }
        else if (ec)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Timer failed");
            return;
        }

        respQueue.erase(
            std::remove_if(respQueue.begin(), respQueue.end(),
                           [](auto& resp) {
                               if (resp.first > 0)
                               {
                                   return false;
                               }

                               uint8_t msgType;
                               uint8_t srcEid;
                               uint8_t msgTag;
                               bool tagOwner;
                               std::vector<uint8_t> response;
                               std::tie(msgType, srcEid, msgTag, tagOwner,
                                        response) = resp.second;
                               sendMessageReceivedSignal(
                                   msgType, srcEid, msgTag, tagOwner, response);
                               return true;
                           }),
            respQueue.end());

        if (respQueue.empty())
        {
            delayTimer->cancel();
            timerExpired = true;
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "Response queue empty, canceling timer");
        }
        else
        {
            std::for_each(respQueue.begin(), respQueue.end(),
                          [](auto& resp) { resp.first -= retryTimeMilliSec; });
            processResponse();
        }
    });
}

static std::unordered_map<uint16_t, std::string> vendorMap = {
    {0x8086, "Intel"},
};

static void createAsyncDelay(boost::asio::yield_context& yield,
                             const uint16_t timeout)
{
    boost::asio::steady_timer timer(bus->get_io_context());
    boost::system::error_code ec;

    timer.expires_after(std::chrono::milliseconds(timeout));
    timer.async_wait(yield[ec]);
}

static bool handleAtomicResponseTimeout(boost::asio::yield_context& yield,
                                        const int processingDelay,
                                        const uint16_t timeout)
{
    uint16_t uProcessingDelay = static_cast<uint16_t>(processingDelay);

    if (uProcessingDelay < timeout && processingDelay > 0)
    {
        createAsyncDelay(yield, uProcessingDelay);
        return true;
    }

    else
    {
        createAsyncDelay(yield, timeout);
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "mctp-emulator: No response");
        return false;
    }
}

static void createResponseSignal(int processingDelay, const uint8_t srcEid,
                                 const uint8_t msgType, const bool tagOwner,
                                 const uint8_t msgTag,
                                 std::vector<uint8_t>& response)
{
    if (processingDelay < 0)
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "mctp-emulator: No response, Infinite delay");
        return;
    }

    else if (processingDelay == 0)
    {
        sendMessageReceivedSignal(msgType, srcEid, msgTag, tagOwner, response);
    }

    else
    {
        respQueue.push_back(std::make_pair(
            processingDelay,
            std::make_tuple(msgType, srcEid, msgTag, tagOwner, response)));
        if (timerExpired)
        {
            processResponse();
        }
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "mctp-emulator: Response added to process queue");
    }
}

std::optional<std::pair<int, std::vector<uint8_t>>>
    processPayload(std::ifstream& jfile, bool validEid,
                   const std::vector<uint8_t>& payload)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "processPayload called...");
    std::string messageType;
    uint8_t msgType;
    json reqResp = nullptr;
    json reqRespData = nullptr;

    reqResp = json::parse(jfile, nullptr, false);
    // process the MCTP command only oif the above validation are successful
    if (validEid)
    {
        msgType = payload.at(0);
        try
        {
            messageType = getMessageType(msgType);
            reqRespData = reqResp[messageType];
        }
        catch (json::exception& e)
        {
            std::cerr << "message: " << e.what() << '\n'
                      << "exception id: " << e.id << std::endl;
            return std::nullopt;
        }

        std::vector<uint8_t> reqHeader;
        if (messageType == "VDPCI")
        {
            if (payload.size() < sizeof(mctp_vdpci_intel_hdr))
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "mctp-emulator: Invalid VDPCI message: Insufficient bytes "
                    "in "
                    "Payload");
                return std::nullopt;
            }

            const auto* vdpciMessage =
                reinterpret_cast<const mctp_vdpci_intel_hdr*>(payload.data());
            auto vendorIter =
                vendorMap.find(be16toh(vdpciMessage->vdpci_hdr.vendor_id));
            if (vendorIter == vendorMap.end())
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "mctp-emulator: Invalid VDPCI message: Unknown Vendor ID");
                return std::nullopt;
            }

            const auto& vendorString = vendorIter->second;
            if (vendorString == "Intel" && !(vdpciMessage->reserved >= 0x80 &&
                                             vdpciMessage->reserved <= 0xb1))
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "mctp-emulator: Invalid VDPCI message: Unexpected value in "
                    "reserved byte");
                return std::nullopt;
            }
            try
            {
                auto vendorTypeCode =
                    std::to_string(vdpciMessage->vendor_type_code);
                reqRespData = reqRespData[vendorString][vendorTypeCode];
            }
            catch (json::exception& e)
            {
                std::cerr << "message: " << e.what() << '\n'
                          << "exception id: " << e.id << std::endl;
                return std::nullopt;
            }
            reqHeader.insert(reqHeader.end(), payload.begin(),
                             payload.begin() + sizeof(mctp_vdpci_intel_hdr));
        }
        else if (messageType == "PLDM")
        {
            // MCTPMsgType | rqDInstanceID | PLDMType | PLDMCmd
            constexpr size_t minPldmReqSize = 4;
            if (payload.size() < minPldmReqSize)
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "mctp-emulator: Invalid PLDM message: Insufficient bytes "
                    "in "
                    "Payload");
                return std::nullopt;
            }
            uint8_t rqDInstanceID = payload.at(1);

            reqHeader.push_back(msgType);
            reqHeader.push_back(rqDInstanceID);
        }
        else if (messageType == "SECUREDMSG")
        {
            // MCTPMsgType | SessionId | SPDMVersion | RequestResponseCode
            constexpr size_t minSpdmReqSize = 4;
            if (payload.size() < minSpdmReqSize)
            {
                phosphor::logging::log<phosphor::logging::level::WARNING>(
                    "mctp-emulator: Invalid SPDM message: Insufficient bytes "
                    "in "
                    "Payload");
                return std::nullopt;
            }
            uint8_t rqDSessionID = payload.at(1);

            reqHeader.push_back(msgType);
            reqHeader.push_back(rqDSessionID);
        }
        else
        {
            reqHeader.push_back(msgType);
        }

        for (auto iter : reqRespData)
        {
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "mctp-emulator: Parsing commands..");

            std::vector<uint8_t> req = {};
            try
            {
                req.assign(std::begin(iter["request"]),
                           std::end(iter["request"]));
            }
            catch (json::exception& e)
            {
                std::cerr << "message: " << e.what() << '\n'
                          << "exception id: " << e.id << std::endl;
                continue;
            }

            req.insert(req.begin(), reqHeader.begin(), reqHeader.end());
            if (req == payload)
            {
                int processingDelayMilliSec = 0;
                std::vector<uint8_t> response = {};
                phosphor::logging::log<phosphor::logging::level::INFO>(
                    "mctp-emulator: Request Matched");
                try
                {
                    if (iter.contains("processing-delay"))
                    {
                        processingDelayMilliSec = iter["processing-delay"];
                    }

                    // Fill the response header as per the MCTP message type
                    // Note:- PLDM requests and responses in the JSON
                    // file should starts from second byte of message
                    // header(HdrVer | PLDMType )
                    if (messageType == "PLDM")
                    {
                        constexpr uint8_t makeResp = 0x7F;
                        response.assign(reqHeader.begin(), reqHeader.end());
                        response.at(1) = response.at(1) & makeResp;
                    }
                    if (messageType == "SECUREDMSG")
                    {
                        response.assign(reqHeader.begin(), reqHeader.end());
                    }
                    response.insert(response.end(),
                                    std::begin(iter["response"]),
                                    std::end(iter["response"]));
                }

                catch (json::exception& e)
                {
                    std::cerr << "message: " << e.what() << '\n'
                              << "exception id: " << e.id << std::endl;
                    continue;
                }

                return std::make_pair(processingDelayMilliSec, response);
            }
        }
    }
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "mctp-emulator: No matching request found");
    return std::nullopt;
}

std::optional<std::pair<int, std::vector<uint8_t>>>
    processMctpCommand(uint8_t dstEid, const std::vector<uint8_t> payload)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "processMctpCommand called...");

    // enable this check for reserved values error in case of eid
    if (dstEid > 0 && dstEid <= 7)
    {

        phosphor::logging::log<phosphor::logging::level::INFO>(
            "mctp-emulator: invalid reserved EID, values upto 7 reserved");
        return std::nullopt;
    }

    if (endpointInterface.find(dstEid) == endpointInterface.end())
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "mctp-emulator: No EID match found hence no processPayload");
        return std::nullopt;
    }

    try
    {
        std::ifstream jsonFile;
        jsonFile.exceptions(std::ios::failbit | std::ios::badbit);

        // the EID concatenated should match the EID's in endpoint.json
        // the resulting named file should be already present as req_resp_x
        std::string filename = epReqRespFile;
        filename.append(std::to_string(dstEid)).append(".json");
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("Req_Resp_x filename: " + filename).c_str());

        jsonFile.open(filename, std::ios::in);
        if (!jsonFile.good())
        {
            std::cerr << "unable to open " << filename << "\n";
            return std::nullopt;
        }

        return processPayload(jsonFile, true, payload);
    }
    catch (const std::ifstream::failure& e)
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "mctp-emulator: file handling error");
        return std::nullopt;
    }
    catch (std::exception& e)
    {
        std::cerr << "message: " << e.what() << std::endl;
        return std::nullopt;
    }
}

MctpBinding::MctpBinding(
    std::shared_ptr<sdbusplus::asio::object_server>& objServer,
    std::string& objPath) :
    objectServer(objServer)
{
    eid = 8;

    uint8_t bindingType = 0xFF; // OEM Binding
    uint8_t bindingMedium = 0XFF;
    bool staticEidSupport = false;
    std::string bindingMode("xyz.openbmc_project.MCTP.BusOwner");
    delayTimer =
        std::make_unique<boost::asio::steady_timer>(bus->get_io_context());

    mctpInterface = objServer->add_interface(objPath, mctpIntf.c_str());

    // Provide specific EID in json data file to add to network
    mctpInterface->register_method("AddDevice", [this](uint8_t destId) {
        addEndpoints(hotSwappableDataFile, destId);
    });

    // Provide specific EID in json data file to remove from network
    mctpInterface->register_method("RemoveDevice", [this](uint8_t destId) {
        std::ifstream jsonFile(hotSwappableDataFile);
        if (!jsonFile.good())
        {
            std::cerr << "unable to open " << hotSwappableDataFile << "\n";
        }
        json endpoints = nullptr;
        try
        {
            endpoints = json::parse(jsonFile, nullptr, false);
        }
        catch (json::exception& e)
        {
            std::cerr << "Error parsing " << hotSwappableDataFile << "\n"
                      << "message: " << e.what() << '\n'
                      << "exception id: " << e.id << std::endl;
            return;
        }
        for (auto endpoint : endpoints["Endpoints"])
        {
            mctp_eid_t endpoint_id = endpoint["Eid"];
            if (endpoint_id == destId)
            {
                removeInterface(endpoint_id, msgInterfaces);
                removeInterface(endpoint_id, vendorInterfaces);
                removeInterface(endpoint_id, uuidInterfaces);
                removeInterface(endpoint_id, endpointInterface);

                return;
            }
        }
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("mctp-emulator: EID " + std::to_string(destId) +
             " is not a hot swappable endpoint")
                .c_str());
    });

    mctpInterface->register_method(
        "SendMctpMessagePayload",
        [](uint8_t dstEid, uint8_t msgTag, bool tagOwner,
           std::vector<uint8_t> payload) {
            int rc = -1;

            phosphor::logging::log<phosphor::logging::level::INFO>(
                "mctp-emulator: Received Payload");

            auto responsePair = processMctpCommand(dstEid, payload);

            if (responsePair.has_value())
            {
                rc = 0;

                int processingDelay = std::get<0>(*responsePair);
                std::vector<uint8_t> response = std::get<1>(*responsePair);

                createResponseSignal(processingDelay, dstEid, payload.at(0),
                                     !tagOwner, msgTag, response);
            }

            return rc;
        });

    mctpInterface->register_method(
        "SendReceiveMctpMessagePayload",
        [](boost::asio::yield_context yield, uint8_t dstEid,
           std::vector<uint8_t> payload, uint16_t timeout) {
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "mctp-emulator: Received Payload");

            auto responsePair = processMctpCommand(dstEid, payload);

            if (responsePair.has_value())
            {
                int processingDelay = std::get<0>(*responsePair);
                std::vector<uint8_t> response = std::get<1>(*responsePair);

                if (handleAtomicResponseTimeout(yield, processingDelay,
                                                timeout))
                {
                    return response;
                }
                else
                {
                    phosphor::logging::log<phosphor::logging::level::INFO>(
                        "mctp-emulator: Unable to respond within timeout");
                    throw sdbusplus::xyz::openbmc_project::Common::Error::
                        Timeout();
                }
            }

            else
            {
                createAsyncDelay(yield, timeout);
                phosphor::logging::log<phosphor::logging::level::INFO>(
                    "mctp-emulator: Error in request");
                throw sdbusplus::xyz::openbmc_project::Common::Error::Timeout();
            }
        });

    mctpInterface->register_signal<uint8_t, uint8_t, uint8_t, bool,
                                   std::vector<uint8_t>>(
        "MessageReceivedSignal");

    mctpInterface->register_property("Eid", eid);

    mctpInterface->register_property("BindingID", bindingType);

    mctpInterface->register_property("BindingMediumID", bindingMedium);

    mctpInterface->register_property("StaticEidSupport", staticEidSupport);

    mctpInterface->register_property(
        "UUID", std::vector<uint8_t>(uuid.begin(), uuid.end()));

    mctpInterface->register_property("BindingMode", bindingMode);

    mctpInterface->initialize();

    getSystemAppUuid();
    endpointIntf = objServer->add_interface(objPath, uuidCommonIntf.c_str());
    endpointIntf->register_property("UUID", uuid);

    endpointIntf->initialize();
}

MctpBinding::~MctpBinding()
{
    /* remove all interfaces */
    for (auto& [ep, iface] : msgInterfaces)
    {
        objectServer->remove_interface(iface);
    }
    for (auto& [ep, iface] : vendorInterfaces)
    {
        objectServer->remove_interface(iface);
    }
    for (auto& [ep, iface] : uuidInterfaces)
    {
        objectServer->remove_interface(iface);
    }
    for (auto& [ep, iface] : endpointInterface)
    {
        objectServer->remove_interface(iface);
    }
}
