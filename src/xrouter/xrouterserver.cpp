//******************************************************************************
//******************************************************************************

#include "xrouterserver.h"
#include "xrouterlogger.h"
#include "xbridge/util/settings.h"
#include "xrouterapp.h"

#include "json/json_spirit_reader_template.h"
#include "json/json_spirit_writer_template.h"
#include "json/json_spirit_utils.h"

#include <boost/chrono/chrono.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/range/adaptor/map.hpp>

#include <algorithm>
#include <iostream>

namespace xrouter
{  

//*****************************************************************************
//*****************************************************************************
bool XRouterServer::start()
{
    // start xrouter
    try
    {
        // sessions
        Settings & s = settings();
        std::vector<std::string> wallets = s.exchangeWallets();
        for (std::vector<std::string>::iterator i = wallets.begin(); i != wallets.end(); ++i)
        {
            xbridge::WalletParam wp;
            wp.currency                    = *i;
            wp.title                       = s.get<std::string>(*i + ".Title");
            wp.address                     = s.get<std::string>(*i + ".Address");
            wp.m_ip                        = s.get<std::string>(*i + ".Ip");
            wp.m_port                      = s.get<std::string>(*i + ".Port");
            wp.m_user                      = s.get<std::string>(*i + ".Username");
            wp.m_passwd                    = s.get<std::string>(*i + ".Password");
            wp.addrPrefix[0]               = s.get<int>(*i + ".AddressPrefix", 0);
            wp.scriptPrefix[0]             = s.get<int>(*i + ".ScriptPrefix", 0);
            wp.secretPrefix[0]             = s.get<int>(*i + ".SecretPrefix", 0);
            wp.COIN                        = s.get<uint64_t>(*i + ".COIN", 0);
            wp.txVersion                   = s.get<uint32_t>(*i + ".TxVersion", 1);
            wp.minTxFee                    = s.get<uint64_t>(*i + ".MinTxFee", 0);
            wp.feePerByte                  = s.get<uint64_t>(*i + ".FeePerByte", 200);
            wp.method                      = s.get<std::string>(*i + ".CreateTxMethod");
            wp.blockTime                   = s.get<int>(*i + ".BlockTime", 0);
            wp.requiredConfirmations       = s.get<int>(*i + ".Confirmations", 0);

            if (wp.m_ip.empty() || wp.m_port.empty() ||
                wp.m_user.empty() || wp.m_passwd.empty() ||
                wp.COIN == 0 || wp.blockTime == 0)
            {
                continue;
            }

            xrouter::WalletConnectorXRouterPtr conn;
            if ((wp.method == "ETH") || (wp.method == "ETHER"))
            {
                conn.reset(new EthWalletConnectorXRouter);
                *conn = wp;
            }
            else if ((wp.method == "BTC") || (wp.method == "BLOCK"))
            {
                conn.reset(new BtcWalletConnectorXRouter);
                *conn = wp;
            }
            else
            {
                conn.reset(new BtcWalletConnectorXRouter);
                *conn = wp;
            }
            if (!conn)
            {
                continue;
            }

            this->addConnector(conn);
        }
    }
    catch (std::exception & e)
    {
        //ERR() << e.what();
        //ERR() << __FUNCTION__;
    }

    return true;
}

void XRouterServer::addConnector(const WalletConnectorXRouterPtr & conn)
{
    boost::mutex::scoped_lock l(m_connectorsLock);
    m_connectors.push_back(conn);
    m_connectorCurrencyMap[conn->currency] = conn;
}

WalletConnectorXRouterPtr XRouterServer::connectorByCurrency(const std::string & currency) const
{
    boost::mutex::scoped_lock l(m_connectorsLock);
    if (m_connectorCurrencyMap.count(currency))
    {
        return m_connectorCurrencyMap.at(currency);
    }

    return WalletConnectorXRouterPtr();
}

void XRouterServer::sendPacketToClient(const XRouterPacketPtr& packet, CNode* pnode)
{
    pnode->PushMessage("xrouter", packet->body());
}

//*****************************************************************************
//*****************************************************************************
static bool verifyBlockRequirement(const XRouterPacketPtr& packet)
{
    if (packet->size() < 36) {
        LOG() << "Packet not big enough";
        return false;
    }

    uint256 txHash(packet->data());
    CTransaction txval;
    uint256 hashBlock;
    int offset = 32;
    uint32_t vout = *static_cast<uint32_t*>(static_cast<void*>(packet->data() + offset));

    CCoins coins;
    CTxOut txOut;
    if (pcoinsTip->GetCoins(txHash, coins)) {
        if (vout > coins.vout.size()) {
            LOG() << "Invalid vout index " << vout;
            return false;
        }

        txOut = coins.vout[vout];
    } else if (GetTransaction(txHash, txval, hashBlock, true)) {
        txOut = txval.vout[vout];
    } else {
        LOG() << "Could not find " << txHash.ToString();
        return false;
    }

    if (txOut.nValue < MIN_BLOCK) {
        LOG() << "Insufficient BLOCK " << txOut.nValue;
        return false;
    }

    CTxDestination destination;
    if (!ExtractDestination(txOut.scriptPubKey, destination)) {
        LOG() << "Unable to extract destination";
        return false;
    }

    auto txKeyID = boost::get<CKeyID>(&destination);
    if (!txKeyID) {
        LOG() << "destination must be a single address";
        return false;
    }

    CPubKey packetKey(packet->pubkey(),
        packet->pubkey() + XRouterPacket::pubkeySize);

    if (packetKey.GetID() != *txKeyID) {
        LOG() << "Public key provided doesn't match UTXO destination.";
        return false;
    }

    return true;
}

//*****************************************************************************
//*****************************************************************************
void XRouterServer::onMessageReceived(CNode* node, XRouterPacketPtr& packet, CValidationState& state)
{
    LOG() << "Processing packet on server side";
    App& app = App::instance();
    
    if (!packet->verify()) {
        LOG() << "unsigned packet or signature error " << __FUNCTION__;
        state.DoS(10, error("XRouter: unsigned packet or signature error"), REJECT_INVALID, "xrouter-error");
        return;
    }

    if (!verifyBlockRequirement(packet)) {
        LOG() << "Block requirement not satisfied\n";
        state.DoS(10, error("XRouter: block requirement not satisfied"), REJECT_INVALID, "xrouter-error");
        return;
    }

    std::string reply;
    uint32_t offset = 36;
    std::string uuid((const char *)packet->data()+offset);
    offset += uuid.size() + 1;
    std::string currency((const char *)packet->data()+offset);
    offset += currency.size() + 1;
    LOG() << "XRouter command: " << std::string(XRouterCommand_ToString(packet->command()));
    if (!app.xrouter_settings.isAvailableCommand(packet->command(), currency)) {
        LOG() << "This command is blocked in xrouter.conf";
        return;
    }
    
    std::chrono::time_point<std::chrono::system_clock> time = std::chrono::system_clock::now();
    if (packet->command() == xrCustomCall) {
        XRouterPluginSettings psettings = app.xrouter_settings.getPluginSettings(currency);
        
        std::string keystr = currency;
        double timeout = psettings.get<double>("timeout", -1.0);
        if (timeout >= 0) {
            if (lastPacketsReceived.count(node)) {
                if (lastPacketsReceived[node].count(keystr)) {
                    std::chrono::time_point<std::chrono::system_clock> prev_time = lastPacketsReceived[node][keystr];
                    std::chrono::system_clock::duration diff = time - prev_time;
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(diff) < std::chrono::milliseconds((int)(timeout * 1000))) {
                        std::string err_msg = "XRouter: too many requests to plugin " + keystr; 
                        state.DoS(100, error(err_msg.c_str()), REJECT_INVALID, "xrouter-error");
                    }
                    if (!lastPacketsReceived.count(node))
                        lastPacketsReceived[node] = boost::container::map<std::string, std::chrono::time_point<std::chrono::system_clock> >();
                    lastPacketsReceived[node][keystr] = time;
                } else {
                    lastPacketsReceived[node][keystr] = time;
                }
            } else {
                lastPacketsReceived[node] = boost::container::map<std::string, std::chrono::time_point<std::chrono::system_clock> >();
                lastPacketsReceived[node][keystr] = time;
            }
        }
        
        std::string feetx((const char *)packet->data()+offset);
        offset += feetx.size() + 1;
        
        std::vector<std::string> params;
        int count = psettings.getMaxParamCount();
        std::string p;
        for (int i = 0; i < count; i++) {
            p = (const char *)packet->data()+offset;
            params.push_back(p);
            offset += p.size() + 1;
        }
        
        reply = processCustomCall(currency, params);
    } else {
        std::string keystr = currency + "::" + XRouterCommand_ToString(packet->command());
        double timeout = app.xrouter_settings.getCommandTimeout(packet->command(), currency);
        if (lastPacketsReceived.count(node)) {
            if (lastPacketsReceived[node].count(keystr)) {
                std::chrono::time_point<std::chrono::system_clock> prev_time = lastPacketsReceived[node][keystr];
                std::chrono::system_clock::duration diff = time - prev_time;
                if (std::chrono::duration_cast<std::chrono::milliseconds>(diff) < std::chrono::milliseconds((int)(timeout * 1000))) {
                    std::string err_msg = "XRouter: too many requests of type " + keystr; 
                    state.DoS(100, error(err_msg.c_str()), REJECT_INVALID, "xrouter-error");
                }
                if (!lastPacketsReceived.count(node))
                    lastPacketsReceived[node] = boost::container::map<std::string, std::chrono::time_point<std::chrono::system_clock> >();
                lastPacketsReceived[node][keystr] = time;
            } else {
                lastPacketsReceived[node][keystr] = time;
            }
        } else {
            lastPacketsReceived[node] = boost::container::map<std::string, std::chrono::time_point<std::chrono::system_clock> >();
            lastPacketsReceived[node][keystr] = time;
        }
            
        switch (packet->command()) {
        case xrGetBlockCount:
            reply = processGetBlockCount(packet, offset, currency);
            break;
        case xrGetBlockHash:
            reply = processGetBlockHash(packet, offset, currency);
            break;
        case xrGetBlock:
            reply = processGetBlock(packet, offset, currency);
            break;
        case xrGetTransaction:
            reply = processGetTransaction(packet, offset, currency);
            break;
        case xrGetAllBlocks:
            reply = processGetAllBlocks(packet, offset, currency);
            break;
        case xrGetAllTransactions:
            reply = processGetAllTransactions(packet, offset, currency);
            break;
        case xrGetBalance:
            reply = processGetBalance(packet, offset, currency);
            break;
        case xrGetBalanceUpdate:
            reply = processGetBalanceUpdate(packet, offset, currency);
            break;
        case xrGetTransactionsBloomFilter:
            reply = processGetTransactionsBloomFilter(packet, offset, currency);
            break;
        case xrSendTransaction:
            reply = processSendTransaction(packet, offset, currency);
            break;
        default:
            LOG() << "Unknown packet";
            return;
        }
    }

    XRouterPacketPtr rpacket(new XRouterPacket(xrReply));

    rpacket->append(uuid);
    rpacket->append(reply);
    sendPacketToClient(rpacket, node);
    return;
}

//*****************************************************************************
//*****************************************************************************
std::string XRouterServer::processGetBlockCount(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    Object result;
    Object error;

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);
    if (conn)
    {
        result.push_back(Pair("result", conn->getBlockCount()));
    }
    else
    {
        error.emplace_back(Pair("error", "No connector for currency " + currency));
        result = error;
    }

    return json_spirit::write_string(Value(result), true);
}

std::string XRouterServer::processGetBlockHash(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string blockId((const char *)packet->data()+offset);
    offset += blockId.size() + 1;

    Object result;
    Object error;

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);
    if (conn)
    {
        result.push_back(Pair("result", conn->getBlockHash(blockId)));
    }
    else
    {
        error.emplace_back(Pair("error", "No connector for currency " + currency));
        result = error;
    }

    return json_spirit::write_string(Value(result), true);
}

std::string XRouterServer::processGetBlock(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string blockHash((const char *)packet->data()+offset);
    offset += blockHash.size() + 1;

    Object result;
    Object error;

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);
    if (conn)
    {
        result = conn->getBlock(blockHash);
    }
    else
    {
        error.emplace_back(Pair("error", "No connector for currency " + currency));
        result = error;
    }
    
    return json_spirit::write_string(Value(result), true);
}

std::string XRouterServer::processGetTransaction(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string hash((const char *)packet->data()+offset);
    offset += hash.size() + 1;

    Object result;
    Object error;

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);
    if (conn)
    {
        result = conn->getTransaction(hash);
    }
    else
    {
        error.emplace_back(Pair("error", "No connector for currency " + currency));
        result = error;
    }

    return json_spirit::write_string(Value(result), true);
}

std::string XRouterServer::processGetAllBlocks(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string number_s((const char *)packet->data()+offset);
    offset += number_s.size() + 1;
    int number = std::stoi(number_s);

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);
    Array result;
    if (conn)
    {
        result = conn->getAllBlocks(number);
    }

    return json_spirit::write_string(Value(result), true);
}

std::string XRouterServer::processGetAllTransactions(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string account((const char *)packet->data()+offset);
    offset += account.size() + 1;
    std::string number_s((const char *)packet->data()+offset);
    offset += number_s.size() + 1;
    int number = std::stoi(number_s);

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);
    int time = 0;
    if (account.find(":") != string::npos) {
        time = std::stoi(account.substr(account.find(":")+1));
        account = account.substr(0, account.find(":"));
    }
    Array result;
    if (conn)
    {
        result = conn->getAllTransactions(account, number, time);
    }

    return json_spirit::write_string(Value(result), true);
}

//*****************************************************************************
//*****************************************************************************
std::string XRouterServer::processGetBalance(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string account((const char *)packet->data()+offset);
    offset += account.size() + 1;

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);
    int time = 0;
    if (account.find(":") != string::npos) {
        time = std::stoi(account.substr(account.find(":")+1));
        account = account.substr(0, account.find(":"));
    }
    std::string result;
    if (conn)
    {
        result = conn->getBalance(account, time);
    }

    return result;
}

std::string XRouterServer::processGetBalanceUpdate(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string account((const char *)packet->data()+offset);
    offset += account.size() + 1;
    std::string number_s((const char *)packet->data()+offset);
    offset += number_s.size() + 1;
    int number = std::stoi(number_s);

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);
    int time = 0;
    if (account.find(":") != string::npos) {
        time = std::stoi(account.substr(account.find(":")+1));
        account = account.substr(0, account.find(":"));
    }

    std::string result;
    if (conn)
    {
        result = conn->getBalanceUpdate(account, number, time);
    }

    return result;
}

std::string XRouterServer::processGetTransactionsBloomFilter(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string number_s((const char *)packet->data()+offset);
    offset += number_s.size() + 1;

    CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
    stream.resize(packet->size() - offset);
    memcpy(&stream[0], packet->data()+offset, packet->size() - offset);

    int number = std::stoi(number_s);

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);

    Array result;
    if (conn)
    {
        result = conn->getTransactionsBloomFilter(number, stream);
    }

    return json_spirit::write_string(Value(result), true);
}

std::string XRouterServer::processSendTransaction(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string transaction((const char *)packet->data()+offset);
    offset += transaction.size() + 1;

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);

    Object result;
    Object error;
    
    if (conn)
    {
        result = conn->sendTransaction(transaction);
    }
    else
    {
        error.emplace_back(Pair("error", "No connector for currency " + currency));
        error.emplace_back(Pair("errorcode", "-100"));
        result = error;
    }
    
    return json_spirit::write_string(Value(result), true);
}

std::string XRouterServer::processCustomCall(std::string name, std::vector<std::string> params)
{
    App& app = App::instance();    
    if (!app.xrouter_settings.hasPlugin(name))
        return "Custom call not found";

    XRouterPluginSettings psettings = app.xrouter_settings.getPluginSettings(name);
    std::string callType = psettings.getParam("type");
    LOG() << "Plugin call " << name << " type = " << callType; 
    if (callType == "rpc") {
        Array jsonparams;
        int count = psettings.getMaxParamCount();
        std::vector<std::string> paramtypes;
        std::string typestring = psettings.getParam("paramsType");
        boost::split(paramtypes, typestring, boost::is_any_of(","));
        std::string p;
        for (int i = 0; i < count; i++) {
            p = params[i];
            if (p == "")
                continue;
            if (paramtypes[i] == "string")
                jsonparams.push_back(p);
            else if (paramtypes[i] == "int") {
                try {
                    jsonparams.push_back(std::stoi(p));
                } catch (...) {
                    return "Parameter #" + std::to_string(i+1) + " can not be converted to integer";
                }
            } else if (paramtypes[i] == "bool") {
                if (params[i] == "true")
                    jsonparams.push_back(true);
                else if (params[i] == "false")
                    jsonparams.push_back(true);
                else
                    return "Parameter #" + std::to_string(i+1) + " can not be converted to bool";
            }
        }
        
        std::string user, passwd, ip, port, command;
        user = psettings.getParam("rpcUser");
        passwd = psettings.getParam("rpcPassword");
        ip = psettings.getParam("rpcIp", "127.0.0.1");
        port = psettings.getParam("rpcPort");
        command = psettings.getParam("rpcCommand");
        Object result = xbridge::rpc::CallRPC(user, passwd, ip, port, command, jsonparams);
        return json_spirit::write_string(Value(result), true);
    } else if (callType == "shell") {
        std::string cmd = psettings.getParam("cmd");
        int count = psettings.getMaxParamCount();
        std::string p;
        for (int i = 0; i < count; i++) {
            cmd += " " + params[i];
        }
        
        LOG() << "Executing shell command " << cmd;
        std::string result = CallCMD(cmd);
        return result;
    }  
    
    return "Unknown type";
}

} // namespace xrouter